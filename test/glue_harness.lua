-- Tools of Destruction's console menu (revision 1.29, pending): the pause
-- menu's Lua glue (src/games/luahelper/lh_glue.lua) and the helper's look
-- (lh_diag.lua), run by a real Lua against a pause menu made of stand-ins
-- (test/test_glue.py, docs/tod-menu.md 11).
--
-- The stand-ins are what the glue meets in the game, as the decompiled
-- pause menu (built/anark/pause/built.dat) has them:
--
--   Anark objects   userdata (newud, test_glue.py's C function) whose fields
--                   live in a table of their own, as tolua's peers do;
--   scene           the scene, its registry _table_ActDeactRegisters
--                   (AnarkLibraryExtensions), and Presentation.scene;
--   Node            the class table, goToSlide as AnarkLibraryExtensions
--                   defines it (here: the slide noted, and the Cheats
--                   screen's behaviour joining the registry on its slide
--                   when the case says it joins late);
--   ListUpdater     the pause's list: menuData as SetupMenus builds it,
--                   OPTIONS_MENU_INDEX, onHelp (slide 12);
--   cheatHandler    the Cheats screen: its methods, listBehavior, the
--                   title and counter texts beside it;
--   qw              qwark's side (lh_menu.c): a page of rows, the requests,
--                   the events, the notes, as plain Lua.
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

local function readfile(p)
  local f = assert(io.open(p, "rb"))
  local s = f:read("*a")
  f:close()
  return s
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

-- The game's own goToSlide (AnarkLibraryExtensions'), as far as the glue can tell.
-- The slide's behaviours activate in the next update: the Cheats screen's list resets itself then (frames()).
local function goToSlide(self, n)
  W.slide = n
  W.activate = n
  W.slides[table.getn(W.slides) + 1] = n
  if n == 16 and W.chlate and not W.chjoined then
    W.chjoined = true
    W.reg[W.ch] = true
  end
end

-- qwark's page: rows of label, value, kind, flags.
local function page(title, rows, root)
  W.title, W.rows, W.gen, W.vgen = title, rows, W.gen + 1, W.vgen + 1
  W.pf = root and 1 or 0
end

local function qw(op, a, b)
  if op == 0 then
    return W.on, W.help, W.gen, W.vgen, table.getn(W.rows), W.cur, 1, W.pf
  elseif op == 1 then
    local r = W.rows[a]
    if not r then return nil end
    return r[1], r[2], r[3], r[4]
  elseif op == 2 then
    return W.title, W.hint
  elseif op == 3 then
    W.events[table.getn(W.events) + 1] = { a, b }
    return 1
  elseif op == 4 then
    W.hstate = a
  elseif op == 5 then
    local r = W.req
    W.req = 0
    return r
  elseif op == 6 then
    W.notes[table.getn(W.notes) + 1] = a
    W.note = a
  end
end

-- A fresh pause: globals, the scene and its behaviours, as the case wants them.
-- c.lu: the list registered from frame c.lu (nil: never); c.ch: the Cheats screen registered at load (true),
-- on its slide (\"late\") or never (nil); c.helpat: menuData's menu and row for Help; c.noscene: only Presentation.scene.
local function world(c)
  for _, k in ipairs({ "qwark_frame", "qwark_quit", "qwark_state", "scene", "Presentation", "Node",
                       "is_cheat_unlocked", "get_cheat_desc", "get_current_skillpoints", "exit_pause_menu" }) do
    _G[k] = nil
  end
  W = { slide = 3, slides = {}, events = {}, notes = {}, on = 1, help = 1, gen = 0, vgen = 0, cur = 0, pf = 1,
        req = 0, hstate = 0, rows = {}, title = "", hint = "", calls = {}, exited = 0 }
  Node = { goToSlide = goToSlide, _useOnActivateDeactivate = function(self) end }
  page("Trainer", { { "Game", "", 1, 1 }, { "Cheats", "", 1, 1 }, { "Info", "x", 0, 0 } }, true)

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

  -- The pause's list (ListUpdater), SetupMenus' menuData.
  local lu, luf = mk({ name = "ListUpdater", OPTIONS_MENU_INDEX = 2, currentSlide = 3, currentMenuIndex = 1 })
  luf.onHelp = function(self) scene:goToSlide(12) end
  luf.onSave = function(self) end
  local md = { {}, {}, {}, {}, {} }
  for i = 1, 8 do md[1][i] = { 32 + i, luf.onSave } end
  local hm, hr = (c.helpat or { 2, 4 })[1], (c.helpat or { 2, 4 })[2]
  md[hm] = md[hm] or {}
  for i = 1, 6 do md[2][i] = md[2][i] or { 41 + i, luf.onSave } end
  md[2][4] = nil
  if hm ~= 2 or hr ~= 4 then md[2][4] = { 45, luf.onSave } end
  md[hm][hr] = { 45, luf.onHelp }
  luf.menuData = md
  W.lu, W.luf, W.md, W.hm, W.hr = lu, luf, md, hm, hr

  -- The Cheats screen (cheatHandler): the game's own methods, which the glue wraps.
  local ls = mk({ name = "listBehavior",
            resetList = function(self)
              W.calls[table.getn(W.calls) + 1] = "reset"
              W.listed = {}
              for i = 1, W.ch.getNumListItems(W.ch) do
                local item = mk({ label = text(""), value = text("") })
                W.ch.setListItemValue(W.ch, i, item)
                W.listed[i] = { item.label.textString, item.value.textString }
              end
            end,
            setSelectedIndex = function(self, i) W.calls[table.getn(W.calls) + 1] = "sel" .. i W.sel = i end,
            updateAllItems = function(self) W.calls[table.getn(W.calls) + 1] = "update" end })
  W.ls = ls
  W.titlenode, W.spl, W.spv = text("CHEATS"), text("SKILL POINTS"), text("12")
  local bg = mk({ Page_name2 = W.titlenode, skillpoint_label = W.spl })
  local par = mk({ shell_BG = bg })
  local ch = mk({ name = "cheatHandler", listBehavior = ls, parent = par, totalSkillpoints = W.spv,
                  itemDesc = text(""), currentlySelected = 1,
                  changeCheatState = function(self, d) W.gamecheat = d end,
                  getNumListItems = function(self) return 30 end,
                  setListItemValue = function(self, i, item) item.label.textString = "cheat" .. i end,
                  isSelectable = function(self, i) return true end,
                  handleSelect = function(self) self:changeCheatState(1) end,
                  handleRight = function(self) self:changeCheatState(1) end,
                  handleLeft = function(self) self:changeCheatState(-1) end,
                  handleBack = function(self) scene:goToSlide(21) end,
                  onSelectedItemChanged = function(self, i) W.changed = i end })
  W.ch = ch
  W.chlate = c.ch == "late"

  -- Other behaviours the registry holds.
  reg[(mk({ name = "blankHandler", onActivate = function() end }))] = false
  reg[(mk({ name = "optHandler" }))] = false
  if c.ch == true then reg[ch] = false end
  W.lufrom = c.lu
  if c.lu == 0 then reg[lu] = true end

  is_cheat_unlocked = function(i) return "game" end
  get_cheat_desc = function(i) return "gamedesc" end
  get_current_skillpoints = function() return 12 end
  exit_pause_menu = function() W.exited = W.exited + 1 end
  _G.qw = qw

  assert(loadstr(GLUE))()
end

local function frames(n)
  for i = 1, n do
    W.frame = (W.frame or 0) + 1
    if W.lufrom and W.frame >= W.lufrom then W.reg[W.lu] = true end
    if W.activate == 16 and W.reg[W.ch] ~= nil then W.ls.resetList(W.ls) end
    W.activate = nil
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

local NOTE_IN, NOTE_NONE, NOTE_NOCH, NOTE_IN_LIST, NOTE_LATE = 1, 2, 3, 4, 5

-- ------------------------------------------------------------ the cases

local function cases(name)
  -- Everything registered at load: the old glue's case.
  world({ lu = 0, ch = true })
  frames(1)
  check(W.note == NOTE_IN, name .. ": list and Cheats screen at load: in, on the first look")
  frames(1)
  check(helprow()[1] == "TRAINER" and type(helprow()[2]) == "function", name .. ": Options' Help row says TRAINER")
  W.help = 0
  frames(1)
  check(helprow()[1] == 45 and helprow()[2] == W.luf.onHelp, name .. ": menu.help 0: the game's Help row again")
  W.help = 1
  frames(1)
  helprow()[2](W.lu)
  check(W.slide == 16 and W.hstate == 1, name .. ": TRAINER: the Cheats screen's slide, the menu open")
  frames(2)
  check(W.listed and W.listed[1][1] == "Game" and W.listed[2][1] == "Cheats" and W.listed[1][2] == ">" and
        W.listed[3][2] == "x" and W.titlenode.textString == "Trainer" and W.spv.textString == "",
        name .. ": qwark's rows on the list, \">\" for a link, the title, no counter")
  check(W.ch.isSelectable(W.ch, 3) == false and W.ch.isSelectable(W.ch, 1) == true and is_cheat_unlocked(2) == false,
        name .. ": an INFO row is not selectable, the game's own call answered by qwark")
  W.ch.handleSelect(W.ch)
  check(W.events[1] and W.events[1][1] == 1 and W.events[1][2] == 1 and W.gamecheat == nil,
        name .. ": the cross: an ACTIVATE event for the row, the game's cheat untouched")
  W.ch.handleBack(W.ch)
  check(W.slide == 21 and W.hstate == 0 and W.titlenode.textString == "CHEATS",
        name .. ": Triangle at qwark's root: back to the list, the menu closed, the title the game's")
  W.ch.handleSelect(W.ch)
  check(W.gamecheat == 1 and is_cheat_unlocked(2) == "game", name .. ": with the menu down the Cheats screen is the game's")

  -- The combo in a pause: the request is taken once the pause settled.
  W.req = 1
  frames(1)
  check(W.slide ~= 16, name .. ": a request in the pause's first frames waits for it to settle")
  frames(20)
  check(W.slide == 16 and W.hstate == 1, name .. ": then the combo's open: the Trainer")
  W.req = 2
  frames(1)
  check(W.hstate == 0 and W.exited == 1, name .. ": the combo again: the menu down and the pause left")

  -- qwark_quit: everything back as it was.
  qwark_quit()
  check(qwark_frame == nil and Node.goToSlide == goToSlide and helprow()[2] == W.luf.onHelp and
        is_cheat_unlocked(1) == "game", name .. ": qwark_quit puts every wrapped function and the Help row back")

  -- The list registered late (its behaviour joined after the glue came).
  world({ lu = 40, ch = true })
  frames(30)
  check(W.note == nil and helprow()[2] == W.luf.onHelp, name .. ": no list yet: nothing said, Help the game's")
  frames(20)
  check(W.note == NOTE_IN and helprow()[1] == "TRAINER", name .. ": the list joins at frame 40: in by the next look")

  -- The Cheats screen joins only when its slide is shown (onInitialize on first activation).
  world({ lu = 0, ch = "late" })
  frames(2)
  check(W.note == NOTE_IN_LIST and helprow()[1] == "TRAINER", name .. ": list at load, no Cheats screen: in, the screen to meet")
  helprow()[2](W.lu)
  check(W.slide == 16 and W.hstate == 1, name .. ": TRAINER: to the Cheats screen's slide")
  frames(1)
  check(W.note == NOTE_LATE and W.listed and W.listed[1][1] == "Game" and W.titlenode.textString == "Trainer",
        name .. ": the screen joined on its slide: met, wrapped, qwark's rows on it")
  frames(3)
  check(W.sel == 1, name .. ": the cursor where qwark keeps it")

  -- The Help row elsewhere than OPTIONS_MENU_INDEX's fourth: found by its callback.
  world({ lu = 0, ch = true, helpat = { 4, 7 } })
  frames(2)
  check(W.note == NOTE_IN and W.md[4][7][1] == "TRAINER" and W.md[2][4][1] == 45 and W.md[2][4][2] == W.luf.onSave,
        name .. ": Help at menuData[4][7]: that row is TRAINER, the others untouched")

  -- No global scene, only Presentation.scene.
  world({ lu = 0, ch = true, noscene = true })
  frames(1)
  check(W.note == NOTE_IN, name .. ": scene not a global: Presentation.scene found")

  -- No list ever: said after 300 frames, Help stays; the combo still opens the Trainer.
  world({ ch = true })
  frames(299)
  check(W.note == nil, name .. ": no list: nothing said before 300 frames")
  frames(1)
  check(W.note == NOTE_NONE and helprow()[2] == W.luf.onHelp, name .. ": 300 frames: no list said, Help the game's")
  W.req = 1
  frames(2)
  check(W.slide == 16 and W.hstate == 1 and W.listed and W.listed[1][1] == "Game",
        name .. ": the combo still opens the Trainer on the Cheats screen")
  W.ch.handleBack(W.ch)
  W.lufrom = W.frame + 1
  frames(61)
  check(W.note == NOTE_IN, name .. ": the list joining late after all: it goes in at the slower looks")

  -- The diag (lh_diag.lua) in that pause: what the glue saw.
  local out = assert(loadstr(DIAG))()
  check(type(out) == "string" and string.find(out, "reg=t,", 1, true) and string.find(out, "ListUpdater:mh", 1, true) and
        string.find(out, "cheatHandler:cl", 1, true) and string.find(out, " lu=2,1,3", 1, true) and
        string.find(out, "[2:6 T4]", 1, true) and string.len(out) < 1024,
        name .. ": the look says the registry, its keys and what they hold, and where Help went: " .. tostring(out))

  -- A Cheats screen that never comes: the Trainer waits on its slide, then goes back.
  world({ lu = 0 })
  frames(2)
  check(W.note == NOTE_IN_LIST, name .. ": no Cheats screen: in with the list")
  helprow()[2](W.lu)
  frames(89)
  check(W.slide == 16 and W.hstate == 1, name .. ": the Trainer waits on the slide")
  frames(1)
  check(W.note == NOTE_NOCH and W.slide == 21 and W.hstate == 0, name .. ": 90 frames: back to the list, said")
  out = assert(loadstr(DIAG))()
  check(string.find(out, "g=", 1, true) == 1 and string.find(out, "ListUpdater:mh+", 1, true) and
        not string.find(out, "cheatHandler", 1, true), name .. ": the look: the list there, no Cheats screen: " .. out)

  -- No scene at all: nothing found, said at 300 frames; the look says why.
  world({ lu = 0, ch = true })
  scene, Presentation = nil, nil
  frames(300)
  check(W.note == NOTE_NONE and helprow()[2] == W.luf.onHelp, name .. ": no scene: no list said, Help the game's")
  out = assert(loadstr(DIAG))()
  check(string.find(out, " s=nnn", 1, true) and string.find(out, "reg=n", 1, true), name .. ": the look: no scene, no registry: " .. out)

  -- qwark gone (the identity cleared): the menu down, Help the game's.
  world({ lu = 0, ch = true })
  frames(2)
  helprow()[2](W.lu)
  frames(1)
  W.on = 0
  frames(1)
  check(W.hstate == 0 and W.slide == 21 and helprow()[2] == W.luf.onHelp, name .. ": qwark gone: the menu leaves, Help the game's")
end

cases(NAME)
print(string.format("%s: %d checks, %d failed", NAME, checks, failures))
if failures ~= 0 then error("glue checks failed") end
