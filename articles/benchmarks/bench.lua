-- the language comparison: fib(30), a sieve below 2,000,000, 200,000 string keys
local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
local function sieve()
  local n, s, c = 2000000, {}, 0
  for i = 2, n do s[i] = true end
  for i = 2, n do
    if s[i] then
      c = c + 1
      for j = i * i, n, i do s[j] = false end
    end
  end
  return c
end
local function dict()
  local n, d, s = 200000, {}, 0
  for i = 1, n do d["k" .. i] = i * 2 end
  for i = 1, n do s = s + d["k" .. i] end
  return s
end
-- os.clock counts centiseconds in the box: each sample is ten runs
local function best(name, f)
  local b, r = math.huge
  for _ = 1, 3 do
    local t = os.clock()
    for _ = 1, 10 do r = f() end
    b = math.min(b, (os.clock() - t) * 100)
  end
  print(string.format("lua %s %.1f %s", name, b, tostring(r)))
end
best("fib", function() return fib(30) end)
best("sieve", sieve)
best("dict", dict)
