# GD AI Bridge

A Geode mod that lets **any AI** read and edit levels in the Geometry Dash editor.
Ask Claude, ChatGPT or any other AI to build, inspect or change your level, and watch it happen live in the editor.

Supports Geometry Dash 2.2081 on Windows, Geode 5.10.1+.

## Installation

### 1. Install the mod

1. Install [Geode](https://geode-sdk.org) if you don't have it yet.
2. Download `ribomdev.gd-ai-bridge.geode` from the [latest release](https://github.com/RibomX/GD-AI-bridge/releases/latest).
3. Put it into your `Geometry Dash/geode/mods` folder (in game: Geode menu → settings icon → *Open Mods Folder*).
4. Restart Geometry Dash.

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

Open a level in the editor and ask your AI something like *"build me a short neon level with a ship part"*.

Keep the game window open while the AI works; a minimized game can't respond.

## Troubleshooting

| Problem | Fix |
| --- | --- |
| "Cannot reach Geometry Dash" | The game isn't running, or the mod isn't installed / enabled. |
| "Game did not respond in time" | The game window is minimized. Restore it. |
| "Not in the level editor" | Open a level in the editor. |
| Port 7312 is taken | Change the port in the mod settings, restart the game, and set `GD_BRIDGE_URL` (e.g. `http://127.0.0.1:7400`) in the MCP config's `env`. |

---

## For developers

### API

The mod runs a small JSON API on `127.0.0.1:7312`, so anything that can make HTTP requests can use it, not just MCP clients:
function-calling models (OpenAI, Gemini, Ollama, ...) or your own scripts.

`GET /` returns the endpoint list, the coordinate system and common object IDs, so an AI can discover everything it needs with one request.

| Method | Path | Body / query |
| --- | --- | --- |
| GET | `/status` | – |
| GET | `/objects` | `x_min`, `x_max`, `offset`, `limit`, `raw=1` |
| GET | `/selection` | `raw=1` |
| GET | `/level-string` | – |
| POST | `/objects` | `{"objects": [{"id": 1, "x": 15, "y": 15}]}` or `{"raw": "1,1,2,15,3,15;"}` |
| POST | `/objects/delete` | `{"uids": [12, 13]}` |
| POST | `/select` | `{"uids": [12]}` |
| POST | `/undo` | – |
| POST | `/playtest` | `{"action": "start"}` / `{"action": "stop"}` |

Object fields: `id`, `x`, `y`, `rotation`, `scale`, `scale_x`, `scale_y`, `flip_x`, `flip_y`, `groups`, and `props` for any other raw GD property (e.g. `{"21": 3}` sets the main color channel).
Units: 1 block = 30; grid cell centers are 15, 45, 75, ...

Adding and deleting objects each create one normal undo step, so Ctrl+Z works as usual.
Objects brought back by undo get new uids, so fetch them again before referring to them.

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
