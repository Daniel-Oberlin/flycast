/*
	HydroThunder4Quest hooks into the Flycast core. Keep call sites to one line each.
*/
#pragma once

struct MapleInputState;

namespace ht4q
{
// Apply the memserver's virtual-controller override (if active) to controller port A.
// Called right after the maple bus polls local input.
void overrideInput(MapleInputState inputState[4]);
}
