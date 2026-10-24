/*
	HydroThunder4Quest: 18 Wheeler's entry in the game table (core/ht4q/game.h).
*/
#include "eighteenwheeler.h"

namespace ht4q::eighteenwheeler
{

static constexpr u32 STAGE_TIME = 0x8c2736b4;	// s32 frames left on the stage clock (60 per second; 0 in the menus)
static constexpr u32 RUNNING = 0x8c403e04;		// u32 1 while the stage runs, 0 on its pause screen

bool inStage()
{
	return (s32)readU32(STAGE_TIME) > 0;
}

static void poll()
{
	camhookPoll();
}

static bool paused()
{
	return inStage() && readU32(RUNNING) == 0;
}

const Game game {
	"RDC-0094", "18 Wheeler",
	GF_ADJUST_PAUSE,	// its pause screen keeps drawing the scene
	// the game's units are metres (the driver's eye is ~2.4 above the road): conv 6.5 m;
	// hudMinZ: the HUD is drawn at 1/w 3.33 (and sprites at 1e17), the cab interior up to ~3.2;
	// no sky correction
	6.5f, 3.25f, 1.f, 0.f,
	poll, startRender, inStage, paused, nullptr,
	{ nullptr, nullptr, nullptr, nullptr },
	nullptr,
};

}	// namespace ht4q::eighteenwheeler
