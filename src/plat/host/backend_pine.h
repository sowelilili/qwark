/*
 * The PINE backend's own seam, so the unit tests can drive it against a fake
 * PINE server without the plat_* entry points (backend_pine.c compiled with
 * -DQWARK_PINE_NO_PLAT leaves those out, and backend_fake.c supplies them).
 *
 * Nothing under src/core or src/games sees this file.
 */
#ifndef QWARK_BACKEND_PINE_H
#define QWARK_BACKEND_PINE_H

#include "../plat.h"

/* PINE, from RPCS3's 3rdparty/pine/pine_server.h. */
#define PINE_DEFAULT_PORT 28012

#define PINE_MSG_READ8       0x00
#define PINE_MSG_READ16      0x01
#define PINE_MSG_READ32      0x02
#define PINE_MSG_READ64      0x03
#define PINE_MSG_WRITE8      0x04
#define PINE_MSG_WRITE16     0x05
#define PINE_MSG_WRITE32     0x06
#define PINE_MSG_WRITE64     0x07
#define PINE_MSG_VERSION     0x08
#define PINE_MSG_TITLE       0x0B
#define PINE_MSG_ID          0x0C
#define PINE_MSG_UUID        0x0D
#define PINE_MSG_GAMEVERSION 0x0E
#define PINE_MSG_STATUS      0x0F

#define PINE_OK   0x00
#define PINE_FAIL 0xFF

/* MsgStatus */
#define PINE_STATUS_RUNNING  0u
#define PINE_STATUS_PAUSED   1u
#define PINE_STATUS_SHUTDOWN 2u

/* Which port to talk to. Call before pine_startup(). */
void pine_set_port(int port);
int  pine_port(void);

/*
 * Opens the socket if it is not already open, at most once a second. Returns 1
 * when there is a live connection. Everything below calls it first, so nothing
 * outside this file has to.
 */
int  pine_ensure(void);

/* Drops the connection; the next pine_ensure() opens a new one. */
void pine_close(void);
int  pine_connected(void);

/* First connect attempt plus the log line. Safe to call more than once. */
void pine_startup(void);

/*
 * One PINE transaction each. All return 0 on success and negative on failure,
 * whether the failure was the socket or a FAIL reply from RPCS3.
 *
 * pine_read and pine_write speak PS3 byte order in `buf`, exactly like the PS3
 * platform: PINE carries the logical value of a word, little-endian, so these
 * two put the big-endian bytes back together on the way through.
 */
int  pine_read(u32 addr, void *buf, u32 len);
int  pine_write(u32 addr, const void *buf, u32 len);

/* MsgStatus and MsgID in one packet. Either pointer may be NULL. */
int  pine_status_and_id(u32 *status, char id[16]);

int  pine_status(u32 *status);
int  pine_title_id(char id[16]);
int  pine_title(char *out, u32 cap);
int  pine_version(char *out, u32 cap);

/*
 * The cached console state the 120 Hz tick asks for. Refreshed by a single
 * status+id packet at most every PINE_POLL_US, so the tick does not spend the
 * socket on a question whose answer changes twice a minute.
 */
#define PINE_POLL_US 50000u

int  pine_game_running(void);
u32  pine_game_pid(void);
int  pine_game_title(char out[16]);

/* Forgets the cache, so the next query goes back to RPCS3. For the tests. */
void pine_forget_cache(void);

/*
 * Where /dev_hdd0 is mapped, from --root. Call before plat_init. Only exists in
 * a build that carries the plat_* entry points, so qwark-rpcs3.exe, not the
 * unit tests.
 */
void pine_set_root(const char *dir);

#endif /* QWARK_BACKEND_PINE_H */
