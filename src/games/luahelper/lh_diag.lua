-- Tools of Destruction's console menu (revision 1.29): what the
-- glue saw, for qwark.log. docs/tod-menu.md 11.
--
-- qwark sends this once, as a request for the pause menu's VM (target MENU,
-- like the glue itself), when the glue's note says a look failed: it found
-- no pause list in its first 30 looks (LM_NOTE_NONE), or the Trainer
-- waited on Game Options' slide and no Game Options behaviour turned up
-- (LM_NOTE_NOCH; revision 1.29's fourth console test moved the Trainer's
-- pages from the Cheats screen to Game Options, docs/tod-menu.md 12). Its answer is one line of text, the request's first
-- result, which qwark logs ("menu: what the glue saw: ..."):
--
--   g=      the glue's state (lh_glue.lua's T, global qwark_state):
--           tries, age, ready (1/0), on (1/0), the Inventory rows it found
--           (how many x the first one's row, or -), the frames left to wait for Game Options,
--           and its take-over (1 on, w waiting, 0 off)
--   s=      the type of scene, Presentation, Presentation.scene, Node and
--           Node.goToSlide, Node._useOnActivateDeactivate, self: one
--           letter each (u userdata, t table, f function, n nil, s string,
--           b boolean, # number)
--   sl=     scene.currentSlide (a field ListUpdater's onActivate sets)
--   reg=    the registry (scene._table_ActDeactRegisters): its type, then
--           how many keys, how many of them userdata, and how many active
--   then each key, at most 24: its element name (cut to 14) and what it
--           holds: m menuData, i onInventory, o optArray (Game Options), c
--           changeCheatState (the Cheats screen), + active (the
--           registry's value)
--   lu=     the first key with menuData: OPTIONS_MENU_INDEX, its
--           currentMenuIndex and currentSlide, then each menu
--           [index:rows i<row where onInventory is> t<row whose tag is 36, Inventory's>
--           T<row that says TRAINER>]
--   err=    anything that raised an error on the way
--
-- Every Anark object is touched only if it is a userdata, and only through
-- pcall, as the glue does. The text it builds is garbage once it is
-- returned; the helper runs this only with a load's room in the pool and
-- collects after it if it left the pool short.

local o, n = {}, 0
local T = qwark_state

local function add(s)
  n = n + 1
  o[n] = tostring(s)
end

local function index(x, k)
  return x[k]
end

local function get(x, k)
  if type(x) ~= "userdata" then return nil end
  local ok, v = pcall(index, x, k)
  if ok then return v end
  return nil
end

local function ty(x)
  local t = type(x)
  if t == "number" then return "#" end
  return string.sub(t, 1, 1)
end

local function yes(x)
  if x then return "1" end
  return "0"
end

local function main()
  if type(T) == "table" then
    local hr = "-"
    if type(T.hr) == "table" and T.hr[1] then hr = table.getn(T.hr) .. "x" .. tostring(T.hr[1][2]) end
    local om = "0"
    if T.om == true then om = "1" elseif T.om then om = "w" end
    add("g=" .. tostring(T.tries) .. "," .. tostring(T.age) .. "," .. yes(T.ready) .. "," .. yes(T.on) .. "," .. hr .. "," .. tostring(T.ow) .. "," .. om)
  else
    add("g=" .. ty(T))
  end
  local S = scene
  local N = Node
  local go, uo
  if type(N) == "table" then go, uo = N.goToSlide, N._useOnActivateDeactivate end
  add(" s=" .. ty(S) .. ty(Presentation) .. ty(get(Presentation, "scene")) .. ty(N) .. ty(go) .. ty(uo) .. ty(self))
  if type(S) ~= "userdata" then S = get(Presentation, "scene") end
  add(" sl=" .. tostring(get(S, "currentSlide")))
  local reg = get(S, "_table_ActDeactRegisters")
  add(" reg=" .. ty(reg))
  if type(reg) ~= "table" then return end
  local c, u, a, lu = 0, 0, 0, nil
  local keys = {}
  for k, v in pairs(reg) do
    c = c + 1
    if type(k) == "userdata" then u = u + 1 end
    if v then a = a + 1 end
    local f = ""
    if type(get(k, "menuData")) == "table" then
      f = f .. "m"
      lu = lu or k
    end
    if get(k, "onInventory") then f = f .. "i" end
    if type(get(k, "optArray")) == "table" then f = f .. "o" end
    if get(k, "changeCheatState") then f = f .. "c" end
    if v then f = f .. "+" end
    if c <= 24 then
      local nm = get(k, "name")
      if type(nm) ~= "string" then nm = ty(k) end
      keys[c] = " " .. string.sub(nm, 1, 14) .. ":" .. f
    end
  end
  add("," .. c .. "," .. u .. "," .. a)
  for i = 1, math.min(c, 24) do add(keys[i]) end
  if not lu then return end
  local md, h = get(lu, "menuData"), get(lu, "onInventory")
  add(" lu=" .. tostring(get(lu, "OPTIONS_MENU_INDEX")) .. "," .. tostring(get(lu, "currentMenuIndex")) .. "," .. tostring(get(lu, "currentSlide")))
  for m, rows in pairs(md) do
    if type(rows) == "table" then
      local s = " [" .. tostring(m) .. ":" .. table.getn(rows)
      for r, it in pairs(rows) do
        if type(it) == "table" then
          if h and it[2] == h then s = s .. " i" .. tostring(r) end
          if it[1] == 36 then s = s .. " t" .. tostring(r) end
          if it[1] == "TRAINER" then s = s .. " T" .. tostring(r) end
        end
      end
      add(s .. "]")
    end
  end
end

local ok, e = pcall(main)
if not ok then add(" err=" .. tostring(e)) end
return table.concat(o)
