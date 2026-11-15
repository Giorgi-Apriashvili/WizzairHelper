#include "wizz/core/json.hpp"

#include "json_grammar.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <utility>

namespace wizz::json {

namespace {

/// ASCII characters that JavaScriptEncoder.Default (via Utf8JsonWriter) escapes:
/// C0 controls, DEL, the HTML-sensitive " & ' + < > and backtick, and backslash.
constexpr std::array<bool, 128> kEscapedAscii = [] {
    std::array<bool, 128> table{};
    for (std::size_t c = 0; c < 0x20; ++c) {
        table[c] = true;
    }
    for (const char c : std::string_view{"\"&'+<>`\\"}) {
        table[static_cast<unsigned char>(c)] = true;
    }
    table[0x7F] = true;
    return table;
}();

/// Appends `\uXXXX` with upper-case hex for one UTF-16 code unit.
void append_unit_escape(std::string& out, std::uint32_t unit)
{
    constexpr std::string_view kHex = "0123456789ABCDEF";
    out += "\\u";
    for (int shift = 12; shift >= 0; shift -= 4) {
        out += kHex[(unit >> shift) & 0xFu];
    }
}

void append_ascii_escape(std::string& out, unsigned char c)
{
    switch (c) {
    case '\b': out += "\\b"; break;
    case '\t': out += "\\t"; break;
    case '\n': out += "\\n"; break;
    case '\f': out += "\\f"; break;
    case '\r': out += "\\r"; break;
    case '\\': out += "\\\\"; break;
    default: append_unit_escape(out, c); break;
    }
}

/// Appends a quoted, escaped JSON string (JavaScriptEncoder.Default rules).
void append_quoted(std::string& out, std::string_view text)
{
    out += '"';
    std::size_t run = 0;  // start of the not yet copied verbatim bytes
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto byte = static_cast<unsigned char>(text[pos]);
        if (byte < 0x80 && !kEscapedAscii[byte]) {
            ++pos;
            continue;
        }
        out.append(text.substr(run, pos - run));
        if (byte < 0x80) {
            append_ascii_escape(out, byte);
            ++pos;
        } else {
            const detail::DecodedScalar scalar = detail::decode_utf8(text, pos);
            const auto code_point = static_cast<std::uint32_t>(scalar.code_point);
            if (code_point > 0xFFFF) {
                const std::uint32_t offset = code_point - 0x10000;
                append_unit_escape(out, 0xD800 + (offset >> 10));
                append_unit_escape(out, 0xDC00 + (offset & 0x3FF));
            } else {
                append_unit_escape(out, code_point);
            }
            pos += scalar.length;
        }
        run = pos;
    }
    out.append(text.substr(run));
    out += '"';
}

} // namespace

Writer& Writer::begin_object()
{
    return open(true);
}

Writer& Writer::end_object()
{
    return close(true);
}

Writer& Writer::begin_array()
{
    return open(false);
}

Writer& Writer::end_array()
{
    return close(false);
}

Writer& Writer::key(std::string_view name)
{
    if (stack_.empty() || !stack_.back().is_object) {
        throw std::logic_error("JSON writer: a key is only valid directly inside an object.");
    }
    Frame& frame = stack_.back();
    if (frame.key_pending) {
        throw std::logic_error("JSON writer: the previous key has no value yet.");
    }
    if (frame.has_items) {
        out_ += ',';
    }
    frame.has_items = true;
    frame.key_pending = true;
    new_line(stack_.size());
    append_quoted(out_, name);
    out_ += layout_ == Layout::indented ? ": " : ":";
    return *this;
}

Writer& Writer::string(std::string_view value)
{
    begin_value();
    append_quoted(out_, value);
    end_value();
    return *this;
}

Writer& Writer::number_lexeme(std::string_view lexeme)
{
    if (!detail::is_number_lexeme(lexeme)) {
        throw std::invalid_argument(std::format("JSON writer: '{}' is not a valid JSON number.", lexeme));
    }
    return scalar(lexeme);
}

Writer& Writer::integer(std::int64_t value)
{
    std::array<char, 24> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return scalar(std::string_view{buffer.data(), result.ptr});
}

Writer& Writer::number(double value)
{
    if (!std::isfinite(value)) {
        throw std::invalid_argument("JSON writer: NaN and infinity are not valid JSON numbers.");
    }
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return scalar(std::string_view{buffer.data(), result.ptr});
}

Writer& Writer::boolean(bool value)
{
    return scalar(value ? "true" : "false");
}

Writer& Writer::null()
{
    return scalar("null");
}

const std::string& Writer::str() const
{
    if (!has_root_) {
        throw std::logic_error("JSON writer: the document is incomplete.");
    }
    return out_;
}

std::string Writer::take()
{
    if (!has_root_) {
        throw std::logic_error("JSON writer: the document is incomplete.");
    }
    std::string document = std::exchange(out_, {});
    has_root_ = false;
    return document;
}

void Writer::begin_value()
{
    if (stack_.empty()) {
        if (has_root_) {
            throw std::logic_error("JSON writer: the document already has a root value.");
        }
        return;
    }
    Frame& frame = stack_.back();
    if (frame.is_object) {
        if (!frame.key_pending) {
            throw std::logic_error("JSON writer: a value inside an object needs a key first.");
        }
        frame.key_pending = false;  // key() already wrote the separator and the indentation
        return;
    }
    if (frame.has_items) {
        out_ += ',';
    }
    frame.has_items = true;
    new_line(stack_.size());
}

void Writer::end_value() noexcept
{
    if (stack_.empty()) {
        has_root_ = true;
    }
}

Writer& Writer::scalar(std::string_view token)
{
    begin_value();
    out_ += token;
    end_value();
    return *this;
}

Writer& Writer::open(bool is_object)
{
    begin_value();
    out_ += is_object ? '{' : '[';
    stack_.push_back(Frame{.is_object = is_object});
    return *this;
}

Writer& Writer::close(bool is_object)
{
    if (stack_.empty() || stack_.back().is_object != is_object) {
        throw std::logic_error(std::format("JSON writer: {} does not match an open {}.",
                                           is_object ? "end_object" : "end_array", is_object ? "object" : "array"));
    }
    const Frame frame = stack_.back();
    if (frame.key_pending) {
        throw std::logic_error("JSON writer: the last key has no value.");
    }
    stack_.pop_back();
    if (frame.has_items) {
        new_line(stack_.size());
    }
    out_ += is_object ? '}' : ']';
    end_value();
    return *this;
}

void Writer::new_line(std::size_t depth)
{
    if (layout_ == Layout::indented) {
        out_ += "\r\n";
        out_.append(2 * depth, ' ');
    }
}

} // namespace wizz::json
