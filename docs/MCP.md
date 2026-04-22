# MCP in Firmius

Firmius treats MCP like infrastructure, not a demo feature.

If an agent runtime claims MCP support, it should do more than list a server and hope for the best. Firmius supports the full day-to-day flow: configure servers, inspect capabilities, load what matters into runtime state, call tools, read resources, resolve prompts, and surface loaded tools directly inside the agent tool list.

## Supported transports

Firmius currently supports these `mcpServers` transport values:

- `stdio`
- `http`

## Where MCP config lives

```text
~/.firmius/config.json
```

Top-level shape:

```json
{
  "mcpServers": {}
}
```

Each entry is keyed by server name.

## Canonical config shape

```json
{
  "mcpServers": {
    "filesystem": {
      "transport": "stdio",
      "stdio": {
        "command": "npx",
        "args": [
          "-y",
          "@modelcontextprotocol/server-filesystem",
          "/tmp"
        ],
        "env": {},
        "cwd": "",
        "enabled": true
      }
    },
    "local-http": {
      "transport": "http",
      "http": {
        "url": "http://127.0.0.1:3001/mcp",
        "authHeader": "Authorization",
        "authBearerToken": "",
        "allowInsecureTls": false,
        "caCertPath": "",
        "enabled": false
      }
    }
  }
}
```

## How MCP works in Firmius

> Runtime note: the old static MCP helper tools are being retired from the live tool surface.

The runtime path that matters is:

1. configure MCP servers in config
2. load capabilities into runtime state through the MCP manager path
3. let loaded MCP tools surface dynamically as real callable tool names

Dynamic loaded MCP tools appear as:

```text
mcp__<server>__<tool>
```

Runtime state still tracks:

`loadedMcpServers`
`loadedMcpTools`
`loadedMcpResources`
`loadedMcpPrompts`

That means the system can move from MCP setup into a tighter active tool surface for the thread without depending on a permanent static MCP family in the registry.

## Dynamic MCP flow

For a zero-credential setup, use the filesystem example in [`examples/mcp/`](../examples/mcp/).

Typical flow:

1. configure the server
2. load the capability set into runtime state
3. call the dynamic tool name that appears after loading

Example dynamic tool name:

```text
mcp__filesystem__read_file
```

## Common failure cases

`Unknown MCP server` — the name is missing from `mcpServers`
`MCP server is disabled` — the transport exists but `enabled` is false
`MCP server command is empty` — `stdio.command` is missing
`MCP server URL is empty` — `http.url` is missing
`MCP server is not loaded` — the runtime has not accepted that server into loaded state
`Loaded MCP tool not available on server` — the dynamic tool was not actually loaded for that server

## Why this matters

Firmius still supports real MCP-backed work, but the durable runtime surface is the dynamic loaded tool itself, not a permanently advertised static MCP helper family.

That keeps the live tool block smaller and pushes the thread toward the real tool contract it can actually call.
