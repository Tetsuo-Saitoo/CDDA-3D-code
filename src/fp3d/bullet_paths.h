#pragma once
#ifndef CATA_SRC_FP3D_BULLET_PATHS_H
#define CATA_SRC_FP3D_BULLET_PATHS_H

// Bullet paths CDDA doesn't have, in FP3D games only (plain CDDA keeps its own): a bullet going on through a body
// it hit, and a bullet glancing off a wall it met at a shallow angle. Soft cover needs nothing here: CDDA's
// map::shoot already lets a bullet through furniture and terrain with `shoot` data, losing damage.
// ballistics.cpp (projectile_attack) calls them for any shooter's plain bullet (paths_apply). Sim thread only,
// except get_path_counts().

#include <cstddef>
#include <cstdint>
#include <vector>

#include "coordinates.h"

class Creature;
class map;
struct damage_instance;
struct projectile;

namespace fp3d
{

// Whether bullet paths apply to `proj`: an FP3D game and a single plain bullet (`is_bullet` as ballistics.cpp has
// it: speed 200 or more, not NO_PENETRATE_OBSTACLES), not a stream, nothing that explodes or spreads an effect
// (custom explosion, an effect's blast, EMP, flashbang or area field), not a laser, plasma, BOUNCE or
// SHATTER_SELF, and only bullet damage (not arrows or bolts, whose damage is stab, whether recovered or not).
bool paths_apply( const projectile &proj, bool is_bullet, bool stream );

// What a body stops of a bullet going through it, in damage, by its size (tiny .. huge).
constexpr double body_stop_tiny = 5.0;
constexpr double body_stop_small = 10.0;
constexpr double body_stop_medium = 20.0;
constexpr double body_stop_large = 35.0;
constexpr double body_stop_huge = 55.0;
// A bullet left with less damage than this stops in the body.
constexpr double min_through_damage = 5.0;

// After `bullet` hit `critter` and dealt its damage: the share of its damage it goes on with through the body (its
// damage less what the body stops, by size), or 0 when it stops there.
double through_body( const projectile &bullet, const Creature &critter );

// A bullet glances off a wall face it meets below this angle (degrees off the face) ...
constexpr double ricochet_max_deg = 25.0;
// ... with this chance at 0 degrees, falling to 0 at ricochet_max_deg; it keeps this share of its damage and flies
// on for at most ricochet_reach squares.
constexpr double ricochet_chance_max = 0.8;
constexpr double ricochet_keep = 0.5;
constexpr int ricochet_reach = 20;

// `bullet` was stopped by the wall at path[i], coming from the passable square `from`; `before` is its damage
// before that square (map::shoot clears it when it stops a bullet). True when it glances off: the path from i on
// is replaced with the bounce (from `from` along the line reflected off the wall's face, at most `range_left` and
// ricochet_reach squares, inside the bubble) and the bullet's damage becomes `before` times ricochet_keep. False
// (nothing changed) for a head-on hit, a corner, a failed roll or no room.
bool ricochet( map &here, projectile &bullet, const damage_instance &before, std::vector<tripoint_bub_ms> &path,
               size_t i, const tripoint_bub_ms &from, int range_left );

// Counters for tests and diagnostics; any thread.
struct path_counts {
    uint64_t through = 0;       // bullets that went on through a body
    uint64_t ricochets = 0;     // bullets that glanced off a wall
};
path_counts get_path_counts();

} // namespace fp3d

#endif // CATA_SRC_FP3D_BULLET_PATHS_H
