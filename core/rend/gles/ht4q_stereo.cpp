/*
	HydroThunder4Quest: stereo rendering for the OpenGL renderer.

	The PVR hands us screen-space x,y plus 1/w per vertex, so each eye is the same frame with
	x shifted by  eye * k * (1/w - 1/c)  (eye = -1 left, +1 right). That is exactly the parallax
	of a camera moved sideways, and stays affine across a triangle because 1/w is.

	Per eye, in NDC (screen width = 2):  shift = -eye * sep * (c * z - 1),  z = 1/w
	  sep = per-eye shift at infinity (far objects never diverge beyond 2*sep in total)
	  c   = convergence distance in game w units (objects at w = c land on the screen plane)
	Applied in the vertex shader (uniform ht4q_stereo), so modifier volumes get it too.
	HUD: the game draws it at 1/w ~1 and 10, the world stays below ~0.6, so vertices with
	z >= HUDZ are shifted as if at depth HUD instead.
	Sky: screen-space backdrop quads (9.5e-6 < 1/w <= 2e-5). The camera hook has the game build them
	for the rendered view; it lays them out by angle (HT4Q_SKY_CORRECT px per radian), so in VR
	they are re-projected with the real projection to hold still in the world.

	Environment:
	  HT4Q_STEREO=sbs | anaglyph   half side-by-side (left eye left) or red/cyan; default off
	  HT4Q_STEREO_SEP=0.015        sep, NDC
	  HT4Q_STEREO_CONV=20          c, game units
	  HT4Q_STEREO_HUDZ=0.75        1/w at or above which a vertex counts as HUD
	  HT4Q_STEREO_HUD=15           depth the HUD is pinned to, game units (35 on Quest)
	  HT4Q_HUD_SHRINK_VR=0.35      in VR, scale the HUD toward the view center by 1 - this
	  HT4Q_HUD_LOCK=1              keep the HUD still in the unhooked camera's (boat's) frame while the
	                               head turns (default 1 in the headset, 0 on the desktop)
	All four can be changed at runtime through the memserver ('S', dcmem.stereo()).
	2-player full screen (core/ht4q/twoplayer.cpp): the game draws player 2's view 480 px below the
	screen, so frames with it are drawn once per player, moved up 480 px for player 2.
	  HT4Q_2P_VIEW=1 | 2 | both    which player is shown (both: side by side, desktop without stereo
	                               only; default both on the desktop, 1 in the headset)
	  HT4Q_FRAMEDUMP=<path>   when <path>.req exists, write the next 4 renders' polygon lists to <path>;
	                          when <path>.shot.req exists, save the rendered frame to <path>.png
*/
#include "gles.h"
#include "ht4q_stereo.h"
#include "quad.h"
#include "ht4q/ht4q.h"
#include "ht4q/stream.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include <stb_image_write.h>
#include <cstdlib>
#include <unistd.h>

namespace ht4q
{

static void dumpList(FILE *f, const char *name, const std::vector<PolyParam>& list, const rend_context& ctx)
{
	for (size_t i = 0; i < list.size(); i++)
	{
		const PolyParam& pp = list[i];
		float zmin = 1e30f, zmax = -1e30f, xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f;
		for (u32 j = pp.first; j < pp.first + pp.count; j++)
		{
			u32 k = ctx.idx[j];
			if (k == ~0u || k >= ctx.verts.size())
				continue;
			const Vertex& v = ctx.verts[k];
			zmin = std::min(zmin, v.z); zmax = std::max(zmax, v.z);
			xmin = std::min(xmin, v.x); xmax = std::max(xmax, v.x);
			ymin = std::min(ymin, v.y); ymax = std::max(ymax, v.y);
		}
		fprintf(f, "%s %4zu n=%4u z=[%.6g %.6g] x=[%.1f %.1f] y=[%.1f %.1f] tex=%d tcw=%08x tsp=%08x isp=%08x clip=%08x\n",
				name, i, pp.count, zmin, zmax, xmin, xmax, ymin, ymax, pp.texture != nullptr,
				pp.tcw.full, pp.tsp.full, pp.isp.full, pp.tileclip);
		static const bool allVerts = getenv("HT4Q_FRAMEDUMP_VERTS") != nullptr;
		if (allVerts)
			for (u32 j = pp.first; j < pp.first + pp.count; j++)
			{
				u32 k = ctx.idx[j];
				if (k < ctx.verts.size())
					fprintf(f, "  v %.3f %.3f %.7g\n", ctx.verts[k].x, ctx.verts[k].y, ctx.verts[k].z);
			}
		else if (zmax < 2e-5f && zmin > 9.5e-6f)	// sky: every vertex with its texture coordinates
			for (u32 j = pp.first; j < pp.first + pp.count; j++)
			{
				u32 k = ctx.idx[j];
				if (k < ctx.verts.size())
					fprintf(f, "  skyv %.2f %.2f u %.5f v %.5f\n", ctx.verts[k].x, ctx.verts[k].y, ctx.verts[k].u, ctx.verts[k].v);
			}
	}
}

static void maybeDumpFrame()
{
	static const char *path = getenv("HT4Q_FRAMEDUMP");
	static int remaining;
	if (path == nullptr)
		return;
	std::string req = std::string(path) + ".req";
	if (remaining == 0)
	{
		if (access(req.c_str(), F_OK) != 0)
			return;
		unlink(req.c_str());
		remaining = 4;	// a game frame may be several renders (e.g. 3D, then HUD on top)
	}
	FILE *f = fopen(path, remaining == 4 ? "w" : "a");
	if (f == nullptr)
		return;
	const rend_context& ctx = *gl.rendContext;
	fprintf(f, "render %d fb %ux%u clear %d fZ_max %g verts %zu passes %zu\n", 4 - remaining, ctx.framebufferWidth,
			ctx.framebufferHeight, ctx.clearFramebuffer, ctx.fZ_max, ctx.verts.size(), ctx.render_passes.size());
	FramePose fp;
	if (framePose(ctx.ht4qPoseId, fp) && fp.rotValid)
		fprintf(f, "pose %u cam %d %d %d game %d %d %d\n", fp.id, fp.camRot[0], fp.camRot[1], fp.camRot[2],
				fp.gameRot[0], fp.gameRot[1], fp.gameRot[2]);
	if (framePose(ctx.ht4qPoseId, fp) && fp.rotValid)
	{
		fprintf(f, "xfm");
		for (float m : fp.camXfm)
			fprintf(f, " %g", m);
		fprintf(f, " zscreen %g immersive %d\n", fp.zscreen, fp.immersive);
	}
	std::map<float, int> zHist;		// raw vertices by 1/w, rounded to 2 significant digits
	for (const Vertex& v : ctx.verts)
	{
		float z = v.z == 0 ? 0 : std::pow(10.f, std::floor(std::log10(std::fabs(v.z))) - 1);
		zHist[z == 0 ? 0 : std::round(v.z / z) * z]++;
	}
	for (const auto& [z, n] : zHist)
		if (z > 0.13f)
			fprintf(f, "zhist %g %d\n", z, n);
	for (const Vertex& v : ctx.verts)
		if (v.z >= 0.5f)
			fprintf(f, "vtx %.1f %.1f %g %02x%02x%02x%02x\n", v.x, v.y, v.z, v.col[0], v.col[1], v.col[2], v.col[3]);
	dumpList(f, "op", ctx.global_param_op, ctx);
	dumpList(f, "pt", ctx.global_param_pt, ctx);
	dumpList(f, "tr", ctx.global_param_tr, ctx);
	for (size_t i = 0; i < ctx.sortedTriangles.size(); i++)	// sorted translucent triangles
	{
		const SortedTriangle& st = ctx.sortedTriangles[i];
		const PolyParam& pp = ctx.global_param_tr[st.polyIndex];
		fprintf(f, "trs %4zu poly %u n=%u tcw=%08x tsp=%08x isp=%08x clip=%08x", i, st.polyIndex, st.count, pp.tcw.full, pp.tsp.full, pp.isp.full, pp.tileclip);
		// all vertices of the sun and flare (1/w < 1e-4), the first 6 of the rest
		const bool far = st.count > 0 && st.first < ctx.idx.size() && ctx.idx[st.first] < ctx.verts.size()
				&& ctx.verts[ctx.idx[st.first]].z < 1e-4f;
		for (u32 j = st.first; j < st.first + (far ? st.count : std::min(st.count, 6u)); j++)
			if (j < ctx.idx.size() && ctx.idx[j] < ctx.verts.size())
				fprintf(f, " (%.1f %.1f %g)", ctx.verts[ctx.idx[j]].x, ctx.verts[ctx.idx[j]].y, ctx.verts[ctx.idx[j]].z);
		fprintf(f, "\n");
	}
	fclose(f);
	remaining--;
}

// <path>.shot.req -> <path>.png of the output framebuffer (not the screen, so nothing else is captured)
static void maybeShot()
{
	static const char *path = getenv("HT4Q_FRAMEDUMP");
	if (path == nullptr || gl.ofbo.framebuffer == nullptr)
		return;
	std::string req = std::string(path) + ".shot.req";
	if (access(req.c_str(), F_OK) != 0)
		return;
	unlink(req.c_str());
	int w = gl.ofbo.framebuffer->getWidth(), h = gl.ofbo.framebuffer->getHeight();
	std::vector<u8> px(w * h * 4);
	gl.ofbo.framebuffer->bind(GL_READ_FRAMEBUFFER);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
	for (u32 i = 3; i < px.size(); i += 4)
		px[i] = 0xff;
	FILE *f = fopen((std::string(path) + ".png").c_str(), "wb");
	if (f == nullptr)
		return;
	stbi_write_png_to_func([](void *ctx, void *data, int size) { fwrite(data, 1, size, (FILE *)ctx); },
			f, w, h, 4, px.data(), w * 4);	// ofbo rows are already top-down
	fclose(f);
}

enum class Mode { Off, SideBySide, Anaglyph, Xr };


static StereoEyes eyes;
const StereoEyes& stereoEyes() { return eyes; }

// HUD lock: the head's rotation relative to the unhooked camera, and the projection distance;
// zscreen 0 = off
struct HudLock {
	float rot[9];
	float zscreen;
};

// Sets the ht4q_stereo / ht4q_hudShrink / ht4q_hudRot vertex uniforms (see VertexShaderSource) in
// every compiled program.
static void setStereo(const float v[4], float hudShrink = 0.f, const HudLock& hud = {})
{
	auto set = [v, hudShrink, &hud](GLuint program) {
		if (program == 0)
			return;
		glcache.UseProgram(program);
		GLint loc = glGetUniformLocation(program, "ht4q_stereo");
		if (loc != -1)
			glUniform4fv(loc, 1, v);
		loc = glGetUniformLocation(program, "ht4q_hudShrink");
		if (loc != -1)
			glUniform1f(loc, hudShrink);
		loc = glGetUniformLocation(program, "ht4q_hudRot");
		if (loc != -1)
			glUniformMatrix3fv(loc, 1, GL_FALSE, hud.rot);	// row-major R read column-major: R^T, camera -> head
		loc = glGetUniformLocation(program, "ht4q_hudZscreen");
		if (loc != -1)
			glUniform1f(loc, hud.zscreen);
	};
	for (auto& it : gl.shaders)
		set(it.second.program);
	set(gl.modvol_shader.program);
}

static void eyeUniform(float v[4], float eye, float sep, float conv, float hudMinZ, float hudDepth)
{
	// ndc.x += -eye * sep * (conv * z - 1)
	v[0] = -eye * sep * conv;
	v[1] = eye * sep;
	v[2] = hudMinZ;
	v[3] = 1.f / hudDepth;
}

// the sky backdrop: quads at 1/w 1e-5 over a background plane at 9e-6
static constexpr float SKY_MIN_Z = 8e-6f, SKY_MAX_Z = 2e-5f;

enum class SkyMode { Off, Capture, Hide };

// Sets the ht4q_sky vertex uniform: sky polygons are drawn normally, scaled into the capture
// image, or hidden.
static void setSkyMode(SkyMode mode)
{
	auto set = [&](GLuint program) {
		if (program == 0)
			return;
		glcache.UseProgram(program);
		GLint loc = glGetUniformLocation(program, "ht4q_sky");
		if (loc != -1)
			glUniform4f(loc, SKY_MIN_Z, SKY_MAX_Z, (float)mode, 0.f);
	};
	for (auto& it : gl.shaders)
		set(it.second.program);
	set(gl.modvol_shader.program);
}

// Full-screen passes draw one attribute-less triangle; core profiles need some VAO bound for that.
static GLuint emptyVao()
{
	static GLuint vao;
	if (vao == 0)
		glGenVertexArrays(1, &vao);
	return vao;
}

// Headset sky. The game draws its sky as a few huge screen-space quads laid out by angle, which no
// re-projection of their corners can keep still in the world. Instead, with the game building a
// level sky for the camera heading (FramePose::skyLevel), the sky polygons are drawn once into
// skyCapture (game coordinates x -160..800, y -80..560), and each eye's background is painted
// from it per pixel: pixel -> view direction -> world azimuth/elevation (undoing the camera's
// pitch and roll) -> the game's layout  x = 320 + k * azimuth,  y = horizon - k * elevation
// (k = HT4Q_SKY_CORRECT px per radian). The sky polygons are then hidden in the eye passes.
static constexpr float CAP_X0 = -160.f, CAP_Y0 = -80.f, CAP_W = 960.f, CAP_H = 640.f;

static const char *SkyFillVertexShader = R"(
void main()
{
	gl_Position = vec4(float(gl_VertexID / 2) * 4.0 - 1.0, float(gl_VertexID % 2) * 4.0 - 1.0, 0.0, 1.0);
}
)";

static const char *SkyFillFragmentShader = R"(
uniform sampler2D sky;
uniform highp vec4 cam;		// zscreen, px per radian, pitch, roll (game convention, radians)
uniform highp vec4 layout_;	// horizon y, top y, output width, output height
uniform highp vec2 xrange;	// game x covered by the sky

void main()
{
	// output rows are stored top-down like Flycast's: row 0 is game y 0
	highp vec2 s = gl_FragCoord.xy / layout_.zw * vec2(640.0, 480.0);
	highp vec3 d = vec3((s.x - 320.0) / cam.x, (240.0 - s.y) / cam.x, 1.0);
	// camera -> level frame: Rx(pitch) Rz(roll), as in the game's Ry(h) Rx(p) Rz(r)
	highp float cr = cos(cam.w), sr = sin(cam.w), cp = cos(cam.z), sp = sin(cam.z);
	highp vec3 a = vec3(cr * d.x - sr * d.y, sr * d.x + cr * d.y, d.z);
	highp vec3 w = vec3(a.x, cp * a.y - sp * a.z, sp * a.y + cp * a.z);
	highp float azimuth = atan(w.x, w.z);
	highp float elevation = atan(w.y, length(w.xz));
	// beyond what the game drew (straight up, far sideways), repeat its edge
	highp vec2 g = vec2(clamp(320.0 + cam.y * azimuth, xrange.x, xrange.y), max(layout_.x - cam.y * elevation, layout_.y));
	highp vec2 uv = (g - vec2(-160.0, -80.0)) / vec2(960.0, 640.0);	// CAP_X0, CAP_Y0, CAP_W, CAP_H
	gl_FragColor = vec4(texture(sky, uv).rgb, 1.0);
}
)";

struct SkyFrame {
	int polys = 0;			// leading opaque polygons that make up the sky
	float horizonY = 0.f;	// game y of the sky's horizon (texture v = 0)
	float topY = 0.f;		// game y of its top edge
	float minX = 0.f, maxX = 640.f;	// game x it covers
};

static SkyFrame findSky(const rend_context& ctx)
{
	SkyFrame sf;
	float hsum = 0.f, hn = 0.f, top = 1e9f, minX = 1e9f, maxX = -1e9f;
	// the sky comes first in the opaque list, with the odd other polygon in between (hidden
	// while capturing)
	const int scan = std::min<int>(ctx.global_param_op.size(), 64);
	for (int i = 0; i < scan; i++)
	{
		const PolyParam& pp = ctx.global_param_op[i];
		if (pp.count > 0 && pp.first + pp.count <= ctx.idx.size())
		{
			u32 k = ctx.idx[pp.first];
			if (k >= ctx.verts.size() || ctx.verts[k].z < SKY_MIN_Z || ctx.verts[k].z > SKY_MAX_Z)
				continue;
			sf.polys = i + 1;
			if (pp.texture != nullptr)
				for (u32 j = pp.first; j < pp.first + pp.count; j++)
				{
					if (ctx.idx[j] >= ctx.verts.size())
						continue;	// strip restart (~0)
					const Vertex& v = ctx.verts[ctx.idx[j]];
					minX = std::min(minX, v.x);
					maxX = std::max(maxX, v.x);
					if (std::fabs(v.v) < 1e-3f)
						hsum += v.y, hn++;
					else
						top = std::min(top, v.y);
				}
		}
	}
	sf.horizonY = hn > 0 ? hsum / hn : 247.f;
	// a few pixels inside the edges, away from the empty capture around them
	sf.topY = top < 1e9f ? top + 4.f : 0.f;
	if (minX < maxX)
	{
		sf.minX = minX + 4.f;
		sf.maxX = maxX - 4.f;
	}
	return sf;
}

static std::unique_ptr<GlFramebuffer> skyCapture;

// Draws the sky polygons into skyCapture. Leaves the output framebuffer bound.
static bool captureSky(const SkyFrame& sf)
{
	const int w = (int)CAP_W * 3 / 2, h = (int)CAP_H * 3 / 2;
	if (skyCapture == nullptr)
		skyCapture = std::make_unique<GlFramebuffer>(w, h, true, true);
	GLint viewport[4];
	glGetIntegerv(GL_VIEWPORT, viewport);
	skyCapture->bind();
	glViewport(0, 0, w, h);
	glcache.Disable(GL_SCISSOR_TEST);
	glcache.ClearColor(0.f, 0.f, 0.f, 0.f);
	glcache.DepthMask(GL_TRUE);
	glStencilMask(0xFF);
	glClearDepthf(0.f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	const bool clipping = ShaderUniforms.base_clipping.enabled;
	ShaderUniforms.base_clipping.enabled = false;
	setSkyMode(SkyMode::Capture);
	DrawOpaqueRange(0, sf.polys);
	ShaderUniforms.base_clipping.enabled = clipping;
	gl.ofbo.framebuffer->bind();
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	return true;
}

// Paints the bound output framebuffer (w x h) with the captured sky for camera fp.
static void fillSky(const SkyFrame& sf, const FramePose& fp, float pxPerRadian, int w, int h)
{
	static GLuint program;
	static GLint camLoc, layoutLoc, xrangeLoc;
	static bool failed;
	if (failed)
		return;
	if (program == 0)
	{
		OpenGlSource vs, fs;
		vs.addSource(SkyFillVertexShader);
		fs.addSource(PixelCompatShader).addSource(SkyFillFragmentShader);
		program = gl_CompileAndLink(vs.generate().c_str(), fs.generate().c_str());
		if (program == 0)
		{
			failed = true;
			return;
		}
		glcache.UseProgram(program);
		glUniform1i(glGetUniformLocation(program, "sky"), 0);
		camLoc = glGetUniformLocation(program, "cam");
		layoutLoc = glGetUniformLocation(program, "layout_");
		xrangeLoc = glGetUniformLocation(program, "xrange");
	}
	const float k = (float)(2 * M_PI / 65536);
	GLint vao;
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
	glcache.Disable(GL_SCISSOR_TEST);
	glcache.Disable(GL_BLEND);
	glcache.Disable(GL_CULL_FACE);
	glcache.Disable(GL_STENCIL_TEST);
	glcache.Disable(GL_DEPTH_TEST);
	glcache.DepthMask(GL_FALSE);
	glcache.UseProgram(program);
	glActiveTexture(GL_TEXTURE0);
	glcache.BindTexture(GL_TEXTURE_2D, skyCapture->getTexture());
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glUniform4f(camLoc, fp.zscreen, pxPerRadian, fp.camRot[1] * k, fp.camRot[2] * k);
	glUniform4f(layoutLoc, sf.horizonY, sf.topY, (float)w, (float)h);
	glUniform2f(xrangeLoc, sf.minX, sf.maxX);
	glBindVertexArray(emptyVao());
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(vao);
	glcache.DepthMask(GL_TRUE);
}

// VR projection (Quest, racing): each eye is a camera moved sideways by eyeOffset game units,
// no screen plane: ndc.x += -eye * eyeOffset * (zscreen / 320) * z
static void eyeUniformVr(float v[4], float eye, const FramePose& fp, float hudMinZ, float hudDepth)
{
	v[0] = -eye * fp.eyeOffset * fp.zscreen / 320.f;
	v[1] = 0.f;
	v[2] = hudMinZ;
	v[3] = 1.f / hudDepth;
}

// ofbo color -> dst (resized to match)
static void copyOutput(std::unique_ptr<GlFramebuffer>& dst, bool withTexture)
{
	const GlFramebuffer& src = *gl.ofbo.framebuffer;
	int w = src.getWidth(), h = src.getHeight();
	if (dst == nullptr || dst->getWidth() != w || dst->getHeight() != h)
		dst = std::make_unique<GlFramebuffer>(w, h, false, withTexture);
	glcache.Disable(GL_SCISSOR_TEST);	// blits are scissored, and DrawStrips leaves the last polygon's clip
	src.bind(GL_READ_FRAMEBUFFER);
	dst->bind(GL_DRAW_FRAMEBUFFER);
	glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

// Depth for the OpenXR projection layer, so the compositor's reprojection knows how far away
// each pixel is. Flycast writes depth = log2(1 + 1e5 * z) / 34 (z = 1/w, see PixelPipelineShader);
// convert it to a standard reversed projection depth  d = A + B / distance  over
// [DEPTH_NEAR, DEPTH_FAR] m, with the HUD at the depth it's shown at.
static constexpr float DEPTH_NEAR = 0.05f, DEPTH_FAR = 10000.f;

static const char *DepthVertexShader = R"(
void main()
{
	// one triangle covering the viewport
	gl_Position = vec4(float(gl_VertexID / 2) * 4.0 - 1.0, float(gl_VertexID % 2) * 4.0 - 1.0, 0.0, 1.0);
}
)";

static const char *DepthFragmentShader = R"(
uniform highp sampler2D raw;
uniform highp vec4 params;	// hud min z, hud z, A, B * units per meter

void main()
{
	highp float z = (exp2(texelFetch(raw, ivec2(gl_FragCoord.xy), 0).r * 34.0) - 1.0) / 100000.0;
	if (z >= params.x)
		z = params.y;
	gl_FragDepth = clamp(params.z + params.w * z, 0.0, 1.0);
}
)";

// Copies the depth of the eye just drawn into ofbo to eyes.depthFbo[eye], converted.
static void copyDepth(int eye, int w, int h, float hudMinZ, float hudZ, float unitsPerMeter)
{
	static GLuint program, rawTex, rawFbo, depthRb[2], depthFbo[2];
	static GLint paramsLoc;
	static int width, height;
	static bool failed;
	if (failed)
		return;
	if (program == 0)
	{
		OpenGlSource vs, fs;
		vs.addSource(DepthVertexShader);
		fs.addSource(DepthFragmentShader);
		program = gl_CompileAndLink(vs.generate().c_str(), fs.generate().c_str());
		if (program == 0)
		{
			failed = true;
			return;
		}
		glcache.UseProgram(program);
		glUniform1i(glGetUniformLocation(program, "raw"), 0);
		paramsLoc = glGetUniformLocation(program, "params");
		glGenTextures(1, &rawTex);
		glGenFramebuffers(1, &rawFbo);
		glGenRenderbuffers(2, depthRb);
		glGenFramebuffers(2, depthFbo);
	}
	if (w != width || h != height)
	{
		// raw: same format as Flycast's depth renderbuffer, so it can be blitted
		glcache.BindTexture(GL_TEXTURE_2D, rawTex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
		glBindFramebuffer(GL_FRAMEBUFFER, rawFbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, rawTex, 0);
		bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
		for (int i = 0; i < 2; i++)
		{
			glBindRenderbuffer(GL_RENDERBUFFER, depthRb[i]);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
			glBindFramebuffer(GL_FRAMEBUFFER, depthFbo[i]);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRb[i]);
			ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
		}
		if (!ok)
		{
			ERROR_LOG(RENDERER, "ht4q stereo: depth framebuffers incomplete, no depth for the headset");
			failed = true;
			gl.ofbo.framebuffer->bind();
			return;
		}
		width = w;
		height = h;
	}
	gl.ofbo.framebuffer->bind(GL_READ_FRAMEBUFFER);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rawFbo);
	glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);

	GLint viewport[4], vao;
	glGetIntegerv(GL_VIEWPORT, viewport);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
	glBindFramebuffer(GL_FRAMEBUFFER, depthFbo[eye]);
	glViewport(0, 0, w, h);
	glcache.Disable(GL_SCISSOR_TEST);
	glcache.Disable(GL_BLEND);
	glcache.Disable(GL_CULL_FACE);
	glcache.Disable(GL_STENCIL_TEST);
	glcache.Enable(GL_DEPTH_TEST);
	glcache.DepthFunc(GL_ALWAYS);
	glcache.DepthMask(GL_TRUE);
	glcache.UseProgram(program);
	glActiveTexture(GL_TEXTURE0);
	glcache.BindTexture(GL_TEXTURE_2D, rawTex);
	const float b = 1.f / (1.f / DEPTH_NEAR - 1.f / DEPTH_FAR), a = -b / DEPTH_FAR;
	glUniform4f(paramsLoc, hudMinZ, hudZ, a, b * unitsPerMeter);
	glBindVertexArray(emptyVao());
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(vao);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	gl.ofbo.framebuffer->bind();

	eyes.depthFbo[eye] = depthFbo[eye];
	eyes.depthNearZ = DEPTH_FAR;	// reversed: depth 0 is far
	eyes.depthFarZ = DEPTH_NEAR;
}

// 2-player full screen: player 2's polygons are tile-clipped to rows 15 and below (480+ px)
static bool hasPlayer2(const rend_context& ctx)
{
	auto scan = [](const std::vector<PolyParam>& list) {
		for (const PolyParam& pp : list)
			if ((pp.tileclip >> 28) >= 2 && ((pp.tileclip >> 12) & 31) >= 15)
				return true;
		return false;
	};
	return twoPlayerFull() && (scan(ctx.global_param_op) || scan(ctx.global_param_pt) || scan(ctx.global_param_tr));
}

// Moves the frame up by y dreamcast pixels in every program (ndcMat, set per frame by renderFrame).
static void shiftFrame(float y)
{
	gl.matrices.ht4qShiftY(y);
	ShaderUniforms.ndcMat = gl.matrices.GetNormalMatrix();
	auto set = [](GLuint program, GLint loc) {
		if (program == 0 || loc == -1)
			return;
		glcache.UseProgram(program);
		glUniformMatrix4fv(loc, 1, GL_FALSE, &ShaderUniforms.ndcMat[0][0]);
	};
	for (auto& it : gl.shaders)
		set(it.second.program, it.second.ndcMat);
	set(gl.modvol_shader.program, gl.modvol_shader.ndcMat);
	set(gl.n2ModVolShader.program, gl.n2ModVolShader.ndcMat);
}

// HT4Q_2P_VIEW: 1, 2, or 0 for both
static int playerView()
{
	static const int view = [] {
		const char *v = getenv("HT4Q_2P_VIEW");
		std::string s = v != nullptr ? v : "";
		if (s == "1" || s == "2")
			return s[0] - '0';
		if (s == "both")
			return 0;
#ifdef HT4Q_XR
		return 1;
#else
		return 0;
#endif
	}();
	return view;
}

// Clears the output framebuffer for another pass over the same frame (the game's background plane
// is in player 1's part of the frame).
static void clearForPass()
{
	gl.ofbo.framebuffer->bind();
	glcache.Disable(GL_SCISSOR_TEST);
	glcache.DepthMask(GL_TRUE);
	glStencilMask(0xFF);
	glcache.ClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

// The remote viewer's image (core/ht4q/stream.cpp): player 2's view in 2-player frames, else the screen
// itself (menus). Drawn before the frame's own passes, scaled to the stream size and read back. A viewer
// in a headset (immersive pose) gets both eyes side by side, drawn for the head pose it sent, like
// player 1's in the headset.
static void streamView(bool player2)
{
	static const std::pair<int, int> eyeSize = [] {
		int w = 960, h = 720;
		if (const char *v = getenv("HT4Q_STREAM_EYE"))
			sscanf(v, "%dx%d", &w, &h);
		return std::pair<int, int>(w & ~15, h & ~15);
	}();
	static std::unique_ptr<GlFramebuffer> img;
	static std::vector<u8> px;
	const StereoParams p = stereoParams();
	StreamFrameInfo info{};
	info.poseId = player2 ? gl.rendContext->ht4qPoseId2 : 0;
	info.zscreen = 320.f;
	FramePose fp;
	const bool vr = player2 && framePose(info.poseId, fp) && fp.immersive;
	if (vr)
	{
		info.stereo = true;
		memcpy(info.headQ, fp.head.q, sizeof(info.headQ));
		memcpy(info.headP, fp.head.p, sizeof(info.headP));
		info.zscreen = fp.zscreen;
	}
	const int ew = eyeSize.first, eh = eyeSize.second, w = vr ? ew * 2 : ew;
	if (img == nullptr || img->getWidth() != w)
		img = std::make_unique<GlFramebuffer>(w, eh, false, false);
	setSkyMode(SkyMode::Off);
	HudLock hud{};
	if (vr)
	{
		memcpy(hud.rot, fp.headRot, sizeof(hud.rot));
		hud.zscreen = fp.zscreen;
	}
	const float hudShrink = vr ? 1.f - (1.f - p.hudShrinkVr) * fp.zscreen / 320.f : 0.f;
	for (int eye = 0; eye < (vr ? 2 : 1); eye++)
	{
		clearForPass();
		if (vr)
		{
			float v[4];
			eyeUniformVr(v, eye == 0 ? -1.f : 1.f, fp, p.hudMinZ, p.hudDepth);
			setStereo(v, hudShrink, hud);
		}
		if (player2)
			shiftFrame(PLAYER2_SHIFT);
		DrawStrips();
		if (player2)
			shiftFrame(0.f);
		glcache.Disable(GL_SCISSOR_TEST);
		gl.ofbo.framebuffer->bind(GL_READ_FRAMEBUFFER);
		img->bind(GL_DRAW_FRAMEBUFFER);
		glBlitFramebuffer(0, 0, gl.ofbo.framebuffer->getWidth(), gl.ofbo.framebuffer->getHeight(),
				eye * ew, 0, eye * ew + ew, eh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	}
	if (vr)
	{
		const float zero[4] {};
		setStereo(zero);
	}
	img->bind(GL_READ_FRAMEBUFFER);
	px.resize((size_t)w * eh * 4);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glReadPixels(0, 0, w, eh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());	// rows top-down, like the output
	streamFrame(px.data(), w, eh, info);
	clearForPass();
}

void drawStripsStereo(int width, int height)
{
	static const Mode mode = [] {
		const char *v = getenv("HT4Q_STEREO");
		std::string s = v != nullptr ? v : "";
#ifdef HT4Q_XR
		if (s.empty())
			return Mode::Xr;
#endif
		return s == "sbs" ? Mode::SideBySide : s == "anaglyph" ? Mode::Anaglyph : Mode::Off;
	}();
	const StereoParams p = stereoParams();
	const float sep = p.sep, conv = p.conv, hudMinZ = p.hudMinZ, hudDepth = p.hudDepth;
	static std::unique_ptr<GlFramebuffer> leftEye, rightEye;

	maybeDumpFrame();
	const bool twoViews = gl.ofbo.framebuffer != nullptr && hasPlayer2(*gl.rendContext);
	int player = twoViews ? playerView() : 1;	// 0 = both
	if (player == 0 && mode != Mode::Off)
		player = 1;
	if (streamActive() && gl.ofbo.framebuffer != nullptr && !gl.rendContext->isRTT)
		streamView(twoViews);
	FramePose fp;
	const u32 poseId = player == 2 ? gl.rendContext->ht4qPoseId2 : gl.rendContext->ht4qPoseId;
	const bool havePose = framePose(poseId, fp);
	// headset sky (see captureSky): only when the game built a level sky for this frame
	SkyFrame sky;
	if (havePose && fp.skyLevel && fp.rotValid && p.skyCorrect > 0.f && gl.ofbo.framebuffer != nullptr)
	{
		sky = findSky(*gl.rendContext);
		if (sky.polys > 0)
			captureSky(sky);
	}
	static const bool hudLockOn = [] {
		const char *v = getenv("HT4Q_HUD_LOCK");
#ifdef HT4Q_XR
		return v == nullptr || v[0] != '0';
#else
		return v != nullptr && v[0] == '1';
#endif
	}();
	HudLock hud{};
	if (hudLockOn && havePose && (mode != Mode::Xr || fp.immersive))
	{
		memcpy(hud.rot, fp.headRot, sizeof(hud.rot));
		hud.zscreen = fp.zscreen;
	}
	auto drawEye = [&]() {
		if (sky.polys > 0)
		{
			fillSky(sky, fp, p.skyCorrect, gl.ofbo.framebuffer->getWidth(), gl.ofbo.framebuffer->getHeight());
			setSkyMode(SkyMode::Hide);
		}
		else
			setSkyMode(SkyMode::Off);
		if (player == 2)
		{
			clearForPass();
			shiftFrame(PLAYER2_SHIFT);
		}
		DrawStrips();
		if (player == 2)
			shiftFrame(0.f);
	};
	if (player == 0)
	{
		// both players side by side: player 1, then player 2 moved up
		static std::unique_ptr<GlFramebuffer> view1, view2;
		setSkyMode(SkyMode::Off);
		DrawStrips();
		copyOutput(view1, false);
		clearForPass();
		shiftFrame(PLAYER2_SHIFT);
		DrawStrips();
		shiftFrame(0.f);
		copyOutput(view2, false);
		const int w = gl.ofbo.framebuffer->getWidth(), h = gl.ofbo.framebuffer->getHeight();
		clearForPass();
		gl.ofbo.framebuffer->bind(GL_DRAW_FRAMEBUFFER);
		view1->bind(GL_READ_FRAMEBUFFER);
		glBlitFramebuffer(0, 0, w, h, 0, h / 4, w / 2, h * 3 / 4, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		view2->bind(GL_READ_FRAMEBUFFER);
		glBlitFramebuffer(0, 0, w, h, w / 2, h / 4, w, h * 3 / 4, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		maybeShot();
		gl.ofbo.framebuffer->bind();
		return;
	}
	if (mode == Mode::Off || gl.ofbo.framebuffer == nullptr)
	{
		const float mono[4] { 0.f, 0.f, hudMinZ, 1.f / hudDepth };
		if (hud.zscreen > 0.f)
			setStereo(mono, 0.f, hud);
		drawEye();
		if (hud.zscreen > 0.f)
		{
			const float zero[4] {};
			setStereo(zero);
		}
		setSkyMode(SkyMode::Off);
		maybeShot();
		return;
	}
	const bool vr = mode == Mode::Xr && havePose && fp.immersive;
	float v[4];
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);

	// left eye
	// keep the HUD's angular size independent of the field of view (tuned at 90 deg, zscreen 320)
	const float hudShrink = vr ? 1.f - (1.f - p.hudShrinkVr) * fp.zscreen / 320.f : 0.f;
	if (vr)
		eyeUniformVr(v, -1.f, fp, hudMinZ, hudDepth);
	else
		eyeUniform(v, -1.f, sep, conv, hudMinZ, hudDepth);
	setStereo(v, hudShrink, hud);
	drawEye();
	copyOutput(leftEye, mode == Mode::Anaglyph);
	const int w = gl.ofbo.framebuffer->getWidth(), h = gl.ofbo.framebuffer->getHeight();
	if (vr)
		copyDepth(0, w, h, hudMinZ, 1.f / hudDepth, p.unitsPerMeter);

	// right eye, into the same output framebuffer
	gl.ofbo.framebuffer->bind();
	glcache.Disable(GL_SCISSOR_TEST);
	glcache.DepthMask(GL_TRUE);
	glStencilMask(0xFF);
	glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	if (scissor)
		glcache.Enable(GL_SCISSOR_TEST);
	if (vr)
		eyeUniformVr(v, 1.f, fp, hudMinZ, hudDepth);
	else
		eyeUniform(v, 1.f, sep, conv, hudMinZ, hudDepth);
	setStereo(v, hudShrink, hud);
	drawEye();
	const float zero[4] {};
	setStereo(zero);
	setSkyMode(SkyMode::Off);
	if (vr)
		copyDepth(1, w, h, hudMinZ, 1.f / hudDepth, p.unitsPerMeter);
	else
		eyes.depthFbo[0] = eyes.depthFbo[1] = 0;
	glcache.Disable(GL_SCISSOR_TEST);
	if (mode == Mode::Xr)
	{
		// keep both eyes for the OpenXR layer; the output framebuffer keeps the right eye
		// as the mono image (menus, flat fallback)
		copyOutput(rightEye, false);
		eyes.fbo[0] = leftEye->getFramebuffer();
		eyes.fbo[1] = rightEye->getFramebuffer();
		eyes.width = w;
		eyes.height = h;
		eyes.poseId = poseId;
		eyes.frame++;
		static auto last = std::chrono::steady_clock::now();
		if (eyes.frame % 300 == 0)
		{
			auto now = std::chrono::steady_clock::now();
			NOTICE_LOG(RENDERER, "ht4q stereo: %.1f stereo frames/s at %dx%d per eye",
					300.0 / std::chrono::duration<double>(now - last).count(), w, h);
			last = now;
		}
	}
	else if (mode == Mode::SideBySide)
	{
		copyOutput(rightEye, false);
		gl.ofbo.framebuffer->bind(GL_DRAW_FRAMEBUFFER);
		leftEye->bind(GL_READ_FRAMEBUFFER);
		glBlitFramebuffer(0, 0, w, h, 0, 0, w / 2, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		rightEye->bind(GL_READ_FRAMEBUFFER);
		glBlitFramebuffer(0, 0, w, h, w / 2, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	}
	else
	{
		// red channel from the left eye over the right eye's green/blue
		gl.ofbo.framebuffer->bind();
		glViewport(0, 0, w, h);
		glcache.Disable(GL_BLEND);
		glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_FALSE);
		gl.quadDrawer->draw(leftEye->getTexture(), false, false);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	}
	maybeShot();
	gl.ofbo.framebuffer->bind();
}

static GLuint panelFbo, panelTex;
static int panelW, panelH;
static GLint panelPrevFbo;

void beginPanelTarget()
{
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &panelPrevFbo);
	if (panelFbo == 0 || panelW != settings.display.width || panelH != settings.display.height)
	{
		if (panelFbo == 0)
		{
			glGenFramebuffers(1, &panelFbo);
			panelTex = glcache.GenTexture();
		}
		panelW = settings.display.width;
		panelH = settings.display.height;
		glcache.BindTexture(GL_TEXTURE_2D, panelTex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, panelW, panelH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, panelFbo);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, panelTex, 0);
	}
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, panelFbo);
	glcache.Disable(GL_SCISSOR_TEST);
	glcache.ClearColor(0.f, 0.f, 0.f, 0.f);
	glClear(GL_COLOR_BUFFER_BIT);
	glcache.Enable(GL_SCISSOR_TEST);	// ImGui draws with it
}

void endPanelTarget()
{
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, panelPrevFbo);
}

unsigned panelFramebuffer()
{
	return panelFbo;
}

}	// namespace ht4q
