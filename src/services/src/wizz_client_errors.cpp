// Wizz Air client errors (04 §10).

#include "wizz/services/wizz_errors.hpp"

#include "wizz/core/text.hpp"

#include <algorithm>
#include <utility>

namespace wizz {

namespace {

[[nodiscard]] std::string validation_message(const std::vector<std::string>& codes)
{
    std::string message = "Wizz Air rejected the request: ";
    for (std::size_t i = 0; i < codes.size(); ++i) {
        if (i > 0) {
            message += ", ";
        }
        message += codes[i];
    }
    return message;
}

} // namespace

WizzValidationError::WizzValidationError(std::vector<std::string> codes)
    : WizzApiError(validation_message(codes), 400),
      codes_(std::make_shared<const std::vector<std::string>>(std::move(codes)))
{
}

bool WizzValidationError::has(std::string_view code) const noexcept
{
    return std::ranges::any_of(*codes_, [code](const std::string& c) { return text::ordinal_ignore_case_equals(c, code); });
}

} // namespace wizz
