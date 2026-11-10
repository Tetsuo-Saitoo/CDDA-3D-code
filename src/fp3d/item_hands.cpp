#include "fp3d/item_hands.h"

#include <charconv>
#include <mutex>
#include <optional>
#include <random>
#include <system_error>
#include <vector>

#include "avatar.h"
#include "contents_change_handler.h"
#include "coordinates.h"
#include "enums.h"
#include "fp3d/bridge.h"
#include "fp3d/held.h"
#include "fp3d/snapshot.h"
#include "input_context.h"
#include "item.h"
#include "item_location.h"
#include "item_pocket.h"
#include "itype.h"
#include "map.h"
#include "map_selector.h"
#include "math_parser_diag_value.h"
#include "output.h"
#include "pickup.h"
#include "ret_val.h"
#include "type_id.h"
#include "string_formatter.h"
#include "translation.h"
#include "translations.h"
#include "units.h"

static const efftype_id effect_incorporeal( "incorporeal" );
static const trait_id trait_SHELL2( "SHELL2" );
static const trait_id trait_SHELL3( "SHELL3" );

namespace fp3d
{
namespace
{
const std::string uid_var = "fp3d_uid";
const std::string spot_var = "fp3d_spot";

std::mutex uid_mutex;

uint64_t new_uid()
{
    std::lock_guard<std::mutex> lk( uid_mutex );
    static std::mt19937_64 engine( std::random_device{}() );
    uint64_t v = 0;
    while( v == 0 ) {
        v = engine() & max_item_uid;
    }
    return v;
}

// All of `s` as a decimal number up to `max`; nullopt otherwise (no sign, no spaces, no leading junk).
std::optional<uint64_t> parse_decimal( const std::string &s, uint64_t max )
{
    if( s.empty() || s.size() > 16 ) {
        return std::nullopt;
    }
    uint64_t v = 0;
    const std::from_chars_result r = std::from_chars( s.data(), s.data() + s.size(), v );
    if( r.ec != std::errc() || r.ptr != s.data() + s.size() || v > max ) {
        return std::nullopt;
    }
    return v;
}

uint32_t type_index( const item &it )
{
    return intern_id( id_kind::itype, it.typeId().str() );
}

// An item var as text, only when it holds text (anything else reads as none).
std::string text_var( const item &it, const std::string &key )
{
    const diag_value *v = it.maybe_get_value( key );
    return v != nullptr && v->is_str() ? v->str() : std::string();
}

bool matches( const item &it, const item_ref &r )
{
    return !it.is_null() && read_item_handle( it ) == r.uid && type_index( it ) == r.type;
}

std::optional<tripoint_bub_ms> bubble_square( const map &here, const ui_point &p )
{
    const tripoint_bub_ms b = here.get_bub( tripoint_abs_ms( p.x, p.y, p.z ) );
    if( !here.inbounds( b ) ) {
        return std::nullopt;
    }
    return b;
}

// Where CDDA lets you pick up and drop by hand: your square and the eight around it, same level.
bool within_reach( const avatar &u, const tripoint_bub_ms &p )
{
    return p.z() == u.posz() && square_dist( u.pos_bub(), p ) <= 1;
}

// The one item with this handle; none when two share it (one the snapshot never listed, so never renumbered).
item_location find_on_square( map &here, const tripoint_bub_ms &p, const item_ref &r )
{
    item_location found;
    for( item &it : here.i_at( p ) ) {
        if( matches( it, r ) ) {
            if( found ) {
                return item_location();
            }
            found = item_location( map_cursor( p ), &it );
        }
    }
    return found;
}

item_location find_carried( avatar &u, const item_ref &r )
{
    item_location found;
    for( item_location &loc : u.all_items_loc() ) {
        if( loc && matches( *loc, r ) ) {
            if( found ) {
                return item_location();
            }
            found = loc;
        }
    }
    return found;
}

bool refuse( avatar &u, const std::string &why )
{
    u.add_msg_if_player( m_info, "%s", why );
    return false;
}

void after_inventory_change( avatar &u )
{
    u.flag_encumbrance();
    u.invalidate_weight_carried_cache();
}

bool take( avatar &u, const item_action &a )
{
    map &here = get_map();
    if( u.is_mounted() ) {
        return refuse( u, _( "You can't pick anything up while you're riding." ) );
    }
    const std::optional<tripoint_bub_ms> p = bubble_square( here, a.pos );
    if( !p || !within_reach( u, *p ) ) {
        return refuse( u, _( "You can't reach that from here." ) );
    }
    if( !here.accessible_items( *p ) ) {
        return refuse( u, _( "You can't get at the items in there." ) );
    }
    item_location loc = find_on_square( here, *p, a.item );
    if( !loc ) {
        return refuse( u, _( "That isn't there any more." ) );
    }
    item &it = *loc;
    const std::string name = it.tname();
    // As CDDA's pickup: spilt liquid and gas can't be taken by hand.
    if( it.made_of_from_type( phase_id::LIQUID ) ) {
        return refuse( u, _( "Spilt liquid cannot be picked back up.  Try mopping it instead." ) );
    }
    if( it.made_of_from_type( phase_id::GAS ) ) {
        return refuse( u, _( "Spilt gasses cannot be picked up.  They will disappear over time." ) );
    }
    if( it.is_frozen_liquid() ) {
        return refuse( u, _( "Chunks of frozen liquid cannot be picked up without the correct tools." ) );
    }
    // CDDA asks before stealing; with no menu, someone else's things stay where they are.
    if( !it.is_owned_by( u, true ) ) {
        return refuse( u, string_format( _( "The %s isn't yours." ), name ) );
    }
    if( it.is_bucket_nonempty() ) {
        return refuse( u, string_format( _( "Can't stash %s while it's not empty" ), it.display_name() ) );
    }
    if( a.hand == item_hand::main ) {
        if( const item_location weapon = u.get_wielded_item() ) {
            return refuse( u, string_format( _( "Your hand is full: you're holding your %s." ), weapon->tname() ) );
        }
        const ret_val<void> can = u.can_wield( it );
        if( !can.success() ) {
            return refuse( u, can.str() );
        }
        it.erase_var( spot_var );
        // CDDA's wield: obtain cost, the item leaves the square.
        if( !u.wield( loc ) ) {
            return refuse( u, string_format( _( "You can't pick up the %s." ), name ) );
        }
        u.add_msg_if_player( _( "You pick up the %s." ), name );
        return true;
    }
    if( a.hand == item_hand::off ) {
        const std::string why = offhand_unavailable( u );
        if( !why.empty() ) {
            return refuse( u, why );
        }
        if( it.is_two_handed( u ) ) {
            return refuse( u, string_format( _( "Your %s is too heavy to hold in one hand." ), name ) );
        }
    }
    // As CDDA's pickup: up to four times the carry weight, and room in a pocket for all of it.
    if( !u.can_pickWeight( it, false ) ) {
        return refuse( u, string_format( _( "The %s is too heavy to carry." ), name ) );
    }
    if( !u.can_stash( it ) ) {
        return refuse( u, string_format( _( "There's no room in your inventory for the %s." ), name ) );
    }
    const int cost = loc.obtain_cost( u );
    item copy = it;
    copy.erase_var( spot_var );
    if( a.hand == item_hand::off ) {
        mark_offhand_pending( copy );
    }
    item_location added = u.i_add( copy, true, nullptr, &it, /*allow_drop=*/false, /*allow_wield=*/false );
    if( !added ) {
        return refuse( u, string_format( _( "There's no room in your inventory for the %s." ), name ) );
    }
    contents_change_handler handler;
    handler.unseal_pocket_containing( loc );
    loc.remove_item();
    handler.handle_by( u );
    u.mod_moves( -cost );
    after_inventory_change( u );
    u.add_msg_if_player( _( "You pick up the %s." ), name );
    if( a.hand == item_hand::off ) {
        const std::string why = hold_carried( u, added );
        if( !why.empty() ) {
            clear_offhand_mark( *added );
            u.add_msg_if_player( m_info, "%s", why );
        }
    }
    return true;
}

bool put( avatar &u, const item_action &a )
{
    map &here = get_map();
    item_location src = find_carried( u, a.item );
    if( !src ) {
        return refuse( u, _( "You don't have that any more." ) );
    }
    if( u.is_worn( *src ) ) {
        return refuse( u, string_format( _( "Take off your %s first." ), src->tname() ) );
    }
    const ret_val<void> can_let_go = u.can_drop( *src );
    if( !can_let_go.success() ) {
        return refuse( u, can_let_go.str() );
    }
    item_location dst = find_carried( u, a.container );
    if( !dst ) {
        // As CDDA: riding, you can't reach into things on the ground.
        if( u.is_mounted() ) {
            return refuse( u, _( "You can't reach that while you're riding." ) );
        }
        const std::optional<tripoint_bub_ms> p = bubble_square( here, a.pos );
        if( !p || !within_reach( u, *p ) ) {
            return refuse( u, _( "You can't reach that from here." ) );
        }
        if( !here.accessible_items( *p ) ) {
            return refuse( u, _( "You can't get at the items in there." ) );
        }
        dst = find_on_square( here, *p, a.container );
    }
    if( !dst ) {
        return refuse( u, _( "That container isn't there any more." ) );
    }
    item &it = *src;
    if( src.get_item() == dst.get_item() || src.eventually_contains( dst ) ) {
        return refuse( u, string_format( _( "You can't put your %s inside itself." ), it.tname() ) );
    }
    const auto failed = [&]( const std::string & why ) {
        //~ %1$s: item we failed to put in the container, %2$s: container to put item in, %3$s: reason it failed
        return refuse( u, string_format( _( "Could not put %1$s into %2$s, %3$s." ), it.type_name(), dst->type_name(),
                                         why ) );
    };
    if( it.is_bucket_nonempty() ) {
        return failed( _( "item would spill" ) );
    }
    item_pocket *pocket = nullptr;
    ret_val<void> ok = ret_val<void>::make_success();
    if( a.pocket >= 0 ) {
        std::vector<item_pocket *> pockets = dst->get_all_contained_pockets();
        if( static_cast<size_t>( a.pocket ) >= pockets.size() ) {
            return failed( _( "it has no such pocket" ) );
        }
        pocket = pockets[a.pocket];
        if( pocket->is_forbidden() ) {
            return failed( _( "it has no such pocket" ) );
        }
    }
    // Already where it's going: nothing to move. (A copy would merge into the original.)
    if( src.has_parent() && ( pocket != nullptr ? pocket == src.parent_pocket() :
                              src.parent_item().get_item() == dst.get_item() ) ) {
        return failed( _( "it's already in there" ) );
    }
    if( pocket != nullptr ) {
        const ret_val<item_pocket::contain_code> r = pocket->can_contain( it );
        if( !r.success() ) {
            ok = ret_val<void>::make_failure( r.str() );
        }
    } else {
        ok = dst->can_contain_directly( it );
    }
    if( ok.success() ) {
        // CDDA offers to wield the container first when its parent is full; with no menu, refuse.
        ok = dst.parents_can_contain_recursive( &it );
    }
    if( !ok.success() ) {
        return failed( ok.str() );
    }
    // CDDA's insert cost: take it out, move it in.
    const int cost = src.obtain_cost( u ) + Pickup::cost_to_move_item( u, it );
    const std::string name = it.display_name();
    const std::string dst_name = dst->type->nname( 1 );
    item copy = it;
    clear_offhand_mark( copy );
    copy.erase_var( spot_var );
    if( pocket != nullptr ) {
        const ret_val<item *> in = pocket->insert_item( copy );
        if( !in.success() ) {
            return failed( in.str() );
        }
        if( pocket->sealed() ) {
            pocket->unseal();
        }
        dst->on_contents_changed();
    } else {
        const ret_val<void> in = dst->put_in( copy, pocket_type::CONTAINER, /*unseal_pockets=*/true, &u );
        if( !in.success() ) {
            return failed( in.str() );
        }
    }
    if( !src ) {
        debugmsg( "fp3d put: the source item went away during the insert" );
        return false;
    }
    contents_change_handler handler;
    handler.add_unsealed( dst );
    handler.unseal_pocket_containing( src );
    src.remove_item();
    handler.handle_by( u );
    u.mod_moves( -cost );
    after_inventory_change( u );
    //~ %1$s: item to put in the container, %2$s: container to put item in
    u.add_msg_if_player( _( "You put your %1$s into the %2$s." ), name, dst_name );
    return true;
}

bool drop( avatar &u, const item_action &a )
{
    map &here = get_map();
    item_location src = find_carried( u, a.item );
    if( !src ) {
        return refuse( u, _( "You don't have that any more." ) );
    }
    if( u.is_worn( *src ) ) {
        return refuse( u, string_format( _( "Take off your %s first." ), src->tname() ) );
    }
    const ret_val<void> can_let_go = u.can_drop( *src );
    if( !can_let_go.success() ) {
        return refuse( u, can_let_go.str() );
    }
    // Items that act when dropped (CDDA's drop_action) go through CDDA's own drop.
    if( src->type->drop_action ) {
        return refuse( u, string_format( _( "You can't put your %s down by hand." ), src->tname() ) );
    }
    const std::optional<tripoint_bub_ms> p = bubble_square( here, a.pos );
    if( !p || !within_reach( u, *p ) ) {
        return refuse( u, _( "You can't reach that from here." ) );
    }
    const std::string name = src->tname();
    if( !here.can_put_items_ter_furn( *p ) ) {
        return refuse( u, string_format( _( "You can't put your %s there." ), name ) );
    }
    // CDDA's drop spills onto the next squares when one is full; a placed item stays where it was put or not at all.
    if( here.free_volume( *p ) < src->volume() ) {
        return refuse( u, string_format( _( "There's no room for your %s there." ), name ) );
    }
    const int cost = src.obtain_cost( u ) + Pickup::cost_to_move_item( u, *src );
    item copy = *src;
    clear_offhand_mark( copy );
    if( a.has_spot ) {
        copy.set_var( spot_var, std::to_string( a.spot_x ) + "," + std::to_string( a.spot_y ) );
    } else {
        copy.erase_var( spot_var );
    }
    const item &placed = here.add_item_or_charges( *p, copy, /*overflow=*/false );
    if( placed.is_null() ) {
        return refuse( u, string_format( _( "There's no room for your %s there." ), name ) );
    }
    contents_change_handler handler;
    handler.unseal_pocket_containing( src );
    src.remove_item();
    handler.handle_by( u );
    u.mod_moves( -cost );
    after_inventory_change( u );
    u.add_msg_if_player( a.has_spot ? _( "You put your %s down." ) : _( "You drop your %s." ), name );
    return true;
}
} // namespace

uint64_t read_item_handle( const item &it )
{
    if( !it.has_var( uid_var ) ) {
        return 0;
    }
    return parse_decimal( text_var( it, uid_var ), max_item_uid ).value_or( 0 );
}

uint64_t item_handle( item &it, std::unordered_set<uint64_t> *seen )
{
    uint64_t uid = read_item_handle( it );
    if( uid == 0 || ( seen != nullptr && seen->count( uid ) != 0 ) ) {
        do {
            uid = new_uid();
        } while( seen != nullptr && seen->count( uid ) != 0 );
        it.set_var( uid_var, std::to_string( uid ) );
    }
    if( seen != nullptr ) {
        seen->insert( uid );
    }
    return uid;
}

bool item_spot( const item &it, uint16_t &x, uint16_t &y )
{
    if( !it.has_var( spot_var ) ) {
        return false;
    }
    const std::string s = text_var( it, spot_var );
    const size_t comma = s.find( ',' );
    if( s.size() > 16 || comma == std::string::npos ) {
        return false;
    }
    const std::optional<uint64_t> px = parse_decimal( s.substr( 0, comma ), spot_scale - 1 );
    const std::optional<uint64_t> py = parse_decimal( s.substr( comma + 1 ), spot_scale - 1 );
    if( !px || !py ) {
        return false;
    }
    x = static_cast<uint16_t>( *px );
    y = static_cast<uint16_t>( *py );
    return true;
}

void register_item_actions( input_context &ctxt )
{
    ctxt.register_action( item_action_name, no_translation( "take, put or drop one item" ) );
}

bool is_item_action( const std::string &action )
{
    return action == item_action_name;
}

bool run_item_action( avatar &u )
{
    const std::optional<item_action> a = take_item_action();
    if( !a ) {
        return false;
    }
    // As CDDA's do_regular_action for pickup and drop.
    if( u.has_active_mutation( trait_SHELL2 ) || u.has_active_mutation( trait_SHELL3 ) ) {
        return refuse( u, _( "You can't do that while you're in your shell." ) );
    }
    if( u.has_effect( effect_incorporeal ) && a->op != item_op::drop ) {
        return refuse( u, _( "You lack the substance to affect anything." ) );
    }
    switch( a->op ) {
        case item_op::take:
            return take( u, *a );
        case item_op::put:
            return put( u, *a );
        case item_op::drop:
            return drop( u, *a );
    }
    return false;
}

} // namespace fp3d
