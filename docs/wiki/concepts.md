# Core concepts

**Session:** a durable conversation and its state. **Agent:** a model-backed
worker with a persona and tools. **Artifact:** stored text or a file result.
**Work graph:** nodes and edges describing dependencies, retries, gates, and
results. **Managed run:** the driver advances ready graph nodes and announces
their live status. **Gate:** a verification or review condition that must pass
before work is accepted.

These boundaries let you replace a fragile “please do everything” prompt with
small, inspectable steps.