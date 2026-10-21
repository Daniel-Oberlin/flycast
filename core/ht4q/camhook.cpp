/*
	HydroThunder4Quest: head-pose camera hook for Hydro Thunder (USA) (Rev 1), game ID T-9702N.

	The gamecam setup at 0x8c045850 calls BuildCam(cam R4, heading R5, pitch R6, roll R7,
	x FR4, y FR5, z FR6) through the literal at 0x8c0458b4. We point that literal at a stub in
	the IP.BIN area (zero during gameplay, never write-protected by the dynarec) that adds a pose
	block to the arguments and jumps on to BuildCam. Each maple poll we refresh the pose block
	from the current head pose.

	Head pose sources, highest priority first:
	  - memserver 'H' override (scripted tests; later the headset)
	  - mouse position over the game window (HT4Q_MOUSEHEAD = both | look | lean | off, default both)

	Environment:
	  HT4Q_CAMHOOK=0          don't install the hook
	  HT4Q_LEVEL=roll | all   cancel the game camera's roll (and pitch) so the horizon stays level
*/
#include "types.h"
#include "hw/sh4/sh4_mem.h"
#include "input/mouse.h"
#include "ht4q.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace ht4q
{

static constexpr u32 CALL_LITERAL = 0x8c0458b4;	// literal holding BuildCam's address
static constexpr u32 BUILDCAM = 0x8c093d44;
static constexpr u32 STUB = 0x8c00b900;
static constexpr u32 POSE = STUB + 0x40;
static constexpr u32 STUB_AREA = 0x60;			// bytes that must be free before installing
static constexpr u32 DRAW_HEADING = 0x8c1fca74;	// _nDrawHeading, bradians

static const u16 stubCode[] = {
	0xD009,		// 00 mov.l @(L_POSE,PC),R0
	0xF009,		// 02 fmov.s @R0+,FR0
	0xF400,		// 04 fadd FR0,FR4      x
	0xF009,		// 06 fmov.s @R0+,FR0
	0xF500,		// 08 fadd FR0,FR5      y
	0xF009,		// 0a fmov.s @R0+,FR0
	0xF600,		// 0c fadd FR0,FR6      z
	0x6106,		// 0e mov.l @R0+,R1
	0x351C,		// 10 add R1,R5         heading += dh
	0x6106,		// 12 mov.l @R0+,R1
	0x6206,		// 14 mov.l @R0+,R2
	0x2629,		// 16 and R2,R6         pitch &= pitchMask
	0x361C,		// 18 add R1,R6         pitch += dp
	0x6106,		// 1a mov.l @R0+,R1
	0x6206,		// 1c mov.l @R0+,R2
	0x2729,		// 1e and R2,R7         roll &= rollMask
	0x371C,		// 20 add R1,R7         roll += dr
	0xD002,		// 22 mov.l @(L_ORIG,PC),R0
	0x402B,		// 24 jmp @R0
	0x0009,		// 26 nop
	(u16)POSE, (u16)(POSE >> 16),			// 28 L_POSE
	(u16)BUILDCAM, (u16)(BUILDCAM >> 16),	// 2c L_ORIG
};

struct PoseBlock {
	float dx, dy, dz;	// world space
	s32 dh;
	s32 dp;
	u32 pitchMask;
	s32 dr;
	u32 rollMask;
};
static_assert(sizeof(stubCode) <= POSE - STUB && POSE - STUB + sizeof(PoseBlock) <= STUB_AREA, "stub layout");

static std::mutex headMutex;
static bool headOverride;
static HeadPose overridePose;

void setHeadPoseOverride(bool active, const HeadPose& pose)
{
	std::lock_guard<std::mutex> lock(headMutex);
	headOverride = active;
	overridePose = pose;
}

static u8 *ram(u32 addr) { return &mem_b[addr & RAM_MASK]; }
static u32 readU32(u32 addr) { u32 v; memcpy(&v, ram(addr), 4); return v; }
static void writeU32(u32 addr, u32 v) { memcpy(ram(addr), &v, 4); }

static const char *env(const char *name, const char *def)
{
	const char *v = getenv(name);
	return v != nullptr && v[0] != '\0' ? v : def;
}

static HeadPose mousePose()
{
#ifdef HT4Q_XR
	static const std::string mode = env("HT4Q_MOUSEHEAD", "off");	// Quest: the headset drives it
#else
	static const std::string mode = env("HT4Q_MOUSEHEAD", "both");
#endif
	HeadPose p{};
	if (mode == "off" || mo_x_prev[0] == -1)	// no mouse motion seen yet
		return p;
	float nx = std::clamp((mo_x_abs[0] - 320) / 320.f, -1.f, 1.f);
	float ny = std::clamp((mo_y_abs[0] - 240) / 240.f, -1.f, 1.f);
	if (mode == "both" || mode == "lean")
	{
		p.x = 2.f * nx;
		p.y = -1.f * ny;
	}
	if (mode == "both" || mode == "look")
	{
		p.yaw = -40.f * nx;
		p.pitch = -25.f * ny;
	}
	return p;
}

static s32 degToBrad(float deg) { return (s32)std::lround(deg * 65536.f / 360.f); }

// Returns false when the game, RAM layout or stub area isn't what we expect.
static bool ensureInstalled()
{
	u32 lit = readU32(CALL_LITERAL);
	if (lit == STUB)
	{
		if (memcmp(ram(STUB), stubCode, sizeof(stubCode)) == 0)
			return true;
		writeU32(CALL_LITERAL, BUILDCAM);	// something overwrote the stub: back out
		WARN_LOG(COMMON, "ht4q camhook: stub overwritten, hook removed");
		return false;
	}
	if (lit != BUILDCAM)
		return false;	// gamecam module not loaded (yet)
	for (u32 i = 0; i < STUB_AREA; i++)
		if (*ram(STUB + i) != 0)
			return false;
	memcpy(ram(STUB), stubCode, sizeof(stubCode));
	PoseBlock zero{ 0, 0, 0, 0, 0, ~0u, 0, ~0u };
	memcpy(ram(POSE), &zero, sizeof(zero));
	writeU32(CALL_LITERAL, STUB);			// emu thread: dynarec invalidates the caller's block
	NOTICE_LOG(COMMON, "ht4q camhook installed");
	return true;
}

static void camhookPoll()
{
	static const bool enabled = std::string(env("HT4Q_CAMHOOK", "1")) != "0";
	static const std::string level = env("HT4Q_LEVEL", "off");
	if (!enabled || settings.content.gameId != "T-9702N" || !ensureInstalled())
		return;

	HeadPose target;
	{
		std::lock_guard<std::mutex> lock(headMutex);
		target = headOverride ? overridePose : mousePose();
	}
	// light smoothing for the mouse; scripted poses apply immediately
	static HeadPose cur{};
	float a = headOverride ? 1.f : 0.35f;
	for (int i = 0; i < 6; i++)
		(&cur.x)[i] += a * ((&target.x)[i] - (&cur.x)[i]);

	// head-local (x right, y up, z back) -> world, using the game's draw heading.
	// right = (cos h, 0, -sin h), back = (sin h, 0, cos h)
	float h = (s32)readU32(DRAW_HEADING) * (float)(2 * M_PI / 65536);
	PoseBlock pb;
	pb.dx = cur.x * std::cos(h) + cur.z * std::sin(h);
	pb.dy = cur.y;
	pb.dz = -cur.x * std::sin(h) + cur.z * std::cos(h);
	// game: +heading turns right, +pitch looks down; HeadPose is right-handed like OpenXR
	pb.dh = degToBrad(-cur.yaw);
	pb.dp = degToBrad(-cur.pitch);
	pb.dr = degToBrad(cur.roll);
	pb.pitchMask = level == "all" ? 0 : ~0u;
	pb.rollMask = level == "all" || level == "roll" ? 0 : ~0u;
	memcpy(ram(POSE), &pb, sizeof(pb));
}

void onMaplePoll(MapleInputState inputState[4])
{
	memserverPoll(inputState);
	camhookPoll();
}

}	// namespace ht4q
