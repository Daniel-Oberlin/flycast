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
u32 startRender2();			// player 2's pose in 2-player races
bool racing();				// a race is running (the camera follows a boat)
void frameRatePoll();		// framerate.cpp
void tracksPoll();			// tracks.cpp
bool twoPlayerFull(bool on);	// twoplayer.cpp
void fixSunDepth(rend_context& ctx, u32 first, u32 end);	// sun.cpp
}
