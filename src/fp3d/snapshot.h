#pragma once
#ifndef CATA_SRC_FP3D_SNAPSHOT_H
#define CATA_SRC_FP3D_SNAPSHOT_H

// FP3D block B3: immutable copies of the reality bubble for the client. See contracts/snapshot.md.
// Captured on the sim thread (capture_snapshot), handed over with publish_snapshot / latest_snapshot.
// Every snapshot lists all submaps of the bubble on the levels it takes (want_snapshot_levels; every level
// by default); ones that didn't change since the previous capture are shared (same pointer, same revisions), so
// a client that only reads the newest snapshot never misses a change.

#include <array>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fp3d/bridge.h"

class Creature;
class map;
class vehicle;

namespace fp3d
{

constexpr uint32_t contract_version = 0;
constexpr int sm_size = 12;                       // SEEX == SEEY
constexpr int sm_tiles = sm_size * sm_size;       // index = y * 12 + x

// tile flags
constexpr uint8_t tile_has_roof = 1;
constexpr uint8_t tile_open_air_below = 2;        // no floor: a hole or open air
constexpr uint8_t tile_has_sign = 4;              // graffiti / sign text
constexpr uint8_t tile_has_vehicle = 8;

// vis values
constexpr uint8_t vis_never = 0;
constexpr uint8_t vis_remembered = 1;
constexpr uint8_t vis_seen_lit = 2;
constexpr uint8_t vis_seen_dark = 3;              // in line of sight but too dark to make out

// item_pile_view::corpse_state bits (CDDA's corpse flags).
constexpr uint16_t corpse_bled = 1, corpse_field_dressed = 2, corpse_dress_failed = 4, corpse_skinned = 8,
                   corpse_quartered = 16, corpse_pulped = 32, corpse_gibbed = 64;

// One item a client can point at (fp3d/item_hands.h): its handle and itype, as an item action names it.
struct item_handle_view {
    uint64_t uid = 0;
    uint32_t type = 0;                            // itype index
    int16_t spot_x = -1;                          // where the client placed it, permille of the square; -1 none
    int16_t spot_y = -1;
    int16_t yaw = -1;                             // how it lies there: whole degrees 0..359; -1 no pose stored
    bool side = false;                            // lies on its side (only with a yaw)
    int8_t shelf = -1;                            // the furniture shelf it was put on (client's 0..15); -1 none
    bool operator==( const item_handle_view &o ) const;
};

struct item_pile_view {
    uint8_t tile = 0;
    uint16_t count = 0;                           // items on the tile
    std::array<uint32_t, 4> top{};                // itype indices, largest volume first; 0 = none
    uint32_t corpse = 0;                          // mtype index of the largest corpse here, 0 = none
    uint16_t corpse_state = 0;                    // its butchery state, corpse_* bits below
    // the first max_handles_per_tile items (max_handles_near_player next to the player), CDDA's stack order
    std::vector<item_handle_view> handles;
    bool operator==( const item_pile_view &o ) const;
};

// L8: an item in a container lying on the player's square or one next to it, one level deep: container pockets only,
// not sealed or forbidden ones (take with `container` takes it out).
constexpr size_t max_inside_per_container = 64;
constexpr size_t max_inside_per_submap = 256;
struct inside_view {
    uint64_t container = 0;                       // the lying container's handle
    uint64_t uid = 0;
    uint32_t type = 0;                            // itype index
    int16_t pocket = 0;                           // index into the container's pockets, as item actions take it
    bool operator==( const inside_view &o ) const;
};

// Live item tiles: what an item shows of its state. Listed for the carried items and for those lying on the player's
// square or one next to it (with what is directly inside them), only when there is something to show.
constexpr size_t max_item_states = 512;
constexpr size_t max_state_contents = 256;       // contents looked at per item
struct item_state_view {
    uint64_t uid = 0;                             // the item's handle
    uint8_t temp = 0;                             // 0 normal, 1 hot, 2 cold, 3 frozen (CDDA's HOT / COLD / FROZEN flags)
    int16_t rot = -1;                             // relative rot in permille, 0..2000; -1 = it doesn't rot. A container:
    // the worst of what is directly inside
    int16_t fill = -1;                            // permille of its CONTAINER pockets' capacity in use, 0..1000; -1 = it
    // has no such pocket
    uint32_t fill_type = 0;                       // itype index of the content with the largest volume, 0 none
    bool see_through = false;                     // one of those pockets is of a see-through material
    int32_t fill_ml = -1;                         // the volume inside, ml; -1 = no such pocket
    int16_t dirt = -1;                            // a gun's fouling 0..10000 (as gun_view::dirt); -1 = not a gun, or
    // one that doesn't foul
};

// Exact numbers for the item lists (G2). One entry for every carried item, in the order of `items`, then for every
// item lying on the player's square or one next to it and for what is directly inside those.
constexpr size_t max_item_amounts = 1024;
constexpr int32_t max_item_amount = 1000000000;   // weight and volume are clamped to this
struct item_amount_view {
    uint64_t uid = 0;                             // the item's handle
    int32_t count = 1;                            // charges when CDDA counts it by charges, else 1; 1..INT32_MAX
    int32_t weight_g = 0;                         // CDDA's weight with everything in it, the whole stack
    int32_t volume_ml = 0;                        // the room it takes where it is, as CDDA counts it (a rigid
    // container: its own size; a soft one: plus its contents), the whole stack
};

// carried_view::flags
constexpr uint8_t carried_worn = 1, carried_wielded = 2, carried_offhand = 4;

// A carried item (worn, wielded or inside one of those), depth first.
struct carried_view {
    uint64_t uid = 0;
    uint32_t type = 0;                            // itype index
    uint64_t parent = 0;                          // the container's uid; 0 = on the body (worn or wielded)
    int16_t pocket = -1;                          // index into the parent's pockets (as item actions take it)
    uint8_t flags = 0;                            // carried_* bits
};

// A square that adds light to CDDA's lightmap (C6): the strongest of its terrain/furniture, item and field sources.
constexpr uint8_t light_fixed = 0;                // terrain or furniture (light_emitted)
constexpr uint8_t light_item = 1;                 // an item lying there (item::getlight)
constexpr uint8_t light_fire = 2;                 // a field (fire and other glowing fields)
constexpr size_t max_lights_per_submap = 16;
struct light_view {
    uint8_t tile = 0;
    uint8_t luminance = 0;                        // CDDA luminance, rounded, clamped to 0..255
    uint8_t kind = light_fixed;
    bool operator==( const light_view &o ) const;
};

struct field_view {
    uint8_t tile = 0;
    uint32_t type = 0;                            // field type index
    uint8_t intensity = 0;
    uint16_t age_turns = 0;
    bool operator==( const field_view &o ) const;
};

// L9 (fp3d/cooking.h): a square that heats food for CDDA's crafting.
constexpr uint8_t heat_from_fire = 0, heat_from_furniture = 1, heat_from_vehicle = 2;
struct heat_view {
    uint8_t tile = 0;
    uint8_t source = heat_from_fire;              // heat_from_*: a fire alone, furniture, a vehicle part
    uint32_t tool = 0;                            // itype index of the heat pseudo tool; 0 = only a fire
    uint8_t fire = 0;                             // fd_fire intensity on the tile, 0..3
    int32_t charges = 0;                          // what CDDA's crafting inventory gives the tool there (fuel, power)
    bool lit = false;                             // usable for crafting now: a fire burns or the tool has charges
    bool operator==( const heat_view &o ) const;
};

struct cookware_item_view {
    uint64_t uid = 0;                             // handle (item_hands.h)
    uint32_t type = 0;                            // itype index
    int32_t count = 0;                            // charges if counted by charges, else 1
    bool operator==( const cookware_item_view &o ) const;
};

// An item with the COOK or BOIL quality lying on a heat square (top level of the map stack).
struct cookware_view {
    uint8_t tile = 0;
    uint64_t uid = 0;                             // handle, the same as the pile's
    uint32_t type = 0;                            // itype index
    std::vector<cookware_item_view> contents;     // its CONTAINER pockets, one level; cap 16
    uint32_t craft_result = 0;                    // an in-progress craft inside: itype index of the result, 0 none
    std::string craft_recipe;                     // its recipe id, "" none
    uint16_t craft_progress = 0;                  // permille
    bool operator==( const cookware_view &o ) const;
};

struct submap_view {
    ui_point pos;                                 // absolute submap
    uint32_t revision = 0;                        // bumps when ter/furn/trap/flags change: rebuild the mesh
    uint32_t detail_revision = 0;                 // bumps when light/vis/items/fields/heat/cookware change
    std::array<uint32_t, sm_tiles> ter{};
    std::array<uint32_t, sm_tiles> furn{};
    std::array<uint32_t, sm_tiles> trap{};        // only traps the player knows about or sees
    std::array<uint8_t, sm_tiles> light{};        // CDDA light level, clamped to 0..255
    std::array<uint8_t, sm_tiles> vis{};
    std::array<uint8_t, sm_tiles> flags{};
    std::vector<item_pile_view> items;            // tiles seen now only (cap 144)
    std::vector<field_view> fields;               // tiles seen now only (cap 4 per tile)
    std::vector<heat_view> heat;                  // tiles seen now only (cap 32)
    std::vector<cookware_view> cookware;          // on those heat tiles (cap 16)
    std::vector<light_view> lights;               // tiles seen now only, brightest first (cap 16)
    std::vector<inside_view> inside;              // L8: next to the player only (caps above)
    std::vector<std::pair<uint64_t, uint32_t>> inside_more;   // containers cut short: [handle, items left out]
    bool same_shape( const submap_view &o ) const;
    bool same_detail( const submap_view &o ) const;
    // write_snapshot_json's text of this view and the ids it names, made the first time the view is written and
    // kept: a view never changes after its capture. Only write_snapshot_json touches it (under its own mutex).
    struct written_text {
        std::string text;
        uint32_t hash = 0;                        // 32-bit FNV-1a of `text`
        std::vector<uint32_t> ter;                // each list unique and sorted
        std::vector<uint32_t> furn;
        std::vector<uint32_t> trap;
        std::vector<uint32_t> field;
    };
    mutable std::shared_ptr<const written_text> written;
};

struct creature_view {
    uint64_t uid = 0;                             // stable while the creature lives (session-scoped)
    uint8_t kind = 0;                             // 0 player, 1 npc, 2 monster
    uint32_t type = 0;                            // mtype index (monsters) or 0
    ui_point pos;
    float facing_deg = 0;                         // 0 until block D1
    uint32_t move_mode = 0;                       // move_mode index (characters) or 0
    uint8_t hp_frac = 0;                          // 0..255
    uint32_t wielded = 0;                         // itype index (characters) or 0
    uint8_t attitude = 1;                         // to the player: 0 hostile, 1 neutral, 2 friendly (player: 1)
    std::string name;                             // NPCs: their name, cleaned and capped (client_text); else ""
};

struct vehicle_part_view {
    int mount_x = 0;
    int mount_y = 0;
    uint32_t vpart = 0;
    uint8_t hp_frac = 0;
    uint8_t state = 0;                            // bit0 open, bit1 broken, bit2 enabled, bit3 gap filler
    // The square the part is on, from the vehicle's `pos`: CDDA turns the mount by its own stepped rotation around
    // a pivot the client doesn't have, so the client can't work it out. dz: a vehicle on a ramp can have parts a
    // level up or down. Each -max_part_offset..max_part_offset (a vehicle is far smaller).
    int8_t dx = 0;
    int8_t dy = 0;
    int8_t dz = 0;
};
constexpr int max_part_offset = 127;
// A gap filler: while a vehicle stands at an angle CDDA puts a copy of an edge part (a board, a door) on the
// square the diagonal step would leave open, and the game blocks that square like the part itself. Listed after
// the vehicle's own parts, with the copied part's mount, vpart, hp and state, and its own square.
constexpr uint8_t part_gap_filler = 8;

// What a vehicle carries where the player can get at it: the cargo spaces on the avatar's square and the eight
// around it, the squares CDDA's own item menus list as nearby (reached by a clear path). CDDA has one cargo space
// a square (vpart_position::cargo). Items are listed like the map's handles, in CDDA's stack order.
constexpr size_t max_cargo_spaces = 9;
constexpr size_t max_cargo_items = 32;
struct cargo_item_view {
    uint64_t uid = 0;                             // item handle (item_hands.h)
    uint32_t type = 0;                            // itype index
};
struct vehicle_cargo_view {
    uint16_t part = 0;                            // index into the vehicle's `parts`: the cargo part's row
    int32_t used_ml = 0;                          // volume of what is in it
    int32_t capacity_ml = 0;                      // volume it holds
    uint32_t more = 0;                            // items not listed (beyond max_cargo_items)
    std::vector<cargo_item_view> items;           // cap max_cargo_items
};

struct vehicle_view {
    uint64_t uid = 0;
    // CDDA's position of the vehicle, an absolute square: its origin part's until it rolls, then its turning
    // point. Parts are placed from it by their dx, dy, dz.
    ui_point pos;
    float face_deg = 0;
    float turn_deg = 0;
    float velocity = 0;                           // m/s
    std::string name;
    std::vector<vehicle_part_view> parts;         // cap 4096
    std::vector<vehicle_cargo_view> cargo;        // cap max_cargo_spaces; empty unless the avatar is next to one
};

struct worn_view {
    uint32_t type = 0;                            // itype index
    uint8_t layer = 0;                            // highest layer_level (0 personal .. 5 belted/aux)
    uint8_t damage = 0;                           // item::damage_level(): 0 fine .. 5 nearly destroyed
    uint8_t degradation = 0;                      // 0..255 of the itype's max degradation
    std::vector<uint32_t> covers;                 // body_part indices
};

// The wielded gun, for the client's weapon view (combat block 2).
constexpr size_t max_gun_faults = 16;
constexpr size_t max_fault_id_bytes = 64;
// Gun mods by hand (2026-11-17), all on the wielded gun. A mod it has; a slot kind it has now (an installed mod
// can add slots to the type's own, id_tables.h); and for every carried gun mod that isn't in a gun, whether it goes
// on (item_hands.h fit_gunmod).
constexpr uint8_t gun_mod_irremovable = 1;        // can't be taken off: CDDA's IRREMOVABLE, integral mods too
constexpr size_t max_gun_mods = 16;
constexpr size_t max_gun_slots = 16;
constexpr size_t max_gun_fits = 32;
constexpr size_t max_gun_fit_reason_bytes = 80;
struct gun_mod_view {
    uint64_t uid = 0;                             // the mod's item handle, as mod_remove takes it
    uint32_t type = 0;                            // itype index
    std::string location;                         // gunmod_location_text
    uint8_t flags = 0;                            // gun_mod_irremovable
};
struct gun_slot_view {
    std::string location;
    uint8_t slots = 0;
    uint8_t free = 0;
};
struct gun_fit_view {
    uint64_t uid = 0;                             // a carried gun mod, as `items` lists it
    bool ok = false;
    uint8_t chance = 0;                           // CDDA's odds without tools, 0..100
    uint8_t risk = 0;
    std::string reason;                           // CDDA's text when it doesn't fit; "" when it does
};

struct gun_view {
    uint32_t type = 0;                   // itype index
    int16_t dirt = -1;                   // CDDA's fouling 0..10000; -1 for a NON_FOULING gun
    int32_t rounds = -1;                 // loaded rounds; -1 for a gun that takes no ammo
    std::vector<std::string> faults;     // fault ids it has (cap max_gun_faults, ids up to max_fault_id_bytes)
    bool suppressed = false;             // a gunmod (built-in too) lowers its report, as item::gun_noise counts it
    std::vector<gun_mod_view> mods;      // cap max_gun_mods, CDDA's order (item::gunmods)
    std::vector<gun_slot_view> slots;    // cap max_gun_slots
    std::vector<gun_fit_view> fits;      // cap max_gun_fits
};

// An effect on the player, for the HUD: what CDDA's @ screen lists (effects with no display name are left out).
constexpr size_t max_player_effects = 64;
constexpr size_t max_effect_id_bytes = 64;
constexpr size_t max_effect_name_columns = 40;
struct effect_view {
    std::string id;                      // effect_type id, e.g. "drunk", "bleed"; up to max_effect_id_bytes
    std::string name;                    // CDDA's display name, color tags removed; up to max_effect_name_columns
    uint32_t body_part = 0;              // body_part index; 0 = the whole body
    uint8_t intensity = 1;
    int8_t rating = 0;                   // -1 bad, 0 neutral, 1 good, 2 mixed
    int32_t seconds_left = -1;           // -1 permanent
};

// Text from game data (names from mods, item names) as the client gets it: color tags removed, at most
// `max_columns` wide, no control characters, at most `max_bytes` long and never cut inside a UTF-8 character.
std::string client_text( const std::string &raw, size_t max_columns, size_t max_bytes );

// CDDA's morale screen, for the HUD's mood line.
constexpr size_t max_mood_entries = 12;
constexpr size_t max_mood_name_columns = 60;
constexpr size_t max_mood_face_bytes = 16;
struct mood_entry {
    std::string name;                    // the morale screen's line, color tags removed; up to max_mood_name_columns
    int16_t value = 0;                   // its morale points now (decay and temper applied), never 0
    uint8_t percent = 0;                 // its share of its side (good or bad), cut to a whole number as the
    // morale screen shows it; 0..100
};
struct mood_view {
    int16_t level = 0;                   // CDDA's morale level
    int8_t band = 0;                     // -3 .. 3 (mood_band)
    std::string face;                    // CDDA's mood face for this character and level; up to max_mood_face_bytes
    uint16_t total = 0;                  // how many entries CDDA has (entries may hold fewer)
    std::vector<mood_entry> entries;     // largest |value| first, then by name; cap max_mood_entries
};
// The band of a morale level, by the thresholds of CDDA's default mood face: >= 100: 3, >= 50: 2, >= 10: 1,
// >= -10: 0, >= -50: -1, >= -100: -2, lower: -3.
int8_t mood_band( int level );

// What the character is busy with, for a "time passes" line while the game runs on by itself.
struct activity_view {
    std::string id;                               // CDDA's activity type id ("ACT_WAIT"); letters, digits and _,
                                                  // up to max_activity_id_bytes ("" if it is anything else)
    std::string verb;                             // CDDA's verb for it ("waiting"); up to max_activity_verb_columns
};
constexpr size_t max_activity_id_bytes = 64;
constexpr size_t max_activity_verb_columns = 40;

// The vehicle the player is on, for a driving HUD: only numbers CDDA itself computes and shows. Captured only
// while the player is boarded, so snapshots on foot don't grow.
constexpr size_t max_vehicle_engines = 16;
constexpr size_t max_vehicle_fuels = 16;
constexpr int max_vehicle_speed = 10000000;       // CDDA's units; far beyond any vehicle
constexpr uint8_t engine_enabled = 1;             // switched on in the vehicle's engine list
constexpr uint8_t engine_running = 2;             // the vehicle's engines are on and this one is switched on and works
constexpr uint8_t engine_broken = 4;
struct vehicle_engine_view {
    uint32_t vpart = 0;                           // vpart index
    uint8_t state = 0;                            // engine_* bits
    uint8_t hp_frac = 0;                          // 0..255
    uint32_t fuel = 0;                            // itype index of the fuel it burns now; 0 = none (muscle, wind)
};
struct vehicle_fuel_view {
    uint32_t type = 0;                            // itype index
    int32_t left = 0;                             // CDDA's charges of that fuel in all tanks
    int32_t capacity = 0;                         // ... and what the tanks for it hold
};
struct player_vehicle_view {
    uint64_t uid = 0;                             // the vehicle_view's uid
    std::string name;                             // as vehicle_view::name: plain text, capped
    bool in_control = false;                      // CDDA's player_in_control: holds the controls, on a square with some
    bool at_controls = false;                     // controls on the player's square (driving or not)
    // Speeds in CDDA's units, mph * 100; negative backwards. cruise: the speed it is set to reach. safe: the top
    // speed that doesn't strain the engines. max: its top speed.
    int32_t velocity = 0;
    int32_t cruise_velocity = 0;
    int32_t safe_velocity = 0;
    int32_t max_velocity = 0;
    // The player's speed unit option ("mph", "km/h" or "t/t": squares a turn) and what a speed above is multiplied
    // by to show it in that unit (CDDA's convert_velocity).
    std::string speed_unit;
    float speed_scale = 0.01f;
    bool engine_on = false;
    std::vector<vehicle_engine_view> engines;     // cap max_vehicle_engines
    std::vector<vehicle_fuel_view> fuels;         // every fuel in a tank or burnt by an engine, battery charge not; cap max_vehicle_fuels
    int32_t battery_left = 0;                     // kJ in the vehicle's batteries
    int32_t battery_capacity = 0;                 // kJ they hold; 0 = no battery
    float turn_deg = 0;                           // the facing it is steering to
    bool skidding = false;
    int32_t mass_kg = 0;
    // CDDA's overall condition (the vehicle screen's "Status"): 100 minus the share of all parts' damage. Above 95
    // CDDA says "like new", above 67 "dented", above 34 "battered", above 0 "wrecked", at 0 "destroyed".
    uint8_t health_pct = 0;
};

struct player_view {
    uint64_t uid = 0;
    ui_point pos;
    float facing_deg = 0;
    int32_t moves = 0;
    uint32_t move_mode = 0;
    std::vector<uint32_t> body_parts;             // body_part indices; hp arrays follow this order
    std::vector<int16_t> hp;
    std::vector<int16_t> hp_max;
    int32_t stamina = 0;
    int32_t stamina_max = 0;
    int16_t hunger = 0;
    int16_t thirst = 0;
    int16_t sleepiness = 0;
    int16_t pain = 0;
    int16_t morale = 0;
    uint32_t wielded = 0;
    // Per body part, same order as body_parts: effect intensities (0 = none).
    std::vector<uint8_t> bleed;
    std::vector<uint8_t> bandaged;
    std::vector<uint8_t> disinfected;
    std::vector<uint8_t> bite;                    // 1 bitten wound
    std::vector<uint8_t> infected;
    std::vector<worn_view> worn;                  // inner to outer (CDDA's worn order); cap 64
    std::vector<carried_view> items;              // wielded, then worn items and what's inside; cap 256
    std::vector<item_state_view> item_states;     // of those, then of items lying in reach; cap max_item_states
    std::vector<item_amount_view> item_amounts;   // the same items, every one; cap max_item_amounts
    std::optional<gun_view> wielded_gun;          // when the wielded item is a gun
    std::vector<effect_view> effects;             // cap max_player_effects
    mood_view mood;
    std::optional<activity_view> activity;        // while an activity runs
    bool asleep = false;                          // asleep (the sleep effect), not while still trying to
    // Vehicles: `driving` while the player holds a vehicle's controls (CDDA's controlling_vehicle: the move actions
    // then steer and change speed); `vehicle` the uid (vehicle_view) of the vehicle the player is boarded on, 0 =
    // none. Driving a vehicle by remote control is `driving` with `vehicle` 0 or another vehicle's.
    bool driving = false;
    uint64_t vehicle = 0;
    // CDDA's peek (game::peek) stands the avatar on the square next to it to see from there, asks what to look at,
    // and puts it back. While that lasts `peeking` is set and `peeking_from` is the square the body is on; `pos` is
    // the peeked square, which everything seen is seen from. (peek_scope below.)
    bool peeking = false;
    ui_point peeking_from;
    std::optional<player_vehicle_view> vehicle_state;   // only while boarded on a vehicle
};

struct environment_view {
    int64_t turn_of_day_s = 0;
    uint8_t season = 0;                           // season_type
    float sun_alt_deg = 0;
    float sun_az_deg = 0;
    uint8_t moon_phase = 0;                       // moon_phase enum
    uint32_t weather = 0;                         // weather index
    float temperature_c = 0;                      // air temperature on the player's square
    float wind_speed = 0;                         // m/s, the region's wind
    float wind_dir_deg = 0;                       // compass degrees the wind comes from (0 north, 90 east)
    uint8_t ambient_light = 0;                    // natural light level at the player's z
    // What the current weather type does.
    std::string weather_id;                       // weather_type id; up to max_weather_id_bytes ("" if longer)
    std::string weather_name;                     // CDDA's name for it; up to max_weather_name_columns
    uint8_t precip = 0;                           // 0 none, 1 very light, 2 light, 3 heavy (precip_class)
    uint8_t precip_kind = 0;                      // 0 none, 1 rain, 2 snow (CDDA's snowflake animation or a snow sound)
    uint8_t weather_sound = 0;                    // weather_sound_category, 0 (silent) .. 11 (cloudy)
    float sight_penalty = 1;                      // CDDA's sight cost factor per square: 1 none, fog 1.7; 0..100
    int16_t light_modifier = 0;                   // added to natural light (clouds: negative); -1000..1000
    float sun_multiplier = 1;                     // how much sun gets through: 1 clear sky; 0..10
    int16_t sound_attn = 0;                       // how much the weather drowns sounds; 0..1000
    bool dangerous = false;
    // At the player.
    float wind_at_player = 0;                     // m/s on the player's square, terrain and cover counted
    uint8_t humidity = 0;                         // percent
    bool sheltered = false;                       // game::is_sheltered: roof, underground or roofed vehicle
};
constexpr size_t max_weather_id_bytes = 64;
constexpr size_t max_weather_name_columns = 40;

struct snapshot {
    uint32_t contract_version = fp3d::contract_version;
    int64_t turn = 0;
    uint64_t seq = 0;
    uint32_t ids_generation = 0;                  // the id tables (block B6) these indices belong to
    ui_point bubble_origin;                       // absolute submap of bubble (0,0); z = 0
    player_view player;
    int8_t z_first = 0;                           // the levels `submaps` holds, lowest and highest
    int8_t z_last = 0;
    std::vector<std::shared_ptr<const submap_view>> submaps;  // 11 * 11 * the levels (21 when all are taken)
    std::vector<creature_view> creatures;         // perceived only (cap 1024)
    std::vector<vehicle_view> vehicles;           // cap 256
    environment_view env;
};

// Session id tables for kinds CDDA has no int ids for. Index 0 is "none"; indices are handed out on first use
// and never change during the session. ter, furn, trap and field use CDDA's own int ids (0 = null).
// damage_type, activity, sound ("id/variant") and explosion are used by events (block B4).
enum class id_kind : int {
    itype, mtype, vpart, weather, body_part, move_mode, damage_type, activity, sound, explosion, count
};
uint32_t intern_id( id_kind kind, const std::string &id );
std::vector<std::string> id_names( id_kind kind );
// One name of a session table without copying the table: "" for index 0 ("none") or past the end.
std::string id_name( id_kind kind, uint32_t index );
// Empties every session table (block B6 refills them; see id_tables.h).
void reset_id_names();
// Name for an index of ter/furn/trap/field ("ter", "furn", "trap", "field"); "" if out of range.
std::string cdda_id_name( const std::string &table, uint32_t index );

// Sim side. Captures the bubble; submaps unchanged since the previous capture are shared with it.
std::shared_ptr<const snapshot> capture_snapshot();
// Which levels a capture takes. Walking a level costs the same whether anyone reads it or not, so a host that
// only writes a few levels (write_snapshot_json's range) says so with every write: captures then take the widest
// range wanted so far, `below` and `above` the player's level, and leave the rest out of `submaps`. Before the
// first call, and after want_all_snapshot_levels(), a capture takes every level. A level that is left out keeps
// its revision counters, so they never run backwards when it is taken again. Any thread; the next capture uses it.
void want_snapshot_levels( int below, int above );
void want_all_snapshot_levels();
// Sim side. Forgets revisions and creature uids (call after loading a different game).
void reset_snapshots();
// Sim side. The uid a creature has (or gets) in snapshots, and the seq the next capture will have.
uint64_t creature_uid( const Creature &c );
uint64_t next_snapshot_seq();
// Sim side. Lives while CDDA's peek has the avatar on the peeked square: captures then say where the body is
// (player_view::peeking_from). `end()` ends it early; the destructor ends it in any case.
class peek_scope
{
    public:
        explicit peek_scope( const ui_point &body );
        ~peek_scope();
        peek_scope( const peek_scope & ) = delete;
        peek_scope &operator=( const peek_scope & ) = delete;
        void end();
    private:
        bool live = true;
};
// Sim side: makes `s` the newest snapshot, unless a later capture (a higher seq) is published already: the
// newest one never goes back to an earlier capture. nullptr clears it. Client side: reads it (nullptr before the
// first publish).
void publish_snapshot( std::shared_ptr<const snapshot> s );
std::shared_ptr<const snapshot> latest_snapshot();

// Writes `s` as JSON (submaps with z in [z_min, z_max] only) plus the names of the ids it uses.
// A submap's text is made once and reused while the snapshots share the view (nearly all of them between two
// snapshots); `reuse_submap_text` false writes every submap anew, for the check that both give the same bytes.
void write_snapshot_json( std::ostream &out, const snapshot &s, int z_min, int z_max,
                          bool reuse_submap_text = true );

// For a host whose client keeps the snapshot it read last and wants only what changed since. Writes the newest
// snapshot for the levels `z_below` / `z_above` around the player (clamped; the captures are told as with
// want_snapshot_levels) as write_snapshot_json does, with one more member before "submaps":
//   "submap_keys": [[x, y, z, revision, detail_revision, hash], ...], a row for every submap of those levels in
//   the snapshot's order; hash is the 32-bit FNV-1a of the bytes of that submap's object in "submaps".
// `since` is the seq of the snapshot the client holds (0 = none). It is only ever looked up among the last
// `served_snapshots_kept` snapshots this call wrote. When it is one of them, written for the same levels, with
// the same bubble origin and the player on the same level, the JSON is a delta:
//   "base": since, before "submap_keys"; and "submaps" has only the objects whose bytes differ from the base's
//   object at that position, or that the base has none for, and always the ones holding the player's square
//   or one next to it, now and in the base.
// Otherwise it is a full snapshot: no "base", every object. Everything outside "submaps" is whole each time.
// False (nothing written) before the first publish. Any thread.
constexpr size_t served_snapshots_kept = 4;
struct client_snapshot_info {
    uint64_t seq = 0;                             // the snapshot written
    uint64_t base = 0;                            // the one it is a delta against; 0 = a full snapshot
    int submaps = 0;                              // key rows
    int bodies = 0;                               // objects in "submaps"
};
bool write_client_snapshot_json( std::ostream &out, int z_below, int z_above, uint64_t since,
                                 client_snapshot_info &info );

// Character sheet (G4): the character screen's data (CDDA's character sheet) for a character query (bridge.h).
// Read-only; runs on the sim thread. All text is game data (mods): color tags, line breaks and control characters
// removed, every string and list capped. Shape: contracts/bridge_api.md "Character queries".
constexpr size_t max_sheet_speed_modifiers = 32;
constexpr size_t max_sheet_skills = 64;
constexpr size_t max_sheet_proficiencies = 64;   // known and learning, each
constexpr size_t max_sheet_traits = 128;
constexpr size_t max_sheet_bionics = 64;
constexpr size_t max_sheet_effects = 64;
constexpr size_t max_sheet_name_columns = 60;
constexpr size_t max_sheet_description_columns = 400;
constexpr size_t max_sheet_id_bytes = 64;

// The avatar's character sheet as JSON. Over `max_bytes`, descriptions are emptied from the end (effects first,
// then bionics, traits, proficiencies, skills) and "truncated" is true. Still over, entries go from the ends of the
// lists: effects, proficiencies being learned, known ones, traits, bionics, speed modifiers, skills. Only the
// fixed fields left can still be over `max_bytes` (a few KB; the bridge then answers "too large").
std::string run_character_query( uint64_t query_id, size_t max_bytes );

// Sim side. The vehicle in the reality bubble that a snapshot gave `uid` to, nullptr when there is none. Looks
// up only: a vehicle no snapshot has listed yet has no uid and is never found.
vehicle *vehicle_by_uid( map &here, uint64_t uid );
// Sim side. CDDA's part index of each of the vehicle's own part rows, in the order the snapshot lists them
// (vehicle_view::parts before its gap fillers), so a row number from a client finds its part.
std::vector<int> vehicle_part_rows( const vehicle &v );
std::string character_query_error_json( uint64_t query_id, const std::string &error );

} // namespace fp3d

#endif // CATA_SRC_FP3D_SNAPSHOT_H
