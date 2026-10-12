// mcpp/agent.hpp — a minimal, self-hosted agent loop on top of mcpp.
//
// The seed of the planned edge-agent runtime: a tool-calling agent built
// entirely from mcpp's own pieces — an OpenAI-compatible chat backend that
// rides HttpClientTransport, and a tool registry that is just an MCP client.
// No framework, no dependencies: ~150 lines you can read in one sitting.
//
//   mcpp::OpenAiChatBackend llm("http://127.0.0.1:8080/v1", "qwen3");
//   mcpp::ProcessTransport tools({"./code-intel-mcp"});
//   mcpp::McpClient mcp(tools);
//   mcpp::Agent agent(llm, mcp, "You are a terse coding assistant.");
//   auto result = agent.run("Find the definition of McpServer", 8);

#pragma once

#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "client.hpp"
#include "http_client_transport.hpp"
#include "json.hpp"

namespace mcpp {

// ---------------------------------------------------------------- backend

// OpenAI-compatible chat completions client (llama-server, vLLM, Ollama's
// OpenAI endpoint, or the cloud). One HTTP request per call — the transport
// is reused with keep-alive-free semantics, which suits interactive loops.
class OpenAiChatBackend {
public:
    OpenAiChatBackend(std::string base_url, std::string model,
                      std::string api_key = "")
        : model_(std::move(model)) {
        transport_ = std::make_unique<HttpClientTransport>(std::move(base_url));
        if (!api_key.empty())
            transport_->set_header("Authorization", "Bearer " + api_key);
    }

    // messages: OpenAI chat format; tools: OpenAI function-tool format.
    // Returns the full response object (choices[0].message holds the turn).
    Json chat(const Json& messages, const Json& tools = nullptr) const {
        Json body = Json::object();
        body["model"] = model_;
        body["messages"] = messages;
        if (tools.is_array() && tools.size() > 0) {
            body["tools"] = tools;
            body["tool_choice"] = "auto";
        }
        transport_->write_message(body.dump());
        std::string raw;
        if (!transport_->read_message(raw))
            throw std::runtime_error("mcpp::Agent: LLM transport closed");
        Json resp = Json::parse(raw);
        if (const Json* err = resp.find("error"))
            throw std::runtime_error("mcpp::Agent: LLM error: " + err->dump());
        return resp;
    }

private:
    std::string model_;
    std::unique_ptr<HttpClientTransport> transport_;
};

// ------------------------------------------------------------------ agent

struct AgentResult {
    std::string final_text;   // last assistant message without tool calls
    int turns = 0;            // LLM round-trips used
    bool hit_turn_limit = false;
};

// Observes every step (for CLIs / transcripts); return false to abort.
using AgentTrace = std::function<bool(const Json& /*assistant_message*/)>;

class Agent {
public:
    Agent(const OpenAiChatBackend& llm, McpClient& mcp, std::string system = "")
        : llm_(llm), mcp_(mcp), system_(std::move(system)) {}

    Agent& on_step(AgentTrace trace) {
        trace_ = std::move(trace);
        return *this;
    }

    AgentResult run(const std::string& user_message, int max_turns = 8) {
        Json messages = Json::array();
        if (!system_.empty()) {
            Json m = Json::object();
            m["role"] = "system";
            m["content"] = system_;
            messages.push_back(std::move(m));
        }
        {
            Json m = Json::object();
            m["role"] = "user";
            m["content"] = user_message;
            messages.push_back(std::move(m));
        }

        // advertise the MCP server's tools in OpenAI function format
        Json tools = Json::array();
        for (const auto& t : mcp_.list_tools()) {
            Json tool = Json::object();
            tool["type"] = "function";
            tool["function"] = Json::object();
            tool["function"]["name"] = t.name;
            tool["function"]["description"] = t.description;
            tool["function"]["parameters"] = t.input_schema;
            tools.push_back(std::move(tool));
        }

        AgentResult out;
        for (int turn = 1; turn <= max_turns; ++turn) {
            out.turns = turn;
            Json resp = llm_.chat(messages, tools);
            const Json* choices = resp.find("choices");
            if (!choices || choices->as_array().empty())
                throw std::runtime_error("mcpp::Agent: response has no choices");
            Json assistant = choices->at(0).get("message");
            bool keep_going = trace_ ? trace_(assistant) : true;

            const Json* calls = assistant.find("tool_calls");
            bool has_calls = calls && calls->is_array() && calls->size() > 0;
            messages.push_back(assistant);
            if (!keep_going) break;

            if (has_calls) {
                for (const Json& call : calls->as_array()) {
                    std::string name = call["function"].get("name").as_string();
                    Json args = Json::object();
                    if (const Json* raw = call["function"].find("arguments")) {
                        if (raw->is_string() && !raw->as_string().empty())
                            args = Json::parse(raw->as_string());
                    }
                    ToolResult result = mcp_.call_tool(name, std::move(args));
                    Json m = Json::object();
                    m["role"] = "tool";
                    m["tool_call_id"] = call.get_or<std::string>("id", "");
                    m["content"] = result.text_content();
                    messages.push_back(std::move(m));
                }
                continue;  // feed results back to the model
            }

            out.final_text = assistant.get_or<std::string>("content", "");
            return out;
        }
        out.hit_turn_limit = true;
        return out;
    }

private:
    const OpenAiChatBackend& llm_;
    McpClient& mcp_;
    std::string system_;
    AgentTrace trace_;
};

}  // namespace mcpp
