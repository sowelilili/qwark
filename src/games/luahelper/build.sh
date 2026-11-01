#!/bin/sh
#
# Builds the Lua helper for every game that has one (Tools of Destruction for
# now) and regenerates the committed source qwark carries it in,
# src/games/luahelper_bins.c and .h. The same shape as the savefile helper's
# build.sh, which this follows step for step.
#
# Run it from a Cygwin login shell that has the Sony SDK on its PATH:
#
#     sh src/games/luahelper/build.sh              build and regenerate
#     sh src/games/luahelper/build.sh --if-stale   only when a source is newer
#
# --if-stale does nothing when the generated file is up to date, and nothing
# when the SDK is not on the PATH: the generated file is committed, and the host
# builds and the tests never need the SDK. The makefile's `luahelper` and
# `luahelper-if-stale` run it, as for the other helpers, and a plain `make`
# runs the second (docs/tod-poc.md section 4).
#
# Per game: compile luahelper.c against the game's header, assemble the entry,
# link both at the cave with every LH_FN_* bound by --defsym, cut .text out as
# raw bytes, and read the two entry symbols back out of the ELF so both hook
# words are computed from what was linked.
set -e

cd "$(dirname "$0")/../../.."

DIR=src/games/luahelper
OUT=$DIR/out
GEN_C=src/games/luahelper_bins.c
GEN_H=src/games/luahelper_bins.h

CC=ppu-lv2-gcc
LD=ppu-lv2-ld
NM=ppu-lv2-nm
OBJCOPY=ppu-lv2-objcopy
OBJDUMP=ppu-lv2-objdump

CFLAGS="-O2 -std=gnu99 -nostdlib -fno-builtin -Wall -Wextra -Werror -I$DIR"
ASFLAGS="-Wa,-mregnames -I$DIR"

# game tag : header : title : LH_GAME number
GAMES="tod"

sources="$DIR/luahelper.c $DIR/luahelper.h $DIR/lh_entry.S $DIR/luahelper.ld.in $DIR/build.sh src/core/luamb.h          $DIR/lh_menu.c $DIR/lh_menu.h $DIR/lh_glue.lua $DIR/lh_diag.lua src/core/menubox.h"
for g in $GAMES; do sources="$sources $DIR/lh_$g.h"; done

if [ "$1" = "--if-stale" ]; then
	if ! command -v $CC >/dev/null 2>&1; then
		echo "luahelper: no $CC on the PATH, keeping the committed $GEN_C"
		exit 0
	fi
	stale=0
	[ -f "$GEN_C" ] || stale=1
	[ -f "$GEN_H" ] || stale=1
	for s in $sources; do
		if [ "$s" -nt "$GEN_C" ]; then stale=1; fi
	done
	if [ "$stale" = "0" ]; then exit 0; fi
	echo "luahelper: a source is newer than $GEN_C, rebuilding the helper"
fi

command -v $CC >/dev/null 2>&1 || {
	echo "luahelper: $CC is not on the PATH; run this from the SDK's Cygwin shell" >&2
	exit 1
}

# ---------------------------------------------------------------- little tools

# A plain `#define NAME 0x...` out of a header, carriage returns removed first;
# a /* comment */ may follow the number, as in luamb.h.
getdef() {
	tr -d '\r' < "$1" |
		sed -n "s/^#define[ \t][ \t]*$2[ \t][ \t]*\(0x[0-9A-Fa-f][0-9A-Fa-f]*\|[0-9][0-9]*\)[ \t]*\(\/\*.*\)\{0,1\}\$/\1/p"
}

need() {
	value=$(getdef "$1" "$2")
	[ -n "$value" ] || { echo "luahelper: $1 does not define $2" >&2; exit 1; }
	echo "$value"
}

# The C array body for a raw binary: twelve bytes a line, tab indented.
emit_bytes() {
	od -An -v -tx1 "$1" | awk '
		{
			for (i = 1; i <= NF; i++) {
				if (n % 12 == 0) printf "\t"
				printf "0x%s,", toupper($i)
				n++
				if (n % 12 == 0) printf "\n"; else printf " "
			}
		}
		END { if (n % 12 != 0) printf "\n" }'
}

# A Lua file's comment lines, indentation and empty lines out (no line of
# ours has code before a comment).
lua_strip() {
	tr -d '\r' < "$1" | sed -e 's/^[ \t]*//' -e '/^--/d' -e '/^$/d'
}

# Revision 1.29: and the spaces around = == ~= <= >= + - * / < > and ",".
# Safe for our Lua alone, so it is checked: no string literal of the file
# holds a space (only spaces are taken out, so a literal without one comes
# through as it was), and no "- -" (which would become a comment); ".."
# keeps its spaces (a number before it would become a malformed number).
lua_min() {
	bad=$(awk -F'"' '{ for (i = 2; i <= NF; i += 2) if ($i ~ / /) print NR ": " $i }' "$1")
	[ -z "$bad" ] || { echo "luahelper: $1 has a string with a space, which lua_min cannot keep: $bad" >&2; exit 1; }
	if grep -n -- '- -' "$1" >&2; then
		echo "luahelper: $1 has \"- -\", which lua_min would make a comment" >&2
		exit 1
	fi
	sed -e 's/ *\(==\|~=\|<=\|>=\|=\|[-+*\/<>,]\) */\1/g' "$1"
}

game_number() {
	case "$1" in
	tod) echo 1 ;;
	esac
}

game_title() {
	case "$1" in
	tod) echo NPEA00452 ;;
	esac
}

# `bl` (link 1) or `b` (link 0) from `from` to `to`, a signed 26-bit displacement.
branch_word() {
	delta=$(($2 - $1))
	[ $delta -lt $((0x02000000)) ] && [ $delta -gt -$((0x02000000)) ] ||
		{ echo "luahelper: $2 is out of branch range of $1" >&2; exit 1; }
	printf '0x%08X' $(( 0x48000000 | (delta & 0x03FFFFFC) | $3 ))
}

symbol() {
	$NM "$1" | tr -d '\r' | awk -v s="$2" '$3 == s { print "0x" $1 }'
}

rm -rf "$OUT"
mkdir -p "$OUT"
: > "$OUT/arrays.c"
: > "$OUT/descs.txt"
: > "$OUT/report.txt"

for g in $GAMES; do
	hdr=$DIR/lh_$g.h
	num=$(game_number "$g")
	title=$(game_title "$g")

	cave=$(need "$hdr" LH_CAVE)
	cave_end=$(need "$hdr" LH_CAVE_END)
	mailbox=$(need "$hdr" LH_MAILBOX)
	mailbox_end=$(need "$hdr" LH_MAILBOX_END)
	hook_addr=$(need "$hdr" LH_HOOK_ADDR)
	hook_orig=$(need "$hdr" LH_HOOK_ORIG)
	err_addr=$(need "$hdr" LH_ERR_HOOK_ADDR)
	err_orig=$(need "$hdr" LH_ERR_HOOK_ORIG)
	pause_ms=$(need "$hdr" LH_PAUSE_MS)
	ms_state=$(need "$hdr" LH_MS_STATE)
	menu_addr=$(need "$hdr" LH_MENU_HOOK_ADDR)
	menu_orig=$(need "$hdr" LH_MENU_HOOK_ORIG)
	pause_addr=$(need "$hdr" LH_PAUSE_HOOK_ADDR)
	pause_orig=$(need "$hdr" LH_PAUSE_HOOK_ORIG)
	vm=$(need "$hdr" LH_VM)
	vm_state=$(need "$hdr" LH_VM_STATE)
	mb_size=$(need src/core/luamb.h LUAMB_SIZE)

	[ $((mailbox + mb_size)) -le $((mailbox_end)) ] || {
		echo "luahelper: $g mailbox of $mb_size bytes runs past $mailbox_end" >&2
		exit 1
	}

	# Every LH_FN_ (a function with its own descriptor) and, revision 1.29,
	# every LH_TR_ (one of the game's cross-TOC trampolines, which runs with
	# its caller's r2) becomes a symbol: .lh_<name>, .lh_tr_<name>.
	defsyms=$(tr -d '\r' < "$hdr" |
		sed -n -e 's/^#define[ \t][ \t]*LH_FN_\([A-Z0-9_]*\)[ \t][ \t]*\(0x[0-9A-Fa-f]*\)[ \t]*$/\1 \2/p' \
		       -e 's/^#define[ \t][ \t]*LH_\(TR_[A-Z0-9_]*\)[ \t][ \t]*\(0x[0-9A-Fa-f]*\)[ \t]*$/\1 \2/p' |
		awk '{ printf "--defsym .lh_%s=%s ", tolower($1), $2 }')
	[ -n "$defsyms" ] || { echo "luahelper: $hdr names no LH_FN_ function" >&2; exit 1; }

	# Revision 1.29: the console menu's glue, its comments and indentation
	# taken out (a line that starts with "--" is a comment, and no line of the
	# glue has code before one). qwark sends it to the pause menu's VM as a
	# request (it is far too big for the RPCS3 patch), so it goes into
	# luahelper_bins.c beside the cave, and has to fit a request. Since the
	# third console test (docs/tod-menu.md 11) the spaces around its
	# operators and commas go too (lua_min): that is what keeps it in one.
	# The look qwark sends when the glue's look failed (lh_diag.lua) is
	# only stripped; it is small.
	glue=$OUT/$g-glue.lua
	diag=$OUT/$g-diag.lua
	chunk_cap=$(need src/core/luamb.h LUAMB_CHUNK_CAP)
	lua_strip "$DIR/lh_glue.lua" > "$OUT/$g-glue-full.lua"
	lua_min "$OUT/$g-glue-full.lua" > "$glue"
	lua_strip "$DIR/lh_diag.lua" > "$diag"

	$CC $CFLAGS -DLH_GAME=$num -c -o "$OUT/$g.o" "$DIR/luahelper.c"
	# Through the driver, as the savefile helper's stubs are: it stamps the
	# CellOS Lv-2 ABI byte the linker needs, and runs cpp over the .S.
	$CC $ASFLAGS -DLH_GAME=$num -c -o "$OUT/$g-entry.o" "$DIR/lh_entry.S"

	sed -e "s/@TEXT_ORG@/$cave/" "$DIR/luahelper.ld.in" > "$OUT/$g.ld"
	# shellcheck disable=SC2086
	$LD -T "$OUT/$g.ld" $defsyms -o "$OUT/$g.elf" "$OUT/$g-entry.o" "$OUT/$g.o"

	unexpected=$($OBJDUMP -h "$OUT/$g.elf" | tr -d '\r' |
		awk '$1 ~ /^[0-9]+$/ && $3 !~ /^0+$/ && $2 != ".text" { print $2 }')
	[ -z "$unexpected" ] || {
		echo "luahelper: $g put code or data in unexpected sections: $unexpected" >&2
		exit 1
	}

	bin=$OUT/$g.bin
	$OBJCOPY -O binary --only-section=.text "$OUT/$g.elf" "$bin"
	size=$(wc -c < "$bin" | tr -d ' ')
	[ $((cave + size)) -le $((cave_end)) ] || {
		echo "luahelper: $g helper is $size bytes and runs past $cave_end" >&2
		exit 1
	}
	vma=$($OBJDUMP -h "$OUT/$g.elf" | tr -d '\r' | awk '$2 == ".text" { print "0x" $4 }')
	[ -n "$vma" ] && [ $((vma)) -eq $((cave)) ] || {
		echo "luahelper: $g .text linked at ${vma:-nowhere}, not at its cave $cave" >&2
		exit 1
	}

	entry=$(symbol "$OUT/$g.elf" .lh_hook)
	tap=$(symbol "$OUT/$g.elf" .lh_err_tap)
	menu=$(symbol "$OUT/$g.elf" .lh_menu_hook)
	pause=$(symbol "$OUT/$g.elf" .lh_pause_hook)
	[ -n "$entry" ] && [ -n "$tap" ] && [ -n "$menu" ] && [ -n "$pause" ] ||
		{ echo "luahelper: $g is missing .lh_hook, .lh_err_tap, .lh_menu_hook or .lh_pause_hook" >&2; exit 1; }
	[ $((entry)) -eq $((cave)) ] || { echo "luahelper: $g .lh_hook is at $entry, not at the cave" >&2; exit 1; }

	hook_word=$(branch_word "$hook_addr" "$entry" 1)
	err_word=$(branch_word "$err_addr" "$tap" 0)
	menu_word=$(branch_word "$menu_addr" "$menu" 1)
	pause_word=$(branch_word "$pause_addr" "$pause" 1)

	{
		echo "/* $g, .text at $cave, $size bytes */"
		echo "static const u8 lh_${g}_text[] = {"
		emit_bytes "$bin"
		echo "};"
		echo
		echo "static const struct lh_cave lh_${g}_caves[] = {"
		echo "	{ ${cave}u, lh_${g}_text, (u32)sizeof(lh_${g}_text) }"
		echo "};"
		echo
	} >> "$OUT/arrays.c"

	{
		echo "static const struct lh_hook lh_${g}_hooks[] = {"
		echo "	{ ${hook_addr}u, ${hook_word}u, ${hook_orig}u },   /* bl .lh_hook       -> $entry */"
		echo "	{ ${err_addr}u, ${err_word}u, ${err_orig}u },   /* b  .lh_err_tap    -> $tap */"
		echo "	{ ${menu_addr}u, ${menu_word}u, ${menu_orig}u },   /* bl .lh_menu_hook  -> $menu */"
		echo "	{ ${pause_addr}u, ${pause_word}u, ${pause_orig}u }    /* bl .lh_pause_hook -> $pause */"
		echo "};"
		echo
	} >> "$OUT/hooks.c"

	glue_size=$(wc -c < "$glue" | tr -d ' ')
	[ "$glue_size" -le $((chunk_cap)) ] || {
		echo "luahelper: $g glue is $glue_size bytes, more than a request's $chunk_cap" >&2
		exit 1
	}
	diag_size=$(wc -c < "$diag" | tr -d ' ')
	[ "$diag_size" -le $((chunk_cap)) ] || {
		echo "luahelper: $g diag is $diag_size bytes, more than a request's $chunk_cap" >&2
		exit 1
	}
	{
		echo "/* $g, the console menu's glue (lh_glue.lua), $glue_size bytes of Lua */"
		echo "static const u8 lh_${g}_glue[] = {"
		emit_bytes "$glue"
		echo "};"
		echo
		echo "/* $g, the look qwark sends when the glue's failed (lh_diag.lua), $diag_size bytes of Lua */"
		echo "static const u8 lh_${g}_diag[] = {"
		emit_bytes "$diag"
		echo "};"
		echo
	} >> "$OUT/arrays.c"

	echo "	{ \"$title\", LH_TABLE(lh_${g}_caves), 1, lh_${g}_hooks, 4," >> "$OUT/descs.txt"
	echo "	  ${mailbox}u, LUAMB_SIZE, ${vm}u, ${vm_state}u," >> "$OUT/descs.txt"
	echo "	  ${pause_ms}u, ${ms_state}u, LH_TABLE(lh_${g}_glue), ${glue_size}u," >> "$OUT/descs.txt"
	echo "	  LH_TABLE(lh_${g}_diag), ${diag_size}u }," >> "$OUT/descs.txt"

	echo "  $g .text $cave $size bytes (cave to $cave_end)" >> "$OUT/report.txt"
	echo "  $g hook $hook_addr = $hook_word (bl $entry), was $hook_orig" >> "$OUT/report.txt"
	echo "  $g err  $err_addr = $err_word (b $tap), was $err_orig" >> "$OUT/report.txt"
	echo "  $g menu $menu_addr = $menu_word (bl $menu), was $menu_orig" >> "$OUT/report.txt"
	echo "  $g pause $pause_addr = $pause_word (bl $pause), was $pause_orig" >> "$OUT/report.txt"
	echo "  $g glue $glue_size bytes of Lua (a request holds $((chunk_cap)))" >> "$OUT/report.txt"
	echo "  $g diag $diag_size bytes of Lua" >> "$OUT/report.txt"
	echo "  $g mailbox $mailbox, $mb_size bytes" >> "$OUT/report.txt"
done

sed '$ s/},$/}/' "$OUT/descs.txt" > "$OUT/descs-final.txt"

{
	cat <<'EOF'
/*
 * The Lua helper, as bytes.
 *
 * GENERATED by src/games/luahelper/build.sh out of the sources beside it. Do
 * not edit it by hand; edit the helper and run
 *
 *     sh src/games/luahelper/build.sh
 *
 * from the SDK's Cygwin shell.
 *
 * Each game has one cave, the compiled helper, and four hook words: the branch
 * that replaces the game's call to its gameplay VM's update, the branch that
 * replaces its empty reportError, and (revision 1.29, the console menu) the
 * calls that replace the pause menu's update and the test of the pause
 * button. All are computed from the addresses the helper linked at. The cave's bytes are where something puts them into
 * the game: the emulator builds (QWARK_EMULATOR), which make an RPCS3 patch of
 * them, and the game module (QWARK_GAME_MODULE, tod.sprx), out of whose
 * read-only segment the core installs them on a console (revision 1.27). The
 * core itself carries none.
 */
#include "../core/buildmodes.h"
#include "luahelper_bins.h"

#if defined(QWARK_EMULATOR) || QWARK_GAME_MODULE

EOF
	cat "$OUT/arrays.c"
	cat <<'EOF'
#define LH_TABLE(x) (x)
#else
#define LH_TABLE(x) 0
#endif /* QWARK_EMULATOR || QWARK_GAME_MODULE */

EOF
	cat "$OUT/hooks.c"
	echo "static const struct lh_desc lh_descs[] = {"
	cat "$OUT/descs-final.txt"
	echo "};"
	cat <<'EOF'

const struct lh_desc *lh_desc_for_title(const char *title_id)
{
	unsigned i, k;

	if (title_id == NULL) return NULL;
	for (i = 0; i < sizeof(lh_descs) / sizeof(lh_descs[0]); i++) {
		const char *t = lh_descs[i].title;

		for (k = 0; t[k] != 0 && title_id[k] == t[k]; k++) {}
		if (t[k] == 0 && title_id[k] == 0) return &lh_descs[i];
	}
	return NULL;
}
EOF
} > "$GEN_C"

cat > "$GEN_H" <<'EOF'
/*
 * The Lua helper, as bytes: what qwark puts into a Lua game so that it runs a
 * chunk of source from the mailbox on the game's own thread (src/core/luamb.h,
 * docs/tod-poc.md).
 *
 * GENERATED by src/games/luahelper/build.sh. Do not edit it by hand.
 */
#ifndef QWARK_LUAHELPER_BINS_H
#define QWARK_LUAHELPER_BINS_H

#include "../plat/plat.h"
#include "../core/luamb.h"

/* One run of bytes at one address in the game. */
struct lh_cave {
	u32 addr;
	const u8 *bytes;
	u32 len;
};

/* One hook: the word that goes in, and the game's own word it replaces. */
struct lh_hook {
	u32 addr;
	u32 value;
	u32 orig;
};

/* Everything qwark needs to install one game's helper and talk to it. */
struct lh_desc {
	const char *title;          /* the executable's title id */

	/* NULL but in the emulator builds and the game module; the count stays. */
	const struct lh_cave *caves;
	u8 ncaves;

	/* The VM hook first, then the error tap, then the menu's two (revision 1.29). */
	const struct lh_hook *hooks;
	u8 nhooks;

	u32 mailbox;                /* LUAMB_SIZE bytes of the game's RW memory */
	u32 mailbox_size;
	u32 vm;                     /* the gameplay VM object */
	u32 vm_state;               /* the lua_State* is the word at vm + vm_state, 0 between levels */

	/*
	 * Revision 1.29, the console menu: the pause menu's VM, whose lua_State*
	 * is at menu_off into the menu system that the word at menu_ptr holds (0
	 * with no pause), and the menu's Lua glue, which qwark sends it (NULL but
	 * in the emulator builds and the game module; the length stays).
	 */
	u32 menu_ptr;
	u32 menu_off;
	const u8 *glue;
	u32 glue_len;
	/*
	 * Revision 1.29, the third console test: the look qwark sends that VM
	 * once when the glue's own look failed (lh_diag.lua), whose answer is
	 * a line of text for qwark.log. NULL but where the glue is; the length
	 * stays.
	 */
	const u8 *diag;
	u32 diag_len;
};

/* NULL for an executable with no Lua helper. */
const struct lh_desc *lh_desc_for_title(const char *title_id);

#endif /* QWARK_LUAHELPER_BINS_H */
EOF

echo "luahelper: regenerated $GEN_C and $GEN_H"
cat "$OUT/report.txt"
