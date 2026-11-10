#pragma once
#ifndef CATA_SRC_FP3D_SNAPSHOT_H
#define CATA_SRC_FP3D_SNAPSHOT_H

// FP3D block B3: immutable copies of the reality bubble for the client. See contracts/snapshot.md.
// Captured on the sim thread (capture_snapshot), handed over with publish_snapshot / latest_snapshot.
// Every snapshot lists all submaps of the bubble; ones that didn't change since the previous capture are
// shared (same pointer, same revisions), so a client that only reads the newest snapshot never misses a change.

#include <array>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "fp3d/bridge.h"

class Creature;

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

struct item_pile_view {
    uint8_t tile = 0;
    uint16_t count = 0;                           // items on the tile
    std::array<uint32_t, 4> top{};                // itype indices, largest volume first; 0 = none
    bool operator==( const item_pile_view &o ) const;
};

struct field_view {
    uint8_t tile = 0;
    uint32_t type = 0;                            // field type index
    uint8_t intensity = 0;
    uint16_t age_turns = 0;
    bool operator==( const field_view &o ) const;
};

struct submap_view {
    ui_point pos;                                 // absolute submap
    uint32_t revision = 0;                        // bumps when ter/furn/trap/flags change: rebuild the mesh
    uint32_t detail_revision = 0;                 // bumps when light/vis/items/fields change
    std::array<uint32_t, sm_tiles> ter{};
    std::array<uint32_t, sm_tiles> furn{};
    std::array<uint32_t, sm_tiles> trap{};        // only traps the player knows about or sees
    std::array<uint8_t, sm_tiles> light{};        // CDDA light level, clamped to 0..255
    std::array<uint8_t, sm_tiles> vis{};
    std::array<uint8_t, sm_tiles> flags{};
    std::vector<item_pile_view> items;            // tiles seen now only (cap 144)
    std::vector<field_view> fields;               // tiles seen now only (cap 4 per tile)
    bool same_shape( const submap_view &o ) const;
    bool same_detail( const submap_view &o ) const;
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
};

struct vehicle_part_view {
    int mount_x = 0;
    int mount_y = 0;
    uint32_t vpart = 0;
    uint8_t hp_frac = 0;
    uint8_t state = 0;                            // bit0 open, bit1 broken, bit2 enabled
};

struct vehicle_view {
    uint64_t uid = 0;
    ui_point pos;                                 // absolute square of the vehicle's origin part
    float face_deg = 0;
    float turn_deg = 0;
    float velocity = 0;                           // m/s
    std::string name;
    std::vector<vehicle_part_view> parts;         // cap 4096
};

struct worn_view {
    uint32_t type = 0;                            // itype index
    uint8_t layer = 0;                            // highest layer_level (0 personal .. 5 belted/aux)
    uint8_t damage = 0;                           // item::damage_level(): 0 fine .. 5 nearly destroyed
    uint8_t degradation = 0;                      // 0..255 of the itype's max degradation
    std::vector<uint32_t> covers;                 // body_part indices
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
};

struct environment_view {
    int64_t turn_of_day_s = 0;
    uint8_t season = 0;                           // season_type
    float sun_alt_deg = 0;
    float sun_az_deg = 0;
    uint8_t moon_phase = 0;                       // moon_phase enum
    uint32_t weather = 0;                         // weather index
    float temperature_c = 0;
    float wind_speed = 0;                         // m/s
    float wind_dir_deg = 0;
    uint8_t ambient_light = 0;                    // natural light level at the player's z
};

struct snapshot {
    uint32_t contract_version = fp3d::contract_version;
    int64_t turn = 0;
    uint64_t seq = 0;
    uint32_t ids_generation = 0;                  // the id tables (block B6) these indices belong to
    ui_point bubble_origin;                       // absolute submap of bubble (0,0); z = 0
    player_view player;
    std::vector<std::shared_ptr<const submap_view>> submaps;  // 11 * 11 * 21
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
// Empties every session table (block B6 refills them; see id_tables.h).
void reset_id_names();
// Name for an index of ter/furn/trap/field ("ter", "furn", "trap", "field"); "" if out of range.
std::string cdda_id_name( const std::string &table, uint32_t index );

// Sim side. Captures the bubble; submaps unchanged since the previous capture are shared with it.
std::shared_ptr<const snapshot> capture_snapshot();
// Sim side. Forgets revisions and creature uids (call after loading a different game).
void reset_snapshots();
// Sim side. The uid a creature has (or gets) in snapshots, and the seq the next capture will have.
uint64_t creature_uid( const Creature &c );
uint64_t next_snapshot_seq();
// Sim side: makes `s` the newest snapshot. Client side: reads it (nullptr before the first publish).
void publish_snapshot( std::shared_ptr<const snapshot> s );
std::shared_ptr<const snapshot> latest_snapshot();

// Writes `s` as JSON (submaps with z in [z_min, z_max] only) plus the names of the ids it uses.
void write_snapshot_json( std::ostream &out, const snapshot &s, int z_min, int z_max );

} // namespace fp3d

#endif // CATA_SRC_FP3D_SNAPSHOT_H
