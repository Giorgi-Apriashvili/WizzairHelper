#include "wizz/ui/replay_transport.hpp"

#include "wizz/core/cancellation.hpp"
#include "wizz/core/json.hpp"
#include "wizz/core/text_decoding.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <iterator>
#include <utility>

namespace wizz::ui {

namespace {

constexpr std::string_view kJsonContentType = "application/json; charset=utf-8";
constexpr std::string_view kHtmlContentType = "text/html; charset=utf-8";
constexpr std::string_view kTextContentType = "text/plain; charset=utf-8";
constexpr std::string_view kNotFoundBody = R"({"message":"No HTTP resource was found"})";

constexpr std::array<std::string_view, 15> kKnownFields{
    "method", "url",     "urlPrefix", "urlContains", "bodyContains", "status",  "file",  "body",
    "contentType", "headers", "delayMs", "error",  "message",      "times",   "comment"};

[[nodiscard]] std::string read_bytes(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw ReplayConfigError(std::format("Cannot read replay file {}.", path.string()));
    }
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view utf8)
{
    return std::filesystem::path{std::u8string_view{reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()}};
}

[[nodiscard]] std::string string_field(const json::Value& value, std::string_view name, std::size_t index)
{
    if (!value.is_string()) {
        throw ReplayConfigError(std::format("replay.json rule {}: \"{}\" must be a string.", index, name));
    }
    return std::string{value.as_string()};
}

[[nodiscard]] int int_field(const json::Value& value, std::string_view name, std::size_t index, int minimum)
{
    const std::optional<std::int32_t> number = value.try_int32();
    if (!number || *number < minimum) {
        throw ReplayConfigError(
            std::format("replay.json rule {}: \"{}\" must be an integer of at least {}.", index, name, minimum));
    }
    return *number;
}

[[nodiscard]] std::string default_content_type(const std::filesystem::path& file)
{
    std::string extension = file.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    if (extension == ".json") {
        return std::string{kJsonContentType};
    }
    if (extension == ".html" || extension == ".htm") {
        return std::string{kHtmlContentType};
    }
    return std::string{kTextContentType};
}

[[nodiscard]] ReplayTransport::Rule parse_rule(const json::Value& element, std::size_t index,
                                               const std::filesystem::path& directory)
{
    if (!element.is_object()) {
        throw ReplayConfigError(std::format("replay.json rule {} is not an object.", index));
    }
    for (const json::Member& member : element.as_object()) {
        if (std::ranges::find(kKnownFields, member.name()) == kKnownFields.end()) {
            throw ReplayConfigError(std::format("replay.json rule {}: unknown field \"{}\".", index, member.name()));
        }
    }
    ReplayTransport::Rule rule;
    const auto optional_string = [&](std::string_view name) -> std::optional<std::string> {
        const json::Value* value = element.find(name);
        return value ? std::optional<std::string>{string_field(*value, name, index)} : std::nullopt;
    };
    rule.method = optional_string("method");
    rule.url = optional_string("url");
    rule.url_prefix = optional_string("urlPrefix");
    rule.url_contains = optional_string("urlContains");
    rule.body_contains = optional_string("bodyContains");
    if (const json::Value* status = element.find("status")) {
        rule.status = int_field(*status, "status", index, 100);
    }
    const json::Value* file = element.find("file");
    const json::Value* body = element.find("body");
    if (file && body) {
        throw ReplayConfigError(std::format("replay.json rule {}: use either \"file\" or \"body\".", index));
    }
    std::string content_type{kJsonContentType};
    if (file) {
        const std::filesystem::path relative = path_from_utf8(string_field(*file, "file", index));
        rule.body = read_bytes(directory / relative);
        content_type = default_content_type(relative);
    } else if (body) {
        rule.body = string_field(*body, "body", index);
    }
    if (const json::Value* type = element.find("contentType")) {
        rule.content_type = type->is_null() ? std::nullopt
                                            : std::optional<std::string>{string_field(*type, "contentType", index)};
    } else {
        rule.content_type = std::move(content_type);
    }
    if (const json::Value* headers = element.find("headers")) {
        if (!headers->is_object()) {
            throw ReplayConfigError(std::format("replay.json rule {}: \"headers\" must be an object.", index));
        }
        for (const json::Member& header : headers->as_object()) {
            rule.headers.emplace_back(header.name(), string_field(header.value(), header.name(), index));
        }
    }
    if (const json::Value* delay = element.find("delayMs")) {
        rule.delay = std::chrono::milliseconds{int_field(*delay, "delayMs", index, 0)};
    }
    if (const std::optional<std::string> error = optional_string("error")) {
        if (*error == "transport") {
            rule.failure = ReplayTransport::Rule::Failure::transport;
        } else if (*error == "timeout") {
            rule.failure = ReplayTransport::Rule::Failure::timeout;
        } else {
            throw ReplayConfigError(
                std::format("replay.json rule {}: \"error\" must be \"transport\" or \"timeout\".", index));
        }
    }
    if (std::optional<std::string> message = optional_string("message")) {
        rule.failure_message = *std::move(message);
    }
    if (const json::Value* times = element.find("times")) {
        rule.times = static_cast<std::size_t>(int_field(*times, "times", index, 1));
    }
    return rule;
}

[[nodiscard]] std::vector<ReplayTransport::Rule> load_rules(const std::filesystem::path& directory)
{
    const std::filesystem::path manifest = directory / ReplayTransport::kManifestName;
    json::Value root;
    try {
        root = json::parse(decode_text(read_bytes(manifest)));
    } catch (const json::Error& error) {
        throw ReplayConfigError(std::format("{} is not valid JSON: {}", manifest.string(), error.what()));
    }
    try {
        const json::Value* rules = root.is_object() ? root.find("rules") : nullptr;
        if (!rules || !rules->is_array()) {
            throw ReplayConfigError(std::format("{} has no \"rules\" array.", manifest.string()));
        }
        std::vector<ReplayTransport::Rule> parsed;
        std::size_t index = 0;
        for (const json::Value& element : rules->as_array()) {
            parsed.push_back(parse_rule(element, index++, directory));
        }
        return parsed;
    } catch (const json::TypeError& error) {
        // A name or text holding an unpaired surrogate escape (D-28).
        throw ReplayConfigError(std::format("{} cannot be read: {}", manifest.string(), error.what()));
    }
}

} // namespace

ReplayTransport::ReplayTransport(const std::filesystem::path& directory) : ReplayTransport(load_rules(directory)) {}

ReplayTransport::ReplayTransport(std::vector<Rule> rules) : rules_(std::move(rules)), used_(rules_.size(), 0) {}

bool ReplayTransport::matches(const Rule& rule, const http::Request& request)
{
    if (rule.method && *rule.method != request.method) {
        return false;
    }
    if (rule.url && *rule.url != request.url) {
        return false;
    }
    if (rule.url_prefix && !request.url.starts_with(*rule.url_prefix)) {
        return false;
    }
    if (rule.url_contains && request.url.find(*rule.url_contains) == std::string::npos) {
        return false;
    }
    if (rule.body_contains && (!request.body || request.body->find(*rule.body_contains) == std::string::npos)) {
        return false;
    }
    return true;
}

http::Response ReplayTransport::send(const http::Request& request, std::stop_token stop)
{
    std::optional<std::size_t> chosen;
    {
        const std::scoped_lock lock{mutex_};
        urls_.push_back(request.url);
        for (std::size_t i = 0; i < rules_.size(); ++i) {
            const Rule& rule = rules_[i];
            if (rule.times && used_[i] >= *rule.times) {
                continue;
            }
            if (matches(rule, request)) {
                ++used_[i];
                chosen = i;
                break;
            }
        }
    }
    throw_if_cancelled(stop);
    if (!chosen) {
        return http::Response{.status = 404,
                              .headers = {},
                              .content_type = std::string{kJsonContentType},
                              .body = std::string{kNotFoundBody}};
    }
    const Rule& rule = rules_[*chosen];
    if (rule.delay.count() > 0) {
        cancellable_sleep(std::chrono::duration_cast<Duration>(rule.delay), stop);
    }
    switch (rule.failure) {
    case Rule::Failure::transport:
        throw http::TransportError(rule.failure_message);
    case Rule::Failure::timeout:
        throw http::TimeoutError(rule.failure_message);
    case Rule::Failure::none:
        break;
    }
    return http::Response{.status = rule.status, .headers = rule.headers, .content_type = rule.content_type, .body = rule.body};
}

std::vector<std::string> ReplayTransport::urls() const
{
    const std::scoped_lock lock{mutex_};
    return urls_;
}

std::size_t ReplayTransport::request_count() const
{
    const std::scoped_lock lock{mutex_};
    return urls_.size();
}

} // namespace wizz::ui
