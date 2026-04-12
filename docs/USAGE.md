# USAGE TUT

WOW, so you actually wanna use Firmius!?!? THANKS!!

## Purposes

In Firmius, there are purposes. A "purpose" is basically the system prompt and manifest for an agent. It defines it's personality,
work instructions, and how it should report to its peers (if any)

### Default personas

- Lead
This agent is like the top-level overseer. It'll take your task and start discovery, trying to find an optimal execution path.
It decides if multi-agent orchestration is needed, or if it can be done by it's self. This is the starting point.
Discovery-, diagnosis-, audit-, and other read-only tasks should usually stay on the lead's todo/direct lane instead of becoming plans.
When the work needs delegated execution, the lead can mutate and create PLANS in the active thread.
Small plans (roughly `<= 3` chunks) may be committed directly by lead; larger plans should go through `planner` + `plan_checker`.
When it reaches execution, it will delegate each plan chunk to an executor, paralellizing where possible.
- Executor
This is the agent that gets shit done. It directly modifies code according to the chunk, or manages it's own worker subagents to complete the chunk sub-tasks. After it's done, it generates a worker report artifact, and dies. Purpose fulfilled. Bye bye!!
- Planner
Self-explanatory, but important! This agent must use a good SOTA model (gpt 5.4 best :O) because it plans the entire execution graph. It takes lead's completed discovery context and formulates a DRAFT plan with chunks and subchunks. It should not be used as a substitute for unfinished diagnosis or repo familiarization. It writes that to an artifact, and DIES!! X_X
- Plan Checker
Draft plans are.. NOT always perfect. Sometimes the planner is too vague, sometimes it makes circular dependencies.. and someone needs to check that. That SOMEONE is a clump of ones and zeros constantly changing.. an AGENT, DUMMY! This agent checks the drafted plan against a predefined and calibrated constraint set i wrote, and responds with a verdict: Accept, or Accept with FIXES. Lead sees this, and if ACCEPT, it exits the plan workflow, and commits the plan with it's tools. If accept with fixes, then it respawns the planner subagent to fix the plan based on the criteria provided by the plan checker subagent.
- Worker
General purpose, but executor-owned in the normal flow. Workers handle bounded chunk tasks delegated by an executor rather than owning chunks or top-level planning.
- Scout
Used for searching or retrieving information. Read-only, owns no part of the plan.

OK.. you know purposes now.. GREAT! That's the surface level. Let's get to the nitty gritty, fritty!

## Providers

OK, now let's get to the actual part where all this intelligence comes from.. PROVIDERS!!
In firmius, there are two types of providers: OAuth, and APIKey based providers.
OAuth providers use accounts with retry/switch logic to execute stream requests. They also manage quota, token refresh, etc..
APIKey providers just talk to the api and done. Nothing else to note.. You only need an api key to operate them. Also, they support multiple api keys per provider, so they have retry/switch logic too.
To add a provider, simply use the /connect command, second arg the ID of the provider you wanna connect!
OAuth providers have a nice pretty wizard to help you through the process, and so do API key providers! YAY!
To view all accounts for an **OAuth** provider, use `/accounts <id>`. It'll list all accounts.
To view quotas for an **OAuth** provider, use `/quotas <id>`. It will list all quotas for all accounts in that provider.
