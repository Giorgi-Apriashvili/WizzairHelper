#include "support/fake_transport.hpp"

#include "wizz/core/cancellation.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

namespace wizz::test {

namespace {

[[nodiscard]] constexpr char ascii_lower(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool equals_ignore_ascii_case(std::string_view a, std::string_view b) noexcept
{
    return std::ranges::equal(a, b, {}, ascii_lower, ascii_lower);
}

[[nodiscard]] ScriptedResponse make_response(int status, std::optional<std::string> content_type, std::string body)
{
    return ScriptedResponse{
        .response = http::Response{.status = status,
                                   .headers = {},
                                   .content_type = std::move(content_type),
                                   .body = std::move(body)},
        .body_error = std::nullopt,
    };
}

} // namespace

std::vector<std::string> RecordedRequest::header_values(std::string_view name) const
{
    std::vector<std::string> values;
    for (const auto& [header_name, value] : headers) {
        if (equals_ignore_ascii_case(header_name, name)) {
            values.push_back(value);
        }
    }
    return values;
}

ScriptedResponse ScriptedResponse::with_header(std::string name, std::string value) const
{
    ScriptedResponse copy = *this;
    copy.response.headers.emplace_back(std::move(name), std::move(value));
    return copy;
}

ScriptedResponse json_response(std::string body, int status)
{
    return make_response(status, "application/json; charset=utf-8", std::move(body));
}

ScriptedResponse html_response(std::string body, int status)
{
    return make_response(status, "text/html; charset=utf-8", std::move(body));
}

ScriptedResponse string_content_response(int status)
{
    return make_response(status, "text/plain; charset=utf-8", {});
}

ScriptedResponse bare_response(int status)
{
    return make_response(status, std::nullopt, {});
}

ScriptedResponse broken_stream_response()
{
    ScriptedResponse broken = make_response(200, "application/json", std::string{kBrokenStreamPrefix});
    broken.body_error = std::string{kBrokenStreamError};
    return broken;
}

FakeTransport::FakeTransport(Responder responder) : responder_(std::move(responder)) {}

http::Response FakeTransport::send(const http::Request& request, std::stop_token stop)
{
    RecordedRequest record{
        .method = request.method,
        .url = request.url,
        .body = request.body,
        .headers = request.headers,
        .content_type = request.content_type,
    };
    {
        const std::scoped_lock lock{mutex_};
        requests_.push_back(record);
    }
    throw_if_cancelled(stop);
    ScriptedResponse scripted = responder_(record);
    if (scripted.body_error) {
        throw http::TransportError{*scripted.body_error};
    }
    return std::move(scripted.response);
}

std::vector<RecordedRequest> FakeTransport::requests() const
{
    const std::scoped_lock lock{mutex_};
    return requests_;
}

std::size_t FakeTransport::request_count() const
{
    const std::scoped_lock lock{mutex_};
    return requests_.size();
}

std::vector<std::string> FakeTransport::urls() const
{
    const std::scoped_lock lock{mutex_};
    return requests_ | std::views::transform(&RecordedRequest::url) | std::ranges::to<std::vector>();
}

std::size_t FakeTransport::count_urls_containing(std::string_view fragment) const
{
    const std::scoped_lock lock{mutex_};
    return static_cast<std::size_t>(std::ranges::count_if(
        requests_, [fragment](const RecordedRequest& r) { return r.url.find(fragment) != std::string::npos; }));
}

} // namespace wizz::test
