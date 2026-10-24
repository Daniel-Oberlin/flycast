/*
	HydroThunder4Quest: the games we have hooks for, and what each one provides.

	Everything game-specific (RAM addresses, code patches, menu knowledge) lives in a folder per game
	(core/ht4q/hydrothunder/, core/ht4q/crazytaxi/) behind this table; the rest of core/ht4q, the
	stereo renderer and the XR presenter are shared and ask the current game through it.
*/
#pragma once
#include "types.h"
#include "ht4q.h"
#include <cstring>
#include <functional>
#include <string>

struct rend_context;

namespace ht4q
{

enum GameFeature : u32 {
	GF_FPS60 = 1,		// launcher: 60 fps option (frameRate60)
	GF_TRACKS = 2,		// launcher: Tracks tab (unlockedTracks)
	GF_ADJUST_PAUSE = 4,	// the view adjustment pauses the game (its pause screen keeps drawing the scene)
};

struct Game {
	const char *id;			// disc product number (IP.BIN), as Flycast reports it (settings.content.gameId)
	const char *title;
	u32 features;			// GameFeature
	// stereo defaults for this game's scale (env variables still override them, see params.cpp)
	float conv, hudMinZ, unitsPerMeter, skyCorrect;
	// emu thread, once per maple poll while the game runs: install and refresh its patches
	void (*poll)();
	// emu thread, when the game starts a render: the head pose its camera used (FramePose id, 0 = none)
	u32 (*startRender)();
	// in normal play with the head driving the camera (not menus, replays...)
	bool (*inGameplay)();
	// the game's own pause screen is up (view adjustment pauses with Start, see controls.cpp)
	bool (*paused)();
	// TA thread, before the translucent polygons [first, end) of a pass are sorted (may be null)
	void (*fixTranslucent)(rend_context& ctx, u32 first, u32 end);
	// controller function labels for the Dreamcast buttons A, B, X, Y (null: "DC A"...)
	const char *buttonLabels[4];
	// texture hash of the logo on its title screen, for the launcher (null: the title as text)
	const char *logoTexture;
};

// All games with hooks
extern const Game *const games[];
extern const size_t gameCount;
// The game running now (null: none, or one we have no hooks for)
const Game *currentGame();
const Game *gameById(const std::string& id);

// Main RAM access for the game modules (emu thread)
u8 *ram(u32 addr);
inline u32 readU32(u32 addr) { u32 v; memcpy(&v, ram(addr), 4); return v; }
inline void writeU32(u32 addr, u32 v) { memcpy(ram(addr), &v, 4); }
inline float readF32(u32 addr) { float v; memcpy(&v, ram(addr), 4); return v; }
// Copies code into RAM at addr if that area (size bytes) is all zero or already holds it.
bool installStub(u32 addr, u32 size, const void *code, u32 codeSize);
const char *env(const char *name, const char *def);

// Head pose for the game modules (core/ht4q/headpose.cpp): the current pose from the highest
// priority source (memserver override, headset during gameplay, mouse), smoothed for the mouse.
struct HeadSample {
	HeadPose pose;
	bool immersive;		// headset in use and gameplay: present as a VR projection
	bool xrValid;		// headset in use (menus too)
	XrHeadPose xr;
	float eyeOffset;	// half the IPD, game units
	float headRot[9];	// pose rotation R = Ry(yaw) Rx(pitch) Rz(roll), row-major
};
HeadSample sampleHead(bool gameplay);
// Stores fp under a new id (returned, never 0) for framePose().
u32 storeFramePose(FramePose fp);
// Calls f on the stored pose with this id (under the lock); false if it is gone.
bool updateFramePose(u32 id, const std::function<void(FramePose&)>& f);

namespace hydrothunder { extern const Game game; }
namespace crazytaxi { extern const Game game; }

}	// namespace ht4q
