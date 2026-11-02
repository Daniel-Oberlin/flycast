/*
	HydroThunder4Quest: Crazy Taxi's entry in the game table (core/ht4q/game.h).
*/
#include "crazytaxi.h"

namespace ht4q::crazytaxi
{

static void poll()
{
	camhookPoll();
}

static bool paused()
{
	return false;
}

const Game game {
	"RDC-0102", "Crazy Taxi",
	0,
	// conv: about 6.5 m; hudMinZ: the HUD is drawn at 1/w 3.3..20, the green arrow over the taxi (a 3D
	// model just in front of the camera) at 1.2..1.7, the world below ~0.8; unitsPerMeter: the taxi
	// ('59 Cadillac, ~5.7 x 2.0 m) is ~52 x 18 units; no sky correction
	60.f, 1.1f, 9.f, 0.f,
	poll, startRender, driving, paused, nullptr,
	{ nullptr, nullptr, nullptr, nullptr },
	"610fc05e 375e7192 faab19a9 1150f2f1",	// the title logo's top row: emblem, CRA, ZY TA, XI
};

}	// namespace ht4q::crazytaxi
