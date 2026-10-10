#!/usr/bin/env node
// MCP adapter for the GD AI Bridge mod: exposes the mod's local HTTP API as MCP tools.
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";

const BASE = process.env.GD_BRIDGE_URL ?? "http://127.0.0.1:7312";

async function call(method, path, body) {
  let res;
  try {
    res = await fetch(BASE + path, {
      method,
      headers: body ? { "Content-Type": "application/json" } : {},
      body: body ? JSON.stringify(body) : undefined,
    });
  } catch {
    throw new Error(`Cannot reach Geometry Dash at ${BASE}. Is the game running with the GD AI Bridge mod?`);
  }
  const data = await res.json();
  if (!res.ok) throw new Error(data.error ?? `HTTP ${res.status}`);
  return data;
}

function query(params) {
  const entries = Object.entries(params).filter(([, v]) => v !== undefined);
  return entries.length ? "?" + new URLSearchParams(entries.map(([k, v]) => [k, String(v)])) : "";
}

const result = (data) => ({ content: [{ type: "text", text: JSON.stringify(data) }] });

const server = new McpServer({ name: "gd-ai-bridge", version: "0.1.0" });

function tool(name, description, inputSchema, run) {
  server.registerTool(name, { description, inputSchema }, async (args) => {
    try {
      return result(await run(args));
    } catch (e) {
      return { content: [{ type: "text", text: e.message }], isError: true };
    }
  });
}

const objectSchema = z.object({
  id: z.number().int().describe("GD object ID, e.g. 1 = default block, 8 = spike. See gd_reference for more"),
  x: z.number().describe("X position. 1 block = 30 units; grid cell centers are 15, 45, 75..."),
  y: z.number().describe("Y position, same units as x; 15 = first row above the ground"),
  rotation: z.number().optional().describe("Degrees, clockwise"),
  scale: z.number().optional(),
  scale_x: z.number().optional(),
  scale_y: z.number().optional(),
  flip_x: z.boolean().optional(),
  flip_y: z.boolean().optional(),
  groups: z.array(z.number().int()).optional().describe("Group IDs (for triggers)"),
  props: z
    .record(z.string(), z.union([z.string(), z.number()]))
    .optional()
    .describe("Any other raw GD object property by key, e.g. {\"21\": 3} = main color channel 3"),
});

tool(
  "gd_status",
  "Check whether the Geometry Dash level editor is open; returns level name, object count and playtest state.",
  {},
  () => call("GET", "/status"),
);

tool(
  "gd_reference",
  "Coordinate system and common object IDs (blocks, spikes, pads, orbs, portals, triggers). Call once before building.",
  {},
  async () => {
    const { units, common_object_ids } = await call("GET", "/");
    return { units, common_object_ids };
  },
);

tool(
  "gd_get_objects",
  "List objects in the open level, optionally only within an x range.",
  {
    x_min: z.number().optional(),
    x_max: z.number().optional(),
    offset: z.number().int().optional(),
    limit: z.number().int().optional().describe("Default 1000"),
    raw: z.boolean().optional().describe("Also return each object's raw GD save string"),
  },
  (a) => call("GET", "/objects" + query({ ...a, raw: a.raw ? 1 : undefined })),
);

tool(
  "gd_get_selection",
  "List the objects the user currently has selected in the editor.",
  { raw: z.boolean().optional() },
  (a) => call("GET", "/selection" + query({ raw: a.raw ? 1 : undefined })),
);

tool(
  "gd_get_level_string",
  "Get the full raw level string (all objects and level settings) of the open level.",
  {},
  () => call("GET", "/level-string"),
);

tool(
  "gd_add_objects",
  "Add objects to the open level. All objects are added as a single undo step. Returns the new objects' uids.",
  {
    objects: z.array(objectSchema).optional(),
    raw: z.string().optional().describe("Alternative: raw GD object string, objects separated by ';'"),
  },
  (a) => call("POST", "/objects", a),
);

tool(
  "gd_delete_objects",
  "Delete objects by uid (from gd_get_objects). Single undo step.",
  { uids: z.array(z.number().int()) },
  (a) => call("POST", "/objects/delete", a),
);

tool(
  "gd_select",
  "Select objects in the editor by uid so the user can see them. Empty list deselects all.",
  { uids: z.array(z.number().int()) },
  (a) => call("POST", "/select", a),
);

tool("gd_undo", "Undo the last editor action.", {}, () => call("POST", "/undo"));

tool(
  "gd_playtest",
  "Start or stop the editor playtest.",
  { action: z.enum(["start", "stop"]) },
  (a) => call("POST", "/playtest", a),
);

await server.connect(new StdioServerTransport());
