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
	Sky: screen-space backdrop quads (9.5e-6 < 1/w <= 2e-5) are rotated by the camera-hook offset the
	game didn't apply to them (skyRotation), in every mode.

	Environment:
	  HT4Q_STEREO=sbs | anaglyph   half side-by-side (left eye left) or red/cyan; default off
	  HT4Q_STEREO_SEP=0.015        sep, NDC
	  HT4Q_STEREO_CONV=20          c, game units
	  HT4Q_STEREO_HUDZ=0.75        1/w at or above which a vertex counts as HUD
	  HT4Q_STEREO_HUD=15           depth the HUD is pinned to, game units (35 on Quest)
	  HT4Q_HUD_SHRINK_VR=0.35      in VR, scale the HUD toward the view center by 1 - this
	All four can be changed at runtime through the memserver ('S', dcmem.stereo()).
	  HT4Q_FRAMEDUMP=<path>   when <path>.req exists, write the next 4 renders' polygon lists to <path>;
	                          when <path>.shot.req exists, save the rendered frame to <path>.png
*/
#include "gles.h"
#include "ht4q_stereo.h"
#include "quad.h"
#include "ht4q/ht4q.h"
#include <chrono>
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
	for (const Vertex& v : ctx.verts)
		if (v.z >= 0.5f)
			fprintf(f, "vtx %.1f %.1f %g %02x%02x%02x%02x\n", v.x, v.y, v.z, v.col[0], v.col[1], v.col[2], v.col[3]);
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

enum class Mode { Off, SideBySide, Anaglyph, Xr };

// sky quads are at 1/w 1e-5; the background plane (9e-6) clears the screen and must stay put
static constexpr float SKY_MIN_Z = 9.5e-6f, SKY_MAX_Z = 2e-5f;

static StereoEyes eyes;
const StereoEyes& stereoEyes() { return eyes; }

// Sets the ht4q_stereo / ht4q_hudShrink vertex uniforms (see VertexShaderSource) in every
// compiled program.
static void setStereo(const float v[4], float hudShrink = 0.f)
{
	auto set = [v, hudShrink](GLuint program) {
		if (program == 0)
			return;
		glcache.UseProgram(program);
		GLint loc = glGetUniformLocation(program, "ht4q_stereo");
		if (loc != -1)
			glUniform4fv(loc, 1, v);
		loc = glGetUniformLocation(program, "ht4q_hudShrink");
		if (loc != -1)
			glUniform1f(loc, hudShrink);
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

// The game draws its sky backdrop as screen-space quads (1/w ~1e-5) placed for its own camera
// angles, ignoring the camera hook. Rotation taking directions from that camera's space into
// the hooked camera's: rel = H^T G, with the game's M = Ry(heading) Rx(pitch) Rz(roll)
// (camera space x right, y up, z forward). Column-major for glUniformMatrix3fv.
static void skyRotation(const s32 game[3], const s32 cam[3], float out[9])
{
	auto mat = [](const s32 a[3], float m[3][3]) {
		const float k = (float)(2 * M_PI / 65536);
		const float ch = std::cos(a[0] * k), sh = std::sin(a[0] * k);
		const float cp = std::cos(a[1] * k), sp = std::sin(a[1] * k);
		const float cr = std::cos(a[2] * k), sr = std::sin(a[2] * k);
		// Ry(h) * Rx(p) * Rz(r)
		m[0][0] = ch * cr + sh * sp * sr; m[0][1] = -ch * sr + sh * sp * cr; m[0][2] = sh * cp;
		m[1][0] = cp * sr;                m[1][1] = cp * cr;                 m[1][2] = -sp;
		m[2][0] = -sh * cr + ch * sp * sr; m[2][1] = sh * sr + ch * sp * cr; m[2][2] = ch * cp;
	};
	float g[3][3], h[3][3];
	mat(game, g);
	mat(cam, h);
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
		{
			float v = 0;
			for (int k = 0; k < 3; k++)
				v += h[k][i] * g[k][j];		// (H^T G)[i][j]
			out[j * 3 + i] = v;
		}
}

static void setSky(const float rot[9], float skyMinZ, float skyMaxZ, float zscreen)
{
	auto set = [&](GLuint program) {
		if (program == 0)
			return;
		glcache.UseProgram(program);
		GLint loc = glGetUniformLocation(program, "ht4q_skyRot");
		if (loc != -1)
			glUniformMatrix3fv(loc, 1, GL_FALSE, rot);
		loc = glGetUniformLocation(program, "ht4q_sky");
		if (loc != -1)
			glUniform3f(loc, skyMinZ, skyMaxZ, zscreen);
	};
	for (auto& it : gl.shaders)
		set(it.second.program);
	set(gl.modvol_shader.program);
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
	glBindVertexArray(0);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(vao);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	gl.ofbo.framebuffer->bind();

	eyes.depthFbo[eye] = depthFbo[eye];
	eyes.depthNearZ = DEPTH_FAR;	// reversed: depth 0 is far
	eyes.depthFarZ = DEPTH_NEAR;
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
	FramePose fp;
	const u32 poseId = gl.rendContext->ht4qPoseId;
	const bool havePose = framePose(poseId, fp);
	{
		static const float identity[9] { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
		float rot[9];
		if (havePose && fp.rotValid && stereoParams().skyCorrect != 0.f)
		{
			skyRotation(fp.gameRot, fp.camRot, rot);
			setSky(rot, SKY_MIN_Z, SKY_MAX_Z, fp.zscreen);
		}
		else
			setSky(identity, 0.f, 0.f, 320.f);
	}
	if (mode == Mode::Off || gl.ofbo.framebuffer == nullptr)
	{
		DrawStrips();
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
	setStereo(v, hudShrink);
	DrawStrips();
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
	setStereo(v, hudShrink);
	DrawStrips();
	const float zero[4] {};
	setStereo(zero);
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

}	// namespace ht4q
