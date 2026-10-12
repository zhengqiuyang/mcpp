// mcpp/client.hpp — minimal MCP client.
//
// Synchronous request/response over any Transport. The client skips server
// notifications, answers server-side "ping" probes, and surfaces error
// responses as McpRemoteError. Enough for testing servers, bridging, and
// embedding an MCP client into a C++ application.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "json.hpp"
#include "protocol.hpp"
#include "transport.hpp"

namespace mcpp {

struct McpRemoteError : std::runtime_error {
    int code;
    Json data;
    McpRemoteError(int c, const std::string& message, Json d = nullptr)
        : std::runtime_error(message), code(c), data(std::move(d)) {}
};

class McpClient {
public:
    struct ServerInfo {
        std::string name;
        std::string version;
        std::string instructions;
    };
    struct ToolInfo {
        std::string name;
        std::string description;
        Json input_schema;
    };

    explicit McpClient(Transport& transport) : transport_(transport) {}

    // ---- server-initiated traffic -----------------------------------------
    //
    // Requests the server may send us (sampling/createMessage, roots/list,
    // elicitation/create, ping). Return the result object; throw
    // McpRemoteError to answer with that error. Unhandled methods get
    // METHOD_NOT_FOUND, which is spec-correct for unsupported capabilities.
    void set_request_handler(const std::string& method,
                             std::function<Json(const Json&)> handler) {
        request_handlers_[method] = std::move(handler);
    }
    // Convenience: sampling/createMessage — return {role, content:{...}}.
    void set_sampling_handler(std::function<Json(const Json&)> handler) {
        set_request_handler("sampling/createMessage", std::move(handler));
    }
    // Convenience: roots/list — return a JSON array of {uri, name}.
    void set_roots_handler(std::function<Json(const Json&)> handler) {
        set_request_handler("roots/list", std::move(handler));
    }
    // Convenience: elicitation/create — return {action, content?}.
    void set_elicitation_handler(std::function<Json(const Json&)> handler) {
        set_request_handler("elicitation/create", std::move(handler));
    }
    // Notifications from the server (notifications/tools/list_changed,
    // notifications/resources/updated, notifications/message,
    // notifications/progress...). Unhandled notifications are ignored.
    void set_notification_handler(const std::string& method,
                                  std::function<void(const Json&)> handler) {
        notification_handlers_[method] = std::move(handler);
    }

    // Optional hook invoked when the transport has no data while awaiting a
    // response. Return true if the hook may have produced new input (the
    // client retries the read); false means "give up". Used by single-threaded
    // tests and embedded integrations that pump the transport externally.
    void set_idle_hook(std::function<bool()> hook) { idle_hook_ = std::move(hook); }

    // Raw JSON-RPC call: sends a request, blocks for the matching response,
    // returns its "result". Throws McpRemoteError on error responses.
    Json call(const std::string& method, Json params = nullptr) {
        Json id = next_id_++;
        transport_.write_message(msg::request(id, method, std::move(params)).dump());
        return await_response(id);
    }

    void notify(const std::string& method, Json params = nullptr) {
        transport_.write_message(msg::notification(method, std::move(params)).dump());
        transport_.drain_response();
    }

    // Typed convenience: full initialize handshake. Call exactly once.
    ServerInfo initialize(const std::string& client_name = "mcpp-client",
                          const std::string& client_version = "1.0.0") {
        Json params = Json::object();
        params["protocolVersion"] = kLatestProtocolVersion;
        params["capabilities"] = Json::object();
        Json info = Json::object();
        info["name"] = client_name;
        info["version"] = client_version;
        params["clientInfo"] = std::move(info);

        Json result = call("initialize", std::move(params));
        notify("notifications/initialized");

        ServerInfo si;
        si.name = result.get_or<std::string>("name", "");
        if (const Json* srv = result.find("serverInfo")) {
            si.name = srv->get_or<std::string>("name", "");
            si.version = srv->get_or<std::string>("version", "");
        }
        si.instructions = result.get_or<std::string>("instructions", "");
        server_version_ = result.get_or<std::string>("protocolVersion", "");
        return si;
    }

    std::vector<ToolInfo> list_tools() {
        Json result = call("tools/list");
        std::vector<ToolInfo> out;
        if (const Json* tools = result.find("tools")) {
            for (const Json& t : tools->as_array()) {
                ToolInfo ti;
                ti.name = t.get("name").as_string();
                ti.description = t.get_or<std::string>("description", "");
                if (const Json* schema = t.find("inputSchema")) ti.input_schema = *schema;
                out.push_back(std::move(ti));
            }
        }
        return out;
    }

    ToolResult call_tool(const std::string& name, Json args = Json::object()) {
        Json params = Json::object();
        params["name"] = name;
        params["arguments"] = std::move(args);
        Json result = call("tools/call", std::move(params));

        ToolResult tr;
        tr.is_error = result.get_or<bool>("isError", false);
        if (const Json* content = result.find("content"))
            for (const Json& c : content->as_array())
                tr.content.push_back(Content::from_json(c));
        return tr;
    }

    const std::string& negotiated_protocol_version() const { return server_version_; }

private:
    Json await_response(const Json& id) {
        std::string line;
        for (;;) {
            if (!transport_.read_message(line)) {
                if (idle_hook_ && idle_hook_()) continue;
                throw std::runtime_error("mcpp: transport closed while waiting for response");
            }
            Json m;
            try {
                m = Json::parse(line);
            } catch (const Json::Error&) {
                continue;  // skip malformed lines from the peer
            }
            if (!m.is_object()) continue;

            // Server -> client request: answer pings, route the rest to
            // registered handlers (sampling / roots / elicitation / custom).
            if (msg::has_method(m)) {
                const std::string method = m.get_or<std::string>("method", "");
                if (!m.contains("id")) {
                    if (auto it = notification_handlers_.find(method);
                        it != notification_handlers_.end())
                        it->second(m.contains("params") ? m.get("params") : Json::object());
                    continue;  // other notifications: ignore
                }
                Json reply;
                if (method == "ping") {
                    reply = msg::response(m.get("id"), Json::object());
                } else if (auto it = request_handlers_.find(method);
                           it != request_handlers_.end()) {
                    try {
                        Json result = it->second(
                            m.contains("params") ? m.get("params") : Json::object());
                        reply = msg::response(m.get("id"), std::move(result));
                    } catch (const McpRemoteError& e) {
                        reply = msg::error(m.get("id"), e.code, e.what(), e.data);
                    } catch (const std::exception& e) {
                        reply = msg::error(m.get("id"), rpc::INTERNAL_ERROR, e.what());
                    }
                    transport_.write_message(reply.dump());
                    continue;
                } else {
                    reply = msg::error(m.get("id"), rpc::METHOD_NOT_FOUND,
                                       "client does not implement: " + method);
                }
                transport_.write_message(reply.dump());
                continue;
            }

            // A response — is it ours?
            const Json* rid = m.find("id");
            if (!rid || !(*rid == id)) continue;
            if (const Json* err = m.find("error")) {
                int code = err->get_or<int64_t>("code", rpc::INTERNAL_ERROR);
                std::string text = err->get_or<std::string>("message", "unknown error");
                throw McpRemoteError(code, text, err->find("data") ? *err->find("data") : Json());
            }
            const Json* result = m.find("result");
            return result ? *result : Json::object();
        }
    }

    Transport& transport_;
    int64_t next_id_ = 1;
    std::string server_version_;
    std::function<bool()> idle_hook_;
    std::map<std::string, std::function<Json(const Json&)>> request_handlers_;
    std::map<std::string, std::function<void(const Json&)>> notification_handlers_;
};

}  // namespace mcpp
