# MCP IN FIRMIUS (YEAH, IT'S REAL NOW)

Old docs used to talk about MCP like it was some distant prophecy.

Nope.
This doc covers the MCP behavior that is **actually implemented in the code right now**.

For starter configs without credentials, go peek at `examples/mcp/`.

## What transport modes exist?

Firmius supports these `mcpServers` transport values today:

- `stdio`
- `http`

`remote` is **not** its own separate transport key in the current parser/runtime.

## Where the config lives

MCP servers are configured in:

```text
~/.firmius/config.json
```

Top-level key:

```json
{
  "mcpServers": {}
}
```

Each server is keyed by server name.

## Canonical config shape

This is the canonical nested shape written by the config loader:

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

## Important notes before you get clever

- Legacy flat fields are still accepted on load for backward compatibility.
- Save output gets normalized back into the nested canonical shape above.
- Disabled servers are ignored by `mcp_list` and `mcp_search`.
- `allowInsecureTls` and `caCertPath` are parsed, but current HTTP runtime still errors if you try to use them.

So yeah... `stdio` and straightforward `http` are the happy paths right now.

## The tool model, minus the mystery

### Discovery

Use these first:

- `mcp_list` — list tools/resources/prompts on enabled configured server(s)
- `mcp_search` — search capability names/descriptions across enabled configured server(s)

### Loading runtime state

`mcp_load` validates your requested tools/resources/prompts against live server capabilities and stores the selection in runtime state.

Loaded state is tracked in the runtime overlay via:

- `loadedMcpServers`
- `loadedMcpTools`
- `loadedMcpResources`
- `loadedMcpPrompts`

### Reads that DO require loaded state

These need a successful `mcp_load` first:

- `mcp_read_resource`
- `mcp_get_prompt`

### Calls that do NOT require loaded state

`mcp_call` is looser.

It validates the requested server/tool against live `tools/list`, but it does **not** require a prior `mcp_load` to succeed.

### Dynamic tool exposure

After a successful `mcp_load`, loaded MCP tools can also appear as dynamic tool names in this shape:

```text
mcp__<server>__<tool>
```

That means you can go from generic MCP discovery into a tighter “this specific remote tool is now in the live tool list” flow.

## Credential-free quick flow

Wanna test MCP without turning this into a credential-management side quest? Use the local filesystem stdio server from `examples/mcp/config.filesystem-stdio.json`.

### 1) List server capabilities

Tool: `mcp_list`

```json
{"server_name":"filesystem"}
```

### 2) Search what is available

Tool: `mcp_search`

```json
{"query":"read","server_name":"filesystem"}
```

### 3) Load what you want for this run

Tool: `mcp_load`

```json
{
  "server_name":"filesystem",
  "tools":["read_file"],
  "resources":[],
  "prompts":[]
}
```

### 4) Call a remote MCP tool

Tool: `mcp_call`

```json
{
  "server_name":"filesystem",
  "tool_name":"read_file",
  "arguments":{"path":"/tmp/example.txt"}
}
```

### 5) Load and read a resource or prompt

Tool: `mcp_load`

```json
{
  "server_name":"filesystem",
  "resources":["file:///tmp/example.txt"],
  "prompts":[]
}
```

Tool: `mcp_read_resource`

```json
{"server_name":"filesystem","uri":"file:///tmp/example.txt"}
```

Tool: `mcp_get_prompt`

```json
{"server_name":"filesystem","prompt_name":"example_prompt","arguments":{}}
```

## Common failure causes

- `Unknown MCP server` — the name is not present under `mcpServers`.
- `MCP server is disabled` — set `enabled: true` on the active transport block.
- `MCP server command is empty` — your `stdio.command` is missing.
- `MCP server URL is empty` — your `http.url` is missing.
- `MCP server is not loaded` — you tried a load-required read/get flow without `mcp_load`.
- `MCP resource is not loaded...` — include the URI in `mcp_load`.
- `MCP prompt is not loaded...` — include the prompt in `mcp_load`.

## Short version

If you just want the least annoying path:

1. use the filesystem stdio example
2. `mcp_list`
3. `mcp_search`
4. `mcp_load`
5. `mcp_call`

That's the happy path. Keep it boring first. Then get weird.
