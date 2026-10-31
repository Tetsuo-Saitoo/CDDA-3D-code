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
};

#endif // CATA_SRC_FP3D_GAME_ACCESS_H
