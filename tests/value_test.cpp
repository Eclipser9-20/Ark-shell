#include "value.h"
#include <cassert>
#include <cstdio>
#include <string>

static void roundtrip(const std::string& json) {
    Value v;
    assert(fromJson(json, v));
    std::string out = toJson(v);
    Value v2;
    assert(fromJson(out, v2));           // re-parse must succeed
    assert(toJson(v2) == out);           // and be stable
}

int main() {
    // Scalars
    Value v;
    assert(fromJson("42", v) && v.type == Value::Type::Int && v.i == 42);
    assert(fromJson("3.5", v) && v.type == Value::Type::Float);
    assert(fromJson("true", v) && v.b);
    assert(fromJson("\"hi\\n\"", v) && v.s == "hi\n");
    assert(fromJson("null", v) && v.type == Value::Type::Null);

    // Malformed rejected
    assert(!fromJson("{", v));
    assert(!fromJson("{}garbage", v));
    assert(!fromJson("", v));

    // Object field access preserves order + values
    assert(fromJson("{\"name\":\"ark\",\"n\":3}", v));
    assert(v.isRecord());
    assert(v.rec[0].first == "name" && v.rec[1].first == "n");
    assert(v.find("name")->s == "ark");
    assert(v.find("n")->i == 3);
    assert(v.find("missing") == nullptr);

    // Table detection
    Value table;
    assert(fromJson("[{\"a\":1},{\"a\":2}]", table));
    assert(table.isTable());
    Value notTable;
    assert(fromJson("[1,2,3]", notTable));
    assert(!notTable.isTable()); // list of scalars is not a table

    // Round-trips
    roundtrip("{\"a\":[1,2,{\"b\":\"x\"}],\"c\":true}");
    roundtrip("[]");
    roundtrip("{}");

    // Rendering a table produces borders + every column header
    std::string rendered = renderText(table);
    assert(rendered.find("a") != std::string::npos);
    assert(rendered.find("|") != std::string::npos);
    assert(rendered.find("1") != std::string::npos && rendered.find("2") != std::string::npos);

    // Legacy text: JSON detected
    Value lv = parseLegacyText("  {\"k\":1}  ");
    assert(lv.isRecord() && lv.find("k")->i == 1);

    // Legacy text: one-column list (like `ls`)
    Value names = parseLegacyText("foo\nbar\nbaz\n");
    assert(names.isList() && names.list.size() == 3 && names.list[1].s == "bar");

    // Legacy text: whitespace table -> col0/col1
    Value cols = parseLegacyText("alice 30\nbob 25\n");
    assert(cols.isTable());
    assert(cols.list[0].find("col0")->s == "alice");
    assert(cols.list[1].find("col1")->s == "25");

    // Scalar coercions used by filters
    assert(Value::str("42").asNumber() == 42);
    assert(Value::integer(0).truthy() == false);
    assert(Value::str("").truthy() == false);

    printf("all value/json tests passed\n");
    return 0;
}
