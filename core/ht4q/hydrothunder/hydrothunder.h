/*
	HydroThunder4Quest: Hydro Thunder (USA) (Rev 1), game ID T-9702N - all RAM addresses in this
	folder are for that build.
*/
#pragma once
#include "../game.h"

namespace ht4q::hydrothunder
{
void camhookPoll();			// camhook.cpp
u32 startRender();
bool racing();				// a race is running (the camera follows a boat)
void frameRatePoll();		// framerate.cpp
void tracksPoll();			// tracks.cpp
void fixSunDepth(rend_context& ctx, u32 first, u32 end);	// sun.cpp
}
