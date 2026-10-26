/*
	HydroThunder4Quest: head-pose camera hook for Crazy Taxi (ReviveDC image, RDC-0102).

	The game's matrix library keeps the current matrix in the SH-4's back floating-point bank
	(XMTRX, as Sega's Ninja library does). The camera update (0x8c02693c) builds the view for the
	current camera mode in XMTRX and stores it into the camera record (CAM_VIEW) with njGetMatrix
	(0x0c076390), called through the literal CALL_LITERAL. That view is world -> camera with the
	camera looking down -z, y up, x right - OpenXR's axes - so the head pose applies directly:
	view' = H^-1 * view, H = the head's pose in camera space.

	We point CALL_LITERAL at STUB, which (when the block's enabled flag is set) copies the block's
	head id to usedId, multiplies XMTRX by the block's HINV with ftrv, and jumps on to njGetMatrix.
	The stub and its block live at the top of the IP.BIN area (0x8c00fe00..0x8c00ff90), past a
	library table at 0x8c00fc00 (at most 0x78 bytes): zero during play. Each poll we refresh HINV.
	The stub was assembled with a script (one column of the view per ftrv); see the listing.
*/
#include "crazytaxi.h"
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>

namespace ht4q::crazytaxi
{

static constexpr u32 CALL_LITERAL = 0x8c0287ac;	// njGetMatrix's address for the camera update's 3 stores
static constexpr u32 GET_MATRIX = 0x0c076390;	// njGetMatrix(dst): XMTRX -> dst
static constexpr u32 STUB = 0x8c00fe00;
static constexpr u32 BLOCK = STUB + 0x100;
static constexpr u32 AREA = 0x190;				// stub + block: must be zero before installing
static constexpr u32 CAM_MODE = 0x8c1775e0;		// camera record: mode (0 = driving)
// Projection: Ninja's projection matrix (0x8c2b0948) has the distance (641.89 px for 640x480) at
// PROJ_DIST, the same for x and y; but the rendered image is narrower: head yaw tests (frame dumps,
// every vertex before and after a 10 deg turn) fit 594 px horizontally (+-2) and 640 vertically, so
// something later squeezes x by ~0.925. Horizontal 57 deg, vertical 41 deg.
static constexpr u32 PROJ_DIST = 0x8c2b094c;
static constexpr float X_SQUEEZE = 0.925f;
// Field of view: while driving, the camera's projection is rebuilt every frame by a Ninja perspective
// function (PERSP, R4 = horizontal angle, 65536 per turn: about 60 deg at speed, distance 554). With a
// headset we redirect its entry to FOV_STUB, which replaces R4 with FOV_ANGLE when that is non-zero, so
// the image covers about the headset's field of view rather than a ~60 deg window. The stub sits below
// the camera stub (0x8c00fd00..0x8c00fd2c, zero during play).
static constexpr u32 PERSP = 0x8c0782e0;
static constexpr u32 FOV_STUB = 0x8c00fd00;
static constexpr u32 FOV_ANGLE = FOV_STUB + 0x28;
static constexpr u32 FOV_AREA = 0x2c;
static const u16 perspEntry[] {		// PERSP's first 6 instructions (saving R14..R9)
	0x2FE6, 0x2FD6, 0x2FC6, 0x2FB6, 0x2FA6, 0x2F96,
};
static const u16 perspJump[] {		// ... replaced with a jump to FOV_STUB
	0xD001,		// 00 mov.l @(L_STUB,PC),R0
	0x402B,		// 02 jmp @R0
	0x0009,		// 04 nop
	0x0009,		// 06 nop
	(u16)FOV_STUB, (u16)(FOV_STUB >> 16),	// 08 L_STUB
};
static const u16 fovStubCode[] {
	0x2FE6, 0x2FD6, 0x2FC6, 0x2FB6, 0x2FA6, 0x2F96,	// 00 PERSP's first 6 instructions
	0xD104,		// 0c mov.l @(L_ANGLE,PC),R1
	0x6112,		// 0e mov.l @R1,R1
	0x2118,		// 10 tst R1,R1
	0x8901,		// 12 bt back (+1)
	0x6413,		// 14 mov R1,R4   our angle
	0x0009,		// 16 nop
	0xD002,		// 18 back: mov.l @(L_BACK,PC),R0
	0x402B,		// 1a jmp @R0
	0x0009,		// 1c nop
	0x0009,		// 1e nop
	(u16)FOV_ANGLE, (u16)(FOV_ANGLE >> 16),			// 20 L_ANGLE
	(u16)(PERSP + 12), (u16)((PERSP + 12) >> 16),	// 24 L_BACK
};
static_assert(sizeof(perspJump) == 12 && sizeof(fovStubCode) == FOV_ANGLE - FOV_STUB, "fov stub layout");

static const u16 stubCode[] = {
	0xD137,		// 00 mov.l @(L_BLOCK,PC),R1  R1 = block
	0x6012,		// 02 mov.l @R1,R0   enabled
	0x2008,		// 04 tst R0,R0
	0x8968,		// 06 bt orig (+104)
	0x5011,		// 08 mov.l @(4,R1),R0  id
	0x1102,		// 0a mov.l R0,@(8,R1)  usedId = id
	0x6213,		// 0c mov R1,R2
	0x727F,		// 0e add #127,R2
	0x7211,		// 10 add #17,R2   R2 = block+0x90
	0xFBFD,		// 12 frchg
	0xF2FB,		// 14 fmov.s FR15,@-R2
	0xF2EB,		// 16 fmov.s FR14,@-R2
	0xF2DB,		// 18 fmov.s FR13,@-R2
	0xF2CB,		// 1a fmov.s FR12,@-R2
	0xF2BB,		// 1c fmov.s FR11,@-R2
	0xF2AB,		// 1e fmov.s FR10,@-R2
	0xF29B,		// 20 fmov.s FR9,@-R2
	0xF28B,		// 22 fmov.s FR8,@-R2
	0xF27B,		// 24 fmov.s FR7,@-R2
	0xF26B,		// 26 fmov.s FR6,@-R2
	0xF25B,		// 28 fmov.s FR5,@-R2
	0xF24B,		// 2a fmov.s FR4,@-R2
	0xF23B,		// 2c fmov.s FR3,@-R2
	0xF22B,		// 2e fmov.s FR2,@-R2
	0xF21B,		// 30 fmov.s FR1,@-R2
	0xF20B,		// 32 fmov.s FR0,@-R2
	0xFBFD,		// 34 frchg
	0x6213,		// 36 mov R1,R2
	0x7210,		// 38 add #16,R2
	0xFBFD,		// 3a frchg
	0xF029,		// 3c fmov.s @R2+,FR0
	0xF129,		// 3e fmov.s @R2+,FR1
	0xF229,		// 40 fmov.s @R2+,FR2
	0xF329,		// 42 fmov.s @R2+,FR3
	0xF429,		// 44 fmov.s @R2+,FR4
	0xF529,		// 46 fmov.s @R2+,FR5
	0xF629,		// 48 fmov.s @R2+,FR6
	0xF729,		// 4a fmov.s @R2+,FR7
	0xF829,		// 4c fmov.s @R2+,FR8
	0xF929,		// 4e fmov.s @R2+,FR9
	0xFA29,		// 50 fmov.s @R2+,FR10
	0xFB29,		// 52 fmov.s @R2+,FR11
	0xFC29,		// 54 fmov.s @R2+,FR12
	0xFD29,		// 56 fmov.s @R2+,FR13
	0xFE29,		// 58 fmov.s @R2+,FR14
	0xFF29,		// 5a fmov.s @R2+,FR15
	0xFBFD,		// 5c frchg
	0x6213,		// 5e mov R1,R2
	0x7250,		// 60 add #80,R2
	0xF029,		// 62 fmov.s @R2+,FR0
	0xF129,		// 64 fmov.s @R2+,FR1
	0xF229,		// 66 fmov.s @R2+,FR2
	0xF329,		// 68 fmov.s @R2+,FR3
	0xF1FD,		// 6a ftrv XMTRX,FV0
	0xF23B,		// 6c fmov.s FR3,@-R2
	0xF22B,		// 6e fmov.s FR2,@-R2
	0xF21B,		// 70 fmov.s FR1,@-R2
	0xF20B,		// 72 fmov.s FR0,@-R2
	0x7210,		// 74 add #16,R2
	0xF029,		// 76 fmov.s @R2+,FR0
	0xF129,		// 78 fmov.s @R2+,FR1
	0xF229,		// 7a fmov.s @R2+,FR2
	0xF329,		// 7c fmov.s @R2+,FR3
	0xF1FD,		// 7e ftrv XMTRX,FV0
	0xF23B,		// 80 fmov.s FR3,@-R2
	0xF22B,		// 82 fmov.s FR2,@-R2
	0xF21B,		// 84 fmov.s FR1,@-R2
	0xF20B,		// 86 fmov.s FR0,@-R2
	0x7210,		// 88 add #16,R2
	0xF029,		// 8a fmov.s @R2+,FR0
	0xF129,		// 8c fmov.s @R2+,FR1
	0xF229,		// 8e fmov.s @R2+,FR2
	0xF329,		// 90 fmov.s @R2+,FR3
	0xF1FD,		// 92 ftrv XMTRX,FV0
	0xF23B,		// 94 fmov.s FR3,@-R2
	0xF22B,		// 96 fmov.s FR2,@-R2
	0xF21B,		// 98 fmov.s FR1,@-R2
	0xF20B,		// 9a fmov.s FR0,@-R2
	0x7210,		// 9c add #16,R2
	0xF029,		// 9e fmov.s @R2+,FR0
	0xF129,		// a0 fmov.s @R2+,FR1
	0xF229,		// a2 fmov.s @R2+,FR2
	0xF329,		// a4 fmov.s @R2+,FR3
	0xF1FD,		// a6 ftrv XMTRX,FV0
	0xF23B,		// a8 fmov.s FR3,@-R2
	0xF22B,		// aa fmov.s FR2,@-R2
	0xF21B,		// ac fmov.s FR1,@-R2
	0xF20B,		// ae fmov.s FR0,@-R2
	0x7210,		// b0 add #16,R2
	0x6213,		// b2 mov R1,R2
	0x7250,		// b4 add #80,R2
	0xFBFD,		// b6 frchg
	0xF029,		// b8 fmov.s @R2+,FR0
	0xF129,		// ba fmov.s @R2+,FR1
	0xF229,		// bc fmov.s @R2+,FR2
	0xF329,		// be fmov.s @R2+,FR3
	0xF429,		// c0 fmov.s @R2+,FR4
	0xF529,		// c2 fmov.s @R2+,FR5
	0xF629,		// c4 fmov.s @R2+,FR6
	0xF729,		// c6 fmov.s @R2+,FR7
	0xF829,		// c8 fmov.s @R2+,FR8
	0xF929,		// ca fmov.s @R2+,FR9
	0xFA29,		// cc fmov.s @R2+,FR10
	0xFB29,		// ce fmov.s @R2+,FR11
	0xFC29,		// d0 fmov.s @R2+,FR12
	0xFD29,		// d2 fmov.s @R2+,FR13
	0xFE29,		// d4 fmov.s @R2+,FR14
	0xFF29,		// d6 fmov.s @R2+,FR15
	0xFBFD,		// d8 frchg
	0xD002,		// da mov.l @(L_ORIG,PC),R0  orig:
	0x402B,		// dc jmp @R0
	0x0009,		// de nop
	(u16)BLOCK, (u16)(BLOCK >> 16),	// L_BLOCK
	(u16)GET_MATRIX, (u16)(GET_MATRIX >> 16),	// L_ORIG
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
static_assert(sizeof(PoseBlock) + BLOCK - STUB <= AREA, "block layout");
static constexpr u32 USED_ID = BLOCK + offsetof(PoseBlock, usedId);

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
		WARN_LOG(COMMON, "ht4q crazytaxi: stub overwritten, hook removed");
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
	memcpy(ram(STUB), stubCode, sizeof(stubCode));
	writeU32(CALL_LITERAL, STUB);	// emu thread: dynarec invalidates the caller's block
	NOTICE_LOG(COMMON, "ht4q crazytaxi camhook installed");
	return true;
}

// With a headset, the angle that makes the image about stereoParams().vrFovDeg wide; 0 (the game's own)
// without one.
static void widenFov(bool headset)
{
	if (!installStub(FOV_STUB, FOV_AREA, fovStubCode, sizeof(fovStubCode)))
		return;
	if (memcmp(ram(PERSP), perspJump, sizeof(perspJump)) != 0)
	{
		if (memcmp(ram(PERSP), perspEntry, sizeof(perspEntry)) != 0)
			return;		// not the expected code
		memcpy(ram(PERSP), perspJump, sizeof(perspJump));	// emu thread: the dynarec invalidates it
	}
	const float vrFov = stereoParams().vrFovDeg;
	u32 angle = 0;
	if (headset && vrFov > 0.f)
	{
		// the projection distance is 320 / tan(angle / 2); the image is X_SQUEEZE narrower
		const float dist = 320.f / std::tan(vrFov * (float)M_PI / 360.f) / X_SQUEEZE;
		angle = (u32)std::lround(2.f * std::atan(320.f / dist) * 65536.f / (2.f * (float)M_PI));
	}
	writeU32(FOV_ANGLE, angle);
}

bool driving()
{
	return readU32(CAM_MODE) == 0;
}

u32 startRender()
{
	if (!hookActive() || readU32(CALL_LITERAL) != STUB)
		return 0;
	return readU32(USED_ID);
}

void camhookPoll()
{
	if (!hookActive() || !ensureInstalled())
		return;
	const HeadSample head = sampleHead(driving());
	widenFov(head.xrValid);
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
	float dist = readF32(PROJ_DIST);
	if (!(dist > 100.f && dist < 5000.f))
		dist = 641.89f;
	fp.zscreen = dist * X_SQUEEZE;
	fp.zscreenY = dist;
	pb.id = storeFramePose(fp);
	pb.usedId = readU32(USED_ID);
	memcpy(ram(BLOCK), &pb, offsetof(PoseBlock, temp));
}

}	// namespace ht4q::crazytaxi
