/*
	HydroThunder4Quest: Crazy Taxi's entry in the game table (core/ht4q/game.h).
*/
#include "crazytaxi.h"

namespace ht4q::crazytaxi
{

static void poll()
{
}

static u32 startRender()
{
	return 0;
}

static bool inGameplay()
{
	return false;
}

static bool paused()
{
	return false;
}

const Game game {
	"RDC-0102", "Crazy Taxi",
	0,
	// conv, hudMinZ, unitsPerMeter, skyCorrect: not tuned yet
	20.f, 0.75f, 1.f, 0.f,
	poll, startRender, inGameplay, paused, nullptr,
	{ nullptr, nullptr, nullptr, nullptr },
	nullptr,
};

}	// namespace ht4q::crazytaxi
