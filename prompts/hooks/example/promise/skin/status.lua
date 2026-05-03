local promise = state.read("thread", "promise")
if type(promise) ~= "table" or not promise.id then
  return ""
end

local function as_number(value, fallback)
  local n = tonumber(value)
  if n == nil then
    return fallback
  end
  return n
end

local state_label = tostring(promise.state or "open")
local iter = as_number(promise.iteration, 0)
local max = as_number(promise.max_iterations, 0)
local validator = tostring(promise.validator or "shrike")
local brief = tostring(promise.brief or promise.task or "")
if #brief > 80 then
  brief = brief:sub(1, 80) .. "..."
end

local counter = "#" .. tostring(iter)
if max > 0 then
  counter = counter .. "/" .. tostring(max)
end

return "promise " .. state_label .. " " .. counter .. " validator=" .. validator .. " pact=" .. tostring(promise.id) .. " " .. brief
