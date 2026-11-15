// Wizz Air protocol text: API-URL discovery scanner, strict cache check,
// timetableV2 body and parser, validation codes, classifier helpers
// (04 §7.3, §7.4, §9.3, §9.4, §11.5).

#include "wizz/services/wizz_protocol.hpp"

#include "wizz/core/datetime_parse.hpp"
#include "wizz/core/decimal.hpp"
#include "wizz/core/json.hpp"
#include "wizz/core/text.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace wizz::protocol {

namespace {

constexpr std::string_view kApiHost = "be.wizzair.com";

// ---------------------------------------------------------------------------
// Character classes of the .NET regexes (04 §7.3)
// ---------------------------------------------------------------------------

struct CodePoint {
    char32_t value = text::kReplacementCharacter;
    std::size_t length = 1;
};

[[nodiscard]] constexpr bool is_continuation(unsigned char byte) noexcept
{
    return (byte & 0xC0u) == 0x80u;
}

/// The code point starting at `s[i]` (`i < s.size()`). An ill-formed sequence reads
/// as one U+FFFD byte, which no pattern element matches — exactly what the
/// replacement character the .NET string would hold there does.
[[nodiscard]] constexpr CodePoint decode_at(std::string_view s, std::size_t i) noexcept
{
    const auto lead = static_cast<unsigned char>(s[i]);
    if (lead < 0x80u) {
        return {lead, 1};
    }
    std::size_t length = 0;
    char32_t value = 0;
    char32_t minimum = 0;
    if (lead >= 0xC2u && lead <= 0xDFu) {
        length = 2;
        value = lead & 0x1Fu;
        minimum = 0x80;
    } else if (lead >= 0xE0u && lead <= 0xEFu) {
        length = 3;
        value = lead & 0x0Fu;
        minimum = 0x800;
    } else if (lead >= 0xF0u && lead <= 0xF4u) {
        length = 4;
        value = lead & 0x07u;
        minimum = 0x10000;
    } else {
        return {};
    }
    if (s.size() - i < length) {
        return {};
    }
    for (std::size_t k = 1; k < length; ++k) {
        const auto byte = static_cast<unsigned char>(s[i + k]);
        if (!is_continuation(byte)) {
            return {};
        }
        value = (value << 6) | (byte & 0x3Fu);
    }
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        return {};
    }
    return {value, length};
}

/// .NET regex `\d`: a UTF-16 code unit of category Nd. Supplementary digits are
/// surrogate pairs in .NET and never match.
[[nodiscard]] bool is_regex_digit(char32_t c) noexcept
{
    if (c < 0x80) {
        return c >= U'0' && c <= U'9';
    }
    return c <= 0xFFFF && text::is_digit(c);
}

/// .NET regex `\s` = `char.IsWhiteSpace` (`[\f\n\r\t\v\x85\p{Z}]`), all in the BMP.
[[nodiscard]] constexpr bool is_regex_space(char32_t c) noexcept
{
    return text::is_white_space(c);
}

/// A forward-only matcher over one candidate position. Every step either consumes
/// what the pattern element requires and returns true, or returns false.
class Cursor {
public:
    constexpr Cursor(std::string_view s, std::size_t pos) noexcept : s_(s), pos_(pos) {}

    [[nodiscard]] constexpr std::size_t pos() const noexcept { return pos_; }

    /// A literal, case-sensitive.
    [[nodiscard]] constexpr bool literal(std::string_view expected) noexcept
    {
        if (s_.substr(pos_).starts_with(expected)) {
            pos_ += expected.size();
            return true;
        }
        return false;
    }

    /// `c?`
    constexpr void optional(char c) noexcept
    {
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
        }
    }

    /// `\s*`
    constexpr void skip_spaces() noexcept
    {
        while (pos_ < s_.size()) {
            const CodePoint cp = decode_at(s_, pos_);
            if (!is_regex_space(cp.value)) {
                return;
            }
            pos_ += cp.length;
        }
    }

    /// `(?:\\?/)`
    [[nodiscard]] constexpr bool slash() noexcept
    {
        optional('\\');
        return literal("/");
    }

    /// `\d+(?:\.\d+)+`
    [[nodiscard]] bool version() noexcept
    {
        if (!digits()) {
            return false;
        }
        int groups = 0;
        while (pos_ < s_.size() && s_[pos_] == '.' && digit_at(pos_ + 1)) {
            ++pos_;
            (void)digits();
            ++groups;
        }
        return groups > 0;
    }

private:
    [[nodiscard]] bool digit_at(std::size_t i) const noexcept
    {
        return i < s_.size() && is_regex_digit(decode_at(s_, i).value);
    }

    /// `\d+`
    [[nodiscard]] bool digits() noexcept
    {
        const std::size_t start = pos_;
        while (pos_ < s_.size()) {
            const CodePoint cp = decode_at(s_, pos_);
            if (!is_regex_digit(cp.value)) {
                break;
            }
            pos_ += cp.length;
        }
        return pos_ > start;
    }

    std::string_view s_;
    std::size_t pos_;
};

/// `group1.Replace("\\/", "/")`.
[[nodiscard]] std::string unescape_slashes(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '/') {
            continue;  // the '/' is appended on the next iteration
        }
        out.push_back(s[i]);
    }
    return out;
}

/// `ApiUrlConfigRegex`: the first match in document order (every match starts at "apiUrl").
[[nodiscard]] std::optional<std::string> match_primary(std::string_view html)
{
    constexpr std::string_view kKey = "apiUrl";
    for (std::size_t p = html.find(kKey); p != std::string_view::npos; p = html.find(kKey, p + 1)) {
        Cursor c{html, p + kKey.size()};
        c.optional('\\');
        c.optional('"');
        c.skip_spaces();
        if (!c.literal(":")) {
            continue;
        }
        c.skip_spaces();
        c.optional('\\');
        if (!c.literal("\"")) {
            continue;
        }
        const std::size_t start = c.pos();
        if (c.literal("https:") && c.slash() && c.slash() && c.literal(kApiHost) && c.slash() && c.version() &&
            c.slash() && c.literal("Api")) {
            return unescape_slashes(html.substr(start, c.pos() - start));
        }
    }
    return std::nullopt;
}

/// `ApiUrlAnyRegex` over all non-overlapping matches, highest `System.Version` first,
/// the earliest match on ties (stable `OrderByDescending(...).FirstOrDefault()`).
[[nodiscard]] std::optional<std::string> match_fallback(std::string_view html)
{
    std::optional<SystemVersion> best;
    std::string_view best_text;
    std::size_t q = html.find(kApiHost);
    while (q != std::string_view::npos) {
        Cursor c{html, q + kApiHost.size()};
        std::size_t next = q + 1;
        if (c.slash()) {
            const std::size_t start = c.pos();
            if (c.version()) {
                const std::string_view captured = html.substr(start, c.pos() - start);
                if (c.slash() && c.literal("Api")) {
                    next = c.pos();  // matches never overlap
                    if (const std::optional<SystemVersion> version = SystemVersion::try_parse(captured);
                        version && (!best || *version > *best)) {
                        best = version;
                        best_text = captured;
                    }
                }
            }
        }
        q = html.find(kApiHost, next);
    }
    if (!best) {
        return std::nullopt;
    }
    std::string url{"https://be.wizzair.com/"};
    url += best_text;
    url += "/Api";
    return url;
}

// ---------------------------------------------------------------------------
// System.Version
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr bool is_ascii_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

/// .NET number-parsing white space (`Number.IsWhite`): U+0009–U+000D and U+0020.
[[nodiscard]] constexpr bool is_number_white(char c) noexcept
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

/// `int.TryParse(s, NumberStyles.Integer, InvariantCulture, out v) && v >= 0`
/// (Version.TryParseComponent). Like .NET, trailing NUL characters are tolerated.
[[nodiscard]] constexpr std::optional<std::int32_t> parse_version_component(std::string_view s) noexcept
{
    std::size_t i = 0;
    while (i < s.size() && is_number_white(s[i])) {
        ++i;
    }
    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        negative = s[i] == '-';
        ++i;
    }
    if (i == s.size() || !is_ascii_digit(s[i])) {
        return std::nullopt;
    }
    constexpr std::int64_t kMax = std::numeric_limits<std::int32_t>::max();
    std::int64_t value = 0;
    for (; i < s.size() && is_ascii_digit(s[i]); ++i) {
        value = value * 10 + (s[i] - '0');
        if (value > kMax + 1) {
            return std::nullopt;  // overflow (or, negated, below Int32.MinValue)
        }
    }
    while (i < s.size() && is_number_white(s[i])) {
        ++i;
    }
    while (i < s.size() && s[i] == '\0') {
        ++i;
    }
    if (i != s.size()) {
        return std::nullopt;
    }
    if (negative) {
        value = -value;
    }
    if (value < 0 || value > kMax) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(value);
}

// ---------------------------------------------------------------------------
// Timetable rows (04 §9.4)
// ---------------------------------------------------------------------------

/// `TryDate(e, n)`: the member is a JSON string that `TryParseLocal` accepts.
[[nodiscard]] std::optional<LocalDateTime> try_date(const json::Value& e, std::string_view name)
{
    const std::optional<std::string_view> value = e.get_string(name);
    return value ? parse_local_datetime(*value) : std::nullopt;
}

[[nodiscard]] bool is_literally_true(const json::Value& e, std::string_view name)
{
    const json::Value* value = e.find(name);
    return value != nullptr && value->is_true();
}

/// `JsonElement.GetDecimal()`: exact, scale kept; FormatException when it does not fit.
[[nodiscard]] Decimal decimal_of(const json::Value& number)
{
    const std::expected<Decimal, DecimalParseError> value = Decimal::from_json_number(number.number_lexeme());
    if (!value) {
        throw json::FormatError("One of the identified items was in an invalid format.");
    }
    return *value;
}

[[nodiscard]] std::optional<DayFare> parse_row(const json::Value& f)
{
    // Str() throws json::TypeError when `f` is not an object (InvalidOperationException).
    const std::optional<std::string_view> from = f.get_string("departureStation");
    const std::optional<std::string_view> to = f.get_string("arrivalStation");
    if (!from || !to) {
        return std::nullopt;
    }
    const std::optional<LocalDateTime> departure = try_date(f, "departureDate");
    if (!departure) {
        return std::nullopt;
    }

    Decimal amount;
    std::string_view currency;
    if (const json::Value* price = f.find("price"); price != nullptr && price->is_object()) {
        if (const json::Value* value = price->find("amount"); value != nullptr && value->is_number()) {
            amount = decimal_of(*value);
        }
        currency = price->get_string("currencyCode").value_or(std::string_view{});
    }

    const std::string_view price_type = f.get_string("priceType").value_or(std::string_view{});
    FareStatus status = FareStatus::available;
    if (text::ordinal_ignore_case_equals(price_type, "checkPrice")) {
        status = FareStatus::check_on_site;
    } else if (amount.signum() <= 0) {
        return std::nullopt;  // 0, negative, missing price, non-numeric amount
    }

    std::vector<LocalDateTime> departures;
    std::optional<LocalDateTime> flagged;
    if (const json::Value* dates = f.find("departureDates"); dates != nullptr && dates->is_array()) {
        for (const json::Value& d : dates->as_array()) {
            if (d.is_string()) {
                if (const std::optional<LocalDateTime> legacy = parse_local_datetime(d.as_string())) {
                    departures.push_back(*legacy);  // legacy V1 plain string
                }
            } else if (d.is_object()) {
                if (const std::optional<LocalDateTime> entry = try_date(d, "date")) {
                    departures.push_back(*entry);
                    if (is_literally_true(d, "isCheapestOfTheDay")) {
                        flagged = entry;  // the last flagged entry wins
                    }
                }
            }
        }
    }
    std::ranges::sort(departures);

    LocalDateTime cheapest = *departure;
    if (time_of_day(*departure) == Ticks::zero() && !departures.empty()) {
        cheapest = flagged.value_or(departures.front());  // V1 puts midnight in departureDate
    }

    DayFare fare;
    fare.from = text::to_upper_invariant(*from);
    fare.to = text::to_upper_invariant(*to);
    fare.date = date_of(*departure);
    fare.cheapest_departure = cheapest;
    fare.departures = std::move(departures);
    fare.price_per_person = Money{status == FareStatus::available ? amount : Decimal{},
                                  text::to_upper_invariant(currency)};
    fare.status = status;
    fare.has_mac_flight = is_literally_true(f, "hasMacFlight");
    return fare;
}

[[nodiscard]] std::optional<json::Value> try_parse_json(std::string_view body)
{
    try {
        return json::parse(body);
    } catch (const json::ParseError&) {
        return std::nullopt;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::optional<SystemVersion> SystemVersion::try_parse(std::string_view text) noexcept
{
    constexpr auto npos = std::string_view::npos;
    const std::size_t major_end = text.find('.');
    if (major_end == npos) {
        return std::nullopt;
    }
    std::size_t minor_end = text.find('.', major_end + 1);
    std::size_t build_end = npos;
    if (minor_end != npos) {
        build_end = text.find('.', minor_end + 1);
        if (build_end != npos && text.find('.', build_end + 1) != npos) {
            return std::nullopt;  // five or more components
        }
    }

    std::array<std::string_view, 4> fields{};
    std::size_t count = 0;
    std::size_t start = 0;
    for (const std::size_t end : {major_end, minor_end, build_end, npos}) {
        if (end == npos) {
            fields[count++] = text.substr(start);
            break;
        }
        fields[count++] = text.substr(start, end - start);
        start = end + 1;
    }

    std::array<std::int32_t, 4> parts{-1, -1, -1, -1};
    for (std::size_t k = 0; k < count; ++k) {
        const std::optional<std::int32_t> part = parse_version_component(fields[k]);
        if (!part) {
            return std::nullopt;
        }
        parts[k] = *part;
    }
    return SystemVersion{parts};
}

std::string SystemVersion::to_string() const
{
    std::string out = std::to_string(major()) + '.' + std::to_string(minor());
    if (build() >= 0) {
        out += '.' + std::to_string(build());
        if (revision() >= 0) {
            out += '.' + std::to_string(revision());
        }
    }
    return out;
}

std::optional<std::string> extract_api_url(std::string_view html)
{
    if (std::optional<std::string> url = match_primary(html)) {
        return url;
    }
    return match_fallback(html);
}

bool is_valid_cached_api_url(std::string_view trimmed) noexcept
{
    constexpr std::string_view kPrefix = "https://be.wizzair.com/";
    constexpr std::string_view kSuffix = "/Api";
    if (!trimmed.starts_with(kPrefix) || !trimmed.ends_with(kSuffix) ||
        trimmed.size() < kPrefix.size() + kSuffix.size()) {
        return false;
    }
    // \d+(\.\d+)+ over ASCII digits, nothing else.
    const std::string_view version = trimmed.substr(kPrefix.size(), trimmed.size() - kPrefix.size() - kSuffix.size());
    int groups = 0;
    bool in_digits = false;
    for (const char c : version) {
        if (is_ascii_digit(c)) {
            in_digits = true;
        } else if (c == '.' && in_digits) {
            in_digits = false;
            ++groups;
        } else {
            return false;
        }
    }
    return in_digits && groups > 0;
}

std::string build_timetable_body(std::string_view from, std::string_view to, Date start, Date end,
                                 PriceType price_type, int adults)
{
    json::Writer writer;
    writer.begin_object()
        .key("flightList")
        .begin_array()
        .begin_object()
        .key("departureStation")
        .string(text::to_upper_invariant(from))
        .key("arrivalStation")
        .string(text::to_upper_invariant(to))
        .key("from")
        .string(start.iso())
        .key("to")
        .string(end.iso())
        .end_object()
        .end_array()
        .key("priceType")
        .string(price_type == PriceType::wizz_discount_club ? "wdc" : "regular")
        .key("adultCount")
        .integer(std::max(1, adults))
        .key("childCount")
        .integer(0)
        .key("infantCount")
        .integer(0)
        .end_object();
    return writer.take();
}

std::vector<DayFare> parse_timetable(std::string_view body)
{
    const json::Value root = json::parse(body);
    const json::Value* flights = root.find("outboundFlights");  // json::TypeError unless an object
    std::vector<DayFare> rows;
    if (flights == nullptr || !flights->is_array()) {
        return rows;
    }
    for (const json::Value& f : flights->as_array()) {
        if (std::optional<DayFare> fare = parse_row(f)) {
            rows.push_back(std::move(*fare));
        }
    }
    return rows;
}

std::vector<std::string> parse_validation_codes(std::string_view body)
{
    std::vector<std::string> codes;
    const std::optional<json::Value> root = try_parse_json(body);
    if (!root || !root->is_object()) {
        return codes;
    }
    // A body whose text cannot be read (e.g. a lone-surrogate escape, DECISIONS
    // D-28) counts as "no codes": the response then classifies as an ordinary
    // 400 WizzApiError instead of escaping the client (D-29).
    try {
        const json::Value* list = root->find("validationCodes");
        if (list == nullptr || !list->is_array()) {
            return codes;
        }
        for (const json::Value& code : list->as_array()) {
            if (code.is_string()) {
                codes.emplace_back(code.as_string());
            }
        }
    } catch (const json::Error&) {
        return {};
    }
    return codes;
}

bool looks_like_json(std::string_view body) noexcept
{
    const std::string_view rest = text::trim_start(body);
    return !rest.empty() && (rest.front() == '{' || rest.front() == '[');
}

std::string snippet(std::string_view body)
{
    if (text::utf16_length(body) <= kSnippetLength) {
        return std::string{body};
    }
    // Every UTF-16 code unit comes from at most 3 UTF-8 bytes, so the first 200
    // units lie within the first 600 bytes; decoding a longer prefix is exact
    // for them even if the prefix ends inside a character.
    std::u16string head = text::utf8_to_utf16(body.substr(0, std::min(body.size(), kSnippetLength * 3 + 4)));
    std::size_t length = kSnippetLength;
    if (const char16_t last = head[length - 1]; last >= 0xD800 && last <= 0xDBFF) {
        --length;  // never split a surrogate pair
    }
    head.resize(length);
    return text::utf16_to_utf8(head) + "…";
}

} // namespace wizz::protocol
