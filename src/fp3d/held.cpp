#include "fp3d/held.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "avatar.h"
#include "bodypart.h"
#include "calendar.h"
#include "enums.h"
#include "fp3d/game_access.h"
#include "fp3d/hands.h"
#include "game.h"
#include "game_inventory.h"
#include "handle_liquid.h"
#include "input_context.h"
#include "item.h"
#include "item_location.h"
#include "map.h"
#include "map_selector.h"
#include "math_parser_diag_value.h"
#include "output.h"
#include "player_activity.h"
#include "string_formatter.h"
#include "translation.h"
#include "translations.h"
#include "type_id.h"
#include "units.h"
#include "veh_interact.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "vpart_range.h"

namespace fp3d
{
namespace
{
const std::string offhand_var = "fp3d_offhand";
const std::string token_value = "fp3d_offhand_token";
const std::string hold_action = "fp3d_hold_off";
const std::string stow_action = "fp3d_stow_off";
const std::string use_prefix = "fp3d_use_";
const quality_id qual_HOSE( "HOSE" );
const activity_id ACT_FILL_LIQUID( "ACT_FILL_LIQUID" );
constexpr size_t max_name_bytes = 128;

std::mutex held_mutex;
held_view held_latest;
uint64_t held_seq = 0;

bool starts_with( const std::string &s, const std::string &p )
{
    return s.size() >= p.size() && s.compare( 0, p.size(), p ) == 0;
}

// Why the off hand can't hold anything right now ("" = it can).
std::string offhand_blocked( avatar &u )
{
    const item_location weapon = u.get_wielded_item();
    if( weapon && weapon->is_two_handed( u ) ) {
        return string_format( _( "Both your hands are on your %s." ), weapon->tname() );
    }
    if( u.is_limb_broken( bodypart_id( "arm_l" ) ) ) {
        return _( "Your left arm is broken." );
    }
    return {};
}

// The avatar's current off-hand token (0 = nothing held). The held item's var carries the same token, so an item that
// left the avatar (dropped, thrown, traded, wielded) isn't held again when it comes back: its token is stale.
double offhand_token( const avatar &u )
{
    const diag_value *v = u.maybe_get_value( token_value );
    return v != nullptr && v->is_dbl() ? v->dbl() : 0.0;
}

// Carried items marked as held (normally at most one); not the wielded item, not worn ones. Stale marks on carried
// items are erased, and the token is dropped once its item is gone.
std::vector<item_location> marked_items( avatar &u )
{
    std::vector<item_location> out;
    const double token = offhand_token( u );
    const item_location weapon = u.get_wielded_item();
    for( item_location &loc : u.all_items_loc() ) {
        if( !loc->has_var( offhand_var ) ) {
            continue;
        }
        if( token == 0.0 || loc->get_var( offhand_var, 0.0 ) != token ) {
            loc->erase_var( offhand_var );
        } else if( !( weapon && weapon.get_item() == loc.get_item() ) && !u.is_worn( *loc ) ) {
            out.push_back( loc );
        }
    }
    if( out.empty() && token != 0.0 ) {
        u.remove_value( token_value );
    }
    return out;
}

bool hold( avatar &u )
{
    const std::string why = offhand_blocked( u );
    if( !why.empty() ) {
        u.add_msg_if_player( m_info, why );
        return false;
    }
    const item *weapon = u.get_wielded_item() ? u.get_wielded_item().get_item() : nullptr;
    const auto carried = [&u, weapon]( const item & it ) {
        return &it != weapon && !u.is_worn( it ) && !it.made_of( phase_id::LIQUID );
    };
    const auto one_handed = [&u, carried]( const item & it ) {
        return carried( it ) && !it.is_two_handed( u );
    };
    // CDDA's menu shows a popup (a key prompt) when nothing fits: say why in a message instead.
    const item *heavy = nullptr;
    bool any = false;
    for( const item_location &l : u.all_items_loc() ) {
        any = any || one_handed( *l );
        if( heavy == nullptr && carried( *l ) && l->is_two_handed( u ) ) {
            heavy = l.get_item();
        }
    }
    if( !any ) {
        if( heavy != nullptr ) {
            u.add_msg_if_player( m_info, _( "Your %s is too heavy to hold in one hand." ), heavy->tname() );
        } else {
            u.add_msg_if_player( m_info, _( "You have nothing to hold in your off hand." ) );
        }
        return false;
    }
    item_location loc = game_menus::inv::titled_filter_menu( one_handed, u, _( "Hold in your off hand" ), -1,
                        _( "You have nothing to hold in your off hand." ) );
    if( !loc ) {
        return false;
    }
    for( item_location &m : marked_items( u ) ) {
        m->erase_var( offhand_var );
    }
    // A fresh token: the turn number (never reused), above the last one.
    const double token = std::max( offhand_token( u ) + 1.0,
                                   static_cast<double>( to_turns<int64_t>( calendar::turn - calendar::turn_zero ) ) );
    u.set_value( token_value, token );
    loc->set_var( offhand_var, token );
    u.mod_moves( -u.item_handling_cost( *loc ) );
    u.add_msg_if_player( _( "You hold your %s in your off hand." ), loc->tname() );
    return true;
}

bool stow( avatar &u )
{
    std::vector<item_location> marked = marked_items( u );
    if( marked.empty() ) {
        u.add_msg_if_player( m_info, _( "Your off hand is empty." ) );
        return false;
    }
    u.mod_moves( -u.item_handling_cost( *marked.front() ) );
    u.add_msg_if_player( _( "You put away your %s." ), marked.front()->tname() );
    for( item_location &m : marked ) {
        m->erase_var( offhand_var );
    }
    u.remove_value( token_value );
    return true;
}

// Siphons a tank of `veh` (the one on `target` if it holds liquid, else the only one, else the player picks) into
// `container` with CDDA's liquid fill activity.
bool siphon( avatar &u, vehicle &veh, const tripoint_bub_ms &target, const item_location &container )
{
    map &here = get_map();
    if( veh.is_moving() ) {
        u.add_msg_if_player( m_info, _( "You can't siphon from a moving vehicle." ) );
        return false;
    }
    std::vector<vpart_reference> tanks;
    for( const vpart_reference &vp : veh.get_any_parts( VPFLAG_FLUIDTANK ) ) {
        if( vp.part().contains_liquid() ) {
            tanks.push_back( vp );
        }
    }
    if( tanks.empty() ) {
        u.add_msg_if_player( m_info, _( "The %s has no liquid left to siphon." ), veh.name );
        return false;
    }
    std::optional<vpart_reference> tank;
    for( const vpart_reference &vp : tanks ) {
        if( vp.pos_bub( here ) == target ) {
            tank = vp;
            break;
        }
    }
    if( !tank && tanks.size() == 1 ) {
        tank = tanks.front();
    }
    if( !tank ) {
        tank = veh_interact::select_part( here, veh, []( const map &, const vehicle_part & pt ) {
            return pt.contains_liquid();
        }, _( "Siphon from which tank?" ) );
    }
    if( !tank ) {
        return false;
    }
    item liquid( tank->part().get_base().only_item() );
    std::string err;
    if( container->get_remaining_capacity_for_liquid( liquid, u, &err ) <= 0 ) {
        u.add_msg_if_player( m_info, err.empty() ? string_format( _( "Your %1$s can't hold any %2$s." ),
                             container->tname(), liquid.tname() ) : err );
        return false;
    }
    liquid_dest_opt dest;
    dest.dest_opt = LD_ITEM;
    dest.item_loc = container;
    if( !liquid_handler::perform_liquid_transfer( liquid, nullptr, &veh, tank->part_index(), nullptr, dest,
            false ) ) {
        return false;
    }
    u.add_msg_if_player( _( "You siphon %1$s into your %2$s." ), item::tname( liquid.typeId() ),
                         container->tname() );
    return true;
}

// The first liquid in a container (nullptr if none).
item *liquid_in( item_location container )
{
    for( item *c : container->all_items_top() ) {
        if( c->made_of( phase_id::LIQUID ) ) {
            return c;
        }
    }
    return nullptr;
}

// A tank of `veh` that takes `liquid`: the one on `target`, else the first.
std::optional<vpart_reference> tank_for( vehicle &veh, const tripoint_bub_ms &target, const item &liquid )
{
    map &here = get_map();
    std::optional<vpart_reference> first;
    for( const vpart_reference &vp : veh.get_any_parts( VPFLAG_FLUIDTANK ) ) {
        if( !vp.part().can_reload( liquid ) ) {
            continue;
        }
        if( vp.pos_bub( here ) == target ) {
            return vp;
        }
        if( !first ) {
            first = vp;
        }
    }
    return first;
}

// Pours `liquid` (in the held `container`) into `tank`, like CDDA's vehicle refill.
bool refuel( avatar &u, item_location container, item &liquid, const vpart_reference &tank )
{
    if( tank.vehicle().is_moving() ) {
        u.add_msg_if_player( m_info, _( "You can't refuel a moving vehicle." ) );
        return false;
    }
    item_location poured( container, &liquid );
    if( !u.pour_into( tank, *poured ) ) {
        u.add_msg_if_player( m_info, _( "The %1$s won't take any more %2$s." ), tank.part().name(),
                             poured->tname() );
        return false;
    }
    if( poured->charges <= 0 ) {
        poured.remove_item();
    }
    container->on_contents_changed();
    u.mod_moves( -1000 ); // as CDDA's refill (veh_interact::do_refill)
    return true;
}

// Fills the held `container` from `lying` (a liquid item there) or else `endless` (terrain), with CDDA's fill
// activity: one turn per second while the client watches the level rise.
bool fill( const tripoint_bub_ms &target, const item_location &container, item &endless, item *lying )
{
    liquid_dest_opt dest;
    dest.dest_opt = LD_ITEM;
    dest.item_loc = container;
    if( lying != nullptr ) {
        item_location loc( map_cursor( target ), lying );
        if( !liquid_handler::perform_liquid_transfer( loc, dest ) ) {
            return false;
        }
    } else if( !liquid_handler::perform_liquid_transfer( endless, &target, nullptr, -1, nullptr, dest,
               false ) ) {
        return false;
    }
    return true;    // CDDA's activity tells how it went ("You filled ... to the brim", "won't hold")
}

bool use_on( avatar &u, const tripoint_bub_ms &target )
{
    map &here = get_map();
    const item_location main = u.get_wielded_item();
    const item_location off = offhand_item( u );
    const auto is_hose = []( const item_location & l ) {
        return l && l->has_quality_nonrecursive( qual_HOSE );
    };
    item_location hose;
    item_location container;
    if( is_hose( main ) ) {
        hose = main;
        container = off;
    } else if( is_hose( off ) ) {
        hose = off;
        container = main;
    }
    const optional_vpart_position vp = here.veh_at( target );
    if( vp && hose ) {
        if( !container || container->get_total_capacity() <= 0_ml ) {
            u.add_msg_if_player( m_info, _( "Hold a container in your other hand to siphon into." ) );
            return false;
        }
        return siphon( u, vp->vehicle(), target, container );
    }
    std::vector<item_location> containers;
    for( const item_location &l : { main, off } ) {
        if( l && l->is_watertight_container() ) {
            containers.push_back( l );
        }
    }
    // A held container's liquid into a vehicle tank that takes it (refuel from a jerrycan).
    if( vp ) {
        for( const item_location &c : containers ) {
            item *liquid = liquid_in( c );
            if( liquid == nullptr ) {
                continue;
            }
            if( const std::optional<vpart_reference> tank = tank_for( vp->vehicle(), target, *liquid ) ) {
                return refuel( u, c, *liquid, *tank );
            }
        }
    }
    // A held container filled from the liquid there: endless (river, lake) or lying there (keg, barrel, puddle).
    item endless = here.liquid_from( target );
    item *lying = nullptr;
    if( endless.is_null() ) {
        for( item &it : here.i_at( target ) ) {
            if( it.made_of( phase_id::LIQUID ) ) {
                lying = &it;
                break;
            }
        }
    }
    const item *source = lying != nullptr ? lying : endless.is_null() ? nullptr : &endless;
    if( source != nullptr && source->made_of( phase_id::LIQUID ) && !containers.empty() ) {
        std::string err;
        for( const item_location &c : containers ) {
            if( c->get_remaining_capacity_for_liquid( *source, u, err.empty() ? &err : nullptr ) > 0 ) {
                return fill( target, c, endless, lying );
            }
        }
        u.add_msg_if_player( m_info, err.empty() ? string_format( _( "Your %1$s can't hold any %2$s." ),
                             containers.front()->tname(), source->tname() ) : err );
        return false;
    }
    fp3d_game_access::examine( *g, target );
    return true;
}

std::string capped( std::string s )
{
    if( s.size() > max_name_bytes ) {
        s.resize( max_name_bytes );
    }
    return s;
}

hand_view view_of( const item_location &loc )
{
    hand_view h;
    if( !loc ) {
        return h;
    }
    const item &it = *loc;
    h.held = true;
    h.id = capped( it.typeId().str() );
    h.name = capped( remove_color_tags( item::tname( it.typeId() ) ) );
    const units::volume capacity = it.get_total_capacity();
    if( capacity > 0_ml ) {
        h.capacity_ml = units::to_milliliter( capacity );
        h.ml = units::to_milliliter( it.get_total_contained_volume() );
        for( const item *c : it.all_items_top() ) {
            if( c->made_of( phase_id::LIQUID ) ) {
                h.liquid = capped( remove_color_tags( item::tname( c->typeId() ) ) );
                break;
            }
        }
    }
    return h;
}
} // namespace

void register_held_actions( input_context &ctxt )
{
    ctxt.register_action( hold_action, no_translation( "hold in the off hand" ) );
    ctxt.register_action( stow_action, no_translation( "put away what the off hand holds" ) );
    for( const std::string &d : hand_dir_names() ) {
        ctxt.register_action( use_prefix + d, no_translation( "use what you hold on what you face" ) );
    }
}

bool is_held_action( const std::string &action )
{
    return action == hold_action || action == stow_action || starts_with( action, use_prefix );
}

bool held_action( avatar &u, const std::string &action )
{
    if( action == hold_action ) {
        return hold( u );
    }
    if( action == stow_action ) {
        return stow( u );
    }
    int dx = 0;
    int dy = 0;
    if( starts_with( action, use_prefix ) && hand_dir( action.substr( use_prefix.size() ), dx, dy ) ) {
        return use_on( u, u.pos_bub() + tripoint_rel_ms( dx, dy, 0 ) );
    }
    return false;
}

item_location offhand_item( avatar &u )
{
    if( !offhand_blocked( u ).empty() ) {
        return item_location();
    }
    std::vector<item_location> marked = marked_items( u );
    return marked.empty() ? item_location() : marked.front();
}

bool watched_activity( const avatar &u )
{
    return u.activity && u.activity.id() == ACT_FILL_LIQUID;
}

void publish_held()
{
    avatar &u = get_avatar();
    held_view v;
    v.turn = to_turns<int64_t>( calendar::turn - calendar::turn_zero );
    v.main = view_of( u.get_wielded_item() );
    v.off = view_of( offhand_item( u ) );
    if( u.activity ) {
        v.activity = capped( u.activity.id().str() );
    }
    std::lock_guard<std::mutex> lk( held_mutex );
    v.seq = ++held_seq;
    held_latest = std::move( v );
}

held_view latest_held()
{
    std::lock_guard<std::mutex> lk( held_mutex );
    return held_latest;
}
} // namespace fp3d
