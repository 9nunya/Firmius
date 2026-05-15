local contract = state.read("thread", "proveit")
if type(contract) ~= "table" or contract.state ~= "open" or not contract.id then
  return outcome.allow({})
end

local payload = event.payload or {}
if contract.agent_id and contract.agent_id ~= "" and payload.agent_id ~= contract.agent_id then
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

local function compact_ledger(values, limit)
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
    if type(item) == "table" then
      local line = tostring(item.kind or "tool") .. ": " .. one_line(tostring(item.summary or ""), "<empty>")
      if item.success == false then
        line = line .. " [failed]"
      end
      table.insert(out, "  - " .. line)
    else
      table.insert(out, "  - " .. one_line(tostring(item), "<empty>"))
    end
  end
  if #out == 0 then
    return "  <none>"
  end
  return table.concat(out, "\n")
end

local function trim(value)
  if type(value) ~= "string" then
    return ""
  end
  return value:gsub("^%s+", ""):gsub("%s+$", "")
end

local function extract_claim_block(text)
  if type(text) ~= "string" then
    return nil
  end
  return text:match("<PROVEIT_CLAIM>%s*(.-)%s*</PROVEIT_CLAIM>")
end

local function verdict_from(result)
  if type(result) ~= "table" then
    return "reject", {
      { claim = "<missing>", problem = "Validator returned no result.", required_next_proof = "Restate the claim and answer with a proper rebuttal." }
    }, {}
  end

  local json = result.json
  local verdict = nil
  local rebuttal = nil
  local evidence = nil
  if type(json) == "table" then
    verdict = json.verdict
    if type(verdict) == "table" then
      verdict = verdict.kind
    end
    rebuttal = json.rebuttal
    evidence = json.evidence
  end

  if type(verdict) ~= "string" or verdict == "" then
    local text = string.lower(result.text or "")
    verdict = text:find("accept", 1, true) and "accept" or "reject"
  end

  if type(rebuttal) ~= "table" or #rebuttal == 0 then
    rebuttal = {
      {
        claim = "<unspecified>",
        problem = result.text or "Validator did not provide a structured rebuttal.",
        required_next_proof = "Answer the strongest missing point directly."
      }
    }
  end

  return string.lower(verdict), rebuttal, evidence or {}
end

local iteration = as_number(contract.iteration, 0)
local next_iteration = iteration + 1
local max_iterations = as_number(contract.max_iterations, 0)
local validator = contract.validator
if type(validator) ~= "string" or validator == "" then
  validator = "reviewer"
end

state.write("thread", "proveit.state", "validating")
state.write("thread", "proveit.iteration", next_iteration)

local log = thread.log_summary() or {}
local final_message = payload.extra and payload.extra.final_message or log.final_message or ""
local summary = contract.summary or {}
local evidence = contract.evidence or {}
local claim_block = extract_claim_block(final_message)

if not claim_block then
  state.write("thread", "proveit.state", "open")
  state.write("thread", "proveit.last_verdict", "missing_claim_block")
  state.write("thread", "proveit.last_suggestion", "Stop denied because no PROVEIT_CLAIM block was present.")
  return outcome.block({
    reason = "proveit claim block missing",
    reminder = table.concat({
      "PROVEIT STILL OPEN: " .. tostring(contract.id),
      "",
      "You tried to stop without a PROVEIT_CLAIM block.",
      "State the exact claim you proved and the compact proof for it.",
      "Use:",
      "<PROVEIT_CLAIM>",
      "claim: one sentence stating what is proven",
      "proof:",
      "- concise evidence item",
      "- concise evidence item",
      "limits:",
      "- what is not proven / remaining uncertainty",
      "</PROVEIT_CLAIM>",
    }, "\n")
  })
end

local task = table.concat({
  "You are Reviewer in PROVEIT mode.",
  "",
  "This is not a generic audit and not a promise check.",
  "Your role is adversarial rebuttal: attack the proof if it overclaims, but do not reject just because the agent did not dump raw transcript or tool logs.",
  "Judge the proof packet against the compact evidence summary.",
  "If you reject, you must argue with the claim by naming the exact weak point and the exact next proof needed.",
  "",
  "Return exactly one JSON object and no prose:",
  [[{"verdict":{"kind":"accept"|"reject"},"rebuttal":[{"claim":"...","problem":"...","required_next_proof":"..."}],"evidence":[{"claim":"...","anchor":"..."}]}]],
  "",
  "Accept when the claim is narrow and adequately supported by the compact evidence packet.",
  "Reject only when you can name a concrete overreach, contradiction, or missing proof step.",
  "Do not ask for full transcript archaeology. Do not demand raw tool results unless the compact packet itself is contradictory.",
  "",
  "Evidence contract id: " .. tostring(contract.id),
  "Iteration: " .. tostring(next_iteration) .. (max_iterations > 0 and ("/" .. tostring(max_iterations)) or ""),
  "",
  "Target task:",
  tostring(contract.task or contract.brief or ""),
  "",
  "Claim block from the agent:",
  trim(claim_block),
  "",
  "Compact evidence summary:",
  "  commands=" .. tostring(summary.commands or 0),
  "  reads=" .. tostring(summary.reads or 0),
  "  edits=" .. tostring(summary.edits or 0),
  "  tests=" .. tostring(summary.tests or 0),
  "  validations=" .. tostring(summary.validations or 0),
  "  failures=" .. tostring(summary.failures or 0),
  "",
  "Commands run:",
  list_lines(log.commands_run, nil, 20),
  "",
  "Files edited:",
  list_lines(log.files_edited, nil, 24),
  "",
  "Files read:",
  list_lines(log.files_read, nil, 24),
  "",
  "Evidence ledger:",
  compact_ledger(evidence, 24),
}, "\n")

local result = agent.spawn(validator, task, { timeout_sec = 180 })
local verdict, rebuttal, result_evidence = verdict_from(result)

state.append("thread", "proveit.history[]", {
  iteration = next_iteration,
  validator = validator,
  validator_agent_id = result and result.agent_id or "",
  verdict = verdict,
  rebuttal = rebuttal,
  evidence = result_evidence,
})

if verdict == "accept" or verdict == "sealed" then
  state.write("thread", "proveit.state", "sealed")
  state.write("thread", "proveit.last_verdict", "accept")
  state.write("thread", "proveit.last_suggestion", "Claim survived rebuttal.")
  return outcome.allow({
    text = "PROVEIT SEALED: " .. tostring(contract.id) .. " accepted by " .. validator .. "."
  })
end

state.write("thread", "proveit.state", "open")
state.write("thread", "proveit.last_verdict", "reject")
state.write("thread", "proveit.last_suggestion", "Answer the validator rebuttal directly.")

local rebuttal_lines = {}
for _, item in ipairs(rebuttal) do
  if type(item) == "table" then
    table.insert(rebuttal_lines, "- claim: " .. one_line(tostring(item.claim or "<unspecified>"), "<unspecified>"))
    table.insert(rebuttal_lines, "  problem: " .. one_line(tostring(item.problem or "<unspecified>"), "<unspecified>"))
    table.insert(rebuttal_lines, "  next proof: " .. one_line(tostring(item.required_next_proof or "<unspecified>"), "<unspecified>"))
  end
end
if #rebuttal_lines == 0 then
  table.insert(rebuttal_lines, "- claim: <unspecified>")
  table.insert(rebuttal_lines, "  problem: validator did not provide a structured rebuttal")
  table.insert(rebuttal_lines, "  next proof: answer the strongest missing point directly")
end

local remaining = ""
if max_iterations > 0 then
  remaining = "\nIterations remaining before escalation: " .. tostring(math.max(0, max_iterations - next_iteration)) .. "."
end

return outcome.block({
  reason = "proveit rejected by " .. validator,
  reminder = table.concat({
    "PROVEIT REBUTTED: " .. tostring(contract.id),
    "Witness rejected completion attempt #" .. tostring(next_iteration) .. ".",
    "",
    "Answer these rebuttals directly:",
    table.concat(rebuttal_lines, "\n"),
    remaining,
    "",
    "Do not restate the same story.",
    "Narrow the claim, add the missing proof, or overturn the rebuttal with concrete evidence.",
    "When ready, end with a fresh <PROVEIT_CLAIM> block.",
  }, "\n")
})
