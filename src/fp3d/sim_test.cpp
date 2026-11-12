// FP3D block B2 test: runs CDDA's real game loop on a worker thread behind the bridge and plays it from the
// main thread like a client would: open a door, pick up an apple, eat it, look around, throw a rock at a
// square, wait 5 minutes, run in real time until a zombie auto-pauses it, then save, quit and reload.
// Every request is printed. Exit code 0 only if all goals are verified from game state.
//
// usage: fp3d-sim-test   (run from the cdda/ folder)
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <functional>
#include <mutex>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <iostream>
#if defined(_WIN32)
#include <windows.h>
#endif

#include "achievement.h"
#include "avatar.h"
#include "cached_options.h"
#include "calendar.h"
#include "coordinates.h"
#include "debug.h"
#include "do_turn.h"
#include "field.h"
#include "field_type.h"
#include "flexbuffer_json.h"
#include "fp3d/arrival.h"
#include "fp3d/boot.h"
#include "fp3d/data_check.h"
#include "fp3d/bridge.h"
#include "fp3d/cheats.h"
#include "fp3d/cooking.h"
#include "fp3d/game_access.h"
#include "fp3d/bullet_paths.h"
#include "fp3d/gun_faults.h"
#include "fp3d/held.h"
#include "fp3d/item_hands.h"
#include "fp3d/points.h"
#include "fp3d/shot.h"
#include "fp3d/test_range.h"
#include "fp3d/events.h"
#include "fp3d/snapshot.h"
#include "fp3d/swing.h"
#include "game.h"
#include "game_constants.h"
#include "item.h"
#include "line.h"
#include "math_parser_diag_value.h"
#include "json_loader.h"
#include "map.h"
#include "map_scale_constants.h"
#include "mapdata.h"
#include "messages.h"
#include "monster.h"
#include "npc.h"
#include "options.h"
#include "overmap.h"
#include "overmapbuffer.h"
#include "path_info.h"
#include "pocket_type.h"
#include "rng.h"
#include "scenario.h"
#include "start_location.h"
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
const mtype_id mon_horse( "mon_horse" );
const itype_id itype_2h_flail_steel( "2h_flail_steel" );
const itype_id itype_gasoline( "gasoline" );
const itype_id itype_hose( "hose" );
const itype_id itype_jerrycan( "jerrycan" );
const itype_id itype_water( "water" );
const itype_id itype_glock_19( "glock_19" );
const itype_id itype_9mm( "9mm" );
const itype_id itype_glockmag( "glockmag" );
const itype_id itype_223( "223" );
const itype_id itype_plastic_bag_vac( "plastic_bag_vac" );
const itype_id itype_bag_plastic( "bag_plastic" );
const itype_id itype_bottle_plastic( "bottle_plastic" );
const itype_id itype_1l_gold( "1l_gold" );
const itype_id itype_remington_870( "remington_870" );
const itype_id itype_scar_l( "scar_l" );
const itype_id itype_backpack_giant( "backpack_giant" );
const itype_id itype_pebble( "pebble" );
const itype_id itype_bottle_twoliter( "bottle_twoliter" );
const ter_str_id ter_t_water_sh( "t_water_sh" );
const vproto_id vehicle_prototype_none( "none" );
const vpart_id vpart_frame( "frame" );
const vpart_id vpart_tank_small( "tank_small" );
const vpart_id vpart_seat( "seat" );
const ter_str_id ter_t_door_c( "t_door_c" );
const ter_str_id ter_t_floor( "t_floor" );
const ter_str_id ter_t_wall( "t_wall" );
const ter_str_id ter_t_concrete_wall( "t_concrete_wall" );
const ter_str_id ter_t_stairs_down( "t_stairs_down" );
const ter_str_id ter_t_stairs_up( "t_stairs_up" );
const trait_id trait_DEBUG_NIGHTVISION( "DEBUG_NIGHTVISION" );
const itype_id itype_pot( "pot" );
const itype_id itype_meat( "meat" );
const furn_str_id furn_f_fireplace( "f_fireplace" );
const furn_str_id furn_f_woodstove( "f_woodstove" );

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
    int west_water = 0;      // throw/pour checks: water charges inside the items lying west
    int carried_9mm = 0;     // item checks 2: 9 mm rounds carried outside guns and magazines
    int carry_kg = 0;        // cheat checks: the avatar's weight capacity
    int torso_hp_lost = -1;  // ... hp the torso lost to a 10 damage hit (-1: not probed)
    int kit_guns = 0;        // ... carried Glock 19, Remington 870 and SCAR-L with ammo loaded
    bool big_pack = false;   // ... the kit's big backpack, worn (or carried when CDDA won't let it be worn)
    bool achievements = true; // ... CDDA's achievements still on in this game
    // Gun item checks: every carried item with a handle, its rounds loaded (-1: not a gun or magazine) and charges.
    struct carried_count {
        uint64_t uid = 0;
        int ammo = -1;
        int charges = 0;
    };
    std::vector<carried_count> counts;
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
std::atomic<int> data_check_failures{ -1 };     // data check skip checks at boot (-1: not run)
std::atomic<int> world_name_failures{ -1 };     // world name checks at boot (-1: not run)
std::atomic<int> random_start_failures{ -1 };   // random start location check after start_game (-1: not run)
std::atomic<int> sealed_items{ -1 };            // item checks 2: items placed in the gun safe         // cheat checks: hit the torso, wield an empty Glock
// Hand checks: 1 = a zombie to the west and a rock in hand, 2 = a two-handed flail in hand, 3 = a hose in hand,
// an empty jerrycan carried and a one-tile vehicle north with 4 L of gasoline in a small tank, 4 = a zombie west,
// 5 = zombies gone and a spare hose carried, 6 = the spare hose dropped here, 7 = picked up again, 8 = the jerrycan
// wielded (the hose in hand gone), 9 = shallow water east and a puddle of 2 L of water south, 10 = a new empty
// jerrycan wielded, 11 = a zombie 3 squares west and a loaded pistol in hand, 12 = item checks: empty hands, two
// rocks, a plastic bottle and a plastic bag west, a rock 3 squares west, a rock with a hostile save's handle and
// spot north-west, 1 L of gold and a bottle of water carried, 13 = cooking checks (run_cooking_checks): a
// fireplace with a fire and a pot of 1 L of water south-west, an unlit wood stove south-east, a pot of raw meat 2
// west 1 north, a rock 2 west 1 south, a pot 8 west (out of reach), a pot put 2 west 2 south and taken away again,
// 16 = gun item checks (run_gun_item_checks; see the case below), 17 = throw and pour checks
// (run_throw_pour_checks; see the case below).
// Done on the sim thread right before its next request.
std::atomic<int> hand_setup{ 0 };
// Cooking checks: what scene 14 left for the driver (sim thread writes, driver reads).
std::atomic<uint64_t> cook_far_uid{ 0 };      // the pot out of reach
std::atomic<uint64_t> cook_gone_uid{ 0 };     // the pot taken away again (a stale handle)
std::atomic<double> cook_capture_ms{ 0 };     // one full capture of the scene
std::atomic<double> cook_heat_ms{ 0 };        // the heat and cookware part of it
std::atomic<size_t> cook_heat_squares{ 0 };   // squares it built a crafting inventory for
// Gun item checks: the handles scene 16 gave (sim thread writes, driver reads).
std::atomic<uint64_t> gun_glock_uid{ 0 };      // a carried Glock 19 with a full magazine
std::atomic<uint64_t> gun_spare_uid{ 0 };      // a carried empty Glock magazine
std::atomic<uint64_t> gun_9mm_uid{ 0 };        // carried loose 9 mm
std::atomic<uint64_t> gun_223_uid{ 0 };        // 10 carried .223
std::atomic<uint64_t> gun_sealed_uid{ 0 };     // 10 9 mm in a sealed vacuum bag
std::atomic<uint64_t> gun_far_glock_uid{ 0 };  // a loaded Glock 3 squares west
std::atomic<uint64_t> gun_far_9mm_uid{ 0 };    // 5 9 mm 3 squares west
std::atomic<uint64_t> gun_near_9mm_uid{ 0 };   // 5 9 mm 1 square west
// Throw and pour checks: the handles scene 17 gave (sim thread writes, driver reads).
std::atomic<uint64_t> tp_rock_uid{ 0 };        // a wielded rock
std::atomic<uint64_t> tp_pebble_uid{ 0 };      // 5 carried pebbles (one stack)
std::atomic<uint64_t> tp_ground_rock_uid{ 0 }; // a rock north-east
std::atomic<uint64_t> tp_bottle_a_uid{ 0 };    // carried bottles A and B with 500 ml of water each
std::atomic<uint64_t> tp_water_a_uid{ 0 };
std::atomic<uint64_t> tp_bottle_b_uid{ 0 };
std::atomic<uint64_t> tp_water_b_uid{ 0 };
std::atomic<uint64_t> tp_sealed_uid{ 0 };      // a carried bottle of 500 ml of water, sealed
std::atomic<uint64_t> tp_sealed_water_uid{ 0 };
std::atomic<uint64_t> tp_rock_bag_uid{ 0 };    // a carried plastic bag holding a rock
std::atomic<uint64_t> tp_big_uid{ 0 };         // a carried 2 L bottle with 1.5 L of water
std::atomic<uint64_t> tp_big_water_uid{ 0 };
std::atomic<uint64_t> tp_bottle_g_uid{ 0 };    // carried empty bottles G and H
std::atomic<uint64_t> tp_bottle_h_uid{ 0 };
std::atomic<uint64_t> tp_bottle_c_uid{ 0 };    // an empty bottle west
std::atomic<uint64_t> tp_bottle_d_uid{ 0 };    // a bottle full of water north
std::atomic<uint64_t> tp_open_bag_uid{ 0 };    // an empty plastic bag north (not watertight)

// Range op checks: 32 operations queued from inside the sim's hook (so none run before the queue is full), then one
// more, which is refused. They run at the next main-loop request.
std::atomic<bool> range_fill{ false };
std::atomic<uint64_t> range_fill_last{ 0 };    // the last one's id
std::atomic<bool> range_fill_ok{ false };      // ids 1 apart, 32 of them, the 33rd refused

// Teleport cheat checks (run_point_teleport_checks): at the next main-loop request the hook sets up round pt_round:
// it notes whether the player stands where the last round should have left it, then queues this round's teleports.
std::atomic<int> pt_round{ 0 };
std::array<std::atomic<uint64_t>, 5> pt_ids{};   // this round's teleports in order; 0 = not queued
std::atomic<int> pt_ended_ok{ -1 };              // 1 = where the last round should have left the player
std::optional<tripoint_abs_ms> pt_expected;      // sim thread only: where this round should leave the player
std::optional<tripoint_abs_ms> pt_blood;         // sim thread only: the square round 2 bloodied

// Range resume checks (run_range_resume_checks): sim_main gives the avatar the test range's avatar values, so this
// normal world's save carries them, and after the reload resumes the range as the bridge does after a load. What it
// found right after: -1 until then.
std::atomic<int> resume_said{ -1 };      // resume_test_range(): 1 yes, 0 no
std::atomic<int> resume_active{ -1 };    // range_active()
std::atomic<int> resume_zones{ -1 };     // range_zones().size()
std::atomic<int> resume_frozen{ -1 };    // ai_frozen()
std::atomic<int> resume_marked{ -1 };    // the loaded avatar still carries fp3d_range 1 (the save kept it)

// The test range's values on a normal game's avatar (fp3d/test_range.h): a crafted save, AI frozen, the hub built.
void mark_as_range()
{
    avatar &u = get_avatar();
    u.set_value( "fp3d_range", 1.0 );
    u.set_value( "fp3d_range_seed", 7.0 );
    u.set_value( "fp3d_range_ai_frozen", 1.0 );
    u.set_value( "fp3d_range_built_hub", 1.0 );
}

// Right after the reload, as bridge/src/sim_session.cpp does after loading a save.
void note_resume()
{
    resume_said.store( fp3d::resume_test_range() ? 1 : 0 );
    resume_active.store( fp3d::range_active() ? 1 : 0 );
    resume_zones.store( static_cast<int>( fp3d::range_zones().size() ) );
    resume_frozen.store( fp3d::ai_frozen() ? 1 : 0 );
    const diag_value *v = get_avatar().maybe_get_value( "fp3d_range" );
    resume_marked.store( v != nullptr && v->is_dbl() && v->dbl() == 1.0 ? 1 : 0 );
}

void fill_range_ops()
{
    fp3d::range_op heal;
    heal.kind = fp3d::range_op_kind::heal;
    std::vector<fp3d::range_op> ops( 24, heal );
    fp3d::range_op op;
    op.kind = fp3d::range_op_kind::ai_frozen;
    op.number = 1;
    ops.push_back( op );
    op = {};
    op.kind = fp3d::range_op_kind::spawn_monster;
    op.monster = "mon_zombie";
    ops.push_back( op );
    op = {};
    op.kind = fp3d::range_op_kind::spawn_item;
    op.itype = 1;
    op.number = 1;
    ops.push_back( op );
    op = {};
    op.kind = fp3d::range_op_kind::teleport;
    ops.push_back( op );
    op = {};
    op.kind = fp3d::range_op_kind::rebuild_zone;
    op.number = 1;
    ops.push_back( op );
    op = {};
    op.kind = fp3d::range_op_kind::time_of_day;
    ops.push_back( op );
    op = {};
    op.kind = fp3d::range_op_kind::advance;
    op.number = 1;
    ops.push_back( op );
    op = {};
    op.kind = fp3d::range_op_kind::advance;
    op.number = int64_t{ 1 } << 40;
    ops.push_back( op );
    uint64_t last = 0;
    bool queued = true;
    for( const fp3d::range_op &o : ops ) {
        const uint64_t id = fp3d::request_range_op( o );
        queued = queued && id != 0 && ( last == 0 || id == last + 1 );
        last = id;
    }
    range_fill_last.store( last );
    range_fill_ok.store( queued && ops.size() == fp3d::max_range_ops_waiting && fp3d::request_range_op( heal ) == 0 );
}

void queue_point_teleport( size_t i, const tripoint_abs_ms &p )
{
    fp3d::range_op op;
    op.kind = fp3d::range_op_kind::point_teleport;
    op.square = fp3d::ui_point{ p.x(), p.y(), p.z() };
    pt_ids[i].store( fp3d::request_range_op( op ) );
}

// A square 3 to 6 squares from `from` on its floor, free to arrive on (fp3d/arrival.h).
std::optional<tripoint_abs_ms> free_square_near( const tripoint_abs_ms &from )
{
    for( const tripoint_abs_ms &p : closest_points_first( from, 3, 6 ) ) {
        if( p.z() == from.z() && fp3d::arrival_square( p ) == p ) {
            return p;
        }
    }
    return std::nullopt;
}

void set_up_point_teleports( int round )
{
    avatar &u = get_avatar();
    map &here = get_map();
    const tripoint_abs_ms at = u.pos_abs();
    pt_ended_ok.store( pt_expected && at == *pt_expected ? 1 : 0 );
    pt_expected.reset();
    switch( round ) {
        case 1:
            // Refused: far out (no overflow), outside the bubble, above the top floor, the player's own square. Then
            // a free square.
            queue_point_teleport( 0, tripoint_abs_ms( INT_MAX, INT_MIN, 0 ) );
            queue_point_teleport( 1, here.get_abs( tripoint_bub_ms( MAPSIZE_X + 5, 5, u.posz() ) ) );
            queue_point_teleport( 2, tripoint_abs_ms( at.x(), at.y(), OVERMAP_HEIGHT + 1 ) );
            queue_point_teleport( 3, at );
            pt_expected = free_square_near( at );
            if( pt_expected ) {
                queue_point_teleport( 4, *pt_expected );
            }
            break;
        case 2:
            // Onto blood (a field): the nearest free square instead.
            pt_blood = free_square_near( at );
            if( pt_blood ) {
                here.add_field( here.get_bub( *pt_blood ), fd_blood, 1 );
                pt_expected = fp3d::arrival_square( *pt_blood );
                queue_point_teleport( 0, *pt_blood );
            }
            break;
        case 3:
            // In a vehicle (a frame and a seat put under the player, boarded): refused, the player stays.
            if( pt_blood ) {
                here.remove_field( here.get_bub( *pt_blood ), fd_blood );
            }
            if( vehicle *veh = here.add_vehicle( vehicle_prototype_none, u.pos_bub(), 0_degrees, 0, 0 ) ) {
                veh->install_part( here, point_rel_ms::zero, vpart_frame );
                veh->install_part( here, point_rel_ms::zero, vpart_seat );
                here.add_vehicle_to_cache( veh );
                here.board_vehicle( u.pos_bub(), &u );
            }
            pt_expected = at;
            if( const std::optional<tripoint_abs_ms> t = free_square_near( at ) ) {
                queue_point_teleport( 0, *t );
            }
            break;
        case 4:
            // Riding (a horse next to the player, mounted): refused, the player stays.
            here.unboard_vehicle( u.pos_bub() );
            if( const optional_vpart_position vp = here.veh_at( u.pos_bub() ) ) {
                here.destroy_vehicle( &vp->vehicle() );
            }
            for( const tripoint_bub_ms &p : closest_points_first( u.pos_bub(), 1, 1 ) ) {
                if( here.passable( p ) && g->is_empty( p ) ) {
                    if( monster *horse = g->place_critter_at( mon_horse, p ) ) {
                        u.mount_creature( *horse );
                    }
                    break;
                }
            }
            pt_expected = u.pos_abs();
            if( const std::optional<tripoint_abs_ms> t = free_square_near( u.pos_abs() ) ) {
                queue_point_teleport( 0, *t );
            }
            break;
        default:
            // Off the horse (unhurt), the horse gone.
            if( u.is_mounted() ) {
                u.forced_dismount();
                u.healall( 100 );
            }
            g->clear_zombies();
            break;
    }
}

void observe()
{
    // As the client's hook (bridge/src/sim_session.cpp).
    fp3d::run_range_requests();
    if( range_fill.exchange( false ) ) {
        fill_range_ops();
    }
    if( const int round = pt_round.exchange( 0 ); round != 0 ) {
        set_up_point_teleports( round );
    }
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
        case 14: {
            // The item scene's gun safe and dresser go first.
            here.furn_set( p + tripoint_rel_ms( 0, 1, 0 ), furn_str_id::NULL_ID() );
            here.furn_set( p + tripoint_rel_ms( 0, -3, 0 ), furn_str_id::NULL_ID() );
            g->clear_zombies();
            for( int dx = -9; dx <= 4; dx++ ) {
                for( int dy = -4; dy <= 4; dy++ ) {
                    here.i_clear( p + tripoint_rel_ms( dx, dy, 0 ) );
                }
            }
            for( int dx = -9; dx <= -5; dx++ ) {
                here.ter_set( p + tripoint_rel_ms( dx, 0, 0 ), ter_t_floor );
                here.furn_set( p + tripoint_rel_ms( dx, 0, 0 ), furn_str_id::NULL_ID() );
            }
            const tripoint_bub_ms fire_at = p + tripoint_rel_ms( -1, 1, 0 );
            here.furn_set( fire_at, furn_f_fireplace );
            here.furn_set( p + tripoint_rel_ms( 1, 1, 0 ), furn_f_woodstove );
            here.add_field( fire_at, fd_fire, 2 );
            item water_pot( itype_pot );
            water_pot.put_in( item( itype_water, calendar::turn, 4 ), pocket_type::CONTAINER );
            here.add_item( fire_at, water_pot );
            item meat_pot( itype_pot );
            meat_pot.put_in( item( itype_meat, calendar::turn ), pocket_type::CONTAINER );
            here.add_item( p + tripoint_rel_ms( -2, -1, 0 ), meat_pot );
            here.add_item( p + tripoint_rel_ms( -2, 1, 0 ), item( itype_rock ) );
            item &far_pot = here.add_item( p + tripoint_rel_ms( -8, 0, 0 ), item( itype_pot ) );
            cook_far_uid = far_pot.is_null() ? 0 : fp3d::item_handle( far_pot );
            const tripoint_bub_ms gone_at = p + tripoint_rel_ms( -2, 2, 0 );
            item &gone_pot = here.add_item( gone_at, item( itype_pot ) );
            cook_gone_uid = gone_pot.is_null() ? 0 : fp3d::item_handle( gone_pot );
            here.i_clear( gone_at );
            // One capture of this scene timed (the hook below captures again and publishes).
            fp3d::take_heat_capture_ms();
            const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            fp3d::capture_snapshot();
            cook_capture_ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
            size_t squares = 0;
            cook_heat_ms = fp3d::take_heat_capture_ms( &squares );
            cook_heat_squares = squares;
            break;
        }
        case 15: {
            // Stairs down north of the avatar, stairs up under them, floor around both (the siphon scene's
            // vehicle gone).
            g->clear_zombies();
            const tripoint_bub_ms s = p + tripoint_rel_ms( 0, -1, 0 );
            for( int dz = -1; dz <= 0; dz++ ) {
                for( int dx = -1; dx <= 1; dx++ ) {
                    for( int dy = -1; dy <= 1; dy++ ) {
                        const tripoint_bub_ms q = s + tripoint_rel_ms( dx, dy, dz );
                        if( const optional_vpart_position vp = here.veh_at( q ) ) {
                            here.destroy_vehicle( &vp->vehicle() );
                        }
                        here.ter_set( q, ter_t_floor );
                        here.furn_set( q, furn_str_id::NULL_ID() );
                        here.i_clear( q );
                        here.clear_fields( q );
                    }
                }
            }
            here.ter_set( s, ter_t_stairs_down );
            here.ter_set( s + tripoint_rel_ms( 0, 0, -1 ), ter_t_stairs_up );
            break;
        }
        case 16: {
            // Empty hands, nothing lying around but a loaded Glock 19 and 5 rounds of 9 mm 3 squares west (out of
            // reach) and 5 rounds 1 square west. Carried: a Glock 19 with a full magazine, an empty Glock magazine,
            // 30 loose 9 mm, 10 .223, 10 9 mm in a sealed vacuum bag.
            avatar &u = get_avatar();
            g->clear_zombies();
            for( int dx = -4; dx <= 4; dx++ ) {
                for( int dy = -4; dy <= 4; dy++ ) {
                    here.i_clear( p + tripoint_rel_ms( dx, dy, 0 ) );
                }
            }
            u.remove_weapon();
            const auto loaded_glock = []() {
                item mag( itype_glockmag );
                mag.ammo_set( itype_9mm, 15 );
                item gun( itype_glock_19 );
                gun.put_in( mag, pocket_type::MAGAZINE_WELL );
                return gun;
            };
            const auto handle = []( item_location loc ) {
                return loc ? fp3d::item_handle( *loc ) : 0;
            };
            gun_glock_uid = handle( u.i_add( loaded_glock() ) );
            gun_spare_uid = handle( u.i_add( item( itype_glockmag ) ) );
            gun_9mm_uid = handle( u.i_add( item( itype_9mm, calendar::turn, 30 ) ) );
            gun_223_uid = handle( u.i_add( item( itype_223, calendar::turn, 10 ) ) );
            item bag( itype_plastic_bag_vac );
            bag.put_in( item( itype_9mm, calendar::turn, 10 ), pocket_type::CONTAINER );
            for( item_pocket *pk : bag.get_all_contained_pockets() ) {
                pk->seal();
            }
            item_location sealed = u.i_add( bag );
            uint64_t sealed_uid = 0;
            if( sealed ) {
                for( item *in : sealed->all_items_top( pocket_type::CONTAINER ) ) {
                    sealed_uid = fp3d::item_handle( *in );
                }
            }
            gun_sealed_uid = sealed_uid;
            const tripoint_bub_ms far_sq = p + tripoint_rel_ms( -3, 0, 0 );
            here.ter_set( far_sq, ter_t_floor );
            here.furn_set( far_sq, furn_str_id::NULL_ID() );
            item &far_gun = here.add_item( far_sq, loaded_glock() );
            gun_far_glock_uid = far_gun.is_null() ? 0 : fp3d::item_handle( far_gun );
            item &far_ammo = here.add_item( far_sq, item( itype_9mm, calendar::turn, 5 ) );
            gun_far_9mm_uid = far_ammo.is_null() ? 0 : fp3d::item_handle( far_ammo );
            const tripoint_bub_ms near_sq = p + tripoint_rel_ms( -1, 0, 0 );
            here.ter_set( near_sq, ter_t_floor );
            here.furn_set( near_sq, furn_str_id::NULL_ID() );
            item &near_ammo = here.add_item( near_sq, item( itype_9mm, calendar::turn, 5 ) );
            gun_near_9mm_uid = near_ammo.is_null() ? 0 : fp3d::item_handle( near_ammo );
            break;
        }
        case 17: {
            // Floor from 8 west to 4 east, 4 north to 4 south, nothing lying there, no vehicles or fields. Worn and
            // carried things replaced: a backpack worn, a rock wielded; carried: 5 pebbles, bottles A and B with
            // 500 ml of water each, a sealed bottle of 500 ml of water, a plastic bag holding a rock, a 2 L bottle
            // with 1.5 L of water (a pot is an open container: carried, it spills), empty bottles G and H. Lying
            // around: an empty bottle C west, a bottle D full of water and an empty plastic bag north, a rock
            // north-east.
            avatar &u = get_avatar();
            g->clear_zombies();
            for( int dx = -8; dx <= 4; dx++ ) {
                for( int dy = -4; dy <= 4; dy++ ) {
                    const tripoint_bub_ms q = p + tripoint_rel_ms( dx, dy, 0 );
                    if( const optional_vpart_position vp = here.veh_at( q ) ) {
                        here.destroy_vehicle( &vp->vehicle() );
                    }
                    here.ter_set( q, ter_t_floor );
                    here.furn_set( q, furn_str_id::NULL_ID() );
                    here.i_clear( q );
                    here.clear_fields( q );
                }
            }
            u.clear_worn();
            u.inv->clear();
            u.remove_weapon();
            u.wear_item( item( itype_backpack ), false );
            u.set_wielded_item( item( itype_rock ) );
            const auto handle = []( item_location loc ) {
                return loc ? fp3d::item_handle( *loc ) : 0;
            };
            const auto inside = []( item_location loc ) {
                uint64_t uid = 0;
                if( loc ) {
                    for( item *in : loc->all_items_top( pocket_type::CONTAINER ) ) {
                        uid = fp3d::item_handle( *in );
                    }
                }
                return uid;
            };
            const auto bottle_of_water = []( bool sealed ) {
                item b( itype_bottle_plastic );
                b.put_in( item( itype_water, calendar::turn, 2 ), pocket_type::CONTAINER );
                if( sealed ) {
                    for( item_pocket *pk : b.get_all_contained_pockets() ) {
                        pk->seal();
                    }
                }
                return b;
            };
            tp_rock_uid = handle( u.get_wielded_item() );
            tp_pebble_uid = handle( u.i_add( item( itype_pebble, calendar::turn, 5 ) ) );
            item_location a = u.i_add( bottle_of_water( false ) );
            tp_bottle_a_uid = handle( a );
            tp_water_a_uid = inside( a );
            item_location b = u.i_add( bottle_of_water( false ) );
            tp_bottle_b_uid = handle( b );
            tp_water_b_uid = inside( b );
            item_location sealed = u.i_add( bottle_of_water( true ) );
            tp_sealed_uid = handle( sealed );
            tp_sealed_water_uid = inside( sealed );
            item rock_bag( itype_bag_plastic );
            rock_bag.put_in( item( itype_rock ), pocket_type::CONTAINER );
            tp_rock_bag_uid = handle( u.i_add( rock_bag ) );
            item big( itype_bottle_twoliter );
            big.put_in( item( itype_water, calendar::turn, 6 ), pocket_type::CONTAINER );
            item_location big_loc = u.i_add( big );
            tp_big_uid = handle( big_loc );
            tp_big_water_uid = inside( big_loc );
            tp_bottle_g_uid = handle( u.i_add( item( itype_bottle_plastic ) ) );
            tp_bottle_h_uid = handle( u.i_add( item( itype_bottle_plastic ) ) );
            const auto lay = [&here]( const tripoint_bub_ms & at, const item & it ) {
                item &laid = here.add_item( at, it );
                return laid.is_null() ? 0 : fp3d::item_handle( laid );
            };
            tp_bottle_c_uid = lay( p + tripoint_rel_ms( -1, 0, 0 ), item( itype_bottle_plastic ) );
            tp_bottle_d_uid = lay( p + tripoint_rel_ms( 0, -1, 0 ), bottle_of_water( false ) );
            tp_open_bag_uid = lay( p + tripoint_rel_ms( 0, -1, 0 ), item( itype_bag_plastic ) );
            tp_ground_rock_uid = lay( p + tripoint_rel_ms( 1, -1, 0 ), item( itype_rock ) );
            // an earlier scene's walls and fields: the transparency cache must see the new floor before the throw
            here.build_map_cache( p.z() );
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
    o.achievements = get_achievements().is_enabled();
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
        for( const item *in : it.all_items_top() ) {
            o.west_water += in->typeId() == itype_water ? in->charges : 0;
        }
    }
    o.loads = loads.load();
    for( const auto &m : Messages::recent_messages( 6 ) ) {
        o.messages.push_back( m.second );
    }
    // Block B3: the client reads this snapshot while the sim waits for the answer.
    fp3d::publish_snapshot( fp3d::capture_snapshot() );
    fp3d::publish_held();   // as the bridge's hook does
    // After the capture: items that just came out of something have their handles now.
    for( const item_location &loc : get_avatar().all_items_loc() ) {
        const uint64_t uid = fp3d::read_item_handle( *loc );
        if( uid != 0 ) {
            observed::carried_count c;
            c.uid = uid;
            c.ammo = loc->is_gun() || loc->is_magazine() ? loc->ammo_remaining() : -1;
            c.charges = loc->count_by_charges() ? loc->charges : 1;
            o.counts.push_back( c );
        }
    }
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
    // The world boot used: fp3d-sim, or fp3d-sim-2, ... where an older unmarked fp3d-sim is left alone.
    const std::string active = world_generator->active_world != nullptr ?
                               world_generator->active_world->world_name : std::string();
    std::string active_upper = active;
    for( char &ch : active_upper ) {
        ch = ch >= 'a' && ch <= 'z' ? static_cast<char>( ch - 'a' + 'A' ) : ch;
    }
    check( !active.empty() && fp3d::world_name_taken( active_upper ) == std::optional<std::string>( active ),
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
    check( fp3d::reserved_world_name( "FP3D-x" ) && fp3d::reserved_world_name( "fp3d-" ) &&
           fp3d::reserved_world_name( "Fp3D-live" ) && !fp3d::reserved_world_name( "fp3d" ) &&
           !fp3d::reserved_world_name( "x-fp3d-" ) && !fp3d::reserved_world_name( "" ),
           "fp3d- names reserved, any case" );
    check( fp3d::replaceable_world( active ) && fp3d::owned_world( active ) &&
           fp3d::replacing_world_name( "fp3d-sim" ) == active, "the world boot made is its own to replace" );
    // An fp3d- folder FP3D didn't mark (a player's, or an older FP3D's): left alone, the next name is used.
    const std::filesystem::path blocked = std::filesystem::u8path( PATH_INFO::savedir() ) / "fp3d-blocked";
    std::filesystem::create_directories( blocked, ec );
    check( !ec && fp3d::replacing_world_name( "fp3d-blocked" ) == "fp3d-blocked-2" &&
           fp3d::replacing_world_name( "FP3D-BLOCKED" ) == "FP3D-BLOCKED-2",
           "someone else's world left alone, the next name used" );
    std::filesystem::remove_all( blocked, ec );
    const std::filesystem::path theirs = std::filesystem::u8path( PATH_INFO::savedir() ) / "fp3d-theirs";
    std::filesystem::create_directories( theirs, ec );
    const bool plain = !fp3d::owned_world( "fp3d-theirs" );
    std::filesystem::create_directories( theirs / fp3d::owned_world_marker, ec );
    check( plain && !fp3d::owned_world( "fp3d-theirs" ) && !fp3d::owned_world( "fp3d-nothere" ),
           "unmarked worlds aren't replaced" );
    std::filesystem::remove_all( theirs, ec );
    return failed;
}

// A random start (the sim's character is a quick start's) lands in the nearest overmap, in CDDA's search order, that
// has any of the scenario's start locations (fp3d::nearest_random_start): none searched before it has one.
int check_random_start()
{
    int failed = 0;
    const auto check = [&]( bool ok, const std::string &what ) {
        std::printf( "  random start: %-50s %s\n", what.c_str(), ok ? "PASS" : "FAIL" );
        failed += ok ? 0 : 1;
    };
    const scenario &scen = *get_scenario();
    const point_abs_om start_om = project_to<coords::om>( get_avatar().pos_abs_omt().xy() );
    check( get_avatar().random_start_location, "the sim's character starts at random" );
    int searched = 0;
    bool reached = false;
    for( const point_abs_om &omp : closest_points_first( point_abs_om(), 3 ) ) {
        searched++;
        if( omp == start_om ) {
            reached = true;
            break;
        }
        // Made by the search already: get() makes nothing new here.
        const overmap &om = overmap_buffer.get( omp );
        for( const start_location &loc : start_locations::get_all() ) {
            if( scen.allowed_start( loc.id ) && fp3d::start_terrains_in( om, loc ) > 0 ) {
                check( false, "overmap " + std::to_string( searched ) + " has " + loc.id.str() + ", passed over" );
            }
        }
    }
    check( reached, "started in overmap " + std::to_string( searched ) + " of the search" );
    return failed;
}

// FP3D's data check skip (fp3d/data_check.h): the same data gives the same fingerprint, another mod or a world's own
// mods change that, a garbled marker means check, and a check that passed is skipped the next time.
int check_data_check()
{
    namespace fs = std::filesystem;
    int failed = 0;
    const auto check = [&]( bool ok, const char *what ) {
        std::printf( "  data check: %-40s %s\n", what, ok ? "PASS" : "FAIL" );
        failed += ok ? 0 : 1;
    };
    std::printf( "  data check: this boot %s CDDA's check\n", fp3d::data_check_skipped() ? "skipped" : "ran" );
    check( !fp3d::data_check_skipped(), "test programs always check (seeded RNG)" );
    fp3d::allow_data_check_skip_in_tests( true );
    const std::vector<mod_id> mods = world_generator->active_world->active_mod_order;
    const std::optional<std::string> fp = fp3d::data_fingerprint( mods );
    check( fp && fp == fp3d::data_fingerprint( mods ), "same data, same fingerprint" );
    const mod_id extra( "magiclysm" );
    std::vector<mod_id> more = mods;
    more.push_back( extra );
    const std::optional<std::string> fp_more = extra.is_valid() ? fp3d::data_fingerprint( more ) : std::nullopt;
    check( fp_more && fp_more != fp, "another mod, another fingerprint" );
    const fs::path world_mods = ( PATH_INFO::world_base_save_path() / "mods" ).get_unrelative_path();
    std::error_code ec;
    const bool had_folder = fs::exists( world_mods, ec );
    fs::create_directories( world_mods, ec );
    check( fp3d::data_fingerprint( mods ) == fp, "empty world mods folder: same" );
    {
        std::ofstream mod( world_mods / "fp3d_sim_mod.json", std::ios::binary );
        mod << "[]";
    }
    check( !fp3d::data_fingerprint( mods ), "world's own mods: checked every boot" );
    fs::remove( world_mods / "fp3d_sim_mod.json", ec );
    if( !had_folder ) {
        fs::remove( world_mods, ec );
    }
    {
        std::ofstream marker( fs::u8path( fp3d::data_check_marker_path() ), std::ios::binary | std::ios::trunc );
        marker << "fp3d data check 1\nnot a fingerprint\n";
    }
    fp3d::prepare_data_check( mods );
    check( !fp3d::data_check_skipped() && !get_option<bool>( "SKIP_VERIFICATION" ), "garbled marker: the check runs" );
    fp3d::finish_data_check();
    const bool errors = debug_has_error_been_observed();
    fp3d::prepare_data_check( mods );
    const bool skipped = fp3d::data_check_skipped() && get_option<bool>( "SKIP_VERIFICATION" );
    fp3d::finish_data_check();
    check( errors ? !skipped : skipped, errors ? "CDDA reported errors: not remembered" : "passed: skipped the next time" );
    check( !get_option<bool>( "SKIP_VERIFICATION" ), "the option is back afterwards" );
    if( skipped ) {
        // A boot that failed between prepare and finish: the next prepare puts the option back first instead of
        // taking it for the player's own choice.
        fp3d::prepare_data_check( mods );
        fp3d::prepare_data_check( mods );
        const bool again = fp3d::data_check_skipped();
        fp3d::finish_data_check();
        check( again && !get_option<bool>( "SKIP_VERIFICATION" ), "failed boot: option back, skip kept" );
    }
    const fs::path marker = fs::u8path( fp3d::data_check_marker_path() );
    fs::path kept = marker;
    kept += ".sim";
    const bool had_marker = fs::exists( marker, ec );
    if( had_marker ) {
        fs::rename( marker, kept, ec );
    }
    get_options().get_option( "SKIP_VERIFICATION" ).setValue( "true" );
    fp3d::prepare_data_check( mods );
    const bool left = !fp3d::data_check_skipped() && get_option<bool>( "SKIP_VERIFICATION" );
    fp3d::finish_data_check();
    check( left && get_option<bool>( "SKIP_VERIFICATION" ) && !fs::exists( marker, ec ),
           "player's own skip: left alone, not remembered" );
    get_options().get_option( "SKIP_VERIFICATION" ).setValue( "false" );
    if( had_marker ) {
        fs::rename( kept, marker, ec );
    }
    fp3d::allow_data_check_skip_in_tests( false );
    return failed;
}

void sim_main()
{
    try {
        fp3d::boot_world( "fp3d-sim", true );
        world_name_failures = check_world_names();
        data_check_failures = check_data_check();
        std::printf( "sim: data loaded, starting game\n" );
        // The real new-game path: calendar, start location, starting items, game mode.
        if( !fp3d_game_access::start_game( *g ) ) {
            throw std::runtime_error( "start_game failed" );
        }
        random_start_failures = check_random_start();
        build_scene();
        mark_as_range();
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
        note_resume();
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
        const std::optional<fp3d::ui_request> next = fp3d::wait_request( 10000, req.id );
        const double ms = ms_since( t );
        t = std::chrono::steady_clock::now();
        if( !next || !next->timed ) {
            std::printf( "    real time: no main-loop request after %.0f ms\n", ms );
            return false;
        }
        // A turn is 250 ms at this timescale: never faster (paced), but a loaded machine (parallel builds) may be
        // slower, so the upper bound is loose and a slow step may have let a turn or two more pass.
        const int64_t turns = next->turn - req.turn;
        const bool step_ok = turns >= 1 && ms >= 150 && turns <= 2 + static_cast<int64_t>( ms / 250 ) &&
                             ms <= 10000;
        std::printf( "    real time: turn %lld -> %lld after %.0f ms %s\n", static_cast<long long>( req.turn ),
                     static_cast<long long>( next->turn ), ms, step_ok ? "ok" : "BAD" );
        ok = ok && step_ok;
        req = *next;
    }

    spawn_zombie = true; // placed by the sim right before its next request
    t = std::chrono::steady_clock::now();
    // Up to 10 s: on a loaded machine the game thread may be busy for seconds (an overmap loading, say).
    while( !fp3d::get_clock().paused && ms_since( t ) < 10000 ) {
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
    const std::optional<fp3d::ui_request> resumed = fp3d::wait_request( 10000, req.id );
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
    const std::optional<fp3d::ui_request> pending = fp3d::wait_request( 10000 );
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
        case fp3d::request_kind::world_setup:
        case fp3d::request_kind::chargen:
        case fp3d::request_kind::shot:
        case fp3d::request_kind::swing:
            // Not asked by these goals (world setup and character creation have their own tests; shots and swings
            // go CDDA's way): an empty answer.
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

    // Client-flown shots (fp3d/shot.h): a planned miss and a planned head hit are both taken (CDDA's roll decides
    // what lands and a miss keeps CDDA's line: run_combat_checks covers that). Pistol reloaded, zombie restored
    // each time (hand_setup 11).
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
                // a miss ends a square short of the zombie and a square off its line; a hit is in its head
                s.end = fp3d::ui_point{ c->origin.x - ( hit ? 3 : 2 ), c->origin.y + ( hit ? 0 : 1 ), c->origin.z };
                s.hit_uid = hit ? b.zombie_uid : 0;
                s.part = "head";
                s.quality = 0.1f;
                a.pos = fp3d::ui_point{ c->origin.x - 3, c->origin.y, c->origin.z };
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
    const fp3d::shot_counts c0 = fp3d::get_shot_counts();
    const auto miss = client_shot( false );
    const fp3d::shot_counts cm = fp3d::get_shot_counts();
    check( miss.first > 0 && cm.planned == c0.planned + 1, "client shot: planned miss taken",
           "hp " + std::to_string( miss.first ) + " -> " + std::to_string( miss.second ) );
    const auto head = client_shot( true );
    const fp3d::shot_counts c1 = fp3d::get_shot_counts();
    check( head.first > 0 && head.second <= head.first && c1.planned == c0.planned + 2, "client shot: planned head hit",
           "hp " + std::to_string( head.first ) + " -> " + std::to_string( head.second ) + ", plans used " +
           std::to_string( c1.planned - c0.planned ) );
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
    // Held again where it lies (pocket 0), then put into that same pocket: it just leaves the hand.
    bool held_again = false;
    if( const std::optional<fp3d::ui_request> h0 = fp3d::wait_request( 5000 ); h0 && h0->timed ) {
        fp3d::queue_action( "fp3d_hold_off" );
        const std::optional<fp3d::ui_request> m = fp3d::wait_request( 20000, h0->id );
        std::string label;
        const int e = m && !m->timed ? first_entry_with( *m, "rock", label ) : -1;
        if( e >= 0 ) {
            fp3d::ui_answer a;
            a.id = m->id;
            a.entry = e;
            fp3d::answer( a );
            const std::optional<fp3d::ui_request> h1 = fp3d::wait_request( 20000, m->id );
            held_again = h1 && h1->timed && fp3d::latest_held().off.id == "rock";
        } else if( m && !m->timed ) {
            fp3d::ui_answer a;
            a.id = m->id;
            a.cancel = true;
            fp3d::answer( a );
        }
    }
    res = run( put );
    const fp3d::carried_view still = carried( rock_b.uid );
    check( held_again && res.back && res.spent > 0 && has( res.msg, "You put" ) && still.parent == backpack.uid &&
           still.pocket == 0 && !fp3d::latest_held().off.held, "put the held item into the pocket it lies in",
           show( res ) );
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

// L9 (fp3d/cooking.h): heat sources and cookware in the snapshot, and recipe queries from a container's contents:
// CDDA's recipes, read-only, answered by the main loop, refused when the container is gone, wrong or out of reach.
static bool run_cooking_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  cooking: %-44s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    struct parsed_match {
        std::string recipe;
        std::string result;
        std::string name;
        int batch_max = 0;
        int time_s = 0;
        bool known = false;
        bool skills_ok = false;
        bool tools_ok = false;
        bool can_start = false;
        std::vector<std::string> missing;
    };
    struct parsed {
        bool valid = false;
        bool ok = false;
        std::string error;
        int64_t id = 0;
        int64_t container = 0;
        std::vector<parsed_match> matches;
    };
    const auto parse = []( const std::string & text ) {
        parsed out;
        try {
            const JsonValue root = json_loader::from_string( text );
            JsonObject jo = root.get_object();
            jo.allow_omitted_members();
            out.ok = jo.get_bool( "ok" );
            out.error = jo.get_string( "error" );
            out.id = jo.get_member( "id" ).get_int64();
            out.container = jo.get_member( "container" ).get_int64();
            for( JsonObject m : jo.get_array( "matches" ) ) {
                m.allow_omitted_members();
                parsed_match pm;
                pm.recipe = m.get_string( "recipe" );
                pm.result = m.get_string( "result" );
                pm.name = m.get_string( "name" );
                pm.batch_max = m.get_int( "batch_max" );
                pm.time_s = m.get_int( "time_s" );
                pm.known = m.get_bool( "known" );
                pm.skills_ok = m.get_bool( "skills_ok" );
                pm.tools_ok = m.get_bool( "tools_ok" );
                pm.can_start = m.get_bool( "can_start" );
                for( const JsonValue v : m.get_array( "missing" ) ) {
                    pm.missing.push_back( v.get_string() );
                }
                out.matches.push_back( pm );
            }
            out.valid = true;
        } catch( const std::exception & ) {
            out.valid = false;
        }
        return out;
    };
    // Posts a query and waits for its result; "refused" when the bridge says no.
    const auto query = []( uint64_t uid, uint32_t type, const fp3d::ui_point & pos ) {
        const uint64_t id = fp3d::post_recipe_query( fp3d::item_ref{ uid, type }, pos );
        if( id == 0 ) {
            return std::string( "refused" );
        }
        const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        while( ms_since( t0 ) < 20000 ) {
            if( std::optional<std::string> res = fp3d::take_query_result( id ) ) {
                return *res;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
        }
        return std::string( "timeout" );
    };
    const auto type_of = []( const char *id ) {
        return fp3d::intern_id( fp3d::id_kind::itype, id );
    };

    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    hand_setup = 14;
    fp3d::queue_action( "pause" );
    r = fp3d::wait_request( 20000, r->id );
    const std::shared_ptr<const fp3d::snapshot> s0 = fp3d::latest_snapshot();
    if( !r || !r->timed || !s0 ) {
        check( false, "scene", "" );
        return false;
    }
    const fp3d::ui_point me = s0->player.pos;
    const auto at = [&me]( int dx, int dy ) {
        return fp3d::ui_point{ me.x + dx, me.y + dy, me.z };
    };
    const fp3d::ui_point fire_at = at( -1, 1 );
    const fp3d::ui_point stove_at = at( 1, 1 );
    const fp3d::ui_point meat_at = at( -2, -1 );
    const fp3d::ui_point rock_at = at( -2, 1 );
    const fp3d::ui_point far_at = at( -8, 0 );
    const fp3d::ui_point gone_at = at( -2, 2 );
    const uint32_t t_pot = type_of( "pot" );
    const uint32_t t_water = type_of( "water" );
    const uint32_t t_rock = type_of( "rock" );
    // The submap of absolute square `p` in s0 and the tile index in it.
    const auto submap_at = [&s0]( const fp3d::ui_point & p, int &tile ) -> const fp3d::submap_view * {
        const auto floor_div = []( int a )
        {
            return a >= 0 ? a / fp3d::sm_size : ( a - fp3d::sm_size + 1 ) / fp3d::sm_size;
        };
        const int sx = floor_div( p.x );
        const int sy = floor_div( p.y );
        tile = ( p.y - sy * fp3d::sm_size ) * fp3d::sm_size + ( p.x - sx * fp3d::sm_size );
        for( const std::shared_ptr<const fp3d::submap_view> &sm : s0->submaps )
        {
            if( sm->pos.x == sx && sm->pos.y == sy && sm->pos.z == p.z ) {
                return sm.get();
            }
        }
        return nullptr;
    };
    const auto heat_at = [&]( const fp3d::ui_point & p ) {
        int tile = 0;
        if( const fp3d::submap_view *sm = submap_at( p, tile ) ) {
            for( const fp3d::heat_view &h : sm->heat ) {
                if( h.tile == tile ) {
                    return std::optional<fp3d::heat_view>( h );
                }
            }
        }
        return std::optional<fp3d::heat_view>();
    };
    const auto heat_text = []( const std::optional<fp3d::heat_view> &h ) {
        return h ? "source " + std::to_string( h->source ) + " tool " + std::to_string( h->tool ) + " fire " +
               std::to_string( h->fire ) + " charges " + std::to_string( h->charges ) + ( h->lit ? " lit" : " unlit" ) :
               std::string( "none" );
    };

    // Heat sources: the fireplace burns (CDDA's `fire`), the wood stove has no fuel.
    const std::optional<fp3d::heat_view> fh = heat_at( fire_at );
    check( fh && fh->lit && fh->fire > 0 && fh->source == fp3d::heat_from_furniture &&
           fh->tool == type_of( "fake_fireplace" ), "fireplace with a fire: lit", heat_text( fh ) );
    const std::optional<fp3d::heat_view> sh = heat_at( stove_at );
    check( sh && !sh->lit && sh->fire == 0 && sh->charges == 0 && sh->source == fp3d::heat_from_furniture &&
           sh->tool == type_of( "fake_woodstove" ), "wood stove without fuel: unlit", heat_text( sh ) );
    check( !heat_at( meat_at ) && !heat_at( at( 0, 0 ) ), "no heat where there is none", "" );

    // Cookware on the fire: the pot, its handle as the pile lists it, the water inside.
    fp3d::cookware_view pot;
    {
        int tile = 0;
        if( const fp3d::submap_view *sm = submap_at( fire_at, tile ) ) {
            for( const fp3d::cookware_view &cw : sm->cookware ) {
                if( cw.tile == tile && cw.type == t_pot ) {
                    pot = cw;
                }
            }
        }
    }
    uint64_t pile_pot = 0;
    for( const fp3d::item_handle_view &h : handles_at( fire_at ) ) {
        pile_pot = h.type == t_pot ? h.uid : pile_pot;
    }
    const bool water_in = pot.contents.size() == 1 && pot.contents[0].type == t_water && pot.contents[0].count == 4 &&
                          pot.contents[0].uid != 0;
    check( pot.uid != 0 && pot.uid == pile_pot && water_in && pot.craft_result == 0 && pot.craft_recipe.empty(),
           "pot on the fire listed with its water", "uid " + std::to_string( pot.uid ) + " pile " +
           std::to_string( pile_pot ) + ", " + std::to_string( pot.contents.size() ) + " inside" );
    int meat_tile = 0;
    const fp3d::submap_view *meat_sm = submap_at( meat_at, meat_tile );
    check( meat_sm != nullptr && std::none_of( meat_sm->cookware.begin(), meat_sm->cookware.end(),
    [&]( const fp3d::cookware_view & cw ) {
        return cw.tile == meat_tile;
    } ), "a pot off the heat is no cookware entry", "" );
    // C6 lights: the fire on the fireplace lights its square (a field: kind 2); the z range and natural light ride
    // along at the top and in env.
    int fire_tile = 0;
    const fp3d::submap_view *fire_sm = submap_at( fire_at, fire_tile );
    const bool fire_lit = fire_sm != nullptr && std::any_of( fire_sm->lights.begin(), fire_sm->lights.end(),
    [&]( const fp3d::light_view & l ) {
        return l.tile == fire_tile && l.kind == fp3d::light_fire && l.luminance > 0;
    } );
    std::ostringstream js;
    fp3d::write_snapshot_json( js, *s0, me.z, me.z );
    const std::string json = js.str();
    check( fire_lit && fire_sm->lights.size() <= fp3d::max_lights_per_submap &&
           json.find( "\"z_levels\":[-10,10]" ) != std::string::npos &&
           json.find( "\"ambient_light\":" ) != std::string::npos && json.find( "\"lights\":[[" ) != std::string::npos,
           "lights: the fire's square, z range, natural light", fire_sm ? std::to_string( fire_sm->lights.size() ) +
           " lights" : "no submap" );
    check( json.find( "\"heat\":[[" ) != std::string::npos && json.find( "\"cookware\":[{\"tile\":" ) !=
           std::string::npos && json.find( "\"craft_recipe\":\"\"" ) != std::string::npos, "snapshot JSON: heat and cookware",
           "" );
    check( cook_heat_squares.load() >= 2, "capture cost", std::to_string( cook_capture_ms.load() ) +
           " ms capture, heat + cookware " + std::to_string( cook_heat_ms.load() ) + " ms, " +
           std::to_string( cook_heat_squares.load() ) + " squares" );

    // Queries, answered while the main loop waits.
    const std::chrono::steady_clock::time_point tq = std::chrono::steady_clock::now();
    const parsed water = parse( query( pot.uid, t_pot, fire_at ) );
    const double water_ms = ms_since( tq );
    size_t checked = 0;
    const double sim_ms = fp3d::last_recipe_query_ms( &checked );
    const auto find_result = []( const parsed & p, const char *result ) {
        for( const parsed_match &m : p.matches ) {
            if( m.result == result ) {
                return std::optional<parsed_match>( m );
            }
        }
        return std::optional<parsed_match>();
    };
    const std::optional<parsed_match> boil = find_result( water, "water_clean" );
    check( water.valid && water.ok && water.container == static_cast<int64_t>( pot.uid ) && boil &&
           boil->time_s > 0 && boil->tools_ok && boil->batch_max == 4, "pot of water: boiling it (water_clean)",
           boil ? boil->recipe + " \"" + boil->name + "\" " + std::to_string( boil->time_s ) + " s, batch " +
           std::to_string( boil->batch_max ) + ( boil->can_start ? ", can start" : ", can't start" ) +
           ", " + std::to_string( water.matches.size() ) + " matches" : "none" );
    check( water_ms < 2000, "query cost", std::to_string( sim_ms ) + " ms in the sim, " + std::to_string( checked ) +
           " recipes checked, " + std::to_string( water_ms ) + " ms round trip" );
    uint64_t meat_pot = 0;
    for( const fp3d::item_handle_view &h : handles_at( meat_at ) ) {
        meat_pot = h.type == t_pot ? h.uid : meat_pot;
    }
    const parsed meat = parse( query( meat_pot, t_pot, meat_at ) );
    const std::optional<parsed_match> cooked = find_result( meat, "meat_cooked" );
    std::string missing;
    if( cooked ) {
        for( const std::string &m : cooked->missing ) {
            missing += " [" + m + "]";
        }
    }
    check( meat.valid && meat.ok && cooked && cooked->time_s > 0 && cooked->tools_ok, "pot of raw meat: cooked meat",
           cooked ? cooked->recipe + " " + std::to_string( cooked->time_s ) + " s" + missing + ", " +
           std::to_string( meat.matches.size() ) + " matches" : "none" );

    // Refusals: malformed refs never get posted; wrong or gone items, wrong squares, far ones say why.
    check( fp3d::post_recipe_query( { 0, t_pot }, fire_at ) == 0 && fp3d::post_recipe_query( { pot.uid, 0 }, fire_at ) == 0 &&
           fp3d::post_recipe_query( { fp3d::max_item_uid + 1, t_pot }, fire_at ) == 0 &&
           fp3d::post_recipe_query( { pot.uid, t_pot }, fp3d::ui_point{ INT32_MAX, 0, 0 } ) == 0,
           "malformed queries refused", "" );
    const auto refused = [&]( const parsed & p, const char *error ) {
        return p.valid && !p.ok && p.error == error && p.matches.empty();
    };
    const parsed wrong_type = parse( query( pot.uid, t_rock, fire_at ) );
    check( refused( wrong_type, "not found" ), "wrong itype for the uid: not found", wrong_type.error );
    const parsed wrong_square = parse( query( pot.uid, t_pot, meat_at ) );
    check( refused( wrong_square, "not found" ), "wrong square: not found", wrong_square.error );
    const parsed stale = parse( query( cook_gone_uid.load(), t_pot, gone_at ) );
    check( cook_gone_uid.load() != 0 && refused( stale, "not found" ), "item gone (stale handle): not found",
           stale.error );
    const parsed far_away = parse( query( cook_far_uid.load(), t_pot, far_at ) );
    check( cook_far_uid.load() != 0 && refused( far_away, "out of reach" ), "8 squares away: out of reach",
           far_away.error );
    uint64_t rock = 0;
    for( const fp3d::item_handle_view &h : handles_at( rock_at ) ) {
        rock = h.type == t_rock ? h.uid : rock;
    }
    const parsed not_cont = parse( query( rock, t_rock, rock_at ) );
    check( rock != 0 && refused( not_cont, "not a container" ), "a rock: not a container", not_cont.error );

    // Posted during another prompt (the wait menu): they wait for the main loop; a 9th is refused; each result
    // is taken once.
    r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting again", "" );
        return false;
    }
    fp3d::ui_answer wait;
    wait.id = r->id;
    wait.action = "wait";
    fp3d::answer( wait );
    std::optional<fp3d::ui_request> prompt = fp3d::wait_request( 5000, r->id );
    std::vector<uint64_t> ids;
    uint64_t ninth = 1;
    if( prompt && !prompt->timed ) {
        for( size_t i = 0; i < fp3d::max_pending_queries; i++ ) {
            ids.push_back( fp3d::post_recipe_query( { pot.uid, t_pot }, fire_at ) );
        }
        ninth = fp3d::post_recipe_query( { pot.uid, t_pot }, fire_at );
    }
    std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );
    const bool held = fp3d::pending_queries() == fp3d::max_pending_queries && !ids.empty() &&
                      !fp3d::take_query_result( ids.front() );
    check( prompt && !prompt->timed && ninth == 0 && std::count( ids.begin(), ids.end(), uint64_t{ 0 } ) == 0,
           "a 9th pending query refused", prompt ? std::string( fp3d::kind_name( prompt->kind ) ) + " prompt" : "no prompt" );
    // Back to the main loop: cancel whatever the wait action asked.
    for( int i = 0; i < 10 && prompt && !prompt->timed; i++ ) {
        fp3d::ui_answer cancel;
        cancel.id = prompt->id;
        cancel.cancel = true;
        fp3d::answer( cancel );
        prompt = fp3d::wait_request( 5000, prompt->id );
    }
    int taken = 0;
    int again = 0;
    for( const uint64_t id : ids ) {
        const std::optional<std::string> res = fp3d::take_query_result( id );
        taken += res && parse( *res ).ok && parse( *res ).id == static_cast<int64_t>( id ) ? 1 : 0;
        again += fp3d::take_query_result( id ) ? 1 : 0;
    }
    check( held && prompt && prompt->timed && taken == static_cast<int>( ids.size() ),
           "held during a prompt, run by the main loop", std::to_string( taken ) + " results" );
    check( again == 0 && !fp3d::take_query_result( 999999 ), "a result is taken once", "" );
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

// Client-flown shots (fp3d/shot.h, contracts/ui_requests.md): CDDA's dispersion roll decides each projectile and
// the client's outcome can only lower it. Each case builds a scene on the sim thread (this function's own
// before-ask hook, which also runs the shared observe() and is put back at the end), queues "fire" (or "throw"),
// answers the target request with FIRE plus `shot` and each `shot` request as the case says, then checks game
// state and the shot counters. "Skilled" scenes (gun skills 10, steady aim, the zombie adjacent) make CDDA's
// roll hit, so a hit the client claims must land and a refused answer shows in the counters.
static bool run_combat_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  combat: %-44s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    struct scene {
        int zx = -1;                // the hostile zombie, squares from the avatar (0, 0 = none)
        int zy = 0;
        int sx = 0;                 // a second hostile zombie (0, 0 = none)
        int sy = 0;
        int fx = 0;                 // a friendly zombie (0, 0 = none)
        int fy = 0;
        int ax = 0;                 // where to aim (0, 0 = the hostile zombie)
        int ay = 0;
        int wx = 0;                 // a wall square (0, 0 = none)
        int wy = 0;
        int wall_row = 0;           // a concrete wall along this row, across the scene's width (0 = none)
        int rounds = -1;            // rounds in the gun (-1 = full)
        bool spare_mag = false;     // carry a full spare magazine
        bool second_mag = false;    // and a second one with 5 rounds (two stacks, so CDDA asks which)
        int skill = 10;             // gun skills
        bool steady = true;         // recoil 0; else at its max (no aim yet)
        std::string gun = "glock_19";
        std::string mode;           // gun mode, "" = its default
        std::string fault;          // a fault set on the gun ("" = none)
        std::string ammo;           // the gun's ammo, "" = 9mm
        bool spare_gun = false;     // also a jammed loaded `gun` in the pack
        bool ground_gun = false;    // and one lying a square west
        bool turret_shot = false;   // fire 3 rounds from a jammed `gun` nobody holds, as a vehicle turret does
        int moves = 0;              // moves left for the turn that fires (0 = a full turn)
        bool armed = true;          // wield a loaded `gun`; else empty hands (throwing wields the rock)
        bool rock = false;          // carry a rock to throw
        std::string melee;          // wield this item instead of `gun` ("" = none)
    };
    struct seen {
        int ammo = -1;
        int hp = -1;                // the hostile zombie's hp (-1: none or gone)
        double size = 0.0;          // its ranged target size
        int side_hp = -1;
        int friendly_hp = -1;
        double friendly_size = 0.0;
        uint64_t uid = 0;
        uint64_t side_uid = 0;
        uint64_t friendly_uid = 0;
        int rocks = 0;              // rocks lying in the scene's area
        bool has_plan = false;
        int64_t turn = 0;
        std::vector<std::string> messages;   // since the scene was set
        bool jammed = false;        // the wielded gun has a jam fault
        int moves = 0;
        uint64_t mag_uid = 0;       // the wielded gun's magazine, given a handle (snapshots don't list it)
        uint32_t mag_type = 0;
        int turret_fired = -1;      // rounds the turret_shot fired
    };
    struct state {
        std::mutex mx;
        std::optional<scene> want;  // the next scene to build
        seen last;                  // observed before the newest request
        uint64_t uids[3] = {};      // sim thread only
        int turret_fired = -1;      // sim thread only
    };
    const std::shared_ptr<state> st = std::make_shared<state>();
    const auto hook = [st]() {
        std::optional<scene> sc;
        // Only at the main loop's request: a prompt inside an action (a grab's torn pocket, say) comes while CDDA
        // still holds references to the avatar's effects and items, which building a scene would clear.
        if( fp3d::in_timed_hook() ) {
            std::lock_guard<std::mutex> lk( st->mx );
            sc.swap( st->want );
        }
        avatar &u = get_avatar();
        map &here = get_map();
        const tripoint_bub_ms p = u.pos_bub();
        if( sc ) {
            g->clear_zombies();
            // Seeing in the dark, so what the avatar sees doesn't hang on light left by an earlier test (a fire
            // burning on indoors, say, which the field clearing below puts out)
            if( !u.has_trait( trait_DEBUG_NIGHTVISION ) ) {
                u.set_mutation( trait_DEBUG_NIGHTVISION );
            }
            for( int dx = -10; dx <= 2; dx++ ) {
                for( int dy = -5; dy <= 5; dy++ ) {
                    const tripoint_bub_ms q = p + tripoint_rel_ms( dx, dy, 0 );
                    if( here.inbounds( q ) && !here.veh_at( q ) ) {
                        here.ter_set( q, ter_t_floor );
                        here.furn_set( q, furn_str_id::NULL_ID() );
                        // an earlier test's fire and its smoke, say (remove_field keeps the caches right)
                        std::vector<field_type_id> fields;
                        for( const auto &fd : here.field_at( q ) ) {
                            fields.push_back( fd.first );
                        }
                        for( const field_type_id &fd : fields ) {
                            here.remove_field( q, fd );
                        }
                        if( q != p ) {
                            here.i_clear( q );
                        }
                    }
                }
            }
            if( sc->wx != 0 || sc->wy != 0 ) {
                here.ter_set( p + tripoint_rel_ms( sc->wx, sc->wy, 0 ), ter_t_wall );
            }
            if( sc->wall_row != 0 ) {
                for( int dx = -10; dx <= 2; dx++ ) {
                    const tripoint_bub_ms q = p + tripoint_rel_ms( dx, sc->wall_row, 0 );
                    if( here.inbounds( q ) && !here.veh_at( q ) ) {
                        here.ter_set( q, ter_t_concrete_wall );
                    }
                }
            }
            // Walled in all round, so no bullet going on through a body, overshooting or glancing off a wall
            // leaves the scene to hurt someone outside it (who might then come for the avatar in a later scene).
            for( int dx = -11; dx <= 3; dx++ ) {
                for( int dy = -6; dy <= 6; dy++ ) {
                    if( dx != -11 && dx != 3 && dy != -6 && dy != 6 ) {
                        continue;
                    }
                    const tripoint_bub_ms q = p + tripoint_rel_ms( dx, dy, 0 );
                    if( here.inbounds( q ) && !here.veh_at( q ) ) {
                        here.furn_set( q, furn_str_id::NULL_ID() );
                        here.ter_set( q, ter_t_concrete_wall );
                    }
                }
            }
            u.set_all_parts_hp_to_max();
            u.clear_effects();
            for( const char *s : { "gun", "pistol", "smg", "shotgun", "rifle", "throw", "melee", "bashing",
                                   "stabbing", "cutting", "unarmed"
                                 } ) {
                u.set_skill_level( skill_id( s ), sc->skill );
            }
            u.recoil = sc->steady ? 0.0 : MAX_RECOIL;
            if( !sc->melee.empty() ) {
                u.set_wielded_item( item( itype_id( sc->melee ) ) );
            } else if( sc->armed ) {
                item gun( itype_id( sc->gun ) );
                gun.ammo_set( sc->ammo.empty() ? itype_9mm : itype_id( sc->ammo ), sc->rounds );
                if( !sc->mode.empty() ) {
                    gun.gun_set_mode( gun_mode_id( sc->mode ) );
                }
                if( !sc->fault.empty() ) {
                    gun.set_fault( fault_id( sc->fault ), true, false );
                }
                u.set_wielded_item( gun );
            } else {
                u.remove_weapon();
            }
            if( sc->rock ) {
                u.i_add( item( itype_rock ) );
            }
            if( sc->spare_mag ) {
                item mag( itype_glockmag );
                mag.ammo_set( itype_9mm );
                u.i_add( mag );
            }
            if( sc->second_mag ) {
                item mag( itype_glockmag );
                mag.ammo_set( itype_9mm, 5 );
                u.i_add( mag );
            }
            const auto jammed_gun = [&]() {
                item gun( itype_id( sc->gun ) );
                gun.ammo_set( itype_9mm );
                gun.set_fault( fault_id( "fault_stovepipe" ), true, false );
                return gun;
            };
            if( sc->spare_gun ) {
                u.i_add( jammed_gun() );
            }
            if( sc->ground_gun ) {
                here.add_item( p + tripoint_rel_ms( -1, 0, 0 ), jammed_gun() );
            }
            const auto place = [&]( int x, int y, bool friendly ) -> uint64_t {
                if( x == 0 && y == 0 ) {
                    return 0;
                }
                monster *m = g->place_critter_at( mon_zombie, p + tripoint_rel_ms( x, y, 0 ) );
                if( m == nullptr ) {
                    return 0;
                }
                if( friendly ) {
                    m->friendly = -1;
                }
                return fp3d::creature_uid( *m );
            };
            st->uids[0] = place( sc->zx, sc->zy, false );
            st->uids[1] = place( sc->sx, sc->sy, false );
            st->uids[2] = place( sc->fx, sc->fy, true );
            if( sc->moves > 0 ) {
                u.set_moves( sc->moves );
            }
            st->turret_fired = -1;
            if( sc->turret_shot ) {
                // turret_data::fire: Character::fire_gun with the turret's own gun, not the wielded one
                item base( itype_id( sc->gun ) );
                base.ammo_set( itype_9mm );
                base.set_fault( fault_id( "fault_stovepipe" ), true, false );
                here.build_map_cache( p.z() );
                st->turret_fired = u.fire_gun( here, p + tripoint_rel_ms( sc->zx, sc->zy, 0 ), 3, base );
            }
            Messages::clear_messages();
            here.build_map_cache( p.z() );
        }
        observe();
        seen o;
        if( item_location w = u.get_wielded_item() ) {
            o.ammo = w->ammo_remaining();
            o.jammed = !fp3d::jam_of( *w ).is_null();
            if( item *mag = w->magazine_current() ) {
                o.mag_uid = fp3d::item_handle( *mag );
                o.mag_type = fp3d::intern_id( fp3d::id_kind::itype, mag->typeId().str() );
            }
        }
        o.moves = u.get_moves();
        o.turret_fired = st->turret_fired;
        o.uid = st->uids[0];
        o.side_uid = st->uids[1];
        o.friendly_uid = st->uids[2];
        for( monster &m : g->all_monsters() ) {
            const uint64_t id = fp3d::creature_uid( m );
            if( id != 0 && id == o.uid ) {
                o.hp = m.get_hp();
                o.size = m.ranged_target_size();
            } else if( id != 0 && id == o.side_uid ) {
                o.side_hp = m.get_hp();
            } else if( id != 0 && id == o.friendly_uid ) {
                o.friendly_hp = m.get_hp();
                o.friendly_size = m.ranged_target_size();
            }
        }
        for( int dx = -10; dx <= 2; dx++ ) {
            for( int dy = -5; dy <= 5; dy++ ) {
                const tripoint_bub_ms q = p + tripoint_rel_ms( dx, dy, 0 );
                if( q != p && here.inbounds( q ) ) {
                    for( const item &it : here.i_at( q ) ) {
                        o.rocks += it.typeId() == itype_rock ? 1 : 0;
                    }
                }
            }
        }
        o.has_plan = fp3d::has_shot_plan();
        o.turn = to_turns<int64_t>( calendar::turn - calendar::turn_zero );
        for( const auto &m : Messages::recent_messages( 40 ) ) {
            o.messages.push_back( m.second );
        }
        std::lock_guard<std::mutex> lk( st->mx );
        st->last = std::move( o );
    };
    fp3d::set_before_ask_hook( hook );
    struct restore_hook {
        ~restore_hook() {
            fp3d::set_before_ask_hook( observe );
        }
    } restore_now;
    const auto seen_now = [st]() {
        std::lock_guard<std::mutex> lk( st->mx );
        return st->last;
    };

    // Who the client says its bullet hit, and the rest of its `shot`.
    enum class who { none, zombie, side, friendly, ghost };
    struct claim {
        who hit = who::zombie;
        int ex = 0;                 // a miss: where it stopped, squares from the avatar
        int ey = 0;
        std::string part = "head";
        float quality = 0.05f;
    };
    const auto at = []( const fp3d::ui_request & r, int dx, int dy ) {
        return fp3d::ui_point{ r.origin.x + dx, r.origin.y + dy, r.origin.z };
    };
    const auto make_shot = [&]( const claim & c, const scene & sc, const fp3d::ui_request & r, const seen & b,
    bool fly ) {
        fp3d::shot_answer s;
        s.part = c.part;
        s.quality = c.quality;
        s.fly = fly;
        switch( c.hit ) {
            case who::zombie:
                s.hit_uid = b.uid;
                s.end = at( r, sc.zx, sc.zy );
                break;
            case who::side:
                s.hit_uid = b.side_uid;
                s.end = at( r, sc.sx, sc.sy );
                break;
            case who::friendly:
                s.hit_uid = b.friendly_uid;
                s.end = at( r, sc.fx, sc.fy );
                break;
            case who::ghost:
                s.hit_uid = 0x7fffffffULL;
                s.end = at( r, sc.zx, sc.zy );
                break;
            case who::none:
                s.hit_uid = 0;
                s.end = at( r, c.ex, c.ey );
                break;
        }
        return s;
    };

    struct result {
        seen before;
        seen after;
        fp3d::shot_counts c0;
        fp3d::shot_counts c1;
        std::optional<fp3d::ui_request> target;     // the first target request
        std::vector<fp3d::ui_request> shots;        // the shot requests
        std::vector<fp3d::event> projectiles;       // projectile events from the action on
        bool back = false;                          // back in the main loop afterwards
    };
    // How the target request is answered: FIRE, an aim-and-shoot action, a cancel (all with the shot), or FIRE
    // while throwing.
    enum class answer_with { fire, aimed_shot, cancel };
    // Answer a shell's shot request for each pellet too: with `per` (each), or also a ghost, a hit off the line and
    // 3 more than asked (mixed).
    enum class pellets_with { none, each, mixed };
    pellets_with pellet_answers = pellets_with::none;
    // Builds `sc`, starts `action`, answers the target request with `with` and the FIRE answer's shot `plan` (with
    // `fly`), and each shot request with `per` (nullopt: cancel).
    const auto run = [&]( const scene & sc, const char *action, answer_with with, const claim & plan, bool fly,
    const std::optional<claim> &per ) {
        result r;
        // A stray prompt (a debug message, say) is printed and answered, and the scene built again.
        const auto settle = [&]( std::optional<fp3d::ui_request> s ) {
            for( int k = 0; k < 5 && s && !s->timed; k++ ) {
                fp3d::ui_answer a;
                a.id = s->id;
                if( s->kind == fp3d::request_kind::popup && !s->actions.empty() ) {
                    a.action = s->actions.front();
                } else {
                    a.cancel = true;
                }
                print_request( *s, "unexpected, " + ( a.cancel ? std::string( "cancel" ) : a.action ) );
                fp3d::answer( a );
                s = fp3d::wait_request( 20000, s->id );
            }
            return s;
        };
        std::optional<fp3d::ui_request> q = settle( fp3d::wait_request( 5000 ) );
        for( int attempt = 0; attempt < 3; attempt++ ) {
            if( !q || !q->timed ) {
                return r;
            }
            {
                std::lock_guard<std::mutex> lk( st->mx );
                st->want = sc;
            }
            fp3d::queue_action( "pause" );
            const std::optional<fp3d::ui_request> built = fp3d::wait_request( 20000, q->id );
            q = settle( built );
            if( built && built->timed ) {
                break;
            }
        }
        if( !q || !q->timed ) {
            return r;
        }
        r.before = seen_now();
        r.c0 = fp3d::get_shot_counts();
        {
            std::vector<fp3d::event> old;
            fp3d::drain_events( old );
        }
        fp3d::queue_action( action );
        std::optional<fp3d::ui_request> c = fp3d::wait_request( 20000, q->id );
        int n = 0;
        while( c && !c->timed && n++ < 40 ) {
            fp3d::ui_answer a;
            a.id = c->id;
            if( c->kind == fp3d::request_kind::target && !r.target ) {
                r.target = *c;
                a.pos = sc.ax != 0 || sc.ay != 0 ? at( *c, sc.ax, sc.ay ) : at( *c, sc.zx, sc.zy );
                a.shot = make_shot( plan, sc, *c, r.before, fly );
                a.cancel = with == answer_with::cancel;
                a.action = "FIRE";
                if( with == answer_with::aimed_shot ) {
                    for( const std::string &act : c->actions ) {
                        if( act.size() > 5 && act.compare( act.size() - 5, 5, "_SHOT" ) == 0 ) {
                            a.action = act;   // the last one: the most careful aim
                        }
                    }
                }
            } else if( c->kind == fp3d::request_kind::shot ) {
                r.shots.push_back( *c );
                if( per ) {
                    a.shot = make_shot( *per, sc, *c, r.before, false );
                    if( pellet_answers != pellets_with::none ) {
                        const fp3d::pellet_answer pa{ a.shot->end, a.shot->hit_uid, a.shot->part, a.shot->quality };
                        a.shot->pellets.assign( c->pellets.size(), pa );
                        if( pellet_answers == pellets_with::mixed && a.shot->pellets.size() >= 2 ) {
                            fp3d::pellet_answer ghost = pa;
                            ghost.hit_uid = 0x7fffffffULL;
                            fp3d::pellet_answer side = pa;
                            side.hit_uid = r.before.side_uid;
                            side.end = at( *c, sc.sx, sc.sy );
                            a.shot->pellets[0] = ghost;
                            a.shot->pellets[1] = side;
                            a.shot->pellets.insert( a.shot->pellets.end(), 3, pa );
                        }
                    }
                } else {
                    a.cancel = true;
                }
            } else if( c->kind == fp3d::request_kind::item_select ) {
                std::string label;
                a.entry = first_entry_with( *c, "rock", label );
                a.cancel = a.entry < 0;
                print_request( *c, a.cancel ? "cancel" : label );
            } else if( c->kind == fp3d::request_kind::popup && !c->actions.empty() ) {
                a.action = has( c->actions, "YES" ) ? "YES" : c->actions.front();
                print_request( *c, a.action );
            } else {
                a.cancel = true;   // aiming again after the shot, or anything else
                if( c->kind != fp3d::request_kind::target ) {
                    print_request( *c, "cancel" );
                }
            }
            fp3d::answer( a );
            const uint64_t id = c->id;
            c = fp3d::wait_request( 20000, id );
        }
        r.back = c && c->timed;
        r.after = seen_now();
        r.c1 = fp3d::get_shot_counts();
        std::vector<fp3d::event> evs;
        fp3d::drain_events( evs );
        for( fp3d::event &e : evs ) {
            if( e.kind == fp3d::event_kind::projectile ) {
                r.projectiles.push_back( std::move( e ) );
            }
        }
        return r;
    };
    const auto num = []( double v ) {
        char buf[32];
        std::snprintf( buf, sizeof( buf ), "%.1f", v );
        return std::string( buf );
    };
    const auto counts = [&]( const result & r ) {
        return "asked +" + std::to_string( r.c1.asked - r.c0.asked ) + " accepted +" +
               std::to_string( r.c1.accepted - r.c0.accepted ) + " refused +" +
               std::to_string( r.c1.refused - r.c0.refused ) + " planned +" +
               std::to_string( r.c1.planned - r.c0.planned ) + " parts +" + std::to_string( r.c1.parts - r.c0.parts ) +
               ", hp " + std::to_string( r.before.hp ) + " -> " + std::to_string( r.after.hp ) + ", ammo " +
               std::to_string( r.before.ammo ) + " -> " + std::to_string( r.after.ammo ) + ( r.back ? "" : ", stuck" );
    };
    const auto delta = []( uint64_t a, uint64_t b ) {
        return static_cast<int64_t>( b - a );
    };
    const auto fired = [&]( const result & r, int rounds ) {
        return r.back && r.after.ammo == r.before.ammo - rounds;
    };

    const scene point_blank{};          // a zombie adjacent to the west, skilled
    const claim head{};                 // hit the zombie in the head, quality 0.05

    // Accepted: the target request tells the spread, the shot request CDDA's roll; the client's hit lands.
    {
        const result r = run( point_blank, "fire", answer_with::fire, head, true, head );
        const fp3d::ui_request t = r.target.value_or( fp3d::ui_request() );
        check( r.target && t.mode == "fire" && std::isfinite( t.dispersion ) && t.dispersion > 0.0f &&
               t.dispersion_typical > 0.0f && t.weapon_wielded, "target request: dispersion, wielded",
               "dispersion " + num( t.dispersion ) + " typical " + num( t.dispersion_typical ) );
        const fp3d::ui_request s = r.shots.empty() ? fp3d::ui_request() : r.shots.front();
        check( r.shots.size() == 1 && s.kind == fp3d::request_kind::shot && std::isfinite( s.shot_error ) &&
               s.shot_error >= 0.0f && s.shot_angle >= 0.0f && s.shot_angle < 360.0f && s.dispersion > 0.0f &&
               s.range > 0, "shot request: error, angle, dispersion", std::to_string( r.shots.size() ) +
               " requests, error " + num( s.shot_error ) + " angle " + num( s.shot_angle ) + " max " +
               num( s.dispersion ) + " range " + std::to_string( s.range ) );
        check( fired( r, 1 ) && delta( r.c0.accepted, r.c1.accepted ) == 1 && r.after.hp < r.before.hp &&
               !r.after.has_plan, "accepted hit (point blank, head, 0.05)", counts( r ) );
    }
    // Refused shot answers leave the projectile to CDDA's own path, not the FIRE answer's outcome (CDDA's own roll
    // hits the adjacent zombie).
    const auto refused = [&]( const char *what, const scene & sc, const claim & per ) {
        const result r = run( sc, "fire", answer_with::fire, head, true, per );
        check( fired( r, 1 ) && r.shots.size() == 1 && delta( r.c0.refused, r.c1.refused ) == 1 &&
               delta( r.c0.accepted, r.c1.accepted ) == 0 && delta( r.c0.planned, r.c1.planned ) == 0 &&
               r.after.hp < r.before.hp, what, counts( r ) );
        return r;
    };
    {
        claim nan_quality = head;
        nan_quality.quality = std::nanf( "" );
        refused( "refused: NaN quality", point_blank, nan_quality );
        claim ghost = head;
        ghost.hit = who::ghost;
        refused( "refused: no such creature", point_blank, ghost );
        claim outside = { who::none, -500, 0, "", 0.99f };
        refused( "refused: end out of the bubble", point_blank, outside );
        claim long_shot = { who::none, -40, 0, "", 0.99f };
        refused( "refused: end beyond the gun's range", point_blank, long_shot );
        scene with_side = point_blank;
        with_side.sx = -3;
        with_side.sy = 4;
        claim side = head;
        side.hit = who::side;
        const result r = refused( "refused: a hit off CDDA's line", with_side, side );
        check( r.before.side_hp > 0 && r.after.side_hp == r.before.side_hp, "  the creature off the line unhurt",
               "hp " + std::to_string( r.before.side_hp ) + " -> " + std::to_string( r.after.side_hp ) );
    }
    // Cancelled shot requests too, even when the FIRE answer claimed a hit; a FIRE answer's miss off the line with
    // `fly` and a cancel: CDDA's own path hits the adjacent zombie. Without `fly` the plan's outcome never goes
    // beyond the first projectile of a burst.
    {
        const result r = run( point_blank, "fire", answer_with::fire, head, true, std::nullopt );
        check( fired( r, 1 ) && delta( r.c0.refused, r.c1.refused ) == 1 && delta( r.c0.planned, r.c1.planned ) == 0 &&
               r.after.hp < r.before.hp, "refused: cancel", counts( r ) );
        const claim beside = { who::none, -1, 1, "", 0.99f };
        int own_hits = 0;
        int own_runs = 0;
        int plans = 0;
        for( int i = 0; i < 3; i++ ) {
            const result o = run( point_blank, "fire", answer_with::fire, beside, true, std::nullopt );
            if( fired( o, 1 ) && delta( o.c0.refused, o.c1.refused ) == 1 ) {
                own_runs++;
                own_hits += o.after.hp < o.before.hp ? 1 : 0;
                plans += static_cast<int>( delta( o.c0.planned, o.c1.planned ) );
            }
        }
        check( own_runs == 3 && plans == 0 && own_hits >= 2, "fly cancel: CDDA's own path, not the FIRE miss",
               std::to_string( own_runs ) + " runs, " + std::to_string( own_hits ) + " hit, plans used " +
               std::to_string( plans ) );
        scene burst = point_blank;
        burst.gun = "hk_mp5";
        burst.mode = "BURST";
        const result b = run( burst, "fire", answer_with::fire, head, true, std::nullopt );
        check( fired( b, 3 ) && b.shots.size() == 3 && delta( b.c0.asked, b.c1.asked ) == 3 &&
               delta( b.c0.refused, b.c1.refused ) == 3 && delta( b.c0.planned, b.c1.planned ) == 0,
               "burst: a shot request per round", counts( b ) );
        const result n = run( burst, "fire", answer_with::fire, head, false, std::nullopt );
        check( fired( n, 3 ) && n.shots.empty() && delta( n.c0.planned, n.c1.planned ) == 1,
               "burst without fly: plan for the first round only", counts( n ) );
    }
    // A trigger pull asks for its first rounds only (the cap: 8 in play, 2 here): an MP5's 3-round burst with `fly`
    // sends 2 shot requests; the third round takes CDDA's own path, unasked.
    {
        scene burst = point_blank;
        burst.gun = "hk_mp5";
        burst.mode = "BURST";
        fp3d::set_asks_per_pull( 2 );
        const result r = run( burst, "fire", answer_with::fire, head, true, std::nullopt );
        fp3d::set_asks_per_pull( fp3d::default_asks_per_pull );
        check( fired( r, 3 ) && r.shots.size() == 2 && delta( r.c0.asked, r.c1.asked ) == 2 &&
               delta( r.c0.capped, r.c1.capped ) == 1, "burst past the request cap: CDDA's own path",
               counts( r ) + ", " + std::to_string( r.shots.size() ) + " shot requests" );
    }
    // A shotgun shell's shot request names each pellet's spread, and the answer for each pellet counts as a
    // projectile of its own: 00 buck at a zombie 4 squares west, every pellet claimed on it. Without pellets in the
    // answer (an older client) the whole shell spreads around its one outcome, as before.
    {
        scene shell = point_blank;
        shell.zx = -4;
        shell.gun = "remington_870";
        shell.ammo = "shot_00";
        pellet_answers = pellets_with::each;
        const result r = run( shell, "fire", answer_with::fire, head, true, head );
        pellet_answers = pellets_with::none;
        const fp3d::ui_request s = r.shots.empty() ? fp3d::ui_request() : r.shots.front();
        bool spreads = !s.pellets.empty() && s.pellets.size() <= fp3d::max_shot_pellets;
        for( const float v : s.pellets ) {
            spreads = spreads && std::isfinite( v ) && std::abs( v ) <= 1800.0f;
        }
        check( r.shots.size() == 1 && spreads, "shell: the shot request names each pellet",
               std::to_string( s.pellets.size() ) + " pellets" );
        const int64_t pellets = delta( r.c0.pellets, r.c1.pellets );
        check( fired( r, 1 ) && delta( r.c0.accepted, r.c1.accepted ) == 1 &&
               pellets == static_cast<int64_t>( s.pellets.size() ) && r.after.hp < r.before.hp,
               "shell: each pellet's answer checked and rolled", counts( r ) + ", pellets +" + std::to_string( pellets ) );
        const result o = run( shell, "fire", answer_with::fire, head, true, head );
        check( fired( o, 1 ) && delta( o.c0.accepted, o.c1.accepted ) == 1 && delta( o.c0.pellets, o.c1.pellets ) == 0 &&
               o.after.hp < o.before.hp, "shell: one outcome without pellets", counts( o ) );
        // a pellet naming a creature not offered, one hitting a creature off its line, and 3 more than asked:
        // those two are refused, the extra ones ignored
        scene mixed = shell;
        mixed.sx = -3;
        mixed.sy = 4;
        pellet_answers = pellets_with::mixed;
        const result m = run( mixed, "fire", answer_with::fire, head, true, head );
        pellet_answers = pellets_with::none;
        const size_t asked = m.shots.empty() ? 0 : m.shots.front().pellets.size();
        const int64_t passed = delta( m.c0.pellets, m.c1.pellets );
        check( fired( m, 1 ) && asked >= 3 && passed == static_cast<int64_t>( asked ) - 2 &&
               m.after.side_hp == m.before.side_hp, "shell: bad pellets refused, extra ones ignored",
               counts( m ) + ", pellets +" + std::to_string( passed ) + " of " + std::to_string( asked ) );
    }
    // A bullet with damage to spare goes on through the body it hit: 9mm (26) through a zombie (20) keeps 6.
    {
        scene in_line = point_blank;
        in_line.zx = -2;
        in_line.sx = -5;
        const fp3d::path_counts p0 = fp3d::get_path_counts();
        const result r = run( in_line, "fire", answer_with::fire, head, false, std::nullopt );
        const fp3d::path_counts p1 = fp3d::get_path_counts();
        // the event names the zombie hit and ends past it
        bool past = false;
        for( const fp3d::event &e : r.projectiles ) {
            past = past || ( r.target && e.other == r.before.uid &&
                             std::max( std::abs( e.pos.x - r.target->origin.x ), std::abs( e.pos.y - r.target->origin.y ) ) >= 3 );
        }
        check( fired( r, 1 ) && r.after.hp < r.before.hp && p1.through == p0.through + 1 && past,
               "bullet goes on through the body it hit", counts( r ) + ", through +" +
               std::to_string( p1.through - p0.through ) + ( past ? ", ends past it" : ", stops at it" ) );
    }
    // A bullet meeting a wall at a shallow angle may glance off it, never head-on: a concrete wall along the row 2
    // north, 16 shots at its square 10 west (11 degrees off its face), then 4 straight at it.
    {
        scene glance = point_blank;
        glance.zx = 0;
        glance.wall_row = -2;
        glance.ax = -10;
        glance.ay = -2;
        const claim at_wall = { who::none, -10, -2, "", 0.99f };
        const fp3d::path_counts p0 = fp3d::get_path_counts();
        int shots = 0;
        int bounced = 0;
        int flew_on = 0;    // ... to the west wall, 10 out: past the wall square it glanced off (8 or so west)
        for( int i = 0; i < 16; i++ ) {
            const uint64_t before = fp3d::get_path_counts().ricochets;
            const result r = run( glance, "fire", answer_with::fire, at_wall, false, std::nullopt );
            shots += fired( r, 1 ) ? 1 : 0;
            if( fp3d::get_path_counts().ricochets == before || !r.target ) {
                continue;
            }
            bounced++;
            for( const fp3d::event &e : r.projectiles ) {
                if( std::max( std::abs( e.pos.x - r.target->origin.x ), std::abs( e.pos.y - r.target->origin.y ) ) >= 10 ) {
                    flew_on++;
                    break;
                }
            }
        }
        const fp3d::path_counts p1 = fp3d::get_path_counts();
        scene head_on = glance;
        head_on.ax = 0;
        const claim north = { who::none, 0, -2, "", 0.99f };
        int straight = 0;
        for( int i = 0; i < 4; i++ ) {
            straight += fired( run( head_on, "fire", answer_with::fire, north, false, std::nullopt ), 1 ) ? 1 : 0;
        }
        const fp3d::path_counts p2 = fp3d::get_path_counts();
        check( shots == 16 && bounced > 0 && p1.ricochets == p0.ricochets + bounced && flew_on == bounced,
               "bullet glances off a wall and flies on", std::to_string( shots ) + " shots, " +
               std::to_string( bounced ) + " ricochets, " + std::to_string( flew_on ) + " flew on" );
        check( straight == 4 && p2.ricochets == p1.ricochets, "  never head-on",
               std::to_string( straight ) + " shots, " + std::to_string( p2.ricochets - p1.ricochets ) + " ricochets" );
    }
    // The client's part only weights CDDA's pick (QA): point blank, skilled, every hit claimed on a leg dead
    // centre; the leg lands on some of them, not on all (a roll against its chance times 4). A leg (15% cover, not
    // the head's 5%), so neither none nor all of 16 is a likely roll.
    {
        const claim dead_head = { who::zombie, 0, 0, "leg", 0.0f };
        int hits = 0;
        int landed = 0;
        for( int i = 0; i < 16; i++ ) {
            const result r = run( point_blank, "fire", answer_with::fire, dead_head, true, dead_head );
            if( !fired( r, 1 ) || delta( r.c0.accepted, r.c1.accepted ) != 1 ) {
                continue;
            }
            hits++;
            landed += delta( r.c0.parts, r.c1.parts ) > 0 ? 1 : 0;
        }
        check( hits >= 10 && landed > 0 && landed < hits, "client part weights CDDA's pick",
               "leg landed on " + std::to_string( landed ) + " of " + std::to_string( hits ) + " claimed hits" );
    }
    // Parts: unknown or junk ids are ignored (CDDA picks), the hit still counts.
    {
        claim unknown = head;
        unknown.part = "zz_no_such_part";
        const result r = run( point_blank, "fire", answer_with::fire, head, true, unknown );
        check( fired( r, 1 ) && delta( r.c0.accepted, r.c1.accepted ) == 1 && delta( r.c0.parts, r.c1.parts ) == 0 &&
               r.after.hp < r.before.hp, "unknown part ignored", counts( r ) );
        claim junk = head;
        junk.part = std::string( 32, '#' ) + std::string( 32, '\xff' );
        const result j = run( point_blank, "fire", answer_with::fire, head, true, junk );
        check( fired( j, 1 ) && delta( j.c0.accepted, j.c1.accepted ) == 1 && delta( j.c0.parts, j.c1.parts ) == 0 &&
               j.after.hp < j.before.hp, "64-byte junk part ignored", counts( j ) );
    }
    // The FIRE answer alone (no fly): a hit lands; a miss ending off the zombie's line doesn't turn the bullet: it
    // flies the aim line (stopping at the zombie or flying on along it), never toward the client's end.
    {
        const result r = run( point_blank, "fire", answer_with::fire, head, false, std::nullopt );
        check( fired( r, 1 ) && r.shots.empty() && delta( r.c0.planned, r.c1.planned ) == 1 &&
               r.after.hp < r.before.hp, "FIRE answer: hit", counts( r ) );
        scene three = point_blank;
        three.zx = -3;
        const claim side_miss = { who::none, -2, 1, "", 0.99f };
        const result m = run( three, "fire", answer_with::fire, side_miss, false, std::nullopt );
        const int end_dy = m.projectiles.empty() || !m.target ? 99 : m.projectiles.front().pos.y - m.target->origin.y;
        check( fired( m, 1 ) && delta( m.c0.planned, m.c1.planned ) == 1 && end_dy == 0,
               "FIRE answer: a miss keeps the aim line", "end dy " + std::to_string( end_dy ) + ", " + counts( m ) );
    }
    // Nothing stored: a cancel, a throw, a hit on a creature the request didn't offer, an outcome from an earlier turn.
    {
        const result c = run( point_blank, "fire", answer_with::cancel, head, true, std::nullopt );
        check( c.back && c.after.ammo == c.before.ammo && c.shots.empty() && !c.after.has_plan &&
               delta( c.c0.planned, c.c1.planned ) == 0 && c.after.hp == c.before.hp, "cancel with a shot: no plan",
               counts( c ) );
        scene throw_scene = point_blank;
        throw_scene.zx = -3;
        throw_scene.armed = false;
        throw_scene.rock = true;
        const result t = run( throw_scene, "throw", answer_with::fire, head, true, std::nullopt );
        check( t.back && t.target && t.target->mode == "throw" && t.after.rocks > t.before.rocks &&
               t.shots.empty() && !t.after.has_plan && delta( t.c0.planned, t.c1.planned ) == 0,
               "throw with a shot: ignored", "rocks lying " + std::to_string( t.before.rocks ) + " -> " +
               std::to_string( t.after.rocks ) + ", " + counts( t ) );
        // a friendly zombie a square beside the line behind the hostile one: near enough for the claim, but out of
        // the way of a bullet going on through the hostile
        scene with_friend = point_blank;
        with_friend.zx = -3;
        with_friend.fx = -5;
        with_friend.fy = 1;
        claim friend_hit = head;
        friend_hit.hit = who::friendly;
        const result f = run( with_friend, "fire", answer_with::fire, friend_hit, true, friend_hit );
        check( fired( f, 1 ) && delta( f.c0.refused, f.c1.refused ) == 1 && delta( f.c0.planned, f.c1.planned ) == 0 &&
               f.before.friendly_hp > 0 && f.after.friendly_hp == f.before.friendly_hp,
               "hit on a creature not offered: ignored", "friendly hp " + std::to_string( f.before.friendly_hp ) +
               " -> " + std::to_string( f.after.friendly_hp ) + ", " + counts( f ) );
        scene slow = point_blank;           // no zombie; aiming from scratch with 2 moves left
        slow.zx = 0;
        slow.ax = -5;
        slow.steady = false;
        slow.moves = 2;
        const claim on_line = { who::none, -4, 0, "", 0.99f };
        const result e = run( slow, "fire", answer_with::aimed_shot, on_line, true, std::nullopt );
        const int64_t shot_turn = e.shots.empty() ? -1 : e.shots.front().turn;
        const int64_t aim_turn = e.target ? e.target->turn : -1;
        check( fired( e, 1 ) && shot_turn > aim_turn && delta( e.c0.planned, e.c1.planned ) == 0 &&
               delta( e.c0.asked, e.c1.asked ) == 1, "multi-turn aim: shot request on the firing turn",
               "aimed on turn " + std::to_string( aim_turn ) + ", fired on " + std::to_string( shot_turn ) + ", " +
               counts( e ) );
        const result l = run( slow, "fire", answer_with::aimed_shot, on_line, false, std::nullopt );
        const int64_t l_aim = l.target ? l.target->turn : -1;
        check( fired( l, 1 ) && l.shots.empty() && l.after.turn > l_aim && delta( l.c0.planned, l.c1.planned ) == 0,
               "outcome from an earlier turn dropped", "aimed on turn " + std::to_string( l_aim ) + ", back on " +
               std::to_string( l.after.turn ) + ", " + counts( l ) );
    }
    // Unskilled and unaimed at 8 squares, the client claiming a dead-centre head shot every time: CDDA's roll
    // decides, so these can't all be critical head shots.
    {
        scene far_zombie;
        far_zombie.zx = -8;
        far_zombie.skill = 0;
        far_zombie.steady = false;
        const claim dead_centre = { who::zombie, 0, 0, "head", 0.0f };
        int shots = 0;
        int crits = 0;
        int hits = 0;
        int malfunctions = 0;
        for( int i = 0; i < 20; i++ ) {
            const result r = run( far_zombie, "fire", answer_with::fire, dead_centre, false, std::nullopt );
            if( !fired( r, 1 ) ) {
                // an unskilled shooter's gun malfunctions now and then (CDDA's roll): that shot doesn't count
                for( const std::string &m : r.after.messages ) {
                    if( m.find( "malfunctions" ) != std::string::npos ) {
                        malfunctions++;
                        break;
                    }
                }
                continue;
            }
            shots++;
            hits += r.after.hp < r.before.hp ? 1 : 0;
            bool crit = false;
            for( const std::string &m : r.after.messages ) {
                crit = crit || m.find( "Critical" ) != std::string::npos || m.find( "in the head" ) != std::string::npos;
            }
            crits += crit ? 1 : 0;
        }
        check( shots + malfunctions == 20 && shots >= 15 && crits < shots, "20 claimed head shots: CDDA's roll decides",
               std::to_string( shots ) + " fired, " + std::to_string( malfunctions ) + " malfunctioned, " +
               std::to_string( hits ) + " hit, " + std::to_string( crits ) + " critical / head" );
    }
    const auto outcomes = [&]( const result & r ) {
        return delta( r.c0.accepted, r.c1.accepted ) + delta( r.c0.planned, r.c1.planned );
    };
    // CDDA's miss stays a miss: unskilled and unaimed, the creature 5 squares west on the aimed square, the client
    // claims a miss that ends on its square or flies over it and lands 2 squares behind. Where the shot request's
    // error makes CDDA's own roll a miss (iso_tangent(5, error) over the creature's size), it is never hurt: a
    // hostile zombie, and a friendly one (aimed at, the confirm popup answered yes).
    struct miss_tally {
        int used = 0;
        int on = 0;                 // CDDA misses with the claim ending on the creature's square
        int past = 0;               // CDDA misses with the claim past it
        int hurt_on_miss = 0;
        int hurt = 0;
    };
    const auto over_cdda_miss = [&]( bool friendly ) {
        miss_tally t;
        scene sc;
        sc.skill = 0;
        sc.steady = false;
        if( friendly ) {
            sc.zx = 0;
            sc.fx = -5;
            sc.ax = -5;
        } else {
            sc.zx = -5;
        }
        for( int i = 0; i < 40 && ( t.on < 2 || t.past < 2 ); i++ ) {
            const bool on_square = t.on < 2 && ( t.past >= 2 || i % 2 == 0 );
            const claim c = { who::none, on_square ? -5 : -7, 0, "", 0.99f };
            const result r = run( sc, "fire", answer_with::fire, c, true, c );
            const int hp0 = friendly ? r.before.friendly_hp : r.before.hp;
            const int hp1 = friendly ? r.after.friendly_hp : r.after.hp;
            const double size = friendly ? r.before.friendly_size : r.before.size;
            if( !fired( r, 1 ) || r.shots.size() != 1 || hp0 <= 0 || !( size > 0.0 ) ||
                delta( r.c0.accepted, r.c1.accepted ) != 1 ) {
                continue;
            }
            t.used++;
            const bool was_hurt = hp1 < hp0;
            t.hurt += was_hurt ? 1 : 0;
            const double tiles = iso_tangent( 5.0, units::from_arcmin( r.shots.front().shot_error ) );
            if( tiles / size >= 1.01 ) {
                ( on_square ? t.on : t.past )++;
                t.hurt_on_miss += was_hurt ? 1 : 0;
            }
        }
        return t;
    };
    for( const bool friendly : { false, true } ) {
        const miss_tally t = over_cdda_miss( friendly );
        check( t.on >= 2 && t.past >= 2 && t.hurt_on_miss == 0, friendly ?
               "client miss over CDDA's miss: friendly unhurt" : "client miss over CDDA's miss: no hit",
               std::to_string( t.used ) + " used, CDDA misses " + std::to_string( t.on ) + " ending on it + " +
               std::to_string( t.past ) + " past it, hurt on those " + std::to_string( t.hurt_on_miss ) +
               ", hurt in all " + std::to_string( t.hurt ) );
    }
    // A client hit on a creature off the aimed square gets that creature's own roll (its square's distance off
    // CDDA's deviated line, with the error's vertical part over the distance along it, against its size): aiming
    // at a wall with the zombie beside it, unskilled, a claimed dead-centre head shot on the zombie lands no
    // critical and no head part when that roll is 0.25 or worse (a critical needs under 0.2, the head part under
    // sqrt(its 5% chance) ~ 0.22).
    {
        scene wall_aim;
        wall_aim.zx = -3;
        wall_aim.zy = 1;
        wall_aim.wx = -3;
        wall_aim.wy = 0;
        wall_aim.ax = -3;
        wall_aim.ay = 0;
        wall_aim.skill = 0;
        const claim dead_centre = { who::zombie, 0, 0, "head", 0.0f };
        int used = 0;
        int poor = 0;
        int bad = 0;
        int hits = 0;
        std::string owns;
        for( int i = 0; i < 24 && poor < 4; i++ ) {
            const result r = run( wall_aim, "fire", answer_with::fire, dead_centre, true, dead_centre );
            if( !fired( r, 1 ) || r.shots.size() != 1 || delta( r.c0.accepted, r.c1.accepted ) != 1 ||
                !( r.before.size > 0.0 ) ) {
                continue;
            }
            used++;
            // the contract's roll: aim line west (-1, 0) turned by err * sin(angle) to the right, the zombie at (-3, 1)
            const double err = r.shots.front().shot_error;
            const double angle = units::to_radians( units::from_degrees( r.shots.front().shot_angle ) );
            const double h = units::to_radians( units::from_arcmin( err * std::sin( angle ) ) );
            const double vx = -std::cos( h );
            const double vy = -std::sin( h );
            const double along_line = std::max( -3.0 * vx + 1.0 * vy, 0.0 );
            const double off = std::abs( -3.0 * vy - 1.0 * vx );
            const double vert = iso_tangent( along_line, units::from_arcmin( std::abs( err * std::cos( angle ) ) ) );
            // and never better than CDDA's roll for an unintended hit (at least 0.4)
            const double own = std::min( 1.0, std::max( std::hypot( off, vert ) / r.before.size, 0.4 ) );
            char own_text[32];
            std::snprintf( own_text, sizeof( own_text ), " %.2f", own );
            owns += own_text;
            hits += r.after.hp < r.before.hp ? 1 : 0;
            if( own >= 0.25 ) {
                poor++;
                bool crit = delta( r.c0.parts, r.c1.parts ) > 0;
                for( const std::string &m : r.after.messages ) {
                    crit = crit || m.find( "Critical" ) != std::string::npos;
                }
                bad += crit ? 1 : 0;
            }
        }
        check( poor >= 3 && bad == 0, "client hit off the aimed square: its own roll",
               std::to_string( used ) + " used, own rolls" + owns + ", " + std::to_string( hits ) + " hit, " +
               std::to_string( bad ) + " critical / head part at 0.25 or worse" );
    }
    // A client miss on open ground keeps CDDA's overshoot: claimed a square short of the zombie (another one 3
    // squares behind it), the projectile flies on past that square. A miss that ends on a wall stops there.
    {
        scene in_line = point_blank;
        in_line.zx = -3;
        in_line.sx = -6;
        const claim short_miss = { who::none, -2, 0, "", 0.99f };
        int used = 0;
        int past = 0;
        int front_hits = 0;
        int behind_hits = 0;
        std::string ends;
        for( int i = 0; i < 6; i++ ) {
            const result r = run( in_line, "fire", answer_with::fire, short_miss, true, short_miss );
            if( !fired( r, 1 ) || !r.target || r.projectiles.empty() || outcomes( r ) != 1 ) {
                continue;
            }
            used++;
            const int dx = r.projectiles.front().pos.x - r.target->origin.x;
            ends += " " + std::to_string( dx );
            past += dx < -2 ? 1 : 0;
            front_hits += r.after.hp < r.before.hp ? 1 : 0;
            behind_hits += r.after.side_hp < r.before.side_hp ? 1 : 0;
        }
        check( used >= 3 && past == used, "client miss on open ground: overshoot flies on",
               std::to_string( used ) + " used, ends x" + ends + ", zombie hit " + std::to_string( front_hits ) +
               ", the one behind hit " + std::to_string( behind_hits ) );
        // A miss ending on a wall on CDDA's line stops there (aimed at the wall, a zombie behind it); one ending on a
        // wall beside the line doesn't turn or stop the bullet: it flies on past that wall's distance.
        const auto wall_runs = [&]( const scene & sc, const claim & c, int &w_used ) {
            std::vector<int> ds;
            for( int i = 0; i < 2; i++ ) {
                const result r = run( sc, "fire", answer_with::fire, c, true, c );
                if( !fired( r, 1 ) || !r.target || r.projectiles.empty() || outcomes( r ) != 1 ) {
                    continue;
                }
                w_used++;
                const fp3d::ui_point &e = r.projectiles.front().pos;
                ds.push_back( std::max( std::abs( e.x - r.target->origin.x ), std::abs( e.y - r.target->origin.y ) ) );
                if( r.after.hp < r.before.hp ) {
                    ds.back() += 100;   // the zombie behind the wall hurt
                }
            }
            return ds;
        };
        scene walled = point_blank;
        walled.zx = -3;
        walled.wx = -2;
        walled.ax = -2;
        const claim wall_miss = { who::none, -2, 0, "", 0.99f };
        int on_used = 0;
        std::string on_ends;
        int stopped = 0;
        for( const int d : wall_runs( walled, wall_miss, on_used ) ) {
            on_ends += " " + std::to_string( d );
            stopped += d <= 2 ? 1 : 0;
        }
        check( on_used >= 1 && stopped == on_used, "client miss on a wall on the line: stops there",
               std::to_string( on_used ) + " used, end distances" + on_ends );
        scene beside = point_blank;
        beside.zx = -3;
        beside.wx = -2;
        beside.wy = 1;
        const claim beside_miss = { who::none, -2, 1, "", 0.99f };
        int off_used = 0;
        std::string off_ends;
        int flew_on = 0;
        for( const int d : wall_runs( beside, beside_miss, off_used ) ) {
            off_ends += " " + std::to_string( d );
            flew_on += d % 100 >= 3 ? 1 : 0;
        }
        check( off_used >= 1 && flew_on == off_used, "client miss on a wall beside the line: flies on",
               std::to_string( off_used ) + " used, end distances" + off_ends + " (+100: the zombie hurt)" );
    }
    // A miss never picks the direction (lead-bridge's case): aiming at a zombie 6 squares west, unskilled, with a
    // friendly zombie on the diagonal at (-4, -4) that was never aimed at, the client claims a miss ending at (-1, -1),
    // a square off the aim line. The bullet keeps CDDA's line (within 30 degrees of the aim, as CDDA's wild misses
    // are), so the friendly is never hurt; with the claim's diagonal it was hit on most CDDA misses.
    {
        scene diagonal;
        diagonal.zx = -6;
        diagonal.fx = -4;
        diagonal.fy = -4;
        diagonal.skill = 0;
        diagonal.steady = false;
        const claim corner = { who::none, -1, -1, "", 0.99f };
        int used = 0;
        int friend_hurt = 0;
        int zombie_hurt = 0;
        for( int i = 0; i < 10; i++ ) {
            const bool fly = i < 6;
            const result r = run( diagonal, "fire", answer_with::fire, corner, fly, corner );
            if( !fired( r, 1 ) || r.before.friendly_hp <= 0 ) {
                continue;
            }
            used++;
            friend_hurt += r.after.friendly_hp < r.before.friendly_hp ? 1 : 0;
            zombie_hurt += r.after.hp < r.before.hp ? 1 : 0;
        }
        check( used >= 8 && friend_hurt == 0, "client miss off the line: the bullet keeps CDDA's line",
               std::to_string( used ) + " fired (6 fly, 4 FIRE answer), friendly on the diagonal hurt " +
               std::to_string( friend_hurt ) + ", the aimed zombie hurt " + std::to_string( zombie_hurt ) );
    }
    // CDDA's wild miss isn't the client's to place: aiming at an empty square 8 west, unskilled, the client claims a
    // miss ending on the cursor every time. Where CDDA's roll is a square or more off there, CDDA's wild-miss path
    // runs: turned by its whole error (30 degrees at most, as CDDA's own), to the side the shot request showed
    // (sin(shot_angle) > 0: the shooter's right, north when facing west), at CDDA's scattered distance.
    {
        scene empty_aim;
        empty_aim.zx = 0;
        empty_aim.ax = -8;
        empty_aim.skill = 0;
        empty_aim.steady = false;
        const claim on_cursor = { who::none, -8, 0, "", 0.99f };
        int wild = 0;
        int wild_on_cursor = 0;
        int placed = 0;
        std::string ends;
        for( int i = 0; i < 16 && wild < 6; i++ ) {
            const result r = run( empty_aim, "fire", answer_with::fire, on_cursor, true, on_cursor );
            if( !fired( r, 1 ) || r.shots.size() != 1 || !r.target || r.projectiles.empty() ) {
                continue;
            }
            if( iso_tangent( 8.0, units::from_arcmin( r.shots.front().shot_error ) ) < 1.0 ) {
                continue;
            }
            wild++;
            const fp3d::ui_point &e = r.projectiles.front().pos;
            const int ex = e.x - r.target->origin.x;
            const int ey = e.y - r.target->origin.y;
            ends += " (" + std::to_string( ex ) + "," + std::to_string( ey ) + ")";
            wild_on_cursor += ex == -8 && ey == 0 ? 1 : 0;
            const double deg = 180.0 / M_PI;
            const double want = std::min( r.shots.front().shot_error / 60.0, 30.0 );
            const double dist = std::hypot( ex, ey );
            const double got = dist > 0.0 ? std::atan2( std::abs( ey ), -ex ) * deg : 0.0;
            const double slack = dist > 0.0 ? std::atan2( 1.0, dist ) * deg + 2.0 : 180.0;
            const double sin_angle = std::sin( r.shots.front().shot_angle / deg );
            const bool side_ok = ey == 0 || ( sin_angle >= 0.0 ? ey < 0 : ey > 0 );
            placed += std::abs( got - want ) <= slack && side_ok ? 1 : 0;
        }
        check( wild >= 3 && wild_on_cursor < wild && placed == wild, "client miss on CDDA's wild miss: CDDA places it",
               std::to_string( wild ) + " wild misses, " + std::to_string( placed ) + " at CDDA's angle and the "
               "request's side, on the cursor " + std::to_string( wild_on_cursor ) + ", ends" + ends );
    }
    // One roll per creature: skilled and steady, aiming at a zombie 8 west with another hostile one on the line
    // at 4 west, the client claims the near one every time. When that creature's own roll misses, CDDA's path runs
    // and meets it without rolling it again (spared): it stays unhurt in those runs.
    {
        scene two_in_line = point_blank;
        two_in_line.zx = -8;
        two_in_line.sx = -4;
        const claim near_one = { who::side, 0, 0, "", 0.5f };
        int used = 0;
        int spared_runs = 0;
        int hurt_when_spared = 0;
        int hits = 0;
        for( int i = 0; i < 10; i++ ) {
            const result r = run( two_in_line, "fire", answer_with::fire, near_one, true, near_one );
            if( !fired( r, 1 ) || r.before.side_hp <= 0 || delta( r.c0.accepted, r.c1.accepted ) != 1 ) {
                continue;
            }
            used++;
            const bool hurt = r.after.side_hp < r.before.side_hp;
            if( delta( r.c0.spared, r.c1.spared ) > 0 ) {
                spared_runs++;
                hurt_when_spared += hurt ? 1 : 0;
            } else {
                hits += hurt ? 1 : 0;
            }
        }
        check( spared_runs >= 2 && hurt_when_spared == 0, "a claim's missed roll: no second roll",
               std::to_string( used ) + " used, spared " + std::to_string( spared_runs ) + " (hurt then " +
               std::to_string( hurt_when_spared ) + "), claimed hits landed " + std::to_string( hits ) );
    }
    // A hit claimed on a creature a square off CDDA's line (lead-bridge's case): skilled and steady, aiming at a
    // zombie 8 squares west, the client claims a dead-centre hit on another zombie at (-3, 1), close by. Its own
    // roll is its square's distance off the line against its size, so it misses and CDDA's own path (along the
    // aim line, never through that square) runs: it stays unhurt.
    {
        scene near_side;
        near_side.zx = -8;
        near_side.sx = -3;
        near_side.sy = 1;
        claim side_hit = { who::side, 0, 0, "head", 0.0f };
        int used = 0;
        int side_hurt = 0;
        for( int i = 0; i < 3; i++ ) {
            const result r = run( near_side, "fire", answer_with::fire, side_hit, true, side_hit );
            if( !fired( r, 1 ) || r.before.side_hp <= 0 || delta( r.c0.accepted, r.c1.accepted ) != 1 ) {
                continue;
            }
            used++;
            side_hurt += r.after.side_hp < r.before.side_hp ? 1 : 0;
        }
        check( used >= 2 && side_hurt == 0, "client hit a square off CDDA's line: its own roll misses",
               std::to_string( used ) + " used, the zombie off the line hurt " + std::to_string( side_hurt ) );
    }
    // The main-loop reload through the action queue: a pistol with 3 rounds and two different spare magazines
    // carried, so CDDA's ammo selector asks (an item_select, not CDDA's own menu); it gets its first entry.
    {
        scene reload = point_blank;
        reload.zx = 0;
        reload.rounds = 3;
        reload.spare_mag = true;
        reload.second_mag = true;
        seen before;
        seen after;
        bool back = false;
        int asked = 0;
        int selects = 0;
        std::optional<fp3d::ui_request> q = fp3d::wait_request( 5000 );
        if( q && q->timed ) {
            {
                std::lock_guard<std::mutex> lk( st->mx );
                st->want = reload;
            }
            fp3d::queue_action( "pause" );
            q = fp3d::wait_request( 20000, q->id );
        }
        if( q && q->timed ) {
            before = seen_now();
            fp3d::queue_action( "reload_weapon" );
            std::optional<fp3d::ui_request> c = fp3d::wait_request( 20000, q->id );
            while( c && !c->timed && asked++ < 20 ) {
                fp3d::ui_answer a;
                a.id = c->id;
                std::string note = "cancel";
                if( c->kind == fp3d::request_kind::item_select || c->kind == fp3d::request_kind::menu ) {
                    selects += c->kind == fp3d::request_kind::item_select ? 1 : 0;
                    for( const fp3d::ui_entry &e : c->entries ) {
                        if( e.enabled ) {
                            a.entry = e.id;
                            note = e.label;
                            break;
                        }
                    }
                    a.cancel = a.entry < 0;
                } else if( c->kind == fp3d::request_kind::popup && !c->actions.empty() ) {
                    a.action = has( c->actions, "YES" ) ? "YES" : c->actions.front();
                    note = a.action;
                } else {
                    a.cancel = true;
                }
                print_request( *c, note );
                fp3d::answer( a );
                const uint64_t id = c->id;
                c = fp3d::wait_request( 20000, id );
            }
            back = c && c->timed;
            after = seen_now();
        }
        check( back && before.ammo == 3 && after.ammo > before.ammo && selects >= 1,
               "main-loop reload_weapon: ammo asked, rounds go up", "ammo " + std::to_string( before.ammo ) + " -> " +
               std::to_string( after.ammo ) + ", " + std::to_string( asked ) + " requests, " +
               std::to_string( selects ) + " item_select" + ( back ? "" : ", stuck" ) );
    }
    // Jams (block 3). A jammed gun isn't aimed: a message, no random quick fix at aim start, nothing fired.
    const auto said = []( const seen & s, const char *text ) {
        for( const std::string &m : s.messages ) {
            if( m.find( text ) != std::string::npos ) {
                return true;
            }
        }
        return false;
    };
    {
        scene jammed = point_blank;
        jammed.fault = "fault_stovepipe";
        const result r = run( jammed, "fire", answer_with::fire, head, true, head );
        check( r.back && !r.target && r.before.jammed && r.after.jammed && r.after.ammo == r.before.ammo &&
               r.after.hp == r.before.hp && said( r.after, "jammed" ), "jammed gun: aim refused, no quick fix",
               counts( r ) + ( r.after.jammed ? ", still jammed" : ", cleared" ) );
    }
    // A turret's gun isn't the wielded one: a jam on it doesn't stop its fire (R and `clear` can't reach it), as in
    // CDDA. Fired through Character::fire_gun as turret_data::fire does, 3 rounds from a jammed Glock nobody holds.
    {
        scene turret = point_blank;
        turret.zx = -4;
        turret.turret_shot = true;
        std::optional<fp3d::ui_request> q = fp3d::wait_request( 5000 );
        if( q && q->timed ) {
            {
                std::lock_guard<std::mutex> lk( st->mx );
                st->want = turret;
            }
            fp3d::queue_action( "pause" );
            q = fp3d::wait_request( 20000, q->id );
        }
        const seen t = seen_now();
        check( q && q->timed && t.turret_fired == 3 && !said( t, "is jammed" ),
               "turret's jammed gun fires on (not wielded)", "fired " + std::to_string( t.turret_fired ) );
    }
    // A burst stops at a jam from an earlier round: black-powder .300 BLK (recoil 1520) doesn't cycle a SIG MCX
    // Rattler (1700 needed), so CDDA leaves the first round's case in the chamber on full auto; its loop fired on
    // regardless, the avatar's now stops there.
    {
        scene burst = point_blank;
        burst.gun = "sig_mcx_rattler_sbr";
        burst.ammo = "bp_300blk";
        burst.mode = "AUTO";
        const result r = run( burst, "fire", answer_with::fire, head, true, std::nullopt );
        check( fired( r, 1 ) && r.shots.size() == 1 && r.after.jammed && said( r.after, "fails to cycle" ) &&
               said( r.after, "jammed" ), "burst stops at a jam", counts( r ) + ", " +
               std::to_string( r.shots.size() ) + " shot requests" + ( r.after.jammed ? ", jammed" : "" ) );
    }
    // Reload (R) on a jammed gun clears it instead: CDDA's mend with the fault's fix and its time (10 s), no reload
    // (3 rounds stay, a spare magazine carried), the jam gone. No zombie, so nothing interrupts it.
    {
        scene clear = point_blank;
        clear.zx = 0;
        clear.rounds = 3;
        clear.spare_mag = true;
        clear.fault = "fault_stovepipe";
        seen before;
        seen after;
        bool back = false;
        int asked = 0;
        std::optional<fp3d::ui_request> q = fp3d::wait_request( 5000 );
        if( q && q->timed ) {
            {
                std::lock_guard<std::mutex> lk( st->mx );
                st->want = clear;
            }
            fp3d::queue_action( "pause" );
            q = fp3d::wait_request( 20000, q->id );
        }
        if( q && q->timed ) {
            before = seen_now();
            fp3d::queue_action( "reload_weapon" );
            std::optional<fp3d::ui_request> c = fp3d::wait_request( 30000, q->id );
            while( c && !c->timed && asked++ < 10 ) {
                fp3d::ui_answer a;
                a.id = c->id;
                a.cancel = true;
                print_request( *c, "unexpected, cancel" );
                fp3d::answer( a );
                const uint64_t id = c->id;
                c = fp3d::wait_request( 20000, id );
            }
            back = c && c->timed;
            after = seen_now();
        }
        const int64_t took = after.turn - before.turn;
        check( back && asked == 0 && before.jammed && !after.jammed && before.ammo == 3 && after.ammo == 3 &&
               took >= 9 && took <= 15 && said( after, "You eject the spent casing" ),
               "reload on a jammed gun: CDDA's mend, no reload", std::to_string( took ) + " turns, ammo " +
               std::to_string( before.ammo ) + " -> " + std::to_string( after.ammo ) +
               ( after.jammed ? ", jammed" : ", cleared" ) + ( back ? "" : ", stuck" ) );
    }
    // The `clear` item action (bridge.h): only the wielded gun itself, named by its snapshot handle (hostile
    // input). Refusals say why and spend nothing (same turn, same moves, still jammed); then the jammed gun in hand
    // is cleared by CDDA's mend. CDDA's jam fixes need no tools or skills, so "no fix can be done" can't be reached
    // with its data.
    {
        const uint32_t t_glock = fp3d::intern_id( fp3d::id_kind::itype, "glock_19" );
        struct by_handle {
            bool back = false;
            seen before;
            seen after;
            std::string note;
        };
        // Builds `sc`, then sends `clear` for the item `pick` names in the snapshot ({} = nothing to send).
        const auto clear_run = [&]( const scene & sc,
        const std::function<fp3d::item_ref( const fp3d::snapshot & )> &pick ) {
            by_handle c;
            std::optional<fp3d::ui_request> q = fp3d::wait_request( 5000 );
            if( q && q->timed ) {
                {
                    std::lock_guard<std::mutex> lk( st->mx );
                    st->want = sc;
                }
                fp3d::queue_action( "pause" );
                q = fp3d::wait_request( 20000, q->id );
            }
            const std::shared_ptr<const fp3d::snapshot> snap = fp3d::latest_snapshot();
            if( !q || !q->timed || !snap ) {
                c.note = ", no main loop";
                return c;
            }
            c.before = seen_now();
            fp3d::item_action a;
            a.op = fp3d::item_op::clear;
            a.item = pick( *snap );
            if( a.item.uid == 0 || !fp3d::queue_item_action( a ) ) {
                c.note = ", not queued";
                return c;
            }
            std::optional<fp3d::ui_request> r = fp3d::wait_request( 30000, q->id );
            int asked = 0;
            while( r && !r->timed && asked++ < 5 ) {
                fp3d::ui_answer ans;
                ans.id = r->id;
                ans.cancel = true;
                print_request( *r, "unexpected, cancel" );
                fp3d::answer( ans );
                const uint64_t id = r->id;
                r = fp3d::wait_request( 20000, id );
            }
            c.back = r && r->timed && asked == 0;
            c.after = seen_now();
            return c;
        };
        const auto wielded = []( const fp3d::snapshot & s ) {
            for( const fp3d::carried_view &v : s.player.items ) {
                if( v.flags & fp3d::carried_wielded ) {
                    return fp3d::item_ref{ v.uid, v.type };
                }
            }
            return fp3d::item_ref{};
        };
        // A magazine's handle from when it was carried, now inside the wielded gun.
        const auto inside_wielded = [&]( const fp3d::snapshot & ) {
            const seen now = seen_now();
            return fp3d::item_ref{ now.mag_uid, now.mag_type };
        };
        const auto carried_glock = [&]( const fp3d::snapshot & s ) {
            for( const fp3d::carried_view &v : s.player.items ) {
                if( v.type == t_glock && !( v.flags & fp3d::carried_wielded ) ) {
                    return fp3d::item_ref{ v.uid, v.type };
                }
            }
            return fp3d::item_ref{};
        };
        const auto ground_glock = [&]( const fp3d::snapshot & s ) {
            const fp3d::ui_point w{ s.player.pos.x - 1, s.player.pos.y, s.player.pos.z };
            for( const fp3d::item_handle_view &h : handles_at( w ) ) {
                if( h.type == t_glock ) {
                    return fp3d::item_ref{ h.uid, h.type };
                }
            }
            return fp3d::item_ref{};
        };
        const auto spent_nothing = []( const by_handle & c ) {
            return c.back && c.after.turn == c.before.turn && c.after.moves == c.before.moves &&
                   c.before.jammed && c.after.jammed;
        };
        const auto shown = []( const by_handle & c ) {
            return "turn +" + std::to_string( c.after.turn - c.before.turn ) + ", moves " +
                   std::to_string( c.before.moves ) + " -> " + std::to_string( c.after.moves ) +
                   ( c.after.jammed ? ", jammed" : ", cleared" ) + c.note +
                   ( c.after.messages.empty() ? std::string() : " [" + c.after.messages.back() + "]" );
        };
        scene jam = point_blank;
        jam.zx = 0;
        jam.fault = "fault_stovepipe";
        scene with_others = jam;
        with_others.spare_gun = true;
        with_others.ground_gun = true;
        by_handle c = clear_run( with_others, carried_glock );
        check( spent_nothing( c ) && said( c.after, "Hold the gun" ), "clear: a gun carried but not wielded",
               shown( c ) );
        c = clear_run( with_others, ground_glock );
        check( spent_nothing( c ) && said( c.after, "don't have that" ), "clear: a gun lying on the ground",
               shown( c ) );
        c = clear_run( jam, inside_wielded );
        // Refused either way: CDDA's carried items don't reach into a magazine well, so it isn't found at all.
        check( spent_nothing( c ) && ( said( c.after, "Hold the gun" ) || said( c.after, "don't have that" ) ),
               "clear: the magazine in the wielded gun",
               shown( c ) );
        scene clean = jam;
        clean.fault.clear();
        c = clear_run( clean, wielded );
        check( c.back && c.after.turn == c.before.turn && c.after.moves == c.before.moves && !c.after.jammed &&
               said( c.after, "isn't jammed" ), "clear: a gun that isn't jammed", shown( c ) );
        c = clear_run( jam, wielded );
        const int64_t took = c.after.turn - c.before.turn;
        check( c.back && c.before.jammed && !c.after.jammed && took >= 9 && took <= 15 &&
               c.after.ammo == c.before.ammo && said( c.after, "You eject the spent casing" ),
               "clear: the wielded jammed gun, CDDA's mend", shown( c ) );
    }
    // Aimed melee (fp3d/swing.h): an aimed strike asks one swing request for the creature struck, a plain one
    // none; the client's part (an answer for that creature on its square) only weights CDDA's pick; any other
    // answer leaves CDDA's pick; the off hand says so; a reach weapon strikes along the direction.
    {
        struct swung {
            bool back = false;
            std::vector<fp3d::ui_request> swings;
            seen before;
            seen after;
            fp3d::swing_counts c0;
            fp3d::swing_counts c1;
        };
        using swing_answerer = std::function<std::optional<fp3d::shot_answer>( const fp3d::ui_request &,
                               const seen & )>;
        const auto swing_run = [&]( const scene & sc, const std::string & action, const swing_answerer & reply ) {
            swung s;
            // A stray prompt is printed and answered, and the scene built again: the creature may have grabbed
            // the avatar after the last strike, and struggling free on the pause can tear a pocket off (a popup).
            const auto settle = []( std::optional<fp3d::ui_request> r ) {
                for( int k = 0; k < 5 && r && !r->timed; k++ ) {
                    fp3d::ui_answer a;
                    a.id = r->id;
                    if( r->kind == fp3d::request_kind::popup && !r->actions.empty() ) {
                        a.action = r->actions.front();
                    } else {
                        a.cancel = true;
                    }
                    print_request( *r, "unexpected, " + ( a.cancel ? std::string( "cancel" ) : a.action ) );
                    fp3d::answer( a );
                    r = fp3d::wait_request( 20000, r->id );
                }
                return r;
            };
            std::optional<fp3d::ui_request> q = settle( fp3d::wait_request( 5000 ) );
            for( int attempt = 0; attempt < 3; attempt++ ) {
                if( !q || !q->timed ) {
                    return s;
                }
                {
                    std::lock_guard<std::mutex> lk( st->mx );
                    st->want = sc;
                }
                fp3d::queue_action( "pause" );
                const std::optional<fp3d::ui_request> built = fp3d::wait_request( 20000, q->id );
                q = settle( built );
                if( built && built->timed ) {
                    break;
                }
            }
            if( !q || !q->timed ) {
                return s;
            }
            s.before = seen_now();
            s.c0 = fp3d::get_swing_counts();
            fp3d::queue_action( action );
            std::optional<fp3d::ui_request> c = fp3d::wait_request( 20000, q->id );
            int asked = 0;
            while( c && !c->timed && asked++ < 10 ) {
                fp3d::ui_answer a;
                a.id = c->id;
                if( c->kind == fp3d::request_kind::swing ) {
                    s.swings.push_back( *c );
                    if( std::optional<fp3d::shot_answer> sa = reply( *c, s.before ) ) {
                        a.shot = sa;
                    } else {
                        a.cancel = true;
                    }
                } else {
                    print_request( *c, "unexpected, cancel" );
                    a.cancel = true;
                }
                const uint64_t id = c->id;
                fp3d::answer( a );
                c = fp3d::wait_request( 20000, id );
            }
            s.back = c && c->timed;
            s.after = seen_now();
            s.c1 = fp3d::get_swing_counts();
            return s;
        };
        const auto aimed_at = []( const char *part ) {
            return [part]( const fp3d::ui_request & r, const seen & b ) {
                fp3d::shot_answer a;
                a.end = r.cursor;
                a.hit_uid = b.uid;
                a.part = part;
                return std::optional<fp3d::shot_answer>( a );
            };
        };
        const auto shown = []( const swung & s ) {
            std::string out = std::to_string( s.swings.size() ) + " swing requests, asked +" +
                              std::to_string( s.c1.asked - s.c0.asked ) + " accepted +" +
                              std::to_string( s.c1.accepted - s.c0.accepted ) + " refused +" +
                              std::to_string( s.c1.refused - s.c0.refused ) + " parts +" +
                              std::to_string( s.c1.parts - s.c0.parts ) + ", hp " + std::to_string( s.before.hp ) +
                              " -> " + std::to_string( s.after.hp );
            if( !s.swings.empty() ) {
                const fp3d::ui_request &r = s.swings.front();
                out += ", at (" + std::to_string( r.cursor.x - r.origin.x ) + "," +
                       std::to_string( r.cursor.y - r.origin.y ) + ") range " + std::to_string( r.range ) + " " +
                       r.mode;
            }
            return out + ( s.back ? "" : ", stuck" );
        };
        const auto at = []( const swung & s, int dx, int dy ) {
            return s.swings.size() == 1 && s.swings[0].cursor.x - s.swings[0].origin.x == dx &&
                   s.swings[0].cursor.y - s.swings[0].origin.y == dy && s.swings[0].cursor.z == s.swings[0].origin.z;
        };
        scene bat = point_blank;
        bat.zx = -1;
        bat.melee = "bat";
        swung s = swing_run( bat, "fp3d_attack_main_aim_LEFT", aimed_at( "head" ) );
        check( s.back && at( s, -1, 0 ) && s.swings[0].range == 1 && s.swings[0].mode == "main" &&
               s.c1.asked - s.c0.asked == 1 && s.c1.accepted - s.c0.accepted == 1,
               "aimed strike: one swing request for its creature", shown( s ) );
        s = swing_run( bat, "fp3d_attack_main_LEFT", aimed_at( "head" ) );
        check( s.back && s.swings.empty() && s.c1.asked == s.c0.asked, "plain strike: no swing request",
               shown( s ) );
        int legs = 0;   // a zombie's leg covers 15%, its head only 5%
        int accepted = 0;
        int ran = 0;
        for( int i = 0; i < 16; i++ ) {
            s = swing_run( bat, "fp3d_attack_main_aim_LEFT", aimed_at( "leg" ) );
            ran += s.back && s.swings.size() == 1 ? 1 : 0;
            accepted += static_cast<int>( s.c1.accepted - s.c0.accepted );
            legs += static_cast<int>( s.c1.parts - s.c0.parts );
        }
        check( ran == 16 && accepted == 16 && legs >= 1, "the client's part weights CDDA's pick",
               std::to_string( ran ) + " swung, " + std::to_string( accepted ) + " accepted, " +
               std::to_string( legs ) + " landed on the leg" );
        const swing_answerer other_uid = []( const fp3d::ui_request & r, const seen & b ) {
            fp3d::shot_answer a;
            a.end = r.cursor;
            a.hit_uid = b.uid + 12345;
            a.part = "head";
            return std::optional<fp3d::shot_answer>( a );
        };
        const swing_answerer other_square = []( const fp3d::ui_request & r, const seen & b ) {
            fp3d::shot_answer a;
            a.end = r.cursor;
            a.end.y += 1;
            a.hit_uid = b.uid;
            a.part = "head";
            return std::optional<fp3d::shot_answer>( a );
        };
        const swing_answerer cancel = []( const fp3d::ui_request &, const seen & ) {
            return std::optional<fp3d::shot_answer>();
        };
        int refused = 0;
        int parts = 0;
        int swings = 0;
        for( const swing_answerer *r : { &other_uid, &other_square, &cancel } ) {
            s = swing_run( bat, "fp3d_attack_main_aim_LEFT", *r );
            swings += s.back ? static_cast<int>( s.swings.size() ) : 0;
            refused += static_cast<int>( s.c1.refused - s.c0.refused );
            parts += static_cast<int>( s.c1.parts - s.c0.parts );
        }
        check( swings == 3 && refused == 3 && parts == 0, "another creature, square or a cancel: CDDA's pick",
               std::to_string( swings ) + " swings, refused " + std::to_string( refused ) + ", parts " +
               std::to_string( parts ) );
        scene bare = point_blank;
        bare.zx = -1;
        bare.armed = false;
        s = swing_run( bare, "fp3d_attack_off_aim_LEFT", aimed_at( "head" ) );
        check( s.back && at( s, -1, 0 ) && s.swings[0].mode == "off", "off-hand aimed strike says off", shown( s ) );
        scene spear = point_blank;
        spear.zx = -2;
        spear.melee = "spear_wood";
        s = swing_run( spear, "fp3d_attack_main_aim_LEFT", aimed_at( "head" ) );
        check( s.back && at( s, -2, 0 ) && s.swings[0].range == 2 && s.after.moves < s.before.moves,
               "a spear strikes 2 squares off", shown( s ) + ", moves " + std::to_string( s.before.moves ) + " -> " +
               std::to_string( s.after.moves ) );
        scene short_reach = spear;
        short_reach.melee = "bat";
        s = swing_run( short_reach, "fp3d_attack_main_aim_LEFT", aimed_at( "head" ) );
        check( s.back && s.swings.empty() && said( s.after, "Nothing there to hit" ),
               "a bat doesn't reach 2 squares", shown( s ) );
        // unseen behind a wall: no strike (it would bash the wall, spending a turn only when someone stood there)
        scene walled = spear;
        walled.wx = -1;
        s = swing_run( walled, "fp3d_attack_main_aim_LEFT", aimed_at( "head" ) );
        check( s.back && s.swings.empty() && said( s.after, "Nothing there to hit" ) && s.after.moves == s.before.moves &&
               s.after.hp == s.before.hp, "a spear doesn't strike someone unseen behind a wall",
               shown( s ) + ", moves " + std::to_string( s.before.moves ) + " -> " + std::to_string( s.after.moves ) );
    }
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
    // 2 s at 4x is 8 turns; a loaded machine gets up to 30 s for 3 of them (an overmap loading meanwhile took over
    // 10 s with parallel builds running). A prompt (a request that doesn't expire) would stop the clock: noted,
    // cancelled so the game goes on.
    std::string prompts;
    while( ms_since( t ) < 2000 || ( r->turn < turn0 + 3 && ms_since( t ) < 30000 ) ) {
        if( const std::optional<fp3d::ui_request> next = fp3d::wait_request( 50, r->id ) ) {
            r = next;
            if( !next->timed ) {
                prompts += std::string( " [" ) + fp3d::kind_name( next->kind ) + " " + next->context + ": " +
                           next->title.substr( 0, 60 ) + " | " + next->text.substr( 0, 120 ) + "]";
                fp3d::ui_answer a;
                a.id = next->id;
                a.cancel = true;
                fp3d::answer( a );
            }
        }
    }
    const fp3d::clock_state c = fp3d::get_clock();
    const observed o = snapshot();
    // What the game thread is on now: a pending request, or none (busy elsewhere).
    const std::optional<fp3d::ui_request> now = fp3d::wait_request( 0 );
    const std::string state = now ? std::string( ", now " ) + fp3d::kind_name( now->kind ) +
                              ( now->timed ? " timed" : " untimed" ) + " turn " + std::to_string( now->turn ) :
                              ", no request pending";
    check( o.zombies_placed > placed_before && o.visible > 0, "a new zombie in view", "seen " +
           std::to_string( o.visible ) );
    check( !c.paused && r->turn >= turn0 + 3 && !c.auto_pause_hostiles && prompts.empty(),
           "auto-pause off: real time runs on", "turns +" + std::to_string( r->turn - turn0 ) + " in " +
           std::to_string( static_cast<int64_t>( ms_since( t ) ) ) + " ms" + ( c.paused ? ", paused: " +
                   c.pause_reason : "" ) + state + prompts );
    fp3d::set_auto_pause_hostiles( true );
    fp3d::set_clock_mode( fp3d::clock_mode::turn_based );
    fp3d::set_paused( false );
    check( fp3d::get_clock().auto_pause_hostiles, "auto-pause back on", "" );
    return failed == 0;
}

// Cheats and the test kit (fp3d/cheats.h, 2026-11-10): all off at first; names checked exactly; each one on, the
// invincible / weight / ammo ones seen working; the kit handed out; all off again.
// D2: the stairs action. Out-of-range fields are refused when queued; stairs too far, the wrong way or missing
// are refused with a message and nothing happens; from next to the stairs the avatar goes down, then back up.
static bool run_stairs_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  stairs: %-38s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    const auto pos_now = []() {
        const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
        return s ? s->player.pos : fp3d::ui_point{ 0, 0, -999 };
    };
    const auto str = []( const fp3d::ui_point & q ) {
        return std::to_string( q.x ) + "," + std::to_string( q.y ) + "," + std::to_string( q.z );
    };
    const auto same = []( const fp3d::ui_point & a, const fp3d::ui_point & b ) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    };
    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    hand_setup = 15;
    fp3d::queue_action( "pause" );
    r = fp3d::wait_request( 20000, r->id );
    if( !r || !r->timed ) {
        check( false, "scene", "" );
        return false;
    }
    const fp3d::ui_point start = pos_now();
    const fp3d::ui_point stairs{ start.x, start.y - 1, start.z };
    // One action: queue it, wait for the main loop again, return the last message.
    const auto run = [&]( const fp3d::ui_point & at, int dir ) {
        std::vector<fp3d::event> evs;
        const std::optional<fp3d::ui_request> r0 = fp3d::wait_request( 5000 );
        if( !r0 || !r0->timed ) {
            return std::string( "<no main loop>" );
        }
        fp3d::drain_events( evs );
        evs.clear();
        if( !fp3d::queue_stairs_action( fp3d::stairs_action{ at, dir } ) ) {
            return std::string( "<not queued>" );
        }
        const std::optional<fp3d::ui_request> r1 = fp3d::wait_request( 40000, r0->id );
        fp3d::drain_events( evs );
        std::string msg = r1 && r1->timed ? "" : "<not back>";
        for( const fp3d::event &e : evs ) {
            if( e.kind == fp3d::event_kind::message ) {
                msg = e.text;
            }
        }
        return msg;
    };
    const auto has = []( const std::string & s, const char *part ) {
        return s.find( part ) != std::string::npos;
    };

    check( !fp3d::queue_stairs_action( { stairs, 0 } ) && !fp3d::queue_stairs_action( { stairs, 2 } ) &&
           !fp3d::queue_stairs_action( { stairs, -2 } ) &&
           !fp3d::queue_stairs_action( { { stairs.x, stairs.y, 11 }, 1 } ) &&
           !fp3d::queue_stairs_action( { { stairs.x, stairs.y, -11 }, -1 } ) &&
           !fp3d::queue_stairs_action( { { 1 << 29, stairs.y, stairs.z }, -1 } ) &&
           !fp3d::queue_stairs_action( { { stairs.x, -( 1 << 29 ), stairs.z }, -1 } ) &&
           !fp3d::queue_action( fp3d::stairs_action_name ), "bad fields refused when queued", "" );
    std::string m = run( { stairs.x, stairs.y - 1, stairs.z }, -1 );
    check( has( m, "too far" ) && same( pos_now(), start ), "two squares away refused", m );
    m = run( { stairs.x, stairs.y, stairs.z + 1 }, -1 );
    check( has( m, "too far" ) && same( pos_now(), start ), "another level refused", m );
    m = run( stairs, 1 );
    check( has( m, "no stairs up" ) && same( pos_now(), start ), "wrong way refused", m );
    m = run( { start.x - 1, start.y, start.z }, -1 );
    check( has( m, "no stairs down" ) && same( pos_now(), start ), "no stairs refused", m );
    m = run( stairs, -1 );
    const fp3d::ui_point below{ stairs.x, stairs.y, stairs.z - 1 };
    const bool down = same( pos_now(), below );
    check( down, "from next to them, down", str( pos_now() ) + " [" + m + "]" );
    m = down ? run( below, 1 ) : "<not below>";
    check( down && same( pos_now(), stairs ), "standing on them, up", str( pos_now() ) + " [" + m + "]" );
    return failed == 0;
}

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
    check( !fp3d::cheats_allowed() && !fp3d::request_cheat( fp3d::cheat::invincible, true ) &&
           !fp3d::request_test_kit() && snapshot().achievements, "refused until allowed, achievements on", "" );
    fp3d::set_cheats_allowed( true );

    // All on: applied at the main loop's next request (the sim asks again right after).
    for( size_t i = 0; i < fp3d::cheat_count; i++ ) {
        fp3d::request_cheat( static_cast<fp3d::cheat>( i ), true );
    }
    r = fp3d::wait_request( 20000, r->id );
    const uint32_t all = ( 1U << fp3d::cheat_count ) - 1;
    const fp3d::cheats_view on = fp3d::latest_cheats();
    check( r && on.on == all && !snapshot().achievements, "all on, achievements off", std::to_string( on.on ) );
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
    fp3d::set_cheats_allowed( false );
    check( !fp3d::request_cheat( fp3d::cheat::invincible, true ) && !fp3d::request_test_kit() &&
           !hurt.achievements, "refused again once disallowed", "" );
    return failed == 0;
}

// Test-range operations from the client (fp3d/cheats.h request_range_op, 2026-11-10): refused until cheats are
// allowed, bounded, and refused by the game outside a range game (this one isn't) and outside the main loop's hook.
static bool run_range_op_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  %-50s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "range ops: main loop waiting", "" );
        return false;
    }
    fp3d::range_op heal;
    heal.kind = fp3d::range_op_kind::heal;
    check( !fp3d::cheats_allowed() && fp3d::request_range_op( heal ) == 0, "range ops refused until allowed", "" );
    fp3d::set_cheats_allowed( true );
    fp3d::range_op big;
    big.kind = fp3d::range_op_kind::spawn_monster;
    big.monster = std::string( fp3d::max_monster_id + 1, 'a' );
    check( fp3d::request_range_op( big ) == 0, "range ops: overlong monster id refused", "" );
    // Off the sim thread, or outside the main loop's hook: refused, nothing changed.
    check( fp3d::heal() == "Only between turns." && fp3d::set_ai_frozen( true ) == "Only between turns." &&
           !fp3d::ai_frozen(), "range ops: only in the main loop's hook", "" );

    // They wait for the main loop's next request; 32 wait at most. The last 16 results are kept.
    range_fill.store( true );
    fp3d::queue_action( "pause" );
    r = fp3d::wait_request( 20000, r->id );
    const uint64_t last = range_fill_last.load();
    check( r && range_fill_ok.load(), "range ops: 32 wait, the next is refused", std::to_string( last ) );
    const std::vector<fp3d::range_op_result> res = fp3d::latest_range_results();
    bool refused = res.size() == fp3d::max_range_results && res.back().id == last;
    for( const fp3d::range_op_result &x : res ) {
        refused = refused && x.error == "Only in the test range.";
    }
    check( r && refused && !fp3d::ai_frozen(),
           "range ops: refused outside a range game", res.empty() ? "" : res.back().error );
    fp3d::set_cheats_allowed( false );
    check( fp3d::request_range_op( heal ) == 0, "range ops refused again once disallowed", "" );
    return failed == 0 && r.has_value();
}

// The teleport cheat (fp3d/cheats.h range_op_kind::point_teleport, 2026-11-10), in this normal game: refused until
// cheats are allowed; the client's square bounded and in the bubble; arrival on a free square, the nearest one when
// it isn't; refused in a vehicle and while riding.
static bool run_point_teleport_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  %-50s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "teleport cheat: main loop waiting", "" );
        return false;
    }
    fp3d::range_op tp;
    tp.kind = fp3d::range_op_kind::point_teleport;
    check( !fp3d::cheats_allowed() && fp3d::request_range_op( tp ) == 0, "teleport cheat refused until allowed", "" );
    fp3d::set_cheats_allowed( true );
    // Round n's results in order, once the hook set it up (a heal, refused outside the range, wakes the main loop).
    const auto round = [&]( int n, size_t count ) {
        for( std::atomic<uint64_t> &id : pt_ids ) {
            id.store( 0 );
        }
        pt_ended_ok.store( -1 );
        pt_round.store( n );
        fp3d::range_op heal;
        heal.kind = fp3d::range_op_kind::heal;
        fp3d::request_range_op( heal );
        std::vector<std::string> got;
        const auto t = std::chrono::steady_clock::now();
        while( ms_since( t ) < 20000 ) {
            got.clear();
            const std::vector<fp3d::range_op_result> res = fp3d::latest_range_results();
            for( size_t i = 0; i < count; i++ ) {
                for( const fp3d::range_op_result &x : res ) {
                    if( x.id != 0 && x.id == pt_ids[i].load() ) {
                        got.push_back( x.error );
                        break;
                    }
                }
            }
            if( got.size() == count && pt_ended_ok.load() != -1 ) {
                break;
            }
            if( std::optional<fp3d::ui_request> next = fp3d::wait_request( 100, r->id ) ) {
                r = next;
            }
        }
        got.resize( count, "(no result)" );
        return got;
    };
    const std::string outside = "That square is outside the reality bubble.";
    std::vector<std::string> got = round( 1, 5 );
    check( got[0] == outside && got[1] == outside && got[2] == outside,
           "teleport cheat: squares outside the bubble refused", got[0] + " | " + got[1] + " | " + got[2] );
    check( got[3] == "You are already there.", "teleport cheat: own square refused", got[3] );
    check( got[4].empty(), "teleport cheat: to a free square", got[4] );
    got = round( 2, 1 );
    check( pt_ended_ok.load() == 1, "teleport cheat: arrived on that square", "" );
    check( got[0].empty(), "teleport cheat: onto blood", got[0] );
    got = round( 3, 1 );
    check( pt_ended_ok.load() == 1, "teleport cheat: arrived next to the blood", "" );
    check( got[0] == "Not while in a vehicle.", "teleport cheat: refused in a vehicle", got[0] );
    got = round( 4, 1 );
    check( pt_ended_ok.load() == 1, "teleport cheat: stayed in the vehicle", "" );
    check( got[0] == "Not while riding.", "teleport cheat: refused while riding", got[0] );
    round( 5, 0 );
    check( pt_ended_ok.load() == 1, "teleport cheat: stayed on the horse", "" );
    fp3d::set_cheats_allowed( false );
    check( fp3d::request_range_op( tp ) == 0, "teleport cheat refused again once disallowed", "" );
    return failed == 0;
}

// A saved range game resumes only in a range world (fp3d/test_range.h resume_test_range, 2026-11-10): this normal
// world's save carries the range's avatar values (mark_as_range), and resumed after the reload as the bridge does, it
// stays a normal game: no range, no zones, AI not frozen.
static bool run_range_resume_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  %-50s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    check( resume_marked.load() == 1 && resume_said.load() == 0 && resume_active.load() == 0 &&
           resume_zones.load() == 0 && resume_frozen.load() == 0, "range resume: a normal world's marked save refused",
           "marked " + std::to_string( resume_marked.load() ) + ", said " + std::to_string( resume_said.load() ) +
           ", active " + std::to_string( resume_active.load() ) + ", zones " + std::to_string( resume_zones.load() ) +
           ", frozen " + std::to_string( resume_frozen.load() ) );
    check( !fp3d::range_world( "fp3d-sim" ) && !fp3d::range_world( "fp3d-range-sim" ),
           "range resume: fp3d-sim is no range world", "" );
    return failed == 0;
}

// Unload and reload by handle (fp3d/item_hands.h, 2026-11-10): a gun's magazine out into a hand, a magazine's rounds
// into the pack, a magazine loaded from loose rounds and from rounds lying in reach, the gun loaded with it. Refusals
// (incompatible ammo, itself, nothing loaded, out of reach, sealed, gone) say why, spend nothing and change nothing;
// malformed requests never get queued.
static bool run_gun_item_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  guns: %-44s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    struct result {
        bool back = false;       // in the main loop again (no menu came up)
        int spent = 0;           // moves
        std::string msg;         // the last message
    };
    const auto run = [&]( const fp3d::item_action & ia ) {
        result res;
        std::vector<fp3d::event> evs;
        const std::optional<fp3d::ui_request> r0 = fp3d::wait_request( 5000 );
        if( !r0 || !r0->timed ) {
            res.msg = "<no main loop>";
            return res;
        }
        const observed o0 = snapshot();
        fp3d::drain_events( evs );
        evs.clear();
        if( !fp3d::queue_item_action( ia ) ) {
            res.msg = "<not queued>";
            return res;
        }
        const std::optional<fp3d::ui_request> r1 = fp3d::wait_request( 60000, r0->id );
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
    const auto has = []( const std::string & s, const char *part ) {
        return s.find( part ) != std::string::npos;
    };
    const auto show = []( const result & r ) {
        return "[" + r.msg + "] " + std::to_string( r.spent ) + " moves";
    };
    const auto type_of = []( const char *id ) {
        return fp3d::intern_id( fp3d::id_kind::itype, id );
    };
    // The carried item with this handle (uid 0: not carried, or inside a gun or magazine).
    const auto count_of = []( uint64_t uid ) {
        for( const observed::carried_count &c : snapshot().counts ) {
            if( c.uid == uid ) {
                return c;
            }
        }
        return observed::carried_count();
    };
    const auto held_uid = []( uint32_t type, uint8_t flag ) {
        for( const fp3d::carried_view &c : carried_now() ) {
            if( c.type == type && ( c.flags & flag ) ) {
                return c.uid;
            }
        }
        return uint64_t{ 0 };
    };
    const auto lying_at = []( const fp3d::ui_point & p, uint64_t uid ) {
        for( const fp3d::item_handle_view &h : handles_at( p ) ) {
            if( h.uid == uid ) {
                return true;
            }
        }
        return false;
    };

    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    hand_setup = 16;
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
    const uint32_t t_glock = type_of( "glock_19" );
    const uint32_t t_mag = type_of( "glockmag" );
    const uint32_t t_9mm = type_of( "9mm" );
    const uint32_t t_223 = type_of( "223" );
    const uint64_t glock = gun_glock_uid;
    const uint64_t spare = gun_spare_uid;
    const uint64_t nine = gun_9mm_uid;
    const uint64_t rifle_rounds = gun_223_uid;
    const uint64_t sealed = gun_sealed_uid;
    const uint64_t far_glock = gun_far_glock_uid;
    const uint64_t far_9mm = gun_far_9mm_uid;
    const uint64_t near_9mm = gun_near_9mm_uid;
    check( glock != 0 && spare != 0 && nine != 0 && rifle_rounds != 0 && sealed != 0 && far_glock != 0 &&
           far_9mm != 0 && near_9mm != 0 && count_of( glock ).ammo == 15 && count_of( spare ).ammo == 0 &&
           count_of( sealed ).charges == 10 && !fp3d::latest_held().main.held, "scene",
           "glock " + std::to_string( count_of( glock ).ammo ) + " rounds" );

    // The bridge refuses malformed requests before they reach the game.
    fp3d::item_action ul;
    ul.op = fp3d::item_op::unload;
    ul.item = { glock, t_glock };
    ul.hand = fp3d::item_hand::main;
    fp3d::item_action rl;
    rl.op = fp3d::item_op::reload;
    rl.item = { spare, t_mag };
    rl.container = { nine, t_9mm };
    fp3d::item_action no_ammo = rl;
    no_ammo.container = {};
    fp3d::item_action ammo_big = rl;
    ammo_big.container.uid = fp3d::max_item_uid + 1;
    fp3d::item_action ammo_untyped = rl;
    ammo_untyped.container.type = 0;
    fp3d::item_action target_zero = rl;
    target_zero.item.uid = 0;
    fp3d::item_action unload_big = ul;
    unload_big.item.uid = fp3d::max_item_uid + 1;
    fp3d::item_action bad_op = ul;
    bad_op.op = static_cast<fp3d::item_op>( 8 );
    fp3d::item_action bad_hand = ul;
    bad_hand.hand = static_cast<fp3d::item_hand>( 3 );
    check( !fp3d::queue_item_action( no_ammo ) && !fp3d::queue_item_action( ammo_big ) &&
           !fp3d::queue_item_action( ammo_untyped ) && !fp3d::queue_item_action( target_zero ) &&
           !fp3d::queue_item_action( unload_big ) && !fp3d::queue_item_action( bad_op ) &&
           !fp3d::queue_item_action( bad_hand ), "malformed unload and reload refused by the bridge", "" );

    // Unload: the gun's magazine into the main hand; its rounds into the pack; nothing left to unload.
    result res = run( ul );
    const uint64_t mag = held_uid( t_mag, fp3d::carried_wielded );
    check( res.back && res.spent > 0 && fp3d::latest_held().main.id == "glockmag" && count_of( glock ).ammo == 0 &&
           mag != 0 && count_of( mag ).ammo == 15, "unload: a gun's magazine into the main hand", show( res ) );
    fp3d::item_action ul_mag = ul;
    ul_mag.item = { mag, t_mag };
    ul_mag.hand = fp3d::item_hand::pack;
    const int loose0 = snapshot().carried_9mm;
    res = run( ul_mag );
    const int loose1 = snapshot().carried_9mm;
    check( res.back && res.spent > 0 && count_of( mag ).ammo == 0 && loose1 == loose0 + 15,
           "unload: a magazine's rounds into the pack", std::to_string( loose0 ) + " -> " + std::to_string( loose1 ) +
           " loose " + show( res ) );
    fp3d::item_action ul_spare = ul;
    ul_spare.item = { spare, t_mag };
    res = run( ul_spare );
    check( res.back && res.spent == 0 && has( res.msg, "isn't loaded" ) && count_of( spare ).ammo == 0,
           "unload: nothing loaded", show( res ) );

    // Reload: the magazine (in hand) with loose rounds, then the gun with the magazine.
    const int nine0 = count_of( nine ).charges;
    fp3d::item_action rl_mag = rl;
    rl_mag.item = { mag, t_mag };
    res = run( rl_mag );
    const int in_mag = count_of( mag ).ammo;
    const int nine1 = count_of( nine ).charges;
    check( res.back && res.spent > 0 && nine0 >= 15 && in_mag == 15 && nine1 == nine0 - 15,
           "reload: a magazine with loose rounds", std::to_string( nine0 ) + " -> " + std::to_string( nine1 ) +
           " loose, " + std::to_string( in_mag ) + " in the magazine " + show( res ) );
    fp3d::item_action rl_gun = rl;
    rl_gun.item = { glock, t_glock };
    rl_gun.container = { mag, t_mag };
    res = run( rl_gun );
    check( res.back && res.spent > 0 && count_of( glock ).ammo == 15 && count_of( mag ).uid == 0 &&
           !fp3d::latest_held().main.held, "reload: the gun with that magazine",
           std::to_string( count_of( glock ).ammo ) + " in the gun " + show( res ) );

    // Refusals: each says why, spends nothing, changes nothing.
    fp3d::item_action rl_223 = rl;
    rl_223.container = { rifle_rounds, t_223 };
    res = run( rl_223 );
    check( res.back && res.spent == 0 && has( res.msg, "can't reload" ) && count_of( spare ).ammo == 0 &&
           count_of( rifle_rounds ).charges == 10, "reload: .223 into a 9 mm magazine", show( res ) );
    fp3d::item_action rl_self = rl;
    rl_self.container = { spare, t_mag };
    res = run( rl_self );
    check( res.back && res.spent == 0 && has( res.msg, "itself" ) && count_of( spare ).ammo == 0,
           "reload: a magazine with itself", show( res ) );
    fp3d::item_action ul_far = ul;
    ul_far.item = { far_glock, t_glock };
    ul_far.pos = far_west;
    ul_far.hand = fp3d::item_hand::pack;
    res = run( ul_far );
    check( res.back && res.spent == 0 && has( res.msg, "can't reach" ) && lying_at( far_west, far_glock ),
           "unload: a gun 3 squares away", show( res ) );
    fp3d::item_action rl_far = rl;
    rl_far.container = { far_9mm, t_9mm };
    rl_far.pos = far_west;
    res = run( rl_far );
    check( res.back && res.spent == 0 && has( res.msg, "can't reach" ) && count_of( spare ).ammo == 0 &&
           lying_at( far_west, far_9mm ), "reload: rounds 3 squares away", show( res ) );
    fp3d::item_action rl_sealed = rl;
    rl_sealed.container = { sealed, t_9mm };
    res = run( rl_sealed );
    check( res.back && res.spent == 0 && has( res.msg, "sealed" ) && count_of( spare ).ammo == 0 &&
           count_of( sealed ).charges == 10, "reload: rounds in a sealed bag", show( res ) );
    fp3d::item_action rl_gone = rl;
    rl_gone.container = { 0x5eed, t_9mm };
    rl_gone.pos = here;
    res = run( rl_gone );
    check( res.back && res.spent == 0 && has( res.msg, "isn't there" ) && count_of( spare ).ammo == 0,
           "reload: ammo that isn't there", show( res ) );

    // Reload from rounds lying in reach; unload into the off hand.
    fp3d::item_action rl_near = rl;
    rl_near.container = { near_9mm, t_9mm };
    rl_near.pos = west;
    res = run( rl_near );
    check( res.back && res.spent > 0 && count_of( spare ).ammo == 5 && !lying_at( west, near_9mm ),
           "reload: a magazine with rounds lying in reach", show( res ) );
    fp3d::item_action ul_off = ul;
    ul_off.hand = fp3d::item_hand::off;
    res = run( ul_off );
    check( res.back && res.spent > 0 && fp3d::latest_held().off.id == "glockmag" && count_of( glock ).ammo == 0,
           "unload: a gun's magazine into the off hand", show( res ) );
    return failed == 0;
}

// Throw and pour by handle (fp3d/item_hands.h, 2026-11-10): a wielded rock thrown at a square lands near it, one
// pebble of a stack flies; water poured onto the ground, into a bottle lying in reach, into a carried bottle (as much
// as fits, the rest stays) and into a given pocket (by the water's own handle). Refusals (out of range, worn, not
// carried, another level; not a liquid, sealed, into itself, not watertight, full, out of reach, no such pocket) say
// why, spend nothing and change nothing; malformed requests never get queued.
static bool run_throw_pour_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  throw/pour: %-44s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    struct result {
        bool back = false;       // in the main loop again (no menu came up)
        int spent = 0;           // moves
        std::string msg;         // the last message
    };
    const auto run = [&]( const fp3d::item_action & ia ) {
        result res;
        std::vector<fp3d::event> evs;
        const std::optional<fp3d::ui_request> r0 = fp3d::wait_request( 5000 );
        if( !r0 || !r0->timed ) {
            res.msg = "<no main loop>";
            return res;
        }
        const observed o0 = snapshot();
        fp3d::drain_events( evs );
        evs.clear();
        if( !fp3d::queue_item_action( ia ) ) {
            res.msg = "<not queued>";
            return res;
        }
        const std::optional<fp3d::ui_request> r1 = fp3d::wait_request( 60000, r0->id );
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
    const auto has = []( const std::string & s, const char *part ) {
        return s.find( part ) != std::string::npos;
    };
    const auto show = []( const result & r ) {
        return "[" + r.msg + "] " + std::to_string( r.spent ) + " moves";
    };
    const auto type_of = []( const char *id ) {
        return fp3d::intern_id( fp3d::id_kind::itype, id );
    };
    // The carried item with this handle (uid 0: not carried).
    const auto count_of = []( uint64_t uid ) {
        for( const observed::carried_count &c : snapshot().counts ) {
            if( c.uid == uid ) {
                return c;
            }
        }
        return observed::carried_count();
    };
    // The carried item of this type inside the carried item `parent` (uid 0: none).
    const auto inside = []( uint64_t parent, uint32_t type ) {
        for( const fp3d::carried_view &c : carried_now() ) {
            if( c.parent == parent && c.type == type ) {
                return c.uid;
            }
        }
        return uint64_t{ 0 };
    };
    // Where an item of `type` (or with handle `uid`, when not 0) lies within 3 squares of `p`; nullopt if nowhere.
    const auto lying_near = []( const fp3d::ui_point & p, uint64_t uid, uint32_t type ) {
        std::optional<fp3d::ui_point> at;
        for( int dx = -3; dx <= 3 && !at; dx++ ) {
            for( int dy = -3; dy <= 3 && !at; dy++ ) {
                const fp3d::ui_point q{ p.x + dx, p.y + dy, p.z };
                for( const fp3d::item_handle_view &h : handles_at( q ) ) {
                    if( uid != 0 ? h.uid == uid : h.type == type ) {
                        at = q;
                    }
                }
            }
        }
        return at;
    };

    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    hand_setup = 17;
    fp3d::queue_action( "pause" );
    r = fp3d::wait_request( 20000, r->id );
    const std::shared_ptr<const fp3d::snapshot> s0 = fp3d::latest_snapshot();
    if( !r || !r->timed || !s0 ) {
        check( false, "scene", "" );
        return false;
    }
    const fp3d::ui_point here = s0->player.pos;
    const fp3d::ui_point south{ here.x, here.y + 1, here.z };
    const fp3d::ui_point target{ here.x - 4, here.y, here.z };
    const uint32_t t_rock = type_of( "rock" );
    const uint32_t t_pebble = type_of( "pebble" );
    const uint32_t t_bottle = type_of( "bottle_plastic" );
    const uint32_t t_bag = type_of( "bag_plastic" );
    const uint32_t t_big = type_of( "bottle_twoliter" );
    const uint32_t t_water = type_of( "water" );
    const uint32_t t_backpack = type_of( "backpack" );
    const uint64_t rock = tp_rock_uid;
    const uint64_t pebbles = tp_pebble_uid;
    const uint64_t ground_rock = tp_ground_rock_uid;
    const uint64_t bottle_a = tp_bottle_a_uid;
    const uint64_t water_a = tp_water_a_uid;
    const uint64_t bottle_b = tp_bottle_b_uid;
    const uint64_t water_b = tp_water_b_uid;
    const uint64_t sealed = tp_sealed_uid;
    const uint64_t sealed_water = tp_sealed_water_uid;
    const uint64_t rock_bag = tp_rock_bag_uid;
    const uint64_t big = tp_big_uid;
    const uint64_t big_water = tp_big_water_uid;
    const uint64_t bottle_g = tp_bottle_g_uid;
    const uint64_t bottle_h = tp_bottle_h_uid;
    const uint64_t bottle_c = tp_bottle_c_uid;
    const uint64_t bottle_d = tp_bottle_d_uid;
    const uint64_t open_bag = tp_open_bag_uid;
    uint64_t backpack = 0;
    for( const fp3d::carried_view &c : carried_now() ) {
        if( c.type == t_backpack && ( c.flags & fp3d::carried_worn ) ) {
            backpack = c.uid;
        }
    }
    const observed o0 = snapshot();
    check( rock != 0 && pebbles != 0 && ground_rock != 0 && bottle_a != 0 && water_a != 0 && bottle_b != 0 &&
           water_b != 0 && sealed != 0 && sealed_water != 0 && rock_bag != 0 && big != 0 && big_water != 0 &&
           bottle_g != 0 && bottle_h != 0 && bottle_c != 0 && bottle_d != 0 && open_bag != 0 && backpack != 0 &&
           fp3d::latest_held().main.id == "rock" && count_of( pebbles ).charges == 5 &&
           count_of( water_a ).charges == 2 && count_of( big_water ).charges == 6 && o0.puddle == 0 &&
           o0.west_water == 0, "scene", std::to_string( carried_now().size() ) + " carried" );

    // The bridge refuses malformed requests before they reach the game.
    fp3d::item_action th;
    th.op = fp3d::item_op::throw_;
    th.item = { rock, t_rock };
    th.pos = target;
    fp3d::item_action po;
    po.op = fp3d::item_op::pour;
    po.item = { bottle_a, t_bottle };
    po.pos = south;
    fp3d::item_action throw_untyped = th;
    throw_untyped.item.type = 0;
    fp3d::item_action throw_far = th;
    throw_far.pos.x = -( 1 << 30 );
    fp3d::item_action pour_both = po;
    pour_both.container = { bottle_c, t_bottle };
    fp3d::item_action pour_half_ref = po;
    pour_half_ref.pos = {};
    pour_half_ref.container = { bottle_c, 0 };
    fp3d::item_action pour_pocket_no_container = po;
    pour_pocket_no_container.pocket = 0;
    fp3d::item_action pour_pocket_big = po;
    pour_pocket_big.pos = {};
    pour_pocket_big.container = { bottle_c, t_bottle };
    pour_pocket_big.pocket = fp3d::max_item_pocket + 1;
    fp3d::item_action pour_untyped = po;
    pour_untyped.item.type = 0;
    fp3d::item_action bad_op = th;
    bad_op.op = static_cast<fp3d::item_op>( 8 );
    check( !fp3d::queue_item_action( throw_untyped ) && !fp3d::queue_item_action( throw_far ) &&
           !fp3d::queue_item_action( pour_both ) && !fp3d::queue_item_action( pour_half_ref ) &&
           !fp3d::queue_item_action( pour_pocket_no_container ) && !fp3d::queue_item_action( pour_pocket_big ) &&
           !fp3d::queue_item_action( pour_untyped ) && !fp3d::queue_item_action( bad_op ),
           "malformed throw and pour refused by the bridge", "" );

    // Throw refusals: each says why, spends nothing, changes nothing.
    fp3d::item_action th_range = th;
    th_range.pos = { here.x - 40, here.y, here.z };
    result res = run( th_range );
    check( res.back && res.spent == 0 && has( res.msg, "that far" ) && fp3d::latest_held().main.id == "rock",
           "throw: beyond the throw range", show( res ) );
    fp3d::item_action th_worn = th;
    th_worn.item = { backpack, t_backpack };
    res = run( th_worn );
    check( res.back && res.spent == 0 && has( res.msg, "Take off" ) && count_of( backpack ).uid != 0,
           "throw: a worn backpack", show( res ) );
    fp3d::item_action th_ground = th;
    th_ground.item = { ground_rock, t_rock };
    res = run( th_ground );
    check( res.back && res.spent == 0 && has( res.msg, "don't have" ), "throw: a rock lying on the ground",
           show( res ) );
    fp3d::item_action th_up = th;
    th_up.pos = { target.x, target.y, target.z + 1 };
    res = run( th_up );
    check( res.back && res.spent == 0 && has( res.msg, "own level" ) && fp3d::latest_held().main.id == "rock",
           "throw: at another level", show( res ) );
    fp3d::item_action th_self = th;
    th_self.pos = here;
    res = run( th_self );
    check( res.back && res.spent == 0 && has( res.msg, "square to throw" ) && fp3d::latest_held().main.id == "rock",
           "throw: at the avatar's own square", show( res ) );

    // Throw: the wielded rock lands near the square thrown at; one pebble of five flies from the pack.
    res = run( th );
    const std::optional<fp3d::ui_point> landed = lying_near( target, rock, 0 );
    check( res.back && res.spent > 0 && count_of( rock ).uid == 0 && !fp3d::latest_held().main.held &&
           landed.has_value(), "throw: the wielded rock 4 squares west", ( landed ? "landed at " +
                   std::to_string( landed->x - here.x ) + "," + std::to_string( landed->y - here.y ) :
                   std::string( "not found" ) ) + " " + show( res ) );
    fp3d::item_action th_pebble = th;
    th_pebble.item = { pebbles, t_pebble };
    th_pebble.pos = { here.x - 3, here.y, here.z };
    res = run( th_pebble );
    const std::optional<fp3d::ui_point> pebble_at = lying_near( th_pebble.pos, 0, t_pebble );
    check( res.back && res.spent > 0 && count_of( pebbles ).charges == 4 && pebble_at.has_value(),
           "throw: one pebble of a stack", std::to_string( count_of( pebbles ).charges ) + " left " + show( res ) );

    // Pour refusals.
    fp3d::item_action po_rock = po;
    po_rock.item = { rock_bag, t_bag };
    res = run( po_rock );
    check( res.back && res.spent == 0 && has( res.msg, "no liquid" ), "pour: a bag holding a rock", show( res ) );
    fp3d::item_action po_sealed = po;
    po_sealed.item = { sealed, t_bottle };
    res = run( po_sealed );
    check( res.back && res.spent == 0 && has( res.msg, "sealed" ) && count_of( sealed_water ).charges == 2 &&
           snapshot().puddle == 0, "pour: a sealed bottle", show( res ) );
    fp3d::item_action po_into = po;
    po_into.pos = {};
    po_into.item = { bottle_b, t_bottle };
    po_into.container = { bottle_b, t_bottle };
    res = run( po_into );
    check( res.back && res.spent == 0 && has( res.msg, "itself" ) && count_of( water_b ).charges == 2,
           "pour: a bottle into itself", show( res ) );
    fp3d::item_action po_bag = po_into;
    po_bag.container = { open_bag, t_bag };
    res = run( po_bag );
    check( res.back && res.spent == 0 && has( res.msg, "won't hold" ) && count_of( water_b ).charges == 2,
           "pour: into a plastic bag (not watertight)", show( res ) );
    fp3d::item_action po_full = po_into;
    po_full.container = { bottle_d, t_bottle };
    res = run( po_full );
    check( res.back && res.spent == 0 && has( res.msg, "brim" ) && count_of( water_b ).charges == 2,
           "pour: into a full bottle", show( res ) );
    fp3d::item_action po_far = po;
    po_far.pos = { here.x - 3, here.y, here.z };
    res = run( po_far );
    check( res.back && res.spent == 0 && has( res.msg, "can't reach" ) && count_of( water_a ).charges == 2,
           "pour: onto a square 3 away", show( res ) );
    fp3d::item_action po_pocket = po_into;
    po_pocket.item = { big, t_big };
    po_pocket.container = { bottle_h, t_bottle };
    po_pocket.pocket = 5;
    res = run( po_pocket );
    check( res.back && res.spent == 0 && has( res.msg, "no such pocket" ) && count_of( big_water ).charges == 6,
           "pour: into a pocket the bottle doesn't have", show( res ) );

    // Pour: onto the ground (a puddle there), into a bottle lying west, into a carried bottle (as much as fits),
    // the water's own handle into a given pocket.
    res = run( po );
    check( res.back && res.spent > 0 && snapshot().puddle == 2 && count_of( water_a ).uid == 0 &&
           count_of( bottle_a ).uid != 0, "pour: water onto the ground south",
           std::to_string( snapshot().puddle ) + " in the puddle " + show( res ) );
    fp3d::item_action po_c = po_into;
    po_c.container = { bottle_c, t_bottle };
    res = run( po_c );
    check( res.back && res.spent > 0 && snapshot().west_water == 2 && count_of( water_b ).uid == 0,
           "pour: into a bottle lying west", std::to_string( snapshot().west_water ) + " there " + show( res ) );
    fp3d::item_action po_g = po_into;
    po_g.item = { big, t_big };
    po_g.container = { bottle_g, t_bottle };
    res = run( po_g );
    const uint64_t water_g = inside( bottle_g, t_water );
    check( res.back && res.spent > 0 && count_of( big_water ).charges == 4 && water_g != 0 &&
           count_of( water_g ).charges == 2 && has( res.msg, "left over" ), "pour: 2 L bottle into a 500 ml one",
           std::to_string( count_of( big_water ).charges ) + " left " + show( res ) );
    fp3d::item_action po_h = po_into;
    po_h.item = { big_water, t_water };
    po_h.container = { bottle_h, t_bottle };
    po_h.pocket = 0;
    res = run( po_h );
    const uint64_t water_h = inside( bottle_h, t_water );
    check( res.back && res.spent > 0 && count_of( big_water ).charges == 2 && water_h != 0 && water_h != big_water &&
           count_of( water_h ).charges == 2, "pour: the water's own handle into pocket 0", show( res ) );
    return failed == 0;
}

// Boot progress and CDDA's debug messages in an FP3D game (fp3d/bridge.h, 2026-11-10): phases and steps count,
// texts are cut on whole UTF-8 characters, and a debugmsg is kept, never a prompt (test_mode off for that one
// message, the bridge active). Main thread, while the game thread waits for an answer.
static bool run_boot_progress_checks()
{
    fp3d::begin_boot();
    fp3d::note_boot_step( "Phase A", "file 1" );
    fp3d::note_boot_step( "Phase A", "file 2" );
    fp3d::note_boot_step( "Phase B" );
    const fp3d::boot_progress p = fp3d::latest_boot_progress();
    // 299 ASCII bytes, then a 2-byte character across the 300-byte cut: the whole character goes.
    const std::string long_text = std::string( fp3d::max_boot_text - 1, 'x' ) + "\xC3\xA9 and more";
    const bool was_test = test_mode;
    const bool was_active = fp3d::active();
    fp3d::set_active( true );
    test_mode = false;
    bool prompted = false;
    try {
        debugmsg( "%s", long_text );
    } catch( ... ) {
        prompted = true;
    }
    test_mode = was_test;
    fp3d::set_active( was_active );
    const fp3d::boot_progress q = fp3d::latest_boot_progress();
    const bool ok = p.phase == "Phase B" && p.steps == 3 && p.errors == 0 && !prompted && q.errors == 1 &&
                    q.last_error == std::string( fp3d::max_boot_text - 1, 'x' );
    std::printf( "  boot progress: %s (phase \"%s\", %llu steps, %llu errors, last %zu bytes%s)\n", ok ? "PASS" : "FAIL",
                 p.phase.c_str(), static_cast<unsigned long long>( p.steps ),
                 static_cast<unsigned long long>( q.errors ), q.last_error.size(), prompted ? ", prompted" : "" );
    return ok;
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
    const bool cooking_ok = items_ok && run_cooking_checks();
    const bool cheats_ok = cooking_ok && run_cheat_checks();
    const bool stairs_ok = cheats_ok && run_stairs_checks();
    const bool guns_ok = stairs_ok && run_gun_item_checks();
    const bool combat_ok = guns_ok && run_combat_checks();
    const bool range_ok = combat_ok && run_range_op_checks() && run_point_teleport_checks();
    const bool throw_pour_ok = range_ok && run_throw_pour_checks();
    const bool resume_ok = run_range_resume_checks();
    // While the game thread waits for its next answer (CDDA's log is still open).
    const bool boot_ok = run_boot_progress_checks();

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
    std::printf( "cooking: %s\n", cooking_ok ? "ok" : "BAD" );
    std::printf( "cheats: %s\n", cheats_ok ? "ok" : "BAD" );
    std::printf( "stairs: %s\n", stairs_ok ? "ok" : "BAD" );
    std::printf( "guns: %s\n", guns_ok ? "ok" : "BAD" );
    std::printf( "combat: %s\n", combat_ok ? "ok" : "BAD" );
    std::printf( "range ops: %s\n", range_ok ? "ok" : "BAD" );
    std::printf( "range resume: %s\n", resume_ok ? "ok" : "BAD" );
    std::printf( "throw/pour: %s\n", throw_pour_ok ? "ok" : "BAD" );
    const bool worlds_ok = world_name_failures == 0;
    std::printf( "worlds: %s\n", worlds_ok ? "ok" : "BAD" );
    std::printf( "boot progress: %s\n", boot_ok ? "ok" : "BAD" );
    const bool data_check_ok = data_check_failures == 0;
    std::printf( "data check: %s\n", data_check_ok ? "ok" : "BAD" );
    const bool random_start_ok = random_start_failures == 0;
    std::printf( "random start: %s\n", random_start_ok ? "ok" : "BAD" );
    return passed == static_cast<int>( n_goals ) && snap_ok && events_ok && queue_ok && safemode_ok && hands_ok &&
           siphon_ok &&
           shoot_ok && items_ok && cooking_ok && cheats_ok && stairs_ok && guns_ok && combat_ok && range_ok &&
           resume_ok && throw_pour_ok && worlds_ok && boot_ok && data_check_ok && random_start_ok ?
           EXIT_SUCCESS : EXIT_FAILURE;
}
