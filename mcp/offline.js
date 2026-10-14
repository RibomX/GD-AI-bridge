// Offline mode: edits levels directly in CCLocalLevels.dat while Geometry Dash is closed.
// Implements the same routes as the mod's HTTP API, so tools work the same either way.
import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";
import { execFileSync } from "node:child_process";

const DEFAULT_SAVE = path.join(process.env.LOCALAPPDATA ?? "", "GeometryDash", "CCLocalLevels.dat");
// GD_SAVE_PATH points the adapter at another file (used for testing on a copy).
const SAVE_PATH = process.env.GD_SAVE_PATH ?? DEFAULT_SAVE;
const BACKUPS_KEPT = 5;
const MAX_BATCH = 10000;

// Copy of the mod's reference (src/Api.cpp, apiInfo) for when the game is closed. Keep both in sync.
export const REFERENCE = {
  units: "1 block = 30 units. Grid cell centers sit at 15, 45, 75... y = 15 is the first row above the ground.",
  common_object_ids: {
    blocks: "1 default block",
    hazards: "8 spike, 39 small spike",
    pads: "35 yellow, 140 pink, 67 blue (gravity)",
    orbs: "36 yellow, 141 pink, 84 blue (gravity)",
    gamemode_portals: "12 cube, 13 ship, 47 ball, 111 ufo, 660 wave, 745 robot, 1331 spider, 1933 swing",
    other_portals: "10 gravity normal, 11 gravity flip, 45 mirror on, 46 mirror off, 99 normal size, 101 mini",
    speed: "200 slow, 201 normal, 202 fast, 203 faster, 1334 fastest",
    triggers: "899 color, 901 move, 1006 pulse, 1007 alpha, 1049 toggle, 1268 spawn, 1346 rotate",
    misc: "31 start position",
  },
  physics: {
    cube_jump: "height ~64 at every speed; length: slow 106, normal 136, fast 176, faster 206, fastest 252",
    pads: "yellow pad: height 131, length 189; pink pad: height 55, length 124 (normal speed, triggered ~25 before its center)",
    orbs: "yellow orb: same arc as a normal jump (64 / 130); pink orb: height 33, length 91 (normal speed)",
    ship: "holding from the ground at normal speed: +2 after 15 units, +24 after 60, +90 after 120, +131 after 150; " +
      "after releasing it keeps rising ~70 more over ~120 units before falling",
    timing_tip: "to clear one spike at normal speed, tap ~60 units before its x; the player hitbox is about one block",
  },
};

export function isGameRunning() {
  try {
    const out = execFileSync("tasklist", ["/FI", "IMAGENAME eq GeometryDash.exe", "/NH"], { encoding: "utf8" });
    return out.toLowerCase().includes("geometrydash.exe");
  } catch {
    return false;
  }
}

// --- save file encoding: XOR 11 -> url-safe base64 -> gzip -> XML plist ---

function decodeSave(buf) {
  const text = Buffer.from(buf.map((b) => b ^ 11)).toString("latin1").replace(/\0/g, "");
  return zlib.gunzipSync(Buffer.from(text.replace(/-/g, "+").replace(/_/g, "/"), "base64")).toString("utf8");
}

function encodeSave(xml) {
  const b64 = zlib.gzipSync(Buffer.from(xml, "utf8")).toString("base64").replace(/\+/g, "-").replace(/\//g, "_");
  return Buffer.from(Buffer.from(b64, "latin1").map((b) => b ^ 11));
}

function decodeLevelString(s) {
  if (!s) return "";
  if (!s.startsWith("H4sI")) return s; // already plain
  return zlib.gunzipSync(Buffer.from(s.replace(/-/g, "+").replace(/_/g, "/"), "base64")).toString("utf8");
}

function encodeLevelString(s) {
  return zlib.gzipSync(Buffer.from(s, "utf8")).toString("base64").replace(/\+/g, "-").replace(/\//g, "_");
}

// --- minimal XML tree, serialized back exactly as GD writes it ---

function parseXml(xml) {
  const root = { tag: "#root", children: [] };
  const stack = [root];
  const re = /<(\/?)([^\s>\/]+)([^>]*?)(\/?)>|([^<]+)/g;
  let m;
  while ((m = re.exec(xml))) {
    const parent = stack[stack.length - 1];
    if (m[5] !== undefined) {
      parent.children.push({ text: m[5] });
    } else if (m[1]) {
      stack.pop();
    } else {
      const node = { tag: m[2], attrs: m[3], children: [], selfClosing: Boolean(m[4]) };
      parent.children.push(node);
      if (!node.selfClosing && !m[2].startsWith("?")) stack.push(node);
    }
  }
  return root;
}

function serializeXml(node) {
  if (node.text !== undefined) return node.text;
  const inner = node.children.map(serializeXml).join("");
  if (node.tag === "#root") return inner;
  if (node.tag.startsWith("?")) return `<${node.tag}${node.attrs}>`;
  if (node.selfClosing) return `<${node.tag}${node.attrs}/>`;
  return `<${node.tag}${node.attrs}>${inner}</${node.tag}>`;
}

const textOf = (node) => node?.children.map((c) => c.text ?? "").join("") ?? "";

// A plist dict is a flat list of <k>key</k><value> pairs.
function dictEntries(dict) {
  const els = dict.children.filter((c) => c.tag);
  const out = [];
  for (let i = 0; i + 1 < els.length; i += 2) out.push([textOf(els[i]), els[i + 1]]);
  return out;
}

const dictGet = (dict, key) => dictEntries(dict).find(([k]) => k === key)?.[1];

function dictSetString(dict, key, value) {
  const existing = dictGet(dict, key);
  if (existing) {
    existing.tag = "s";
    existing.selfClosing = false;
    existing.children = [{ text: value }];
  } else {
    dict.children.push({ tag: "k", attrs: "", children: [{ text: key }] });
    dict.children.push({ tag: "s", attrs: "", children: [{ text: value }] });
  }
}

const xmlEscape = (s) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
const xmlUnescape = (s) => s.replace(/&lt;/g, "<").replace(/&gt;/g, ">").replace(/&amp;/g, "&");

// --- state of this adapter process ---

let current = null; // { name, header, objects: [{ uid, fields: [[k, v], ...] }] }
let nextUid = 1;
let history = []; // previous level strings of the current level, for undo
let backedUp = false;

function load() {
  if (!fs.existsSync(SAVE_PATH)) throw new Error(`Save file not found: ${SAVE_PATH}`);
  const tree = parseXml(decodeSave(fs.readFileSync(SAVE_PATH)));
  const plist = tree.children.find((c) => c.tag === "plist");
  const top = plist.children.find((c) => c.tag === "dict");
  const list = dictGet(top, "LLM_01");
  if (!list) throw new Error("No local levels found in the save file");
  const levels = dictEntries(list)
    .filter(([k]) => /^k_\d+$/.test(k))
    .map(([k, d]) => ({ key: k, dict: d, name: xmlUnescape(textOf(dictGet(d, "k2"))) }));
  return { tree, list, levels };
}

function ensureClosed() {
  if (SAVE_PATH === DEFAULT_SAVE && isGameRunning()) {
    throw new Error("Geometry Dash is running. Close it completely before editing offline, or open the level in the editor to edit it live.");
  }
}

function backup() {
  if (backedUp) return;
  const dir = path.dirname(SAVE_PATH);
  const stamp = new Date().toISOString().replace(/[:.]/g, "-");
  fs.copyFileSync(SAVE_PATH, path.join(dir, `CCLocalLevels.dat.gdaibridge-backup-${stamp}`));
  const old = fs
    .readdirSync(dir)
    .filter((f) => f.startsWith("CCLocalLevels.dat.gdaibridge-backup-"))
    .sort()
    .reverse()
    .slice(BACKUPS_KEPT);
  for (const f of old) fs.unlinkSync(path.join(dir, f));
  backedUp = true;
}

function writeTree(tree) {
  ensureClosed();
  backup();
  const tmp = SAVE_PATH + ".gdaibridge-tmp";
  fs.writeFileSync(tmp, encodeSave(serializeXml(tree)));
  decodeSave(fs.readFileSync(tmp)); // verify it reads back before replacing the real file
  fs.renameSync(tmp, SAVE_PATH);
}

function parseLevel(name, levelString) {
  const parts = levelString.split(";");
  const header = parts.shift() ?? "";
  const objects = parts
    .filter((p) => p.trim())
    .map((p) => {
      const kv = p.split(",");
      const fields = [];
      for (let i = 0; i + 1 < kv.length; i += 2) fields.push([kv[i], kv[i + 1]]);
      return { uid: nextUid++, fields };
    });
  return { name, header, objects };
}

const levelString = (lvl) => [lvl.header, ...lvl.objects.map((o) => o.fields.map((f) => f.join(",")).join(","))].join(";") + ";";

function openLevel(name) {
  ensureClosed();
  const { levels } = load();
  const found = name ? levels.find((l) => l.name.toLowerCase() === name.toLowerCase()) : levels[0];
  if (!found) throw new Error(`No local level named "${name}". Levels: ${levels.map((l) => l.name).join(", ")}`);
  current = parseLevel(found.name, decodeLevelString(textOf(dictGet(found.dict, "k4"))));
  history = [];
  return current;
}

function level() {
  return current ?? openLevel();
}

// Write the current level back into the save file, keeping one undo step.
function commit(previousString) {
  const { tree, levels } = load();
  const entry = levels.find((l) => l.name === current.name);
  if (!entry) throw new Error(`Level "${current.name}" no longer exists in the save file`);
  dictSetString(entry.dict, "k4", encodeLevelString(levelString(current)));
  writeTree(tree);
  if (previousString !== undefined) history.push(previousString);
}

// Apply a change to the current level as one undoable step.
function change(fn) {
  ensureClosed();
  const lvl = level();
  const before = levelString(lvl);
  const result = fn(lvl);
  commit(before);
  return result;
}

// --- objects ---

const field = (o, key, fallback = 0) => {
  const f = o.fields.find(([k]) => k === key);
  return f ? Number(f[1]) : fallback;
};

function setField(o, key, value) {
  const f = o.fields.find(([k]) => k === key);
  if (f) f[1] = String(value);
  else o.fields.push([key, String(value)]);
}

// Same object fields as the mod's API.
function objectFields(o) {
  const fields = [["1", String(o.id)], ["2", String(o.x ?? 0)], ["3", String(o.y ?? 0)]];
  if (o.rotation !== undefined) fields.push(["6", String(o.rotation)]);
  if (o.scale !== undefined) fields.push(["32", String(o.scale)]);
  if (o.scale_x !== undefined) fields.push(["128", String(o.scale_x)]);
  if (o.scale_y !== undefined) fields.push(["129", String(o.scale_y)]);
  if (o.flip_x) fields.push(["4", "1"]);
  if (o.flip_y) fields.push(["5", "1"]);
  if (Array.isArray(o.groups)) fields.push(["57", o.groups.join(".")]);
  for (const [k, v] of Object.entries(o.props ?? {})) fields.push([k, String(v)]);
  return fields;
}

function addObjects(lvl, list) {
  if (list.length > MAX_BATCH) throw new Error(`That would create ${list.length} objects (max ${MAX_BATCH})`);
  const created = list.map((fields) => ({ uid: nextUid++, fields }));
  lvl.objects.push(...created);
  return { created: created.length, uids: created.map((o) => o.uid) };
}

function targets(lvl, body) {
  if (Array.isArray(body.uids)) {
    const set = new Set(body.uids);
    return lvl.objects.filter((o) => set.has(o.uid));
  }
  if (body.region) {
    const r = { x_min: -1e9, x_max: 1e9, y_min: -1e9, y_max: 1e9, ...body.region };
    return lvl.objects.filter((o) => {
      const x = field(o, "2"), y = field(o, "3");
      return x >= r.x_min && x <= r.x_max && y >= r.y_min && y <= r.y_max;
    });
  }
  throw new Error('Send {"uids": [...]} or {"region": {"x_min", "x_max", "y_min", "y_max"}}');
}

function compact(o) {
  let s = `${o.uid},${field(o, "1")},${field(o, "2")},${field(o, "3")}`;
  const rot = field(o, "6"), sx = field(o, "128", field(o, "32", 1)), sy = field(o, "129", field(o, "32", 1));
  if (rot !== 0 || sx !== 1 || sy !== 1) s += `,${rot},${sx},${sy}`;
  return s;
}

// Without the game we only know object types by ID, so the map is less exact than online.
const HAZARDS = new Set([8, 39, 103, 392]);
const PADS = new Set([35, 140, 67]);
const ORBS = new Set([36, 141, 84]);
const PORTALS = new Set([10, 11, 12, 13, 45, 46, 47, 99, 101, 111, 286, 287, 660, 745, 747, 749, 1331, 1933]);
const SPEED = new Set([200, 201, 202, 203, 1334]);
const TRIGGERS = new Set([899, 901, 1006, 1007, 1049, 1268, 1346, 1347, 1520, 1585, 1611, 1612, 1613, 1616, 1811, 1812, 1814, 1815, 1817, 1818, 1819, 1912, 1913, 1914, 1916, 1917, 1931, 1932, 1934, 1935, 2015, 2016, 2062, 2066, 2067, 2068, 2899, 2900, 2901, 2903, 2904, 2905, 2907, 2909, 2910, 2911, 2912, 2913, 2914, 2915, 2916, 2917, 2919, 2920, 2921, 2922, 2923, 2924, 2925, 2999, 3006, 3007, 3008, 3009, 3010, 3011, 3012, 3013, 3014, 3015, 3016, 3017, 3018, 3019, 3020, 3021, 3022, 3023, 3024, 3029, 3030, 3031, 3032, 3033, 3600, 3602, 3603, 3604, 3605, 3606, 3607, 3608, 3609, 3612, 3613, 3614, 3615, 3617, 3618, 3619, 3620, 3640, 3641, 3642, 3643, 3660, 3661, 3662]);
const PRIORITY = { "^": 6, P: 5, o: 4, _: 3, ">": 2, "#": 1 };

function mapChar(id) {
  if (HAZARDS.has(id)) return "^";
  if (PADS.has(id)) return "_";
  if (ORBS.has(id)) return "o";
  if (PORTALS.has(id)) return "P";
  if (SPEED.has(id)) return ">";
  if (TRIGGERS.has(id)) return null;
  return "#";
}

function map(lvl, q) {
  const CELL = 30;
  const xMin = num(q.x_min, 0), xMax = num(q.x_max, xMin + 60 * CELL);
  const yMin = num(q.y_min, 0), yMax = num(q.y_max, yMin + 12 * CELL);
  const cols = Math.ceil((xMax - xMin) / CELL), rows = Math.ceil((yMax - yMin) / CELL);
  if (cols <= 0 || rows <= 0) throw new Error("Empty area");
  if (cols > 300 || rows > 80) throw new Error("Area too big (max 300 x 80 blocks)");
  const grid = Array.from({ length: rows }, () => Array(cols).fill("."));
  for (const o of lvl.objects) {
    const c = mapChar(field(o, "1"));
    if (!c) continue;
    const col = Math.floor((field(o, "2") - xMin) / CELL), row = Math.floor((field(o, "3") - yMin) / CELL);
    if (col < 0 || col >= cols || row < 0 || row >= rows) continue;
    const cell = grid[row][col];
    if (cell === "." || PRIORITY[c] > PRIORITY[cell]) grid[row][col] = c;
  }
  let out = "";
  for (let r = rows - 1; r >= 0; r--) out += `${String(yMin + r * CELL + CELL / 2).padStart(5)}|${grid[r].join("")}\n`;
  if (yMin <= 0) out += `${"".padStart(5)}|${"=".repeat(cols)}\n`;
  return {
    x_min: xMin,
    cell: CELL,
    legend: "# block (offline: also decoration), ^ hazard, _ pad, o orb, P portal, > speed, = ground, . empty",
    map: out,
  };
}

const num = (v, fallback) => (v === undefined || v === "" || Number.isNaN(Number(v)) ? fallback : Number(v));

function placed(body, x, y) {
  return objectFields({ ...body, x, y });
}

// --- level list / create ---

function listLevels() {
  const { levels } = load();
  return { levels: levels.map((l) => l.name), open: current?.name ?? null };
}

function createLevel(name) {
  if (!name) throw new Error('Send {"name": "..."}');
  ensureClosed();
  const { tree, list, levels } = load();
  if (levels.some((l) => l.name.toLowerCase() === name.toLowerCase())) throw new Error(`A level named "${name}" already exists`);
  if (!levels.length) throw new Error("Create one level in the game first; it is used as a template");

  // New level = copy of the newest level's basic settings, without its objects or online data.
  const template = levels[0].dict;
  const keep = new Set(["kCEK", "k5", "k13", "k21", "k16", "k50", "k47", "k48"]);
  const dict = { tag: "d", attrs: "", children: [] };
  for (const [k, v] of dictEntries(template)) {
    if (!keep.has(k)) continue;
    dict.children.push({ tag: "k", attrs: "", children: [{ text: k }] }, structuredClone(v));
  }
  const header = decodeLevelString(textOf(dictGet(template, "k4"))).split(";")[0];
  dictSetString(dict, "k2", xmlEscape(name));
  dictSetString(dict, "k4", encodeLevelString(header + ";"));

  // Newest level is k_0: shift the others down by one. Find all key nodes before
  // renaming any, or a renamed k_0 -> k_1 would be found again as k_1.
  const keyNodes = levels.map((l) => [l.key, list.children.find((c) => c.tag === "k" && textOf(c) === l.key)]);
  for (const [key, node] of keyNodes) node.children = [{ text: `k_${Number(key.slice(2)) + 1}` }];
  const isArr = list.children.findIndex((c) => c.tag === "k" && textOf(c) === "_isArr");
  const insertAt = isArr >= 0 ? isArr + 2 : 0;
  list.children.splice(insertAt, 0, { tag: "k", attrs: "", children: [{ text: "k_0" }] }, dict);
  writeTree(tree);
  return { created: name, ...openLevelInfo(openLevel(name)) };
}

const openLevelInfo = (lvl) => ({ level_name: lvl.name, object_count: lvl.objects.length });

// --- routes, mirroring the mod's HTTP API ---

const NEEDS_GAME = "This needs Geometry Dash running with the level open in the editor (offline mode can't play the level).";

export function request(method, route, body = {}) {
  const [p, qs] = route.split("?");
  const q = Object.fromEntries(new URLSearchParams(qs ?? ""));
  const key = `${method} ${p}`;

  switch (key) {
    case "GET /status": {
      const lvl = level();
      return { in_editor: false, offline: true, ...openLevelInfo(lvl), playtesting: false };
    }
    case "GET /": return { name: "GD AI Bridge (offline)", offline: true, ...REFERENCE };
    case "GET /levels": return listLevels();
    case "POST /levels/open": return { opened: true, ...openLevelInfo(openLevel(body.name)) };
    case "POST /levels/create": return createLevel(body.name);

    case "GET /objects": {
      const lvl = level();
      const xMin = num(q.x_min, -1e9), xMax = num(q.x_max, 1e9);
      const offset = num(q.offset, 0), limit = num(q.limit, 1000);
      const matched = lvl.objects.filter((o) => field(o, "2") >= xMin && field(o, "2") <= xMax);
      const page = matched.slice(offset, offset + limit);
      if (q.format === "compact") {
        return { total: matched.length, format: "uid,id,x,y[,rotation,scale_x,scale_y];...", objects: page.map(compact).join(";") + (page.length ? ";" : "") };
      }
      return {
        total: matched.length,
        objects: page.map((o) => ({
          uid: o.uid, id: field(o, "1"), x: field(o, "2"), y: field(o, "3"), rotation: field(o, "6"),
          scale_x: field(o, "128", field(o, "32", 1)), scale_y: field(o, "129", field(o, "32", 1)),
          ...(q.raw ? { raw: o.fields.map((f) => f.join(",")).join(",") } : {}),
        })),
      };
    }
    case "GET /map": return map(level(), q);
    case "GET /level-string": return { level_string: levelString(level()) };

    case "POST /objects":
      return change((lvl) => {
        if (Array.isArray(body.objects)) {
          for (const o of body.objects) if (o.id === undefined) throw new Error('Each object needs at least an "id"');
          return addObjects(lvl, body.objects.map(objectFields));
        }
        if (typeof body.raw === "string") {
          return addObjects(lvl, body.raw.split(";").filter((s) => s.trim()).map((s) => {
            const kv = s.split(","), fields = [];
            for (let i = 0; i + 1 < kv.length; i += 2) fields.push([kv[i], kv[i + 1]]);
            return fields;
          }));
        }
        throw new Error('Send {"objects": [...]} or {"raw": "<GD object string>"}');
      });

    case "POST /objects/fill":
      return change((lvl) => {
        let { x1 = 15, y1 = 15, x2 = x1, y2 = y1 } = body;
        const sx = body.step_x ?? body.step ?? 30, sy = body.step_y ?? body.step ?? 30;
        if (sx <= 0 || sy <= 0) throw new Error("step must be > 0");
        if (x2 < x1) [x1, x2] = [x2, x1];
        if (y2 < y1) [y1, y2] = [y2, y1];
        const cols = Math.floor((x2 - x1) / sx) + 1, rows = Math.floor((y2 - y1) / sy) + 1;
        if (cols * rows > MAX_BATCH) throw new Error(`That would create ${cols * rows} objects (max ${MAX_BATCH})`);
        const list = [];
        for (let r = 0; r < rows; r++) for (let c = 0; c < cols; c++) list.push(placed(body, x1 + c * sx, y1 + r * sy));
        return addObjects(lvl, list);
      });

    case "POST /objects/line":
      return change((lvl) => {
        const { x1 = 15, y1 = 15, x2 = x1, y2 = y1 } = body;
        const length = Math.hypot(x2 - x1, y2 - y1);
        const count = body.count !== undefined ? Math.max(1, body.count) : Math.floor(length / (body.step ?? 30)) + 1;
        const list = [];
        for (let i = 0; i < count; i++) {
          const t = count === 1 ? 0 : i / (count - 1);
          list.push(placed(body, x1 + (x2 - x1) * t, y1 + (y2 - y1) * t));
        }
        return addObjects(lvl, list);
      });

    case "POST /objects/copy":
      return change((lvl) => {
        const src = targets(lvl, body);
        if (!src.length) throw new Error("No objects matched");
        const dx = body.dx ?? 0, dy = body.dy ?? 0, times = Math.max(1, body.times ?? 1);
        const list = [];
        for (let t = 1; t <= times; t++) {
          for (const o of src) {
            const copy = { fields: o.fields.map((f) => [...f]) };
            setField(copy, "2", field(o, "2") + dx * t);
            setField(copy, "3", field(o, "3") + dy * t);
            list.push(copy.fields);
          }
        }
        return addObjects(lvl, list);
      });

    case "POST /objects/move":
      return change((lvl) => {
        const objs = targets(lvl, body);
        for (const o of objs) {
          setField(o, "2", field(o, "2") + (body.dx ?? 0));
          setField(o, "3", field(o, "3") + (body.dy ?? 0));
        }
        return { moved: objs.length };
      });

    case "POST /objects/update":
      return change((lvl) => {
        if (!body.set) throw new Error('Send {"uids": [...], "set": {...}}');
        const objs = targets(lvl, body);
        if (!objs.length) throw new Error("No objects matched");
        const overrides = objectFields({ id: 0, ...body.set }).filter(([k]) =>
          k === "1" ? body.set.id !== undefined : k === "2" ? body.set.x !== undefined : k === "3" ? body.set.y !== undefined : true);
        for (const o of objs) for (const [k, v] of overrides) setField(o, k, v);
        // Offline objects keep their uids (unlike live mode, where they are replaced).
        return { updated: objs.length, uids: objs.map((o) => o.uid) };
      });

    case "POST /objects/delete":
      return change((lvl) => {
        const gone = new Set(targets(lvl, body));
        lvl.objects = lvl.objects.filter((o) => !gone.has(o));
        return { deleted: gone.size };
      });

    case "POST /undo": {
      ensureClosed();
      const previous = history.pop();
      if (previous === undefined) throw new Error("Nothing to undo in this offline session");
      current = parseLevel(current.name, previous);
      commit();
      return { ok: true };
    }

    case "GET /selection":
    case "POST /select":
    case "POST /playtest":
    case "POST /test":
      throw new Error(NEEDS_GAME);
  }
  throw new Error(`Unknown route ${key}`);
}
