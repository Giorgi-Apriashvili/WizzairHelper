#include "wizz/core/currency_format.hpp"

#include <algorithm>
#include <iterator>

namespace wizz::currency_format {
namespace {

constexpr char ascii_upper(char ch) noexcept
{
    return ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch;
}

/// StringComparison.OrdinalIgnoreCase for ASCII codes (.NET folds no non-ASCII
/// letter onto these, e.g. "ısk" != "ISK").
constexpr bool equals_ignore_case(std::string_view a, std::string_view b) noexcept
{
    return std::ranges::equal(a, b, {}, ascii_upper, ascii_upper);
}

} // namespace

int decimals(std::string_view code) noexcept
{
    const bool no_minor_units =
        std::ranges::any_of(kNoMinorUnits, [code](std::string_view c) { return equals_ignore_case(c, code); });
    return no_minor_units ? 0 : 2;
}

std::string_view number_format(std::string_view code) noexcept
{
    return decimals(code) == 0 ? "#,##0" : "#,##0.00";
}

std::string format(const Decimal& amount, std::string_view code, const NumberSymbols& symbols)
{
    std::string text = amount.format(number_format(code), symbols);
    text += ' ';
    std::ranges::transform(code, std::back_inserter(text), ascii_upper);
    return text;
}

} // namespace wizz::currency_format
