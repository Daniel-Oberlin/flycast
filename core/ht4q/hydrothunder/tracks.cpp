/*
	HydroThunder4Quest: open tracks the player hasn't won yet (option ht4q.UnlockTracks, a bit per
	group, set on the launcher's Tracks tab).

	The game keeps what is unlocked in Tracks_LockState (retail Rev 1 0x8c26b8d0): two copies, for
	1 and 2 players (picked by the SPLITSCREEN flag), each six longs - medium tracks, hard tracks,
	and the four bonus tracks (Nile Adventure, Hydro Speedway, Catacomb, Castle Von Dandy). The
	track menu reads it when it opens, and the game's progress save stores it, like its own
	name-entry unlock codes do.
	The poll sets the chosen flags whenever they are clear: the game clears them when it loads the
	save at boot. Unticking a group stops that but doesn't clear a flag (it may have been won).
*/
#include "hydrothunder.h"
#include "cfg/option.h"

namespace ht4q
{

static config::Option<int, false> UnlockTracks("UnlockTracks", 0, "ht4q");

static constexpr u32 LOCK_STATE = 0x8c26b8d0;		// long[2][6]
static constexpr u32 LOCK_STATE_LITERAL = 0x8c07cdb8;	// the game's pointer to it (build check)

int unlockedTracks() { return UnlockTracks; }
void setUnlockedTracks(int groups) { UnlockTracks.set(groups & ((1 << TG_COUNT) - 1)); }

void hydrothunder::tracksPoll()
{
	const int groups = UnlockTracks;
	if (groups == 0)
		return;
	if (readU32(LOCK_STATE_LITERAL) != LOCK_STATE)
		return;		// not Rev 1, or not loaded yet
	for (int copy = 0; copy < 2; copy++)
		for (int g = 0; g < TG_COUNT; g++)
		{
			u8 *flag = ram(LOCK_STATE + (copy * TG_COUNT + g) * 4);
			const s32 one = 1;
			if ((groups & (1 << g)) && memcmp(flag, "\0\0\0\0", 4) == 0)
				memcpy(flag, &one, 4);
		}
}

}	// namespace ht4q
