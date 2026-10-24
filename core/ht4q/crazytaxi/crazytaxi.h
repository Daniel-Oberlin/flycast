/*
	HydroThunder4Quest: Crazy Taxi. The image we have is the ReviveDC CD-R rebuild (game ID
	RDC-0102, IP.BIN V1.004 2012-07-07; game code dated 1999); all RAM addresses in this folder are
	for it. The retail GD-ROM (MK-51035) is a different ID and is not checked.
*/
#pragma once
#include "../game.h"

namespace ht4q::crazytaxi
{
void camhookPoll();		// camhook.cpp
u32 startRender();
bool driving();			// the driving camera is on
}
