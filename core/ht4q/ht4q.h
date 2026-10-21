/*
	HydroThunder4Quest hooks into the Flycast core. Keep call sites to one line each.
*/
#pragma once

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

// internal
void memserverPoll(MapleInputState inputState[4]);
}
