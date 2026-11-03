-- Tools of Destruction's Trainer (revision 1.29, pending): the galactic map's
-- glue (src/games/luahelper/lh_gmap.lua), run by a real Lua against a map
-- made of stand-ins (test/test_glue.py, docs/tod-menu.md 17).
--
-- The stand-ins are what the glue meets in the map's own VM, as the
-- decompiled map (built/anark/galacticmap/built.dat, chunk gmapHndler) and
-- the executable (GalMap_*, tolua_galacticmap_*) have them:
--
--   gmapHndler      the map's handler, its methods after the game's own:
--                   onInitialize's two arrays, initPlanetData, isSelectable,
--                   handleSelect (zoom, then load_level), handleBack,
--                   onSelectedItemChanged (the views, the panel's texts),
--                   setListItemValue; joined to the registry in its
--                   onInitialize (_useOnActivateDeactivate);
--   ScrollableList  its list: rows, setSelectedIndex, moveSelection over
--                   the rows isSelectable passes, updateAllItems;
--   the bindings    tolua's, an argument count they do not want an error;
--                   the save's tests as GalMap_isLevelVisitable makes them
--                   (unlocked, not hidden, not Metropolis, Fastoon only while
--                   Fastoon II is not), get_location_name and the panel's
--                   getters mapping Fastoon to Fastoon II once it is
--                   visitable, the views noted (a level view of Metropolis,
--                   or a sector view of Solana, the map has no camera for),
--                   load_level as GalMap_LoadLevel (visitable, then the
--                   space combat redirect and the Fastoon rule);
--   qw              lh_menu.c's LM_OP_PICK: the level noted.
--
-- GMAP is the glue's bytes (the source, or as qwark sends them), NAME the
-- run's name.

table.getn = table.getn or function(t) return #t end
local loadstr = loadstring or load

local failures, checks = 0, 0
local function check(c, what)
  checks = checks + 1
  if c then
    print("PASS " .. what)
  else
    failures = failures + 1
    print("FAIL " .. what)
  end
end

local W

local Node = { _useOnActivateDeactivate = function(self) scene._table_ActDeactRegisters[self] = false end,
               goToSlide = function(self, n) W.slide = n end }

local function mk(fields)
  local f = fields or {}
  local mt = {}
  mt.__index = function(o, k)
    local v = f[k]
    if v == nil then v = Node[k] end
    return v
  end
  mt.__newindex = function(o, k, v) f[k] = v end
  return newud(mt), f
end

local function tolua(n, f)
  return function(...)
    if select("#", ...) ~= n then error("#ferror in function", 0) end
    return f(...)
  end
end

local LOC = { [0] = "Planet Kerwan", "Planet Cobalia", "Planet Kortog", "Planet Fastoon", "Voron Asteroid Belt",
              "Planet Mukow", "Nundac Asteroid Ring", "Planet Ardolis", "Rakar Star Cluster", "Rykan V",
              "Planet Sargasso", "Kreeli Comet", "Planet Viceron", "Verdigris Black Hole", "Planet Jasindu",
              "Ublik Passage", "Planet Reepor", "Planet Igliak", "Planet Fastoon " }
-- Fastoon II's name ends in a space, as the console showed ("Fastoon  II" before the glue trimmed it).
local SECTOR = { [0] = 0, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 1 }
local SECNAME = { [0] = "Solana Galaxy", "Cerullean Sector", "Verdigris Sector", "Praxus Sector" }

-- The save: unlocked levels, hidden levels, visited levels (the space combat redirect).
local function visitable(l)
  if l == 3 and visitable(18) then return false end
  return W.unlocked[l] == true and not W.hidden[l] and l ~= 0
end

-- The panel's getters map Fastoon to Fastoon II once Fastoon II is visitable (FUN_00036d38 and the others).
local function fastoon(l)
  if l == 3 and visitable(18) then return 18 end
  return l
end

local function bindings()
  LEVEL_METROPOLIS, LEVEL_COBALIA, LEVEL_STRATUS_CITY, LEVEL_FASTOON = 0, 1, 2, 3
  LEVEL_SPACE_COMBAT_I, LEVEL_IMPERIAL_FIGHT_FEST, LEVEL_APOGEE_SPACE_STATION, LEVEL_PIRATE_BASE = 4, 5, 6, 7
  LEVEL_SPACE_COMBAT_II, LEVEL_RYKAN_V, LEVEL_SARGASSO, LEVEL_IRIS, LEVEL_ZORDOOM_PRISON = 8, 9, 10, 11, 12
  LEVEL_SPACE_COMBAT_III, LEVEL_KERCHU_CITY, LEVEL_SLAGS_FLEET, LEVEL_CRAGMITE_RUINS = 13, 14, 15, 16
  LEVEL_MERIDIAN_CITY, LEVEL_FASTOON_II, LEVEL_COUNT = 17, 18, 19
  SECTOR_SOLANA, SECTOR_CERULEAN, SECTOR_VERDIGRIS, SECTOR_PRAXUS, SECTOR_COUNT = 0, 1, 2, 3, 4
  OBJ_TREASURE_MAPPER = 6
  is_level_visible = tolua(1, function(l) return visitable(l) end)
  is_level_visitable = tolua(1, function(l) return visitable(l) end)
  is_sector_visible = tolua(1, function(s)
    for l = 0, 18 do if SECTOR[l] == s and visitable(l) then return true end end
    return false
  end)
  get_level_sector = tolua(1, function(l) return SECTOR[l] end)
  get_sector_name = tolua(1, function(s) return SECNAME[s] end)
  get_location_name = tolua(1, function(l) return LOC[fastoon(l)] end)
  get_level_name = tolua(1, function(l) return "level" .. fastoon(l) end)
  get_level_description = tolua(1, function(l) return "desc" .. fastoon(l) end)
  get_special_bolts_collected = tolua(1, function(l) return 0 end)
  get_special_bolts_total = tolua(1, function(l) return 1 end)
  get_level_missions_total = tolua(1, function(l) return 2 end)
  get_level_missions_completed = tolua(1, function(l) return 0 end)
  get_next_level = tolua(0, function() return W.next end)
  get_level_image = tolua(1, function(l) return "img" .. l end)
  get_num_objects = tolua(1, function(k) return 0 end)
  is_level_seen = tolua(1, function(l) return true end)
  set_global_view = tolua(0, function() W.view = "global" end)
  set_sector_view = tolua(1, function(s)
    W.view = "sector" .. s
    if s == 0 then W.bad = (W.bad or 0) + 1 end
  end)
  set_level_view = tolua(1, function(l)
    W.view = "level" .. l
    if l == 0 then W.bad = (W.bad or 0) + 1 end
  end)
  set_highlight_level = tolua(1, function(l) W.hl = l end)
  exit_galactic_map = tolua(0, function() W.exits = W.exits + 1 end)
  -- GalMap_LoadLevel: visitable, then GalMap_selectLevel's redirects.
  load_level = tolua(1, function(l)
    if not visitable(l) then return end
    local sc = ({ [5] = 4, [9] = 8, [14] = 13 })[l]
    if sc and not W.visited[sc] then l = sc end
    if l == 3 or l == 18 then l = W.unlocked[18] and 18 or 3 end
    W.gamepick = l
    W.exits = W.exits + 1
  end)
  EffectEvent = tolua(2, function(e, k) end)
  EVENT_GAINED_FOCUS, CLASS_HIGHLIGHT_BOX = 1, 2
  PlayImageMovie = function(t, i) end
  PauseImageMovie = function(t, b) end
  ak = { output = function(s) end }
  qw = function(op, a)
    if op == 10 then W.picks[table.getn(W.picks) + 1] = a end
  end
end

-- The map's list (ScrollableList, after the game's chunk, as far as the handler uses it).
local function list()
  local lb, f = mk({ rows = {}, selectedItemIndex = 1, hasFocus = true })
  for i = 1, 8 do f.rows[i] = (mk({ itemName = (mk({ textString = "", textColor = { setColor = function() end } })) })) end
  f.setSelectedIndex = function(self, index, isReset, dontCall)
    local n = self.listUpdater:getNumListItems()
    if index < 1 then index = 1 elseif index > n then index = n end
    self.selectedItemIndex = index
    if not dontCall then self.listUpdater:onSelectedItemChanged(index, self.rows[1], isReset == true) end
    self:updateAllItems()
  end
  f.updateAllItems = function(self)
    local n = self.listUpdater:getNumListItems()
    for i = 1, 8 do
      local k = self.selectedItemIndex + i - 1
      if k <= n then self.listUpdater:setListItemValue(k, self.rows[i]) else self.listUpdater:clearListItemValue(k, self.rows[i]) end
    end
  end
  f.moveSelection = function(self, d)
    local n, j = self.listUpdater:getNumListItems(), self.selectedItemIndex
    repeat
      j = j + d
      if j < 1 or j > n then return end
    until self.listUpdater:isSelectable(j)
    self.selectedItemIndex = j
    self.listUpdater:onSelectedItemChanged(j, self.rows[1], false)
  end
  return lb, f
end

-- gmapHndler, after the game's chunk.
local function handler(lb)
  local h, f = mk({ listBehavior = lb })
  f.onInitialize = function(self)
    self:_useOnActivateDeactivate()
    self.newPlanet = 2
    self.levelArray = { LEVEL_COBALIA, LEVEL_STRATUS_CITY, LEVEL_FASTOON, LEVEL_FASTOON_II, LEVEL_SPACE_COMBAT_I,
                        LEVEL_IMPERIAL_FIGHT_FEST, LEVEL_APOGEE_SPACE_STATION, LEVEL_PIRATE_BASE, LEVEL_SPACE_COMBAT_II,
                        LEVEL_RYKAN_V, LEVEL_SARGASSO, LEVEL_IRIS, LEVEL_ZORDOOM_PRISON, LEVEL_SPACE_COMBAT_III,
                        LEVEL_KERCHU_CITY, LEVEL_SLAGS_FLEET, LEVEL_CRAGMITE_RUINS, LEVEL_MERIDIAN_CITY }
    self.sectorArray = { SECTOR_CERULEAN, SECTOR_VERDIGRIS, SECTOR_PRAXUS }
    self:initPlanetData()
  end
  f.onActivate = function(self)
    self.inListMode = true
    self.currentlySelected = self.newPlanet
    self.listBehavior:setSelectedIndex(self.newPlanet, false, false)
  end
  f.initPlanetData = function(self)
    self.planetArray = {}
    for i = 1, table.getn(self.sectorArray) do
      if is_sector_visible(self.sectorArray[i]) then
        table.insert(self.planetArray, { enum = self.sectorArray[i], isSector = true,
                                         label = get_sector_name(self.sectorArray[i]) })
        for j = 1, table.getn(self.levelArray) do
          if get_level_sector(self.levelArray[j]) == self.sectorArray[i] and is_level_visible(self.levelArray[j]) then
            table.insert(self.planetArray, { enum = self.levelArray[j], isSector = false,
                                             sector = get_sector_name(get_level_sector(self.levelArray[j])),
                                             label = get_location_name(self.levelArray[j]),
                                             desc = get_level_description(self.levelArray[j]),
                                             boltsCollected = get_special_bolts_collected(self.levelArray[j]),
                                             boltsTotal = get_special_bolts_total(self.levelArray[j]) })
          end
        end
      end
    end
    for i = 1, table.getn(self.planetArray) do
      if not self.planetArray[i].isSector and self.planetArray[i].enum == get_next_level() then self.newPlanet = i end
    end
  end
  f.isSelectable = function(self, i)
    if self.planetArray[i].isSector then return false end
    if not is_level_visitable(self.planetArray[i].enum) then return false end
    return true
  end
  f.getNumListItems = function(self) return table.getn(self.planetArray) end
  f.setListItemValue = function(self, i, item) item.itemName.textString = self.planetArray[i].label end
  f.clearListItemValue = function(self, i, item) item.itemName.textString = "" end
  f.handleSelect = function(self)
    if self.inListMode then
      self.inListMode = false
      set_level_view(self.planetArray[self.currentlySelected].enum)
      self.parent:goToSlide(3)
    else
      ak.output("LOADING")
      load_level(self.planetArray[self.currentlySelected].enum)
    end
  end
  f.handleBack = function(self)
    if self.inListMode then
      exit_galactic_map()
    else
      self.inListMode = true
      self.parent:goToSlide(1)
      set_sector_view(get_level_sector(self.planetArray[self.currentlySelected].enum))
    end
  end
  f.onSelectedItemChanged = function(self, i, item, reset)
    local d = self.planetArray[i]
    if not d.isSector then
      W.panel = { d.label, get_level_name(d.enum), d.desc }
      set_highlight_level(d.enum)
      get_level_image(d.enum)
      if self.inListMode then set_sector_view(get_level_sector(d.enum)) else set_level_view(d.enum) end
    else
      set_sector_view(d.enum)
    end
    self.currentlySelected = i
  end
  f.parent = (mk({}))
  return h, f
end

-- A map as the game opens it: c.unlocked the save's unlocked levels, c.hidden, c.visited, c.next get_next_level();
-- c.late the handler not initialized yet.
local function world(c)
  W = { unlocked = {}, hidden = c.hidden or {}, visited = c.visited or {}, next = c.next or 1, picks = {}, exits = 0 }
  for _, l in ipairs(c.unlocked or { 1 }) do W.unlocked[l] = true end
  for _, k in ipairs({ "scene", "Presentation", "qw" }) do _G[k] = nil end
  bindings()
  local reg = {}
  scene = mk({ _table_ActDeactRegisters = reg })
  Presentation = mk({ scene = scene })
  W.reg = reg
  local lb, lf = list()
  local h, hf = handler(lb)
  lf.listUpdater = h
  W.h, W.hf, W.lb, W.lf = h, hf, lb, lf
  if not c.late then
    h:onInitialize()
    h:onActivate()
  end
end

local function glue()
  local f = assert(loadstr(GMAP))
  return f()
end

local function rows()
  local out = {}
  for i, d in ipairs(W.hf.planetArray) do out[i] = (d.isSector and "#" or "") .. d.label end
  return out
end

local function rowof(e)
  for i, d in ipairs(W.hf.planetArray) do
    if not d.isSector and d.enum == e then return i end
  end
end

local function select(i)
  W.lb:setSelectedIndex(i, false, false)
end

-- A pick as the player makes it on a fresh map: the row, Select (the zoom), Select (the load).
local function pick(i)
  W.hf.inListMode = true
  select(i)
  W.h:handleSelect()
  W.h:handleSelect()
end

local function cases(name)
  -- A new game's save: Cobalia alone. The game's own list: Cerullean, Cobalia.
  world({ unlocked = { 1 }, next = 1 })
  check(table.getn(W.hf.planetArray) == 2, name .. ": the game's list on a new game: Cerullean and Cobalia")
  local r = glue()
  check(r == "in23", name .. ": the glue goes in: " .. tostring(r))
  local rs = rows()
  check(table.getn(rs) == 23 and rs[1] == "#Solana Galaxy" and rs[2] == "Planet Kerwan" and rs[3] == "#Cerullean Sector",
        name .. ": Solana Galaxy and Kerwan first, then Cerullean")
  check(rs[6] == "Planet Fastoon" and rs[7] == "Planet Fastoon II" and rs[8] == "Voron Asteroid Belt",
        name .. ": Fastoon, then Fastoon II, a row of its own, then Voron")
  check(LOC[18] == "Planet Fastoon " and rs[7] == "Planet Fastoon II" and get_location_name(18) == "Planet Fastoon II" and
        get_location_name(2) == "Planet Kortog", name .. ": the game's name with its trailing space trimmed: one space before II")
  check(rs[12] == "#Verdigris Sector" and rs[18] == "#Praxus Sector" and rs[23] == "Planet Igliak",
        name .. ": every sector and every planet, Igliak last")
  check(W.hf.currentlySelected == rowof(1) and W.lf.selectedItemIndex == rowof(1) and W.hf.newPlanet == rowof(1),
        name .. ": the cursor and the flashing row stay on Cobalia")
  check(W.lb.rows[1].itemName.textString == "Planet Cobalia", name .. ": the list's rows show the new list")
  check(W.bad == nil, name .. ": no view of Metropolis or Solana was asked for")

  -- Every row a pick; Kerwan's views are the whole map.
  local selectable = 0
  for i = 1, 23 do if W.h:isSelectable(i) then selectable = selectable + 1 end end
  check(selectable == 19 and not W.h:isSelectable(1) and W.h:isSelectable(2), name .. ": 19 rows to pick, no header")
  select(2)
  check(W.view == "global" and W.bad == nil and W.panel[1] == "Planet Kerwan" and W.panel[2] == "level0",
        name .. ": Kerwan in the list: the whole map, its panel Kerwan's")
  W.h:handleSelect()
  check(W.view == "global" and W.bad == nil and W.h.inListMode == false, name .. ": Select on Kerwan: zoomed in, the whole map")
  W.h:handleBack()
  check(W.view == "global" and W.bad == nil and W.h.inListMode == true, name .. ": Back: the list again, the whole map")
  W.h:handleSelect()
  W.h:handleSelect()
  check(W.picks[1] == 0 and W.exits == 1 and W.gamepick == nil, name .. ": Select, Select on Kerwan: the pick 0, the map closed")

  -- Fastoon II, and Mukow before Voron's space combat: the level picked.
  pick(rowof(18))
  check(W.picks[2] == 18 and W.exits == 2, name .. ": Fastoon II: the pick 18")
  pick(rowof(5))
  check(W.picks[3] == 5 and W.view == "level5", name .. ": Mukow: the pick 5, no space combat first")
  W.h:handleBack()
  check(W.view == "sector1", name .. ": Back from Mukow: Cerullean's view")

  -- Sent again (an answer lost): nothing twice.
  check(glue() == "in" and table.getn(W.hf.planetArray) == 23, name .. ": sent again: in, the list as it was")

  -- A save at Fastoon II: the game shows one Fastoon (Fastoon II); the glue both, each picking its own level.
  world({ unlocked = { 1, 2, 3, 18 }, next = 18 })
  check(table.getn(W.hf.planetArray) == 4 and W.hf.planetArray[4].enum == 18, name .. ": the game's list at Fastoon II: one Fastoon")
  select(4)
  r = glue()
  rs = rows()
  check(r == "in23" and rs[6] == "Planet Fastoon" and rs[7] == "Planet Fastoon II", name .. ": both Fastoons, distinct")
  check(W.hf.currentlySelected == rowof(18) and W.hf.newPlanet == rowof(18), name .. ": the cursor stays on Fastoon II")
  pick(rowof(3))
  check(W.picks[1] == 3, name .. ": Fastoon at Fastoon II: the pick 3")

  -- The handler not up yet: wait, and nothing changed.
  world({ late = true })
  check(glue() == "wait" and is_level_visible ~= nil and W.picks[1] == nil, name .. ": no handler yet: wait")
  W.h:onInitialize()
  check(glue() == "wait", name .. ": the handler but its list not activated: wait")
  W.h:onActivate()
  check(glue() == "in23", name .. ": then in")

  -- A game function missing: nothing changed.
  world({})
  exit_galactic_map = nil
  check(glue() == "nofn" and table.getn(W.hf.planetArray) == 2, name .. ": no exit_galactic_map: nothing changed")

  -- An error while the list is made again (Kerwan's description, say): the game's own list and globals back, said.
  world({})
  local gd, lvis = get_level_description, is_level_visible
  get_level_description = tolua(1, function(l)
    if l == 0 then error("boom") end
    return gd(l)
  end)
  r = glue()
  check(type(r) == "string" and string.find(r, "error:", 1, true) == 1, name .. ": an error is the answer: " .. tostring(r))
  check(table.getn(W.hf.planetArray) == 2 and is_level_visible == lvis and table.getn(W.hf.levelArray) == 18 and
        table.getn(W.hf.sectorArray) == 3, name .. ": and the game's own list, arrays and globals are back")
end

cases(NAME)
print(string.format("%s: %d checks, %d failed", NAME, checks, failures))
if failures ~= 0 then error("gmap checks failed") end
