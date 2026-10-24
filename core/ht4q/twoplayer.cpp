/*
	HydroThunder4Quest: 2-player games with a full screen per player (option ht4q.TwoPlayerFull).

	The games' split screens are patched (Game::twoPlayerFull, in each game's folder) so that each
	player gets a full 640x480 view, player 2's PLAYER2_SHIFT (480) px below player 1's: the TA
	frame holds both, rows 0-479 and 480-959. The renderer then draws such frames once per player,
	moved up 480 px for player 2 (ht4q_stereo.cpp); tile clips keep the other player's polygons out.
	Player 2's view is what the host streams to the joined headset (stream.cpp).
*/
#include "game.h"
#include "cfg/option.h"

namespace ht4q
{

static config::Option<bool, false> TwoPlayerFull("TwoPlayerFull", false, "ht4q");

bool twoPlayerFull() { return TwoPlayerFull; }
void setTwoPlayerFull(bool on) { TwoPlayerFull.set(on); }

void twoPlayerPoll()
{
	static const Game *appliedGame;
	static bool applied;
	const Game *g = currentGame();
	if (g == nullptr || g->twoPlayerFull == nullptr)
		return;
	const bool on = TwoPlayerFull;
	if (g == appliedGame && on == applied)
		return;
	if (!g->twoPlayerFull(on))
		return;		// not the expected code (other build, or not loaded yet): asked again next time
	NOTICE_LOG(COMMON, "ht4q: %s 2-player %s", g->title, on ? "full screen" : "split screen");
	appliedGame = g;
	applied = on;
}

}	// namespace ht4q
