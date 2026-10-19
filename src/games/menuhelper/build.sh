#!/bin/sh
#
# Builds the console menu's renderers and regenerates the committed source qwark
# carries them in, src/games/menuhelper_bins.c and .h. A renderer is the shared
# core (mh_core.c) and a game's shim (mh_<game>.c, its stubs and its state
# image), both compiled with the game's header (mh_<game>.h) included first,
# like the savefile helper's one source for four games: Deadlocked's (rac4),
# Up Your Arsenal's (rac3), Going Commando's (rac2) and Ratchet & Clank's
# (rac1). What differs between them - the linker's entry and the game
# addresses it is given, the hook words and what qwark reads to know the menu
# is down - is a few functions per game below (<game>_entry, <game>_ldflags,
# <game>_hooks, <game>_query, and <game>_extra for a shim of more than one C
# file).
#
# A game whose free code is in several runs rather than one (RaC1: its text
# segment's tail is a mod's) names them MH_CAVE, MH_CAVE2, MH_CAVE3 ... in its
# header. Its renderer is compiled with -ffunction-sections and linked with a
# map made here (pack_map): the stubs and the core together at the start of
# MH_CAVE, then every function of the shim in the first area it fits, the
# largest first. Each area is a code cave of its own in the generated file.
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
# Per renderer: compile the core and the shim (warnings are errors), assemble
# its stubs and its starting state through the C preprocessor, link it at the
# game's addresses (mh.ld.in), cut the code, the mailbox and the data out as
# bytes, compute every hook word from the linked addresses, and check the
# result against the header: nothing in .data or .bss, nothing past any area,
# the state where the C looks for it, every branch within reach. Then python -I
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

CFLAGS="-Os -std=gnu99 -nostdlib -fno-builtin -ffreestanding -mno-altivec -Wall -Wextra -Werror -I$DIR"

RENDERERS="rac4 rac3 rac2 rac1"

fail() {
	echo "menuhelper: $*" >&2
	exit 1
}

# ---------------------------------------------------------------- staleness

sources="$DIR/build.sh $DIR/mh.ld.in $DIR/mh_common.h $DIR/mh_shim.h $DIR/mh_core.c src/core/menubox.h"
for g in $RENDERERS; do
	sources="$sources $DIR/mh_$g.h $DIR/mh_$g.c $DIR/mh_${g}_stubs.S $DIR/mh_${g}_state.S"
	for f in $DIR/mh_${g}_*.c $DIR/mh_${g}_*.h; do
		[ ! -f "$f" ] || sources="$sources $f"
	done
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

# `b target` at `site`, no link: a signed 26-bit displacement.
bword() {
	delta=$(($2 - $1))
	[ $delta -lt $((0x02000000)) ] && [ $delta -ge -$((0x02000000)) ] ||
		fail "$(printf '0x%08X' $1) cannot reach $(printf '0x%08X' $2) with a b"
	printf '0x%08X' $((0x48000000 | (delta & 0x03FFFFFC)))
}

# A symbol that must be there.
needsym() {
	v=$(sym "$1" "$2")
	[ -n "$v" ] || fail "$1 has no symbol $2"
	echo "$v"
}

# ------------------------------------------------------- per game: Deadlocked

rac4_entry() { echo mh_frame_stub; }

rac4_ldflags() {
	echo "--defsym game_handle_menu_input=$(need "$1" GAME_HANDLE_MENU_INPUT)"
	# The planet picker's stubs (mailbox version 3) go on to these.
	for x in HANDLE_SELECTION GHOST_LISTED GHOST_TESTED COUNT_DONE BACK_TAKEN CLOSE_TAKEN INPUT_DONE; do
		echo "--defsym dl_$(echo $x | tr 'A-Z' 'a-z')=$(need "$1" DL_$x)"
	done
}

# The hook words, one a line: address, value, the game's word, what it is.
rac4_hooks() {
	hdr=$1; elf=$2
	slot=$(need "$hdr" MH_SCREEN_SLOT)
	screen_id=$(need "$hdr" MH_SCREEN_ID)
	start_desc=$(needsym "$elf" mh_start)
	stop_desc=$(needsym "$elf" mh_stop)
	frame_stub=$(needsym "$elf" mh_frame_stub)
	screen_stub=$(needsym "$elf" mh_screen_stub)
	inject_stub=$(needsym "$elf" mh_inject_stub)
	help_stub=$(needsym "$elf" mh_help_label)
	worlds_desc=$(needsym "$elf" mh_worlds_stop)
	for d in $start_desc $stop_desc $worlds_desc; do
		[ $((d)) -ge $((data)) ] && [ $((d)) -lt $((data + data_size)) ] || fail "rac4: descriptor $d is not in data.bin"
	done
	printf '0x%08X 0x00000000 0x00000000 screen %d fcInit\n' $((slot)) $((screen_id))
	printf '0x%08X 0x00000000 0x00000000 fcReset\n' $((slot + 4))
	printf '0x%08X %s %s fcStart: mh_start\n' $((slot + 8)) "$start_desc" "$(need "$hdr" MH_ORIG_SLOT_START)"
	printf '0x%08X %s 0x00000000 fcStop: mh_stop\n' $((slot + 12)) "$stop_desc"
	site=$(need "$hdr" MH_SITE_FRAME)
	echo "$site $(blword "$site" "$frame_stub") $(need "$hdr" MH_ORIG_FRAME) bl mh_frame_stub ($frame_stub)"
	site=$(need "$hdr" MH_SITE_SCREEN)
	echo "$site $(blword "$site" "$screen_stub") $(need "$hdr" MH_ORIG_SCREEN) bl mh_screen_stub ($screen_stub)"
	site=$(need "$hdr" MH_SITE_INJECT)
	echo "$site $(blword "$site" "$inject_stub") $(need "$hdr" MH_ORIG_INJECT) bl mh_inject_stub ($inject_stub)"
	# The planet picker (mailbox version 3): Worlds' X, its four gates, its stop.
	stub=$(needsym "$elf" mh_select_stub)
	site=$(need "$hdr" MH_SITE_SELECT)
	echo "$site $(blword "$site" "$stub") $(need "$hdr" MH_ORIG_SELECT) bl mh_select_stub ($stub)"
	for gate in GHOST COUNT BACK CLOSE; do
		lower=$(echo $gate | tr 'A-Z' 'a-z')
		stub=$(needsym "$elf" mh_gate_$lower)
		site=$(need "$hdr" MH_SITE_$gate)
		echo "$site $(bword "$site" "$stub") $(need "$hdr" MH_ORIG_$gate) b mh_gate_$lower ($stub)"
	done
	echo "$(need "$hdr" MH_WORLDS_STOP) $worlds_desc $(need "$hdr" MH_ORIG_WORLDS_STOP) Worlds' fcStop: mh_worlds_stop"
	site=$(need "$hdr" MH_SITE_HELP_LABEL)
	echo "$site $(blword "$site" "$help_stub") $(need "$hdr" MH_ORIG_HELP_LABEL) bl mh_help_label ($help_stub)"
	echo "$(need "$hdr" MH_SITE_HELP_SCREEN) $(need "$hdr" MH_HELP_SCREEN_WORD) $(need "$hdr" MH_ORIG_HELP_SCREEN) li r4,36: the Help row opens screen 36"
}

# What says the menu is down: mode word, its paused value, pending word, none, screen word, the menu's screen.
rac4_query() {
	echo "$(need "$1" DL_GAME_MODE) $(need "$1" DL_MODE_PAUSE) $(need "$1" DL_PENDING_MODE) $(need "$1" DL_MODE_NONE) $(need "$1" DL_MENU_SCREEN) $(need "$1" MH_SCREEN_ID)"
}

# --------------------------------------------------------- per game: UYA

rac3_entry() { echo mh_msg_stub; }

rac3_ldflags() {
	echo "--defsym game_msg_exit=$(need "$1" GAME_MSG_EXIT) --defsym game_map_accept=$(need "$1" GAME_MAP_ACCEPT)"
}

rac3_hooks() {
	hdr=$1; elf=$2
	msg_stub=$(needsym "$elf" mh_msg_stub)
	help_desc=$(needsym "$elf" mh_help)
	[ $((help_desc)) -ge $((data)) ] && [ $((help_desc)) -lt $((data + data_size)) ] ||
		fail "rac3: mh_help's descriptor $help_desc is not in data.bin"
	site=$(need "$hdr" MH_SITE_MSG)
	echo "$site $(bword "$site" "$msg_stub") $(need "$hdr" MH_ORIG_MSG) b mh_msg_stub ($msg_stub)"
	for r in 8:ROWS8:mh_rows8_stub:MH_ORIG_ROWS8 9a:ROWS9A:mh_rows9a_stub:MH_ORIG_ROWS9 9b:ROWS9B:mh_rows9b_stub:MH_ORIG_ROWS9; do
		name=$(echo "$r" | cut -d: -f2); stub=$(echo "$r" | cut -d: -f3); orig=$(echo "$r" | cut -d: -f4)
		site=$(need "$hdr" MH_SITE_$name)
		at=$(needsym "$elf" "$stub")
		echo "$site $(blword "$site" "$at") $(need "$hdr" "$orig") bl $stub ($at)"
	done
	for r in INJECT:mh_inject_stub COMBO:mh_combo_stub LAUNCH:mh_launch_stub MAP_LIST:mh_map_list_stub; do
		name=${r%%:*}; stub=${r#*:}
		site=$(need "$hdr" MH_SITE_$name)
		at=$(needsym "$elf" "$stub")
		echo "$site $(blword "$site" "$at") $(need "$hdr" MH_ORIG_$name) bl $stub ($at)"
	done
	for t in 8 9A 9B; do
		echo "$(need "$hdr" MH_SITE_HELP${t}_TEXT) $(printf '0x%08X' $(($(need "$hdr" MH_MSG_HELP)))) $(need "$hdr" MH_ORIG_HELP_TEXT) HELP row's text: TRAINER (table $t)"
		echo "$(need "$hdr" MH_SITE_HELP${t}_CB) $help_desc $(need "$hdr" MH_ORIG_HELP_CB) HELP row's callback: mh_help (table $t)"
	done
}

rac3_query() {
	echo "$(need "$1" U_GAME_MODE) $(need "$1" U_MODE_PAUSE) $(need "$1" U_PENDING_MODE) $(need "$1" U_MODE_NONE) $(printf '0x%08X' $(($(need "$1" MH_SHIM) + $(need "$1" SH_UP)))) $(need "$1" U_SCREEN_UP)"
}

# ------------------------------------------------- per game: Going Commando

rac2_entry() { echo mh_msg_stub; }

rac2_ldflags() {
	echo "--defsym game_msg_exit=$(need "$1" GAME_MSG_EXIT)"
}

# The label hook, the Start path's three, then the root's Help entry: its
# screen, one of the screens at MH_SCREENS (the first of the data image's
# read-only data), and its label.
rac2_hooks() {
	hdr=$1; elf=$2
	screens=$(needsym "$elf" mh_screens)
	[ "$screens" = "$(printf '0x%08X' $(($(need "$hdr" MH_SCREENS))))" ] ||
		fail "rac2: mh_screens is at $screens, not MH_SCREENS: the stubs' read-only data is not the first"
	[ $((screens)) -ge $((data)) ] && [ $((screens + $(need "$hdr" MS_SIZE))) -le $((data + data_size)) ] ||
		fail "rac2: the screens are not in data.bin"
	for r in MSG:mh_msg_stub:b INJECT:mh_inject_stub:bl COMBO:mh_combo_stub:bl ROOT:mh_root_stub:bl; do
		name=$(echo "$r" | cut -d: -f1); stub=$(echo "$r" | cut -d: -f2); op=$(echo "$r" | cut -d: -f3)
		site=$(need "$hdr" MH_SITE_$name)
		at=$(needsym "$elf" "$stub")
		if [ "$op" = "b" ]; then word=$(bword "$site" "$at"); else word=$(blword "$site" "$at"); fi
		echo "$site $word $(need "$hdr" MH_ORIG_$name) $op $stub ($at)"
	done
	help=$(printf '0x%08X' $((screens + $(need "$hdr" MS_HELP) * $(need "$hdr" R2_SCREEN_SIZE))))
	echo "$(need "$hdr" MH_SITE_HELP_SCREEN) $help $(need "$hdr" MH_ORIG_HELP_SCREEN) the root's Help entry: the menu's Help screen"
	echo "$(need "$hdr" MH_SITE_HELP_LABEL) $(printf '0x%08X' $(($(need "$hdr" MH_MSG_HELP)))) $(need "$hdr" MH_ORIG_HELP_LABEL) the root's Help label: 0xF000, Trainer"
}

rac2_query() {
	echo "$(need "$1" R2_GAME_MODE) $(need "$1" R2_MODE_PAUSE) $(need "$1" R2_PENDING_MODE) $(need "$1" R2_MODE_NONE) $(printf '0x%08X' $(($(need "$1" MH_SHIM) + $(need "$1" SH_UP)))) $(need "$1" R2_SCREEN_UP)"
}

# ------------------------------------------------------ per game: Ratchet & Clank

rac1_entry() { echo mh_text_stub; }

rac1_ldflags() { :; }

# The shim's other files: the CONFIG widget and the planet picker.
rac1_extra() { echo mh_rac1_cfg mh_rac1_pick; }

rac1_hooks() {
	hdr=$1; elf=$2
	text_stub=$(needsym "$elf" mh_text_stub)
	play_stub=$(needsym "$elf" mh_play_stub)
	site=$(need "$hdr" MH_SITE_TEXT)
	echo "$site $(bword "$site" "$text_stub") $(need "$hdr" MH_ORIG_TEXT) b mh_text_stub ($text_stub)"
	site=$(need "$hdr" MH_SITE_PLAY)
	echo "$site $(blword "$site" "$play_stub") $(need "$hdr" MH_ORIG_PLAY) bl mh_play_stub ($play_stub)"
	echo "$(need "$hdr" MH_SITE_HELP_TEXT) $(need "$hdr" MH_HELP_WORD) $(need "$hdr" MH_ORIG_HELP_TEXT) Help entry's text and kind: Trainer, go to a page"
	echo "$(need "$hdr" MH_SITE_HELP_PAGE) $(printf '0x%08X' $(($(need "$hdr" MH_SHIM) + $(need "$hdr" SH_PAGE_LIST)))) $(need "$hdr" MH_ORIG_HELP_PAGE) Help entry's page: the menu's MAIN page"
}

rac1_query() {
	echo "$(need "$1" R1_STATE) $(need "$1" R1_STATE_PAUSE) $(need "$1" R1_RESUME_PAGE) $(need "$1" R1_RESUME_NONE) $(printf '0x%08X' $(($(need "$1" MH_SHIM) + $(need "$1" SH_UP)))) $(need "$1" R1_SCREEN_UP)"
}

# ---------------------------------------------------- several code areas

# An object's code sections, one a line: object section size alignment.
sec_list() {
	$OBJDUMP -h "$1" | tr -d '\r' |
		awk -v ob="$(basename "$1")" '$1 ~ /^[0-9]+$/ && $2 ~ /^\.text/ && $3 !~ /^0+$/ {
			split($7, a, "\\*\\*"); print ob, $2, strtonum("0x" $3), 2 ^ a[2] }'
}

# pack_map OBJ_DIR AREAS...: the link map of a renderer in several code areas,
# each AREA "org:end". The stubs and the core open the first; the shim's
# functions go, the largest first, into the first area with room.
pack_map() {
	o=$1; shift
	n=0
	for a in "$@"; do
		n=$((n + 1))
		echo "$n ${a%%:*} ${a#*:}"
	done > "$o/areas.txt"
	{ sec_list "$o/stubs.o"; sec_list "$o/mh_core.o"; } > "$o/fixed.txt"
	for f in shim $extra; do sec_list "$o/$f.o"; done | sort -k3,3nr -k2,2 > "$o/items.txt"
	awk -v fail=0 '
		function up(x, al) { return int((x + al - 1) / al) * al }
		FILENAME ~ /areas/ { cap[$1] = strtonum($3) - strtonum($2); used[$1] = 0; na = $1; next }
		FILENAME ~ /fixed/ { used[1] = up(used[1], $4) + $3; next }
		{
			for (i = 1; i <= na; i++) {
				off = up(used[i], $4)
				if (off + $3 <= cap[i]) { used[i] = off + $3; print i, $1, $2; next }
			}
			print "nowhere", $1, $2, $3; fail = 1
		}
		END { for (i = 1; i <= na; i++) printf "used %d %d %d\n", i, used[i], cap[i] > "/dev/stderr"; exit fail }
	' "$o/areas.txt" "$o/fixed.txt" "$o/items.txt" > "$o/placed.txt" 2> "$o/used.txt" ||
		fail "$g: the shim's functions do not fit its code areas: $(grep nowhere "$o/placed.txt" | tr '\n' ' ')"

	echo 'OUTPUT_FORMAT("elf64-powerpc-celloslv2")'
	echo "ENTRY($(${g}_entry))"
	echo
	echo 'SECTIONS'
	echo '{'
	while read -r i org end; do
		sec=.text; [ "$i" = 1 ] || sec=.text$i
		echo "	. = $org;"
		echo "	$sec : {"
		if [ "$i" = 1 ]; then
			echo "		*(.text.entry)"
			echo "		_core_start = .;"
			echo "		*mh_core.o(.text) *mh_core.o(.text.*)"
			echo "		_core_end = .;"
		fi
		awk -v i="$i" '$1 == i { printf "\t\t*%s(%s)\n", $2, $3 }' "$o/placed.txt"
		[ "$i" != 1 ] || echo "		*(.text)"
		echo "	}"
		[ "$i" = 1 ] && echo "	_text_end = .;" || echo "	_text${i}_end = .;"
		echo
	done < "$o/areas.txt"
	sed -n '/= @BOX_ORG@;/,$p' "$DIR/mh.ld.in" | sed -e "s/@BOX_ORG@/$mbox/" -e "s/@DATA_ORG@/$data/"
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
	mbox=$(need "$hdr" MH_BOX)
	mbox_end=$(need "$hdr" MH_BOX_END)
	data=$(need "$hdr" MH_DATA)
	data_end=$(need "$hdr" MH_DATA_END)
	state=$(need "$hdr" MH_STATE)
	shim=$(need "$hdr" MH_SHIM)
	mbox_size=$(need "$hdr" MH_MBOX_SIZE)
	# The code areas past the first (MH_CAVE2 ...), and the shim's other files.
	areas="$cave:$cave_end"
	ncodes=1
	while [ -n "$(getdef "$hdr" MH_CAVE$((ncodes + 1)))" ]; do
		ncodes=$((ncodes + 1))
		areas="$areas $(need "$hdr" MH_CAVE$ncodes):$(need "$hdr" MH_CAVE${ncodes}_END)"
	done
	pack=
	[ $ncodes -eq 1 ] || pack=-ffunction-sections
	extra=
	! type ${g}_extra >/dev/null 2>&1 || extra=$(${g}_extra)

	[ $((data)) -eq $((state)) ] || fail "$hdr: MH_STATE is not MH_DATA"

	# The core and the shim, each with the game's header first.
	$CC $CFLAGS $pack -include "$hdr" -c -o "$obj/mh_core.o" "$DIR/mh_core.c"
	$CC $CFLAGS $pack -include "$hdr" -c -o "$obj/shim.o" "$DIR/mh_$g.c"
	extra_o=
	for f in $extra; do
		$CC $CFLAGS $pack -include "$hdr" -c -o "$obj/$f.o" "$DIR/$f.c"
		extra_o="$extra_o $obj/$f.o"
	done
	# Through the driver, which runs the preprocessor for .S and stamps the
	# CellOS Lv-2 ABI on the object; the linker refuses to mix objects otherwise.
	$CC -I$DIR -c -o "$obj/stubs.o" "$DIR/mh_${g}_stubs.S"
	$CC -I$DIR -c -o "$obj/state.o" "$DIR/mh_${g}_state.S"

	if [ $ncodes -eq 1 ]; then
		sed -e "s/@TEXT_ORG@/$cave/" -e "s/@BOX_ORG@/$mbox/" -e "s/@DATA_ORG@/$data/" \
			-e "s/@ENTRY@/$(${g}_entry)/" "$DIR/mh.ld.in" > "$obj/mh.ld"
	else
		pack_map "$obj" $areas > "$obj/mh.ld" || exit 1
	fi
	$LD --fatal-warnings -T "$obj/mh.ld" -Map "$obj/mh.map" -o "$obj/mh.elf" \
		$(${g}_ldflags "$hdr") \
		"$obj/stubs.o" "$obj/mh_core.o" "$obj/shim.o" $extra_o "$obj/state.o"

	# name size vma, one line per section
	$OBJDUMP -h "$obj/mh.elf" | tr -d '\r' |
		awk '$1 ~ /^[0-9]+$/ { print $2, $3, $4 }' > "$obj/sections.txt"
	bad=$(awk '$2 !~ /^0+$/ && $1 !~ /^\.text[2-9]?$/ && $1 != ".mh_box" && $1 != ".mh_box_zero" &&
	           $1 != ".mh_state" && $1 != ".mh_state_zero" && $1 != ".rodata" &&
	           $1 != ".toc" && $1 != ".opd" { print $1 }' "$obj/sections.txt")
	[ -z "$bad" ] || fail "$g: sections that must be empty are not: $bad"

	text_vma=0x$(awk '$1 == ".text" { print $3 }' "$obj/sections.txt")
	box_vma=0x$(awk '$1 == ".mh_box" { print $3 }' "$obj/sections.txt")
	state_vma=0x$(awk '$1 == ".mh_state" { print $3 }' "$obj/sections.txt")
	[ $((text_vma)) -eq $((cave)) ] || fail "$g: .text starts at $text_vma, not $cave"
	[ $((box_vma)) -eq $((mbox)) ] || fail "$g: the mailbox starts at $box_vma, not $mbox"
	[ $((state_vma)) -eq $((data)) ] || fail "$g: the state starts at $state_vma, not $data"
	[ "$(sym "$obj/mh.elf" mh_box)" = "$(printf '0x%08X' $((mbox)))" ] ||
		fail "$g: mh_box is not at MH_BOX"
	[ "$(sym "$obj/mh.elf" mh_state)" = "$(printf '0x%08X' $((state)))" ] ||
		fail "$g: mh_state is not at MH_STATE"
	[ "$(sym "$obj/mh.elf" mh_shim)" = "$(printf '0x%08X' $((shim)))" ] ||
		fail "$g: mh_shim is at $(sym "$obj/mh.elf" mh_shim), not MH_SHIM: the core's state changed size"

	$OBJCOPY -O binary -j .text "$obj/mh.elf" "$obj/code.bin"
	n=1
	for a in $areas; do
		[ $n -eq 1 ] || {
			org=${a%%:*}; org_end=${a#*:}
			vma=0x$(awk -v s=".text$n" '$1 == s { print $3 }' "$obj/sections.txt")
			[ $((vma)) -eq $((org)) ] || fail "$g: .text$n starts at $vma, not $org"
			$OBJCOPY -O binary -j .text$n "$obj/mh.elf" "$obj/code$n.bin"
			size=$(wc -c < "$obj/code$n.bin" | tr -d ' ')
			[ $((org + size)) -eq $(($(sym "$obj/mh.elf" _text${n}_end))) ] ||
				fail "$g: code$n.bin is $size bytes, the link ends elsewhere"
			[ $((org + size)) -le $((org_end)) ] || fail "$g: code area $n holds $size bytes and runs past $org_end"
		}
		n=$((n + 1))
	done
	$OBJCOPY -O binary -j .mh_box "$obj/mh.elf" "$obj/box.bin"
	$OBJCOPY -O binary -j .mh_state "$obj/mh.elf" "$obj/state.bin"
	$OBJCOPY -O binary -j .rodata -j .toc -j .opd "$obj/mh.elf" "$obj/rest.bin"
	# The images as the game will hold them, for the checks below and check.py's.
	$OBJCOPY -O binary -j .mh_box -j .mh_box_zero "$obj/mh.elf" "$obj/box-full.bin"
	$OBJCOPY -O binary -j .mh_state -j .mh_state_zero -j .rodata -j .toc -j .opd "$obj/mh.elf" "$obj/data.bin"
	code_size=$(wc -c < "$obj/code.bin" | tr -d ' ')
	box_head=$(wc -c < "$obj/box.bin" | tr -d ' ')
	box_size=$(wc -c < "$obj/box-full.bin" | tr -d ' ')
	box_zeros=$((box_size - box_head))
	state_head=$(wc -c < "$obj/state.bin" | tr -d ' ')
	state_zeros=$((0x$(awk '$1 == ".mh_state_zero" { print $2 }' "$obj/sections.txt")))
	rest_vma=$(printf '0x%08X' $((0x$(awk '$1 == ".rodata" { print $3 }' "$obj/sections.txt"))))
	rest_size=$(wc -c < "$obj/rest.bin" | tr -d ' ')
	data_size=$(wc -c < "$obj/data.bin" | tr -d ' ')
	[ $((data + state_head + state_zeros)) -eq $((rest_vma)) ] ||
		fail "$g: the read-only data does not follow the state ($rest_vma)"
	[ $((state_head + state_zeros + rest_size)) -eq $((data_size)) ] ||
		fail "$g: the data image is not the state, its zeros and the read-only data"
	core_start=$(sym "$obj/mh.elf" _core_start)
	core_end=$(sym "$obj/mh.elf" _core_end)
	[ -n "$core_start" ] && [ -n "$core_end" ] || fail "$g: the core's bounds are missing"
	[ $((box_size)) -eq $((mbox_size)) ] && [ $((mbox + box_size)) -eq $(($(sym "$obj/mh.elf" _box_end))) ] ||
		fail "$g: box.bin is $box_size bytes, not the mailbox's $mbox_size"
	[ $((mbox + box_size)) -le $((mbox_end)) ] || fail "$g: the mailbox runs past $mbox_end"
	[ $((cave + code_size)) -eq $(($(sym "$obj/mh.elf" _text_end))) ] ||
		fail "$g: code.bin is $code_size bytes, the link ends elsewhere"
	[ $((data + data_size)) -eq $(($(sym "$obj/mh.elf" _data_end))) ] ||
		fail "$g: data.bin is $data_size bytes, the link ends elsewhere"
	[ $((cave + code_size)) -le $((cave_end)) ] || fail "$g: the code is $code_size bytes and runs past $cave_end"
	[ $((data + data_size)) -le $((data_end)) ] || fail "$g: the data is $data_size bytes and runs past $data_end"

	$OBJDUMP -d "$obj/mh.elf" > "$obj/mh.dis"

	# The hook words, from the linked addresses: address, value, the game's word, what.
	${g}_hooks "$hdr" "$obj/mh.elf" > "$obj/hooks.txt" || fail "$g: the hook words"
	nhooks=$(wc -l < "$obj/hooks.txt" | tr -d ' ')
	[ "$nhooks" -gt 0 ] || fail "$g: no hook words"
	query=$(${g}_query "$hdr")
	set -- $query
	[ $# -eq 6 ] || fail "$g: what says the menu is down is not six words"
	q_mode=$1; q_paused=$2; q_pending=$3; q_none=$4; q_screen=$5; q_screen_id=$6

	{
		echo "/* $g: the mailbox, $box_size bytes at $mbox as the install leaves it: the header, then $box_zeros zeros */"
		echo "static const u8 mh_${g}_box[] = {"
		emit_bytes "$obj/box.bin"
		echo "};"
		echo
		echo "/* $g: the core's and the shim's state at $data: the core's magic, then $state_zeros zeros */"
		echo "static const u8 mh_${g}_state[] = {"
		emit_bytes "$obj/state.bin"
		echo "};"
		echo
		echo "/* $g: the read-only data, the TOC and the descriptors, $rest_size bytes at $rest_vma */"
		echo "static const u8 mh_${g}_data[] = {"
		emit_bytes "$obj/rest.bin"
		echo "};"
		echo
		echo "/* $g: the code, $code_size bytes at $cave: the stubs, then the core ($core_start..$core_end), then the shim */"
		echo "static const u8 mh_${g}_code[] = {"
		emit_bytes "$obj/code.bin"
		echo "};"
		echo
		n=1
		for a in $areas; do
			if [ $n -gt 1 ]; then
				echo "/* $g: the code, $(wc -c < "$obj/code$n.bin" | tr -d ' ') bytes at ${a%%:*}: more of the shim */"
				echo "static const u8 mh_${g}_code$n[] = {"
				emit_bytes "$obj/code$n.bin"
				echo "};"
				echo
			fi
			n=$((n + 1))
		done
		echo "static const struct mh_cave mh_${g}_caves[] = {"
		echo "	{ ${mbox}u, mh_${g}_box, (u32)sizeof(mh_${g}_box), 0, ${box_zeros}u },"
		echo "	{ ${data}u, mh_${g}_state, (u32)sizeof(mh_${g}_state), 0, ${state_zeros}u },"
		echo "	{ ${rest_vma}u, mh_${g}_data, (u32)sizeof(mh_${g}_data), 0, 0u },"
		n=1
		for a in $areas; do
			sep=","
			[ $n -lt $ncodes ] || sep=""
			[ $n -eq 1 ] && name=mh_${g}_code || name=mh_${g}_code$n
			echo "	{ ${a%%:*}u, $name, (u32)sizeof($name), 1, 0u }$sep"
			n=$((n + 1))
		done
		echo "};"
		echo
		echo "/* In the order they go in; they come out in the reverse order. */"
		echo "static const struct mh_word mh_${g}_hooks[] = {"
		k=0
		while read -r h_addr h_value h_orig h_what; do
			k=$((k + 1))
			sep=","
			[ $k -lt $nhooks ] || sep=" "
			printf '\t{ 0x%08Xu, 0x%08Xu, 0x%08Xu }%s   /* %s */\n' \
				$((h_addr)) $((h_value)) $((h_orig)) "$sep" "$h_what"
		done < "$obj/hooks.txt"
		echo "};"
		echo
	} >> "$OUT/arrays.c"

	{
		echo "	{ $((game)), mh_${g}_caves, $((3 + ncodes)), mh_${g}_hooks, $nhooks,"
		echo "	  ${mbox}u, ${mbox_size}u, ${state}u,"
		echo "	  ${q_mode}u, ${q_paused}u, ${q_pending}u, ${q_none}u,"
		echo "	  ${q_screen}u, ${q_screen_id}u },"
	} >> "$OUT/descs.txt"

	{
		n=2
		for a in $(echo "$areas" | cut -s -d' ' -f2-); do
			size=$(wc -c < "$obj/code$n.bin" | tr -d ' ')
			echo "$g: code area $n, $size bytes at ${a%%:*} ($((${a#*:} - ${a%%:*} - size)) left)"
			n=$((n + 1))
		done
		echo "$g: code $code_size bytes at $cave ($((cave_end - cave - code_size)) left), mailbox $box_size bytes at $mbox, data $data_size bytes at $data ($((data_end - data - data_size)) left); the core is $((core_end - core_start)) bytes of the code"
		while read -r h_addr h_value h_orig h_what; do
			echo "  $h_addr -> $h_value  $h_what"
		done < "$obj/hooks.txt"
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

/*
 * One run of bytes at one address - the mailbox, the state, the read-only
 * data, or the code - and then `zeros` bytes of 0, which the module carries as
 * a count rather than as bytes.
 */
struct mh_cave {
	u32 addr;
	const u8 *bytes;
	u32 len;
	u8  code;      /* 1 for the code, which an install compares before adopting */
	u32 zeros;
};

/* One word a renderer replaces, with the game's own word there. */
struct mh_word {
	u32 addr;
	u32 value;
	u32 original;
};

struct mh_desc {
	u8 game_id;

	/* The mailbox, the state and the read-only data first, then the code: all before any hook. */
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
