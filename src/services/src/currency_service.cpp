#include "wizz/services/currency_service.hpp"

#include "wizz/core/cancellation.hpp"
#include "wizz/core/errors.hpp"
#include "wizz/core/fx_rates.hpp"
#include "wizz/core/json.hpp"

#include <format>
#include <stdexcept>
#include <utility>

namespace wizz {

namespace {

void report(const NoticeSink& notices, std::string message)
{
    if (notices) {
        notices(ClientNotice{.message = std::move(message), .wait_until_utc = std::nullopt});
    }
}

[[nodiscard]] constexpr bool is_success_status(int status) noexcept
{
    return status >= 200 && status <= 299;
}

} // namespace

/// Holds the single-flight lock for one `get_rates` call (`await _lock.WaitAsync(ct)`
/// ... `finally { _lock.Release(); }`). Like SemaphoreSlim.WaitAsync, a token that is
/// already cancelled throws before the lock is looked at.
class CurrencyService::FlightGuard {
public:
    FlightGuard(CurrencyService& owner, const std::stop_token& stop) : owner_(owner)
    {
        throw_if_cancelled(stop);
        std::unique_lock lock(owner_.flight_mutex_);
        ++owner_.flight_waiters_;
        const bool acquired = owner_.flight_released_.wait(lock, stop, [this] { return !owner_.flight_busy_; });
        --owner_.flight_waiters_;
        if (!acquired) {
            throw OperationCancelled{};
        }
        owner_.flight_busy_ = true;
    }

    ~FlightGuard()
    {
        {
            const std::lock_guard lock(owner_.flight_mutex_);
            owner_.flight_busy_ = false;
        }
        owner_.flight_released_.notify_all();
    }

    FlightGuard(const FlightGuard&) = delete;
    FlightGuard& operator=(const FlightGuard&) = delete;
    FlightGuard(FlightGuard&&) = delete;
    FlightGuard& operator=(FlightGuard&&) = delete;

private:
    CurrencyService& owner_;
};

int CurrencyService::lock_waiters() const
{
    const std::lock_guard lock(flight_mutex_);
    return flight_waiters_;
}

std::string CurrencyService::downloading_notice(std::string_view source_name)
{
    return std::format("Downloading exchange rates ({})", source_name);
}

CurrencyService::CurrencyService(std::shared_ptr<http::Transport> transport, FileCache cache,
                                 std::shared_ptr<const Clock> clock)
    : transport_(std::move(transport)), cache_(std::move(cache)), clock_(std::move(clock))
{
    if (!transport_) {
        throw std::invalid_argument("CurrencyService needs an HTTP transport.");
    }
    if (!clock_) {
        throw std::invalid_argument("CurrencyService needs a clock.");
    }
}

FxRatesPtr CurrencyService::get_rates(const NoticeSink& notices, std::stop_token stop)
{
    if (FxRatesPtr rates = fresh_memory()) {
        return rates; // lock-free fast path
    }

    const FlightGuard flight{*this, stop};
    if (FxRatesPtr rates = fresh_memory()) {
        return rates; // another caller loaded them while this one waited
    }
    if (FxRatesPtr fresh = try_load_cache(kMaxAge)) {
        return remember(std::move(fresh));
    }

    const UtcInstant now = clock_->utc_now();
    const bool sources_just_failed = prefetch_failed_at_ && *prefetch_failed_at_ <= now &&
                                     now - *prefetch_failed_at_ < kRetryAfterFailedPrefetch;
    if (!sources_just_failed) {
        for (const FxSource& source : kSources) {
            report(notices, downloading_notice(source.name));
            if (FxRatesPtr rates = try_source(source, stop)) {
                cache_.try_write(kCacheFileName, fx_rates::serialize_rates_cache(*rates));
                return remember(std::move(rates));
            }
        }
    }

    if (const FxRatesPtr stale = try_load_cache(std::nullopt)) {
        report(notices, std::string{kCachedRatesNotice});
        auto approximate = std::make_shared<FxRates>(*stale);
        approximate->is_approximate = true;
        return remember(std::move(approximate));
    }

    report(notices, std::string{kBuiltInRatesNotice});
    return remember(fx_rates::built_in_rates());
}

bool CurrencyService::prefetch(std::stop_token stop)
{
    if (fresh_memory()) {
        return true;
    }
    const FlightGuard flight{*this, stop};
    if (fresh_memory()) {
        return true;  // a search loaded them while this one waited
    }
    if (FxRatesPtr fresh = try_load_cache(kMaxAge)) {
        remember(std::move(fresh));
        return true;
    }
    for (const FxSource& source : kSources) {
        if (FxRatesPtr rates = try_source(source, stop)) {
            cache_.try_write(kCacheFileName, fx_rates::serialize_rates_cache(*rates));
            remember(std::move(rates));
            prefetch_failed_at_.reset();
            return true;
        }
    }
    prefetch_failed_at_ = clock_->utc_now();  // no stale copy, no built-in table: a search decides
    return false;
}

FxRatesPtr CurrencyService::fresh_memory() const
{
    const std::shared_ptr<const Memory> memory = memory_.load();
    if (!memory) {
        return nullptr;
    }
    const Duration life = memory->rates->is_approximate ? kApproximateMemoryLife : kMaxAge;
    if (clock_->utc_now() - memory->remembered_at > life) {
        return nullptr; // IsStale: strict >
    }
    return memory->rates;
}

FxRatesPtr CurrencyService::remember(FxRatesPtr rates)
{
    memory_.store(std::make_shared<const Memory>(Memory{.rates = rates, .remembered_at = clock_->utc_now()}));
    return rates;
}

FxRatesPtr CurrencyService::try_load_cache(std::optional<Duration> max_age) const
{
    const std::optional<std::string> text = cache_.try_read(kCacheFileName, max_age, *clock_);
    if (!text) {
        return nullptr;
    }
    try {
        return std::make_shared<const FxRates>(fx_rates::parse_rates_cache(*text));
    } catch (const json::Error&) {
        // JsonException, InvalidOperationException, FormatException of a number: no cache.
    } catch (const FormatError&) {
        // "Empty FX cache.", "FX cache has no date.", "FX cache has no rates.", ...: no cache.
    }
    return nullptr;
}

FxRatesPtr CurrencyService::try_source(const FxSource& source, std::stop_token stop) const
{
    const http::Request request{
        .method = "GET",
        .url = std::string{source.url},
        .headers = {{"User-Agent", std::string{kUserAgent}}, {"Accept", std::string{kAccept}}},
        .body = std::nullopt,
        .content_type = std::nullopt,
        .timeout = kSourceTimeout,
    };
    try {
        const http::Response response = transport_->send(request, stop);
        if (!is_success_status(response.status)) {
            return nullptr;
        }
        const std::string body = http::decode_body_text(response);
        FxRates rates = source.name.starts_with(fx_rates::kFrankfurterSource)
                            ? fx_rates::parse_frankfurter_v2(body)
                            : fx_rates::parse_fawaz(body, source.name);
        if (rates.per_eur.size() < kMinimumRates) {
            return nullptr;
        }
        return std::make_shared<const FxRates>(std::move(rates));
    } catch (const http::TimeoutError&) {
        // Only a timeout while the user has NOT cancelled moves on (checked below).
        throw_if_cancelled(stop);
    } catch (const http::TransportError&) {
        // HttpRequestException; one raised by a user cancel is that cancel.
        throw_if_cancelled(stop);
    } catch (const json::Error&) {
        // JsonException / InvalidOperationException / FormatException: malformed table (D-13).
    } catch (const FormatError&) {
        // FormatException / KeyNotFoundException: wrong shape or a missing member (D-13).
    }
    return nullptr;
}

} // namespace wizz
