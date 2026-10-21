/*
	HydroThunder4Quest: head-pose camera hook for Hydro Thunder (USA) (Rev 1), game ID T-9702N.

	The gamecam setup at 0x8c045850 calls BuildCam(cam R4, heading R5, pitch R6, roll R7,
	x FR4, y FR5, z FR6) through the literal at 0x8c0458b4. We point that literal at a stub in
	the IP.BIN area (zero during gameplay, never write-protected by the dynarec) that adds a pose
	block to the arguments and jumps on to BuildCam. Each maple poll we refresh the pose block
	from the current head pose.

	Head pose sources, highest priority first:
	  - memserver 'H' override (scripted tests)
	  - the headset while racing on Quest (setXrHead) - "immersive"
	  - mouse position over the game window (HT4Q_MOUSEHEAD = both | look | lean | off, default both)

	Every pose gets an id; the stub copies the id it consumed to usedId, and onStartRender()
	reads it when the game starts rendering, so each rendered frame knows its head pose.

	Environment:
	  HT4Q_CAMHOOK=0          don't install the hook
	  HT4Q_LEVEL=roll | all   cancel the game camera's roll (and pitch) so the horizon stays level
	                          (default off; all in Quest builds)
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
#include <cstddef>

namespace ht4q
{

static constexpr u32 CALL_LITERAL = 0x8c0458b4;	// literal holding BuildCam's address
static constexpr u32 BUILDCAM = 0x8c093d44;
static constexpr u32 STUB = 0x8c00b900;
static constexpr u32 POSE = STUB + 0x40;
static constexpr u32 STUB_AREA = 0x80;			// bytes that must be free before installing
static constexpr u32 DRAW_HEADING = 0x8c1fca74;	// _nDrawHeading, bradians
static constexpr u32 ATTACHED_PHYS = 0x8c1fc988;	// _pAttachedPhys: non-null while racing
static constexpr u32 VIEWPORT_ZSCREEN = 0x8c26b990;	// projection distance for 640 px (~320 = 90 deg)
static constexpr u32 DRAW_HPR = DRAW_HEADING;		// _nDrawHeading/Pitch/Roll: unhooked, drive the sky
static constexpr u32 CAM_ROT = 0x8c1fc99c + 144;	// _CamXfmData.rot: what BuildCam got (hooked)

static const u16 stubCode[] = {
	0xD00B,		// 00 mov.l @(L_POSE,PC),R0
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
	0x6106,		// 22 mov.l @R0+,R1
	0x2012,		// 24 mov.l R1,@R0      usedId = id: which pose this camera was built with
	0x7004,		// 26 add #4,R0
	0x2052,		// 28 mov.l R5,@R0      skyHeading = final camera heading
	0xD002,		// 2a mov.l @(L_ORIG,PC),R0
	0x402B,		// 2c jmp @R0
	0x0009,		// 2e nop
	(u16)POSE, (u16)(POSE >> 16),			// 30 L_POSE
	(u16)BUILDCAM, (u16)(BUILDCAM >> 16),	// 34 L_ORIG
};

struct PoseBlock {
	float dx, dy, dz;	// world space
	s32 dh;
	s32 dp;
	u32 pitchMask;
	s32 dr;
	u32 rollMask;
	u32 id;				// FramePose ring id of this pose
	u32 usedId;			// written by the stub
	s32 skyHeading;		// written by the stub: the heading the sky is built for (see SKY_LITERALS)
};
static_assert(sizeof(stubCode) <= POSE - STUB && POSE - STUB + sizeof(PoseBlock) <= STUB_AREA, "stub layout");
static constexpr u32 USED_ID = POSE + offsetof(PoseBlock, usedId);
static constexpr u32 SKY_HEADING = POSE + offsetof(PoseBlock, skyHeading);

// Sky: the sky module builds the backdrop for Player_aData[player].Phys.nHeading (the boat,
// 0x8c256570), read through these three literals, so it ignored the head and only covered the
// boat's forward view. They are pointed at skyHeading instead - the heading the camera was
// actually built with - so the sky is generated for wherever the camera looks. Single player only
// (the readers add player * 0x330).
static constexpr u32 PHYS_HEADING = 0x8c256570;
static const u32 skyLiterals[] = { 0x8c04b890, 0x8c04c07c, 0x8c04c424 };

// Field of view: Viewport_Set (0x8c07dc30, fov R4 in bradians, FR4) computes the projection
// distance (Viewport_zscreen) and the culling frustum. It runs when a camera mode is set up,
// called through these literals. They are pointed at VP_STUB, which substitutes VP_FOV when
// it is non-zero (headset in use) and jumps on, so the game renders - and culls - wider.
static constexpr u32 VIEWPORT_SET = 0x8c07dc30;
static constexpr u32 VP_STUB = 0x8c00b9c0;
static constexpr u32 VP_FOV = VP_STUB + 0x18;
static constexpr u32 VP_AREA = 0x20;
static const u32 viewportSetLiterals[] = {
	0x8c03ae14, 0x8c03bc84, 0x8c03bdb8, 0x8c03bec0, 0x8c03bfdc, 0x8c03c1d4, 0x8c03c4a8,
	0x8c03c61c, 0x8c03c898, 0x8c03ca80, 0x8c03cbfc, 0x8c03cd70, 0x8c03cf34, 0x8c0799a0,
};
static const u16 vpStubCode[] = {
	0xD003,		// 00 mov.l @(L_FOV,PC),R0
	0x6002,		// 02 mov.l @R0,R0
	0x2008,		// 04 tst R0,R0
	0x8900,		// 06 bt 0a
	0x6403,		// 08 mov R0,R4        fov = VP_FOV
	0xD002,		// 0a mov.l @(L_VP,PC),R0
	0x402B,		// 0c jmp @R0
	0x0009,		// 0e nop
	(u16)VP_FOV, (u16)(VP_FOV >> 16),				// 10 L_FOV
	(u16)VIEWPORT_SET, (u16)(VIEWPORT_SET >> 16),	// 14 L_VP
};
static_assert(sizeof(vpStubCode) <= VP_FOV - VP_STUB && VP_FOV + 4 <= VP_STUB + VP_AREA, "vp stub layout");
static_assert(STUB + STUB_AREA <= VP_STUB, "stub areas overlap");

// Poses handed to the game, by id, so the renderer can tell the headset which head pose each
// frame was drawn for.
static std::mutex frameMutex;
static FramePose frames[64];
static u32 poseSeq;

static std::mutex headMutex;
static bool headOverride;
static HeadPose overridePose;

void setHeadPoseOverride(bool active, const HeadPose& pose)
{
	std::lock_guard<std::mutex> lock(headMutex);
	headOverride = active;
	overridePose = pose;
}

static std::mutex xrMutex;
static bool xrValid;
static XrHeadPose xrHead;
static float xrIpd;

void setXrHead(bool valid, const XrHeadPose& head, float ipdMeters)
{
	std::lock_guard<std::mutex> lock(xrMutex);
	xrValid = valid;
	xrHead = head;
	xrIpd = ipdMeters;
}

// LOCAL-space head pose -> camera-relative HeadPose (degrees, game units). Euler order matches
// R = Ry(yaw) Rx(pitch) Rz(roll).
static HeadPose headFromXr(const XrHeadPose& h, float unitsPerMeter)
{
	const float x = h.q[0], y = h.q[1], z = h.q[2], w = h.q[3];
	const float m02 = 2 * (x * z + w * y), m12 = 2 * (y * z - w * x), m22 = 1 - 2 * (x * x + y * y);
	const float m10 = 2 * (x * y + w * z), m11 = 1 - 2 * (x * x + z * z);
	constexpr float deg = 180.f / (float)M_PI;
	HeadPose p;
	p.yaw = std::atan2(m02, m22) * deg;
	p.pitch = std::asin(std::clamp(-m12, -1.f, 1.f)) * deg;
	p.roll = std::atan2(m10, m11) * deg;
	p.x = h.p[0] * unitsPerMeter;
	p.y = h.p[1] * unitsPerMeter;
	p.z = h.p[2] * unitsPerMeter;
	return p;
}

bool framePose(u32 id, FramePose& out)
{
	if (id == 0)
		return false;
	std::lock_guard<std::mutex> lock(frameMutex);
	out = frames[id % std::size(frames)];
	return out.id == id;
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
	PoseBlock zero{ 0, 0, 0, 0, 0, ~0u, 0, ~0u, 0, 0, (s32)readU32(PHYS_HEADING) };
	memcpy(ram(POSE), &zero, sizeof(zero));
	writeU32(CALL_LITERAL, STUB);			// emu thread: dynarec invalidates the caller's block
	NOTICE_LOG(COMMON, "ht4q camhook installed");
	return true;
}

// Sky literal redirect; needs the camera stub (which writes skyHeading).
static void ensureSkyHook()
{
	static bool done;
	static const bool enabled = std::string(env("HT4Q_SKYHOOK", "1")) != "0";
	if (done || !enabled)
		return;
	for (u32 lit : skyLiterals)
	{
		u32 v = readU32(lit);
		if (v != PHYS_HEADING && v != SKY_HEADING)
			return;		// not the expected code (yet)
	}
	for (u32 lit : skyLiterals)
		writeU32(lit, SKY_HEADING);
	done = true;
	NOTICE_LOG(COMMON, "ht4q sky hook installed");
}

// Viewport_Set redirect; best effort, independent of the camera hook.
static void ensureViewportHook()
{
	static bool done;
	if (done)
		return;
	bool any = false;
	for (u32 lit : viewportSetLiterals)
	{
		u32 v = readU32(lit);
		if (v != VIEWPORT_SET && v != VP_STUB)
			return;		// not the expected code (yet)
		any |= v == VIEWPORT_SET;
	}
	if (memcmp(ram(VP_STUB), vpStubCode, sizeof(vpStubCode)) != 0)
	{
		for (u32 i = 0; i < VP_AREA; i++)
			if (*ram(VP_STUB + i) != 0)
				return;
		memcpy(ram(VP_STUB), vpStubCode, sizeof(vpStubCode));
	}
	if (any)
		for (u32 lit : viewportSetLiterals)
			writeU32(lit, VP_STUB);
	done = true;
	NOTICE_LOG(COMMON, "ht4q viewport hook installed");
}

static bool hookActive()
{
	static const bool enabled = std::string(env("HT4Q_CAMHOOK", "1")) != "0";
	return enabled && settings.content.gameId == "T-9702N";
}

u32 onStartRender()
{
	if (!hookActive() || readU32(CALL_LITERAL) != STUB)
		return 0;
	u32 id = readU32(USED_ID);
	float zscreen;
	memcpy(&zscreen, ram(VIEWPORT_ZSCREEN), 4);
	std::lock_guard<std::mutex> lock(frameMutex);
	FramePose& fp = frames[id % std::size(frames)];
	if (fp.id == id)
	{
		if (zscreen > 100.f && zscreen < 2000.f)
			fp.zscreen = zscreen;
		memcpy(fp.gameRot, ram(DRAW_HPR), sizeof(fp.gameRot));
		memcpy(fp.camRot, ram(CAM_ROT), sizeof(fp.camRot));
		fp.rotValid = true;
	}
	return id;
}

static void camhookPoll()
{
#ifdef HT4Q_XR
	static const std::string level = env("HT4Q_LEVEL", "all");	// keep the horizon level in VR
#else
	static const std::string level = env("HT4Q_LEVEL", "off");
#endif
	if (!hookActive() || !ensureInstalled())
		return;
	ensureViewportHook();
	ensureSkyHook();

	FramePose fp{};
	{
		std::lock_guard<std::mutex> lock(xrMutex);
		u32 phys = readU32(ATTACHED_PHYS);
		// immersive: racing with a headset - the head drives the camera
		fp.immersive = xrValid && (phys & 0x1f000000) == 0x0c000000;
		// wider field of view whenever the headset is in use; takes effect at the next camera setup
		float fovDeg = stereoParams().vrFovDeg;
		writeU32(VP_FOV, xrValid && fovDeg > 0 ? (u32)std::lround(fovDeg * 65536.f / 360.f) : 0);
		fp.head = xrHead;
		fp.eyeOffset = xrIpd / 2 * stereoParams().unitsPerMeter;
	}
	HeadPose target;
	bool immediate;
	{
		std::lock_guard<std::mutex> lock(headMutex);
		immediate = headOverride || fp.immersive;
		target = headOverride ? overridePose
				: fp.immersive ? headFromXr(fp.head, stereoParams().unitsPerMeter)
				: mousePose();
	}
	// light smoothing for the mouse; scripted and headset poses apply immediately
	static HeadPose cur{};
	float a = immediate ? 1.f : 0.35f;
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
	pb.id = ++poseSeq == 0 ? ++poseSeq : poseSeq;
	pb.usedId = readU32(USED_ID);
	pb.skyHeading = (s32)readU32(SKY_HEADING);
	{
		std::lock_guard<std::mutex> lock(frameMutex);
		fp.id = pb.id;
		fp.zscreen = 320.f;
		frames[pb.id % std::size(frames)] = fp;
	}
	memcpy(ram(POSE), &pb, sizeof(pb));
}

void onMaplePoll(MapleInputState inputState[4])
{
	applyXrInput(inputState);
	memserverPoll(inputState);	// scripted virtual controller wins
	camhookPoll();
}

}	// namespace ht4q
