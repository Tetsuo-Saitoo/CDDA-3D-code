#pragma once
#ifndef CATA_SRC_FP3D_ARRIVAL_H
#define CATA_SRC_FP3D_ARRIVAL_H

// FP3D: moving the player somewhere without walking there (the test range's zone teleport, the teleport cheat):
// what to drop before it, where to arrive, what to refresh after it. Sim thread only, between turns.

#include <optional>

#include "coords_fwd.h"

class avatar;

namespace fp3d
{
// Before the bubble is loaded elsewhere under the player, as CDDA's own moves that put you elsewhere (game.cpp drops
// the grab there): let go of what is grabbed (it stays behind), stop hauling (the hauled items' references die with
// the bubble), forget the auto-move route.
void stop_moving( avatar &u );
// Before the player is taken away from where it stands: stop_moving, and the client's queued actions, meant for the
// old place, dropped too.
void leave_square( avatar &u );
// Where the player arrives when the area is loaded: `centre` when it is free, else the nearest free square at most
// arrival_search_squares (test_range.h) from it (ring by ring). Free: in the bubble, passable, a floor to stand on
// (not open air, not deep water), no field (fire, smoke, acid, ...), no trap, no vehicle part, no monster or NPC (the
// avatar itself, the one moving, doesn't count). nullopt if there is none.
std::optional<tripoint_abs_ms> arrival_square( const tripoint_abs_ms &centre );
// After the avatar moved within the loaded bubble: recentre it and refresh what the turn would.
void settle_avatar();
} // namespace fp3d

#endif // CATA_SRC_FP3D_ARRIVAL_H
