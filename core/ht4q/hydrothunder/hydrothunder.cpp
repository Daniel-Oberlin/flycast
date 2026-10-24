/*
	HydroThunder4Quest: Hydro Thunder's entry in the game table (core/ht4q/game.h).
*/
#include "hydrothunder.h"

namespace ht4q::hydrothunder
{

static constexpr u32 GAME_PAUSED = 0x8c259a08;		// 1 while the game's pause screen is up

static void poll()
{
	frameRatePoll();
	tracksPoll();
	camhookPoll();
}

static bool paused()
{
	return readU32(GAME_PAUSED) == 1;
}

const Game game {
	"T-9702N", "Hydro Thunder",
	GF_FPS60 | GF_TRACKS,
	// conv, hudMinZ, unitsPerMeter (a guess: feet - eye 7.5 units above the boat, chase camera ~18
	// behind), skyCorrect (the sky moves 270 px per radian of camera turn)
	20.f, 0.75f, 3.28f, 270.f,
	poll, startRender, racing, paused, fixSunDepth,
	{ "Boost (DC A)", "DC B", "DC X", "Change view (DC Y)" },
	"5069ed0e",
};

}	// namespace ht4q::hydrothunder
