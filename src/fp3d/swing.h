#pragma once
#ifndef CATA_SRC_FP3D_SWING_H
#define CATA_SRC_FP3D_SWING_H

// FP3D: aimed melee. An aimed hand action ("fp3d_attack_<main|off>_aim_<DIR>", fp3d/hands.h) means the client's
// crosshair (or, in VR, the hand) is on the creature struck: the avatar's strike then asks one `swing` request
// (origin: the avatar's square, cursor: the creature's square, range: its distance, mode: "main" or "off"),
// answered with a `shot` (end: that square, hit_uid: that creature, part: the body part / weak point aimed at;
// quality is ignored). CDDA still rolls the hit; when it hits, the client's part only weights CDDA's pick, as for
// shots (shot.h part_lands): its own chance times up to aim_weight_max for a strong hit, CDDA's pick otherwise.
// A cancel, an answer for another creature or square, or an unknown part leaves CDDA's pick. Plain strikes, bump
// attacks and any other melee never ask. Sim thread only, except get_swing_counts().

#include <cstdint>
#include <optional>
#include <string>

#include "type_id.h"

class Character;
class Creature;
class monster;
struct weakpoint;
struct weakpoint_attack;

namespace fp3d
{

// Held by hand_attack for an aimed strike: the avatar's next swing (melee_attack_abstract) asks a swing request.
class aimed_strike
{
    public:
        aimed_strike();
        ~aimed_strike();
        aimed_strike( const aimed_strike & ) = delete;
        aimed_strike &operator=( const aimed_strike & ) = delete;
};

// melee.cpp (melee_attack_abstract): one swing of `attacker` at `target`. The outermost swing of an aimed strike
// by the avatar asks the client (in the constructor); its answer holds for that swing only.
class swing_scope
{
    public:
        swing_scope( const Character &attacker, const Creature &target );
        ~swing_scope();
        swing_scope( const swing_scope & ) = delete;
        swing_scope &operator=( const swing_scope & ) = delete;
};

// melee.cpp, once the swing's hit is rolled: CDDA's hit spread (>= 0 a hit) and whether it is a critical.
void swing_rolled( int hit_spread, bool critical );
// The goodhit (0 dead centre .. 1 graze) a melee hit counts as for the client's part: 0 for a critical, else
// 1 / (1 + hit_spread / 10).
double swing_goodhit( int hit_spread, bool critical );

// The client's part for the current swing's hit on `target`, if it lands; else nullopt / null and CDDA picks.
// melee.cpp for a Character target, monster.cpp (absorb_hit) for a monster's weak point.
std::optional<bodypart_id> swing_body_part( const Creature &target );
const weakpoint *swing_weakpoint( const monster &target, const weakpoint_attack &attack );

// Counters for tests and diagnostics; any thread.
struct swing_counts {
    uint64_t asked = 0;         // swing requests sent
    uint64_t accepted = 0;      // answers naming the creature struck and a part
    uint64_t refused = 0;       // any other answer (cancel, another creature or square, no part)
    uint64_t parts = 0;         // hits that landed on the client's part
};
swing_counts get_swing_counts();

} // namespace fp3d

#endif // CATA_SRC_FP3D_SWING_H
