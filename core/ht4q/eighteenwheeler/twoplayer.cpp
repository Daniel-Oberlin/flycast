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
	256-458), and its polygons get player 2's tile clip (rows 480+), so it would never show. The race
	clock and progress bar between the halves (rows 196-290) are drawn once, with player 1's clip.
	All are translucent; fixHud, before the pass is sorted, moves player 2's HUD down 224 rows
	(256 -> 480) and adds a copy of the clock and bar for player 2 (480 rows down, its clip).
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

static constexpr float BAND_TOP = 196.f, BAND_BOTTOM = 290.f;	// the clock and progress bar

static bool allVerts(const rend_context& ctx, const PolyParam& pp, bool (*pred)(const Vertex&))
{
	for (u32 j = pp.first; j < pp.first + pp.count; j++)
		if (!pred(ctx.verts[j]))
			return false;
	return true;
}

void fixHud(rend_context& ctx, u32 first, u32& end)
{
	if (!ht4q::twoPlayerFull() || !versus())
		return;
	const u32 player2Row = (u32)PLAYER2_SHIFT / 32;
	const bool canAdd = end == ctx.global_param_tr.size();
	const u32 stop = end;
	for (u32 i = first; i < stop && i < ctx.global_param_tr.size(); i++)
	{
		PolyParam& pp = ctx.global_param_tr[i];
		if ((pp.tileclip >> 28) < 2 || pp.count == 0 || pp.first + pp.count > ctx.verts.size())
			continue;
		const u32 clipTop = (pp.tileclip >> 12) & 31;
		if (clipTop >= player2Row)
		{
			// player 2's HUD, still in the split layout's rows
			if (allVerts(ctx, pp, [](const Vertex& v) { return v.z >= HUD_Z && v.y < PLAYER2_SHIFT; }))
				for (u32 j = pp.first; j < pp.first + pp.count; j++)
					ctx.verts[j].y += PLAYER2_SHIFT - SPLIT_TOP;
		}
		else if (clipTop == 0 && canAdd
				&& allVerts(ctx, pp, [](const Vertex& v) { return v.z >= HUD_Z && v.y >= BAND_TOP && v.y <= BAND_BOTTOM; }))
		{
			// the clock and progress bar: a copy for player 2
			PolyParam copy = pp;
			copy.first = (u32)ctx.verts.size();
			for (u32 j = pp.first; j < pp.first + pp.count; j++)
			{
				Vertex v = ctx.verts[j];
				v.y += PLAYER2_SHIFT;
				ctx.verts.push_back(v);
			}
			const u32 rows = (pp.tileclip >> 17 & 31) - clipTop;
			copy.tileclip = (pp.tileclip & ~(31u << 12 | 31u << 17)) | player2Row << 12 | (player2Row + rows) << 17;
			ctx.global_param_tr.push_back(copy);
			end++;
		}
	}
}

}	// namespace ht4q::eighteenwheeler
