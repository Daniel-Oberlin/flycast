/*
	HydroThunder4Quest: run Hydro Thunder at 60 fps instead of 30 (option ht4q.FrameRate60).

	Why the game runs at 30 (retail Rev 1, found with the memserver's pc sampling):
	- its graphics library (Kamui) processes a finished frame only every 2nd vblank: the vblank
	  handler counts [KAMUI_VSYNC_DIV] down and skips while it isn't negative. The game sets it to 1.
	- the game logic advances one fixed step per frame: the motion integrator computes
	  dt = frames * 1/30 from a literal (INTEGRATOR_DT), and the timers add the Gameloop frame time.
	The patches, each written only over the value we expect (and put back when the option is off):
	1. divider 1 -> 0: a frame every vblank, 60 fps.
	2. Gameloop timing block (fps, frame time, fields per frame) -> 60: timers and countdowns keep
	   real time.
	3. integrator step 1/30 -> 1/60: boats move at the right speed.
	Measured on the desktop: boat distance against real time within 3% of 30 fps over 24 s (the
	velocity damping, applied once per step, now runs twice as often).
*/
#include "hydrothunder.h"
#include "cfg/option.h"
#include <cmath>
#include <cstring>

namespace ht4q
{

static config::Option<bool, false> FrameRate60("FrameRate60", false, "ht4q");

static constexpr u32 KAMUI_VSYNC_DIV = 0x8c3cf23c;	// vblanks skipped per frame
static constexpr u32 GAMELOOP_TIMING = 0x8c1fdab4;	// fTargetFPS, nApproxTargetFPS, fTargetFrameTime,
													// nTargetFieldsPerFrame, fTargetFieldsPerFrame
static constexpr u32 INTEGRATOR_DT = 0x8c05d8c0;	// literal: seconds per frame step

struct Timing { float fps; u32 approxFps; float frameTime; u32 fields; float fieldsF; };
static const Timing timing30 { 30.f, 30, 1.f / 30, 2, 2.f };
static const Timing timing60 { 60.f, 60, 1.f / 60, 1, 1.f };

// Writes `to` over `from` (4 bytes), only if `from` or `to` is there; false when neither is.
static bool swap32(u32 addr, u32 from, u32 to)
{
	u32 v;
	memcpy(&v, ram(addr), 4);
	if (v == to)
		return true;
	if (v != from)
		return false;
	memcpy(ram(addr), &to, 4);	// emu thread: the dynarec invalidates code it compiled from here
	return true;
}

static bool sameTiming(const Timing& a, const Timing& b)
{
	// the game's frame time is 1/30 rounded down one ulp, so compare loosely
	return a.approxFps == b.approxFps && a.fields == b.fields && std::abs(a.frameTime - b.frameTime) < 1e-6f;
}

bool frameRate60() { return FrameRate60; }
void setFrameRate60(bool on) { FrameRate60.set(on); }

void hydrothunder::frameRatePoll()
{
	static bool applied;
	const bool on = FrameRate60;
	if (!on && !applied)
		return;
	// the game's own constants, as found in Rev 1
	const u32 dt30 = 0x3d088888, dt60 = 0x3c888888;
	if (!swap32(INTEGRATOR_DT, on ? dt30 : dt60, on ? dt60 : dt30))
		return;		// not the expected code (other build, or not loaded yet)
	Timing t;
	memcpy(&t, ram(GAMELOOP_TIMING), sizeof(t));
	if (sameTiming(t, on ? timing30 : timing60))
		memcpy(ram(GAMELOOP_TIMING), on ? &timing60 : &timing30, sizeof(t));
	swap32(KAMUI_VSYNC_DIV, on ? 1 : 0, on ? 0 : 1);
	if (applied != on)
		NOTICE_LOG(COMMON, "ht4q: %d fps", on ? 60 : 30);
	applied = on;
}

}	// namespace ht4q
