#pragma once
#include <string>
#include <utility>
#include <vector>

// ── Structured data value (the currency of Hybrid Data Pipelines) ───────────
// A nushell-style dynamic value: null / bool / int / float / string / list /
// record (an ORDERED key→value map). A "table" is just a List of Records with
// consistent keys -- no separate type. Structured ark commands pass Values to
// each other; at a boundary with a legacy tool the Value is rendered to text,
// and text coming from a legacy tool is parsed back into a Value.
struct Value {
    enum class Type { Null, Bool, Int, Float, Str, List, Record };
    Type type = Type::Null;
    bool b = false;
    long long i = 0;
    double f = 0.0;
    std::string s;
    std::vector<Value> list;                          // Type::List
    // Ordered record, stored as two PARALLEL vectors (recKeys[i] -> recVals[i])
    // rather than std::vector<std::pair<std::string, Value>>. Only std::vector<T>
    // is portably allowed to hold an INCOMPLETE T (C++17, both libc++ and
    // libstdc++); a vector<pair<string,Value>> makes libstdc++ (Linux) eagerly
    // instantiate the pair, which needs Value complete at this self-referential
    // point and fails to build. recVals is a plain vector<Value>, same as list.
    std::vector<std::string> recKeys;                 // Type::Record keys (ordered)
    std::vector<Value> recVals;                       // Type::Record values (parallel)

    static Value null() { return Value{}; }
    static Value boolean(bool v) { Value x; x.type = Type::Bool; x.b = v; return x; }
    static Value integer(long long v) { Value x; x.type = Type::Int; x.i = v; return x; }
    static Value real(double v) { Value x; x.type = Type::Float; x.f = v; return x; }
    static Value str(std::string v) { Value x; x.type = Type::Str; x.s = std::move(v); return x; }
    static Value makeList(std::vector<Value> v) { Value x; x.type = Type::List; x.list = std::move(v); return x; }
    static Value record() { Value x; x.type = Type::Record; return x; }

    bool isList() const { return type == Type::List; }
    bool isRecord() const { return type == Type::Record; }
    // A table = a non-empty list whose every element is a record.
    bool isTable() const;

    // Record field access (returns nullptr if absent / not a record).
    const Value* find(const std::string& key) const;
    Value* find(const std::string& key);
    void set(const std::string& key, Value v); // append or overwrite (ordered)

    // Scalar coercions used by filters (where col > 5) and rendering.
    std::string asString() const;   // human text for any scalar; "" for containers
    double asNumber() const;        // best-effort numeric view (0 if non-numeric)
    bool truthy() const;

    // Record helpers over the parallel recKeys/recVals vectors.
    size_t recSize() const { return recKeys.size(); }
    void recPush(std::string key, Value v) {
        recKeys.push_back(std::move(key));
        recVals.push_back(std::move(v));
    }
};

// ── JSON (the on-the-wire format between two structured commands) ────────────
std::string toJson(const Value& v, bool pretty = false);
// Parse JSON text; returns false (and leaves `out` untouched) on malformed input.
bool fromJson(const std::string& text, Value& out);

// ── Rendering & legacy-text parsing (the "hybrid / auto-fallback" boundary) ──
// Human-facing rendering: a table -> nushell-style bordered table; a list of
// scalars -> one per line; a record -> a two-column key/value table; a scalar
// -> its text. This is what a structured command emits to a terminal or a
// legacy tool.
std::string renderText(const Value& v);

// Parse arbitrary legacy tool output into a Value: JSON if it looks like JSON,
// otherwise a table split on whitespace (first row treated as a header when it
// looks like one), or a one-column list of lines when rows have no internal
// whitespace (e.g. `ls`). This is what lets `cat data.json | get x` and
// `ls | where ...` work without the upstream being a structured command.
Value parseLegacyText(const std::string& text);
