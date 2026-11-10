#include "fp3d/item_hands.h"

#include <charconv>
#include <mutex>
#include <optional>
#include <random>
#include <system_error>
#include <vector>

#include "activity_actor_definitions.h"
#include "avatar.h"
#include "calendar.h"
#include "contents_change_handler.h"
#include "coordinates.h"
#include "enums.h"
#include "flag.h"
#include "fp3d/bridge.h"
#include "fp3d/gun_faults.h"
#include "fp3d/held.h"
#include "fp3d/snapshot.h"
#include "game_constants.h"
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
#include "pocket_type.h"
#include "ret_val.h"
#include "type_id.h"
#include "string_formatter.h"
#include "translation.h"
#include "translations.h"
#include "units.h"

static const ammotype ammo_plutonium( "plutonium" );

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

// Why the carried item `loc` can't be reached into ("" = it can): it's in a sealed or forbidden pocket.
std::string pocket_block( const item_location &loc )
{
    for( item_location at = loc; at.has_parent(); at = at.parent_item() ) {
        const item_pocket *pk = at.parent_pocket();
        if( pk == nullptr ) {
            break;
        }
        if( pk->is_forbidden() ) {
            return string_format( _( "You can't get at the %s in there." ), loc->tname() );
        }
        if( pk->sealed() ) {
            //~ %1$s: an item, %2$s: the sealed container it's in
            return string_format( _( "The %1$s is sealed in the %2$s.  Open it first." ), loc->tname(),
                                  at.parent_item()->type_name() );
        }
    }
    return {};
}

// The item `r` names: carried (not in a sealed or forbidden pocket), else lying on `pos` where `take` could get it
// (in reach, not ridden past, in sight, not someone else's). Empty, with `why` set, otherwise.
item_location find_reachable( avatar &u, const ui_point &pos, const item_ref &r, std::string &why )
{
    item_location loc = find_carried( u, r );
    if( loc ) {
        why = pocket_block( loc );
        return why.empty() ? loc : item_location();
    }
    map &here = get_map();
    if( u.is_mounted() ) {
        why = _( "You can't reach that while you're riding." );
        return item_location();
    }
    const std::optional<tripoint_bub_ms> p = bubble_square( here, pos );
    if( !p || !within_reach( u, *p ) ) {
        why = _( "You can't reach that from here." );
        return item_location();
    }
    if( !here.accessible_items( *p ) || !here.could_see_items( *p, u.pos_bub() ) ) {
        why = _( "You can't get at the items in there." );
        return item_location();
    }
    loc = find_on_square( here, *p, r );
    if( !loc ) {
        why = _( "That isn't there any more." );
        return item_location();
    }
    if( !loc->is_owned_by( u, true ) ) {
        why = string_format( _( "The %s isn't yours." ), loc->tname() );
        return item_location();
    }
    return loc;
}

// Something CDDA's unload would take out of `it`.
bool has_load( const item &it )
{
    return it.magazine_current() != nullptr || it.ammo_remaining() > 0 || it.casings_count() > 0;
}

// What CDDA's unload offers for `it` (Character::unload): the item, then its loaded gunmods and full brass catchers.
std::vector<item *> unload_options( item &it )
{
    std::vector<item *> opts( 1, &it );
    for( item *e : it.gunmods() ) {
        if( ( e->is_gun() && !e->has_flag( flag_NO_UNLOAD ) && has_load( *e ) ) ||
            ( e->has_flag( flag_BRASS_CATCHER ) && !e->is_container_empty() ) ) {
            opts.push_back( e );
        }
    }
    return opts;
}

// Items inside a gun or magazine are never held: an off-hand mark one carries (a magazine loaded from the off hand)
// is stale, and must not come back out with it.
void clear_loaded_marks( item &it )
{
    for( const pocket_type pt : {
             pocket_type::MAGAZINE, pocket_type::MAGAZINE_WELL
         } ) {
        for( item *e : it.all_items_top( pt ) ) {
            clear_offhand_mark( *e );
            clear_loaded_marks( *e );
        }
    }
}

// Why `hand` can't take `out` (what an unload of `source` puts there) now; "" = it can. As `take` checks a hand.
std::string hand_refusal( avatar &u, const item &out, item_hand hand, const item_location &source )
{
    if( hand == item_hand::main ) {
        if( const item_location weapon = u.get_wielded_item() ) {
            return string_format( _( "Your hand is full: you're holding your %s." ), weapon->tname() );
        }
        const ret_val<void> can = u.can_wield( out );
        return can.success() ? std::string() : can.str();
    }
    const std::string why = offhand_unavailable( u );
    if( !why.empty() ) {
        return why;
    }
    if( const item_location held = offhand_item( u ) ) {
        if( held.get_item() == source.get_item() ) {
            return string_format( _( "Your off hand is full: you're holding your %s." ), held->tname() );
        }
    }
    if( out.is_two_handed( u ) ) {
        return string_format( _( "Your %s is too heavy to hold in one hand." ), out.tname() );
    }
    if( !source.held_by( u ) && !u.can_pickWeight( out, false ) ) {
        return string_format( _( "The %s is too heavy to carry." ), out.tname() );
    }
    if( !u.can_stash( out ) ) {
        return string_format( _( "There's no room in your inventory for the %s." ), out.tname() );
    }
    return {};
}

// Puts `obj` (out of what's being unloaded) where `hand` says, once hand_refusal passed: the inventory as CDDA's
// unload does (add_or_drop_with_msg: dropped when it doesn't fit), wielded, or stashed and held in the off hand.
// The hands get a copy; the moves are the unload's.
bool place( avatar &u, item &obj, item_hand hand, const item *avoid, const item *original )
{
    if( hand == item_hand::pack ) {
        return u.add_or_drop_with_msg( obj, true, avoid, original );
    }
    item copy = obj;
    clear_offhand_mark( copy );
    copy.erase_var( spot_var );
    if( hand == item_hand::main ) {
        return u.Character::wield( copy, 0 );
    }
    mark_offhand_pending( copy );
    item_location added = u.i_add( copy, true, avoid, nullptr, /*allow_drop=*/false, /*allow_wield=*/false );
    if( !added ) {
        return false;
    }
    const std::string why = hold_carried( u, added );
    if( !why.empty() ) {
        clear_offhand_mark( *added );
        u.add_msg_if_player( m_info, "%s", why );
    }
    return true;
}

// unload_activity_actor::unload for a magazine, at once for the same moves (an activity can't send its rounds to a
// hand): the first rounds into `hand`, the rest (other ammo mixed in, a belt's links) into the inventory.
bool unload_magazine_now( avatar &u, item_location &targloc, int moves, item_hand hand )
{
    item &mag = *targloc;
    int qty = 0;
    bool any = false;
    bool in_hand = false;
    std::vector<item *> taken;
    for( item *c : mag.all_items_top() ) {
        if( c->ammo_type() == ammo_plutonium ) {
            c->charges /= PLUTONIUM_CHARGES;
        }
        const item_hand where = !in_hand && !c->has_flag( flag_CASING ) ? hand : item_hand::pack;
        if( place( u, *c, where, &mag, c ) ) {
            in_hand = in_hand || where != item_hand::pack;
            qty += c->charges;
            taken.push_back( c );
            any = true;
        }
    }
    for( item *c : taken ) {
        mag.remove_item( *c );
    }
    if( mag.is_ammo_belt() ) {
        if( mag.type->magazine->linkage ) {
            item link( *mag.type->magazine->linkage, calendar::turn, qty );
            any = u.add_or_drop_with_msg( link, true ) || any;
        }
        if( any ) {
            u.add_msg_if_player( _( "You disassemble your %s." ), mag.tname() );
        }
    } else if( any ) {
        u.add_msg_if_player( _( "You unload your %s." ), mag.tname() );
    }
    if( mag.has_flag( flag_MAG_DESTROY ) && mag.ammo_remaining() == 0 ) {
        targloc.remove_item();
    }
    u.recoil = MAX_RECOIL;
    u.mod_moves( -moves );
    after_inventory_change( u );
    return any;
}

// CDDA's unload (Character::unload) with no menu: which part comes out is chosen as below, never asked; what comes
// out goes to `a.hand`. Into the inventory with nothing to choose, it is Character::unload itself.
// The wielded gun itself only: not something carried, lying around, or inside the gun (its magazine, a gunmod).
bool clear( avatar &u, const item_action &a )
{
    const item_location gun = find_carried( u, a.item );
    if( !gun ) {
        return refuse( u, _( "You don't have that any more." ) );
    }
    const item_location wielded = u.get_wielded_item();
    if( !wielded || wielded.get_item() != gun.get_item() ) {
        return refuse( u, _( "Hold the gun to clear it." ) );
    }
    const std::string why = clear_jam( u, gun );
    if( !why.empty() ) {
        return refuse( u, why );
    }
    return true;
}

bool unload( avatar &u, const item_action &a )
{
    std::string why;
    item_location loc = find_reachable( u, a.pos, a.item, why );
    if( !loc ) {
        return refuse( u, why );
    }
    item &it = *loc;
    // CDDA's unload also empties containers: that's take and put here.
    if( !it.is_gun() && !it.is_magazine() && !it.uses_magazine() ) {
        return refuse( u, string_format( _( "You can't unload a %s!" ), it.tname() ) );
    }
    // CDDA asks which part to unload when a gunmod is loaded too: here the item itself first, then its mods in order.
    const std::vector<item *> opts = unload_options( it );
    item *target = opts.size() > 1 && !has_load( it ) ? opts[1] : &it;
    item_location targloc = target == &it ? loc : item_location( loc, target );
    // CDDA's own checks and messages.
    if( !target->has_flag( flag_BRASS_CATCHER ) ) {
        if( !target->is_magazine() && !target->uses_magazine() ) {
            return refuse( u, string_format( _( "You can't unload a %s!" ), target->tname() ) );
        }
        if( target->has_flag( flag_NO_UNLOAD ) ) {
            if( target->has_flag( flag_RECHARGE ) || target->has_flag( flag_USE_UPS ) ) {
                return refuse( u, string_format( _( "You can't unload a rechargeable %s!" ), target->tname() ) );
            }
            return refuse( u, string_format( _( "You can't unload a %s!" ), target->tname() ) );
        }
        if( !has_load( *target ) ) {
            return refuse( u, string_format( target->is_tool() ? _( "Your %s isn't charged." ) :
                                             _( "Your %s isn't loaded." ), target->tname() ) );
        }
    }
    // Liquids out of a magazine go through CDDA's liquid menu (where to pour them): not by hand.
    bool liquid = false;
    if( target->is_magazine() ) {
        for( const item *c : target->all_items_top() ) {
            liquid = liquid || c->made_of( phase_id::LIQUID );
        }
    } else if( target->magazine_current() == nullptr && target->ammo_remaining() > 0 ) {
        liquid = item( target->ammo_current(), calendar::turn, 1 ).made_of_from_type( phase_id::LIQUID );
    }
    if( liquid ) {
        return refuse( u, string_format( _( "You can't pour out your %s by hand." ), target->tname() ) );
    }
    // What lands in the hand, checked before anything changes. With nothing for a hand (casings, a brass catcher's
    // contents), all of it goes into the inventory as CDDA puts it.
    std::optional<item> out;
    if( target->is_magazine() ) {
        for( const item *c : target->all_items_top() ) {
            if( !c->has_flag( flag_CASING ) ) {
                out = *c;
                break;
            }
        }
    } else if( const item *mag = target->magazine_current() ) {
        out = *mag;
    } else if( target->ammo_remaining() > 0 ) {
        out = item( target->ammo_current(), calendar::turn, target->ammo_remaining() );
    }
    const item_hand hand = out ? a.hand : item_hand::pack;
    if( hand != item_hand::pack ) {
        why = hand_refusal( u, *out, hand, loc );
        if( !why.empty() ) {
            return refuse( u, why );
        }
    }
    clear_loaded_marks( *target );
    if( hand == item_hand::pack && ( opts.size() == 1 || target != &it ) && !target->is_container() ) {
        // Nothing to ask: CDDA's own unload.
        const bool done = u.unload( targloc );
        after_inventory_change( u );
        return done;
    }
    // The rest of Character::unload, for `target` and `hand`.
    target->casings_handle( [&u]( item & e ) {
        return u.i_add_or_drop( e );
    } );
    u.invalidate_weight_carried_cache();
    if( target->is_magazine() ) {
        int moves = 0;
        for( const item *content : target->all_items_top() ) {
            moves += u.item_reload_cost( it, *content, content->charges );
        }
        if( it.is_ammo_belt() ) {
            moves /= 2;
        }
        if( hand == item_hand::pack ) {
            u.assign_activity( unload_activity_actor( moves, targloc ) );
            return true;
        }
        return unload_magazine_now( u, targloc, moves, hand );
    }
    if( item *mag = target->magazine_current() ) {
        // Out whole, for half the time it takes to put in.
        const int cost = u.item_reload_cost( *target, *mag, -1 ) / 2;
        if( !place( u, *mag, hand, nullptr, mag ) ) {
            return false;
        }
        u.mod_moves( -cost );
        target->remove_items_with( [target]( const item & e ) {
            return target->magazine_current() == &e;
        } );
    } else if( target->ammo_remaining() > 0 ) {
        const int qty = target->ammo_remaining();
        item ammo( target->ammo_current(), calendar::turn, qty );
        if( target->is_filthy() ) {
            ammo.set_flag( flag_FILTHY );
        }
        if( !place( u, ammo, hand, nullptr, nullptr ) ) {
            return false;
        }
        u.mod_moves( -u.item_reload_cost( *target, ammo, qty ) / 2 );
        target->ammo_set( target->ammo_current(), target->ammo_remaining() - qty );
    } else if( target->has_flag( flag_BRASS_CATCHER ) ) {
        target->spill_contents( u );
    }
    if( target->is_tool() && target->active && target->ammo_remaining() == 0 ) {
        target->deactivate( &u );
    }
    u.add_msg_if_player( _( "You unload your %s." ), target->tname() );
    if( it.has_flag( flag_MAG_DESTROY ) && it.ammo_remaining() == 0 ) {
        loc.remove_item();
    }
    u.recoil = MAX_RECOIL;
    after_inventory_change( u );
    return true;
}

// What CDDA's reload menu offers to load for `base` (Character::list_ammo): the item, the magazine in it, its
// gunmods and their magazines.
std::vector<item_location> reload_targets( item_location base )
{
    std::vector<item_location> opts;
    opts.push_back( base );
    if( item *mag = base->magazine_current() ) {
        opts.emplace_back( base, mag );
    }
    for( item *mod : base->gunmods() ) {
        item_location mod_loc( base, mod );
        opts.push_back( mod_loc );
        if( item *mag = mod->magazine_current() ) {
            opts.emplace_back( mod_loc, mag );
        }
    }
    return opts;
}

// reload_activity_actor::finish asks what to do (wield or drop it) when the loaded item no longer fits the pocket
// it's in. Here the reload is refused before it starts instead: why, or "" when it fits. `dest` is loaded on a copy
// as item::reload loads it, and the growth checked against the room left in each pocket around it.
std::string outgrows_pocket( item_location dest, const item &ammo, int qty )
{
    if( !dest.has_parent() ) {
        return {};
    }
    item loaded = *dest;
    if( loaded.is_magazine() ) {
        item rounds = ammo.has_flag( flag_SPEEDLOADER ) || ammo.has_flag( flag_SPEEDLOADER_CLIP ) ?
                      ammo.first_ammo() : ammo;
        rounds.charges = qty;
        if( !loaded.put_in( rounds, pocket_type::MAGAZINE ).success() ) {
            return {};
        }
    } else {
        if( item *old = loaded.magazine_current() ) {
            loaded.remove_item( *old );
        }
        if( !loaded.put_in( ammo, pocket_type::MAGAZINE_WELL ).success() ) {
            return {};
        }
    }
    units::volume grow = loaded.volume() - dest->volume();
    units::mass heavier = loaded.weight() - dest->weight();
    for( item_location at = dest; at.has_parent(); at = at.parent_item() ) {
        const item_pocket *pk = at.parent_pocket();
        if( pk == nullptr ) {
            break;
        }
        if( grow > pk->remaining_volume() || heavier > pk->remaining_weight() ) {
            //~ %1$s: the gun or magazine being loaded, %2$s: the container it's in
            return string_format( _( "Loaded, the %1$s wouldn't fit in the %2$s any more.  Take it out first." ),
                                  dest->tname(), at.parent_item()->type_name() );
        }
        const pocket_data *data = pk->get_pocket_data();
        if( pk->rigid() ) {
            grow = 0_ml;
        } else {
            grow = grow * data->volume_multiplier;
        }
        heavier = heavier * data->weight_multiplier;
        if( grow <= 0_ml && heavier <= 0_gram ) {
            break;
        }
    }
    return {};
}

// CDDA's reload with the ammo chosen (game::reload after its menu): an item::reload_option for the first of what
// CDDA's menu offers that takes this ammo now, as much as fits, run by reload_activity_actor (CDDA's moves).
bool reload( avatar &u, const item_action &a )
{
    // CDDA's reload picks up what it loads first: here the client takes it first.
    item_location target = find_carried( u, a.item );
    if( !target ) {
        return refuse( u, _( "You don't have that any more." ) );
    }
    std::string why = pocket_block( target );
    if( !why.empty() ) {
        return refuse( u, why );
    }
    item &it = *target;
    if( !it.is_gun() && !it.is_magazine() && !it.uses_magazine() ) {
        return refuse( u, string_format( _( "You can't reload a %s!" ), it.tname() ) );
    }
    // Bows and such: CDDA's reload only picks their default ammo.
    if( it.has_flag( flag_RELOAD_AND_SHOOT ) ) {
        return refuse( u, string_format( _( "The %s is loaded as you shoot it." ), it.tname() ) );
    }
    item_location ammo = find_reachable( u, a.pos, a.container, why );
    if( !ammo ) {
        return refuse( u, why );
    }
    if( ammo.get_item() == target.get_item() ) {
        return refuse( u, string_format( _( "You can't load the %s with itself." ), it.tname() ) );
    }
    if( target.eventually_contains( ammo ) || ammo.eventually_contains( target ) ) {
        //~ %1$s: gun or magazine, %2$s: ammo
        return refuse( u, string_format( _( "You can't reload the %1$s with the %2$s." ), it.tname(),
                                         ammo->tname() ) );
    }
    // As CDDA's ammo search (Character::find_ammo): no frozen or spilt liquid, nothing out of a magazine or a gun.
    if( ammo->is_frozen_liquid() ) {
        return refuse( u, string_format( _( "The %s is frozen." ), ammo->tname() ) );
    }
    if( ammo.where() == item_location::type::map && ammo->made_of( phase_id::LIQUID ) ) {
        return refuse( u, string_format( _( "You can't load spilt %s." ), ammo->tname() ) );
    }
    if( ammo.has_parent() ) {
        item_location holder = ammo.parent_item();
        if( holder->is_magazine() || holder->magazine_current() == ammo.get_item() ) {
            //~ %1$s: ammo, %2$s: the magazine or gun it's in
            return refuse( u, string_format( _( "Take the %1$s out of the %2$s first." ), ammo->tname(),
                                             holder->tname() ) );
        }
    }
    std::vector<item_location> opts = reload_targets( target );
    item_location *pick = nullptr;
    bool compatible = false;
    for( item_location &p : opts ) {
        if( !p.can_reload_with( ammo, false ) ) {
            continue;
        }
        compatible = true;
        if( p.can_reload_with( ammo, true ) && u.can_reload( *p, ammo.get_item() ) ) {
            pick = &p;
            break;
        }
    }
    if( pick == nullptr ) {
        if( !compatible ) {
            //~ %1$s: gun or magazine, %2$s: ammo
            return refuse( u, string_format( _( "You can't reload the %1$s with the %2$s." ), it.tname(),
                                             ammo->tname() ) );
        }
        if( it.is_ammo_belt() && it.type->magazine->linkage &&
            !u.has_charges( *it.type->magazine->linkage, 1 ) ) {
            return refuse( u, string_format( _( "You need at least one %s to reload the %s!" ),
                                             item::nname( *it.type->magazine->linkage, 1 ), it.tname() ) );
        }
        const item *mag = it.magazine_current();
        if( ( it.ammo_remaining() > 0 && it.remaining_ammo_capacity() == 0 ) ||
            ( mag != nullptr && mag->is_magazine_full() ) ) {
            return refuse( u, string_format( _( "The %s is already fully loaded!" ), it.tname() ) );
        }
        //~ %1$s: gun or magazine, %2$s: ammo
        return refuse( u, string_format( _( "You can't load the %2$s into the %1$s now." ), it.tname(),
                                         ammo->tname() ) );
    }
    item_location dest = *pick;
    item::reload_option opt( &u, dest, ammo );
    if( !opt ) {
        return refuse( u, string_format( _( "You can't reload the %s." ), dest->tname() ) );
    }
    why = outgrows_pocket( dest, *ammo, opt.qty() );
    if( !why.empty() ) {
        return refuse( u, why );
    }
    // As game::reload: a fouled gun takes longer.
    const int extra_moves = dest->get_var( "dirt", 0 ) > 7800 ? 2500 : 0;
    if( extra_moves > 0 ) {
        u.add_msg_if_player( m_warning, _( "You struggle to reload the fouled %s." ), dest->tname() );
    }
    u.assign_activity( reload_activity_actor( std::move( opt ), extra_moves ) );
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
    ctxt.register_action( item_action_name, no_translation( "take, put, drop, unload or reload one item" ) );
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
        case item_op::unload:
            return unload( u, *a );
        case item_op::reload:
            return reload( u, *a );
        case item_op::clear:
            return clear( u, *a );
    }
    return false;
}

} // namespace fp3d
