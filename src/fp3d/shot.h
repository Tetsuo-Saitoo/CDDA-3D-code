#pragma once
#ifndef CATA_SRC_FP3D_SHOT_H
#define CATA_SRC_FP3D_SHOT_H

// Client-flown shots (contracts/ui_requests.md). The 3D client flies the bullet (drop, drag, time of flight)
// against the creature figures; CDDA's dispersion roll still decides whether each projectile can hit, and the
// client's outcome can only lower it:
// - A FIRE answer with `shot` sets a plan for the avatar's next trigger pull with its wielded gun at the aimed
//   square. Without `fly`, its outcome (where the bullet stopped, the creature and part hit, the quality) applies
//   to the first projectile only, on the game turn it was set.
// - With `fly`, each projectile of that trigger pull sends one `shot` request with the error CDDA rolled for it;
//   the client flies that bullet and answers where it went. A cancel, or an answer that fails the checks, leaves
//   that projectile to CDDA's own path (the plan's outcome is not used with `fly`). A shotgun shell's request also
//   names each pellet's spread (CDDA's own roll, roll_pellets); the client flies every pellet, and each pellet's
//   answer is checked and rolled as a projectile of its own along the shell's line turned by its spread.
// A client hit counts only if the creature's own roll hits (CDDA's roll for the aimed square's creature; for any
// other, its square's distance off the projectile's line against its size, never better than CDDA's roll for an
// unintended hit), with goodhit = max(that roll, the client's quality). A client miss, or a hit its roll misses,
// leaves the path to CDDA (its near miss along the aim line with its overshoot, or its wild miss; with `fly` the
// wild miss turns to the side the shot request showed): the creatures it meets get CDDA's rolls, the aimed
// square's one never better than CDDA's own (a CDDA miss stays a miss), and a creature whose own roll already
// missed for the claim isn't rolled again. The planned part only weights CDDA's pick: it lands on a roll against
// its own hit chance times up to aim_weight_max (dead centre), else CDDA's pick stands. With `fly` a trigger pull
// asks for its first default_asks_per_pull projectiles; later ones take CDDA's own path unasked. CDDA
// does everything else (ammo, recoil, noise, time, damage, armour, wounds).
// The plan is dropped when the trigger pull ends or the aiming ends without one (aim_activity_actor).
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
class dispersion_sources;
class item;
class map;
class monster;
struct projectile;
struct projectile_attack_aim;
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
    // What the plan fires: fire_gun must fire this gun at this square (compared, never dereferenced).
    const item *gun = nullptr;
    tripoint_abs_ms target;
    // From the target request the answer belongs to:
    int range = 0;                    // the gun's range for the avatar, squares
    std::vector<uint64_t> targets;    // creatures a client hit may name: the cursor creature, hostile candidates
    uint64_t cursor_uid = 0;          // the cursor creature (0 = none); any other must still be hostile
};

void set_shot_plan( const shot_plan &p );
void clear_shot_plan();
bool has_shot_plan();
// Sets p.targets and p.cursor_uid: snapshot uids of `cursor` (may be null) and of the hostile creatures on
// `squares` (bubble). A client hit may name the cursor creature or a hostile one, never a bystander CDDA would ask
// about first.
void set_shot_targets( shot_plan &p, const Creature *cursor, const std::vector<tripoint_bub_ms> &squares );

// Character::fire_gun. While it lives in the avatar's fire_gun with the plan's gun (wielded, firing itself, not
// an attached gunmod) at the plan's square, the avatar's projectiles use the plan. When the avatar's fire_gun
// ends, the plan is cleared; other shooters leave it alone.
class firing_scope
{
    public:
        firing_scope( const Character &shooter, const item &gun, const map &here, const tripoint_bub_ms &target );
        ~firing_scope();
        firing_scope( const firing_scope & ) = delete;
        firing_scope &operator=( const firing_scope & ) = delete;
    private:
        bool avatar_ = false;
};

// The outcome of one projectile.
struct shot_outcome {
    uint64_t hit_uid = 0;       // the creature hit, the projectile stops at its square `end`; 0 = CDDA's path
    tripoint_bub_ms end;
    uint64_t spared_uid = 0;    // a creature whose own roll for the client's claim missed: not rolled again
    double goodhit = 1.0;       // missed_by for the hit
    std::string part;           // the client's part; checked again when the hit lands
};

// One pellet's spread off the shell's line: `arcmin` (0..1800) to the shooter's right (side +1) or left (-1).
struct pellet {
    double arcmin = 0.0;
    int side = 1;
};
// ballistics.cpp: every pellet's spread for `proj` in an FP3D game, rolled before the projectile flies as CDDA
// rolls each one in its pellet loop (none without shot_spread, or outside FP3D games: CDDA's own roll order).
std::vector<pellet> roll_pellets( const projectile &proj );

// projectile_attack: one per call, right after CDDA's dispersion roll. In the avatar's firing_scope the outermost
// call decides the projectile's outcome (with one `shot` request when the plan flies, naming `pellets` for a
// shell); nested calls (a bounce) and other shooters get none, so CDDA's own path runs. Throws shutdown_signal
// like ask().
class projectile_scope
{
    public:
        projectile_scope( map &here, const Creature *origin, const tripoint_bub_ms &source,
                          const tripoint_bub_ms &target, const projectile &proj, const dispersion_sources &dispersion,
                          const projectile_attack_aim &aim, const std::vector<pellet> &pellets );
        ~projectile_scope();
        projectile_scope( const projectile_scope & ) = delete;
        projectile_scope &operator=( const projectile_scope & ) = delete;
        // null: CDDA's own path. For a shell whose pellets the client flew one by one, the current pellet's (see
        // select_pellet()); none before the pellet loop.
        const shot_outcome *outcome() const;
        // The pellet loop's pellet `j` is next: outcome() and the client's part become its own when the client
        // flew each pellet; else nothing changes.
        void select_pellet( size_t j );
        // Whether outcome() is a pellet's own: its roll already counts the pellet's spread.
        bool per_pellet() const;
        // With `fly`: the side (+1 / -1, the sign CDDA's wild miss adds its angle with) the shot request's deviated
        // line turned to, which CDDA's wild miss takes instead of its coin flip; the angle is CDDA's own roll.
        std::optional<int> wild_side() const;
    private:
        bool outer_ = false;
        std::optional<int> side_;
};

// missed_by for `critter` met on the projectile's path, `cur` being CDDA's own (its roll on the aimed square, its
// unintended-hit roll elsewhere): the outcome's creature gets its goodhit (its own roll at best, plus `spread_add`
// for pellets); the spared creature 1 (its roll was made); the aimed square's creature, when it isn't the hit one,
// CDDA's roll for an unintended hit, never below `cur`; any other creature keeps `cur`.
double outcome_missed_by( const shot_outcome &o, const Creature &critter, double cur, bool at_target,
                          double cdda_missed_by, double spread_add );

// creature.cpp: the client's part for the current projectile's hit on `target` by `source`, if it lands
// (goodhit <= sqrt(hit chance): a weak point's weakpoint::hit_chance, a body part's share of the hit size).
// Else null / nullopt, and CDDA picks the part.
const weakpoint *planned_weakpoint( const monster &target, const weakpoint_attack &attack, double goodhit );
std::optional<bodypart_id> planned_body_part( const Creature &target, const Creature *source,
        double goodhit );

// A dead-centre hit makes the client's part this many times as likely as its own chance (1 for a graze).
constexpr double aim_weight_max = 4.0;
// `shot` requests per trigger pull at most (a long `fly` burst would stall the client one frame per round).
constexpr int default_asks_per_pull = 8;
// Tests only (not bound for the client): the cap, clamped to 1..64.
void set_asks_per_pull( int n );

// Counters for tests and diagnostics; any thread.
struct shot_counts {
    uint64_t asked = 0;         // `shot` requests sent
    uint64_t accepted = 0;      // `shot` answers that passed the checks
    uint64_t refused = 0;       // `shot` answers cancelled, without a shot, or failing a check
    uint64_t pellets = 0;       // pellet answers that passed the checks (their shot answer counts as accepted)
    uint64_t capped = 0;       // `fly` projectiles past the cap: CDDA's own path, not asked
    uint64_t planned = 0;       // FIRE-answer outcomes that passed the checks (first projectile)
    uint64_t parts = 0;         // client parts that landed
    uint64_t spared = 0;        // creatures met on the path that a claim's missed roll kept from a second roll
};
shot_counts get_shot_counts();

} // namespace fp3d

#endif // CATA_SRC_FP3D_SHOT_H
