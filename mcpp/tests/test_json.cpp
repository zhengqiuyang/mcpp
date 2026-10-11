// Unit tests for mcpp/json.hpp — run: ./test_json
#include "mcpp/json.hpp"
#include "test_util.hpp"

using mcpp::Json;

static void test_scalars() {
    CHECK_EQ(Json::parse("null").type(), Json::Type::Null);
    CHECK_EQ(Json::parse("true").as_bool(), true);
    CHECK_EQ(Json::parse("false").as_bool(), false);
    CHECK_EQ(Json::parse("42").as_int(), 42);
    CHECK_EQ(Json::parse("-17").as_int(), -17);
    CHECK_EQ(Json::parse("0").as_int(), 0);
    CHECK_EQ(Json::parse("3.5").as_double(), 3.5);
    CHECK_EQ(Json::parse("-2.25e2").as_double(), -225.0);
    CHECK_EQ(Json::parse("\"hi\"").as_string(), std::string("hi"));
    CHECK_EQ(Json::parse("  \n\t 42 \r\n ").as_int(), 42);  // surrounding whitespace
}

static void test_int_exactness() {
    // 2^53+1 cannot be represented by double; int64 storage must keep it exact.
    const char* big = "9007199254740993";
    Json j = Json::parse(big);
    CHECK(j.is_int());
    CHECK_EQ(j.dump(), std::string(big));
    CHECK_EQ(j.as_int(), 9007199254740993LL);
    // int64 min round-trips exactly
    Json lo = Json::parse("-9223372036854775808");
    CHECK_EQ(lo.dump(), std::string("-9223372036854775808"));
    // overflowing int64 degrades gracefully to double
    Json over = Json::parse("99999999999999999999");
    CHECK(over.is_double());
}

static void test_double_roundtrip() {
    const double vals[] = {0.1, 1.0 / 3.0, 3.141592653589793, 1e-300, 1e300, -0.0, 5e-324};
    for (double v : vals) {
        Json j = v;
        CHECK_EQ(Json::parse(j.dump()).as_double(), v);
    }
}

static void test_strings() {
    CHECK_EQ(Json::parse("\"a\\nb\\tc\"").as_string(), std::string("a\nb\tc"));
    CHECK_EQ(Json::parse("\"quote:\\\" slash:\\\\ slash2:\\/\"").as_string(),
             std::string("quote:\" slash:\\ slash2:/"));
    CHECK_EQ(Json::parse("\"\\u0041\\u00e9\\u4e2d\"").as_string(), std::string("A\xc3\xa9\xe4\xb8\xad"));
    // surrogate pair: U+1F600
    CHECK_EQ(Json::parse("\"\\ud83d\\ude00\"").as_string(), std::string("\xf0\x9f\x98\x80"));
    // serializer escapes control characters
    Json j = std::string("a\"b\\c\nd\te\01f");
    CHECK_EQ(j.dump(), std::string("\"a\\\"b\\\\c\\nd\\te\\u0001f\""));
    CHECK_EQ(Json::parse(j.dump()).as_string(), j.as_string());
    // empty string
    CHECK_EQ(Json::parse("\"\"").as_string(), std::string(""));
}

static void test_parse_errors() {
    auto throws_with = [](const char* text) {
        try {
            Json::parse(text);
            return false;
        } catch (const Json::Error&) {
            return true;
        }
    };
    CHECK(throws_with(""));                    // empty input
    CHECK(throws_with("{"));                   // unterminated object
    CHECK(throws_with("[1,2,]"));              // trailing comma
    CHECK(throws_with("{\"a\":1,}"));          // trailing comma in object
    CHECK(throws_with("01x"));                 // invalid number tail
    CHECK(throws_with("\"unterminated"));      // unterminated string
    CHECK(throws_with("\"bad \\q escape\""));  // invalid escape
    CHECK(throws_with("\"\\ud83d\""));         // lone high surrogate
    CHECK(throws_with("\"\\udc00\""));         // lone low surrogate
    CHECK(throws_with("null garbage"));        // trailing content
    CHECK(throws_with("tru"));                 // truncated literal
    CHECK(throws_with("{'a':1}"));             // single quotes are not JSON
    CHECK(throws_with("[1 2]"));               // missing comma
    CHECK(throws_with("nul\x01"));             // control char in literal tail
    // error carries an offset
    try {
        Json::parse("[1,]");
        CHECK(false);
    } catch (const Json::Error& e) {
        CHECK(e.offset > 0);
    }
}

static void test_containers() {
    Json obj = Json::object();
    obj["name"] = "mcpp";
    obj["count"] = 3;
    obj["ratio"] = 0.5;
    obj["ok"] = true;
    obj["nested"]["deep"] = Json::array();
    CHECK(obj.is_object());
    CHECK_EQ(obj.size(), static_cast<size_t>(5));
    CHECK(obj.contains("name"));
    CHECK(!obj.contains("missing"));
    CHECK_EQ(obj.get("name").as_string(), std::string("mcpp"));
    CHECK(obj.find("missing") == nullptr);
    CHECK_EQ(obj.get_or<std::string>("missing", "dflt"), std::string("dflt"));
    CHECK_EQ(obj.get_or<int64_t>("count", 0), 3);
    CHECK_EQ(obj.get_or<double>("missing", 1.5), 1.5);
    bool threw = false;
    try { obj.get("missing"); } catch (const std::logic_error&) { threw = true; }
    CHECK(threw);

    Json arr = Json::array();
    arr.push_back(1).push_back("two").push_back(3.0).push_back(Json::object());
    CHECK_EQ(arr.size(), static_cast<size_t>(4));
    CHECK_EQ(arr.at(0).as_int(), 1);
    CHECK_EQ(arr.at(1).as_string(), std::string("two"));
    CHECK(arr.at(3).is_object());
    threw = false;
    try { arr.at(9); } catch (const std::out_of_range&) { threw = true; }
    CHECK(threw);

    // sorted-key determinism: two different insertion orders dump identically
    Json a = Json::object(), b = Json::object();
    a["x"] = 1; a["y"] = 2;
    b["y"] = 2; b["x"] = 1;
    CHECK_EQ(a.dump(), b.dump());
}

static void test_roundtrip_document() {
    const char* src =
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"protocolVersion\":\"2025-06-18\","
        "\"capabilities\":{\"tools\":{\"listChanged\":true}},\"serverInfo\":{\"name\":\"demo\","
        "\"version\":\"1.0\"},\"content\":[{\"type\":\"text\",\"text\":\"line1\\nline2\"},"
        "{\"type\":\"image\",\"data\":\"aGk=\",\"mimeType\":\"image/png\"}]}}";
    Json j = Json::parse(src);
    CHECK_EQ(j.get("id").as_int(), 7);
    CHECK_EQ(j["result"]["serverInfo"]["name"].as_string(), std::string("demo"));
    const std::vector<Json>& content = j["result"]["content"].as_array();
    CHECK_EQ(content.size(), static_cast<size_t>(2));
    CHECK_EQ(content.at(1).get("mimeType").as_string(), std::string("image/png"));
    Json reparsed = Json::parse(j.dump());
    CHECK(reparsed == j);
}

static void test_equality() {
    CHECK(Json(5) == Json(5));
    CHECK(Json(5) != Json(6));
    CHECK(Json(5.0) == Json(5.0));
    CHECK(Json(5) == Json(5.0));   // numeric cross-type compare
    CHECK(Json("a") != Json("b"));
    CHECK(Json(nullptr) == Json(nullptr));
    CHECK(Json::parse("[1,[2,3]]") == Json::parse("[1,[2,3]]"));
    CHECK(Json::parse("{\"a\":1}") != Json::parse("{\"a\":2}"));
}

static void test_pretty_print() {
    Json j = Json::object();
    j["b"] = 1;
    j["a"] = Json::array();
    j["a"].push_back(1).push_back(2);
    CHECK_EQ(j.dump(), std::string("{\"a\":[1,2],\"b\":1}"));
    CHECK_EQ(j.dump(2), std::string("{\n  \"a\": [\n    1,\n    2\n  ],\n  \"b\": 1\n}"));
}

int main() {
    test_scalars();
    test_int_exactness();
    test_double_roundtrip();
    test_strings();
    test_parse_errors();
    test_containers();
    test_roundtrip_document();
    test_equality();
    test_pretty_print();
    std::printf("test_json: %d checks, %d failures\n", mcpptest::g_checks, mcpptest::g_failures);
    return mcpptest::g_failures == 0 ? 0 : 1;
}
