-- Tools of Destruction's console menu (revision 1.29, pending): the Lua glue
-- that shows qwark's menu with the pause menu's own Anark screens.
-- docs/tod-menu.md has the design (section 12 for this layout).
--
-- The Lua helper loads this once into each pause menu's VM (its lua_State
-- at the menu system's +0x106C), on the main thread, after the menu's own
-- update, and then calls qwark_frame every frame of the pause menu.
--
-- Every Anark object here (a Node, the scene, a behaviour's self) is a
-- handle the game looks up in the presentation the Anark runtime has
-- pushed, and outside a push that presentation is NULL: a Node method then
-- reads through a NULL pointer, which no pcall catches (docs/tod-menu.md 9).
-- The helper runs this file and qwark_frame only with the pause's
-- presentation pushed, and only once the game's own checks say it is up
-- (lh_menu.c, lm_ready). What is here never calls a method on anything but
-- a userdata it got from the game, through pcall.
--
-- Memory (docs/tod-menu.md 10): the pause menu's Lua allocates from a small
-- pool that faults when a size runs out. So nothing here makes garbage frame
-- after frame: no closure per call (pcall gets a function and its
-- arguments), no vararg function, no walk of the scene. The helper counts the
-- pool before every frame and calls qwark_quit when it stays short.
--
-- What it shows (the fourth console test, docs/tod-menu.md 12):
--
--   * the pause list's Inventory row becomes TRAINER (each ListUpdater
--     menuData row whose callback is its onInventory: the main list's, and
--     the space combat and arena lists') while qwark says so (menu.help);
--     Inventory is read-only and of no use to a speedrunner, and Options'
--     Help stays the game's (it was Help's row until docs/tod-menu.md 14);
--   * a MAIN page (qwark's root, Unlocks) is a menu of the pause's own list,
--     the ListUpdater's: menuData[TR], {label, callback} as SetupMenus makes
--     its menus, shown as setOptionsText shows Options (currentMenuIndex,
--     then the list's resetList), the cross the list's own handleSelect
--     calling the row's callback;
--   * a CONFIG page (a section, Planets, a save category ...) is the game's
--     GAME OPTIONS screen (optHandler, slide 9): its six rows, label and
--     value, its title (Page_name2) the page's, its description qwark's last
--     answer; Up and Down move as its changeSelection does, the cross and
--     Right are its adjustOption(1), Left adjustOption(-1), Triangle onBack.
--
-- Sizes (the fifth console test, docs/tod-menu.md 13): the list's frame is
-- its component's slide, one a row count (the game's menus go to 3, 7 and 9
-- for their eight, six and five rows), so a MAIN page goes to the slide of
-- its own count; Game Options' rows past a page's end are hidden.
--
-- Take-over is scoped. Nothing of the game's is touched while the Trainer is
-- not up, but the Inventory row; entering it (TRAINER, or the menu combo) puts
-- the Trainer's fields and methods on the list, and showing a CONFIG page
-- puts them on Game Options, each one recorded with the game's own value
-- before it, and leaving (the root's Triangle, the combo, qwark gone, an
-- error, qwark_quit) puts every one back, in reverse, so the game's list,
-- Game Options and every other screen are the game's again. No game global
-- is ever wrapped: a tolua function of the game's takes no argument it does
-- not want (tolua_isnoobj), so a wrapper passing nils broke the game's own
-- screen (the Cheats screen, docs/tod-menu.md 12).
--
-- Nothing is decided here. The pages, their rows, what a row does and the
-- live values are qwark's, in C (src/core/menu.c), posted into the console
-- menu's mailbox; the Lua helper copies a page qwark finished writing and
-- posts presses back (lh_menu.c), and this file reaches it through one C
-- function, qw(op, ...), lh_menu.h's LM_OP_*.
--
-- Lua 5.0: no # operator, no %, string functions through `string.`, and a
-- vararg is `arg`. Comments only on lines of their own: the build drops every
-- line that starts with "--" and every line's indentation, and the spaces
-- around operators and commas (so no string literal here holds a space).

local Q = qw
local SL = SetLocalizedText

-- qw()'s operations (lh_menu.h LM_OP_*), as numbers where they are used (the fifth console test: the request's room went on the frames' sizes):
-- 0 POLL, 1 ROW, 2 TEXT, 3 EVENT, 4 STATE, 5 TAKE, 6 NOTE, 7 PRESS, 8 EDGE, 9 NEXT.
-- The mailbox's events (src/core/menubox.h MBOX_EV_*), the same way: 1 ACTIVATE, 2 LEFT, 3 RIGHT, 4 BACK.
-- The numbers below, named where they are used (a request holds 8,192 bytes, so no local names them):
-- LM_NOTE_* for qwark's log, 1 in (Game Options known), 2 no list, 3 no Game Options screen, 4 in (Game Options not met yet), 5 Game Options met late;
-- the Inventory rows found, T.hr: { the menu's rows, the row, the game's own item } each (lh_diag.lua reads it);
-- K_LINK 1 (menubox.h); REQ_OPEN 1, REQ_CLOSE 2;
-- the pause's slides: 9 Game Options (onGameOptions), 1 its own back (optHandler's onBack), 21 the blank that brings the list back (the Cheats screen's back);
-- 6 the Trainer's menu in the list's menuData (the game's are 1 to 5);
-- 300 frames of looks for the pause's list every tenth frame, then every sixtieth; 90 frames the Trainer waits on Game Options' slide for its behaviour.
local SP = string.char(32)

-- The menu's state for this pause (qwark_state: lh_diag.lua reads it).
local T = { age = 0, tries = 0, gen = -1, vgen = -1, n = 1, pf = 0, help = -1, hr = {} }
qwark_state = T
local G = {}
-- Everything the glue put into a game's object, and the game's own value it took the place of: what leaving puts back.
local WO, WK, WG, NW = {}, {}, {}, 0
local S, LU, LS, O
local back, leave

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

-- A field of an Anark object (or a game's table), nil for one it does not have, never an error.
local function get(o, k)
  if type(o) == "table" then return o[k] end
  if not obj(o) then return nil end
  local ok, v = pcall(index, o, k)
  if ok then return v end
  return nil
end

-- A method of an Anark object, called under pcall with what it is given and nothing more (tolua refuses a nil it does not want).
local function call(o, k, a, b)
  local f = get(o, k)
  if type(f) ~= "function" then return nil end
  if a == nil then pcall(f, o) elseif b == nil then pcall(f, o, a) else pcall(f, o, a, b) end
end

-- A text node's text, as the game's own code sets it (SetLocalizedText: a string is shown as it is).
local function text(o, s)
  if not obj(o) then return end
  if type(SL) == "function" then pcall(SL, o, s) else pcall(assign, o, "textString", s) end
end

-- o[k] = v, the game's value recorded first.
local function put(o, k, v)
  NW = NW + 1
  WO[NW], WK[NW], WG[NW] = o, k, get(o, k)
  pcall(assign, o, k, v)
end

-- Everything put since mark `to` back as the game had it, the last first.
local function unwind(to)
  for i = NW, to + 1, -1 do
    pcall(assign, WO[i], WK[i], WG[i])
    WO[i], WK[i], WG[i] = nil, nil, nil
  end
  NW = to
end

-- Row i of the page (1-based): label, value, kind, flags; nil past the end.
local function row(i)
  return Q(1, i)
end

local function cue(event, class)
  if EffectEvent and event and class then pcall(EffectEvent, event, class) end
end

local function slide(n)
  pcall(G.slide, S, n)
end

-- Whether a behaviour is up (the registry's value: its globalactive at the last update).
local function active(n)
  local reg = get(S, "_table_ActDeactRegisters")
  return type(reg) == "table" and reg[n] == true
end

-- The cross on row i, posted where the row takes it (lh_menu.c, lm_press); a cue where the screen plays none of its own.
local function activate(i, q)
  if Q(7, 1, i) == 1 and q then cue(EVENT_SUCCESS, CLASS_BUTTON) end
end

-- The cursor came to row i: at the window's edge of a page that goes on, qwark slides the window (lh_menu.c, lm_edge); once a row.
local function scroll(i)
  if i == T.seen then return end
  T.seen = i
  Q(8, i)
end

-- ------------------------------------------------------- the pause's list

-- A MAIN page as the Trainer's menu: {label, callback} a row, as SetupMenus makes the game's; a value after its label.
local function lfill()
  local t = {}
  for i = 1, T.n do
    local l, v = row(i)
    l = l or ""
    if v and v ~= "" then l = l .. ":" .. SP .. v end
    t[i] = { l, G.act }
  end
  local md = get(LU, "menuData")
  if type(md) == "table" then md[6] = t end
end

-- The list's cursor on row i, quietly (setSelectedIndex as a reset).
local function lsel(i)
  call(LS, "setSelectedIndex", math.min(i, T.n), true)
end

-- The list's frame for 1 to 9 rows: the slide of the list's own component (the ListUpdater's parent) that frames that many, the next larger for four rows or fewer than three (its slides 1, 3, 5, 7, 9 and 11 frame nine, eight, seven, six, five and three).
local LSL = { 11, 11, 11, 9, 9, 7, 5, 3, 1 }

-- The list on slide s as the game's doScreenChange puts it there (the slide, and currentSlide, which the list's onActivate goes back to).
local function lframe(s)
  if s and s ~= get(LU, "currentSlide") then
    pcall(assign, LU, "currentSlide", s)
    pcall(G.slide, get(LU, "parent"), s)
  end
end

-- The list on the Trainer's take-over: its back, its cursor's moves, its menu; each recorded.
local function lon()
  T.ci = get(LU, "currentIndex")
  T.sl = get(LU, "currentSlide")
  put(LU, "onMenuBack", G.back)
  local changed = get(LU, "onSelectedItemChanged")
  if type(changed) == "function" then
    put(LU, "onSelectedItemChanged", function(self, i, item, reset)
      changed(self, i, item, reset)
      if T.on and not T.om and i and not reset then scroll(i) end
    end)
  end
  put(get(LU, "menuData"), 6, nil)
  put(LU, "currentMenuIndex", 6)
end

-- -------------------------------------------------------- Game Options

-- Game Options' rows, all (the game's onActivate) or row i (its changeSelection): label, value (">" for a link), no bar.
local function oitem(self, i)
  local rw = get(O, "row")
  if type(rw) ~= "table" then return end
  for k = i or 1, i or table.getn(rw) do
    local r = rw[k]
    local l, v, kd = row(k)
    if l == nil then l, v = "", "" elseif kd == 1 and v == "" then v = ">" end
    text(get(r, "optLabel"), l)
    text(get(r, "optValue"), v)
    local b = get(r, "bar")
    if obj(b) then pcall(assign, b, "opacity", 0) end
  end
end

-- Game Options' rows past the first n hidden, the others shown (n 9: all, as the game has them).
local function orows(n)
  local rw = get(O, "row")
  if type(rw) == "table" then
    for k = 1, table.getn(rw) do pcall(assign, rw[k], "opacity", k > n and 0 or 100) end
  end
end

-- The page's own texts there: the title, qwark's last answer in the description, and in each option's (changeSelection shows it).
local function otexts()
  local t, h = Q(2)
  text(G.title, t)
  text(get(O, "desc"), h)
  local op = get(O, "option")
  if type(op) == "table" then
    for k = 1, table.getn(op) do op[k].desc = h end
  end
end

-- Game Options showing the window, the cursor on row c: false while its rows are not built yet.
local function oshow(c)
  local rw = get(O, "row")
  if type(rw) ~= "table" then return false end
  local op = {}
  for k = 1, math.max(T.n, table.getn(rw)) do op[k] = { enabled = true } end
  pcall(assign, O, "option", op)
  pcall(assign, O, "max", math.min(T.n, table.getn(rw)))
  orows(T.n)
  otexts()
  oitem(O)
  pcall(G.osel, O, math.min(c, T.n) - (tonumber(get(O, "currentSelection")) or 1))
  return true
end

-- Up or Down there: to the next row the cursor stands on (lh_menu.c, lm_next), as the game's changeSelection moves it; a page at a time: where the cursor would leave the window, lm_next asks qwark for the next one and the cursor stays until it comes (docs/tod-menu.md 15).
local function osel(self, d)
  local i = tonumber(get(O, "currentSelection")) or 1
  local j = Q(9, i, d, tonumber(get(O, "max")) or 1)
  if j ~= i then pcall(G.osel, O, j - i) end
end

-- Game Options on the Trainer's take-over: its options (the game's onActivate builds its rows of them), its methods, its title, Node.goToSlide; each recorded.
local function oon()
  T.om = true
  T.omk = NW
  put(O, "optArray", G.arr)
  put(O, "option", get(O, "option"))
  put(O, "max", get(O, "max"))
  put(O, "updateItemData", oitem)
  put(O, "changeSelection", osel)
  put(O, "adjustOption", function(self, d)
    local i = tonumber(get(O, "currentSelection")) or 1
    if Q(7, d < 0 and 2 or 3, i) ~= 1 and d > 0 then activate(i, true) end
  end)
  put(O, "onBack", G.back)
  put(G.title, "textString", get(G.title, "textString"))
  put(Node, "goToSlide", function(self, n)
    if T.om == true and n == 1 then return back() end
    return G.slide(self, n)
  end)
end

-- Game Options the game's again.
local function ooff()
  orows(9)
  if T.om then unwind(T.omk) end
  T.om = nil
end

-- ------------------------------------------------------------ the pages

-- A new page (or window): a MAIN one in the list, a CONFIG one on Game Options.
local function page(gen, vgen, n, cur, pt, pf)
  T.gen, T.vgen, T.n, T.pf, T.pt, T.seen, T.cur = gen, vgen, math.max(n, 1), pf, pt, 0, cur + 1
  if pt == 0 then
    if T.om then
      ooff()
      slide(1)
      T.wait = 2
    end
    lframe(LSL[math.min(T.n, 9)])
    lfill()
    call(LS, "resetList")
    lsel(T.cur)
  elseif T.om == true then
    oshow(T.cur)
  elseif not T.om then
    if O then oon() else T.om, T.omk, T.ow = "wait", NW, 90 end
    slide(9)
    T.wait = 2
  end
end

-- The same page's values changed.
local function values(vgen, pf)
  T.vgen, T.pf = vgen, pf
  if T.om == true then
    otexts()
    oitem(O)
  elseif not T.om then
    lfill()
    call(LS, "updateAllItems")
  end
end

-- Two frames after a screen changed: the cursor where qwark keeps it (a screen's activation puts its own first).
local function place()
  if T.om == true then
    if not oshow(T.cur) then T.wait = 1 end
  elseif not T.om then
    lsel(T.cur)
  end
end

-- The menu up: the Trainer's menu in the pause's list (the list brought back first if a screen of the game's is up); Triangle at its root goes back to the list it came from, its frame and the cursor on TRAINER (leave).
local function open()
  if T.on or not LU then return end
  local on, help, gen, vgen, n, cur, pt, pf = Q(0)
  if on ~= 1 then return end
  T.on = true
  Q(4, 1)
  lon()
  if not active(LU) then
    slide(21)
    T.wait = 2
  end
  page(gen, vgen, n, cur, pt, pf)
end

-- The menu down: everything the game's again; back to the list it was opened from, or nowhere.
leave = function(nav)
  if not T.on then return end
  T.on = false
  Q(4, 0)
  local om = T.om
  ooff()
  unwind(0)
  lframe(T.sl)
  T.wait = nil
  if nav then
    if om then slide(1) end
    call(LS, "resetList")
    if T.ci then call(LS, "setSelectedIndex", T.ci, true) end
  end
end

-- Triangle: back a page, or out at qwark's root.
back = function()
  if math.mod(T.pf, 2) == 1 then
    cue(EVENT_LOST_FOCUS, CLASS_MENU)
    leave(true)
  else
    Q(3, 4, 1)
  end
end

-- ------------------------------------------------------------- the look

-- One look among the registry's behaviours (AnarkLibraryExtensions: every one that called _useOnActivateDeactivate) for what is still missing:
-- the list, by the menuData rows whose callback is its own onInventory, in whichever menus hold them; Game Options (optHandler), by its optArray.
local function look()
  local reg = get(S, "_table_ActDeactRegisters")
  if type(reg) ~= "table" then return end
  for n in pairs(reg) do
    if obj(n) then
      local md, h = get(n, "menuData"), get(n, "onInventory")
      if not LU and type(md) == "table" and type(h) == "function" then
        for m, rows in pairs(md) do
          if type(rows) == "table" then
            for r, it in pairs(rows) do
              if type(it) == "table" and it[2] == h then
                LU, LS = n, get(n, "listBehavior")
                table.insert(T.hr, { rows, r, it })
              end
            end
          end
        end
      end
      if not O and type(get(n, "optArray")) == "table" and type(get(n, "changeSelection")) == "function" then
        O, G.osel = n, get(n, "changeSelection")
        G.title = get(get(get(n, "parent"), "shell_BG"), "Page_name2")
      end
    end
  end
end

-- The scene and Node first, then the list and Game Options, each as it turns up.
local function setup()
  T.tries = T.tries + 1
  if not LU and T.tries == 300 then Q(6, 2) end
  if not T.ready then
    S = scene
    if not obj(S) then S = get(Presentation, "scene") end
    if not obj(S) or type(Node) ~= "table" or type(Node.goToSlide) ~= "function" then return false end
    G.slide = Node.goToSlide
    G.trainer = { "TRAINER", function(self) open() end }
    G.act = function(self) activate(tonumber(get(LU, "currentIndex")) or 1) end
    G.back = function(self) back() end
    G.arr = {}
    for k = 1, 8 do G.arr[k] = { enabled = true, desc = "" } end
    T.ready = true
  end
  if not (LU and O) and (T.om == "wait" or math.mod(T.tries, T.tries > 300 and 60 or 10) == 1) then
    local had = LU
    look()
    if LU and not had then
      Q(6, O and 1 or 4)
    end
  end
  return true
end

-- The Inventory rows (where the look found them): TRAINER while qwark says so (menu.help), the game's own Inventory otherwise;
-- then the list's own redraw (its updateAllItems: each row's text from menuData), so the row reads right at once, not only after the cursor's next move (docs/tod-menu.md 15).
local function helprow(h)
  if h == T.help or not LU then return end
  T.help = h
  for _, x in pairs(T.hr) do
    if h == 1 then x[1][x[2]] = G.trainer else x[1][x[2]] = x[3] end
  end
  call(LS, "updateAllItems")
end

local function frame()
  T.age = T.age + 1
  if not setup() then return end
  local on, help, gen, vgen, n, cur, pt, pf = Q(0)
  if on ~= 1 then
    leave(true)
    helprow(0)
    return
  end
  helprow(help)
  -- qwark's request: taken once the pause's list is up (or the Trainer is), so the combo's pause opens on the Trainer as soon as it can; from a screen of the game's once the pause has settled (30 frames).
  if T.on or (LU and (active(LU) or T.age > 30)) then
    local req = Q(5)
    if req == 1 then open()
    elseif req == 2 then
      leave(false)
      if exit_pause_menu then exit_pause_menu() end
    end
  end
  if not T.on then return end
  -- A CONFIG page before Game Options' behaviour turned up (it joins on its slide's first showing): waited for, then back to the list.
  if T.om == "wait" then
    if O then
      T.om = nil
      oon()
      T.wait = 1
      Q(6, 5)
    else
      T.ow = T.ow - 1
      if T.ow <= 0 then
        leave(true)
        Q(6, 3)
      end
      return
    end
  end
  if gen ~= T.gen or math.max(n, 1) ~= T.n or pt ~= T.pt then
    page(gen, vgen, n, cur, pt, pf)
  elseif vgen ~= T.vgen then
    values(vgen, pf)
  end
  if T.wait then
    T.wait = T.wait - 1
    if T.wait <= 0 then
      T.wait = nil
      place()
    end
  end
end

-- An error anywhere: everything the game's again and the menu down, then the error to the helper, which stops calling this.
function qwark_frame()
  local ok, e = pcall(frame)
  if not ok then
    pcall(leave, false)
    pcall(helprow, 0)
    T.on = false
    unwind(0)
    error(e, 0)
  end
end

-- The helper's way out when the pool stays short: the menu down, the Inventory rows and everything put the game's again, qwark_frame gone.
function qwark_quit()
  pcall(leave, false)
  pcall(helprow, 0)
  T.on = false
  unwind(0)
  qwark_frame = nil
  qwark_quit = nil
end
