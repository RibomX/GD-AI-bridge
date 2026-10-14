# GD AI Bridge

A Geode mod that lets **any AI** read and edit levels in the Geometry Dash editor.
Ask Claude, ChatGPT or any other AI to build, inspect or change your level, and watch it happen live in the editor.

Supports Geometry Dash 2.2081 on Windows, Geode 5.10.1+.

## What's new in v0.2.0

- **Build much faster** – new tools fill areas, draw lines of objects, copy/repeat sections, move and edit objects in one step instead of placing every block separately
- **AI tests its own levels** – plays the level with planned inputs (up to 4x speed) and reports where and on what the player died
- **Measured physics** – jump heights and lengths per speed, pads, orbs and ship, so the AI builds beatable jumps on the first try
- **ASCII map + compact object list** – the AI sees the level layout for a fraction of the tokens
- **Works while minimized** – in the editor the game keeps running in the background
- **Offline mode** – with the game closed, the AI can list, create and edit your local levels directly in the save file (with automatic backups)

Coming from v0.1.0? See [Updating](#updating-from-an-older-version).

## What the AI can do

- **Build fast** – place single objects, or fill areas, draw lines, copy and repeat whole sections in one step
- **See the level** – a cheap ASCII map of any area, plus a compact object list
- **Test its own work** – play the level with planned inputs and get back where and on what the player died
- **Know the physics** – jump heights and lengths per speed, pads, orbs and ship, measured in-game
- **Work in the background** – the game keeps running while minimized in the editor
- **Work with the game closed** – list, create and edit your local levels directly in the save file

Everything the AI does is a normal editor action, so Ctrl+Z works as usual.

> AI testing is for checking your level while you build. Verifying a level for upload is up to you – the mod never verifies levels.

## Installation

### 1. Install the mod

1. Install [Geode](https://geode-sdk.org) if you don't have it yet.
2. Download `ribomdev.gd-ai-bridge.geode` from the [latest release](https://github.com/RibomX/GD-AI-bridge/releases/latest).
3. Put it into your `Geometry Dash/geode/mods` folder (in game: Geode menu → settings icon → *Open Mods Folder*).
4. Restart Geometry Dash.

#### Updating from an older version

1. Close Geometry Dash.
2. In the mods folder, delete the old `ribomdev.gd-ai-bridge.geode` (and any `ribomdev.gd-ai-bridge (1).geode`). Keep only the new file – if there are two, Geode may load the old one.
3. Start the game. In the mod list you should see the new version.

Your AI connection doesn't need any changes: `npx` downloads the newest adapter automatically.

> Geode's mod index does not accept mods that use AI to place objects, so this mod is only available here on GitHub.

### 2. Connect your AI

Install [Node.js](https://nodejs.org) (the LTS version), then pick your AI below.

#### AI in the terminal – one command

Run the command for your AI once in a terminal, then restart the AI.

**Claude Code**

```bash
claude mcp add -s user geometry-dash -- cmd /c npx -y gd-ai-bridge-mcp
```

**Gemini CLI**

```bash
gemini mcp add -s user geometry-dash cmd /c npx -y gd-ai-bridge-mcp
```

**Codex CLI**

```bash
codex mcp add geometry-dash -- cmd /c npx -y gd-ai-bridge-mcp
```

#### AI apps – paste into the config file

1. Open the app's MCP config file:

   | App | Where |
   | --- | --- |
   | Claude Desktop | *Settings → Developer → Edit Config* |
   | Cursor | *Settings → MCP → Add new MCP server* |
   | LM Studio | *Program* tab → *Install* → *Edit mcp.json* |

2. Look at what's inside and pick the matching case.

   **The file is empty, or only contains `{}`** – replace everything with:

   ```json
   {
     "mcpServers": {
       "geometry-dash": {
         "command": "cmd",
         "args": ["/c", "npx", "-y", "gd-ai-bridge-mcp"]
       }
     }
   }
   ```

   **The file already has `"mcpServers"` with other servers** – don't paste a second `{ ... }` block.
   Add only the `"geometry-dash"` entry inside the existing `"mcpServers"`, and put a comma between it and the next server:

   ```json
   {
     "mcpServers": {
       "geometry-dash": {
         "command": "cmd",
         "args": ["/c", "npx", "-y", "gd-ai-bridge-mcp"]
       },
       "some-other-server": {
         "command": "...",
         "args": ["..."]
       }
     },
     "preferences": { "...": "..." }
   }
   ```

   Everything outside `"mcpServers"` (like `"preferences"` above) stays exactly as it was.

   You can also add it *after* the other servers. Either way, the rule is: **every server's closing `}` needs a comma, except the last one.**

   **The file has other settings but no `"mcpServers"`** – add `"mcpServers"` as the first entry, with a comma after it:

   ```json
   {
     "mcpServers": {
       "geometry-dash": {
         "command": "cmd",
         "args": ["/c", "npx", "-y", "gd-ai-bridge-mcp"]
       }
     },
     "preferences": { "...": "..." }
   }
   ```

3. Save the file and fully restart the app (Claude Desktop: right-click its tray icon → *Quit*, then open it again).

> **VS Code** uses `.vscode/mcp.json` and the key is `"servers"` instead of `"mcpServers"`; the `"geometry-dash"` entry is the same.

> **Careful:** a single missing comma or extra `}` stops the app from loading *any* MCP server. If nothing shows up after the restart, check the commas and braces first.

### 3. Use it

**Live (game open):** open a level in the editor and ask your AI something like *"build me a short neon level with a ship part, and test that it's beatable"*.
You can minimize the game while the AI works – in the editor it keeps running (mod setting *Keep running when minimized*).

**Offline (game closed):** close Geometry Dash completely and ask your AI to *"create a new level called Neon Run and build ..."*. It edits your save file directly; you'll see the level next time you start the game.
Before its first change, the AI backs up your save file as `CCLocalLevels.dat.gdaibridge-backup-<date>` next to it (`%LOCALAPPDATA%\GeometryDash`; the last 5 are kept).
Playtests need the game, so offline the AI can build but not test.

> Don't start the game while the AI is editing offline. The AI stops writing as soon as it sees the game running, but the game would overwrite changes made after it started.

## Troubleshooting

| Problem | Fix |
| --- | --- |
| "Geometry Dash is running but the mod doesn't answer" | The mod isn't installed / enabled, or an old copy is loaded (see [Updating](#updating-from-an-older-version)). |
| "Game did not respond in time" | The game is loading or a popup is open; try again. If you turned off *Keep running when minimized*, restore the window. |
| "Not in the level editor" | Open a level in the editor, or close the game completely to work offline. |
| A level made offline doesn't show up | Start the game only after the AI is done. Your previous save file is in the backups next to it. |
| Port 7312 is taken | Change the port in the mod settings, restart the game, and set `GD_BRIDGE_URL` (e.g. `http://127.0.0.1:7400`) in the MCP config's `env`. |

---

## For developers

### API

The mod runs a small JSON API on `127.0.0.1:7312`, so anything that can make HTTP requests can use it, not just MCP clients:
function-calling models (OpenAI, Gemini, Ollama, ...) or your own scripts.

`GET /` returns the endpoint list, the coordinate system, common object IDs and measured physics, so an AI can discover everything it needs with one request.

| Method | Path | Body / query |
| --- | --- | --- |
| GET | `/status` | – |
| GET | `/objects` | `x_min`, `x_max`, `offset`, `limit`, `raw=1`, `format=compact` |
| GET | `/map` | `x_min`, `x_max`, `y_min`, `y_max` – ASCII map, one character per block |
| GET | `/selection` | `raw=1` |
| GET | `/level-string` | – |
| POST | `/objects` | `{"objects": [{"id": 1, "x": 15, "y": 15}]}` or `{"raw": "1,1,2,15,3,15;"}` |
| POST | `/objects/fill` | `{"id": 1, "x1": 15, "y1": 15, "x2": 285, "y2": 75, "step"?: 30}` |
| POST | `/objects/line` | `{"id": 8, "x1": 300, "y1": 15, "x2": 600, "step"?: 30 \| "count"?: 5}` |
| POST | `/objects/copy` | `{"uids": [...] \| "region": {...}, "dx": 300, "dy"?: 0, "times"?: 3}` |
| POST | `/objects/move` | `{"uids": [...] \| "region": {...}, "dx": 30, "dy": 0}` |
| POST | `/objects/update` | `{"uids": [...] \| "region": {...}, "set": {"rotation": 90, "props": {"21": 3}}}` |
| POST | `/objects/delete` | `{"uids": [12, 13]}` or `{"region": {"x_min": 0, "x_max": 300}}` |
| POST | `/select` | `{"uids": [12]}` |
| POST | `/undo` | – |
| POST | `/playtest` | `{"action": "start"}` / `{"action": "stop"}` |
| POST | `/test` | `{"inputs": [{"x": 255}, {"x": 900, "release": 1100}], "until_x"?, "speed"?: 1-4, "path_step"?}` |

Object fields: `id`, `x`, `y`, `rotation`, `scale`, `scale_x`, `scale_y`, `flip_x`, `flip_y`, `groups`, and `props` for any other raw GD property (e.g. `{"21": 3}` sets the main color channel).
A `region` is `{"x_min", "x_max", "y_min", "y_max"}` (all optional). Fill, line and copy create at most 10 000 objects per request.
Units: 1 block = 30; grid cell centers are 15, 45, 75, ...; y = 15 is the first row above the ground.

Every change is one normal undo step, so Ctrl+Z works as usual – except `update`, which replaces the objects (they get new uids) and takes two undo steps.
Objects brought back by undo get new uids, so fetch them again before referring to them.

`/test` plays the level with the given inputs (a press without `release` is a tap) and answers when the run ends:
`result` (`passed` / `died` / `stopped` / `timeout`), position, `progress_percent`, `objects_near_death` and the player `path`.

Offline mode lives in the MCP adapter ([`mcp/offline.js`](mcp/offline.js)), not in the mod: with the game closed it serves the same routes from the save file, plus `GET /levels`, `POST /levels/open` and `POST /levels/create`.

Example:

```bash
curl -X POST http://127.0.0.1:7312/objects -d "{\"objects\":[{\"id\":1,\"x\":15,\"y\":15},{\"id\":8,\"x\":45,\"y\":15}]}"
```

### Security

The server listens on `127.0.0.1` only. Requests that carry an `Origin` header (web pages) or a foreign `Host` header (DNS rebinding) are rejected, so websites open in your browser cannot touch your levels.

### Working on the MCP adapter

The adapter lives in [`mcp/`](mcp) and is published to npm as `gd-ai-bridge-mcp`. To run it from source:

```bash
cd mcp
npm install
```

Claude Code picks up the local copy automatically from [`.mcp.json`](.mcp.json) when you open this folder.

### Building the mod

Requirements: Visual Studio 2022 Build Tools (MSVC 19.44+), CMake, Ninja, Geode CLI + SDK.

```bash
build.bat
```

This builds `build/ribomdev.gd-ai-bridge.geode` and installs it into your Geode mods folder.

### Releasing

1. Bump `version` in `mod.json` and `mcp/package.json`, run `build.bat`.
2. `cd mcp && npm publish`
3. Create a GitHub release and attach `build/ribomdev.gd-ai-bridge.geode`.

## License

[MIT](LICENSE)
