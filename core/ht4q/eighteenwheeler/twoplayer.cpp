/*
	HydroThunder4Quest: 18 Wheeler's Versus with a full screen per player (core/ht4q/twoplayer.cpp).

	Versus splits the screen top/bottom with a band between (the race clock and the progress bar,
	drawn over the whole 640x480 screen). Each player's camera gets its viewport from a table
	(VIEWPORTS: x, y, width, height, ?, -230 per entry; entry 2 is the 1-player full screen
	32, 0, 576, 480) when the stage starts: player 1 (32, 0, 576, 224), player 2 (32, 256, 576, 224).
	The projection follows the viewport (aspect width / height), so a 480-high viewport gives the
	1-player view (578.8 px, 58 x 45 deg). The players' HUDs are laid out in a 576 x 224 virtual
	screen scaled to the viewport (the literals at HUD_HEIGHT), so they are made 480 high too - the
	HUD keeps its split-screen size, in the top part of player 1's view, the lower part of player 2's.
	The patches give player 1 (32, 0, 576, 480) and player 2 (32, 480, 576, 480): its view below the
	screen, PLAYER2_SHIFT px down. They take effect when a stage starts.
	Player 2's HUD doesn't follow its viewport: its positions carry the split screen's offset (rows
	256-458), and its polygons get player 2's tile clip (rows 480+), so it would never show. They are
	all translucent; movePlayer2Hud moves them down 224 rows (256 -> 480) before the pass is sorted.
*/
#include "eighteenwheeler.h"
#include "hw/pvr/ta_ctx.h"

namespace ht4q::eighteenwheeler
{

static constexpr u32 VIEWPORTS = 0x8c1e9000;	// float[6] per entry
static const FloatPatch patches[] {
	{ VIEWPORTS + 12, 224.f, 480.f },			// player 1's height
	{ VIEWPORTS + 0x18 + 4, 256.f, 480.f },		// player 2's top
	{ VIEWPORTS + 0x18 + 12, 224.f, 480.f },	// player 2's height
	{ 0x8c06c85c, 224.f, 480.f },				// HUD_HEIGHT: the players' HUD layout height
	{ 0x8c06c6fc, 224.f, 480.f },
};

bool twoPlayerFull(bool on)
{
	return applyFloatPatches(patches, std::size(patches), on);
}

static constexpr float HUD_Z = 3.25f;		// 1/w: the HUD is drawn at 3.33, the world below ~3.2
static constexpr float SPLIT_TOP = 256.f;	// player 2's HUD rows in the split layout

void movePlayer2Hud(rend_context& ctx, u32 first, u32 end)
{
	if (!ht4q::twoPlayerFull() || !versus())
		return;
	const float down = PLAYER2_SHIFT - SPLIT_TOP;
	const u32 player2Row = (u32)PLAYER2_SHIFT / 32;
	for (u32 i = first; i < end && i < ctx.global_param_tr.size(); i++)
	{
		const PolyParam& pp = ctx.global_param_tr[i];
		if ((pp.tileclip >> 28) < 2 || ((pp.tileclip >> 12) & 31) < player2Row)
			continue;	// not clipped to player 2's rows
		if (pp.count == 0 || pp.first + pp.count > ctx.verts.size())
			continue;
		bool hud = true;
		for (u32 j = pp.first; j < pp.first + pp.count && hud; j++)
			hud = ctx.verts[j].z >= HUD_Z && ctx.verts[j].y < PLAYER2_SHIFT;
		if (hud)
			for (u32 j = pp.first; j < pp.first + pp.count; j++)
				ctx.verts[j].y += down;
	}
}

}	// namespace ht4q::eighteenwheeler
