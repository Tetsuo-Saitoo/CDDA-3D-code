#include "fp3d/swing.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "bodypart.h"
#include "character.h"
#include "character_martial_arts.h"
#include "coordinates.h"
#include "creature.h"
#include "fp3d/bridge.h"
#include "fp3d/hands.h"
#include "fp3d/points.h"
#include "fp3d/shot.h"
#include "fp3d/snapshot.h"
#include "item_location.h"
#include "line.h"
#include "martialarts.h"
#include "monster.h"
#include "output.h"
#include "rng.h"
#include "translation.h"
#include "weakpoint.h"

namespace fp3d
{
namespace
{
// The client's answer for the swing under way: the creature struck, what it aimed at and what it called.
struct swing_plan {
    uint64_t uid = 0;
    std::string part;           // "" = no part, only a call
    int want = technique_none;  // the technique family called, until the hit has used it
};

int aimed_depth = 0;            // aimed_strike guards alive
bool aimed_asked = false;       // this aimed strike asked already (one request per strike)
int swing_depth = 0;            // swing_scope nesting (a technique's extra hits swing inside the first)
std::optional<swing_plan> plan;
std::optional<double> goodhit;  // set by swing_rolled for the outermost swing (the one that asked)

std::atomic<uint64_t> n_asked{ 0 };
std::atomic<uint64_t> n_accepted{ 0 };
std::atomic<uint64_t> n_refused{ 0 };
std::atomic<uint64_t> n_parts{ 0 };
std::atomic<uint64_t> n_hints{ 0 };
std::atomic<uint64_t> n_hinted{ 0 };

// The plan's part for `target`, while the current swing has rolled a hit and the plan names that creature.
const std::string *planned( const Creature &target )
{
    if( swing_depth == 0 || !plan || !goodhit || plan->part.empty() || creature_uid( target ) != plan->uid ) {
        return nullptr;
    }
    return &plan->part;
}

// The family a swing answer's action calls: these exact words only (ask() has taken control characters out of
// the action already, as for every answer).
int called_family( const std::string &action )
{
    if( action == "down" ) {
        return technique_down;
    }
    if( action == "stun" ) {
        return technique_stun;
    }
    if( action == "disarm" ) {
        return technique_disarm;
    }
    return technique_none;
}

// A technique's name is read this far (bytes): a mod's name has no limit of its own.
constexpr size_t max_technique_source = 1024;

// How many bytes the well-formed UTF-8 character at s[i] takes; 0 if there is none there (a stray or cut-off
// byte, an overlong form, a surrogate, anything past U+10FFFF).
size_t utf8_char_size( const std::string &s, size_t i )
{
    // the byte at s[k] is there and lies in lo .. hi
    const auto within = [&s]( size_t k, unsigned lo, unsigned hi ) {
        if( k >= s.size() ) {
            return false;
        }
        const unsigned c = static_cast<unsigned char>( s[k] );
        return c >= lo && c <= hi;
    };
    if( i >= s.size() ) {
        return 0;
    }
    const unsigned b = static_cast<unsigned char>( s[i] );
    if( b < 0x80u ) {
        return 1;
    }
    if( b >= 0xC2u && b <= 0xDFu ) {
        return within( i + 1, 0x80u, 0xBFu ) ? 2 : 0;
    }
    if( b >= 0xE0u && b <= 0xEFu ) {
        return within( i + 1, b == 0xE0u ? 0xA0u : 0x80u, b == 0xEDu ? 0x9Fu : 0xBFu ) &&
               within( i + 2, 0x80u, 0xBFu ) ? 3 : 0;
    }
    if( b >= 0xF0u && b <= 0xF4u ) {
        return within( i + 1, b == 0xF0u ? 0x90u : 0x80u, b == 0xF4u ? 0x8Fu : 0xBFu ) &&
               within( i + 2, 0x80u, 0xBFu ) && within( i + 3, 0x80u, 0xBFu ) ? 4 : 0;
    }
    return 0;
}

// The whole well-formed UTF-8 characters of `s` from its start, as many as fit `max_bytes`; bytes that belong to
// none are left out.
std::string whole_utf8( const std::string &s, size_t max_bytes )
{
    std::string out;
    size_t i = 0;
    while( i < s.size() ) {
        const size_t n = utf8_char_size( s, i );
        if( n == 0 ) {
            i++;
            continue;
        }
        if( out.size() + n > max_bytes ) {
            break;
        }
        out.append( s, i, n );
        i += n;
    }
    return out;
}
} // namespace

aimed_strike::aimed_strike()
{
    if( aimed_depth++ == 0 ) {
        aimed_asked = false;
    }
}

aimed_strike::~aimed_strike()
{
    if( --aimed_depth == 0 ) {
        aimed_asked = false;
    }
}

swing_scope::swing_scope( const Character &attacker, const Creature &target )
{
    if( swing_depth++ > 0 ) {
        return;     // a swing inside a swing (a technique's extra hit) keeps the outer swing's answer
    }
    plan.reset();
    goodhit.reset();
    if( aimed_depth == 0 || aimed_asked || !active() || !attacker.is_avatar() ) {
        return;
    }
    aimed_asked = true;
    const tripoint_abs_ms from = attacker.pos_abs();
    const tripoint_abs_ms at = target.pos_abs();
    ui_request req;
    req.kind = request_kind::swing;
    req.origin = to_ui_point( from );
    req.cursor = to_ui_point( at );
    req.range = rl_dist( from, at );
    req.mode = offhand_strike() ? "off" : "main";
    n_asked++;
    ui_answer a;
    try {
        a = ask( req );
    } catch( ... ) {
        swing_depth--;  // the destructor won't run for a constructor that throws
        throw;
    }
    const uint64_t uid = creature_uid( target );
    // An answer for the creature struck, on its square: only that one's part and call count.
    const bool on_target = !a.cancel && a.shot && uid != 0 && a.shot->hit_uid == uid &&
                           a.shot->end.x == req.cursor.x && a.shot->end.y == req.cursor.y &&
                           a.shot->end.z == req.cursor.z;
    const int want = on_target ? called_family( a.action ) : technique_none;
    if( want != technique_none ) {
        n_hints++;
    }
    if( on_target && ( !a.shot->part.empty() || want != technique_none ) ) {
        plan = swing_plan{ uid, a.shot->part, want };
    }
    if( on_target && !a.shot->part.empty() ) {
        n_accepted++;
    } else {
        n_refused++;
    }
}

swing_scope::~swing_scope()
{
    if( --swing_depth == 0 ) {
        plan.reset();
        goodhit.reset();
    }
}

double swing_goodhit( int hit_spread, bool critical )
{
    if( critical ) {
        return 0.0;
    }
    return 1.0 / ( 1.0 + std::max( hit_spread, 0 ) / 10.0 );
}

void swing_rolled( int hit_spread, bool critical )
{
    // Only the outermost swing's roll: a swing inside it (a technique's extra hit on another creature, a counter)
    // rolls for itself, and the plan's creature is hit after it (a monster's weak point is picked in absorb_hit).
    if( swing_depth != 1 || !plan ) {
        return;
    }
    if( hit_spread < 0 ) {
        goodhit.reset();    // a miss lands nowhere
        return;
    }
    goodhit = swing_goodhit( hit_spread, critical );
}

std::optional<bodypart_id> swing_body_part( const Creature &target )
{
    const std::string *part = planned( target );
    if( part == nullptr ) {
        return std::nullopt;
    }
    std::optional<bodypart_id> id = client_body_part( target, *part, *goodhit );
    if( id ) {
        n_parts++;
    }
    return id;
}

const weakpoint *swing_weakpoint( const monster &target, const weakpoint_attack &attack )
{
    if( attack.source == nullptr || !attack.source->is_avatar() ) {
        return nullptr;
    }
    const std::string *part = planned( target );
    if( part == nullptr ) {
        return nullptr;
    }
    const weakpoint *w = client_weakpoint( target, attack, *part, *goodhit );
    if( w != nullptr ) {
        n_parts++;
    }
    return w;
}

int technique_family( const ma_technique &tec )
{
    if( tec.id == tec_none || tec.dummy || tec.defensive ) {
        return technique_none;
    }
    if( tec.disarms || tec.take_weapon ) {
        return technique_disarm;
    }
    if( tec.down_dur > 0 ) {
        return technique_down;
    }
    if( tec.stun_dur > 0 ) {
        return technique_stun;
    }
    if( tec.knockback_dist > 0 ) {
        return technique_push;
    }
    return technique_strike;
}

std::string technique_text( const std::string &name )
{
    // Cut first: remove_color_tags repeats the rest of the text once per opener that lies before one '>'.
    std::string s = sanitize_text( whole_utf8( remove_color_tags( whole_utf8( name, max_technique_source ) ),
                                   max_technique_name ), max_technique_name );
    // That one pass leaves a tag for an opener inside an opener ("<col<color_red>or_red>"), and a byte dropped
    // after it can put one together ("<co\x01lor_red>"): whatever still reads as a tag is taken out, until none
    // does. Each round takes out 7 bytes or more, from a '<' to a '>' or the end, so the characters stay whole.
    for( ;; ) {
        const size_t open = s.find( "<color_" );
        const size_t close = s.find( "</color>" );
        if( open == std::string::npos && close == std::string::npos ) {
            return s;
        }
        if( close < open ) {
            s.erase( close, 8 );
        } else {
            const size_t end = s.find( '>', open );
            s.erase( open, end == std::string::npos ? std::string::npos : end - open + 1 );
        }
    }
}

technique_note swing_note( const Character &attacker, const matec_id &tec, bool seen )
{
    if( !attacker.is_avatar() || !seen || !tec.is_valid() || tec == tec_none ) {
        return {};
    }
    return { technique_text( tec->name.translated() ), technique_family( *tec ) };
}

std::optional<std::tuple<matec_id, attack_vector_id, sub_bodypart_str_id>> swing_technique(
            const Character &attacker, const Creature &target, const item_location &weapon, bool critical,
            std::vector<matec_id> &tried )
{
    if( swing_depth != 1 || !plan || plan->want == technique_none || !attacker.is_avatar() ||
        creature_uid( target ) != plan->uid ) {
        return std::nullopt;
    }
    const int want = std::exchange( plan->want, technique_none );    // one call per swing
    // CDDA's own list for this weapon, style and limbs (pick_technique reads the same one). Each technique of
    // the called family is evaluated once, by CDDA's rules; one that only comes up on a roll (a negative
    // weighting) is left to CDDA's pick. No text from the client becomes an id here.
    const std::vector<matec_id> all = attacker.martial_arts_data->get_all_techniques( weapon, attacker );
    std::vector<std::pair<std::tuple<matec_id, attack_vector_id, sub_bodypart_str_id>, int>> allowed;
    int64_t total = 0;
    for( const matec_id &id : all ) {
        if( !id.is_valid() || id->weighting < 0 || technique_family( *id ) != want ||
            std::find( tried.begin(), tried.end(), id ) != tried.end() ) {
            continue;
        }
        tried.push_back( id );
        if( const auto tec = attacker.evaluate_technique( id, target, weapon, critical, false, false ) ) {
            // as often as CDDA's pick would list it (a weighting comes from game data: a sum, no copies)
            const int weight = std::max( id->weighting, 1 );
            allowed.emplace_back( *tec, weight );
            total += weight;
        }
    }
    if( allowed.empty() ) {
        return std::nullopt;
    }
    n_hinted++;
    int64_t roll = rng( 1, static_cast<int>( std::min<int64_t>( total, INT_MAX ) ) );
    for( const auto &[tec, weight] : allowed ) {
        roll -= weight;
        if( roll <= 0 ) {
            return tec;
        }
    }
    return allowed.back().first;
}

swing_counts get_swing_counts()
{
    swing_counts c;
    c.asked = n_asked.load();
    c.accepted = n_accepted.load();
    c.refused = n_refused.load();
    c.parts = n_parts.load();
    c.hints = n_hints.load();
    c.hinted = n_hinted.load();
    return c;
}

} // namespace fp3d
