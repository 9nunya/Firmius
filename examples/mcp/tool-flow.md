# MCP TOOL FLOW (THE NO-DRAMA VERSION)

All examples below are the JSON args you pass to the named Firmius MCP tools.

## 1) See what the server even has

Tool: `mcp_list`

```json
{"server_name":"filesystem"}
```

## 2) Search for the thing you care about

Tool: `mcp_search`

```json
{"query":"read","server_name":"filesystem"}
```

## 3) Load tools/resources/prompts into runtime state

Tool: `mcp_load`

```json
{
  "server_name":"filesystem",
  "tools":["read_file"],
  "resources":[],
  "prompts":[]
}
```

After a successful load, Firmius can expose loaded tools as dynamic names shaped like:

```text
mcp__filesystem__read_file
```

## 4) Call the remote tool

Tool: `mcp_call`

```json
{
  "server_name":"filesystem",
  "tool_name":"read_file",
  "arguments":{"path":"/tmp/example.txt"}
}
```

## 5) If you want resources/prompts, load those too

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

## Tiny caveat so you don't get got

- `mcp_call` does **not** require `mcp_load` first.
- `mcp_read_resource` and `mcp_get_prompt` **do** require load state.

So if reads/prompts are failing while calls work... that's probably why.
