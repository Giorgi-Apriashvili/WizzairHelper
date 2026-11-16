// Updater: the daily check schedule in <data root>\updates\update-state.json (D-40).

#include "wizz/services/updater.hpp"

#include "wizz/core/datetime_parse.hpp"
#include "wizz/core/json.hpp"

#include <stdexcept>
#include <utility>

namespace wizz::update {
namespace {

constexpr std::string_view kLastSuccessKey = "LastSuccessfulCheckUtc";
constexpr std::string_view kLatestSeenKey = "LatestSeenVersion";
constexpr std::string_view kLastFailureKey = "LastFailedCheckUtc";

/// `yyyy-MM-ddTHH:mm:ss[.fffffff]Z` as written by format_iso_utc; nullopt for anything
/// without the `Z` or not a date-time.
[[nodiscard]] std::optional<UtcInstant> parse_utc(std::string_view text)
{
    if (!text.ends_with('Z')) {
        return std::nullopt;
    }
    const auto wall = parse_local_datetime(text); // the trailing Z is accepted and ignored
    if (!wall) {
        return std::nullopt;
    }
    return UtcInstant{wall->time_since_epoch()};
}

/// The member as a string; nullopt when absent or null. Throws json::TypeError for
/// another kind, which `parse` turns into the empty state.
[[nodiscard]] std::optional<std::string_view> optional_text(const json::Value& root, std::string_view name)
{
    const json::Value* value = root.find(name);
    if (value == nullptr || value->is_null()) {
        return std::nullopt;
    }
    return value->as_string();
}

} // namespace

UpdateState UpdateState::parse(std::string_view json_text) noexcept
{
    try {
        const json::Value root = json::parse(json_text);
        if (!root.is_object()) {
            return {};
        }
        bool valid = true;
        // A present value must have the right format, or the whole state is dropped.
        const auto checked = [&valid]<typename T>(std::optional<std::string_view> text, std::optional<T> value) {
            valid = valid && (!text || value);
            return value;
        };
        const auto last_success = optional_text(root, kLastSuccessKey);
        const auto latest_seen = optional_text(root, kLatestSeenKey);
        const auto last_failure = optional_text(root, kLastFailureKey);
        UpdateState state{
            .last_success = checked(last_success, last_success ? parse_utc(*last_success) : std::nullopt),
            .latest_seen = checked(latest_seen, latest_seen ? parse_version(*latest_seen) : std::nullopt),
            .last_failure = checked(last_failure, last_failure ? parse_utc(*last_failure) : std::nullopt),
        };
        return valid ? state : UpdateState{};
    } catch (const json::Error&) {
        return {};
    }
}

std::string UpdateState::to_json() const
{
    json::Writer writer{json::Layout::indented};
    writer.begin_object();
    if (last_success) {
        writer.key(kLastSuccessKey).string(format_iso_utc(*last_success));
    }
    if (latest_seen) {
        writer.key(kLatestSeenKey).string(latest_seen->to_string());
    }
    if (last_failure) {
        writer.key(kLastFailureKey).string(format_iso_utc(*last_failure));
    }
    writer.end_object();
    return writer.take();
}

bool is_check_due(const UpdateState& state, UtcInstant now) noexcept
{
    // Times in the future (the clock was set back) are not trusted.
    if (state.last_failure && *state.last_failure <= now && now - *state.last_failure < kFailureRetryInterval) {
        return false;
    }
    if (!state.last_success || *state.last_success > now) {
        return true;
    }
    return now - *state.last_success >= kCheckInterval;
}

UpdateSchedule::UpdateSchedule(std::filesystem::path updates_dir, std::shared_ptr<const Clock> clock)
    : store_(std::move(updates_dir)), clock_(std::move(clock))
{
    if (!clock_) {
        throw std::invalid_argument("UpdateSchedule needs a clock");
    }
}

UpdateState UpdateSchedule::load() const
{
    // No maximum age: the clock is not consulted for the read itself.
    const auto text = store_.try_read(kStateFileName, std::nullopt, *clock_);
    return text ? UpdateState::parse(*text) : UpdateState{};
}

bool UpdateSchedule::is_check_due() const
{
    return update::is_check_due(load(), clock_->utc_now());
}

void UpdateSchedule::record_success(std::optional<Version> latest_seen) const
{
    UpdateState state = load();
    state.last_success = clock_->utc_now();
    state.latest_seen = latest_seen;
    state.last_failure.reset();
    save(state);
}

void UpdateSchedule::record_failure() const
{
    UpdateState state = load();
    state.last_failure = clock_->utc_now();
    save(state);
}

void UpdateSchedule::record(const std::expected<ReleaseInfo, UpdateError>& result) const
{
    if (result) {
        record_success(result->version);
    } else if (result.error().kind == UpdateErrorKind::no_release) {
        record_success(std::nullopt);
    } else {
        record_failure();
    }
}

void UpdateSchedule::save(const UpdateState& state) const
{
    store_.try_write(kStateFileName, state.to_json());
}

} // namespace wizz::update
