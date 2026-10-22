/*
	HydroThunder4Quest: sun and lens flare.

	The game draws the sun and its flare as translucent screen-space quads at 1/w = 0.5, in front
	of everything, and decides by itself when the sun is visible - wrongly in some tunnels (the
	unmodified game does the same). During races we move them to just in front of the sky
	(1/w 8e-6..2e-5) instead: scenery then hides them per pixel, translucent haze sorts in front
	of them, and stereo puts them at infinity.

	The sun disc itself is a projected world point, so it stays put in the headset. The rest are
	lens effects tied to the screen: a large glare centred on it and flare ghosts on the line from
	the sun through its centre. In the headset those follow the head, which reads as the sun moving,
	so in the immersive view only the disc is kept: of the sun sprites, the one farthest from the
	screen centre (the ghosts lie between the sun and the centre, the glare on it).
*/
#include "ht4q.h"
#include "hw/pvr/ta_ctx.h"
#include <cstdlib>
#include <vector>

namespace ht4q
{

static constexpr float GAME_SUN_Z = 0.5f;
static constexpr float SUN_Z = 3e-5f;

void fixSunDepth(rend_context& ctx, u32 first, u32 end)
{
	// HT4Q_SUNZ: 1/w to draw them at instead (0.5 = as the game does, for comparing)
	static const float sunZ = getenv("HT4Q_SUNZ") ? (float)atof(getenv("HT4Q_SUNZ")) : SUN_Z;
	// HT4Q_SUNFLARE: 0 = disc only, 1 = everything; default: disc only in the immersive view
	static const char *flareEnv = getenv("HT4Q_SUNFLARE");
	FramePose fp;
	if (!framePose(ctx.ht4qPoseId, fp))
		return;
	const bool discOnly = flareEnv != nullptr ? flareEnv[0] == '0' : fp.immersive;
	std::vector<u32> suns;
	for (u32 i = first; i < end && i < ctx.global_param_tr.size(); i++)
	{
		const PolyParam& pp = ctx.global_param_tr[i];
		if (pp.count == 0 || pp.first + pp.count > ctx.verts.size())
			continue;
		bool sun = true;
		for (u32 j = pp.first; j < pp.first + pp.count && sun; j++)
			sun = ctx.verts[j].z == GAME_SUN_Z;
		if (!sun)
			continue;
		for (u32 j = pp.first; j < pp.first + pp.count; j++)
			ctx.verts[j].z = sunZ;
		suns.push_back(i);
	}
	if (!discOnly)
		return;
	// the polygons come as separate strips: group consecutive ones with the same texture into sprites
	struct Sprite { size_t begin, end; float off; };
	std::vector<Sprite> sprites;
	for (size_t k = 0; k < suns.size(); k++)
	{
		const PolyParam& pp = ctx.global_param_tr[suns[k]];
		if (sprites.empty() || suns[k] != suns[k - 1] + 1 || pp.tcw.full != ctx.global_param_tr[suns[k - 1]].tcw.full)
			sprites.push_back({ k, k, 0.f });
		sprites.back().end = k + 1;
	}
	if (sprites.size() < 2)
		return;
	for (Sprite& sp : sprites)
	{
		float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
		for (size_t k = sp.begin; k < sp.end; k++)
		{
			const PolyParam& pp = ctx.global_param_tr[suns[k]];
			for (u32 j = pp.first; j < pp.first + pp.count; j++)
			{
				x0 = std::min(x0, ctx.verts[j].x); x1 = std::max(x1, ctx.verts[j].x);
				y0 = std::min(y0, ctx.verts[j].y); y1 = std::max(y1, ctx.verts[j].y);
			}
		}
		const float dx = (x0 + x1) / 2 - 320.f, dy = (y0 + y1) / 2 - 240.f;
		sp.off = dx * dx + dy * dy;
	}
	const Sprite *disc = &sprites[0];
	for (const Sprite& sp : sprites)
		if (sp.off > disc->off)
			disc = &sp;
	for (const Sprite& sp : sprites)
		if (&sp != disc)
			for (size_t k = sp.begin; k < sp.end; k++)
			{
				const PolyParam& pp = ctx.global_param_tr[suns[k]];
				for (u32 j = pp.first; j < pp.first + pp.count; j++)
					ctx.verts[j].x = ctx.verts[j].y = -1000.f;	// degenerate, off screen
			}
}

}	// namespace ht4q
