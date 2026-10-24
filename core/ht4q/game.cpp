/*
	HydroThunder4Quest: the game table (game.h) and the core hooks that dispatch to the current game.
*/
#include "game.h"
#include "emulator.h"

namespace ht4q
{

const Game *const games[] { &hydrothunder::game, &crazytaxi::game };
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
	memserverPoll(inputState);	// scripted virtual controller wins
	if (const Game *g = currentGame())
		g->poll();
}

u32 onStartRender()
{
	const Game *g = currentGame();
	return g != nullptr ? g->startRender() : 0;
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

}	// namespace ht4q
