local payload = event.payload or {}
local extra = payload.extra or {}
local task = tostring(extra.raw_args or "")

local function trim(value)
  return value:gsub("^%s+", ""):gsub("%s+$", "")
end

task = trim(task)
if task == "" then
  return outcome.block({ reason = "usage: /proveit <task>" })
end

local agent_id = tostring(payload.agent_id or "")
local thread_id = tostring(payload.thread_id or "")
local contract_id = "proveit-" .. (agent_id ~= "" and agent_id or thread_id)

local proveit_meta_prompt = table.concat({
  "PROVEIT CONTRACT ACTIVE: " .. contract_id,
  "",
  "PROVEIT is not a promise clone. It is an argument over proof.",
  "Your job is to make a claim and support it well enough to survive rebuttal.",
  "The validator is allowed to attack your proof. If it rejects, answer the rebuttal directly.",
  "",
  "What PROVEIT means:",
  "- You are trying to prove a specific claim about the repo or runtime.",
  "- Strong proof is compact, specific, and grounded in actual reads, edits, commands, tests, or outputs.",
  "- Do not dump your whole history. Build a case.",
  "",
  "How to finish a PROVEIT run:",
  "- Your final assistant message must include exactly one claim block.",
  "- Use this exact format:",
  "<PROVEIT_CLAIM>",
  "claim: one sentence stating what is proven",
  "proof:",
  "- concise evidence item",
  "- concise evidence item",
  "limits:",
  "- what is not proven / remaining uncertainty",
  "</PROVEIT_CLAIM>",
  "",
  "How to win:",
  "- Make the claim narrow enough to prove.",
  "- Use concrete repo/runtime evidence.",
  "- If rebutted, answer the rebuttal instead of restating the same story.",
  "",
  "Failure modes to avoid:",
  "- Vague triumph language with no exact claim.",
  "- Citing anchors or commands you did not actually produce.",
  "- Over-claiming beyond the evidence ledger.",
}, "\n")

if task == "cancel!" then
  local contract = state.read("thread", "proveit")
  if type(contract) ~= "table" or not contract.id then
    return outcome.block({ reason = "No open proveit contract to cancel." })
  end
  if tostring(contract.agent_id or "") ~= "" and tostring(contract.agent_id or "") ~= agent_id then
    return outcome.block({ reason = "Open proveit contract belongs to a different agent." })
  end

  agent.cancel(agent_id)
  state.delete("thread", "proveit")

  return outcome.allow({
    text = "PROVEIT CANCELLED: " .. tostring(contract.id),
    outcome = "proveit_cancelled"
  })
end

state.write("thread", "proveit.id", contract_id)
state.write("thread", "proveit.brief", task)
state.write("thread", "proveit.task", task)
state.write("thread", "proveit.validator", "reviewer")
state.write("thread", "proveit.state", "open")
state.write("thread", "proveit.iteration", 0)
state.write("thread", "proveit.max_iterations", 4)
state.write("thread", "proveit.agent_id", agent_id)
state.write("thread", "proveit.last_verdict", "collecting")
state.write("thread", "proveit.last_suggestion", "")
state.write("thread", "proveit.summary", {
  commands = 0,
  reads = 0,
  edits = 0,
  tests = 0,
  failures = 0,
  validations = 0,
})
state.write("thread", "proveit.evidence", {})
state.write("thread", "proveit.history", {})

return outcome.allow({
  text = table.concat({
    proveit_meta_prompt,
    "",
    "TARGET CLAIM TO PROVE:",
    task,
  }, "\n"),
  outcome = "proveit_opened"
})
