/*
	HydroThunder4Quest: head-pose camera hook for 18 Wheeler: American Pro Trucker (ReviveDC image,
	RDC-0094).

	The game's matrix library keeps the current matrix in the SH-4's back floating-point bank
	(XMTRX). The camera update (0x8c08dc0e) builds a player's view in XMTRX from the camera's eye,
	target and angles, then stores it into that player's slot of the view table (VIEW_TABLE, 64 bytes
	each) with the library's get-matrix (0x0c156060: XMTRX -> R4), called through CALL_LITERAL. That
	view is world -> camera with the camera looking down -z, y up, x right - OpenXR's axes - so the
	head pose applies directly: view' = H^-1 * view, H = the head's pose in camera space.

	We point CALL_LITERAL at STUB, which (when R4 is the main view, slot 0, and the block's enabled
	flag is set) copies the block's head id to usedId, multiplies XMTRX by the block's HINV with ftrv,
	and jumps on to get-matrix. Other views (the rear-view mirror is a separate camera) pass through.
	The stub and its block live at the top of the IP.BIN area (0x8c00fe00..0x8c00ff90), zero during
	play. Each poll we refresh HINV. Assembled with a script (one column of the view per ftrv).
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
static constexpr u32 STUB = 0x8c00fe00;
static constexpr u32 BLOCK = STUB + 0x100;
static constexpr u32 AREA = 0x190;				// stub + block: must be zero before installing
// Projection: the library's projection matrix (0x8c39eed8) has the distance at PROJ_DIST, the same
// for x and y (578.78 for 640x480: 58 x 45 deg). Head yaw/pitch tests (frame dumps, every vertex
// before and after a 10 deg turn) fit 578.8 both ways; 1/w in the TA = 1/distance.
static constexpr u32 PROJ_DIST = 0x8c39eedc;

static const u16 stubCode[] = {
0xD03B,		// 00 mov.l @(L_VIEW,PC),R0  R0 = main view
	0x3040,		// 02 cmp/eq R4,R0
	0x8B6C,		// 04 bf orig (+108)
	0xD138,		// 06 mov.l @(L_BLOCK,PC),R1  R1 = block
	0x6012,		// 08 mov.l @R1,R0   enabled
	0x2008,		// 0a tst R0,R0
	0x8968,		// 0c bt orig (+104)
	0x5011,		// 0e mov.l @(4,R1),R0  id
	0x1102,		// 10 mov.l R0,@(8,R1)  usedId = id
	0x6213,		// 12 mov R1,R2
	0x727F,		// 14 add #127,R2
	0x7211,		// 16 add #17,R2   R2 = block+0x90
	0xFBFD,		// 18 frchg
	0xF2FB,		// 1a fmov.s FR15,@-R2
	0xF2EB,		// 1c fmov.s FR14,@-R2
	0xF2DB,		// 1e fmov.s FR13,@-R2
	0xF2CB,		// 20 fmov.s FR12,@-R2
	0xF2BB,		// 22 fmov.s FR11,@-R2
	0xF2AB,		// 24 fmov.s FR10,@-R2
	0xF29B,		// 26 fmov.s FR9,@-R2
	0xF28B,		// 28 fmov.s FR8,@-R2
	0xF27B,		// 2a fmov.s FR7,@-R2
	0xF26B,		// 2c fmov.s FR6,@-R2
	0xF25B,		// 2e fmov.s FR5,@-R2
	0xF24B,		// 30 fmov.s FR4,@-R2
	0xF23B,		// 32 fmov.s FR3,@-R2
	0xF22B,		// 34 fmov.s FR2,@-R2
	0xF21B,		// 36 fmov.s FR1,@-R2
	0xF20B,		// 38 fmov.s FR0,@-R2
	0xFBFD,		// 3a frchg
	0x6213,		// 3c mov R1,R2
	0x7210,		// 3e add #16,R2
	0xFBFD,		// 40 frchg
	0xF029,		// 42 fmov.s @R2+,FR0
	0xF129,		// 44 fmov.s @R2+,FR1
	0xF229,		// 46 fmov.s @R2+,FR2
	0xF329,		// 48 fmov.s @R2+,FR3
	0xF429,		// 4a fmov.s @R2+,FR4
	0xF529,		// 4c fmov.s @R2+,FR5
	0xF629,		// 4e fmov.s @R2+,FR6
	0xF729,		// 50 fmov.s @R2+,FR7
	0xF829,		// 52 fmov.s @R2+,FR8
	0xF929,		// 54 fmov.s @R2+,FR9
	0xFA29,		// 56 fmov.s @R2+,FR10
	0xFB29,		// 58 fmov.s @R2+,FR11
	0xFC29,		// 5a fmov.s @R2+,FR12
	0xFD29,		// 5c fmov.s @R2+,FR13
	0xFE29,		// 5e fmov.s @R2+,FR14
	0xFF29,		// 60 fmov.s @R2+,FR15
	0xFBFD,		// 62 frchg
	0x6213,		// 64 mov R1,R2
	0x7250,		// 66 add #80,R2
	0xF029,		// 68 fmov.s @R2+,FR0
	0xF129,		// 6a fmov.s @R2+,FR1
	0xF229,		// 6c fmov.s @R2+,FR2
	0xF329,		// 6e fmov.s @R2+,FR3
	0xF1FD,		// 70 ftrv XMTRX,FV0
	0xF23B,		// 72 fmov.s FR3,@-R2
	0xF22B,		// 74 fmov.s FR2,@-R2
	0xF21B,		// 76 fmov.s FR1,@-R2
	0xF20B,		// 78 fmov.s FR0,@-R2
	0x7210,		// 7a add #16,R2
	0xF029,		// 7c fmov.s @R2+,FR0
	0xF129,		// 7e fmov.s @R2+,FR1
	0xF229,		// 80 fmov.s @R2+,FR2
	0xF329,		// 82 fmov.s @R2+,FR3
	0xF1FD,		// 84 ftrv XMTRX,FV0
	0xF23B,		// 86 fmov.s FR3,@-R2
	0xF22B,		// 88 fmov.s FR2,@-R2
	0xF21B,		// 8a fmov.s FR1,@-R2
	0xF20B,		// 8c fmov.s FR0,@-R2
	0x7210,		// 8e add #16,R2
	0xF029,		// 90 fmov.s @R2+,FR0
	0xF129,		// 92 fmov.s @R2+,FR1
	0xF229,		// 94 fmov.s @R2+,FR2
	0xF329,		// 96 fmov.s @R2+,FR3
	0xF1FD,		// 98 ftrv XMTRX,FV0
	0xF23B,		// 9a fmov.s FR3,@-R2
	0xF22B,		// 9c fmov.s FR2,@-R2
	0xF21B,		// 9e fmov.s FR1,@-R2
	0xF20B,		// a0 fmov.s FR0,@-R2
	0x7210,		// a2 add #16,R2
	0xF029,		// a4 fmov.s @R2+,FR0
	0xF129,		// a6 fmov.s @R2+,FR1
	0xF229,		// a8 fmov.s @R2+,FR2
	0xF329,		// aa fmov.s @R2+,FR3
	0xF1FD,		// ac ftrv XMTRX,FV0
	0xF23B,		// ae fmov.s FR3,@-R2
	0xF22B,		// b0 fmov.s FR2,@-R2
	0xF21B,		// b2 fmov.s FR1,@-R2
	0xF20B,		// b4 fmov.s FR0,@-R2
	0x7210,		// b6 add #16,R2
	0x6213,		// b8 mov R1,R2
	0x7250,		// ba add #80,R2
	0xFBFD,		// bc frchg
	0xF029,		// be fmov.s @R2+,FR0
	0xF129,		// c0 fmov.s @R2+,FR1
	0xF229,		// c2 fmov.s @R2+,FR2
	0xF329,		// c4 fmov.s @R2+,FR3
	0xF429,		// c6 fmov.s @R2+,FR4
	0xF529,		// c8 fmov.s @R2+,FR5
	0xF629,		// ca fmov.s @R2+,FR6
	0xF729,		// cc fmov.s @R2+,FR7
	0xF829,		// ce fmov.s @R2+,FR8
	0xF929,		// d0 fmov.s @R2+,FR9
	0xFA29,		// d2 fmov.s @R2+,FR10
	0xFB29,		// d4 fmov.s @R2+,FR11
	0xFC29,		// d6 fmov.s @R2+,FR12
	0xFD29,		// d8 fmov.s @R2+,FR13
	0xFE29,		// da fmov.s @R2+,FR14
	0xFF29,		// dc fmov.s @R2+,FR15
	0xFBFD,		// de frchg
	0xD002,		// e0 mov.l @(L_ORIG,PC),R0  orig:
	0x402B,		// e2 jmp @R0
	0x0009,		// e4 nop
	0x0009,		// e6 pad
	(u16)BLOCK, (u16)(BLOCK >> 16),	// L_BLOCK
	(u16)GET_MATRIX, (u16)(GET_MATRIX >> 16),	// L_ORIG
	(u16)VIEW_TABLE, (u16)(VIEW_TABLE >> 16),	// L_VIEW
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
	memcpy(ram(STUB), stubCode, sizeof(stubCode));
	writeU32(CALL_LITERAL, STUB);	// emu thread: dynarec invalidates the caller's block
	NOTICE_LOG(COMMON, "ht4q 18wheeler camhook installed");
	return true;
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
	const HeadSample head = sampleHead(inStage());
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
		dist = 578.78f;
	fp.zscreen = dist;
	fp.zscreenY = dist;
	pb.id = storeFramePose(fp);
	pb.usedId = readU32(USED_ID);
	memcpy(ram(BLOCK), &pb, offsetof(PoseBlock, temp));
}

}	// namespace ht4q::eighteenwheeler
