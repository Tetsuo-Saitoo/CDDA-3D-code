#pragma once
#ifndef CATA_SRC_FP3D_BOOT_H
#define CATA_SRC_FP3D_BOOT_H

#include <string>

#include "coordinates.h"

namespace fp3d
{
// Loads game data and creates a fresh world `world_name` (replacing one of that name) under ./fp3d_user,
// with a random avatar. Follow with g->start_game() to play, or load_bubble_in_town() to just look.
// Run it on the thread that will own the game. Throws std::runtime_error on failure.
// `headless` also turns off options that make input poll on a timer (animations) and creates CDDA's window,
// which stays hidden as long as the bridge is active: call fp3d::set_active( true ) first.
void boot_world( const std::string &world_name, bool headless );

// Loads the reality bubble centred on the nearest city and puts the avatar in its middle.
// Returns the bubble origin (absolute submap).
tripoint_abs_sm load_bubble_in_town();
} // namespace fp3d

#endif // CATA_SRC_FP3D_BOOT_H
