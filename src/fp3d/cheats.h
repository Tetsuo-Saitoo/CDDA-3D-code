#pragma once
#ifndef CATA_SRC_FP3D_CHEATS_H
#define CATA_SRC_FP3D_CHEATS_H

// FP3D: a test loadout and cheats the player can turn on for testing (client calls, only while the bridge is active).
// Every cheat is off in every game until the player turns it on; it then stays with the character (CDDA traits and
// an avatar var, which saves keep) until turned off. Built on CDDA's own debug traits:
//   invincible       DEBUG_NODMG (takes no damage) + DEBUG_PREVENT_DEATH
//   no_needs         DEBUG_LS (hunger, thirst and sleepiness held stable)
//   no_weight_limit  FP3D_CHEAT_CARRY (data/json/mutations/fp3d_cheats.json: +10,000 kg carry weight)
//   unlimited_ammo   avatar var `fp3d_cheat_ammo`: the wielded gun is refilled every turn (its loaded ammo, else
//                    its default ammo; a magazine-fed gun gets its default magazine when it has none)
//   stamina          DEBUG_STAMINA + DEBUG_CARDIO
//   no_temperature   DEBUG_NOTEMP
//   night_vision     DEBUG_NIGHTVISION (also reveals the map)
// The test kit: a big backpack worn, a loaded pistol, shotgun and rifle with spare magazines and ammo, a machete,
// tools, bandages, food and water; what doesn't fit lands at the avatar's feet.
// Requests are applied in the main loop between turns (bridge.h job_action), also while paused.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fp3d/bridge.h"

class avatar;

namespace fp3d
{
enum class cheat : uint8_t {
    invincible, no_needs, no_weight_limit, unlimited_ammo, stamina, no_temperature, night_vision, count
};
constexpr size_t cheat_count = static_cast<size_t>( cheat::count );
const char *cheat_name( cheat c );
// The cheat with exactly this name; nullopt for anything else.
std::optional<cheat> cheat_from_name( const std::string &name );

// Client side. Cheats and test kits are refused until the client allows them (the test range, or the player's
// own debug switch); off at start. Turning it off drops requests not applied yet. Cheats already on stay on.
void set_cheats_allowed( bool on );
bool cheats_allowed();
// Client side. Ask for a change at the main loop's next request; false when not allowed or after shutdown. The
// first cheat turned on (or kit given) turns the game's achievements off, as CDDA's debug menu does.
bool request_cheat( cheat c, bool on );
bool request_test_kit();

// The test range's operations (fp3d/test_range.h), asked for like cheats. They run in the before-ask hook of the
// main loop's next request (run_range_requests), also while paused; the game refuses them outside range games.
// point_teleport is the teleport cheat, in any game while cheats are allowed: the player goes to `square` (absolute
// map square) in the loaded bubble, or to the nearest free square near it (fp3d/arrival.h); refused while riding or
// in a vehicle. The first one turns the game's achievements off.
enum class range_op_kind : uint8_t {
    spawn_item, spawn_monster, teleport, time_of_day, advance, ai_frozen, heal, rebuild_zone, point_teleport
};
struct range_op {
    range_op_kind kind = range_op_kind::heal;
    uint32_t itype = 0;      // spawn_item: session itype index (id tables)
    // spawn_item count, teleport and rebuild_zone zone index, time_of_day minute, advance minutes, ai_frozen 0/1
    int64_t number = 0;
    ui_point square;         // spawn_item, spawn_monster, point_teleport
    std::string monster;     // spawn_monster: mtype id
};
constexpr size_t max_range_ops_waiting = 32;
constexpr size_t max_range_results = 16;
// Client side: the operation's id (1, 2, ...); 0 when cheats aren't allowed, max_range_ops_waiting already wait,
// the monster id is longer than test_range.h's max_monster_id, or after shutdown. Disallowing cheats drops what waits.
uint64_t request_range_op( const range_op &op );
struct range_op_result {
    uint64_t id = 0;
    std::string error;       // "" = done
};
// Client side: the last max_range_results results, oldest first.
std::vector<range_op_result> latest_range_results();
// Sim side, in the before-ask hook: runs what waits when it's a main-loop request's hook (in_timed_hook).
void run_range_requests();

struct cheats_view {
    uint64_t seq = 0;              // bumps on every publish; 0 = nothing published yet
    uint32_t on = 0;               // bit i: cheat i is on
    uint64_t kits_given = 0;       // test kits handed out since start
};
// Client side.
cheats_view latest_cheats();

// Sim side: applies what the client asked for (input_context runs run_jobs() for job_action).
void run_cheat_requests( avatar &u );
void run_jobs();
// Sim side, once per turn: refills the wielded gun while unlimited_ammo is on.
void cheats_turn( avatar &u );
// Sim side: copies the cheat state for the client (publish_held calls it: before every request and once per turn).
void publish_cheats( const avatar &u );
bool cheat_on( const avatar &u, cheat c );
} // namespace fp3d

#endif // CATA_SRC_FP3D_CHEATS_H
