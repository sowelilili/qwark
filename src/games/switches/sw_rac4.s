# Ratchet: Deadlocked (NPEA00423): the code switches.
#
# sw_rac1.s says what a switch is and why it exists: an RPCS3 patch replaces
# each instruction qwark patches on a console with a branch to a trampoline
# here, and the trampoline runs the patched form or the original depending on a
# flag byte, so under RPCS3 the checkbox is a byte write. Deadlocked has three
# such toggles and fifteen sites between them.
#
# The flags are three bytes of zero-filled BSS thirteen bytes past the savefile
# helper's scratch (0x15CD71D..0x15CD773): zero at every boot, and nothing in
# the executable refers to them.
#
# The caves run 0x667BCC..0x667DB8, over string helpers of the shipped lobby
# code that nothing reaches, directly below the savefile helper's own cave at
# 0x667F70; 0x667DB8..0x667F70 is spare. Each cave follows the last with no
# gap. The table the bot-crash trampoline loads is the one piece of data,
# sixteen bytes on a sixteen-byte boundary, and build 53's fast loads put the
# second halves of two trampolines and a third one after it, in what was the
# spare span, so that nothing before them moved (below).
#
# Nothing reaches the span: its eight functions (0x667BCC, 0x667C4C, 0x667CCC,
# 0x667D4C, 0x667DCC, 0x667E4C, 0x667ECC and 0x667F68) are branched to only
# from inside themselves, and no word of the executable holds the address of
# any of their descriptors (0x9A0870..0x9A08A8).
#
# build.sh assembles this, links it with sw_rac4.ld and cuts each .cave.* and
# .site.* section out as bytes, so every branch below is computed by the link.

	.set    SW_FLAG_CRASH_PATCHES, 0x015CD780
	.set    SW_FLAG_FAST_LOADS,    0x015CD781
	.set    SW_FLAG_REFILL_AMMO,   0x015CD782
	.globl  SW_FLAG_CRASH_PATCHES
	.globl  SW_FLAG_FAST_LOADS
	.globl  SW_FLAG_REFILL_AMMO

# ----------------------------------------------------------------- refill ammo
#
# 0x1D7D18: beq 0x1D7D24, which skips the call that refills the weapon. Patched:
# nop, so the call always runs. It is also qwark's Deadlocked fingerprint, which
# accepts this branch as well once the switches are in.
#
# The condition is tested first, on the cr0 the game set, and only the taken
# path looks at the flag: there r3 is the byte the game compared with 0 and cr0
# says equal, and with the flag clear both read exactly that way again before
# the branch the game would have taken. With the flag set the refill call runs,
# and it sets r3 itself before it reads it.
	.section .cave.refill_ammo, "ax"
refill_ammo:
	bne     cr0, 1f
	lis     r3, SW_FLAG_REFILL_AMMO@ha
	lbz     r3, SW_FLAG_REFILL_AMMO@l(r3)
	cmpwi   cr0, r3, 0
	bne     cr0, 1f
	b       refill_ammo_ret + 8     # the beq's own target, 0x1D7D24
1:	b       refill_ammo_ret

	.section .site.refill_ammo, "ax"
	b       refill_ammo
refill_ammo_ret:

# ------------------------------------------------------------------ fast loads
#
# Build 53, on RaC2's model: rac4.h says what the six words a console writes do
# and why. Here they are three sites on one flag, one for each group of them
# that runs together, so a flag that changes between two of them can never
# leave half the patch running:
#
#   0x2A8AF4  the autosave's call, and with it its two argument words before it
#             (0x2A8AE8, 0x2A8AEC), which the trampoline sets itself
#   0x2A8C50  the update's call, and with it the copy of its answer (0x2A8C5C):
#             the trampoline answers 1 in r3, and the game's own
#             `ori r27, r3, 0` copies it
#   0x2A8CC8  the first word of the transition's frame
#
# The first two keep the caves and the six words builds 50 to 53 gave their
# trampolines, and their patched forms are second halves after the bot-crash
# table, with the third trampoline (fast loads, second halves, below). Every
# crash trampoline is where it was, byte for byte, and so is refill ammo's.
#
# 0x2A8C50 is the site builds 50 to 52 had, and branches to the same cave, so
# its word is theirs, 0x483BEFB0; the cave behind it is not, and the look tells
# the two apart by that. Build 53 had the second site at 0x2A8C5C until this
# change.

# 0x2A8AF4: bl 0x3C8D54, memcard_Save(0, the level). Patched: FadeToBlack(2, 0)
# instead (fast_loads_1_on). r5 and cr0 are dead here: the call clobbers both,
# and memcard_Save takes two arguments.
	.section .cave.fast_loads_1, "ax"
fast_loads_1:
	lis     r5, SW_FLAG_FAST_LOADS@ha
	lbz     r5, SW_FLAG_FAST_LOADS@l(r5)
	cmpwi   cr0, r5, 0
	bne     cr0, fast_loads_1_on
	bl      rac4_memcard_save
	b       fast_loads_1_ret

	.section .site.fast_loads_1, "ax"
	b       fast_loads_1
fast_loads_1_ret:

# 0x2A8C50: bcctrl 20, 30, the call through CTR to g_fnTransitionUpdate.
# Patched: play the movie the flight has queued, if there is one, and answer 1
# (fast_loads_2_on). The trampoline leaves CTR, r3 (the update's argument) and
# r2 as the game loaded them; r5 is dead, holding the mode the loop has already
# stored, and cr0 with it, which the call clobbers either way.
	.section .cave.fast_loads_2, "ax"
fast_loads_2:
	lis     r5, SW_FLAG_FAST_LOADS@ha
	lbz     r5, SW_FLAG_FAST_LOADS@l(r5)
	cmpwi   cr0, r5, 0
	bne     cr0, fast_loads_2_on
	bcctrl  20, 30
	b       fast_loads_2_ret

	.section .site.fast_loads_2, "ax"
	b       fast_loads_2
fast_loads_2_ret:

# --------------------------------------------------------------- crash patches
#
# robo's "DL Crash Patches": six dropship words, four vendor words and the bot
# crash. Each word the mod changes becomes a trampoline that runs the mod's word
# with the flag set and the game's own with it clear:
#
#     lis   rX, FLAG@ha
#     lbz   rX, FLAG@l(rX)
#     cmpwi cr0, rX, 0
#     bne   cr0, 1f
#     O                      flag 0: the game's word
#     b     site + 4
# 1:  P                      flag 1: the mod's word (nothing, for a nop)
#     b     site + 4

# The dropship. r4 is scratch at all six; cr6, live across 0x51E200, is not
# touched.

# 0x51E1E8: fsubs f6, f7, f6 -> fsubs f6, f3, f6.
	.section .cave.crash_dropship_1, "ax"
crash_dropship_1:
	lis     r4, SW_FLAG_CRASH_PATCHES@ha
	lbz     r4, SW_FLAG_CRASH_PATCHES@l(r4)
	cmpwi   cr0, r4, 0
	bne     cr0, 1f
	fsubs   f6, f7, f6
	b       crash_dropship_1_ret
1:	fsubs   f6, f3, f6
	b       crash_dropship_1_ret

	.section .site.crash_dropship_1, "ax"
	b       crash_dropship_1
crash_dropship_1_ret:

# 0x51E200: vsubfp v6, v5, v6 -> vsubfp v6, v7, v6.
	.section .cave.crash_dropship_2, "ax"
crash_dropship_2:
	lis     r4, SW_FLAG_CRASH_PATCHES@ha
	lbz     r4, SW_FLAG_CRASH_PATCHES@l(r4)
	cmpwi   cr0, r4, 0
	bne     cr0, 1f
	vsubfp  v6, v5, v6
	b       crash_dropship_2_ret
1:	vsubfp  v6, v7, v6
	b       crash_dropship_2_ret

	.section .site.crash_dropship_2, "ax"
	b       crash_dropship_2
crash_dropship_2_ret:

# 0x51E21C: fsubs f6, f6, f27 -> fsubs f6, f27, f6.
	.section .cave.crash_dropship_3, "ax"
crash_dropship_3:
	lis     r4, SW_FLAG_CRASH_PATCHES@ha
	lbz     r4, SW_FLAG_CRASH_PATCHES@l(r4)
	cmpwi   cr0, r4, 0
	bne     cr0, 1f
	fsubs   f6, f6, f27
	b       crash_dropship_3_ret
1:	fsubs   f6, f27, f6
	b       crash_dropship_3_ret

	.section .site.crash_dropship_3, "ax"
	b       crash_dropship_3
crash_dropship_3_ret:

# 0x51E228: vsubfp v6, v6, v28 -> vsubfp v6, v28, v6.
	.section .cave.crash_dropship_4, "ax"
crash_dropship_4:
	lis     r4, SW_FLAG_CRASH_PATCHES@ha
	lbz     r4, SW_FLAG_CRASH_PATCHES@l(r4)
	cmpwi   cr0, r4, 0
	bne     cr0, 1f
	vsubfp  v6, v6, v28
	b       crash_dropship_4_ret
1:	vsubfp  v6, v28, v6
	b       crash_dropship_4_ret

	.section .site.crash_dropship_4, "ax"
	b       crash_dropship_4
crash_dropship_4_ret:

# 0x51E240: fsubs f6, f5, f6 -> nop.
	.section .cave.crash_dropship_5, "ax"
crash_dropship_5:
	lis     r4, SW_FLAG_CRASH_PATCHES@ha
	lbz     r4, SW_FLAG_CRASH_PATCHES@l(r4)
	cmpwi   cr0, r4, 0
	bne     cr0, 1f
	fsubs   f6, f5, f6
1:	b       crash_dropship_5_ret

	.section .site.crash_dropship_5, "ax"
	b       crash_dropship_5
crash_dropship_5_ret:

# 0x51E24C: vsubfp v6, v4, v6 -> nop.
	.section .cave.crash_dropship_6, "ax"
crash_dropship_6:
	lis     r4, SW_FLAG_CRASH_PATCHES@ha
	lbz     r4, SW_FLAG_CRASH_PATCHES@l(r4)
	cmpwi   cr0, r4, 0
	bne     cr0, 1f
	vsubfp  v6, v4, v6
1:	b       crash_dropship_6_ret

	.section .site.crash_dropship_6, "ax"
	b       crash_dropship_6
crash_dropship_6_ret:

# The vendor. No volatile register is free at these four, so the scratch is r28,
# which the function saves in its prologue and restores on the way out and which
# holds nothing at any of them.

# 0x618008: subfc r6, r7, r11 -> subfc r6, r6, r11.
	.section .cave.crash_vendor_1, "ax"
crash_vendor_1:
	lis     r28, SW_FLAG_CRASH_PATCHES@ha
	lbz     r28, SW_FLAG_CRASH_PATCHES@l(r28)
	cmpwi   cr0, r28, 0
	bne     cr0, 1f
	subfc   r6, r7, r11
	b       crash_vendor_1_ret
1:	subfc   r6, r6, r11
	b       crash_vendor_1_ret

	.section .site.crash_vendor_1, "ax"
	b       crash_vendor_1
crash_vendor_1_ret:

# 0x618024: vcmpequw v6, v4, v5 -> vcmpequw v6, v9, v5.
	.section .cave.crash_vendor_2, "ax"
crash_vendor_2:
	lis     r28, SW_FLAG_CRASH_PATCHES@ha
	lbz     r28, SW_FLAG_CRASH_PATCHES@l(r28)
	cmpwi   cr0, r28, 0
	bne     cr0, 1f
	vcmpequw v6, v4, v5
	b       crash_vendor_2_ret
1:	vcmpequw v6, v9, v5
	b       crash_vendor_2_ret

	.section .site.crash_vendor_2, "ax"
	b       crash_vendor_2
crash_vendor_2_ret:

# 0x618034: subfc r7, r7, r11 -> subfc r7, r7, r6.
	.section .cave.crash_vendor_3, "ax"
crash_vendor_3:
	lis     r28, SW_FLAG_CRASH_PATCHES@ha
	lbz     r28, SW_FLAG_CRASH_PATCHES@l(r28)
	cmpwi   cr0, r28, 0
	bne     cr0, 1f
	subfc   r7, r7, r11
	b       crash_vendor_3_ret
1:	subfc   r7, r7, r6
	b       crash_vendor_3_ret

	.section .site.crash_vendor_3, "ax"
	b       crash_vendor_3
crash_vendor_3_ret:

# 0x61806C: and r9, r12, r9 -> andc r9, r9, r12.
	.section .cave.crash_vendor_4, "ax"
crash_vendor_4:
	lis     r28, SW_FLAG_CRASH_PATCHES@ha
	lbz     r28, SW_FLAG_CRASH_PATCHES@l(r28)
	cmpwi   cr0, r28, 0
	bne     cr0, 1f
	and     r9, r12, r9
	b       crash_vendor_4_ret
1:	andc    r9, r9, r12
	b       crash_vendor_4_ret

	.section .site.crash_vendor_4, "ax"
	b       crash_vendor_4
crash_vendor_4_ret:

# The bot crash. On a console the mod rewrites a word of data, the last word of
# the vperm control table at 0x808830 (0x0C0D0E0F -> 0x14151617). Under RPCS3
# that word is in the code segment too, which RPCS3 makes read-only once it has
# loaded the executable, so neither qwark nor the game can write it. The switch
# hooks the one instruction that reads the table instead, 0x8088EC,
# lvx v7, r4, r30, and with the flag set loads a copy of the patched table from
# the cave. r5 is dead here.
	.section .cave.crash_bot, "ax"
crash_bot:
	lis     r5, SW_FLAG_CRASH_PATCHES@ha
	lbz     r5, SW_FLAG_CRASH_PATCHES@l(r5)
	cmpwi   cr0, r5, 0
	bne     cr0, 1f
	lvx     v7, r4, r30
	b       crash_bot_ret
1:	lis     r5, crash_bot_vec@ha
	addi    r5, r5, crash_bot_vec@l
	lvx     v7, 0, r5
	b       crash_bot_ret

	.section .site.crash_bot, "ax"
	b       crash_bot
crash_bot_ret:

# The table the mod leaves at 0x808830, for lvx: its own section, so the link
# puts it on the sixteen-byte boundary sw_rac4.ld gives it.
	.section .cave.crash_bot_vec, "a"
crash_bot_vec:
	.long   0x00010203, 0x04050607, 0x10111213, 0x14151617

# ------------------------------------------------- fast loads, second halves
#
# After the bot-crash table, in what was the spare span, so that nothing before
# them moved. The first two are the patched forms the trampolines above branch
# to; the third is the frame's whole trampoline.

# FadeToBlack(2, 0) for memcard_Save(0, the level): the screen goes black at
# once, before Transition_Pre's reads, and nothing is saved.
	.section .cave.fast_loads_1_on, "ax"
fast_loads_1_on:
	li      r3, 2
	li      r4, 0
	bl      rac4_fade_to_black
	b       fast_loads_1_ret

# The movie, if the flight has one queued: 0x2AA004 plays it in a loop of its
# own frames and clears the word, or returns at once. Then 1 for the answer,
# which the game copies into r27 at 0x2A8C5C, so the loop goes round once.
	.section .cave.fast_loads_2_on, "ax"
fast_loads_2_on:
	bl      rac4_space_movie
	li      r3, 1
	b       fast_loads_2_ret

# 0x2A8CC8: li r3, 1, BeginFrame's argument and the first word of the
# transition's frame. Patched: b 0x2A8CE8, past BeginFrame, Transition_Draw and
# EndFrame to memcard_Update's call, so the one time round draws nothing and
# flips nothing. r3 is the scratch, since the game sets it here, and cr0 is
# dead: on either path the next thing the game does is a call.
	.section .cave.fast_loads_3, "ax"
fast_loads_3:
	lis     r3, SW_FLAG_FAST_LOADS@ha
	lbz     r3, SW_FLAG_FAST_LOADS@l(r3)
	cmpwi   cr0, r3, 0
	bne     cr0, 1f
	li      r3, 1
	b       fast_loads_3_ret
1:	b       fast_loads_3_ret + 0x1C    # 0x2A8CE8, past the frame

	.section .site.fast_loads_3, "ax"
	b       fast_loads_3
fast_loads_3_ret:
