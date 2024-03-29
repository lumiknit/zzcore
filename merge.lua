#!/usr/bin/lua

NAME = "zzcore_min.c"
VERSION = "0.0.1"
AUTHOR = "lumiknit"

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

replaceHeader = function(src, hd)
  return src:gsub("#include \"zzcore.h\"", hd)
end

removeComments = function(src)
  return src:gsub("/%*.-%*/", " ")
            :gsub("//.-\n", "\n")
end

reduce = function(src)
  return src:gsub("\n%s*\n", "\n")
end

addComments = function(src)
  hd = "// ----------------------\n"
  nm = "// -- " .. NAME .. " " .. VERSION .. "\n"
  au = "// -- authour: " .. AUTHOR .. "\n"
  tl = "\n// ----------------------"
  return hd .. nm .. au .. src .. tl
end

h = readFile("./zzcore.h")
c = readFile("./zzcore.c")
t = removeComments(replaceHeader(c, h))
t = reduce(t)
t = addComments(t)
writeFile("zzcore_min.c", t)
