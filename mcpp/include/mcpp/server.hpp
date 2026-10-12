// mcpp/server.hpp — MCP server core: registration API + JSON-RPC dispatch.
//
// Usage:
//   mcpp::McpServer server("com.example.demo", "1.0.0");
//   server.add_tool("echo", "Echo a message", {{"message", "string", "text to echo"}},
//                   [](const mcpp::Json& args) {
//                       return mcpp::ToolResult::text(args.get("message").as_string());
//                   });
//   server.run(mcpp::StdioTransport{});
//
// Handlers run on the transport read thread; throw RpcError to control the
// error code, any other exception becomes INTERNAL_ERROR. Tool *execution*
// failures should be returned via ToolResult::error(...) per the MCP spec.

#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "json.hpp"
#include "protocol.hpp"
#include "transport.hpp"
#include "version.hpp"

namespace mcpp {

struct ServerOptions {
    std::string instructions;  // optional instructions surfaced at initialize
    // Page size for tools/resources/prompts list responses. Spec clients page
    // via cursor; servers that always return everything may leave this large.
    size_t page_size = 1000;
    // Validate tools/call arguments against each tool's inputSchema and
    // reject mismatches with -32602 before the handler runs. Handlers should
    // treat arguments as pre-checked when this is on (the default).
    bool validate_arguments = true;
};

// Per-request context handed to two-argument tool handlers: progress
// reporting (only visible when the request carried a _meta.progressToken and
// the server runs over a transport) and cooperative cancellation checkpoints.
// With the synchronous run() loop, cancellation is checked between messages,
// so checkpoints mainly serve threaded integrations.
class ToolContext {
public:
    // Fire a notifications/progress message bound to the caller's
    // progressToken; no-op when the token is absent (spec: optional).
    void report_progress(int64_t progress, std::optional<int64_t> total = std::nullopt,
                         const std::string& message = "") {
        if (!has_token_ || !sink_) return;
        Json p = Json::object();
        p["progressToken"] = progress_token_;
        p["progress"] = progress;
        if (total) p["total"] = *total;
        if (!message.empty()) p["message"] = message;
        sink_->write_message(msg::notification("notifications/progress", std::move(p)).dump());
    }

    // Throw REQUEST_CANCELLED if the client cancelled this request.
    void cancel_checkpoint() {
        if (cancelled_) throw RpcError(mcp_error::REQUEST_CANCELLED, "request cancelled");
    }

    bool cancelled() const { return cancelled_; }

private:
    friend class McpServer;
    Json progress_token_;
    bool has_token_ = false;
    Transport* sink_ = nullptr;
    bool cancelled_ = false;
};

class McpServer {
public:
    using ToolHandler = std::function<ToolResult(const Json&)>;
    using ToolContextHandler = std::function<ToolResult(const Json&, ToolContext&)>;
    using ResourceHandler = std::function<std::string()>;  // returns the text body
    using PromptHandler = std::function<PromptResult(const Json& args)>;

    McpServer(std::string server_name, std::string server_version,
              ServerOptions options = {})
        : name_(std::move(server_name)),
          version_(std::move(server_version)),
          options_(std::move(options)) {}

    // ---------------------------------------------------------- registration

    void add_tool(std::string tool_name, std::string description,
                  std::vector<ToolParam> params, ToolHandler handler) {
        add_tool_raw(std::move(tool_name), std::move(description),
                     build_input_schema(params), std::move(handler));
    }

    // Register with a hand-written JSON Schema (full control over nested
    // types, enums, defaults …). `input_schema` must be an object.
    void add_tool_raw(std::string tool_name, std::string description,
                      Json input_schema, ToolHandler handler) {
        Tool t;
        t.name = std::move(tool_name);
        t.description = std::move(description);
        t.input_schema = std::move(input_schema);
        t.handler = std::move(handler);
        tools_.push_back(std::move(t));
    }

    // Context-aware variant: handlers receive progress/cancellation plumbing.
    void add_tool(std::string tool_name, std::string description,
                  std::vector<ToolParam> params, ToolContextHandler handler) {
        Tool t;
        t.name = std::move(tool_name);
        t.description = std::move(description);
        t.input_schema = build_input_schema(params);
        t.ctx_handler = std::move(handler);
        tools_.push_back(std::move(t));
    }

    void add_resource(std::string uri, std::string resource_name, std::string mime,
                      ResourceHandler handler) {
        Resource r;
        r.uri = std::move(uri);
        r.name = std::move(resource_name);
        r.mime = std::move(mime);
        r.handler = std::move(handler);
        resources_.push_back(std::move(r));
    }

    void add_prompt(std::string prompt_name, std::string description,
                    std::vector<ToolParam> args, PromptHandler handler) {
        Prompt p;
        p.name = std::move(prompt_name);
        p.description = std::move(description);
        p.args = std::move(args);
        p.handler = std::move(handler);
        prompts_.push_back(std::move(p));
    }

    // ------------------------------------------------------------- run loop

    // Serve requests on the transport until EOF. Notifications produce no
    // reply; the transport decides what that means (silence on stdio, a 202
    // on HTTP).
    void run(Transport& transport) {
        std::string line;
        while (transport.read_message(line)) {
            auto reply = handle_line(line, &transport);
            transport.write_message(reply ? *reply : std::string());
        }
    }

    // Handle one raw (serialized) message; returns the serialized reply, or
    // nullopt when there is nothing to send (notifications, parse-and-ignore).
    // `sink` (optional) enables server-initiated notifications (progress).
    std::optional<std::string> handle_line(std::string_view line, Transport* sink = nullptr) {
        Json message;
        try {
            message = Json::parse(line);
        } catch (const Json::Error& e) {
            return msg::error(Json(nullptr), rpc::PARSE_ERROR, e.what()).dump();
        }
        auto reply = handle_message(message, sink);
        if (!reply) return std::nullopt;
        return reply->dump();
    }

    // Handle one parsed message. Never throws for input problems — malformed
    // requests produce error responses. Handler exceptions are caught.
    std::optional<Json> handle_message(const Json& message, Transport* sink = nullptr) {
        active_sink_ = sink;
        if (!message.is_object())
            return msg::error(Json(nullptr), rpc::INVALID_REQUEST,
                              "message must be a JSON object");

        const bool has_id = message.contains("id");
        Json id = has_id ? message.get("id") : Json(nullptr);

        // Responses to requests this server may have issued as a client:
        // nothing to do (client role lives in mcpp/client.hpp).
        if (msg::is_response(message)) return std::nullopt;

        if (!message.contains("method"))
            return has_id ? std::optional<Json>(msg::error(
                                id, rpc::INVALID_REQUEST, "missing \"method\""))
                          : std::nullopt;

        std::string method;
        try {
            method = message.get("method").as_string();
        } catch (const std::exception&) {
            return has_id ? std::optional<Json>(msg::error(
                                id, rpc::INVALID_REQUEST, "\"method\" must be a string"))
                          : std::nullopt;
        }

        static const Json kNoParams = Json::object();
        const Json& params = message.contains("params") ? message.get("params") : kNoParams;

        // ---- lifecycle -------------------------------------------------
        if (method == "initialize") return msg::response(id, build_initialize_result(params));
        if (method == "notifications/initialized") {
            ready_ = true;
            return std::nullopt;
        }
        if (method == "exit") return std::nullopt;  // convenience for local testing

        // Cancellation arrives as a notification — handle before the
        // generic "never reply to notifications" rule below.
        if (method == "notifications/cancelled") {
            if (const Json* rid = params.find("requestId"))
                cancelled_ids_.insert(rid->dump());
            return std::nullopt;
        }

        // JSON-RPC: never reply to notifications, whatever the method says.
        if (!has_id) return std::nullopt;

        if (!ready_ && method != "ping") {
            if (!has_id) return std::nullopt;
            return msg::error(id, mcp_error::SERVER_NOT_INITIALIZED,
                              "server not initialized; send \"initialize\" first");
        }

        // ---- utilities -------------------------------------------------
        if (method == "ping") return msg::response(id, Json::object());

        // ---- tools -----------------------------------------------------
        if (method == "tools/list") return msg::response(id, build_tools_list(params));
        if (method == "tools/call") return handle_tools_call(id, params);

        // ---- resources -------------------------------------------------
        if (method == "resources/list")
            return msg::response(id, build_resources_list(params));
        if (method == "resources/read") return handle_resources_read(id, params);

        // ---- prompts ---------------------------------------------------
        if (method == "prompts/list") return msg::response(id, build_prompts_list(params));
        if (method == "prompts/get") return handle_prompts_get(id, params);

        // Unknown method: error for requests, silence for notifications.
        if (!has_id) return std::nullopt;
        return msg::error(id, rpc::METHOD_NOT_FOUND, "unknown method: " + method);
    }

    // ------------------------------------------------------------- status

    bool ready() const { return ready_; }
    size_t tool_count() const { return tools_.size(); }
    const std::string& name() const { return name_; }
    const std::string& version() const { return version_; }

private:
    struct Tool {
        std::string name;
        std::string description;
        Json input_schema;
        ToolHandler handler;              // legacy one-argument form
        ToolContextHandler ctx_handler;   // context-aware form (takes precedence)
    };
    struct Resource {
        std::string uri;
        std::string name;
        std::string mime;
        ResourceHandler handler;
    };
    struct Prompt {
        std::string name;
        std::string description;
        std::vector<ToolParam> args;
        PromptHandler handler;
    };

    Json build_initialize_result(const Json& params) {
        // Echo the client's version when we speak it, else advertise ours.
        std::string requested = params.get_or<std::string>("protocolVersion", "");
        std::string negotiated = known_protocol_version(requested) ? requested
                                                                   : kLatestProtocolVersion;
        Json result = Json::object();
        result["protocolVersion"] = negotiated;

        Json caps = Json::object();
        if (!tools_.empty()) {
            caps["tools"] = Json::object();
            caps["tools"]["listChanged"] = false;
        }
        if (!resources_.empty()) {
            caps["resources"] = Json::object();
            caps["resources"]["subscribe"] = false;
            caps["resources"]["listChanged"] = false;
        }
        if (!prompts_.empty()) {
            caps["prompts"] = Json::object();
            caps["prompts"]["listChanged"] = false;
        }
        result["capabilities"] = std::move(caps);

        Json info = Json::object();
        info["name"] = name_;
        info["version"] = version_;
        result["serverInfo"] = std::move(info);
        if (!options_.instructions.empty()) result["instructions"] = options_.instructions;
        return result;
    }

    // Opaque list cursor: an index into the registration order.
    static size_t parse_cursor(const Json& params) {
        std::string c = params.get_or<std::string>("cursor", "");
        if (c.empty()) return 0;
        return static_cast<size_t>(std::strtoull(c.c_str(), nullptr, 10));
    }

    void apply_page(const Json& params, size_t total, size_t& begin, size_t& end,
                    Json& result) const {
        begin = std::min(parse_cursor(params), total);
        end = std::min(begin + options_.page_size, total);
        if (end < total) result["nextCursor"] = std::to_string(end);
    }

    Json build_tools_list(const Json& params) const {
        Json result = Json::object();
        size_t begin = 0, end = 0;
        apply_page(params, tools_.size(), begin, end, result);
        result["tools"] = Json::array();
        for (size_t i = begin; i < end; ++i) {
            const auto& t = tools_[i];
            Json jt = Json::object();
            jt["name"] = t.name;
            jt["description"] = t.description;
            jt["inputSchema"] = t.input_schema;
            result["tools"].push_back(std::move(jt));
        }
        return result;
    }

    std::optional<Json> handle_tools_call(const Json& id, const Json& params) {
        if (!params.contains("name") || !params.get("name").is_string())
            return msg::error(id, rpc::INVALID_PARAMS, "tools/call requires string \"name\"");
        const std::string& name = params.get("name").as_string();
        auto it = std::find_if(tools_.begin(), tools_.end(),
                               [&](const Tool& t) { return t.name == name; });
        if (it == tools_.end())
            return msg::error(id, rpc::INVALID_PARAMS, "unknown tool: " + name);

        Json empty_args = Json::object();
        const Json* args = params.find("arguments");
        if (args && !args->is_object())
            return msg::error(id, rpc::INVALID_PARAMS, "\"arguments\" must be an object");
        if (!args) args = &empty_args;

        if (options_.validate_arguments) {
            std::string err = validate_against_schema(*args, it->input_schema);
            if (!err.empty())
                return msg::error(id, rpc::INVALID_PARAMS, "invalid arguments: " + err);
        }

        ToolContext ctx;
        if (const Json* meta = params.find("_meta")) {
            if (const Json* token = meta->find("progressToken")) {
                ctx.progress_token_ = *token;
                ctx.has_token_ = true;
            }
        }
        ctx.sink_ = active_sink_;
        ctx.cancelled_ = cancelled_ids_.count(id.dump()) > 0;

        try {
            ToolResult result = it->ctx_handler  ? it->ctx_handler(*args, ctx)
                                                 : it->handler(*args);
            return msg::response(id, result.to_json());
        } catch (const RpcError& e) {
            return msg::error(id, e.code, e.what(), e.data);
        } catch (const std::exception& e) {
            return msg::error(id, rpc::INTERNAL_ERROR, e.what());
        } catch (...) {
            return msg::error(id, rpc::INTERNAL_ERROR, "unknown handler exception");
        }
    }

    Json build_resources_list(const Json& params) const {
        Json result = Json::object();
        size_t begin = 0, end = 0;
        apply_page(params, resources_.size(), begin, end, result);
        result["resources"] = Json::array();
        for (size_t i = begin; i < end; ++i) {
            const auto& r = resources_[i];
            Json jr = Json::object();
            jr["uri"] = r.uri;
            jr["name"] = r.name;
            jr["mimeType"] = r.mime;
            result["resources"].push_back(std::move(jr));
        }
        return result;
    }

    std::optional<Json> handle_resources_read(const Json& id, const Json& params) {
        if (!params.contains("uri") || !params.get("uri").is_string())
            return msg::error(id, rpc::INVALID_PARAMS, "resources/read requires string \"uri\"");
        const std::string& uri = params.get("uri").as_string();
        auto it = std::find_if(resources_.begin(), resources_.end(),
                               [&](const Resource& r) { return r.uri == uri; });
        if (it == resources_.end())
            return msg::error(id, rpc::INVALID_PARAMS, "unknown resource: " + uri);
        try {
            Json contents = Json::object();
            contents["uri"] = it->uri;
            contents["mimeType"] = it->mime;
            contents["text"] = it->handler();
            Json result = Json::object();
            result["contents"] = Json::array();
            result["contents"].push_back(std::move(contents));
            return msg::response(id, std::move(result));
        } catch (const std::exception& e) {
            return msg::error(id, rpc::INTERNAL_ERROR, e.what());
        }
    }

    Json build_prompts_list(const Json& params) const {
        Json result = Json::object();
        size_t begin = 0, end = 0;
        apply_page(params, prompts_.size(), begin, end, result);
        result["prompts"] = Json::array();
        for (size_t i = begin; i < end; ++i) {
            const auto& p = prompts_[i];
            Json jp = Json::object();
            jp["name"] = p.name;
            jp["description"] = p.description;
            if (!p.args.empty()) {
                Json args = Json::array();
                for (const auto& a : p.args) {
                    Json ja = Json::object();
                    ja["name"] = a.name;
                    ja["description"] = a.description;
                    ja["required"] = a.required;
                    args.push_back(std::move(ja));
                }
                jp["arguments"] = std::move(args);
            }
            result["prompts"].push_back(std::move(jp));
        }
        return result;
    }

    std::optional<Json> handle_prompts_get(const Json& id, const Json& params) {
        if (!params.contains("name") || !params.get("name").is_string())
            return msg::error(id, rpc::INVALID_PARAMS, "prompts/get requires string \"name\"");
        const std::string& name = params.get("name").as_string();
        auto it = std::find_if(prompts_.begin(), prompts_.end(),
                               [&](const Prompt& p) { return p.name == name; });
        if (it == prompts_.end())
            return msg::error(id, rpc::INVALID_PARAMS, "unknown prompt: " + name);

        Json empty_args = Json::object();
        const Json* args = params.find("arguments");
        if (args && !args->is_object())
            return msg::error(id, rpc::INVALID_PARAMS, "\"arguments\" must be an object");
        if (!args) args = &empty_args;

        try {
            return msg::response(id, it->handler(*args).to_json());
        } catch (const std::exception& e) {
            return msg::error(id, rpc::INTERNAL_ERROR, e.what());
        }
    }

    std::string name_;
    std::string version_;
    ServerOptions options_;
    std::vector<Tool> tools_;
    std::vector<Resource> resources_;
    std::vector<Prompt> prompts_;
    bool ready_ = false;
    Transport* active_sink_ = nullptr;          // current message's transport
    std::unordered_set<std::string> cancelled_ids_;
};

}  // namespace mcpp
