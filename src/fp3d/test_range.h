#pragma once
#ifndef CATA_SRC_FP3D_TEST_RANGE_H
#define CATA_SRC_FP3D_TEST_RANGE_H

// K7 test range (PLAN.md K7): a generated world with every weapon, item, vehicle, monster, terrain and furniture laid
// out in zones, monsters frozen, and a small hub with what the tests need. The CDDA simulation still decides every
// outcome; the range only builds the scene and offers a few operations (spawn, teleport, time, heal, AI freeze).
//
// World: `fp3d-range`, quiet world options (no cities, monster spawns, hordes or random NPCs), a fixed seed and start
// time. The avatar value `fp3d_range` marks a range game (saves keep it). Zones lie 8 OMT apart on one row, so only
// one is in the reality bubble at a time; each uses at most 120x120 squares around its centre square (the north-west
// corner of its centre OMT), z -1..+1. A zone is built the first time the player gets there (avatar value
// `fp3d_range_built_<id>`): the area is cleared (floor, solid ground below, open air above, no furniture, items,
// vehicles, traps, fields or creatures) and the content placed.
//
// Everything here runs on the game thread, except range info (latest_range_info) and ai_frozen(). Operations
// validate their own input (it comes from the client: hostile) and return "" on success, else why not.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "coordinates.h"
#include "fp3d/bridge.h"

namespace fp3d
{
// A square relative to a zone's centre square (x east, y south, z up).
struct range_offset {
    int x = 0;
    int y = 0;
    int z = 0;
};

enum class zone_kind : uint8_t { hub, gallery, armory, halls, lots, pens, lanes, kitchen };
const char *zone_kind_name( zone_kind k );

struct range_zone {
    std::string id;              // "hub", "gallery_01", "halls_food_02", ...
    std::string name;            // display name
    zone_kind kind = zone_kind::hub;
    tripoint_abs_omt centre;
    int part = 0;                // 0-based part of its kind (halls: of its category)
    int parts = 1;
    std::string category;        // halls: the item category id
    size_t first = 0;            // the slice of the kind's content list this zone shows
    size_t count = 0;
    bool built = false;
};

// Content the registry was sized from (loaded data, mods included).
struct range_counts {
    size_t guns = 0;
    size_t items = 0;            // other items (not guns, not pseudo items)
    std::vector<std::pair<std::string, size_t>> items_per_category;   // sorted by category id
    size_t vehicles = 0;         // prototypes
    size_t monsters = 0;
    size_t terrain = 0;          // without t_null
    size_t furniture = 0;        // without f_null, f_clear
    size_t emitters = 0;         // terrain and furniture that emit fields (the gallery's isolated rows)
};

// Zone layout constants.
constexpr int zone_spacing_omt = 8;
constexpr int zone_half_squares = 60;            // a zone uses centre - 60 .. centre + 59
constexpr size_t armory_guns_per_zone = 400;
constexpr size_t hall_items_per_zone = 2500;     // ~5000 squares with aisles
constexpr size_t lot_vehicles_per_zone = 25;
constexpr size_t pen_monsters_per_zone = 100;
constexpr int max_spawn_count = 1000;
constexpr int max_advance_minutes = 10080;       // a week
constexpr size_t max_monster_id = 64;

// Sim side, after boot_new_game_data( "fp3d-range", true, headless ): seeds CDDA's RNG, makes the world quiet, fixes
// the start date (spring, day 2, 10:00), creates a fixed avatar, marks the game as a range, starts it in the hub, builds
// the hub and freezes the AI. Throws std::runtime_error on failure. Follow with do_turn().
void boot_test_range( unsigned seed );
// Forgets the zone registry and unfreezes the AI; every boot does it first (boot.cpp).
void reset_test_range();
// True when the avatar is a range game's (game thread).
bool range_active();

// The zones (game thread), hub first, the rest sorted by id; built on first use from the loaded data, so a loaded
// range save gets the same zones. Empty when not in a range game.
const std::vector<range_zone> &range_zones();
range_counts range_content_counts();
// The zone the avatar stands in, -1 if none.
int current_zone();
tripoint_abs_ms zone_centre( const range_zone &z );

// The hub's props, for builders and tests: what, where (from the hub's centre square) and the CDDA id.
enum class prop_kind : uint8_t { terrain, furniture, item, monster, vehicle, field };
struct hub_prop {
    const char *what;
    range_offset at;
    prop_kind kind;
    const char *id;
    int count = 1;                 // items: copies or charges; fields: intensity; vehicles: fuel percent
    const char *contents = "";     // items: what is inside (a gun: its ammo, in its default magazine)
    int contents_count = 0;
};
const std::vector<hub_prop> &hub_props();

// The pieces of gallery zone `zone` (empty if it isn't a gallery): every terrain and furniture id on its own square
// with space around; emitters in their own rows at the south edge. The player arrives on the centre square; stairs up
// are one square east of it (z0 -> z1). Game thread.
struct gallery_piece {
    std::string id;
    bool furniture = false;
    range_offset at;
};
std::vector<gallery_piece> gallery_pieces( size_t zone );
constexpr range_offset gallery_stairs{ 1, 0, 0 };

// Operations (game thread). Squares are absolute (tripoint_abs_ms), like the bridge's ui_point. All but
// set_ai_frozen refuse ("Only between turns.") unless called from the before-ask hook of a main-loop (timed)
// request: never in the middle of an action, never from another thread.
// `count` copies (or charges) of the item with session itype index `itype` (snapshot / id tables) on `square`.
std::string spawn_item( uint32_t itype, int count, const ui_point &square );
// A monster `mtype` (id, at most 64 characters) on `square`: in the bubble, passable, nobody there.
std::string spawn_monster( const std::string &mtype, const ui_point &square );
// Moves the player to zone `index` (range games only) and builds it the first time.
std::string teleport_to_zone( size_t index );
// Jumps forward to the next `minute` of the day (0..1439): today if still ahead, else tomorrow.
std::string set_time_of_day( int minute );
// Waits `minutes` (1..10080) as real turns: CDDA's wait activity, which the main loop runs turn by turn (fast-forward,
// interrupted like any wait). Returns once it is started.
std::string advance( int minutes );
// Monsters and NPCs skip their turns (no plans, no moves, no attacks); they still take damage and die.
std::string set_ai_frozen( bool on );
// Full hit points, no pain, no bad effects, fed, watered, rested, full stamina.
std::string heal();

// Either thread.
bool ai_frozen();

// Range info for the client: captured on the game thread before each request (cheap when nothing changed), read from
// any thread.
struct range_zone_info {
    std::string id;
    std::string name;
    std::string kind;
    bool built = false;
};
struct range_info {
    uint64_t seq = 0;             // bumps when anything below changes
    bool range = false;
    bool ai_frozen = false;
    std::vector<range_zone_info> zones;
    int zone = -1;                // index the player is in, -1 none
};
void capture_range_info();
range_info latest_range_info();
} // namespace fp3d

#endif // CATA_SRC_FP3D_TEST_RANGE_H
