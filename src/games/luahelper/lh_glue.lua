-- Tools of Destruction's console menu (revision 1.29, pending): the Lua glue
-- that shows qwark's menu with the pause menu's own Anark screens.
-- docs/tod-menu.md has the design.
--
-- The Lua helper loads this once into each pause menu's VM (its lua_State
-- at the menu system's +0x106C), on the main thread, after the menu's own
-- update, and then calls qwark_frame every frame of the pause menu.
--
-- Nothing is decided here. The pages, their rows, what a row does and the
-- live values are qwark's, in C (src/core/menu.c), posted into the console
-- menu's mailbox exactly as for the four original games; the Lua helper
-- copies a page qwark finished writing and posts presses back
-- (src/games/luahelper/lh_menu.c), and this file reaches it through one C
-- function, qw(op, ...), lh_menu.h's LM_OP_*. What is here is the glue
-- the game's menu system needs:
--
--   * Options' Help row becomes TRAINER (pause.ListUpdater's menuData,
--     OPTIONS_MENU_INDEX, row 4) while qwark says so (menu.help);
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
-- game's again the moment the menu leaves it.
--
-- Lua 5.0: no # operator, no %, string functions through `string.`, and
-- a vararg is `arg`. Comments only on lines of their own: the build drops
-- every line that starts with "--" and every line's indentation, nothing
-- else.

local Q = qw
-- The globals the pause menu's own code looks its functions up in.
local GL = _G or getfenv(0)

-- qw()'s operations (lh_menu.h).
local OP_POLL, OP_ROW, OP_TEXT, OP_EVENT, OP_STATE, OP_TAKE = 0, 1, 2, 3, 4, 5
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

-- The menu's state for this pause.
local T = { on = false, age = 0, ready = false, tries = 0, gen = -1, vgen = -1,
            n = 1, pf = 0, seen = 0, help = -1 }
-- The game's own functions, as they were before the glue wrapped them.
local G = {}
local S, LU, CH, TITLE, SPL, SPV

local function has(x, bit)
  return math.mod(math.floor((x or 0) / bit), 2) == 1
end

-- A field of an Anark object, nil for one it does not have, never an error.
local function get(o, k)
  local ok, v = pcall(function() return o[k] end)
  if ok then return v end
  return nil
end

local function settext(o, s)
  if o then pcall(function() o.textString = s end) end
end

local function gettext(o)
  if o then return get(o, "textString") end
  return nil
end

-- Every node under n, depth first, until f answers true.
local function walk(n, depth, f)
  if f(n) then return true end
  if depth > 24 then return false end
  local c = get(n, "getNumChildren") and n:getNumChildren() or 0
  for i = 1, c do
    local ch = n:getChildByIndex(i)
    if ch and walk(ch, depth + 1, f) then return true end
  end
  return false
end

local function find(root, name)
  local found
  walk(root, 0, function(n)
    if get(n, "name") == name then found = n return true end
    return false
  end)
  return found
end

-- Row i of the page (1-based): label, value, kind, flags; nil past the end.
local function row(i)
  return Q(OP_ROW, i)
end

local function enabled(i)
  local l, v, k, f = row(i)
  return l ~= nil and has(f, F_ENABLED)
end

local function steps(i)
  local l, v, k, f = row(i)
  return l ~= nil and has(f, F_STEPS)
end

-- The page's own texts on the Cheats screen: its title, the last answer.
local function texts()
  local title, hint = Q(OP_TEXT)
  settext(TITLE, title)
  settext(SPL, "")
  settext(SPV, "")
  if CH and CH.itemDesc then settext(CH.itemDesc, hint) end
end

local function cue(event, class)
  if EffectEvent and event and class then pcall(EffectEvent, event, class) end
end

local function cursor()
  return (CH and CH.currentlySelected) or 1
end

-- The menu up, on the Cheats screen.
local function open()
  if T.on or not T.ready then return end
  local on, help, gen, vgen, n, cur, pt, pf = Q(OP_POLL)
  if on ~= 1 then return end
  T.on = true
  T.gen, T.vgen, T.n, T.pf, T.seen = gen, vgen, math.max(n, 1), pf, 0
  T.cur = cur + 1
  T.title = T.title or gettext(TITLE)
  T.spl = T.spl or gettext(SPL)
  Q(OP_STATE, 1)
  cue(EVENT_SUCCESS, CLASS_BUTTON)
  G.slide(S, SLIDE_CHEATS)
end

-- The menu down: the Cheats screen the game's again; back to the pause's list, or nowhere.
local function leave(tolist)
  if not T.on then return end
  T.on = false
  Q(OP_STATE, 0)
  if T.title then settext(TITLE, T.title) end
  if T.spl then settext(SPL, T.spl) end
  if tolist then G.slide(S, SLIDE_LIST) end
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
  if enabled(i) and Q(OP_EVENT, EV_ACTIVATE, i) == 1 then cue(EVENT_SUCCESS, CLASS_BUTTON) end
end

-- Left (-1) or Right (+1) on row i: a step where the row takes one.
local function step(i, d)
  if not steps(i) then return end
  if d < 0 then Q(OP_EVENT, EV_LEFT, i) else Q(OP_EVENT, EV_RIGHT, i) end
end

-- The cursor came to row i: past the window's last row it can stand on, or its first, qwark slides the window.
local function slide(i)
  if not (has(T.pf, P_MORE_UP) or has(T.pf, P_MORE_DOWN)) or i == T.seen then return end
  T.seen = i
  local before, after = false, false
  for k = 1, T.n do
    if enabled(k) then
      if k < i then before = true end
      if k > i then after = true end
    end
  end
  if has(T.pf, P_MORE_DOWN) and not after then Q(OP_EVENT, EV_SCROLL_DOWN, i)
  elseif has(T.pf, P_MORE_UP) and not before then Q(OP_EVENT, EV_SCROLL_UP, i) end
end

-- The handler's methods while the menu is up; the game's own otherwise.
local function wrap(o, k, f)
  local g = o[k]
  if type(g) ~= "function" then return end
  o[k] = function(self, a, b, c)
    if T.on then return f(self, a, b, c) end
    return g(self, a, b, c)
  end
end

-- A global function of the game's, the same way.
local function wrapg(k, f)
  local g = G[k] or GL[k]
  if type(g) ~= "function" then return end
  G[k] = g
  GL[k] = function(...)
    if T.on then return f(unpack(arg)) end
    return g(unpack(arg))
  end
end

local function install()
  wrap(CH, "getNumListItems", function(self) return T.n end)
  wrap(CH, "isSelectable", function(self, i) return enabled(i) end)
  wrap(CH, "setListItemValue", function(self, i, item)
    local l, v, k = row(i)
    if l == nil then l, v = "", "" end
    if k == K_LINK and v == "" then v = ">" end
    if item then
      settext(get(item, "label"), l)
      settext(get(item, "value"), v)
    end
  end)
  wrap(CH, "handleSelect", function(self) activate(cursor()) end)
  wrap(CH, "handleRight", function(self) step(cursor(), 1) end)
  wrap(CH, "handleLeft", function(self) step(cursor(), -1) end)
  wrap(CH, "handleBack", function(self) back() end)
  -- What the screen's own handleSelect, Right and Left call: their step, for a game that bound them once.
  wrap(CH, "changeCheatState", function(self, d)
    local i = cursor()
    if steps(i) then step(i, d) elseif d > 0 then activate(i) end
  end)
  local changed = CH.onSelectedItemChanged
  if type(changed) == "function" then
    CH.onSelectedItemChanged = function(self, i, item, reset)
      changed(self, i, item, reset)
      if T.on and i then slide(i) end
    end
  end
  -- The screen's own code asks these of the game; with the menu up qwark answers.
  wrapg("get_num_cheats", function() return T.n end)
  wrapg("is_cheat_unlocked", function(i) return enabled(i + 1) end)
  wrapg("get_cheat_name", function(i) local l = row(i + 1) return l or "" end)
  wrapg("get_cheat_desc", function(i) local t, h = Q(OP_TEXT) return h end)
  wrapg("get_cheat_value", function(i) local l, v = row(i + 1) return v or "" end)
  wrapg("get_cheat_state", function(i) return 1 end)
  wrapg("get_num_cheat_states", function(i) return 3 end)
  wrapg("get_cheat_state_name", function(i, s) local l, v = row(i + 1) return v or "" end)
  wrapg("set_cheat_state", function(i, s)
    if s == 1 then return end
    if s == 2 then step(i + 1, 1) else step(i + 1, -1) end
  end)
  wrapg("get_current_skillpoints", function() return "" end)
  -- Back out of the Cheats screen by its slide, for a game whose Triangle reaches it so.
  G.slide = Node.goToSlide
  Node.goToSlide = function(self, n)
    if T.on and n == SLIDE_LIST then back() return end
    return G.slide(self, n)
  end
end

-- The pause's nodes, once they are all there; tried every tenth frame.
local function setup()
  if T.ready then return true end
  T.tries = T.tries + 1
  if math.mod(T.tries, 10) ~= 1 then return false end
  S = scene or (Presentation and Presentation.scene)
  if not S or not Node or type(Node.goToSlide) ~= "function" then return false end
  local lu, ch
  walk(S, 0, function(n)
    local name = get(n, "name")
    if name == "cheatHandler" and type(get(n, "changeCheatState")) == "function" then ch = n
    elseif name == "ListUpdater" and type(get(n, "menuData")) == "table" and get(n, "OPTIONS_MENU_INDEX") then lu = n end
    return lu ~= nil and ch ~= nil
  end)
  if not (lu and ch) then return false end
  local options = lu.menuData[lu.OPTIONS_MENU_INDEX]
  if type(options) ~= "table" or type(options[4]) ~= "table" or options[4][2] ~= lu.onHelp then return false end
  LU, CH = lu, ch
  local screen = get(CH, "parent") or S
  TITLE = find(screen, "Page_name2")
  SPL = find(screen, "skillpoint_label")
  SPV = find(screen, "skillpoint_value")
  G.help = options[4]
  G.trainer = { "TRAINER", function(self) open() end }
  install()
  T.ready = true
  return true
end

-- Options' fourth row: TRAINER while qwark says so, the game's Help otherwise.
local function helprow(h)
  if h == T.help then return end
  T.help = h
  local options = LU.menuData[LU.OPTIONS_MENU_INDEX]
  if h == 1 then options[4] = G.trainer else options[4] = G.help end
end

function qwark_frame()
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
  local ls = get(CH, "listBehavior")
  if gen ~= T.gen or math.max(n, 1) ~= T.n then
    T.gen, T.vgen, T.n, T.pf, T.seen = gen, vgen, math.max(n, 1), pf, 0
    texts()
    if ls then
      ls:resetList()
      ls:setSelectedIndex(math.min(cur + 1, T.n))
    end
  elseif vgen ~= T.vgen then
    T.vgen, T.pf = vgen, pf
    texts()
    if ls then ls:updateAllItems() end
  end
  -- The frame after open(): the Cheats screen is up, with its first row; the cursor goes where qwark keeps it.
  if T.cur then
    texts()
    if ls then ls:setSelectedIndex(math.min(T.cur, T.n)) end
    T.cur = nil
  end
end
