/*
	HydroThunder4Quest: sun and lens flare depth.

	The game draws the sun and its flare as translucent screen-space quads at 1/w = 0.5, in front
	of everything, and decides by itself when the sun is visible - wrongly in some tunnels (the
	unmodified game does the same). During races we move them to just in front of the sky
	(1/w 8e-6..2e-5) instead: scenery then hides them per pixel, translucent haze sorts in front
	of them, and stereo puts them at infinity.

	Also hides the game's pause box while the view is being adjusted (hidePauseBox).
*/
#include "hydrothunder.h"
#include "hw/pvr/ta_ctx.h"
#include <cstdlib>

namespace ht4q::hydrothunder
{

static constexpr float GAME_SUN_Z = 0.5f;
static constexpr float SUN_Z = 3e-5f;

// While the player adjusts the view (controls.cpp), the game shows its pause screen: a box in the
// middle (1/w 5) with text on it (1/w 10). Polygons entirely in front at that spot are moved off
// screen so the view stays clear.
static void hidePauseBox(rend_context& ctx, u32 first, u32 end)
{
	for (u32 i = first; i < end && i < ctx.global_param_tr.size(); i++)
	{
		const PolyParam& pp = ctx.global_param_tr[i];
		if (pp.count == 0 || pp.first + pp.count > ctx.verts.size())
			continue;
		bool box = true;
		for (u32 j = pp.first; j < pp.first + pp.count && box; j++)
		{
			const Vertex& v = ctx.verts[j];
			box = v.z >= 4.5f && v.x >= 158.f && v.x <= 483.f && v.y >= 188.f && v.y <= 303.f;
		}
		if (box)
			for (u32 j = pp.first; j < pp.first + pp.count; j++)
				ctx.verts[j].x = ctx.verts[j].y = -1000.f;
	}
}

void fixSunDepth(rend_context& ctx, u32 first, u32 end)
{
	// HT4Q_SUNZ: 1/w to draw them at instead (0.5 = as the game does, for comparing)
	static const float sunZ = getenv("HT4Q_SUNZ") ? (float)atof(getenv("HT4Q_SUNZ")) : SUN_Z;
	FramePose fp;
	if (!framePose(ctx.ht4qPoseId, fp))
		return;
	for (u32 i = first; i < end && i < ctx.global_param_tr.size(); i++)
	{
		const PolyParam& pp = ctx.global_param_tr[i];
		if (pp.count == 0 || pp.first + pp.count > ctx.verts.size())
			continue;
		bool sun = true;
		for (u32 j = pp.first; j < pp.first + pp.count && sun; j++)
			sun = ctx.verts[j].z == GAME_SUN_Z;
		if (sun)
			for (u32 j = pp.first; j < pp.first + pp.count; j++)
				ctx.verts[j].z = sunZ;
	}
	if (adjustingView())
		hidePauseBox(ctx, first, end);
}

}	// namespace ht4q::hydrothunder
