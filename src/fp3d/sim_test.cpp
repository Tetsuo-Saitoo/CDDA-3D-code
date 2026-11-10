// FP3D block B2 test: runs CDDA's real game loop on a worker thread behind the bridge and plays it from the
// main thread like a client would: open a door, pick up an apple, eat it, look around, throw a rock at a
// square, wait 5 minutes, run in real time until a zombie auto-pauses it, then save, quit and reload.
// Every request is printed. Exit code 0 only if all goals are verified from game state.
//
// usage: fp3d-sim-test   (run from the cdda/ folder)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <iostream>
#if defined(_WIN32)
#include <windows.h>
#endif

#include "avatar.h"
#include "cached_options.h"
#include "calendar.h"
#include "coordinates.h"
#include "debug.h"
#include "do_turn.h"
#include "fp3d/boot.h"
#include "fp3d/bridge.h"
#include "fp3d/cheats.h"
#include "fp3d/game_access.h"
#include "fp3d/held.h"
#include "fp3d/points.h"
#include "fp3d/shot.h"
#include "fp3d/events.h"
#include "fp3d/snapshot.h"
#include "game.h"
#include "item.h"
#include "math_parser_diag_value.h"
#include "map.h"
#include "mapdata.h"
#include "messages.h"
#include "monster.h"
#include "npc.h"
#include "path_info.h"
#include "pocket_type.h"
#include "rng.h"
#include "type_id.h"
#include "units.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "worldfactory.h"

namespace
{
const itype_id itype_apple( "apple" );
const itype_id itype_backpack( "backpack" );
const itype_id itype_rock( "rock" );
const mtype_id mon_zombie( "mon_zombie" );
const itype_id itype_2h_flail_steel( "2h_flail_steel" );
const itype_id itype_gasoline( "gasoline" );
const itype_id itype_hose( "hose" );
const itype_id itype_jerrycan( "jerrycan" );
const itype_id itype_water( "water" );
const itype_id itype_glock_19( "glock_19" );
const itype_id itype_9mm( "9mm" );
const itype_id itype_bag_plastic( "bag_plastic" );
const itype_id itype_bottle_plastic( "bottle_plastic" );
const itype_id itype_1l_gold( "1l_gold" );
const itype_id itype_remington_870( "remington_870" );
const itype_id itype_scar_l( "scar_l" );
const itype_id itype_backpack_giant( "backpack_giant" );
const ter_str_id ter_t_water_sh( "t_water_sh" );
const vproto_id vehicle_prototype_none( "none" );
const vpart_id vpart_frame( "frame" );
const vpart_id vpart_tank_small( "tank_small" );
const ter_str_id ter_t_door_c( "t_door_c" );
const ter_str_id ter_t_floor( "t_floor" );
const trait_id trait_DEBUG_NIGHTVISION( "DEBUG_NIGHTVISION" );

// Game state the driver checks; copied on the sim thread right before every request.
struct observed {
    int64_t turn = 0;
    std::string east_ter;
    int apples_here = 0;
    int apples_carried = 0;
    int rocks_carried = 0;
    int rocks_west = 0;      // rocks on the ground 1-8 squares west of the avatar
    int loads = 0;           // times the game was reloaded from its save
    int zombies_placed = 0;
    int visible = 0;         // creatures the avatar sees
    int safe_mode = 0;
    int can_ml = -1;         // siphon check: gasoline in the carried jerrycan (-1 none carried)
    int tank_ml = -1;        // gasoline left in the vehicle north (-1 no vehicle)
    int puddle = 0;          // charges of water lying south
    int hoses = 0;           // hoses carried (wielded included)
    int ammo = -1;           // shoot check: rounds in the wielded gun (-1 nothing wielded)
    int zombie_hp = -1;      // hp of the first monster (-1 none)
    uint64_t zombie_uid = 0; // its snapshot uid
    int moves = 0;
    int speed = 100;
    int bag_rocks = 0;       // item checks: rocks inside a plastic bag lying west
    int carried_9mm = 0;     // item checks 2: 9 mm rounds carried outside guns and magazines
    int carry_kg = 0;        // cheat checks: the avatar's weight capacity
    int torso_hp_lost = -1;  // ... hp the torso lost to a 10 damage hit (-1: not probed)
    int kit_guns = 0;        // ... carried Glock 19, Remington 870 and SCAR-L with ammo loaded
    bool big_pack = false;   // ... the kit's big backpack, worn (or carried when CDDA won't let it be worn)
    std::vector<std::string> messages;
};
std::mutex observed_mutex;
observed last_observed;
std::string sim_error;
std::atomic<int> loads{ 0 };
std::atomic<bool> spawn_zombie{ false };
std::atomic<bool> clear_zombies_now{ false };  // safe mode checks: remove all monsters before the next request
std::atomic<int> new_game_safe_mode{ -1 };     // g->safe_mode at the new game's first main-loop request
std::atomic<int> loaded_safe_mode{ -1 };       // ... and at the first one after the reload
std::atomic<bool> cheat_probe{ false };
std::atomic<int> world_name_failures{ -1 };     // world name checks at boot (-1: not run)
std::atomic<int> sealed_items{ -1 };            // item checks 2: items placed in the gun safe         // cheat checks: hit the torso, wield an empty Glock
// Hand checks: 1 = a zombie to the west and a rock in hand, 2 = a two-handed flail in hand, 3 = a hose in hand,
// an empty jerrycan carried and a one-tile vehicle north with 4 L of gasoline in a small tank, 4 = a zombie west,
// 5 = zombies gone and a spare hose carried, 6 = the spare hose dropped here, 7 = picked up again, 8 = the jerrycan
// wielded (the hose in hand gone), 9 = shallow water east and a puddle of 2 L of water south, 10 = a new empty
// jerrycan wielded, 11 = a zombie 3 squares west and a loaded pistol in hand, 12 = item checks: empty hands, two
// rocks, a plastic bottle and a plastic bag west, a rock 3 squares west, a rock with a hostile save's handle and
// spot north-west, 1 L of gold and a bottle of water carried. Done on the sim thread right before its next request.
std::atomic<int> hand_setup{ 0 };

void observe()
{
    const avatar &u = get_avatar();
    map &here = get_map();
    const tripoint_bub_ms p = u.pos_bub();
    static int zombies_placed = 0;
    if( clear_zombies_now.exchange( false ) ) {
        g->clear_zombies();
    }
    if( spawn_zombie.exchange( false ) &&
        g->place_critter_at( mon_zombie, p + tripoint_rel_ms( -4, -4, 0 ) ) != nullptr ) {
        zombies_placed++;
    }
    switch( hand_setup.exchange( 0 ) ) {
        case 1:
            g->clear_zombies();
            g->place_critter_at( mon_zombie, p + tripoint_rel_ms( -1, 0, 0 ) );
            get_avatar().set_wielded_item( item( itype_rock ) );
            break;
        case 2:
            get_avatar().set_wielded_item( item( itype_2h_flail_steel ) );
            break;
        case 3: {
            g->clear_zombies();
            get_avatar().set_wielded_item( item( itype_hose ) );
            get_avatar().i_add( item( itype_jerrycan ) );
            vehicle *veh = here.add_vehicle( vehicle_prototype_none, p + tripoint_rel_ms( 0, -1, 0 ), 0_degrees,
                                             0, 0 );
            if( veh != nullptr ) {
                veh->install_part( here, point_rel_ms::zero, vpart_frame );
                const int tank = veh->install_part( here, point_rel_ms::zero, vpart_tank_small );
                if( tank >= 0 ) {
                    veh->part( tank ).ammo_set( itype_gasoline, 4000 );
                }
                here.add_vehicle_to_cache( veh );
            }
            break;
        }
        case 4:
            g->clear_zombies();
            g->place_critter_at( mon_zombie, p + tripoint_rel_ms( -1, 0, 0 ) );
            break;
        case 5:
            g->clear_zombies();
            get_avatar().i_add( item( itype_hose ) );
            break;
        case 6: {
            avatar &u = get_avatar();
            const item *weapon = u.get_wielded_item() ? u.get_wielded_item().get_item() : nullptr;
            for( item_location &loc : u.all_items_loc() ) {
                if( loc->typeId() == itype_hose && loc.get_item() != weapon ) {
                    here.add_item_or_charges( p, *loc );
                    loc.remove_item();
                    break;
                }
            }
            break;
        }
        case 7: {
            map_stack items = here.i_at( p );
            for( auto it = items.begin(); it != items.end(); ++it ) {
                if( it->typeId() == itype_hose ) {
                    get_avatar().i_add( *it );
                    items.erase( it );
                    break;
                }
            }
            break;
        }
        case 8: {
            avatar &u = get_avatar();
            const item *weapon = u.get_wielded_item() ? u.get_wielded_item().get_item() : nullptr;
            for( item_location &loc : u.all_items_loc() ) {
                if( loc->typeId() == itype_jerrycan && loc.get_item() != weapon ) {
                    const item can = *loc;
                    loc.remove_item();
                    u.set_wielded_item( can );
                    break;
                }
            }
            break;
        }
        case 9:
            here.ter_set( p + tripoint_rel_ms( 1, 0, 0 ), ter_t_water_sh );
            here.add_item_or_charges( p + tripoint_rel_ms( 0, 1, 0 ), item( itype_water, calendar::turn, 8 ) );
            break;
        case 10:
            get_avatar().set_wielded_item( item( itype_jerrycan ) );
            break;
        case 11: {
            g->clear_zombies();
            g->place_critter_at( mon_zombie, p + tripoint_rel_ms( -3, 0, 0 ) );
            item gun( itype_glock_19 );
            gun.ammo_set( itype_9mm, 15 );
            get_avatar().set_wielded_item( gun );
            break;
        }
        case 12: {
            avatar &u = get_avatar();
            g->clear_zombies();
            for( int dx = -4; dx <= 4; dx++ ) {
                for( int dy = -4; dy <= 4; dy++ ) {
                    here.i_clear( p + tripoint_rel_ms( dx, dy, 0 ) );
                }
            }
            u.remove_weapon();
            const tripoint_bub_ms w = p + tripoint_rel_ms( -1, 0, 0 );
            here.add_item( w, item( itype_rock ) );
            here.add_item( w, item( itype_rock ) );
            here.add_item( w, item( itype_bottle_plastic ) );
            here.add_item( w, item( itype_bag_plastic ) );
            here.add_item( p + tripoint_rel_ms( -3, 0, 0 ), item( itype_rock ) );
            item odd( itype_rock );
            odd.set_var( "fp3d_uid", "12abc" );
            odd.set_var( "fp3d_spot", "5000,-1" );
            here.add_item( p + tripoint_rel_ms( -1, -1, 0 ), odd );
            u.i_add( item( itype_1l_gold ) );
            item bottle( itype_bottle_plastic );
            bottle.put_in( item( itype_water, calendar::turn, 2 ), pocket_type::CONTAINER );
            u.i_add( bottle );
            break;
        }
        case 13: {
            // A locked gun safe south (sealed: never seen into or reached by hand) with a rock whose handle a
            // client could know from before, a dresser three squares north (seen into only from next to it), a
            // zombie corpse east, 20 rounds of 9 mm in the pack.
            const tripoint_bub_ms s = p + tripoint_rel_ms( 0, 1, 0 );
            here.i_clear( s );
            here.furn_set( s, furn_str_id( "f_gunsafe_ml" ) );
            item hidden( itype_rock );
            hidden.set_var( "fp3d_uid", "777" );
            here.add_item( s, hidden );
            sealed_items = static_cast<int>( here.i_at( s ).size() );
            const tripoint_bub_ms n = p + tripoint_rel_ms( 0, -3, 0 );
            here.i_clear( n );
            here.furn_set( n, furn_str_id( "f_dresser" ) );
            here.add_item( n, item( itype_rock ) );
            here.add_item( p + tripoint_rel_ms( 1, 0, 0 ), item::make_corpse( mon_zombie ) );
            get_avatar().i_add( item( itype_9mm, calendar::turn, 20 ) );
            // FP3D vars of other types than ours (a save edited by hand): read as none, no debug message.
            item typed( itype_rock );
            typed.set_var( "fp3d_uid", diag_value( 5.0 ) );
            typed.set_var( "fp3d_spot", diag_value( 1.0 ) );
            here.add_item( p + tripoint_rel_ms( 0, -1, 0 ), typed );
            item marked( itype_rock );
            marked.set_var( "fp3d_offhand", diag_value( std::string( "x" ) ) );
            get_avatar().i_add( marked );
            break;
        }
        default:
            break;
    }
    observed o;
    if( cheat_probe.exchange( false ) ) {
        const bodypart_id torso = bodypart_str_id( "torso" ).id();
        const int before = u.get_hp( torso );
        get_avatar().apply_damage( nullptr, torso, 10 );
        o.torso_hp_lost = before - u.get_hp( torso );
        item empty( itype_glock_19 );
        empty.ammo_set( itype_9mm );
        empty.ammo_unset();
        get_avatar().set_wielded_item( empty );
    }
    o.carry_kg = static_cast<int>( units::to_kilogram( u.weight_capacity() ) );
    for( const item_location &it : get_avatar().all_items_loc() ) {
        if( it->typeId() == itype_9mm && !( it.has_parent() && ( it.parent_item()->is_gun() ||
                                            it.parent_item()->is_magazine() ) ) ) {
            o.carried_9mm += it->charges;
        }
    }
    for( const item_location &it : get_avatar().all_items_loc() ) {
        if( ( it->typeId() == itype_glock_19 || it->typeId() == itype_remington_870 ||
              it->typeId() == itype_scar_l ) && it->ammo_remaining() > 0 ) {
            o.kit_guns++;
        }
        o.big_pack = o.big_pack || it->typeId() == itype_backpack_giant;
    }
    o.zombies_placed = zombies_placed;
    o.visible = static_cast<int>( u.get_visible_creatures( 60 ).size() );
    o.safe_mode = static_cast<int>( g->safe_mode );
    o.turn = to_turns<int64_t>( calendar::turn - calendar::turn_zero );
    o.east_ter = here.ter( p + tripoint_rel_ms( 1, 0, 0 ) ).id().str();
    for( const item &it : here.i_at( p ) ) {
        if( it.typeId() == itype_apple ) {
            o.apples_here++;
        }
    }
    for( int dx = -8; dx <= -1; dx++ ) {
        for( int dy = -2; dy <= 2; dy++ ) {
            for( const item &it : here.i_at( p + tripoint_rel_ms( dx, dy, 0 ) ) ) {
                if( it.typeId() == itype_rock ) {
                    o.rocks_west++;
                }
            }
        }
    }
    o.apples_carried = u.amount_of( itype_apple );
    for( const item_location &loc : get_avatar().all_items_loc() ) {
        if( loc->typeId() == itype_jerrycan ) {
            o.can_ml = units::to_milliliter( loc->get_total_contained_volume() );
            break;
        }
    }
    if( const optional_vpart_position vp = here.veh_at( p + tripoint_rel_ms( 0, -1, 0 ) ) ) {
        o.tank_ml = vp->vehicle().fuel_left( here, itype_gasoline );
    }
    for( const item &it : here.i_at( p + tripoint_rel_ms( 0, 1, 0 ) ) ) {
        if( it.typeId() == itype_water ) {
            o.puddle += it.charges;
        }
    }
    o.hoses = u.amount_of( itype_hose );
    if( const item_location w = u.get_wielded_item() ) {
        o.ammo = w->ammo_remaining();
    }
    for( monster &m : g->all_monsters() ) {
        o.zombie_hp = m.get_hp();
        o.zombie_uid = fp3d::creature_uid( m );
        break;
    }
    o.rocks_carried = u.amount_of( itype_rock );
    o.moves = u.get_moves();
    o.speed = u.get_speed();
    for( const item &it : here.i_at( p + tripoint_rel_ms( -1, 0, 0 ) ) ) {
        if( it.typeId() == itype_bag_plastic ) {
            for( const item *in : it.all_items_top() ) {
                o.bag_rocks += in->typeId() == itype_rock ? 1 : 0;
            }
        }
    }
    o.loads = loads.load();
    for( const auto &m : Messages::recent_messages( 6 ) ) {
        o.messages.push_back( m.second );
    }
    // Block B3: the client reads this snapshot while the sim waits for the answer.
    fp3d::publish_snapshot( fp3d::capture_snapshot() );
    fp3d::publish_held();   // as the bridge's hook does
    std::lock_guard<std::mutex> lk( observed_mutex );
    last_observed = o;
}

observed snapshot()
{
    std::lock_guard<std::mutex> lk( observed_mutex );
    return last_observed;
}

// A 9x9 floor around the avatar, a closed door to the east, an apple underfoot, a rock carried in a
// worn backpack, empty hands, no monsters or NPCs (some starts put one next to you). The avatar sees in
// the dark: starts can be unlit rooms, and the auto-pause check needs the zombie seen. Backpack and
// empty hands because the random start differs between standard libraries (same seed, other
// distributions): some starts have no pockets, and in test_mode CDDA won't put away a wielded item
// (dispose_item), so throwing the rock would fail.
void build_scene()
{
    map &here = get_map();
    avatar &u = get_avatar();
    // Like the unit tests' clear_npcs(); before clearing items, so whatever they drop is gone too.
    g->reload_npcs();
    for( npc &guy : g->all_npcs() ) {
        guy.die( &here, nullptr );
    }
    g->cleanup_dead();
    const tripoint_bub_ms p = u.pos_bub();
    for( int dx = -4; dx <= 4; dx++ ) {
        for( int dy = -4; dy <= 4; dy++ ) {
            const tripoint_bub_ms q = p + tripoint_rel_ms( dx, dy, 0 );
            here.ter_set( q, ter_t_floor );
            here.furn_set( q, furn_str_id::NULL_ID() );
            here.i_clear( q );
        }
    }
    here.ter_set( p + tripoint_rel_ms( 1, 0, 0 ), ter_t_door_c );
    here.add_item( p, item( itype_apple ) );
    u.remove_weapon();
    u.wear_item( item( itype_backpack ), false );
    u.i_add( item( itype_rock ) );
    u.set_mutation( trait_DEBUG_NIGHTVISION );
    g->clear_zombies();
}

// A fresh world gets a folder of its own (boot.h): names in use are found ignoring case, folders CDDA doesn't
// list included; only "fp3d-" worlds are replaceable. Run on the sim thread right after boot.
int check_world_names()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what ) {
        std::printf( "  worlds: %-38s %s\n", what, ok ? "PASS" : "FAIL" );
        failed += ok ? 0 : 1;
    };
    check( fp3d::world_name_taken( "FP3D-SIM" ) == std::optional<std::string>( "fp3d-sim" ),
           "a listed world, other case" );
    check( !fp3d::world_name_taken( "fp3d-sim-free" ), "a free name" );
    const std::filesystem::path odd = std::filesystem::u8path( PATH_INFO::savedir() ) / "Unlisted-Fp3d";
    std::error_code ec;
    std::filesystem::create_directories( odd, ec );
    check( !ec && fp3d::world_name_taken( "unlisted-fp3d" ) == std::optional<std::string>( "Unlisted-Fp3d" ),
           "an unlisted folder, other case" );
    std::filesystem::remove_all( odd, ec );
    check( fp3d::replaceable_world( "fp3d-live" ) && !fp3d::replaceable_world( "fp3d-" ) &&
           !fp3d::replaceable_world( "World" ) && !fp3d::replaceable_world( "FP3D-live" ),
           "only fp3d- worlds replaceable" );
    return failed;
}

void sim_main()
{
    try {
        fp3d::boot_world( "fp3d-sim", true );
        world_name_failures = check_world_names();
        std::printf( "sim: data loaded, starting game\n" );
        // The real new-game path: calendar, start location, starting items, game mode.
        if( !fp3d_game_access::start_game( *g ) ) {
            throw std::runtime_error( "start_game failed" );
        }
        build_scene();
        const std::string save_id = get_avatar().get_save_id();
        std::printf( "sim: game started, scene built\n" );
        fp3d::set_before_ask_hook( observe );
        while( !do_turn() ) {
        }
        if( g->uquit != QUIT_SAVED ) {
            throw std::runtime_error( "game ended without saving" );
        }
        // Back to the game the way the main menu loads one.
        std::printf( "sim: saved and quit, reloading\n" );
        g->setup();
        if( !fp3d_game_access::load_game( *g, save_t::from_save_id( save_id ) ) ) {
            throw std::runtime_error( "load failed" );
        }
        loads++;
        fp3d::reset_snapshots();
        while( !do_turn() ) {
        }
        sim_error = "game ended";
    } catch( const fp3d::shutdown_signal & ) {
        // normal end: the driver shut the bridge down
    } catch( const std::exception &e ) {
        sim_error = e.what();
    }
    fp3d::shutdown();
    fp3d::shutdown_world();
}

#if defined(_WIN32)
LONG WINAPI on_fatal_exception( EXCEPTION_POINTERS *info )
{
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if( code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO ) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    std::fprintf( stderr, "FATAL: exception 0x%08lx at %p\n", static_cast<unsigned long>( code ),
                  info->ExceptionRecord->ExceptionAddress );
    debug_write_backtrace( std::cerr );
    std::cerr.flush();
    std::_Exit( 3 );
}
#endif

bool has( const std::vector<std::string> &v, const std::string &s )
{
    return std::find( v.begin(), v.end(), s ) != v.end();
}

int first_entry_with( const fp3d::ui_request &req, const std::string &needle, std::string &label )
{
    for( const fp3d::ui_entry &e : req.entries ) {
        if( e.enabled && e.label.find( needle ) != std::string::npos ) {
            label = e.label;
            return e.id;
        }
    }
    label.clear();
    return -1;
}

struct driver_state {
    int64_t issue_turn = 0;
    bool looked = false;
};

struct goal {
    const char *action;        // main-loop action that starts it; nullptr = the real-time script
    const char *what;
    int dx;                    // direction answer, or target square relative to the prompt's origin
    int dy;
    const char *target_action; // action for target requests ("" = just pick the square)
    const char *item;          // entry to pick in item requests
    int menu_entry;            // entry to pick in menus (-1 = cancel)
    bool ( *done )( const observed &, const driver_state & );
};

const goal goals[] = {
    {
        "open", "open the door east", 1, 0, "", "", -1,
        []( const observed & o, const driver_state & ) { return o.east_ter == "t_door_o"; }
    },
    {
        "pickup", "pick up the apple", 0, 0, "", "apple", -1,
        []( const observed & o, const driver_state & ) { return o.apples_here == 0 && o.apples_carried == 1; }
    },
    {
        "eat", "eat the apple", 0, 0, "", "apple", -1,
        []( const observed & o, const driver_state & ) { return o.apples_carried == 0 && o.apples_here == 0; }
    },
    {
        "look", "look 2 squares east", 2, 0, "", "", -1,
        []( const observed &, const driver_state & d ) { return d.looked; }
    },
    {
        "throw", "throw the rock west", -3, 0, "FIRE", "rock", -1,
        []( const observed & o, const driver_state & ) { return o.rocks_carried == 0 && o.rocks_west == 1; }
    },
    {
        // wait() menu entry 3 = 5 minutes
        "wait", "wait 5 minutes", 0, 0, "", "", 3,
        []( const observed & o, const driver_state & d ) { return o.turn >= d.issue_turn + 300; }
    },
    { nullptr, "real time + auto-pause", 0, 0, "", "", -1, nullptr },
    {
        "save", "save, quit and reload", 0, 0, "", "", -1,
        []( const observed & o, const driver_state & ) {
            return o.loads == 1 && o.east_ter == "t_door_o" && o.apples_here + o.apples_carried == 0 &&
                   o.rocks_west == 1;
        }
    },
};

double ms_since( std::chrono::steady_clock::time_point t )
{
    return std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t ).count();
}

// Real time at 4x (250 ms turns): three unanswered main-loop requests must each expire after about a
// turn and advance the game one turn. Then a zombie comes into view: the clock must auto-pause and stay
// paused until the client resumes it.
bool realtime_checks( fp3d::ui_request &req )
{
    bool ok = true;
    fp3d::set_timescale( 4.0 );
    fp3d::set_clock_mode( fp3d::clock_mode::real_time );
    std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    for( int i = 0; i < 3; i++ ) {
        const std::optional<fp3d::ui_request> next = fp3d::wait_request( 3000, req.id );
        const double ms = ms_since( t );
        t = std::chrono::steady_clock::now();
        if( !next || !next->timed ) {
            std::printf( "    real time: no main-loop request after %.0f ms\n", ms );
            return false;
        }
        const bool step_ok = next->turn > req.turn && next->turn <= req.turn + 2 && ms >= 150 && ms <= 1000;
        std::printf( "    real time: turn %lld -> %lld after %.0f ms %s\n", static_cast<long long>( req.turn ),
                     static_cast<long long>( next->turn ), ms, step_ok ? "ok" : "BAD" );
        ok = ok && step_ok;
        req = *next;
    }

    spawn_zombie = true; // placed by the sim right before its next request
    t = std::chrono::steady_clock::now();
    while( !fp3d::get_clock().paused && ms_since( t ) < 3000 ) {
        if( const std::optional<fp3d::ui_request> next = fp3d::wait_request( 50, req.id ) ) {
            req = *next;
        }
    }
    const fp3d::clock_state paused = fp3d::get_clock();
    const observed o = snapshot();
    std::printf( "    auto-pause: %s (%s) after %.0f ms; zombies placed %d, creatures seen %d, safe mode %d\n",
                 paused.paused ? "paused" : "NOT paused", paused.pause_reason.c_str(), ms_since( t ),
                 o.zombies_placed, o.visible, o.safe_mode );
    ok = ok && paused.paused && paused.pause_reason == "hostile spotted";

    // While paused nothing may advance: at most one more request (posted already paused) and no turn.
    const int64_t turn_at_pause = req.turn;
    int while_paused = 0;
    t = std::chrono::steady_clock::now();
    while( ms_since( t ) < 750 ) {
        if( const std::optional<fp3d::ui_request> next = fp3d::wait_request( 50, req.id ) ) {
            while_paused++;
            ok = ok && next->paused && next->turn <= turn_at_pause + 1;
            req = *next;
        }
    }
    std::printf( "    paused for 750 ms: %d new requests, turn %lld -> %lld\n", while_paused,
                 static_cast<long long>( turn_at_pause ), static_cast<long long>( req.turn ) );
    ok = ok && while_paused <= 1;

    fp3d::set_paused( false );
    const std::optional<fp3d::ui_request> resumed = fp3d::wait_request( 3000, req.id );
    std::printf( "    resumed: %s\n", resumed && resumed->turn > turn_at_pause ? "turns advance" : "STUCK" );
    return ok && resumed && resumed->turn > turn_at_pause;
}

// Runs the checks, then always leaves the clock turn-based and running with `req` = the newest pending
// request (one may have expired before the switch; now it no longer expires).
bool run_realtime( fp3d::ui_request &req )
{
    const bool ok = realtime_checks( req );
    fp3d::set_clock_mode( fp3d::clock_mode::turn_based );
    fp3d::set_paused( false );
    const std::optional<fp3d::ui_request> pending = fp3d::wait_request( 3000 );
    if( !pending ) {
        return false;
    }
    req = *pending;
    return ok;
}

// Answers a request that isn't the main loop's, following the current goal.
fp3d::ui_answer answer_for( const fp3d::ui_request &req, const goal &cur, driver_state &d, std::string &note )
{
    fp3d::ui_answer a;
    a.id = req.id;
    switch( req.kind ) {
        case fp3d::request_kind::key:
            a.cancel = true;
            note = "cancel";
            break;
        case fp3d::request_kind::direction:
            a.pos = fp3d::ui_point{ cur.dx, cur.dy, 0 };
            note = "(" + std::to_string( cur.dx ) + "," + std::to_string( cur.dy ) + ")";
            break;
        case fp3d::request_kind::target:
            a.pos = fp3d::ui_point{ req.origin.x + cur.dx, req.origin.y + cur.dy, req.origin.z };
            a.action = cur.target_action;
            d.looked = d.looked || req.mode == "look";
            note = req.mode + " at origin+(" + std::to_string( cur.dx ) + "," + std::to_string( cur.dy ) + ") " +
                   a.action;
            break;
        case fp3d::request_kind::popup:
            a.action = has( req.actions, "YES" ) ? "YES" : req.actions.front();
            note = a.action;
            break;
        case fp3d::request_kind::item_select:
        case fp3d::request_kind::item_multi: {
            const int id = first_entry_with( req, cur.item, note );
            a.entry = id;
            a.cancel = id < 0;
            if( id >= 0 ) {
                a.picks.emplace_back( id, 0 );
            }
            break;
        }
        case fp3d::request_kind::menu:
            // Characters with an alarm clock get "Set an alarm?" before the wait menu.
            a.entry = first_entry_with( req, "Wait a while", note ) >= 0 ? 0 : cur.menu_entry;
            a.cancel = cur.menu_entry < 0;
            note = a.cancel ? "cancel" : "entry " + std::to_string( a.entry );
            break;
        case fp3d::request_kind::string:
            a.cancel = true;
            note = "cancel";
            break;
    }
    return a;
}

void print_request( const fp3d::ui_request &req, const std::string &note )
{
    std::printf( "[%llu] %-11s ctx=%-12s actions=%zu entries=%zu cand=%zu -> %s\n",
                 static_cast<unsigned long long>( req.id ), fp3d::kind_name( req.kind ), req.context.c_str(),
                 req.actions.size(), req.entries.size(), req.candidates.size(), note.c_str() );
    if( !req.text.empty() ) {
        std::printf( "    text: %.120s\n", req.text.c_str() );
    }
}
} // namespace

// Block B5: actions queued instead of answered. Starts with the main loop waiting on a timed request.
// Hand attacks (fp3d/hands.h): the off hand strikes unarmed even with a weapon wielded, the main hand uses
// the weapon, nobody there or a two-handed weapon means no attack, and a made-up hand is refused.
static bool run_hand_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  hands: %-36s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    // Queues one action, waits for the main loop again, returns the events in between.
    const auto act = [&]( const std::string &a, std::vector<fp3d::event> &evs ) {
        evs.clear();
        const std::optional<fp3d::ui_request> r0 = fp3d::wait_request( 5000 );
        if( !r0 || !r0->timed ) {
            return false;
        }
        fp3d::drain_events( evs );
        evs.clear();
        fp3d::queue_action( a );
        const std::optional<fp3d::ui_request> r1 = fp3d::wait_request( 20000, r0->id );
        fp3d::drain_events( evs );
        return r1 && r1->timed;
    };
    const auto player_attacks = []( const std::vector<fp3d::event> &evs, bool unarmed ) {
        int n = 0;
        for( const fp3d::event &e : evs ) {
            if( e.kind == fp3d::event_kind::attack && e.uid == 1 && ( e.type == 0 ) == unarmed ) {
                n++;
            }
        }
        return n;
    };
    const auto told = []( const std::vector<fp3d::event> &evs, const std::string &text ) {
        return std::any_of( evs.begin(), evs.end(), [&text]( const fp3d::event & e ) {
            return e.kind == fp3d::event_kind::message && e.text.find( text ) != std::string::npos;
        } );
    };
    std::vector<fp3d::event> evs;
    hand_setup = 1;
    check( act( "pause", evs ), "scene: zombie west, rock in hand", "" );
    check( act( "fp3d_attack_off_LEFT", evs ) && player_attacks( evs, true ) == 1 &&
           player_attacks( evs, false ) == 0, "off hand strikes unarmed", "" );
    check( act( "fp3d_attack_main_LEFT", evs ) && player_attacks( evs, false ) == 1 &&
           player_attacks( evs, true ) == 0, "main hand uses the rock", "" );
    check( act( "fp3d_attack_off_UP", evs ) && player_attacks( evs, true ) + player_attacks( evs, false ) == 0 &&
           told( evs, "Nothing there to hit" ), "nobody there: no attack", "" );
    hand_setup = 2;
    act( "pause", evs );
    check( act( "fp3d_attack_off_LEFT", evs ) && player_attacks( evs, true ) + player_attacks( evs, false ) == 0 &&
           told( evs, "Both your hands" ), "two-handed weapon: no off hand", "" );
    check( act( "fp3d_attack_foot_LEFT", evs ) && told( evs, "Can't do that here" ), "made-up hand refused", "" );
    return failed == 0;
}

// Held items and context use (fp3d/held.h): pick a jerrycan for the off hand through the hold prompt, then use
// the wielded hose on the vehicle north: it siphons into the jerrycan with no menu, one turn per second, and
// the held view shows the jerrycan filling. Then: the off hand strikes with the jerrycan, a held item dropped and
// picked up isn't held again, the wielded jerrycan refuels the vehicle, and E fills it from a puddle and from water.
static bool run_siphon_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  siphon: %-35s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    hand_setup = 3;
    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    fp3d::queue_action( "pause" );   // the setup runs before the next request
    r = fp3d::wait_request( 20000, r->id );
    const observed before = snapshot();
    check( r && r->timed && before.can_ml == 0 && before.tank_ml == 4000, "scene: hose, jerrycan, 4 L tank",
           "can " + std::to_string( before.can_ml ) + " ml, tank " + std::to_string( before.tank_ml ) + " ml" );
    if( !r ) {
        return false;
    }
    // Hold: CDDA's item menu comes up; pick the jerrycan.
    fp3d::queue_action( "fp3d_hold_off" );
    std::optional<fp3d::ui_request> menu = fp3d::wait_request( 20000, r->id );
    std::string label;
    const int entry = menu ? first_entry_with( *menu, "jerrycan", label ) : -1;
    check( menu && !menu->timed && entry >= 0, "hold asks which item", label );
    if( !menu || menu->timed || entry < 0 ) {
        return false;
    }
    fp3d::ui_answer pick;
    pick.id = menu->id;
    pick.entry = entry;
    fp3d::answer( pick );
    r = fp3d::wait_request( 20000, menu->id );
    fp3d::held_view h = fp3d::latest_held();
    check( r && r->timed && h.main.id == "hose" && h.off.id == "jerrycan" && h.off.capacity_ml >= 10000,
           "hose in the main hand, jerrycan in the off", h.main.id + " / " + h.off.id + " " +
           std::to_string( h.off.capacity_ml ) + " ml" );
    if( !r ) {
        return false;
    }
    // Use the hose on the vehicle north: an activity runs (no request) until the tank is empty.
    const auto t0 = std::chrono::steady_clock::now();
    fp3d::queue_action( "fp3d_use_UP" );
    std::vector<int64_t> levels;
    std::optional<fp3d::ui_request> done;
    while( !done && std::chrono::steady_clock::now() - t0 < std::chrono::seconds( 40 ) ) {
        done = fp3d::wait_request( 200, r->id );
        h = fp3d::latest_held();
        if( h.activity == "ACT_FILL_LIQUID" && ( levels.empty() || levels.back() != h.off.ml ) ) {
            levels.push_back( h.off.ml );
        }
    }
    const double secs = std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count();
    const observed after = snapshot();
    h = fp3d::latest_held();
    check( done && done->timed, "back in the main loop", "" );
    check( after.can_ml == 4000 && after.tank_ml == 0, "4 L moved into the jerrycan",
           "can " + std::to_string( after.can_ml ) + " ml, tank " + std::to_string( after.tank_ml ) + " ml" );
    check( h.off.ml == 4000 && h.off.liquid == "gasoline", "held view: jerrycan with gasoline",
           std::to_string( h.off.ml ) + " ml " + h.off.liquid );
    std::string seen;
    for( const int64_t v : levels ) {
        seen += std::to_string( v ) + " ";
    }
    check( levels.size() >= 3, "fill seen rising turn by turn", seen );
    // One turn per 1 s / timescale (the real-time goal left the clock running faster).
    const double turn_s = 1.0 / fp3d::get_clock().timescale;
    check( secs >= 4 * turn_s, "paced in real time", std::to_string( secs ) + " s, " +
           std::to_string( turn_s ) + " s per turn" );
    // Nothing left to siphon: a message, no time.
    fp3d::queue_action( "fp3d_use_UP" );
    std::vector<fp3d::event> evs;
    const std::optional<fp3d::ui_request> again = fp3d::wait_request( 20000, done ? done->id : 0 );
    fp3d::drain_events( evs );
    const bool told = std::any_of( evs.begin(), evs.end(), []( const fp3d::event & e ) {
        return e.kind == fp3d::event_kind::message && e.text.find( "no liquid left" ) != std::string::npos;
    } );
    check( again && again->timed && told, "empty tank: told so", "" );
    // Queues one action and waits (activities included) for the main loop again; the events in between.
    const auto act = [&]( const std::string &a, std::vector<fp3d::event> &evs ) {
        evs.clear();
        const std::optional<fp3d::ui_request> r0 = fp3d::wait_request( 5000 );
        if( !r0 || !r0->timed ) {
            return false;
        }
        fp3d::drain_events( evs );
        evs.clear();
        fp3d::queue_action( a );
        const std::optional<fp3d::ui_request> r1 = fp3d::wait_request( 40000, r0->id );
        fp3d::drain_events( evs );
        return r1 && r1->timed;
    };
    const auto said = []( const std::vector<fp3d::event> &evs, const std::string &text ) {
        return std::any_of( evs.begin(), evs.end(), [&text]( const fp3d::event & e ) {
            return e.kind == fp3d::event_kind::message && e.text.find( text ) != std::string::npos;
        } );
    };
    const auto hold = [&]( const std::string &what ) {
        const std::optional<fp3d::ui_request> r0 = fp3d::wait_request( 5000 );
        if( !r0 || !r0->timed ) {
            return false;
        }
        fp3d::queue_action( "fp3d_hold_off" );
        const std::optional<fp3d::ui_request> m = fp3d::wait_request( 20000, r0->id );
        std::string l;
        const int e = m ? first_entry_with( *m, what, l ) : -1;
        if( !m || m->timed || e < 0 ) {
            return false;
        }
        fp3d::ui_answer a;
        a.id = m->id;
        a.entry = e;
        fp3d::answer( a );
        const std::optional<fp3d::ui_request> r1 = fp3d::wait_request( 20000, m->id );
        return r1 && r1->timed && fp3d::latest_held().off.id == what;
    };
    const auto last_msg = []( const std::vector<fp3d::event> &evs ) {
        std::string m;
        for( const fp3d::event &e : evs ) {
            if( e.kind == fp3d::event_kind::message ) {
                m = e.text;
            }
        }
        return "[" + m + "]";
    };
    const auto ml = []( const char *what, int v ) {
        return std::string( what ) + " " + std::to_string( v ) + " ml ";
    };
    // The off hand strikes with what it holds.
    std::vector<fp3d::event> ev;
    hand_setup = 4;
    act( "pause", ev );
    const bool struck = act( "fp3d_attack_off_LEFT", ev );
    const bool armed = std::any_of( ev.begin(), ev.end(), []( const fp3d::event & e ) {
        return e.kind == fp3d::event_kind::attack && e.uid == 1 && e.type != 0;
    } );
    check( struck && armed, "off hand strikes with the jerrycan", "" );
    // Put the jerrycan away.
    check( act( "fp3d_stow_off", ev ) && !fp3d::latest_held().off.held, "stow empties the off hand",
           fp3d::latest_held().off.id );
    // 4 L of gasoline make the jerrycan too heavy for one hand (this character): not offered to hold.
    {
        const std::optional<fp3d::ui_request> r0 = fp3d::wait_request( 5000 );
        fp3d::queue_action( "fp3d_hold_off" );
        const std::optional<fp3d::ui_request> m = r0 ? fp3d::wait_request( 20000, r0->id ) : std::nullopt;
        std::string l;
        const bool menu_up = m && !m->timed;
        const bool offered = menu_up && first_entry_with( *m, "jerrycan", l ) >= 0;
        if( menu_up ) {
            fp3d::ui_answer a;
            a.id = m->id;
            a.cancel = true;
            fp3d::answer( a );
        }
        const std::optional<fp3d::ui_request> r1 = menu_up ? fp3d::wait_request( 20000, m->id ) : m;
        check( r1 && r1->timed && !offered && !fp3d::latest_held().off.held, "too heavy for one hand: not offered",
               menu_up ? "menu" : "no menu" );
    }
    // A spare hose held, dropped, picked up: carried but not held (its mark's token went stale).
    hand_setup = 5;
    act( "pause", ev );
    check( hold( "hose" ), "spare hose held", fp3d::latest_held().off.id );
    hand_setup = 6;
    act( "pause", ev );
    h = fp3d::latest_held();
    check( !h.off.held, "dropped: off hand empty", h.off.id );
    hand_setup = 7;
    act( "pause", ev );
    h = fp3d::latest_held();
    const int hoses = snapshot().hoses;
    check( !h.off.held && hoses == 2, "picked up: carried, not held", h.off.id + " hoses " +
           std::to_string( hoses ) );
    // Wielded, no hose: E pours it into the vehicle's tank.
    hand_setup = 8;
    act( "pause", ev );
    const bool poured = act( "fp3d_use_UP", ev );
    const observed refueled = snapshot();
    check( poured && refueled.tank_ml == 4000 && refueled.can_ml == 0 && said( ev, "You refill" ),
           "refuel from the held jerrycan", fp3d::latest_held().main.id + " " + ml( "can", refueled.can_ml ) +
           ml( "tank", refueled.tank_ml ) + last_msg( ev ) );
    // E fills it from the puddle south, then from the shallow water east.
    hand_setup = 9;
    act( "pause", ev );
    const bool from_puddle = act( "fp3d_use_DOWN", ev );
    const observed puddled = snapshot();
    h = fp3d::latest_held();
    check( from_puddle && puddled.puddle == 0 && puddled.can_ml == 2000 && h.main.liquid == "water",
           "fill from a puddle", ml( "can", puddled.can_ml ) + h.main.liquid + " puddle " +
           std::to_string( puddled.puddle ) );
    // The puddle's water doesn't combine with the river's (CDDA: dirty, other temperature): told so, no menu.
    // A new jerrycan fills up.
    check( act( "fp3d_use_RIGHT", ev ) && said( ev, "won't hold" ), "waters that don't mix: told so",
           last_msg( ev ) );
    hand_setup = 10;
    act( "pause", ev );
    const bool from_water = act( "fp3d_use_RIGHT", ev );
    const observed filled = snapshot();
    h = fp3d::latest_held();
    check( from_water && filled.can_ml >= 10000 && filled.can_ml == h.main.capacity_ml, "fill from shallow water",
           ml( "can", filled.can_ml ) + ml( "of", static_cast<int>( h.main.capacity_ml ) ) + last_msg( ev ) +
           ( from_water ? "" : " stuck" ) );
    return failed == 0;
}

// Shooting a zombie with a loaded pistol: "fire" opens CDDA's target_ui, which asks the client; FIRE at the
// zombie shoots once (it crashed here: aim modes unset headless), then CDDA aims again for the next shot;
// cancelling that goes back to the main loop.
static bool run_shoot_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  shoot: %-36s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    hand_setup = 11;
    fp3d::queue_action( "pause" );
    r = fp3d::wait_request( 20000, r->id );
    const observed before = snapshot();
    check( r && r->timed && before.ammo == 15 && before.zombie_hp > 0, "scene: loaded pistol, zombie west",
           "ammo " + std::to_string( before.ammo ) + " hp " + std::to_string( before.zombie_hp ) );
    if( !r ) {
        return false;
    }
    fp3d::queue_action( "fire" );
    int targets = 0;
    int others = 0;
    const auto t0 = std::chrono::steady_clock::now();
    std::optional<fp3d::ui_request> cur = fp3d::wait_request( 20000, r->id );
    while( cur && !cur->timed && targets < 20 && others < 20 &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds( 60 ) ) {
        fp3d::ui_answer a;
        a.id = cur->id;
        std::string note;
        if( cur->kind == fp3d::request_kind::target && targets++ == 0 ) {
            a.pos = fp3d::ui_point{ cur->origin.x - 3, cur->origin.y, cur->origin.z };
            a.action = "FIRE";
            note = cur->mode + " " + cur->status + " -> zombie FIRE";
        } else if( cur->kind == fp3d::request_kind::target ) {
            a.cancel = true;
            note = cur->mode + " again -> cancel";
        } else if( cur->kind == fp3d::request_kind::popup && !cur->actions.empty() ) {
            others++;
            a.action = has( cur->actions, "YES" ) ? "YES" : cur->actions.front();
            note = a.action + " (" + cur->text.substr( 0, 60 ) + ")";
        } else {
            others++;
            a.cancel = true;
            note = "cancel (" + cur->text.substr( 0, 60 ) + ")";
        }
        print_request( *cur, note );
        fp3d::answer( a );
        const auto id = cur->id;
        cur = fp3d::wait_request( 20000, id );
    }
    const observed after = snapshot();
    check( cur && cur->timed, "back in the main loop", std::to_string( targets ) + " target, " +
           std::to_string( others ) + " other requests" + ( cur ? "" : ", no request (stuck or crashed)" ) );
    check( targets == 2 && others == 0, "aim, fire, aim again, cancel", std::to_string( targets ) +
           " target requests" );
    check( after.ammo == before.ammo - 1, "one shot fired", "ammo " + std::to_string( before.ammo ) + " -> " +
           std::to_string( after.ammo ) );
    std::printf( "  shoot: zombie hp %d -> %d\n", before.zombie_hp, after.zombie_hp );

    // Client-flown shots (fp3d/shot.h): a planned miss leaves the zombie as it was, a planned head hit
    // always hurts it. Pistol reloaded, zombie restored each time (hand_setup 11).
    const auto client_shot = [&]( bool hit ) {
        hand_setup = 11;
        std::optional<fp3d::ui_request> q = fp3d::wait_request( 5000 );
        if( !q || !q->timed ) {
            return std::make_pair( -1, -1 );
        }
        fp3d::queue_action( "pause" );
        q = fp3d::wait_request( 20000, q->id );
        const observed b = snapshot();
        if( !q ) {
            return std::make_pair( -1, -1 );
        }
        fp3d::queue_action( "fire" );
        std::optional<fp3d::ui_request> c = fp3d::wait_request( 20000, q->id );
        int n = 0;
        while( c && !c->timed && n++ < 20 ) {
            fp3d::ui_answer a;
            a.id = c->id;
            if( c->kind == fp3d::request_kind::target && n == 1 ) {
                fp3d::shot_answer s;
                // a miss stops 2 squares north of the zombie; a hit is in its head
                s.end = fp3d::ui_point{ c->origin.x - 3, c->origin.y - ( hit ? 0 : 2 ), c->origin.z };
                s.hit_uid = hit ? b.zombie_uid : 0;
                s.part = "head";
                s.quality = 0.1f;
                a.pos = s.end;
                a.action = "FIRE";
                a.shot = s;
            } else {
                a.cancel = true;
            }
            fp3d::answer( a );
            const auto id = c->id;
            c = fp3d::wait_request( 20000, id );
        }
        return std::make_pair( b.zombie_hp, snapshot().zombie_hp );
    };
    const auto miss = client_shot( false );
    check( miss.first > 0 && miss.second == miss.first, "client shot: planned miss",
           "hp " + std::to_string( miss.first ) + " -> " + std::to_string( miss.second ) );
    const auto head = client_shot( true );
    check( head.first > 0 && head.second < head.first, "client shot: planned head hit",
           "hp " + std::to_string( head.first ) + " -> " + std::to_string( head.second ) );
    return failed == 0;
}

// The handles listed on absolute square `p` in the newest snapshot.
static std::vector<fp3d::item_handle_view> handles_at( const fp3d::ui_point &p )
{
    const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
    if( !s ) {
        return {};
    }
    const auto floor_div = []( int a ) {
        return a >= 0 ? a / fp3d::sm_size : ( a - fp3d::sm_size + 1 ) / fp3d::sm_size;
    };
    const int sx = floor_div( p.x );
    const int sy = floor_div( p.y );
    const int tile = ( p.y - sy * fp3d::sm_size ) * fp3d::sm_size + ( p.x - sx * fp3d::sm_size );
    for( const std::shared_ptr<const fp3d::submap_view> &sm : s->submaps ) {
        if( sm->pos.x != sx || sm->pos.y != sy || sm->pos.z != p.z ) {
            continue;
        }
        for( const fp3d::item_pile_view &pile : sm->items ) {
            if( pile.tile == tile ) {
                return pile.handles;
            }
        }
    }
    return {};
}

static std::vector<fp3d::carried_view> carried_now()
{
    const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
    return s ? s->player.items : std::vector<fp3d::carried_view>();
}

// Single items by handle (fp3d/item_hands.h): take, put and drop with no menus, CDDA's rules and move costs, and
// refusals that say why.
static bool run_item_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  items: %-40s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    struct result {
        bool queued = false;
        bool back = false;       // in the main loop again
        int spent = 0;           // moves
        std::string msg;         // the last message
    };
    const auto run = [&]( const fp3d::item_action & ia ) {
        result res;
        std::vector<fp3d::event> evs;
        const std::optional<fp3d::ui_request> r0 = fp3d::wait_request( 5000 );
        if( !r0 || !r0->timed ) {
            return res;
        }
        const observed o0 = snapshot();
        fp3d::drain_events( evs );
        evs.clear();
        res.queued = fp3d::queue_item_action( ia );
        if( !res.queued ) {
            return res;
        }
        const std::optional<fp3d::ui_request> r1 = fp3d::wait_request( 40000, r0->id );
        fp3d::drain_events( evs );
        const observed o1 = snapshot();
        res.back = r1 && r1->timed;
        res.spent = static_cast<int>( o1.turn - o0.turn ) * o0.speed + o0.moves - o1.moves;
        for( const fp3d::event &e : evs ) {
            if( e.kind == fp3d::event_kind::message ) {
                res.msg = e.text;
            }
        }
        return res;
    };
    const auto has = []( const std::string &s, const char *part ) {
        return s.find( part ) != std::string::npos;
    };
    const auto show = []( const result & r ) {
        return "[" + r.msg + "] " + std::to_string( r.spent ) + " moves";
    };
    const auto type_of = []( const char *id ) {
        return fp3d::intern_id( fp3d::id_kind::itype, id );
    };
    const auto find_type = []( const std::vector<fp3d::item_handle_view> &hs, uint32_t type, size_t nth ) {
        for( const fp3d::item_handle_view &h : hs ) {
            if( h.type == type && nth-- == 0 ) {
                return h;
            }
        }
        return fp3d::item_handle_view();
    };
    const auto carried = []( uint64_t uid ) {
        for( const fp3d::carried_view &c : carried_now() ) {
            if( c.uid == uid ) {
                return c;
            }
        }
        return fp3d::carried_view();
    };
    const auto carried_type = []( uint32_t type, uint8_t flags ) {
        for( const fp3d::carried_view &c : carried_now() ) {
            if( c.type == type && ( c.flags & flags ) == flags ) {
                return c;
            }
        }
        return fp3d::carried_view();
    };
    const auto on = [&]( const fp3d::ui_point & p, uint64_t uid ) {
        for( const fp3d::item_handle_view &h : handles_at( p ) ) {
            if( h.uid == uid ) {
                return h;
            }
        }
        return fp3d::item_handle_view();
    };

    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    hand_setup = 12;
    fp3d::queue_action( "pause" );
    r = fp3d::wait_request( 20000, r->id );
    const std::shared_ptr<const fp3d::snapshot> s0 = fp3d::latest_snapshot();
    if( !r || !r->timed || !s0 ) {
        check( false, "scene", "" );
        return false;
    }
    const fp3d::ui_point here = s0->player.pos;
    const fp3d::ui_point west{ here.x - 1, here.y, here.z };
    const fp3d::ui_point far_west{ here.x - 3, here.y, here.z };
    const fp3d::ui_point north_west{ here.x - 1, here.y - 1, here.z };
    const uint32_t t_rock = type_of( "rock" );
    const uint32_t t_bottle = type_of( "bottle_plastic" );
    const uint32_t t_bag = type_of( "bag_plastic" );
    const uint32_t t_gold = type_of( "1l_gold" );
    const uint32_t t_water = type_of( "water" );
    const uint32_t t_backpack = type_of( "backpack" );

    // Every item on a square gets its own handle; uids read back from a save are checked, not trusted.
    const std::vector<fp3d::item_handle_view> w0 = handles_at( west );
    const fp3d::item_handle_view rock_a = find_type( w0, t_rock, 0 );
    const fp3d::item_handle_view rock_b = find_type( w0, t_rock, 1 );
    const fp3d::item_handle_view bottle = find_type( w0, t_bottle, 0 );
    const fp3d::item_handle_view bag = find_type( w0, t_bag, 0 );
    check( w0.size() == 4 && rock_a.uid != 0 && rock_b.uid != 0 && rock_a.uid != rock_b.uid && bottle.uid != 0 &&
           bag.uid != 0 && rock_a.spot_x == -1, "a handle per item on a square", std::to_string( w0.size() ) +
           " handles" );
    const std::vector<fp3d::item_handle_view> nw = handles_at( north_west );
    check( nw.size() == 1 && nw[0].uid != 0 && nw[0].uid != 12 && nw[0].spot_x == -1 && nw[0].spot_y == -1,
           "a save's malformed handle and spot", nw.empty() ? "none" : std::to_string( nw[0].uid ) );
    const fp3d::carried_view gold = carried_type( t_gold, 0 );
    const fp3d::carried_view water = carried_type( t_water, 0 );
    const fp3d::carried_view backpack = carried_type( t_backpack, fp3d::carried_worn );
    const fp3d::carried_view water_bottle = carried( water.parent );
    check( gold.uid != 0 && water.uid != 0 && water_bottle.type == t_bottle && water.pocket == 0 &&
           backpack.uid != 0 && backpack.parent == 0, "carried items listed with their containers",
           std::to_string( carried_now().size() ) + " carried" );

    // The bridge refuses malformed requests before they reach the game.
    fp3d::item_action bad;
    bad.op = fp3d::item_op::take;
    bad.item = { rock_a.uid, t_rock };
    bad.pos = west;
    fp3d::item_action spot_bad = bad;
    spot_bad.op = fp3d::item_op::drop;
    spot_bad.has_spot = true;
    spot_bad.spot_x = 1000;
    fp3d::item_action uid_zero = bad;
    uid_zero.item.uid = 0;
    fp3d::item_action uid_big = bad;
    uid_big.item.uid = fp3d::max_item_uid + 1;
    fp3d::item_action no_container = bad;
    no_container.op = fp3d::item_op::put;
    fp3d::item_action pocket_big = bad;
    pocket_big.op = fp3d::item_op::put;
    pocket_big.container = { bag.uid, t_bag };
    pocket_big.pocket = fp3d::max_item_pocket + 1;
    fp3d::item_action far_pos = bad;
    far_pos.pos.x = 1 << 30;
    check( !fp3d::queue_item_action( spot_bad ) && !fp3d::queue_item_action( uid_zero ) &&
           !fp3d::queue_item_action( uid_big ) && !fp3d::queue_item_action( no_container ) &&
           !fp3d::queue_item_action( pocket_big ) && !fp3d::queue_item_action( far_pos ),
           "malformed requests refused by the bridge", "" );

    // Take: out of reach, into the pack, the same handle again, into each hand.
    fp3d::item_action take;
    take.op = fp3d::item_op::take;
    const std::vector<fp3d::item_handle_view> distant = handles_at( far_west );
    take.item = { distant.empty() ? rock_a.uid : distant[0].uid, t_rock };
    take.pos = far_west;
    result res = run( take );
    check( res.back && res.spent == 0 && has( res.msg, "can't reach" ), "take: out of reach", show( res ) );
    take.item = { rock_a.uid, rock_a.type };
    take.pos = west;
    res = run( take );
    check( res.back && res.spent > 0 && has( res.msg, "pick up" ) && carried( rock_a.uid ).uid != 0 &&
           on( west, rock_a.uid ).uid == 0, "take into the pack", show( res ) );
    res = run( take );
    check( res.back && res.spent == 0 && has( res.msg, "isn't there" ), "take: stale handle", show( res ) );
    take.item = { bottle.uid, bottle.type };
    take.hand = fp3d::item_hand::main;
    res = run( take );
    check( res.back && res.spent > 0 && fp3d::latest_held().main.id == "bottle_plastic" &&
           ( carried( bottle.uid ).flags & fp3d::carried_wielded ), "take into the main hand", show( res ) );
    take.item = { bag.uid, bag.type };
    res = run( take );
    check( res.back && res.spent == 0 && has( res.msg, "hand is full" ), "take: main hand full", show( res ) );
    take.item = { rock_b.uid, rock_b.type };
    take.hand = fp3d::item_hand::off;
    res = run( take );
    check( res.back && res.spent > 0 && fp3d::latest_held().off.id == "rock" &&
           ( carried( rock_b.uid ).flags & fp3d::carried_offhand ), "take into the off hand", show( res ) );

    // Put: CDDA's pocket rules say no.
    fp3d::item_action put;
    put.op = fp3d::item_op::put;
    put.item = { rock_a.uid, rock_a.type };
    put.container = { bottle.uid, bottle.type };
    put.pos = here;
    res = run( put );
    check( res.back && res.spent == 0 && has( res.msg, "too big" ), "put: too big (rock into a bottle)",
           show( res ) );
    put.item = { gold.uid, gold.type };
    put.container = { bag.uid, bag.type };
    put.pos = west;
    res = run( put );
    check( res.back && res.spent == 0 && has( res.msg, "too heavy" ), "put: too heavy (gold into a bag)",
           show( res ) );
    put.item = { water.uid, water.type };
    put.container = { backpack.uid, backpack.type };
    res = run( put );
    check( res.back && res.spent == 0 && has( res.msg, "liquid" ), "put: not watertight (water into a pack)",
           show( res ) );
    put.item = { rock_a.uid, rock_a.type };
    put.container = { 0x5eed, bag.type };
    res = run( put );
    check( res.back && res.spent == 0 && has( res.msg, "isn't there" ), "put: stale container handle",
           show( res ) );
    // Put: into a bag on the ground, into a pack pocket by index (from the off hand), into the held bottle.
    put.container = { bag.uid, bag.type };
    res = run( put );
    check( res.back && res.spent > 0 && snapshot().bag_rocks == 1 && carried( rock_a.uid ).uid == 0,
           "put into a bag on the ground", show( res ) );
    put.item = { rock_b.uid, rock_b.type };
    put.container = { backpack.uid, backpack.type };
    put.pocket = 0;
    res = run( put );
    const fp3d::carried_view in_pack = carried( rock_b.uid );
    check( res.back && res.spent > 0 && in_pack.parent == backpack.uid && in_pack.pocket == 0 &&
           !fp3d::latest_held().off.held, "put into a worn pack's pocket 0", show( res ) );
    put.pocket = -1;
    put.item = { water.uid, water.type };
    put.container = { bottle.uid, bottle.type };
    res = run( put );
    check( res.back && res.spent > 0 && carried( water.uid ).parent == bottle.uid, "put water into the held bottle",
           show( res ) );

    // Drop: placed with a spot (read back from the snapshot), dropped without one.
    fp3d::item_action drop;
    drop.op = fp3d::item_op::drop;
    drop.item = { bottle.uid, bottle.type };
    drop.pos = west;
    drop.has_spot = true;
    drop.spot_x = 250;
    drop.spot_y = 750;
    res = run( drop );
    const fp3d::item_handle_view placed = on( west, bottle.uid );
    check( res.back && res.spent > 0 && placed.spot_x == 250 && placed.spot_y == 750 &&
           !fp3d::latest_held().main.held, "drop: placed on a spot", show( res ) );
    drop.item = { gold.uid, gold.type };
    drop.pos = here;
    drop.has_spot = false;
    res = run( drop );
    const fp3d::item_handle_view dropped = on( here, gold.uid );
    check( res.back && res.spent > 0 && dropped.uid == gold.uid && dropped.spot_x == -1, "drop: no spot",
           show( res ) );
    drop.item = { backpack.uid, backpack.type };
    res = run( drop );
    check( res.back && res.spent == 0 && has( res.msg, "Take off" ), "drop: worn item refused", show( res ) );
    drop.item = { gold.uid, gold.type };
    res = run( drop );
    check( res.back && res.spent == 0 && has( res.msg, "don't have" ), "drop: not carried any more", show( res ) );

    // Sealed furniture keeps its contents from hands and eyes; closed containers show theirs only from next to
    // them; a stack put where it already is stays one stack; forbidden pockets (a corpse's) take nothing.
    r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "second scene", "" );
        return false;
    }
    hand_setup = 13;
    fp3d::queue_action( "pause" );
    r = fp3d::wait_request( 20000, r->id );
    const fp3d::ui_point south{ here.x, here.y + 1, here.z };
    const fp3d::ui_point north3{ here.x, here.y - 3, here.z };
    const fp3d::ui_point east{ here.x + 1, here.y, here.z };
    const fp3d::ui_point north{ here.x, here.y - 1, here.z };
    const std::vector<fp3d::item_handle_view> on_east = handles_at( east );
    const std::vector<fp3d::item_handle_view> typed = handles_at( north );
    check( r && typed.size() == 1 && typed[0].uid != 0 && typed[0].uid != 5 && typed[0].spot_x == -1 &&
           !fp3d::latest_held().off.held, "vars of other types read as none",
           typed.empty() ? "none" : std::to_string( typed[0].uid ) );
    check( r && sealed_items == 1 && handles_at( south ).empty() && handles_at( north3 ).empty() &&
           on_east.size() == 1, "sealed and distant container contents hidden",
           std::to_string( sealed_items ) + " in the safe" );
    take.item = { 777, t_rock };
    take.pos = south;
    take.hand = fp3d::item_hand::pack;
    res = run( take );
    check( res.back && res.spent == 0 && has( res.msg, "get at" ), "take: out of a locked gun safe", show( res ) );
    put.item = { rock_b.uid, rock_b.type };
    put.container = { 777, t_rock };
    put.pos = south;
    put.pocket = -1;
    res = run( put );
    check( res.back && res.spent == 0 && has( res.msg, "get at" ), "put: into a locked gun safe", show( res ) );
    const fp3d::carried_view rounds = carried_type( type_of( "9mm" ), 0 );
    const int before = snapshot().carried_9mm;
    put.item = { rounds.uid, rounds.type };
    put.container = { rounds.parent, carried( rounds.parent ).type };
    put.pocket = rounds.pocket;
    res = run( put );
    put.pocket = -1;
    const result again = run( put );
    const int after = snapshot().carried_9mm;
    check( rounds.uid != 0 && rounds.parent != 0 && before == 20 && after == 20 && res.spent == 0 &&
           again.spent == 0 && has( res.msg, "already" ) && has( again.msg, "already" ),
           "put: a stack where it already is", std::to_string( before ) + " -> " + std::to_string( after ) +
           " rounds " + show( res ) );
    put.item = { rock_b.uid, rock_b.type };
    put.container = { on_east.empty() ? 0 : on_east[0].uid, on_east.empty() ? 0 : on_east[0].type };
    put.pos = east;
    put.pocket = 0;
    res = run( put );
    check( res.back && res.spent == 0 && has( res.msg, "no such pocket" ) && carried( rock_b.uid ).uid != 0,
           "put: a corpse's forbidden pocket", show( res ) );
    return failed == 0;
}

static bool run_queue_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  queue: %-36s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    const std::optional<fp3d::ui_request> r0 = fp3d::wait_request( 5000 );
    if( !r0 || !r0->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    std::vector<fp3d::event> evs;
    fp3d::drain_events( evs );
    loaded_safe_mode = snapshot().safe_mode;

    // One bad action, then three turns of waiting, all without answering a request. The real-time goal's zombie
    // is still in view: CDDA's safe mode would refuse to pass time near it, but it is off in FP3D games.
    fp3d::queue_action( "no_such_action" );
    for( int i = 0; i < 3; i++ ) {
        fp3d::queue_action( "pause" );
    }
    const std::optional<fp3d::ui_request> r1 = fp3d::wait_request( 20000, r0->id );
    evs.clear();
    fp3d::drain_events( evs );
    bool told = false;
    std::string msgs;
    for( const fp3d::event &e : evs ) {
        if( e.kind == fp3d::event_kind::message ) {
            told = told || e.text.find( "no_such_action" ) != std::string::npos;
            msgs += " | " + e.text;
        }
    }
    const bool ran = r1 && r1->timed && r1->id >= r0->id + 4 && r1->turn >= r0->turn + 2 &&
                     fp3d::queued_actions() == 0;
    check( ran, "4 queued actions, no answers", r1 ? "turns +" + std::to_string( r1->turn - r0->turn ) +
           ", requests +" + std::to_string( r1->id - r0->id ) + ( ran ? "" : msgs ) : "no request" );
    check( told, "bad action dropped with a message", "" );
    if( !r1 ) {
        return false;
    }

    // Paused: the queue waits; resuming lets the sim take it.
    fp3d::set_paused( true );
    fp3d::queue_action( "pause" );
    const std::optional<fp3d::ui_request> during = fp3d::wait_request( 1000, r1->id );
    const bool held = !during && fp3d::queued_actions() == 1;
    fp3d::set_paused( false );
    const std::optional<fp3d::ui_request> r2 = fp3d::wait_request( 20000, r1->id );
    check( held && r2 && r2->timed && fp3d::queued_actions() == 0, "held while paused, taken on resume", "" );

    // Bounded: the 33rd action is refused.
    fp3d::set_paused( true );
    int accepted = 0;
    for( size_t i = 0; i <= fp3d::max_queued_actions; i++ ) {
        accepted += fp3d::queue_action( "pause" ) ? 1 : 0;
    }
    fp3d::clear_actions();
    check( accepted == static_cast<int>( fp3d::max_queued_actions ) && fp3d::queued_actions() == 0,
           "full queue refuses, clear empties", std::to_string( accepted ) + " accepted" );
    fp3d::set_paused( false );
    return failed == 0;
}

// Safe mode (2026-11-10): FP3D games start with CDDA's safe mode off, new and loaded (boot.cpp); the `safemode`
// action (Shift+1) still toggles it; auto-pause on a new hostile is its own clock setting (bridge.h).
static bool run_safemode_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  safe mode: %-36s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    check( new_game_safe_mode == SAFE_MODE_OFF, "off in a new game", std::to_string( new_game_safe_mode ) );
    check( loaded_safe_mode == SAFE_MODE_OFF, "off in a loaded game", std::to_string( loaded_safe_mode ) );
    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    // The toggle: on, then off again. The sim observes right before each request.
    fp3d::queue_action( "safemode" );
    r = fp3d::wait_request( 20000, r->id );
    const int on = snapshot().safe_mode;
    check( r && on != SAFE_MODE_OFF, "safemode action turns it on", std::to_string( on ) );
    if( !r ) {
        return false;
    }
    fp3d::queue_action( "safemode" );
    r = fp3d::wait_request( 20000, r->id );
    const int off = snapshot().safe_mode;
    check( r && off == SAFE_MODE_OFF, "and off again", std::to_string( off ) );
    if( !r ) {
        return false;
    }

    // Auto-pause turned off: a new zombie comes into view in real time and the clock runs on. Monsters are
    // cleared first (and two turns pass) so the zombie counts as newly seen.
    clear_zombies_now = true;
    for( int i = 0; i < 2 && r; i++ ) {
        fp3d::queue_action( "pause" );
        r = fp3d::wait_request( 20000, r->id );
    }
    if( !r ) {
        check( false, "turns pass after clearing monsters", "" );
        return false;
    }
    const int placed_before = snapshot().zombies_placed;
    fp3d::set_auto_pause_hostiles( false );
    fp3d::set_timescale( 4.0 );
    fp3d::set_clock_mode( fp3d::clock_mode::real_time );
    spawn_zombie = true;
    const int64_t turn0 = r->turn;
    const std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    while( ms_since( t ) < 2000 ) {
        if( const std::optional<fp3d::ui_request> next = fp3d::wait_request( 50, r->id ) ) {
            r = next;
        }
    }
    const fp3d::clock_state c = fp3d::get_clock();
    const observed o = snapshot();
    check( o.zombies_placed > placed_before && o.visible > 0, "a new zombie in view", "seen " +
           std::to_string( o.visible ) );
    check( !c.paused && r->turn >= turn0 + 3 && !c.auto_pause_hostiles, "auto-pause off: real time runs on",
           "turns +" + std::to_string( r->turn - turn0 ) + ( c.paused ? ", paused: " + c.pause_reason : "" ) );
    fp3d::set_auto_pause_hostiles( true );
    fp3d::set_clock_mode( fp3d::clock_mode::turn_based );
    fp3d::set_paused( false );
    check( fp3d::get_clock().auto_pause_hostiles, "auto-pause back on", "" );
    return failed == 0;
}

// Cheats and the test kit (fp3d/cheats.h, 2026-11-10): all off at first; names checked exactly; each one on, the
// invincible / weight / ammo ones seen working; the kit handed out; all off again.
static bool run_cheat_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  cheats: %-38s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    // The scene gives the avatar DEBUG_NIGHTVISION (dark starts), which is the night_vision cheat.
    const fp3d::cheats_view first = fp3d::latest_cheats();
    const uint32_t scene = 1U << static_cast<int>( fp3d::cheat::night_vision );
    check( first.seq > 0 && first.on == scene, "all off in a normal game (but the scene's)",
           std::to_string( first.on ) );
    bool names_ok = true;
    for( const char *bad : {
             "", "INVINCIBLE", "invincible ", " invincible", "invincible\n", "god", "unlimited_ammo2",
             "no_needs\x01"
         } ) {
        names_ok = names_ok && !fp3d::cheat_from_name( bad );
    }
    names_ok = names_ok && !fp3d::cheat_from_name( std::string( 100000, 'a' ) ) &&
               fp3d::cheat_from_name( "night_vision" ) == fp3d::cheat::night_vision;
    check( names_ok, "only exact names accepted", "" );
    check( !fp3d::queue_action( fp3d::job_action ) && !fp3d::queue_action( fp3d::save_action ) &&
           !fp3d::queue_action( fp3d::item_action_name ), "bridge's own actions can't be queued", "" );
    check( !fp3d::request_cheat( fp3d::cheat::count, true ) &&
           !fp3d::request_cheat( static_cast<fp3d::cheat>( 200 ), true ), "out-of-range cheat refused", "" );

    // All on: applied at the main loop's next request (the sim asks again right after).
    for( size_t i = 0; i < fp3d::cheat_count; i++ ) {
        fp3d::request_cheat( static_cast<fp3d::cheat>( i ), true );
    }
    r = fp3d::wait_request( 20000, r->id );
    const uint32_t all = ( 1U << fp3d::cheat_count ) - 1;
    const fp3d::cheats_view on = fp3d::latest_cheats();
    check( r && on.on == all, "all on", std::to_string( on.on ) );
    if( !r ) {
        return false;
    }

    // Probe: a 10 damage hit on the torso and an empty Glock wielded, then a turn passes (the gun refills).
    cheat_probe = true;
    fp3d::queue_action( "pause" );
    r = fp3d::wait_request( 20000, r->id );
    const observed hit = snapshot();
    if( r ) {
        fp3d::queue_action( "pause" );
        r = fp3d::wait_request( 20000, r->id );
    }
    const observed after = snapshot();
    check( hit.torso_hp_lost == 0, "invincible: a hit does nothing", std::to_string( hit.torso_hp_lost ) );
    check( hit.carry_kg >= 10000, "no weight limit", std::to_string( hit.carry_kg ) + " kg" );
    check( r && after.ammo >= 15, "unlimited ammo refills the empty Glock", std::to_string( after.ammo ) );
    if( !r ) {
        return false;
    }

    // The test kit.
    const uint64_t kits = fp3d::latest_cheats().kits_given;
    check( fp3d::request_test_kit(), "test kit asked for", "" );
    r = fp3d::wait_request( 20000, r->id );
    const observed kit = snapshot();
    check( r && fp3d::latest_cheats().kits_given == kits + 1 && kit.big_pack && kit.kit_guns >= 3,
           "test kit: big pack, 3 loaded guns", std::to_string( kit.kit_guns ) + " guns" );
    if( !r ) {
        return false;
    }

    // All off again; the probe now hurts.
    for( size_t i = 0; i < fp3d::cheat_count; i++ ) {
        fp3d::request_cheat( static_cast<fp3d::cheat>( i ), false );
    }
    r = fp3d::wait_request( 20000, r->id );
    check( r && fp3d::latest_cheats().on == 0, "all off again", std::to_string( fp3d::latest_cheats().on ) );
    if( !r ) {
        return false;
    }
    cheat_probe = true;
    fp3d::queue_action( "pause" );
    r = fp3d::wait_request( 20000, r->id );
    const observed hurt = snapshot();
    check( r && hurt.torso_hp_lost > 0 && hurt.carry_kg < 1000, "off: a hit hurts, normal weight limit",
           std::to_string( hurt.torso_hp_lost ) + " hp, " + std::to_string( hurt.carry_kg ) + " kg" );
    return failed == 0;
}

int main()
{
    std::setvbuf( stdout, nullptr, _IONBF, 0 );
#if defined(_WIN32)
    // CDDA's crash handler shows a message box; a test must print the stack and exit instead.
    AddVectoredExceptionHandler( 1, on_fatal_exception );
#endif
    test_mode = true;
    setupDebug( DebugOutput::std_err );
    rng_set_engine_seed( 42 );
    fp3d::set_active( true );
    fp3d::set_events_enabled( true );
    std::thread sim( sim_main );
    // Drained at every request like a client would; counted per kind and checked at the end.
    size_t event_counts[static_cast<size_t>( fp3d::event_kind::turn_end ) + 1] = {};
    size_t events_dropped = 0;
    uint64_t last_event_seq = 0;
    bool seq_ordered = true;
    const auto drain = [&]() {
        std::vector<fp3d::event> evs;
        events_dropped += fp3d::drain_events( evs );
        for( const fp3d::event &e : evs ) {
            event_counts[static_cast<size_t>( e.kind )]++;
            seq_ordered = seq_ordered && e.seq >= last_event_seq;
            last_event_seq = e.seq;
        }
    };

    const size_t n_goals = std::size( goals );
    size_t g_idx = 0;
    bool issued = false;
    int passed = 0;
    int requests = 0;
    driver_state d;
    while( g_idx < n_goals && requests < 400 ) {
        std::optional<fp3d::ui_request> req = fp3d::wait_request( 180000 );
        if( !req && !fp3d::is_shut_down() ) {
            // The sim is stuck outside ask() (a loop that never asks); join() would hang.
            std::printf( "FAIL: no request within 180 s; timed-out polls: %s\n", fp3d::poll_report().c_str() );
            std::_Exit( 2 );
        }
        if( !req ) {
            std::printf( "no request within the time limit (or sim stopped)\n" );
            break;
        }
        requests++;
        drain();
        if( req->timed && new_game_safe_mode < 0 ) {
            new_game_safe_mode = snapshot().safe_mode;
        }
        if( req->timed && issued ) {
            // Back in the main loop after issuing the goal's action: check the result.
            const goal &gl = goals[g_idx];
            const observed o = snapshot();
            const bool ok = gl.done( o, d );
            std::printf( "  goal %-24s %s (turn %lld)\n", gl.what, ok ? "PASS" : "FAIL",
                         static_cast<long long>( o.turn ) );
            for( const std::string &m : o.messages ) {
                std::printf( "    msg: %s\n", m.c_str() );
            }
            passed += ok ? 1 : 0;
            issued = false;
            if( ++g_idx == n_goals ) {
                break;
            }
        }
        if( req->timed && goals[g_idx].action == nullptr ) {
            const bool ok = run_realtime( *req );
            std::printf( "  goal %-24s %s (turn %lld)\n", goals[g_idx].what, ok ? "PASS" : "FAIL",
                         static_cast<long long>( req->turn ) );
            passed += ok ? 1 : 0;
            ++g_idx;
        }
        const goal &cur = goals[g_idx];
        fp3d::ui_answer a;
        std::string note;
        if( req->timed ) {
            a.id = req->id;
            a.action = cur.action;
            note = a.action;
            issued = true;
            d.issue_turn = req->turn;
        } else {
            a = answer_for( *req, cur, d, note );
        }
        print_request( *req, note );
        fp3d::answer( a );
    }
    drain();
    const bool queue_ok = g_idx == n_goals && run_queue_checks();
    const bool safemode_ok = queue_ok && run_safemode_checks();
    const bool hands_ok = safemode_ok && run_hand_checks();
    const bool siphon_ok = hands_ok && run_siphon_checks();
    const bool shoot_ok = siphon_ok && run_shoot_checks();
    const bool items_ok = shoot_ok && run_item_checks();
    const bool cheats_ok = items_ok && run_cheat_checks();

    fp3d::shutdown();
    sim.join();
    if( !sim_error.empty() ) {
        std::printf( "sim error: %s\n", sim_error.c_str() );
    }
    std::printf( "%d/%zu goals passed, %d requests\n", passed, n_goals, requests );
    // The newest published snapshot is the one taken before the last request.
    const std::shared_ptr<const fp3d::snapshot> last = fp3d::latest_snapshot();
    const observed o = snapshot();
    const bool snap_ok = last && last->turn == o.turn && last->submaps.size() == 2541 &&
                         !last->creatures.empty() && last->creatures[0].kind == 0;
    std::printf( "snapshots: %s (seq %llu, turn %lld)\n", snap_ok ? "ok" : "BAD",
                 last ? static_cast<unsigned long long>( last->seq ) : 0ULL,
                 last ? static_cast<long long>( last->turn ) : -1LL );
    drain();
    using fp3d::event_kind;
    const auto count = [&]( event_kind k ) {
        return event_counts[static_cast<size_t>( k )];
    };
    // The goals end turns, print messages, throw a rock and start activities (eat, wait).
    const bool events_ok = count( event_kind::turn_end ) > 0 && count( event_kind::message ) > 0 &&
                           count( event_kind::projectile ) > 0 && count( event_kind::anim ) > 0 &&
                           events_dropped == 0 && seq_ordered;
    std::printf( "events: %s (turn_end %zu, message %zu, sound %zu, projectile %zu, anim %zu, attack %zu, "
                 "dropped %zu)\n", events_ok ? "ok" : "BAD", count( event_kind::turn_end ),
                 count( event_kind::message ), count( event_kind::sound ), count( event_kind::projectile ),
                 count( event_kind::anim ), count( event_kind::attack ), events_dropped );
    std::printf( "queue: %s\n", queue_ok ? "ok" : "BAD" );
    std::printf( "safe mode: %s\n", safemode_ok ? "ok" : "BAD" );
    std::printf( "hands: %s\n", hands_ok ? "ok" : "BAD" );
    std::printf( "siphon: %s\n", siphon_ok ? "ok" : "BAD" );
    std::printf( "shoot: %s\n", shoot_ok ? "ok" : "BAD" );
    std::printf( "items: %s\n", items_ok ? "ok" : "BAD" );
    std::printf( "cheats: %s\n", cheats_ok ? "ok" : "BAD" );
    const bool worlds_ok = world_name_failures == 0;
    std::printf( "worlds: %s\n", worlds_ok ? "ok" : "BAD" );
    return passed == static_cast<int>( n_goals ) && snap_ok && events_ok && queue_ok && safemode_ok && hands_ok &&
           siphon_ok &&
           shoot_ok && items_ok && cheats_ok && worlds_ok ?
           EXIT_SUCCESS : EXIT_FAILURE;
}
