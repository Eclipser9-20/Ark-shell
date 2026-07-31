#include "value.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

// Shortest decimal string that round-trips back to the same double (like jq /
// modern JSON writers). The default ostream precision (6) silently truncates,
// e.g. 0.123456789012345 -> "0.123457". Try 15/16/17 sig digits, take the first
// that round-trips.
static std::string dtoa(double d) {
    char buf[32];
    for (int prec : {15, 16, 17}) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, d);
        if (std::strtod(buf, nullptr) == d) return buf;
    }
    std::snprintf(buf, sizeof buf, "%.17g", d);
    return buf;
}

// ── Value helpers ───────────────────────────────────────────────────────────
bool Value::isTable() const {
    if (type != Type::List || list.empty()) return false;
    for (const auto& e : list) if (e.type != Type::Record) return false;
    return true;
}

const Value* Value::find(const std::string& key) const {
    if (type != Type::Record) return nullptr;
    for (auto& kv : rec) if (kv.first == key) return &kv.second;
    return nullptr;
}
Value* Value::find(const std::string& key) {
    if (type != Type::Record) return nullptr;
    for (auto& kv : rec) if (kv.first == key) return &kv.second;
    return nullptr;
}
void Value::set(const std::string& key, Value v) {
    for (auto& kv : rec) if (kv.first == key) { kv.second = std::move(v); return; }
    type = Type::Record;
    rec.emplace_back(key, std::move(v));
}

std::string Value::asString() const {
    switch (type) {
        case Type::Null: return "";
        case Type::Bool: return b ? "true" : "false";
        case Type::Int: return std::to_string(i);
        case Type::Float: return dtoa(f);
        case Type::Str: return s;
        default: return ""; // containers have no scalar text
    }
}
double Value::asNumber() const {
    switch (type) {
        case Type::Bool: return b ? 1 : 0;
        case Type::Int: return (double)i;
        case Type::Float: return f;
        case Type::Str: { try { return std::stod(s); } catch (...) { return 0; } }
        default: return 0;
    }
}
bool Value::truthy() const {
    switch (type) {
        case Type::Null: return false;
        case Type::Bool: return b;
        case Type::Int: return i != 0;
        case Type::Float: return f != 0;
        case Type::Str: return !s.empty();
        case Type::List: return !list.empty();
        case Type::Record: return !rec.empty();
    }
    return false;
}

// ── JSON serialization ──────────────────────────────────────────────────────
static void jsonEscape(const std::string& s, std::string& out) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
                else out += c;
        }
    }
    out += '"';
}
static void toJsonImpl(const Value& v, std::string& out, bool pretty, int depth) {
    auto indent = [&](int d) { if (pretty) { out += '\n'; out.append(d * 2, ' '); } };
    switch (v.type) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += v.b ? "true" : "false"; break;
        case Value::Type::Int: out += std::to_string(v.i); break;
        case Value::Type::Float: out += dtoa(v.f); break;
        case Value::Type::Str: jsonEscape(v.s, out); break;
        case Value::Type::List:
            if (v.list.empty()) { out += "[]"; break; }
            out += '[';
            for (size_t k = 0; k < v.list.size(); k++) {
                if (k) out += ',';
                indent(depth + 1);
                toJsonImpl(v.list[k], out, pretty, depth + 1);
            }
            indent(depth); out += ']';
            break;
        case Value::Type::Record:
            if (v.rec.empty()) { out += "{}"; break; }
            out += '{';
            for (size_t k = 0; k < v.rec.size(); k++) {
                if (k) out += ',';
                indent(depth + 1);
                jsonEscape(v.rec[k].first, out);
                out += pretty ? ": " : ":";
                toJsonImpl(v.rec[k].second, out, pretty, depth + 1);
            }
            indent(depth); out += '}';
            break;
    }
}
std::string toJson(const Value& v, bool pretty) {
    std::string out;
    toJsonImpl(v, out, pretty, 0);
    return out;
}

// ── JSON parsing (recursive descent) ────────────────────────────────────────
namespace {
struct JsonParser {
    const std::string& t;
    size_t i = 0;
    bool ok = true;
    explicit JsonParser(const std::string& s) : t(s) {}

    void ws() { while (i < t.size() && (t[i]==' '||t[i]=='\t'||t[i]=='\n'||t[i]=='\r')) i++; }
    bool eof() { return i >= t.size(); }
    char peek() { return i < t.size() ? t[i] : '\0'; }

    Value parse() { ws(); Value v = value(); ws(); return v; }

    Value value() {
        ws();
        if (eof()) { ok = false; return Value::null(); }
        char c = t[i];
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') return Value::str(string());
        if (c == 't' || c == 'f') return boolean();
        if (c == 'n') { if (t.compare(i,4,"null")==0){i+=4;return Value::null();} ok=false; return Value::null(); }
        if (c == '-' || (c >= '0' && c <= '9')) return number();
        ok = false; return Value::null();
    }
    Value object() {
        Value v = Value::record(); i++; // '{'
        ws();
        if (peek() == '}') { i++; return v; }
        for (;;) {
            ws();
            if (peek() != '"') { ok = false; return v; }
            std::string key = string();
            ws();
            if (peek() != ':') { ok = false; return v; }
            i++;
            Value val = value();
            v.rec.emplace_back(std::move(key), std::move(val));
            ws();
            if (peek() == ',') { i++; continue; }
            if (peek() == '}') { i++; break; }
            ok = false; return v;
        }
        return v;
    }
    Value array() {
        Value v = Value::makeList({}); i++; // '['
        ws();
        if (peek() == ']') { i++; return v; }
        for (;;) {
            v.list.push_back(value());
            ws();
            if (peek() == ',') { i++; continue; }
            if (peek() == ']') { i++; break; }
            ok = false; return v;
        }
        return v;
    }
    std::string string() {
        std::string out; i++; // opening quote
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c == '\\' && i < t.size()) {
                char e = t[i++];
                switch (e) {
                    case 'n': out += '\n'; break; case 't': out += '\t'; break;
                    case 'r': out += '\r'; break; case '"': out += '"'; break;
                    case '\\': out += '\\'; break; case '/': out += '/'; break;
                    case 'b': out += '\b'; break; case 'f': out += '\f'; break;
                    case 'u': { // \uXXXX -> UTF-8, combining surrogate pairs for non-BMP
                        if (i + 4 <= t.size()) {
                            unsigned cp = (unsigned)strtol(t.substr(i,4).c_str(), nullptr, 16); i += 4;
                            // A high surrogate (D800-DBFF) followed by a low surrogate
                            // (DC00-DFFF) encodes one code point above U+FFFF.
                            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= t.size() && t[i] == '\\' && t[i+1] == 'u') {
                                unsigned lo = (unsigned)strtol(t.substr(i+2,4).c_str(), nullptr, 16);
                                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                    i += 6;
                                }
                            }
                            if (cp < 0x80) out += (char)cp;
                            else if (cp < 0x800) { out += (char)(0xC0|(cp>>6)); out += (char)(0x80|(cp&0x3F)); }
                            else if (cp < 0x10000) { out += (char)(0xE0|(cp>>12)); out += (char)(0x80|((cp>>6)&0x3F)); out += (char)(0x80|(cp&0x3F)); }
                            else { out += (char)(0xF0|(cp>>18)); out += (char)(0x80|((cp>>12)&0x3F)); out += (char)(0x80|((cp>>6)&0x3F)); out += (char)(0x80|(cp&0x3F)); }
                        }
                        break;
                    }
                    default: out += e; break;
                }
            } else out += c;
        }
        if (i < t.size()) i++; // closing quote
        return out;
    }
    Value boolean() {
        if (t.compare(i,4,"true")==0) { i+=4; return Value::boolean(true); }
        if (t.compare(i,5,"false")==0) { i+=5; return Value::boolean(false); }
        ok = false; return Value::null();
    }
    Value number() {
        size_t s = i;
        bool isFloat = false;
        if (peek() == '-') i++;
        while (i < t.size() && ((t[i]>='0'&&t[i]<='9')||t[i]=='.'||t[i]=='e'||t[i]=='E'||t[i]=='+'||t[i]=='-')) {
            if (t[i]=='.'||t[i]=='e'||t[i]=='E') isFloat = true;
            i++;
        }
        std::string num = t.substr(s, i - s);
        if (isFloat) return Value::real(strtod(num.c_str(), nullptr));
        return Value::integer(strtoll(num.c_str(), nullptr, 10));
    }
};
} // namespace

bool fromJson(const std::string& text, Value& out) {
    JsonParser p(text);
    Value v = p.parse();
    if (!p.ok) return false;
    // Trailing non-whitespace = malformed (guards `{}garbage`).
    while (p.i < text.size()) { char c = text[p.i]; if (c!=' '&&c!='\t'&&c!='\n'&&c!='\r') return false; p.i++; }
    out = std::move(v);
    return true;
}

// ── Rendering (nushell-style) ───────────────────────────────────────────────
namespace {
// Collect the union of record keys across a table, preserving first-seen order.
std::vector<std::string> tableColumns(const Value& table) {
    std::vector<std::string> cols;
    for (const auto& row : table.list)
        for (const auto& kv : row.rec)
            if (std::find(cols.begin(), cols.end(), kv.first) == cols.end())
                cols.push_back(kv.first);
    return cols;
}
// UTF-8 display width (count code points, not bytes) so box borders line up
// with multibyte content.
size_t dispWidth(const std::string& s) {
    size_t w = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) w++;
    return w;
}
std::string padTo(const std::string& s, size_t w) {
    size_t cur = dispWidth(s);
    return cur >= w ? s : s + std::string(w - cur, ' ');
}
std::string renderTable(const Value& table) {
    auto cols = tableColumns(table);
    std::vector<size_t> w(cols.size());
    for (size_t c = 0; c < cols.size(); c++) w[c] = dispWidth(cols[c]);
    std::vector<std::vector<std::string>> cells;
    for (const auto& row : table.list) {
        std::vector<std::string> line;
        for (size_t c = 0; c < cols.size(); c++) {
            const Value* v = row.find(cols[c]);
            std::string txt = v ? (v->type == Value::Type::List || v->type == Value::Type::Record
                                   ? toJson(*v) : v->asString()) : "";
            w[c] = std::max(w[c], dispWidth(txt));
            line.push_back(std::move(txt));
        }
        cells.push_back(std::move(line));
    }
    auto rule = [&](const char* l, const char* m, const char* r) {
        std::string s = l;
        for (size_t c = 0; c < cols.size(); c++) { s += std::string(w[c] + 2, '-'); s += (c + 1 < cols.size()) ? m : r; }
        return s;
    };
    // Simple ASCII-ish box using the same rounded corners as nuLs elsewhere.
    std::string out;
    auto row = [&](const std::vector<std::string>& r) {
        std::string s = "|";
        for (size_t c = 0; c < cols.size(); c++) s += " " + padTo(r[c], w[c]) + " |";
        return s + "\n";
    };
    out += rule("+", "+", "+") + "\n";
    out += row(cols);
    out += rule("+", "+", "+") + "\n";
    for (auto& line : cells) out += row(line);
    out += rule("+", "+", "+") + "\n";
    return out;
}
} // namespace

std::string renderText(const Value& v) {
    if (v.isTable()) return renderTable(v);
    if (v.type == Value::Type::List) {
        std::string out;
        for (const auto& e : v.list) out += (e.type == Value::Type::Record || e.type == Value::Type::List
                                             ? toJson(e) : e.asString()) + "\n";
        return out;
    }
    if (v.type == Value::Type::Record) {
        // Two-column key/value table.
        Value asTable = Value::makeList({});
        for (const auto& kv : v.rec) {
            Value r = Value::record();
            r.set("key", Value::str(kv.first));
            r.set("value", kv.second.type == Value::Type::Record || kv.second.type == Value::Type::List
                           ? Value::str(toJson(kv.second)) : Value::str(kv.second.asString()));
            asTable.list.push_back(std::move(r));
        }
        return renderTable(asTable);
    }
    return v.asString() + "\n";
}

// ── Legacy text -> Value (the auto-fallback ingest) ─────────────────────────
Value parseLegacyText(const std::string& text) {
    // Trim leading whitespace to sniff for JSON.
    size_t p = 0;
    while (p < text.size() && (text[p]==' '||text[p]=='\t'||text[p]=='\n'||text[p]=='\r')) p++;
    if (p < text.size() && (text[p] == '{' || text[p] == '[')) {
        Value v;
        if (fromJson(text, v)) return v;
    }
    // Split into non-empty lines.
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) lines.push_back(cur);
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    if (lines.empty()) return Value::makeList({});

    // If NO line has internal whitespace, it's a one-column list (e.g. `ls`) --
    // return a plain list of strings.
    auto hasCols = [](const std::string& l) {
        size_t a = l.find_first_not_of(" \t");
        if (a == std::string::npos) return false;
        return l.find_first_of(" \t", a) != std::string::npos;
    };
    bool anyCols = false;
    for (auto& l : lines) if (hasCols(l)) { anyCols = true; break; }
    auto split = [](const std::string& l) {
        std::vector<std::string> f; std::string w;
        for (char c : l) { if (c==' '||c=='\t') { if(!w.empty()){f.push_back(w);w.clear();} } else w+=c; }
        if (!w.empty()) f.push_back(w);
        return f;
    };
    if (!anyCols) {
        Value out = Value::makeList({});
        for (auto& l : lines) if (!l.empty()) out.list.push_back(Value::str(l));
        return out;
    }
    // Otherwise a whitespace table: generic column names col0, col1, ...
    Value out = Value::makeList({});
    for (auto& l : lines) {
        if (l.empty()) continue;
        auto fields = split(l);
        Value r = Value::record();
        for (size_t c = 0; c < fields.size(); c++) r.set("col" + std::to_string(c), Value::str(fields[c]));
        out.list.push_back(std::move(r));
    }
    return out;
}
