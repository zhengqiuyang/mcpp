// v0.4 protocol completeness: server logging, prompt completion, resource
// subscriptions, dynamic tools + list_changed; client sampling/roots/
// elicitation handlers and notification callbacks.
#include "mcpp/client.hpp"
#include "mcpp/server.hpp"
#include "test_util.hpp"

#include <cstring>
#include <string>

using namespace mcpp;

// ----------------------------------------------------------------- server

static void test_logging() {
    McpServer server("log.test", "1.0");
    server.handle_message(msg::notification("notifications/initialized"));

    struct Sink : Transport {
        bool read_message(std::string&) override { return false; }
        void write_message(const std::string& m) override { sent.push_back(m); }
        std::vector<std::string> sent;
    } sink2;

    // invalid level rejected
    Json bad = msg::request(Json(1), "logging/setLevel");
    bad["params"]["level"] = "loud";
    CHECK_EQ(server.handle_message(bad)->get("error").get("code").as_int(),
             rpc::INVALID_PARAMS);

    Json ok = msg::request(Json(2), "logging/setLevel");
    ok["params"]["level"] = "debug";
    CHECK(server.handle_message(ok)->contains("result"));

    server.handle_message(msg::request(Json(3), "ping"), &sink2);
    server.log("info", "hello", Json{{"k", "v"}});
    CHECK_EQ(sink2.sent.size(), static_cast<size_t>(1));
    Json logged = Json::parse(sink2.sent.at(0));
    CHECK_EQ(logged.get("method").as_string(), std::string("notifications/message"));
    CHECK_EQ(logged["params"].get("level").as_string(), std::string("info"));
    CHECK_EQ(logged["params"].get("data").get("k").as_string(), std::string("v"));

    // without a sink: silent no-op, no crash
    server.log("error", "nobody listens");
}

static void test_prompt_completion() {
    McpServer server("comp.test", "1.0");
    server.add_prompt(
        "review", "Review code", {{"language", "string", "lang"}},
        [](const Json& a) { return PromptResult::user("review " + a.get("language").as_string()); },
        [](const std::string& arg, const std::string& prefix) {
            if (arg != "language") return std::vector<std::string>{};
            std::vector<std::string> hits;
            for (const char* lang : {"c", "cpp", "python", "typescript"})
                if (std::strncmp(lang, prefix.c_str(), prefix.size()) == 0) hits.push_back(lang);
            return hits;
        });
    server.handle_message(msg::notification("notifications/initialized"));

    Json req = msg::request(Json(1), "completion/complete");
    req["params"]["ref"]["type"] = "ref/prompt";
    req["params"]["ref"]["name"] = "review";
    req["params"]["argument"]["name"] = "language";
    req["params"]["argument"]["value"] = "c";
    auto r = server.handle_message(req);
    const Json& values = r->get("result")["completion"]["values"];
    CHECK_EQ(values.size(), static_cast<size_t>(2));  // "c", "cpp"
    CHECK_EQ(values.at(0).as_string(), std::string("c"));

    // unknown prompt / not a prompt ref -> INVALID_PARAMS
    Json bad = msg::request(Json(2), "completion/complete");
    bad["params"]["ref"]["type"] = "ref/prompt";
    bad["params"]["ref"]["name"] = "nope";
    bad["params"]["argument"]["name"] = "language";
    CHECK_EQ(server.handle_message(bad)->get("error").get("code").as_int(),
             rpc::INVALID_PARAMS);
}

static void test_resource_subscribe_and_dynamic_tools() {
    McpServer server("dyn.test", "1.0");
    server.add_tool("t1", "one", {}, [](const Json&) { return ToolResult::text("1"); });
    server.add_resource("cfg://app", "app config", "application/json",
                        [] { return std::string("{}"); });
    server.handle_message(msg::notification("notifications/initialized"));

    struct Sink : Transport {
        bool read_message(std::string&) override { return false; }
        void write_message(const std::string& m) override { sent.push_back(m); }
        std::vector<std::string> sent;
    } sink;

    // subscribe to known resource; unknown rejected
    Json sub = msg::request(Json(1), "resources/subscribe");
    sub["params"]["uri"] = "cfg://app";
    CHECK(server.handle_message(sub, &sink)->contains("result"));
    Json sub_bad = msg::request(Json(2), "resources/subscribe");
    sub_bad["params"]["uri"] = "cfg://nope";
    CHECK_EQ(server.handle_message(sub_bad, &sink)->get("error").get("code").as_int(),
             rpc::INVALID_PARAMS);

    // updated flows only for subscribed uris
    server.notify_resource_updated("cfg://app");
    server.notify_resource_updated("cfg://other");
    CHECK_EQ(sink.sent.size(), static_cast<size_t>(1));
    CHECK_EQ(Json::parse(sink.sent.at(0))["params"].get("uri").as_string(),
             std::string("cfg://app"));

    // dynamic tools: remove + add + list_changed notification
    Json before = server.handle_message(msg::request(Json(3), "tools/list")).value();
    CHECK_EQ(before["result"]["tools"].size(), static_cast<size_t>(1));
    server.remove_tool("t1");
    server.add_tool("t2", "two", {}, [](const Json&) { return ToolResult::text("2"); });
    server.notify_tools_changed();
    CHECK_EQ(sink.sent.size(), static_cast<size_t>(2));
    CHECK_EQ(Json::parse(sink.sent.at(1)).get("method").as_string(),
             std::string("notifications/tools/list_changed"));
    Json after = server.handle_message(msg::request(Json(4), "tools/list")).value();
    CHECK_EQ(after["result"]["tools"].size(), static_cast<size_t>(1));
    CHECK_EQ(after["result"]["tools"].at(0).get("name").as_string(), std::string("t2"));
    CHECK(!server.has_tool("t1") && server.has_tool("t2"));

    // capabilities advertise listChanged + subscribe
    Json init = msg::request(Json(5), "initialize");
    init["params"]["protocolVersion"] = "2025-06-18";
    Json caps = server.handle_message(init)->get("result").get("capabilities");
    CHECK_EQ(caps["tools"].get("listChanged").as_bool(), true);
    CHECK_EQ(caps["resources"].get("subscribe").as_bool(), true);
}

// ----------------------------------------------------------------- client

static void test_client_server_initiated_traffic() {
    InMemoryTransport client_end, server_end;
    client_end.connect(server_end);

    McpClient client(client_end);
    int sampled = 0, notified = 0, roots_called = 0;
    client.set_sampling_handler([&](const Json& p) {
        ++sampled;
        CHECK_EQ(p.get("maxTokens").as_int(), 64);
        Json out = Json::object();
        out["role"] = "assistant";
        out["content"] = Json::object();
        out["content"]["type"] = "text";
        out["content"]["text"] = "sampled reply";
        return out;
    });
    client.set_roots_handler([&](const Json&) {
        ++roots_called;
        Json arr = Json::array();
        Json r = Json::object();
        r["uri"] = "file:///repo";
        r["name"] = "repo";
        arr.push_back(std::move(r));
        return arr;
    });
    client.set_notification_handler("notifications/tools/list_changed",
                                    [&](const Json&) { ++notified; });

    // A fake "server": pushes a sampling request, a roots request, a
    // notification, and finally answers a real call — the client must
    // interleave all of it correctly.
    Json sampling = msg::request(Json(501), "sampling/createMessage");
    sampling["params"]["maxTokens"] = 64;
    server_end.write_message(sampling.dump());

    Json roots = msg::request(Json(502), "roots/list");
    server_end.write_message(roots.dump());

    server_end.write_message(msg::notification("notifications/tools/list_changed").dump());

    Json reply = msg::response(Json(1), Json{{"ok", true}});
    server_end.write_message(reply.dump());

    Json result = client.call("anything/at/all");
    CHECK_EQ(result.get("ok").as_bool(), true);

    CHECK_EQ(sampled, 1);
    CHECK_EQ(roots_called, 1);
    CHECK_EQ(notified, 1);

    // the client's replies to the fake server are well-formed (its own
    // request went out first, then the handler replies)
    std::string line;
    CHECK(server_end.read_message(line));
    CHECK_EQ(Json::parse(line).get("id").as_int(), 1);  // the client's own call
    CHECK(server_end.read_message(line));
    Json s1 = Json::parse(line);
    CHECK_EQ(s1.get("id").as_int(), 501);
    CHECK_EQ(s1["result"]["content"].get("text").as_string(), std::string("sampled reply"));
    CHECK(server_end.read_message(line));
    Json s2 = Json::parse(line);
    CHECK_EQ(s2.get("id").as_int(), 502);
    CHECK_EQ(s2["result"].size(), static_cast<size_t>(1));

    // unsupported server request -> METHOD_NOT_FOUND, no crash
    server_end.write_message(
        msg::request(Json(503), "elicitation/create").dump());
    Json reply2 = msg::response(Json(2), Json{{"ok", true}});
    server_end.write_message(reply2.dump());
    Json result2 = client.call("again");
    CHECK_EQ(result2.get("ok").as_bool(), true);
    CHECK(server_end.read_message(line));  // the client's own call (id 2)
    CHECK(server_end.read_message(line));
    CHECK_EQ(Json::parse(line).get("error").get("code").as_int(), rpc::METHOD_NOT_FOUND);
}

int main() {
    test_logging();
    test_prompt_completion();
    test_resource_subscribe_and_dynamic_tools();
    test_client_server_initiated_traffic();
    std::printf("test_v04: %d checks, %d failures\n", mcpptest::g_checks, mcpptest::g_failures);
    return mcpptest::g_failures == 0 ? 0 : 1;
}
