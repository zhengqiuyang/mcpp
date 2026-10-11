// mcpp/protocol.hpp — MCP wire-level types and JSON-RPC 2.0 message helpers.
//
// Implements the stable core of the Model Context Protocol: the initialize
// handshake, tools/resources/prompts primitives and their content blocks.
// Version note: we negotiate by echoing the client's requested version when
// we know it, otherwise replying with our latest supported revision.

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "json.hpp"

namespace mcpp {

// ------------------------------------------------------------ error codes

namespace rpc {
constexpr int PARSE_ERROR = -32700;
constexpr int INVALID_REQUEST = -32600;
constexpr int METHOD_NOT_FOUND = -32601;
constexpr int INVALID_PARAMS = -32602;
constexpr int INTERNAL_ERROR = -32603;
}  // namespace rpc

namespace mcp_error {
constexpr int REQUEST_CANCELLED = -32800;
constexpr int REQUEST_TIMED_OUT = -32801;
constexpr int SERVER_NOT_INITIALIZED = -32002;
}  // namespace mcp_error

// Protocol revisions we can speak. Update kLatest when a new spec lands.
inline const char* kLatestProtocolVersion = "2025-06-18";
inline bool known_protocol_version(const std::string& v) {
    return v == "2024-11-05" || v == "2025-03-26" || v == "2025-06-18";
}

// ---------------------------------------------------------- content blocks

struct Content {
    enum class Kind { Text, Image, Resource };
    Kind kind = Kind::Text;

    std::string text_;          // Text
    std::string data_;          // Image: base64 payload
    std::string mime_;          // Image / Resource mime type
    std::string resource_uri_;  // Resource: uri
    std::string blob_text_;     // Resource with embedded text (ResourceContents form)

    static Content text(std::string t) {
        Content c;
        c.kind = Kind::Text;
        c.text_ = std::move(t);
        return c;
    }
    static Content image(std::string base64, std::string mime_type = "image/png") {
        Content c;
        c.kind = Kind::Image;
        c.data_ = std::move(base64);
        c.mime_ = std::move(mime_type);
        return c;
    }
    // An embedded resource exposed as {uri, mimeType?, text}.
    static Content resource(std::string uri, std::string text_body,
                            std::string mime_type = "text/plain") {
        Content c;
        c.kind = Kind::Resource;
        c.resource_uri_ = std::move(uri);
        c.blob_text_ = std::move(text_body);
        c.mime_ = std::move(mime_type);
        return c;
    }

    Json to_json() const {
        Json j = Json::object();
        switch (kind) {
            case Kind::Text:
                j["type"] = "text";
                j["text"] = text_;
                break;
            case Kind::Image:
                j["type"] = "image";
                j["data"] = data_;
                j["mimeType"] = mime_;
                break;
            case Kind::Resource:
                j["type"] = "resource";
                j["resource"] = Json::object();
                j["resource"]["uri"] = resource_uri_;
                j["resource"]["mimeType"] = mime_;
                j["resource"]["text"] = blob_text_;
                break;
        }
        return j;
    }

    static Content from_json(const Json& j) {
        const std::string& type = j.get("type").as_string();
        if (type == "text") return Content::text(j.get("text").as_string());
        if (type == "image")
            return Content::image(j.get("data").as_string(), j.get_or<std::string>("mimeType", "image/png"));
        if (type == "resource") {
            const Json& r = j.get("resource");
            return Content::resource(r.get("uri").as_string(),
                                     r.get_or<std::string>("text", ""),
                                     r.get_or<std::string>("mimeType", "text/plain"));
        }
        throw std::invalid_argument("mcpp: unknown content type \"" + type + "\"");
    }
};

// ------------------------------------------------------- tool call results

struct ToolResult {
    std::vector<Content> content;
    bool is_error = false;

    static ToolResult text(std::string t) {
        ToolResult r;
        r.content.push_back(Content::text(std::move(t)));
        return r;
    }
    static ToolResult error(std::string t) {
        ToolResult r;
        r.is_error = true;
        r.content.push_back(Content::text(std::move(t)));
        return r;
    }
    ToolResult& add(Content c) {
        content.push_back(std::move(c));
        return *this;
    }

    // Concatenate text blocks — convenience for callers of McpClient::call_tool.
    std::string text_content() const {
        std::string out;
        for (const auto& c : content)
            if (c.kind == Content::Kind::Text) {
                if (!out.empty()) out.push_back('\n');
                out += c.text_;
            }
        return out;
    }

    Json to_json() const {
        Json j = Json::object();
        j["content"] = Json::array();
        for (const auto& c : content) j["content"].push_back(c.to_json());
        j["isError"] = is_error;
        return j;
    }
};

// ---------------------------------------------------------- prompt results

struct PromptMessage {
    std::string role;  // "user" | "assistant"
    Content content;
};

struct PromptResult {
    std::vector<PromptMessage> messages;
    std::string description;  // optional

    static PromptResult user(std::string text) {
        PromptResult r;
        r.messages.push_back(PromptMessage{"user", Content::text(std::move(text))});
        return r;
    }

    Json to_json() const {
        Json j = Json::object();
        if (!description.empty()) j["description"] = description;
        j["messages"] = Json::array();
        for (const auto& m : messages) {
            Json jm = Json::object();
            jm["role"] = m.role;
            jm["content"] = m.content.to_json();
            j["messages"].push_back(std::move(jm));
        }
        return j;
    }
};

// ------------------------------------------------------------ message glue

// A thrown RpcError is converted into a JSON-RPC error response by the server.
struct RpcError : std::runtime_error {
    int code;
    Json data;
    RpcError(int c, const std::string& msg, Json d = nullptr)
        : std::runtime_error(msg), code(c), data(std::move(d)) {}
};

namespace msg {

inline Json request(const Json& id, const std::string& method, Json params = nullptr) {
    Json j = Json::object();
    j["jsonrpc"] = "2.0";
    j["id"] = id;
    j["method"] = method;
    if (!params.is_null()) j["params"] = std::move(params);
    return j;
}

inline Json notification(const std::string& method, Json params = nullptr) {
    Json j = Json::object();
    j["jsonrpc"] = "2.0";
    j["method"] = method;
    if (!params.is_null()) j["params"] = std::move(params);
    return j;
}

inline Json response(const Json& id, Json result) {
    Json j = Json::object();
    j["jsonrpc"] = "2.0";
    j["id"] = id;
    j["result"] = std::move(result);
    return j;
}

inline Json error(const Json& id, int code, const std::string& message, Json data = nullptr) {
    Json j = Json::object();
    j["jsonrpc"] = "2.0";
    j["id"] = id;
    Json e = Json::object();
    e["code"] = code;
    e["message"] = message;
    if (!data.is_null()) e["data"] = std::move(data);
    j["error"] = std::move(e);
    return j;
}

// A message is a request/notification when it carries "method"; a response
// carries "result" or "error" instead.
inline bool has_method(const Json& m) { return m.is_object() && m.contains("method"); }
inline bool is_response(const Json& m) {
    return m.is_object() && (m.contains("result") || m.contains("error"));
}
inline bool is_notification(const Json& m) { return has_method(m) && !m.contains("id"); }

}  // namespace msg

// ------------------------------------------------- tool input schema sugar

struct ToolParam {
    std::string name;
    std::string type = "string";  // JSON Schema type
    std::string description;
    bool required = true;
};

// Build {"type":"object", "properties": {...}, "required": [...]}.
inline Json build_input_schema(const std::vector<ToolParam>& params) {
    Json schema = Json::object();
    schema["type"] = "object";
    schema["properties"] = Json::object();
    schema["required"] = Json::array();
    for (const auto& p : params) {
        Json prop = Json::object();
        prop["type"] = p.type;
        if (!p.description.empty()) prop["description"] = p.description;
        schema["properties"][p.name] = std::move(prop);
        if (p.required) schema["required"].push_back(p.name);
    }
    return schema;
}

}  // namespace mcpp
