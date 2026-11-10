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

struct terrain_def {               // terrain and furniture
    std::string id;
    std::string name;
    std::string symbol;            // UTF-8, the classic tile symbol
    std::string color;             // CDDA colour name, e.g. "c_light_gray"
    std::string looks_like;
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

struct item_def {
    std::string id;
    std::string name;
    std::string symbol;
    std::string color;
    std::string looks_like;
    std::string category;
    std::string gun_ammo;   // a gun's first ammo type (9mm, 223, shot, arrow, ...), "" if not a gun
    std::string gun;        // skill a gun uses (pistol, rifle, archery, launcher, ...), "" if not a gun
    int volume_ml = 0;
    int weight_g = 0;
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

struct id_tables {
    uint32_t generation = 0;       // +1 per build; a client drops everything it cached on a change
    std::vector<terrain_def> ter;  // index = CDDA int id (snapshot ter), 0 = t_null
    std::vector<terrain_def> furn; // index = CDDA int id, 0 = f_null
    std::vector<trap_def> traps;   // index = CDDA int id, 0 = tr_null
    std::vector<field_def> fields; // index = CDDA int id, 0 = fd_null
    std::vector<item_def> items;   // index = session itype index (0 = none, empty def)
    std::vector<monster_def> monsters;  // index = session mtype index
    std::vector<vpart_def> vparts;      // index = session vpart index
};

// Sim side, after a world's data is loaded (boot does it): pre-fills the session tables and builds the
// definitions. Indices handed out before are invalid afterwards (generation changes).
void build_id_tables();
// Either side: the newest tables (nullptr before the first build).
std::shared_ptr<const id_tables> latest_id_tables();

// Writes `t` plus every session table's names as JSON.
void write_id_tables_json( std::ostream &out, const id_tables &t );

} // namespace fp3d

#endif // CATA_SRC_FP3D_ID_TABLES_H
