// examples/llama_agent.cpp — a complete tool-calling agent in ~130 lines,
// built only from mcpp pieces: spawn any MCP server, expose its tools to an
// OpenAI-compatible LLM (llama-server, vLLM, Ollama, cloud), run the loop.
//
//   ./llama_agent --llm http://127.0.0.1:8080/v1 --model qwen2.5:7b \
//                 --mcp ./code-intel-mcp --prompt "Where is McpServer defined?"
//
// Every LLM turn and tool call is printed as it happens.
#include "mcpp/agent.hpp"
#include "mcpp/process_transport.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    using namespace mcpp;

    std::string llm_url = "http://127.0.0.1:8080/v1";
    std::string model = "local";
    std::string api_key = std::getenv("OPENAI_API_KEY") ? std::getenv("OPENAI_API_KEY") : "";
    std::string prompt;
    int max_turns = 8;
    std::vector<std::string> mcp_command;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--llm") llm_url = next();
        else if (a == "--model") model = next();
        else if (a == "--api-key") api_key = next();
        else if (a == "--prompt") prompt = next();
        else if (a == "--max-turns") max_turns = std::atoi(next().c_str());
        else if (a == "--mcp") { while (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) mcp_command.push_back(argv[++i]); }
        else if (a == "--help" || a == "-h") { prompt.clear(); break; }
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); return 2; }
    }
    if (mcp_command.empty() || prompt.empty()) {
        std::fprintf(stderr,
                     "usage: llama_agent --mcp <server command> --prompt <text>\n"
                     "                    [--llm URL] [--model NAME] [--api-key KEY]\n"
                     "                    [--max-turns N]\n"
                     "\n"
                     "example:\n"
                     "  llama_agent --mcp ./code-intel-mcp \\\n"
                     "              --llm http://127.0.0.1:8080/v1 --model qwen2.5 \\\n"
                     "              --prompt \"Where is McpServer defined?\"\n");
        return 2;
    }

    try {
        OpenAiChatBackend llm(llm_url, model, api_key);
        ProcessTransport tools(mcp_command);
        McpClient mcp(tools);
        auto info = mcp.initialize("llama-agent", "0.1.0");
        std::fprintf(stderr, "* tools: %s v%s\n", info.name.c_str(), info.version.c_str());

        Agent agent(llm, mcp, "You are a terse assistant. Use the provided tools "
                              "when they help; answer from their results.");
        agent.on_step([](const Json& msg) {
            if (const Json* calls = msg.find("tool_calls")) {
                for (const Json& c : calls->as_array())
                    std::fprintf(stderr, "* tool  %s(%s)\n",
                                 c["function"].get("name").as_string().c_str(),
                                 c["function"].get_or<std::string>("arguments", "").c_str());
            }
            if (const Json* text = msg.find("content"))
                if (text->is_string() && !text->as_string().empty())
                    std::fprintf(stderr, "* think %s\n", text->as_string().c_str());
            return true;
        });

        AgentResult result = agent.run(prompt, max_turns);
        std::printf("%s\n", result.final_text.c_str());
        std::fprintf(stderr, "* turns: %d%s\n", result.turns,
                     result.hit_turn_limit ? " (hit limit)" : "");
        return result.hit_turn_limit ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
