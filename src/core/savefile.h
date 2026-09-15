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
 * there. It is code, so a platform that cannot patch code cannot have it, and
 * everything here answers UNSUPPORTED under RPCS3.
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
 */
#define SAVEFILE_ERR_NONE       0
#define SAVEFILE_ERR_MISSING    1  /* RESTORE: no such file on the console */
#define SAVEFILE_ERR_IO         2  /* a read, a write or an open failed */
#define SAVEFILE_ERR_SHORT      3  /* the file is not exactly the buffer's size */
#define SAVEFILE_ERR_NO_HELPER  4  /* the game went away underneath the copy */
#define SAVEFILE_ERR_BUSY       5  /* the aside buffer was already spoken for */

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
 * also uses. Four times as many calls move exactly the same bytes per tick, so a
 * transfer takes the same number of ticks it always did.
 */
#define SAVEFILE_COPY_CHUNK      QSCRATCH_BYTES
#define SAVEFILE_CHUNKS_PER_TICK 8

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
 * Forget that the helper was installed. The session calls this on the way into
 * INGAME: a new process has none of it, so the next request writes it again.
 */
void savefile_forget(void);

/* config.txt `savefile_helper`: 0 never writes the helper into a game. */
void savefile_set_enabled(int on);

/*
 * Writes the caves and the hook words unless this process already has them.
 * ST_OK when qwark has installed it. Called by set-aside/load actions, valid
 * restores and RaC1's Force autosave. Status, metadata and raw buffer I/O never
 * install code.
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
 */
int savefile_info(u8 *supported, u8 *installed, u8 *running, u8 *pending, u32 *size);

/*
 * The transfer half of SAVEFILE_INFO, revision 1.10. `done` and `total` are the
 * bytes of the copy that is running, both 0 when none is; `error` is why the
 * last one stopped early and survives until the next one starts.
 */
void savefile_transfer(u32 *done, u32 *total, u8 *error);

/* SAVEFILE_READ and SAVEFILE_WRITE, bounded to the aside buffer. */
int savefile_read(u32 offset, u32 len, u8 *out, u32 *outlen);
int savefile_write(u32 offset, const u8 *data, u32 len);

/* The two requests the flagged ACTIONs make. */
int savefile_set_aside(void);
int savefile_load_aside(void);

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
 */
int savefile_store(const char *category, const char *name);
int savefile_restore(const char *category, const char *name);

/*
 * The gate the three library ops share, checked on the network thread under the
 * core lock: UNSUPPORTED where code cannot be patched or the game has no helper,
 * NOT_INGAME outside INGAME, BUSY while a transfer is running.
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
