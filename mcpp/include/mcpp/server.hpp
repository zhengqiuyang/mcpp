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
#include <utility>
#include <vector>

#include "json.hpp"
#include "protocol.hpp"
#include "transport.hpp"

namespace mcpp {

struct ServerOptions {
    std::string instructions;  // optional instructions surfaced at initialize
};

class McpServer {
public:
    using ToolHandler = std::function<ToolResult(const Json&)>;
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

    // Serve requests on the transport until EOF.
    void run(Transport& transport) {
        std::string line;
        while (transport.read_message(line)) {
            auto reply = handle_line(line);
            if (reply) transport.write_message(*reply);
        }
    }

    // Handle one raw (serialized) message; returns the serialized reply, or
    // nullopt when there is nothing to send (notifications, parse-and-ignore).
    std::optional<std::string> handle_line(std::string_view line) {
        Json message;
        try {
            message = Json::parse(line);
        } catch (const Json::Error& e) {
            return msg::error(Json(nullptr), rpc::PARSE_ERROR, e.what()).dump();
        }
        auto reply = handle_message(message);
        if (!reply) return std::nullopt;
        return reply->dump();
    }

    // Handle one parsed message. Never throws for input problems — malformed
    // requests produce error responses. Handler exceptions are caught.
    std::optional<Json> handle_message(const Json& message) {
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

        // JSON-RPC: never reply to notifications, whatever the method says.
        if (!has_id) return std::nullopt;

        if (!ready_ && method != "ping") {
            if (!has_id) return std::nullopt;
            return msg::error(id, mcp_error::SERVER_NOT_INITIALIZED,
                              "server not initialized; send \"initialize\" first");
        }

        // ---- utilities -------------------------------------------------
        if (method == "ping") return msg::response(id, Json::object());
        if (method == "notifications/cancelled") return std::nullopt;  // sync handlers can't cancel

        // ---- tools -----------------------------------------------------
        if (method == "tools/list") return msg::response(id, build_tools_list());
        if (method == "tools/call") return handle_tools_call(id, params);

        // ---- resources -------------------------------------------------
        if (method == "resources/list") return msg::response(id, build_resources_list());
        if (method == "resources/read") return handle_resources_read(id, params);

        // ---- prompts ---------------------------------------------------
        if (method == "prompts/list") return msg::response(id, build_prompts_list());
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
        ToolHandler handler;
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

    Json build_tools_list() const {
        Json result = Json::object();
        result["tools"] = Json::array();
        for (const auto& t : tools_) {
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

        try {
            return msg::response(id, it->handler(*args).to_json());
        } catch (const RpcError& e) {
            return msg::error(id, e.code, e.what(), e.data);
        } catch (const std::exception& e) {
            return msg::error(id, rpc::INTERNAL_ERROR, e.what());
        } catch (...) {
            return msg::error(id, rpc::INTERNAL_ERROR, "unknown handler exception");
        }
    }

    Json build_resources_list() const {
        Json result = Json::object();
        result["resources"] = Json::array();
        for (const auto& r : resources_) {
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

    Json build_prompts_list() const {
        Json result = Json::object();
        result["prompts"] = Json::array();
        for (const auto& p : prompts_) {
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
};

}  // namespace mcpp
