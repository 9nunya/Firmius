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

local function append_evidence(kind, summary, detail)
  state.append("thread", "proveit.evidence[]", {
    kind = kind,
    tool = tostring(payload.tool or ""),
    success = payload.tool_success == true,
    summary = summary,
    detail = detail or "",
  })
end

local tool = tostring(payload.tool or "")
local args = payload.tool_args or {}
local result = payload.tool_result or {}
local success = payload.tool_success == true

if tool == "Process" or tool == "process" then
  local command = tostring(args.command or "")
  local stdout = tostring(result.stdout or "")
  local stderr = tostring(result.stderr or "")
  local raw_result = tostring(result.result or "")
  local exit_code = tostring(result.exit_code or result.exitCode or "")
  local detail_lines = {}
  table.insert(detail_lines, "command=" .. (command ~= "" and command or "<process>"))
  if exit_code ~= "" then
    table.insert(detail_lines, "exit=" .. exit_code)
  end
  if stdout ~= "" then
    table.insert(detail_lines, "stdout=" .. stdout)
  elseif raw_result ~= "" then
    table.insert(detail_lines, "result=" .. raw_result)
  end
  if stderr ~= "" then
    table.insert(detail_lines, "stderr=" .. stderr)
  end
  append_evidence(
    "command",
    command ~= "" and command or "<process>",
    table.concat(detail_lines, "\n")
  )
  state.write("thread", "proveit.summary.commands", as_number(contract.summary and contract.summary.commands, 0) + 1)
  local lower = string.lower(command)
  if lower:find("test", 1, true) or lower:find("ctest", 1, true) or lower:find("pytest", 1, true) or lower:find("cargo test", 1, true) then
    state.write("thread", "proveit.summary.tests", as_number(contract.summary and contract.summary.tests, 0) + 1)
  end
elseif tool == "Files" or tool == "file_read" or tool == "Read" then
  local path = tostring(args.path or "")
  append_evidence("read", path ~= "" and path or "<read>", "path=" .. (path ~= "" and path or "<read>"))
  state.write("thread", "proveit.summary.reads", as_number(contract.summary and contract.summary.reads, 0) + 1)
elseif tool == "Edit" or tool == "EditWrite" or tool == "EditReplace" or tool == "EditRange" or tool == "file_edit" or tool == "file_write" then
  local path = tostring(args.path or "")
  append_evidence("edit", path ~= "" and path or "<edit>", "path=" .. (path ~= "" and path or "<edit>"))
  state.write("thread", "proveit.summary.edits", as_number(contract.summary and contract.summary.edits, 0) + 1)
elseif tool == "LspDiagnostics" or tool == "LspQuery" or tool == "Work" then
  append_evidence("validation", tool, "tool=" .. tool)
  state.write("thread", "proveit.summary.validations", as_number(contract.summary and contract.summary.validations, 0) + 1)
else
  append_evidence("tool", tool, "tool=" .. tool)
end

if not success then
  state.write("thread", "proveit.summary.failures", as_number(contract.summary and contract.summary.failures, 0) + 1)
end

return outcome.allow({})
