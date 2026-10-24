/*
	HydroThunder4Quest hooks into the Flycast core. Keep call sites to one line each.
*/
#pragma once
#include "types.h"

struct MapleInputState;
struct rend_context;

namespace ht4q
{
// Called on the emulator thread right after the maple bus polls local input (once per poll,
// ~60 Hz). Applies memserver overrides/patches and updates the camera hook.
void onMaplePoll(MapleInputState inputState[4]);

// Head pose relative to the game camera, right-handed like OpenXR: x right, y up, z back (game
// units); degrees, +yaw = turn left, +pitch = look up, +roll = tilt left.
struct HeadPose {
	float x, y, z;
	float yaw, pitch, roll;
};
// While active, replaces the mouse as the camera hook's head pose source.
void setHeadPoseOverride(bool active, const HeadPose& pose);

// Headset head pose in OpenXR LOCAL space (meters), set by the XR presenter every frame.
struct XrHeadPose {
	float q[4];		// x, y, z, w
	float p[3];
};
void setXrHead(bool valid, const XrHeadPose& head, float ipdMeters);

// The head pose a game frame's camera was built with.
struct FramePose {
	u32 id;
	bool immersive;		// head-driven race camera: present as a VR projection
	XrHeadPose head;
	float eyeOffset;	// half the IPD, game units
	float zscreen;		// game projection distance for a 640 px wide image
	bool rotValid;		// gameRot/camRot recorded at start of render
	bool skyLevel;		// the game built a level sky (heading only) for this frame: the renderer adds pitch/roll
	s32 gameRot[3];		// heading, pitch, roll (bradians) the game's sky is drawn for
	s32 camRot[3];		// ... and what the hooked camera actually used
	float camXfm[12];	// its world -> camera transform (Xfm_t.mf), for frame dumps
	float headRot[9];	// head rotation relative to the unhooked camera (row-major, x right, y up, z back)
};
// Emu thread, when the game starts a render: id of the pose its camera used (0 = none).
u32 onStartRender();
bool framePose(u32 id, FramePose& out);
// Render thread, before the translucent polygons [first, end) are sorted: puts the sun and lens
// flare behind the scenery during races (core/ht4q/sun.cpp).
void fixSunDepth(rend_context& ctx, u32 first, u32 end);

// Headset controllers, raw (Quest Touch), read by the XR presenter every frame. core/ht4q/controls.cpp
// turns them into the Dreamcast pad with the player's mapping (launcher settings).
enum XrInput { XI_A, XI_B, XI_X, XI_Y, XI_MENU, XI_LSTICK, XI_RSTICK, XI_LTRIGGER, XI_RTRIGGER,
	XI_LGRIP, XI_RGRIP, XI_COUNT };
struct XrHand {
	bool tracked;	// aim pose valid
	float q[4];		// aim pose in OpenXR LOCAL space: x, y, z, w (-z points forward)
	float p[3];
	float stick[2];	// thumbstick, x right, y up, -1..1
};
struct XrControllers {
	float value[XI_COUNT];	// 0..1 (buttons 0 or 1)
	XrHand hand[2];			// left, right
};
void setXrControllers(bool active, const XrControllers& c);
bool xrControllers(XrControllers& out);	// false while the headset has no input focus

// Dreamcast rumble (Puru Puru pack) for the controllers' haptics: the latest command, seq
// increments with each.
struct Haptics {
	u32 seq;
	float amplitude;	// 0..1, 0 = stop
	u32 durationMs;
};
void onVibration(u32 port, float power, float inclination, u32 durationMs);
Haptics haptics();

// Startup screen (core/ht4q/launcher.cpp), drawn by Flycast's GUI before the game boots.
bool launcherOpen();
void drawLauncher();
// 60 fps patches (core/ht4q/framerate.cpp), applied by frameRatePoll on the emu thread
bool frameRate60();
void setFrameRate60(bool on);
// 2-player races with a full screen per player (core/ht4q/twoplayer.cpp): the game draws player 2's
// view 480 px below player 1's, the renderer draws each one (ht4q_stereo.cpp)
bool twoPlayerFull();
void setTwoPlayerFull(bool on);
constexpr float PLAYER2_SHIFT = 480.f;
// While the view is being adjusted (game paused by us): a panel with the values, drawn with Flycast's
// OSD into the screen image at this rectangle (fractions of the screen: x, y, width, height). In the
// headset the XR presenter shows that part of the screen in front of the eyes.
constexpr float ADJUST_PANEL[4] { 0.25f, 0.29f, 0.5f, 0.42f };
void drawAdjustPanel();

// Stereo shift parameters (see core/rend/gles/ht4q_stereo.cpp). Plain floats: written by the
// memserver thread, read by the render thread; a torn update only lasts one frame.
struct StereoParams {
	float sep;		// per-eye shift at infinity, NDC
	float conv;		// convergence depth, game units
	float hudMinZ;	// 1/w at or above which a vertex counts as HUD
	float hudDepth;	// depth the HUD is pinned to, game units
	float unitsPerMeter;	// game world scale (head tracking, IPD)
	float hudShrinkVr;		// VR: HUD scaled toward the view center by 1 - this
	float skyCorrect;		// VR sky re-projection: game sky px per radian (see ht4q_stereo.cpp), 0: off
	float vrFovDeg;			// horizontal field of view the game renders with in the headset (0: game's 90)
};
StereoParams& stereoParams();

// internal
void memserverPoll(MapleInputState inputState[4]);
u32 readRam32(u32 addr);
bool adjustingView();		// controls.cpp: the view is being adjusted (game paused by us)
void toggleAdjust();		// controls.cpp: as if the adjust button was pressed (test hook)
float depthEffectFactor();	// controls.cpp: eye separation factor
void applyXrInput(MapleInputState inputState[4]);
void frameRatePoll();
void twoPlayerPoll();
}
