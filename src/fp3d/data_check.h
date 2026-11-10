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
// 6-9 s of each boot). FP3D skips that check when the very same data already passed it: the same game version
// and program file (size and modification time of the bridge library or test program), mod list and data files
// (path, size and modification time of every file in the core folder and in each mod's folder). The fingerprints
// of data that passed are kept in the user's config folder; a missing or garbled file means check again. Mods
// from the user folder, or a world's own mods folder, are checked every time. The player's own SKIP_VERIFICATION
// option still wins.
// Not a security check: skipping only drops CDDA's consistency report for data that already passed it.

// Call right before the world's data is loaded (with the world active): sets CDDA's SKIP_VERIFICATION in
// memory when this data already passed. Also puts the option back if a failed boot never reached finish.
void prepare_data_check( const std::vector<mod_id> &mods );
// Call right after the data is loaded and finalized: remembers the data as passed if the check ran and CDDA
// reported no error.
void finish_data_check();
// Whether the last prepare_data_check skipped the check (for tests and the boot log).
bool data_check_skipped();
// Test programs (CDDA's test_mode) check every boot and remember nothing: they build their scenes on a seeded RNG,
// and CDDA's check draws random numbers, so skipping it would change everything after it. A test of the skip
// itself allows it here for a while.
void allow_data_check_skip_in_tests( bool allowed );

// The fingerprint of `mods` (as load_world_modfiles loads them), or nullopt when the data must be checked
// every time (a mod from outside the data folder, a world mods folder, too many files, an unreadable folder,
// a program file that can't be found).
std::optional<std::string> data_fingerprint( const std::vector<mod_id> &mods );
// The marker file's path.
std::string data_check_marker_path();

} // namespace fp3d

#endif // CATA_SRC_FP3D_DATA_CHECK_H
