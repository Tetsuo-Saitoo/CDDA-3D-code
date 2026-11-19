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
//   unstoppable      avatar var `fp3d_cheat_unstoppable`: no grab holds the player once they act (break_grabs at the
//                    start of every action, unstoppable_acts), and walking into a hostile monster or enemy NPC
//                    pushes past it instead of attacking (push_past); attacks need the attack actions then
// The test kit: a big backpack worn, a loaded pistol, shotgun and rifle with spare magazines and ammo, a machete,
// tools, bandages, food and water; what doesn't fit lands at the avatar's feet.
// Requests are applied in the main loop between turns (bridge.h job_action), also while paused.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "coordinates.h"
#include "fp3d/bridge.h"

class avatar;
class Character;

namespace fp3d
{
enum class cheat : uint8_t {
    invincible, no_needs, no_weight_limit, unlimited_ammo, stamina, no_temperature, night_vision, unstoppable, count
};
constexpr size_t cheat_count = static_cast<size_t>( cheat::count );
const char *cheat_name( cheat c );
// The cheat with exactly this name; nullopt for anything else.
std::optional<cheat> cheat_from_name( const std::string &name );

// Client side. Cheats and test kits are refused until the client allows them (the test range, or the player's
// own debug switch); off at start. Turning it off drops requests not applied yet. Cheats already on stay on.
void set_cheats_allowed( bool on );
bool cheats_allowed();
// Between two games: not allowed, nothing asked, no range operation or result, no kit given, nothing published as
// on (seq and range operation ids keep counting).
void reset_cheats();
// Client side. Ask for a change at the main loop's next request; false when not allowed or after shutdown. The
// first cheat turned on (or kit given) turns the game's achievements off, as CDDA's debug menu does.
bool request_cheat( cheat c, bool on );
bool request_test_kit();

// The test range's operations (fp3d/test_range.h), asked for like cheats. They run in the before-ask hook of the
// main loop's next request (run_range_requests), also while paused; the game refuses them outside range games.
// point_teleport is the teleport cheat, in any game while cheats are allowed: the player goes to `square` (absolute
// map square) in the loaded bubble, or to the nearest free square near it (fp3d/arrival.h); refused while riding or
// in a vehicle. The first one turns the game's achievements off.
// weather is the weather cheat, in any game while cheats are allowed: `weather` names a loaded weather type (as the
// id tables list it, id_tables.h) and the sky holds it; "" gives the weather back to the game (in a range game: its
// clear sky, test_range.h keep_range_weather). `wind` holds the wind speed (mph, 0..max_cheat_wind); -1 lets the
// game decide (a range game holds it still). The temperature is never touched. Refused in a world whose weather
// never changes (CDDA's ETERNAL_WEATHER option). CDDA saves neither: a loaded game decides again.
// break_grabs is the one-shot "Break all grabs", in any game while cheats are allowed (the unstoppable cheat on or
// off): every grab on the player and their mount lets go (break_grabs below); done also when nothing held them.
enum class range_op_kind : uint8_t {
    spawn_item, spawn_monster, teleport, time_of_day, advance, ai_frozen, heal, rebuild_zone, point_teleport, weather,
    break_grabs, spawn_vehicle
};
constexpr size_t max_weather_id = 64;
constexpr int max_cheat_wind = 100;    // mph, about 45 m/s
struct range_op {
    range_op_kind kind = range_op_kind::heal;
    uint32_t itype = 0;      // spawn_item: session itype index (id tables)
    // spawn_item count, teleport and rebuild_zone zone index, time_of_day minute, advance minutes, ai_frozen 0/1
    int64_t number = 0;
    ui_point square;         // spawn_item, spawn_monster, point_teleport, spawn_vehicle
    std::string monster;     // spawn_monster: mtype id
    std::string vehicle;     // spawn_vehicle: vehicle prototype id; its facing (degrees, 0..359) is `number`
    std::string weather;     // weather: a weather type id, "" = the game's own
    int wind = -1;           // weather: mph 0..max_cheat_wind, -1 = the game decides
};
constexpr size_t max_range_ops_waiting = 32;
constexpr size_t max_range_results = 16;
// Client side: the operation's id (1, 2, ...); 0 when cheats aren't allowed, max_range_ops_waiting already wait,
// the monster id is longer than test_range.h's max_monster_id, the vehicle id than its max_vehicle_proto_id or
// the weather id than max_weather_id, or after shutdown. Disallowing cheats drops what waits.
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
    std::string weather;           // the weather the sky is held at ("" = the game decides; a range game: "clear")
    int wind = -1;                 // the wind speed held, mph (-1 = the game decides)
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

// Sim side: every grab on `u` lets go the way CDDA's own break-free lets go when its roll is won
// (Character::try_remove_grab): the grabber next to them drops that body part (monster::remove_grab) and the
// grabbed effect goes; a grab with no grabber next to them just goes, as do the grabs on their mount. Returns how
// many went.
int break_grabs( Character &u );
// Sim side, at the start of each action the player takes (handle_action; not a timeout): break_grabs while
// unstoppable is on.
void unstoppable_acts( avatar &u );
// Sim side, from avatar_action::move as the player steps into `dest` (next to them). With unstoppable on, a
// hostile monster or an enemy NPC there is pushed past: the player takes its square at a normal step's cost and
// it takes the square they left (a swim into deep water: the nearest free square next to it, which is also where
// it goes when the square left won't take it); no attack. Not pushed, so CDDA's bump (an attack): a monster CDDA
// never displaces (IMMOBILE, CANNOT_MOVE, harnessed, ridden), an enemy NPC when either stands on a vehicle part or
// either is mounted, anything while the player can't move or rides a fast vehicle.
enum class push_result : uint8_t {
    none,      // unstoppable off, nothing hostile there, or nothing it may push: CDDA's own step (a bump attack)
    refused,   // no room to push it aside, or the swap's step failed: said why
    clear,     // the step goes on without attacking: CDDA's displacement takes the monster to the square left,
               // or it was moved aside already
    done       // the player swapped places with the NPC already (game::swap_critters, a normal step)
};
push_result push_past( avatar &u, const tripoint_bub_ms &dest );
// Whether push_past would try (unstoppable on, a step on one level, a hostile monster or enemy NPC there that it may
// push, as above); changes
// nothing. avatar_action::move asks it to treat the step as a move, not an attack (move_effects).
bool pushes_past( const avatar &u, const tripoint_bub_ms &dest );
} // namespace fp3d

#endif // CATA_SRC_FP3D_CHEATS_H
