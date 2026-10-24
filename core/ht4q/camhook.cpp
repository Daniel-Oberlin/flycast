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

	2-player races: the game builds its one camera for each player in turn (Player_nHuman = the
	player being processed), so the stubs pick the player's pose block. Player 2's lies one
	Player_aData stride (0x330) after player 1's, where the per-player sky readers look (below).
	Player 2's head pose comes only from the memserver override (player 2) for now.

	Environment:
	  HT4Q_CAMHOOK=0          don't install the hook
	  HT4Q_LEVEL=roll | all   cancel the game camera's roll (and pitch) so the horizon stays level
	                          (default off; all in Quest builds)
*/
#include "types.h"
#include "hw/sh4/sh4_mem.h"
#include "input/mouse.h"
#include "ht4q.h"
#include "stream.h"
#include "hw/pvr/ta_ctx.h"
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
static constexpr u32 POSE = STUB + 0x50;
static constexpr u32 STUB_AREA = 0x80;			// bytes that must be free before installing
static constexpr u32 PLAYER_STRIDE = 0x330;		// sizeof(Player_aData[0])
static constexpr u32 POSE2 = POSE + PLAYER_STRIDE;	// player 2's pose block, then its sky pose
static constexpr u32 SKY_POSE2 = POSE2 + 0x30;
static constexpr u32 POSE2_AREA = 0x50;
static constexpr u32 PLAYER_NHUMAN = 0x8c25977c;	// Player_nHuman: the player being processed
static constexpr u32 PLAYER_HUMANS = 0x8c25978c;	// Player_nHumanCount
static constexpr u32 DRAW_HEADING = 0x8c1fca74;	// _nDrawHeading, bradians
static constexpr u32 ATTACHED_PHYS = 0x8c1fc988;	// _pAttachedPhys: non-null while racing
static constexpr u32 VIEWPORT_ZSCREEN = 0x8c26b990;	// projection distance for 640 px (~320 = 90 deg)
static constexpr u32 DRAW_HPR = DRAW_HEADING;		// _nDrawHeading/Pitch/Roll: unhooked, drive the sky
static constexpr u32 CAM_XFM = 0x8c1fc99c;			// _CamXfmData.xfm.mf: world -> camera
static constexpr u32 CAM_ROT = CAM_XFM + 144;		// _CamXfmData.rot: what BuildCam got (hooked)

static const u16 stubCode[] = {
	0xD10E,		// 00 mov.l @(L_HUMAN,PC),R1
	0x6112,		// 02 mov.l @R1,R1
	0xD00E,		// 04 mov.l @(L_POSE,PC),R0
	0x2118,		// 06 tst R1,R1
	0x8900,		// 08 bt 0c
	0xD00E,		// 0a mov.l @(L_POSE2,PC),R0    player 2
	0xF009,		// 0c fmov.s @R0+,FR0
	0xF400,		// 0e fadd FR0,FR4      x
	0xF009,		// 10 fmov.s @R0+,FR0
	0xF500,		// 12 fadd FR0,FR5      y
	0xF009,		// 14 fmov.s @R0+,FR0
	0xF600,		// 16 fadd FR0,FR6      z
	0x6106,		// 18 mov.l @R0+,R1
	0x351C,		// 1a add R1,R5         heading += dh
	0x6106,		// 1c mov.l @R0+,R1
	0x6206,		// 1e mov.l @R0+,R2
	0x2629,		// 20 and R2,R6         pitch &= pitchMask
	0x361C,		// 22 add R1,R6         pitch += dp
	0x6106,		// 24 mov.l @R0+,R1
	0x6206,		// 26 mov.l @R0+,R2
	0x2729,		// 28 and R2,R7         roll &= rollMask
	0x371C,		// 2a add R1,R7         roll += dr
	0x6106,		// 2c mov.l @R0+,R1
	0x2012,		// 2e mov.l R1,@R0      usedId = id: which pose this camera was built with
	0x7004,		// 30 add #4,R0
	0x2052,		// 32 mov.l R5,@R0      skyHeading = final camera heading
	0xD004,		// 34 mov.l @(L_ORIG,PC),R0
	0x402B,		// 36 jmp @R0
	0x0009,		// 38 nop
	0x0009,		// 3a
	(u16)PLAYER_NHUMAN, (u16)(PLAYER_NHUMAN >> 16),	// 3c L_HUMAN
	(u16)POSE, (u16)(POSE >> 16),			// 40 L_POSE
	(u16)POSE2, (u16)(POSE2 >> 16),			// 44 L_POSE2
	(u16)BUILDCAM, (u16)(BUILDCAM >> 16),	// 48 L_ORIG
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
// actually built with - so the sky is generated for wherever the camera looks. The readers add
// player * 0x330, which lands on player 2's pose block.
static constexpr u32 PHYS_HEADING = 0x8c256570;
static const u32 skyLiterals[] = { 0x8c04b890, 0x8c04c07c, 0x8c04c424 };

// The sky panels themselves come from sky_Work(heading R4, pitch R5, roll R6) at 0x8c0744ee,
// called each frame by the camera update with the unhooked draw angles (and by the camera mode
// setups). Those calls go through SKYWORK_STUB, which applies SKY_POSE: the camera's head
// offsets on a flat screen, and in the headset only the heading, with pitch and roll zeroed - a
// level sky that the renderer then paints per pixel for the real view (ht4q_stereo.cpp).
static constexpr u32 SKY_WORK = 0x8c0744ee;
static constexpr u32 SKYWORK_STUB = 0x8c00b980;
static constexpr u32 SKYWORK_AREA = 0x40;
static const u32 skyWorkLiterals[] = {
	0x8c04634c, 0x8c03b6d8, 0x8c03bfd4, 0x8c03c1cc, 0x8c03c4e8, 0x8c03cbf4, 0x8c03cfd0,
};
static constexpr u32 SKY_POSE = 0x8c00b9e0;
static constexpr u32 SKY_POSE_AREA = 0x20;
struct SkyPose {
	s32 dh;
	s32 dp;
	u32 pitchMask;
	s32 dr;
	u32 rollMask;
};
static_assert(sizeof(SkyPose) <= SKY_POSE_AREA, "sky pose layout");
static_assert(sizeof(PoseBlock) <= SKY_POSE2 - POSE2 && SKY_POSE2 + SKY_POSE_AREA <= POSE2 + POSE2_AREA, "pose 2 layout");
static const u16 skyWorkStubCode[] = {
	0xD109,		// 00 mov.l @(L_HUMAN,PC),R1
	0x6112,		// 02 mov.l @R1,R1
	0xD009,		// 04 mov.l @(L_POSE,PC),R0
	0x2118,		// 06 tst R1,R1
	0x8900,		// 08 bt 0c
	0xD009,		// 0a mov.l @(L_POSE2,PC),R0    player 2
	0x6106,		// 0c mov.l @R0+,R1
	0x341C,		// 0e add R1,R4         heading += dh
	0x6106,		// 10 mov.l @R0+,R1
	0x6206,		// 12 mov.l @R0+,R2
	0x2529,		// 14 and R2,R5         pitch &= pitchMask
	0x351C,		// 16 add R1,R5         pitch += dp
	0x6106,		// 18 mov.l @R0+,R1
	0x6206,		// 1a mov.l @R0+,R2
	0x2629,		// 1c and R2,R6         roll &= rollMask
	0x361C,		// 1e add R1,R6         roll += dr
	0xD004,		// 20 mov.l @(L_SKY,PC),R0
	0x402B,		// 22 jmp @R0
	0x0009,		// 24 nop
	0x0009,		// 26
	(u16)PLAYER_NHUMAN, (u16)(PLAYER_NHUMAN >> 16),	// 28 L_HUMAN
	(u16)SKY_POSE, (u16)(SKY_POSE >> 16),	// 2c L_POSE
	(u16)SKY_POSE2, (u16)(SKY_POSE2 >> 16),	// 30 L_POSE2
	(u16)SKY_WORK, (u16)(SKY_WORK >> 16),	// 34 L_SKY
};
static_assert(sizeof(skyWorkStubCode) <= SKYWORK_AREA && STUB + STUB_AREA <= SKYWORK_STUB, "sky stub layout");

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
static_assert(STUB + STUB_AREA <= VP_STUB && SKYWORK_STUB + SKYWORK_AREA <= VP_STUB, "stub areas overlap");

// Sun and lens flare: the sun drawer (0x8c01d8d8) projects the sun's world point with the camera
// correctly, then places its eight sprites at t * that offset from the centre through a sprite
// routine that works in a smaller layout and stretches it to the screen by 1.25 x 1.1 - offsets
// included. So the sun sat 1.25x / 1.1x too far from the centre and, with head tracking, slid
// faster than the world as the head turned. Its one call to the routine (literal SUN_SPRITE_LITERAL)
// goes through SUN_STUB, which divides the offset (FR4, FR5) by that stretch first.
static constexpr u32 SPRITE_DRAW = 0x8c0539a0;
static constexpr u32 SUN_SPRITE_LITERAL = 0x8c01dd98;
static constexpr u32 SUN_STUB = 0x8c00ba00;
static constexpr u32 SUN_AREA = 0x20;
static const u16 sunStubCode[] = {
	0xC703,		// 00 mova @(L_SCALE,PC),R0
	0xF009,		// 02 fmov.s @R0+,FR0
	0xF402,		// 04 fmul FR0,FR4      x /= 1.25
	0xF008,		// 06 fmov.s @R0,FR0
	0xF502,		// 08 fmul FR0,FR5      y /= 1.1
	0xD003,		// 0a mov.l @(L_DRAW,PC),R0
	0x402B,		// 0c jmp @R0
	0x0009,		// 0e nop
	0xCCCD, 0x3F4C,		// 10 L_SCALE 0.8f
	0xBA2F, 0x3F68,		// 14         1 / 1.1f
	(u16)SPRITE_DRAW, (u16)(SPRITE_DRAW >> 16),	// 18 L_DRAW
};
static_assert(sizeof(sunStubCode) <= SUN_AREA && VP_STUB + VP_AREA <= SUN_STUB, "sun stub layout");

// Poses handed to the game, by id, so the renderer can tell the headset which head pose each
// frame was drawn for.
static std::mutex frameMutex;
static FramePose frames[128];
static u32 poseSeq;

static std::mutex headMutex;
static bool headOverride[2];
static HeadPose overridePose[2];

void setHeadPoseOverride(bool active, const HeadPose& pose, int player)
{
	std::lock_guard<std::mutex> lock(headMutex);
	headOverride[player] = active;
	overridePose[player] = pose;
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
u32 readRam32(u32 addr) { return readU32(addr); }
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
	for (u32 i = 0; i < POSE2_AREA; i++)
		if (*ram(POSE2 + i) != 0)
			return false;
	memcpy(ram(STUB), stubCode, sizeof(stubCode));
	PoseBlock zero{ 0, 0, 0, 0, 0, ~0u, 0, ~0u, 0, 0, (s32)readU32(PHYS_HEADING) };
	memcpy(ram(POSE), &zero, sizeof(zero));
	zero.skyHeading = (s32)readU32(PHYS_HEADING + PLAYER_STRIDE);
	memcpy(ram(POSE2), &zero, sizeof(zero));
	writeU32(CALL_LITERAL, STUB);			// emu thread: dynarec invalidates the caller's block
	NOTICE_LOG(COMMON, "ht4q camhook installed");
	return true;
}

// Sky literal redirect; needs the camera stub (which writes skyHeading).
static bool skyHookDone;
static void ensureSkyHook()
{
	bool& done = skyHookDone;
	static const bool enabled = std::string(env("HT4Q_SKYHOOK", "1")) != "0";
	if (done || !enabled)
		return;
	for (u32 lit : skyLiterals)
	{
		u32 v = readU32(lit);
		if (v != PHYS_HEADING && v != SKY_HEADING)
			return;		// not the expected code (yet)
	}
	for (u32 lit : skyWorkLiterals)
	{
		u32 v = readU32(lit);
		if (v != SKY_WORK && v != SKYWORK_STUB)
			return;
	}
	if (memcmp(ram(SKYWORK_STUB), skyWorkStubCode, sizeof(skyWorkStubCode)) != 0)
	{
		for (u32 i = 0; i < SKYWORK_AREA; i++)
			if (*ram(SKYWORK_STUB + i) != 0)
				return;
		for (u32 i = 0; i < SKY_POSE_AREA; i++)
			if (*ram(SKY_POSE + i) != 0)
				return;
		SkyPose identity{ 0, 0, ~0u, 0, ~0u };
		memcpy(ram(SKY_POSE), &identity, sizeof(identity));
		memcpy(ram(SKY_POSE2), &identity, sizeof(identity));
		memcpy(ram(SKYWORK_STUB), skyWorkStubCode, sizeof(skyWorkStubCode));
	}
	for (u32 lit : skyLiterals)
		writeU32(lit, SKY_HEADING);
	for (u32 lit : skyWorkLiterals)
		writeU32(lit, SKYWORK_STUB);
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

// Sun sprite redirect; best effort, independent of the camera hook.
static void ensureSunHook()
{
	static bool done;
	static const bool enabled = std::string(env("HT4Q_SUNHOOK", "1")) != "0";
	if (done || !enabled)
		return;
	u32 v = readU32(SUN_SPRITE_LITERAL);
	if (v != SPRITE_DRAW && v != SUN_STUB)
		return;		// not the expected code (yet)
	if (memcmp(ram(SUN_STUB), sunStubCode, sizeof(sunStubCode)) != 0)
	{
		for (u32 i = 0; i < SUN_AREA; i++)
			if (*ram(SUN_STUB + i) != 0)
				return;
		memcpy(ram(SUN_STUB), sunStubCode, sizeof(sunStubCode));
	}
	writeU32(SUN_SPRITE_LITERAL, SUN_STUB);
	done = true;
	NOTICE_LOG(COMMON, "ht4q sun hook installed");
}

static bool hookActive()
{
	static const bool enabled = std::string(env("HT4Q_CAMHOOK", "1")) != "0";
	return enabled && settings.content.gameId == "T-9702N";
}

void onStartRender(rend_context& rend)
{
	rend.ht4qPoseId = rend.ht4qPoseId2 = 0;
	if (!hookActive() || readU32(CALL_LITERAL) != STUB)
		return;
	const u32 id = readU32(USED_ID);
	rend.ht4qPoseId = id;
	if (readU32(PLAYER_HUMANS) == 2)
		rend.ht4qPoseId2 = readU32(POSE2 + offsetof(PoseBlock, usedId));
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
		memcpy(fp.camXfm, ram(CAM_XFM), sizeof(fp.camXfm));
		fp.rotValid = true;	// the camera last built: player 2's in 2-player races
	}
}

// Hands one player's head pose to the game (its pose block and sky pose) and records the frame pose.
static void writePose(int player, const HeadPose& cur, FramePose& fp, bool twoPlayers, const std::string& level)
{
	// head -> unhooked camera rotation R = Ry(yaw) Rx(pitch) Rz(roll), for the HUD (ht4q_stereo.cpp)
	{
		constexpr float rad = (float)M_PI / 180.f;
		const float cy = std::cos(cur.yaw * rad), sy = std::sin(cur.yaw * rad);
		const float cp = std::cos(cur.pitch * rad), sp = std::sin(cur.pitch * rad);
		const float cr = std::cos(cur.roll * rad), sr = std::sin(cur.roll * rad);
		const float r[9] = {
			cy * cr + sy * sp * sr, -cy * sr + sy * sp * cr, sy * cp,
			cp * sr, cp * cr, -sp,
			-sy * cr + cy * sp * sr, sy * sr + cy * sp * cr, cy * cp,
		};
		memcpy(fp.headRot, r, sizeof(r));
	}
	// head-local (x right, y up, z back) -> world, using the game's draw heading (2 players: the boat's,
	// as the one camera's draw heading is whichever player was processed last).
	// right = (cos h, 0, -sin h), back = (sin h, 0, cos h)
	const u32 headingAddr = twoPlayers ? PHYS_HEADING + player * PLAYER_STRIDE : DRAW_HEADING;
	float h = (s32)readU32(headingAddr) * (float)(2 * M_PI / 65536);
	const u32 block = player == 0 ? POSE : POSE2;
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
	pb.usedId = readU32(block + offsetof(PoseBlock, usedId));
	pb.skyHeading = (s32)readU32(block + offsetof(PoseBlock, skyHeading));
	// headset: a level sky, painted for the real view by the renderer (one player only for now)
	static const bool skyTest = env("HT4Q_SKY_TEST", "0")[0] == '1';	// same on the desktop, for testing
	const bool skyLevel = (fp.immersive || skyTest) && stereoParams().skyCorrect > 0.f && !twoPlayers;
	SkyPose sky = skyLevel ? SkyPose{ pb.dh, 0, 0, 0, 0 } : SkyPose{ pb.dh, pb.dp, pb.pitchMask, pb.dr, pb.rollMask };
	if (skyHookDone)
		memcpy(ram(player == 0 ? SKY_POSE : SKY_POSE2), &sky, sizeof(sky));
	fp.skyLevel = skyLevel && skyHookDone;
	{
		std::lock_guard<std::mutex> lock(frameMutex);
		fp.id = pb.id;
		fp.zscreen = 320.f;
		frames[pb.id % std::size(frames)] = fp;
	}
	memcpy(ram(block), &pb, sizeof(pb));
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
	ensureSunHook();

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
		fp.eyeOffset = xrIpd / 2 * stereoParams().unitsPerMeter * depthEffectFactor();
	}
	HeadPose target, target2{};
	bool immediate;
	{
		std::lock_guard<std::mutex> lock(headMutex);
		immediate = headOverride[0] || fp.immersive;
		target = headOverride[0] ? overridePose[0]
				: fp.immersive ? headFromXr(fp.head, stereoParams().unitsPerMeter)
				: mousePose();
		if (headOverride[1])
			target2 = overridePose[1];
	}
	// light smoothing for the mouse; scripted and headset poses apply immediately
	static HeadPose cur{};
	float a = immediate ? 1.f : 0.35f;
	for (int i = 0; i < 6; i++)
		(&cur.x)[i] += a * ((&target.x)[i] - (&cur.x)[i]);

	const bool twoPlayers = readU32(PLAYER_HUMANS) == 2;
	writePose(0, cur, fp, twoPlayers, level);
	FramePose fp2{};
	fp2.eyeOffset = fp.eyeOffset;
	writePose(1, target2, fp2, twoPlayers, level);	// kept neutral outside 2-player races
}

void onMaplePoll(MapleInputState inputState[4])
{
	applyXrInput(inputState);
	streamPoll(inputState);		// the remote viewer's controller on port B
	memserverPoll(inputState);	// scripted virtual controller wins
	frameRatePoll();
	twoPlayerPoll();
	camhookPoll();
}

}	// namespace ht4q
