/*
	HydroThunder4Quest: head pose sources, shared by the games' camera hooks.

	Highest priority first:
	  - memserver 'H' override (scripted tests)
	  - the headset during gameplay on Quest (setXrHead) - "immersive"
	  - mouse position over the game window (HT4Q_MOUSEHEAD = both | look | lean | off, default both
	    on the desktop, off on Quest)

	Every pose a camera hook hands to its game is stored under an id (storeFramePose); the hook
	reports which id the game rendered with (Game::startRender), so each rendered frame knows its
	head pose (framePose).
*/
#include "game.h"
#include "hw/sh4/sh4_mem.h"
#include "input/mouse.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>

namespace ht4q
{

static std::mutex frameMutex;
static FramePose frames[64];
static u32 poseSeq;

static std::mutex headMutex;
static bool headOverride;
static HeadPose overridePose;

void setHeadPoseOverride(bool active, const HeadPose& pose)
{
	std::lock_guard<std::mutex> lock(headMutex);
	headOverride = active;
	overridePose = pose;
}

static std::mutex xrMutex;
static bool xrValid;
static XrHeadPose xrHead;
static float xrIpd;

void setXrHead(bool valid, const XrHeadPose& head, float ipdMeters)
{
	std::lock_guard<std::mutex> lock(xrMutex);
	xrValid = valid;
	xrHead = head;
	xrIpd = ipdMeters;
}

// LOCAL-space head pose -> camera-relative HeadPose (degrees, game units). Euler order matches
// R = Ry(yaw) Rx(pitch) Rz(roll).
static HeadPose headFromXr(const XrHeadPose& h, float unitsPerMeter)
{
	const float x = h.q[0], y = h.q[1], z = h.q[2], w = h.q[3];
	const float m02 = 2 * (x * z + w * y), m12 = 2 * (y * z - w * x), m22 = 1 - 2 * (x * x + y * y);
	const float m10 = 2 * (x * y + w * z), m11 = 1 - 2 * (x * x + z * z);
	constexpr float deg = 180.f / (float)M_PI;
	HeadPose p;
	p.yaw = std::atan2(m02, m22) * deg;
	p.pitch = std::asin(std::clamp(-m12, -1.f, 1.f)) * deg;
	p.roll = std::atan2(m10, m11) * deg;
	p.x = h.p[0] * unitsPerMeter;
	p.y = h.p[1] * unitsPerMeter;
	p.z = h.p[2] * unitsPerMeter;
	return p;
}

bool framePose(u32 id, FramePose& out)
{
	if (id == 0)
		return false;
	std::lock_guard<std::mutex> lock(frameMutex);
	out = frames[id % std::size(frames)];
	return out.id == id;
}

u32 storeFramePose(FramePose fp)
{
	std::lock_guard<std::mutex> lock(frameMutex);
	fp.id = ++poseSeq == 0 ? ++poseSeq : poseSeq;
	frames[fp.id % std::size(frames)] = fp;
	return fp.id;
}

bool updateFramePose(u32 id, const std::function<void(FramePose&)>& f)
{
	if (id == 0)
		return false;
	std::lock_guard<std::mutex> lock(frameMutex);
	FramePose& fp = frames[id % std::size(frames)];
	if (fp.id != id)
		return false;
	f(fp);
	return true;
}

u8 *ram(u32 addr) { return &mem_b[addr & RAM_MASK]; }
u32 readRam32(u32 addr) { return readU32(addr); }

const char *env(const char *name, const char *def)
{
	const char *v = getenv(name);
	return v != nullptr && v[0] != '\0' ? v : def;
}

bool installStub(u32 addr, u32 size, const void *code, u32 codeSize)
{
	if (memcmp(ram(addr), code, codeSize) == 0)
		return true;
	for (u32 i = 0; i < size; i++)
		if (*ram(addr + i) != 0)
			return false;
	memcpy(ram(addr), code, codeSize);	// emu thread: the dynarec invalidates code it compiled from here
	return true;
}

static HeadPose mousePose()
{
#ifdef HT4Q_XR
	static const std::string mode = env("HT4Q_MOUSEHEAD", "off");	// Quest: the headset drives it
#else
	static const std::string mode = env("HT4Q_MOUSEHEAD", "both");
#endif
	HeadPose p{};
	if (mode == "off" || mo_x_prev[0] == -1)	// no mouse motion seen yet
		return p;
	float nx = std::clamp((mo_x_abs[0] - 320) / 320.f, -1.f, 1.f);
	float ny = std::clamp((mo_y_abs[0] - 240) / 240.f, -1.f, 1.f);
	if (mode == "both" || mode == "lean")
	{
		p.x = 2.f * nx;
		p.y = -1.f * ny;
	}
	if (mode == "both" || mode == "look")
	{
		p.yaw = -40.f * nx;
		p.pitch = -25.f * ny;
	}
	return p;
}

HeadSample sampleHead(bool gameplay)
{
	HeadSample s{};
	{
		std::lock_guard<std::mutex> lock(xrMutex);
		s.xrValid = xrValid;
		s.immersive = xrValid && gameplay;
		s.xr = xrHead;
		s.eyeOffset = xrIpd / 2 * stereoParams().unitsPerMeter * depthEffectFactor();
	}
	HeadPose target;
	bool immediate;
	{
		std::lock_guard<std::mutex> lock(headMutex);
		immediate = headOverride || s.immersive;
		target = headOverride ? overridePose
				: s.immersive ? headFromXr(s.xr, stereoParams().unitsPerMeter)
				: mousePose();
	}
	// light smoothing for the mouse; scripted and headset poses apply immediately
	static HeadPose cur{};
	float a = immediate ? 1.f : 0.35f;
	for (int i = 0; i < 6; i++)
		(&cur.x)[i] += a * ((&target.x)[i] - (&cur.x)[i]);
	s.pose = cur;

	constexpr float rad = (float)M_PI / 180.f;
	const float cy = std::cos(cur.yaw * rad), sy = std::sin(cur.yaw * rad);
	const float cp = std::cos(cur.pitch * rad), sp = std::sin(cur.pitch * rad);
	const float cr = std::cos(cur.roll * rad), sr = std::sin(cur.roll * rad);
	const float r[9] = {
		cy * cr + sy * sp * sr, -cy * sr + sy * sp * cr, sy * cp,
		cp * sr, cp * cr, -sp,
		-sy * cr + cy * sp * sr, sy * sr + cy * sp * cr, cy * cp,
	};
	memcpy(s.headRot, r, sizeof(r));
	return s;
}

}	// namespace ht4q
