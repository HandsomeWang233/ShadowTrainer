// Implementation of json_min.hpp. Text-included by ui_bridge.cpp and by the
// tests that need to read a reply back; every out-of-class definition is inline
// so a second translation unit is not a duplicate-symbol error.
// Depends only on the standard library.
#include <cstdio>
#include <cwchar>
#include <iterator>

namespace ce::json {
namespace {

constexpr int max_depth = 32;

struct Parser {
    std::wstring_view text;
    size_t at = 0;
    bool ok = true;

    bool done() const noexcept { return at >= text.size(); }
    wchar_t peek() const noexcept { return at < text.size() ? text[at] : L'\0'; }

    void skip_space() noexcept {
        while (at < text.size()) {
            const wchar_t c = text[at];
            if (c == L' ' || c == L'\t' || c == L'\n' || c == L'\r') ++at;
            else break;
        }
    }

    bool literal(std::wstring_view word) noexcept {
        if (text.size() - at < word.size()) return false;
        if (text.compare(at, word.size(), word) != 0) return false;
        at += word.size();
        return true;
    }

    // \uXXXX, with a surrogate pair assembled the way UTF-16 expects: the two
    // escapes decode to two adjacent code units.
    bool hex4(wchar_t& out) noexcept {
        if (text.size() - at < 4) return false;
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const wchar_t c = text[at + static_cast<size_t>(i)];
            unsigned digit = 0;
            if (c >= L'0' && c <= L'9') digit = static_cast<unsigned>(c - L'0');
            else if (c >= L'a' && c <= L'f') digit = static_cast<unsigned>(c - L'a') + 10;
            else if (c >= L'A' && c <= L'F') digit = static_cast<unsigned>(c - L'A') + 10;
            else return false;
            value = value * 16 + digit;
        }
        at += 4;
        out = static_cast<wchar_t>(value);
        return true;
    }

    bool string_token(std::wstring& out) {
        if (peek() != L'"') return false;
        ++at;
        out.clear();
        while (true) {
            if (done()) return false;
            const wchar_t c = text[at++];
            if (c == L'"') return true;
            if (c != L'\\') { out += c; continue; }
            if (done()) return false;
            const wchar_t escape = text[at++];
            switch (escape) {
            case L'"': out += L'"'; break;
            case L'\\': out += L'\\'; break;
            case L'/': out += L'/'; break;
            case L'b': out += L'\b'; break;
            case L'f': out += L'\f'; break;
            case L'n': out += L'\n'; break;
            case L'r': out += L'\r'; break;
            case L't': out += L'\t'; break;
            case L'u': {
                wchar_t unit = 0;
                if (!hex4(unit)) return false;
                out += unit;
                break;
            }
            default: return false;
            }
        }
    }

    // A number token copied out so the CRT can parse it: wcstod needs a
    // terminator, which a view into the message does not have.
    bool number_token(double& out) noexcept {
        const size_t start = at;
        if (peek() == L'-') ++at;
        while (!done()) {
            const wchar_t c = peek();
            if ((c >= L'0' && c <= L'9') || c == L'.' || c == L'e' || c == L'E' ||
                c == L'+' || c == L'-') ++at;
            else break;
        }
        if (at == start) return false;
        wchar_t buffer[64]{};
        const size_t length = at - start;
        if (length >= std::size(buffer)) return false;
        for (size_t i = 0; i < length; ++i) buffer[i] = text[start + i];
        wchar_t* end = nullptr;
        const double parsed = std::wcstod(buffer, &end);
        if (end != buffer + length) return false;
        out = parsed;
        return true;
    }

    bool value(Value& out, int depth) {
        if (depth > max_depth) return false;
        skip_space();
        if (done()) return false;
        switch (peek()) {
        case L'{': {
            ++at;
            out = Value{};
            out.kind = Value::Kind::Object;
            skip_space();
            if (peek() == L'}') { ++at; return true; }
            while (true) {
                skip_space();
                std::wstring name;
                if (!string_token(name)) return false;
                skip_space();
                if (peek() != L':') return false;
                ++at;
                Value member;
                if (!value(member, depth + 1)) return false;
                out.members.emplace_back(std::move(name), std::move(member));
                skip_space();
                if (peek() == L',') { ++at; continue; }
                if (peek() == L'}') { ++at; return true; }
                return false;
            }
        }
        case L'[': {
            ++at;
            out = Value{};
            out.kind = Value::Kind::Array;
            skip_space();
            if (peek() == L']') { ++at; return true; }
            while (true) {
                Value element;
                if (!value(element, depth + 1)) return false;
                out.items.push_back(std::move(element));
                skip_space();
                if (peek() == L',') { ++at; continue; }
                if (peek() == L']') { ++at; return true; }
                return false;
            }
        }
        case L'"': {
            std::wstring token;
            if (!string_token(token)) return false;
            out = Value{};
            out.kind = Value::Kind::String;
            out.text = std::move(token);
            return true;
        }
        default: break;
        }
        out = Value{};
        if (literal(L"true")) { out.kind = Value::Kind::Bool; out.boolean = true; return true; }
        if (literal(L"false")) { out.kind = Value::Kind::Bool; out.boolean = false; return true; }
        if (literal(L"null")) { out.kind = Value::Kind::Null; return true; }
        double parsed = 0.0;
        if (!number_token(parsed)) return false;
        out.kind = Value::Kind::Number;
        out.number = parsed;
        return true;
    }
};

bool digits(std::wstring_view text, uint64_t& out) noexcept {
    if (text.empty()) return false;
    uint64_t value = 0;
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9') return false;
        const uint64_t digit = static_cast<uint64_t>(c - L'0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

} // namespace

inline const Value* Value::find(std::wstring_view name) const noexcept {
    if (kind != Kind::Object) return nullptr;
    for (const auto& member : members)
        if (member.first == name) return &member.second;
    return nullptr;
}

inline std::wstring_view Value::as_string(std::wstring_view fallback) const noexcept {
    return kind == Kind::String ? std::wstring_view(text) : fallback;
}

inline double Value::as_number(double fallback) const noexcept {
    if (kind == Kind::Number) return number;
    if (kind == Kind::Bool) return boolean ? 1.0 : 0.0;
    if (kind == Kind::String) {
        uint64_t parsed = 0;
        if (digits(text, parsed)) return static_cast<double>(parsed);
    }
    return fallback;
}

inline bool Value::as_bool(bool fallback) const noexcept {
    if (kind == Kind::Bool) return boolean;
    if (kind == Kind::Number) return number != 0.0;
    return fallback;
}

inline uint64_t Value::as_u64(uint64_t fallback) const noexcept {
    if (kind == Kind::String) {
        uint64_t parsed = 0;
        return digits(text, parsed) ? parsed : fallback;
    }
    if (kind == Kind::Number && number >= 0.0) return static_cast<uint64_t>(number);
    if (kind == Kind::Bool) return boolean ? 1u : 0u;
    return fallback;
}

inline int64_t Value::as_i64(int64_t fallback) const noexcept {
    if (kind == Kind::String) {
        std::wstring_view body = text;
        bool negative = false;
        if (!body.empty() && (body.front() == L'-' || body.front() == L'+')) {
            negative = body.front() == L'-';
            body.remove_prefix(1);
        }
        uint64_t magnitude = 0;
        if (!digits(body, magnitude)) return fallback;
        return negative ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude);
    }
    if (kind == Kind::Number) return static_cast<int64_t>(number);
    if (kind == Kind::Bool) return boolean ? 1 : 0;
    return fallback;
}

inline bool parse(std::wstring_view text, Value& out) noexcept {
    Parser parser{text};
    Value parsed;
    if (!parser.value(parsed, 0)) return false;
    parser.skip_space();
    if (!parser.done()) return false;
    out = std::move(parsed);
    return true;
}

inline Writer& Writer::prefix() {
    if (pending_key_) {
        pending_key_ = false;
        return *this;
    }
    if (!frames_.empty()) {
        if (!frames_.back().empty) out_ += L',';
        frames_.back().empty = false;
    }
    return *this;
}

inline void Writer::escaped(std::wstring_view value) {
    static const wchar_t* hex = L"0123456789abcdef";
    out_ += L'"';
    for (const wchar_t c : value) {
        switch (c) {
        case L'"': out_ += L"\\\""; break;
        case L'\\': out_ += L"\\\\"; break;
        case L'\n': out_ += L"\\n"; break;
        case L'\r': out_ += L"\\r"; break;
        case L'\t': out_ += L"\\t"; break;
        case L'\b': out_ += L"\\b"; break;
        case L'\f': out_ += L"\\f"; break;
        default:
            if (c < 0x20 || c == 0x7f) {
                out_ += L"\\u00";
                out_ += hex[(c >> 4) & 0xf];
                out_ += hex[c & 0xf];
            } else {
                out_ += c;
            }
        }
    }
    out_ += L'"';
}

inline Writer& Writer::object() {
    prefix();
    out_ += L'{';
    frames_.push_back(Frame{L'{', true});
    return *this;
}

inline Writer& Writer::array() {
    prefix();
    out_ += L'[';
    frames_.push_back(Frame{L'[', true});
    return *this;
}

inline Writer& Writer::end() {
    if (frames_.empty()) return *this;
    out_ += frames_.back().opener == L'{' ? L'}' : L']';
    frames_.pop_back();
    return *this;
}

inline Writer& Writer::key(std::wstring_view name) {
    if (!frames_.empty()) {
        if (!frames_.back().empty) out_ += L',';
        frames_.back().empty = false;
    }
    escaped(name);
    out_ += L':';
    pending_key_ = true;
    return *this;
}

inline Writer& Writer::string(std::wstring_view value) {
    prefix();
    escaped(value);
    return *this;
}

inline Writer& Writer::boolean(bool value) {
    prefix();
    out_ += value ? L"true" : L"false";
    return *this;
}

inline Writer& Writer::integer(long long value) {
    prefix();
    out_ += std::to_wstring(value);
    return *this;
}

inline Writer& Writer::number(double value) {
    prefix();
    wchar_t buffer[40]{};
    std::swprintf(buffer, std::size(buffer), L"%.17g", value);
    out_ += buffer;
    return *this;
}

inline Writer& Writer::null() {
    prefix();
    out_ += L"null";
    return *this;
}

inline Writer& Writer::u64(uint64_t value) {
    prefix();
    escaped(std::to_wstring(value));
    return *this;
}

inline Writer& Writer::i64(int64_t value) {
    prefix();
    escaped(std::to_wstring(value));
    return *this;
}

inline Writer& Writer::raw(std::wstring_view fragment) {
    prefix();
    out_ += fragment;
    return *this;
}

inline std::wstring quote(std::wstring_view value) {
    Writer writer;
    writer.string(value);
    return writer.take();
}

} // namespace ce::json
