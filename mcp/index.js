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

// Same fields minus the position, for tools that place many copies of one object.
const templateFields = objectSchema.omit({ x: true, y: true }).shape;

// Which objects a tool acts on: explicit uids, or everything inside an area.
const targetFields = {
  uids: z.array(z.number().int()).optional(),
  region: z
    .object({
      x_min: z.number().optional(),
      x_max: z.number().optional(),
      y_min: z.number().optional(),
      y_max: z.number().optional(),
    })
    .optional()
    .describe("Alternative to uids: all objects inside this area"),
};

tool(
  "gd_status",
  "Check whether the Geometry Dash level editor is open; returns level name, object count and playtest state.",
  {},
  () => call("GET", "/status"),
);

tool(
  "gd_reference",
  "Coordinate system, common object IDs (blocks, spikes, pads, orbs, portals, triggers) and measured jump physics " +
    "(heights and lengths per speed, pads, orbs, ship). Call once before building.",
  {},
  async () => {
    const { units, common_object_ids, physics } = await call("GET", "/");
    return { units, common_object_ids, physics };
  },
);

tool(
  "gd_get_objects",
  "List objects in the open level, optionally only within an x range. Compact by default: " +
    "'uid,id,x,y[,rotation,scale_x,scale_y];...' (rotation/scale only when not default).",
  {
    x_min: z.number().optional(),
    x_max: z.number().optional(),
    offset: z.number().int().optional(),
    limit: z.number().int().optional().describe("Default 1000"),
    json: z.boolean().optional().describe("Full JSON per object instead of the compact format"),
    raw: z.boolean().optional().describe("JSON only: also return each object's raw GD save string"),
  },
  ({ json, raw, ...a }) =>
    call("GET", "/objects" + query({ ...a, raw: raw ? 1 : undefined, format: json || raw ? undefined : "compact" })),
);

tool(
  "gd_map",
  "ASCII map of a level area, one character per block (# block, ^ hazard, o orb, _ pad, P portal...). " +
    "The cheapest way to see the layout. Defaults to 60 x 12 blocks from x_min.",
  {
    x_min: z.number().optional(),
    x_max: z.number().optional(),
    y_min: z.number().optional(),
    y_max: z.number().optional(),
  },
  (a) => call("GET", "/map" + query(a)),
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
  "gd_fill",
  "Fill a rectangle with one object type (e.g. a wall or floor of blocks) in a single call. " +
    "Corners are object positions, e.g. x1=15,y1=15,x2=285,y2=75 fills 10x3 blocks. Single undo step.",
  {
    ...templateFields,
    x1: z.number(),
    y1: z.number(),
    x2: z.number(),
    y2: z.number(),
    step: z.number().optional().describe("Spacing, default 30 (one block)"),
    step_x: z.number().optional(),
    step_y: z.number().optional(),
  },
  (a) => call("POST", "/objects/fill", a),
);

tool(
  "gd_line",
  "Place one object type along a line, e.g. a row of spikes or a diagonal of orbs. Single undo step.",
  {
    ...templateFields,
    x1: z.number(),
    y1: z.number(),
    x2: z.number(),
    y2: z.number().optional().describe("Defaults to y1 (horizontal line)"),
    step: z.number().optional().describe("Spacing, default 30"),
    count: z.number().int().optional().describe("Alternative to step: exactly this many objects, evenly spaced"),
  },
  (a) => call("POST", "/objects/line", a),
);

tool(
  "gd_copy",
  "Duplicate objects (by uids or region) with an offset; 'times' repeats it, each copy shifted by dx,dy more. " +
    "Great for repeating a pattern. Single undo step.",
  { ...targetFields, dx: z.number(), dy: z.number().optional(), times: z.number().int().optional() },
  (a) => call("POST", "/objects/copy", a),
);

tool(
  "gd_move",
  "Move objects (by uids or region) by dx,dy. Keeps their uids. Single undo step.",
  { ...targetFields, dx: z.number(), dy: z.number().optional() },
  (a) => call("POST", "/objects/move", a),
);

tool(
  "gd_update",
  "Change properties of existing objects (by uids or region): id, position, rotation, scale, flip, groups, props. " +
    "The objects are replaced, so they get new uids (returned); undoing takes 2 steps.",
  { ...targetFields, set: objectSchema.partial() },
  (a) => call("POST", "/objects/update", a),
);

tool(
  "gd_delete_objects",
  "Delete objects by uids or inside a region. Single undo step.",
  targetFields,
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
  "gd_test",
  "Play the level automatically with a planned list of inputs and get the result: passed/died, where the player " +
    "died, which objects were next to it, progress % and the player's path ('x,y;...'). The game plays the inputs " +
    "itself at exact positions. Use it to check that a section is beatable, then fix the level or the inputs and retry.",
  {
    inputs: z
      .array(
        z.object({
          x: z.number().describe("Player x where the button is pressed"),
          release: z.number().optional().describe("Player x where it is released; omit for a short tap (one jump)"),
        }),
      )
      .optional()
      .describe("Button presses in order. Holding works for ship/wave/ufo and repeated cube jumps"),
    until_x: z.number().optional().describe("Counts as passed at this x. Default: just past the last object"),
    speed: z.number().optional().describe("Game speed while testing, 0.25-4 (default 1)"),
    timeout: z.number().optional().describe("Seconds before giving up (default 120)"),
    path_step: z.number().optional().describe("Record the path every N units of x (5-300, default 30)"),
  },
  (a) => call("POST", "/test", a),
);

tool(
  "gd_playtest",
  "Start or stop the editor playtest.",
  { action: z.enum(["start", "stop"]) },
  (a) => call("POST", "/playtest", a),
);

await server.connect(new StdioServerTransport());
