#pragma once
#ifndef CATA_SRC_FP3D_WORLDGEN_H
#define CATA_SRC_FP3D_WORLDGEN_H

// FP3D world creation without CDDA's curses world generator: the sim sends the world settings (CDDA's
// "world_default" options) and the usable mods as one `world_setup` request, the client draws its own form and
// answers with JSON, and the sim checks it and creates the world. See contracts/ui_requests.md.

#include <string>
#include <vector>

#include "options.h"
#include "type_id.h"

class WORLD;

namespace fp3d
{
// The settings and mods as JSON. Needs options and mods loaded (after world_generator->init()).
std::string world_setup_catalog_json();

// Checks the answer {"options": {id: value}, "mods": [ids]} and fills `options` (starting from the world
// defaults) and `mods` (dependencies added, core mods first, dependencies before dependents). Returns "" on
// success, otherwise why not. The answer comes from the client: every id and value is checked.
std::string apply_world_setup( const std::string &json, options_manager::options_container &options,
                               std::vector<mod_id> &mods );

// Asks the client for the settings of new world `name` (re-asked with `status` until valid) and creates it.
// Returns nullptr if the client cancelled. Throws shutdown_signal on shutdown, std::runtime_error if CDDA can't
// write the world.
WORLD *run_world_setup( const std::string &name );
} // namespace fp3d

#endif // CATA_SRC_FP3D_WORLDGEN_H
