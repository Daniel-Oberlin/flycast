/*
	HydroThunder4Quest hooks into the Flycast core. Keep call sites to one line each.
*/
#pragma once

struct MapleInputState;

namespace ht4q
{
// Called on the emulator thread right after the maple bus polls local input (once per poll,
// ~60 Hz). Applies the memserver's virtual-controller override and any queued code patches.
void onMaplePoll(MapleInputState inputState[4]);
}
