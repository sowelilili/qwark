-- Tools of Destruction's Trainer (revision 1.29, pending): the Lua glue that
-- makes the galactic map qwark opens offer every planet. docs/tod-menu.md 17
-- has the design.
--
-- qwark opens the map itself (gmap() sent to the gameplay VM, from the
-- Trainer's Planets row) and sends this file to the map's own Anark VM, a
-- request of target MAP, which the Lua helper serves from the map's frame
-- (luahelper.c, lh_map_tick), with the map's presentation pushed and only
-- once the game's own words say it is up. The ship's own map never gets it.
-- The map's VM is made when the map opens and goes when it closes, so all
-- this changes goes with it.
--
-- The map's handler (gmapHndler) lists the levels of its levelArray in the
-- sectors of its sectorArray that the save allows (is_level_visible,
-- is_sector_visible, is_level_visitable, all GalMap_isLevelVisitable, which
-- never passes Metropolis, and Fastoon only while Fastoon II does not), and
-- a pick is load_level(level) (GalMap_LoadLevel), which asks the same again
-- and maps Fastoon and Fastoon II to whichever the save is at. Here:
--
--   * Kerwan (Metropolis) goes first into its list, under its own galaxy's
--     header (SECTOR_SOLANA, "Solana Galaxy"); the map has no view of it (the
--     game itself shows the whole map in Metropolis, GalMap_setupView), so
--     its row and its header show the whole map (set_global_view);
--   * every level is listed and can be picked, and Fastoon II is a row of its
--     own beside Fastoon, its name with " II" after it;
--   * a pick is qw(10, level) (lh_menu.c lm_pick: the level into
--     g_galMap.selectedLevel, where the game's own pick leaves it) and the
--     game's own close, exit_galactic_map(): GalMapState_exit loads it, the
--     game's own load, whatever the save says of the level.
--
-- The list is the game's own (initPlanetData run again with the two arrays
-- longer and the save's tests answered yes), and the cursor goes back to the
-- row it was on. It answers "in" and the rows' count, or "wait" while the
-- handler or its list is not up yet (qwark sends it again), or why not.
--
-- Lua 5.0, and the same build rules as lh_glue.lua: comments only on lines
-- of their own, no string literal with a space.

local Q = qw

local function index(o, k)
  return o[k]
end

local function assign(o, k, v)
  o[k] = v
end

-- A field of an Anark object or a table, nil for one it does not have, never an error.
local function get(o, k)
  if type(o) == "table" then return o[k] end
  if type(o) ~= "userdata" then return nil end
  local ok, v = pcall(index, o, k)
  if ok then return v end
  return nil
end

-- The map's handler: the behaviour in the registry with the map's two arrays and its list.
local function handler()
  local s = scene
  if type(s) ~= "userdata" then s = get(Presentation, "scene") end
  local reg = get(s, "_table_ActDeactRegisters")
  if type(reg) ~= "table" then return nil end
  for n in pairs(reg) do
    if type(n) == "userdata" and type(get(n, "levelArray")) == "table" and type(get(n, "sectorArray")) == "table" and type(get(n, "initPlanetData")) == "function" then
      return n
    end
  end
  return nil
end

-- The row of planetArray `a` that is `e` (a level, or a sector's header with `sec`), or nil.
local function find(a, e, sec)
  if type(a) ~= "table" or e == nil then return nil end
  for i, d in pairs(a) do
    if type(d) == "table" and d.enum == e and (d.isSector == true) == sec then return i end
  end
  return nil
end

local function main()
  local H = handler()
  if not H then return "wait" end
  if get(H, "qwark") then return "in" end
  local lb = get(H, "listBehavior")
  if type(get(lb, "rows")) ~= "table" or get(H, "inListMode") == nil then return "wait" end
  local lv, sv, gv, ln, ex = set_level_view, set_sector_view, set_global_view, get_location_name, exit_galactic_map
  local v1, v2, v3, ll = is_level_visible, is_level_visitable, is_sector_visible, load_level
  local init, la, sa = get(H, "initPlanetData"), get(H, "levelArray"), get(H, "sectorArray")
  if type(Q) ~= "function" or type(lv) ~= "function" or type(sv) ~= "function" or type(gv) ~= "function" or type(ln) ~= "function" or type(ex) ~= "function" then return "nofn" end
  local K, SO, F2 = LEVEL_METROPOLIS, SECTOR_SOLANA, LEVEL_FASTOON_II
  if type(K) ~= "number" or type(SO) ~= "number" or type(F2) ~= "number" then return "noenum" end

  -- Where the cursor and the flashing row are, by what they are: their indices move.
  local pa, cur, np = get(H, "planetArray"), get(H, "currentlySelected"), get(H, "newPlanet")
  local ce, cs, ne = nil, false, nil
  if type(pa) == "table" and pa[cur] then ce, cs = pa[cur].enum, pa[cur].isSector == true end
  if type(pa) == "table" and pa[np] then ne = pa[np].enum end

  -- The globals the map's own Lua calls by name, in this VM alone.
  set_level_view = function(l) if l == K then gv() else lv(l) end end
  set_sector_view = function(s) if s == SO then gv() else sv(s) end end
  is_level_visible = function(l) return true end
  is_level_visitable = function(l) return true end
  is_sector_visible = function(s) return true end
  get_location_name = function(l)
    local t = ln(l)
    if l == F2 and type(t) == "string" then t = t .. string.char(32) .. "II" end
    return t
  end
  load_level = function(l)
    Q(10, l)
    ex()
  end

  -- Kerwan and its galaxy first, then the game's own list made again.
  table.insert(la, 1, K)
  table.insert(sa, 1, SO)
  pcall(assign, H, "qwark", true)
  local ok, e = pcall(init, H)
  pa = get(H, "planetArray")
  if not ok or type(pa) ~= "table" then
    -- Not made: the game's own list and globals again, as the map had them.
    table.remove(la, 1)
    table.remove(sa, 1)
    set_level_view, set_sector_view, get_location_name, is_level_visible, is_level_visitable, is_sector_visible, load_level = lv, sv, ln, v1, v2, v3, ll
    pcall(init, H)
    return "error:" .. tostring(e)
  end
  np = find(pa, ne, false) or get(H, "newPlanet")
  pcall(assign, H, "newPlanet", np)
  cur = find(pa, ce, cs) or np
  pcall(assign, H, "currentlySelected", cur)
  local f = get(lb, "setSelectedIndex")
  if type(f) == "function" then pcall(f, lb, cur, false, false) end
  return "in" .. table.getn(pa)
end

local ok, r = pcall(main)
if ok then return r end
return "error:" .. tostring(r)
