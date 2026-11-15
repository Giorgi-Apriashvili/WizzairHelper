#pragma once
// Bytes-to-text decoding shared by HTTP bodies (04 §2.2, .NET ReadAsStringAsync)
// and cache files (04 §6, .NET File.ReadAllText). Both pick the encoding from an
// explicit charset or from a byte-order mark, default to UTF-8 and replace
// ill-formed input with U+FFFD instead of failing.

#include <optional>
#include <string>
#include <string_view>

namespace wizz {

/// Decodes `bytes` to UTF-8 text the way .NET does.
///
/// - `charset` given (a .NET encoding name, ASCII case-insensitive, surrounding
///   blanks ignored): decode with that encoding after stripping *its own* byte-order
///   mark only. ISO-8859-1 and its aliases (`latin1`, `l1`, `cp819`, ...),
///   UTF-16LE/BE (`utf-16`, `unicode`, `utf-16be`, ...) and UTF-32LE/BE are
///   recognised; every other name, including unknown ones, decodes as UTF-8
///   (DECISIONS D-20; .NET would throw for unknown names).
/// - no charset: detect a UTF-8, UTF-16LE/BE or UTF-32LE/BE byte-order mark
///   (StreamReader rules), strip it and decode with that encoding; without a BOM
///   decode as UTF-8.
///
/// Ill-formed input becomes U+FFFD: one per maximal ill-formed subpart for UTF-8
/// (Unicode §3.9), one per unpaired surrogate or invalid code point for UTF-16/32
/// and one for trailing bytes that do not fill a code unit.
[[nodiscard]] std::string decode_text(std::string_view bytes, std::optional<std::string_view> charset = std::nullopt);

} // namespace wizz
