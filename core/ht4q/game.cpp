/*
	HydroThunder4Quest: the game table (game.h) and the core hooks that dispatch to the current game.
*/
#include "game.h"
#include "stream.h"
#include "emulator.h"
#include "hw/pvr/ta_ctx.h"

namespace ht4q
{

const Game *const games[] { &hydrothunder::game, &crazytaxi::game, &eighteenwheeler::game };
const size_t gameCount = std::size(games);

const Game *gameById(const std::string& id)
{
	for (const Game *g : games)
		if (id == g->id)
			return g;
	return nullptr;
}

const Game *currentGame()
{
	static std::string lastId;
	static const Game *last;
	const std::string& id = settings.content.gameId;
	if (id != lastId)
	{
		lastId = id;
		last = gameById(id);
		if (last != nullptr)
		{
			applyGameStereoDefaults(*last);
			NOTICE_LOG(COMMON, "ht4q: hooks for %s (%s)", last->title, last->id);
		}
	}
	return last;
}

void onMaplePoll(MapleInputState inputState[4])
{
	applyXrInput(inputState);
	streamPoll(inputState);		// the remote viewer's controller on port B
	memserverPoll(inputState);	// scripted virtual controller wins
	twoPlayerPoll();
	if (const Game *g = currentGame())
		g->poll();
}

void onStartRender(rend_context& rend)
{
	const Game *g = currentGame();
	rend.ht4qPoseId = g != nullptr ? g->startRender() : 0;
	rend.ht4qPoseId2 = g != nullptr && g->startRender2 != nullptr && twoPlayerFull() ? g->startRender2() : 0;
}

void fixTranslucent(rend_context& ctx, u32 first, u32 end)
{
	const Game *g = currentGame();
	if (g != nullptr && g->fixTranslucent != nullptr)
		g->fixTranslucent(ctx, first, end);
}

bool inGameplay()
{
	const Game *g = currentGame();
	return g != nullptr && g->inGameplay();
}

bool gamePaused()
{
	const Game *g = currentGame();
	return g != nullptr && g->paused();
}

bool adjustPauses()
{
	const Game *g = currentGame();
	return g != nullptr && (g->features & GF_ADJUST_PAUSE) != 0;
}

}	// namespace ht4q
