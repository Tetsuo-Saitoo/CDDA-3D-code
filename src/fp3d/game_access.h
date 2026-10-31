#pragma once
#ifndef CATA_SRC_FP3D_GAME_ACCESS_H
#define CATA_SRC_FP3D_GAME_ACCESS_H

#include "game.h"

// FP3D: the one friend of `game` (declared in game.h). Keep this the only way fp3d code reaches game
// internals, so the upstream diff in game.h stays one line.
class fp3d_game_access
{
    public:
        // The real new-game path (calendar, start location, starting items, game mode).
        static bool start_game( game &g ) {
            return g.start_game();
        }
        // Loads a saved character into the active world; call g.setup() first (as the main menu does).
        static bool load_game( game &g, const save_t &save ) {
            return g.load( save );
        }
};

#endif // CATA_SRC_FP3D_GAME_ACCESS_H
