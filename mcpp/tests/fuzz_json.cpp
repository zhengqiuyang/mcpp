// fuzz_json.cpp — randomized robustness testing for mcpp/json.hpp.
// Deterministic PRNG, no external fuzzing framework: parse must never crash,
// and any accepted document must round-trip (parse(dump(x)) == x).
//
// Run: ./fuzz_json [iterations]     (default 100000, ~1s)
#include "mcpp/json.hpp"
#include "test_util.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using mcpp::Json;

namespace {

struct XorShift {
    uint64_t s;
    explicit XorShift(uint64_t seed) : s(seed ? seed : 0x9e3779b97f4a7c15ull) {}
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
};

const char* kCorpus[] = {
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"add\","
    "\"arguments\":{\"a\":19,\"b\":23},\"_meta\":{\"progressToken\":7}}}",
    "{\"a\":[1,2,{\"b\":[true,false,null]}],\"c\":\"\\u00e9\\ud83d\\ude00\",\"d\":-2.5e-3}",
    "[\"nested\",[[[\"deep\",[]]]],{\"k\":\"\\\"\\\\\\b\\f\\n\\r\\t\"}]",
    "{\"big\":9007199254740993,\"neg\":-9223372036854775808,\"zero\":0}",
    "{}", "[]", "\"\"", "null", "true", "42", "-0.0", "1e308",
};

}  // namespace

int main(int argc, char** argv) {
    const int iters = argc > 1 ? std::atoi(argv[1]) : 100000;
    XorShift rng(0xC0FFEE);

    long parsed_ok = 0, roundtrips = 0, rejected = 0;

    for (int i = 0; i < iters; ++i) {
        std::string input;
        const int mode = rng.below(3);

        if (mode == 0) {
            // fully random bytes
            const size_t len = rng.below(64);
            input.resize(len);
            for (char& c : input) c = static_cast<char>(rng.next() & 0xff);
        } else if (mode == 1) {
            // mutate a corpus document
            input = kCorpus[rng.below(sizeof(kCorpus) / sizeof(kCorpus[0]))];
            const int ops = 1 + rng.below(8);
            for (int op = 0; op < ops && !input.empty(); ++op) {
                const int what = rng.below(4);
                const size_t at = rng.below(static_cast<uint32_t>(input.size()));
                if (what == 0) input[at] = static_cast<char>(rng.next() & 0xff);
                else if (what == 1) input.insert(at, 1, static_cast<char>(rng.next() & 0xff));
                else if (what == 2) input.erase(at, 1);
                else input.insert(at, kCorpus[rng.below(3)]);  // splice garbage in
            }
        } else {
            // structured deep nesting around the depth limit
            const size_t depth = 1 + rng.below(220);
            input.assign(depth, '[');
            input.append(depth, ']');
        }

        try {
            Json j = Json::parse(input);
            ++parsed_ok;
            // invariant: accepted documents round-trip exactly
            Json reparsed = Json::parse(j.dump());
            if (reparsed == j) ++roundtrips;
            else {
                ++mcpptest::g_failures;
                std::fprintf(stderr, "FUZZ roundtrip mismatch on: %.80s\n", input.c_str());
            }
        } catch (const Json::Error&) {
            ++rejected;
        }  // any other exception escapes and fails the test
    }

    ++mcpptest::g_checks;
    std::printf("fuzz_json: %d iters (accepted=%ld rejected=%ld roundtrips-ok=%ld)\n",
                iters, parsed_ok, rejected, roundtrips);
    return mcpptest::g_failures == 0 ? 0 : 1;
}
