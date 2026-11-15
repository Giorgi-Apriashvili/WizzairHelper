#pragma once
// CurrencyService (03 §7.2, protocol-reference §10): where the exchange rates of a
// search come from. Memory first, then a fresh `fx.json`, then the three network
// sources in order, then a stale `fx.json` of any age, then the built-in table.
// The pure parts — the provider parsers, the disk-cache format and the built-in
// table — live in wizz/core/fx_rates.hpp; this class adds the I/O, the caching and
// the notices.
//
// FX traffic does NOT go through the Wizz request gate and is not counted in the
// Wizz request count (04 §5, §13).

#include "wizz/core/domain.hpp"
#include "wizz/core/http.hpp"
#include "wizz/core/progress.hpp"
#include "wizz/core/time.hpp"
#include "wizz/services/file_cache.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace wizz {

/// One exchange-rate source (03 §7.2 `Sources`).
struct FxSource {
    /// Also the `Source` of a Fawaz table and the text of its download notice.
    std::string_view name;
    std::string_view url;
};

/// `CurrencyService.GetRatesAsync` (03 §7.2). One instance is shared by both legs
/// of a search, so the second leg normally hits memory and sends no notice.
///
/// Thread-safe: `get_rates` may run on several worker threads at once. Fresh rates
/// in memory are returned without taking any lock; otherwise one caller at a time
/// (single flight) consults the disk cache and the network while the others wait
/// — cancellably — and then find the rates it remembered.
class CurrencyService {
public:
    /// The sources, tried in this order (03 §7.2). A name starting with
    /// "Frankfurter" (ordinal) selects ParseFrankfurterV2, any other ParseFawaz.
    static constexpr std::array<FxSource, 3> kSources{{
        {.name = "Frankfurter", .url = "https://api.frankfurter.dev/v2/rates?base=EUR"},
        {.name = "currency-api (jsDelivr)",
         .url = "https://cdn.jsdelivr.net/npm/@fawazahmed0/currency-api@latest/v1/currencies/eur.json"},
        {.name = "currency-api (Cloudflare)", .url = "https://latest.currency-api.pages.dev/v1/currencies/eur.json"},
    }};

    /// The disk cache file in the FileCache directory (`%LOCALAPPDATA%\WizzairHelper\cache`).
    static constexpr std::string_view kCacheFileName = "fx.json";
    /// A disk cache younger than this (by its last-write time, `>` is stale) is used
    /// without asking the network; in memory, non-approximate rates live this long.
    static constexpr Duration kMaxAge = std::chrono::hours{12};
    /// Memory life of approximate rates (stale disk cache or built-in table).
    static constexpr Duration kApproximateMemoryLife = std::chrono::minutes{10};
    /// Per-source timeout (http::Request::timeout); a user cancel stays separate.
    static constexpr std::chrono::milliseconds kSourceTimeout{15'000};
    /// A table with fewer rates (the `EUR` entry counts) is not usable.
    static constexpr std::size_t kMinimumRates = 10;

    /// Request headers of every source call, in this order.
    static constexpr std::string_view kUserAgent = "WizzairHelper/1.0 (desktop app)";
    static constexpr std::string_view kAccept = "application/json";

    /// Notices (03 §7.2, 01 §18.5 C6..C8): one before each source is tried, then at
    /// most one fallback notice. None carries a WaitUntilUtc.
    ///
    /// "Downloading exchange rates ({source_name})".
    [[nodiscard]] static std::string downloading_notice(std::string_view source_name);
    static constexpr std::string_view kCachedRatesNotice = "Exchange-rate services unreachable \u2014 using cached rates";
    static constexpr std::string_view kBuiltInRatesNotice =
        "Exchange-rate services unreachable \u2014 using built-in approximate rates";

    /// `transport` is shared (the app passes its single HTTP client), `cache` may be
    /// FileCache::disabled(), `clock` supplies UTC now for the cache ages and the
    /// memory staleness. Throws std::invalid_argument when `transport` or `clock` is null.
    CurrencyService(std::shared_ptr<http::Transport> transport, FileCache cache, std::shared_ptr<const Clock> clock);

    CurrencyService(const CurrencyService&) = delete;
    CurrencyService& operator=(const CurrencyService&) = delete;
    CurrencyService(CurrencyService&&) = delete;
    CurrencyService& operator=(CurrencyService&&) = delete;
    ~CurrencyService() = default;

    /// The rates to price a search with (03 §7.2), never null:
    /// 1. rates remembered in memory that are not stale — no lock, no I/O, no notice
    ///    and no cancellation check;
    /// 2. otherwise, after the single-flight lock (its wait throws
    ///    OperationCancelled when `stop` fires, also when `stop` is already set),
    ///    memory again;
    /// 3. `fx.json` at most kMaxAge old, read with `parse_rates_cache`;
    /// 4. each of kSources in order: notice "Downloading exchange rates ({name})",
    ///    then `GET` with kUserAgent / kAccept and kSourceTimeout. A non-2xx status,
    ///    a table that does not parse (json::Error, wizz::FormatError — D-13), a
    ///    table with fewer than kMinimumRates rates, a timeout or a transport error
    ///    moves on to the next source. The first usable table is written to
    ///    `fx.json` (atomically, by FileCache) and remembered;
    /// 5. `fx.json` of any age: notice kCachedRatesNotice, a copy flagged approximate;
    /// 6. notice kBuiltInRatesNotice and `fx_rates::built_in_rates()` (the same instance
    ///    every time).
    /// Whatever is returned is remembered with the current UTC time. It is stale once
    /// `utc_now() - remembered > (approximate ? kApproximateMemoryLife : kMaxAge)`
    /// (strict `>`), measured from that load time, not from the file's age — so a
    /// disk cache can be trusted for up to ~24 h in all.
    ///
    /// User cancellation (wizz::OperationCancelled) always propagates and is never
    /// swallowed as a failed source: a timeout or transport error observed while
    /// `stop` is set is reported as the cancellation. `notices` (may be empty) runs
    /// synchronously on the calling thread. Other exceptions (e.g. from `notices`)
    /// propagate unchanged.
    [[nodiscard]] FxRatesPtr get_rates(const NoticeSink& notices, std::stop_token stop);

private:
    /// What `Remember` stored: the rates and when they were loaded.
    struct Memory {
        FxRatesPtr rates;
        UtcInstant remembered_at;
    };

    class FlightGuard;

    /// The remembered rates while they are not stale, else nullptr.
    [[nodiscard]] FxRatesPtr fresh_memory() const;
    /// `Remember(r)`: stamps `r` with utc_now() and publishes it.
    FxRatesPtr remember(FxRatesPtr rates);
    /// `TryLoadCache(maxAge)`: nullptr when there is no file, it is too old or it does not parse.
    [[nodiscard]] FxRatesPtr try_load_cache(std::optional<Duration> max_age) const;
    /// One source attempt; nullptr when it is unusable.
    [[nodiscard]] FxRatesPtr try_source(const FxSource& source, std::stop_token stop) const;

    std::shared_ptr<http::Transport> transport_;
    FileCache cache_;
    std::shared_ptr<const Clock> clock_;

    /// Lock-free fast path: readers load the snapshot, `remember` swaps it.
    std::atomic<std::shared_ptr<const Memory>> memory_;

    /// Single-flight lock (`SemaphoreSlim(1)` with a cancellable wait).
    std::mutex flight_mutex_;
    std::condition_variable_any flight_released_;
    bool flight_busy_ = false;
};

} // namespace wizz
