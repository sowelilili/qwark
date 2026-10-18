/*
 * How a menu renderer reaches game memory and game functions, on the console
 * and in the host tests.
 *
 * On the console a game address is the pointer itself and a game function is
 * called through its own descriptor. Under MH_HOST (test/test_menu.c) the same
 * C runs on a PC against qwark's fake console: every access goes through
 * mh_host_ptr(), which the test supplies, and is big-endian byte by byte, so
 * the bytes qwark writes and the bytes the renderer writes are the bytes a
 * console would hold. Game functions are the test's mocks there.
 *
 * Nothing here is game-specific; mh_<game>.c includes it.
 */
#ifndef QWARK_MH_COMMON_H
#define QWARK_MH_COMMON_H

/* The host tests include qwark's plat.h first, which has the same names. */
#ifndef QWARK_PLAT_H
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef signed int         s32;
#endif

#ifdef MH_HOST

u8 *mh_host_ptr(u32 addr);

static u32 r8(u32 a)  { return *mh_host_ptr(a); }
static u32 r16(u32 a) { return ((u32)*mh_host_ptr(a) << 8) | *mh_host_ptr(a + 1); }
static u32 r32(u32 a)
{
	return ((u32)*mh_host_ptr(a) << 24) | ((u32)*mh_host_ptr(a + 1) << 16) |
	       ((u32)*mh_host_ptr(a + 2) << 8) | *mh_host_ptr(a + 3);
}
static void w8(u32 a, u32 v)  { *mh_host_ptr(a) = (u8)v; }
static void w16(u32 a, u32 v) { w8(a, v >> 8); w8(a + 1, v); }
static void w32(u32 a, u32 v) { w8(a, v >> 24); w8(a + 1, v >> 16); w8(a + 2, v >> 8); w8(a + 3, v); }
static float rf32(u32 a)
{
	union { u32 u; float f; } x;
	x.u = r32(a);
	return x.f;
}

#define mh_sync() __sync_synchronize()
#define GAME_FN(type, opd, mock) (mock)

#else

#define r8(a)     ((u32)*(volatile u8 *)(u32)(a))
#define r16(a)    ((u32)*(volatile u16 *)(u32)(a))
#define r32(a)    (*(volatile u32 *)(u32)(a))
#define rf32(a)   (*(volatile float *)(u32)(a))
#define w8(a, v)  (*(volatile u8 *)(u32)(a) = (u8)(v))
#define w16(a, v) (*(volatile u16 *)(u32)(a) = (u16)(v))
#define w32(a, v) (*(volatile u32 *)(u32)(a) = (u32)(v))

/*
 * lwsync: the order the renderer reads the page in, and the order it writes an
 * event in, is the order another thread sees.
 */
#define mh_sync() __asm__ __volatile__("lwsync" : : : "memory")
#define GAME_FN(type, opd, mock) ((type)(opd))

#endif

#endif /* QWARK_MH_COMMON_H */
