/*
 * The savefile helper's console side, and the savefile library that lives beside
 * it: installing the helper, asking it for a save, and moving the bytes between
 * the buffer it parks one in and a file on the console.
 *
 * Protocol 1.9 put the aside buffer there. There is one path for all four games:
 * the SAVE_ASIDE action asks the game to copy its save into the buffer, the
 * LOAD_ASIDE action asks it to take back whatever is in there, and SAVEFILE_READ
 * and SAVEFILE_WRITE stream the buffer to and from a PC.
 *
 * Protocol 1.10 moved the library of record onto the console. A save is up to
 * 2 MB and streaming it from the PC on every load was two seconds of wire for
 * bytes the console had already seen, so the files now live at
 *
 *     /dev_hdd0/qwark/savefiles/<TITLEID>/<category>/<name>.sav
 *
 * with the CRC32 of each one beside it as <name>.sav.sum, and qwark copies
 * between the file and the aside buffer itself. The PC keeps a mirror so nothing
 * is lost if the console is wiped, and a file that exists only on the PC is
 * uploaded once, with the ordinary file ops, and then lives here. SAVEFILE_READ
 * and SAVEFILE_WRITE are still what they were, but a normal save or load no
 * longer goes near them: they are the debug and test path.
 *
 * The helper itself is PowerPC code that lives in the game's process. qwark
 * carries it as bytes in src/games/sfhelper_bins.c and writes it in on the first
 * action that needs it, so a user never loads a mod for it and never knows it is
 * there. It is code, so a platform that cannot patch code cannot have qwark put
 * it there.
 *
 * Build 47 gives RPCS3 the other road in. RPCS3 applies its own patch files to
 * the executable as it loads it, before it recompiles anything, so a patch that
 * writes the helper's caves and hook words at load is code the game really runs.
 * SAVEFILE_PATCH hands a client those words to write into RPCS3's patch file;
 * on a platform that refuses code patches qwark then looks, once a session, for
 * its own helper already in the game, and where it finds every byte of it the
 * savefile block works exactly as it does on a console after an install. Where
 * it does not, everything that needs the helper answers UNSUPPORTED as before.
 *
 * Everything that touches game memory is tick thread only, the transfer state
 * machine included. The three library ops - the categories, the listing and
 * creating or deleting a category - touch files and never the game, so they run
 * on the network thread where a directory walk or a CRC over a 2 MB file costs
 * the tick loop nothing. They are refused while a transfer is in flight, which
 * is the whole of the arbitration between the two threads.
 */
#ifndef QWARK_SAVEFILE_H
#define QWARK_SAVEFILE_H

#include "proto.h"
#include "util.h"

/* SAVEFILE_INFO's `pending` bits: a request the helper has not answered yet. */
#define SAVEFILE_PENDING_SET_ASIDE 0x01
#define SAVEFILE_PENDING_LOAD      0x02
/*
 * Revision 1.10. qwark is copying between a file and the aside buffer right now.
 * `done` and `total` say how far it has got; the bit clears when the copy is
 * over *and* the request that goes with it has settled, so a client that polls
 * this sees the same "the work is finished" guarantee the other two bits carry.
 */
#define SAVEFILE_PENDING_TRANSFER  0x04

/*
 * SAVEFILE_INFO's `error`, revision 1.10. Set when a transfer stops early, and
 * kept until the next STORE or RESTORE starts, so a client that polls after the
 * pending bit has gone still learns why nothing happened.
 *
 * Build 43 made it the reason a set-aside or a load was refused too. Build 53:
 * a set-aside or a load clears it as it starts, as a STORE or a RESTORE always
 * has, so it is the reason for the last of the four, and a request that is
 * refused for a reason with no code here, or goes out, leaves no earlier
 * refusal's reason behind it.
 */
#define SAVEFILE_ERR_NONE       0
#define SAVEFILE_ERR_MISSING    1  /* RESTORE: no such file on the console */
#define SAVEFILE_ERR_IO         2  /* a read, a write or an open failed */
#define SAVEFILE_ERR_SHORT      3  /* the file is not exactly the buffer's size */
#define SAVEFILE_ERR_NO_HELPER  4  /* the game went away underneath the copy */
#define SAVEFILE_ERR_BUSY       5  /* the aside buffer was already spoken for */
/*
 * 5 is also, since build 53, a set-aside or a load asked for while a STORE or a
 * RESTORE is running: the set-aside would write over the save being stored,
 * and the load would hand the game a buffer the RESTORE is still filling.
 */
/*
 * Build 43. The game was not somewhere a save could be taken or put back: it
 * was in a cutscene, a transition, a menu or a lobby, or a mode change was
 * queued. Deadlocked is the game this is known for; `savefile_ready` in
 * struct game_api is what each game answers with, and a game that does not
 * answer at all is treated as always ready.
 *
 * Build 53: in RaC1, RaC2 and RaC3 it is a load the game's pause cannot be
 * entered for at all - a planet load already on its way, the title screen, a
 * movie or the space transition, a mode the game's mode change will not leave -
 * or one whose pause never came within the helper's wait. Their helpers take
 * the game into the pause themselves from everything else.
 */
#define SAVEFILE_ERR_NOT_IDLE   6

/*
 * Build 53, revision 1.20. There was no save to act on. A set-aside found the
 * game holding no save yet - RaC1, RaC2 and RaC3 have none until they have
 * saved or loaded once in this process (on a new game, the first autosave) -
 * and a load found the aside buffer holding something that is not a save of
 * this game, or nothing at all and no save in the game to fill it from. Those
 * three answer it.
 *
 * All four answer it for a load of a buffer qwark filled itself, by a RESTORE
 * or SAVEFILE_WRITE, whose first word is 0: every helper reads that as an empty
 * buffer and loads the game's own save instead, so a RESTORE of such a file
 * stops with it after the copy, raising nothing (savefile.c, g_aside_ours).
 */
#define SAVEFILE_ERR_NO_SAVE    7

/*
 * What the helper leaves in a request byte when it drops a request for the same
 * reason - the belt to qwark's braces, because qwark asks on its own tick and
 * the game gets frames of its own between that and the one the helper runs on.
 * The helper never acts on a request it answers this way; qwark puts the byte
 * back to 0, drops the request and reports SAVEFILE_ERR_NOT_IDLE.
 *
 * It is a value of the request byte because the request byte is the whole
 * interface: it already carries 0 idle, 1 requested, and, in Deadlocked, 2 for
 * a load menu that is still up. SF_API_REFUSED in src/games/sfhelper/sfhelper.h
 * is the same number on the other side.
 *
 * Build 53: RaC1's, RaC2's and RaC3's helpers leave SAVEFILE_HELPER_NO_SAVE
 * when there is no save to act on, which qwark reports as SAVEFILE_ERR_NO_SAVE;
 * their loads take the game into its pause first and leave
 * SAVEFILE_HELPER_REFUSED when the game is somewhere the pause cannot be
 * entered from, or has not come to it within the helper's wait.
 */
#define SAVEFILE_HELPER_REFUSED 0xFE
#define SAVEFILE_HELPER_NO_SAVE 0xFD

/*
 * How the copy is chunked on the tick thread. A chunk is one mem_read or
 * mem_write and one file read or write, out of the shared scratch buffer in
 * util.c; the two numbers multiply out to 128 KB per tick, so a 2 MB save is
 * sixteen ticks, an eighth of a second of the 120 Hz loop. Telemetry, freezes
 * and every other request keep flowing throughout, which is the reason the copy
 * is a state machine at all rather than a loop that owns the tick thread for as
 * long as it takes.
 *
 * The chunk was 64 KB (PLAT_MEM_MAX, the largest single read) in a 64 KB buffer
 * of its own until build 35, when the buffer became the 16 KB one the mod loader
 * also uses, and 8 KB in build 37. Eight times as many calls move exactly the
 * same bytes per tick, so a transfer takes the same number of ticks it always
 * did: the two numbers still multiply out to 128 KB.
 */
#define SAVEFILE_COPY_CHUNK      QSCRATCH_BYTES
#define SAVEFILE_CHUNKS_PER_TICK 16

/* What a save is called, and what its CRC sidecar is called. */
#define SAVEFILE_EXT     ".sav"
#define SAVEFILE_SUM_EXT ".sum"

#define SAVEFILE_PATH_MAX 512

/*
 * How many consecutive ticks a request byte has to read 0 before qwark calls the
 * request answered. 30 ticks of the 120 Hz loop is a quarter of a second.
 *
 * A save is the reason this exists. The helper copies the game's save buffer
 * into the aside buffer and clears the byte afterwards, so a byte that reads 0
 * says the copy is over - but it says it across a running game, with no barrier
 * between the copy's stores and the clearing store, and one unlucky or failed
 * read would otherwise be enough to tell a client the save is ready. The window
 * costs a quarter of a second on an operation a user waits seconds for, and it
 * is the belt to the helper's braces: neither on its own can hand out a
 * half-copied buffer.
 */
#define SAVEFILE_SETTLE_TICKS 30

/*
 * Forget that the helper was installed, and, build 47, whether it was found
 * already in the game. The session calls this on the way into INGAME and on the
 * way out of it: a new process has none of qwark's install in it, so the next
 * request writes it again, and it may or may not have been loaded with an RPCS3
 * patch, so the next tick looks again.
 */
void savefile_forget(void);

/* config.txt `savefile_helper`: 0 never writes the helper into a game. */
void savefile_set_enabled(int on);

/*
 * Build 53: the setting above, for a feature that needs the helper and would
 * rather refuse at once than find out at the install (UYA's trophy reset).
 */
int savefile_enabled(void);

/*
 * Writes the caves and the hook words unless this process already has them.
 * ST_OK when qwark has installed it. Called by set-aside/load actions, valid
 * restores and RaC1's Force autosave. Status, metadata and raw buffer I/O never
 * install code.
 *
 * Build 47: where code cannot be patched, ST_OK when an RPCS3 patch put the
 * helper in at load and UNSUPPORTED when nothing did. It writes nothing either
 * way, not even the request bytes the console install zeroes first: the patch
 * zeroed them at load, the hook has been live since the game booted, and those
 * bytes are the helper's own.
 */
int savefile_install(void);

/*
 * SAVEFILE_INFO. `supported` is 0 for a game qwark has no helper for, and that
 * is an ST_OK answer, not an error: it is how a client knows to hide the panel.
 * `running` is the helper's own byte, which it writes on every call, so it says
 * the code is installed *and* the hook is being reached. `pending` stays set
 * from the moment a request goes out until its byte has read 0 for a whole
 * settle window, so a client that polls it can treat a clear bit as the work
 * being over rather than as the request not having landed yet.
 *
 * Build 47: it answers on a platform that refuses code patches too, where
 * `installed` says whether an RPCS3 patch put the helper in this process. It is
 * BUSY there for as long as qwark cannot read the game to find out.
 */
int savefile_info(u8 *supported, u8 *installed, u8 *running, u8 *pending, u32 *size);

/*
 * SAVEFILE_PATCH, revision 1.15, network thread under the core lock. The running
 * game's helper as what a patch writes to put it in: the (address, value) words
 * - every cave as consecutive words in address order, caves in the table's
 * order, then the hook words - and after them the three request bytes a console
 * install zeroes, as single bytes at 0, with the CRC of everything after the
 * stamp in front. It reads the generated table and nothing else, so it answers
 * whether or not the platform patches code. NOT_INGAME outside a game,
 * UNSUPPORTED for a game with no helper or with the helper switched off, FULL
 * for a reply that would not fit `cap` (never, for the four games today:
 * RaC3's is the largest since build 53, at 5348 bytes with its trophy reset and its thread).
 *
 * Build 51: only in the emulator builds (QWARK_EMULATOR, plat.h), as is the
 * look for a patched-in helper. The PS3 module answers SAVEFILE_PATCH
 * UNSUPPORTED (net.c) and only ever has the helper it installs itself.
 */
#ifdef QWARK_EMULATOR
int savefile_patch(u8 *out, u32 cap, u32 *len);
#endif

/*
 * The transfer half of SAVEFILE_INFO, revision 1.10. `done` and `total` are the
 * bytes of the copy that is running, both 0 when none is; `error` is why the
 * last one stopped early and survives until the next one starts - or, build 53,
 * until the next set-aside or load is asked for.
 */
void savefile_transfer(u32 *done, u32 *total, u8 *error);

/* SAVEFILE_READ and SAVEFILE_WRITE, bounded to the aside buffer. */
int savefile_read(u32 offset, u32 len, u8 *out, u32 *outlen);
int savefile_write(u32 offset, const u8 *data, u32 len);

/*
 * The two requests the flagged ACTIONs make. Build 43: either answers ST_BUSY
 * with SAVEFILE_ERR_NOT_IDLE, and raises nothing at the game, when the running
 * game says it is not somewhere a save may be taken or put back. Build 53: or
 * with SAVEFILE_ERR_NO_SAVE, when it has no save to take or the aside buffer
 * none to give back; or with SAVEFILE_ERR_BUSY while a STORE or a RESTORE is
 * running. Either clears `error` as it starts.
 */
int savefile_set_aside(void);
int savefile_load_aside(void);

/*
 * Build 53. What savefile_load_aside would answer, without raising anything:
 * ST_OK when a load request would go out now, otherwise its refusal, with the
 * transfer's `error` set as the request would set it. RaC1, RaC2 and RaC3 ask
 * first because their load actions force fast loads on before the request, and
 * a load that is refused must not leave them forced.
 */
int savefile_load_ready(void);

/*
 * Build 53. How many load requests the helper has dropped in this process: a
 * count that only goes up, which a game compares with the last value it saw to
 * undo what it did for a load that never came (RaC1, RaC2 and RaC3 put back the
 * fast loads their load actions forced). Only savefile_load_aside's loads are
 * counted: a RESTORE's does nothing a game would undo, and one dropped while a
 * planet load runs must not put back the fast loads that load forced. Tick
 * thread, like the request watching that counts them.
 */
u32 savefile_load_refusals(void);

/*
 * The settle window and the transfer, one tick's worth. The session calls it
 * every INGAME tick; it reads nothing at all while nothing is outstanding.
 */
void savefile_tick(void);

/* ------------------------------------------------------ the console library */

/*
 * SAVEFILE_STORE and SAVEFILE_RESTORE, both tick thread.
 *
 * STORE raises the set-aside request, waits out the settle window the helper's
 * answer needs, and then copies the aside buffer into
 * <QWARK_SAVEDIR>/<TITLEID>/<category>/<name>, summing it as it goes and
 * writing the sum beside it. RESTORE is the reverse and raises the load request
 * only once the whole file is in the buffer.
 *
 * Both answer BUSY when a transfer is already in flight or the game has a
 * request of its own outstanding, NOT_FOUND for a file that is not there,
 * BAD_ARG for a name qwark will not have or a file that is not exactly the size
 * of the buffer, and neither raises anything at the game when it refuses.
 *
 * Build 43: BUSY with SAVEFILE_ERR_NOT_IDLE is the other refusal, for a game
 * that is not sitting still. It is checked before either op touches a file.
 * Build 53: a STORE is BUSY with SAVEFILE_ERR_NO_SAVE when the game has no save
 * to set aside, and a RESTORE stops with it after the copy when the file is not
 * a save of the running game.
 */
int savefile_store(const char *category, const char *name);
int savefile_restore(const char *category, const char *name);

/*
 * The gate the three library ops share, checked on the network thread under the
 * core lock: UNSUPPORTED where the game has no helper, or, where code cannot be
 * patched, no RPCS3 patch put one in; NOT_INGAME outside INGAME; BUSY while a
 * transfer is running, and, build 47, in the moment before the tick thread has
 * looked for a patched-in helper, since this thread never reads the game.
 */
int savefile_library_gate(void);

/*
 * SAVEFILE_CATEGORIES and SAVEFILE_LIST, network thread. The listing reports
 * every <name>.sav in the category with its size and its CRC32; the CRC comes
 * from the .sum sidecar qwark wrote when it stored the file, and is computed
 * once and written out for a file the client uploaded itself.
 */
int savefile_categories(u8 *out, u32 cap, u32 *len);
int savefile_list(const char *category, u8 *out, u32 cap, u32 *len);

/*
 * SAVEFILE_CATEGORY, network thread. op 0 creates the folder, op 1 removes it.
 * A delete sweeps up any orphaned .sum sidecars first, so a client that deleted
 * the saves and forgot their sums does not end up with a folder it cannot
 * remove; a category that still holds a save is refused.
 */
int savefile_category(u8 op, const char *name);

/* Whether a name off the wire is one qwark will put in a path. */
int savefile_name_ok(const char *name);

#endif /* QWARK_SAVEFILE_H */
