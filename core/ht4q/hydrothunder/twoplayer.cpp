/*
	HydroThunder4Quest: Hydro Thunder's 2-player races with a full screen per player (core/ht4q/twoplayer.cpp).

	How the game splits the screen (retail Rev 1): with two humans, its gameloop (0x8c073710) draws
	each player in turn, first calling the viewport setter 0x8c098394(height, top), which writes
	dcViewportYMin/YMax/YTrans (0x8c1b05b8...) and sets the PVR user tile clip to those rows:
	player 1 (256, 0), player 2 (224, 256). Each view is a crop of a normal 640x480 projection
	(same zscreen), centered 32 px higher for player 2. The HUD of player 2 is drawn 200 px lower
	(dcOrthoYOffset).
	The patches give both players a full 640x480 view, player 2's below the screen (rows 480-959,
	tile rows 15-29): heights 256 and 224 -> 480, top of player 2 = 480 (the same literal), no
	32 px centering, HUD offset 480.
*/
#include "hydrothunder.h"

namespace ht4q::hydrothunder
{

static const FloatPatch patches[] {
	{ 0x8c07388c, 256.f, 480.f },	// player 1's height, also player 2's top
	{ 0x8c0738ac, 224.f, 480.f },	// player 2's height
	{ 0x8c098484, -32.f, 0.f },		// YTrans adjustment when top > 0
	{ 0x8c073f30, 200.f, 480.f },	// player 2's HUD offset
};

bool twoPlayerFull(bool on)
{
	return applyFloatPatches(patches, std::size(patches), on);
}

}	// namespace ht4q::hydrothunder
