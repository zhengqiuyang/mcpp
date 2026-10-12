// v0.2 feature tests: list pagination, progress notifications, cooperative
// cancellation, and HTTP sessions (Mcp-Session-Id + DELETE).
#include "mcpp/client.hpp"
#include "mcpp/http_client_transport.hpp"
#include "mcpp/http_transport.hpp"
#include "mcpp/server.hpp"
#include "test_util.hpp"

#include <stdexcept>
#include <thread>

using namespace mcpp;

// ---------------------------------------------------------------- pagination

static void test_pagination() {
    ServerOptions opts;
    opts.page_size = 2;
    McpServer server("page.test", "1.0", opts);
    server.add_tool("t1", "one", {}, [](const Json&) { return ToolResult::text("1"); });
    server.add_tool("t2", "two", {}, [](const Json&) { return ToolResult::text("2"); });
    server.add_tool("t3", "three", {}, [](const Json&) { return ToolResult::text("3"); });
    server.handle_message(msg::notification("notifications/initialized"));

    Json p1 = server.handle_message(msg::request(Json(1), "tools/list")).value();
    CHECK_EQ(p1["result"]["tools"].size(), static_cast<size_t>(2));
    CHECK_EQ(p1["result"].get("nextCursor").as_string(), std::string("2"));

    Json page2 = msg::request(Json(2), "tools/list");
    page2["params"]["cursor"] = "2";
    Json p2 = server.handle_message(page2).value();
    CHECK_EQ(p2["result"]["tools"].size(), static_cast<size_t>(1));
    CHECK(!p2["result"].contains("nextCursor"));

    // out-of-range cursor clamps to the end, no crash
    Json page9 = msg::request(Json(3), "tools/list");
    page9["params"]["cursor"] = "99";
    Json p9 = server.handle_message(page9).value();
    CHECK_EQ(p9["result"]["tools"].size(), static_cast<size_t>(0));
}

// ------------------------------------------------------- progress & cancel

class RecordingTransport : public Transport {
public:
    bool read_message(std::string&) override { return false; }
    void write_message(const std::string& msg) override { sent.push_back(msg); }
    std::vector<std::string> sent;
};

static void test_progress_notifications() {
    McpServer server("prog.test", "1.0");
    server.add_tool("work", "long work",
                    {{"n", "integer", "steps"}},
                    [](const Json& a, ToolContext& ctx) {
                        int64_t n = a.get("n").as_int();
                        for (int64_t i = 1; i <= n; ++i)
                            ctx.report_progress(i, n, "step " + std::to_string(i));
                        return ToolResult::text("done");
                    });
    server.handle_message(msg::notification("notifications/initialized"));

    RecordingTransport sink;
    Json call = msg::request(Json(11), "tools/call");
    call["params"]["name"] = "work";
    call["params"]["arguments"]["n"] = 3;
    call["params"]["_meta"]["progressToken"] = 77;
    auto reply = server.handle_message(call, &sink);
    CHECK(reply.has_value());
    CHECK_EQ(reply->get("result").get("content").at(0).get("text").as_string(),
             std::string("done"));
    CHECK_EQ(sink.sent.size(), static_cast<size_t>(3));
    Json first = Json::parse(sink.sent.at(0));
    CHECK_EQ(first.get("method").as_string(), std::string("notifications/progress"));
    CHECK_EQ(first["params"].get("progressToken").as_int(), 77);
    CHECK_EQ(first["params"].get("progress").as_int(), 1);
    CHECK_EQ(first["params"].get("total").as_int(), 3);
    CHECK_EQ(first["params"].get("message").as_string(), std::string("step 1"));

    // without a progressToken nothing is emitted
    RecordingTransport sink2;
    Json quiet = msg::request(Json(12), "tools/call");
    quiet["params"]["name"] = "work";
    quiet["params"]["arguments"]["n"] = 2;
    server.handle_message(quiet, &sink2);
    CHECK(sink2.sent.empty());
}

static void test_cancellation_checkpoint() {
    McpServer server("cancel.test", "1.0");
    server.add_tool("chatty", "checks cancellation", {},
                    [](const Json&, ToolContext& ctx) -> ToolResult {
                        ctx.cancel_checkpoint();  // throws when cancelled
                        return ToolResult::text("ran to completion");
                    });
    server.handle_message(msg::notification("notifications/initialized"));

    // normal path: not cancelled
    Json ok = msg::request(Json(1), "tools/call");
    ok["params"]["name"] = "chatty";
    auto r1 = server.handle_message(ok);
    CHECK_EQ(r1->get("result").get("content").at(0).get("text").as_string(),
             std::string("ran to completion"));

    // pre-cancelled id -> checkpoint throws REQUEST_CANCELLED
    Json cancel = msg::notification("notifications/cancelled");
    cancel["params"]["requestId"] = 42;
    CHECK(!server.handle_message(cancel).has_value());
    Json doomed = msg::request(Json(42), "tools/call");
    doomed["params"]["name"] = "chatty";
    auto r2 = server.handle_message(doomed);
    CHECK_EQ(r2->get("error").get("code").as_int(), mcp_error::REQUEST_CANCELLED);
}

// ------------------------------------------------------------ HTTP sessions

static void test_http_sessions() {
    McpServer server("sess.test", "1.0");
    server.add_tool("add", "Add two integers",
                    {{"a", "integer", "left"}, {"b", "integer", "right"}},
                    [](const Json& a) {
                        return ToolResult::text(std::to_string(a.get("a").as_int() +
                                                               a.get("b").as_int()));
                    });
    HttpTransport server_transport(0, "127.0.0.1", HttpTransport::Mode::Sessions);
    const uint16_t port = server_transport.port();
    std::thread server_thread([&] { server.run(server_transport); });

    {
        HttpClientTransport ct("http://127.0.0.1:" + std::to_string(port) + "/mcp");
        McpClient client(ct);

        auto info = client.initialize("sess-test", "1.0");
        CHECK_EQ(info.name, std::string("sess.test"));
        CHECK(ct.session_id().rfind("mcp-", 0) == 0);
        CHECK(ct.last_response_headers().find("Mcp-Session-Id") != std::string::npos);

        // the session id is attached to subsequent requests automatically
        auto r = client.call_tool("add", Json{{"a", 20}, {"b", 22}});
        CHECK_EQ(r.text_content(), std::string("42"));

        ct.end_session();
        CHECK(ct.session_id().empty());

        // after DELETE, a fresh session-less POST must be rejected with 400
        bool rejected = false;
        try {
            HttpClientTransport ct2("http://127.0.0.1:" + std::to_string(port) + "/mcp");
            McpClient c2(ct2);
            c2.list_tools();  // non-initialize without session header
        } catch (const std::runtime_error& e) {
            rejected = std::string(e.what()).find("400") != std::string::npos;
        }
        CHECK(rejected);
    }

    server_transport.stop();
    server_thread.join();
}

// --------------------------------------------------- argument validation

static void test_argument_validation() {
    McpServer server("valid.test", "1.0");
    server.add_tool("add", "Add two integers",
                    {{"a", "integer", "left"}, {"b", "integer", "right"}},
                    [](const Json& a) {
                        return ToolResult::text(std::to_string(a.get("a").as_int() +
                                                               a.get("b").as_int()));
                    });
    server.handle_message(msg::notification("notifications/initialized"));

    auto call = [](Json args) {
        Json c = msg::request(Json(9), "tools/call");
        c["params"]["name"] = "add";
        c["params"]["arguments"] = std::move(args);
        return c;
    };
    auto err_code = [](const std::optional<Json>& r) {
        return r->get("error").get("code").as_int();
    };
    auto err_text = [](const std::optional<Json>& r) {
        return r->get("error").get("message").as_string();
    };

    // valid arguments pass
    Json ok_args = Json::object();
    ok_args["a"] = 1;
    ok_args["b"] = 2;
    auto ok = server.handle_message(call(std::move(ok_args)));
    CHECK(ok->contains("result"));

    // missing required property
    Json missing = Json::object();
    missing["a"] = 1;
    auto r1 = server.handle_message(call(std::move(missing)));
    CHECK_EQ(err_code(r1), rpc::INVALID_PARAMS);
    CHECK(err_text(r1).find("missing required property \"b\"") != std::string::npos);

    // wrong type
    Json wrong = Json::object();
    wrong["a"] = "one";
    wrong["b"] = 2;
    auto r2 = server.handle_message(call(std::move(wrong)));
    CHECK_EQ(err_code(r2), rpc::INVALID_PARAMS);
    CHECK(err_text(r2).find("expected integer") != std::string::npos);

    // extra unknown properties are allowed (schemas here are open)
    Json extra = Json::object();
    extra["a"] = 1;
    extra["b"] = 2;
    extra["verbose"] = true;
    auto r3 = server.handle_message(call(std::move(extra)));
    CHECK(r3->contains("result"));

    // opting out restores lenient behaviour (handler sees the raw args)
    ServerOptions lax;
    lax.validate_arguments = false;
    McpServer lenient("valid.test", "1.0", lax);
    lenient.add_tool("add", "Add two integers",
                     {{"a", "integer", "left"}, {"b", "integer", "right"}},
                     [](const Json& a) {
                         return ToolResult::text(std::to_string(
                             a.get("a").as_int() + a.get_or<int64_t>("b", 0)));
                     });
    lenient.handle_message(msg::notification("notifications/initialized"));
    Json partial = Json::object();
    partial["a"] = 7;
    auto r4 = lenient.handle_message(call(std::move(partial)));
    CHECK_EQ(r4->get("result").get("content").at(0).get("text").as_string(),
             std::string("7"));
}

int main() {
    test_pagination();
    test_progress_notifications();
    test_cancellation_checkpoint();
    test_http_sessions();
    test_argument_validation();
    std::printf("test_features: %d checks, %d failures\n", mcpptest::g_checks,
                mcpptest::g_failures);
    return mcpptest::g_failures == 0 ? 0 : 1;
}
