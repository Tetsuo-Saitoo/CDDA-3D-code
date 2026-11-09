#pragma once
#ifndef CATA_SRC_FP3D_BOOT_H
#define CATA_SRC_FP3D_BOOT_H

#include <string>

#include "coordinates.h"

namespace fp3d
{
// Sets CDDA's data directory (`base_path`, "" = the current directory) and user directory (config, saves,
// debug.log) and creates the user subfolders. boot_world() calls it with "" and "./fp3d_user" unless it
// already ran. Throws std::runtime_error on failure.
void init_paths( const std::string &base_path, const std::string &user_dir );

// Loads game data and creates a fresh world `world_name` (replacing one of that name) under the user directory,
// with a random avatar. Follow with g->start_game() to play, or load_bubble_in_town() to just look.
// Run it on the thread that will own the game. Throws std::runtime_error on failure.
// `headless` also turns off options that make input poll on a timer (animations) and creates CDDA's window,
// which stays hidden as long as the bridge is active: call fp3d::set_active( true ) first.
void boot_world( const std::string &world_name, bool headless );

// boot_world's first half, for character creation (chargen.h): loads game data and, with `fresh_world`, creates
// world `world_name` (replacing one of that name), otherwise uses the existing world of that name (throws if
// there is none). The avatar is blank: build it (run_chargen / avatar::create), then g->start_game().
// `world_setup` (fresh worlds only) asks the client for the world's settings and mods first (worldgen.h).
void boot_new_game_data( const std::string &world_name, bool fresh_world, bool headless,
                         bool world_setup = false );

// Loads game data and the saved character `save_name` (its save id, i.e. the character name) from the existing
// world `world_name` under the user directory, the way CDDA's main menu loads a game (g->setup(), then
// game::load). Deletes nothing. Follow with do_turn() to play. Run it on the thread that will own the game.
// Throws std::runtime_error if the world or the save doesn't exist or the load fails. `headless` as above.
void boot_saved_game( const std::string &world_name, const std::string &save_name, bool headless );

// CDDA's own exit path (exit_handler in main.cpp) plus SDL_Quit, on the thread that ran boot_world. Call it before
// the process or DLL exits after a headless boot: CDDA's atexit( SDL_Quit ) otherwise runs before its static tile
// caches free their textures (a crash), and inside DllMain it deadlocks on SDL's timer thread. Debug messages
// afterwards only go to the log.
void shutdown_world();

// Loads the reality bubble centred on the nearest city and puts the avatar in its middle.
// Returns the bubble origin (absolute submap).
tripoint_abs_sm load_bubble_in_town();
} // namespace fp3d

#endif // CATA_SRC_FP3D_BOOT_H
