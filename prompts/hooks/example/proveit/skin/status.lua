local contract = state.read("thread", "proveit")
if type(contract) ~= "table" or not contract.id then
  return ""
end

local function as_number(value, fallback)
  local n = tonumber(value)
  if n == nil then
    return fallback
  end
  return n
end

local summary = contract.summary or {}
local brief = tostring(contract.brief or contract.task or "")
if #brief > 72 then
  brief = brief:sub(1, 72) .. "..."
end

local state_label = tostring(contract.state or "open")
local iter = as_number(contract.iteration, 0)
local max = as_number(contract.max_iterations, 0)
local counter = "#" .. tostring(iter)
if max > 0 then
  counter = counter .. "/" .. tostring(max)
end

return ">:( PROVE IT! " .. state_label:upper() .. " " .. counter ..
  "  " .. tostring(summary.commands or 0) ..
  " 󰷊 " .. tostring(summary.reads or 0) ..
  "  " .. tostring(summary.edits or 0) ..
  " 󰙨 " .. tostring(summary.tests or 0)
