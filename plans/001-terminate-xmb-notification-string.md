# Plan 001: An XMB notification's wide string is always terminated

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 3416191..HEAD -- src/plat/ps3/vsh_notify.h`
> If the file changed since this plan was written, compare the "Current
> state" excerpt against the live code before proceeding; on a mismatch, treat
> it as a STOP condition.

## Status

- **Priority**: P3
- **Effort**: S
- **Risk**: LOW
- **Depends on**: none
- **Category**: bug
- **Planned at**: commit `3416191`, 2026-11-10

## Why this matters

qwark is a PS3 plugin. When it shows a notification while the console is at
the XMB (not in a game), it converts the message to a wide string in a stack
buffer and hands that to the console's `NotifyWithTexture`. For a message of
200 bytes or more the conversion fills the buffer's first 199 elements and
writes **no terminator**, and the buffer's last element is never initialised.
The console then reads past the end of the text into whatever is on the
thread's stack until it meets a zero: junk at the end of the toast, and a read
beyond a stack buffer.

The wire protocol lets a client send exactly such a message: `NOTIFY` carries
"text, at most 255 bytes" (`docs/PROTOCOL.md`, op 0x0003), and
`src/core/net.c:1133-1141` passes up to 255 bytes straight to this function.
The fix is one line: zero the buffer before converting into it.

## Current state

All paths are relative to the repository root (`qwark/`).

- `src/plat/ps3/vsh_notify.h` — 46 lines, one function, `vshNotify_Game`,
  called only by `plat_notify` in `src/plat/ps3/plat_ps3.c:116-119`. It is
  PS3-only code: the host builds and the host tests never compile it.

The whole function today (`src/plat/ps3/vsh_notify.h:25-43`):

```c
static __attribute__((unused)) int32_t vshNotify_Game(const char *msg)
{
	if(IS_INGAME)
		return vshtask_notify(0, msg);

	uint32_t _plugin = View_Find(VSH_NOTIFY_GAME_PLUGIN);
	if (_plugin <= 0)
		return FAILED;

	int len = strlen(msg); if(len >= 200) len = 198;

	wchar_t message[len + 2];

	mbstowcs((wchar_t *)message, (const char *)msg, len + 1);  //size_t stdc_FCAC2E8E(wchar_t *dest, const char *src, size_t max)

	int teximg, dummy = 0;
	LoadRCOTexture(&teximg, _plugin, VSH_NOTIFY_GAME_TEXTURE);
	return NotifyWithTexture(0, VSH_NOTIFY_GAME_TEXTURE, 0, &teximg, &dummy, "", "", 0, message, 0, 0, 0);
}
```

How the bug arises, so you can check your understanding against the code:

- `mbstowcs(dest, src, n)` writes at most `n` wide characters and adds a
  terminator only if it reaches the end of `src` within those `n`.
- For a message shorter than 200 bytes, `len` is its length, `n` is `len + 1`,
  and the terminator fits at index `len`. Correct.
- For a message of 200 to 255 bytes, `len` becomes 198, the buffer has 200
  elements (indices 0–199), `n` is 199, and `mbstowcs` fills indices 0–198
  with text and stops. Index 199 is never written. No terminator.
- A message with bytes `mbstowcs` cannot convert makes it stop early, also
  without a terminator.

The sender that can reach this from the network (`src/core/net.c:1133-1141`,
for context only — **do not change it**):

```c
	case OP_NOTIFY: {
		char msg[256];
		u32 n = reqlen;
		if (n > 255) n = 255;
		memcpy(msg, req, n);
		msg[n] = 0;
		plat_notify(msg);
		return ST_OK;
	}
```

**Conventions and constraints** (from `CLAUDE.md`):

- The compiler is ppu-lv2-gcc 4.1.1: gnu99 only. No C11, no `_Static_assert`.
- No `malloc`, no new static storage: the module asks the console for no
  memory after it has loaded. The fix below uses neither.
- This header keeps the style of the code it was derived from (tabs, `if(`
  without a space in places). Match the lines around your change; do not
  reformat the function.
- Comments in this codebase are full sentences that say *why*.
- "Do not commit or add attribution trailers unless asked."
- "Delete the `.prx` and `.sym` leftovers after a build."

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| PS3 build (from Windows `cmd`) | `_Make.bat` (run in the repository root) | ends without a compiler error; `qwark.sprx` in the root has a new timestamp |
| PS3 build (from Git Bash) | the command in the block below | same |
| Host tests (from Git Bash, repository root) | `sh test/run.sh` | exit 0; the last line is `installer: 1307 passed, 0 failed`; no line starts with `  FAIL` |

The PS3 build from Git Bash, as `CLAUDE.md` gives it, with the leftovers removed:

```
/c/cygwin64/bin/bash.exe --login -c "cd '/cygdrive/c/Users/atune/Documents/RaCMAN Development/rewrite/qwark' && make && rm -f qwark.prx qwark.sym"
```

The PS3 build needs the PS3 SDK and Cygwin, which the maintainer's machine
has. `_Make.bat` removes the `.prx` and `.sym` leftovers itself. A plain
`make` builds a *test* module; that is what you want. Its outputs
(`qwark.sprx`, `qwark.build`, `ui.pak`, `rac*.sprx`, `tod.sprx`) are all
git-ignored.

The host tests take a few minutes and need
`C:/ghcup/ghc/9.4.7/mingw/bin/clang.exe` and Python on `PATH`. They passed on
commit `3416191` (verified when this plan was written).

## Scope

**In scope** (the only file you should modify):

- `src/plat/ps3/vsh_notify.h`

**Out of scope** (do NOT touch, even though they look related):

- `src/core/net.c` — the 255-byte limit on `NOTIFY` is the wire contract
  (`docs/PROTOCOL.md`); the fix belongs where the buffer is.
- The `IS_INGAME` branch (`vshtask_notify(0, msg)`): it passes a plain
  `char *` and is not affected.
- The `len >= 200` cap and the buffer size: leave both as they are.
- `dist/` — the committed release binaries. Do **not** run `make dist`,
  `make release` or `make pkg`. Shipping this fix is the maintainer's release
  step, with its own build-number rules.
- `docs/PROTOCOL.md`, `README.md`, `CLAUDE.md` — nothing a client can observe
  changes.

## Git workflow

- The repository rule is: **do not commit unless the operator asks**. Leave
  your change in the working tree.
- If the operator does ask for a commit: work on `main` is the norm here, the
  message is one plain sentence describing what is now true, with no
  attribution trailers. Example from `git log`:
  `A mod switched off stays off, and the Mods page no longer refuses a mod over one it shares no byte with`.
- Do NOT push.

## Steps

### Step 1: Zero the buffer before the conversion

In `src/plat/ps3/vsh_notify.h`, between the declaration of `message` (line 36)
and the `mbstowcs` call (line 38), add a `memset` with a comment that says
why, so that the result is:

```c
	wchar_t message[len + 2];

	/*
	 * mbstowcs writes no terminator when the text has more wide characters than
	 * it was given room for, which is every message of 200 bytes or more (NOTIFY
	 * carries up to 255), nor when it meets a byte it cannot convert. The buffer
	 * is one element longer than the most it writes, so zeroed first it is
	 * terminated whatever the conversion does.
	 */
	memset(message, 0, sizeof(message));

	mbstowcs((wchar_t *)message, (const char *)msg, len + 1);  //size_t stdc_FCAC2E8E(wchar_t *dest, const char *src, size_t max)
```

Change nothing else in the function. `sizeof(message)` on this
variable-length array is evaluated at run time and is the array's full size in
bytes, which is what is wanted. `memset` is already available here: the only
file that includes this header, `src/plat/ps3/plat_ps3.c`, includes
`<string.h>` (line 24), and the module's own `memset` is in
`src/plat/ps3/libc.c:20`.

**Verify**: `grep -n "memset(message, 0, sizeof(message));" src/plat/ps3/vsh_notify.h`
→ exactly one match, on a line before the `mbstowcs` line.
`git diff --stat` → one file changed, insertions only.

### Step 2: Build the PS3 module

Run the PS3 build (see "Commands you will need").

**Verify**: the build ends without an error or a new warning that names
`vsh_notify.h`, and `ls -l qwark.sprx` shows a timestamp from this build.
Then `ls qwark.prx qwark.sym` → both absent (remove them if present).

### Step 3: Host tests and a clean tree

**Verify**: `sh test/run.sh` → exit 0, last line `installer: 1307 passed, 0 failed`.
Then `git status --short` → ` M src/plat/ps3/vsh_notify.h` and the `plans/`
folder (shown as `?? plans/` unless the operator has committed it). Anything
else listed is a STOP condition.

## Test plan

There is no automated test for this change and none is to be added: the
function exists only in the PS3 build, calls console firmware, and cannot be
compiled on the host. The gates are the PS3 compile (step 2) and the unchanged
host suite (step 3).

For the maintainer, on a console (not part of the executor's done criteria):
at the XMB, send a `NOTIFY` of 200 or more characters from a client and check
that the toast shows the first 199 characters and nothing after them. Before
this fix the same message may show stray characters at its end.

## Done criteria

ALL must hold:

- [ ] `grep -c "memset(message, 0, sizeof(message));" src/plat/ps3/vsh_notify.h` prints `1`
- [ ] The PS3 build completed without errors after the change
- [ ] `sh test/run.sh` exits 0
- [ ] `git status --short` lists only `src/plat/ps3/vsh_notify.h` and the `plans/` folder, and no `.prx` or `.sym` file exists in the repository root
- [ ] `git diff -- dist/` is empty
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back (do not improvise) if:

- `vshNotify_Game` does not match the excerpt in "Current state".
- The PS3 build cannot be run where you are (no Cygwin, no SDK). Make the
  edit, run the host tests, and report the plan as **BLOCKED: PS3 compile not
  verified** in `plans/README.md` rather than DONE.
- The PS3 build fails in a file other than `vsh_notify.h`, or reports that
  `memset` is undeclared there.
- After the build, `git status --short` lists any tracked file other than
  `src/plat/ps3/vsh_notify.h` (the build regenerated a committed file, for
  example one of `src/games/*_bins.c`). Do not commit or revert it yourself;
  report it.
- You are tempted to change the length cap, the buffer size, or `net.c`.

## Maintenance notes

- The fix reaches consoles only with the next module build the maintainer
  releases (`make release`, then `dist/`); nothing in this plan ships it.
- If the 200-character cap is ever raised, keep the buffer at least one
  element longer than the count passed to `mbstowcs`, or the terminator this
  relies on has nowhere to live.
- A reviewer should confirm the diff is the comment and the one `memset` line,
  and nothing else.
