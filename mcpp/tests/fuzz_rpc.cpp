// fuzz_rpc.cpp — randomized robustness testing for McpServer dispatch.
// Feeds random values, mutated JSON-RPC requests and hostile edge cases
// through handle_message/handle_line: never crash, and any produced reply
// must itself be valid serialized JSON.
//
// Run: ./fuzz_rpc [iterations]     (default 50000, ~1s)
#include "mcpp/server.hpp"
#include "test_util.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace mcpp;

namespace {

struct XorShift {
    uint64_t s;
    explicit XorShift(uint64_t seed) : s(seed ? seed : 0x5DEECE66Dull) {}
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
};

Json random_value(XorShift& rng, int depth) {
    switch (rng.below(depth > 4 ? 4 : 7)) {
        case 0: return Json::object();
        case 1: return Json::array();
        case 2: return Json(nullptr);
        case 3: return Json(rng.next() & 1 ? true : false);
        case 4: return Json(static_cast<int64_t>(rng.next()));
        case 5: return Json("method\"id\x01\xff\u00e9");
        default: {
            Json arr = Json::array();
            for (int i = 0, n = rng.below(4); i < n; ++i)
                arr.push_back(random_value(rng, depth + 1));
            return arr;
        }
    }
}

const char* kRequests[] = {
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"add\","
    "\"arguments\":{\"a\":1,\"b\":2}}}",
    "{\"jsonrpc\":\"2.0\",\"id\":\"str-id\",\"method\":\"tools/list\"}",
    "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":1}}",
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\"}",
    "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"initialize\",\"params\":"
    "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"clientInfo\":{\"name\":\"f\",\"version\":\"0\"}}}",
};

std::string mutate(XorShift& rng, std::string s) {
    const int ops = 1 + rng.below(6);
    for (int op = 0; op < ops && !s.empty(); ++op) {
        const size_t at = rng.below(static_cast<uint32_t>(s.size()));
        switch (rng.below(3)) {
            case 0: s[at] = static_cast<char>(rng.next() & 0xff); break;
            case 1: s.erase(at, 1); break;
            default: s.insert(at, 1, "\"{}[],:"[rng.below(7)]); break;
        }
    }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    const int iters = argc > 1 ? std::atoi(argv[1]) : 50000;
    XorShift rng(0xF00D);

    McpServer server("fuzz.server", "1.0");
    server.add_tool("add", "Add two integers",
                    {{"a", "integer", "left"}, {"b", "integer", "right"}},
                    [](const Json& a) {
                        return ToolResult::text(std::to_string(a.get("a").as_int() +
                                                               a.get("b").as_int()));
                    });
    server.add_tool("boom", "always throws", {}, [](const Json&) -> ToolResult {
        throw std::runtime_error("deliberate");
    });

    long replies = 0, silence = 0;
    for (int i = 0; i < iters; ++i) {
        std::string line;
        switch (rng.below(3)) {
            case 0: line = random_value(rng, 0).dump(); break;
            case 1:
                line = mutate(rng, kRequests[rng.below(sizeof(kRequests) / sizeof(kRequests[0]))]);
                break;
            default: line = mutate(rng, std::string(kRequests[0])); break;
        }

        // raw-line path: reply (when present) must be valid JSON
        if (auto reply = server.handle_line(line)) {
            ++replies;
            try {
                ++mcpptest::g_checks;
                Json::parse(*reply);  // throws on invalid → test failure
            } catch (const Json::Error& e) {
                ++mcpptest::g_failures;
                std::fprintf(stderr, "FUZZ non-serializable reply: %s (input %.60s)\n",
                             e.what(), line.c_str());
            }
        } else {
            ++silence;
        }
    }
    ++mcpptest::g_checks;

    std::printf("fuzz_rpc: %d iters (replies=%ld silent=%ld)\n", iters, replies, silence);
    return mcpptest::g_failures == 0 ? 0 : 1;
}
