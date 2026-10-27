/*
	HydroThunder4Quest: 18 Wheeler's sky with head tracking.

	The game draws its sky as a dome around the camera with a matrix of its own (built every frame
	from the camera's pitch, at 0x8c3c4700), not from the view our camera hook changes, so the sky
	turned with the head. Its polygons are the farthest in the frame (1/w about 0.0008; the farthest
	scenery is above 0.002) and near-plane clipped by the game, so every vertex is a real point in
	camera space. Before the frame is drawn we turn them by the head rotation the view was built with:
	camera space (x right, y up, z back) from the screen position and 1/w, into the head's frame, and
	back to the screen. Turning can take part of a triangle behind the eye, which the renderer can't
	clip (its positions are already divided through), so each triangle is clipped against a near plane
	here and the sky polygon rebuilt from the pieces. The sky is far enough that the head's position
	doesn't matter. The flat backdrop behind everything (one screen-sized quad, 1/w ~1e-5) stays put.
*/
#include "eighteenwheeler.h"
#include "hw/pvr/ta_ctx.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace ht4q::eighteenwheeler
{

static constexpr float SKY_Z = 0.0012f;		// 1/w: sky polygons reach below this, scenery doesn't
static constexpr float BACKDROP_Z = 1e-4f;	// ... and the backdrop is below this
static constexpr float NEAR = 0.3f;			// distance the turned triangles are clipped at (the game's near plane)
static constexpr u32 RESTART = ~0u;

static bool isSky(const rend_context& ctx, const PolyParam& pp)
{
	if (pp.count == 0 || pp.first + pp.count > ctx.idx.size())
		return false;
	bool sky = false;
	for (u32 j = pp.first; j < pp.first + pp.count; j++)
	{
		const u32 k = ctx.idx[j];
		if (k >= ctx.verts.size())
			continue;
		if (ctx.verts[k].z < BACKDROP_Z)
			return false;
		sky |= ctx.verts[k].z < SKY_Z;
	}
	return sky;
}

namespace {

struct ClipVertex {
	float h[3];		// head space
	Vertex v;		// attributes (position recomputed from h)
};

static ClipVertex lerp(const ClipVertex& a, const ClipVertex& b, float t)
{
	ClipVertex r = a;
	for (int i = 0; i < 3; i++)
		r.h[i] = a.h[i] + (b.h[i] - a.h[i]) * t;
	auto lu8 = [t](u8 x, u8 y) { return (u8)std::lround(x + (y - x) * t); };
	for (int i = 0; i < 4; i++)
	{
		r.v.col[i] = lu8(a.v.col[i], b.v.col[i]);
		r.v.spc[i] = lu8(a.v.spc[i], b.v.spc[i]);
		r.v.col1[i] = lu8(a.v.col1[i], b.v.col1[i]);
		r.v.spc1[i] = lu8(a.v.spc1[i], b.v.spc1[i]);
	}
	r.v.u = a.v.u + (b.v.u - a.v.u) * t;
	r.v.v = a.v.v + (b.v.v - a.v.v) * t;
	r.v.u1 = a.v.u1 + (b.v.u1 - a.v.u1) * t;
	r.v.v1 = a.v.v1 + (b.v.v1 - a.v.v1) * t;
	return r;
}

// Builds one strip of independent triangles, joined by degenerate ones (with or without primitive
// restart), each starting at an even position so its winding is kept.
struct StripBuilder {
	rend_context& ctx;
	float f, fy, cy;
	std::vector<u32> strip;

	u32 add(const ClipVertex& c)
	{
		Vertex v = c.v;
		const float w = -c.h[2];
		v.x = 320.f + c.h[0] * f / w;
		v.y = cy - c.h[1] * fy / w;
		v.z = 1.f / w;
		ctx.verts.push_back(v);
		return (u32)ctx.verts.size() - 1;
	}
	void triangle(u32 a, u32 b, u32 c)
	{
		if (!strip.empty())
		{
			strip.push_back(strip.back());
			strip.push_back(a);
			if (strip.size() % 2 == 1)
				strip.push_back(a);	// so the triangle's a lands on an even position
		}
		strip.push_back(a);
		strip.push_back(b);
		strip.push_back(c);
	}
	// a convex polygon (the clipped triangle), in its winding order
	void polygon(const std::vector<ClipVertex>& poly)
	{
		if (poly.size() < 3)
			return;
		std::vector<u32> ids;
		for (const ClipVertex& c : poly)
			ids.push_back(add(c));
		for (size_t i = 1; i + 1 < ids.size(); i++)
			triangle(ids[0], ids[i], ids[i + 1]);
	}
};

}	// namespace

static std::vector<ClipVertex> clipNear(const ClipVertex (&tri)[3])
{
	std::vector<ClipVertex> out;
	for (int i = 0; i < 3; i++)
	{
		const ClipVertex& a = tri[i];
		const ClipVertex& b = tri[(i + 1) % 3];
		const float da = -a.h[2] - NEAR, db = -b.h[2] - NEAR;
		if (da >= 0.f)
			out.push_back(a);
		if ((da >= 0.f) != (db >= 0.f))
			out.push_back(lerp(a, b, da / (da - db)));
	}
	return out;
}

void fixSky(rend_context& ctx)
{
	FramePose fp[2];
	const bool have[2] { viewPose(0, fp[0]), versus() && viewPose(1, fp[1]) };
	if (!have[0] && !have[1])
		return;
	const u32 player2Row = (u32)PLAYER2_SHIFT / 32;
	const size_t origVerts = ctx.verts.size();
	for (PolyParam& pp : ctx.global_param_op)
	{
		if (!isSky(ctx, pp))
			continue;
		const int player = ht4q::twoPlayerFull() && ((pp.tileclip >> 12) & 31) >= player2Row ? 1 : 0;
		if (!have[player])
			continue;
		const FramePose& p = fp[player];
		StripBuilder sb { ctx, p.zscreen, p.zscreenY > 0.f ? p.zscreenY : p.zscreen,
			player == 1 ? 240.f + PLAYER2_SHIFT : 240.f, {} };
		const float *r = p.headRot;	// head -> camera, row-major
		auto toHead = [&](u32 k) {
			ClipVertex c;
			c.v = ctx.verts[k];
			const float w = 1.f / c.v.z;
			const float cam[3] { (c.v.x - 320.f) * w / sb.f, (sb.cy - c.v.y) * w / sb.fy, -w };
			for (int i = 0; i < 3; i++)	// R^T cam
				c.h[i] = r[i] * cam[0] + r[3 + i] * cam[1] + r[6 + i] * cam[2];
			return c;
		};
		// the polygon's strips (split at primitive restarts), one triangle at a time
		u32 runStart = pp.first;
		const u32 end = pp.first + pp.count;
		for (u32 j = pp.first; j <= end; j++)
		{
			if (j < end && ctx.idx[j] != RESTART)
				continue;
			for (u32 t = runStart; t + 2 < j; t++)
			{
				u32 a = ctx.idx[t], b = ctx.idx[t + 1], c = ctx.idx[t + 2];
				if (a == b || b == c || a == c || std::max({ a, b, c }) >= origVerts)
					continue;	// degenerate (strip joins)
				if ((t - runStart) % 2 == 1)
					std::swap(a, b);	// odd triangles of a strip are wound the other way
				if (ctx.verts[a].z <= 0.f || ctx.verts[b].z <= 0.f || ctx.verts[c].z <= 0.f)
					continue;
				const ClipVertex tri[3] { toHead(a), toHead(b), toHead(c) };
				sb.polygon(clipNear(tri));
			}
			runStart = j + 1;
		}
		pp.first = (u32)ctx.idx.size();
		pp.count = (u32)sb.strip.size();
		ctx.idx.insert(ctx.idx.end(), sb.strip.begin(), sb.strip.end());
	}
}

}	// namespace ht4q::eighteenwheeler
