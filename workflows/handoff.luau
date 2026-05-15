-- /handoff
--
-- Contract:
-- - Ask the CURRENT focused agent to immediately emit a structured handoff JSON.
-- - Capture it synchronously.
-- - Reset the CURRENT focused agent (clears history).
-- - Re-inject that handoff JSON as the new seed message to the same agent id.
--
-- Why agent.ask?
-- - It targets an existing agent id (default: focused agent) and synchronously
--   waits for its outcome (via Engine::waitForAgentOutcome). It does NOT spawn
--   a new agent, preserving persona/identity.

local function trim(s)
  s = tostring(s or "")
  return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function safe_tostring(x)
  if x == nil then return "" end
  return tostring(x)
end

local function json_escape(s)
  s = safe_tostring(s)
  s = s:gsub('\\', '\\\\')
  s = s:gsub('"', '\\"')
  s = s:gsub('\n', '\\n')
  s = s:gsub('\r', '\\r')
  s = s:gsub('\t', '\\t')
  return s
end

local function to_minijson(obj)
  local t = type(obj)
  if t == "nil" then return "null" end
  if t == "boolean" then return obj and "true" or "false" end
  if t == "number" then return tostring(obj) end
  if t == "string" then return '"' .. json_escape(obj) .. '"' end
  if t ~= "table" then return '"' .. json_escape(tostring(obj)) .. '"' end

  -- Detect array-like table
  local is_array = true
  local max_i = 0
  for k, _ in pairs(obj) do
    if type(k) ~= "number" then
      is_array = false
      break
    end
    if k > max_i then max_i = k end
  end

  if is_array then
    local parts = {}
    for i = 1, max_i do
      parts[#parts + 1] = to_minijson(obj[i])
    end
    return "[" .. table.concat(parts, ",") .. "]"
  end

  local parts = {}
  for k, v in pairs(obj) do
    parts[#parts + 1] = '"' .. json_escape(k) .. '":' .. to_minijson(v)
  end
  return "{" .. table.concat(parts, ",") .. "}"
end

-- Envelope
local payload_env = event.payload or {}
local extra_env = payload_env.extra or {}

local thread_id = safe_tostring(payload_env.thread_id or event.thread_id)
local agent_id = safe_tostring(payload_env.agent_id or event.agent_id)
local persona = safe_tostring(payload_env.persona or event.persona)
local active_mode = safe_tostring(payload_env.active_mode or event.active_mode)
local workflow_id = safe_tostring(extra_env.workflow_id or payload_env.completed_workflow or event.workflow_id)
local slash_command = safe_tostring(extra_env.slash_command or event.slash_command)

-- Runtime snapshots
local log_summary = thread.log_summary() or {}
local user_messages = thread.messages({ role = "user" }) or {}
local tool_calls = thread.tool_calls({ limit = 80 }) or {}
local tool_results = thread.tool_results({ limit = 80 }) or {}

local payload = {
  kind = "firmius_handoff_context",
  identity = {
    thread_id = thread_id,
    agent_id = agent_id,
    persona = persona,
    active_mode = active_mode,
  },
  workflow = {
    workflow_id = workflow_id,
    slash_command = slash_command,
  },
  log_summary = log_summary,
  user_messages = user_messages,
  tool_calls = tool_calls,
  tool_results = tool_results,
}

local handoff_schema_prompt = [[
You are the focused agent. You MUST immediately generate a HANDOFF.

Output rules:
- Output ONE JSON object only. No markdown. No commentary.
- JSON must be valid.
- Include concrete file paths, tool names, commands, and any file:line anchors you have.
- If unknown, use null or "unknown"; do not omit keys.

Required keys:
{
  "handoff_title": string,
  "thread_id": string,
  "agent_persona": string,
  "active_mode": string,
  "generated_at": string,

  "current_task": string,
  "current_progress": string,
  "task_history": string,
  "task_log": string,

  "user_messages": array,
  "files_read": array,
  "files_edited": array,
  "commands_run": array,
  "tool_calls": array,
  "tool_results": array,

  "knowns": array,
  "beliefs": array,
  "unknowns": array,
  "risks": array,

  "next_actions": array,
  "verification": {
    "recommended": string,
    "last_run": string
  }
}

Context payload JSON:
]]

local delegated_task = handoff_schema_prompt .. "\n" .. to_minijson(payload)

-- Ask focused agent (same id) to generate the handoff *now*.
local res = agent.ask(agent_id, delegated_task, { timeout_sec = 180 })

local handoff_text = ""
if res ~= nil then
  handoff_text = safe_tostring(res.text)
end

if trim(handoff_text) == "" then
  handoff_text = '{"handoff_title":"handoff_failed","thread_id":"' .. json_escape(thread_id) .. '",' ..
                '"agent_persona":"' .. json_escape(persona) .. '",' ..
                '"active_mode":"' .. json_escape(active_mode) .. '",' ..
                '"generated_at":"unknown",' ..
                '"current_task":"unknown","current_progress":"unknown","task_history":"unknown","task_log":"handoff generator returned empty",' ..
                '"user_messages":[],"files_read":[],"files_edited":[],"commands_run":[],"tool_calls":[],"tool_results":[],' ..
                '"knowns":[],"beliefs":[],"unknowns":[],"risks":[],"next_actions":[],' ..
                '"verification":{"recommended":"unknown","last_run":"unknown"}' ..
                '}'
end

-- Persist the handoff into hook state for retrieval/debugging.
state.write("global", "handoff.last_json", {
  thread_id = thread_id,
  agent_id = agent_id,
  persona = persona,
  active_mode = active_mode,
  handoff_json = handoff_text,
})

-- Reset the CURRENT agent (clears its history and state).
agent.reset(agent_id)

-- Re-inject the handoff JSON as the new seed task for the same agent id.
local reinject_prompt =
  "You are starting fresh. Treat the following HANDOFF JSON as your entire context.\n" ..
  "Respond by confirming you loaded it and stating the next concrete action.\n\n" ..
  handoff_text

agent.execute(agent_id, reinject_prompt)

-- Return the handoff as the workflow's reminder/trophy payload.
return outcome.allow({ outcome = "handoff", reminder = handoff_text })
