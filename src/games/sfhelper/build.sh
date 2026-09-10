#!/bin/sh
#
# Builds the savefile helper for all four games and regenerates the committed
# source qwark carries it in, src/games/sfhelper_bins.c and .h.
#
# Run it from a Cygwin login shell that has the Sony SDK on its PATH, which is
# the same shell `make` runs in:
#
#     sh src/games/sfhelper/build.sh              build and regenerate
#     sh src/games/sfhelper/build.sh --if-stale   only when a source is newer
#
# --if-stale is what `make` uses. It does nothing at all when the generated file
# is up to date, and nothing at all when the SDK is not on the PATH, so a build
# on a machine without the cross compiler still works: the generated file is
# committed, and the host builds and the tests never need the SDK.
#
# Per game the steps are: compile sfhelper.c against that game's header, assemble
# the game's stub if it has one, link both at the cave addresses, cut each output
# section out as raw bytes, and read the hook target's address back out of the
# ELF so the branch word can be computed rather than copied.
set -e

cd "$(dirname "$0")/../../.."

DIR=src/games/sfhelper
OUT=$DIR/out
GEN_C=src/games/sfhelper_bins.c
GEN_H=src/games/sfhelper_bins.h

CC=ppu-lv2-gcc
LD=ppu-lv2-ld
NM=ppu-lv2-nm
OBJCOPY=ppu-lv2-objcopy
OBJDUMP=ppu-lv2-objdump

CFLAGS="-O2 -std=gnu99 -nostdlib -fno-builtin -Wall -Wextra -Werror -I$DIR"
ASFLAGS="-Wa,-mregnames"

GAMES="rac1 rac2 rac3 rac4"

# ---------------------------------------------------------------- staleness

sources="$DIR/sfhelper.c $DIR/sfhelper.h $DIR/sfhelper.ld.in $DIR/build.sh"
for g in $GAMES; do
	sources="$sources $DIR/sf_$g.h"
	if [ -f "$DIR/sf_${g}_stub.s" ]; then sources="$sources $DIR/sf_${g}_stub.s"; fi
done

if [ "$1" = "--if-stale" ]; then
	if ! command -v $CC >/dev/null 2>&1; then
		echo "sfhelper: no $CC on the PATH, keeping the committed $GEN_C"
		exit 0
	fi

	stale=0
	[ -f "$GEN_C" ] || stale=1
	[ -f "$GEN_H" ] || stale=1
	for s in $sources; do
		if [ "$s" -nt "$GEN_C" ]; then stale=1; fi
	done

	if [ "$stale" = "0" ]; then exit 0; fi
	echo "sfhelper: a source is newer than $GEN_C, rebuilding the helper"
fi

command -v $CC >/dev/null 2>&1 || {
	echo "sfhelper: $CC is not on the PATH; run this from the SDK's Cygwin shell" >&2
	exit 1
}

# --------------------------------------------------------------- little tools

# A plain `#define NAME 0x...` or `#define NAME <digits>` out of a header. The
# carriage returns come off first, as they do for the objdump and nm output
# below: a checkout with CRLF endings would otherwise leave one on the end of
# every address and match nothing at all.
getdef() {
	value=$(tr -d '\r' < "$1" |
		sed -n "s/^#define[ \t][ \t]*$2[ \t][ \t]*\(0x[0-9A-Fa-f][0-9A-Fa-f]*\|[0-9][0-9]*\)[ \t]*\$/\1/p")
	echo "$value"
}

need() {
	value=$(getdef "$1" "$2")
	[ -n "$value" ] || { echo "sfhelper: $1 does not define $2" >&2; exit 1; }
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

rm -rf "$OUT"
mkdir -p "$OUT"

# ------------------------------------------------------------ build each game

# One line per game for the descriptor table, built up as the games go by.
: > "$OUT/descs.txt"
: > "$OUT/arrays.c"
: > "$OUT/report.txt"

game_number() {
	case "$1" in
	rac1) echo 1 ;;
	rac2) echo 2 ;;
	rac3) echo 3 ;;
	rac4) echo 4 ;;
	esac
}

for g in $GAMES; do
	hdr=$DIR/sf_$g.h
	num=$(game_number "$g")
	stub=$DIR/sf_${g}_stub.s

	cave=$(need "$hdr" SF_CAVE)
	hook_addr=$(need "$hdr" SF_HOOK_ADDR)
	hook_abs=$(need "$hdr" SF_HOOK_ABSOLUTE)
	api_mod=$(need "$hdr" SF_API_MOD)
	api_load=$(need "$hdr" SF_API_LOAD)
	api_setaside=$(need "$hdr" SF_API_SETASIDE)
	aside_addr=$(need "$hdr" SF_ASIDE_ADDR)
	aside_size=$(need "$hdr" SF_ASIDE_SIZE)

	if [ -f "$stub" ]; then
		stub_org=$(need "$hdr" SF_CAVE_STUB)
		hook_sym=.sf_input_hook
	else
		stub_org=$cave
		hook_sym=.sf_entry
	fi

	# Every SF_FN_<NAME> becomes an undefined symbol the link binds to a hard
	# address, so the header is the only place a game function's address appears.
	defsyms=$(tr -d '\r' < "$hdr" |
		sed -n 's/^#define[ \t][ \t]*SF_FN_\([A-Z0-9_]*\)[ \t][ \t]*\(0x[0-9A-Fa-f]*\)[ \t]*$/\1 \2/p' |
		awk '{ printf "--defsym .sf_%s=%s ", tolower($1), $2 }')
	[ -n "$defsyms" ] || { echo "sfhelper: $hdr names no SF_FN_ function" >&2; exit 1; }

	objs="$OUT/$g.o"
	$CC $CFLAGS -DSF_GAME=$num -c -o "$OUT/$g.o" "$DIR/sfhelper.c"
	if [ -f "$stub" ]; then
		# Through the compiler driver, not ppu-lv2-as: the driver stamps the
		# CellOS Lv-2 ABI byte on the object, and an object without it makes the
		# linker pick plain PPC64 Linux and then refuse the compiler's own output.
		$CC $ASFLAGS -c -o "$OUT/$g-stub.o" "$stub"
		objs="$OUT/$g-stub.o $objs"
	fi

	sed -e "s/@STUB_ORG@/$stub_org/" -e "s/@TEXT_ORG@/$cave/" \
		"$DIR/sfhelper.ld.in" > "$OUT/$g.ld"

	# shellcheck disable=SC2086
	$LD -T "$OUT/$g.ld" $defsyms -o "$OUT/$g.elf" $objs

	# Anything the compiler put outside the two sections the map places would be
	# dropped silently by the section-by-section cut below, so refuse it here
	# instead: a helper that needs .rodata or .data is a helper to rewrite. An
	# empty section is not a problem; ld leaves an empty long-branch table behind.
	unexpected=$($OBJDUMP -h "$OUT/$g.elf" | tr -d '\r' |
		awk '$1 ~ /^[0-9]+$/ && $3 !~ /^0+$/ && $2 != ".text" && $2 != ".text.stub" { print $2 }')
	[ -z "$unexpected" ] || {
		echo "sfhelper: $g put code or data in unexpected sections: $unexpected" >&2
		exit 1
	}

	caves=""
	ncaves=0
	for section in .text.stub .text; do
		bin=$OUT/$g$(echo "$section" | tr . _).bin
		$OBJCOPY -O binary --only-section=$section "$OUT/$g.elf" "$bin" 2>/dev/null || true
		[ -s "$bin" ] || continue

		if [ "$section" = ".text.stub" ]; then addr=$stub_org; else addr=$cave; fi
		name="sf_${g}_$(echo "$section" | sed 's/^\.//; s/\./_/g')"
		size=$(wc -c < "$bin" | tr -d ' ')

		{
			echo "/* $g, $section at $addr, $size bytes */"
			echo "static const u8 ${name}[] = {"
			emit_bytes "$bin"
			echo "};"
			echo
		} >> "$OUT/arrays.c"

		caves="$caves	{ ${addr}u, ${name}, (u32)sizeof(${name}) },
"
		ncaves=$((ncaves + 1))
		echo "  $g $section $addr $size bytes" >> "$OUT/report.txt"
	done

	# The hook target's address comes out of the ELF, so the branch word is
	# computed from what was actually linked rather than copied from a mod.
	target=$($NM "$OUT/$g.elf" | tr -d '\r' |
		awk -v s="$hook_sym" '$3 == s { print "0x" $1 }')
	[ -n "$target" ] || { echo "sfhelper: $g has no $hook_sym symbol" >&2; exit 1; }

	if [ "$hook_abs" = "1" ]; then
		# `bla target`: AA and LK both set, and the target has to be a positive
		# 26-bit address.
		[ $((target)) -lt $((0x02000000)) ] ||
			{ echo "sfhelper: $g cave $target is out of absolute branch range" >&2; exit 1; }
		word=$(printf '0x%08X' $(( 0x48000000 | (target & 0x03FFFFFC) | 0x3 )))
	else
		# `bl target`: a signed 26-bit displacement from the hook site.
		delta=$((target - hook_addr))
		[ $delta -lt $((0x02000000)) ] && [ $delta -gt -$((0x02000000)) ] ||
			{ echo "sfhelper: $g cave is out of relative branch range of $hook_addr" >&2; exit 1; }
		word=$(printf '0x%08X' $(( 0x48000000 | (delta & 0x03FFFFFC) | 0x1 )))
	fi

	{
		echo "static const struct sf_cave sf_${g}_caves[] = {"
		printf '%s' "$caves"
		echo "};"
		echo
		echo "static const struct patch_word sf_${g}_hooks[] = {"
		echo "	{ ${hook_addr}u, ${word}u }"
		echo "};"
		echo
	} >> "$OUT/arrays.c"

	echo "	{ GAME_$(echo "$g" | tr a-z A-Z), sf_${g}_caves, $ncaves, sf_${g}_hooks, 1," >> "$OUT/descs.txt"
	echo "	  ${api_mod}u, ${api_load}u, ${api_setaside}u, ${aside_addr}u, ${aside_size}u }," >> "$OUT/descs.txt"
	echo "  $g hook $hook_addr = $word (-> $target)" >> "$OUT/report.txt"
done

# The last descriptor must not carry a trailing comma.
sed '$ s/},$/}/' "$OUT/descs.txt" > "$OUT/descs-final.txt"

# --------------------------------------------------------------- emit the source

cat > "$GEN_H" <<'EOF'
/*
 * The savefile helper, as bytes: what qwark writes into each game so that the
 * game can put its save data somewhere qwark can read it.
 *
 * GENERATED by src/games/sfhelper/build.sh out of the sources beside it. Do not
 * edit it by hand; edit the helper and run
 *
 *     sh src/games/sfhelper/build.sh
 *
 * from the SDK's Cygwin shell. It is committed because the helper is PowerPC
 * code the Sony cross compiler produces, and the host builds and the unit tests
 * are clang on a PC with no SDK anywhere near them: they still have to compile
 * the table, install it into the fake console and check the bytes that land.
 */
#ifndef QWARK_SFHELPER_BINS_H
#define QWARK_SFHELPER_BINS_H

#include "../core/mem.h"
#include "../core/proto.h"

/* One run of bytes at one address. A game has one, or two when it needs a stub. */
struct sf_cave {
	u32 addr;
	const u8 *bytes;
	u32 len;
};

/*
 * Everything qwark needs to install one game's helper and talk to it. The
 * addresses are the helper's own, out of src/games/sfhelper/sf_<game>.h, so this
 * file is the one place in qwark that knows them.
 */
struct sf_desc {
	u8 game_id;

	const struct sf_cave *caves;
	u8 ncaves;

	/* The words that branch the game into the caves. */
	const struct patch_word *hooks;
	u8 nhooks;

	/* The helper writes 1 here every call, so 1 means it is running. */
	u32 api_mod;
	/* The client writes 1 to either; the helper clears it when it is done. */
	u32 api_load;
	u32 api_setaside;

	/* Where the save is parked, and how much of it there is. */
	u32 aside_addr;
	u32 aside_size;
};

/* NULL for a game with no helper, and for GAME_NONE. */
const struct sf_desc *sf_desc_for_game(u8 game_id);

#endif /* QWARK_SFHELPER_BINS_H */
EOF

{
	cat <<'EOF'
/*
 * The savefile helper, as bytes.
 *
 * GENERATED by src/games/sfhelper/build.sh out of the sources beside it. Do not
 * edit it by hand; edit the helper and run
 *
 *     sh src/games/sfhelper/build.sh
 *
 * from the SDK's Cygwin shell.
 *
 * Each array is one code cave: the compiled helper, and for RaC1 the input hook
 * stub that goes in front of it. Each hook word is the branch that reaches a
 * cave, computed from the address the helper actually linked at rather than
 * copied out of a mod.
 */
#include "sfhelper_bins.h"

EOF
	cat "$OUT/arrays.c"
	echo "static const struct sf_desc sf_descs[] = {"
	cat "$OUT/descs-final.txt"
	echo "};"
	cat <<'EOF'

const struct sf_desc *sf_desc_for_game(u8 game_id)
{
	unsigned i;

	for (i = 0; i < sizeof(sf_descs) / sizeof(sf_descs[0]); i++) {
		if (sf_descs[i].game_id == game_id) return &sf_descs[i];
	}
	return NULL;
}
EOF
} > "$GEN_C"

echo "sfhelper: regenerated $GEN_C and $GEN_H"
cat "$OUT/report.txt"
