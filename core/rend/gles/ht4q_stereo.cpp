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

	Environment:
	  HT4Q_STEREO=sbs | anaglyph   half side-by-side (left eye left) or red/cyan; default off
	  HT4Q_STEREO_SEP=0.015        sep, NDC
	  HT4Q_STEREO_CONV=20          c, game units
	  HT4Q_STEREO_HUDZ=0.75        1/w at or above which a vertex counts as HUD
	  HT4Q_STEREO_HUD=15           depth the HUD is pinned to, game units
	  HT4Q_FRAMEDUMP=<path>   when <path>.req exists, write the next 4 renders' polygon lists to <path>;
	                          when <path>.shot.req exists, save the rendered frame to <path>.png
*/
#include "gles.h"
#include "ht4q_stereo.h"
#include "quad.h"
#include <cmath>
#include <cstdio>
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
		fprintf(f, "%s %4zu n=%4u z=[%.6g %.6g] x=[%.1f %.1f] y=[%.1f %.1f] tex=%d tcw=%08x tsp=%08x isp=%08x\n",
				name, i, pp.count, zmin, zmax, xmin, xmax, ymin, ymax, pp.texture != nullptr,
				pp.tcw.full, pp.tsp.full, pp.isp.full);
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
	std::map<float, int> zHist;		// raw vertices by 1/w, rounded to 2 significant digits
	for (const Vertex& v : ctx.verts)
	{
		float z = v.z == 0 ? 0 : std::pow(10.f, std::floor(std::log10(std::fabs(v.z))) - 1);
		zHist[z == 0 ? 0 : std::round(v.z / z) * z]++;
	}
	for (const auto& [z, n] : zHist)
		if (z > 0.13f)
			fprintf(f, "zhist %g %d\n", z, n);
	dumpList(f, "op", ctx.global_param_op, ctx);
	dumpList(f, "pt", ctx.global_param_pt, ctx);
	dumpList(f, "tr", ctx.global_param_tr, ctx);
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

enum class Mode { Off, SideBySide, Anaglyph };

static float envFloat(const char *name, float def)
{
	const char *v = getenv(name);
	return v != nullptr && v[0] != '\0' ? (float)atof(v) : def;
}

// Sets the ht4q_stereo vertex uniform (see VertexShaderSource) in every compiled program.
static void setStereo(const float v[4])
{
	auto set = [v](GLuint program) {
		if (program == 0)
			return;
		GLint loc = glGetUniformLocation(program, "ht4q_stereo");
		if (loc != -1)
		{
			glcache.UseProgram(program);
			glUniform4fv(loc, 1, v);
		}
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

// ofbo color -> dst (resized to match)
static void copyOutput(std::unique_ptr<GlFramebuffer>& dst, bool withTexture)
{
	const GlFramebuffer& src = *gl.ofbo.framebuffer;
	int w = src.getWidth(), h = src.getHeight();
	if (dst == nullptr || dst->getWidth() != w || dst->getHeight() != h)
		dst = std::make_unique<GlFramebuffer>(w, h, false, withTexture);
	src.bind(GL_READ_FRAMEBUFFER);
	dst->bind(GL_DRAW_FRAMEBUFFER);
	glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

void drawStripsStereo(int width, int height)
{
	static const Mode mode = [] {
		const char *v = getenv("HT4Q_STEREO");
		std::string s = v != nullptr ? v : "";
		return s == "sbs" ? Mode::SideBySide : s == "anaglyph" ? Mode::Anaglyph : Mode::Off;
	}();
	static const float sep = envFloat("HT4Q_STEREO_SEP", 0.015f);
	static const float conv = envFloat("HT4Q_STEREO_CONV", 20.f);
	static const float hudMinZ = envFloat("HT4Q_STEREO_HUDZ", 0.75f);
	static const float hudDepth = envFloat("HT4Q_STEREO_HUD", 15.f);
	static std::unique_ptr<GlFramebuffer> leftEye, rightEye;

	maybeDumpFrame();
	if (mode == Mode::Off || gl.ofbo.framebuffer == nullptr)
	{
		DrawStrips();
		maybeShot();
		return;
	}
	float v[4];
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);

	// left eye
	eyeUniform(v, -1.f, sep, conv, hudMinZ, hudDepth);
	setStereo(v);
	DrawStrips();
	copyOutput(leftEye, mode == Mode::Anaglyph);

	// right eye, into the same output framebuffer
	gl.ofbo.framebuffer->bind();
	glcache.Disable(GL_SCISSOR_TEST);
	glcache.DepthMask(GL_TRUE);
	glStencilMask(0xFF);
	glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	if (scissor)
		glcache.Enable(GL_SCISSOR_TEST);
	eyeUniform(v, 1.f, sep, conv, hudMinZ, hudDepth);
	setStereo(v);
	DrawStrips();
	const float zero[4] {};
	setStereo(zero);

	const int w = gl.ofbo.framebuffer->getWidth(), h = gl.ofbo.framebuffer->getHeight();
	glcache.Disable(GL_SCISSOR_TEST);
	if (mode == Mode::SideBySide)
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

}	// namespace ht4q
