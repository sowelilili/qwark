-- Tools of Destruction's console menu (revision 1.29, pending): the pause
-- menu's Lua glue (src/games/luahelper/lh_glue.lua) and the helper's look
-- (lh_diag.lua), run by a real Lua against a pause menu made of stand-ins
-- (test/test_glue.py, docs/tod-menu.md 11 and 12).
--
-- The stand-ins are what the glue meets in the game, as the decompiled
-- pause menu (built/anark/pause/built.dat) has them, their methods written
-- after the game's own and calling each other by name as the game's do:
--
--   Anark objects   userdata (newud, test_glue.py's C function) whose fields
--                   live in a table of their own, as tolua's peers do;
--   scene           the scene, its registry _table_ActDeactRegisters
--                   (AnarkLibraryExtensions), and Presentation.scene;
--   Node            the class table, goToSlide as AnarkLibraryExtensions
--                   defines it (here: the slide noted; the next frame's
--                   update activates the slide's behaviours, a behaviour
--                   that joins late joining then);
--   ListUpdater     the pause's list: menuData as SetupMenus builds it,
--                   handleSelect, handleBack, onMenuBack, ActivateItem, and
--                   its ScrollableList (resetList, setSelectedIndex, the
--                   cursor's moves, nine rows); its parent the list's
--                   component, whose slide is its frame (the fifth console
--                   test: 1, 3, 5, 7, 9 and 11 frame nine, eight, seven, six,
--                   five and three rows), which its onActivate goes back to;
--   optHandler      GAME OPTIONS: optArray, onActivate, changeSelection,
--                   updateItemData, adjustOption, onUp/onDown/onLeft/onRight
--                   and onBack (scene:goToSlide(1)), six rows, its title;
--   cheatHandler    the Cheats screen, and the game's cheat globals as tolua
--                   binds them: an argument it does not want is an error
--                   (tolua_isnoobj), which is what broke the game's Cheats
--                   screen in the fourth console test;
--   qw              qwark's side (lh_menu.c): a page of rows, the requests,
--                   the events, presses posted where a row takes them, the
--                   window's edge, the next row the cursor stands on, the
--                   notes, as plain Lua.
--
-- Each case loads the glue into a fresh state of the world and runs frames
-- (qwark_frame) the way the helper does, and checks what the game and qwark
-- would see. Lua 5.0 has math.mod and table.getn; the Lua running this may
-- not, so they are put back first.

math.mod = math.mod or math.fmod
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

-- ------------------------------------------------------------ the world

local W

-- An Anark object: a userdata whose fields are a table of its own.
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

local function text(s)
  return (mk({ textString = s }))
end

-- The game's own goToSlide (AnarkLibraryExtensions'), as far as the glue can tell: the slide changes at the next update.
-- The list's component (the ListUpdater's parent) has slides of its own, its frame's, noted apart.
local function goToSlide(self, n)
  if self == W.lcomp then
    W.lslide = n
    return
  end
  W.slide = n
  W.activate = n
  W.slides[table.getn(W.slides) + 1] = n
end

-- tolua's binding of a game function taking `n` arguments: one more (even a nil) is an error, as tolua_isnoobj makes it.
local function tolua(n, f)
  return function(...)
    if select("#", ...) ~= n then error("#ferror in function", 0) end
    return f(...)
  end
end

-- qwark's page: rows of label, value, kind, flags; ptype 0 MAIN, 1 CONFIG; pflags bit0 root, bit1 more above, bit2 more below.
local function page(title, rows, pt, pf, cur)
  W.title, W.rows, W.gen, W.vgen = title, rows, W.gen + 1, W.vgen + 1
  W.pt, W.pf, W.cur = pt, pf, cur or 0
end

local ROOT = { { "Game", "", 1, 1 }, { "Cheats", "", 1, 1 }, { "Manips", "", 1, 1 }, { "Planets", "", 1, 1 },
               { "Unlocks", "", 1, 1 } }
local CHEATS = { { "Infinite ammo", "Off", 2, 5 }, { "Status", "x", 0, 0 }, { "Max bolts", "", 3, 1 },
                 { "Ghost", "On", 2, 7 } }

local function flagsof(i)
  local r = W.rows[i]
  return r and r[4] or 0
end

local function bit(x, b)
  return math.mod(math.floor(x / b), 2) == 1
end

local function post(kind, i)
  W.events[table.getn(W.events) + 1] = { kind, i }
  return 1
end

-- What lh_menu.c does for each LM_OP_* (the labels, values and title in capitals, as lm_take copies them).
local function qw(op, a, b, c)
  if op == 0 then
    return W.on, W.help, W.gen, W.vgen, table.getn(W.rows), W.cur, W.pt, W.pf
  elseif op == 1 then
    local r = W.rows[a]
    if not r then return nil end
    return string.upper(r[1]), string.upper(r[2]), r[3], r[4]
  elseif op == 2 then
    return string.upper(W.title), W.hint
  elseif op == 3 then
    return post(a, b)
  elseif op == 4 then
    W.hstate = a
  elseif op == 5 then
    local r = W.req
    W.req = 0
    return r
  elseif op == 6 then
    W.notes[table.getn(W.notes) + 1] = a
    W.note = a
  elseif op == 7 then
    local f = flagsof(b)
    if a == 1 and not bit(f, 1) then return 0 end
    if (a == 2 or a == 3) and not bit(f, 4) then return 0 end
    return post(a, b)
  elseif op == 8 then
    local before, after = false, false
    for k = 1, table.getn(W.rows) do
      if bit(flagsof(k), 1) then
        if k < a then before = true end
        if k > a then after = true end
      end
    end
    if bit(W.pf, 4) and not after then return post(8, a) end
    if bit(W.pf, 2) and not before then return post(7, a) end
    return 0
  elseif op == 9 then
    local m = math.min(c, table.getn(W.rows))
    if m == 0 or a > m then return a end
    local j = a
    for k = 1, m do
      j = j + b
      if j > m then j = 1 elseif j < 1 then j = m end
      if j == a or bit(flagsof(j), 1) then return j end
    end
    return a
  end
end

-- A fresh pause, as the case wants it: c.lu the list registered from frame c.lu (nil: never); c.opt Game Options
-- registered at load (true), on its slide's first showing ("late") or never (nil); c.helpat menuData's menu and row
-- for Help; c.noscene only Presentation.scene; c.slide the scene's slide at the start (3, the list).
local function world(c)
  for _, k in ipairs({ "qwark_frame", "qwark_quit", "qwark_state", "scene", "Presentation", "Node", "SetLocalizedText",
                       "EffectEvent", "is_cheat_unlocked", "get_cheat_desc", "get_current_skillpoints",
                       "get_num_cheats", "exit_pause_menu" }) do
    _G[k] = nil
  end
  W = { slide = c.slide or 3, slides = {}, events = {}, notes = {}, on = 1, help = 1, gen = 0, vgen = 0, cur = 0,
        pf = 1, pt = 0, req = 0, hstate = 0, rows = {}, title = "", hint = "", cues = 0, exited = 0, sets = 0 }
  Node = { goToSlide = goToSlide, _useOnActivateDeactivate = function(self) end }
  W.goToSlide = goToSlide
  SetLocalizedText = function(o, s) o.textString = s end
  EffectEvent = tolua(2, function(e, k) W.cues = W.cues + 1 end)
  EVENT_SUCCESS, CLASS_BUTTON, EVENT_LOST_FOCUS, CLASS_MENU = 1, 2, 3, 4
  page("Trainer", ROOT, 0, 1)

  local reg = {}
  W.reg = reg
  local s = mk({ _table_ActDeactRegisters = reg })
  W.scene = s
  if c.noscene then
    Presentation = mk({ scene = s })
  else
    scene = s
    Presentation = mk({ scene = s })
  end

  -- The pause's list (ListUpdater), after the game's chunk: SetupMenus' menuData, its handlers, its ScrollableList.
  local lu, luf = mk({ name = "ListUpdater", OPTIONS_MENU_INDEX = 2, currentSlide = 7, currentMenuIndex = 2,
                       currentIndex = 1, MAIN_MENU_INDEX = 1 })
  luf.onHelp = function(self) scene:goToSlide(12) end
  luf.onSave = function(self) W.saved = true end
  luf.handleSelect = function(self)
    local item = self.menuData[self.currentMenuIndex][self.currentIndex]
    if item and item[2] then item[2](self) end
  end
  luf.handleBack = function(self)
    if self.currentMenuIndex == self.MAIN_MENU_INDEX then exit_pause_menu() else self:onMenuBack() end
  end
  luf.onMenuBack = function(self)
    W.gameback = (W.gameback or 0) + 1
    self.currentMenuIndex = self.MAIN_MENU_INDEX
    self.listBehavior:resetList()
  end
  luf.getNumListItems = function(self) return table.getn(self.menuData[self.currentMenuIndex]) end
  luf.isSelectable = function(self, i) return self.menuData[self.currentMenuIndex][i] and true or false end
  luf.setListItemValue = function(self, i, item) SetLocalizedText(item, self.menuData[self.currentMenuIndex][i][1]) end
  luf.clearListItemValue = function(self, i, item) SetLocalizedText(item, "") end
  luf.onSelectedItemChanged = function(self, i, item, reset) self.currentIndex = i end
  luf.onActivate = function(self)
    self.parent:goToSlide(self.currentSlide)
    self.listBehavior:updateAllItems()
  end
  W.lcomp = mk({ name = "list" })
  W.lslide = 7
  luf.parent = W.lcomp
  local md = { {}, {}, {}, {}, {} }
  for i = 1, 8 do md[1][i] = { 32 + i, luf.onSave } end
  for i = 1, 5 do md[3][i] = { 47 + i, luf.onSave } end
  local hm, hr = (c.helpat or { 2, 4 })[1], (c.helpat or { 2, 4 })[2]
  for i = 1, 6 do md[2][i] = { 41 + i, luf.onSave } end
  md[hm][hr] = { 45, luf.onHelp }
  luf.menuData = md
  W.lu, W.luf, W.md, W.hm, W.hr = lu, luf, md, hm, hr
  W.btn = {}
  for i = 1, 9 do W.btn[i] = text("") end
  local ls, lsf = mk({ name = "listBehavior", listUpdater = lu, selectedItemIndex = 1, hasFocus = true })
  lsf.updateAllItems = function(self)
    local n = self.listUpdater:getNumListItems()
    for i = 1, 9 do
      if i <= n then self.listUpdater:setListItemValue(i, W.btn[i]) else self.listUpdater:clearListItemValue(i, W.btn[i]) end
    end
  end
  lsf.setSelectedIndex = function(self, i, isReset, dont)
    local n = self.listUpdater:getNumListItems()
    if i > n then i = n end
    if i < 1 then i = 1 end
    self.selectedItemIndex = i
    if not dont then self.listUpdater:onSelectedItemChanged(i, W.btn[i], isReset and true or false) end
    self:updateAllItems()
  end
  lsf.resetList = function(self)
    W.resets = (W.resets or 0) + 1
    self:setSelectedIndex(1, true)
  end
  -- The cursor's moves (Up and Down: moveSelection), wrapping, past rows that cannot be selected.
  lsf.move = function(self, d)
    local n, j = self.listUpdater:getNumListItems(), self.selectedItemIndex
    repeat
      j = j + d
      if j > n then j = 1 elseif j < 1 then j = n end
    until self.listUpdater:isSelectable(j) or j == self.selectedItemIndex
    self.selectedItemIndex = j
    self.listUpdater:onSelectedItemChanged(j, W.btn[j], false)
  end
  luf.listBehavior = ls
  W.ls = ls

  -- GAME OPTIONS (optHandler), after the game's chunk.
  W.opttitle = text("GAME OPTIONS")
  W.optdesc = text("")
  local bg = mk({ Page_name2 = W.opttitle })
  local par = mk({ name = "options", shell_BG = bg })
  local orows = {}
  for i = 1, 6 do orows[i] = mk({ optLabel = text(""), optValue = text(""), bar = mk({ opacity = 0 }) }) end
  local o, of = mk({ name = "optHandler", parent = par, desc = W.optdesc, currentSelection = 1 })
  local arr = {}
  W.gameopt = { true, false, true, true, false, true }
  for i = 1, 8 do
    arr[i] = { optName = 100 + i, desc = 200 + i, enabled = i ~= 4 and i ~= 7,
               get = function() return W.gameopt[i] end, set = function(v) W.gameopt[i] = v W.sets = W.sets + 1 end }
  end
  of.optArray = arr
  of.localizeKey = { ["true"] = 22, ["false"] = 23 }
  of.onActivate = function(self)
    self.option = {}
    for i = 1, table.getn(self.optArray) do
      if self.optArray[i].enabled then table.insert(self.option, self.optArray[i]) end
    end
    self.row = {}
    for i = 1, 6 do
      local child = orows[i]
      SetLocalizedText(child.optLabel, "")
      SetLocalizedText(child.optValue, "")
      child.bar.opacity = 0
      self.row[i] = child
    end
    if table.getn(self.row) < table.getn(self.option) then self.max = table.getn(self.row) else self.max = table.getn(self.option) end
    self:updateItemData()
    W.hl = self.currentSelection
    SetLocalizedText(self.desc, self.option[self.currentSelection].desc)
  end
  of.changeSelection = function(self, delta)
    local new = self.currentSelection + delta
    if self.max < new then new = 1 elseif new < 1 then new = self.max end
    self.currentSelection = new
    W.hl = new
    EffectEvent(5, 6)
    SetLocalizedText(self.desc, self.option[new].desc)
    self:updateItemData(new)
  end
  of.updateItemData = function(self, i)
    local s, f = i, i
    if s == nil then s, f = 1, self.max end
    for k = s, f do
      SetLocalizedText(self.row[k].optLabel, self.option[k].optName)
      SetLocalizedText(self.row[k].optValue, self.localizeKey[tostring(self.option[k].get())])
    end
  end
  of.adjustOption = function(self, d)
    local o = self.option[self.currentSelection]
    o.set(not o.get())
    self:updateItemData(self.currentSelection)
  end
  of.onLeft = function(self) self:adjustOption(-1) end
  of.onRight = function(self) self:adjustOption(1) end
  of.onDown = function(self) self:changeSelection(1) end
  of.onUp = function(self) self:changeSelection(-1) end
  of.onBack = function(self) scene:goToSlide(1) end
  W.o, W.of, W.orows = o, of, orows
  W.ogame = {}
  for _, k in ipairs({ "optArray", "onActivate", "changeSelection", "updateItemData", "adjustOption", "onBack" }) do
    W.ogame[k] = of[k]
  end
  W.optlate = c.opt == "late"

  -- The Cheats screen (cheatHandler), its game globals as tolua binds them; the glue must never touch either.
  get_num_cheats = tolua(0, function() return 3 end)
  is_cheat_unlocked = tolua(1, function(i) return true end)
  get_cheat_desc = tolua(1, function(i) return "desc" .. i end)
  get_current_skillpoints = tolua(0, function() return 12 end)
  W.cheatglobals = { get_num_cheats, is_cheat_unlocked, get_cheat_desc, get_current_skillpoints }
  local ch, chf = mk({ name = "cheatHandler", itemDesc = text("text"), totalSkillpoints = text("") })
  chf.changeCheatState = function(self, d) end
  chf.getNumListItems = function(self) return get_num_cheats() end
  chf.onActivate = function(self)
    self.totalSkillpoints.textString = get_current_skillpoints()
    self.itemDesc.textString = ""
    if is_cheat_unlocked(0) then self.itemDesc.textString = get_cheat_desc(0) end
  end
  W.ch, W.chf = ch, chf
  W.chgame = {}
  for k, v in pairs(chf) do W.chgame[k] = v end

  -- The registry: behaviours that joined in their onInitialize.
  reg[(mk({ name = "blankHandler", onActivate = function() end }))] = false
  reg[ch] = false
  if c.opt == true then reg[o] = false end
  W.lufrom = c.lu
  if c.lu == 0 then reg[lu] = W.slide == 3 or W.slide == 7 end

  exit_pause_menu = tolua(0, function() W.exited = W.exited + 1 end)
  _G.qw = qw

  assert(loadstr(GLUE))()
end

-- A frame: the game's update (the slide asked for shows, its behaviours activate, the others go), then the glue's.
local function frames(n)
  for i = 1, n do
    W.frame = (W.frame or 0) + 1
    if W.lufrom and W.frame >= W.lufrom and W.reg[W.lu] == nil then W.reg[W.lu] = W.slide == 3 or W.slide == 7 end
    local a = W.activate
    W.activate = nil
    if a == 9 then
      if W.reg[W.o] == nil and W.optlate then W.reg[W.o] = false end
      if W.reg[W.lu] ~= nil then W.reg[W.lu] = false end
      if W.reg[W.o] ~= nil then
        W.reg[W.o] = true
        W.o:onActivate()
      end
    elseif a == 1 or a == 21 then
      if W.reg[W.o] ~= nil then W.reg[W.o] = false end
      if W.reg[W.lu] ~= nil then
        W.reg[W.lu] = true
        W.lu:onActivate()
      end
    elseif a == 16 then
      if W.reg[W.lu] ~= nil then W.reg[W.lu] = false end
      W.reg[W.ch] = true
      W.ch:onActivate()
    elseif a then
      if W.reg[W.lu] ~= nil then W.reg[W.lu] = false end
    end
    if qwark_frame then qwark_frame() end
  end
end

local function hasnote(n)
  for _, v in ipairs(W.notes) do if v == n then return true end end
  return false
end

local function helprow()
  return W.md[W.hm][W.hr]
end

local function lastev()
  return W.events[table.getn(W.events)] or {}
end

-- The game's own list and Game Options, as the game left them: every method and field the glue may put.
local function gameslists()
  if W.luf.onMenuBack == nil or W.luf.currentMenuIndex == 6 or W.md[6] ~= nil then return false end
  if W.luf.currentSlide ~= 7 or W.lslide ~= 7 then return false end
  for i = 1, 6 do
    if W.orows[i].opacity ~= nil and W.orows[i].opacity ~= 100 then return false end
  end
  for k, v in pairs(W.ogame) do
    if W.of[k] ~= v then return false end
  end
  return Node.goToSlide == W.goToSlide and W.opttitle.textString == "GAME OPTIONS"
end

-- The Cheats screen and its globals untouched.
local function cheatsuntouched()
  for k, v in pairs(W.chgame) do if W.chf[k] ~= v then return false end end
  local g = W.cheatglobals
  return get_num_cheats == g[1] and is_cheat_unlocked == g[2] and get_cheat_desc == g[3] and get_current_skillpoints == g[4]
end

local function btns(n)
  local t = {}
  for i = 1, n do t[i] = tostring(W.btn[i].textString) end
  return table.concat(t, ",")
end

-- TRAINER from Options, as the player does it: the cursor on its row, the cross.
local function trainer()
  W.luf.currentMenuIndex = W.hm
  W.ls:setSelectedIndex(W.hr, true)
  W.lu:handleSelect()
end

-- ------------------------------------------------------------ the cases

local function cases(name)
  -- Everything registered at load.
  world({ lu = 0, opt = true })
  frames(1)
  check(W.note == 1, name .. ": list and Game Options at load: in, on the first look")
  check(helprow()[1] == "TRAINER" and type(helprow()[2]) == "function", name .. ": Options' Help row says TRAINER")
  W.help = 0
  frames(1)
  check(helprow()[1] == 45 and helprow()[2] == W.luf.onHelp, name .. ": menu.help 0: the game's Help row again")
  W.help = 1
  frames(1)
  check(gameslists() and cheatsuntouched(), name .. ": with the Trainer down nothing of the game's is touched but the Help row")

  -- TRAINER: the root, a MAIN page, in the pause's own list.
  trainer()
  check(W.hstate == 1 and W.luf.currentMenuIndex == 6 and btns(6) == "GAME,CHEATS,MANIPS,PLANETS,UNLOCKS," and
        W.ls.selectedItemIndex == 1 and W.slide == 3, name .. ": TRAINER: the root in the pause's list, capitals, no screen change: " .. btns(6))
  check(W.lslide == 9 and W.luf.currentSlide == 9, name .. ": the root's five rows: the list's five-row frame, slide 9 (Options' was 7)")
  -- A link: the cross is the list's own handleSelect, the row's callback ours.
  W.ls:move(1)
  W.lu:handleSelect()
  check(lastev()[1] == 1 and lastev()[2] == 2, name .. ": the cross on CHEATS: an ACTIVATE for row 2")
  check(cheatsuntouched(), name .. ": the Cheats screen untouched by the Trainer")

  -- qwark opens the Cheats section: a CONFIG page, on Game Options.
  page("Cheats", CHEATS, 1, 0)
  frames(1)
  check(W.slide == 9 and W.of.optArray ~= W.ogame.optArray and W.of.updateItemData ~= W.ogame.updateItemData,
        name .. ": a CONFIG page: Game Options' slide, taken over")
  frames(1)
  local r = W.orows
  check(r[1].optLabel.textString == "INFINITE AMMO" and r[1].optValue.textString == "OFF" and r[2].optValue.textString == "X" and
        r[3].optValue.textString == "" and r[4].optLabel.textString == "GHOST" and r[5].optLabel.textString == "" and
        W.opttitle.textString == "CHEATS", name .. ": its rows, values and title, Game Options' rows past the page empty")
  check(r[1].opacity == 100 and r[4].opacity == 100 and r[5].opacity == 0 and r[6].opacity == 0 and W.lslide == 9,
        name .. ": a page of four rows: Game Options' fifth and sixth rows hidden")
  frames(1)
  check(W.of.currentSelection == 1 and W.hl == 1 and W.of.max == 4, name .. ": the cursor where qwark keeps it, four rows to move in")
  W.o:onDown()
  check(W.of.currentSelection == 3, name .. ": Down past the INFO row, as the game's changeSelection moves")
  W.o:onRight()
  check(lastev()[1] == 1 and lastev()[2] == 3, name .. ": the cross (onRight) on an action: ACTIVATE")
  W.o:onUp()
  W.o:onRight()
  check(lastev()[1] == 3 and lastev()[2] == 1, name .. ": Right on a toggle that steps: RIGHT")
  W.o:onLeft()
  check(lastev()[1] == 2 and lastev()[2] == 1 and W.sets == 0, name .. ": Left: LEFT, the game's options never set")
  W.hint = "Infinite ammo: On"
  W.vgen = W.vgen + 1
  W.rows = { { "Infinite ammo", "On", 2, 5 }, CHEATS[2], CHEATS[3], CHEATS[4] }
  frames(1)
  check(r[1].optValue.textString == "ON" and W.optdesc.textString == "Infinite ammo: On",
        name .. ": a new value and qwark's answer in the description")
  local nev = table.getn(W.events)
  W.o:onBack()
  W.ogame.onBack(W.o)
  check(table.getn(W.events) == nev + 2 and lastev()[1] == 4 and W.slide == 9,
        name .. ": Triangle, by its name or the game's own onBack: BACK, the slide kept")

  -- Back to the root: Game Options the game's again, the list the Trainer's.
  page("Trainer", ROOT, 0, 1, 1)
  frames(1)
  check(W.slide == 1 and W.of.optArray == W.ogame.optArray and W.of.updateItemData == W.ogame.updateItemData and
        W.of.changeSelection == W.ogame.changeSelection and W.of.adjustOption == W.ogame.adjustOption and
        W.of.onBack == W.ogame.onBack and Node.goToSlide == W.goToSlide and W.opttitle.textString == "GAME OPTIONS",
        name .. ": back to the root: Game Options' every method, field and title the game's, the list's slide")
  frames(2)
  check(W.luf.currentMenuIndex == 6 and btns(2) == "GAME,CHEATS" and W.ls.selectedItemIndex == 2,
        name .. ": the root in the list again, the cursor on CHEATS")
  check(W.lslide == 9 and r[5].opacity == 100 and r[6].opacity == 100,
        name .. ": the list's activation keeps the Trainer's frame; Game Options' rows all shown again")
  -- The fifth console test: each row count its own frame (a page of n rows: the slide that frames n, or the next larger).
  local want = { 11, 11, 11, 9, 9, 7, 5, 3, 1, 1, 1 }
  local got = {}
  for n = 1, 11 do
    local rows = {}
    for k = 1, n do rows[k] = { "R" .. k, "", 1, 1 } end
    page("Trainer", rows, 0, 1)
    frames(1)
    got[n] = W.lslide == want[n] and W.luf.currentSlide == want[n] and btns(1) == "R1" and W.slide == 1
  end
  check(got[1] and got[2] and got[3] and got[4] and got[5] and got[6] and got[7] and got[8] and got[9] and got[10] and got[11],
        name .. ": one to eleven rows: slides 11, 11, 11, 9, 9, 7, 5, 3, 1, 1, 1")
  page("Trainer", ROOT, 0, 1, 1)
  frames(3)

  -- Triangle at the root: out, the list Options again with the cursor on TRAINER.
  W.lu:handleBack()
  check(W.hstate == 0 and W.luf.currentMenuIndex == 2 and W.md[6] == nil and W.luf.currentIndex == W.hr and
        W.gameback == nil and gameslists(), name .. ": Triangle at the root: Options again, the cursor on TRAINER, all the game's")
  check(cheatsuntouched(), name .. ": the Cheats screen still the game's")

  -- The game's own screens after a Trainer visit.
  W.ls:setSelectedIndex(1, true)
  W.lu:handleSelect()
  check(W.saved == true, name .. ": the Options list's own rows work")
  W.luf.currentMenuIndex = 2
  W.lu:handleBack()
  check(W.gameback == 1 and W.luf.currentMenuIndex == 1, name .. ": the list's own back is the game's")
  local ok = pcall(function()
    scene:goToSlide(9)
    frames(1)
    W.o:onRight()
    W.o:onDown()
  end)
  check(ok and W.slide == 9 and W.sets == 1 and W.orows[1].optLabel.textString == 101 and W.of.max == 6 and W.of.currentSelection ~= nil,
        name .. ": the game's Game Options after a visit: its options, rows and setting")
  ok = pcall(function()
    scene:goToSlide(16)
    frames(1)
  end)
  check(ok and W.ch.totalSkillpoints.textString == 12 and W.ch.itemDesc.textString == "desc0",
        name .. ": the game's Cheats screen after a visit: its counter and description (no tolua error)")

  -- The combo in a pause on its list: taken at once, the Trainer in the list.
  world({ lu = 0, opt = true })
  frames(1)
  W.req = 1
  frames(1)
  check(W.hstate == 1 and W.luf.currentMenuIndex == 6 and btns(1) == "GAME", name .. ": the combo on the pause's list: the Trainer at once")
  W.req = 2
  frames(1)
  check(W.hstate == 0 and W.exited == 1 and gameslists(), name .. ": the combo again: the menu down, the pause left, all the game's")

  -- The fifth console test: a load asked for while the Trainer is down (the client's, in the pause): qwark's close leaves the pause.
  world({ lu = 0, opt = true })
  frames(1)
  W.req = 2
  frames(1)
  check(W.hstate == 0 and W.exited == 1 and gameslists(), name .. ": a close with the Trainer down: the pause left, nothing of the game's touched")
  -- And from the Trainer on a CONFIG page (a planet): the menu leaves Game Options, then the pause.
  world({ lu = 0, opt = true })
  frames(1)
  trainer()
  page("Planets", { { "A", "", 3, 1 }, { "B", "", 3, 1 } }, 1, 0)
  frames(3)
  check(W.orows[3].opacity == 0, name .. ": Planets of two rows: Game Options' third row hidden")
  W.req = 2
  frames(1)
  check(W.hstate == 0 and W.exited == 1 and gameslists() and W.orows[3].opacity == 100,
        name .. ": the planet's close: the Trainer down, the pause left, Game Options' rows and the list's frame the game's")

  -- The combo from a screen of the game's: the list brought back first.
  world({ lu = 0, opt = true, slide = 2 })
  frames(1)
  W.req = 1
  frames(1)
  check(W.hstate == 0, name .. ": the combo on a game's screen in the pause's first frames waits")
  frames(30)
  check(W.hstate == 1 and W.slides[1] == 21 and W.luf.currentMenuIndex == 6 and btns(1) == "GAME",
        name .. ": then the list comes back with the Trainer in it")

  -- Game Options joins only on its slide's first showing.
  world({ lu = 0, opt = "late" })
  frames(1)
  check(W.note == 4, name .. ": list at load, no Game Options: in, the screen to meet")
  trainer()
  page("Cheats", CHEATS, 1, 0)
  frames(1)
  check(W.slide == 9 and W.of.updateItemData == W.ogame.updateItemData, name .. ": a CONFIG page: to Game Options' slide, nothing to take yet")
  frames(1)
  check(W.note == 5 and W.of.updateItemData ~= W.ogame.updateItemData, name .. ": Game Options joined on its slide: met and taken over")
  frames(2)
  check(W.orows[1].optLabel.textString == "INFINITE AMMO" and W.opttitle.textString == "CHEATS" and W.of.currentSelection == 1,
        name .. ": its rows ours over the game's")
  -- qwark_quit there: every bit the game's again.
  qwark_quit()
  check(qwark_frame == nil and gameslists() and helprow()[2] == W.luf.onHelp and cheatsuntouched(),
        name .. ": qwark_quit on Game Options puts every method, field, title and the Help row back")

  -- A Game Options that never comes: the Trainer waits on the slide, then goes back.
  world({ lu = 0 })
  frames(1)
  trainer()
  page("Cheats", CHEATS, 1, 0)
  frames(90)
  check(W.slide == 9 and W.hstate == 1, name .. ": the Trainer waits on Game Options' slide")
  frames(1)
  check(W.note == 3 and W.slide == 1 and W.hstate == 0 and gameslists(), name .. ": 90 frames: back to the list, said, all the game's")
  local out = assert(loadstr(DIAG))()
  check(string.find(out, "ListUpdater:mh", 1, true) and string.find(out, "cheatHandler:c", 1, true) and
        not string.find(out, "optHandler", 1, true) and string.len(out) < 1024, name .. ": the look: the list, no Game Options: " .. out)

  -- A page longer than Game Options' window: the cursor at its edge asks for the next.
  world({ lu = 0, opt = true })
  frames(1)
  trainer()
  page("Planets", { { "A", "", 3, 1 }, { "B", "", 3, 1 }, { "C", "", 3, 1 }, { "D", "", 3, 1 }, { "E", "", 3, 1 },
                    { "F", "", 3, 1 } }, 1, 4)
  frames(3)
  for i = 1, 4 do W.o:onDown() end
  check(table.getn(W.events) == 0, name .. ": inside the window: no scroll")
  W.o:onDown()
  check(lastev()[1] == 8 and lastev()[2] == 6, name .. ": to the window's last row with more below: SCROLL_DOWN")
  out = assert(loadstr(DIAG))()
  check(string.find(out, "optHandler:o", 1, true) and string.find(out, ",1", 1, true) and string.find(out, "[6:", 1, true),
        name .. ": the look: Game Options taken over, the Trainer's menu: " .. out)

  -- qwark gone on Game Options: the menu leaves, all the game's, the list back.
  W.on = 0
  frames(1)
  check(W.hstate == 0 and W.slide == 1 and gameslists() and helprow()[2] == W.luf.onHelp, name .. ": qwark gone: the menu leaves, all the game's")

  -- An error in the glue's frame: all the game's before it goes to the helper.
  world({ lu = 0, opt = true })
  frames(1)
  trainer()
  page("Cheats", CHEATS, 1, 0)
  frames(3)
  W.rows = nil
  ok = pcall(qwark_frame)
  W.rows = CHEATS
  check(not ok and gameslists() and W.luf.currentMenuIndex == 2 and helprow()[2] == W.luf.onHelp, name .. ": an error: everything the game's first")

  -- The list registered late (its behaviour joined after the glue came).
  world({ lu = 40, opt = true })
  frames(30)
  check(W.note == nil and helprow()[2] == W.luf.onHelp, name .. ": no list yet: nothing said, Help the game's")
  frames(20)
  check(W.note == 1 and helprow()[1] == "TRAINER", name .. ": the list joins at frame 40: in by the next look")

  -- The Help row elsewhere than OPTIONS_MENU_INDEX's fourth: found by its callback.
  world({ lu = 0, opt = true, helpat = { 3, 2 } })
  frames(1)
  check(W.note == 1 and W.md[3][2][1] == "TRAINER" and W.md[2][4][1] == 45 and W.md[2][4][2] == W.luf.onSave,
        name .. ": Help at menuData[3][2]: that row is TRAINER, the others untouched")

  -- No global scene, only Presentation.scene.
  world({ lu = 0, opt = true, noscene = true })
  frames(1)
  check(W.note == 1, name .. ": scene not a global: Presentation.scene found")

  -- No list ever: said after 300 frames, Help stays, and the combo opens nothing.
  world({ opt = true })
  frames(299)
  check(W.note == nil, name .. ": no list: nothing said before 300 frames")
  frames(1)
  check(W.note == 2 and helprow()[2] == W.luf.onHelp, name .. ": 300 frames: no list said, Help the game's")
  W.req = 1
  frames(2)
  check(W.hstate == 0 and W.slide == 3, name .. ": no list: the combo opens nothing")
  out = assert(loadstr(DIAG))()
  check(string.find(out, "g=", 1, true) == 1 and string.find(out, "optHandler:o", 1, true), name .. ": the look: " .. out)

  -- No scene at all: nothing found, said at 300 frames; the look says why.
  world({ lu = 0, opt = true })
  scene, Presentation = nil, nil
  frames(300)
  check(W.note == 2 and helprow()[2] == W.luf.onHelp, name .. ": no scene: no list said, Help the game's")
  out = assert(loadstr(DIAG))()
  check(string.find(out, " s=nnn", 1, true) and string.find(out, "reg=n", 1, true), name .. ": the look: no scene, no registry: " .. out)
end

cases(NAME)
print(string.format("%s: %d checks, %d failed", NAME, checks, failures))
if failures ~= 0 then error("glue checks failed") end
