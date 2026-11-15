#include "wizz/core/json.hpp"

#include "json_grammar.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <limits>
#include <system_error>
#include <type_traits>
#include <utility>

namespace wizz::json {

namespace {

/// JsonValueKind name of the value's actual kind, as .NET prints it.
[[nodiscard]] std::string_view actual_kind_name(const Value& value) noexcept
{
    switch (value.kind()) {
    case Kind::null: return "Null";
    case Kind::boolean: return value.is_true() ? "True" : "False";
    case Kind::number: return "Number";
    case Kind::string: return "String";
    case Kind::array: return "Array";
    case Kind::object: return "Object";
    }
    return "Undefined";
}

[[noreturn]] void throw_type_error(std::string_view expected, const Value& actual)
{
    throw TypeError(std::format("The requested operation requires an element of type '{}', but the target "
                                "element has type '{}'.",
                                expected, actual_kind_name(actual)));
}

/// For a number that std::from_chars reported out of range: true when it is too
/// large for a double (|x| ≥ 1), false when it is too small (underflow towards 0).
[[nodiscard]] bool magnitude_at_least_one(std::string_view lexeme) noexcept
{
    const std::size_t exponent_mark = lexeme.find_first_of("eE");
    std::string_view mantissa = lexeme.substr(0, exponent_mark);
    if (mantissa.starts_with('-')) {
        mantissa.remove_prefix(1);
    }
    const std::size_t point = std::min(mantissa.find('.'), mantissa.size());
    const std::size_t first = mantissa.find_first_of("123456789");
    if (first == std::string_view::npos) {
        return false;  // zero never overflows
    }
    // Decimal exponent of the leading significant digit before applying "e…".
    const auto leading = first < point ? static_cast<std::int64_t>(point - first - 1)
                                       : -static_cast<std::int64_t>(first - point);

    std::int64_t exponent = 0;
    if (exponent_mark != std::string_view::npos) {
        std::string_view digits = lexeme.substr(exponent_mark + 1);
        const bool negative = digits.starts_with('-');
        if (digits.starts_with('-') || digits.starts_with('+')) {
            digits.remove_prefix(1);
        }
        // Saturate so that `leading + exponent` cannot overflow; only the sign matters.
        constexpr std::int64_t kSaturated = std::numeric_limits<std::int64_t>::max() / 4;
        if (std::from_chars(digits.data(), digits.data() + digits.size(), exponent).ec != std::errc{}) {
            exponent = kSaturated;
        }
        exponent = std::min(exponent, kSaturated);
        if (negative) {
            exponent = -exponent;
        }
    }
    return leading + exponent >= 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Value accessors
// ---------------------------------------------------------------------------

bool Value::is_true() const noexcept
{
    const bool* flag = std::get_if<bool>(&storage_);
    return flag != nullptr && *flag;
}

bool Value::as_bool() const
{
    const bool* flag = std::get_if<bool>(&storage_);
    if (flag == nullptr) {
        throw_type_error("Boolean", *this);
    }
    return *flag;
}

std::string_view Value::as_string() const&
{
    if (const auto* unreadable = std::get_if<UnreadableText>(&storage_)) {
        throw TypeError(unreadable->message);
    }
    const std::string* text = std::get_if<std::string>(&storage_);
    if (text == nullptr) {
        throw_type_error("String", *this);
    }
    return *text;
}

std::string_view Value::number_lexeme() const&
{
    const NumberText* number = std::get_if<NumberText>(&storage_);
    if (number == nullptr) {
        throw_type_error("Number", *this);
    }
    return number->lexeme;
}

double Value::as_double() const
{
    const std::string_view lexeme = number_lexeme();
    double result = 0.0;
    const std::errc ec = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), result).ec;
    if (ec == std::errc{}) {
        return result;
    }
    if (ec == std::errc::result_out_of_range && !magnitude_at_least_one(lexeme)) {
        return lexeme.starts_with('-') ? -0.0 : 0.0;
    }
    throw FormatError(std::format("The JSON number {} cannot be represented as a Double.", lexeme));
}

std::optional<std::int32_t> Value::try_int32() const noexcept
{
    const NumberText* number = std::get_if<NumberText>(&storage_);
    if (number == nullptr || number->lexeme.find_first_of(".eE") != std::string::npos) {
        return std::nullopt;
    }
    const std::string& lexeme = number->lexeme;
    std::int32_t result = 0;
    const auto [end, ec] = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), result);
    if (ec != std::errc{} || end != lexeme.data() + lexeme.size()) {
        return std::nullopt;
    }
    return result;
}

std::span<const Value> Value::as_array() const&
{
    const auto* items = std::get_if<std::vector<Value>>(&storage_);
    if (items == nullptr) {
        throw_type_error("Array", *this);
    }
    return *items;
}

const Value& Value::at(std::size_t index) const&
{
    const std::span<const Value> items = as_array();
    if (index >= items.size()) {
        throw std::out_of_range(std::format("JSON array index {} is out of range (size {}).", index, items.size()));
    }
    return items[index];
}

std::span<const Member> Value::as_object() const&
{
    const auto* members = std::get_if<std::vector<Member>>(&storage_);
    if (members == nullptr) {
        throw_type_error("Object", *this);
    }
    return *members;
}

const Value* Value::find(std::string_view name) const&
{
    // From the last member to the first, like TryGetProperty: the last duplicate wins.
    // name() throws at an unreadable name (D-28; .NET throws there only when the
    // escaped name is longer than `name` and agrees with it up to its first backslash).
    const std::span<const Member> members = as_object();
    for (auto member = members.rbegin(); member != members.rend(); ++member) {
        if (member->name() == name) {
            return &member->value();
        }
    }
    return nullptr;
}

std::string_view Member::name() const&
{
    return name_.as_string();
}

std::optional<std::string_view> Value::get_string(std::string_view name) const&
{
    const Value* member = find(name);
    if (member == nullptr || !member->is_string()) {
        return std::nullopt;
    }
    return member->as_string();
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

namespace detail {

/// Single-pass recursive-descent parser. Recursion is bounded by kMaxDepth.
class Parser {
public:
    explicit Parser(std::string_view text) noexcept : text_(text) {}

    [[nodiscard]] Value parse_document()
    {
        skip_whitespace();
        if (at_end()) {
            fail("The input does not contain any JSON tokens. Expected the input to start with a valid JSON "
                 "token, while the input is completed.");
        }
        Value root = parse_value();
        skip_whitespace();
        if (!at_end()) {
            fail(std::format("{} is invalid after a single JSON value. Expected end of data.", found()));
        }
        return root;
    }

private:
    using Storage = Value::Storage;

    // Value::kind() relies on the alternative order mirroring Kind.
    template <Kind K, typename T>
    static constexpr bool kAlternative = std::is_same_v<std::variant_alternative_t<static_cast<std::size_t>(K), Storage>, T>;
    static_assert(kAlternative<Kind::null, std::monostate> && kAlternative<Kind::boolean, bool> &&
                  kAlternative<Kind::number, Value::NumberText> && kAlternative<Kind::string, std::string> &&
                  kAlternative<Kind::array, std::vector<Value>> && kAlternative<Kind::object, std::vector<Member>>);

    [[nodiscard]] Value parse_value()
    {
        if (at_end()) {
            fail("Expected a JSON value, but instead reached end of data.");
        }
        switch (text_[pos_]) {
        case '{': return parse_object();
        case '[': return parse_array();
        case '"': return parse_string();
        case 't': return parse_literal("true", Value{Storage{std::in_place_type<bool>, true}});
        case 'f': return parse_literal("false", Value{Storage{std::in_place_type<bool>, false}});
        case 'n': return parse_literal("null", Value{});
        default: break;
        }
        if (text_[pos_] == '-' || is_digit(text_[pos_])) {
            return parse_number();
        }
        fail(std::format("{} is an invalid start of a value.", found()));
    }

    [[nodiscard]] Value parse_literal(std::string_view literal, Value value)
    {
        for (const char expected : literal) {
            if (at_end() || text_[pos_] != expected) {
                fail(std::format("Expected the JSON literal '{}', but found {}.", literal, found()));
            }
            ++pos_;
        }
        return value;
    }

    [[nodiscard]] Value parse_number()
    {
        const std::size_t start = pos_;
        const NumberScan scan = scan_number(text_, pos_);
        pos_ = scan.end;
        switch (scan.problem) {
        case NumberProblem::none: break;
        case NumberProblem::missing_integer_digit:
            fail(std::format("Expected a digit after the minus sign, but found {}.", found()));
        case NumberProblem::leading_zero: fail(std::format("Invalid leading zero before {}.", found()));
        case NumberProblem::missing_fraction_digit:
            fail(std::format("Expected a digit after the decimal point, but found {}.", found()));
        case NumberProblem::missing_exponent_digit:
            fail(std::format("Expected a digit in the exponent, but found {}.", found()));
        }
        return Value{Storage{std::in_place_type<Value::NumberText>, std::string{text_.substr(start, pos_ - start)}}};
    }

    [[nodiscard]] Value parse_array()
    {
        enter_container("array");
        std::vector<Value> items;
        skip_whitespace();
        if (consume(']')) {
            return leave_container(Storage{std::move(items)});
        }
        for (;;) {
            items.push_back(parse_value());
            skip_whitespace();
            if (consume(']')) {
                return leave_container(Storage{std::move(items)});
            }
            if (!consume(',')) {
                fail(std::format("Expected ',' or ']' after an array element, but found {}.", found()));
            }
            skip_whitespace();
            if (!at_end() && text_[pos_] == ']') {
                fail("The JSON array contains a trailing comma at the end which is not supported in this mode.");
            }
        }
    }

    [[nodiscard]] Value parse_object()
    {
        enter_container("object");
        std::vector<Member> members;
        skip_whitespace();
        if (consume('}')) {
            return leave_container(Storage{std::move(members)});
        }
        for (;;) {
            if (at_end() || text_[pos_] != '"') {
                fail(std::format("Expected a property name starting with '\"', but found {}.", found()));
            }
            Value name = parse_string();
            skip_whitespace();
            if (!consume(':')) {
                fail(std::format("Expected ':' after a property name, but found {}.", found()));
            }
            skip_whitespace();
            members.push_back(Member{std::move(name), parse_value()});
            skip_whitespace();
            if (consume('}')) {
                return leave_container(Storage{std::move(members)});
            }
            if (!consume(',')) {
                fail(std::format("Expected ',' or '}}' after a property value, but found {}.", found()));
            }
            skip_whitespace();
            if (!at_end() && text_[pos_] == '}') {
                fail("The JSON object contains a trailing comma at the end which is not supported in this mode.");
            }
        }
    }

    /// Reads a string token starting at its opening quote: the decoded text, or an
    /// UnreadableText carrying the first unpaired-surrogate problem (D-28).
    [[nodiscard]] Value parse_string()
    {
        ++pos_;  // opening quote
        std::string text;
        std::optional<std::string> unreadable;
        std::size_t run = pos_;  // start of the not yet copied verbatim bytes
        for (;;) {
            if (at_end()) {
                fail("Expected end of string, but instead reached end of data.");
            }
            const auto byte = static_cast<unsigned char>(text_[pos_]);
            if (byte == '"') {
                text.append(text_.substr(run, pos_ - run));
                ++pos_;
                if (unreadable) {
                    return Value{Storage{std::in_place_type<Value::UnreadableText>,
                                         Value::UnreadableText{std::move(*unreadable)}}};
                }
                return Value{Storage{std::in_place_type<std::string>, std::move(text)}};
            }
            if (byte == '\\') {
                text.append(text_.substr(run, pos_ - run));
                parse_escape(text, unreadable);
                run = pos_;
            } else if (byte < 0x20) {
                fail(std::format("{} is invalid within a JSON string. The string should be correctly escaped.",
                                 found()));
            } else if (byte < 0x80) {
                ++pos_;
            } else {
                const DecodedScalar scalar = decode_utf8(text_, pos_);
                if (!scalar.valid) {
                    fail(std::format("Invalid UTF-8 byte sequence starting with {} within a JSON string.",
                                     found()));
                }
                pos_ += scalar.length;
            }
        }
    }

    /// Decodes the escape sequence at `pos_` (the backslash) and appends it as UTF-8.
    /// An unpaired surrogate escape is valid JSON; it records in `unreadable` (first
    /// problem only) what JsonElement.GetString would throw (D-28).
    void parse_escape(std::string& text, std::optional<std::string>& unreadable)
    {
        ++pos_;  // backslash
        if (at_end()) {
            fail("Expected an escaped character, but instead reached end of data.");
        }
        const char kind = text_[pos_];
        switch (kind) {
        case '"': text += '"'; break;
        case '\\': text += '\\'; break;
        case '/': text += '/'; break;
        case 'b': text += '\b'; break;
        case 'f': text += '\f'; break;
        case 'n': text += '\n'; break;
        case 'r': text += '\r'; break;
        case 't': text += '\t'; break;
        case 'u': {
            ++pos_;
            const char32_t unit = parse_hex4();  // parse_hex4 advances past the digits
            if (unit < 0xD800 || unit > 0xDFFF) {
                append_utf8(text, unit);
                return;
            }
            // JsonReaderHelper.TryUnescape (verified on .NET 10): a low surrogate first is
            // invalid; a high one needs a following \u escape, which must be a low surrogate.
            if (unit >= 0xDC00) {
                note_unreadable(unreadable, unit);
                return;
            }
            if (text_.substr(pos_, 2) != "\\u") {
                note_unreadable(unreadable, std::nullopt);
                return;
            }
            pos_ += 2;
            const char32_t low = parse_hex4();  // still a ParseError when malformed
            if (low < 0xDC00 || low > 0xDFFF) {
                note_unreadable(unreadable, low);
                return;
            }
            append_utf8(text, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
            return;
        }
        default:
            fail(std::format("{} is not a valid escapable character within a JSON string. The string should be "
                             "correctly escaped.",
                             found()));
        }
        ++pos_;
    }

    /// Reads exactly four hex digits (either case) at `pos_`.
    [[nodiscard]] char32_t parse_hex4()
    {
        char32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            if (at_end()) {
                fail("Expected four hex digits after '\\u', but instead reached end of data.");
            }
            const char c = text_[pos_];
            char32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<char32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<char32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<char32_t>(c - 'A' + 10);
            } else {
                fail(std::format("{} is not a hex digit following '\\u' within a JSON string. The string should be "
                                 "correctly escaped.",
                                 found()));
            }
            value = (value << 4) | digit;
            ++pos_;
        }
        return value;
    }

    /// Keeps the first problem of a string: the missing low surrogate (`invalid_unit`
    /// nullopt) or the code unit .NET names as invalid (formatted "0x{:X2}").
    static void note_unreadable(std::optional<std::string>& unreadable, std::optional<char32_t> invalid_unit)
    {
        if (unreadable) {
            return;
        }
        unreadable = invalid_unit ? std::format("Cannot read invalid UTF-16 JSON text as string. Invalid surrogate "
                                                "value: '0x{:02X}'.",
                                                static_cast<std::uint32_t>(*invalid_unit))
                                  : std::string{"Cannot read incomplete UTF-16 JSON text as string with missing low "
                                                "surrogate."};
    }

    static void append_utf8(std::string& text, char32_t code_point)
    {
        const auto put = [&](std::uint32_t byte) { text += static_cast<char>(byte); };
        const auto cp = static_cast<std::uint32_t>(code_point);
        if (cp < 0x80) {
            put(cp);
        } else if (cp < 0x800) {
            put(0xC0 | (cp >> 6));
            put(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            put(0xE0 | (cp >> 12));
            put(0x80 | ((cp >> 6) & 0x3F));
            put(0x80 | (cp & 0x3F));
        } else {
            put(0xF0 | (cp >> 18));
            put(0x80 | ((cp >> 12) & 0x3F));
            put(0x80 | ((cp >> 6) & 0x3F));
            put(0x80 | (cp & 0x3F));
        }
    }

    /// Consumes the opening bracket of an array or object, enforcing kMaxDepth.
    void enter_container(std::string_view what)
    {
        if (depth_ >= kMaxDepth) {
            fail(std::format("CurrentDepth ({0}) is equal to or larger than the maximum allowed depth of {0}. "
                             "Cannot read next JSON {1}.",
                             kMaxDepth, what));
        }
        ++depth_;
        ++pos_;
    }

    [[nodiscard]] Value leave_container(Storage storage) noexcept
    {
        --depth_;
        return Value{std::move(storage)};
    }

    void skip_whitespace() noexcept
    {
        while (!at_end()) {
            const char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                return;
            }
            ++pos_;
        }
    }

    [[nodiscard]] bool consume(char expected) noexcept
    {
        if (at_end() || text_[pos_] != expected) {
            return false;
        }
        ++pos_;
        return true;
    }

    [[nodiscard]] bool at_end() const noexcept { return pos_ >= text_.size(); }

    /// The byte at the current position as .NET prints it ('x' or '0xNN'), or "end of data".
    [[nodiscard]] std::string found() const
    {
        if (at_end()) {
            return "end of data";
        }
        const auto byte = static_cast<unsigned char>(text_[pos_]);
        if (byte >= 0x20 && byte < 0x7F) {
            return std::format("'{}'", static_cast<char>(byte));
        }
        return std::format("'0x{:02X}'", byte);
    }

    [[noreturn]] void fail(std::string_view message) const
    {
        const std::string_view consumed = text_.substr(0, std::min(pos_, text_.size()));
        const auto line = std::ranges::count(consumed, '\n');
        const std::size_t line_start = consumed.rfind('\n');
        const std::size_t column = line_start == std::string_view::npos ? consumed.size()
                                                                        : consumed.size() - line_start - 1;
        throw ParseError(std::format("{} LineNumber: {} | BytePositionInLine: {}.", message, line, column));
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    int depth_ = 0;
};

} // namespace detail

Value parse(std::string_view utf8)
{
    return detail::Parser{utf8}.parse_document();
}

} // namespace wizz::json
