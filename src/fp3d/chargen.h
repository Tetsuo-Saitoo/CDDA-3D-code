#pragma once
#ifndef CATA_SRC_FP3D_CHARGEN_H
#define CATA_SRC_FP3D_CHARGEN_H

// FP3D character creation without CDDA's curses tabs: the sim sends a catalog (scenarios, professions,
// backgrounds, traits, skills, point rules) as one `chargen` request, the client draws its own screens and
// answers with the choices as JSON, and the sim checks and applies them. See contracts/ui_requests.md.

#include <string>
#include <vector>

class avatar;

namespace fp3d
{
// Character templates in CDDA's templates folder (plain names only, files of at most 256 KiB, sorted, at most
// 256).
std::vector<std::string> list_templates();

// The catalog as JSON. Needs the world's game data loaded (boot_new_game_data). Run on the sim thread.
std::string chargen_catalog_json();

// Checks the choices (JSON, contracts/ui_requests.md "chargen") and, if they are all valid, builds `u` from them
// (scenario, profession, backgrounds, stats, traits, skills, name, gender, age, height) and calls
// avatar::initialize. A transfer template ("Character to Template") loads as saved, without initialize, as in
// CDDA. Returns "" on success, otherwise why not; `u` is then in an unspecified state.
// The choices come from the client: every id, number and string is checked.
std::string apply_chargen( avatar &u, const std::string &choices_json );

// Asks the client for a character (a `chargen` request, re-asked with `status` set to the problem until the
// answer is valid) and builds the avatar. False if the client cancelled. Throws shutdown_signal on shutdown.
bool run_chargen();
} // namespace fp3d

#endif // CATA_SRC_FP3D_CHARGEN_H
