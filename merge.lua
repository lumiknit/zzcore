#!/usr/bin/lua

readFile = function(path)
  local f = io.open(path, "rb")
  if f then
    local content = f:read("*a")
    f:close()
    return content
  end
end

writeFile = function(path, content)
  local f = io.open(path, "wb")
  if f then
    f:write(content)
    f:close()
  end
end

local h = readFile("./zzcore.h")
local c = readFile("./zzcore.c")
local result = string.gsub(c, "#include \"zzcore.h\"", h)
writeFile("zzcore_min.c", result)
