/*
 * The savefile helper's console side: installing it, asking it for a save, and
 * moving the bytes of the buffer it parks one in.
 *
 * Protocol 1.9. There is one path for all four games. The client triggers the
 * SAVE_ASIDE action, watches SAVEFILE_INFO until the request bit clears, and
 * reads the aside buffer out with SAVEFILE_READ; loading is the same in reverse.
 * No file is written on the console at any point, and the old tempsave under
 * /dev_hdd0/game/<TITLEID>/USRDIR is gone.
 *
 * The helper itself is PowerPC code that lives in the game's process. qwark
 * carries it as bytes in src/games/sfhelper_bins.c and writes it in on the first
 * request of a session, so a user never loads a mod for it and never knows it is
 * there. It is code, so a platform that cannot patch code cannot have it, and
 * everything here answers UNSUPPORTED under RPCS3.
 *
 * Tick thread only, like everything else that touches game memory.
 */
#ifndef QWARK_SAVEFILE_H
#define QWARK_SAVEFILE_H

#include "proto.h"

/* SAVEFILE_INFO's `pending` bits: a request the helper has not answered yet. */
#define SAVEFILE_PENDING_SET_ASIDE 0x01
#define SAVEFILE_PENDING_LOAD      0x02

/*
 * Forget that the helper was installed. The session calls this on the way into
 * INGAME: a new process has none of it, so the next request writes it again.
 */
void savefile_forget(void);

/*
 * Writes the caves and the hook words unless this process already has them.
 * ST_OK when the helper is in, whoever put it there. Every other entry point
 * here calls it, and so does RaC1's Force autosave, which drives a fourth
 * request byte of its own.
 */
int savefile_install(void);

/*
 * SAVEFILE_INFO. `supported` is 0 for a game qwark has no helper for, and that
 * is an ST_OK answer, not an error: it is how a client knows to hide the panel.
 * `running` is the helper's own byte, which it writes on every call, so it says
 * the code is installed *and* the hook is being reached.
 */
int savefile_info(u8 *supported, u8 *installed, u8 *running, u8 *pending, u32 *size);

/* SAVEFILE_READ and SAVEFILE_WRITE, bounded to the aside buffer. */
int savefile_read(u32 offset, u32 len, u8 *out, u32 *outlen);
int savefile_write(u32 offset, const u8 *data, u32 len);

/* The two requests the flagged ACTIONs make. */
int savefile_set_aside(void);
int savefile_load_aside(void);

#endif /* QWARK_SAVEFILE_H */
