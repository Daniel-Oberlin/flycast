/*
	HydroThunder4Quest: 2-player races with a full screen per player (option ht4q.TwoPlayerFull).

	How the game splits the screen (retail Rev 1): with two humans, its gameloop (0x8c073710) draws
	each player in turn, first calling the viewport setter 0x8c098394(height, top), which writes
	dcViewportYMin/YMax/YTrans (0x8c1b05b8...) and sets the PVR user tile clip to those rows:
	player 1 (256, 0), player 2 (224, 256). Each view is a crop of a normal 640x480 projection
	(same zscreen), centered 32 px higher for player 2. The HUD of player 2 is drawn 200 px lower
	(dcOrthoYOffset).
	The patches give both players a full 640x480 view, player 2's below the screen (rows 480-959,
	tile rows 15-29): heights 256 and 224 -> 480, top of player 2 = 480 (the same literal), no
	32 px centering, HUD offset 480. The renderer then draws the frame once per player, moved up
	480 px for player 2 (ht4q_stereo.cpp); the tile clips keep the other player's polygons out.
*/
#include "ht4q.h"
#include "cfg/option.h"
#include "emulator.h"
#include "hw/sh4/sh4_mem.h"
#include <cstring>

namespace ht4q
{

static config::Option<bool, false> TwoPlayerFull("TwoPlayerFull", false, "ht4q");

static constexpr u32 SPLIT_HEIGHT_P1 = 0x8c07388c;	// literal: player 1's height, also player 2's top
static constexpr u32 SPLIT_HEIGHT_P2 = 0x8c0738ac;	// literal: player 2's height
static constexpr u32 SPLIT_CENTER = 0x8c098484;		// literal: YTrans adjustment when top > 0
static constexpr u32 SPLIT_HUD_Y = 0x8c073f30;		// literal: player 2's HUD offset

struct Patch { u32 addr; float stock, full; };
static const Patch patches[] {
	{ SPLIT_HEIGHT_P1, 256.f, 480.f },
	{ SPLIT_HEIGHT_P2, 224.f, 480.f },
	{ SPLIT_CENTER, -32.f, 0.f },
	{ SPLIT_HUD_Y, 200.f, 480.f },
};

static u8 *ram(u32 addr) { return &mem_b[addr & RAM_MASK]; }

bool twoPlayerFull() { return TwoPlayerFull; }
void setTwoPlayerFull(bool on) { TwoPlayerFull.set(on); }

void twoPlayerPoll()
{
	if (settings.content.gameId != "T-9702N")
		return;
	static bool applied;
	const bool on = TwoPlayerFull;
	if (on == applied)
		return;
	// all or nothing: only when every literal holds the value we expect
	for (const Patch& p : patches)
	{
		float v;
		memcpy(&v, ram(p.addr), 4);
		if (v != p.stock && v != p.full)
			return;		// not the expected code (other build, or not loaded yet)
	}
	for (const Patch& p : patches)
		memcpy(ram(p.addr), on ? &p.full : &p.stock, 4);	// emu thread: like the game's own writes
	NOTICE_LOG(COMMON, "ht4q: 2-player %s", on ? "full screen" : "split screen");
	applied = on;
}

}	// namespace ht4q
