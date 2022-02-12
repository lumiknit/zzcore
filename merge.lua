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

replaceHeader = function(src, hd)
  return src:gsub("#include \"zzcore.h\"", hd)
end

removeComments = function(src)
  return src:gsub("/%*[^*]*%*/", " ")
            :gsub("//[^\n]*\n", "\n")
end

reduce = function(src)
  return src:gsub("\n%s*\n", "\n")
end

h = readFile("./zzcore.h")
c = readFile("./zzcore.c")
t = removeComments(replaceHeader(c, h))
t = reduce(t)
writeFile("zzcore_min.c", t)
