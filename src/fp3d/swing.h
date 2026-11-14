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
//
// The call: the same answer's `action` may call a technique family, exactly "down", "stun" or "disarm" (any other
// text is no call). The word is compared as the bridge hands it over: ask() takes control characters out of every
// answer's action first (contracts/ui_requests.md, Rules), so "down\n" is "down"; nothing else is trimmed or
// folded. It is read only from an answer for the creature struck on its square; such an answer needs no part.
// On a hit, the techniques CDDA lists for this weapon, style and limbs that belong to the called family are
// tried first, each by CDDA's own rules (evaluate_technique: skill, critical, conditions, attack vector); one of
// those it allows is used, else CDDA picks as always. The call never names a technique and never forces one.
// The avatar's hit then says what it did: its attack event (fp3d/events.h on_attack) carries the technique's
// name and family. Families: 0 none, 1 strike, 2 down, 3 stun, 4 disarm, 5 push.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "type_id.h"

class Character;
class Creature;
class item_location;
class ma_technique;
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

// melee.cpp, once the swing's hit is rolled: CDDA's hit spread (>= 0 a hit) and whether it is a critical. Only the
// outermost swing's roll is kept (the swing that asked): a swing inside it rolls for itself.
void swing_rolled( int hit_spread, bool critical );
// The goodhit (0 dead centre .. 1 graze) a melee hit counts as for the client's part: 0 for a critical, else
// 1 / (1 + hit_spread / 10).
double swing_goodhit( int hit_spread, bool critical );

// The client's part for the current swing's hit on `target`, if it lands; else nullopt / null and CDDA picks.
// melee.cpp for a Character target, monster.cpp (absorb_hit) for a monster's weak point.
std::optional<bodypart_id> swing_body_part( const Creature &target );
const weakpoint *swing_weakpoint( const monster &target, const weakpoint_attack &attack );

// Technique families, by what a technique does to the creature struck.
constexpr int technique_none = 0;
constexpr int technique_strike = 1;
constexpr int technique_down = 2;
constexpr int technique_stun = 3;
constexpr int technique_disarm = 4;
constexpr int technique_push = 5;
constexpr size_t max_technique_name = 64;   // bytes

// The family of `tec`, from its own data, the first that fits: tec_none, a dummy or a defensive one is none; one
// that disarms or takes the weapon is a disarm; then one that downs, one that stuns, one that knocks back (push);
// anything else is a strike.
int technique_family( const ma_technique &tec );
// A technique's name for the client: no colour tags (none left by nesting or by a dropped byte either), whole
// well-formed UTF-8 characters, at most max_technique_name bytes, no control characters. Names come from game data
// and mods: a very long one is cut before anything else is done with it.
std::string technique_text( const std::string &name );

// What a hit's attack event says about its technique.
struct technique_note {
    std::string name;
    int family = 0;
};
// melee.cpp, once a hit's technique is picked: the note for the avatar's hit with a real technique on a creature
// the player saw (`seen`); empty for anyone else, an unseen creature, tec_none or an unknown id.
technique_note swing_note( const Character &attacker, const matec_id &tec, bool seen );

// melee.cpp, where a hit picks its technique: the technique for the current swing's call, if there is one and
// CDDA allows a technique of that family now; else nullopt and CDDA picks. The ids evaluated here are added to
// `tried`, for pick_technique to leave out (each technique is evaluated once per hit). Once per swing.
std::optional<std::tuple<matec_id, attack_vector_id, sub_bodypart_str_id>> swing_technique(
            const Character &attacker, const Creature &target, const item_location &weapon, bool critical,
            std::vector<matec_id> &tried );

// Counters for tests and diagnostics; any thread.
struct swing_counts {
    uint64_t asked = 0;         // swing requests sent
    uint64_t accepted = 0;      // answers naming the creature struck and a part
    uint64_t refused = 0;       // any other answer (cancel, another creature or square, no part)
    uint64_t parts = 0;         // hits that landed on the client's part
    uint64_t hints = 0;         // answers for the creature struck that call a family
    uint64_t hinted = 0;        // hits whose technique came from the call
};
swing_counts get_swing_counts();

} // namespace fp3d

#endif // CATA_SRC_FP3D_SWING_H
