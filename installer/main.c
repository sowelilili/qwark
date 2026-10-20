/*
 * qwark's installer: the EBOOT.BIN of qwark-installer.pkg, for a console with
 * no PC. Package Manager installs the package to /dev_hdd0/game/QWRK00001; run
 * from the XMB, it asks once, installs the qwark.sprx and ui.pak that came with
 * it (install.c), says what it did, and offers to restart the console, which
 * loads a boot plugin only as it starts.
 *
 * Everything shown is a system message dialog. A dialog is drawn into the
 * application's own frames, so the app keeps a plain screen of its own and
 * flips it for as long as one is up: two colour buffers, filled once, and
 * nothing drawn into them after.
 *
 * Nothing here is borrowed: the restart is the lv2 power call (379) that
 * webMAN MOD's own installer makes, with its quick-restart argument, and PS3HEN
 * answers 0x1337 to syscall 8 with 0x1337, which is how webMAN tells it apart.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <sys/syscall.h>
#include <sys/timer.h>
#include <cell/cell_fs.h>
#include <cell/gcm.h>
#include <cell/sysmodule.h>
#include <sysutil_common.h>
#include <sysutil_msgdialog.h>
#include <sysutil_sysparam.h>

#include "install.h"

SYS_PROCESS_PARAM(1001, 0x10000)

/* ------------------------------------------------------------------ screen */

#define COLOR_BUFFERS   2
#define COMMAND_SIZE    0x10000u
#define IO_SIZE         0x100000u
/* What shows round the dialogs: a dark blue, X8R8G8B8. */
#define BACKGROUND      0x00141B2Eu

static int g_buffer;
static int g_flipped;

static int screen_init(void)
{
	CellVideoOutState state;
	CellVideoOutResolution res;
	CellVideoOutConfiguration video;
	CellGcmConfig gcm;
	uint32_t pitch, words, offset, i, k;
	void *io;

	io = memalign(IO_SIZE, IO_SIZE);
	if (io == NULL || cellGcmInit(COMMAND_SIZE, IO_SIZE, io) != CELL_OK) return -1;

	if (cellVideoOutGetState(CELL_VIDEO_OUT_PRIMARY, 0, &state) != CELL_OK) return -1;
	if (cellVideoOutGetResolution(state.displayMode.resolutionId, &res) != CELL_OK) return -1;
	pitch = (uint32_t)res.width * 4u;
	words = pitch / 4u * (uint32_t)res.height;

	memset(&video, 0, sizeof(video));
	video.resolutionId = state.displayMode.resolutionId;
	video.format = CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8R8G8B8;
	video.pitch = pitch;
	if (cellVideoOutConfigure(CELL_VIDEO_OUT_PRIMARY, &video, NULL, 0) != CELL_OK) return -1;

	cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
	cellGcmGetConfiguration(&gcm);

	/* The buffers sit at the start of the RSX's local memory, filled once from here. */
	for (i = 0; i < COLOR_BUFFERS; i++) {
		uint32_t *pixels = (uint32_t *)gcm.localAddress + i * words;

		for (k = 0; k < words; k++) pixels[k] = BACKGROUND;
		if (cellGcmAddressToOffset(pixels, &offset) != CELL_OK) return -1;
		if (cellGcmSetDisplayBuffer((uint8_t)i, offset, pitch, (uint32_t)res.width, (uint32_t)res.height) != CELL_OK)
			return -1;
	}
	return 0;
}

static void screen_flip(void)
{
	if (g_flipped) {
		while (cellGcmGetFlipStatus() != 0) sys_timer_usleep(300);
	}
	cellGcmResetFlipStatus();
	if (cellGcmSetFlip(gCellGcmCurrentContext, (uint8_t)g_buffer) == CELL_OK) {
		cellGcmFlush(gCellGcmCurrentContext);
		cellGcmSetWaitFlip(gCellGcmCurrentContext);
	}
	g_buffer ^= 1;
	g_flipped = 1;
}

/* ------------------------------------------------------------------ dialogs */

static volatile int g_quit;
static volatile int g_button;

static void on_system(uint64_t status, uint64_t param, void *user)
{
	(void)param;
	(void)user;
	if (status == CELL_SYSUTIL_REQUEST_EXITGAME) g_quit = 1;
}

static void on_button(int button, void *user)
{
	(void)user;
	g_button = button;
}

static void frame(void)
{
	cellSysutilCheckCallback();
	screen_flip();
}

#define ASK_YESNO   (CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL | CELL_MSGDIALOG_TYPE_BG_VISIBLE | \
                     CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO | CELL_MSGDIALOG_TYPE_DISABLE_CANCEL_OFF | \
                     CELL_MSGDIALOG_TYPE_DEFAULT_CURSOR_YES)
#define TELL_ERROR  (CELL_MSGDIALOG_TYPE_SE_TYPE_ERROR | CELL_MSGDIALOG_TYPE_BG_VISIBLE | \
                     CELL_MSGDIALOG_TYPE_BUTTON_TYPE_OK | CELL_MSGDIALOG_TYPE_DISABLE_CANCEL_OFF | \
                     CELL_MSGDIALOG_TYPE_DEFAULT_CURSOR_OK)

/*
 * Shows text and waits for a button: CELL_MSGDIALOG_BUTTON_*, or -1 when the
 * dialog would not open or the system asked the app to quit (the PS button's
 * Quit Game), which closes it.
 */
static int ask(unsigned int type, const char *text)
{
	int tries = 0, rc, wait;

	g_button = -1;
	while ((rc = cellMsgDialogOpen2(type, text, on_button, NULL, NULL)) != CELL_OK) {
		if ((unsigned int)rc != CELL_SYSUTIL_ERROR_BUSY || ++tries > 300 || g_quit) return -1;
		frame();
	}
	while (g_button < 0) {
		if (g_quit) {
			cellMsgDialogAbort();
			for (wait = 0; wait < 30 && g_button < 0; wait++) frame();
			return -1;
		}
		frame();
	}
	return g_button;
}

/* ------------------------------------------------------------------ the console */

/* PS3HEN answers syscall 8 with 0x1337 when asked 0x1337; Cobra and plain firmware answer an error. */
static int on_hen(void)
{
	system_call_1(8, 0x1337);
	return (int)p1 == 0x1337;
}

/* lv2's power call, sys_sm_shutdown, asked for a quick restart. 0: the console is on its way down. */
static unsigned int restart_call(void)
{
	system_call_3(379, 0x8201, 0, 0);
	return (unsigned int)p1;
}

static unsigned int restart(void)
{
	/*
	 * The XMB leaves this file while it runs and deletes it at a clean power-off,
	 * and a start that finds it says the console was not turned off properly. A
	 * restart through the power call is clean, so it goes first, as webMAN does.
	 */
	cellFsUnlink("/dev_hdd0/tmp/turnoff");
	return restart_call();
}

/* ------------------------------------------------------------------ main */

static char g_text[INST_MESSAGE_MAX];

static void run(const char *usrdir)
{
	inst_result r;
	unsigned int error;
	int wait;

	inst_confirm_message(g_text, sizeof(g_text));
	if (ask(ASK_YESNO, g_text) != CELL_MSGDIALOG_BUTTON_YES) return;

	inst_run(usrdir, &r);
	inst_result_message(&r, on_hen(), g_text, sizeof(g_text));
	if (!r.ok) {
		ask(TELL_ERROR, g_text);
		return;
	}
	if (ask(ASK_YESNO, g_text) != CELL_MSGDIALOG_BUTTON_YES) return;

	error = restart();
	if (error == 0) {
		/* The system asks the app to quit on the way down; ten seconds of nothing means it did not go. */
		for (wait = 0; wait < 600 && !g_quit; wait++) frame();
		if (g_quit) return;
		error = 0xFFFFFFFFu;
	}
	inst_restart_failed_message(error, g_text, sizeof(g_text));
	ask(TELL_ERROR, g_text);
}

int main(int argc, char **argv)
{
	char usrdir[256];

	inst_usrdir(argc > 0 ? argv[0] : NULL, usrdir, sizeof(usrdir));

	cellSysmoduleLoadModule(CELL_SYSMODULE_FS);
	cellSysutilRegisterCallback(0, on_system, NULL);

	/* Without a screen no dialog shows, so nothing is done at all. */
	if (screen_init() == 0) run(usrdir);

	cellSysutilUnregisterCallback(0);
	cellSysmoduleUnloadModule(CELL_SYSMODULE_FS);
	return 0;
}
