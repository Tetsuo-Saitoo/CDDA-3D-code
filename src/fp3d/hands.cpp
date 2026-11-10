#include "fp3d/hands.h"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#include "avatar.h"
#include "bodypart.h"
#include "coordinates.h"
#include "creature.h"
#include "creature_tracker.h"
#include "fp3d/held.h"
#include "fp3d/swing.h"
#include "input_context.h"
#include "item.h"
#include "item_location.h"
#include "map.h"
#include "monster.h"
#include "npc.h"
#include "translation.h"
#include "translations.h"

namespace fp3d
{
namespace
{
const std::string attack_prefix = "fp3d_attack_";
// The furthest a reach weapon strikes along a direction, in squares.
constexpr int max_reach = 3;

struct dir_step {
    const char *name;
    int dx;
    int dy;
};
constexpr std::array<dir_step, 8> dirs = { {
        { "UP", 0, -1 }, { "RIGHTUP", 1, -1 }, { "RIGHT", 1, 0 }, { "RIGHTDOWN", 1, 1 },
        { "DOWN", 0, 1 }, { "LEFTDOWN", -1, 1 }, { "LEFT", -1, 0 }, { "LEFTUP", -1, -1 }
    }
};

bool offhand_active = false;
// What the off hand strikes with during an off-hand strike (fp3d/held.h; empty = unarmed). Optional so no
// item_location is built during static initialisation.
std::optional<item_location> offhand_weapon_loc;

// Set for the length of one off-hand strike, also when the attack throws.
struct offhand_guard {
    explicit offhand_guard( item_location weapon ) {
        offhand_active = true;
        offhand_weapon_loc = std::move( weapon );
    }
    ~offhand_guard() {
        offhand_active = false;
        offhand_weapon_loc.reset();
    }
    offhand_guard( const offhand_guard & ) = delete;
    offhand_guard &operator=( const offhand_guard & ) = delete;
};

bool starts_with( const std::string &s, const std::string &p )
{
    return s.size() >= p.size() && s.compare( 0, p.size(), p ) == 0;
}
} // namespace

void register_hand_actions( input_context &ctxt )
{
    for( const char *hand : { "main", "off" } ) {
        for( const dir_step &d : dirs ) {
            ctxt.register_action( attack_prefix + hand + "_" + d.name,
                                  no_translation( std::string( "attack, " ) + hand + " hand" ) );
            ctxt.register_action( attack_prefix + hand + "_aim_" + d.name,
                                  no_translation( std::string( "aimed attack, " ) + hand + " hand" ) );
        }
    }
}

bool is_hand_action( const std::string &action )
{
    return starts_with( action, attack_prefix );
}

bool hand_attack( avatar &u, const std::string &action )
{
    if( !is_hand_action( action ) ) {
        return false;
    }
    std::string rest = action.substr( attack_prefix.size() );
    bool off = false;
    if( starts_with( rest, "off_" ) ) {
        off = true;
        rest = rest.substr( 4 );
    } else if( starts_with( rest, "main_" ) ) {
        rest = rest.substr( 5 );
    } else {
        return false;
    }
    const bool aimed = starts_with( rest, "aim_" );
    if( aimed ) {
        rest = rest.substr( 4 );
    }
    const auto d = std::find_if( dirs.begin(), dirs.end(), [&rest]( const dir_step & s ) {
        return rest == s.name;
    } );
    if( d == dirs.end() ) {
        return false;
    }
    const tripoint_bub_ms target = u.pos_bub() + tripoint_rel_ms( d->dx, d->dy, 0 );
    Creature *c = get_creature_tracker().creature_at<Creature>( target );
    // Nobody next to you that way: the main hand's weapon may reach further along it (CDDA's reach attack).
    tripoint_bub_ms reach_target = target;
    if( ( c == nullptr || c == &u ) && !off ) {
        const item_location weapon = u.get_wielded_item();
        const int reach = weapon ? std::clamp( weapon->current_reach_range( u ), 1, max_reach ) : 1;
        for( int r = 2; r <= reach; r++ ) {
            const tripoint_bub_ms q = u.pos_bub() + tripoint_rel_ms( d->dx * r, d->dy * r, 0 );
            // only a creature you see: a strike must not find (or give away) someone unseen behind a wall
            Creature *beyond = get_creature_tracker().creature_at<Creature>( q );
            if( beyond != nullptr && u.sees( get_map(), *beyond ) ) {
                c = beyond;
                reach_target = q;
                break;
            }
        }
    }
    if( c == nullptr || c == &u ) {
        u.add_msg_if_player( m_info, _( "Nothing there to hit." ) );
        return false;
    }
    if( const npc *guy = dynamic_cast<const npc *>( c ) ) {
        if( !guy->is_enemy() ) {
            u.add_msg_if_player( m_info, _( "You hold back from hitting %s." ), guy->get_name() );
            return false;
        }
    } else if( const monster *mon = dynamic_cast<const monster *>( c ) ) {
        if( mon->friendly != 0 ) {
            u.add_msg_if_player( m_info, _( "You hold back from hitting your %s." ), mon->get_name() );
            return false;
        }
    }
    // An aimed strike asks the client which part it aims at (fp3d/swing.h).
    std::optional<aimed_strike> aim;
    if( aimed ) {
        aim.emplace();
    }
    if( !off && reach_target != target ) {
        const int moves = u.get_moves();
        u.reach_attack( reach_target );
        return u.get_moves() != moves;
    }
    if( !off ) {
        return u.melee_attack( *c, true );
    }
    const item_location weapon = u.get_wielded_item();
    if( weapon && weapon->is_two_handed( u ) ) {
        u.add_msg_if_player( m_info, _( "Both your hands are on your %s." ), weapon->tname() );
        return false;
    }
    if( u.is_limb_broken( bodypart_id( "arm_l" ) ) ) {
        u.add_msg_if_player( m_bad, _( "Your left arm is broken." ) );
        return false;
    }
    const offhand_guard guard( offhand_item( u ) );
    return u.melee_attack( *c, true );
}

bool offhand_strike()
{
    return offhand_active;
}

item_location offhand_weapon()
{
    return offhand_active && offhand_weapon_loc ? *offhand_weapon_loc : item_location();
}

std::vector<std::string> hand_dir_names()
{
    std::vector<std::string> out;
    for( const dir_step &d : dirs ) {
        out.emplace_back( d.name );
    }
    return out;
}

bool hand_dir( const std::string &name, int &dx, int &dy )
{
    const auto d = std::find_if( dirs.begin(), dirs.end(), [&name]( const dir_step & s ) {
        return name == s.name;
    } );
    if( d == dirs.end() ) {
        return false;
    }
    dx = d->dx;
    dy = d->dy;
    return true;
}
} // namespace fp3d
