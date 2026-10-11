// Protocol-level integration tests for mcpp — exercises McpServer and
// McpClient against each other over an in-memory transport, plus direct
// handle_message cases for every error path.
#include "mcpp/client.hpp"
#include "mcpp/server.hpp"
#include "test_util.hpp"

#include <cctype>

using namespace mcpp;

static Json rpc_id(int64_t v) { return Json(v); }

static int error_code(const Json& reply) { return reply.get("error").get("code").as_int(); }

static void test_initialize_handshake() {
    McpServer server("demo.server", "0.1.0");
    server.add_tool("echo", "Echo text", {{"message", "string", "text to echo"}},
                    [](const Json& a) { return ToolResult::text(a.get("message").as_string()); });

    Json init = msg::request(rpc_id(1), "initialize", Json::object());
    init["params"]["protocolVersion"] = "2025-06-18";
    init["params"]["clientInfo"]["name"] = "tester";
    auto reply = server.handle_message(init);
    CHECK(reply.has_value());
    const Json& result = reply->get("result");
    CHECK_EQ(result.get("protocolVersion").as_string(), std::string("2025-06-18"));
    CHECK_EQ(result["serverInfo"]["name"].as_string(), std::string("demo.server"));
    CHECK(result["capabilities"].contains("tools"));

    // unknown version -> we answer with our latest
    Json init2 = msg::request(rpc_id(2), "initialize", Json::object());
    init2["params"]["protocolVersion"] = "1999-01-01";
    auto reply2 = server.handle_message(init2);
    CHECK_EQ(reply2->get("result").get("protocolVersion").as_string(),
             std::string(kLatestProtocolVersion));
}

static void test_before_initialize_gated() {
    McpServer server("demo.server", "0.1.0");
    server.add_tool("t", "d", {}, [](const Json&) { return ToolResult::text("ok"); });
    auto reply = server.handle_message(msg::request(rpc_id(1), "tools/list"));
    CHECK(reply.has_value());
    CHECK_EQ(error_code(*reply), mcp_error::SERVER_NOT_INITIALIZED);
    // ping is always allowed
    auto pong = server.handle_message(msg::request(rpc_id(2), "ping"));
    CHECK(pong.has_value() && pong->contains("result"));
}

static void test_tools_flow() {
    McpServer server("demo.server", "0.1.0");
    server.add_tool("add", "Add two ints",
                    {{"a", "integer", "left"}, {"b", "integer", "right"}},
                    [](const Json& a) {
                        return ToolResult::text(std::to_string(a.get("a").as_int() +
                                                               a.get("b").as_int()));
                    });
    server.add_tool("boom", "throws", {}, [](const Json&) -> ToolResult {
        throw std::runtime_error("kaboom");
    });
    server.add_tool("denied", "rpc error", {}, [](const Json&) -> ToolResult {
        throw RpcError(rpc::INVALID_PARAMS, "bad input");
    });
    server.handle_message(msg::notification("notifications/initialized"));

    auto list = server.handle_message(msg::request(rpc_id(1), "tools/list"));
    const Json& tools = list->get("result").get("tools");
    CHECK_EQ(tools.size(), static_cast<size_t>(3));
    const Json& schema = tools.at(0).get("inputSchema");
    CHECK_EQ(schema.get("type").as_string(), std::string("object"));
    CHECK(schema["properties"].contains("a"));
    CHECK_EQ(schema.get("required").size(), static_cast<size_t>(2));

    Json call = msg::request(rpc_id(2), "tools/call");
    call["params"]["name"] = "add";
    call["params"]["arguments"]["a"] = 19;
    call["params"]["arguments"]["b"] = 23;
    auto sum = server.handle_message(call);
    CHECK_EQ(sum->get("result").get("content").at(0).get("text").as_string(),
             std::string("42"));
    CHECK_EQ(sum->get("result").get("isError").as_bool(), false);

    // handler exception -> INTERNAL_ERROR with message preserved
    auto exploded = server.handle_message(
        msg::request(rpc_id(3), "tools/call", Json::object(
            {{"name", Json("boom")}})));
    CHECK_EQ(error_code(*exploded), rpc::INTERNAL_ERROR);
    CHECK_EQ(exploded->get("error").get("message").as_string(), std::string("kaboom"));

    // RpcError lets handlers control the code
    auto denied = server.handle_message(
        msg::request(rpc_id(4), "tools/call", Json::object({{"name", Json("denied")}})));
    CHECK_EQ(error_code(*denied), rpc::INVALID_PARAMS);

    // unknown tool
    auto unknown = server.handle_message(
        msg::request(rpc_id(5), "tools/call", Json::object({{"name", Json("nope")}})));
    CHECK_EQ(error_code(*unknown), rpc::INVALID_PARAMS);
}

static void test_resources_and_prompts() {
    McpServer server("demo.server", "0.1.0");
    server.add_resource("config://app", "app config", "application/json",
                        [] { return std::string("{\"theme\":\"dark\"}"); });
    server.add_prompt("review", "Code review prompt", {{"language", "string", "lang"}},
                      [](const Json& a) {
                          return PromptResult::user("Review this " +
                                                    a.get_or<std::string>("language", "code"));
                      });
    server.handle_message(msg::notification("notifications/initialized"));

    auto rlist = server.handle_message(msg::request(rpc_id(1), "resources/list"));
    CHECK_EQ(rlist->get("result").get("resources").at(0).get("uri").as_string(),
             std::string("config://app"));

    Json rd = msg::request(rpc_id(2), "resources/read");
    rd["params"]["uri"] = "config://app";
    auto body = server.handle_message(rd);
    CHECK_EQ(body->get("result").get("contents").at(0).get("text").as_string(),
             std::string("{\"theme\":\"dark\"}"));

    Json missing = msg::request(rpc_id(3), "resources/read");
    missing["params"]["uri"] = "config://nope";
    CHECK_EQ(error_code(*server.handle_message(missing)), rpc::INVALID_PARAMS);

    auto plist = server.handle_message(msg::request(rpc_id(4), "prompts/list"));
    CHECK_EQ(plist->get("result").get("prompts").at(0).get("name").as_string(),
             std::string("review"));

    Json pg = msg::request(rpc_id(5), "prompts/get");
    pg["params"]["name"] = "review";
    pg["params"]["arguments"]["language"] = "C++ module";
    auto prompt = server.handle_message(pg);
    CHECK_EQ(prompt->get("result").get("messages").at(0).get("content").get("text").as_string(),
             std::string("Review this C++ module"));
}

static void test_error_envelopes() {
    McpServer server("demo.server", "0.1.0");
    server.handle_message(msg::notification("notifications/initialized"));

    // notifications and unknown notifications produce no reply
    CHECK(!server.handle_message(msg::notification("tools/list")).has_value());
    CHECK(!server.handle_message(msg::notification("some/unknown/thing")).has_value());

    // unknown request method
    auto e = server.handle_message(msg::request(rpc_id(9), "no/such/method"));
    CHECK_EQ(error_code(*e), rpc::METHOD_NOT_FOUND);

    // non-object message
    auto arr = server.handle_message(Json::array());
    CHECK_EQ(error_code(*arr), rpc::INVALID_REQUEST);

    // missing method
    auto nomethod = server.handle_message(
        Json::object({{"jsonrpc", Json("2.0")}, {"id", rpc_id(1)}}));
    CHECK_EQ(error_code(*nomethod), rpc::INVALID_REQUEST);

    // parse error via handle_line, id must be null
    auto bad = server.handle_line("{not json");
    CHECK(bad.has_value());
    Json parsed = Json::parse(*bad);
    CHECK(parsed.get("id").is_null());
    CHECK_EQ(error_code(parsed), rpc::PARSE_ERROR);

    // server ignores responses aimed at a client role
    CHECK(!server.handle_message(msg::response(rpc_id(1), Json::object())).has_value());
}

static void test_end_to_end_client_server() {
    InMemoryTransport client_end, server_end;
    client_end.connect(server_end);

    McpServer server("e2e.server", "1.2.3");
    server.add_tool("greet", "Greet someone",
                    {{"who", "string", "who to greet"}, {"shout", "boolean", "uppercase"}},
                    [](const Json& a) {
                        std::string who = a.get_or<std::string>("who", "world");
                        if (a.get_or<bool>("shout", false)) {
                            for (auto& c : who) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
                        }
                        return ToolResult::text("hello, " + who);
                    });
    server.add_tool("multi", "text + embedded resource", {}, [](const Json&) {
        ToolResult r = ToolResult::text("part one");
        r.add(Content::resource("mem://notes", "remember the milk"));
        return r;
    });

    // Single-threaded pumping: when the client starves, feed the server.
    McpClient client(client_end);
    client.set_idle_hook([&] {
        std::string l;
        bool delivered = false;
        while (server_end.read_message(l)) {
            auto reply = server.handle_line(l);
            if (reply) {
                server_end.write_message(*reply);
                delivered = true;
            }
        }
        return delivered;
    });

    McpClient::ServerInfo info = client.initialize("e2e-client", "0.0.1");
    CHECK_EQ(info.name, std::string("e2e.server"));
    CHECK_EQ(info.version, std::string("1.2.3"));

    auto tools = client.list_tools();
    CHECK_EQ(tools.size(), static_cast<size_t>(2));
    CHECK_EQ(tools.at(0).name, std::string("greet"));

    auto r1 = client.call_tool("greet", Json::object({{"who", Json("ada")}, {"shout", Json(true)}}));
    CHECK_EQ(r1.text_content(), std::string("hello, ADA"));
    CHECK_EQ(r1.is_error, false);

    auto r2 = client.call_tool("multi");
    CHECK_EQ(r2.content.size(), static_cast<size_t>(2));
    CHECK(r2.content.at(1).kind == Content::Kind::Resource);
    CHECK_EQ(r2.content.at(1).blob_text_, std::string("remember the milk"));

    bool threw = false;
    try {
        client.call_tool("nope");
    } catch (const McpRemoteError& e) {
        threw = true;
        CHECK_EQ(e.code, rpc::INVALID_PARAMS);
    }
    CHECK(threw);
}

int main() {
    test_initialize_handshake();
    test_before_initialize_gated();
    test_tools_flow();
    test_resources_and_prompts();
    test_error_envelopes();
    test_end_to_end_client_server();
    std::printf("test_protocol: %d checks, %d failures\n", mcpptest::g_checks, mcpptest::g_failures);
    return mcpptest::g_failures == 0 ? 0 : 1;
}
