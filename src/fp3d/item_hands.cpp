#include "fp3d/item_hands.h"

#include <charconv>
#include <cmath>
#include <mutex>
#include <optional>
#include <random>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "activity_actor_definitions.h"
#include "avatar.h"
#include "calendar.h"
#include "contents_change_handler.h"
#include "coordinates.h"
#include "creature_tracker.h"
#include "enums.h"
#include "flag.h"
#include "fp3d/bridge.h"
#include "fp3d/events.h"
#include "fp3d/gun_faults.h"
#include "fp3d/held.h"
#include "fp3d/snapshot.h"
#include "game_constants.h"
#include "input_context.h"
#include "item.h"
#include "item_location.h"
#include "item_pocket.h"
#include "itype.h"
#include "line.h"
#include "map.h"
#include "map_selector.h"
#include "mapdata.h"
#include "math_parser_diag_value.h"
#include "monster.h"
#include "mtype.h"
#include "npc.h"
#include "output.h"
#include "pickup.h"
#include "pocket_type.h"
#include "projectile.h"
#include "ret_val.h"
#include "type_id.h"
#include "string_formatter.h"
#include "translation.h"
#include "translations.h"
#include "units.h"

static const ammotype ammo_plutonium( "plutonium" );

static const efftype_id effect_incorporeal( "incorporeal" );
static const json_character_flag json_flag_CANNOT_ATTACK( "CANNOT_ATTACK" );
static const trait_id trait_SHELL2( "SHELL2" );
static const trait_id trait_SHELL3( "SHELL3" );

namespace fp3d
{
namespace
{
const std::string uid_var = "fp3d_uid";
const std::string spot_var = "fp3d_spot";
const std::string pose_var = "fp3d_pose";

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

// How the action being run went, for its `item_result` event (run_item_action): refused with a reason, or done
// without taking time (the free actions).
bool action_refused = false;
bool action_done_free = false;
std::string action_refusal;

bool refuse( avatar &u, const std::string &why )
{
    action_refused = true;
    action_refusal = why;
    u.add_msg_if_player( m_info, "%s", why );
    return false;
}

// The free actions come from the client's physics, many a second: a refusal goes to the result event only, not
// into the message log.
bool refuse_quietly( const std::string &why )
{
    action_refused = true;
    action_refusal = why;
    return false;
}

// The item is where it came from no more: it lies as CDDA puts it.
void forget_spot( item &it )
{
    it.erase_var( spot_var );
    it.erase_var( pose_var );
}

// Where the client put the item on its square and how it lies there; neither when the action gives no spot.
void store_spot( item &it, const item_action &a )
{
    forget_spot( it );
    if( a.has_spot ) {
        it.set_var( spot_var, std::to_string( a.spot_x ) + "," + std::to_string( a.spot_y ) );
        if( a.has_pose ) {
            it.set_var( pose_var, std::to_string( a.yaw ) + "," + ( a.side ? "1" : "0" ) );
        }
    }
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
        forget_spot( it );
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
    forget_spot( copy );
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
    // Already where it's going: nothing to move. (A copy would merge into the original.) But the off-hand item
    // lies in a pocket while it's held, so putting it there just takes it out of the hand.
    if( src.has_parent() && ( pocket != nullptr ? pocket == src.parent_pocket() :
                              src.parent_item().get_item() == dst.get_item() ) ) {
        const item_location off = offhand_item( u );
        if( !off || off.get_item() != src.get_item() ) {
            return failed( _( "it's already in there" ) );
        }
        const std::string name = it.display_name();
        const std::string dst_name = dst->type->nname( 1 );
        clear_offhand_mark( it );
        u.mod_moves( -Pickup::cost_to_move_item( u, it ) );
        after_inventory_change( u );
        //~ %1$s: item to put in the container, %2$s: container to put item in
        u.add_msg_if_player( _( "You put your %1$s into the %2$s." ), name, dst_name );
        return true;
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
    forget_spot( copy );
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
    store_spot( copy, a );
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
    forget_spot( copy );
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

// CDDA's unload (Character::unload) with no menu: which part comes out is chosen as below, never asked; what comes
// out goes to `a.hand`. Into the inventory with nothing to choose, it is Character::unload itself.
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

// CDDA's throw (avatar_action::plthrow) at a given square, with no target menu. CDDA's checks: what can be wielded,
// throw_range (too heavy at 0), a wielded part of the body, a mech's batteries, relax gas. Worn items aren't thrown
// (take them off first); nor is what sits in a sealed pocket or inside a gun or magazine. Blind throws (CDDA's throw
// from a peeking position) aren't offered: the target is a square the avatar sees, on its own level, in the bubble,
// not its own square, within throw_range by the target menu's distance. A non-hostile NPC there is never thrown at
// (CDDA asks first).
// The item flies from the hand holding it. Anything else carried is thrown from the main hand when it is empty, else
// from a free off hand, for the moves CDDA's wield charges to get it out (item_handling_cost); it is never wielded,
// so what the main hand holds stays there (CDDA puts it away first, asking where when it doesn't fit). One unit of a
// stack counted by charges flies, as CDDA throws one; the rest stays where it was, and the unit thrown gets a handle
// of its own. Character::throw_item does the rest: its moves, stamina, dispersion and practice.
bool throw_at( avatar &u, const item_action &a )
{
    map &here = get_map();
    item_location src = find_carried( u, a.item );
    if( !src ) {
        return refuse( u, _( "You don't have that any more." ) );
    }
    if( u.has_flag( json_flag_CANNOT_ATTACK ) ) {
        return refuse( u, _( "You are incapable of throwing anything!" ) );
    }
    bool in_mech = false;
    if( u.is_mounted() ) {
        const monster *mount = u.mounted_creature.get();
        if( mount != nullptr && mount->has_flag( mon_flag_RIDEABLE_MECH ) ) {
            if( !mount->check_mech_powered() ) {
                return refuse( u, string_format( _( "Your %s refuses to move as its batteries have been drained." ),
                                                 mount->get_name() ) );
            }
            in_mech = true;
        }
    }
    item &it = *src;
    const std::string name = it.tname();
    if( u.is_worn( it ) ) {
        return refuse( u, string_format( _( "Take off your %s first." ), name ) );
    }
    std::string why = pocket_block( src );
    if( !why.empty() ) {
        return refuse( u, why );
    }
    if( src.has_parent() && !src.parent_pocket()->is_type( pocket_type::CONTAINER ) ) {
        //~ %1$s: an item, %2$s: the gun, magazine or tool it is part of
        return refuse( u, string_format( _( "Take the %1$s out of the %2$s first." ), name,
                                         src.parent_item()->tname() ) );
    }
    // CDDA asks before using someone else's things; with no menu, they aren't thrown.
    if( !it.is_owned_by( u, true ) ) {
        return refuse( u, string_format( _( "The %s isn't yours." ), name ) );
    }
    if( !in_mech ) {
        const ret_val<void> can = u.can_wield( it );
        if( !can.success() ) {
            return refuse( u, can.str() );
        }
    }
    const int range = u.throw_range( it );
    if( range < 0 ) {
        return refuse( u, _( "You don't have that item." ) );
    }
    if( range == 0 ) {
        return refuse( u, _( "That is too heavy to throw." ) );
    }
    const item_location weapon = u.get_wielded_item();
    const bool wielded = weapon && weapon.get_item() == src.get_item();
    if( wielded && it.has_flag( flag_NO_UNWIELD ) ) {
        return refuse( u, _( "That's part of your body, you can't throw that!" ) );
    }
    const item_location off = offhand_item( u );
    const bool from_pack = !in_mech && !wielded && !( off && off.get_item() == src.get_item() );
    if( from_pack && weapon ) {
        why = offhand_unavailable( u );
        if( why.empty() && off ) {
            //~ %1$s: wielded item, %2$s: item held in the off hand
            why = string_format( _( "Your hands are full: you're holding your %1$s and your %2$s." ), weapon->tname(),
                                 off->tname() );
        }
        if( why.empty() && it.is_two_handed( u ) ) {
            why = string_format( _( "Your %s is too heavy to hold in one hand." ), name );
        }
        if( !why.empty() ) {
            return refuse( u, why );
        }
    }
    const std::optional<tripoint_bub_ms> target = bubble_square( here, a.pos );
    if( !target ) {
        return refuse( u, _( "That's too far away." ) );
    }
    const tripoint_bub_ms from = u.pos_bub();
    if( target->z() != from.z() ) {
        return refuse( u, _( "You can only throw at something on your own level." ) );
    }
    if( *target == from ) {
        return refuse( u, _( "Pick a square to throw at." ) );
    }
    // target_ui::dist_fn, as CDDA's throw menu measures its range.
    const int dist = static_cast<int>( std::round( rl_dist_exact( from, *target ) ) );
    if( dist > range ) {
        return refuse( u, string_format( _( "You can't throw your %s that far." ), name ) );
    }
    if( !u.sees( here, *target ) ) {
        return refuse( u, _( "You can't see where you're throwing." ) );
    }
    if( const npc *who = get_creature_tracker().creature_at<npc>( *target ); who != nullptr &&
        !who->guaranteed_hostile() && u.sees( here, *who ) ) {
        return refuse( u, string_format( _( "You don't want to hit %s." ), who->get_name() ) );
    }
    // CDDA's roll: a failure costs what CDDA says, with its message.
    if( !u.try_break_relax_gas( _( "You concentrate mightily, and your body obeys!" ),
                                _( "You can't muster up the effort to throw anything…" ) ) ) {
        return false;
    }
    const int handling = from_pack ? u.item_handling_cost( it, true, INVENTORY_HANDLING_PENALTY ) : 0;
    item thrown = it;
    clear_offhand_mark( thrown );
    forget_spot( thrown );
    if( it.count_by_charges() && it.charges > 1 ) {
        thrown.charges = 1;
        thrown.erase_var( uid_var );
        it.mod_charges( -1 );
        src.on_contents_changed();
    } else if( wielded ) {
        u.remove_weapon();
    } else {
        contents_change_handler handler;
        handler.unseal_pocket_containing( src );
        src.remove_item();
        handler.handle_by( u );
    }
    u.mod_moves( -handling );
    u.throw_item( *target, thrown );
    after_inventory_change( u );
    return true;
}

// The container `r` names for a pour: carried (not in a sealed or forbidden pocket), else lying on the avatar's
// square or one next to it where `take` could get at it (not ridden past, in sight, not someone else's). A pour
// gives no square for it: all nine are searched, and a handle found twice names nothing.
item_location find_near( avatar &u, const item_ref &r, std::string &why )
{
    item_location loc = find_carried( u, r );
    if( loc ) {
        why = pocket_block( loc );
        return why.empty() ? loc : item_location();
    }
    if( u.is_mounted() ) {
        why = _( "You can't reach that while you're riding." );
        return item_location();
    }
    map &here = get_map();
    const tripoint_bub_ms at = u.pos_bub();
    item_location found;
    for( const tripoint_bub_ms &p : here.points_in_radius( at, 1 ) ) {
        if( !here.inbounds( p ) || !here.accessible_items( p ) || !here.could_see_items( p, at ) ) {
            continue;
        }
        for( item &it : here.i_at( p ) ) {
            if( !matches( it, r ) ) {
                continue;
            }
            if( found ) {
                why = _( "That isn't within reach." );
                return item_location();
            }
            found = item_location( map_cursor( p ), &it );
        }
    }
    if( !found ) {
        why = _( "That isn't within reach." );
        return item_location();
    }
    if( !found->is_owned_by( u, true ) ) {
        why = string_format( _( "The %s isn't yours." ), found->tname() );
        return item_location();
    }
    return found;
}

// Character::pour_into for one given pocket of `dst` (CDDA's fill picks the best pocket itself): the pocket's rules
// (watertight, ammo and flag restrictions, room, what a carried bucket holds) and the room its parents leave.
// The charges of `liquid` moved; 0 with `why` set when none can go. What goes in gets a handle of its own.
int pour_into_pocket( avatar &u, item_location dst, item_pocket &pk, const item &liquid, std::string &why )
{
    item unit = liquid;
    unit.charges = 1;
    const ret_val<item_pocket::contain_code> can = pk.can_contain( unit );
    if( !can.success() ) {
        //~ %1$s: liquid, %2$s: container, %3$s: reason it failed
        why = string_format( _( "Could not pour %1$s into %2$s, %3$s." ), liquid.tname(), dst->tname(), can.str() );
        return 0;
    }
    const item_location weapon = u.get_wielded_item();
    const bool held_or_ground = ( weapon && weapon.get_item() == dst.get_item() ) || !u.has_item( *dst );
    if( !pk.can_contain_liquid( held_or_ground ) ) {
        why = string_format( _( "That %s must be on the ground or held to hold contents!" ), dst->tname() );
        return 0;
    }
    int amount = std::min( liquid.charges, pk.remaining_capacity_for_item( liquid ) );
    if( !dst->all_pockets_rigid() ) {
        amount = std::min( amount, dst.max_charges_by_parent_recursive( liquid ).value() );
    }
    if( amount <= 0 ) {
        why = string_format( _( "Your %1$s can't hold any more %2$s." ), dst->tname(), liquid.tname() );
        return 0;
    }
    item part = liquid;
    part.charges = amount;
    part.erase_var( uid_var );
    const ret_val<item *> in = pk.insert_item( part );
    if( !in.success() ) {
        why = string_format( _( "Could not pour %1$s into %2$s, %3$s." ), liquid.tname(), dst->tname(), in.str() );
        return 0;
    }
    dst.on_contents_changed();
    return amount;
}

// Character::pour_into (CDDA's best pocket): the charges of `liquid` moved. 0 when none can go, with `why` set,
// or empty when CDDA said why itself. What goes in gets a handle of its own (the source keeps its).
int pour_into_best( avatar &u, item_location dst, const item &liquid, std::string &why )
{
    item rest = liquid;
    rest.erase_var( uid_var );
    if( !u.pour_into( dst, rest, /*ignore_settings=*/true, /*silent=*/true ) ) {
        return 0;
    }
    const int moved = liquid.charges - rest.charges;
    if( moved > 0 ) {
        return moved;
    }
    // CDDA's fill passes over sealed pockets without a word.
    bool sealed = false;
    for( const item_pocket *pk : dst->get_all_contained_pockets() ) {
        sealed = sealed || pk->sealed();
    }
    why = sealed ? string_format( _( "The %s is sealed.  Open it first." ), dst->tname() ) :
          string_format( _( "Your %1$s can't hold any more %2$s." ), dst->tname(), liquid.tname() );
    return 0;
}

// The liquid out of the carried container `a.item` (or, when `a.item` is a liquid, that liquid out of its container),
// with CDDA's liquid rules and no menu, onto the square `a.pos` or into the container `a.container` (one or the other,
// bridge.h). Only liquids pour: frozen ones and solids stay, and a container holding none is refused, as is one sealed
// or in a sealed pocket, or a liquid in a gun, magazine or tool.
// Onto the ground (CDDA's "pour on the ground"): the avatar's square or one next to it that takes items; all of it,
// map::add_item_or_charges spilling onto the next squares when that one is full; into deep water it's gone.
// Into a container (carried or in reach, never the source or something inside it): CDDA's Character::pour_into (its
// best pocket) or the given pocket; as much as fits moves and the rest stays in the source container ("There's some
// left over!", where CDDA's liquid menu would ask again).
// 100 moves for each liquid moved, as CDDA's liquid handling.
bool pour( avatar &u, const item_action &a )
{
    map &here = get_map();
    item_location src = find_carried( u, a.item );
    if( !src ) {
        return refuse( u, _( "You don't have that any more." ) );
    }
    item_location cont = src;
    std::vector<item_location> liquids;
    if( src->made_of( phase_id::LIQUID ) ) {
        if( !src.has_parent() || !src.parent_pocket()->is_type( pocket_type::CONTAINER ) ) {
            return refuse( u, string_format( _( "You can't pour out your %s by hand." ), src->tname() ) );
        }
        cont = src.parent_item();
        liquids.push_back( src );
    } else {
        bool frozen = false;
        bool solid = false;
        for( item *c : src->all_items_top( pocket_type::CONTAINER ) ) {
            if( c->made_of( phase_id::LIQUID ) ) {
                liquids.emplace_back( src, c );
            } else if( c->is_frozen_liquid() ) {
                frozen = true;
            } else {
                solid = true;
            }
        }
        if( liquids.empty() ) {
            const std::string nm = src->type->nname( 1 );
            return refuse( u, string_format( frozen ? _( "What's in your %s is frozen solid." ) : solid ?
                                             _( "There's no liquid in your %s to pour." ) : _( "Your %s is empty." ), nm ) );
        }
    }
    for( const item_location &l : liquids ) {
        const std::string why = pocket_block( l );
        if( !why.empty() ) {
            return refuse( u, why );
        }
    }
    const std::string cont_name = cont->type->nname( 1 );
    const std::string liquid_name = liquids.front()->tname();
    int moved_stacks = 0;
    bool left_over = false;
    if( a.container.uid != 0 ) {
        std::string why;
        item_location dst = find_near( u, a.container, why );
        if( !dst ) {
            return refuse( u, why );
        }
        if( dst.get_item() == cont.get_item() || cont.eventually_contains( dst ) ) {
            return refuse( u, string_format( _( "You can't pour your %s into itself." ), cont_name ) );
        }
        item_pocket *pocket = nullptr;
        if( a.pocket >= 0 ) {
            std::vector<item_pocket *> pockets = dst->get_all_contained_pockets();
            if( static_cast<size_t>( a.pocket ) >= pockets.size() || pockets[a.pocket]->is_forbidden() ) {
                return refuse( u, string_format( _( "The %s has no such pocket." ), dst->tname() ) );
            }
            pocket = pockets[a.pocket];
            if( pocket->sealed() ) {
                return refuse( u, string_format( _( "The %s is sealed.  Open it first." ), dst->tname() ) );
            }
        }
        const std::string dst_name = dst->type->nname( 1 );
        for( item_location &l : liquids ) {
            const int moved = pocket != nullptr ? pour_into_pocket( u, dst, *pocket, *l, why ) :
                              pour_into_best( u, dst, *l, why );
            if( moved <= 0 ) {
                if( moved_stacks == 0 ) {
                    if( !why.empty() ) {
                        refuse( u, why );
                    }
                    return false;
                }
                left_over = true;
                break;
            }
            moved_stacks++;
            l->charges -= moved;
            if( l->charges <= 0 ) {
                l.remove_item();
            } else {
                l.on_contents_changed();
                left_over = true;
            }
        }
        dst.make_active();
        u.add_msg_if_player( _( "You pour %1$s into the %2$s." ), liquid_name, dst_name );
    } else {
        const std::optional<tripoint_bub_ms> p = bubble_square( here, a.pos );
        if( !p || !within_reach( u, *p ) ) {
            return refuse( u, _( "You can't reach that from here." ) );
        }
        if( !here.can_put_items_ter_furn( *p ) ) {
            return refuse( u, _( "You can't pour there!" ) );
        }
        const bool into_water = here.has_flag( ter_furn_flag::TFLAG_SWIMMABLE, *p );
        for( item_location &l : liquids ) {
            item spilt = *l;
            forget_spot( spilt );
            // map::add_item_or_charges never keeps a liquid on a square of liquid: it is gone, as CDDA pours it.
            if( !into_water && here.add_item_or_charges( *p, spilt ).is_null() ) {
                if( moved_stacks == 0 ) {
                    return refuse( u, string_format( _( "There's no room to pour your %s there." ), cont_name ) );
                }
                left_over = true;
                break;
            }
            moved_stacks++;
            l.remove_item();
        }
        //~ %1$s: liquid, %2$s: its container
        u.add_msg_if_player( _( "You pour the %1$s out of your %2$s." ), liquid_name, cont_name );
    }
    if( left_over ) {
        u.add_msg_if_player( _( "There's some left over!" ) );
    }
    u.mod_moves( -100 * moved_stacks );
    after_inventory_change( u );
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

bool item_pose( const item &it, uint16_t &yaw, bool &side )
{
    uint16_t x = 0;
    uint16_t y = 0;
    if( !it.has_var( pose_var ) || !item_spot( it, x, y ) ) {
        return false;
    }
    const std::string s = text_var( it, pose_var );
    const size_t comma = s.find( ',' );
    if( s.size() > 8 || comma == std::string::npos ) {
        return false;
    }
    const std::optional<uint64_t> deg = parse_decimal( s.substr( 0, comma ), max_item_yaw );
    const std::optional<uint64_t> lies = parse_decimal( s.substr( comma + 1 ), 1 );
    if( !deg || !lies ) {
        return false;
    }
    yaw = static_cast<uint16_t>( *deg );
    side = *lies == 1;
    return true;
}

namespace
{
// The free actions (place, spot) take no time, so the turn bounds them: max_free_item_ops in one turn, and an item
// is moved once. Sim thread only.
struct free_ops {
    time_point turn = calendar::before_time_starts;
    int used = 0;
    std::unordered_set<uint64_t> moved;
};
free_ops free_ops_now;

// "" when one more free action may run this turn (and it is counted), else why not.
std::string free_op_refusal( uint64_t uid, bool moves )
{
    free_ops &f = free_ops_now;
    if( f.turn != calendar::turn ) {
        f.turn = calendar::turn;
        f.used = 0;
        f.moved.clear();
    }
    if( f.used >= max_free_item_ops ) {
        return "Too many at once.";
    }
    if( moves && f.moved.count( uid ) != 0 ) {
        return "That was moved a moment ago.";
    }
    f.used++;
    if( moves ) {
        f.moved.insert( uid );
    }
    return "";
}

// The lying item a free action names, when the client's physics can have touched it: on a square near the avatar
// (physics_reach, same level) that it sees and whose items can be got at, not someone else's, not spilt liquid or
// gas. `why` says what is wrong otherwise.
item_location find_for_physics( avatar &u, map &here, const item_action &a, tripoint_bub_ms &at, std::string &why )
{
    const std::optional<tripoint_bub_ms> p = bubble_square( here, a.pos );
    if( !p || p->z() != u.posz() || square_dist( u.pos_bub(), *p ) > physics_reach ||
        ( *p != u.pos_bub() && !u.sees( here, *p ) ) ) {
        why = "That is out of reach.";
        return item_location();
    }
    if( !here.accessible_items( *p ) || !here.could_see_items( *p, u.pos_bub() ) ) {
        why = "The items there can't be got at.";
        return item_location();
    }
    item_location loc = find_on_square( here, *p, a.item );
    if( !loc ) {
        why = "That isn't there any more.";
        return item_location();
    }
    if( loc->made_of_from_type( phase_id::LIQUID ) || loc->made_of_from_type( phase_id::GAS ) ||
        loc->is_frozen_liquid() ) {
        why = "That can't be moved by hand.";
        return item_location();
    }
    if( !loc->is_owned_by( u, true ) ) {
        why = "That isn't yours.";
        return item_location();
    }
    at = *p;
    return loc;
}

// place: the client's physics moved a lying item onto the next square. No time, no message: CDDA's rules for what
// the square takes, and the item is one the avatar could have lifted there itself.
bool move_lying( avatar &u, const item_action &a )
{
    map &here = get_map();
    tripoint_bub_ms from;
    std::string why;
    item_location src = find_for_physics( u, here, a, from, why );
    if( !src ) {
        return refuse_quietly( why );
    }
    const std::optional<tripoint_bub_ms> to = bubble_square( here, a.to );
    if( !to || to->z() != from.z() || square_dist( from, *to ) != 1 ) {
        return refuse_quietly( "It can only move to the next square." );
    }
    // Items that act when dropped (CDDA's drop_action) aren't shoved around either.
    if( src->type->drop_action || !u.can_lift( *src ) ) {
        return refuse_quietly( "That can't be moved like that." );
    }
    // Not through what blocks the way (a closed door or gate, a vehicle's wall), and not over an edge: an item that
    // falls is CDDA's to drop, with its time and its harm, never a free action's.
    if( here.impassable( *to ) || !here.has_floor_or_water( *to ) ) {
        return refuse_quietly( "It can't go there." );
    }
    if( !here.can_put_items_ter_furn( *to ) || here.has_flag( ter_furn_flag::TFLAG_DESTROY_ITEM, *to ) ||
        here.free_volume( *to ) < src->volume() ) {
        return refuse_quietly( "There's no room for it there." );
    }
    why = free_op_refusal( a.item.uid, true );
    if( !why.empty() ) {
        return refuse_quietly( why );
    }
    item copy = *src;
    store_spot( copy, a );
    // On that square or not at all. A stack counted by charges can merge with the same thing there and take
    // that stack's handle, as when it is put into a pocket.
    item &placed = here.add_item_or_charges( *to, copy, /*overflow=*/false );
    if( placed.is_null() ) {
        return refuse_quietly( "There's no room for it there." );
    }
    // A merged stack keeps the other stack's handle (and its spot): that one has moved this turn too.
    free_ops_now.moved.insert( read_item_handle( placed ) );
    src.remove_item();
    action_done_free = true;
    return false;
}

// spot: how a lying item rests on its square changes. No time, no message.
bool set_rest( avatar &u, const item_action &a )
{
    map &here = get_map();
    tripoint_bub_ms at;
    std::string why;
    item_location src = find_for_physics( u, here, a, at, why );
    if( !src ) {
        return refuse_quietly( why );
    }
    why = free_op_refusal( a.item.uid, false );
    if( !why.empty() ) {
        return refuse_quietly( why );
    }
    store_spot( *src, a );
    action_done_free = true;
    return false;
}

// hold: a carried item into the main hand (CDDA's wield and its cost) or the off hand (held.h). Nothing is ever put
// away to make room: a hand that holds something else refuses.
bool hold_in_hand( avatar &u, const item_action &a )
{
    item_location src = find_carried( u, a.item );
    if( !src ) {
        return refuse( u, _( "You don't have that any more." ) );
    }
    const std::string name = src->tname();
    if( u.is_worn( *src ) ) {
        return refuse( u, string_format( _( "Take off your %s first." ), name ) );
    }
    // Only what can be taken out by hand: not from a sealed or forbidden pocket, not a part of something (a gun's
    // magazine, a mod), not a liquid or gas out of its container.
    const std::string blocked_in = pocket_block( src );
    if( !blocked_in.empty() ) {
        return refuse( u, blocked_in );
    }
    if( src.has_parent() && !src.parent_pocket()->is_type( pocket_type::CONTAINER ) ) {
        return refuse( u, string_format( _( "Your %s is part of something else." ), name ) );
    }
    if( src->made_of_from_type( phase_id::LIQUID ) || src->made_of_from_type( phase_id::GAS ) ) {
        return refuse( u, string_format( _( "You can't hold the %s in your hand." ), name ) );
    }
    const item_location weapon = u.get_wielded_item();
    const bool wielded = weapon && weapon.get_item() == src.get_item();
    const item_location off = offhand_item( u );
    const bool in_off = off && off.get_item() == src.get_item();
    if( a.hand == item_hand::main ) {
        if( wielded ) {
            return refuse( u, string_format( _( "You're already holding your %s." ), name ) );
        }
        if( weapon ) {
            return refuse( u, string_format( _( "Your hand is full: you're holding your %s." ), weapon->tname() ) );
        }
        const ret_val<void> can = u.can_wield( *src );
        if( !can.success() ) {
            return refuse( u, can.str() );
        }
        // Both hands for a two-handed item: what the other hand holds isn't put away for it.
        if( off && !in_off && src->is_two_handed( u ) ) {
            return refuse( u, string_format( _( "You need both hands free for your %s." ), name ) );
        }
        // CDDA's wield from where the item is carried, with its cost; no theft question (it is carried already).
        if( !u.Character::wield( src ) ) {
            return refuse( u, string_format( _( "You can't hold your %s." ), name ) );
        }
        // From the other hand: it is no longer held there.
        if( item_location held = u.get_wielded_item() ) {
            clear_offhand_mark( *held );
        }
        after_inventory_change( u );
        u.add_msg_if_player( _( "You take your %s in hand." ), name );
        return true;
    }
    if( in_off ) {
        return refuse( u, string_format( _( "You're already holding your %s." ), name ) );
    }
    if( src->is_two_handed( u ) ) {
        return refuse( u, string_format( _( "Your %s is too heavy to hold in one hand." ), name ) );
    }
    if( off ) {
        return refuse( u, string_format( _( "Your other hand is full: you're holding your %s." ), off->tname() ) );
    }
    // A wielded two-handed item was refused above, so what is left here is an arm that can't hold anything.
    const std::string blocked = offhand_unavailable( u );
    if( !blocked.empty() ) {
        return refuse( u, blocked );
    }
    if( !wielded ) {
        const int cost = u.item_handling_cost( *src );
        const std::string why = hold_carried( u, src );
        if( !why.empty() ) {
            return refuse( u, why );
        }
        u.mod_moves( -cost );
        u.add_msg_if_player( _( "You take your %s in your other hand." ), name );
        return true;
    }
    // From the main hand to the other: off-hand items are carried in a pocket, so it needs room in one. An open
    // container with something in it would spill (CDDA asks what to do with it), so it stays where it is.
    if( src->is_bucket_nonempty() ) {
        return refuse( u, string_format( _( "Your %s would spill." ), name ) );
    }
    const ret_val<void> can_let_go = u.can_unwield( *src );
    if( !can_let_go.success() ) {
        return refuse( u, can_let_go.str() );
    }
    if( !u.can_stash( *src ) ) {
        return refuse( u, string_format( _( "There's no room in your inventory for the %s." ), name ) );
    }
    const int cost = u.item_handling_cost( *src );
    item copy = *src;
    mark_offhand_pending( copy );
    item_location added = u.i_add( copy, true, &*src, nullptr, /*allow_drop=*/false, /*allow_wield=*/false );
    if( !added ) {
        return refuse( u, string_format( _( "There's no room in your inventory for the %s." ), name ) );
    }
    u.remove_weapon();
    const std::string why = hold_carried( u, added );
    if( !why.empty() ) {
        // It stays put away in the pocket it went to.
        clear_offhand_mark( *added );
        u.add_msg_if_player( m_info, "%s", why );
    }
    u.mod_moves( -cost );
    after_inventory_change( u );
    u.add_msg_if_player( _( "You take your %s in your other hand." ), name );
    return true;
}
} // namespace

void register_item_actions( input_context &ctxt )
{
    ctxt.register_action( item_action_name, no_translation( "take, put, drop, unload, reload, clear, throw, pour, place, spot or hold one item" ) );
}

bool is_item_action( const std::string &action )
{
    return action == item_action_name;
}

namespace
{
bool run_one_item_action( avatar &u, const std::optional<item_action> &a )
{
    // As CDDA's do_regular_action for pickup and drop. Place and spot never print a message.
    const bool free_op = a->op == item_op::place || a->op == item_op::spot;
    if( u.has_active_mutation( trait_SHELL2 ) || u.has_active_mutation( trait_SHELL3 ) ) {
        const std::string why = _( "You can't do that while you're in your shell." );
        return free_op ? refuse_quietly( why ) : refuse( u, why );
    }
    if( u.has_effect( effect_incorporeal ) && a->op != item_op::drop ) {
        const std::string why = _( "You lack the substance to affect anything." );
        return free_op ? refuse_quietly( why ) : refuse( u, why );
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
        case item_op::throw_:
            return throw_at( u, *a );
        case item_op::pour:
            return pour( u, *a );
        case item_op::place:
            return move_lying( u, *a );
        case item_op::spot:
            return set_rest( u, *a );
        case item_op::hold:
            return hold_in_hand( u, *a );
    }
    return false;
}
} // namespace

bool run_item_action( avatar &u )
{
    const std::optional<item_action> a = take_item_action();
    if( !a ) {
        return false;
    }
    action_refused = false;
    action_done_free = false;
    action_refusal.clear();
    const bool took_time = run_one_item_action( u, a );
    // Done: it took time (an activity it started counts) or it was a free action that went through.
    if( a->tag != 0 && events_enabled() ) {
        const bool done = !action_refused && ( took_time || action_done_free );
        on_item_result( a->item.uid, a->tag, done, done ? std::string() : action_refusal );
    }
    return took_time;
}

} // namespace fp3d
