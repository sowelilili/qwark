-- Tools of Destruction's console menu (revision 1.29, pending): the Lua glue
-- that shows qwark's menu with the pause menu's own Anark screens.
-- docs/tod-menu.md has the design.
--
-- The Lua helper loads this once into each pause menu's VM (its lua_State
-- at the menu system's +0x106C), on the main thread, after the menu's own
-- update, and then calls qwark_frame every frame of the pause menu.
--
-- Every Anark object here (a Node, the scene, a behaviour's self) is a
-- handle the game looks up in the presentation the Anark runtime has
-- pushed, and outside a push that presentation is NULL: a Node method then
-- reads through a NULL pointer, which no pcall catches (the crash of
-- 2026-10-31, docs/tod-menu.md 9). The helper runs this file and
-- qwark_frame only with the pause's presentation pushed, as the game's own
-- AnarkMenuSystem_reset does, and only once the game's own checks say the
-- presentation is up (lh_menu.c, lm_ready). What is here never calls a
-- method on anything but a userdata it got from the game, through pcall.
--
-- Memory (the second crash, docs/tod-menu.md 10): the pause menu's Lua
-- allocates from a small pool of fixed blocks, set up afresh each pause,
-- that faults when a size runs out instead of falling back to its heap, and
-- Lua 5.0 collects only when its byte count doubles, which says nothing of
-- that pool. So nothing here makes garbage frame after frame: no closure per
-- call (pcall gets a function and its arguments), no vararg function (each
-- call of one makes a table), no walk of the scene (each node it reaches
-- becomes a Lua object the game keeps). The helper counts the pool before
-- every frame, collects when it runs low, and calls qwark_quit when even a
-- collection leaves too little: everything wrapped goes back, and the glue
-- is garbage.
--
-- Finding the pause's behaviours (the third console test, docs/tod-menu.md
-- 11): by what they hold, among the keys of scene._table_ActDeactRegisters,
-- the table every behaviour joins in its onInitialize. The pause's list
-- (ListUpdater: the menuData row whose callback is its own onHelp, in
-- whichever menu holds it) is all the TRAINER row needs; the Cheats screen
-- (cheatHandler: changeCheatState) only the Trainer page, and a behaviour
-- of a slide not shown yet may not have joined, so it is looked for again
-- every frame once the Trainer goes to its slide, and the page waits for it.
-- The combo needs neither: only the scene and Node.goToSlide. The list is
-- looked for every tenth frame and, past TRIES looks, every sixtieth for the
-- rest of the pause; the notes say what came of it, and qwark sends a look
-- of its own (lh_diag.lua, its answer in qwark.log) when one is a failure.
--
-- Nothing is decided here. The pages, their rows, what a row does and the
-- live values are qwark's, in C (src/core/menu.c), posted into the console
-- menu's mailbox exactly as for the four original games; the Lua helper
-- copies a page qwark finished writing and posts presses back
-- (src/games/luahelper/lh_menu.c), and this file reaches it through one C
-- function, qw(op, ...), lh_menu.h's LM_OP_*. What is here is the glue
-- the game's menu system needs:
--
--   * Options' Help row becomes TRAINER (the ListUpdater's menuData row
--     whose callback is its onHelp: OPTIONS_MENU_INDEX's fourth) while qwark
--     says so (menu.help);
--   * TRAINER, or qwark's open request (the menu combo), shows the page on
--     the game's Cheats screen (slide 16): its handler (cheatHandler) and
--     its ScrollableList draw qwark's rows, the label at the left and the
--     value at the right, the title in its header and qwark's last answer
--     in its description box;
--   * the cross, Left, Right and Triangle there become qwark's events, and
--     Triangle on qwark's root leaves for the pause's list (slide 21), as
--     the Cheats screen's own back does; a close request leaves the pause.
--
-- Everything is wrapped, never replaced: while the menu is not up every
-- wrapped function calls the game's own, so the Cheats screen is the
-- game's again the moment the menu leaves it. If anything here fails, the
-- Help row is the game's again before the error goes to the helper, which
-- stops calling qwark_frame for this pause.
--
-- Lua 5.0: no # operator, no %, string functions through `string.`, and
-- a vararg is `arg`. Comments only on lines of their own: the build drops
-- every line that starts with "--" and every line's indentation, nothing
-- else.

local Q = qw
-- The globals the pause menu's own code looks its functions up in.
local GL = _G or getfenv(0)

-- qw()'s operations (lh_menu.h).
local OP_POLL, OP_ROW, OP_TEXT, OP_EVENT, OP_STATE, OP_TAKE, OP_NOTE = 0, 1, 2, 3, 4, 5, 6
-- What the glue tells qwark's log (LM_NOTE_*): in (the Cheats screen known, or not yet), no list, no Cheats screen, the Cheats screen found late.
local NOTE_IN, NOTE_NONE, NOTE_NOCH, NOTE_IN_LIST, NOTE_LATE = 1, 2, 3, 4, 5
-- The mailbox's words (src/core/menubox.h).
local K_LINK = 1
local F_ENABLED, F_STEPS = 1, 4
local EV_ACTIVATE, EV_LEFT, EV_RIGHT, EV_BACK, EV_SCROLL_UP, EV_SCROLL_DOWN = 1, 2, 3, 4, 7, 8
local P_ROOT, P_MORE_UP, P_MORE_DOWN = 1, 2, 4
local REQ_OPEN, REQ_CLOSE = 1, 2
-- The pause menu's slides: the Cheats screen, and the list (blankHandler's).
local SLIDE_CHEATS, SLIDE_LIST = 16, 21
-- Frames of a new pause before a request is taken: the pause settles first.
local SETTLE = 15
-- The look for the pause's list: every tenth frame this many times, then every sixtieth.
local TRIES = 30
-- Frames the Trainer waits on the Cheats screen's slide for its behaviour before it gives up.
local WAITCH = 90

-- The menu's state for this pause (qwark_state: lh_diag.lua reads it).
local T = { on = false, age = 0, ready = false, tries = 0, gen = -1, vgen = -1,
            n = 1, pf = 0, seen = 0, help = -1, chw = 0 }
qwark_state = T
-- The game's own functions, as they were before the glue wrapped them.
local G = {}
-- Every function the glue put into a game's table, and the game's own it took the place of: what qwark_quit puts back.
local WO, WK, WG, NW = {}, {}, {}, 0
local S, LU, CH, TITLE, SPL, SPV

local function has(x, bit)
  return math.mod(math.floor((x or 0) / bit), 2) == 1
end

-- An Anark object: a userdata the game handed out. Nothing else is ever indexed or called.
local function obj(o)
  return type(o) == "userdata"
end

-- o[k] and o[k] = v, for pcall to call with their arguments: no closure made a call.
local function index(o, k)
  return o[k]
end

local function assign(o, k, v)
  o[k] = v
end

-- A field of an Anark object, nil for one it does not have, never an error.
local function get(o, k)
  if not obj(o) then return nil end
  local ok, v = pcall(index, o, k)
  if ok then return v end
  return nil
end

-- A method of an Anark object, called under pcall: its first result, nil for none or an error.
-- No argument is passed as none at all: tolua refuses a nil where it wants nothing (tolua_isnoobj).
local function call(o, k, a)
  local f = get(o, k)
  if type(f) ~= "function" then return nil end
  local ok, v
  if a == nil then ok, v = pcall(f, o) else ok, v = pcall(f, o, a) end
  if ok then return v end
  return nil
end

local function settext(o, s)
  if obj(o) then pcall(assign, o, "textString", s) end
end

-- o[k] = f in place of the game's g, kept so qwark_quit can put g back.
local function put(o, k, f, g)
  NW = NW + 1
  WO[NW], WK[NW], WG[NW] = o, k, g
  o[k] = f
end

-- Row i of the page (1-based): label, value, kind, flags; nil past the end.
local function row(i)
  return Q(OP_ROW, i)
end

-- Whether row i has the flag b: F_ENABLED, the cursor stands on it; F_STEPS, Left and Right step it.
local function flag(i, b)
  local l, v, k, f = row(i)
  return l ~= nil and has(f, b)
end

-- The page's own texts on the Cheats screen: its title, the last answer.
local function texts()
  local title, hint = Q(OP_TEXT)
  settext(TITLE, title)
  settext(SPL, "")
  settext(SPV, "")
  settext(get(CH, "itemDesc"), hint)
end

local function cue(event, class)
  if EffectEvent and event and class then pcall(EffectEvent, event, class) end
end

local function cursor()
  return tonumber(get(CH, "currentlySelected")) or 1
end

local function slide(n)
  pcall(G.slide, S, n)
end

-- The menu up, on the Cheats screen.
local function open()
  if T.on or not T.ready then return end
  local on, help, gen, vgen, n, cur, pt, pf = Q(OP_POLL)
  if on ~= 1 then return end
  T.on = true
  T.gen, T.vgen, T.n, T.pf, T.seen = gen, vgen, math.max(n, 1), pf, 0
  T.cur, T.wait, T.chw = cur + 1, 2, WAITCH
  T.title = T.title or get(TITLE, "textString")
  T.spl = T.spl or get(SPL, "textString")
  Q(OP_STATE, 1)
  cue(EVENT_SUCCESS, CLASS_BUTTON)
  slide(SLIDE_CHEATS)
end

-- The menu down: the Cheats screen the game's again; back to the pause's list, or nowhere.
local function leave(tolist)
  if not T.on then return end
  T.on = false
  Q(OP_STATE, 0)
  if T.title then settext(TITLE, T.title) end
  if T.spl then settext(SPL, T.spl) end
  if tolist then slide(SLIDE_LIST) end
end

-- Triangle: back a page, or out at qwark's root.
local function back()
  if has(T.pf, P_ROOT) then
    cue(EVENT_LOST_FOCUS, CLASS_MENU)
    leave(true)
  else
    Q(OP_EVENT, EV_BACK, cursor())
  end
end

-- The cross on row i.
local function activate(i)
  if flag(i, F_ENABLED) and Q(OP_EVENT, EV_ACTIVATE, i) == 1 then cue(EVENT_SUCCESS, CLASS_BUTTON) end
end

-- Left (-1) or Right (+1) on row i: a step where the row takes one.
local function step(i, d)
  if not flag(i, F_STEPS) then return end
  if d < 0 then Q(OP_EVENT, EV_LEFT, i) else Q(OP_EVENT, EV_RIGHT, i) end
end

-- The cursor came to row i: past the window's last row it can stand on, or its first, qwark slides the window.
local function scroll(i)
  if not (has(T.pf, P_MORE_UP) or has(T.pf, P_MORE_DOWN)) or i == T.seen then return end
  T.seen = i
  local before, after = false, false
  for k = 1, T.n do
    if flag(k, F_ENABLED) then
      if k < i then before = true end
      if k > i then after = true end
    end
  end
  if has(T.pf, P_MORE_DOWN) and not after then Q(OP_EVENT, EV_SCROLL_DOWN, i)
  elseif has(T.pf, P_MORE_UP) and not before then Q(OP_EVENT, EV_SCROLL_UP, i) end
end

-- The handler's methods while the menu is up; the game's own otherwise.
local function wrap(o, k, f)
  local g = get(o, k)
  if type(g) ~= "function" then return end
  put(o, k, function(self, a, b, c)
    if T.on then return f(self, a, b, c) end
    return g(self, a, b, c)
  end, g)
end

-- A global function of the game's, the same way (no vararg: those it wraps take three arguments at most).
local function wrapg(k, f)
  local g = G[k] or GL[k]
  if type(g) ~= "function" then return end
  G[k] = g
  put(GL, k, function(a, b, c)
    if T.on then return f(a, b, c) end
    return g(a, b, c)
  end, g)
end

-- The Cheats screen's own: its methods, its title and counter. Once, when its behaviour is found.
local function installCH()
  local bg = get(get(CH, "parent"), "shell_BG")
  TITLE = get(bg, "Page_name2")
  SPL = get(bg, "skillpoint_label")
  SPV = get(CH, "totalSkillpoints") or get(bg, "skillpoint_value")
  T.title = T.title or get(TITLE, "textString")
  T.spl = T.spl or get(SPL, "textString")
  wrap(CH, "getNumListItems", function(self) return T.n end)
  wrap(CH, "isSelectable", function(self, i) return flag(i, F_ENABLED) end)
  wrap(CH, "setListItemValue", function(self, i, item)
    local l, v, k = row(i)
    if l == nil then l, v = "", "" end
    if k == K_LINK and v == "" then v = ">" end
    settext(get(item, "label"), l)
    settext(get(item, "value"), v)
  end)
  wrap(CH, "handleSelect", function(self) activate(cursor()) end)
  wrap(CH, "handleRight", function(self) step(cursor(), 1) end)
  wrap(CH, "handleLeft", function(self) step(cursor(), -1) end)
  wrap(CH, "handleBack", function(self) back() end)
  -- What the screen's own handleSelect, Right and Left call: their step, for a game that bound them once.
  wrap(CH, "changeCheatState", function(self, d)
    local i = cursor()
    if flag(i, F_STEPS) then step(i, d) elseif d > 0 then activate(i) end
  end)
  local changed = get(CH, "onSelectedItemChanged")
  if type(changed) == "function" then
    put(CH, "onSelectedItemChanged", function(self, i, item, reset)
      changed(self, i, item, reset)
      if T.on and i then scroll(i) end
    end, changed)
  end
end

-- The game's globals and Node's goToSlide: once, when the scene is found.
local function installG()
  -- What the screen's own code that stays (onActivate, onUpdate, onSelectedItemChanged) asks of the game; with the menu up qwark answers.
  -- The rest of its cheat calls are in the methods wrapped above, which the list and the handler reach by name.
  wrapg("is_cheat_unlocked", function(i) return flag(i + 1, F_ENABLED) end)
  wrapg("get_cheat_desc", function(i) local t, h = Q(OP_TEXT) return h end)
  wrapg("get_current_skillpoints", function() return "" end)
  -- Back out of the Cheats screen by its slide, for a game whose Triangle reaches it so.
  G.slide = Node.goToSlide
  put(Node, "goToSlide", function(self, n)
    if T.on and n == SLIDE_LIST then back() return end
    return G.slide(self, n)
  end, G.slide)
end

-- The pause's Cheats screen, by what it holds.
local function isCH(n)
  return type(get(n, "changeCheatState")) == "function" and type(get(n, "getNumListItems")) == "function"
end

-- One look among the registry's behaviours (AnarkLibraryExtensions: every one that called _useOnActivateDeactivate) for what is still missing:
-- the list, by the menuData row whose callback is its own onHelp, in whichever menu holds it; the Cheats screen.
local function look()
  local reg = get(S, "_table_ActDeactRegisters")
  if type(reg) ~= "table" then return end
  for n in pairs(reg) do
    if obj(n) then
      local md, h = get(n, "menuData"), get(n, "onHelp")
      if not LU and type(md) == "table" and type(h) == "function" then
        for m, rows in pairs(md) do
          if type(rows) == "table" then
            for r, it in pairs(rows) do
              if type(it) == "table" and it[2] == h then LU, T.lm, T.lr, G.help = n, m, r, it end
            end
          end
        end
      end
      if not CH and isCH(n) then
        CH = n
        installCH()
        T.gen = -1
        if T.on then Q(OP_NOTE, NOTE_LATE) end
      end
    end
  end
end

-- The scene and Node first (all the combo needs), then the list and the Cheats screen, each as it turns up.
local function setup()
  T.tries = T.tries + 1
  if not LU and T.tries == TRIES * 10 then Q(OP_NOTE, NOTE_NONE) end
  if not T.ready then
    S = scene
    if not obj(S) then S = get(Presentation, "scene") end
    if not obj(S) or type(Node) ~= "table" or type(Node.goToSlide) ~= "function" then return false end
    G.trainer = { "TRAINER", function(self) open() end }
    installG()
    T.ready = true
  end
  if not (LU and CH) and ((T.on and not CH) or math.mod(T.tries, T.tries > TRIES * 10 and 60 or 10) == 1) then
    local had = LU
    look()
    if LU and not had then
      if CH then Q(OP_NOTE, NOTE_IN) else Q(OP_NOTE, NOTE_IN_LIST) end
    end
  end
  return true
end

-- Options' Help row (where the look found it): TRAINER while qwark says so, the game's Help otherwise.
local function helprow(h)
  if h == T.help or not LU then return end
  local rows = get(LU, "menuData")
  rows = type(rows) == "table" and rows[T.lm]
  if type(rows) ~= "table" then return end
  T.help = h
  if h == 1 then rows[T.lr] = G.trainer else rows[T.lr] = G.help end
end

local function frame()
  T.age = T.age + 1
  if not setup() then return end
  local on, help, gen, vgen, n, cur, pt, pf = Q(OP_POLL)
  if on ~= 1 then
    leave(true)
    helprow(0)
    return
  end
  helprow(help)
  if T.age > SETTLE then
    local req = Q(OP_TAKE)
    if req == REQ_OPEN then open()
    elseif req == REQ_CLOSE then
      leave(false)
      if exit_pause_menu then exit_pause_menu() end
    end
  end
  if not T.on then return end
  -- The Trainer on the Cheats screen's slide before its behaviour turned up: waited for, then back to the list.
  if not CH then
    T.chw = T.chw - 1
    if T.chw <= 0 then
      leave(true)
      Q(OP_NOTE, NOTE_NOCH)
    end
    return
  end
  local ls = get(CH, "listBehavior")
  if gen ~= T.gen or math.max(n, 1) ~= T.n then
    T.gen, T.vgen, T.n, T.pf, T.seen = gen, vgen, math.max(n, 1), pf, 0
    texts()
    call(ls, "resetList")
    call(ls, "setSelectedIndex", math.min(cur + 1, T.n))
  elseif vgen ~= T.vgen then
    T.vgen, T.pf = vgen, pf
    texts()
    call(ls, "updateAllItems")
  end
  -- Two frames after open() (or after the Cheats screen turned up): the screen is up, with its first row; the cursor goes where qwark keeps it.
  if T.cur then
    T.wait = T.wait - 1
    if T.wait <= 0 then
      texts()
      call(ls, "setSelectedIndex", math.min(T.cur, T.n))
      T.cur = nil
    end
  end
end

-- An error anywhere: the game's Help row back and the menu down, then the error to the helper, which stops calling this.
function qwark_frame()
  local ok, e = pcall(frame)
  if not ok then
    pcall(leave, false)
    pcall(helprow, 0)
    T.on = false
    error(e, 0)
  end
end

-- The helper's way out when the pool stays short: the menu down, the Help row and every wrapped function the game's again, qwark_frame gone.
function qwark_quit()
  pcall(leave, false)
  pcall(helprow, 0)
  T.on = false
  for i = NW, 1, -1 do
    pcall(assign, WO[i], WK[i], WG[i])
  end
  NW = 0
  qwark_frame = nil
  qwark_quit = nil
end
