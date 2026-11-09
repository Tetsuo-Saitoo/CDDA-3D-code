#include "fp3d/hands.h"

#include <algorithm>
#include <array>

#include "avatar.h"
#include "bodypart.h"
#include "coordinates.h"
#include "creature.h"
#include "creature_tracker.h"
#include "input_context.h"
#include "item.h"
#include "item_location.h"
#include "monster.h"
#include "npc.h"
#include "translation.h"
#include "translations.h"

namespace fp3d
{
namespace
{
const std::string attack_prefix = "fp3d_attack_";

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

// Set for the length of one off-hand strike, also when the attack throws.
struct offhand_guard {
    offhand_guard() {
        offhand_active = true;
    }
    ~offhand_guard() {
        offhand_active = false;
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
    const auto d = std::find_if( dirs.begin(), dirs.end(), [&rest]( const dir_step & s ) {
        return rest == s.name;
    } );
    if( d == dirs.end() ) {
        return false;
    }
    const tripoint_bub_ms target = u.pos_bub() + tripoint_rel_ms( d->dx, d->dy, 0 );
    Creature *c = get_creature_tracker().creature_at<Creature>( target );
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
    const offhand_guard guard;
    return u.melee_attack( *c, true );
}

bool offhand_strike()
{
    return offhand_active;
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
