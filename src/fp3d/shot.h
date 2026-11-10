#pragma once
#ifndef CATA_SRC_FP3D_SHOT_H
#define CATA_SRC_FP3D_SHOT_H

// Client-decided shots: the 3D client flies the bullet (drop, drag, time of flight) against the creature
// figures and answers the `target` request with FIRE plus where the shot went. While a plan is set, the
// avatar's projectiles follow it instead of CDDA's dispersion roll: they stop at `end`, hit only the
// creature `hit_uid` (0 = a miss) and that creature's body part / weak point `part`. CDDA still does
// everything else (ammo, recoil, noise, time, damage, armour, wounds). Sim thread only.

#include <cstdint>
#include <string>

#include "coordinates.h"

class Creature;

namespace fp3d
{

struct shot_plan {
    tripoint_abs_ms end;        // where the bullet stops (the square hit, or where a miss lands)
    uint64_t hit_uid = 0;       // creature hit (snapshot uid), 0 = none
    std::string part;           // body part id (characters) or weak point id (monsters); "" = CDDA picks
    double quality = 0.3;       // CDDA's goodhit for the hit: 0 dead centre .. <1 grazing
};

void set_shot_plan( const shot_plan &p );
void clear_shot_plan();
// The plan for shots by `shooter`; null if none or the shooter isn't the avatar.
const shot_plan *shot_plan_for( const Creature *shooter );

} // namespace fp3d

#endif // CATA_SRC_FP3D_SHOT_H
