#pragma once
#ifndef CATA_SRC_FP3D_DATA_CHECK_H
#define CATA_SRC_FP3D_DATA_CHECK_H

#include <optional>
#include <string>
#include <vector>

#include "type_id.h"

namespace fp3d
{

// CDDA checks the consistency of all loaded game data after every load (DynamicDataLoader::check_consistency,
// 6-9 s of each boot). FP3D skips that check when the very same data already passed it: the same game version,
// mod list and data files (path, size and modification time of every file in the core folder and in each
// mod's folder). The fingerprints of data that passed are kept in the user's config folder; a missing or
// garbled file means check again. Mods from the user folder, or a world's own mods folder, are checked every
// time. The player's own SKIP_VERIFICATION option still wins.
// Not a security check: skipping only drops CDDA's consistency report for data that already passed it.

// Call right before the world's data is loaded (with the world active): sets CDDA's SKIP_VERIFICATION in
// memory when this data already passed.
void prepare_data_check( const std::vector<mod_id> &mods );
// Call right after the data is loaded and finalized: remembers the data as passed if the check ran and CDDA
// reported no error.
void finish_data_check();
// Whether the last prepare_data_check skipped the check (for tests and the boot log).
bool data_check_skipped();

// The fingerprint of `mods` (as load_world_modfiles loads them), or nullopt when the data must be checked
// every time (a mod from outside the data folder, a world mods folder, too many files, an unreadable folder).
std::optional<std::string> data_fingerprint( const std::vector<mod_id> &mods );
// The marker file's path.
std::string data_check_marker_path();

} // namespace fp3d

#endif // CATA_SRC_FP3D_DATA_CHECK_H
