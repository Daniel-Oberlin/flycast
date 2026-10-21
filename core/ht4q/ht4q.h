/*
	HydroThunder4Quest hooks into the Flycast core. Keep call sites to one line each.
*/
#pragma once
#include "types.h"

struct MapleInputState;

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
	s32 gameRot[3];		// heading, pitch, roll (bradians) the game's sky is drawn for
	s32 camRot[3];		// ... and what the hooked camera actually used
};
// Emu thread, when the game starts a render: id of the pose its camera used (0 = none).
u32 onStartRender();
bool framePose(u32 id, FramePose& out);

// Dreamcast pad state from the headset's controllers: pressed = DC button bits (core/input/gamepad.h),
// triggers 0..0xffff, stick -32768..32767.
struct XrPadState {
	u32 pressed;
	u16 lt, rt;
	s16 x, y;
};
void setXrInput(bool active, const XrPadState& pad);

// Stereo shift parameters (see core/rend/gles/ht4q_stereo.cpp). Plain floats: written by the
// memserver thread, read by the render thread; a torn update only lasts one frame.
struct StereoParams {
	float sep;		// per-eye shift at infinity, NDC
	float conv;		// convergence depth, game units
	float hudMinZ;	// 1/w at or above which a vertex counts as HUD
	float hudDepth;	// depth the HUD is pinned to, game units
	float unitsPerMeter;	// game world scale (head tracking, IPD)
	float hudShrinkVr;		// VR: HUD scaled toward the view center by 1 - this
	float skyCorrect;		// 1: rotate the sky backdrop with the head (see ht4q_stereo.cpp), 0: game's own
	float vrFovDeg;			// horizontal field of view the game renders with in the headset (0: game's 90)
};
StereoParams& stereoParams();

// internal
void memserverPoll(MapleInputState inputState[4]);
void applyXrInput(MapleInputState inputState[4]);
}
