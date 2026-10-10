# gd-ai-bridge-mcp

MCP server for [GD AI Bridge](https://github.com/RibomDev/GD-ai-bridge), a Geode mod that lets any AI read and edit levels in the Geometry Dash editor.

You also need the mod itself, see the [installation guide](https://github.com/RibomDev/GD-ai-bridge#installation).

```json
{
  "mcpServers": {
    "geometry-dash": {
      "command": "npx",
      "args": ["-y", "gd-ai-bridge-mcp"]
    }
  }
}
```

Set `GD_BRIDGE_URL` if you changed the port in the mod settings (default `http://127.0.0.1:7312`).
