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

state.write("thread", "promise.id", pact_id)
state.write("thread", "promise.brief", task)
state.write("thread", "promise.task", task)
state.write("thread", "promise.done_when", {
  "The requested task is complete and verified."
})
state.write("thread", "promise.validator", "shrike")
state.write("thread", "promise.iteration", 0)
state.write("thread", "promise.max_iterations", 5)
state.write("thread", "promise.state", "open")
state.write("thread", "promise.agent_id", agent_id)
state.write("thread", "promise.history", {})

return outcome.allow({ text = task, outcome = "promise_opened" })
