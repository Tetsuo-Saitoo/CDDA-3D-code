#pragma once
#ifndef CATA_SRC_FP3D_SHOT_H
#define CATA_SRC_FP3D_SHOT_H

// Client-flown shots (contracts/ui_requests.md). The 3D client flies the bullet (drop, drag, time of flight)
// against the creature figures; CDDA's dispersion roll still decides whether each projectile can hit, and the
// client's outcome can only lower it:
// - A FIRE answer with `shot` sets a plan for the avatar's next trigger pull with its wielded gun. Its outcome
//   (where the bullet stopped, the creature and part hit, the quality) applies to the first projectile only, on
//   the game turn it was set.
// - With `fly`, each projectile of that trigger pull sends one `shot` request with the error CDDA rolled for it;
//   the client flies that bullet and answers where it went. A cancel, or an answer that fails the checks, falls
//   back to the plan's outcome (first projectile) or CDDA's own path.
// A client hit counts only if CDDA's roll hits too, with goodhit = max(CDDA's missed_by, the client's quality); a
// client miss stops the bullet at its end; a CDDA miss runs CDDA's own miss path. The planned part lands only if
// goodhit <= sqrt(its hit chance). CDDA does everything else (ammo, recoil, noise, time, damage, armour, wounds).
// Sim thread only, except get_shot_counts().

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "calendar.h"
#include "coordinates.h"
#include "type_id.h"

class Character;
class Creature;
class item;
class map;
class monster;
struct weakpoint;
struct weakpoint_attack;

namespace fp3d
{

struct shot_plan {
    // The FIRE answer's outcome: first projectile of the trigger pull, on game turn `turn` only.
    tripoint_abs_ms end;        // where the bullet stopped (the square hit, or where a miss lands)
    uint64_t hit_uid = 0;       // creature hit (snapshot uid), 0 = none
    std::string part;           // body part id (characters) or weak point id (monsters); "" = CDDA picks
    double quality = 0.3;       // 0 dead centre .. 0.99 grazing
    time_point turn;            // the game turn of the answer
    bool fly = false;           // send a `shot` request for each projectile of the trigger pull
    // From the target request the answer belongs to:
    int range = 0;                    // the gun's range for the avatar, squares
    std::vector<uint64_t> targets;    // creatures a client hit may name: the cursor creature and the candidates
};

void set_shot_plan( const shot_plan &p );
void clear_shot_plan();
bool has_shot_plan();
// Snapshot uids of `cursor` (may be null) and of the creatures on `squares` (bubble), for shot_plan::targets.
std::vector<uint64_t> shot_targets( const Creature *cursor, const std::vector<tripoint_bub_ms> &squares );

// Character::fire_gun. While it lives in the avatar's fire_gun with its wielded gun, the avatar's projectiles use
// the plan. When the avatar's fire_gun ends, the plan is cleared; other shooters leave it alone.
class firing_scope
{
    public:
        firing_scope( const Character &shooter, const item &gun );
        ~firing_scope();
        firing_scope( const firing_scope & ) = delete;
        firing_scope &operator=( const firing_scope & ) = delete;
    private:
        bool avatar_ = false;
};

// The outcome of one projectile.
struct shot_outcome {
    uint64_t hit_uid = 0;       // the creature hit; 0 = a miss
    tripoint_bub_ms end;        // where the projectile stops (the creature's square for a hit)
    double goodhit = 1.0;       // missed_by for the hit
    std::string part;           // the client's part; checked again when the hit lands
};

// projectile_attack: one per call, right after CDDA's dispersion roll. In the avatar's firing_scope the outermost
// call decides the projectile's outcome (with one `shot` request when the plan flies); nested calls (a bounce)
// and other shooters get none, so CDDA's own path runs. Throws shutdown_signal like ask().
class projectile_scope
{
    public:
        projectile_scope( map &here, const Creature *origin, const tripoint_bub_ms &source,
                          const tripoint_bub_ms &target, double rolled_arcmin, double dispersion_max,
                          double missed_by );
        ~projectile_scope();
        projectile_scope( const projectile_scope & ) = delete;
        projectile_scope &operator=( const projectile_scope & ) = delete;
        // null: CDDA's own path
        const shot_outcome *outcome() const;
    private:
        bool outer_ = false;
};

// missed_by for `critter` met on the projectile's path: the outcome's creature gets its goodhit (plus
// `spread_add` for pellets); the aimed square's creature, when it isn't that one, CDDA's roll for an unintended
// hit; any other creature keeps `cur` (CDDA's own roll).
double outcome_missed_by( const shot_outcome &o, const Creature &critter, double cur, bool at_target,
                          double cdda_missed_by, double spread_add );

// creature.cpp: the client's part for the current projectile's hit on `target` by `source`, if it lands
// (goodhit <= sqrt(hit chance): a weak point's weakpoint::hit_chance, a body part's share of the hit size).
// Else null / nullopt, and CDDA picks the part.
const weakpoint *planned_weakpoint( const monster &target, const weakpoint_attack &attack, double goodhit );
std::optional<bodypart_id> planned_body_part( const Creature &target, const Creature *source,
        double goodhit );

// Counters for tests and diagnostics; any thread.
struct shot_counts {
    uint64_t asked = 0;         // `shot` requests sent
    uint64_t accepted = 0;      // `shot` answers used
    uint64_t refused = 0;       // `shot` answers cancelled, without a shot, or failing a check
    uint64_t planned = 0;       // FIRE-answer outcomes used (first projectile)
    uint64_t parts = 0;         // client parts that landed
};
shot_counts get_shot_counts();

} // namespace fp3d

#endif // CATA_SRC_FP3D_SHOT_H
