#pragma once
#ifndef CATA_SRC_FP3D_GAME_ACCESS_H
#define CATA_SRC_FP3D_GAME_ACCESS_H

#include <ctime>

#include "fp3d/shot.h"
#include "game.h"

// FP3D: the one friend of `game` (declared in game.h). Keep this the only way fp3d code reaches game
// internals, so the upstream diff in game.h stays one line.
class fp3d_game_access
{
    public:
        // The real new-game path (calendar, start location, starting items, game mode).
        static bool start_game( game &g ) {
            fp3d::clear_shot_plan();
            return g.start_game();
        }
        // The new-game half of start_game() on a given square, with no start location, NPCs, monsters or missions
        // (the test range, fp3d/test_range.cpp). Set the calendar first.
        static void start_game_at( game &g, const tripoint_abs_ms &start );
        // Loads a saved character into the active world; call g.setup() first (as the main menu does).
        static bool load_game( game &g, const save_t &save ) {
            fp3d::clear_shot_plan();
            return g.load( save );
        }
        // CDDA's examine of one square (doors, furniture, vehicles, items), as the examine action does it.
        static void examine( game &g, const tripoint_bub_ms &p ) {
            g.examine( p );
        }
        // game::save() plus the counters quicksave() resets. Run it at the main loop, like quicksave.
        static bool save_game( game &g ) {
            if( !g.save() ) {
                return false;
            }
            g.moves_since_last_save = 0;
            g.last_save_timestamp = std::time( nullptr );
            return true;
        }
};

#endif // CATA_SRC_FP3D_GAME_ACCESS_H
