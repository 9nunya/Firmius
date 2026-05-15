local promise = state.read("thread", "promise")
if type(promise) ~= "table" or promise.state ~= "open" or not promise.id then
  return outcome.allow({})
end

local payload = event.payload or {}
if promise.agent_id and promise.agent_id ~= "" and payload.agent_id ~= promise.agent_id then
  return outcome.allow({})
end

local function as_number(value, fallback)
  local n = tonumber(value)
  if n == nil then
    return fallback
  end
  return n
end

local function one_line(value, fallback)
  if type(value) ~= "string" or value == "" then
    return fallback
  end
  value = value:gsub("\r", " "):gsub("\n", " ")
  if #value > 900 then
    return value:sub(1, 900) .. "..."
  end
  return value
end

local function list_lines(values, key, limit)
  if type(values) ~= "table" then
    return "  <none>"
  end
  local out = {}
  local count = 0
  for _, item in ipairs(values) do
    count = count + 1
    if count > limit then
      table.insert(out, "  ...")
      break
    end
    local text = item
    if type(item) == "table" and key then
      text = item[key]
    end
    table.insert(out, "  - " .. one_line(tostring(text or ""), "<empty>"))
  end
  if #out == 0 then
    return "  <none>"
  end
  return table.concat(out, "\n")
end

local function verdict_from(result)
  if type(result) ~= "table" then
    return "reject", "Shrike returned no result."
  end
  local json = result.json
  local verdict = nil
  local suggestion = nil
  local evidence = nil
  if type(json) == "table" then
    verdict = json.verdict
    if type(verdict) == "table" then
      verdict = verdict.kind
    end
    suggestion = json.suggestion or json.notes or json.reason
    evidence = json.evidence
  end
  if type(verdict) ~= "string" or verdict == "" then
    local text = string.lower(result.text or "")
    if text:find("accept", 1, true) or text:find("sealed", 1, true) then
      verdict = "accept"
    else
      verdict = "reject"
    end
  end
  if type(suggestion) ~= "string" or suggestion == "" then
    suggestion = result.text or "Shrike did not provide notes."
  end
  return string.lower(verdict), suggestion, evidence
end

local function trim(value)
  if type(value) ~= "string" then
    return ""
  end
  return value:gsub("^%s+", ""):gsub("%s+$", "")
end

local function extract_completion_block(text)
  if type(text) ~= "string" then
    return nil
  end
  return text:match("<PROMISE_COMPLETION>%s*(.-)%s*</PROMISE_COMPLETION>")
end

local iteration = as_number(promise.iteration, 0)
local next_iteration = iteration + 1
local max_iterations = as_number(promise.max_iterations, 0)
local validator = promise.validator
if type(validator) ~= "string" or validator == "" then
  validator = "shrike"
end

state.write("thread", "promise.state", "validating")

local log = thread.log_summary() or {}
local tool_calls = thread.tool_calls({ since_turn = tonumber(promise.opened_turn or 0) or 0, limit = 80 }) or {}
local messages = thread.messages({ since_turn = tonumber(promise.opened_turn or 0) or 0, limit = 120 }) or {}
local final_message = payload.extra and payload.extra.final_message or log.final_message or ""
local stop_reason = payload.extra and payload.extra.stop_reason or "stop"
local completion_block = extract_completion_block(final_message)

if not completion_block then
  state.write("thread", "promise.state", "open")
  state.write("thread", "promise.last_verdict", "missing_completion_tag")
  state.write("thread", "promise.last_suggestion", "Stop denied because no PROMISE_COMPLETION block was present.")
  return outcome.block({
    reason = "promise completion tag missing",
    reminder = table.concat({
      "PROMISE STILL OPEN: " .. tostring(promise.id),
      "",
      "You tried to stop without a PROMISE_COMPLETION block.",
      "Resume the task.",
      "When the work is actually complete, end with:",
      "<PROMISE_COMPLETION>",
      "summary: what is done",
      "verification: exact commands/checks you ran",
      "evidence: concrete anchors or outputs",
      "</PROMISE_COMPLETION>",
    }, "\n")
  })
end

state.write("thread", "promise.iteration", next_iteration)
local meta_iteration = as_number(promise.meta_iteration, 1)

if max_iterations > 0 and next_iteration >= max_iterations then
  local next_meta = meta_iteration + 1
  state.write("thread", "promise.meta_iteration", next_meta)
  state.write("thread", "promise.iteration", 0)
  state.write("thread", "promise.state", "open")
  state.write("thread", "promise.last_verdict", "reset")
  state.write("thread", "promise.last_suggestion", "Promise loop reset after max iterations.")
  state.append("thread", "promise.history[]", {
    iteration = next_iteration,
    meta_iteration = meta_iteration,
    validator = "system",
    validator_agent_id = "",
    verdict = "reset",
    suggestion = "Promise loop reset after max iterations.",
    evidence = {},
  })

  agent.reset(agent_id)
  agent.execute(agent_id, table.concat({
    "PROMISE CONTRACT RESET: previous attempt loop exhausted.",
    "",
    "Meta-iteration: " .. tostring(next_meta),
    "Promise id: " .. tostring(promise.id),
    "",
    "You are starting fresh because you kept trying to stop without satisfying the promise.",
    "Do the work from scratch, reread what matters, and do not emit a completion block early.",
    "",
    "Promised task:",
    tostring(promise.task or promise.brief or ""),
    "",
    "Done when:",
    list_lines(promise.done_when, nil, 12),
  }, "\n"))

  return outcome.block({
    reason = "promise reset after max iterations",
    reminder = "Promise loop reset. Fresh run launched with higher meta-iteration."
  })
end

local task = table.concat({
  "You are Shrike, the promise validator for a Firmius agent.",
  "",
  "Validate whether the promised task is actually complete.",
  "You must not accept based on transcript vibes, partial edits, or a confident summary.",
  "If the agent did not actually read the codebase, inspect files, and run concrete verification when appropriate, reject.",
  "If tests or verification were expected but not run, reject.",
  "Prefer skepticism. The default is reject unless the evidence is concrete.",
  "Return exactly one JSON object and no prose:",
  [[{"verdict":{"kind":"accept"|"reject"},"suggestion":"short notes for the agent","evidence":[{"claim":"...","anchor":"..."}]}]],
  "",
  "Promise id: " .. tostring(promise.id),
  "Iteration: meta " .. tostring(meta_iteration) .. ", local " .. tostring(next_iteration) .. (max_iterations > 0 and ("/" .. tostring(max_iterations)) or ""),
  "Stop reason: " .. tostring(stop_reason),
  "",
  "Promised task:",
  tostring(promise.task or promise.brief or ""),
  "",
  "Done when:",
  list_lines(promise.done_when, nil, 12),
  "",
  "PROMISE_COMPLETION block from the agent:",
  trim(completion_block),
  "",
  "Agent final message:",
  one_line(final_message, "<blank>"),
  "",
  "Commands run:",
  list_lines(log.commands_run, nil, 20),
  "",
  "Files edited:",
  list_lines(log.files_edited, nil, 30),
  "",
  "Files read:",
  list_lines(log.files_read, nil, 30),
  "",
  "Tool calls:",
  list_lines(tool_calls, "name", 50),
  "",
  "Tool results:",
  list_lines(thread.tool_results({ since_turn = tonumber(promise.opened_turn or 0) or 0, limit = 50 }), "result", 50),
  "",
  "Transcript messages:",
  list_lines(messages, "text", 80),
}, "\n")

local result = agent.spawn(validator, task, { timeout_sec = 180 })
local verdict, suggestion, evidence = verdict_from(result)

state.append("thread", "promise.history[]", {
  iteration = next_iteration,
  meta_iteration = meta_iteration,
  validator = validator,
  validator_agent_id = result and result.agent_id or "",
  verdict = verdict,
  suggestion = suggestion,
  evidence = evidence or {},
})

if verdict == "accept" or verdict == "sealed" then
  state.write("thread", "promise.state", "sealed")
  state.write("thread", "promise.sealed_by", validator)
  state.write("thread", "promise.last_verdict", "accept")
  state.write("thread", "promise.last_suggestion", suggestion)
  return outcome.allow({
    text = "PROMISE SEALED: " .. tostring(promise.id) .. " accepted by " .. validator .. "."
  })
end

state.write("thread", "promise.state", "open")
state.write("thread", "promise.last_verdict", "reject")
state.write("thread", "promise.last_suggestion", suggestion)
local remaining = ""
if max_iterations > 0 then
  remaining = "\nIterations remaining before escalation: " .. tostring(math.max(0, max_iterations - next_iteration)) .. "."
end

return outcome.block({
  reason = "promise rejected by " .. validator,
  reminder = table.concat({
    "PROMISE DENIED: " .. tostring(promise.id),
    "Shrike rejected completion attempt #" .. tostring(next_iteration) .. ".",
    "Meta-iteration: " .. tostring(meta_iteration) .. ".",
    "",
    "Promised task:",
    tostring(promise.task or promise.brief or ""),
    "",
    "Shrike notes:",
    tostring(suggestion),
    remaining,
    "",
    "Resume the task. Do not summarize again until the promise is actually satisfied."
  }, "\n")
})
