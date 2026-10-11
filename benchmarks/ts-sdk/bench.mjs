// Apples-to-apples benchmark against mcpp/tests/bench.cpp:
// same workload (add tool, ~130B request), in-memory transport pair,
// sequential awaited round-trips, single event loop.
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { z } from "zod";

const kCallJson = JSON.stringify({
  jsonrpc: "2.0",
  id: 123,
  method: "tools/call",
  params: {
    name: "add",
    arguments: { a: 19, b: 23 },
    _meta: { progressToken: 7 },
  },
});

function bench(name, iters, fn) {
  const t0 = performance.now();
  const r = fn();
  const ms = () => performance.now() - t0;
  // support both sync and promise workloads
  if (r && typeof r.then === "function") {
    return r.then(() => {
      const ops = iters / (ms() / 1000);
      console.log(`${name.padEnd(44)} ${ops.toFixed(0).padStart(9)} ops/s  (${iters} iters in ${ms().toFixed(0)} ms)`);
    });
  }
  const ops = iters / (ms() / 1000);
  console.log(`${name.padEnd(44)} ${ops.toFixed(0).padStart(9)} ops/s  (${iters} iters in ${ms().toFixed(0)} ms)`);
}

const server = new McpServer({ name: "bench.server", version: "1.0" });
server.tool("add", { a: z.number(), b: z.number() }, async ({ a, b }) => ({
  content: [{ type: "text", text: String(a + b) }],
}));

const [serverSide, clientSide] = InMemoryTransport.createLinkedPair();
await server.connect(serverSide);

const client = new Client({ name: "bench", version: "1.0" });
await client.connect(clientSide);

// sanity
const first = await client.callTool({ name: "add", arguments: { a: 19, b: 23 } });
if (first.content[0].text !== "42") throw new Error("unexpected result: " + JSON.stringify(first));

const K_JSON = 200_000;
await bench(`JSON.parse+stringify (MCP tools/call)`, K_JSON, async () => {
  for (let i = 0; i < K_JSON; i++) {
    const j = JSON.parse(kCallJson);
    JSON.stringify(j);
  }
});

const K_ROUND = 50_000;
await bench("full round-trip (wire <-> dispatch)", K_ROUND, async () => {
  for (let i = 0; i < K_ROUND; i++) {
    await client.callTool({ name: "add", arguments: { a: 19, b: 23 } });
  }
});

console.log(`\nnode ${process.version}, single event loop, @modelcontextprotocol/sdk ${JSON.parse(await (await import("node:fs/promises")).readFile("./node_modules/@modelcontextprotocol/sdk/package.json", "utf8")).version}`);
