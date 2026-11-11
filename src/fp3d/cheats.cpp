#include "fp3d/cheats.h"

#include <array>
#include <climits>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "achievement.h"
#include "avatar.h"
#include "calendar.h"
#include "coordinates.h"
#include "fp3d/arrival.h"
#include "fp3d/bridge.h"
#include "fp3d/test_range.h"
#include "game.h"
#include "item.h"
#include "item_location.h"
#include "itype.h"
#include "map.h"
#include "map_scale_constants.h"
#include "messages.h"
#include "translations.h"
#include "type_id.h"

static const itype_id itype_backpack_giant( "backpack_giant" );
static const itype_id itype_bandages( "bandages" );
static const itype_id itype_bottle_plastic( "bottle_plastic" );
static const itype_id itype_crowbar( "crowbar" );
static const itype_id itype_flashlight( "flashlight" );
static const itype_id itype_glock_19( "glock_19" );
static const itype_id itype_hammer( "hammer" );
static const itype_id itype_knife_combat( "knife_combat" );
static const itype_id itype_lighter( "lighter" );
static const itype_id itype_machete( "machete" );
static const itype_id itype_mre_beef( "mre_beef" );
static const itype_id itype_multitool( "multitool" );
static const itype_id itype_pot( "pot" );
static const itype_id itype_remington_870( "remington_870" );
static const itype_id itype_scar_l( "scar_l" );
static const itype_id itype_screwdriver_set( "screwdriver_set" );
static const itype_id itype_water_clean( "water_clean" );

static const trait_id trait_DEBUG_CARDIO( "DEBUG_CARDIO" );
static const trait_id trait_DEBUG_LS( "DEBUG_LS" );
static const trait_id trait_DEBUG_NIGHTVISION( "DEBUG_NIGHTVISION" );
static const trait_id trait_DEBUG_NODMG( "DEBUG_NODMG" );
static const trait_id trait_DEBUG_NOTEMP( "DEBUG_NOTEMP" );
static const trait_id trait_DEBUG_PREVENT_DEATH( "DEBUG_PREVENT_DEATH" );
static const trait_id trait_DEBUG_STAMINA( "DEBUG_STAMINA" );
static const trait_id trait_FP3D_CHEAT_CARRY( "FP3D_CHEAT_CARRY" );

namespace fp3d
{
namespace
{
constexpr std::array<const char *, cheat_count> names = {
    "invincible", "no_needs", "no_weight_limit", "unlimited_ammo", "stamina", "no_temperature", "night_vision"
};
const std::string ammo_value = "fp3d_cheat_ammo";

std::vector<trait_id> traits_of( cheat c )
{
    switch( c ) {
        case cheat::invincible:
            return { trait_DEBUG_NODMG, trait_DEBUG_PREVENT_DEATH };
        case cheat::no_needs:
            return { trait_DEBUG_LS };
        case cheat::no_weight_limit:
            return { trait_FP3D_CHEAT_CARRY };
        case cheat::stamina:
            return { trait_DEBUG_STAMINA, trait_DEBUG_CARDIO };
        case cheat::no_temperature:
            return { trait_DEBUG_NOTEMP };
        case cheat::night_vision:
            return { trait_DEBUG_NIGHTVISION };
        case cheat::unlimited_ammo:
        case cheat::count:
            break;
    }
    return {};
}

// What the client asked for, waiting for the main loop.
struct requests {
    std::mutex m;
    std::array<std::optional<bool>, cheat_count> want{};
    bool kit = false;
    bool allowed = false;
    cheats_view latest;
    uint64_t kits_given = 0;
    std::deque<std::pair<uint64_t, range_op>> range_ops;
    uint64_t range_next_id = 0;
    std::deque<range_op_result> range_results;
};
requests &reqs()
{
    static requests r;
    return r;
}

// As CDDA's debug menu: a game where cheats were used earns no achievements.
void no_achievements()
{
    achievements_tracker &a = get_achievements();
    if( a.is_enabled() ) {
        add_msg( m_mixed, _( "Achievements have been disabled." ) );
        a.set_enabled( false );
    }
}

// The ammo to fill a gun with: what it holds now, else its default; null when it takes no ammo items.
itype_id fill_ammo( const item &gun )
{
    itype_id ammo = gun.ammo_current();
    if( ammo.is_null() ) {
        ammo = gun.ammo_default();
    }
    return !ammo.is_null() && ammo->ammo ? ammo : itype_id::NULL_ID();
}

void top_up( avatar &u )
{
    item_location gun = u.get_wielded_item();
    if( !gun || !gun->is_gun() ) {
        return;
    }
    const itype_id ammo = fill_ammo( *gun );
    if( ammo.is_null() ) {
        return;
    }
    // Only when it isn't full already (refilling makes new ammo items).
    item full( *gun );
    full.ammo_set( ammo );
    if( full.ammo_remaining() > gun->ammo_remaining() ) {
        gun->ammo_set( ammo );
    }
}

void give( avatar &u, item it )
{
    u.i_add_or_drop( it );
}

// `n` of an item: as charges when CDDA counts it by charges (ammo), else n copies.
void give_n( avatar &u, const itype_id &id, int n )
{
    item it( id, calendar::turn );
    if( it.count_by_charges() ) {
        it.charges = n;
        u.i_add_or_drop( it );
    } else {
        u.i_add_or_drop( it, n );
    }
}

// A loaded gun, `spare` loaded spare magazines (magazine-fed guns only) and `loose` rounds.
void give_gun( avatar &u, const itype_id &id, int spare, int loose )
{
    item gun( id, calendar::turn );
    const itype_id ammo = fill_ammo( gun );
    if( ammo.is_null() ) {
        give( u, gun );
        return;
    }
    gun.ammo_set( ammo );
    const itype_id mag = gun.magazine_default();
    give( u, gun );
    if( !gun.magazine_integral() && !mag.is_null() && mag->magazine ) {
        for( int i = 0; i < spare; i++ ) {
            item m( mag, calendar::turn );
            m.ammo_set( ammo );
            give( u, m );
        }
    }
    if( loose > 0 ) {
        give_n( u, ammo, loose );
    }
}

void give_test_kit( avatar &u )
{
    if( !u.wear_item( item( itype_backpack_giant, calendar::turn ), false ) ) {
        give( u, item( itype_backpack_giant, calendar::turn ) );
    }
    give_gun( u, itype_glock_19, 3, 60 );
    give_gun( u, itype_remington_870, 0, 30 );
    give_gun( u, itype_scar_l, 3, 90 );
    for( const itype_id &id : {
             itype_machete, itype_knife_combat, itype_crowbar, itype_hammer, itype_screwdriver_set, itype_multitool,
             itype_lighter, itype_pot
         } ) {
        give( u, item( id, calendar::turn ) );
    }
    item light( itype_flashlight, calendar::turn );
    const itype_id cell = fill_ammo( light );
    if( !cell.is_null() ) {
        light.ammo_set( cell );
    }
    give( u, light );
    give_n( u, itype_bandages, 10 );
    give_n( u, itype_mre_beef, 4 );
    for( int i = 0; i < 3; i++ ) {
        item bottle( itype_bottle_plastic, calendar::turn );
        bottle.put_in( item( itype_water_clean, calendar::turn, 2 ), pocket_type::CONTAINER );
        give( u, bottle );
    }
    add_msg( m_good, "Test kit received." );
}

void set_cheat( avatar &u, cheat c, bool on )
{
    if( c == cheat::unlimited_ammo ) {
        if( on ) {
            u.set_value( ammo_value, std::string( "1" ) );
            top_up( u );
        } else {
            u.remove_value( ammo_value );
        }
    } else {
        for( const trait_id &t : traits_of( c ) ) {
            if( !t.is_valid() ) {
                continue;
            }
            if( on && !u.has_trait( t ) ) {
                u.set_mutation( t );
            } else if( !on && u.has_trait( t ) ) {
                u.unset_mutation( t );
            }
        }
    }
    add_msg( m_info, "Cheat %s %s.", cheat_name( c ), on ? "on" : "off" );
}
} // namespace

const char *cheat_name( cheat c )
{
    const size_t i = static_cast<size_t>( c );
    return i < cheat_count ? names[i] : "";
}

std::optional<cheat> cheat_from_name( const std::string &name )
{
    for( size_t i = 0; i < cheat_count; i++ ) {
        if( name == names[i] ) {
            return static_cast<cheat>( i );
        }
    }
    return std::nullopt;
}

void set_cheats_allowed( bool on )
{
    requests &r = reqs();
    std::lock_guard<std::mutex> lk( r.m );
    r.allowed = on;
    if( !on ) {
        r.want = {};
        r.kit = false;
        r.range_ops.clear();
    }
}

bool cheats_allowed()
{
    requests &r = reqs();
    std::lock_guard<std::mutex> lk( r.m );
    return r.allowed;
}

bool request_cheat( cheat c, bool on )
{
    const size_t i = static_cast<size_t>( c );
    if( i >= cheat_count || is_shut_down() ) {
        return false;
    }
    {
        requests &r = reqs();
        std::lock_guard<std::mutex> lk( r.m );
        if( !r.allowed ) {
            return false;
        }
        r.want[i] = on;
    }
    return want_job();
}

bool request_test_kit()
{
    if( is_shut_down() ) {
        return false;
    }
    {
        requests &r = reqs();
        std::lock_guard<std::mutex> lk( r.m );
        if( !r.allowed ) {
            return false;
        }
        r.kit = true;
    }
    return want_job();
}

uint64_t request_range_op( const range_op &op )
{
    if( is_shut_down() || op.monster.size() > max_monster_id ) {
        return 0;
    }
    uint64_t id = 0;
    {
        requests &r = reqs();
        std::lock_guard<std::mutex> lk( r.m );
        if( !r.allowed || r.range_ops.size() >= max_range_ops_waiting ) {
            return 0;
        }
        id = ++r.range_next_id;
        r.range_ops.emplace_back( id, op );
    }
    return want_job() ? id : 0;
}

std::vector<range_op_result> latest_range_results()
{
    requests &r = reqs();
    std::lock_guard<std::mutex> lk( r.m );
    return std::vector<range_op_result>( r.range_results.begin(), r.range_results.end() );
}

namespace
{
// The teleport cheat (range_op_kind::point_teleport). The client's square is checked, never trusted: bounded, then
// in the loaded bubble. The player arrives as in the test range's zone teleport (fp3d/arrival.h).
std::string teleport_to_square( const ui_point &square )
{
    if( !in_timed_hook() ) {
        return "Only between turns.";
    }
    if( !cheats_allowed() ) {
        return "Cheats are off.";
    }
    avatar &u = get_avatar();
    if( g->uquit != QUIT_NO || u.is_dead_state() ) {
        return "Not now.";
    }
    const char *const outside = "That square is outside the reality bubble.";
    // Bounded before any arithmetic on it (the bubble is far smaller): no overflow in the map's conversions.
    constexpr int max_coord = 1 << 28;
    const auto ok = []( int v ) {
        return v >= -max_coord && v <= max_coord;
    };
    if( !ok( square.x ) || !ok( square.y ) || square.z < -OVERMAP_DEPTH || square.z > OVERMAP_HEIGHT ) {
        return outside;
    }
    map &here = get_map();
    const tripoint_abs_ms target( square.x, square.y, square.z );
    if( !here.inbounds( here.get_bub( target ) ) ) {
        return outside;
    }
    if( u.is_mounted() ) {
        return "Not while riding.";
    }
    if( u.in_vehicle ) {
        return "Not while in a vehicle.";
    }
    const std::optional<tripoint_abs_ms> arrive = arrival_square( target );
    if( !arrive ) {
        return "No free square to arrive on there.";
    }
    if( *arrive == u.pos_abs() ) {
        return "You are already there.";
    }
    no_achievements();
    leave_square( u );
    // Another floor: the map shifts to it first, as CDDA's own moves between floors do (game::place_player).
    if( arrive->z() != u.posz() ) {
        g->vertical_shift( arrive->z() );
    }
    u.setpos( *arrive, false );
    settle_avatar();
    return "";
}

std::string run_range_op( const range_op &op )
{
    // Out-of-range numbers become -1, which every operation refuses.
    const int n = op.number < 0 || op.number > INT_MAX ? -1 : static_cast<int>( op.number );
    switch( op.kind ) {
        case range_op_kind::spawn_item:
            return spawn_item( op.itype, n, op.square );
        case range_op_kind::spawn_monster:
            return spawn_monster( op.monster, op.square );
        case range_op_kind::teleport:
            return teleport_to_zone( n < 0 ? SIZE_MAX : static_cast<size_t>( n ) );
        case range_op_kind::time_of_day:
            return set_time_of_day( n );
        case range_op_kind::advance:
            return advance( n );
        case range_op_kind::ai_frozen:
            return set_ai_frozen( op.number != 0 );
        case range_op_kind::heal:
            return heal();
        case range_op_kind::rebuild_zone:
            return rebuild_zone( n < 0 ? SIZE_MAX : static_cast<size_t>( n ) );
        case range_op_kind::point_teleport:
            return teleport_to_square( op.square );
    }
    return "No such operation.";
}
} // namespace

void run_range_requests()
{
    if( !in_timed_hook() ) {
        return;
    }
    std::deque<std::pair<uint64_t, range_op>> ops;
    {
        requests &r = reqs();
        std::lock_guard<std::mutex> lk( r.m );
        ops.swap( r.range_ops );
    }
    for( const auto &[id, op] : ops ) {
        std::string error = run_range_op( op );
        requests &r = reqs();
        std::lock_guard<std::mutex> lk( r.m );
        r.range_results.push_back( { id, std::move( error ) } );
        while( r.range_results.size() > max_range_results ) {
            r.range_results.pop_front();
        }
    }
}

cheats_view latest_cheats()
{
    requests &r = reqs();
    std::lock_guard<std::mutex> lk( r.m );
    return r.latest;
}

bool cheat_on( const avatar &u, cheat c )
{
    if( c == cheat::unlimited_ammo ) {
        return u.maybe_get_value( ammo_value ) != nullptr;
    }
    const std::vector<trait_id> ts = traits_of( c );
    if( ts.empty() ) {
        return false;
    }
    for( const trait_id &t : ts ) {
        if( !u.has_trait( t ) ) {
            return false;
        }
    }
    return true;
}

void run_cheat_requests( avatar &u )
{
    std::array<std::optional<bool>, cheat_count> want;
    bool kit = false;
    {
        requests &r = reqs();
        std::lock_guard<std::mutex> lk( r.m );
        want = std::exchange( r.want, {} );
        kit = std::exchange( r.kit, false );
    }
    bool turning_on = kit;
    for( size_t i = 0; i < cheat_count; i++ ) {
        turning_on = turning_on || ( want[i] && *want[i] && !cheat_on( u, static_cast<cheat>( i ) ) );
    }
    if( turning_on ) {
        no_achievements();
    }
    for( size_t i = 0; i < cheat_count; i++ ) {
        if( want[i] && cheat_on( u, static_cast<cheat>( i ) ) != *want[i] ) {
            set_cheat( u, static_cast<cheat>( i ), *want[i] );
        }
    }
    if( kit ) {
        give_test_kit( u );
        requests &r = reqs();
        std::lock_guard<std::mutex> lk( r.m );
        r.kits_given++;
    }
    publish_cheats( u );
}

void run_jobs()
{
    run_cheat_requests( get_avatar() );
}

void cheats_turn( avatar &u )
{
    if( cheat_on( u, cheat::unlimited_ammo ) ) {
        top_up( u );
    }
}

void publish_cheats( const avatar &u )
{
    uint32_t on = 0;
    for( size_t i = 0; i < cheat_count; i++ ) {
        if( cheat_on( u, static_cast<cheat>( i ) ) ) {
            on |= uint32_t{ 1 } << i;
        }
    }
    requests &r = reqs();
    std::lock_guard<std::mutex> lk( r.m );
    r.latest.seq++;
    r.latest.on = on;
    r.latest.kits_given = r.kits_given;
}
} // namespace fp3d
