local payload = event.payload or {}
local extra = payload.extra or {}
local task = tostring(extra.raw_args or "")

local function trim(value)
  return value:gsub("^%s+", ""):gsub("%s+$", "")
end

task = trim(task)
if task == "" then
  return outcome.block({ reason = "usage: /promise <task>" })
end

local agent_id = tostring(payload.agent_id or "")
local thread_id = tostring(payload.thread_id or "")
local pact_id = "promise-" .. (agent_id ~= "" and agent_id or thread_id)

local promise_meta_prompt = table.concat({
  "PROMISE CONTRACT ACTIVE: " .. pact_id,
  "",
  "You are bound to a promise. Do not stop with a casual progress note.",
  "A promise means your stop attempt will be checked externally.",
  "If you stop early, you will be denied and sent back to work.",
  "",
  "How promises work:",
  "- The promise stays open until the task is actually complete and verified.",
  "- Reads, edits, commands, tests, and tool results matter more than your prose.",
  "- A vague final message is not completion.",
  "",
  "How to resolve the promise:",
  "- When the task is truly done, your final assistant message must include exactly one completion block.",
  "- Use this exact format:",
  "<PROMISE_COMPLETION>",
  "summary: what is done",
  "verification: exact commands/checks you ran",
  "evidence: concrete anchors or outputs",
  "</PROMISE_COMPLETION>",
  "- Do not emit that block until the work is genuinely complete.",
  "",
  "Failure modes to avoid:",
  "- Saying 'I'll read these files now' and then stopping.",
  "- Claiming completion without tests or verification.",
  "- Writing a completion block before the repo and tool evidence support it.",
}, "\n")

if task == "cancel!" then
  local promise = state.read("thread", "promise")
  if type(promise) ~= "table" or not promise.id then
    return outcome.block({ reason = "No open promise to cancel." })
  end
  if tostring(promise.agent_id or "") ~= "" and tostring(promise.agent_id or "") ~= agent_id then
    return outcome.block({ reason = "Open promise belongs to a different agent." })
  end

  agent.cancel(agent_id)
  state.delete("thread", "promise")

  return outcome.allow({
    text = "PROMISE CANCELLED: " .. tostring(promise.id),
    outcome = "promise_cancelled"
  })
end

state.write("thread", "promise.id", pact_id)
state.write("thread", "promise.brief", task)
state.write("thread", "promise.task", task)
state.write("thread", "promise.done_when", {
  "The requested task is complete and verified."
})
state.write("thread", "promise.validator", "shrike")
state.write("thread", "promise.iteration", 0)
state.write("thread", "promise.meta_iteration", 1)
state.write("thread", "promise.max_iterations", 5)
state.write("thread", "promise.state", "open")
state.write("thread", "promise.agent_id", agent_id)
state.write("thread", "promise.last_verdict", "open")
state.write("thread", "promise.last_suggestion", "")
state.write("thread", "promise.sealed_by", "")
state.write("thread", "promise.opened_turn", 0)
state.write("thread", "promise.history", {})

return outcome.allow({
  text = table.concat({
    promise_meta_prompt,
    "",
    "PROMISED TASK:",
    task,
  }, "\n"),
  outcome = "promise_opened"
})
