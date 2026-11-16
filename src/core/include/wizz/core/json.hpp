#pragma once
// Strict JSON with System.Text.Json semantics.
//
// Reader (02 §5.1, 04 §9.4, 01 §13, 03 §7.2): `parse` accepts exactly what
// JsonDocument.Parse accepts with default options — RFC 8259 without comments or
// trailing commas, white space limited to SP HT LF CR, at most kMaxDepth nested
// arrays/objects, well-formed UTF-8, every escape (incl. \uXXXX surrogate pairs)
// decoded to UTF-8. Numbers keep their source lexeme, so money is parsed from the
// text and never passes through double (06 §3, 03 §7.1). Object lookup is ordinal
// and case-sensitive; for duplicate names the last member wins (04 §9.4).
//
// Unpaired surrogate escapes (D-28): the grammar allows them, so the document parses,
// but that one string cannot be read — as_string, get_string and Member::name throw
// TypeError with the text JsonElement.GetString throws (InvalidOperationException),
// and so does an object lookup that reaches a member name holding one.
//
// Writer (04 §9.3, 01 §13, 01 §17.3, 02 §6.9): Utf8JsonWriter / JsonSerializer
// defaults — strings escaped like JavaScriptEncoder.Default, compact or indented.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace wizz::json {

/// Maximum nesting of arrays and objects (JsonReaderOptions.MaxDepth default):
/// 64 nested containers parse, a 65th is a ParseError.
inline constexpr int kMaxDepth = 64;

/// Base of every JSON failure. Callers that treat "JsonException,
/// InvalidOperationException or FormatException" alike (02 §5.1, 04 §9.2) catch this.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Malformed JSON text (JsonException). The message ends with the .NET-style
/// position " LineNumber: <n> | BytePositionInLine: <m>." (both 0-based).
class ParseError : public Error {
public:
    using Error::Error;
};

/// An accessor was used on the wrong kind of value (InvalidOperationException), e.g.
/// "The requested operation requires an element of type 'Object', but the target
/// element has type 'Array'." (05 CT-SRCH-04), or the text read holds an unpaired
/// surrogate escape (D-28), e.g. "Cannot read incomplete UTF-16 JSON text as string
/// with missing low surrogate.".
class TypeError : public Error {
public:
    using Error::Error;
};

/// A number that does not fit the requested type (FormatException), e.g. 1e400 as a double.
class FormatError : public Error {
public:
    using Error::Error;
};

/// JSON value kinds. `true` and `false` share `boolean` (JsonValueKind True/False).
enum class Kind : std::uint8_t { null, boolean, number, string, array, object };

class Member;

namespace detail {
class Parser;
} // namespace detail

/// An immutable parsed JSON value; containers own their children and moving is O(1).
/// Accessors that return views (string_view, span, pointers) are deleted on
/// temporaries, so a view can never outlive the value it points into.
class Value {
public:
    /// The literal null.
    Value() noexcept = default;

    // Defined after Member is complete (below): a std::vector<Member> member may
    // only be copied or destroyed once Member is a complete type.
    Value(const Value& other);
    Value(Value&& other) noexcept;
    Value& operator=(const Value& other);
    Value& operator=(Value&& other) noexcept;
    ~Value();

    [[nodiscard]] Kind kind() const noexcept
    {
        return std::holds_alternative<UnreadableText>(storage_) ? Kind::string : static_cast<Kind>(storage_.index());
    }
    [[nodiscard]] bool is_null() const noexcept { return kind() == Kind::null; }
    [[nodiscard]] bool is_bool() const noexcept { return kind() == Kind::boolean; }
    [[nodiscard]] bool is_number() const noexcept { return kind() == Kind::number; }
    [[nodiscard]] bool is_string() const noexcept { return kind() == Kind::string; }
    [[nodiscard]] bool is_array() const noexcept { return kind() == Kind::array; }
    [[nodiscard]] bool is_object() const noexcept { return kind() == Kind::object; }
    /// True only for the literal `true` — "is literally true" in 02 §5.1 and 04 §9.4.
    [[nodiscard]] bool is_true() const noexcept;

    /// GetBoolean. TypeError unless a bool.
    [[nodiscard]] bool as_bool() const;

    /// GetString: the decoded UTF-8 text. TypeError unless a string, or when its escapes
    /// hold an unpaired surrogate (D-28).
    [[nodiscard]] std::string_view as_string() const&;
    std::string_view as_string() const&& = delete;

    /// The number exactly as written in the source ("30790.0", "1.5e2", "-0").
    /// TypeError unless a number.
    [[nodiscard]] std::string_view number_lexeme() const&;
    std::string_view number_lexeme() const&& = delete;

    /// GetDouble: the nearest double, locale-independent. TypeError unless a number;
    /// FormatError when the magnitude overflows a double (1e400). Underflow gives ±0.
    [[nodiscard]] double as_double() const;

    /// TryGetInt32 / JsonSerializer Int32 rules: the value when the lexeme is an
    /// integer without fraction or exponent inside int32 range ("250", "-0");
    /// nullopt otherwise — "250.5", "250.0", "2.5e2", out of range, or not a number.
    [[nodiscard]] std::optional<std::int32_t> try_int32() const noexcept;

    /// Array elements in source order. TypeError unless an array.
    [[nodiscard]] std::span<const Value> as_array() const&;
    std::span<const Value> as_array() const&& = delete;

    /// Array element `index`. TypeError unless an array; std::out_of_range past the end.
    [[nodiscard]] const Value& at(std::size_t index) const&;
    const Value& at(std::size_t index) const&& = delete;

    /// Every object member in source order, duplicates included. TypeError unless an object.
    [[nodiscard]] std::span<const Member> as_object() const&;
    std::span<const Member> as_object() const&& = delete;

    /// TryGetProperty: ordinal, case-sensitive lookup that scans from the last member to
    /// the first, so the last duplicate wins; nullptr when absent. TypeError unless an
    /// object, or when the scan reaches a member name holding an unpaired surrogate
    /// before a match (D-28).
    [[nodiscard]] const Value* find(std::string_view name) const&;
    const Value* find(std::string_view name) const&& = delete;

    /// Str(e, n) of 02 §5.1: the member's text when `name` exists and its (last) value
    /// is a JSON string; nullopt otherwise. TypeError unless an object, and when find()
    /// or reading that text does (D-28).
    [[nodiscard]] std::optional<std::string_view> get_string(std::string_view name) const&;
    std::optional<std::string_view> get_string(std::string_view name) const&& = delete;

private:
    friend class detail::Parser;

    struct NumberText {
        std::string lexeme;
    };
    /// A string whose escapes hold an unpaired surrogate (D-28): reading its text
    /// throws TypeError with `message`.
    struct UnreadableText {
        std::string message;
    };
    // Alternative order mirrors Kind; UnreadableText, last, is a second kind of string.
    using Storage = std::variant<std::monostate, bool, NumberText, std::string, std::vector<Value>, std::vector<Member>,
                                 UnreadableText>;

    explicit Value(Storage storage) noexcept;

    Storage storage_;
};

/// One object member as written in the source (JsonProperty).
class Member {
public:
    /// JsonProperty.Name: the decoded name. TypeError when its escapes hold an unpaired
    /// surrogate (D-28); the value stays readable.
    [[nodiscard]] std::string_view name() const&;
    std::string_view name() const&& = delete;

    /// JsonProperty.Value.
    [[nodiscard]] const Value& value() const& noexcept { return value_; }
    const Value& value() const&& = delete;

private:
    friend class detail::Parser;

    Member(Value name, Value value) noexcept : name_(std::move(name)), value_(std::move(value)) {}

    Value name_;  ///< always of kind string
    Value value_;
};

// Value's special members, now that Member is complete.
inline Value::Value(Storage storage) noexcept : storage_(std::move(storage)) {}
inline Value::Value(const Value& other) = default;
inline Value::Value(Value&& other) noexcept = default;
inline Value& Value::operator=(const Value& other) = default;
inline Value& Value::operator=(Value&& other) noexcept = default;
inline Value::~Value() = default;

/// Parses one UTF-8 JSON document (JsonDocument.Parse with default options).
/// Throws ParseError on anything RFC 8259 rejects, on nesting deeper than kMaxDepth,
/// on ill-formed UTF-8 or unescaped control characters in strings, and on any
/// non-white-space content after the root value. Unpaired surrogate escapes parse
/// (D-28). A byte order mark is not white space: decode bodies first
/// (http::decode_body_text strips it).
[[nodiscard]] Value parse(std::string_view utf8);

/// Writer layout. `compact` has no white space (JsonSerializer defaults, 04 §9.3).
/// `indented` (WriteIndented, 01 §13) puts each member and element on its own line,
/// indented two spaces per level, with "\r\n" line breaks (Environment.NewLine on
/// Windows) and ": " after keys; empty containers stay "[]" and "{}".
enum class Layout : std::uint8_t { compact, indented };

/// Builds one JSON document like Utf8JsonWriter. Every writing method returns *this
/// for chaining. Misuse — a value inside an object without a key, a key outside an
/// object, mismatched or missing ends, a second root value — throws std::logic_error.
///
/// Strings and keys are escaped like JavaScriptEncoder.Default: every non-ASCII
/// character and " & ' + < > ` DEL become \uXXXX with upper-case hex (characters
/// beyond U+FFFF as a surrogate pair), \b \t \n \f \r \\ use the short forms, other
/// control characters become \u00XX. Ill-formed UTF-8 is written as � per
/// maximal ill-formed subpart instead of failing.
class Writer {
public:
    explicit Writer(Layout layout = Layout::compact) noexcept : layout_(layout) {}

    Writer& begin_object();
    Writer& end_object();
    Writer& begin_array();
    Writer& end_array();

    /// Property name of the next value; only directly inside an object.
    Writer& key(std::string_view name);

    Writer& string(std::string_view value);
    /// A number written verbatim, e.g. a decimal's text "1.0" or "30790.0".
    /// std::invalid_argument unless it is a valid JSON number.
    Writer& number_lexeme(std::string_view lexeme);
    Writer& integer(std::int64_t value);
    /// Shortest text that round-trips (std::to_chars). std::invalid_argument for NaN or ±∞.
    Writer& number(double value);
    Writer& boolean(bool value);
    Writer& null();

    /// True once exactly one root value has been written completely.
    [[nodiscard]] bool complete() const noexcept { return has_root_; }
    /// The document text. std::logic_error while incomplete.
    [[nodiscard]] const std::string& str() const;
    /// Moves the document text out and resets the writer. std::logic_error while incomplete.
    [[nodiscard]] std::string take();

private:
    struct Frame {
        bool is_object = false;
        bool has_items = false;
        bool key_pending = false;
    };

    void begin_value();
    void end_value() noexcept;
    Writer& scalar(std::string_view token);
    Writer& open(bool is_object);
    Writer& close(bool is_object);
    void new_line(std::size_t depth);

    Layout layout_;
    std::string out_;
    std::vector<Frame> stack_;
    bool has_root_ = false;
};

} // namespace wizz::json
