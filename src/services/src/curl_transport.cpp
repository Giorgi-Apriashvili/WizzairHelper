#include "wizz/services/curl_transport.hpp"

#include "curl_handles.hpp"
#include "proxy_resolver.hpp"

#include "wizz/core/cancellation.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <exception>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace wizz::http {
namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr int kMaxRedirects = 50;                            // 04 §2.2: .NET MaxAutomaticRedirections
constexpr int kPollIntervalMs = 100;                         // 04 §11.2: a user cancel lands within ~100 ms
constexpr long kConnectionLifetimeSeconds = 300;             // 04 §2.1: PooledConnectionLifetime = 5 min
constexpr long kMaxConnectionsPerSession = 8;
constexpr std::size_t kMaxIdleSessions = 4;
constexpr const char* kAcceptEncoding = "gzip, deflate, br"; // 04 §2.1; decoded by libcurl

/// curl_global_init exactly once per process (thread-safe static initialisation),
/// curl_global_cleanup at exit.
class CurlLibrary {
public:
    CurlLibrary()
    {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error("libcurl could not be initialised");
        }
    }
    ~CurlLibrary() { curl_global_cleanup(); }
    CurlLibrary(const CurlLibrary&) = delete;
    CurlLibrary& operator=(const CurlLibrary&) = delete;
};

void ensure_curl_initialised()
{
    static const CurlLibrary library;
}

[[nodiscard]] constexpr char ascii_lower(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] constexpr bool iequals_ascii(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() &&
           std::ranges::equal(a, b, [](char x, char y) { return ascii_lower(x) == ascii_lower(y); });
}

[[nodiscard]] constexpr std::string_view trim_blanks(std::string_view text) noexcept
{
    constexpr std::string_view kBlanks = " \t";
    const auto first = text.find_first_not_of(kBlanks);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(kBlanks) - first + 1);
}

// --- libcurl error handling -------------------------------------------------------

void check_option(CURLcode code, CURLoption option)
{
    if (code == CURLE_OUT_OF_MEMORY) {
        throw std::bad_alloc{};
    }
    if (code != CURLE_OK) {
        throw std::runtime_error(
            std::format("libcurl rejected option {}: {}", static_cast<int>(option), curl_easy_strerror(code)));
    }
}

void check_multi(CURLMcode code)
{
    if (code == CURLM_OUT_OF_MEMORY) {
        throw std::bad_alloc{};
    }
    if (code != CURLM_OK) {
        throw TransportError(curl_multi_strerror(code));
    }
}

// Typed wrappers: curl_easy_setopt is variadic, so every argument must have exactly
// the type libcurl reads (long, curl_off_t, a pointer or a callback).
void set_option(CURL* easy, CURLoption option, long value)
{
    check_option(curl_easy_setopt(easy, option, value), option);
}
void set_option(CURL* easy, CURLoption option, curl_off_t value)
{
    check_option(curl_easy_setopt(easy, option, value), option);
}
void set_option(CURL* easy, CURLoption option, const char* value)
{
    check_option(curl_easy_setopt(easy, option, value), option);
}
void set_option(CURL* easy, CURLoption option, void* value)
{
    check_option(curl_easy_setopt(easy, option, value), option);
}
void set_option(CURL* easy, CURLoption option, curl_write_callback value)
{
    check_option(curl_easy_setopt(easy, option, value), option);
}

// --- shared state -----------------------------------------------------------------

/// A DNS cache shared by every transfer of the transport. Connections are reused
/// through the session pool instead: libcurl does not support sharing a connection
/// pool between threads.
class SharedDnsCache {
public:
    SharedDnsCache()
    {
        if (!handle_) {
            throw std::bad_alloc{};
        }
        const curl_lock_function lock_function = &SharedDnsCache::lock;
        const curl_unlock_function unlock_function = &SharedDnsCache::unlock;
        check(curl_share_setopt(handle_.get(), CURLSHOPT_LOCKFUNC, lock_function));
        check(curl_share_setopt(handle_.get(), CURLSHOPT_UNLOCKFUNC, unlock_function));
        check(curl_share_setopt(handle_.get(), CURLSHOPT_USERDATA, static_cast<void*>(this)));
        check(curl_share_setopt(handle_.get(), CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS));
    }
    SharedDnsCache(const SharedDnsCache&) = delete;
    SharedDnsCache& operator=(const SharedDnsCache&) = delete;

    [[nodiscard]] CURLSH* get() const noexcept { return handle_.get(); }

private:
    struct ShareDeleter {
        void operator()(CURLSH* share) const noexcept { curl_share_cleanup(share); }
    };

    static void check(CURLSHcode code)
    {
        if (code != CURLSHE_OK) {
            throw std::runtime_error(std::format("libcurl share setup failed: {}", curl_share_strerror(code)));
        }
    }
    static void lock(CURL*, curl_lock_data data, curl_lock_access, void* self) noexcept
    {
        static_cast<SharedDnsCache*>(self)->mutexes_[static_cast<std::size_t>(data)].lock();
    }
    static void unlock(CURL*, curl_lock_data data, void* self) noexcept
    {
        static_cast<SharedDnsCache*>(self)->mutexes_[static_cast<std::size_t>(data)].unlock();
    }

    std::array<std::mutex, CURL_LOCK_DATA_LAST> mutexes_;
    std::unique_ptr<CURLSH, ShareDeleter> handle_{curl_share_init()};
};

/// Multi handles kept between requests. Each owns a keep-alive connection pool and is
/// used by one send() at a time, so sequential requests reuse their connections.
class SessionPool {
public:
    class Lease {
    public:
        Lease(SessionPool& pool, curl::Multi multi) noexcept : pool_(pool), multi_(std::move(multi)) {}
        ~Lease() { pool_.release(std::move(multi_)); }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        [[nodiscard]] CURLM* get() const noexcept { return multi_.get(); }

    private:
        SessionPool& pool_;
        curl::Multi multi_;
    };

    SessionPool() { idle_.reserve(kMaxIdleSessions); }

    [[nodiscard]] Lease acquire()
    {
        {
            const std::lock_guard lock(mutex_);
            if (!idle_.empty()) {
                curl::Multi multi = std::move(idle_.back());
                idle_.pop_back();
                return Lease{*this, std::move(multi)};
            }
        }
        curl::Multi multi{curl_multi_init()};
        if (!multi) {
            throw std::bad_alloc{};
        }
        check_multi(curl_multi_setopt(multi.get(), CURLMOPT_MAXCONNECTS, kMaxConnectionsPerSession));
        return Lease{*this, std::move(multi)};
    }

private:
    void release(curl::Multi multi) noexcept
    {
        const std::lock_guard lock(mutex_);
        if (idle_.size() < kMaxIdleSessions) {
            idle_.push_back(std::move(multi)); // capacity is reserved: never allocates
        }
    }

    std::mutex mutex_;
    std::vector<curl::Multi> idle_;
};

/// Adds an easy handle to a multi handle for the lifetime of this object. Removing it
/// aborts an unfinished transfer and closes that connection.
class MultiAttachment {
public:
    MultiAttachment(CURLM* multi, CURL* easy) : multi_(multi), easy_(easy)
    {
        check_multi(curl_multi_add_handle(multi_, easy_));
    }
    ~MultiAttachment() { curl_multi_remove_handle(multi_, easy_); }
    MultiAttachment(const MultiAttachment&) = delete;
    MultiAttachment& operator=(const MultiAttachment&) = delete;

private:
    CURLM* multi_;
    CURL* easy_;
};

// --- URLs and redirects -----------------------------------------------------------

struct UrlParts {
    std::string scheme; ///< lower case
    std::string host;   ///< IPv6 literals keep their brackets
};

[[nodiscard]] std::optional<UrlParts> parse_url(const std::string& url)
{
    const curl::Url handle{curl_url()};
    if (!handle) {
        throw std::bad_alloc{};
    }
    if (curl_url_set(handle.get(), CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) {
        return std::nullopt;
    }
    const auto part = [&](CURLUPart which) -> std::optional<std::string> {
        char* raw = nullptr;
        if (curl_url_get(handle.get(), which, &raw, 0) != CURLUE_OK) {
            return std::nullopt;
        }
        const curl::String owner{raw};
        return std::string{owner.get()};
    };
    auto scheme = part(CURLUPART_SCHEME);
    auto host = part(CURLUPART_HOST);
    if (!scheme || !host) {
        return std::nullopt;
    }
    std::ranges::transform(*scheme, scheme->begin(), ascii_lower);
    return UrlParts{std::move(*scheme), std::move(*host)};
}

[[nodiscard]] constexpr bool is_redirect_status(long status) noexcept
{
    return status == 300 || status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/// .NET RedirectHandler: only http/https targets, and never from https down to http.
[[nodiscard]] bool may_follow(const std::string& from, const std::string& to)
{
    const auto source = parse_url(from);
    const auto target = parse_url(to);
    if (!source || !target || (target->scheme != "http" && target->scheme != "https")) {
        return false;
    }
    return !(source->scheme == "https" && target->scheme == "http");
}

/// .NET RequestRequiresForceGet: the redirected request becomes a GET without content.
[[nodiscard]] constexpr bool forces_get(int status, std::string_view method) noexcept
{
    switch (status) {
    case 300:
    case 301:
    case 302: return method == "POST";
    case 303: return method != "GET";
    default: return false;
    }
}

[[nodiscard]] SteadyClock::time_point deadline_after(std::chrono::milliseconds timeout) noexcept
{
    const auto now = SteadyClock::now();
    const auto room = std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::time_point::max() - now);
    return timeout >= room ? SteadyClock::time_point::max() : now + timeout;
}

[[nodiscard]] long clamp_to_long(std::chrono::milliseconds value) noexcept
{
    return static_cast<long>(std::clamp<std::chrono::milliseconds::rep>(value.count(), 1, LONG_MAX));
}

/// The time left until `deadline`, rounded up to whole milliseconds; TimeoutError when none is.
[[nodiscard]] std::chrono::milliseconds time_left(SteadyClock::time_point deadline, const std::string& url)
{
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - SteadyClock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
        throw TimeoutError(std::format("The request to {} timed out before it could be sent", url));
    }
    return remaining;
}

// --- proxy discovery --------------------------------------------------------------

/// Runs proxy lookups that may block (WinHTTP automatic detection or setup script, an
/// injected selector) on threads of their own, so that a send stops waiting the moment
/// its deadline passes or `stop` fires (04 §11.2). An abandoned lookup runs to completion
/// in the background; finished threads are joined by later lookups, the rest by the
/// destructor.
class ProxyLookups {
public:
    using Lookup = std::function<std::optional<std::string>()>;

    ProxyLookups() = default;
    ProxyLookups(const ProxyLookups&) = delete;
    ProxyLookups& operator=(const ProxyLookups&) = delete;
    ~ProxyLookups() = default; // every jthread joins

    /// The lookup's answer, or what it threw; TimeoutError at `deadline` and
    /// OperationCancelled when `stop` fires (a cancel wins).
    [[nodiscard]] std::optional<std::string> run(Lookup lookup, SteadyClock::time_point deadline,
                                                 const std::stop_token& stop, const std::string& url)
    {
        const auto outcome = std::make_shared<Outcome>();
        {
            const std::scoped_lock lock(mutex_);
            std::erase_if(workers_, [](const Worker& worker) { return worker.outcome->finished(); });
            workers_.reserve(workers_.size() + 1); // push_back cannot throw once the thread runs
            workers_.push_back(
                Worker{outcome, std::jthread{[outcome, lookup = std::move(lookup)] { outcome->complete(lookup); }}});
        }
        return outcome->wait(deadline, stop, url);
    }

private:
    class Outcome {
    public:
        void complete(const Lookup& lookup) noexcept
        {
            std::optional<std::string> proxy;
            std::exception_ptr failure;
            try {
                proxy = lookup();
            } catch (...) {
                failure = std::current_exception();
            }
            {
                const std::scoped_lock lock(mutex_);
                proxy_ = std::move(proxy);
                failure_ = std::move(failure);
                finished_ = true;
            }
            done_.notify_all();
        }

        [[nodiscard]] bool finished()
        {
            const std::scoped_lock lock(mutex_);
            return finished_;
        }

        [[nodiscard]] std::optional<std::string> wait(SteadyClock::time_point deadline, const std::stop_token& stop,
                                                      const std::string& url)
        {
            std::unique_lock lock(mutex_);
            const auto is_finished = [this] { return finished_; };
            const bool finished = deadline == SteadyClock::time_point::max()
                                      ? done_.wait(lock, stop, is_finished)
                                      : done_.wait_until(lock, stop, deadline, is_finished);
            if (!finished) {
                lock.unlock();
                throw_if_cancelled(stop);
                throw TimeoutError(std::format("The request to {} timed out while its proxy was being looked up", url));
            }
            if (failure_) {
                std::rethrow_exception(failure_);
            }
            return std::move(proxy_);
        }

    private:
        std::mutex mutex_; // guards the members below
        std::condition_variable_any done_;
        bool finished_ = false;
        std::optional<std::string> proxy_;
        std::exception_ptr failure_;
    };

    struct Worker {
        std::shared_ptr<Outcome> outcome;
        std::jthread thread;
    };

    std::mutex mutex_; // guards workers_
    std::vector<Worker> workers_;
};

// --- one transfer -----------------------------------------------------------------

/// Receives one response. Status lines reset the header list, so only the headers of
/// the last response (after any 1xx or proxy CONNECT response) remain; lines after
/// the blank line ending the header block are trailers and are ignored.
class ResponseSink {
public:
    void header_line(std::string_view line)
    {
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.remove_suffix(1);
        }
        if (line.starts_with("HTTP/")) {
            response_.headers.clear();
            response_.content_type.reset();
            in_header_block_ = true;
            last_ = Last::none;
            return;
        }
        if (line.empty()) {
            in_header_block_ = false;
            return;
        }
        if (!in_header_block_) {
            return;
        }
        if (line.front() == ' ' || line.front() == '\t') { // obsolete line folding continues the last value
            append_folded(trim_blanks(line));
            return;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            return;
        }
        std::string name{line.substr(0, colon)};
        std::string value{trim_blanks(line.substr(colon + 1))};
        if (iequals_ascii(name, "Content-Type")) {
            if (!response_.content_type) {
                response_.content_type = std::move(value);
                last_ = Last::content_type;
            }
            return;
        }
        response_.headers.emplace_back(std::move(name), std::move(value));
        last_ = Last::header;
    }

    void body(std::string_view bytes) { response_.body.append(bytes); }
    void fail(std::exception_ptr failure) noexcept { failure_ = std::move(failure); }

    [[nodiscard]] const std::exception_ptr& failure() const noexcept { return failure_; }
    [[nodiscard]] Response take_response(int status)
    {
        response_.status = status;
        return std::move(response_);
    }

private:
    enum class Last { none, header, content_type };

    void append_folded(std::string_view continuation)
    {
        std::string* target = last_ == Last::header         ? &response_.headers.back().second
                              : last_ == Last::content_type ? &*response_.content_type
                                                            : nullptr;
        if (target && !continuation.empty()) {
            target->append(" ").append(continuation);
        }
    }

    Response response_;
    bool in_header_block_ = false;
    Last last_ = Last::none;
    std::exception_ptr failure_;
};

std::size_t on_header(char* data, std::size_t size, std::size_t count, void* user) noexcept
{
    auto& sink = *static_cast<ResponseSink*>(user);
    try {
        sink.header_line(std::string_view{data, size * count});
        return size * count;
    } catch (...) {
        sink.fail(std::current_exception());
        return CURL_WRITEFUNC_ERROR;
    }
}

std::size_t on_body(char* data, std::size_t size, std::size_t count, void* user) noexcept
{
    auto& sink = *static_cast<ResponseSink*>(user);
    try {
        sink.body(std::string_view{data, size * count});
        return size * count;
    } catch (...) {
        sink.fail(std::current_exception());
        return CURL_WRITEFUNC_ERROR;
    }
}

[[nodiscard]] bool has_line_break(std::string_view text) noexcept
{
    return text.find_first_of("\r\n") != std::string_view::npos;
}

/// Request headers in send order, then Accept-Encoding and the content headers, as
/// .NET puts them on the wire (04 §2.3). Empty "Name:" lines stop libcurl from adding
/// its own Accept, Content-Type and Expect headers.
[[nodiscard]] curl::HeaderList build_header_list(const Headers& headers, const std::string* content_type,
                                                 bool sends_body)
{
    curl::HeaderList list;
    const auto append = [&list](const std::string& line) {
        curl_slist* head = curl_slist_append(list.get(), line.c_str());
        if (!head) {
            throw std::bad_alloc{};
        }
        static_cast<void>(list.release()); // `head` owns the same list now
        list.reset(head);
    };
    bool has_accept = false;
    bool has_accept_encoding = false;
    for (const auto& [name, value] : headers) {
        if (name.empty() || name.contains(':') || has_line_break(name) || has_line_break(value)) {
            throw std::invalid_argument(std::format("Invalid HTTP request header '{}'", name));
        }
        has_accept = has_accept || iequals_ascii(name, "Accept");
        has_accept_encoding = has_accept_encoding || iequals_ascii(name, "Accept-Encoding");
        append(value.empty() ? name + ";" : name + ": " + value);
    }
    if (!has_accept_encoding) {
        append(std::format("Accept-Encoding: {}", kAcceptEncoding));
    }
    if (content_type) {
        if (has_line_break(*content_type)) {
            throw std::invalid_argument("Invalid HTTP Content-Type");
        }
        append("Content-Type: " + *content_type);
    } else if (sends_body) {
        append("Content-Type:");
    }
    if (!has_accept) {
        append("Accept:");
    }
    append("Expect:"); // never "Expect: 100-continue" (04 §2.2)
    return list;
}

[[nodiscard]] std::string describe(CURLcode code, const std::array<char, CURL_ERROR_SIZE>& error_buffer)
{
    const std::string_view detail = trim_blanks(std::string_view{error_buffer.data()});
    return detail.empty() ? std::string{curl_easy_strerror(code)} : std::string{detail};
}

/// Drives the transfer of `easy` on `multi` to completion and returns its result.
/// Throws OperationCancelled as soon as `stop` fires; curl_multi_wakeup (registered by
/// the caller) cuts the poll short.
[[nodiscard]] CURLcode run_transfer(CURLM* multi, CURL* easy, const std::stop_token& stop)
{
    for (;;) {
        throw_if_cancelled(stop);
        int running = 0;
        check_multi(curl_multi_perform(multi, &running));
        int queued = 0;
        while (const CURLMsg* message = curl_multi_info_read(multi, &queued)) {
            if (message->msg == CURLMSG_DONE && message->easy_handle == easy) {
                return message->data.result;
            }
        }
        check_multi(curl_multi_poll(multi, nullptr, 0, kPollIntervalMs, nullptr));
    }
}

} // namespace

class CurlTransport::Impl {
public:
    explicit Impl(std::optional<WindowsProxyConfig> proxy) : proxy_(std::make_shared<ProxyResolver>(std::move(proxy)))
    {
    }
    explicit Impl(ProxySelector select_proxy) : select_proxy_(std::move(select_proxy)) {}

    [[nodiscard]] Response send(const Request& request, const std::stop_token& stop);

private:
    /// One request on the wire; a redirect produces the next hop.
    struct Hop {
        std::string_view method;
        const std::string& url;
        const Headers& headers;
        const std::string* body;         ///< POST body, nullptr for none
        const std::string* content_type; ///< nullptr for none
    };
    struct HopResult {
        Response response;
        std::optional<std::string> redirect_url; ///< resolved Location of a 3xx response
    };

    /// Ensures curl_global_init ran before any handle below is created.
    struct LibraryGuard {
        LibraryGuard() { ensure_curl_initialised(); }
    };

    [[nodiscard]] HopResult perform(const Hop& hop, SteadyClock::time_point deadline,
                                    std::chrono::milliseconds connect_timeout, const std::stop_token& stop);
    /// The proxy for `url` (nullopt: libcurl's environment rules). A lookup that may
    /// block runs on a helper thread and is waited for until `deadline` or `stop`.
    [[nodiscard]] std::optional<std::string> choose_proxy(const std::string& url, SteadyClock::time_point deadline,
                                                          const std::stop_token& stop);

    LibraryGuard library_;
    SharedDnsCache dns_;     // must outlive every session and transfer
    SessionPool sessions_;
    std::shared_ptr<ProxyResolver> proxy_; // null when `select_proxy_` decides
    ProxySelector select_proxy_;
    ProxyLookups lookups_; // last: joins abandoned lookups first
};

Response CurlTransport::Impl::send(const Request& request, const std::stop_token& stop)
{
    if (request.method != "GET" && request.method != "POST") {
        throw std::invalid_argument(std::format("Unsupported HTTP method '{}'", request.method));
    }
    const auto deadline = deadline_after(request.timeout);
    std::string method = request.method;
    std::string url = request.url;
    const std::string* body = request.method == "POST" && request.body ? &*request.body : nullptr;
    const std::string* content_type = request.content_type ? &*request.content_type : nullptr;
    for (int redirects = 0;; ++redirects) {
        HopResult hop = perform(Hop{method, url, request.headers, body, content_type}, deadline,
                                request.connect_timeout, stop);
        if (!hop.redirect_url || redirects == kMaxRedirects || !may_follow(url, *hop.redirect_url)) {
            return std::move(hop.response);
        }
        if (forces_get(hop.response.status, method)) {
            method = "GET";
            body = nullptr;
            content_type = nullptr;
        }
        url = std::move(*hop.redirect_url);
    }
}

CurlTransport::Impl::HopResult CurlTransport::Impl::perform(const Hop& hop, SteadyClock::time_point deadline,
                                                            std::chrono::milliseconds connect_timeout,
                                                            const std::stop_token& stop)
{
    throw_if_cancelled(stop); // checked before the deadline: a user cancel wins over a timeout
    static_cast<void>(time_left(deadline, hop.url)); // no time left: not even the proxy lookup starts
    // 04 §11.2: the attempt's timer covers proxy discovery too, so the transfer gets
    // only what is left after it.
    const std::optional<std::string> proxy = choose_proxy(hop.url, deadline, stop);
    throw_if_cancelled(stop);
    const std::chrono::milliseconds remaining = time_left(deadline, hop.url);

    const bool is_post = hop.method == "POST";
    ResponseSink sink;
    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    const curl::HeaderList header_list = build_header_list(hop.headers, hop.content_type, is_post);

    const SessionPool::Lease session = sessions_.acquire();
    const curl::Easy easy_handle{curl_easy_init()};
    if (!easy_handle) {
        throw std::bad_alloc{};
    }
    CURL* const easy = easy_handle.get();
    set_option(easy, CURLOPT_ERRORBUFFER, static_cast<void*>(error_buffer.data()));
    set_option(easy, CURLOPT_URL, hop.url.c_str());
    set_option(easy, CURLOPT_PROTOCOLS_STR, "http,https");
    set_option(easy, CURLOPT_NOSIGNAL, 1L);
    set_option(easy, CURLOPT_SHARE, static_cast<void*>(dns_.get()));
    set_option(easy, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
    set_option(easy, CURLOPT_FOLLOWLOCATION, 0L); // redirects follow .NET rules in send()
    set_option(easy, CURLOPT_ACCEPT_ENCODING, kAcceptEncoding);
    set_option(easy, CURLOPT_TIMEOUT_MS, clamp_to_long(remaining));
    set_option(easy, CURLOPT_CONNECTTIMEOUT_MS, clamp_to_long(connect_timeout));
    set_option(easy, CURLOPT_MAXLIFETIME_CONN, kConnectionLifetimeSeconds);
    set_option(easy, CURLOPT_SSL_VERIFYPEER, 1L);
    set_option(easy, CURLOPT_SSL_VERIFYHOST, 2L);
    set_option(easy, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
    set_option(easy, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
    set_option(easy, CURLOPT_HTTPHEADER, static_cast<void*>(header_list.get()));
    set_option(easy, CURLOPT_HEADERFUNCTION, &on_header);
    set_option(easy, CURLOPT_HEADERDATA, static_cast<void*>(&sink));
    set_option(easy, CURLOPT_WRITEFUNCTION, &on_body);
    set_option(easy, CURLOPT_WRITEDATA, static_cast<void*>(&sink));
    if (is_post) {
        // libcurl keeps this pointer (no copy): it must stay valid for the whole transfer.
        set_option(easy, CURLOPT_POST, 1L);
        set_option(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(hop.body ? hop.body->size() : 0));
        set_option(easy, CURLOPT_POSTFIELDS, hop.body ? hop.body->c_str() : "");
    } else {
        set_option(easy, CURLOPT_HTTPGET, 1L);
    }
    if (proxy) { // nullopt: environment variables, libcurl's own rules
        set_option(easy, CURLOPT_PROXY, proxy->c_str());
        set_option(easy, CURLOPT_NOPROXY, ""); // the choice above is final
        if (!proxy->empty()) {
            // Corporate proxies: Negotiate/NTLM through SSPI with the signed-in user's credentials.
            set_option(easy, CURLOPT_PROXYAUTH, static_cast<long>(CURLAUTH_NEGOTIATE | CURLAUTH_NTLM));
            set_option(easy, CURLOPT_PROXYUSERPWD, ":");
        }
    }

    CURLcode result = CURLE_OK;
    {
        const MultiAttachment attachment{session.get(), easy};
        const std::stop_callback wake_on_cancel{stop, [multi = session.get()] { curl_multi_wakeup(multi); }};
        result = run_transfer(session.get(), easy, stop);
    }
    throw_if_cancelled(stop); // a user cancel wins over whatever the transfer produced
    if (sink.failure()) {
        std::rethrow_exception(sink.failure());
    }
    if (result == CURLE_OUT_OF_MEMORY) {
        throw std::bad_alloc{};
    }
    if (result == CURLE_OPERATION_TIMEDOUT) {
        throw TimeoutError(describe(result, error_buffer));
    }
    if (result != CURLE_OK) {
        throw TransportError(describe(result, error_buffer));
    }

    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    std::optional<std::string> redirect_url;
    if (char* location = nullptr; is_redirect_status(status) &&
                                  curl_easy_getinfo(easy, CURLINFO_REDIRECT_URL, &location) == CURLE_OK && location) {
        redirect_url.emplace(location);
    }
    return HopResult{sink.take_response(static_cast<int>(status)), std::move(redirect_url)};
}

std::optional<std::string> CurlTransport::Impl::choose_proxy(const std::string& url, SteadyClock::time_point deadline,
                                                             const std::stop_token& stop)
{
    if (select_proxy_) {
        return lookups_.run([select = select_proxy_, url] { return select(url); }, deadline, stop, url);
    }
    auto parts = parse_url(url);
    if (!parts) {
        return std::nullopt; // libcurl reports the malformed URL
    }
    if (!proxy_->may_block(parts->host)) {
        return proxy_->proxy_for(url, parts->scheme, parts->host); // direct, manual or environment: no wait
    }
    return lookups_.run([resolver = proxy_, url, parts = std::move(*parts)] {
        return resolver->proxy_for(url, parts.scheme, parts.host);
    }, deadline, stop, url);
}

CurlTransport::CurlTransport() : impl_(std::make_unique<Impl>(std::nullopt)) {}

CurlTransport::CurlTransport(WindowsProxyConfig proxy)
    : impl_(std::make_unique<Impl>(std::optional<WindowsProxyConfig>{std::move(proxy)}))
{
}

CurlTransport::CurlTransport(ProxySelector select_proxy) : impl_(std::make_unique<Impl>(std::move(select_proxy))) {}

CurlTransport::~CurlTransport() = default;

Response CurlTransport::send(const Request& request, std::stop_token stop)
{
    return impl_->send(request, stop);
}

std::string CurlTransport::library_version()
{
    ensure_curl_initialised();
    return curl_version();
}

} // namespace wizz::http
