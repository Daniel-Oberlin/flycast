/*
	HydroThunder4Quest: 18 Wheeler: American Pro Trucker. The image we have is the ReviveDC CD-R
	rebuild (game ID RDC-0094, IP.BIN V1.500 2012-05-08; game 1999-2000); all RAM addresses in this
	folder are for it. The retail GD-ROM has a different ID and is not checked.
*/
#pragma once
#include "../game.h"

namespace ht4q::eighteenwheeler
{
void camhookPoll();		// camhook.cpp
u32 startRender();
bool inStage();			// eighteenwheeler.cpp: a stage is running (its clock is set), paused or not
}
