#include "fp3d/cheats.h"

#include <algorithm>
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
#include "bodypart.h"
#include "calendar.h"
#include "character.h"
#include "coordinates.h"
#include "creature.h"
#include "creature_tracker.h"
#include "effect.h"
#include "fp3d/arrival.h"
#include "fp3d/bridge.h"
#include "fp3d/test_range.h"
#include "game.h"
#include "item.h"
#include "item_location.h"
#include "itype.h"
#include "map.h"
#include "map_scale_constants.h"
#include "mapdata.h"
#include "messages.h"
#include "monster.h"
#include "mtype.h"
#include "npc.h"
#include "options.h"
#include "translations.h"
#include "type_id.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "weather.h"
#include "weather_type.h"

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

static const efftype_id effect_harnessed( "harnessed" );
static const efftype_id effect_pet( "pet" );
static const efftype_id effect_ridden( "ridden" );

static const flag_id json_flag_GRAB( "GRAB" );

static const json_character_flag json_flag_CANNOT_MOVE( "CANNOT_MOVE" );
static const json_character_flag json_flag_WATERWALKING( "WATERWALKING" );

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
    "invincible", "no_needs", "no_weight_limit", "unlimited_ammo", "stamina", "no_temperature", "night_vision",
    "unstoppable"
};
const std::string ammo_value = "fp3d_cheat_ammo";
const std::string unstoppable_value = "fp3d_cheat_unstoppable";

// The cheats kept in an avatar var rather than traits.
const std::string *value_of( cheat c )
{
    switch( c ) {
        case cheat::unlimited_ammo:
            return &ammo_value;
        case cheat::unstoppable:
            return &unstoppable_value;
        case cheat::invincible:
        case cheat::no_needs:
        case cheat::no_weight_limit:
        case cheat::stamina:
        case cheat::no_temperature:
        case cheat::night_vision:
        case cheat::count:
            break;
    }
    return nullptr;
}

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
        case cheat::unstoppable:
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
    if( const std::string *value = value_of( c ) ) {
        if( on ) {
            u.set_value( *value, std::string( "1" ) );
            if( c == cheat::unlimited_ammo ) {
                top_up( u );
            }
        } else {
            u.remove_value( *value );
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

void reset_cheats()
{
    requests &r = reqs();
    std::lock_guard<std::mutex> lk( r.m );
    r.want = {};
    r.kit = false;
    r.allowed = false;
    const uint64_t seq = r.latest.seq;
    r.latest = cheats_view();
    r.latest.seq = seq + 1;
    r.kits_given = 0;
    r.range_ops.clear();
    r.range_results.clear();
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
    if( is_shut_down() || op.monster.size() > max_monster_id || op.weather.size() > max_weather_id ) {
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

// The loaded weather type with exactly this id (never "null"), found among the loaded ones: a client's text never
// becomes an id of its own.
std::optional<weather_type_id> loaded_weather( const std::string &id )
{
    if( id.empty() || id.size() > max_weather_id || id == WEATHER_NULL.str() ) {
        return std::nullopt;
    }
    for( const weather_type &w : weather_types::get_all() ) {
        if( w.id.str() == id ) {
            return w.id;
        }
    }
    return std::nullopt;
}

// The weather cheat (range_op_kind::weather), as CDDA's debug menu: the override, then the weather now.
std::string set_weather( const std::string &id, int wind )
{
    if( !in_timed_hook() ) {
        return "Only between turns.";
    }
    if( !cheats_allowed() ) {
        return "Cheats are off.";
    }
    const avatar &u = get_avatar();
    if( g->uquit != QUIT_NO || u.is_dead_state() ) {
        return "Not now.";
    }
    if( wind < -1 || wind > max_cheat_wind ) {
        return "No such wind.";
    }
    std::optional<weather_type_id> chosen;
    if( !id.empty() ) {
        chosen = loaded_weather( id );
        if( !chosen ) {
            return "No such weather.";
        }
    }
    // The option wins over any override (weather_manager::update_weather).
    if( get_option<std::string>( "ETERNAL_WEATHER" ) != "normal" ) {
        return "This world's weather never changes.";
    }
    no_achievements();
    weather_manager &w = get_weather();
    const bool range = chosen ? range_active() : keep_range_weather();
    if( chosen ) {
        w.weather_override = *chosen;
    } else if( !range ) {
        w.weather_override = WEATHER_NULL;
    }
    if( wind >= 0 ) {
        w.windspeed_override = wind;
    } else if( range ) {
        w.windspeed_override = 0;
    } else {
        w.windspeed_override = std::nullopt;
    }
    w.set_nextweather( calendar::turn );
    w.update_weather();
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
        case range_op_kind::weather:
            return set_weather( op.weather, op.wind );
        case range_op_kind::break_grabs:
            // Checked again here, on the sim thread: allowing may have been turned off since it was queued.
            if( !cheats_allowed() ) {
                return "Cheats aren't allowed.";
            }
            if( g->uquit != QUIT_NO || get_avatar().is_dead_state() ) {
                return "Not now.";
            }
            no_achievements();
            break_grabs( get_avatar() );
            return "";
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
    if( const std::string *value = value_of( c ) ) {
        return u.maybe_get_value( *value ) != nullptr;
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

int break_grabs( Character &u )
{
    int n = 0;
    if( u.is_mounted() ) {
        // As CDDA's break-free for a mount: the mount's grabbed effects go.
        monster &mount = *u.mounted_creature;
        std::vector<efftype_id> ids;
        for( const effect &e : mount.get_effects_with_flag( json_flag_GRAB ) ) {
            ids.push_back( e.get_id() );
        }
        for( const efftype_id &id : ids ) {
            mount.remove_effect( id );
            n++;
        }
        if( !ids.empty() ) {
            u.add_msg_if_player( m_good, _( "Your %s breaks free from the grab!" ), mount.get_name() );
        }
    }
    // Copied first: removing an effect would leave the list's references dangling.
    std::vector<std::pair<efftype_id, bodypart_id>> grabs;
    for( const effect &e : u.get_effects_with_flag( json_flag_GRAB ) ) {
        grabs.emplace_back( e.get_id(), e.get_bp() );
    }
    map &here = get_map();
    creature_tracker &creatures = get_creature_tracker();
    for( const auto &[id, bp] : grabs ) {
        monster *grabber = nullptr;
        for( const tripoint_bub_ms &p : here.points_in_radius( u.pos_bub( here ), 1, 0 ) ) {
            monster *const m = creatures.creature_at<monster>( p );
            if( m != nullptr && m->is_grabbing( bp.id() ) ) {
                grabber = m;
                break;
            }
        }
        if( grabber != nullptr ) {
            grabber->remove_grab( bp.id() );
            u.add_msg_player_or_npc( m_good, _( "You break %s grab on your %s!" ),
                                     _( "<npcname> breaks %s grab on their %s!" ), grabber->disp_name( true ),
                                     body_part_name( bp ) );
        }
        u.remove_effect( id, bp );
        n++;
    }
    return n;
}

void unstoppable_acts( avatar &u )
{
    if( cheat_on( u, cheat::unstoppable ) ) {
        break_grabs( u );
    }
}

bool pushes_past( const avatar &u, const tripoint_bub_ms &dest )
{
    // A player who can't move steps (and bumps) as CDDA has it.
    if( !cheat_on( u, cheat::unstoppable ) || u.has_flag( json_flag_CANNOT_MOVE ) ) {
        return false;
    }
    map &here = get_map();
    const tripoint_bub_ms from = u.pos_bub( here );
    // A step on one level only; stairs and ramps stay CDDA's.
    if( dest.z() != from.z() || rl_dist( from, dest ) != 1 ) {
        return false;
    }
    // On a fast vehicle CDDA asks about diving off first: its bump.
    const vehicle *const veh0 = veh_pointer_or_null( here.veh_at( from ) );
    if( veh0 != nullptr && std::abs( veh0->velocity ) > 100 ) {
        return false;
    }
    // Hostile as avatar_action::move judges it for its bump attack.
    creature_tracker &creatures = get_creature_tracker();
    if( const monster *const m = creatures.creature_at<monster>( dest, true ) ) {
        // One CDDA never displaces (a turret, a fungal tower, harnessed, ridden) is bumped as without the cheat.
        if( m->has_flag( mon_flag_IMMOBILE ) || m->has_flag( json_flag_CANNOT_MOVE ) ||
            m->has_effect( effect_harnessed ) || m->has_effect( effect_ridden ) ) {
            return false;
        }
        return m->friendly == 0 && !m->has_effect( effect_pet );
    }
    const npc *const np = creatures.creature_at<npc>( dest );
    if( np == nullptr || !np->is_enemy() ) {
        return false;
    }
    // CDDA's swap unboards both and boards them again, which a refused step would leave wrong: on a vehicle part
    // (or mounted) an enemy NPC is bumped as without the cheat.
    return !u.is_mounted() && !np->is_mounted() && !here.veh_at( from ) && !here.veh_at( dest );
}

// The step into `dest` will be avatar_action::move's swim (deep water, no boat): no displacement then.
static bool step_swims( const avatar &u, const map &here, const tripoint_bub_ms &dest )
{
    return here.has_flag( ter_furn_flag::TFLAG_SWIMMABLE, dest ) &&
           here.has_flag( ter_furn_flag::TFLAG_DEEP_WATER, dest ) && !here.has_flag_furn( "BRIDGE", dest ) &&
           !here.veh_at( dest ) && !u.has_flag( json_flag_WATERWALKING );
}

push_result push_past( avatar &u, const tripoint_bub_ms &dest )
{
    if( !pushes_past( u, dest ) ) {
        return push_result::none;
    }
    map &here = get_map();
    const tripoint_bub_ms from = u.pos_bub( here );
    creature_tracker &creatures = get_creature_tracker();
    if( monster *const m = creatures.creature_at<monster>( dest, true ) ) {
        if( m->is_hallucination() ) {
            // As CDDA's bump into one: it vanishes (its displacement doesn't see hallucinations).
            m->die( &here, &u );
            return push_result::clear;
        }
        // CDDA's displacement in game::place_player moves it to the square left (a mount pushes it aside). A swim
        // never gets there.
        if( !step_swims( u, here, dest ) && ( u.is_mounted() || m->can_move_to( from ) ) ) {
            return push_result::clear;
        }
        // The square left won't take it (deep water, a ledge), or the step is a swim: the nearest free square next
        // to it.
        for( const tripoint_bub_ms &p : here.points_in_radius( dest, 1, 0 ) ) {
            if( p != dest && p != from && g->is_empty( p ) && m->can_move_to( p ) && m->move_to( p ) ) {
                add_msg( _( "You push the %s aside." ), m->name() );
                return push_result::clear;
            }
        }
        add_msg( m_info, _( "There's no room to push past the %s." ), m->name() );
        return push_result::refused;
    }
    npc *const np = creatures.creature_at<npc>( dest );
    if( np == nullptr ) {
        return push_result::none;
    }
    // CDDA's swap with a follower: the NPC takes the square left and the player walks (game::walk_move, a normal
    // step's cost). The walk may refuse and the map may shift, so absolute squares are compared.
    const tripoint_abs_ms from_abs = here.get_abs( from );
    const tripoint_abs_ms dest_abs = here.get_abs( dest );
    if( !g->swap_critters( u, *np ) ) {
        return push_result::refused;
    }
    if( u.pos_abs() != dest_abs ) {
        // The walk said why; the NPC goes back to its own square.
        if( np->pos_abs() == from_abs ) {
            np->setpos( dest_abs );
        }
        return push_result::refused;
    }
    add_msg( _( "You push past %s." ), np->get_name() );
    return push_result::done;
}

void publish_cheats( const avatar &u )
{
    uint32_t on = 0;
    for( size_t i = 0; i < cheat_count; i++ ) {
        if( cheat_on( u, static_cast<cheat>( i ) ) ) {
            on |= uint32_t{ 1 } << i;
        }
    }
    const weather_manager &w = get_weather();
    std::string held = w.weather_override == WEATHER_NULL ? std::string() : w.weather_override.str();
    const int wind = w.windspeed_override ? std::clamp( *w.windspeed_override, 0, max_cheat_wind ) : -1;
    requests &r = reqs();
    std::lock_guard<std::mutex> lk( r.m );
    r.latest.seq++;
    r.latest.on = on;
    r.latest.kits_given = r.kits_given;
    r.latest.weather = std::move( held );
    r.latest.wind = wind;
}
} // namespace fp3d
