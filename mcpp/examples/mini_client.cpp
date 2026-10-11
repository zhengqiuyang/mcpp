// examples/mini_client.cpp — spawn any MCP stdio server and talk to it.
//
// Demonstrates the client half of mcpp against an arbitrary server binary:
//   ./mini_client ./echo_server
//   ./mini_client npx -- -y @modelcontextprotocol/server-everything --call echo --args '{"message":"hi"}'
//
// ProcessTransport (child process + two pipes) is implemented here; once it
// hardens on real platforms it will graduate into mcpp/process_transport.hpp.
#include "mcpp/client.hpp"
#include "mcpp/server.hpp"  // ToolResult

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace mcpp;

class ProcessTransport : public Transport {
public:
    explicit ProcessTransport(const std::vector<std::string>& argv) {
#ifdef _WIN32
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE in_read = nullptr, in_write = nullptr, out_read = nullptr, out_write = nullptr;
        if (!CreatePipe(&in_read, &in_write, &inherit, 0) ||
            !CreatePipe(&out_read, &out_write, &inherit, 0))
            die("CreatePipe failed");

        // Parent ends must not leak into the child.
        SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

        std::string cmdline;
        for (const auto& a : argv) {
            if (!cmdline.empty()) cmdline.push_back(' ');
            bool quote = a.find(' ') != std::string::npos;
            if (quote) cmdline.push_back('"');
            cmdline += a;
            if (quote) cmdline.push_back('"');
        }
        std::vector<char> mutable_cmd(cmdline.begin(), cmdline.end());
        mutable_cmd.push_back('\0');

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in_read;
        si.hStdOutput = out_write;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, 0, nullptr,
                            nullptr, &si, &pi))
            die("CreateProcess failed (command not found?)");
        proc_ = pi.hProcess;
        CloseHandle(pi.hThread);
        CloseHandle(in_read);   // child's ends
        CloseHandle(out_write);
        write_ = in_write;
        read_ = out_read;
#else
        int in_pipe[2], out_pipe[2];
        if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) die("pipe failed");
        pid_ = fork();
        if (pid_ == 0) {
            dup2(in_pipe[0], STDIN_FILENO);
            dup2(out_pipe[1], STDOUT_FILENO);
            close(in_pipe[0]); close(in_pipe[1]);
            close(out_pipe[0]); close(out_pipe[1]);
            std::vector<char*> cargv;
            for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
            cargv.push_back(nullptr);
            execvp(cargv[0], cargv.data());
            _exit(127);
        }
        close(in_pipe[0]);
        close(out_pipe[1]);
        read_file_ = fdopen(out_pipe[0], "r");
        write_file_ = fdopen(in_pipe[1], "w");
        if (!read_file_ || !write_file_) die("fdopen failed");
#endif
    }

    ~ProcessTransport() override {
#ifdef _WIN32
        if (write_) CloseHandle(write_);
        if (read_) CloseHandle(read_);
        if (proc_) {
            WaitForSingleObject(proc_, 2000);
            TerminateProcess(proc_, 0);
            CloseHandle(proc_);
        }
#else
        if (write_file_) fclose(write_file_);
        if (read_file_) fclose(read_file_);
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            waitpid(pid_, nullptr, 0);
        }
#endif
    }

    bool read_message(std::string& out) override {
        out.clear();
#ifdef _WIN32
        char buf[4096];
        DWORD n = 0;
        for (;;) {
            if (!ReadFile(read_, buf, sizeof buf, &n, nullptr) || n == 0)
                return !out.empty();
            out.append(buf, n);
            if (out.find('\n') != std::string::npos) {
                out.erase(out.find('\n'));
                break;
            }
        }
#else
        char buf[65536];
        for (;;) {
            if (!std::fgets(buf, sizeof buf, read_file_)) return !out.empty();
            out += buf;
            if (std::strchr(buf, '\n')) {
                out.erase(out.find('\n'));
                break;
            }
        }
#endif
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
    }

    void write_message(const std::string& msg) override {
#ifdef _WIN32
        DWORD n = 0;
        WriteFile(write_, msg.data(), static_cast<DWORD>(msg.size()), &n, nullptr);
        WriteFile(write_, "\n", 1, &n, nullptr);
        FlushFileBuffers(write_);
#else
        std::fwrite(msg.data(), 1, msg.size(), write_file_);
        std::fputc('\n', write_file_);
        std::fflush(write_file_);
#endif
    }

private:
    [[noreturn]] static void die(const char* what) {
        std::fprintf(stderr, "ProcessTransport: %s\n", what);
        std::exit(2);
    }

#ifdef _WIN32
    HANDLE read_ = nullptr;
    HANDLE write_ = nullptr;
    HANDLE proc_ = nullptr;
#else
    FILE* read_file_ = nullptr;
    FILE* write_file_ = nullptr;
    pid_t pid_ = -1;
#endif
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: mini_client <server-command> [args...] [--call <tool> --args '<json>']\n");
        return 2;
    }
    std::vector<std::string> command;
    std::string call_tool, call_args = "{}";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--call" && i + 1 < argc) call_tool = argv[++i];
        else if (a == "--args" && i + 1 < argc) call_args = argv[++i];
        else command.push_back(std::move(a));
    }

    ProcessTransport transport(command);
    McpClient client(transport);

    auto info = client.initialize("mcpp-mini-client", "0.1.0");
    std::printf("connected: %s v%s (protocol %s)\n", info.name.c_str(),
                info.version.c_str(), client.negotiated_protocol_version().c_str());

    auto tools = client.list_tools();
    std::printf("%zu tools:\n", tools.size());
    for (const auto& t : tools) std::printf("  - %-16s %s\n", t.name.c_str(), t.description.c_str());

    if (!call_tool.empty()) {
        try {
            Json args = Json::parse(call_args);
            ToolResult r = client.call_tool(call_tool, std::move(args));
            std::printf("\n%s %s:\n%s\n", call_tool.c_str(),
                        r.is_error ? "(error)" : "(ok)", r.text_content().c_str());
        } catch (const McpRemoteError& e) {
            std::printf("\n%s failed: [%d] %s\n", call_tool.c_str(), e.code, e.what());
        }
    }
    return 0;
}
