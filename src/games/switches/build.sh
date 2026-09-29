#!/bin/sh
#
# Builds the code switches for all four games and regenerates the committed
# source qwark carries them in, src/games/switches_bins.c and .h.
#
# Run it from a Cygwin login shell that has the Sony SDK on its PATH, which is
# the same shell `make` runs in:
#
#     sh src/games/switches/build.sh              build and regenerate
#     sh src/games/switches/build.sh --if-stale   only when a source is newer
#
# --if-stale is what `make` uses. It does nothing at all when the generated file
# is up to date, and nothing at all when the SDK is not on the PATH, so a build
# on a machine without the cross assembler still works: the generated file is
# committed, and the host builds and the tests never need the SDK.
#
# Per game the steps are: assemble sw_<game>.s, link it with sw_<game>.ld, which
# places every .cave.* section at its trampoline's address and every .site.*
# section at the instruction it replaces, and cut each of those sections out as
# raw bytes. Every branch - into a trampoline, back out of it, and to the game's
# own functions - is therefore computed by the link rather than by hand. The
# flag bytes are the SW_FLAG_* symbols the source defines.
#
# The SDK's tools are Windows programs, so every path handed to them here is
# relative to the repository root.
set -e

cd "$(dirname "$0")/../../.."

DIR=src/games/switches
OUT=$DIR/out
GEN_C=src/games/switches_bins.c
GEN_H=src/games/switches_bins.h

CC=ppu-lv2-gcc
LD=ppu-lv2-ld
NM=ppu-lv2-nm
OBJCOPY=ppu-lv2-objcopy
OBJDUMP=ppu-lv2-objdump

ASFLAGS="-Wa,-mregnames"

GAMES="rac1 rac2 rac3 rac4"

# ---------------------------------------------------------------- staleness

sources="$DIR/build.sh"
for g in $GAMES; do
	sources="$sources $DIR/sw_$g.s $DIR/sw_$g.ld"
done

if [ "$1" = "--if-stale" ]; then
	if ! command -v $CC >/dev/null 2>&1; then
		echo "switches: no $CC on the PATH, keeping the committed $GEN_C"
		exit 0
	fi

	stale=0
	[ -f "$GEN_C" ] || stale=1
	[ -f "$GEN_H" ] || stale=1
	for s in $sources; do
		if [ "$s" -nt "$GEN_C" ]; then stale=1; fi
	done

	if [ "$stale" = "0" ]; then exit 0; fi
	echo "switches: a source is newer than $GEN_C, rebuilding the switches"
fi

command -v $CC >/dev/null 2>&1 || {
	echo "switches: $CC is not on the PATH; run this from the SDK's Cygwin shell" >&2
	exit 1
}

# --------------------------------------------------------------- little tools

# The C array body for a raw binary: twelve bytes a line, tab indented, and no
# space left at the end of a line.
emit_bytes() {
	od -An -v -tx1 "$1" | awk '
		{ for (i = 1; i <= NF; i++) b[n++] = toupper($i) }
		END {
			for (k = 0; k < n; k++) {
				if (k % 12 == 0) printf "\t"
				printf "0x%s,", b[k]
				if (k % 12 == 11 || k == n - 1) printf "\n"; else printf " "
			}
		}'
}

# A four-byte binary as one big-endian word, 0x%08X.
word_of() {
	od -An -v -tx1 "$1" | tr -d ' \n\r' | awk '{ printf "0x%s", toupper($0) }'
}

# The address an absolute symbol of the linked ELF stands for, 0x%08X, or
# nothing when the ELF has no such symbol.
sym_of() {
	value=$($NM "$1" | tr -d '\r' |
		awk -v s="$2" '$3 == s && ($2 == "A" || $2 == "a") { print $1 }')
	[ -z "$value" ] || printf '0x%08X' $((0x$value))
}

upper() {
	echo "$1" | tr a-z A-Z
}

game_number() {
	case "$1" in
	rac1) echo 1 ;;
	rac2) echo 2 ;;
	rac3) echo 3 ;;
	rac4) echo 4 ;;
	esac
}

rm -rf "$OUT"
mkdir -p "$OUT"

: > "$OUT/defines.h"
: > "$OUT/arrays.c"
: > "$OUT/descs.txt"
: > "$OUT/report.txt"

# ------------------------------------------------------------ build each game

for g in $GAMES; do
	G=$(upper "$g")
	elf=$OUT/$g.elf

	# Through the compiler driver, not ppu-lv2-as: the driver stamps the CellOS
	# Lv-2 ABI byte on the object, which the linker's output format wants.
	$CC $ASFLAGS -c -o "$OUT/$g.o" "$DIR/sw_$g.s"
	$LD -T "$DIR/sw_$g.ld" -o "$elf" "$OUT/$g.o"

	# name size vma, one line per section, in address order. An empty section
	# is nothing to write and is left out: ld leaves an empty long-branch table
	# behind, and the assembler's own .text, .data and .bss are empty here.
	$OBJDUMP -h "$elf" | tr -d '\r' |
		awk '$1 ~ /^[0-9]+$/ && $3 !~ /^0+$/ { print $4, $2, $3 }' |
		sort | awk '{ print $2, $3, $1 }' > "$OUT/$g.sections"

	# Anything else with bytes in it would be dropped by the cut below without
	# a word, so it fails the build instead.
	unexpected=$(awk '$1 !~ /^\.cave\./ && $1 !~ /^\.site\./ { print $1 }' "$OUT/$g.sections")
	[ -z "$unexpected" ] || {
		echo "switches: $g put code or data outside .cave.* and .site.*: $unexpected" >&2
		exit 1
	}

	cave_start=$(sym_of "$elf" SW_CAVE_START)
	cave_end=$(sym_of "$elf" SW_CAVE_END)
	[ -n "$cave_start" ] && [ -n "$cave_end" ] || {
		echo "switches: sw_$g.ld does not define SW_CAVE_START and SW_CAVE_END" >&2
		exit 1
	}

	caves=""
	ncaves=0
	sites=""
	nsites=0
	: > "$OUT/$g.sitedefs"

	while read -r section size vma; do
		name=$(echo "$section" | sed 's/^\.[a-z]*\.//')
		addr=$(printf '0x%08X' $((0x$vma)))
		len=$((0x$size))
		bin=$OUT/$g$(echo "$section" | tr . _).bin

		$OBJCOPY -O binary --only-section="$section" "$elf" "$bin"
		[ "$(wc -c < "$bin" | tr -d ' ')" = "$len" ] || {
			echo "switches: $g $section cut out as the wrong length" >&2
			exit 1
		}

		case "$section" in
		.cave.*)
			if [ $((addr)) -lt $((cave_start)) ] || [ $((addr + len)) -gt $((cave_end)) ]; then
				echo "switches: $g $section at $addr, $len bytes, is outside $cave_start..$cave_end" >&2
				exit 1
			fi

			{
				echo "/* $g $section at $addr, $len bytes */"
				echo "static const u8 sw_${g}_${name}[] = {"
				emit_bytes "$bin"
				echo "};"
				echo
			} >> "$OUT/arrays.c"

			caves="$caves	{ ${addr}u, sw_${g}_${name}, (u32)sizeof(sw_${g}_${name}) },
"
			ncaves=$((ncaves + 1))
			echo "  $g cave $name $addr $len bytes" >> "$OUT/report.txt"
			;;

		.site.*)
			[ "$len" = "4" ] || {
				echo "switches: $g $section is $len bytes, not one instruction" >&2
				exit 1
			}

			word=$(word_of "$bin")

			# Every site is a plain relative `b`, no link and no absolute
			# address: the trampoline branches back itself.
			[ $((word & 0xFC000003)) -eq $((0x48000000)) ] || {
				echo "switches: $g $section is $word, not a relative branch" >&2
				exit 1
			}
			disp=$((word & 0x03FFFFFC))
			[ $disp -lt $((0x02000000)) ] || disp=$((disp - 0x04000000))
			target=$(printf '0x%08X' $((addr + disp)))

			sites="$sites	{ ${addr}u, ${word}u },   /* $name -> $target */
"
			nsites=$((nsites + 1))
			{
				echo "#define SW_${G}_SITE_$(upper "$name")_ADDR ${addr}u"
				echo "#define SW_${G}_SITE_$(upper "$name") ${word}u"
			} >> "$OUT/$g.sitedefs"
			echo "  $g site $name $addr = $word (-> $target)" >> "$OUT/report.txt"
			;;
		esac
	done < "$OUT/$g.sections"

	[ "$ncaves" -gt 0 ] && [ "$nsites" -gt 0 ] || {
		echo "switches: $g has no caves or no sites" >&2
		exit 1
	}

	# The flag bytes, in address order.
	$NM "$elf" | tr -d '\r' |
		awk '($2 == "A" || $2 == "a") && $3 ~ /^SW_FLAG_/ { print $1, substr($3, 9) }' |
		sort > "$OUT/$g.flags"

	flags=""
	nflags=0
	echo "/* $g: its flag bytes, then its sites */" >> "$OUT/defines.h"
	while read -r value fname; do
		addr=$(printf '0x%08X' $((0x$value)))
		flags="$flags	${addr}u,   /* $(echo "$fname" | tr A-Z a-z) */
"
		nflags=$((nflags + 1))
		echo "#define SW_${G}_FLAG_${fname} ${addr}u" >> "$OUT/defines.h"
		echo "  $g flag $fname $addr" >> "$OUT/report.txt"
	done < "$OUT/$g.flags"

	[ "$nflags" -gt 0 ] || { echo "switches: $g defines no SW_FLAG_ byte" >&2; exit 1; }

	cat "$OUT/$g.sitedefs" >> "$OUT/defines.h"
	echo >> "$OUT/defines.h"

	{
		echo "static const struct sw_cave sw_${g}_caves[] = {"
		printf '%s' "$caves" | sed '$ s/},$/}/'
		echo "};"
		echo
		echo "static const struct patch_word sw_${g}_sites[] = {"
		printf '%s' "$sites" | sed '$ s/},   /}    /'
		echo "};"
		echo
		echo "static const u32 sw_${g}_flags[] = {"
		printf '%s' "$flags" | sed '$ s/u,   /u    /'
		echo "};"
		echo
	} >> "$OUT/arrays.c"

	echo "	{ GAME_${G}, sw_${g}_caves, $ncaves, sw_${g}_sites, $nsites, sw_${g}_flags, $nflags }," >> "$OUT/descs.txt"
done

# The last descriptor must not carry a trailing comma.
sed '$ s/},$/}/' "$OUT/descs.txt" > "$OUT/descs-final.txt"

# --------------------------------------------------------------- emit the source

{
	cat <<'EOF'
/*
 * The code switches, as bytes: what an RPCS3 patch writes into each game so
 * that qwark's instruction-patch toggles become flag bytes it can write.
 *
 * GENERATED by src/games/switches/build.sh out of the sources beside it. Do not
 * edit it by hand; edit the sources and run
 *
 *     sh src/games/switches/build.sh
 *
 * from the SDK's Cygwin shell. It is committed because the trampolines are
 * PowerPC code the Sony cross assembler and linker produce, and the host builds
 * and the unit tests are clang on a PC with no SDK anywhere near them: they
 * still have to compile the table, hand it out and find it in the fake console.
 *
 * src/games/switches/sw_rac1.s says what a switch is.
 */
#ifndef QWARK_SWITCHES_BINS_H
#define QWARK_SWITCHES_BINS_H

#include "../core/mem.h"
#include "../core/proto.h"

/*
 * Per game: every flag byte, 1 to run the patched form of its instructions and
 * 0 for the game's own, and every site, the address of an instruction the patch
 * replaces and the branch word it puts there.
 */
EOF
	cat "$OUT/defines.h"
	cat <<'EOF'
/* One trampoline, or the one piece of data a trampoline reads. */
struct sw_cave {
	u32 addr;
	const u8 *bytes;
	u32 len;
};

/*
 * Everything one game's switches are: the caves, the site words that branch
 * into them, and the flag bytes they read. All three are in address order.
 */
struct sw_desc {
	u8 game_id;

	const struct sw_cave *caves;
	u8 ncaves;

	const struct patch_word *sites;
	u8 nsites;

	const u32 *flags;
	u8 nflags;
};

/* NULL for a game with no switches, and for GAME_NONE. */
const struct sw_desc *sw_desc_for_game(u8 game_id);

#endif /* QWARK_SWITCHES_BINS_H */
EOF
} > "$GEN_H"

{
	cat <<'EOF'
/*
 * The code switches, as bytes.
 *
 * GENERATED by src/games/switches/build.sh out of the sources beside it. Do not
 * edit it by hand; edit the sources and run
 *
 *     sh src/games/switches/build.sh
 *
 * from the SDK's Cygwin shell.
 *
 * Each array is one cave: a trampoline, or the table Deadlocked's bot-crash
 * trampoline loads. Each site word is the branch the link computed from where
 * sw_<game>.ld placed the site and its trampoline.
 */
#include "switches_bins.h"

EOF
	cat "$OUT/arrays.c"
	echo "static const struct sw_desc sw_descs[] = {"
	cat "$OUT/descs-final.txt"
	echo "};"
	cat <<'EOF'

const struct sw_desc *sw_desc_for_game(u8 game_id)
{
	unsigned i;

	for (i = 0; i < sizeof(sw_descs) / sizeof(sw_descs[0]); i++) {
		if (sw_descs[i].game_id == game_id) return &sw_descs[i];
	}
	return NULL;
}
EOF
} > "$GEN_C"

echo "switches: regenerated $GEN_C and $GEN_H"
cat "$OUT/report.txt"
