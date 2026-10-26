/*
	HydroThunder4Quest: head-pose camera hook for 18 Wheeler: American Pro Trucker (ReviveDC image,
	RDC-0094).

	The game's matrix library keeps the current matrix in the SH-4's back floating-point bank
	(XMTRX). The camera update (0x8c08dc0e) builds a player's view in XMTRX from the camera's eye,
	target and angles, then stores it into that player's slot of the view table (VIEW_TABLE, 64 bytes
	each) with the library's get-matrix (0x0c156060: XMTRX -> R4), called through CALL_LITERAL. That
	view is world -> camera with the camera looking down -z, y up, x right - OpenXR's axes - so the
	head pose applies directly: view' = H^-1 * view, H = the head's pose in camera space.

	We point CALL_LITERAL at STUB, which (when R4 is a player's view - slot 0, or slot 1 in Versus -
	and that player's block has its enabled flag set) copies the block's head id to usedId, multiplies
	XMTRX by the block's HINV with ftrv, and jumps on to get-matrix. Other views (the rear-view mirror
	is a separate camera) pass through. The stub and the two blocks live at the top of the IP.BIN area
	(0x8c00fd00..0x8c00ff20), zero during play. Each poll we refresh HINV. Assembled with a script
	(one column of the view per ftrv).
*/
#include "eighteenwheeler.h"
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>

namespace ht4q::eighteenwheeler
{

static constexpr u32 CALL_LITERAL = 0x8c08dda4;	// get-matrix's address for the camera update's store
static constexpr u32 GET_MATRIX = 0x0c156060;	// get-matrix(dst): XMTRX -> dst (dst 0: the stack top)
static constexpr u32 VIEW_TABLE = 0x0c2735b8;	// the views, one per player; [0] is the main one
static constexpr u32 STUB = 0x8c00fd00;
static constexpr u32 BLOCK = STUB + 0x100;		// player 1's pose
static constexpr u32 BLOCK2 = BLOCK + 0x90;		// player 2's
static constexpr u32 AREA = 0x220;				// stub + blocks: must be zero before installing
// Projection: the library's projection matrix (0x8c39eed8) has the distance at PROJ_DIST, the same
// for x and y (578.78 for 640x480: 58 x 45 deg). Head yaw/pitch tests (frame dumps, every vertex
// before and after a 10 deg turn) fit 578.8 both ways; 1/w in the TA = 1/distance.
static constexpr u32 PROJ_DIST = 0x8c39eedc;
// The driving camera's vertical field of view (65536 per turn: 0x2008 = 45 deg), which the projection
// follows from the next frame; set from the view mode's settings when the view changes.
static constexpr u32 CAM_FOV = 0x8c56a490;

static const u16 stubCode[] = {
0xD13B,		// 00 mov.l @(L_BLOCK,PC),R1  R1 = player 1 block
	0xD03D,		// 02 mov.l @(L_VIEW,PC),R0  R0 = player 1 view
	0x3040,		// 04 cmp/eq R4,R0
	0x8903,		// 06 bt common (+3)
	0x7040,		// 08 add #64,R0   player 2 view
	0x3040,		// 0a cmp/eq R4,R0
	0x8B6C,		// 0c bf orig (+108)
	0xD13B,		// 0e mov.l @(L_BLOCK2,PC),R1  R1 = player 2 block
	0x6012,		// 10 mov.l @R1,R0   enabled
	0x2008,		// 12 tst R0,R0
	0x8968,		// 14 bt orig (+104)
	0x5011,		// 16 mov.l @(4,R1),R0  id
	0x1102,		// 18 mov.l R0,@(8,R1)  usedId = id
	0x6213,		// 1a mov R1,R2
	0x727F,		// 1c add #127,R2
	0x7211,		// 1e add #17,R2   R2 = block+0x90
	0xFBFD,		// 20 frchg
	0xF2FB,		// 22 fmov.s FR15,@-R2
	0xF2EB,		// 24 fmov.s FR14,@-R2
	0xF2DB,		// 26 fmov.s FR13,@-R2
	0xF2CB,		// 28 fmov.s FR12,@-R2
	0xF2BB,		// 2a fmov.s FR11,@-R2
	0xF2AB,		// 2c fmov.s FR10,@-R2
	0xF29B,		// 2e fmov.s FR9,@-R2
	0xF28B,		// 30 fmov.s FR8,@-R2
	0xF27B,		// 32 fmov.s FR7,@-R2
	0xF26B,		// 34 fmov.s FR6,@-R2
	0xF25B,		// 36 fmov.s FR5,@-R2
	0xF24B,		// 38 fmov.s FR4,@-R2
	0xF23B,		// 3a fmov.s FR3,@-R2
	0xF22B,		// 3c fmov.s FR2,@-R2
	0xF21B,		// 3e fmov.s FR1,@-R2
	0xF20B,		// 40 fmov.s FR0,@-R2
	0xFBFD,		// 42 frchg
	0x6213,		// 44 mov R1,R2
	0x7210,		// 46 add #16,R2
	0xFBFD,		// 48 frchg
	0xF029,		// 4a fmov.s @R2+,FR0
	0xF129,		// 4c fmov.s @R2+,FR1
	0xF229,		// 4e fmov.s @R2+,FR2
	0xF329,		// 50 fmov.s @R2+,FR3
	0xF429,		// 52 fmov.s @R2+,FR4
	0xF529,		// 54 fmov.s @R2+,FR5
	0xF629,		// 56 fmov.s @R2+,FR6
	0xF729,		// 58 fmov.s @R2+,FR7
	0xF829,		// 5a fmov.s @R2+,FR8
	0xF929,		// 5c fmov.s @R2+,FR9
	0xFA29,		// 5e fmov.s @R2+,FR10
	0xFB29,		// 60 fmov.s @R2+,FR11
	0xFC29,		// 62 fmov.s @R2+,FR12
	0xFD29,		// 64 fmov.s @R2+,FR13
	0xFE29,		// 66 fmov.s @R2+,FR14
	0xFF29,		// 68 fmov.s @R2+,FR15
	0xFBFD,		// 6a frchg
	0x6213,		// 6c mov R1,R2
	0x7250,		// 6e add #80,R2
	0xF029,		// 70 fmov.s @R2+,FR0
	0xF129,		// 72 fmov.s @R2+,FR1
	0xF229,		// 74 fmov.s @R2+,FR2
	0xF329,		// 76 fmov.s @R2+,FR3
	0xF1FD,		// 78 ftrv XMTRX,FV0
	0xF23B,		// 7a fmov.s FR3,@-R2
	0xF22B,		// 7c fmov.s FR2,@-R2
	0xF21B,		// 7e fmov.s FR1,@-R2
	0xF20B,		// 80 fmov.s FR0,@-R2
	0x7210,		// 82 add #16,R2
	0xF029,		// 84 fmov.s @R2+,FR0
	0xF129,		// 86 fmov.s @R2+,FR1
	0xF229,		// 88 fmov.s @R2+,FR2
	0xF329,		// 8a fmov.s @R2+,FR3
	0xF1FD,		// 8c ftrv XMTRX,FV0
	0xF23B,		// 8e fmov.s FR3,@-R2
	0xF22B,		// 90 fmov.s FR2,@-R2
	0xF21B,		// 92 fmov.s FR1,@-R2
	0xF20B,		// 94 fmov.s FR0,@-R2
	0x7210,		// 96 add #16,R2
	0xF029,		// 98 fmov.s @R2+,FR0
	0xF129,		// 9a fmov.s @R2+,FR1
	0xF229,		// 9c fmov.s @R2+,FR2
	0xF329,		// 9e fmov.s @R2+,FR3
	0xF1FD,		// a0 ftrv XMTRX,FV0
	0xF23B,		// a2 fmov.s FR3,@-R2
	0xF22B,		// a4 fmov.s FR2,@-R2
	0xF21B,		// a6 fmov.s FR1,@-R2
	0xF20B,		// a8 fmov.s FR0,@-R2
	0x7210,		// aa add #16,R2
	0xF029,		// ac fmov.s @R2+,FR0
	0xF129,		// ae fmov.s @R2+,FR1
	0xF229,		// b0 fmov.s @R2+,FR2
	0xF329,		// b2 fmov.s @R2+,FR3
	0xF1FD,		// b4 ftrv XMTRX,FV0
	0xF23B,		// b6 fmov.s FR3,@-R2
	0xF22B,		// b8 fmov.s FR2,@-R2
	0xF21B,		// ba fmov.s FR1,@-R2
	0xF20B,		// bc fmov.s FR0,@-R2
	0x7210,		// be add #16,R2
	0x6213,		// c0 mov R1,R2
	0x7250,		// c2 add #80,R2
	0xFBFD,		// c4 frchg
	0xF029,		// c6 fmov.s @R2+,FR0
	0xF129,		// c8 fmov.s @R2+,FR1
	0xF229,		// ca fmov.s @R2+,FR2
	0xF329,		// cc fmov.s @R2+,FR3
	0xF429,		// ce fmov.s @R2+,FR4
	0xF529,		// d0 fmov.s @R2+,FR5
	0xF629,		// d2 fmov.s @R2+,FR6
	0xF729,		// d4 fmov.s @R2+,FR7
	0xF829,		// d6 fmov.s @R2+,FR8
	0xF929,		// d8 fmov.s @R2+,FR9
	0xFA29,		// da fmov.s @R2+,FR10
	0xFB29,		// dc fmov.s @R2+,FR11
	0xFC29,		// de fmov.s @R2+,FR12
	0xFD29,		// e0 fmov.s @R2+,FR13
	0xFE29,		// e2 fmov.s @R2+,FR14
	0xFF29,		// e4 fmov.s @R2+,FR15
	0xFBFD,		// e6 frchg
	0xD002,		// e8 mov.l @(L_ORIG,PC),R0  orig:
	0x402B,		// ea jmp @R0
	0x0009,		// ec nop
	0x0009,		// ee pad
	(u16)BLOCK, (u16)(BLOCK >> 16),	// L_BLOCK
	(u16)GET_MATRIX, (u16)(GET_MATRIX >> 16),	// L_ORIG
	(u16)VIEW_TABLE, (u16)(VIEW_TABLE >> 16),	// L_VIEW
	(u16)BLOCK2, (u16)(BLOCK2 >> 16),	// L_BLOCK2
};
static_assert(sizeof(stubCode) <= BLOCK - STUB, "stub layout");

struct PoseBlock {
	u32 enabled;
	u32 id;			// FramePose id of this pose
	u32 usedId;		// written by the stub: the pose the last view was built with
	u32 pad;
	float hinv[16];	// inverse head pose, the game's matrix layout (column-major: [c * 4 + r])
	float temp[16];	// the stub's scratch
};
static_assert(sizeof(PoseBlock) <= BLOCK2 - BLOCK && BLOCK2 + sizeof(PoseBlock) <= STUB + AREA, "block layout");
static constexpr u32 blocks[2] { BLOCK, BLOCK2 };

static bool hookActive()
{
	static const bool enabled = std::string(env("HT4Q_CAMHOOK", "1")) != "0";
	return enabled;
}

// Returns false when the game, RAM layout or stub area isn't what we expect.
static bool ensureInstalled()
{
	const u32 lit = readU32(CALL_LITERAL);
	if (lit == STUB)
	{
		if (memcmp(ram(STUB), stubCode, sizeof(stubCode)) == 0)
			return true;
		writeU32(CALL_LITERAL, GET_MATRIX);	// something overwrote the stub: back out
		WARN_LOG(COMMON, "ht4q 18wheeler: stub overwritten, hook removed");
		return false;
	}
	if (lit != GET_MATRIX)
		return false;	// not this build, or not loaded yet
	for (u32 i = 0; i < AREA; i++)
		if (*ram(STUB + i) != 0)
			return false;
	PoseBlock identity{};
	for (int i = 0; i < 4; i++)
		identity.hinv[i * 5] = 1.f;
	memcpy(ram(BLOCK), &identity, sizeof(identity));
	memcpy(ram(BLOCK2), &identity, sizeof(identity));
	memcpy(ram(STUB), stubCode, sizeof(stubCode));
	writeU32(CALL_LITERAL, STUB);	// emu thread: dynarec invalidates the caller's block
	NOTICE_LOG(COMMON, "ht4q 18wheeler camhook installed");
	return true;
}

u32 startRender()
{
	if (!hookActive() || readU32(CALL_LITERAL) != STUB)
		return 0;
	return readU32(BLOCK + offsetof(PoseBlock, usedId));
}

u32 startRender2()
{
	if (!hookActive() || readU32(CALL_LITERAL) != STUB || !versus())
		return 0;
	return readU32(BLOCK2 + offsetof(PoseBlock, usedId));
}

static float projDist()
{
	const float dist = readF32(PROJ_DIST);
	return dist > 100.f && dist < 5000.f ? dist : 578.78f;
}

// Hands one player's head pose to the game (its pose block) and records the frame pose.
static void writePose(int player, const HeadSample& head)
{
	const HeadPose& p = head.pose;
	// H: head -> camera, p_cam = R p_head + t; H^-1 = [R^T | -R^T t]
	const float *r = head.headRot;	// row-major
	const float t[3] { p.x, p.y, p.z };
	PoseBlock pb{};
	pb.enabled = 1;
	for (int row = 0; row < 3; row++)
	{
		float tr = 0.f;
		for (int col = 0; col < 3; col++)
		{
			pb.hinv[col * 4 + row] = r[col * 3 + row];	// (R^T)[row][col] = R[col][row]
			tr -= r[col * 3 + row] * t[col];
		}
		pb.hinv[12 + row] = tr;
	}
	pb.hinv[15] = 1.f;

	FramePose fp{};
	fp.immersive = head.immersive;
	fp.head = head.xr;
	fp.eyeOffset = head.eyeOffset;
	memcpy(fp.headRot, head.headRot, sizeof(fp.headRot));
	fp.zscreen = fp.zscreenY = projDist();	// the same for both players in Versus (full screen each)
	const u32 block = blocks[player];
	pb.id = storeFramePose(fp);
	pb.usedId = readU32(block + offsetof(PoseBlock, usedId));
	memcpy(ram(block), &pb, offsetof(PoseBlock, temp));
}

// With a headset, the image covers about its field of view (stereoParams().vrFovDeg, horizontal)
// rather than a 58 deg window; the game's own value comes back without one.
static void widenFov(bool headset)
{
	static u32 gameFov;
	const u32 fov = readU32(CAM_FOV);
	if (!inStage() || fov == 0 || fov >= 0x8000)
		return;		// no driving camera (or not the expected data)
	const float vrFov = stereoParams().vrFovDeg;
	const float halfV = std::atan(std::tan(vrFov * (float)M_PI / 360.f) * 0.75f);	// 4:3
	const u32 wide = (u32)std::lround(halfV * 2.f * 65536.f / (2.f * (float)M_PI));
	if (fov != wide)
		gameFov = fov;
	const u32 want = headset && vrFov > 0.f ? wide : gameFov;
	if (want != 0 && want != fov)
		writeU32(CAM_FOV, want);
}

void camhookPoll()
{
	if (!hookActive() || !ensureInstalled())
		return;
	const HeadSample head = sampleHead(inStage());
	widenFov(head.xrValid);
	writePose(0, head);
	writePose(1, sampleHead(inStage() && versus(), 1));	// neutral outside Versus
}

}	// namespace ht4q::eighteenwheeler
