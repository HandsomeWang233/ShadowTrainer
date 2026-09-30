#pragma once
// Minimal JSON for the WebView2 bridge: one document tree, one recursive-descent
// parser and one streaming writer. Nothing here knows about the UI, the window
// or the core, so tests drive it directly.
//
// The bridge's 64-bit rule lives here too: every 64-bit quantity (address, id,
// generation, count, size, operation id) crosses as a DECIMAL STRING, because a
// JSON number cannot carry a uint64 exactly and JavaScript's Number cannot hold
// one. Writer::u64/i64 emit strings; Value::as_u64/as_i64 accept either form so
// a hand-written literal in a test still reads back.
//
// The implementation is text-included from json_min.inc by ui_bridge.cpp, the
// same way core.cpp includes typed_core.inc.
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ce::json {

struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::wstring text;                                   // Kind::String
    std::vector<Value> items;                            // Kind::Array
    std::vector<std::pair<std::wstring, Value>> members; // Kind::Object

    bool is_null() const noexcept { return kind == Kind::Null; }
    // Null when absent, so callers can write `if (auto* v = args.find(L"x"))`.
    const Value* find(std::wstring_view name) const noexcept;
    std::wstring_view as_string(std::wstring_view fallback = {}) const noexcept;
    double as_number(double fallback = 0.0) const noexcept;
    bool as_bool(bool fallback = false) const noexcept;
    uint64_t as_u64(uint64_t fallback = 0) const noexcept;
    int64_t as_i64(int64_t fallback = 0) const noexcept;
};

// False on any malformed input, with `out` untouched: a protocol error is a
// rejected command, never a crash. Nesting deeper than 32 levels is rejected.
bool parse(std::wstring_view text, Value& out) noexcept;

// Streaming writer. Containers are opened and closed explicitly; commas are
// inserted by the writer so callers never think about them.
class Writer {
public:
    Writer& object();
    Writer& array();
    Writer& end();

    Writer& key(std::wstring_view name);
    Writer& string(std::wstring_view value);
    Writer& boolean(bool value);
    Writer& integer(long long value);
    Writer& number(double value);
    Writer& null();
    // 64-bit quantities, emitted as decimal strings (see the header comment).
    Writer& u64(uint64_t value);
    Writer& i64(int64_t value);
    // A fragment that is already valid JSON, e.g. a cached section. The pump
    // keeps sections pre-serialized so an unchanged section costs no work.
    Writer& raw(std::wstring_view fragment);

    bool empty() const noexcept { return out_.empty(); }
    const std::wstring& text() const noexcept { return out_; }
    std::wstring take() { return std::move(out_); }

private:
    // One entry per open container: what closes it and whether anything has been
    // written inside it yet, which is all the comma logic needs.
    struct Frame {
        char opener = '{';
        bool empty = true;
    };
    Writer& prefix();
    void escaped(std::wstring_view value);

    std::wstring out_;
    std::vector<Frame> frames_;
    bool pending_key_ = false;
};

// Convenience for the one-off fragments the bridge builds outside a Writer.
std::wstring quote(std::wstring_view value);

} // namespace ce::json
