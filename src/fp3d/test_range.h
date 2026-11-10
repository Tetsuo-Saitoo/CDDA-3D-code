#pragma once
#ifndef CATA_SRC_FP3D_TEST_RANGE_H
#define CATA_SRC_FP3D_TEST_RANGE_H

// K7 test range (PLAN.md K7): a generated world with every weapon, item, vehicle, monster, terrain and furniture laid
// out in zones, monsters frozen, and a small hub with what the tests need. The CDDA simulation still decides every
// outcome; the range only builds the scene and offers a few operations (spawn, teleport, time, heal, AI freeze).
//
// World: `fp3d-range`, quiet world options (no cities, monster spawns, hordes, random NPCs or monster evolution), a
// fixed seed and start time. The avatar value `fp3d_range` marks a range game (saves keep it, but a loaded save is
// never a range game: see range_active). Zones lie 8 OMT apart on one row, so only one is in the reality bubble at a
// time; each uses at most 120x120 squares around its centre square (the north-west corner of its centre OMT),
// z -1..+1. A zone is built the first time the player teleports there, or again on rebuild_zone (avatar value
// `fp3d_range_built_<id>`): the area is cleared (floor, solid ground below, open air above, no furniture, items,
// vehicles, traps, fields or creatures; creatures outside the area stay, though the loaded bubble reaches 12 squares
// past it) and the content placed. A build runs with CDDA's RNG seeded from the boot
// seed and the zone id (the game's own RNG stream put back after), so a zone comes out the same every time.
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
    size_t first = 0;            // the slice of the kind's content list this zone shows (lanes and kitchen: 0, and
    size_t count = 0;            // the number of their targets or props)
    bool built = false;
    // The last build in this session (0 before one): what it placed of `count` and what it left out (didn't fit,
    // refused by CDDA, or not safe to lay out); placed + skipped == count. Not kept in saves.
    size_t placed = 0;
    size_t skipped = 0;
    double build_ms = 0.0;
};

// Content the registry was sized from (loaded data, mods included).
struct range_counts {
    size_t guns = 0;
    size_t melee = 0;            // the weapons category's items but the throwables (the armory shows them too)
    size_t throwables = 0;       // the weapons category's thrown weapons and bombs
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
// The armory's list: guns, then melee weapons, then throwables (each sorted by id); a zone shows a slice of it.
constexpr size_t armory_items_per_zone = 1000;
constexpr size_t hall_items_per_zone = 2500;     // ~5000 squares with aisles
constexpr size_t lot_vehicles_per_zone = 25;
constexpr size_t pen_monsters_per_zone = 100;
constexpr int max_spawn_count = 1000;
constexpr int max_advance_minutes = 10080;       // a week
constexpr size_t max_monster_id = 64;
// The range clock stops this many days after the start of the game (about 10 years): set_time_of_day and advance
// refuse to go past it, far from the end of CDDA's 32-bit turn counter.
constexpr int max_range_days = 3650;
// Teleport: when the zone centre is taken or unsafe (see teleport_to_zone), the player arrives on the nearest free
// square at most this far from it.
constexpr int arrival_search_squares = 10;
// Zone ids are at most max_zone_id characters; a halls zone's id is its base (hall_id_bases) plus "_NN".
constexpr size_t max_zone_id = 64;
constexpr size_t max_hall_id_base = 54;

// Sim side, after boot_new_game_data( "fp3d-range", true, headless ): seeds CDDA's RNG, makes the world quiet, fixes
// the start date (spring, day 2, 10:00), creates a fixed avatar, marks the game as a range, starts it in the hub, builds
// the hub and freezes the AI. Throws std::runtime_error on failure. Follow with do_turn().
void boot_test_range( unsigned seed );
// Forgets the zone registry, ends the range session and unfreezes the AI; every boot does it first (boot.cpp).
void reset_test_range();
// True in a game boot_test_range started (game thread). The avatar value alone (a save, a mod) doesn't make one.
bool range_active();

// The zones (game thread), hub first, the rest sorted by id, ids unique; built on first use from the loaded data
// (the same data gives the same zones). Empty when not in a range game: a loaded save never is one, whatever its
// avatar values say (range_active needs the session boot_test_range starts).
const std::vector<range_zone> &range_zones();
// The id base of each item category's halls zones, in order: "halls_" plus the category id in lower case ('_' for
// anything but a-z, 0-9 and '_'), at most max_hall_id_base characters. Unique: a base already taken (two
// categories that differ only in case or punctuation, or only past the cap) gets "-2", "-3", ..., cut to stay
// within the cap. Either thread (no game state).
std::vector<std::string> hall_id_bases( const std::vector<std::string> &categories );
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
// are one square east of it (z0 -> z1). Plants get a seed under them and open air a wall below, so they stay as they
// are when the submaps load again. Game thread.
struct gallery_piece {
    std::string id;
    bool furniture = false;
    range_offset at;
};
std::vector<gallery_piece> gallery_pieces( size_t zone );
constexpr range_offset gallery_stairs{ 1, 0, 0 };

// The layout of an armory, halls, lots or pens zone (empty for other kinds or a bad index), in the order of its
// slice, for builders and tests. Game thread. Every zone keeps its centre square free (the arrival square).
//  - armory: `what` "gun", "melee" or "thrown", one item type per table square (a gun with its default magazine
//    loaded, or its ammo loaded when it takes no magazine, and a stack of its default ammo); each group starts a
//    new row.
//  - halls: one item type per display rack square.
//  - lots: a vehicle prototype; `at` is its (0,0) mount (facing east), `min`..`max` its bay (inclusive), painted
//    round; bays never overlap each other or the centre. A prototype that doesn't fit the zone gets no piece.
//  - pens: a monster type standing on `at` in a reinforced glass cell whose inside is `min`..`max`, a glass roof
//    over the cell and its walls (z 1: flyers stay in, daylight comes through); `what` names
//    the cell's floor when it isn't concrete: "water" (deep, for a monster that lives in water), "dirt" (for a
//    digger) or "shade" (an indoor floor, for a monster the sun kills). Ordered by species, then id; large monsters
//    get cells 2 squares wide, huge ones 3. Cells only south of the centre.
// Tables and racks stand in rows with walkable aisles between them (rows y % 3 == 0 and columns x % 14 == 0).
struct zone_piece {
    std::string id;
    range_offset at;
    range_offset min;
    range_offset max;
    std::string what;
};
std::vector<zone_piece> zone_pieces( size_t zone );

// The target lanes: a painted shooting line through the centre (y 0), lanes 4 squares apart running north, a wall
// behind them (y lane_backstop_y). Each lane has one target `distance` squares north of its shooting square: a
// zombie (frozen like every range monster) or a practice target (furniture). Either thread (constant).
struct lane_target {
    range_offset shooter;
    range_offset target;
    int distance = 0;
    bool monster = false;
    const char *id = "";
};
const std::vector<lane_target> &lane_targets();
constexpr int lane_backstop_y = -53;

// The kitchen's props (as hub_props): two wood stoves, a fireplace and a brazier lit (firewood and fire), an oven, a
// fridge with food, a sink with a bucket of clean water, counters with pots, pans, food and water containers.
// Either thread (constant).
const std::vector<hub_prop> &kitchen_props();

// Operations (game thread). Squares are absolute (tripoint_abs_ms), like the bridge's ui_point. All refuse
// ("Only between turns.") unless called from the before-ask hook of a main-loop (timed) request (in_timed_hook):
// never in the middle of an action, never from another thread. They also refuse outside range games and once the
// player is dead. The client asks for them through fp3d/cheats.h (request_range_op).
// `count` copies (or charges) of the item with session itype index `itype` (snapshot / id tables) on `square`, as
// many as fit there (nothing spills onto the squares around); "Nothing was placed." when none did.
std::string spawn_item( uint32_t itype, int count, const ui_point &square );
// A monster `mtype` (id, at most 64 characters) on `square`: in the bubble, passable, nobody there.
std::string spawn_monster( const std::string &mtype, const ui_point &square );
// Moves the player to zone `index` (range games only) and builds it the first time. Refused while riding. The player
// lets go of what it grabbed, stops hauling and leaves the vehicle it is in; its auto-move route and the client's
// queued actions are dropped. It arrives on the zone centre, or when that isn't free on the nearest free square
// (arrival_search_squares). Free: passable, a floor to stand on (not open air, not deep water), no field (fire,
// smoke, ...), no trap, no vehicle part, nobody there. With none free it goes back where it was (in the vehicle it
// was in) and the teleport is refused.
std::string teleport_to_zone( size_t index );
// Clears zone `index` and builds it again (range games only). Refused while the player is in that zone, rides or is
// in a vehicle. The zone's area is loaded for the build and the player's surroundings again after it (like two
// teleports, but the player stays on its square): it lets go of what it grabbed and stops hauling, and its auto-move
// route is dropped. Creatures outside the zone's area stay.
std::string rebuild_zone( size_t index );
// Jumps forward to the next `minute` of the day (0..1439): today if still ahead, else tomorrow; the current minute
// (to the turn) changes nothing. Refused past max_range_days after the start.
std::string set_time_of_day( int minute );
// Waits `minutes` (1..10080) as real turns: CDDA's wait activity, which the main loop runs turn by turn (fast-forward,
// interrupted like any wait). Returns once it is started. Refused when it would end past max_range_days after the
// start.
std::string advance( int minutes );
// Monsters and NPCs skip their turns (no plans, no moves, no attacks); they still take damage and die. Not kept in
// saves.
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
