#pragma once
// CurrencyFormat (03 §6.4, 02 §2.7): display decimals per currency and the
// money text used by the grids, summaries and Money.ToString(). The decimals
// are the app's own rule, deliberately not ISO 4217 / CLDR (which give HUF 2).

#include "wizz/core/decimal.hpp"
#include "wizz/core/locale.hpp"

#include <array>
#include <string>
#include <string_view>

namespace wizz::currency_format {

/// Currencies shown without minor units (`NoMinorUnits`, compared ASCII case-insensitively).
inline constexpr std::array<std::string_view, 7> kNoMinorUnits{"HUF", "ISK", "JPY", "KRW", "UZS", "KZT", "AMD"};

/// 0 for the `kNoMinorUnits` codes (any letter case), otherwise 2 — including
/// unknown and empty codes. Re-pricing rounds per person to this many places (03 §6.2).
[[nodiscard]] int decimals(std::string_view code) noexcept;

/// "#,##0" for 0 decimals, "#,##0.00" otherwise (the grids' cell format).
[[nodiscard]] std::string_view number_format(std::string_view code) noexcept;

/// amount.ToString(number_format(code), culture) + " " + upper(code), e.g.
/// "1,234.50 EUR", "19,290 HUF", "-2.35 EUR", "0.00 EUR" for -0.004 (no minus
/// sign once rounded to zero). Rounds half away from zero; separators and the
/// minus sign come from `symbols`. The code is upper-cased in ASCII only
/// (.NET ToUpperInvariant would also upper-case non-ASCII letters; real codes
/// are ASCII).
[[nodiscard]] std::string format(const Decimal& amount, std::string_view code, const NumberSymbols& symbols);

} // namespace wizz::currency_format
