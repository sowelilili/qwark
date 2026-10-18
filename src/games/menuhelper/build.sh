#!/bin/sh
#
# Builds the console menu's renderers and regenerates the committed source qwark
# carries them in, src/games/menuhelper_bins.c and .h. Deadlocked's is the only
# one so far (mh_rac4.*).
#
# Run it from a Cygwin login shell that has the Sony SDK on its PATH, the shell
# `make` runs in:
#
#     sh src/games/menuhelper/build.sh              build and regenerate
#     sh src/games/menuhelper/build.sh --if-stale   only when a source is newer
#
# --if-stale is what `make` uses: nothing at all when the generated file is up
# to date or the SDK is not on the PATH, so the host builds and the tests, which
# compile the committed file, never need the SDK.
#
# Per renderer: compile its C (warnings are errors), assemble its stubs and its
# starting state through the C preprocessor, link both images at the game's
# addresses (mh.ld.in), cut the code and the data out as bytes, compute every
# hook word from the linked addresses, and check the result against the
# header: nothing in .data or .bss, nothing past either area, the state where
# the C looks for it, every branch within reach. Then python -I
# src/games/menuhelper/check.py checks it against the game's executable and
# every mod known (it reads the generated file, so it runs without the SDK).
set -e

cd "$(dirname "$0")/../../.."

DIR=src/games/menuhelper
OUT=$DIR/out
GEN_C=src/games/menuhelper_bins.c
GEN_H=src/games/menuhelper_bins.h

CC=ppu-lv2-gcc
LD=ppu-lv2-ld
NM=ppu-lv2-nm
OBJCOPY=ppu-lv2-objcopy
OBJDUMP=ppu-lv2-objdump

CFLAGS="-O2 -std=gnu99 -nostdlib -fno-builtin -ffreestanding -mno-altivec -Wall -Wextra -Werror -I$DIR"

RENDERERS="rac4"

fail() {
	echo "menuhelper: $*" >&2
	exit 1
}

# ---------------------------------------------------------------- staleness

sources="$DIR/build.sh $DIR/mh.ld.in $DIR/mh_common.h src/core/menubox.h"
for g in $RENDERERS; do
	sources="$sources $DIR/mh_$g.h $DIR/mh_$g.c $DIR/mh_${g}_stubs.S $DIR/mh_${g}_state.S"
done

if [ "$1" = "--if-stale" ]; then
	if ! command -v $CC >/dev/null 2>&1; then
		echo "menuhelper: no $CC on the PATH, keeping the committed $GEN_C"
		exit 0
	fi
	stale=0
	[ -f "$GEN_C" ] || stale=1
	[ -f "$GEN_H" ] || stale=1
	for s in $sources; do
		if [ "$s" -nt "$GEN_C" ]; then stale=1; fi
	done
	if [ "$stale" = "0" ]; then exit 0; fi
	echo "menuhelper: a source is newer than $GEN_C, rebuilding the renderers"
fi

command -v $CC >/dev/null 2>&1 || fail "$CC is not on the PATH; run this from the SDK's Cygwin shell"

# --------------------------------------------------------------- little tools

# A `#define NAME 0x...` out of a header, a /* comment */ after it allowed.
getdef() {
	tr -d '\r' < "$1" |
		sed -n "s/^#define[ \t][ \t]*$2[ \t][ \t]*\(0x[0-9A-Fa-f][0-9A-Fa-f]*\)[ \t]*\(\/\*.*\*\/\)\{0,1\}[ \t]*\$/\1/p"
}

need() {
	v=$(getdef "$1" "$2")
	[ -n "$v" ] || fail "$1 does not define $2"
	echo "$v"
}

# `bl target` at `site`: a signed 26-bit displacement.
blword() {
	delta=$(($2 - $1))
	[ $delta -lt $((0x02000000)) ] && [ $delta -ge -$((0x02000000)) ] ||
		fail "$(printf '0x%08X' $1) cannot reach $(printf '0x%08X' $2) with a bl"
	printf '0x%08X' $((0x48000001 | (delta & 0x03FFFFFC)))
}

# sym ELF NAME: a symbol's address as 0x%08X, or nothing.
sym() {
	v=$($NM "$1" | tr -d '\r' | awk -v s="$2" '$3 == s { print $1; exit }')
	[ -z "$v" ] || printf '0x%08X' $((0x$v))
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

rm -rf "$OUT"
mkdir -p "$OUT"
: > "$OUT/arrays.c"
: > "$OUT/descs.txt"
: > "$OUT/report.txt"

# ------------------------------------------------------------ each renderer

for g in $RENDERERS; do
	hdr=$DIR/mh_$g.h
	obj=$OUT/$g
	mkdir -p "$obj"

	game=$(need "$hdr" MH_GAME_ID)
	cave=$(need "$hdr" MH_CAVE)
	cave_end=$(need "$hdr" MH_CAVE_END)
	data=$(need "$hdr" MH_DATA)
	data_end=$(need "$hdr" MH_DATA_END)
	state=$(need "$hdr" MH_STATE)
	state_off=$(need "$hdr" MH_STATE_OFF)
	mbox_size=$(need "$hdr" MH_MBOX_SIZE)
	slot=$(need "$hdr" MH_SCREEN_SLOT)
	screen_id=$(need "$hdr" MH_SCREEN_ID)
	site_frame=$(need "$hdr" MH_SITE_FRAME)
	site_screen=$(need "$hdr" MH_SITE_SCREEN)
	site_inject=$(need "$hdr" MH_SITE_INJECT)
	site_options=$(need "$hdr" MH_SITE_OPTIONS)
	orig_frame=$(need "$hdr" MH_ORIG_FRAME)
	orig_screen=$(need "$hdr" MH_ORIG_SCREEN)
	orig_inject=$(need "$hdr" MH_ORIG_INJECT)
	orig_options=$(need "$hdr" MH_ORIG_OPTIONS)
	orig_start=$(need "$hdr" MH_ORIG_SLOT_START)
	handle_input=$(need "$hdr" GAME_HANDLE_MENU_INPUT)
	set_buttons=$(need "$hdr" GAME_SET_MENU_BUTTONS)
	mode_addr=$(need "$hdr" DL_GAME_MODE)
	mode_paused=$(need "$hdr" DL_MODE_PAUSE)
	pending_addr=$(need "$hdr" DL_PENDING_MODE)
	pending_none=$(need "$hdr" DL_MODE_NONE)
	screen_addr=$(need "$hdr" DL_MENU_SCREEN)

	[ $((data + state_off)) -eq $((state)) ] || fail "$hdr: MH_STATE is not MH_DATA + MH_STATE_OFF"

	$CC $CFLAGS -c -o "$obj/mh.o" "$DIR/mh_$g.c"
	# Through the driver, which runs the preprocessor for .S and stamps the
	# CellOS Lv-2 ABI on the object; the linker refuses to mix objects otherwise.
	$CC -I$DIR -c -o "$obj/stubs.o" "$DIR/mh_${g}_stubs.S"
	$CC -I$DIR -c -o "$obj/state.o" "$DIR/mh_${g}_state.S"

	sed -e "s/@TEXT_ORG@/$cave/" -e "s/@DATA_ORG@/$data/" "$DIR/mh.ld.in" > "$obj/mh.ld"
	$LD --fatal-warnings -T "$obj/mh.ld" -Map "$obj/mh.map" -o "$obj/mh.elf" \
		--defsym game_handle_menu_input=$handle_input \
		--defsym game_set_menu_buttons=$set_buttons \
		"$obj/stubs.o" "$obj/mh.o" "$obj/state.o"

	# name size vma, one line per section
	$OBJDUMP -h "$obj/mh.elf" | tr -d '\r' |
		awk '$1 ~ /^[0-9]+$/ { print $2, $3, $4 }' > "$obj/sections.txt"
	bad=$(awk '$2 !~ /^0+$/ && $1 != ".text" && $1 != ".mh_state" && $1 != ".rodata" &&
	           $1 != ".toc" && $1 != ".opd" { print $1 }' "$obj/sections.txt")
	[ -z "$bad" ] || fail "$g: sections that must be empty are not: $bad"

	text_vma=0x$(awk '$1 == ".text" { print $3 }' "$obj/sections.txt")
	state_vma=0x$(awk '$1 == ".mh_state" { print $3 }' "$obj/sections.txt")
	[ $((text_vma)) -eq $((cave)) ] || fail "$g: .text starts at $text_vma, not $cave"
	[ $((state_vma)) -eq $((data)) ] || fail "$g: the state starts at $state_vma, not $data"
	[ "$(sym "$obj/mh.elf" mh_state)" = "$(printf '0x%08X' $((state)))" ] ||
		fail "$g: mh_state is not at MH_STATE"

	$OBJCOPY -O binary -j .text "$obj/mh.elf" "$obj/code.bin"
	$OBJCOPY -O binary -j .mh_state -j .rodata -j .toc -j .opd "$obj/mh.elf" "$obj/data.bin"
	code_size=$(wc -c < "$obj/code.bin" | tr -d ' ')
	data_size=$(wc -c < "$obj/data.bin" | tr -d ' ')
	[ $((cave + code_size)) -eq $(($(sym "$obj/mh.elf" _text_end))) ] ||
		fail "$g: code.bin is $code_size bytes, the link ends elsewhere"
	[ $((data + data_size)) -eq $(($(sym "$obj/mh.elf" _data_end))) ] ||
		fail "$g: data.bin is $data_size bytes, the link ends elsewhere"
	[ $((cave + code_size)) -le $((cave_end)) ] || fail "$g: the code is $code_size bytes and runs past $cave_end"
	[ $((data + data_size)) -le $((data_end)) ] || fail "$g: the data is $data_size bytes and runs past $data_end"

	$OBJDUMP -d "$obj/mh.elf" > "$obj/mh.dis"

	# The hook words, from the linked addresses.
	start_desc=$(sym "$obj/mh.elf" mh_start)
	stop_desc=$(sym "$obj/mh.elf" mh_stop)
	frame_stub=$(sym "$obj/mh.elf" mh_frame_stub)
	screen_stub=$(sym "$obj/mh.elf" mh_screen_stub)
	inject_stub=$(sym "$obj/mh.elf" mh_inject_stub)
	options_stub=$(sym "$obj/mh.elf" mh_options_stub)
	[ -n "$start_desc" ] && [ -n "$stop_desc" ] && [ -n "$frame_stub" ] && [ -n "$screen_stub" ] &&
		[ -n "$inject_stub" ] && [ -n "$options_stub" ] || fail "$g: a symbol is missing"
	# The descriptors must be data.bin's, the stubs code.bin's.
	for d in $start_desc $stop_desc; do
		[ $((d)) -ge $((data)) ] && [ $((d)) -lt $((data + data_size)) ] || fail "$g: descriptor $d is not in data.bin"
	done

	w_frame=$(blword "$site_frame" "$frame_stub")
	w_screen=$(blword "$site_screen" "$screen_stub")
	w_inject=$(blword "$site_inject" "$inject_stub")
	w_options=$(blword "$site_options" "$options_stub")

	{
		echo "/* $g: the data image, $data_size bytes at $data: mailbox, state, rodata, TOC, descriptors */"
		echo "static const u8 mh_${g}_data[] = {"
		emit_bytes "$obj/data.bin"
		echo "};"
		echo
		echo "/* $g: the code, $code_size bytes at $cave: the stubs, then the C */"
		echo "static const u8 mh_${g}_code[] = {"
		emit_bytes "$obj/code.bin"
		echo "};"
		echo
		echo "static const struct mh_cave mh_${g}_caves[] = {"
		echo "	{ ${data}u, mh_${g}_data, (u32)sizeof(mh_${g}_data), 0 },"
		echo "	{ ${cave}u, mh_${g}_code, (u32)sizeof(mh_${g}_code), 1 }"
		echo "};"
		echo
		echo "/* In the order they go in; they come out in the reverse order. */"
		echo "static const struct mh_word mh_${g}_hooks[] = {"
		printf '\t{ 0x%08Xu, 0x00000000u, 0x00000000u },   /* screen %d fcInit */\n' $((slot)) $((screen_id))
		printf '\t{ 0x%08Xu, 0x00000000u, 0x00000000u },   /* fcReset */\n' $((slot + 4))
		printf '\t{ 0x%08Xu, %su, %su },   /* fcStart: mh_start */\n' $((slot + 8)) "$start_desc" "$orig_start"
		printf '\t{ 0x%08Xu, %su, 0x00000000u },   /* fcStop: mh_stop */\n' $((slot + 12)) "$stop_desc"
		printf '\t{ %su, %su, %su },   /* bl mh_frame_stub (%s) */\n' "$site_frame" "$w_frame" "$orig_frame" "$frame_stub"
		printf '\t{ %su, %su, %su },   /* bl mh_screen_stub (%s) */\n' "$site_screen" "$w_screen" "$orig_screen" "$screen_stub"
		printf '\t{ %su, %su, %su },   /* bl mh_inject_stub (%s) */\n' "$site_inject" "$w_inject" "$orig_inject" "$inject_stub"
		printf '\t{ %su, %su, %su }    /* bl mh_options_stub (%s) */\n' "$site_options" "$w_options" "$orig_options" "$options_stub"
		echo "};"
		echo
	} >> "$OUT/arrays.c"

	{
		echo "	{ $((game)), mh_${g}_caves, 2, mh_${g}_hooks, 8,"
		echo "	  ${data}u, ${mbox_size}u, ${state}u,"
		echo "	  ${mode_addr}u, ${mode_paused}u, ${pending_addr}u, ${pending_none}u,"
		echo "	  ${screen_addr}u, ${screen_id}u },"
	} >> "$OUT/descs.txt"

	{
		echo "$g: code $code_size bytes at $cave ($((cave_end - cave - code_size)) left), data $data_size bytes at $data ($((data_end - data - data_size)) left)"
		echo "  $(printf '0x%08X' $((slot)))..+16 -> {0, 0, $start_desc, $stop_desc}"
		echo "  $site_frame -> $w_frame  bl mh_frame_stub ($frame_stub)"
		echo "  $site_screen -> $w_screen  bl mh_screen_stub ($screen_stub)"
		echo "  $site_inject -> $w_inject  bl mh_inject_stub ($inject_stub)"
		echo "  $site_options -> $w_options  bl mh_options_stub ($options_stub)"
	} >> "$OUT/report.txt"
done

sed '$ s/},$/}/' "$OUT/descs.txt" > "$OUT/descs-final.txt"

# --------------------------------------------------------------- emit the source

cat > "$GEN_H" <<'EOF'
/*
 * The console menu's renderers, as bytes: what qwark writes into a game so that
 * the game draws qwark's menu with its own widgets (src/core/menu.c).
 *
 * GENERATED by src/games/menuhelper/build.sh out of the sources beside it. Do
 * not edit it by hand; edit the renderer and run
 *
 *     sh src/games/menuhelper/build.sh
 *
 * from the SDK's Cygwin shell. It is committed because the renderer is PowerPC
 * code the Sony cross compiler produces, and the host builds and the tests are
 * clang on a PC with no SDK near them.
 *
 * Embedded in the module for now; the spec's ui.pak on the HDD would hand
 * menu.c the same struct mh_desc out of a file.
 */
#ifndef QWARK_MENUHELPER_BINS_H
#define QWARK_MENUHELPER_BINS_H

#include "../core/proto.h"

/* One run of bytes at one address: the data image, or the code. */
struct mh_cave {
	u32 addr;
	const u8 *bytes;
	u32 len;
	u8  code;      /* 1 for the code, which an install compares before adopting */
};

/* One word a renderer replaces, with the game's own word there. */
struct mh_word {
	u32 addr;
	u32 value;
	u32 original;
};

struct mh_desc {
	u8 game_id;

	/* The data image first, then the code: both before any hook. */
	const struct mh_cave *caves;
	u8 ncaves;

	/* In the order they go in; they come out in the reverse order. */
	const struct mh_word *hooks;
	u8 nhooks;

	/* The mailbox (src/core/menubox.h): where, and how big. */
	u32 mailbox;
	u32 mailbox_size;
	/* The renderer's own state, after the mailbox. */
	u32 state;

	/*
	 * What says the menu is not up, before the hooks come out: the game's mode
	 * word is not `mode_paused`, its pending mode is `pending_none`, and the
	 * screen word is not `screen_id`.
	 */
	u32 mode_addr;
	u32 mode_paused;
	u32 pending_addr;
	u32 pending_none;
	u32 screen_addr;
	u32 screen_id;
};

/* NULL for a game with no renderer, and for GAME_NONE. */
const struct mh_desc *mh_desc_for_game(u8 game_id);

#endif /* QWARK_MENUHELPER_BINS_H */
EOF

{
	cat <<'EOF'
/*
 * The console menu's renderers, as bytes.
 *
 * GENERATED by src/games/menuhelper/build.sh out of the sources beside it. Do
 * not edit it by hand; edit the renderer and run
 *
 *     sh src/games/menuhelper/build.sh
 *
 * from the SDK's Cygwin shell, then python -I src/games/menuhelper/check.py.
 *
 * Per renderer: the data image (the mailbox, the renderer state, its read-only
 * data, its TOC and its descriptors), the code, and the hook words with the
 * game's own word at each, computed from the addresses the renderer linked at.
 */
#include "menuhelper_bins.h"

EOF
	cat "$OUT/arrays.c"
	echo "static const struct mh_desc mh_descs[] = {"
	cat "$OUT/descs-final.txt"
	echo "};"
	cat <<'EOF'

const struct mh_desc *mh_desc_for_game(u8 game_id)
{
	unsigned i;

	for (i = 0; i < sizeof(mh_descs) / sizeof(mh_descs[0]); i++) {
		if (mh_descs[i].game_id == game_id) return &mh_descs[i];
	}
	return NULL;
}
EOF
} > "$GEN_C"

echo "menuhelper: regenerated $GEN_C and $GEN_H"
cat "$OUT/report.txt"
