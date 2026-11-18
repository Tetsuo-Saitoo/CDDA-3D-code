#pragma once
#ifndef CATA_SRC_FP3D_ID_TABLES_H
#define CATA_SRC_FP3D_ID_TABLES_H

// FP3D block B6: the definitions behind the ids in snapshots and events, for the client (meshing rules, model
// lookup, names) and the asset tools. See contracts/id_tables.md. Built once per loaded world on the sim thread;
// the client reads the newest with latest_id_tables(). Building also pre-fills the session tables (itype, mtype,
// vpart, ...) in sorted order, so their indices are the same every session with the same data.

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace fp3d
{

constexpr size_t max_examine_id_bytes = 64;
struct terrain_def {               // terrain and furniture
    std::string id;
    std::string name;
    std::string symbol;            // UTF-8, the classic tile symbol
    std::string color;             // CDDA colour name, e.g. "c_light_gray"
    std::string looks_like;
    std::string examine;           // CDDA's examine action id as the data names it (an actor: its type), "" = none;
    // up to max_examine_id_bytes
    int move_cost = 0;             // terrain: move cost (0 = impassable); furniture: cost modifier (-1 impassable)
    int coverage = 0;              // percent
    int light_emitted = 0;
    bool transparent = false;
    std::vector<std::string> flags;            // CDDA flags, sorted
    std::vector<std::string> connect_groups;   // groups this belongs to
    std::vector<std::string> connects_to;      // groups it joins up with (walls, fences, ...)
    std::vector<std::string> rotates_to;
    std::string open;              // the id it turns into when opened ("" none)
    std::string close;
    std::string roof;              // terrain only: what the floor above becomes
};

struct trap_def {
    std::string id;
    std::string name;
    std::string symbol;
    std::string color;             // (which traps the player knows is the snapshot's business)
};

struct field_def {
    std::string id;
    std::string looks_like;
    int priority = 0;              // drawn over lower ones
    std::vector<std::string> level_names;      // per intensity, 1-based intensity = index + 1
    std::vector<std::string> level_colors;
};

// An item type's own container pockets, in the order item actions number them (`pocket`). An item can gain more
// at run time (attached pouches); those come after these.
constexpr size_t max_itype_pockets = 32;
constexpr size_t max_mod_locations = 16;
// item_def::heal: the item bandages a hurt part, disinfects it, stops bleeding, treats a bite, treats an infection,
// gives hit points back.
constexpr int heal_bandages = 1;
constexpr int heal_disinfects = 2;
constexpr int heal_stops_bleeding = 4;
constexpr int heal_treats_bite = 8;
constexpr int heal_treats_infection = 16;
constexpr int heal_restores_hp = 32;
constexpr size_t max_pocket_name_columns = 40;
enum pocket_flag : uint16_t {
    pocket_rigid = 1,          // size doesn't change with contents
    pocket_watertight = 2,     // holds liquids
    pocket_airtight = 4,       // holds gases
    pocket_holster = 8,        // holds one item
    pocket_open = 16,          // open container: liquids spill when it moves
    pocket_sealable = 32,      // comes sealed / can be sealed
    pocket_ammo_only = 64,     // only some ammo types (quivers, ammo boxes)
    pocket_restricted = 128,   // only some items, flags or materials
    pocket_forbidden = 256,    // the player can't use it
};
struct pocket_def {
    std::string name;          // CDDA's pocket name ("Torso compartment"); often empty
    int64_t volume_ml = 0;
    int64_t max_weight_g = 0;
    int64_t max_item_ml = -1;  // largest single item; -1 = only the pocket's volume
    uint16_t flags = 0;        // pocket_flag bits
};

struct item_def {
    std::string id;
    std::string name;
    std::string symbol;
    std::string color;
    std::string looks_like;
    std::string category;
    std::string phase;      // CDDA's phase of the type: "solid", "liquid", "gas", "plasma"; "" = none set
    std::string gun_ammo;   // a gun's first ammo type (9mm, 223, shot, arrow, ...), "" if not a gun
    std::string gun;        // skill a gun uses (pistol, rifle, archery, launcher, ...), "" if not a gun
    // Gun mods by hand: where a gun mod goes (CDDA's location id: "sights", "underbarrel", "stock mount"; see
    // gunmod_location_text in item_hands.h), "" if not a gun mod; and a gun type's own slots, [location id,
    // how many, 1..max_mod_slots] (cap max_mod_locations; an installed mod can add more, which the snapshot's
    // wielded gun lists). Never present with an empty id or no slots.
    std::string gunmod_location;
    std::vector<std::pair<std::string, int>> mod_locations;
    // First aid: -1 for an item without CDDA's heal use (the field is left out). Else what the item does to a
    // body part, a sum of the heal_* bits (0 when it has the use and none of them).
    int heal = -1;
    int volume_ml = 0;
    int weight_g = 0;
    std::vector<pocket_def> pockets;   // cap max_itype_pockets
};

struct monster_def {
    std::string id;
    std::string name;
    std::string symbol;
    std::string color;
    std::string looks_like;
    int size = 0;                  // creature_size: 1 tiny .. 5 huge
    int hp = 0;
    int speed = 0;                 // 100 = human
    int volume_ml = 0;
    int weight_g = 0;
    std::vector<std::string> species;
    std::string bodytype;          // CDDA's body type: human, dog, insect, snake, bird, blob, ...; may be empty
    std::vector<std::string> weakpoints;   // weak point ids (head, eye, arm, leg, ...; "" = the body), cap 32
};

struct vpart_def {
    std::string id;
    std::string name;
    std::string color;
    std::string looks_like;
    std::string location;          // e.g. "structure", "engine_block", "on_roof"
    std::vector<std::string> flags;
};

// A weather type for the weather cheat's picker (cheats.h): its id and display name (snapshot.h's bounds).
constexpr size_t max_weather_types = 128;
struct weather_def {
    std::string id;
    std::string name;
};

struct id_tables {
    uint32_t generation = 0;       // +1 per build; a client drops everything it cached on a change
    std::vector<terrain_def> ter;  // index = CDDA int id (snapshot ter), 0 = t_null
    std::vector<terrain_def> furn; // index = CDDA int id, 0 = f_null
    std::vector<trap_def> traps;   // index = CDDA int id, 0 = tr_null
    std::vector<field_def> fields; // index = CDDA int id, 0 = fd_null
    std::vector<item_def> items;   // index = session itype index (0 = none, empty def)
    std::vector<monster_def> monsters;  // index = session mtype index
    std::vector<vpart_def> vparts;      // index = session vpart index
    std::vector<weather_def> weathers;  // the loaded weather types but "null", by id; at most max_weather_types
};

// Sim side, after a world's data is loaded (boot does it): pre-fills the session tables and builds the
// definitions. Indices handed out before are invalid afterwards (generation changes).
void build_id_tables();
// Either side: the newest tables (nullptr before the first build).
std::shared_ptr<const id_tables> latest_id_tables();
// Between two games: no tables until the next game's build (the generation keeps counting).
void clear_id_tables();

// Writes `t` plus every session table's names as JSON.
void write_id_tables_json( std::ostream &out, const id_tables &t );

} // namespace fp3d

#endif // CATA_SRC_FP3D_ID_TABLES_H
