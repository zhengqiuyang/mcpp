// End-to-end agent test with zero external dependencies: a scripted
// OpenAI-compatible mock LLM served by mcpp's own HttpTransport, a real
// McpServer over HTTP, and mcpp's Agent loop driving both. Validates the
// full path: tool advertisement -> tool_calls -> MCP dispatch -> tool
// results fed back -> final answer.
#include "mcpp/agent.hpp"
#include "mcpp/http_transport.hpp"
#include "mcpp/server.hpp"
#include "test_util.hpp"

#include <cstdio>
#include <string>
#include <thread>

using namespace mcpp;

namespace {

// A tiny scripted /chat/completions endpoint riding HttpTransport directly.
class MockLlm {
public:
    MockLlm() : transport_(0, "127.0.0.1") {
        serve_ = std::thread([this] {
            std::string body;
            while (transport_.read_message(body)) {
                ++requests_;
                Json req = Json::parse(body);
                request_messages_ = req.get("messages");
                if (const Json* tools = req.find("tools")) request_tools_ = *tools;
                last_request_head_ = transport_.last_request_head();
                transport_.write_message(scripted_response(requests_).dump());
            }
        });
    }
    ~MockLlm() {
        transport_.stop();
        if (serve_.joinable()) serve_.join();
    }

    uint16_t port() const { return transport_.port(); }
    int requests() const { return requests_; }
    const Json& last_messages() const { return request_messages_; }
    const Json& last_tools() const { return request_tools_; }
    const std::string& last_request_head() const { return last_request_head_; }

private:
    static Json scripted_response(int turn) {
        Json msg = Json::object();
        msg["role"] = "assistant";
        if (turn == 1 || turn == 2) {
            msg["content"] = "";
            Json call = Json::object();
            call["id"] = "call-" + std::to_string(turn);
            call["type"] = "function";
            call["function"] = Json::object();
            call["function"]["name"] = turn == 1 ? "add" : "greet";
            call["function"]["arguments"] = turn == 1 ? "{\"a\":19,\"b\":23}"
                                                      : "{\"name\":\"ada\"}";
            msg["tool_calls"] = Json::array();
            msg["tool_calls"].push_back(std::move(call));
        } else {
            msg["content"] = "19+23=42 and hello ada";
        }
        Json resp = Json::object();
        resp["choices"] = Json::array();
        Json choice = Json::object();
        choice["message"] = std::move(msg);
        resp["choices"].push_back(std::move(choice));
        return resp;
    }

    HttpTransport transport_;
    std::thread serve_;
    int requests_ = 0;
    Json request_messages_;
    Json request_tools_;
    std::string last_request_head_;
};

}  // namespace

int main() {
    // ---- the MCP side: a real server over HTTP with two tools -----------
    McpServer server("agent.test", "1.0");
    server.add_tool("add", "Add two integers",
                    {{"a", "integer", "l"}, {"b", "integer", "r"}},
                    [](const Json& a) {
                        return ToolResult::text(std::to_string(a.get("a").as_int() +
                                                               a.get("b").as_int()));
                    });
    server.add_tool("greet", "Greet by name", {{"name", "string", "who"}},
                    [](const Json& a) {
                        return ToolResult::text("hello " + a.get("name").as_string());
                    });
    HttpTransport mcp_transport(0, "127.0.0.1");
    std::thread mcp_thread([&] { server.run(mcp_transport); });

    MockLlm llm;

    {
        // ---- the agent: mcpp client + mcpp backend + mcpp loop ----------
        HttpClientTransport tools_http("http://127.0.0.1:" +
                                        std::to_string(mcp_transport.port()) + "/mcp");
        McpClient mcp(tools_http);
        mcp.initialize("agent-test", "1.0");

        OpenAiChatBackend backend("http://127.0.0.1:" + std::to_string(llm.port()) +
                                      "/v1",
                                  "mock-model", "test-key-123");
        Agent agent(backend, mcp, "be terse");

        int tool_calls_seen = 0;
        agent.on_step([&](const Json& msg) {
            if (const Json* calls = msg.find("tool_calls")) tool_calls_seen += static_cast<int>(calls->size());
            return true;
        });

        AgentResult r = agent.run("compute 19+23 and greet ada", 6);

        CHECK_EQ(r.final_text, std::string("19+23=42 and hello ada"));
        CHECK_EQ(r.turns, 3);
        CHECK_EQ(r.hit_turn_limit, false);
        CHECK_EQ(tool_calls_seen, 2);
        CHECK_EQ(llm.requests(), 3);

        // the first request advertised both MCP tools as OpenAI functions
        CHECK_EQ(llm.last_tools().size(), static_cast<size_t>(2));
        bool saw_add = false;
        for (const Json& t : llm.last_tools().as_array())
            if (t["function"].get("name").as_string() == "add") saw_add = true;
        CHECK(saw_add);

        // the final request contained the tool results fed back
        bool saw_add_result = false, saw_greet_result = false;
        for (const Json& m : llm.last_messages().as_array()) {
            if (m.get_or<std::string>("role", "") != "tool") continue;
            const std::string& c = m.get_or<std::string>("content", "");
            if (c == "42") saw_add_result = true;
            if (c == "hello ada") saw_greet_result = true;
        }
        CHECK(saw_add_result);
        CHECK(saw_greet_result);

        // the custom Authorization header reached the LLM endpoint
        CHECK(llm.last_request_head().find("Authorization: Bearer test-key-123") !=
              std::string::npos);

        // abort hook: returning false stops the loop
        Agent halted(backend, mcp, "");
        halted.on_step([](const Json&) { return false; });
        AgentResult hr = halted.run("anything", 5);
        CHECK(hr.hit_turn_limit);  // stopped without a final answer
        CHECK(hr.final_text.empty());
    }

    mcp_transport.stop();
    mcp_thread.join();

    std::printf("test_agent: %d checks, %d failures\n", mcpptest::g_checks,
                mcpptest::g_failures);
    return mcpptest::g_failures == 0 ? 0 : 1;
}
