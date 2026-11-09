#include "fp3d/held.h"

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

// Carried items marked as held (normally at most one); not the wielded item, not worn ones.
std::vector<item_location> marked_items( avatar &u )
{
    std::vector<item_location> out;
    const item_location weapon = u.get_wielded_item();
    for( item_location &loc : u.all_items_loc() ) {
        if( loc->has_var( offhand_var ) && !( weapon && weapon.get_item() == loc.get_item() ) &&
            !u.is_worn( *loc ) ) {
            out.push_back( loc );
        }
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
    item_location loc = game_menus::inv::titled_filter_menu( [&u, weapon]( const item & it ) {
        return &it != weapon && !u.is_worn( it ) && !it.made_of( phase_id::LIQUID ) && !it.is_two_handed( u );
    }, u, _( "Hold in your off hand" ), -1, _( "You have nothing to hold in your off hand." ) );
    if( !loc ) {
        return false;
    }
    for( item_location &m : marked_items( u ) ) {
        m->erase_var( offhand_var );
    }
    loc->set_var( offhand_var, true );
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
