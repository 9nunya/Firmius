# MCP EXAMPLES, NO CREDIT CARD REQUIRED

These example files match the MCP behavior that Firmius actually implements right now.

## What's in here?

- `config.filesystem-stdio.json` — local stdio server example. Nice for smoke tests and zero-credential messing around.
- `config.http-local.json` — local HTTP MCP endpoint example.
- `tool-flow.md` — the explicit Firmius `mcp_*` tool flow.

## The easy path

If you just wanna prove MCP is alive without summoning auth demons:

1. merge `config.filesystem-stdio.json` into `~/.firmius/config.json`
2. use the calls in `tool-flow.md`
3. point it at `/tmp` and poke around

That's it. Nice and boring. Boring is good when you're testing transport plumbing.
