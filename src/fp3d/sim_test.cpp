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
#include <map>
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
#include "activity_actor_definitions.h"
#include "avatar.h"
#include "cached_options.h"
#include "calendar.h"
#include "character_martial_arts.h"
#include "color.h"
#include "coordinates.h"
#include "creature_tracker.h"
#include "cuboid_rectangle.h"
#include "debug.h"
#include "do_turn.h"
#include "field.h"
#include "field_type.h"
#include "flag.h"
#include "flexbuffer_json.h"
#include "dialogue.h"
#include "effect_on_condition.h"
#include "fp3d/arrival.h"
#include "fp3d/boot.h"
#include "fp3d/data_check.h"
#include "fp3d/bridge.h"
#include "fp3d/cheats.h"
#include "fp3d/cooking.h"
#include "fp3d/game_access.h"
#include "fp3d/glovebox.h"
#include "fp3d/vehicle_work.h"
#include "fp3d/bullet_paths.h"
#include "fp3d/gun_faults.h"
#include "fp3d/held.h"
#include "fp3d/id_tables.h"
#include "fp3d/overmap_read.h"
#include "fp3d/item_hands.h"
#include "fp3d/points.h"
#include "fp3d/shot.h"
#include "fp3d/test_range.h"
#include "fp3d/events.h"
#include "fp3d/snapshot.h"
#include "fp3d/swing.h"
#include "game.h"
#include "game_constants.h"
#include "input_context.h"
#include "inventory.h"
#include "item.h"
#include "item_location.h"
#include "line.h"
#include "math_parser_diag_value.h"
#include "memory_fast.h"
#include "json_loader.h"
#include "map.h"
#include "map_selector.h"
#include "map_scale_constants.h"
#include "mapdata.h"
#include "martialarts.h"
#include "messages.h"
#include "monster.h"
#include "npc.h"
#include "options.h"
#include "output.h"
#include "omdata.h"
#include "overmap.h"
#include "overmap_ui.h"
#include "overmapbuffer.h"
#include "path_info.h"
#include "player_activity.h"
#include "pocket_type.h"
#include "rng.h"
#include "scenario.h"
#include "start_location.h"
#include "string_input_popup.h"
#include "talker.h"
#include "translation.h"
#include "type_id.h"
#include "uilist.h"
#include "units.h"
#include "veh_type.h"
#include "veh_appliance.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "vpart_range.h"
#include "weather.h"
#include "weather_type.h"
#include "worldfactory.h"

namespace
{
const itype_id itype_apple( "apple" );
const itype_id itype_backpack( "backpack" );
const itype_id itype_bandages( "bandages" );
const itype_id itype_rock( "rock" );
const mtype_id mon_zombie( "mon_zombie" );
const mtype_id mon_horse( "mon_horse" );
const mtype_id mon_fungaloid_tower( "mon_fungaloid_tower" );
const itype_id itype_2h_flail_steel( "2h_flail_steel" );
const itype_id itype_gasoline( "gasoline" );
const itype_id itype_hose( "hose" );
const itype_id itype_jerrycan( "jerrycan" );
const itype_id itype_water( "water" );
const itype_id itype_water_clean( "water_clean" );
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
const itype_id itype_rock_large( "rock_large" );
const itype_id itype_v8_combustion( "v8_combustion" );
const ter_str_id ter_t_dirt( "t_dirt" );
const itype_id itype_bottle_twoliter( "bottle_twoliter" );
const ter_str_id ter_t_open_air( "t_open_air" );
const ter_str_id ter_t_pavement( "t_pavement" );
const ter_str_id ter_t_water_sh( "t_water_sh" );
const ter_str_id ter_t_water_dp( "t_water_dp" );
const vproto_id vehicle_prototype_car( "car" );
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
const itype_id itype_log( "log" );
const faction_id faction_your_followers( "your_followers" );
const itype_id itype_splinter( "splinter" );
const itype_id itype_lighter( "lighter" );
const itype_id itype_meat( "meat" );
const furn_str_id furn_f_fireplace( "f_fireplace" );
const furn_str_id furn_f_woodstove( "f_woodstove" );
const trait_id trait_BADHEARING( "BADHEARING" );
const trait_id trait_NIGHTVISION( "NIGHTVISION" );
const bionic_id bio_flashlight( "bio_flashlight" );
const efftype_id effect_winded( "winded" );
const efftype_id effect_grabbed( "grabbed" );
const efftype_id effect_bandaged( "bandaged" );
const efftype_id effect_bleed( "bleed" );
const efftype_id effect_disinfected( "disinfected" );
const flag_id json_flag_GRAB( "GRAB" );

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
// (run_throw_pour_checks; see the case below), 18 = consume checks, 19 and 20 = off-hand weight checks, 21 = siphon
// checks: strength 8 for "too heavy: not offered" (stage 3 sets 12, stage 8 puts the random start's back), 22 =
// lying-container checks, 23 = character query checks (see the case), 24 = their traits and effect taken away.
// Done on the sim thread right before its next request.
// Peek checks (scene 25): the rock carried and the rock wielded.
uint64_t pk_rock_uid = 0;
uint64_t pk_wielded_uid = 0;
std::atomic<int> hand_setup{ 0 };
// The unstoppable checks (unstop_scene, unstop_look): the scene's start square and NPC (sim thread only), and the last
// look at it (the sim thread writes, the driver reads after the next request).
tripoint_abs_ms unstop_origin;
std::optional<character_id> unstop_npc;
struct unstop_look_t {
    std::atomic<int> grabbed{ 0 };        // the player's grabbed effects
    std::atomic<int> grabbing{ 0 };       // the body parts monsters hold (torso, right arm, left leg, left arm)
    std::atomic<int> zombies{ 0 };
    std::atomic<int> hp{ 0 };             // all monsters' hit points together
    std::atomic<int> dx{ 0 };             // the player from the start square
    std::atomic<int> dy{ 0 };
    std::atomic<int> origin{ 0 };         // on the start square: 0 nobody (or the player), 1 a monster, 2 an NPC
    std::atomic<int> east{ 0 };           // the same for the square east of it
    std::atomic<int> under{ 0 };          // monsters on the player's own square
    std::atomic<int> turn{ 0 };
};
unstop_look_t unstop_view;
// The smash checks' scene (hand_setup 40 / 41): what the avatar wielded before it (sim thread only).
std::optional<item> smash_saved_weapon;
// The random start's strength while a scene sets its own (sim thread only; -1 = nothing saved).
int siphon_str_saved = -1;
int offhand_str_saved = -1;
// Cooking checks: what scene 14 left for the driver (sim thread writes, driver reads).
std::atomic<uint64_t> cook_far_uid{ 0 };      // the pot out of reach
std::atomic<uint64_t> cook_gone_uid{ 0 };     // the pot taken away again (a stale handle)
std::atomic<double> cook_capture_ms{ 0 };     // one full capture of the scene
std::atomic<double> cook_heat_ms{ 0 };        // the heat and cookware part of it
std::atomic<size_t> cook_heat_squares{ 0 };   // squares it built a crafting inventory for
std::atomic<int> cook_full_free_ml{ -1 };     // scene 26: the room left on the avatar's square once filled
std::atomic<int> cook_own_resume{ 0 };        // scene 28: CDDA's own way on with the craft: 1 held up, 2 it went on
std::atomic<uint64_t> cook_fuel_uid{ 0 };     // scene 30: the splinter the avatar carries
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
// The consume checks' scene (hand_setup 18).
std::atomic<uint64_t> cs_apple_uid{ 0 };        // a carried apple
std::atomic<uint64_t> cs_rotten_uid{ 0 };       // a carried rotten apple
std::atomic<uint64_t> cs_bottle_uid{ 0 };       // a carried bottle with 500 ml of clean water
std::atomic<uint64_t> cs_water_uid{ 0 };        // the water in it
std::atomic<uint64_t> cs_rock_uid{ 0 };         // a carried rock
std::atomic<uint64_t> cs_west_apple_uid{ 0 };   // an apple west
std::atomic<uint64_t> cs_far_apple_uid{ 0 };    // an apple 2 squares east
std::atomic<uint64_t> cs_spilt_uid{ 0 };        // clean water spilt north
// First aid checks (scene 50): three carried bandages and an apple.
std::atomic<uint64_t> fa_bandage_a_uid{ 0 };
std::atomic<uint64_t> fa_bandage_b_uid{ 0 };
std::atomic<uint64_t> fa_bandage_c_uid{ 0 };
std::atomic<uint64_t> fa_apple_uid{ 0 };
// Lean checks (scene 55): a rock lying behind a wall's end.
std::atomic<uint64_t> lean_rock_uid{ 0 };
// Cargo checks (scenes 56-58): how many bottles went into the car's biggest cargo space, and where that space is
// from the avatar (sim thread writes, the test reads after the scene).
std::atomic<int> cargo_items{ 0 };
std::atomic<int> cargo_dx{ 0 };
std::atomic<int> cargo_dy{ 0 };
std::optional<tripoint_abs_ms> cargo_centre;     // sim thread only: where scene 56 was set up
vehicle *cargo_car = nullptr;                    // sim thread only: scene 56's car, until scene 58
int cargo_part = -1;                             // ... and its biggest cargo space
// Cargo by hand (scene 59): two of the five bottles in the space, the free merchants' bottle there, the carried
// rock, the giant backpack in hand; and a square in the avatar's reach without a cargo space, from the avatar
// (cg_free 0: there is none).
std::atomic<uint64_t> cg_bottle_a_uid{ 0 };
std::atomic<uint64_t> cg_bottle_b_uid{ 0 };
std::atomic<uint64_t> cg_theirs_uid{ 0 };
std::atomic<uint64_t> cg_rock_uid{ 0 };
std::atomic<uint64_t> cg_big_uid{ 0 };
std::atomic<int> cg_free{ 0 };
std::atomic<int> cg_free_dx{ 0 };
std::atomic<int> cg_free_dy{ 0 };
// Glovebox checks (scenes 61, 62). glove_checked: prototypes looked at, the first glove_checked_with of which get a
// glovebox and the rest none; glove_kinds has a bit for each that is as expected, glove_protos is how many
// prototypes have one in all. The scene's fresh car: how many gloveboxes it has, what is around the glovebox (bit 1
// a windshield on its tile, 2 a seat on the tile behind it, 4 that seat is beside the driver's), where it is from
// the avatar on the driver's seat, and the bottle put into it.
const std::array<const char *, 13> glove_checked = { {
        "car", "pickup", "ambulance", "bus", "semi_truck", "humvee",
        "bicycle", "motorcycle", "golf_cart", "shopping_cart", "truck_trailer", "2seater2", "tractor_plow"
    }
};
constexpr size_t glove_checked_with = 6;
std::atomic<int> glove_kinds{ 0 };
std::atomic<int> glove_protos{ 0 };
std::atomic<int> glove_count{ 0 };
std::atomic<int> glove_tile{ 0 };
std::atomic<int> glove_dx{ 0 };
std::atomic<int> glove_dy{ 0 };
std::atomic<uint64_t> glove_bottle_uid{ 0 };
vehicle *glove_car = nullptr;                    // sim thread only: scene 61's car, until scene 62
std::optional<tripoint_abs_ms> glove_from;       // sim thread only: where the avatar stood before scene 61
// The vehicle-work checks' scenes (hand_setup 63 to 66).
std::atomic<int> work_env{ 0 };                  // scene 63: 1 light to work by, 2 morale to craft, 4 the glovebox
// damaged and repairable, 8 the avatar beside it
std::atomic<uint64_t> work_uid{ 0 };             // set by the driver before scene 64: the car's uid in the snapshot
std::atomic<int> work_row{ -1 };                 // ... and the glovebox's part row
std::string work_tiny;                           // scene 64: run_vehicle_work_query within 2500 bytes

// The map checks' scenes (76, 77). Written on the sim thread while a scene runs, read by the driver after it.
std::atomic<int> map_state{ 0 };                 // what each scene managed, by its own bits
int map_at[3] = { 0, 0, 0 };                     // the avatar's overmap cell
int map_vague_at[2] = { 0, 0 };                  // the cell made vague,
int map_unseen_at[2] = { 0, 0 };                 // the one made unseen,
int map_note_at[2] = { 0, 0 };                   // the one with the note
int map_far_at[2] = { 0, 0 };                    // a page origin with no overmap under it
std::string map_here[3];                         // what CDDA draws on the avatar's cell: name, symbol, colour
std::string map_vague[3];                        // ... and on the vague cell at vague vision
std::string map_full_name;                       // that cell's name at full vision
std::string map_note_sym;                        // the note's symbol and colour as CDDA parses them
std::string map_note_color;
std::string map_whole;                           // the avatar's page with its path, and the same in fewer bytes
std::string map_tiny;
int map_was[2] = { 0, 0 };                       // sim thread only: the vision the two cells had
bool map_marked = false;                         // sim thread only: scene 76 set the explored mark
bool map_unseen_marked = false;                  // sim thread only: ... and the one on the unseen cell
int map_blend_at[2] = { 0, 0 };                  // a campsite among forest, all seen vaguely
std::string map_blend[3];                        // what CDDA draws on it there: name, symbol, colour
std::string map_woods_name;                      // a forest's name when seen vaguely
std::vector<oter_id> map_blend_was;              // sim thread only: what the nine cells were
std::string map_plain;                           // the avatar's page without its path, and the same with fewer looks
std::string map_fewer;
std::atomic<int> work_ms{ 0 };                   // ... and how long it took
vehicle *work_car = nullptr;                     // sim thread only: scene 63's car, until scene 66
vehicle *work_machine = nullptr;                 // sim thread only: scene 67's appliance, until scene 66
std::atomic<int> work_machine_state{ 0 };        // scene 67: 1 the part type exists, 2 placed, 4 found on its square
std::optional<tripoint_abs_ms> work_from;        // sim thread only: where the avatar stood before scene 63
std::atomic<int> work_gate_state{ 0 };           // scenes 69 to 73: what each managed, by its own bits
std::optional<tripoint_abs_ms> work_stood;       // sim thread only: where the avatar stood before scene 70
int work_skill[2] = { 0, 0 };                    // sim thread only: its mechanics skill and knowledge before
// The lying-container checks' scene (hand_setup 22).
std::atomic<uint64_t> lc_pack_uid{ 0 };         // a backpack west holding a rock and 2 empty bottles
std::atomic<uint64_t> lc_rock_uid{ 0 };         // the rock
std::atomic<uint64_t> lc_sealed_uid{ 0 };       // a sealed bottle of water west
std::atomic<uint64_t> lc_sealed_water_uid{ 0 };
std::atomic<uint64_t> lc_far_pack_uid{ 0 };     // a backpack 2 squares west holding a rock
// The character query checks' scene (hand_setup 23): the sheet as the sim thread ran it.
std::mutex cq_m;
std::string cq_full;   // run_character_query with no byte limit
std::string cq_cut;    // the same with 300 bytes less room than it needs
std::string cq_tiny;   // the same in 3000 bytes: lists cut too
std::string cq_name;   // the avatar's name as the sheet shows it
int cq_carried[4] = { -1, -1, -1, -1 };   // CDDA's carried weight and its limit (g), volume and its limit (ml)

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

// Item physics checks (run_item_physics_checks): at the next main-loop request the hook builds their scene.
std::atomic<int> physics_setup{ 0 };           // 1: the item physics scene; 2: a pot of water in the main hand

// Weather and mood checks (run_weather_mood_checks): at the next main-loop request the hook 1 = forces a lightning
// storm, adds two morale entries and sets off CDDA's lightning effect, 2 = takes them back.
std::atomic<int> weather_mood_setup{ 0 };
std::atomic<int> weather_wait{ 0 };
// The interrupt checks: 1 map travel to the next overmap tile over flat ground; 2 a 5-minute wait; 3 a 6-hour wait;
// 4 read whether a route is left (interrupt_route: 1 yes, 0 no) and end the travel.
std::atomic<int> interrupt_setup{ 0 };
std::atomic<int> furniture_setup{ 0 };         // the furniture checks' scene (set_up_furniture)
// Number checks (run_number_checks): 1 = ask CDDA's numeric prompts from the hook (ask_numbers); what they returned.
std::atomic<int> number_setup{ 0 };
std::atomic<bool> number_done{ false };
struct number_results {
    bool got_int = false;        // query_int "How many?" (shown default 7)
    int int_value = 0;
    bool big_cancelled = false;  // query_int64_t, cancelled
    int edited = 0;              // edit( int ) from 5
    bool got_stubborn = true;    // query_int "Again?" (from 9), answered out of range every time
    int stubborn_value = 0;
};
std::mutex number_mutex;
number_results number_out;
// Name checks: what scene 33 saw (sim thread writes, driver reads).
struct name_results {
    bool ran = false;
    bool swapped = false;        // the avatar is the NPC's character now
    bool taken_ok = false;       // the name it took is the tidied one
    bool id_kept = false;        // the save id as before
    bool save_refused = false;   // game::save under a refused id
    size_t name_bytes = 0;
};
std::mutex names_mutex;
name_results names_out;
// Bounded polling: what scene 34's two polling loops saw (sim thread writes, driver reads). Polls until the loop
// ended; negative when it ended another way.
std::atomic<int> poll_quit_after{ 0 };
std::atomic<int> poll_asked_after{ 0 };
std::atomic<int> interrupt_route{ -1 };            // 1: a 20-minute wait at 20 C begins; 2: the world's temperature back

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

// Flat open ground nine squares wide with nothing lying on it; two rocks one square east, an engine (too heavy to
// lift) one square south, a wall north-east, open air two east and one north; the avatar empty-handed, wearing only
// a giant backpack with a plastic bottle, a pot, a two-liter bottle of water (rocks would merge into one stack: they
// count by charges) and a large rock
// (two-handed) in it (what earlier checks left it with is gone: the snapshot lists 256 carried items at most), and
// seeing in the dark.
void set_up_item_physics()
{
    avatar &u = get_avatar();
    map &here = get_map();
    const tripoint_bub_ms p = u.pos_bub();
    g->clear_zombies();
    for( int dx = -4; dx <= 4; dx++ ) {
        for( int dy = -4; dy <= 4; dy++ ) {
            const tripoint_bub_ms q = p + tripoint_rel_ms( dx, dy, 0 );
            here.i_clear( q );
            here.furn_set( q, furn_str_id::NULL_ID() );
            here.ter_set( q, ter_t_dirt );
        }
    }
    here.ter_set( p + tripoint_rel_ms( 1, -1, 0 ), ter_t_wall );
    here.ter_set( p + tripoint_rel_ms( 2, -1, 0 ), ter_t_open_air );
    here.add_item( p + tripoint_rel_ms( 1, 0, 0 ), item( itype_rock ) );
    here.add_item( p + tripoint_rel_ms( 1, 0, 0 ), item( itype_rock ) );
    here.add_item( p + tripoint_rel_ms( 0, 1, 0 ), item( itype_v8_combustion ) );
    here.invalidate_map_cache( p.z() );
    here.build_map_cache( p.z() );
    u.remove_weapon();
    u.clear_worn();
    u.inv->clear();
    if( !u.has_trait( trait_DEBUG_NIGHTVISION ) ) {
        u.set_mutation( trait_DEBUG_NIGHTVISION );
    }
    u.wear_item( item( itype_backpack_giant ), false );
    u.i_add( item( itype_bottle_plastic ) );
    u.i_add( item( itype_pot ) );
    item water_bottle( itype_bottle_twoliter );
    water_bottle.put_in( item( itype_water, calendar::turn, 2 ), pocket_type::CONTAINER );
    u.i_add( water_bottle );
    u.i_add( item( itype_rock_large ) );
    u.set_moves( u.get_speed() );
}

// The last item physics checks: what the main hand holds put away, a pot of water in it instead.
void wield_water_pot()
{
    avatar &u = get_avatar();
    if( item_location held = u.get_wielded_item() ) {
        item copy = *held;
        u.remove_weapon();
        u.i_add( copy );
    }
    // A little water (any spills), and strength enough for a pot in one hand: the random character may be weak, and
    // "too heavy for one hand" comes before "would spill".
    item water_pot( itype_pot );
    water_pot.put_in( item( itype_water, calendar::turn, 1 ), pocket_type::CONTAINER );
    if( u.str_max < 8 ) {
        u.str_max = 8;
        u.reset_stats();
    }
    u.wield( water_pot );
    u.set_moves( u.get_speed() );
}

void set_up_weather_mood( bool on )
{
    avatar &u = get_avatar();
    weather_manager &w = get_weather();
    const morale_type craving( "morale_craving_caffeine" );
    const morale_type music( "morale_music" );
    w.weather_override = on ? weather_type_id( "lightning" ) : WEATHER_NULL;
    w.set_nextweather( calendar::turn );
    w.update_weather();
    if( on ) {
        u.add_morale( craving, -40, -40 );
        u.add_morale( music, 15, 15 );
        dialogue d( get_talker_for( u ), nullptr );
        effect_on_condition_id( "lightning" )->apply_true_effects( d );
    } else {
        u.rem_morale( craving );
        u.rem_morale( music );
    }
}

// The weather cheat's wait: 20 minutes of CDDA's wait activity at 20 C (so the storm brings no "You are freezing!"
// question), nothing hostile about; then the world's own temperature again.
void wait_in_weather( bool start, int minutes = 20 )
{
    weather_manager &w = get_weather();
    if( !start ) {
        w.forced_temperature.reset();
        return;
    }
    g->clear_zombies();
    w.forced_temperature = units::from_celsius( 20 );
    get_avatar().assign_activity( player_activity( activity_id( "ACT_WAIT" ),
                                  100 * to_turns<int>( time_duration::from_minutes( minutes ) ), 0 ) );
}

// The running hook (fp3d::set_running_hook), as the bridge sets it: a snapshot while the game runs on by itself.
// Counted, with the game turn of the last one, for run_running_checks.
std::atomic<int> running_calls{ 0 };
std::atomic<int64_t> running_turn{ -1 };
std::atomic<int64_t> running_us{ 0 };   // wall time spent in it
// run_running_checks: 1 = the next running hook asks a question in the middle, as a debug message there would;
// then how often the before-ask hook ran for it (it must not: 0) and what the answer was.
std::atomic<int> observe_calls{ 0 };
std::atomic<int> running_asks{ 0 };
std::atomic<int> running_nested{ -1 };
std::atomic<int> running_answer{ -1 };
void running()
{
    if( running_asks.exchange( 0 ) == 1 ) {
        const int before = observe_calls.load();
        running_answer.store( query_yn( "fp3d test: asked inside the running hook" ) ? 1 : 0 );
        running_nested.store( observe_calls.load() - before );
    }
    const auto t = std::chrono::steady_clock::now();
    const std::shared_ptr<const fp3d::snapshot> s = fp3d::capture_snapshot();
    running_turn.store( s->turn );
    running_calls++;
    fp3d::publish_snapshot( s );
    running_us += std::chrono::duration_cast<std::chrono::microseconds>( std::chrono::steady_clock::now() - t ).count();
}

// The furniture checks' scene: flat ground; a closed safe east and another behind it, a locked safe west, a nailed
// crate north, a dresser south and another 2 squares south: a rock in each.
void set_up_furniture()
{
    avatar &u = get_avatar();
    map &here = get_map();
    const tripoint_bub_ms p = u.pos_bub();
    g->clear_zombies();
    for( int dx = -3; dx <= 3; dx++ ) {
        for( int dy = -3; dy <= 3; dy++ ) {
            const tripoint_bub_ms q = p + tripoint_rel_ms( dx, dy, 0 );
            here.i_clear( q );
            here.furn_set( q, furn_str_id::NULL_ID() );
            here.ter_set( q, ter_t_dirt );
        }
    }
    const auto place = [&]( int dx, int dy, const char *id ) {
        const tripoint_bub_ms q = p + tripoint_rel_ms( dx, dy, 0 );
        here.furn_set( q, furn_str_id( id ) );
        here.add_item( q, item( itype_rock ) );
    };
    place( 1, 0, "f_safe_c" );
    place( 2, 0, "f_safe_c" );
    place( -1, 0, "f_safe_l" );
    place( 0, -1, "f_crate_c" );
    place( 0, 1, "f_dresser" );
    place( 0, 2, "f_dresser" );
    here.invalidate_map_cache( p.z() );
    here.build_map_cache( p.z() );
}

// The interrupt checks' scenes (interrupt_setup).
void set_up_interrupt( int stage )
{
    avatar &u = get_avatar();
    map &here = get_map();
    const tripoint_bub_ms p = u.pos_bub();
    if( stage == 4 ) {
        interrupt_route.store( u.has_destination() ? 1 : 0 );
        u.omt_path.clear();
        return;
    }
    g->clear_zombies();
    if( stage == 1 ) {
        // Map travel as the overmap's travel starts it: ACT_TRAVELLING with a path of overmap tiles (the last is where
        // the avatar is). Its first turn routes to the edge of the next tile, half a tile away (activity_handlers.cpp
        // travel_do_turn), and auto-travel walks it. A route set from here would be dropped by the main loop's next
        // input; the activity sets it after that. Flat ground from the avatar to there.
        const tripoint_abs_omt cur = u.pos_abs_omt();
        const tripoint_abs_ms cur_mid = midpoint( project_bounds<coords::ms>( cur ) );
        const int dir = u.pos_abs().x() <= cur_mid.x() ? 1 : -1;
        const tripoint_abs_omt next = cur + tripoint_rel_omt( dir, 0, 0 );
        const tripoint_bub_ms wp = here.get_bub( clamp( cur_mid, project_bounds<coords::ms>( next ) ) );
        for( int x = std::min( p.x(), wp.x() ) - 3; x <= std::max( p.x(), wp.x() ) + 3; x++ ) {
            for( int y = std::min( p.y(), wp.y() ) - 3; y <= std::max( p.y(), wp.y() ) + 3; y++ ) {
                const tripoint_bub_ms q( x, y, p.z() );
                if( here.inbounds( q ) ) {
                    here.furn_set( q, furn_str_id::NULL_ID() );
                    here.ter_set( q, ter_t_dirt );
                }
            }
        }
        here.invalidate_map_cache( p.z() );
        here.build_map_cache( p.z() );
        u.omt_path = { next + tripoint_rel_omt( dir, 0, 0 ), next, cur };
        u.assign_activity( player_activity( activity_id( "ACT_TRAVELLING" ) ) );
        return;
    }
    const int minutes = stage == 2 ? 5 : 360;
    u.assign_activity( player_activity( activity_id( "ACT_WAIT" ), 100 * to_turns<int>( time_duration::from_minutes(
                                            minutes ) ), 0 ) );
}

// CDDA's own numeric prompts, asked on the sim thread inside the hook; run_number_checks answers them.
void ask_numbers()
{
    number_results res;
    int v = 7;
    res.got_int = query_int( v, true, std::string( "How many?" ) );
    res.int_value = v;
    string_input_popup big;
    big.title( "How far?" );
    const std::optional<int64_t> how_far = big.query_int64_t();
    res.big_cancelled = !how_far && big.canceled();
    int e = 5;
    string_input_popup edit;
    edit.title( "Edit" ).edit( e );
    res.edited = e;
    int stubborn = 9;
    res.got_stubborn = query_int( stubborn, true, std::string( "Again?" ) );
    res.stubborn_value = stubborn;
    std::lock_guard<std::mutex> lk( number_mutex );
    number_out = res;
    number_done.store( true );
}

// The cooking scene's pot goes from one square to another, handle and contents kept (sim thread).
static void move_cook_pot( map &here, const tripoint_bub_ms &from, const tripoint_bub_ms &to )
{
    for( item &it : here.i_at( from ) ) {
        if( it.typeId() == itype_pot ) {
            const item copy = it;
            here.i_rem( from, &it );
            here.add_item( to, copy );
            return;
        }
    }
}

// The cooking scene's pot on the fire holds 4 fresh water again (sim thread).
static void fresh_cook_water( map &here, const tripoint_bub_ms &fire_at )
{
    for( item &it : here.i_at( fire_at ) ) {
        if( it.typeId() == itype_pot ) {
            it.clear_items();
            it.put_in( item( itype_water, calendar::turn, 4 ), pocket_type::CONTAINER );
            it.on_contents_changed();
        }
    }
}

// The unstoppable checks' scenes (run_unstoppable_checks; sim thread). Each starts from flat dirt around the player
// with no zombie, NPC or grab left: 42 a zombie east holding the torso, 43 zombies on all eight squares around, four
// of them holding on (the user's case: no way out while the no-damage cheat keeps you alive), 46 a hostile NPC east,
// 47 deep water around the player (3 by 3) and a zombie east in it, 48 a fungaloid tower (IMMOBILE) east, 49 a hostile
// NPC east on a vehicle seat; none of them gets a move. 44 only clears up (dirt again, no vehicle).
// The scene's monsters' hp: far above what one hit takes (a critical once killed the "off" check's zombie with its
// first blow, so nothing was left holding on), so a check finds them still there.
constexpr int UNSTOP_HP = 10000;

void unstop_scene( int scene )
{
    avatar &u = get_avatar();
    map &here = get_map();
    const tripoint_bub_ms p = u.pos_bub( here );
    g->clear_zombies();
    if( unstop_npc ) {
        g->remove_npc( *unstop_npc );
        overmap_buffer.remove_npc( *unstop_npc );
        unstop_npc.reset();
    }
    u.remove_effect( effect_grabbed );
    // Scene 47's swim may sink a heavy swimmer (and wets what they carry: CDDA's water_affect_items); each scene
    // starts on dry land, above water.
    u.set_underwater( false );
    for( int dx = -2; dx <= 2; dx++ ) {
        for( int dy = -2; dy <= 2; dy++ ) {
            const tripoint_bub_ms q = p + tripoint_rel_ms( dx, dy, 0 );
            if( const optional_vpart_position vp = here.veh_at( q ) ) {
                here.destroy_vehicle( &vp->vehicle() );
            }
            here.i_clear( q );
            here.furn_set( q, furn_str_id::NULL_ID() );
            here.ter_set( q, scene == 47 && std::abs( dx ) <= 1 && std::abs( dy ) <= 1 ? ter_t_water_dp : ter_t_dirt );
        }
    }
    unstop_origin = u.pos_abs();
    if( scene == 44 ) {
        here.invalidate_map_cache( p.z() );
        here.build_map_cache( p.z() );
        return;
    }
    const tripoint_bub_ms east = p + tripoint_rel_ms( 1, 0, 0 );
    if( scene == 46 || scene == 49 ) {
        if( scene == 49 ) {
            if( vehicle *veh = here.add_vehicle( vehicle_prototype_none, east, 0_degrees, 0, 0 ) ) {
                veh->install_part( here, point_rel_ms::zero, vpart_frame );
                veh->install_part( here, point_rel_ms::zero, vpart_seat );
                here.add_vehicle_to_cache( veh );
            }
        }
        shared_ptr_fast<npc> guy = make_shared_fast<npc>();
        guy->normalize();
        guy->randomize();
        guy->spawn_at_precise( here.get_abs( east ) );
        overmap_buffer.insert_npc( guy );
        g->load_npcs();
        guy->set_attitude( NPCATT_KILL );
        guy->set_moves( -1000000 );
        unstop_npc = guy->getID();
    } else if( scene == 47 || scene == 48 ) {
        if( monster *const z = g->place_critter_at( scene == 47 ? mon_zombie : mon_fungaloid_tower, east ) ) {
            z->set_moves( -1000000 );
            z->set_hp( UNSTOP_HP );
        }
    } else {
        for( int dx = -1; dx <= 1; dx++ ) {
            for( int dy = -1; dy <= 1; dy++ ) {
                if( ( dx == 0 && dy == 0 ) || ( scene == 42 && !( dx == 1 && dy == 0 ) ) ) {
                    continue;
                }
                if( monster *const z = g->place_critter_at( mon_zombie, p + tripoint_rel_ms( dx, dy, 0 ) ) ) {
                    z->set_moves( -1000000 );
                    z->set_hp( UNSTOP_HP );
                    // East holds the torso, north-east the right arm, south the left leg, west the left arm.
                    const bodypart_str_id bp = dx == 1 && dy == 0 ? body_part_torso : scene == 42 ?
                                               bodypart_str_id::NULL_ID() : dx == 1 && dy == -1 ? body_part_arm_r :
                                               dx == 0 && dy == 1 ? body_part_leg_l : dx == -1 && dy == 0 ? body_part_arm_l :
                                               bodypart_str_id::NULL_ID();
                    if( !bp.is_null() ) {
                        // As CDDA's grab attack (mattack_actors.cpp): the grabber's side and the effect.
                        z->add_grab( bp );
                        u.add_effect( effect_grabbed, 1_days, bp.id(), true, 100 );
                    }
                }
            }
        }
    }
    here.invalidate_map_cache( p.z() );
    here.build_map_cache( p.z() );
}

// The unstoppable checks' look at the scene (hand_setup 45, between two requests: nothing acts).
void unstop_look()
{
    const avatar &u = get_avatar();
    map &here = get_map();
    int grabbing = 0;
    int zombies = 0;
    int hp = 0;
    for( monster &z : g->all_monsters() ) {
        zombies++;
        hp += z.get_hp();
        for( const bodypart_str_id &bp : {
                 body_part_torso, body_part_arm_r, body_part_leg_l, body_part_arm_l
             } ) {
            grabbing += z.is_grabbing( bp ) ? 1 : 0;
        }
    }
    const tripoint_rel_ms d = u.pos_abs() - unstop_origin;
    creature_tracker &creatures = get_creature_tracker();
    const Creature *at_origin = creatures.creature_at( here.get_bub( unstop_origin ) );
    const Creature *at_east = creatures.creature_at( here.get_bub( unstop_origin ) + tripoint_rel_ms( 1, 0, 0 ) );
    int under = 0;
    for( monster &z : g->all_monsters() ) {
        under += z.pos_abs() == u.pos_abs() ? 1 : 0;
    }
    unstop_view.under = under;
    unstop_view.east = at_east == nullptr || at_east->is_avatar() ? 0 : at_east->is_npc() ? 2 : 1;
    unstop_view.grabbed = static_cast<int>( u.get_effects_with_flag( json_flag_GRAB ).size() );
    unstop_view.grabbing = grabbing;
    unstop_view.zombies = zombies;
    unstop_view.hp = hp;
    unstop_view.dx = d.x();
    unstop_view.dy = d.y();
    unstop_view.origin = at_origin == nullptr || at_origin->is_avatar() ? 0 : at_origin->is_npc() ? 2 : 1;
    unstop_view.turn = to_turn<int>( calendar::turn );
}

void observe()
{
    observe_calls++;
    // As the client's hook (bridge/src/sim_session.cpp).
    fp3d::run_range_requests();
    if( number_setup.exchange( 0 ) != 0 ) {
        ask_numbers();
    }
    if( range_fill.exchange( false ) ) {
        fill_range_ops();
    }
    if( const int round = pt_round.exchange( 0 ); round != 0 ) {
        set_up_point_teleports( round );
    }
    if( const int setup = weather_mood_setup.exchange( 0 ); setup != 0 ) {
        set_up_weather_mood( setup == 1 );
    }
    if( const int ww = weather_wait.exchange( 0 ); ww != 0 ) {
        wait_in_weather( ww != 2, ww == 3 ? 60 : 20 );
    }
    if( const int stage = interrupt_setup.exchange( 0 ); stage != 0 ) {
        set_up_interrupt( stage );
    }
    if( furniture_setup.exchange( 0 ) != 0 ) {
        set_up_furniture();
    }
    if( const int stage = physics_setup.exchange( 0 ); stage == 1 ) {
        set_up_item_physics();
    } else if( stage == 2 ) {
        wield_water_pot();
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
        case 40: {
            // The smash checks (run_smash_checks): flat dirt, a rock wielded, a window east and another 2 north.
            g->clear_zombies();
            for( int dx = -2; dx <= 2; dx++ ) {
                for( int dy = -2; dy <= 2; dy++ ) {
                    const tripoint_bub_ms q = p + tripoint_rel_ms( dx, dy, 0 );
                    here.i_clear( q );
                    here.furn_set( q, furn_str_id::NULL_ID() );
                    here.ter_set( q, ter_t_dirt );
                }
            }
            here.ter_set( p + tripoint_rel_ms( 1, 0, 0 ), ter_str_id( "t_window" ) );
            here.ter_set( p + tripoint_rel_ms( 0, -2, 0 ), ter_str_id( "t_window" ) );
            const item_location w = get_avatar().get_wielded_item();
            smash_saved_weapon = w ? std::optional<item>( *w ) : std::nullopt;
            get_avatar().set_wielded_item( item( itype_rock ) );
            here.invalidate_map_cache( p.z() );
            here.build_map_cache( p.z() );
            break;
        }
        case 42:
            unstop_scene( 42 );
            break;
        case 43:
            unstop_scene( 43 );
            break;
        case 44:
            unstop_scene( 44 );
            break;
        case 46:
            unstop_scene( 46 );
            break;
        case 47:
            unstop_scene( 47 );
            break;
        case 48:
            unstop_scene( 48 );
            break;
        case 49:
            unstop_scene( 49 );
            break;
        case 45:
            unstop_look();
            break;
        case 41: {
            // After the smash checks: flat dirt again (no glass about), the weapon from before.
            for( int dx = -2; dx <= 2; dx++ ) {
                for( int dy = -2; dy <= 2; dy++ ) {
                    const tripoint_bub_ms q = p + tripoint_rel_ms( dx, dy, 0 );
                    here.i_clear( q );
                    here.ter_set( q, ter_t_dirt );
                }
            }
            if( smash_saved_weapon ) {
                get_avatar().set_wielded_item( *smash_saved_weapon );
            } else {
                get_avatar().remove_weapon();
            }
            smash_saved_weapon.reset();
            here.invalidate_map_cache( p.z() );
            here.build_map_cache( p.z() );
            break;
        }
        case 1:
            g->clear_zombies();
            g->place_critter_at( mon_zombie, p + tripoint_rel_ms( -1, 0, 0 ) );
            get_avatar().set_wielded_item( item( itype_rock ) );
            break;
        case 2:
            get_avatar().set_wielded_item( item( itype_2h_flail_steel ) );
            break;
        case 3: {
            // The jerrycan with 4 L of gasoline (4.6 kg) must still fit one hand: strength 12 at least (CDDA's
            // is_two_handed wants 10) until stage 8, which puts the random start's back.
            siphon_str_saved = get_avatar().str_max;
            get_avatar().str_max = std::max( get_avatar().str_max, 12 );
            get_avatar().reset_stats();
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
            if( siphon_str_saved > 0 ) {
                u.str_max = siphon_str_saved;
                siphon_str_saved = -1;
                u.reset_stats();
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
            gun.set_flag( flag_NEVER_JAMS );   // each shot fires (no random malfunction: CDDA's roll)
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
        case 18: {
            // Consume: floor 3 squares around, nothing lying there; a backpack worn, nothing held. Carried: an apple, a
            // rotten apple, a bottle of 500 ml of clean water, a rock. Lying: an apple west, an apple 2 squares east,
            // clean water spilt north.
            avatar &u = get_avatar();
            g->clear_zombies();
            for( int dx = -3; dx <= 3; dx++ ) {
                for( int dy = -3; dy <= 3; dy++ ) {
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
            const auto handle = []( item_location loc ) {
                return loc ? fp3d::item_handle( *loc ) : 0;
            };
            cs_apple_uid = handle( u.i_add( item( itype_apple ) ) );
            item rotten( itype_apple );
            rotten.set_relative_rot( 1.2 );
            cs_rotten_uid = handle( u.i_add( rotten ) );
            item bottle( itype_bottle_plastic );
            bottle.put_in( item( itype_water_clean, calendar::turn, 2 ), pocket_type::CONTAINER );
            item_location b = u.i_add( bottle );
            cs_bottle_uid = handle( b );
            cs_water_uid = 0;
            if( b ) {
                for( item *in : b->all_items_top( pocket_type::CONTAINER ) ) {
                    cs_water_uid = fp3d::item_handle( *in );
                }
            }
            cs_rock_uid = handle( u.i_add( item( itype_rock ) ) );
            const auto lay = [&here]( const tripoint_bub_ms & at, const item & it ) {
                item &laid = here.add_item( at, it );
                return laid.is_null() ? 0 : fp3d::item_handle( laid );
            };
            cs_west_apple_uid = lay( p + tripoint_rel_ms( -1, 0, 0 ), item( itype_apple ) );
            cs_far_apple_uid = lay( p + tripoint_rel_ms( 2, 0, 0 ), item( itype_apple ) );
            cs_spilt_uid = lay( p + tripoint_rel_ms( 0, -1, 0 ), item( itype_water_clean, calendar::turn, 1 ) );
            here.build_map_cache( p.z() );
            break;
        }
        case 22: {
            // Lying containers (L8 #3, #4): floor 3 squares around, nothing lying there; a backpack worn, nothing
            // held. West: a backpack holding a rock and 2 empty bottles (rocks are ammo: several merge into one
            // stack), and a sealed bottle of water. 2 west: a backpack holding a rock.
            // East: 40 rocks. 2 east: 40 rocks.
            avatar &u = get_avatar();
            g->clear_zombies();
            for( int dx = -3; dx <= 3; dx++ ) {
                for( int dy = -3; dy <= 3; dy++ ) {
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
            const tripoint_bub_ms west = p + tripoint_rel_ms( -1, 0, 0 );
            item pack( itype_backpack );
            pack.put_in( item( itype_rock ), pocket_type::CONTAINER );
            pack.put_in( item( itype_bottle_plastic ), pocket_type::CONTAINER );
            pack.put_in( item( itype_bottle_plastic ), pocket_type::CONTAINER );
            item &laid = here.add_item( west, pack );
            lc_pack_uid = laid.is_null() ? 0 : fp3d::item_handle( laid );
            lc_rock_uid = 0;
            if( !laid.is_null() ) {
                for( item *in : laid.all_items_top( pocket_type::CONTAINER ) ) {
                    if( in->typeId() == itype_rock ) {
                        lc_rock_uid = fp3d::item_handle( *in );
                    }
                }
            }
            item bottle( itype_bottle_plastic );
            bottle.put_in( item( itype_water_clean, calendar::turn, 2 ), pocket_type::CONTAINER );
            for( item_pocket *pk : bottle.get_all_contained_pockets() ) {
                pk->seal();
            }
            item &sealed = here.add_item( west, bottle );
            lc_sealed_uid = sealed.is_null() ? 0 : fp3d::item_handle( sealed );
            lc_sealed_water_uid = 0;
            if( !sealed.is_null() ) {
                for( item *in : sealed.all_items_top( pocket_type::CONTAINER ) ) {
                    lc_sealed_water_uid = fp3d::item_handle( *in );
                }
            }
            item far_pack( itype_backpack );
            far_pack.put_in( item( itype_rock ), pocket_type::CONTAINER );
            item &far_laid = here.add_item( p + tripoint_rel_ms( -2, 0, 0 ), far_pack );
            lc_far_pack_uid = far_laid.is_null() ? 0 : fp3d::item_handle( far_laid );
            for( int i = 0; i < 40; i++ ) {
                here.add_item( p + tripoint_rel_ms( 1, 0, 0 ), item( itype_rock ) );
                here.add_item( p + tripoint_rel_ms( 2, 0, 0 ), item( itype_rock ) );
            }
            here.build_map_cache( p.z() );
            break;
        }
        case 21:
            get_avatar().str_max = 8;
            get_avatar().reset_stats();
            break;
        case 23: {
            // Character query (G4): bad hearing as a starting trait, night vision as a mutation, a cranial
            // flashlight (off), 50 of 100 kJ, winded. Then the sheet run here, in full and with 300 bytes less room.
            avatar &u = get_avatar();
            if( u.has_trait( trait_NIGHTVISION ) ) {
                if( u.has_base_trait( trait_NIGHTVISION ) ) {
                    u.toggle_trait( trait_NIGHTVISION );
                } else {
                    u.unset_mutation( trait_NIGHTVISION );
                }
            }
            u.set_mutation( trait_NIGHTVISION );
            if( u.has_trait( trait_BADHEARING ) && !u.has_base_trait( trait_BADHEARING ) ) {
                u.unset_mutation( trait_BADHEARING );
            }
            if( !u.has_trait( trait_BADHEARING ) ) {
                u.toggle_trait( trait_BADHEARING );
            }
            if( !u.has_bionic( bio_flashlight ) ) {
                u.add_bionic( bio_flashlight );
            }
            u.set_max_power_level( 100_kJ );
            u.set_power_level( 50_kJ );
            u.add_effect( effect_winded, 1_hours );
            const std::string full = fp3d::run_character_query( 901, SIZE_MAX );
            const std::string cut = fp3d::run_character_query( 902, full.size() > 300 ? full.size() - 300 : 0 );
            const std::string tiny = fp3d::run_character_query( 903, 3000 );
            std::lock_guard<std::mutex> lk( cq_m );
            cq_full = full;
            cq_cut = cut;
            cq_tiny = tiny;
            cq_name = fp3d::client_text( u.get_name(), fp3d::max_sheet_name_columns, 4 * fp3d::max_sheet_name_columns );
            cq_carried[0] = static_cast<int>( units::to_gram( u.weight_carried() ) );
            cq_carried[1] = static_cast<int>( units::to_gram( u.weight_capacity() ) );
            cq_carried[2] = units::to_milliliter( u.volume_carried() );
            cq_carried[3] = units::to_milliliter( u.volume_capacity() );
            break;
        }
        case 25: {
            // Peek (B2): floor 6 squares around, nothing lying there; a backpack worn, a rock carried, a rock wielded.
            avatar &u = get_avatar();
            g->clear_zombies();
            for( int dx = -6; dx <= 6; dx++ ) {
                for( int dy = -6; dy <= 6; dy++ ) {
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
            item_location carried = u.i_add( item( itype_rock ) );
            pk_rock_uid = carried ? fp3d::item_handle( *carried ) : 0;
            u.set_wielded_item( item( itype_rock ) );
            item_location wielded = u.get_wielded_item();
            pk_wielded_uid = wielded ? fp3d::item_handle( *wielded ) : 0;
            here.build_map_cache( p.z() );
            break;
        }
        case 26: {
            // Cooking: the avatar's square filled (logs, then single rocks) until less room is left than a rock;
            // 4 fresh water in the pot on the fire (CDDA boils it clean there in time).
            fresh_cook_water( here, p + tripoint_rel_ms( -1, 1, 0 ) );
            int n = 0;
            const item log( itype_log );
            while( n < 200 && log.volume() > 0_ml && here.free_volume( p ) >= log.volume() ) {
                here.add_item( p, log );
                n++;
            }
            const item rock( itype_rock, calendar::turn, 1 );
            while( n < 2000 && rock.volume() > 0_ml && here.free_volume( p ) >= rock.volume() ) {
                here.add_item( p, rock );
                n++;
            }
            cook_full_free_ml = units::to_milliliter( here.free_volume( p ) );
            break;
        }
        case 27: {
            // The fillers gone again (anything else on the square stays); 4 fresh water in the pot again.
            fresh_cook_water( here, p + tripoint_rel_ms( -1, 1, 0 ) );
            for( bool again = true; again; ) {
                again = false;
                for( item &it : here.i_at( p ) ) {
                    if( it.typeId() == itype_log || it.typeId() == itype_rock ) {
                        here.i_rem( p, &it );
                        again = true;
                        break;
                    }
                }
            }
            break;
        }
        case 28: {
            // Cooking: the pot off the fire onto the square north of it. CDDA's own way on with the paused craft at
            // the avatar's feet (activating it) is tried: its craft activity must not start.
            move_cook_pot( here, p + tripoint_rel_ms( -1, 1, 0 ), p + tripoint_rel_ms( -1, 0, 0 ) );
            for( item &it : here.i_at( p ) ) {
                if( it.is_craft() ) {
                    item_location loc( map_cursor( p ), &it );
                    get_avatar().assign_activity( craft_activity_actor( loc, false ) );
                    cook_own_resume = get_avatar().activity.is_null() ? 1 : 2;
                    break;
                }
            }
            break;
        }
        case 29:
            // The pot back on the fire.
            move_cook_pot( here, p + tripoint_rel_ms( -1, 0, 0 ), p + tripoint_rel_ms( -1, 1, 0 ) );
            break;
        case 30: {
            // Fuel and a filled lighter carried.
            avatar &u = get_avatar();
            item_location fuel = u.i_add( item( itype_splinter ) );
            cook_fuel_uid = fuel ? fp3d::item_handle( *fuel ) : 0;
            item lighter( itype_lighter );
            lighter.ammo_set( lighter.ammo_default() );
            u.i_add( lighter );
            break;
        }
        case 31:
            // A log on the lit fireplace: it burns to the end of the craft.
            here.add_item( p + tripoint_rel_ms( -1, 1, 0 ), item( itype_log ) );
            break;
        case 32: {
            // The fireplace as scene 14 left it: the pot alone, a fire without fuel.
            const tripoint_bub_ms fire_at = p + tripoint_rel_ms( -1, 1, 0 );
            for( bool again = true; again; ) {
                again = false;
                for( item &it : here.i_at( fire_at ) ) {
                    if( it.typeId() != itype_pot ) {
                        here.i_rem( fire_at, &it );
                        again = true;
                        break;
                    }
                }
            }
            here.remove_field( fire_at, fd_fire );
            here.add_field( fire_at, fd_fire, 2 );
            break;
        }
        case 36: {
            // Gun mods by hand: a Glock 19 in hand, the skills its mods ask for, and in the pack two laser sights,
            // a red dot sight (the pistol has no slot for it), a ported barrel (permanent) and a pistol sight mount
            // (permanent and difficult).
            avatar &u = get_avatar();
            u.set_skill_level( skill_id( "pistol" ), 6 );
            u.set_skill_level( skill_id( "gun" ), 6 );
            u.remove_weapon();
            u.set_wielded_item( item( itype_id( "glock_19" ) ) );
            for( const char *mod : { "rail_laser_sight", "rail_laser_sight", "red_dot_sight", "barrel_ported",
                                     "sights_mount_pistol"
                                   } ) {
                u.i_add( item( itype_id( mod ) ) );
            }
            break;
        }
        case 52: {
            // Gun mods in a vise (FP3D): a bench vise lying on the avatar's square, a clamp (VISE 1, too small for a
            // gun) on the square west of it, another bench vise three squares west, out of reach, and one east of it
            // that belongs to the free merchants.
            avatar &u = get_avatar();
            const tripoint_bub_ms at = u.pos_bub();
            const tripoint_bub_ms west = at + tripoint_rel_ms( -1, 0, 0 );
            const tripoint_bub_ms far_west = at + tripoint_rel_ms( -3, 0, 0 );
            const tripoint_bub_ms east = at + tripoint_rel_ms( 1, 0, 0 );
            for( const tripoint_bub_ms &q : { at, west, far_west, east } ) {
                here.i_clear( q );
            }
            here.add_item( at, item( itype_id( "bench_vise" ) ) );
            here.add_item( west, item( itype_id( "clamp" ) ) );
            here.add_item( far_west, item( itype_id( "bench_vise" ) ) );
            item theirs( itype_id( "bench_vise" ) );
            theirs.set_owner( faction_id( "free_merchants" ) );
            here.add_item( east, theirs );
            break;
        }
        case 37: {
            // Gun mods, part 2: a red dot sight and an underbarrel shotgun put straight into the gun (CDDA's put_in,
            // no skill or slot check), the shotgun loaded and marked as one that can't be unloaded; and no skill
            // left, so the character couldn't put the sight back.
            avatar &u = get_avatar();
            if( item_location gun = u.get_wielded_item(); gun && gun->is_gun() ) {
                gun->put_in( item( itype_id( "red_dot_sight" ) ), pocket_type::MOD );
                item launcher( itype_id( "u_shotgun" ) );
                launcher.ammo_set( launcher.ammo_default(), 2 );
                launcher.set_flag( flag_NO_UNLOAD );
                gun->put_in( launcher, pocket_type::MOD );
            }
            for( const char *skill : { "pistol", "gun", "shotgun" } ) {
                u.set_skill_level( skill_id( skill ), 0 );
                u.set_knowledge_level( skill_id( skill ), 0 );
            }
            break;
        }
        case 38: {
            // Gun mods, part 2: the skills back, and a mod job set aside unfinished.
            avatar &u = get_avatar();
            for( const char *skill : { "pistol", "gun", "shotgun" } ) {
                u.set_skill_level( skill_id( skill ), 6 );
                u.set_knowledge_level( skill_id( skill ), 6 );
            }
            u.backlog.push_front( player_activity( gunmod_add_activity_actor( 100, "x" ) ) );
            break;
        }
        case 50: {
            // First aid: floor 3 squares around, nothing lying there, no monsters; a backpack worn, nothing held.
            // Carried: three bandages and an apple. The left arm is hurt, the right arm bleeds at full health, the
            // right leg is broken; the torso and the rest are whole.
            avatar &u = get_avatar();
            g->clear_zombies();
            for( int dx = -3; dx <= 3; dx++ ) {
                for( int dy = -3; dy <= 3; dy++ ) {
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
            u.healall( 100 );
            u.remove_effect( effect_bleed );
            u.remove_effect( effect_bandaged );
            u.remove_effect( effect_disinfected );
            const bodypart_id arm_l( "arm_l" );
            const bodypart_id arm_r( "arm_r" );
            const bodypart_id leg_r( "leg_r" );
            u.set_part_hp_cur( arm_l, u.get_part_hp_max( arm_l ) - 10 );
            u.set_part_hp_cur( leg_r, 0 );
            u.add_effect( effect_bleed, 1_hours, arm_r, false, 10 );
            const auto handle = []( item_location loc ) {
                return loc ? fp3d::item_handle( *loc ) : 0;
            };
            fa_bandage_a_uid = handle( u.i_add( item( itype_bandages ) ) );
            fa_bandage_b_uid = handle( u.i_add( item( itype_bandages ) ) );
            fa_bandage_c_uid = handle( u.i_add( item( itype_bandages ) ) );
            fa_apple_uid = handle( u.i_add( item( itype_apple ) ) );
            here.build_map_cache( p.z() );
            break;
        }
        case 51: {
            // First aid, after: whole again, nothing bleeding or bandaged.
            avatar &u = get_avatar();
            u.healall( 100 );
            u.remove_effect( effect_bleed );
            u.remove_effect( effect_bandaged );
            u.remove_effect( effect_disinfected );
            break;
        }
        case 53: {
            // Driving: a paved strip nine squares wide from 8 squares west to 60 east of the avatar with nothing on
            // it, no monsters; a car facing east on it with the avatar boarded on its controls, nothing in hand.
            avatar &u = get_avatar();
            g->clear_zombies();
            for( int dx = -8; dx <= 60; dx++ ) {
                for( int dy = -4; dy <= 4; dy++ ) {
                    const tripoint_bub_ms q = p + tripoint_rel_ms( dx, dy, 0 );
                    if( !here.inbounds( q ) ) {
                        continue;
                    }
                    if( const optional_vpart_position vp = here.veh_at( q ) ) {
                        here.destroy_vehicle( &vp->vehicle() );
                    }
                    here.ter_set( q, ter_t_pavement );
                    here.furn_set( q, furn_str_id::NULL_ID() );
                    here.i_clear( q );
                    here.clear_fields( q );
                }
            }
            u.remove_weapon();
            if( u.controlling_vehicle || u.in_vehicle ) {
                u.controlling_vehicle = false;
                here.unboard_vehicle( u.pos_bub() );
            }
            vehicle *car = here.add_vehicle( vehicle_prototype_car, p, 0_degrees, 100, 2, false, true );
            if( car != nullptr ) {
                for( const vpart_reference &vpr : car->get_avail_parts( "CONTROLS" ) ) {
                    const tripoint_bub_ms seat = vpr.pos_bub( here );
                    u.setpos( here, seat );
                    here.board_vehicle( seat, &u );
                    break;
                }
            }
            here.build_map_cache( p.z() );
            break;
        }
        case 54: {
            // Driving, after: the avatar off the car, the car gone.
            avatar &u = get_avatar();
            u.controlling_vehicle = false;
            if( const optional_vpart_position vp = here.veh_at( u.pos_bub() ) ) {
                vehicle *car = &vp->vehicle();
                if( u.in_vehicle ) {
                    here.unboard_vehicle( u.pos_bub() );
                }
                here.destroy_vehicle( car );
            }
            here.build_map_cache( p.z() );
            break;
        }
        case 55: {
            // Leaning: floor 6 squares around, nothing lying there, no monsters. A wall runs along the row north of
            // the avatar from 6 squares west up to the square straight north of it; east of that the row is open.
            // A rock lies three squares straight north: behind the wall from where the avatar stands, in view from
            // the square east of it.
            avatar &u = get_avatar();
            g->clear_zombies();
            for( int dx = -6; dx <= 6; dx++ ) {
                for( int dy = -6; dy <= 6; dy++ ) {
                    const tripoint_bub_ms q = p + tripoint_rel_ms( dx, dy, 0 );
                    if( const optional_vpart_position vp = here.veh_at( q ) ) {
                        here.destroy_vehicle( &vp->vehicle() );
                    }
                    here.ter_set( q, dy == -1 && dx <= 0 ? ter_t_wall : ter_t_floor );
                    here.furn_set( q, furn_str_id::NULL_ID() );
                    here.i_clear( q );
                    here.clear_fields( q );
                }
            }
            if( u.is_crouching() ) {
                u.reset_move_mode();
            }
            item &rock = here.add_item( p + tripoint_rel_ms( 0, -3, 0 ), item( itype_rock ) );
            lean_rock_uid = rock.is_null() ? 0 : fp3d::item_handle( rock );
            here.build_map_cache( p.z() );
            break;
        }
        case 56: {
            // Cargo: floor 12 squares around, nothing lying there, no monsters; a car six squares east. 40 plastic
            // bottles (or as many as fit; they don't stack, as rocks would) go into its biggest cargo space, and the
            // avatar stands on a passable square next to that space.
            avatar &u = get_avatar();
            g->clear_zombies();
            for( int dx = -12; dx <= 12; dx++ ) {
                for( int dy = -12; dy <= 12; dy++ ) {
                    const tripoint_bub_ms q = p + tripoint_rel_ms( dx, dy, 0 );
                    if( !here.inbounds( q ) ) {
                        continue;
                    }
                    if( const optional_vpart_position vp = here.veh_at( q ) ) {
                        here.destroy_vehicle( &vp->vehicle() );
                    }
                    here.ter_set( q, ter_t_pavement );
                    here.furn_set( q, furn_str_id::NULL_ID() );
                    here.i_clear( q );
                    here.clear_fields( q );
                }
            }
            cargo_items = 0;
            cargo_centre = here.get_abs( p );
            vehicle *car = here.add_vehicle( vehicle_prototype_car, p + tripoint_rel_ms( 6, 0, 0 ), 0_degrees, 100, 2,
                                             false, true );
            if( car != nullptr ) {
                int best = -1;
                units::volume room = 0_ml;
                for( const vpart_reference &vpr : car->get_avail_parts( "CARGO" ) ) {
                    if( vpr.items().max_volume() > room ) {
                        room = vpr.items().max_volume();
                        best = static_cast<int>( vpr.part_index() );
                    }
                }
                if( best >= 0 ) {
                    cargo_car = car;
                    cargo_part = best;
                    vehicle_part &space = car->part( best );
                    vehicle_stack old = car->get_items( space );
                    while( !old.empty() ) {
                        old.erase( old.begin() );
                    }
                    int in = 0;
                    for( int i = 0; i < 40; i++ ) {
                        in += car->add_item( here, space, item( itype_bottle_plastic ) ) ? 1 : 0;
                    }
                    cargo_items = in;
                    // A square next to the space the avatar can stand on: off the car if there is one, else on it.
                    const tripoint_bub_ms at = car->bub_part_pos( here, space );
                    std::optional<tripoint_bub_ms> stand;
                    for( const bool off_car : { true, false } ) {
                        for( const tripoint_bub_ms &q : here.points_in_radius( at, 1 ) ) {
                            if( !stand && q != at && here.passable( q ) && ( !off_car || !here.veh_at( q ) ) ) {
                                stand = q;
                            }
                        }
                    }
                    if( stand ) {
                        u.setpos( here, *stand );
                        cargo_dx = at.x() - stand->x();
                        cargo_dy = at.y() - stand->y();
                    }
                }
            }
            here.build_map_cache( p.z() );
            break;
        }
        case 59: {
            // Cargo by hand: scene 56's car, the avatar still next to its biggest cargo space. The space now holds
            // five plastic bottles and a sixth that belongs to the free merchants. The avatar wears a backpack with
            // a rock in it and holds a giant backpack.
            avatar &u = get_avatar();
            cg_bottle_a_uid = 0;
            cg_free = 0;
            if( cargo_car != nullptr && cargo_part >= 0 ) {
                vehicle_part &space = cargo_car->part( cargo_part );
                vehicle_stack old = cargo_car->get_items( space );
                while( !old.empty() ) {
                    old.erase( old.begin() );
                }
                const auto into = [&]( const item & it ) -> uint64_t {
                    const std::optional<vehicle_stack::iterator> at = cargo_car->add_item( here, space, it );
                    return at ? fp3d::item_handle( **at ) : 0;
                };
                cg_bottle_a_uid = into( item( itype_bottle_plastic ) );
                cg_bottle_b_uid = into( item( itype_bottle_plastic ) );
                for( int i = 0; i < 3; i++ ) {
                    into( item( itype_bottle_plastic ) );
                }
                item theirs( itype_bottle_plastic );
                theirs.set_owner( faction_id( "free_merchants" ) );
                cg_theirs_uid = into( theirs );
            }
            u.clear_worn();
            u.inv->clear();
            u.remove_weapon();
            u.wear_item( item( itype_backpack ), false );
            item_location rock = u.i_add( item( itype_rock ) );
            cg_rock_uid = rock ? fp3d::item_handle( *rock ) : 0;
            u.set_wielded_item( item( itype_backpack_giant ) );
            item_location big = u.get_wielded_item();
            cg_big_uid = big ? fp3d::item_handle( *big ) : 0;
            for( const tripoint_bub_ms &q : here.points_in_radius( u.pos_bub(), 1 ) ) {
                if( cg_free.load() == 0 && q != u.pos_bub() && !here.veh_at( q ).cargo() ) {
                    cg_free = 1;
                    cg_free_dx = q.x() - u.pos_bub().x();
                    cg_free_dy = q.y() - u.pos_bub().y();
                }
            }
            here.build_map_cache( p.z() );
            break;
        }
        case 60: {
            // Cargo by hand: giant backpacks into that space until no more fit.
            if( cargo_car != nullptr && cargo_part >= 0 ) {
                vehicle_part &space = cargo_car->part( cargo_part );
                for( int i = 0; i < 400; i++ ) {
                    if( !cargo_car->add_item( here, space, item( itype_backpack_giant ) ) ) {
                        break;
                    }
                }
            }
            break;
        }
        case 57: {
            // Cargo: the avatar eight squares west of the scene's centre, 14 from the car.
            avatar &u = get_avatar();
            if( cargo_centre ) {
                u.setpos( here, here.get_bub( *cargo_centre ) + tripoint_rel_ms( -8, 0, 0 ) );
            }
            here.build_map_cache( p.z() );
            break;
        }
        case 58: {
            // Cargo, after: the car gone.
            for( const wrapped_vehicle &wv : here.get_vehicles() ) {
                if( cargo_centre && wv.v->type == vehicle_prototype_car &&
                    rl_dist( wv.pos, here.get_bub( *cargo_centre ) ) <= 24 ) {
                    here.destroy_vehicle( wv.v );
                    break;
                }
            }
            cargo_centre.reset();
            cargo_car = nullptr;
            cargo_part = -1;
            here.build_map_cache( p.z() );
            break;
        }
        case 61: {
            // Glovebox: floor 12 squares around, a car fresh from its prototype six squares east. A plastic bottle
            // goes into its glovebox and the avatar sits on the driver's seat. Also looked up: which prototypes
            // have a glovebox.
            avatar &u = get_avatar();
            g->clear_zombies();
            for( int dx = -12; dx <= 12; dx++ ) {
                for( int dy = -12; dy <= 12; dy++ ) {
                    const tripoint_bub_ms q = p + tripoint_rel_ms( dx, dy, 0 );
                    if( !here.inbounds( q ) ) {
                        continue;
                    }
                    if( const optional_vpart_position vp = here.veh_at( q ) ) {
                        here.destroy_vehicle( &vp->vehicle() );
                    }
                    here.ter_set( q, ter_t_pavement );
                    here.furn_set( q, furn_str_id::NULL_ID() );
                    here.i_clear( q );
                    here.clear_fields( q );
                }
            }
            const auto boxes_of = []( const vehicle & veh ) {
                int n = 0;
                for( int i = 0; i < veh.part_count(); i++ ) {
                    n += veh.part( i ).info().id == fp3d::glovebox_part() ? 1 : 0;
                }
                return n;
            };
            int with = 0;
            for( const vehicle_prototype &proto : vehicles::get_all_prototypes() ) {
                with += proto.blueprint && boxes_of( *proto.blueprint ) > 0 ? 1 : 0;
            }
            glove_protos = with;
            int kinds = 0;
            for( size_t i = 0; i < glove_checked.size(); i++ ) {
                const vproto_id id( glove_checked[i] );
                const int n = id.is_valid() && id->blueprint ? boxes_of( *id->blueprint ) : -1;
                kinds |= n == ( i < glove_checked_with ? 1 : 0 ) ? 1 << i : 0;
            }
            glove_kinds = kinds;
            glove_count = 0;
            glove_tile = 0;
            glove_dx = 0;
            glove_dy = 0;
            glove_bottle_uid = 0;
            glove_from = here.get_abs( p );
            vehicle *car = here.add_vehicle( vehicle_prototype_car, p + tripoint_rel_ms( 6, 0, 0 ), 0_degrees, 100, 2,
                                             false, true );
            glove_car = car;
            if( car != nullptr ) {
                int box = -1;
                int controls = -1;
                for( int i = 0; i < car->part_count(); i++ ) {
                    const vpart_info &vpi = car->part( i ).info();
                    if( vpi.id == fp3d::glovebox_part() ) {
                        box = i;
                    }
                    if( controls < 0 && vpi.has_flag( "CONTROLS" ) ) {
                        controls = i;
                    }
                }
                glove_count = boxes_of( *car );
                if( box >= 0 && controls >= 0 ) {
                    vehicle_part &space = car->part( box );
                    const point_rel_ms behind = space.mount - point::east;
                    const point_rel_ms driver = car->part( controls ).mount;
                    int tile = 0;
                    for( int i = 0; i < car->part_count(); i++ ) {
                        const vehicle_part &vp = car->part( i );
                        tile |= vp.mount == space.mount && vp.info().has_flag( "WINDSHIELD" ) ? 1 : 0;
                        tile |= vp.mount == behind && vp.info().has_flag( "SEAT" ) ? 2 : 0;
                    }
                    tile |= behind.x() == driver.x() && std::abs( behind.y() - driver.y() ) == 1 ? 4 : 0;
                    glove_tile = tile;
                    const std::optional<vehicle_stack::iterator> in =
                        car->add_item( here, space, item( itype_bottle_plastic ) );
                    glove_bottle_uid = in ? fp3d::item_handle( **in ) : 0;
                    const tripoint_bub_ms seat = car->bub_part_pos( here, car->part( controls ) );
                    const tripoint_bub_ms at = car->bub_part_pos( here, space );
                    u.setpos( here, seat );
                    glove_dx = at.x() - seat.x();
                    glove_dy = at.y() - seat.y();
                }
            }
            here.build_map_cache( p.z() );
            break;
        }
        case 62: {
            // Glovebox, after: the avatar back where it stood, the car gone.
            if( glove_from ) {
                get_avatar().setpos( here, here.get_bub( *glove_from ) );
                glove_from.reset();
            }
            if( glove_car != nullptr ) {
                here.destroy_vehicle( glove_car );
                glove_car = nullptr;
            }
            here.build_map_cache( p.z() );
            break;
        }
        case 63: {
            // Vehicle work: floor 12 squares around, a car fresh from its prototype six squares east, its glovebox
            // damaged. The avatar stands outside the car, on the free square beside it nearest the glovebox, and
            // knows no mechanics.
            avatar &u = get_avatar();
            g->clear_zombies();
            for( int dx = -12; dx <= 12; dx++ ) {
                for( int dy = -12; dy <= 12; dy++ ) {
                    const tripoint_bub_ms q = p + tripoint_rel_ms( dx, dy, 0 );
                    if( !here.inbounds( q ) ) {
                        continue;
                    }
                    if( const optional_vpart_position vp = here.veh_at( q ) ) {
                        here.destroy_vehicle( &vp->vehicle() );
                    }
                    here.ter_set( q, ter_t_pavement );
                    here.furn_set( q, furn_str_id::NULL_ID() );
                    here.i_clear( q );
                    here.clear_fields( q );
                }
            }
            work_env = 0;
            work_from = here.get_abs( p );
            work_skill[0] = u.get_skill_level( skill_id( "mechanics" ) );
            work_skill[1] = u.get_knowledge_level( skill_id( "mechanics" ) );
            u.set_skill_level( skill_id( "mechanics" ), 0 );
            u.set_knowledge_level( skill_id( "mechanics" ), 0 );
            vehicle *car = here.add_vehicle( vehicle_prototype_car, p + tripoint_rel_ms( 6, 0, 0 ), 0_degrees, 100, 2,
                                             false, true );
            work_car = car;
            int env = 0;
            if( car != nullptr ) {
                car->name = "fp3d work car";
                for( int i = 0; i < car->part_count(); i++ ) {
                    vehicle_part &box = car->part( i );
                    if( box.info().id != fp3d::glovebox_part() ) {
                        continue;
                    }
                    car->mod_hp( box, -box.hp() / 2 );
                    env |= box.is_repairable() && !box.is_broken() ? 4 : 0;
                    const tripoint_bub_ms at = car->bub_part_pos( here, box );
                    for( int reach = 1; reach <= 4 && ( env & 8 ) == 0; reach++ ) {
                        for( const tripoint_bub_ms &q : here.points_in_radius( at, reach ) ) {
                            if( here.veh_at( q ) || !here.passable( q ) ) {
                                continue;
                            }
                            bool beside_car = false;
                            for( const tripoint_bub_ms &next : here.points_in_radius( q, 1 ) ) {
                                const optional_vpart_position there = here.veh_at( next );
                                beside_car = beside_car || ( there && &there->vehicle() == car );
                            }
                            if( beside_car ) {
                                u.setpos( here, q );
                                env |= 8;
                                break;
                            }
                        }
                    }
                    break;
                }
            }
            here.build_map_cache( p.z() );
            env |= u.fine_detail_vision_mod() <= 4 ? 1 : 0;
            env |= u.has_morale_to_craft() ? 2 : 0;
            work_env = env;
            break;
        }
        case 67: {
            // Vehicle work: an appliance three squares west of where the avatar stood before scene 63 (the car is
            // east of there). CDDA has another screen for those.
            avatar &u = get_avatar();
            int state = 0;
            work_machine = nullptr;
            const vpart_id machine( "ap_air_compressor" );
            const tripoint_bub_ms machine_at = ( work_from ? here.get_bub( *work_from ) : u.pos_bub() ) +
                                               tripoint_rel_ms( -3, 0, 0 );
            state |= machine.is_valid() ? 1 : 0;
            if( machine.is_valid() && !here.veh_at( machine_at ) &&
                place_appliance( here, machine_at, machine, u ) ) {
                state |= 2;
                if( const optional_vpart_position placed = here.veh_at( machine_at ) ) {
                    work_machine = &placed->vehicle();
                    work_machine->name = "fp3d work machine";
                    state |= 4;
                }
            }
            work_machine_state = state;
            here.build_map_cache( p.z() );
            break;
        }
        case 68: {
            // Vehicle work: the car belongs to someone else now.
            const faction_id other( "robofac" );
            work_machine_state = 0;
            if( work_car != nullptr && other.is_valid() ) {
                work_car->set_owner( other );
                work_machine_state = 1;
            }
            break;
        }
        case 64: {
            // Vehicle work, part 2: mechanics 4, a screwdriver and duct tape in the pack. Also read here, on the sim
            // thread: the glovebox's query within 2500 bytes.
            avatar &u = get_avatar();
            u.set_skill_level( skill_id( "mechanics" ), 4 );
            u.set_knowledge_level( skill_id( "mechanics" ), 4 );
            item driver( itype_id( "screwdriver" ) );
            driver.set_var( "fp3d_work_test", 1 );
            u.i_add( driver );
            item tape( itype_id( "duct_tape" ), calendar::turn, 200 );
            tape.set_var( "fp3d_work_test", 1 );
            u.i_add( tape );
            u.invalidate_crafting_inventory();
            const std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
            work_tiny = fp3d::run_vehicle_work_query( 905, work_uid.load(), work_row.load(), 2500 );
            work_ms = static_cast<int>( std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - began ).count() );
            break;
        }
        case 65: {
            // Vehicle work, part 3: the avatar back where it stood, six squares from the car.
            if( work_from ) {
                get_avatar().setpos( here, here.get_bub( *work_from ) );
            }
            here.build_map_cache( p.z() );
            break;
        }
        case 69: {
            // Vehicle work: a rotor on the car for one read. CDDA rolls what smashing one off may yield just to
            // list it; the read leaves the game's dice as they were. The rotor comes off again here.
            int state = 0;
            const vpart_id rotor( "small_civilian_rotors" );
            if( work_car != nullptr && rotor.is_valid() && work_car->part_count() > 0 &&
                work_car->can_mount( work_car->part( 0 ).mount, *rotor ).success() ) {
                const int had = work_car->part_count();
                const int idx = work_car->install_part( here, work_car->part( 0 ).mount, rotor );
                if( idx >= 0 ) {
                    state |= 1;
                    const std::vector<int> rows = fp3d::vehicle_part_rows( *work_car );
                    const auto at = std::find( rows.begin(), rows.end(), idx );
                    if( at != rows.end() ) {
                        const cata_default_random_engine before = rng_get_engine();
                        const std::string read = fp3d::run_vehicle_work_query( 906, work_uid.load(),
                                                 static_cast<int>( at - rows.begin() ), 64 * 1024 );
                        state |= read.find( "may yield" ) != std::string::npos ? 2 : 0;
                        state |= rng_get_engine() == before ? 4 : 0;
                    }
                    work_car->remove_part( work_car->part( idx ) );
                    work_car->part_removal_cleanup( here );
                    state |= work_car->part_count() == had ? 8 : 0;
                }
            }
            work_gate_state = state;
            here.build_map_cache( p.z() );
            break;
        }
        case 70: {
            // Vehicle work: the avatar on a horse, on a free square next to where it stood.
            avatar &u = get_avatar();
            int state = 0;
            work_stood = u.pos_abs();
            for( const tripoint_bub_ms &q : closest_points_first( u.pos_bub(), 1, 1 ) ) {
                if( here.passable( q ) && g->is_empty( q ) && !here.veh_at( q ) ) {
                    if( monster *horse = g->place_critter_at( mon_horse, q ) ) {
                        u.mount_creature( *horse );
                        state |= 1;
                    }
                    break;
                }
            }
            state |= u.is_mounted() ? 2 : 0;
            work_gate_state = state;
            break;
        }
        case 71: {
            // Vehicle work: off the horse again (as Character::dismount, without its square menu), the horse gone,
            // the avatar back where it stood.
            avatar &u = get_avatar();
            if( u.is_mounted() ) {
                monster *horse = u.mounted_creature.get();
                u.remove_effect( efftype_id( "riding" ) );
                horse->remove_effect( efftype_id( "ridden" ) );
                horse->mounted_player_id = character_id();
                horse->mounted_player = nullptr;
                u.mounted_creature = nullptr;
                g->remove_zombie( *horse );
            }
            if( work_stood ) {
                u.setpos( here, here.get_bub( *work_stood ) );
                work_stood.reset();
            }
            work_gate_state = u.is_mounted() ? 0 : 1;
            here.build_map_cache( p.z() );
            break;
        }
        case 72: {
            // Vehicle work: the avatar has a roomy shell and is inside it.
            avatar &u = get_avatar();
            const trait_id shell( "SHELL2" );
            int state = 0;
            if( shell.is_valid() && !u.has_trait( shell ) ) {
                u.set_mutation( shell );
                u.activate_mutation( shell );
                state |= 1;
            }
            state |= u.has_active_mutation( shell ) ? 2 : 0;
            work_gate_state = state;
            break;
        }
        case 73: {
            // Vehicle work: out of the shell, the shell gone.
            avatar &u = get_avatar();
            const trait_id shell( "SHELL2" );
            if( u.has_trait( shell ) ) {
                u.deactivate_mutation( shell );
                u.unset_mutation( shell );
            }
            work_gate_state = u.has_trait( shell ) ? 0 : 1;
            here.build_map_cache( p.z() );
            break;
        }
        case 74: {
            // Vehicle work: a jumper cable from the car's battery to a battery on a frame put down a few squares
            // away. To list what removing the cable's part yields, CDDA resets a copy of the cable, and with it the
            // "linked" mark on the car's battery; the read leaves that mark as it found it. The cable and the frame
            // are gone again afterwards.
            int state = 0;
            const vpart_id battery_type( "battery_car" );
            const int battery = work_car != nullptr && !work_car->batteries.empty() ?
                                work_car->batteries.front() : -1;
            if( battery >= 0 && battery_type.is_valid() ) {
                const int had = work_car->part_count();
                const tripoint_bub_ms at = work_car->bub_part_pos( here, work_car->part( battery ) );
                vehicle *second = nullptr;
                for( const tripoint_bub_ms &q : closest_points_first( at, 2, 3 ) ) {
                    if( here.inbounds( q ) && !here.veh_at( q ) && here.passable( q ) && g->is_empty( q ) ) {
                        second = here.add_vehicle( vehicle_prototype_none, q, 0_degrees, 0, 0 );
                        break;
                    }
                }
                if( second != nullptr ) {
                    second->install_part( here, point_rel_ms::zero, vpart_frame );
                    if( second->can_mount( point_rel_ms::zero, *battery_type ).success() ) {
                        second->install_part( here, point_rel_ms::zero, battery_type );
                        state |= 1;
                    }
                    here.add_vehicle_to_cache( second );
                    item cable( itype_id( "jumper_cable" ) );
                    if( ( state & 1 ) != 0 &&
                        cable.link_to( *work_car, work_car->part( battery ).mount,
                                       link_state::vehicle_battery ).success() &&
                        cable.link_to( *second, point_rel_ms::zero, link_state::vehicle_battery ).success() ) {
                        state |= 2;
                    }
                    int cable_part = -1;
                    for( const vpart_reference &vpr : work_car->get_any_parts( VPFLAG_POWER_TRANSFER ) ) {
                        cable_part = static_cast<int>( vpr.part_index() );
                    }
                    const std::vector<int> rows = fp3d::vehicle_part_rows( *work_car );
                    const auto row = std::find( rows.begin(), rows.end(), cable_part );
                    if( cable_part >= 0 && row != rows.end() ) {
                        work_car->part( battery ).set_flag( vp_flag::linked_flag );
                        const std::string read = fp3d::run_vehicle_work_query( 907, work_uid.load(),
                                                 static_cast<int>( row - rows.begin() ), 64 * 1024 );
                        state |= read.find( "will yield" ) != std::string::npos ? 4 : 0;
                        state |= work_car->part( battery ).has_flag( vp_flag::linked_flag ) ? 8 : 0;
                        work_car->part( battery ).remove_flag( vp_flag::linked_flag );
                        work_car->remove_part( work_car->part( cable_part ) );
                        work_car->part_removal_cleanup( here );
                    }
                    here.destroy_vehicle( second );
                }
                state |= work_car->part_count() == had ? 16 : 0;
            }
            work_gate_state = state;
            here.build_map_cache( p.z() );
            break;
        }
        case 75: {
            // Vehicle work, mend and refill: the car's fuel tank a third down and a can of gasoline in the pack; an
            // engine with an expired air filter and a new filter in the pack.
            avatar &u = get_avatar();
            int state = 0;
            const fault_id stale( "fault_engine_filter_air" );
            for( int i = 0; work_car != nullptr && i < work_car->part_count(); i++ ) {
                vehicle_part &pt = work_car->part( i );
                if( ( state & 1 ) == 0 && pt.is_tank() && pt.ammo_remaining() > 0 ) {
                    work_car->drain( here, i, pt.ammo_remaining() / 3 );
                    state |= pt.can_reload() ? 1 : 0;
                }
                if( ( state & 2 ) == 0 && stale.is_valid() && pt.faults_potential().count( stale ) > 0 ) {
                    state |= pt.fault_set( stale ) ? 2 : 0;
                }
            }
            item can( itype_id( "jerrycan" ) );
            can.fill_with( item( itype_id( "gasoline" ), calendar::turn ), 2000 );
            can.set_var( "fp3d_work_test", 1 );
            state |= can.is_container_empty() ? 0 : 4;
            u.i_add( can );
            item filter( itype_id( "filter_air" ) );
            filter.set_var( "fp3d_work_test", 1 );
            u.i_add( filter );
            u.invalidate_crafting_inventory();
            work_gate_state = state;
            break;
        }
        case 76: {
            // The map: near the avatar's overmap cell, one cell made vague and one unseen, a note with a symbol, a
            // colour and a danger radius on an explored cell, and a travel path of five cells north. Also read
            // here, on the sim thread: the avatar's page whole and in fewer bytes.
            avatar &u = get_avatar();
            int state = 0;
            const tripoint_abs_omt at = u.pos_abs_omt();
            map_at[0] = at.x();
            map_at[1] = at.y();
            map_at[2] = at.z();
            const auto drawn = []( const tripoint_abs_omt & c, om_vision_level v, std::string * out ) {
                const oter_id &ter = overmap_buffer.ter_existing( c );
                if( ter->blends_adjacent( v ) ) {
                    const oter_vision::blended_omt as = oter_vision::get_blended_omt_info( c, v );
                    out[0] = as.name;
                    out[1] = as.sym;
                    out[2] = get_all_colors().get_name( as.color );
                    return;
                }
                out[0] = ter->get_name( v );
                out[1] = ter->get_symbol( v );
                out[2] = get_all_colors().get_name( ter->get_color( v ) );
            };
            drawn( at, overmap_buffer.seen( at ), map_here );
            // Two cells out of the avatar's sight (CDDA sees what is in sight anew before the next request), on
            // ground that has an overmap and that the player hasn't seen: one to be made vague, one to stay
            // unseen. A cell that looks different when seen vaguely is taken for the first if there is one.
            tripoint_abs_omt vague = at;
            tripoint_abs_omt unseen = at;
            for( const point &dir : { point( 40, 0 ), point( -40, 0 ), point( 0, 40 ), point( 0, -40 ) } ) {
                for( int dy = -4; dy <= 4; dy++ ) {
                    for( int dx = -4; dx <= 4; dx++ ) {
                        const tripoint_abs_omt c = at + tripoint_rel_omt( dir.x + dx, dir.y + dy, 0 );
                        if( overmap_buffer.get_existing( project_to<coords::om>( c ).xy() ) == nullptr ||
                            overmap_buffer.seen( c ) != om_vision_level::unseen ) {
                            continue;
                        }
                        const oter_id &ter = overmap_buffer.ter_existing( c );
                        if( ( state & 1 ) == 0 && !ter->blends_adjacent( om_vision_level::vague ) &&
                            ter->get_name( om_vision_level::vague ) != ter->get_name( om_vision_level::full ) ) {
                            vague = c;
                            state |= 1;
                        } else if( unseen == at ) {
                            unseen = c;
                        } else if( vague == at ) {
                            vague = c;
                        }
                    }
                }
            }
            state |= vague != at && unseen != at ? 8 : 0;
            // The unseen cell gets the explored mark: a mark on a cell the player hasn't seen isn't told.
            map_unseen_marked = unseen != at && !overmap_buffer.is_explored( unseen );
            if( map_unseen_marked ) {
                overmap_buffer.toggle_explored( unseen );
            }
            state |= unseen != at && overmap_buffer.is_explored( unseen ) ? 32 : 0;
            // A cell CDDA blends into its surroundings while it is seen only vaguely: a campsite among forest,
            // further out of sight still. All nine cells are seen vaguely.
            const oter_type_str_id camp_type( "campsite" );
            const oter_id camp = camp_type.is_valid() ? camp_type->get_first() : oter_id();
            const oter_str_id woods( "forest" );
            map_blend_was.clear();
            for( const point &dir : { point( 56, 0 ), point( -56, 0 ), point( 0, 56 ), point( 0, -56 ) } ) {
                const tripoint_abs_omt middle = at + tripoint_rel_omt( dir.x, dir.y, 0 );
                bool usable = camp_type.is_valid() && woods.is_valid() && map_blend_was.empty();
                for( int dy = -1; dy <= 1; dy++ ) {
                    for( int dx = -1; dx <= 1; dx++ ) {
                        const tripoint_abs_omt c = middle + tripoint_rel_omt( dx, dy, 0 );
                        usable = usable && overmap_buffer.get_existing( project_to<coords::om>( c ).xy() ) != nullptr &&
                                 overmap_buffer.seen( c ) == om_vision_level::unseen && c != vague && c != unseen;
                    }
                }
                if( !usable ) {
                    continue;
                }
                for( int dy = -1; dy <= 1; dy++ ) {
                    for( int dx = -1; dx <= 1; dx++ ) {
                        const tripoint_abs_omt c = middle + tripoint_rel_omt( dx, dy, 0 );
                        map_blend_was.push_back( overmap_buffer.ter_existing( c ) );
                        overmap_buffer.ter_set( c, dx == 0 && dy == 0 ? camp : woods.id() );
                        overmap_buffer.set_seen( c, om_vision_level::vague );
                    }
                }
                map_blend_at[0] = middle.x();
                map_blend_at[1] = middle.y();
                const oter_vision::blended_omt as = oter_vision::get_blended_omt_info( middle,
                                                    om_vision_level::vague );
                map_blend[0] = as.name;
                map_blend[1] = as.sym;
                map_blend[2] = get_all_colors().get_name( as.color );
                map_woods_name = woods->get_name( om_vision_level::vague );
                state |= camp->blends_adjacent( om_vision_level::vague ) ? 16 : 0;
            }
            tripoint_abs_omt noted = at;
            for( const point &d : { point( 1, 1 ), point( -1, -1 ), point( 1, -1 ), point( -1, 1 ) } ) {
                const tripoint_abs_omt c = at + tripoint_rel_omt( d.x, d.y, 0 );
                if( noted == at && c != vague && c != unseen ) {
                    noted = c;
                }
            }
            map_vague_at[0] = vague.x();
            map_vague_at[1] = vague.y();
            map_unseen_at[0] = unseen.x();
            map_unseen_at[1] = unseen.y();
            map_note_at[0] = noted.x();
            map_note_at[1] = noted.y();
            map_was[0] = static_cast<int>( overmap_buffer.seen( vague ) );
            map_was[1] = static_cast<int>( overmap_buffer.seen( unseen ) );
            map_full_name = overmap_buffer.ter_existing( vague )->get_name( om_vision_level::full );
            overmap_buffer.set_seen( vague, om_vision_level::vague );
            overmap_buffer.set_seen( unseen, om_vision_level::unseen );
            drawn( vague, om_vision_level::vague, map_vague );
            const std::string text = "!:R;fp3d map note";
            overmap_buffer.add_note( noted, text );
            overmap_buffer.mark_note_dangerous( noted, 3, true );
            const std::tuple<char, nc_color, size_t> shown = overmap_ui::get_note_display_info( text );
            map_note_sym = std::string( 1, std::get<0>( shown ) );
            map_note_color = get_all_colors().get_name( std::get<1>( shown ) );
            map_marked = !overmap_buffer.is_explored( noted );
            if( map_marked ) {
                overmap_buffer.toggle_explored( noted );
            }
            state |= overmap_buffer.has_note( noted ) && overmap_buffer.is_explored( noted ) ? 2 : 0;
            u.omt_path.clear();
            for( int i = 5; i >= 1; i-- ) {
                u.omt_path.push_back( at + tripoint_rel_omt( 0, -i, 0 ) );
            }
            const int ox = at.x() - ( ( at.x() % 32 ) + 32 ) % 32;
            const int oy = at.y() - ( ( at.y() % 32 ) + 32 ) % 32;
            map_far_at[0] = ox + 32 * 3000;
            map_far_at[1] = oy;
            const point_abs_om far_om = project_to<coords::om>( tripoint_abs_omt( map_far_at[0], map_far_at[1],
                                        at.z() ) ).xy();
            state |= overmap_buffer.has( far_om ) ? 0 : 4;
            map_whole = fp3d::run_overmap_query( 908, ox, oy, at.z(), true, 64 * 1024 );
            map_tiny = fp3d::run_overmap_query( 909, ox, oy, at.z(), true,
                                                map_whole.size() > 40 ? map_whole.size() - 40 : 0 );
            map_plain = fp3d::run_overmap_query( 910, ox, oy, at.z(), false, 64 * 1024 );
            map_fewer = fp3d::run_overmap_query( 911, ox, oy, at.z(), false,
                                                 map_plain.size() > 200 ? map_plain.size() - 200 : 0 );
            map_state = state;
            break;
        }
        case 77: {
            // The map, after: the two cells' vision, the note, the mark and the path as they were. No overmap has
            // appeared under the far page.
            avatar &u = get_avatar();
            int state = 0;
            const int z = map_at[2];
            overmap_buffer.set_seen( tripoint_abs_omt( map_vague_at[0], map_vague_at[1], z ),
                                     static_cast<om_vision_level>( map_was[0] ) );
            overmap_buffer.set_seen( tripoint_abs_omt( map_unseen_at[0], map_unseen_at[1], z ),
                                     static_cast<om_vision_level>( map_was[1] ) );
            const tripoint_abs_omt noted( map_note_at[0], map_note_at[1], z );
            overmap_buffer.delete_note( noted );
            if( map_marked && overmap_buffer.is_explored( noted ) ) {
                overmap_buffer.toggle_explored( noted );
            }
            map_marked = false;
            const tripoint_abs_omt unseen( map_unseen_at[0], map_unseen_at[1], z );
            if( map_unseen_marked && overmap_buffer.is_explored( unseen ) ) {
                overmap_buffer.toggle_explored( unseen );
            }
            map_unseen_marked = false;
            // The campsite and its forest: the ground and the vision as they were.
            for( size_t i = 0; i < map_blend_was.size() && map_blend_was.size() == 9; i++ ) {
                const tripoint_abs_omt c( map_blend_at[0] + static_cast<int>( i % 3 ) - 1,
                                          map_blend_at[1] + static_cast<int>( i / 3 ) - 1, z );
                overmap_buffer.ter_set( c, map_blend_was[i] );
                overmap_buffer.set_seen( c, om_vision_level::unseen );
            }
            map_blend_was.clear();
            u.omt_path.clear();
            const point_abs_om far_om = project_to<coords::om>( tripoint_abs_omt( map_far_at[0], map_far_at[1],
                                        z ) ).xy();
            state |= overmap_buffer.has( far_om ) ? 0 : 1;
            state |= overmap_buffer.has_note( noted ) ? 0 : 2;
            map_state = state;
            break;
        }
        case 66: {
            // Vehicle work, after: the avatar back, the car gone, the skill and the pack as before.
            avatar &u = get_avatar();
            if( work_from ) {
                u.setpos( here, here.get_bub( *work_from ) );
                work_from.reset();
                u.set_skill_level( skill_id( "mechanics" ), work_skill[0] );
                u.set_knowledge_level( skill_id( "mechanics" ), work_skill[1] );
            }
            u.remove_items_with( []( const item & it ) {
                return it.has_var( "fp3d_work_test" ) || it.typeId() == itype_id( "fp3d_glovebox" );
            } );
            u.invalidate_crafting_inventory();
            if( work_car != nullptr ) {
                here.destroy_vehicle( work_car );
                work_car = nullptr;
            }
            if( work_machine != nullptr ) {
                here.destroy_vehicle( work_machine );
                work_machine = nullptr;
            }
            // What the work left lying (the glovebox taken off).
            for( int dx = -12; dx <= 12; dx++ ) {
                for( int dy = -12; dy <= 12; dy++ ) {
                    const tripoint_bub_ms q = u.pos_bub() + tripoint_rel_ms( dx, dy, 0 );
                    if( here.inbounds( q ) ) {
                        here.i_clear( q );
                    }
                }
            }
            here.build_map_cache( p.z() );
            break;
        }
        case 39: {
            // Gun mods, part 2: that job is gone.
            get_avatar().backlog.clear();
            break;
        }
        case 35: {
            // Menus: three with the caller's choice on a disabled entry, on an enabled one, and with nothing
            // enabled. The driver reads each request's `selected` and cancels it.
            for( const int pick : { 0, 2 } ) {
                uilist m;
                m.title = "fp3d selected";
                m.addentry( 10, false, 'a', "first, disabled" );
                m.addentry( 11, true, 'b', "second" );
                m.addentry( 12, true, 'c', "third" );
                m.selected = pick;
                m.query();
            }
            uilist none;
            none.title = "fp3d selected";
            none.addentry( 20, false, 'a', "only one, disabled" );
            none.query();
            break;
        }
        case 34: {
            // Bounded polling: a screen that only polls its input context is told QUIT after max_idle_polls polls;
            // one that goes on polling gets a blocking request (the driver answers CONFIRM).
            input_context ctxt( "FP3D_POLL_TEST" );
            ctxt.register_action( "QUIT" );
            ctxt.register_action( "CONFIRM" );
            int polls = 0;
            std::string got;
            do {
                got = ctxt.handle_input( 50 );
                polls++;
            } while( got == "TIMEOUT" && polls < 100000 );
            poll_quit_after = got == "QUIT" ? polls : -polls;
            int more = 0;
            do {
                got = ctxt.handle_input( 50 );
                more++;
            } while( got != "CONFIRM" && more < 100000 );
            poll_asked_after = got == "CONFIRM" ? more : -more;
            break;
        }
        case 33: {
            // Names: an ally with a refused name is taken over (CDDA's control_npc), then a save under a refused
            // save id is tried. Last scene of the game: the avatar is another character afterwards.
            avatar &u = get_avatar();
            name_results res;
            const std::string id_before = u.get_save_id();
            const tripoint_bub_ms npc_at = p + tripoint_rel_ms( 2, 0, 0 );
            shared_ptr_fast<npc> guy = make_shared_fast<npc>();
            guy->normalize();
            guy->randomize();
            guy->spawn_at_precise( here.get_abs( npc_at ) );
            overmap_buffer.insert_npc( guy );
            g->load_npcs();
            guy->set_fac( faction_your_followers );
            g->add_npc_follower( guy->getID() );
            guy->name = std::string( "#Ann\x01/Lee.." );
            const character_id guy_id = guy->getID();
            u.control_npc( *guy );
            res.ran = true;
            res.swapped = u.getID() == guy_id;
            res.taken_ok = u.name == "AnnLee.";
            res.name_bytes = u.name.size();
            res.id_kept = u.get_save_id() == id_before;
            u.set_save_id( "#fp3d refused" );
            res.save_refused = !g->save();
            u.set_save_id( id_before );
            const std::lock_guard<std::mutex> lk( names_mutex );
            names_out = res;
            break;
        }
        case 24: {
            avatar &u = get_avatar();
            u.remove_effect( effect_winded );
            if( u.has_base_trait( trait_BADHEARING ) ) {
                u.toggle_trait( trait_BADHEARING );
            }
            if( u.has_trait( trait_NIGHTVISION ) && !u.has_base_trait( trait_NIGHTVISION ) ) {
                u.unset_mutation( trait_NIGHTVISION );
            }
            break;
        }
        case 19: {
            // Off-hand weight: a backpack worn, nothing wielded, an empty jerrycan carried and held in the off hand;
            // strength 8 until stage 20.
            avatar &u = get_avatar();
            u.clear_worn();
            u.inv->clear();
            u.remove_weapon();
            u.wear_item( item( itype_backpack ), false );
            offhand_str_saved = u.str_max;
            u.str_max = 8;
            u.reset_stats();
            item_location can = u.i_add( item( itype_jerrycan ) );
            if( can ) {
                fp3d::hold_carried( u, can );
            }
            break;
        }
        case 20: {
            // The held jerrycan filled with 10 L of clean water (11.6 kg: two-handed short of strength 26); the
            // random start's strength back.
            avatar &u = get_avatar();
            if( item_location can = fp3d::offhand_item( u ) ) {
                can->put_in( item( itype_water_clean, calendar::turn, 40 ), pocket_type::CONTAINER );
            }
            if( offhand_str_saved > 0 ) {
                u.str_max = offhand_str_saved;
                offhand_str_saved = -1;
                u.reset_stats();
            }
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
    // From where start_game's search starts.
    const point_abs_om origin = get_avatar().world_origin.value_or( point_abs_om() );
    for( const point_abs_om &omp : closest_points_first( origin, 3 ) ) {
        searched++;
        if( omp == start_om ) {
            reached = true;
            break;
        }
        // Made by the search already: get() makes nothing new here.
        const overmap &om = overmap_buffer.get( omp );
        for( const start_location &loc : start_locations::get_all() ) {
            if( scen.allowed_start( loc.id ) && fp3d::has_start_terrain( om, loc ) ) {
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
        fp3d::set_running_hook( running );
        while( !do_turn() ) {
        }
        // Each time the game saves and quits (the save goal, the cooking checks): back to it the way the main menu
        // loads one. Any other end is an error.
        for( ;; ) {
            if( g->uquit != QUIT_SAVED ) {
                throw std::runtime_error( loads == 0 ? "game ended without saving" : "game ended" );
            }
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
        }
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

// How much longer than 250 ms (times slow_build) a real-time turn's slot is in this run: 1, 2, 4 or 8. On a slow
// or busy machine, or a first run, a turn can take longer than its slot; a main-loop request is then already late
// when it is posted and the client never sees one. The real-time checks stretch the slot until requests are seen,
// and keep it for the rest of the run (driver thread only).
int rt_stretch = 1;

// Three real-time turns with the slot stretched by `stretch`: each unanswered main-loop request must expire after
// about a turn and advance the game one turn.
bool realtime_steps( fp3d::ui_request &req, int stretch )
{
    bool ok = true;
    const double turn_ms = 250.0 * fp3d::slow_build * stretch;
    fp3d::set_timescale( 4.0 / ( fp3d::slow_build * stretch ) );
    fp3d::set_clock_mode( fp3d::clock_mode::real_time );
    std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    for( int i = 0; i < 3; i++ ) {
        const std::optional<fp3d::ui_request> next = fp3d::wait_request( 10000, req.id );
        const double ms = ms_since( t );
        t = std::chrono::steady_clock::now();
        if( !next || !next->timed ) {
            std::printf( "    real time: no main-loop request after %.0f ms (slot %.0f ms)\n", ms, turn_ms );
            return false;
        }
        // A turn is its slot long at this timescale: never faster (paced), but a loaded machine (parallel builds) may
        // be slower, so the upper bound is loose and a slow step may have let a turn or two more pass. A slow build
        // (TSan) has turns longer than their slot, so it checks no lower bound.
        const int64_t turns = next->turn - req.turn;
        const bool step_ok = turns >= 1 && ( fp3d::slow_build > 1 || ms >= 150.0 * stretch ) &&
                             turns <= 2 + static_cast<int64_t>( ms / turn_ms ) &&
                             ms <= 10000.0 * fp3d::slow_build * stretch;
        std::printf( "    real time: turn %lld -> %lld after %.0f ms %s\n", static_cast<long long>( req.turn ),
                     static_cast<long long>( next->turn ), ms, step_ok ? "ok" : "BAD" );
        ok = ok && step_ok;
        req = *next;
    }
    return ok;
}

// Real time at 4x (250 ms turns, longer when this machine needs it: rt_stretch): three unanswered main-loop
// requests must each expire after about a turn and advance the game one turn. Then a zombie comes into view: the
// clock must auto-pause and stay paused until the client resumes it.
bool realtime_checks( fp3d::ui_request &req )
{
    bool ok = false;
    for( int stretch = 1; stretch <= 8 && !ok; stretch *= 2 ) {
        rt_stretch = stretch;
        ok = realtime_steps( req, stretch );
        if( !ok && stretch < 8 ) {
            // Turn-based for a moment: the pending request stays, so the next try starts from one that is seen.
            fp3d::set_clock_mode( fp3d::clock_mode::turn_based );
            const std::optional<fp3d::ui_request> pending = fp3d::wait_request( 10000 * fp3d::slow_build );
            if( !pending ) {
                return false;
            }
            req = *pending;
            std::printf( "    real time: trying again with a slot of %d ms\n", 250 * fp3d::slow_build * stretch * 2 );
        }
    }

    spawn_zombie = true; // placed by the sim right before its next request
    std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    // Up to 10 s: on a loaded machine the game thread may be busy for seconds (an overmap loading, say).
    while( !fp3d::get_clock().paused && ms_since( t ) < 10000 * fp3d::slow_build * rt_stretch ) {
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
    while( ms_since( t ) < 750 * fp3d::slow_build ) {
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
        case fp3d::request_kind::number:
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
    const bool off_ran = act( "fp3d_attack_off_LEFT", evs );
    std::string off_seen = std::string( off_ran ? "" : "main loop not back, " ) + std::to_string( player_attacks( evs,
                           true ) ) + " unarmed, " + std::to_string( player_attacks( evs, false ) ) + " armed";
    for( const fp3d::event &e : evs ) {
        if( e.kind == fp3d::event_kind::message ) {
            off_seen += " | " + e.text.substr( 0, 60 );
        }
    }
    check( off_ran && player_attacks( evs, true ) == 1 && player_attacks( evs, false ) == 0,
           "off hand strikes unarmed", off_seen );
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
    check( menu && !menu->timed && entry >= 0, "hold asks which item",
           entry >= 0 ? label : menu ? std::string( fp3d::kind_name( menu->kind ) ) + " in " + menu->context +
           ( menu->timed ? ", timed" : "" ) + ": " + menu->text.substr( 0, 60 ) + " | polls " + fp3d::poll_report() :
           "no request" );
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
    // The held view names each hand's item by its handle: the uid the snapshot lists for the wielded item and for
    // the one held in the off hand.
    uint64_t wielded_uid = 0;
    uint64_t offhand_uid = 0;
    if( const std::shared_ptr<const fp3d::snapshot> held_snap = fp3d::latest_snapshot() ) {
        for( const fp3d::carried_view &c : held_snap->player.items ) {
            if( c.flags & fp3d::carried_wielded ) {
                wielded_uid = c.uid;
            }
            if( c.flags & fp3d::carried_offhand ) {
                offhand_uid = c.uid;
            }
        }
    }
    check( h.main.uid != 0 && h.main.uid == wielded_uid && h.off.uid != 0 && h.off.uid == offhand_uid,
           "held view: each hand's item by its handle",
           std::to_string( h.main.uid == wielded_uid ) + std::to_string( h.off.uid == offhand_uid ) );
    if( !r ) {
        return false;
    }
    // Use the hose on the vehicle north: an activity runs (no request) until the tank is empty.
    const auto t0 = std::chrono::steady_clock::now();
    fp3d::queue_action( "fp3d_use_UP" );
    std::vector<int64_t> levels;
    std::optional<fp3d::ui_request> done;
    while( !done && std::chrono::steady_clock::now() - t0 < std::chrono::seconds( 40 * fp3d::slow_build ) ) {
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
    // 4 L of gasoline make the jerrycan too heavy for one hand at strength 8: not offered to hold.
    hand_setup = 21;
    act( "pause", ev );
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
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds( 60 * fp3d::slow_build ) ) {
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

// The furniture id on absolute square `p` in the newest snapshot ("" when not in it).
static std::string furn_at( const fp3d::ui_point &p )
{
    const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
    if( !s ) {
        return "";
    }
    const auto floor_div = []( int a ) {
        return a >= 0 ? a / fp3d::sm_size : ( a - fp3d::sm_size + 1 ) / fp3d::sm_size;
    };
    const int sx = floor_div( p.x );
    const int sy = floor_div( p.y );
    const int tile = ( p.y - sy * fp3d::sm_size ) * fp3d::sm_size + ( p.x - sx * fp3d::sm_size );
    for( const std::shared_ptr<const fp3d::submap_view> &sm : s->submaps ) {
        if( sm->pos.x == sx && sm->pos.y == sy && sm->pos.z == p.z ) {
            return fp3d::cdda_id_name( "furn", sm->furn[tile] );
        }
    }
    return "";
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
        while( ms_since( t0 ) < 20000 * fp3d::slow_build ) {
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

    // Starting the craft (the `cook` item action). Malformed actions are refused when queued. The sim refuses a
    // recipe the contents don't make, an unknown one, too big a batch and a pot two squares away, with nothing used
    // up, and a full square at the avatar's feet too. water_clean x2 on the fire takes 2 of the 4 water out of the
    // pot, lays the craft at the avatar's feet (the pot's cookware entry shows it) and starts CDDA's craft; stopped
    // with CDDA's question, the craft stays.
    uint32_t cook_tag = 500;
    const auto cook_action = [&]( uint64_t uid, const fp3d::ui_point & pos, const std::string & recipe, int batch ) {
        fp3d::item_action ia;
        ia.op = fp3d::item_op::cook;
        ia.item = fp3d::item_ref{ uid, t_pot };
        ia.pos = pos;
        ia.recipe = recipe;
        ia.batch = batch;
        ia.tag = ++cook_tag;
        return ia;
    };
    {
        fp3d::item_action not_cook = cook_action( pot.uid, fire_at, "water_clean", 1 );
        not_cook.op = fp3d::item_op::take;
        fp3d::item_action batch_on_take = cook_action( pot.uid, fire_at, "", 2 );
        batch_on_take.op = fp3d::item_op::take;
        const bool refused_all =
            !fp3d::queue_item_action( cook_action( pot.uid, fire_at, "water clean", 1 ) ) &&
            !fp3d::queue_item_action( cook_action( pot.uid, fire_at, "water\"clean", 1 ) ) &&
            !fp3d::queue_item_action( cook_action( pot.uid, fire_at, "water\x01", 1 ) ) &&
            !fp3d::queue_item_action( cook_action( pot.uid, fire_at, "", 1 ) ) &&
            !fp3d::queue_item_action( cook_action( pot.uid, fire_at, std::string( fp3d::max_cook_recipe_bytes + 1,
                                      'a' ), 1 ) ) &&
            !fp3d::queue_item_action( cook_action( pot.uid, fire_at, "water_clean", 0 ) ) &&
            !fp3d::queue_item_action( cook_action( pot.uid, fire_at, "water_clean", fp3d::max_cook_batch + 1 ) ) &&
            !fp3d::queue_item_action( not_cook ) && !fp3d::queue_item_action( batch_on_take );
        check( refused_all, "cook: malformed actions refused when queued", "" );
    }
    struct cook_result {
        bool queued = false;
        bool back = false;        // in the main loop again
        int answers = 0;          // item_result events for the tag
        bool done = false;
        std::string why;
        int prompts = 0;
        bool stopped = false;     // interrupt() taken
        std::string texts;        // the prompts' and messages' texts
    };
    // Queues `ia` and waits for the main loop to ask again; with `stop`, interrupts the craft once it started and
    // answers CDDA's question yes. Other prompts are cancelled.
    const auto run_cook = [&]( const fp3d::item_action & ia, bool stop ) {
        cook_result res;
        std::vector<fp3d::event> evs;
        r = fp3d::wait_request( 5000 );
        if( !r || !r->timed ) {
            return res;
        }
        fp3d::drain_events( evs );
        evs.clear();
        res.queued = fp3d::queue_item_action( ia );
        if( !res.queued ) {
            return res;
        }
        const uint64_t before = r->id;
        const auto read_events = [&]() {
            fp3d::drain_events( evs );
            for( const fp3d::event &e : evs ) {
                if( e.kind == fp3d::event_kind::item_result && e.detail == ia.tag ) {
                    res.answers++;
                    res.done = e.amount == 1;
                    res.why = e.text;
                } else if( e.kind == fp3d::event_kind::message ) {
                    res.texts += " | msg: " + e.text.substr( 0, 80 );
                }
            }
            evs.clear();
        };
        const auto t = std::chrono::steady_clock::now();
        while( ms_since( t ) < 120000 * fp3d::slow_build ) {
            read_events();
            if( stop && res.done && !res.stopped ) {
                res.stopped = fp3d::interrupt();
            }
            std::optional<fp3d::ui_request> next = fp3d::wait_request( 100, r->id );
            if( !next ) {
                continue;
            }
            r = next;
            if( r->timed ) {
                if( r->id != before ) {
                    res.back = true;
                    break;
                }
                continue;
            }
            res.prompts++;
            res.texts += " | asked: " + r->text.substr( 0, 80 );
            fp3d::ui_answer a;
            a.id = r->id;
            if( stop && std::find( r->actions.begin(), r->actions.end(), "YES" ) != r->actions.end() ) {
                a.action = "YES";
            } else {
                a.cancel = true;
            }
            fp3d::answer( a );
        }
        read_events();
        return res;
    };
    // The pot's cookware entry on the fire in the newest snapshot.
    const auto pot_now = [&]() {
        std::optional<fp3d::cookware_view> out;
        const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
        if( !s ) {
            return out;
        }
        const auto floor_div = []( int a ) {
            return a >= 0 ? a / fp3d::sm_size : ( a - fp3d::sm_size + 1 ) / fp3d::sm_size;
        };
        const int sx = floor_div( fire_at.x );
        const int sy = floor_div( fire_at.y );
        const int tile = ( fire_at.y - sy * fp3d::sm_size ) * fp3d::sm_size + ( fire_at.x - sx * fp3d::sm_size );
        for( const std::shared_ptr<const fp3d::submap_view> &sm : s->submaps ) {
            if( sm->pos.x != sx || sm->pos.y != sy || sm->pos.z != fire_at.z ) {
                continue;
            }
            for( const fp3d::cookware_view &cw : sm->cookware ) {
                if( cw.tile == tile && cw.uid == pot.uid ) {
                    out = cw;
                }
            }
        }
        return out;
    };
    // Water left in the pot, clean or not (CDDA boils the water in a pot on a fire clean by itself).
    const uint32_t t_clean = type_of( "water_clean" );
    const auto water_left = [&]( const std::optional<fp3d::cookware_view> &cw ) {
        int n = 0;
        if( cw ) {
            for( const fp3d::cookware_item_view &c : cw->contents ) {
                n += c.type == t_water || c.type == t_clean ? c.count : 0;
            }
        }
        return n;
    };
    const auto cook_text = []( const cook_result & c ) {
        return std::string( c.queued ? "" : "not queued " ) + ( c.done ? "done" : "not done" ) + " [" + c.why + "] " +
               std::to_string( c.answers ) + " answers, " + std::to_string( c.prompts ) + " prompts" +
               ( c.back ? "" : ", main loop not back" ) + c.texts;
    };
    const cook_result wrong = run_cook( cook_action( pot.uid, fire_at, "meat_cooked", 1 ), false );
    const cook_result unknown = run_cook( cook_action( pot.uid, fire_at, "no_such_recipe", 1 ), false );
    const cook_result too_many = run_cook( cook_action( pot.uid, fire_at, "water_clean", 5 ), false );
    const cook_result too_far = run_cook( cook_action( meat_pot, meat_at, "meat_cooked", 1 ), false );
    const int water_before = water_left( pot_now() );
    check( wrong.back && wrong.answers == 1 && !wrong.done && unknown.back && unknown.answers == 1 && !unknown.done &&
           too_many.back && too_many.answers == 1 && !too_many.done && too_far.back && too_far.answers == 1 &&
           !too_far.done && water_before == 4, "cook: refusals, nothing used up",
           cook_text( wrong ) + " | " + cook_text( unknown ) + " | " + cook_text( too_many ) + " | " +
           cook_text( too_far ) + " | water " + std::to_string( water_before ) );
    // No room for the craft at the avatar's feet: refused before anything is used up.
    hand_setup = 26;
    fp3d::queue_action( "pause" );
    r = fp3d::wait_request( 20000, r ? r->id : 0 );
    const cook_result full = run_cook( cook_action( pot.uid, fire_at, "water_clean", 2 ), false );
    const std::optional<fp3d::cookware_view> full_cw = pot_now();
    hand_setup = 27;
    fp3d::queue_action( "pause" );
    r = fp3d::wait_request( 20000, r ? r->id : 0 );
    const int full_free = cook_full_free_ml.load();
    check( full.back && full.answers == 1 && !full.done && full.why.find( "room" ) != std::string::npos && full_cw &&
           water_left( full_cw ) == 4 && full_cw->craft_recipe.empty() && full_free >= 0 && full_free < 500,
           "cook: no room at the feet, nothing used up",
           cook_text( full ) + " | free " + std::to_string( full_free ) + " ml, water " +
           std::to_string( water_left( full_cw ) ) );
    const cook_result boiling = run_cook( cook_action( pot.uid, fire_at, "water_clean", 2 ), true );
    const std::optional<fp3d::cookware_view> after = pot_now();
    check( boiling.back && boiling.done && boiling.answers == 1 && boiling.stopped,
           "cook: water_clean x2 started, then stopped", cook_text( boiling ) );
    check( after && water_left( after ) == 2 && after->craft_recipe == "water_clean" &&
           after->craft_result == type_of( "water_clean" ) && after->craft_progress <= 1000,
           "cook: 2 water used, the craft shown with the pot",
           after ? "water " + std::to_string( water_left( after ) ) + ", craft '" + after->craft_recipe + "' " +
           std::to_string( after->craft_progress ) + " permille" : "pot not seen" );

    // A save and a load keep the pot's handle and the paused craft's link to it (both are item vars): the game is
    // saved and quit here and loaded again as the main menu does; everything below runs on the loaded game.
    {
        const int loads_before = snapshot().loads;
        r = fp3d::wait_request( 5000 );
        fp3d::queue_action( "save" );
        std::optional<fp3d::ui_request> sv = fp3d::wait_request( 20000, r ? r->id : 0 );
        for( int i = 0; i < 5 && sv && !sv->timed; i++ ) {
            fp3d::ui_answer a;
            a.id = sv->id;
            if( std::find( sv->actions.begin(), sv->actions.end(), "YES" ) != sv->actions.end() ) {
                a.action = "YES";
            } else {
                a.cancel = true;
            }
            fp3d::answer( a );
            sv = fp3d::wait_request( 300000, sv->id );   // the load takes a while
        }
        r = sv;
        const std::optional<fp3d::cookware_view> kept = pot_now();
        check( r && r->timed && snapshot().loads == loads_before + 1 && kept && after &&
               kept->craft_recipe == "water_clean" && kept->craft_progress == after->craft_progress &&
               water_left( kept ) == water_left( after ),
               "save and load: the pot keeps its handle, the craft its link",
               std::string( r && r->timed ? "" : "main loop not back, " ) + "loads " +
               std::to_string( snapshot().loads - loads_before ) +
               ( kept ? ", craft '" + kept->craft_recipe + "' " + std::to_string( kept->craft_progress ) +
                 " permille, water " + std::to_string( water_left( kept ) ) : ", pot not seen" ) );
        if( !r || !r->timed ) {
            return false;
        }
    }

    // The rest of L9 on that paused craft (2 of the 4 water still in the pot on the fire): pouring out of the pot
    // where it stands, the pot off the heat and back, the fire put out, fed and lit again, and the craft to its end.
    const auto scene = [&]( int n ) {
        hand_setup = n;
        fp3d::queue_action( "pause" );
        r = fp3d::wait_request( 20000, r ? r->id : 0 );
    };
    // The fireplace's heat entry in the newest snapshot.
    const auto fire_now = [&]() {
        std::optional<fp3d::heat_view> out;
        const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
        if( !s ) {
            return out;
        }
        const auto floor_div = []( int a ) {
            return a >= 0 ? a / fp3d::sm_size : ( a - fp3d::sm_size + 1 ) / fp3d::sm_size;
        };
        const int sx = floor_div( fire_at.x );
        const int sy = floor_div( fire_at.y );
        const int tile = ( fire_at.y - sy * fp3d::sm_size ) * fp3d::sm_size + ( fire_at.x - sx * fp3d::sm_size );
        for( const std::shared_ptr<const fp3d::submap_view> &sm : s->submaps ) {
            if( sm->pos.x != sx || sm->pos.y != sy || sm->pos.z != fire_at.z ) {
                continue;
            }
            for( const fp3d::heat_view &h : sm->heat ) {
                if( h.tile == tile ) {
                    out = h;
                }
            }
        }
        return out;
    };
    const auto fire_action = [&]( fp3d::item_op op, uint64_t uid, uint32_t type, const fp3d::ui_point & pos ) {
        fp3d::item_action ia;
        ia.op = op;
        ia.item = fp3d::item_ref{ uid, type };
        ia.pos = pos;
        ia.tag = ++cook_tag;
        return ia;
    };
    const auto has = []( const std::string & text, const char *part ) {
        return text.find( part ) != std::string::npos;
    };
    check( !fp3d::queue_item_action( fire_action( fp3d::item_op::extinguish, pot.uid, t_pot, fire_at ) ) &&
           !fp3d::queue_item_action( fire_action( fp3d::item_op::light, 5, 0, fire_at ) ) &&
           !fp3d::queue_item_action( fire_action( fp3d::item_op::light, 0, t_pot, fire_at ) ) &&
           !fp3d::queue_item_action( fire_action( fp3d::item_op::take, 0, 0, fire_at ) ),
           "light, extinguish: malformed actions refused when queued", "" );

    // One craft a pot: another recipe is refused while this one is paused. The rest of the water is poured out of the
    // pot where it stands, onto the square east of the avatar.
    const cook_result second = run_cook( cook_action( pot.uid, fire_at, "meat_cooked", 1 ), false );
    const cook_result poured = run_cook( fire_action( fp3d::item_op::pour, pot.uid, t_pot, at( 1, 0 ) ), false );
    const std::optional<fp3d::cookware_view> emptied = pot_now();
    check( second.back && second.answers == 1 && !second.done && has( second.why, "already" ) && poured.back &&
           poured.done && emptied && water_left( emptied ) == 0 && emptied->craft_recipe == "water_clean",
           "pour: out of the pot on the fire; one craft a pot",
           cook_text( second ) + " | " + cook_text( poured ) + " | water " + std::to_string( water_left( emptied ) ) );

    // The pot off the heat: CDDA's own way on with the craft is held up, and so is `cook`; nothing moves on.
    cook_own_resume = 0;
    scene( 28 );
    const cook_result off_heat = run_cook( cook_action( pot.uid, at( -1, 0 ), "water_clean", 2 ), false );
    scene( 29 );
    const std::optional<fp3d::cookware_view> back_on = pot_now();
    check( cook_own_resume.load() == 1 && off_heat.back && off_heat.answers == 1 && !off_heat.done &&
           has( off_heat.why, "heat" ) && back_on && after && back_on->craft_recipe == "water_clean" &&
           back_on->craft_progress == after->craft_progress, "cook: off the heat the craft stays paused",
           "CDDA's own way on " + std::to_string( cook_own_resume.load() ) + " | " + cook_text( off_heat ) +
           ( back_on ? " | " + std::to_string( back_on->craft_progress ) + " permille" : " | pot not seen" ) );

    // The fire beaten out: nothing cooks on it.
    const cook_result put_out = run_cook( fire_action( fp3d::item_op::extinguish, 0, 0, fire_at ), false );
    const std::optional<fp3d::heat_view> cold = fire_now();
    const cook_result on_cold = run_cook( cook_action( pot.uid, fire_at, "water_clean", 2 ), false );
    const cook_result no_fire = run_cook( fire_action( fp3d::item_op::extinguish, 0, 0, fire_at ), false );
    check( put_out.back && put_out.done && cold && !cold->lit && cold->fire == 0 && on_cold.back && !on_cold.done &&
           has( on_cold.why, "heat" ) && no_fire.back && !no_fire.done,
           "extinguish: the fire out, nothing cooks on it",
           cook_text( put_out ) + " | " + heat_text( cold ) + " | " + cook_text( on_cold ) + " | " +
           cook_text( no_fire ) );

    // Fuel dropped on the fireplace (the `drop` action), then lit with the carried lighter (no item named).
    scene( 30 );
    const cook_result fed = run_cook( fire_action( fp3d::item_op::drop, cook_fuel_uid.load(),
                                      type_of( "splinter" ), fire_at ), false );
    const cook_result lit = run_cook( fire_action( fp3d::item_op::light, 0, 0, fire_at ), false );
    const cook_result on_self = run_cook( fire_action( fp3d::item_op::light, 0, 0, at( 0, 0 ) ), false );
    const std::optional<fp3d::heat_view> warm = fire_now();
    check( fed.back && fed.done && lit.back && lit.done && warm && warm->lit && warm->fire > 0 && on_self.back &&
           !on_self.done, "light: fuel on the fireplace, lit with the lighter",
           cook_text( fed ) + " | " + cook_text( lit ) + " | " + heat_text( warm ) + " | " + cook_text( on_self ) );

    // Back on a fire the craft goes on to its end; what it made is in the pot.
    scene( 31 );
    const cook_result finished = run_cook( cook_action( pot.uid, fire_at, "water_clean", 2 ), false );
    const std::optional<fp3d::cookware_view> served = pot_now();
    int clean = 0;
    if( served ) {
        for( const fp3d::cookware_item_view &c : served->contents ) {
            clean += c.type == t_clean ? c.count : 0;
        }
    }
    check( finished.back && finished.done && served && served->craft_recipe.empty() && clean == 2 &&
           served->contents.size() == 1, "cook: back on the fire to the end, the result in the pot",
           cook_text( finished ) + " | clean water " + std::to_string( clean ) +
           ( served ? ", craft '" + served->craft_recipe + "'" : ", pot not seen" ) );
    scene( 32 );
    return failed == 0;
}

// The action allow-list and bounded polling (fp3d/bridge.h, scene 34). Late in the game: the scenes before it count
// on the turns and the random numbers as they were.
static bool run_action_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  actions: %-44s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    const std::optional<fp3d::ui_request> r1 = fp3d::wait_request( 5000 );
    if( !r1 || !r1->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    std::vector<fp3d::event> evs;
    fp3d::drain_events( evs );

    // The action allow-list: CDDA actions a client isn't meant to send are dropped like unknown ones (no screen
    // opens), the debug menu too unless cheats are allowed.
    const bool cheats_were = fp3d::cheats_allowed();
    fp3d::set_cheats_allowed( false );
    const std::vector<std::string> not_allowed = { "missions", "diary", "action_menu", "main_menu", "open_options",
                                                   "help", "quickload", "debug"
                                                 };
    for( const std::string &name : not_allowed ) {
        fp3d::queue_action( name );
    }
    // No game turn passes here or in the checks below (the later scenes count on the turns as they were).
    std::optional<fp3d::ui_request> ra = fp3d::wait_request( 20000, r1->id );
    evs.clear();
    fp3d::drain_events( evs );
    size_t refused = 0;
    for( const std::string &name : not_allowed ) {
        refused += std::any_of( evs.begin(), evs.end(), [&name]( const fp3d::event & e ) {
            return e.kind == fp3d::event_kind::message && e.text == "Can't do that here: " + name;
        } ) ? 1 : 0;
    }
    const bool listed = ra && std::find( ra->actions.begin(), ra->actions.end(), "pause" ) != ra->actions.end() &&
                        std::find( ra->actions.begin(), ra->actions.end(), "missions" ) == ra->actions.end() &&
                        std::find( ra->actions.begin(), ra->actions.end(), "debug" ) == ra->actions.end();
    check( ra && ra->timed && refused == not_allowed.size() && listed && fp3d::queued_actions() == 0,
           "actions off the allow-list dropped",
           std::to_string( refused ) + " of " + std::to_string( not_allowed.size() ) + " refused" +
           ( ra ? std::string( ra->timed ? "" : ", a screen opened" ) + ( listed ? "" : ", listed" ) : ", no request" ) );
    // With cheats allowed the debug menu opens (a menu request); it is closed again.
    fp3d::set_cheats_allowed( true );
    bool debug_menu = false;
    if( ra && ra->timed ) {
        fp3d::queue_action( "debug" );
        ra = fp3d::wait_request( 20000, ra->id );
        debug_menu = ra && !ra->timed && ra->kind == fp3d::request_kind::menu;
        for( int i = 0; i < 10 && ra && !ra->timed; i++ ) {
            fp3d::ui_answer cancel;
            cancel.id = ra->id;
            cancel.cancel = true;
            fp3d::answer( cancel );
            ra = fp3d::wait_request( 10000, ra->id );
        }
    }
    fp3d::set_cheats_allowed( cheats_were );
    check( debug_menu && ra && ra->timed, "debug menu only while cheats are allowed", "" );
    if( !ra || !ra->timed ) {
        return false;
    }

    // Bounded polling (scene 34 on the sim thread): the first loop is told QUIT at its 256th poll; the second
    // ignores QUIT and is asked at the 1024th poll in all (its 768th), a request this answers with CONFIRM.
    poll_quit_after = 0;
    poll_asked_after = 0;
    hand_setup = 34;
    fp3d::queue_action( "no_such_action" );   // dropped: the scene runs before the request asked again
    ra = fp3d::wait_request( 20000, ra->id );
    const bool asked = ra && !ra->timed && ra->kind == fp3d::request_kind::key && ra->context == "FP3D_POLL_TEST";
    for( int i = 0; i < 10 && ra && !ra->timed; i++ ) {
        fp3d::ui_answer a;
        a.id = ra->id;
        if( ra->context == "FP3D_POLL_TEST" ) {
            a.action = "CONFIRM";
        } else {
            a.cancel = true;
        }
        fp3d::answer( a );
        ra = fp3d::wait_request( 10000, ra->id );
    }
    check( asked && poll_quit_after.load() == fp3d::max_idle_polls &&
           poll_asked_after.load() == 3 * fp3d::max_idle_polls && ra && ra->timed,
           "a polling screen is told QUIT, then asked",
           "QUIT after " + std::to_string( poll_quit_after.load() ) + " polls, asked after " +
           std::to_string( poll_asked_after.load() ) + " more" );
    if( !ra || !ra->timed ) {
        return false;
    }

    // Menu requests carry CDDA's highlighted entry (scene 35): the caller's choice, the next enabled entry when that
    // one is disabled, -1 when nothing is enabled.
    hand_setup = 35;
    fp3d::queue_action( "no_such_action" );   // dropped: the scene runs before the request asked again
    std::vector<int> highlighted;
    ra = fp3d::wait_request( 20000, ra->id );
    for( int i = 0; i < 10 && ra && !ra->timed; i++ ) {
        if( ra->kind == fp3d::request_kind::menu && ra->title == "fp3d selected" ) {
            highlighted.push_back( ra->selected );
        }
        fp3d::ui_answer cancel;
        cancel.id = ra->id;
        cancel.cancel = true;
        fp3d::answer( cancel );
        ra = fp3d::wait_request( 10000, ra->id );
    }
    std::string highlighted_text;
    for( const int h : highlighted ) {
        highlighted_text += std::to_string( h ) + " ";
    }
    check( highlighted == std::vector<int> { 1, 2, -1 } && ra && ra->timed, "menus carry the highlighted entry",
           highlighted_text );

    return failed == 0;
}

// Input validation for names in play (fp3d/chargen.h): scene 33 on the sim thread. Run last on this game.
// Gun mods by hand (scene 36): mod_add and mod_remove on the wielded gun, with CDDA's own questions and time.
static bool run_gunmod_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  gun mods: %-46s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    hand_setup = 36;
    fp3d::queue_action( "no_such_action" );   // dropped: the scene runs before the request asked again
    r = fp3d::wait_request( 20000, r->id );
    if( !r || !r->timed ) {
        check( false, "scene", "" );
        return false;
    }
    struct result {
        bool queued = false;
        bool back = false;
        int popups = 0;
        int menus = 0;
        std::string asked;        // the questions' texts
        int answers = 0;
        bool done = false;
        std::string why;
        int64_t turns = 0;
        std::string entry;        // the first menu's first entry
    };
    uint32_t next_tag = 700;
    fp3d::item_ref with_vise{ 0, 0 };   // mod_add: the vise the next run names
    // One mod action on the gun; popups answered yes or no, a menu with `menu_entry` (-1 cancels it).
    const auto run = [&]( fp3d::item_op op, const fp3d::item_ref & mod, const fp3d::item_ref & gun, bool yes,
    int menu_entry ) {
        result res;
        std::vector<fp3d::event> evs;
        std::optional<fp3d::ui_request> at = fp3d::wait_request( 5000 );
        if( !at || !at->timed ) {
            return res;
        }
        const int64_t turn0 = at->turn;
        fp3d::drain_events( evs );
        evs.clear();
        fp3d::item_action ia;
        ia.op = op;
        ia.item = mod;
        ia.container = gun;
        ia.tag = ++next_tag;
        ia.vise = with_vise;
        res.queued = fp3d::queue_item_action( ia );
        if( !res.queued ) {
            return res;
        }
        const uint64_t before = at->id;
        const auto t = std::chrono::steady_clock::now();
        while( ms_since( t ) < 120000 * fp3d::slow_build ) {
            std::optional<fp3d::ui_request> next = fp3d::wait_request( 200, at->id );
            if( !next ) {
                continue;
            }
            at = next;
            if( at->timed ) {
                if( at->id != before ) {
                    res.back = true;
                    res.turns = at->turn - turn0;
                    break;
                }
                continue;
            }
            res.asked += ( res.asked.empty() ? "" : " | " ) + ( at->text.empty() ? at->title : at->text );
            fp3d::ui_answer a;
            a.id = at->id;
            if( at->kind == fp3d::request_kind::menu ) {
                if( res.menus == 0 && !at->entries.empty() ) {
                    res.entry = at->entries[0].label;
                }
                res.menus++;
                if( menu_entry < 0 ) {
                    a.cancel = true;
                } else {
                    a.entry = menu_entry;
                }
            } else {
                res.popups++;
                const bool offers_yes = std::find( at->actions.begin(), at->actions.end(), "YES" ) != at->actions.end();
                const bool offers_no = std::find( at->actions.begin(), at->actions.end(), "NO" ) != at->actions.end();
                if( yes && offers_yes ) {
                    a.action = "YES";
                } else if( offers_no ) {
                    a.action = "NO";
                } else {
                    a.cancel = true;
                }
            }
            fp3d::answer( a );
        }
        fp3d::drain_events( evs );
        for( const fp3d::event &e : evs ) {
            if( e.kind == fp3d::event_kind::item_result && e.detail == ia.tag ) {
                res.answers++;
                res.done = e.amount == 1;
                res.why = e.text;
            }
        }
        return res;
    };
    const auto show = []( const result & res ) {
        return std::string( res.queued ? "" : "not queued " ) + ( res.done ? "done" : "not done" ) + " [" + res.why +
               "] asked [" + res.asked + "] " + std::to_string( res.turns ) + " turns, " +
               std::to_string( res.answers ) + " answers";
    };
    const auto has = []( const std::string & str, const char *part ) {
        return str.find( part ) != std::string::npos;
    };
    // The newest snapshot's view of the gun and the pack.
    const auto gun_now = []() {
        const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
        return s && s->player.wielded_gun ? *s->player.wielded_gun : fp3d::gun_view();
    };
    const auto carried_of = []( const std::string & type ) {
        std::vector<uint64_t> uids;
        if( const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot() ) {
            for( const fp3d::carried_view &c : s->player.items ) {
                if( fp3d::id_name( fp3d::id_kind::itype, c.type ) == type ) {
                    uids.push_back( c.uid );
                }
            }
        }
        return uids;
    };
    const auto type_of = []( const std::string & type ) {
        uint32_t index = 0;
        if( const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot() ) {
            for( const fp3d::carried_view &c : s->player.items ) {
                if( fp3d::id_name( fp3d::id_kind::itype, c.type ) == type ) {
                    index = c.type;
                }
            }
            if( s->player.wielded_gun ) {
                for( const fp3d::gun_mod_view &m : s->player.wielded_gun->mods ) {
                    if( fp3d::id_name( fp3d::id_kind::itype, m.type ) == type ) {
                        index = m.type;
                    }
                }
            }
        }
        return index;
    };
    const auto fit_of = [&]( uint64_t uid ) {
        fp3d::gun_fit_view found;
        for( const fp3d::gun_fit_view &f : gun_now().fits ) {
            if( f.uid == uid ) {
                found = f;
            }
        }
        return found;
    };
    const std::vector<uint64_t> guns = carried_of( "glock_19" );
    const std::vector<uint64_t> lasers = carried_of( "rail_laser_sight" );
    const std::vector<uint64_t> dots = carried_of( "red_dot_sight" );
    const std::vector<uint64_t> barrels = carried_of( "barrel_ported" );
    const std::vector<uint64_t> mounts = carried_of( "sights_mount_pistol" );
    if( guns.size() != 1 || lasers.size() != 2 || dots.size() != 1 || barrels.size() != 1 || mounts.size() != 1 ) {
        check( false, "the scene's gun and mods are carried", std::to_string( guns.size() ) + " guns, " +
               std::to_string( lasers.size() ) + " lasers" );
        return false;
    }
    const fp3d::item_ref gun{ guns[0], type_of( "glock_19" ) };
    const fp3d::item_ref laser_a{ lasers[0], type_of( "rail_laser_sight" ) };
    const fp3d::item_ref laser_b{ lasers[1], type_of( "rail_laser_sight" ) };
    const fp3d::item_ref dot{ dots[0], type_of( "red_dot_sight" ) };
    const fp3d::item_ref barrel{ barrels[0], type_of( "barrel_ported" ) };
    const fp3d::item_ref mount{ mounts[0], type_of( "sights_mount_pistol" ) };
    const fp3d::gun_fit_view mount_fit = fit_of( mount.uid );
    check( gun_now().mods.empty() && gun_now().fits.size() == 5 && fit_of( laser_a.uid ).ok &&
           fit_of( laser_b.uid ).ok && fit_of( laser_b.uid ).chance == 100 && !fit_of( dot.uid ).ok &&
           !fit_of( dot.uid ).reason.empty() && fit_of( barrel.uid ).ok && mount_fit.ok && mount_fit.chance > 0 &&
           mount_fit.chance < 100, "the snapshot says which carried mods go on",
           "red dot: " + fit_of( dot.uid ).reason + "; mount " + std::to_string( mount_fit.chance ) + "% risk " +
           std::to_string( mount_fit.risk ) + "%" );
    // Malformed: never queued.
    fp3d::item_action bad;
    bad.op = fp3d::item_op::mod_add;
    bad.item = laser_a;
    const bool no_gun_given = !fp3d::queue_item_action( bad );
    bad.op = fp3d::item_op::mod_remove;
    bad.container = fp3d::item_ref{ 0, gun.type };
    check( no_gun_given && !fp3d::queue_item_action( bad ), "an action without the gun is never queued", "" );
    // Refused: another gun than the one in hand; a mod the gun has no slot for; a mod that isn't carried.
    result res = run( fp3d::item_op::mod_add, laser_a, fp3d::item_ref{ gun.uid + 1, gun.type }, true, -1 );
    check( res.queued && res.back && res.answers == 1 && !res.done && has( res.why, "aren't holding that gun" ) &&
           res.turns == 0, "another gun than the one in hand is refused", show( res ) );
    res = run( fp3d::item_op::mod_add, dot, gun, true, -1 );
    check( res.back && res.answers == 1 && !res.done && has( res.why, fit_of( dot.uid ).reason.c_str() ) &&
           res.turns == 0 && gun_now().mods.empty(), "a mod the gun has no slot for is refused in CDDA's words",
           show( res ) );
    res = run( fp3d::item_op::mod_add, fp3d::item_ref{ laser_b.uid + 77, laser_b.type }, gun, true, -1 );
    check( res.back && res.answers == 1 && !res.done && res.turns == 0, "a mod that isn't carried is refused",
           show( res ) );
    // The second of two alike: that one is installed, not the first of its type, and it takes CDDA's time.
    res = run( fp3d::item_op::mod_add, laser_b, gun, true, -1 );
    fp3d::gun_view g = gun_now();
    check( res.back && res.answers == 1 && res.done && res.popups == 0 && res.menus == 0 && res.turns >= 290 &&
           g.mods.size() == 1 && g.mods[0].uid == laser_b.uid && g.mods[0].location == "rail" &&
           g.mods[0].flags == 0 && carried_of( "rail_laser_sight" ) == std::vector<uint64_t> { laser_a.uid } &&
           !fit_of( laser_a.uid ).ok, "the named mod is installed, in CDDA's time",
           show( res ) + ", " + std::to_string( g.mods.size() ) + " mods" );
    // Off again: back in the pack with its handle.
    res = run( fp3d::item_op::mod_remove, laser_b, gun, true, -1 );
    g = gun_now();
    const std::vector<uint64_t> back_in_pack = carried_of( "rail_laser_sight" );
    check( res.back && res.answers == 1 && res.done && res.turns >= 140 && g.mods.empty() &&
           back_in_pack.size() == 2 &&
           std::find( back_in_pack.begin(), back_in_pack.end(), laser_b.uid ) != back_in_pack.end() &&
           fit_of( laser_b.uid ).ok, "taken off again: back in the pack with its handle", show( res ) );
    res = run( fp3d::item_op::mod_remove, laser_b, gun, true, -1 );
    check( res.back && res.answers == 1 && !res.done && res.turns == 0, "a mod the gun doesn't have is refused",
           show( res ) );
    // A permanent mod: CDDA asks first. No: nothing happens. Yes: on, and it can't be taken off.
    res = run( fp3d::item_op::mod_add, barrel, gun, false, -1 );
    check( res.back && res.popups == 1 && has( res.asked, "Permanently install" ) && res.answers == 1 && !res.done &&
           has( res.why, "Never mind" ) && gun_now().mods.empty(), "a permanent mod: CDDA's question, answered no",
           show( res ) );
    res = run( fp3d::item_op::mod_add, barrel, gun, true, -1 );
    g = gun_now();
    check( res.back && res.popups == 1 && res.answers == 1 && res.done && g.mods.size() == 1 &&
           g.mods[0].uid == barrel.uid && ( g.mods[0].flags & fp3d::gun_mod_irremovable ) != 0,
           "answered yes: installed, marked as one that can't be taken off", show( res ) );
    res = run( fp3d::item_op::mod_remove, barrel, gun, true, -1 );
    check( res.back && res.answers == 1 && !res.done && has( res.why, "can't be taken off" ) && res.turns == 0 &&
           gun_now().mods.size() == 1, "a permanent mod is not taken off", show( res ) );
    // A difficult mod: CDDA's tools menu comes as a request; cancelled, nothing happens.
    res = run( fp3d::item_op::mod_add, mount, gun, true, -1 );
    check( res.back && res.popups == 1 && res.menus == 1 && has( res.asked, "Attach your" ) && res.answers == 1 &&
           !res.done && has( res.why, "Never mind" ) && gun_now().mods.size() == 1,
           "a difficult mod: CDDA's tools menu, cancelled", show( res ) );
    // Part 2. Scenes on the sim thread between the actions.
    const auto scene = [&]( int number ) {
        std::optional<fp3d::ui_request> at = fp3d::wait_request( 5000 );
        if( !at || !at->timed ) {
            return false;
        }
        hand_setup = number;
        fp3d::queue_action( "no_such_action" );
        at = fp3d::wait_request( 20000, at->id );
        return at && at->timed;
    };
    const auto mod_on_gun = [&]( const std::string & type ) {
        fp3d::item_ref found{ 0, 0 };
        for( const fp3d::gun_mod_view &m : gun_now().mods ) {
            if( fp3d::id_name( fp3d::id_kind::itype, m.type ) == type ) {
                found = fp3d::item_ref{ m.uid, m.type };
            }
        }
        return found;
    };
    // Gun mods in a vise (FP3D, item_hands.h vise_odds): CDDA's tools menu starts with its odds without tools.
    const auto odds_text = []( int chance, int risk ) {
        return "(" + std::to_string( chance ) + "%) risking damage (" + std::to_string( risk ) + "%)";
    };
    const fp3d::gun_fit_view plain = fit_of( mount.uid );
    check( has( res.entry, odds_text( plain.chance, plain.risk ).c_str() ), "without a vise: CDDA's own odds",
           res.entry );
    check( fp3d::vise_odds( { 100, 0 } ) == std::make_pair( 100, 0 ) &&
           fp3d::vise_odds( { 61, 9 } ) == std::make_pair( 81, 4 ) &&
           fp3d::vise_odds( { 95, 3 } ) == std::make_pair( 100, 1 ) &&
           fp3d::vise_odds( { -5, 300 } ) == std::make_pair( 15, 50 ), "a vise: 20 points more (at most 100), half the risk",
           "" );
    if( !scene( 52 ) ) {
        check( false, "scene 52", "" );
        return false;
    }
    const auto lying = []( int dx, const std::string & type ) {
        fp3d::item_ref found{ 0, 0 };
        const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
        if( !s ) {
            return found;
        }
        const fp3d::ui_point q{ s->player.pos.x + dx, s->player.pos.y, s->player.pos.z };
        for( const fp3d::item_handle_view &h : handles_at( q ) ) {
            if( fp3d::id_name( fp3d::id_kind::itype, h.type ) == type ) {
                found = fp3d::item_ref{ h.uid, h.type };
            }
        }
        return found;
    };
    const fp3d::item_ref vise = lying( 0, "bench_vise" );
    const fp3d::item_ref clamp = lying( -1, "clamp" );
    const fp3d::item_ref far_vise = lying( -3, "bench_vise" );
    const fp3d::item_ref their_vise = lying( 1, "bench_vise" );
    check( vise.uid != 0 && clamp.uid != 0 && far_vise.uid != 0 && their_vise.uid != 0,
           "scene 52: a vise, a clamp, a vise out of reach, someone else's",
           std::to_string( vise.uid ) + " " + std::to_string( clamp.uid ) + " " + std::to_string( far_vise.uid ) + " " +
           std::to_string( their_vise.uid ) );
    // Malformed: a vise with another op, or half a handle, is never queued.
    fp3d::item_action odd;
    odd.op = fp3d::item_op::mod_remove;
    odd.item = barrel;
    odd.container = gun;
    odd.vise = vise;
    const bool vise_on_remove = fp3d::queue_item_action( odd );
    odd.op = fp3d::item_op::mod_add;
    odd.item = mount;
    odd.vise = fp3d::item_ref{ vise.uid, 0 };
    const bool half_vise = fp3d::queue_item_action( odd );
    check( !vise_on_remove && !half_vise, "a vise goes only with mod_add, a whole handle", "" );
    // Not one that holds a gun, or out of reach: refused before CDDA asks anything.
    with_vise = clamp;
    res = run( fp3d::item_op::mod_add, mount, gun, true, -1 );
    check( res.back && res.answers == 1 && !res.done && res.popups == 0 && res.menus == 0 &&
           has( res.why, "can't hold a gun" ) && res.turns == 0, "a clamp is too small to hold a gun", show( res ) );
    with_vise = far_vise;
    res = run( fp3d::item_op::mod_add, mount, gun, true, -1 );
    check( res.back && res.answers == 1 && !res.done && res.popups == 0 && res.menus == 0 &&
           has( res.why, "isn't within reach" ) && res.turns == 0, "a vise out of reach is refused", show( res ) );
    with_vise = their_vise;
    res = run( fp3d::item_op::mod_add, mount, gun, true, -1 );
    check( res.back && res.answers == 1 && !res.done && res.popups == 0 && res.menus == 0 &&
           has( res.why, "isn't yours" ) && res.turns == 0, "someone else's vise is refused", show( res ) );
    // In the vise within reach: CDDA's own questions, its tools menu with the better odds; cancelled, nothing happens.
    with_vise = vise;
    res = run( fp3d::item_op::mod_add, mount, gun, true, -1 );
    with_vise = fp3d::item_ref{ 0, 0 };
    const std::pair<int, int> clamped = fp3d::vise_odds( { plain.chance, plain.risk } );
    check( clamped.first < 100 && res.back && res.popups == 1 && res.menus == 1 && res.answers == 1 && !res.done &&
           has( res.why, "Never mind" ) && has( res.entry, odds_text( clamped.first, clamped.second ).c_str() ) &&
           gun_now().mods.size() == 1, "in a vise: CDDA's menu offers the better odds", show( res ) + " [" + res.entry +
           "]" );
    // The next action without one: CDDA's own odds again.
    res = run( fp3d::item_op::mod_add, mount, gun, true, -1 );
    check( res.menus == 1 && !res.done && has( res.entry, odds_text( plain.chance, plain.risk ).c_str() ),
           "then without a vise: CDDA's own odds again", res.entry );
    if( !scene( 37 ) ) {
        check( false, "scene 37", "" );
        return false;
    }
    const fp3d::item_ref sight = mod_on_gun( "red_dot_sight" );
    const fp3d::item_ref launcher = mod_on_gun( "u_shotgun" );
    check( sight.uid != 0 && launcher.uid != 0 && gun_now().mods.size() == 3, "scene 37: a sight and a launcher on the gun",
           std::to_string( gun_now().mods.size() ) + " mods" );
    // The character couldn't put the sight back: CDDA asks first. No: it stays.
    res = run( fp3d::item_op::mod_remove, sight, gun, false, -1 );
    check( res.back && res.popups == 1 && has( res.asked, "lacking the skills needed to reattach" ) &&
           res.answers == 1 && !res.done && has( res.why, "Never mind" ) && res.turns == 0 &&
           mod_on_gun( "red_dot_sight" ).uid == sight.uid, "taking off a mod you couldn't put back: CDDA asks; no",
           show( res ) );
    // A loaded mod that can't be unloaded stays on, with a reason.
    res = run( fp3d::item_op::mod_remove, launcher, gun, true, -1 );
    check( res.back && res.answers == 1 && !res.done && has( res.why, "can't unload" ) && res.turns == 0 &&
           mod_on_gun( "u_shotgun" ).uid == launcher.uid, "a mod that can't be unloaded is not taken off",
           show( res ) );
    // An unfinished mod job: nothing else is done to the gun's mods.
    if( !scene( 38 ) ) {
        check( false, "scene 38", "" );
        return false;
    }
    res = run( fp3d::item_op::mod_remove, sight, gun, true, -1 );
    const result waiting_add = run( fp3d::item_op::mod_add, laser_a, gun, true, -1 );
    check( res.back && res.answers == 1 && !res.done && has( res.why, "Finish the work" ) && res.popups == 0 &&
           waiting_add.back && !waiting_add.done && has( waiting_add.why, "Finish the work" ) &&
           gun_now().mods.size() == 3, "refused while a mod job is unfinished", show( res ) );
    // That job gone and the skills back: no question, off in CDDA's time, into the pack.
    if( !scene( 39 ) ) {
        check( false, "scene 39", "" );
        return false;
    }
    res = run( fp3d::item_op::mod_remove, sight, gun, true, -1 );
    const std::vector<uint64_t> sights = carried_of( "red_dot_sight" );
    check( res.back && res.answers == 1 && res.done && res.popups == 0 && res.turns >= 140 &&
           mod_on_gun( "red_dot_sight" ).uid == 0 && gun_now().mods.size() == 2 &&
           std::find( sights.begin(), sights.end(), sight.uid ) != sights.end(),
           "with the skills, no question: taken off", show( res ) );
    return failed == 0;
}

static bool run_name_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  names: %-50s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    hand_setup = 33;
    fp3d::queue_action( "pause" );
    r = fp3d::wait_request( 30000, r->id );
    for( int i = 0; i < 10 && r && !r->timed; i++ ) {
        fp3d::ui_answer cancel;
        cancel.id = r->id;
        cancel.cancel = true;
        fp3d::answer( cancel );
        r = fp3d::wait_request( 10000, r->id );
    }
    name_results res;
    {
        const std::lock_guard<std::mutex> lk( names_mutex );
        res = names_out;
    }
    check( r && r->timed && res.ran && res.swapped && res.taken_ok && res.id_kept,
           "a taken-over name is checked, the save id stays",
           std::string( res.ran ? "" : "scene not run " ) + ( res.swapped ? "" : "not taken over " ) +
           ( res.id_kept ? "" : "save id changed " ) + std::to_string( res.name_bytes ) + " bytes" );
    check( res.ran && res.save_refused, "a save under a refused id is refused", "" );
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
        int dirt = -1;              // the gun's fouling (item var dirt, -1 = none); 10000 always misfires
        std::string ammo;           // the gun's ammo, "" = 9mm
        bool spare_gun = false;     // also a jammed loaded `gun` in the pack
        bool ground_gun = false;    // and one lying a square west
        bool turret_shot = false;   // fire 3 rounds from a jammed `gun` nobody holds, as a vehicle turret does
        int moves = 0;              // moves left for the turn that fires (0 = a full turn)
        bool armed = true;          // wield a loaded `gun`; else empty hands (throwing wields the rock)
        bool rock = false;          // carry a rock to throw
        std::string melee;          // wield this item instead of `gun` ("" = none)
        // no martial-art style (only the wielded weapon's techniques apply), no pain, full stamina
        bool plain_style = false;
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
            if( sc->plain_style ) {
                u.martial_arts_data->set_style( matype_id( "style_none" ), true );
                // the many strikes of the call checks add up to no pain or tiredness for the checks after them
                u.set_pain( 0 );
                u.set_stamina( u.get_stamina_max() );
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
                // No random malfunction (CDDA's roll, about 1 in 300 shots even for a new gun and a skilled
                // shooter: the shot silently doesn't happen): the checks are about the hooks. Jams are set above.
                // A fouled gun keeps CDDA's misfire roll (NEVER_JAMS turns it off): at 10000 it always misfires.
                if( sc->dirt >= 0 ) {
                    gun.set_var( "dirt", sc->dirt );
                } else {
                    gun.set_flag( flag_NEVER_JAMS );
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
                const tripoint_bub_ms at = p + tripoint_rel_ms( x, y, 0 );
                monster *m = g->place_critter_at( mon_zombie, at );
                if( m == nullptr ) {
                    // Rare (2 of 6 runs on one machine, 2026-11-21): said why, so the next one can be traced.
                    const Creature *in_way = get_creature_tracker().creature_at( at );
                    std::printf( "  combat: scene: no zombie placed at %+d,%+d (%s, %s; avatar at %s)\n", x, y,
                                 in_way == nullptr ? "no creature there" : in_way->is_npc() ? "an NPC there" :
                                 in_way->is_avatar() ? "the avatar there" : "a monster there",
                                 here.passable( at ) ? "passable" : "not passable", u.pos_abs().to_string().c_str() );
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
                base.set_flag( flag_NEVER_JAMS );   // its 3 rounds all fire (no random malfunction)
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
        std::vector<fp3d::event> damages;           // damage events from the action on
        std::vector<fp3d::event> misfires;          // misfire events from the action on
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
            } else if( e.kind == fp3d::event_kind::damage ) {
                r.damages.push_back( std::move( e ) );
            } else if( e.kind == fp3d::event_kind::misfire ) {
                r.misfires.push_back( std::move( e ) );
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
        // the damage event names the weak point the hit landed on: the head when the client's part landed (its odds
        // are rolled, part_lands), else whatever CDDA picked, a plain id or none
        const bool landed = delta( r.c0.parts, r.c1.parts ) == 1;
        std::string named;
        for( const fp3d::event &e : r.damages ) {
            named += ( named.empty() ? "\"" : ", \"" ) + e.text + "\"";
        }
        const std::string wp = r.damages.size() == 1 ? r.damages[0].text : std::string( "-" );
        check( r.damages.size() == 1 && ( landed ? wp == "head" : wp == fp3d::weakpoint_text( wp ) ),
               "the damage event names the weak point hit", std::string( landed ? "landed" : "not landed" ) +
               ", damage " + ( named.empty() ? "none" : named ) );
        check( r.misfires.empty(), "a round that fires sends no misfire event",
               std::to_string( r.misfires.size() ) + " misfire events" );
    }
    // A gun fouled to CDDA's most (dirt 10000) misfires: the trigger is pulled, no round goes off (no shot request,
    // no projectile, the round stays), and the player's misfire event says so with the gun.
    {
        scene fouled = point_blank;
        fouled.dirt = 10000;
        const result r = run( fouled, "fire", answer_with::fire, head, true, head );
        const fp3d::event m = r.misfires.empty() ? fp3d::event() : r.misfires.front();
        check( fired( r, 0 ) && r.shots.empty() && r.projectiles.empty() && r.misfires.size() == 1 && m.uid == 1 &&
               m.detail == 1 && m.type != 0,
               "a fouled gun misfires: a misfire event, nothing fired", counts( r ) + ", " +
               std::to_string( r.misfires.size() ) + " misfire events, detail " + std::to_string( m.detail ) +
               ", " + std::to_string( r.shots.size() ) + " shot requests" );
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
        // A real gun shot's projectile event names its ammo (detail) and no flying item (type).
        const uint32_t ammo_fired = m.projectiles.empty() ? 0 : m.projectiles.front().detail;
        check( !m.projectiles.empty() && ammo_fired != 0 && m.projectiles.front().type == 0 &&
               ammo_fired == fp3d::intern_id( fp3d::id_kind::itype, "9mm" ),
               "a gun's projectile names its ammo", std::to_string( ammo_fired ) );
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
        for( int i = 0; i < 20; i++ ) {
            const result r = run( far_zombie, "fire", answer_with::fire, dead_centre, false, std::nullopt );
            if( !fired( r, 1 ) ) {
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
        check( shots == 20 && crits < shots, "20 claimed head shots: CDDA's roll decides",
               std::to_string( shots ) + " fired, " + std::to_string( hits ) + " hit, " + std::to_string( crits ) +
               " critical / head" );
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
    // answer leaves CDDA's pick; the off hand says so; a reach weapon strikes along the direction. The answer may
    // also call a technique family (its action): CDDA's own techniques of that family go first, by CDDA's rules.
    {
        struct swung {
            bool back = false;
            std::vector<fp3d::ui_request> swings;
            seen before;
            seen after;
            fp3d::swing_counts c0;
            fp3d::swing_counts c1;
            std::vector<fp3d::event> attacks;       // the avatar's attack events of the strike
        };
        using swing_answerer = std::function<std::optional<fp3d::shot_answer>( const fp3d::ui_request &,
                               const seen & )>;
        // Builds `sc`, starts `action` and answers each swing request with `reply` (nullopt: a cancel) and the
        // call `call` as the answer's action.
        const auto swing_run = [&]( const scene & sc, const std::string & action, const swing_answerer & reply,
                                    const std::string & call = std::string(),
        const std::optional<std::array<int, 3>> &square = std::nullopt ) {
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
            {
                std::vector<fp3d::event> old;
                fp3d::drain_events( old );
            }
            if( square ) {   // queue_action_at: that square, (dx, dy, dz) from the avatar
                const std::shared_ptr<const fp3d::snapshot> snap = fp3d::latest_snapshot();
                const fp3d::ui_point me = snap ? snap->player.pos : fp3d::ui_point{};
                fp3d::queue_action_at( action, fp3d::ui_point{ me.x + ( *square )[0], me.y + ( *square )[1],
                                       me.z + ( *square )[2] } );
            } else {
                fp3d::queue_action( action );
            }
            std::optional<fp3d::ui_request> c = fp3d::wait_request( 20000, q->id );
            int asked = 0;
            while( c && !c->timed && asked++ < 10 ) {
                fp3d::ui_answer a;
                a.id = c->id;
                if( c->kind == fp3d::request_kind::swing ) {
                    s.swings.push_back( *c );
                    a.action = call;
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
            std::vector<fp3d::event> evs;
            fp3d::drain_events( evs );
            for( fp3d::event &e : evs ) {
                if( e.kind == fp3d::event_kind::attack && e.uid == 1 ) {
                    s.attacks.push_back( std::move( e ) );
                }
            }
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
                              std::to_string( s.c1.parts - s.c0.parts ) + " hints +" +
                              std::to_string( s.c1.hints - s.c0.hints ) + " hinted +" +
                              std::to_string( s.c1.hinted - s.c0.hinted ) + ", hp " + std::to_string( s.before.hp ) +
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
        // The call. With no martial-art style a quarterstaff's techniques are a plain strike (RAPID) and a sweep
        // that downs (SWEEP), both only for a hit that isn't critical (a critical one has no technique then); a
        // bat's only one is a stun on a critical hit (BRUTAL). The avatar's attack event names what it used.
        // (Before the off-hand, reach and wall checks: the block ends on the scene it always ended on.)
        const std::string aim_left = "fp3d_attack_main_aim_LEFT";
        scene staff = point_blank;
        staff.zx = -1;
        staff.melee = "q_staff";
        staff.plain_style = true;
        // read while the sim waits for its next answer
        const matec_id sweep( "SWEEP" );
        const std::string sweep_name = sweep.is_valid() ? sweep->name.translated() : std::string( "?" );
        // What some strikes did, added up.
        struct tally {
            int strikes = 0;
            int clean = 0;          // strikes that came back, asked once and sent one attack event of the avatar
            int accepted = 0;
            int refused = 0;
            int hints = 0;
            int hinted = 0;
            int hits[fp3d::max_technique_family + 1] = {};    // the avatar's hits, by technique family
            int misnamed = 0;       // events whose technique name doesn't go with their family
        };
        const auto add = [&]( tally & t, const swung & r ) {
            t.strikes++;
            t.clean += r.back && r.swings.size() == 1 && r.c1.asked - r.c0.asked == 1 && r.attacks.size() == 1 ? 1 : 0;
            t.accepted += static_cast<int>( r.c1.accepted - r.c0.accepted );
            t.refused += static_cast<int>( r.c1.refused - r.c0.refused );
            t.hints += static_cast<int>( r.c1.hints - r.c0.hints );
            t.hinted += static_cast<int>( r.c1.hinted - r.c0.hinted );
            for( const fp3d::event &e : r.attacks ) {
                const int family = std::clamp<int>( e.amount, 0, fp3d::max_technique_family );
                if( e.detail == 1 ) {
                    t.hits[family]++;
                }
                // a miss and a hit with no technique name nothing; a sweep is the only technique here that downs
                bool fits = !e.text.empty();
                if( e.detail != 1 ) {
                    fits = e.text.empty() && e.amount == 0;
                } else if( family == fp3d::technique_none ) {
                    fits = e.text.empty();
                } else if( family == fp3d::technique_down ) {
                    fits = e.text == sweep_name;
                }
                t.misnamed += fits ? 0 : 1;
            }
        };
        const auto hit_count = []( const tally & t ) {
            int n = 0;
            for( const int h : t.hits ) {
                n += h;
            }
            return n;
        };
        // every hit is of family `a` or `b`
        const auto only = [&]( const tally & t, int a, int b ) {
            return hit_count( t ) == t.hits[a] + ( a == b ? 0 : t.hits[b] );
        };
        const auto told = [&]( const tally & t ) {
            std::string out = std::to_string( t.strikes ) + " strikes (" + std::to_string( t.clean ) +
                              " asked once, one attack event), accepted +" + std::to_string( t.accepted ) +
                              " refused +" + std::to_string( t.refused ) + " hints +" + std::to_string( t.hints ) +
                              " hinted +" + std::to_string( t.hinted ) + ", hits by family";
            for( const int h : t.hits ) {
                out += " " + std::to_string( h );
            }
            return out + ( t.misnamed == 0 ? std::string() : ", misnamed " + std::to_string( t.misnamed ) );
        };
        // Strikes at `sc` with the answer `reply` and the call `call`: `least` of them, then more (up to `most`)
        // until `enough` says so.
        const auto strike_until = [&]( const scene & sc, const swing_answerer & reply, const std::string & call,
        int least, int most, const std::function<bool( const tally & )> &enough ) {
            tally t;
            while( t.strikes < least || ( t.strikes < most && !enough( t ) ) ) {
                add( t, swing_run( sc, aim_left, reply, call ) );
            }
            return t;
        };
        const auto any_hit = [&]( const tally & t ) {
            return hit_count( t ) > 0;
        };
        const auto swept = []( const tally & t ) {
            return t.hits[fp3d::technique_down] > 0;
        };

        // A hit that isn't critical sweeps (2), a critical one has no technique (0): never the plain strike (1).
        tally sum = strike_until( staff, aimed_at( "leg" ), "down", 12, 12, any_hit );
        check( sum.clean == 12 && sum.accepted == 12 && sum.hints == 12 && sum.misnamed == 0 &&
               only( sum, fp3d::technique_none, fp3d::technique_down ) &&
               sum.hinted == sum.hits[fp3d::technique_down], "a called family is used when CDDA allows it",
               told( sum ) );
        // and on until a sweep was seen, if none was
        if( !swept( sum ) ) {
            sum = strike_until( staff, aimed_at( "leg" ), "down", 1, 40, swept );
        }
        check( sum.clean == sum.strikes && swept( sum ) && sum.misnamed == 0 &&
               sum.hinted == sum.hits[fp3d::technique_down] && sweep_name == "Sweep Attack",
               "a called hit names CDDA's technique", "\"" + sweep_name + "\", " + told( sum ) );
        sum = strike_until( staff, aimed_at( "leg" ), "", 1, 60, []( const tally & t ) {
            return t.hits[fp3d::technique_strike] > 0 && t.hits[fp3d::technique_down] > 0;
        } );
        check( sum.clean == sum.strikes && sum.hits[fp3d::technique_strike] >= 1 && swept( sum ) &&
               hit_count( sum ) == sum.hits[fp3d::technique_none] + sum.hits[fp3d::technique_strike] +
               sum.hits[fp3d::technique_down] && sum.hints == 0 && sum.hinted == 0 && sum.misnamed == 0,
               "no call: CDDA's own draw", told( sum ) );
        sum = tally();
        for( const swing_answerer *r : { &other_uid, &other_square, &cancel } ) {
            add( sum, swing_run( staff, aim_left, *r, "down" ) );
        }
        check( sum.clean == 3 && sum.refused == 3 && sum.hints == 0 && sum.hinted == 0 && sum.misnamed == 0,
               "a call for another creature, square or with a cancel is ignored", told( sum ) );
        sum = tally();
        const std::vector<std::string> words = { "DOWN", "down ", "sweep", "strike", std::string( 64, 'a' ) };
        for( const std::string &word : words ) {
            add( sum, swing_run( staff, aim_left, aimed_at( "leg" ), word ) );
        }
        check( sum.clean == 5 && sum.accepted == 5 && sum.hints == 0 && sum.hinted == 0 && sum.misnamed == 0,
               "only the exact words call", told( sum ) );
        // The word is read as the bridge hands it over: ask() takes control characters out of every answer's
        // action first (contracts/ui_requests.md, Rules), so these three are "down".
        sum = tally();
        const std::vector<std::string> cleaned = { "down\n", "do\x01wn", "\x7f" "down" };
        for( const std::string &word : cleaned ) {
            add( sum, swing_run( staff, aim_left, aimed_at( "leg" ), word ) );
        }
        check( sum.clean == 3 && sum.accepted == 3 && sum.hints == 3 && sum.misnamed == 0 &&
               only( sum, fp3d::technique_none, fp3d::technique_down ) &&
               sum.hinted == sum.hits[fp3d::technique_down], "a call is read after the bridge's cleaning",
               told( sum ) );
        scene plain_bat = bat;
        plain_bat.plain_style = true;
        sum = strike_until( plain_bat, aimed_at( "leg" ), "down", 6, 30, any_hit );
        check( sum.clean == sum.strikes && sum.hints == sum.strikes && sum.hinted == 0 && any_hit( sum ) &&
               only( sum, fp3d::technique_none, fp3d::technique_stun ) && sum.misnamed == 0,
               "a family the weapon lacks leaves CDDA's pick", told( sum ) );
        scene unskilled = staff;
        unskilled.skill = 1;
        sum = strike_until( unskilled, aimed_at( "leg" ), "down", 6, 30, any_hit );
        check( sum.clean == sum.strikes && sum.hints == sum.strikes && sum.hinted == 0 && any_hit( sum ) &&
               only( sum, fp3d::technique_none, fp3d::technique_none ) && sum.misnamed == 0,
               "skill gates still hold", told( sum ) );
        sum = strike_until( staff, aimed_at( "" ), "down", 1, 1, any_hit );
        check( sum.clean == 1 && sum.accepted == 0 && sum.refused == 1 && sum.hints == 1 && sum.misnamed == 0 &&
               only( sum, fp3d::technique_none, fp3d::technique_down ) &&
               sum.hinted == sum.hits[fp3d::technique_down], "a call without a part", told( sum ) );
        scene bare = point_blank;
        bare.zx = -1;
        bare.armed = false;
        s = swing_run( bare, "fp3d_attack_off_aim_LEFT", aimed_at( "head" ) );
        check( s.back && at( s, -1, 0 ) && s.swings[0].mode == "off", "off-hand aimed strike says off", shown( s ) );
        scene spear = point_blank;
        spear.zx = -2;
        spear.melee = "spear_wood";
        s = swing_run( spear, "fp3d_attack_main_aim_LEFT", aimed_at( "head" ) );
        // Time spent: fewer moves, or a turn gone by (a strike that costs exactly a turn's moves leaves as many).
        check( s.back && at( s, -2, 0 ) && s.swings[0].range == 2 &&
               ( s.after.turn > s.before.turn || s.after.moves < s.before.moves ),
               "a spear strikes 2 squares off", shown( s ) + ", moves " + std::to_string( s.before.moves ) + " -> " +
               std::to_string( s.after.moves ) + ", turns +" + std::to_string( s.after.turn - s.before.turn ) );
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
               s.after.turn == s.before.turn && s.after.hp == s.before.hp,
               "a spear doesn't strike someone unseen behind a wall",
               shown( s ) + ", moves " + std::to_string( s.before.moves ) + " -> " + std::to_string( s.after.moves ) );
        // A square the client names (queue_action_at): a knight's move off, the main hand only, within reach.
        const auto spent = []( const swung & s ) {
            return s.after.turn > s.before.turn || s.after.moves < s.before.moves;
        };
        const auto idle = [&]( const swung & s, const char *text ) {
            return s.back && s.swings.empty() && s.after.moves == s.before.moves && s.after.turn == s.before.turn &&
                   said( s.after, text );
        };
        scene knight = spear;
        knight.zy = -1;
        s = swing_run( knight, "fp3d_attack_main_aim_at", aimed_at( "head" ), "", std::array<int, 3> { -2, -1, 0 } );
        check( s.back && at( s, -2, -1 ) && s.swings[0].range == 2 && spent( s ),
               "a spear strikes a knight's move off at the named square", shown( s ) );
        s = swing_run( knight, "fp3d_attack_main_at", aimed_at( "head" ), "", std::array<int, 3> { -2, -1, 0 } );
        check( s.back && s.swings.empty() && spent( s ), "... unaimed: CDDA's reach attack, no swing request",
               shown( s ) );
        s = swing_run( knight, "fp3d_attack_main_aim_at", aimed_at( "head" ), "", std::array<int, 3> { -1, 0, 0 } );
        check( s.back && s.swings.empty() && said( s.after, "Nothing there to hit" ) && s.after.moves == s.before.moves &&
               s.after.turn == s.before.turn, "... a named square with nobody there strikes nothing", shown( s ) );
        scene knight_far = knight;
        knight_far.zx = -3;
        s = swing_run( knight_far, "fp3d_attack_main_aim_at", aimed_at( "head" ), "", std::array<int, 3> { -3, -1, 0 } );
        check( idle( s, "out of reach" ), "... 3 off is out of a spear's reach", shown( s ) );
        scene knight_bat = knight;
        knight_bat.melee = "bat";
        s = swing_run( knight_bat, "fp3d_attack_main_aim_at", aimed_at( "head" ), "", std::array<int, 3> { -2, -1, 0 } );
        check( idle( s, "out of reach" ), "... a bat doesn't reach it", shown( s ) );
        s = swing_run( knight, "fp3d_attack_main_aim_at", aimed_at( "head" ), "", std::array<int, 3> { 0, 0, 0 } );
        const bool own = idle( s, "out of reach" );
        s = swing_run( knight, "fp3d_attack_main_aim_at", aimed_at( "head" ), "", std::array<int, 3> { -2, -1, 1 } );
        const bool up = idle( s, "out of reach" );
        s = swing_run( knight, "fp3d_attack_main_aim_at", aimed_at( "head" ) );
        const bool unnamed = idle( s, "Nothing there to hit" );
        s = swing_run( knight, "fp3d_attack_off_aim_at", aimed_at( "head" ), "", std::array<int, 3> { -2, -1, 0 } );
        const bool off = s.back && s.swings.empty() && s.after.moves == s.before.moves &&
                         s.after.turn == s.before.turn && s.after.hp == s.before.hp;
        check( own && up && unnamed && off, "... refused: your own square, another level, no square, the off hand",
               std::string( own ? "" : "own " ) + ( up ? "" : "up " ) + ( unnamed ? "" : "unnamed " ) +
               ( off ? "" : "off" ) );
        scene next_to = spear;
        next_to.zx = -1;
        s = swing_run( next_to, "fp3d_attack_main_aim_at", aimed_at( "head" ), "", std::array<int, 3> { -1, -1, 0 } );
        const bool empty_diag = idle( s, "Nothing there to hit" );
        s = swing_run( next_to, "fp3d_attack_main_aim_at", aimed_at( "head" ), "", std::array<int, 3> { -1, 0, 0 } );
        check( empty_diag && s.back && at( s, -1, 0 ) && s.swings[0].range == 1 && spent( s ),
               "... next to you: a plain strike at that square", shown( s ) );
    }
    return failed == 0;
}

// Smash at a square (2026-11-17; the client's strike keys at a wall, door, window or furniture send
// queue_action_at("smash", square)): CDDA's smash takes exactly the queued square next to the avatar and asks no
// direction; a square farther off or on another level is refused ("Never mind."), nothing asked, and isn't left for
// a later prompt; a smash queued without a square still asks the way.
static bool run_smash_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  smash: %-58s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    hand_setup = 40;
    fp3d::queue_action( "no_such_action" );   // dropped, no turn passes: the scene runs before the next request
    r = fp3d::wait_request( 20000, r->id );
    const std::shared_ptr<const fp3d::snapshot> s0 = fp3d::latest_snapshot();
    if( !r || !r->timed || !s0 || hand_setup.load() != 0 ) {
        check( false, "scene", "" );
        return false;
    }
    const fp3d::ui_point me = s0->player.pos;
    const auto at = [&]( int dx, int dy, int dz = 0 ) {
        return fp3d::ui_point{ me.x + dx, me.y + dy, me.z + dz };
    };
    // The terrain id on absolute square `p` in the newest snapshot ("" when not in it).
    const auto ter_at = []( const fp3d::ui_point & p ) {
        const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
        if( !s ) {
            return std::string();
        }
        const auto floor_div = []( int a ) {
            return a >= 0 ? a / fp3d::sm_size : ( a - fp3d::sm_size + 1 ) / fp3d::sm_size;
        };
        const int sx = floor_div( p.x );
        const int sy = floor_div( p.y );
        const int tile = ( p.y - sy * fp3d::sm_size ) * fp3d::sm_size + ( p.x - sx * fp3d::sm_size );
        for( const std::shared_ptr<const fp3d::submap_view> &sm : s->submaps ) {
            if( sm->pos.x == sx && sm->pos.y == sy && sm->pos.z == p.z ) {
                return fp3d::cdda_id_name( "ter", sm->ter[tile] );
            }
        }
        return std::string();
    };
    // `smash` queued (at `sq`, or plain without one) and run to the next main-loop request: its messages; `prompts`
    // counts what it asked (each cancelled).
    int prompts = 0;
    const auto act = [&]( const std::optional<fp3d::ui_point> &sq ) {
        std::vector<fp3d::event> evs;
        fp3d::drain_events( evs );
        evs.clear();
        prompts = 0;
        if( !( sq ? fp3d::queue_action_at( "smash", *sq ) : fp3d::queue_action( "smash" ) ) ) {
            return std::string( "(refused)" );
        }
        const uint64_t before = r->id;
        const auto t = std::chrono::steady_clock::now();
        while( ms_since( t ) < 20000 * fp3d::slow_build ) {
            std::optional<fp3d::ui_request> next = fp3d::wait_request( 200, r->id );
            if( !next ) {
                continue;
            }
            r = next;
            if( !r->timed ) {
                prompts++;
                fp3d::ui_answer a;
                a.id = r->id;
                a.cancel = true;
                fp3d::answer( a );
                continue;
            }
            if( r->id != before ) {
                break;
            }
        }
        fp3d::drain_events( evs );
        std::string msgs;
        for( const fp3d::event &e : evs ) {
            if( e.kind == fp3d::event_kind::message ) {
                msgs += ( msgs.empty() ? "" : " | " ) + e.text;
            }
        }
        return msgs;
    };
    const auto has = []( const std::string & str, const char *part ) {
        return str.find( part ) != std::string::npos;
    };

    check( ter_at( at( 1, 0 ) ) == "t_window" && ter_at( at( 0, -2 ) ) == "t_window",
           "scene: a window east, another 2 north", ter_at( at( 1, 0 ) ) + " " + ter_at( at( 0, -2 ) ) );
    std::string m = act( at( 0, -2 ) );
    check( prompts == 0 && ter_at( at( 0, -2 ) ) == "t_window" && has( m, "Never mind" ),
           "a window 2 squares off: refused, nothing asked", "[" + m + "]" );
    m = act( std::nullopt );
    check( prompts == 1 && ter_at( at( 1, 0 ) ) == "t_window",
           "then a smash with no square still asks the way", "[" + m + "] " + std::to_string( prompts ) + " asked" );
    m = act( at( 1, 0, 1 ) );
    check( prompts == 0 && ter_at( at( 1, 0 ) ) == "t_window" && has( m, "Never mind" ),
           "a square on the level above: refused, nothing asked", "[" + m + "]" );
    // CDDA rolls each bash: a few tries at most with a rock against glass.
    int tries = 0;
    int asked = 0;
    while( ter_at( at( 1, 0 ) ) == "t_window" && tries < 10 ) {
        m = act( at( 1, 0 ) );
        asked += prompts;
        tries++;
    }
    check( ter_at( at( 1, 0 ) ) != "t_window" && ter_at( at( 0, -2 ) ) == "t_window" && asked == 0,
           "the window next to you: smashed at its square, nothing asked", ter_at( at( 1, 0 ) ) + " after " +
           std::to_string( tries ) + " tries, " + std::to_string( asked ) + " asked, [" + m + "]" );
    hand_setup = 41;
    fp3d::queue_action( "no_such_action" );   // dropped: the scene runs before the next request
    r = fp3d::wait_request( 20000, r->id );
    check( r && r->timed && hand_setup.load() == 0, "after: the weapon from before back", "" );
    return failed == 0;
}

// Safe mode (2026-11-10): FP3D games start with CDDA's safe mode off, new and loaded (boot.cpp); the `safemode`
// action (Shift+1) still toggles it; auto-pause on a new hostile is its own clock setting (bridge.h).
// The Unstoppable cheat (fp3d/cheats.h, 2026-11-17) and "Break all grabs" (range_op_kind::break_grabs). Off: a zombie
// holding the torso keeps holding when the player bumps it (CDDA's bump attack, no step), and zombies all around
// stop a step. On: the next action breaks the grab the way CDDA's won break-free does (both sides cleared), and a
// step into a zombie swaps places with it at a normal step's cost, with no attack; a hostile NPC is pushed past
// too. A swim pushes a zombie aside (never under the player); an IMMOBILE monster and an NPC on a vehicle part are
// bumped as without the cheat. The one-shot breaks a grab with the cheat off.
static bool run_unstoppable_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  unstoppable: %-52s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    // `action` queued and run to the next main-loop request (prompts cancelled): its messages.
    const auto act = [&]( const char *action ) {
        std::vector<fp3d::event> evs;
        fp3d::drain_events( evs );
        evs.clear();
        if( !fp3d::queue_action( action ) ) {
            return std::string( "(refused)" );
        }
        const uint64_t before = r->id;
        const auto t = std::chrono::steady_clock::now();
        while( ms_since( t ) < 20000 * fp3d::slow_build ) {
            std::optional<fp3d::ui_request> next = fp3d::wait_request( 200, r->id );
            if( !next ) {
                continue;
            }
            r = next;
            if( !r->timed ) {
                fp3d::ui_answer a;
                a.id = r->id;
                a.cancel = true;
                fp3d::answer( a );
                continue;
            }
            if( r->id != before ) {
                break;
            }
        }
        fp3d::drain_events( evs );
        std::string msgs;
        for( const fp3d::event &e : evs ) {
            if( e.kind == fp3d::event_kind::message ) {
                msgs += ( msgs.empty() ? "" : " | " ) + e.text;
            }
        }
        return msgs;
    };
    // Scene `n` (unstop_scene / unstop_look) run before the next request; nothing acts ("no_such_action" is dropped).
    const auto scene = [&]( int n ) {
        hand_setup = n;
        fp3d::queue_action( "no_such_action" );
        r = fp3d::wait_request( 20000, r->id );
        return r && r->timed && hand_setup.load() == 0;
    };
    struct look {
        int grabbed = 0;
        int grabbing = 0;
        int zombies = 0;
        int hp = 0;
        int dx = 0;
        int dy = 0;
        int origin = 0;
        int east = 0;
        int under = 0;
        int turn = 0;
    };
    const auto look_now = [&]() {
        look l;
        if( !scene( 45 ) ) {
            l.zombies = -1;
            return l;
        }
        l.grabbed = unstop_view.grabbed.load();
        l.grabbing = unstop_view.grabbing.load();
        l.zombies = unstop_view.zombies.load();
        l.hp = unstop_view.hp.load();
        l.dx = unstop_view.dx.load();
        l.dy = unstop_view.dy.load();
        l.origin = unstop_view.origin.load();
        l.east = unstop_view.east.load();
        l.under = unstop_view.under.load();
        l.turn = unstop_view.turn.load();
        return l;
    };
    const auto describe = []( const look & l ) {
        return string_format( "grabbed %d grabbing %d, %d zombies %d hp, at %+d,%+d, start square %d, east %d, "
                              "under %d, turn %d", l.grabbed, l.grabbing, l.zombies, l.hp, l.dx, l.dy, l.origin,
                              l.east, l.under, l.turn );
    };
    const auto has = []( const std::string & str, const char *part ) {
        return str.find( part ) != std::string::npos;
    };
    // The cheat as the bridge asks for it, then a request so it is applied and published.
    const auto set_unstoppable = [&]( bool on ) {
        const uint32_t bit = 1U << static_cast<int>( fp3d::cheat::unstoppable );
        if( !fp3d::request_cheat( fp3d::cheat::unstoppable, on ) || !scene( 0 ) ) {
            return false;
        }
        return ( ( fp3d::latest_cheats().on & bit ) != 0 ) == on;
    };

    check( fp3d::cheat_from_name( "unstoppable" ) == fp3d::cheat::unstoppable &&
           std::string( fp3d::cheat_name( fp3d::cheat::unstoppable ) ) == "unstoppable", "named \"unstoppable\"", "" );
    const bool cheats_were = fp3d::cheats_allowed();
    fp3d::set_cheats_allowed( false );
    fp3d::range_op op;
    op.kind = fp3d::range_op_kind::break_grabs;
    check( !fp3d::request_cheat( fp3d::cheat::unstoppable, true ) && fp3d::request_range_op( op ) == 0,
           "refused while cheats aren't allowed (cheat, break all grabs)", "" );
    fp3d::set_cheats_allowed( true );
    check( set_unstoppable( false ), "off at first", "" );

    // Off: the grab holds and walking into the grabber is CDDA's bump attack.
    check( scene( 42 ), "scene: a zombie east holding the torso", "" );
    look l = look_now();
    check( l.grabbed == 1 && l.grabbing == 1 && l.zombies == 1 && l.origin == 0, "held: both sides", describe( l ) );
    std::string m = act( "RIGHT" );
    l = look_now();
    // A hit may stagger the zombie, and CDDA's staggered effect ends its side of the grab; the player stays held.
    check( l.grabbed == 1 && l.dx == 0 && l.dy == 0 && l.zombies == 1,
           "off: a step into it attacks, the grab holds", describe( l ) + " [" + m + "]" );

    // On: the next action breaks it, both sides (a fresh grab).
    check( scene( 42 ), "scene: held again", "" );
    check( set_unstoppable( true ), "turned on", "" );
    l = look_now();
    check( l.grabbed == 1 && l.grabbing == 1, "turning it on breaks nothing yet", describe( l ) );
    m = act( "pause" );
    l = look_now();
    check( l.grabbed == 0 && l.grabbing == 0 && l.dx == 0 && l.dy == 0 && has( m, "grab on your torso" ),
           "on: waiting a turn breaks the grab, both sides", describe( l ) + " [" + m + "]" );

    // On, with no-damage on too (the user's case), surrounded and held by four: one step east breaks every grab and
    // swaps places with that zombie, no attack, a normal step's time.
    const bool invincible_was = ( fp3d::latest_cheats().on & ( 1U << static_cast<int>( fp3d::cheat::invincible ) ) ) != 0;
    fp3d::request_cheat( fp3d::cheat::invincible, true );
    check( scene( 43 ), "scene: zombies on all eight squares, four holding on", "" );
    const look before = look_now();
    m = act( "RIGHT" );
    l = look_now();
    check( before.zombies == 8 && before.grabbed == 4 && before.grabbing == 4 && l.grabbed == 0 && l.grabbing == 0 &&
           l.zombies == 8 && l.dx == 1 && l.dy == 0 && l.origin == 1 && l.hp == before.hp && l.turn - before.turn <= 1,
           "on: one step breaks all four grabs and pushes past, no attack",
           describe( before ) + " -> " + describe( l ) + " [" + m + "]" );
    m = act( "RIGHT" );
    l = look_now();
    check( l.dx == 2 && l.dy == 0 && l.zombies == 8, "and the next step is out", describe( l ) + " [" + m + "]" );

    // On: a hostile NPC is pushed past too.
    check( scene( 46 ), "scene: a hostile NPC east", "" );
    m = act( "RIGHT" );
    l = look_now();
    check( l.dx == 1 && l.dy == 0 && l.origin == 2, "on: a step into a hostile NPC swaps places",
           describe( l ) + " [" + m + "]" );

    // On, both in deep water: the step is CDDA's swim, which moves nobody aside, so the zombie goes to a free
    // square next to it first, never under the player.
    check( scene( 47 ), "scene: deep water, a zombie east in it", "" );
    l = look_now();
    const bool zombie_east = l.east == 1;
    m = act( "RIGHT" );
    l = look_now();
    check( zombie_east && l.dx == 1 && l.dy == 0 && l.zombies == 1 && l.under == 0 && l.origin == 0 &&
           has( m, "aside" ), "on: a swim into a zombie pushes it aside, not under you",
           describe( l ) + " [" + m + "]" );

    // On: one CDDA never displaces (a fungaloid tower, IMMOBILE) is bumped as without the cheat: no step.
    check( scene( 48 ), "scene: a fungaloid tower east", "" );
    l = look_now();
    const bool tower_east = l.east == 1;
    m = act( "RIGHT" );
    l = look_now();
    check( tower_east && l.dx == 0 && l.dy == 0 && l.origin == 0 && l.under == 0,
           "on: an immobile one isn't moved", describe( l ) + " [" + m + "]" );

    // On: a hostile NPC on a vehicle part is bumped as without the cheat (CDDA's swap boards and unboards both).
    check( scene( 49 ), "scene: a hostile NPC east on a seat", "" );
    l = look_now();
    const bool npc_east = l.east == 2;
    m = act( "RIGHT" );
    l = look_now();
    // The bump is CDDA's attack: a critical hit can kill the NPC outright, and then nobody is east any more.
    check( npc_east && l.dx == 0 && l.dy == 0 && l.origin == 0 && ( l.east == 2 || ( l.east == 0 && has( m, "dies" ) ) ),
           "on: a hostile NPC on a vehicle isn't swapped", describe( l ) + " [" + m + "]" );

    // Off again: surrounded and held, the step is CDDA's bump attack and every grab holds.
    check( set_unstoppable( false ), "turned off", "" );
    check( scene( 43 ), "scene: surrounded and held again", "" );
    m = act( "RIGHT" );
    l = look_now();
    check( l.dx == 0 && l.dy == 0 && l.zombies == 8 && l.grabbed == 4 && l.grabbing >= 3,
           "off: a step into one is an attack again, the grabs hold", describe( l ) + " [" + m + "]" );
    if( !invincible_was ) {
        fp3d::request_cheat( fp3d::cheat::invincible, false );
    }

    // "Break all grabs" with the cheat off.
    check( scene( 42 ), "scene: held again", "" );
    const uint64_t id = fp3d::request_range_op( op );
    scene( 0 );
    std::string error = "(no result)";
    for( const fp3d::range_op_result &res : fp3d::latest_range_results() ) {
        if( res.id == id ) {
            error = res.error;
        }
    }
    l = look_now();
    check( id != 0 && error.empty() && l.grabbed == 0 && l.grabbing == 0 && l.dx == 0 && l.dy == 0,
           "break all grabs: done with the cheat off", describe( l ) + " [" + error + "]" );

    check( scene( 44 ), "cleared up", "" );
    fp3d::set_cheats_allowed( cheats_were );
    std::printf( "  unstoppable: %s\n", failed == 0 ? "all PASS" : "FAILED" );
    return failed == 0;
}

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
    // 250 ms turns, times slow_build, times what the real-time goal found this machine needs (rt_stretch)
    fp3d::set_timescale( 4.0 / ( fp3d::slow_build * rt_stretch ) );
    fp3d::set_clock_mode( fp3d::clock_mode::real_time );
    spawn_zombie = true;
    const int64_t turn0 = r->turn;
    const std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    // 2 s at 4x is 8 turns; a loaded machine gets up to 30 s for 3 of them (an overmap loading meanwhile took over
    // 10 s with parallel builds running). A prompt (a request that doesn't expire) would stop the clock: noted,
    // cancelled so the game goes on.
    std::string prompts;
    const auto watch = [&]( const std::chrono::steady_clock::time_point & from ) {
        while( ms_since( from ) < 2000 * fp3d::slow_build ||
               ( r->turn < turn0 + 3 && ms_since( from ) < 30000 * fp3d::slow_build ) ) {
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
    };
    watch( t );
    // Too few turns seen and nothing asked: the turns may take longer than their slot now (see rt_stretch). The slot
    // is stretched and the watch repeated.
    while( r->turn < turn0 + 3 && prompts.empty() && rt_stretch < 8 ) {
        rt_stretch *= 2;
        fp3d::set_timescale( 4.0 / ( fp3d::slow_build * rt_stretch ) );
        std::printf( "    real time: too few turns seen, the slot stretched to %d ms\n",
                     250 * fp3d::slow_build * rt_stretch );
        watch( std::chrono::steady_clock::now() );
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
    // With only three levels wanted (as the client reads them), the snapshot follows the player down and up.
    const auto levels_now = [&]() {
        const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
        if( !s ) {
            return std::string( "none" );
        }
        bool here_too = false;
        for( const auto &sm : s->submaps ) {
            here_too = here_too || ( sm->pos.z == s->player.pos.z && sm->pos.x == s->player.pos.x / fp3d::sm_size &&
                                     sm->pos.y == s->player.pos.y / fp3d::sm_size );
        }
        return std::to_string( s->z_first ) + ".." + std::to_string( s->z_last ) + ", " +
               std::to_string( s->submaps.size() ) + " submaps" + ( here_too ? ", the player's among them" : "" );
    };
    const std::string three = std::to_string( 3 * MAPSIZE * MAPSIZE ) + " submaps, the player's among them";
    fp3d::want_snapshot_levels( 1, 1 );
    m = run( stairs, -1 );
    const fp3d::ui_point below{ stairs.x, stairs.y, stairs.z - 1 };
    const bool down = same( pos_now(), below );
    check( down, "from next to them, down", str( pos_now() ) + " [" + m + "]" );
    check( down && levels_now() == std::to_string( below.z - 1 ) + ".." + std::to_string( below.z + 1 ) + ", " + three,
           "the three levels around the new one", levels_now() );
    m = down ? run( below, 1 ) : "<not below>";
    check( down && same( pos_now(), stairs ), "standing on them, up", str( pos_now() ) + " [" + m + "]" );
    check( down && levels_now() == std::to_string( stairs.z - 1 ) + ".." + std::to_string( stairs.z + 1 ) + ", " + three,
           "and around the old one again", levels_now() );
    // Back to every level, as the other checks read them.
    fp3d::want_all_snapshot_levels();
    r = fp3d::wait_request( 5000 );
    if( r && r->timed ) {
        fp3d::queue_action( "pause" );
        r = fp3d::wait_request( 20000, r->id );
    }
    const std::shared_ptr<const fp3d::snapshot> all = fp3d::latest_snapshot();
    check( r && r->timed && all && all->submaps.size() == static_cast<size_t>( MAPSIZE ) * MAPSIZE * OVERMAP_LAYERS,
           "every level again after a turn", levels_now() );
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
    fp3d::range_op long_car;
    long_car.kind = fp3d::range_op_kind::spawn_vehicle;
    long_car.vehicle = std::string( fp3d::max_vehicle_proto_id + 1, 'a' );
    check( fp3d::request_range_op( long_car ) == 0, "range ops: overlong vehicle id refused", "" );
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
        while( ms_since( t ) < 20000 * fp3d::slow_build ) {
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

// The item actions for a client that simulates lying items (fp3d/item_hands.h, 2026-11-12): `place` moves a lying
// item one square without time, `spot` stores how it rests, `hold` takes a carried item into a hand; a tag on any
// item action brings an `item_result` event. Everything the client names is checked: reach, the next square only,
// what the square takes, weight, once a turn per item, a cap per turn, a full hand never emptied.
static bool run_item_physics_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  item physics: %-44s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    struct result {
        bool queued = false;
        bool back = false;        // in the main loop again
        int spent = 0;            // moves
        int answers = 0;          // item_result events for the action's tag
        bool done = false;
        uint64_t uid = 0;         // the result's item
        std::string why;          // the result's text
        std::string msg;          // the last message
    };
    uint32_t next_tag = 100;
    const auto run = [&]( fp3d::item_action ia ) {
        result res;
        std::vector<fp3d::event> evs;
        const std::optional<fp3d::ui_request> r0 = fp3d::wait_request( 5000 );
        if( !r0 || !r0->timed ) {
            return res;
        }
        const observed o0 = snapshot();
        fp3d::drain_events( evs );
        evs.clear();
        ia.tag = ++next_tag;
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
            } else if( e.kind == fp3d::event_kind::item_result && e.detail == ia.tag ) {
                res.answers++;
                res.done = e.amount == 1;
                res.uid = e.uid;
                res.why = e.text;
            }
        }
        return res;
    };
    // One answer for the item; done or refused with exactly this reason; no time either way when `free`.
    const auto went = []( const result & r, uint64_t uid ) {
        return r.queued && r.back && r.answers == 1 && r.uid == uid && r.done && r.why.empty();
    };
    const auto refused = []( const result & r, uint64_t uid, const char *why ) {
        return r.queued && r.back && r.answers == 1 && r.uid == uid && !r.done &&
               r.why.find( why ) != std::string::npos;
    };
    const auto show = []( const result & r ) {
        return std::string( r.queued ? "" : "not queued " ) + ( r.done ? "done" : "refused" ) + " [" + r.why +
               "] msg [" + r.msg + "] " + std::to_string( r.spent ) + " moves, " + std::to_string( r.answers ) +
               " answers";
    };
    const auto type_of = []( const char *id ) {
        return fp3d::intern_id( fp3d::id_kind::itype, id );
    };
    const auto on = []( const fp3d::ui_point & p, uint64_t uid ) {
        for( const fp3d::item_handle_view &h : handles_at( p ) ) {
            if( h.uid == uid ) {
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
    // What the snapshot lists as carried: "type:flags" each, for a failed check's detail.
    const auto carried_list = []() {
        const std::vector<fp3d::carried_view> all = carried_now();
        std::string out = std::to_string( all.size() ) + " carried:";
        for( size_t i = 0; i < all.size() && i < 16; i++ ) {
            out += " " + fp3d::id_name( fp3d::id_kind::itype, all[i].type ) + ":" + std::to_string( all[i].flags );
        }
        return out;
    };
    const auto pass_turn = []() {
        const std::optional<fp3d::ui_request> r0 = fp3d::wait_request( 5000 );
        if( !r0 || !r0->timed ) {
            return false;
        }
        fp3d::queue_action( "pause" );
        const std::optional<fp3d::ui_request> r1 = fp3d::wait_request( 20000, r0->id );
        return r1 && r1->timed;
    };

    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    physics_setup.store( 1 );
    fp3d::queue_action( "pause" );
    r = fp3d::wait_request( 20000, r->id );
    const std::shared_ptr<const fp3d::snapshot> s0 = fp3d::latest_snapshot();
    if( !r || !r->timed || !s0 || physics_setup.load() != 0 ) {
        check( false, "scene", "" );
        return false;
    }
    const fp3d::ui_point here = s0->player.pos;
    const fp3d::ui_point east{ here.x + 1, here.y, here.z };
    const fp3d::ui_point east2{ here.x + 2, here.y, here.z };
    const fp3d::ui_point east3{ here.x + 3, here.y, here.z };
    const fp3d::ui_point south{ here.x, here.y + 1, here.z };
    const fp3d::ui_point wall{ here.x + 1, here.y - 1, here.z };
    const uint32_t t_rock = type_of( "rock" );
    const uint32_t t_engine = type_of( "v8_combustion" );
    const std::vector<fp3d::item_handle_view> lying = handles_at( east );
    const std::vector<fp3d::item_handle_view> heavy = handles_at( south );
    if( lying.size() != 2 || lying[0].type != t_rock || lying[1].type != t_rock || heavy.size() != 1 ||
        heavy[0].type != t_engine ) {
        check( false, "scene: two rocks east, an engine south", std::to_string( lying.size() ) + " east, " +
               std::to_string( heavy.size() ) + " south" );
        return false;
    }
    const uint64_t rock_a = lying[0].uid;
    const uint64_t rock_b = lying[1].uid;
    const auto action = [&]( fp3d::item_op op, uint64_t uid, uint32_t type, const fp3d::ui_point & pos ) {
        fp3d::item_action a;
        a.op = op;
        a.item = { uid, type };
        a.pos = pos;
        return a;
    };
    const auto with_spot = []( fp3d::item_action a, uint16_t x, uint16_t y ) {
        a.has_spot = true;
        a.spot_x = x;
        a.spot_y = y;
        return a;
    };
    const auto with_pose = []( fp3d::item_action a, uint16_t yaw, bool side ) {
        a.has_pose = true;
        a.yaw = yaw;
        a.side = side;
        return a;
    };
    const auto place = [&]( uint64_t uid, uint32_t type, const fp3d::ui_point & from, const fp3d::ui_point & to ) {
        fp3d::item_action a = action( fp3d::item_op::place, uid, type, from );
        a.to = to;
        return a;
    };

    // What the bridge refuses before anything is queued.
    {
        const fp3d::item_action ok = with_spot( action( fp3d::item_op::spot, rock_b, t_rock, east ), 1, 2 );
        fp3d::item_action no_spot = ok;
        no_spot.has_spot = false;
        fp3d::item_action pose_alone = action( fp3d::item_op::place, rock_a, t_rock, east );
        pose_alone.to = east2;
        pose_alone.has_pose = true;
        fp3d::item_action yaw_big = with_pose( ok, 360, false );
        fp3d::item_action shelf_big = ok;
        shelf_big.has_shelf = true;
        shelf_big.shelf = fp3d::max_item_shelf + 1;
        fp3d::item_action shelf_alone = action( fp3d::item_op::place, rock_a, t_rock, east );
        shelf_alone.to = east2;
        shelf_alone.has_shelf = true;
        fp3d::item_action tag_big = ok;
        tag_big.tag = fp3d::max_item_tag + 1u;
        fp3d::item_action to_far = place( rock_a, t_rock, east, east2 );
        to_far.to.x = 1 << 29;
        fp3d::item_action pack = action( fp3d::item_op::hold, rock_a, t_rock, here );
        fp3d::item_action no_op = ok;
        no_op.op = static_cast<fp3d::item_op>( 200 );   // past the last op
        check( !fp3d::queue_item_action( no_spot ) && !fp3d::queue_item_action( pose_alone ) &&
               !fp3d::queue_item_action( yaw_big ) && !fp3d::queue_item_action( tag_big ) &&
               !fp3d::queue_item_action( shelf_big ) && !fp3d::queue_item_action( shelf_alone ) &&
               !fp3d::queue_item_action( to_far ) && !fp3d::queue_item_action( pack ) &&
               !fp3d::queue_item_action( no_op ), "malformed place, spot and hold never queued", "" );
    }

    // place: one square, no time, the spot, pose and shelf stored; once a turn for an item.
    fp3d::item_action placed = with_pose( with_spot( place( rock_a, t_rock, east, east2 ), 250, 750 ), 90, true );
    placed.has_shelf = true;
    placed.shelf = 3;
    result res = run( placed );
    fp3d::item_handle_view h = on( east2, rock_a );
    check( went( res, rock_a ) && res.spent == 0 && res.msg.empty() && h.uid == rock_a && h.spot_x == 250 &&
           h.spot_y == 750 && h.yaw == 90 && h.side && h.shelf == 3 && on( east, rock_a ).uid == 0,
           "place: to the next square, no time, no message", show( res ) + ", yaw " + std::to_string( h.yaw ) +
           ", shelf " + std::to_string( h.shelf ) );
    {
        const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
        std::ostringstream json;
        fp3d::write_snapshot_json( json, *s, here.z, here.z );
        check( json.str().find( "\"poses\":[[" + std::to_string( rock_a ) + ",90,1]]" ) != std::string::npos &&
               json.str().find( "\"shelves\":[[" + std::to_string( rock_a ) + ",3]]" ) != std::string::npos,
               "place: the pose and shelf in the snapshot JSON", "" );
    }
    res = run( place( rock_a, t_rock, east2, east ) );
    check( refused( res, rock_a, "moved a moment ago" ) && res.spent == 0 && res.msg.empty() &&
           on( east2, rock_a ).uid == rock_a, "place: an item moves once a turn", show( res ) );
    // The next turn: on to a square three away (it rolled off), with no spot: the old one is forgotten. From
    // there the client's physics can't have touched it any more.
    check( pass_turn(), "a turn passes", "" );
    res = run( place( rock_a, t_rock, east2, east3 ) );
    h = on( east3, rock_a );
    check( went( res, rock_a ) && h.uid == rock_a && h.spot_x == -1 && h.yaw == -1 && !h.side && h.shelf == -1,
           "place: on without a spot, the old one forgotten", show( res ) );
    check( pass_turn(), "a turn passes", "" );
    res = run( place( rock_a, t_rock, east3, east2 ) );
    const result far_spot = run( with_spot( action( fp3d::item_op::spot, rock_a, t_rock, east3 ), 5, 5 ) );
    check( refused( res, rock_a, "out of reach" ) && refused( far_spot, rock_a, "out of reach" ) &&
           on( east3, rock_a ).uid == rock_a && on( east3, rock_a ).spot_x == -1,
           "place and spot: nothing three squares away", show( res ) + " / " + show( far_spot ) );

    // place refusals: not the next square, another level, a wall, an engine, a handle of the wrong type.
    const result skip = run( place( rock_b, t_rock, east, east3 ) );
    const result same = run( place( rock_b, t_rock, east, east ) );
    fp3d::ui_point up = east2;
    up.z += 1;
    const result level = run( place( rock_b, t_rock, east, up ) );
    check( refused( skip, rock_b, "next square" ) && refused( same, rock_b, "next square" ) &&
           refused( level, rock_b, "next square" ) && on( east, rock_b ).uid == rock_b,
           "place: only to the next square on its level", show( skip ) + " / " + show( same ) + " / " + show( level ) );
    res = run( place( rock_b, t_rock, east, wall ) );
    const fp3d::ui_point air{ here.x + 2, here.y - 1, here.z };
    const result edge = run( place( rock_b, t_rock, east, air ) );
    check( refused( res, rock_b, "can't go there" ) && refused( edge, rock_b, "can't go there" ) &&
           on( east, rock_b ).uid == rock_b && handles_at( wall ).empty() && handles_at( air ).empty(),
           "place: not into a wall, not over an edge", show( res ) + " / " + show( edge ) );
    res = run( place( heavy[0].uid, t_engine, south, fp3d::ui_point{ here.x + 1, here.y + 1, here.z } ) );
    check( refused( res, heavy[0].uid, "can't be moved" ) && on( south, heavy[0].uid ).uid == heavy[0].uid,
           "place: not what the avatar couldn't lift", show( res ) );
    res = run( place( rock_b, t_engine, east, east2 ) );
    const result gone = run( place( rock_b, t_rock, east2, east ) );
    check( refused( res, rock_b, "isn't there" ) && refused( gone, rock_b, "isn't there" ) &&
           on( east, rock_b ).uid == rock_b, "place: the item by uid, type and square", show( res ) + " / " +
           show( gone ) );

    // spot: how it rests, in place; without a pose the old pose goes.
    res = run( with_pose( with_spot( action( fp3d::item_op::spot, rock_b, t_rock, east ), 10, 20 ), 359, false ) );
    h = on( east, rock_b );
    check( went( res, rock_b ) && res.spent == 0 && res.msg.empty() && h.spot_x == 10 && h.spot_y == 20 &&
           h.yaw == 359 && !h.side, "spot: stored in place, no time", show( res ) );
    res = run( with_spot( action( fp3d::item_op::spot, rock_b, t_rock, east ), 30, 40 ) );
    h = on( east, rock_b );
    check( went( res, rock_b ) && h.spot_x == 30 && h.spot_y == 40 && h.yaw == -1,
           "spot: a new spot without a pose forgets the pose", show( res ) );

    // The cap: max_free_item_ops free actions in one turn, the next one refused; a new turn starts over.
    check( pass_turn(), "a turn passes", "" );
    int in_turn = 0;
    for( int i = 0; i < fp3d::max_free_item_ops; i++ ) {
        res = run( with_spot( action( fp3d::item_op::spot, rock_b, t_rock, east ), static_cast<uint16_t>( i ), 0 ) );
        in_turn += went( res, rock_b ) && res.spent == 0 ? 1 : 0;
    }
    res = run( with_spot( action( fp3d::item_op::spot, rock_b, t_rock, east ), 500, 500 ) );
    h = on( east, rock_b );
    check( in_turn == fp3d::max_free_item_ops && refused( res, rock_b, "Too many" ) &&
           h.spot_x == fp3d::max_free_item_ops - 1, "free actions: 16 a turn, the 17th refused",
           std::to_string( in_turn ) + " done, then " + show( res ) );
    check( pass_turn(), "a turn passes", "" );
    res = run( with_spot( action( fp3d::item_op::spot, rock_b, t_rock, east ), 500, 500 ) );
    check( went( res, rock_b ) && on( east, rock_b ).spot_x == 500, "free actions: the next turn starts over",
           show( res ) );

    // A tag on an ordinary action: done when it took its time, refused with the message's words.
    res = run( action( fp3d::item_op::take, rock_b, t_rock, east ) );
    check( went( res, rock_b ) && res.spent > 0 && carried( rock_b ).uid == rock_b, "tag: a take that went through",
           show( res ) + ", " + carried_list() );
    res = run( action( fp3d::item_op::take, rock_b, t_rock, east ) );
    check( refused( res, rock_b, "isn't there any more" ) && res.why == res.msg && res.spent == 0,
           "tag: a refused take says why, as its message", show( res ) );

    // hold: carried things into the hands; a full hand refuses, nothing is put away.
    const uint32_t t_big = type_of( "rock_large" );
    const uint32_t t_pack = type_of( "backpack_giant" );
    const uint32_t kinds[3] = { type_of( "bottle_plastic" ), type_of( "pot" ), type_of( "bottle_twoliter" ) };
    uint64_t things[3] = { 0, 0, 0 };
    uint64_t big = 0;
    uint64_t pack = 0;
    for( const fp3d::carried_view &c : carried_now() ) {
        for( size_t i = 0; i < 3; i++ ) {
            if( c.type == kinds[i] && ( c.flags & 7 ) == 0 ) {
                things[i] = c.uid;
            }
        }
        if( c.type == t_big ) {
            big = c.uid;
        } else if( c.type == t_pack && ( c.flags & fp3d::carried_worn ) != 0 ) {
            pack = c.uid;
        }
    }
    if( things[0] == 0 || things[1] == 0 || things[2] == 0 || big == 0 || pack == 0 ) {
        check( false, "hold: a bottle, a pot, a big bottle, a large rock and the backpack carried", carried_list() );
        return false;
    }
    const auto hold = [&]( uint64_t uid, uint32_t type, fp3d::item_hand hand ) {
        fp3d::item_action a = action( fp3d::item_op::hold, uid, type, fp3d::ui_point{} );
        a.hand = hand;
        return run( a );
    };
    const auto flags = [&]( uint64_t uid ) {
        return static_cast<int>( carried( uid ).flags );
    };
    using fp3d::item_hand;
    res = hold( things[0], kinds[0], item_hand::main );
    check( went( res, things[0] ) && res.spent > 0 && flags( things[0] ) == fp3d::carried_wielded,
           "hold: a carried bottle into the main hand", show( res ) );
    const result again = hold( things[0], kinds[0], item_hand::main );
    const result full = hold( things[1], kinds[1], item_hand::main );
    check( refused( again, things[0], "already holding" ) && refused( full, things[1], "hand is full" ) &&
           again.spent == 0 && full.spent == 0 && flags( things[0] ) == fp3d::carried_wielded &&
           flags( things[1] ) == 0, "hold: a full main hand refuses", show( again ) + " / " + show( full ) );
    res = hold( things[1], kinds[1], item_hand::off );
    check( went( res, things[1] ) && res.spent > 0 && flags( things[1] ) == fp3d::carried_offhand,
           "hold: the pot into the off hand", show( res ) );
    const result off_full = hold( things[2], kinds[2], item_hand::off );
    const result swap = hold( things[0], kinds[0], item_hand::off );
    const result swap2 = hold( things[1], kinds[1], item_hand::main );
    check( refused( off_full, things[2], "other hand is full" ) && refused( swap, things[0], "other hand is full" ) &&
           refused( swap2, things[1], "hand is full" ) && flags( things[0] ) == fp3d::carried_wielded &&
           flags( things[1] ) == fp3d::carried_offhand && flags( things[2] ) == 0,
           "hold: full hands refuse, nothing put away", show( off_full ) + " / " + show( swap ) + " / " + show( swap2 ) );
    const result worn = hold( pack, t_pack, item_hand::main );
    const result two = hold( big, t_big, item_hand::off );
    check( refused( worn, pack, "Take off" ) && worn.why.find( "<color" ) == std::string::npos &&
           refused( two, big, "too heavy to hold in one hand" ) &&
           ( flags( pack ) & fp3d::carried_worn ) != 0, "hold: not a worn item, not a two-handed one in one hand",
           show( worn ) + " / " + show( two ) );
    // Hand over: the off hand's pot is put down, the main hand's bottle goes to the off hand and back.
    res = run( action( fp3d::item_op::drop, things[1], kinds[1], east ) );
    check( went( res, things[1] ) && on( east, things[1] ).uid == things[1], "hold: the off hand's pot put down",
           show( res ) );
    res = hold( things[0], kinds[0], item_hand::off );
    check( went( res, things[0] ) && res.spent > 0 && flags( things[0] ) == fp3d::carried_offhand,
           "hold: from the main hand to the off hand", show( res ) + ", flags " + std::to_string( flags( things[0] ) ) );
    // Both hands for a two-handed item: what the off hand holds isn't put away for it.
    res = hold( big, t_big, item_hand::main );
    check( refused( res, big, "both hands" ) && flags( big ) == 0 && flags( things[0] ) == fp3d::carried_offhand,
           "hold: a two-handed item needs the off hand free", show( res ) );
    res = hold( things[0], kinds[0], item_hand::main );
    check( went( res, things[0] ) && res.spent > 0 && flags( things[0] ) == fp3d::carried_wielded,
           "hold: from the off hand to the main hand", show( res ) + ", flags " + std::to_string( flags( things[0] ) ) );
    res = hold( 4242, t_rock, item_hand::main );
    check( refused( res, 4242, "don't have that" ), "hold: only what is carried", show( res ) );
    // Not the water out of its bottle, in either hand.
    const uint32_t t_water = type_of( "water" );
    uint64_t water = 0;
    for( const fp3d::carried_view &c : carried_now() ) {
        water = c.type == t_water ? c.uid : water;
    }
    const result sip = hold( water, t_water, item_hand::main );
    const result sip2 = hold( water, t_water, item_hand::off );
    check( water != 0 && refused( sip, water, "can't hold" ) && refused( sip2, water, "can't hold" ) &&
           carried( water ).uid == water && sip.spent == 0, "hold: not a liquid out of its container",
           show( sip ) + " / " + show( sip2 ) + ", " + carried_list() );
    // A pot of water in the main hand stays there: it would spill on its way to a pocket.
    physics_setup.store( 2 );
    check( pass_turn() && physics_setup.load() == 0, "a pot of water in the main hand", "" );
    const uint32_t t_pot = type_of( "pot" );
    uint64_t pot = 0;
    for( const fp3d::carried_view &c : carried_now() ) {
        pot = c.type == t_pot && c.flags == fp3d::carried_wielded ? c.uid : pot;
    }
    res = hold( pot, t_pot, item_hand::off );
    check( pot != 0 && refused( res, pot, "would spill" ) && res.spent == 0 && flags( pot ) == fp3d::carried_wielded,
           "hold: an open pot of water stays in the main hand", show( res ) + ", " + carried_list() );
    return failed == 0;
}

// Weather and mood in the live snapshot (contracts/snapshot.md `env` and `player.mood`, 2026-11-12): a forced
// lightning storm and two morale entries show in the snapshot published with the request they were set up at; the
// flash itself comes as an event (counted with the others at the end).
static bool run_weather_mood_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  %-50s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "weather and mood: main loop waiting", "" );
        return false;
    }
    // A turn passes; the hook does `setup` at the main-loop request after it.
    const auto after_turn = [&]( int setup ) {
        weather_mood_setup.store( setup );
        fp3d::queue_action( "pause" );
        const uint64_t before = r->id;
        const auto t = std::chrono::steady_clock::now();
        while( ms_since( t ) < 30000 * fp3d::slow_build &&
               ( weather_mood_setup.load() != 0 || r->id == before || !r->timed ) ) {
            if( const std::optional<fp3d::ui_request> next = fp3d::wait_request( 200, r->id ) ) {
                r = next;
            }
        }
        return weather_mood_setup.load() == 0 && r->id != before && r->timed;
    };
    if( !after_turn( 1 ) ) {
        check( false, "weather and mood: set up at a main-loop request", "" );
        return false;
    }
    const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
    if( !s ) {
        check( false, "weather and mood: a snapshot", "" );
        return false;
    }
    const fp3d::environment_view &e = s->env;
    check( e.weather_id == "lightning" && e.weather_name == "Lightning Storm" && e.precip == 3 && e.precip_kind == 1 &&
           e.weather_sound == 4 && e.sight_penalty > 1.0f && e.light_modifier < 0 && e.sun_multiplier < 0.5f &&
           e.sound_attn > 0 && !e.dangerous, "weather: a lightning storm in the snapshot",
           e.weather_id + " / " + e.weather_name + ", precip " + std::to_string( e.precip ) + " kind " +
           std::to_string( e.precip_kind ) + ", sound " + std::to_string( e.weather_sound ) );
    check( e.humidity <= 100 && e.wind_at_player >= 0.0f && e.wind_speed >= 0.0f &&
           ( !e.sheltered || e.wind_at_player == 0.0f ), "weather: wind, humidity and shelter at the player",
           std::to_string( e.wind_at_player ) + " of " + std::to_string( e.wind_speed ) + " m/s, humidity " +
           std::to_string( e.humidity ) + ( e.sheltered ? ", sheltered" : ", in the open" ) );
    const fp3d::mood_view &m = s->player.mood;
    bool craving = false;
    bool music = false;
    bool ordered = m.entries.size() <= fp3d::max_mood_entries && m.total >= m.entries.size();
    for( size_t i = 0; i < m.entries.size(); i++ ) {
        craving = craving || ( m.entries[i].name == "Craving caffeine" && m.entries[i].value == -40 );
        music = music || ( m.entries[i].name == "Music" && m.entries[i].value == 15 );
        ordered = ordered && m.entries[i].value != 0 && !m.entries[i].name.empty() && m.entries[i].percent <= 100 &&
                  ( i == 0 || std::abs( m.entries[i - 1].value ) >= std::abs( m.entries[i].value ) );
    }
    check( craving && music && ordered && m.level == s->player.morale && m.band == fp3d::mood_band( m.level ) &&
           !m.face.empty(), "mood: level, face and the entries in the snapshot",
           "level " + std::to_string( m.level ) + ", band " + std::to_string( m.band ) + ", face " + m.face + ", " +
           std::to_string( m.entries.size() ) + " of " + std::to_string( m.total ) + " entries" );
    std::ostringstream json;
    fp3d::write_snapshot_json( json, *s, s->player.pos.z, s->player.pos.z );
    const std::string j = json.str();
    check( j.find( "\"weather_id\":\"lightning\",\"weather_name\":\"Lightning Storm\",\"precip\":3,"
                   "\"precip_kind\":1,\"weather_sound\":4," ) != std::string::npos &&
           j.find( "\"sheltered\":" ) != std::string::npos && j.find( "\"mood\":{\"level\":" ) != std::string::npos &&
           j.find( "{\"name\":\"Craving caffeine\",\"value\":-40,\"percent\":" ) != std::string::npos,
           "weather and mood in the snapshot JSON", "" );
    // The flash needs the open sky above ground (CDDA's rule): the event is counted at the end.
    check( s->player.pos.z >= 0, "weather: the player is above ground for the flash",
           "z " + std::to_string( s->player.pos.z ) );
    check( after_turn( 2 ), "weather and mood: taken back", "" );
    const std::shared_ptr<const fp3d::snapshot> s2 = fp3d::latest_snapshot();
    check( s2 && s2->env.weather_id != "lightning" && !s2->env.weather_id.empty(), "weather: back to the world's own",
           s2 ? s2->env.weather_id : "" );
    return failed == 0;
}

// The weather cheat (fp3d/cheats.h range_op_kind::weather, 2026-11-13): a lightning storm and a wind chosen by id show
// in the snapshot and the cheats view, and the flash comes as an event within a bounded wait (CDDA's 1 in 600 a
// turn while it storms: at most 9 waits of 20 minutes, about 1 in 60 million to see none). Unknown ids, "null", a
// wind out of range and an over-long id are refused; "" gives the weather back to the world (no range here).
static bool run_weather_cheat_checks( const std::function<size_t()> &lightning_seen )
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  weather cheat: %-35s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    const auto weather = []( const std::string & id, int wind ) {
        fp3d::range_op op;
        op.kind = fp3d::range_op_kind::weather;
        op.weather = id;
        op.wind = wind;
        return op;
    };
    check( fp3d::request_range_op( weather( "lightning", -1 ) ) == 0, "refused until cheats are allowed", "" );
    fp3d::set_cheats_allowed( true );
    check( fp3d::request_range_op( weather( std::string( fp3d::max_weather_id + 1, 'a' ), -1 ) ) == 0,
           "an over-long id never queued", "" );
    // The ops' results in order, once the hook ran them (asking wakes the main loop).
    const auto run = [&]( const std::vector<fp3d::range_op> &ops ) {
        std::vector<uint64_t> ids;
        for( const fp3d::range_op &op : ops ) {
            ids.push_back( fp3d::request_range_op( op ) );
        }
        std::vector<std::string> got;
        const auto t = std::chrono::steady_clock::now();
        while( ms_since( t ) < 20000 * fp3d::slow_build ) {
            got.clear();
            const std::vector<fp3d::range_op_result> res = fp3d::latest_range_results();
            for( const uint64_t id : ids ) {
                for( const fp3d::range_op_result &x : res ) {
                    if( id != 0 && x.id == id ) {
                        got.push_back( x.error );
                        break;
                    }
                }
            }
            if( got.size() == ids.size() ) {
                break;
            }
            if( std::optional<fp3d::ui_request> next = fp3d::wait_request( 100, r->id ) ) {
                r = next;
            }
        }
        got.resize( ids.size(), "(no result)" );
        return got;
    };
    // Turns pass until the main loop asks again; a question instead is cancelled and fails.
    const auto pass = [&]( double ms ) {
        const uint64_t before = r->id;
        fp3d::queue_action( "pause" );
        const auto t = std::chrono::steady_clock::now();
        while( ms_since( t ) < ms ) {
            if( std::optional<fp3d::ui_request> next = fp3d::wait_request( 200, r->id ) ) {
                r = next;
                if( !r->timed ) {
                    std::printf( "    question: %s %s\n", r->title.c_str(), r->text.c_str() );
                    fp3d::ui_answer a;
                    a.id = r->id;
                    a.cancel = true;
                    fp3d::answer( a );
                    return false;
                }
                if( r->id != before ) {
                    return true;
                }
            }
        }
        return false;
    };
    std::vector<std::string> got = run( { weather( "fp3d_no_such_weather", -1 ), weather( "null", -1 ),
                                          weather( "lightning", fp3d::max_cheat_wind + 1 ), weather( "lightning", -2 ),
                                          weather( "lightning", 10 )
                                        } );
    check( got[0] == "No such weather." && got[1] == "No such weather.", "unknown ids and \"null\" refused",
           got[0] + " | " + got[1] );
    check( got[2] == "No such wind." && got[3] == "No such wind.", "a wind out of range refused",
           got[2] + " | " + got[3] );
    check( got[4].empty(), "a lightning storm, wind 10 mph", got[4] );
    check( pass( 20000 ), "a turn passes", "" );
    std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
    fp3d::cheats_view v = fp3d::latest_cheats();
    check( s && s->env.weather_id == "lightning" && std::fabs( s->env.wind_speed - 10 * 0.44704f ) < 0.01f &&
           v.weather == "lightning" && v.wind == 10, "in the snapshot and the cheats view",
           ( s ? s->env.weather_id + ", " + std::to_string( s->env.wind_speed ) + " m/s" : std::string() ) +
           "; cheats " + v.weather + ", " + std::to_string( v.wind ) );
    // The flash: waits of 20 minutes (CDDA's wait activity, so turns pass quickly) until one comes.
    const size_t before = lightning_seen();
    size_t flashes = 0;
    int waits = 0;
    bool waited = true;
    while( flashes == 0 && waits < 9 && waited ) {
        waits++;
        weather_wait.store( 1 );
        waited = pass( 20000 ) && weather_wait.load() == 0 && pass( 180000 );
        flashes = lightning_seen() - before;
    }
    check( waited && flashes > 0, "a flash comes as an event", std::to_string( flashes ) + " flashes in " +
           std::to_string( waits ) + " waits of 20 minutes" );
    weather_wait.store( 2 );
    check( pass( 20000 ) && weather_wait.load() == 0, "the world's temperature again", "" );
    got = run( { weather( "", -1 ) } );
    check( got[0].empty(), "back to the world's weather", got[0] );
    check( pass( 20000 ), "a turn passes", "" );
    s = fp3d::latest_snapshot();
    v = fp3d::latest_cheats();
    check( s && s->env.weather_id != "lightning" && !s->env.weather_id.empty() && v.weather.empty() && v.wind == -1,
           "the world decides again", ( s ? s->env.weather_id : std::string() ) + "; cheats [" + v.weather + "], " +
           std::to_string( v.wind ) );
    fp3d::set_cheats_allowed( false );
    return failed == 0;
}

// The game running on by itself (fp3d::set_running_hook): during a wait of an hour, which asks nothing, snapshots
// still come, with turns from inside the wait, and no more often than the gap allows.
static bool run_running_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  running: %-41s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    // Turns pass until the main loop asks again; a question instead is cancelled and fails.
    const auto pass = [&]( double ms ) {
        const uint64_t before = r->id;
        fp3d::queue_action( "pause" );
        const auto t = std::chrono::steady_clock::now();
        while( ms_since( t ) < ms ) {
            if( std::optional<fp3d::ui_request> next = fp3d::wait_request( 200, r->id ) ) {
                r = next;
                if( !r->timed ) {
                    std::printf( "    question: %s %s\n", r->title.c_str(), r->text.c_str() );
                    fp3d::ui_answer a;
                    a.id = r->id;
                    a.cancel = true;
                    fp3d::answer( a );
                    return false;
                }
                if( r->id != before ) {
                    return true;
                }
            }
        }
        return false;
    };
    // A plain turn: a request comes, so the running hook stays out of it.
    const int quiet = running_calls.load();
    check( pass( 20000 ) && running_calls.load() == quiet, "no extra snapshot on a turn with a request", "" );
    // The wait: started by the hook at one request, over at the next.
    weather_wait.store( 3 );
    const bool started = pass( 20000 ) && weather_wait.load() == 0;
    const int64_t turn0 = r->turn;
    const int calls0 = running_calls.load();
    const int64_t us0 = running_us.load();
    const auto t0 = std::chrono::steady_clock::now();
    const bool waited = started && pass( 600000 );
    const double ms = ms_since( t0 );
    const int calls = running_calls.load() - calls0;
    const int64_t passed = r->turn - turn0;
    const int64_t last = running_turn.load();
    check( waited && passed >= 3590, "an hour passes by itself", std::to_string( passed ) + " turns in " +
           std::to_string( static_cast<int>( ms ) ) + " ms" );
    check( calls >= 1 && last > turn0 && last < r->turn, "snapshots come meanwhile, from inside the wait",
           std::to_string( calls ) + " snapshots, the last at turn " + std::to_string( last - turn0 ) + " of " +
           std::to_string( passed ) );
    check( calls <= static_cast<int>( ms / fp3d::running_hook_gap_ms ) + 1, "no more often than the gap",
           std::to_string( calls ) + " in " + std::to_string( static_cast<int>( ms ) ) + " ms, " +
           std::to_string( ( running_us.load() - us0 ) / 1000 ) + " ms of it taking them" );
    // A question asked from inside the running hook (a debug message would ask one): the client gets it and
    // answers, and the before-ask hook does not run for it, so no second snapshot is taken inside the first.
    running_nested.store( -1 );
    running_answer.store( -1 );
    weather_wait.store( 3 );
    const bool again = pass( 20000 ) && weather_wait.load() == 0;
    const uint64_t start_id = r->id;
    const int64_t turn1 = r->turn;
    const int calls1 = running_calls.load();
    running_asks.store( 1 );
    fp3d::queue_action( "pause" );
    bool asked = false;
    bool back = false;
    const auto t1 = std::chrono::steady_clock::now();
    while( again && !back && ms_since( t1 ) < 600000 ) {
        const std::optional<fp3d::ui_request> next = fp3d::wait_request( 200, r->id );
        if( !next ) {
            continue;
        }
        r = next;
        if( r->timed ) {
            back = r->id != start_id;
        } else {
            fp3d::ui_answer a;
            a.id = r->id;
            if( r->text.find( "asked inside the running hook" ) != std::string::npos ) {
                asked = true;
                a.action = "YES";
            } else {
                std::printf( "    question: %s %s\n", r->title.c_str(), r->text.c_str() );
                a.cancel = true;
            }
            fp3d::answer( a );
        }
    }
    const std::shared_ptr<const fp3d::snapshot> newest = fp3d::latest_snapshot();
    check( back && asked && running_answer.load() == 1 && running_nested.load() == 0 &&
           running_calls.load() > calls1 && r->turn - turn1 >= 3590 && newest && newest->turn == r->turn,
           "a question inside the hook: asked, no hook of its own",
           std::string( asked ? "asked" : "not asked" ) + ", answer " + std::to_string( running_answer.load() ) +
           ", before-ask hooks inside " + std::to_string( running_nested.load() ) + ", " +
           std::to_string( r->turn - turn1 ) + " turns" );
    running_asks.store( 0 );
    weather_wait.store( 2 );
    check( pass( 20000 ) && weather_wait.load() == 0, "the world's temperature again", "" );
    return failed == 0;
}

// Interrupting (fp3d::interrupt, 2026-11-15): refused while the main loop waits for input, so a later wait runs to its
// end; a wait and an auto-travel route are stopped after CDDA's popup, answered yes.
static bool run_interrupt_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  interrupt: %-39s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    // A scene set up by the hook at the next main-loop request (one turn passes).
    // stray: an interrupt while the turn that sets it up passes, with nothing running to take it.
    const auto set_up = [&]( int stage, bool stray = false ) {
        interrupt_setup.store( stage );
        fp3d::queue_action( "pause" );
        if( stray ) {
            fp3d::interrupt();
        }
        const uint64_t before = r->id;
        const auto t = std::chrono::steady_clock::now();
        while( ms_since( t ) < 20000 * fp3d::slow_build && ( interrupt_setup.load() != 0 || r->id == before || !r->timed ) ) {
            if( std::optional<fp3d::ui_request> next = fp3d::wait_request( 200, r->id ) ) {
                r = next;
            }
        }
        return interrupt_setup.load() == 0 && r->id != before && r->timed;
    };
    // Turns pass from the request set up; popups are answered `yes`. The turns until the main loop asks again, -1
    // when it doesn't.
    std::vector<std::string> popups;
    const auto run_on = [&]( bool interrupt, bool yes ) -> int64_t {
        const int64_t turn0 = r->turn;
        fp3d::queue_action( "pause" );
        if( interrupt && !fp3d::interrupt() ) {
            popups.emplace_back( "(interrupt refused)" );
        }
        const uint64_t before = r->id;
        const auto t = std::chrono::steady_clock::now();
        while( ms_since( t ) < 120000 * fp3d::slow_build ) {
            std::optional<fp3d::ui_request> next = fp3d::wait_request( 200, r->id );
            if( !next ) {
                continue;
            }
            r = next;
            if( !r->timed ) {
                popups.push_back( r->text );
                fp3d::ui_answer a;
                a.id = r->id;
                if( r->kind == fp3d::request_kind::popup && !r->actions.empty() ) {
                    a.action = yes && std::find( r->actions.begin(), r->actions.end(), "YES" ) != r->actions.end() ?
                               "YES" : r->actions.front();
                } else {
                    a.cancel = true;
                }
                fp3d::answer( a );
                continue;
            }
            if( r->id != before ) {
                return r->turn - turn0;
            }
        }
        return -1;
    };
    const auto joined = [&]() {
        std::string s;
        for( const std::string &p : popups ) {
            s += ( s.empty() ? "" : " | " ) + p.substr( 0, 60 );
        }
        return s;
    };

    // While the main loop waits, nothing runs: refused. One that nothing took (no activity that turn) is dropped
    // when the main loop next waits, so the wait started after it runs to its end.
    check( !fp3d::interrupt(), "refused while the main loop waits", "" );
    popups.clear();
    int64_t turns = set_up( 2, true ) ? run_on( false, true ) : -1;
    check( turns >= 300 && popups.empty(), "an earlier one never stops a later wait",
           std::to_string( turns ) + " turns; " + joined() );

    // A long wait, interrupted: CDDA asks, yes stops it.
    popups.clear();
    turns = set_up( 3 ) ? run_on( true, true ) : -1;
    check( turns >= 0 && turns < 21600 && popups.size() == 1 && popups[0].find( "Confirm:" ) != std::string::npos,
           "a wait stopped after CDDA's popup", std::to_string( turns ) + " turns; " + joined() );

    // Map travel, interrupted the same way on its first step: the route is gone (it is at least 10 steps).
    popups.clear();
    turns = set_up( 1 ) ? run_on( true, true ) : -1;
    interrupt_route.store( -1 );
    const bool read = set_up( 4 );
    check( turns >= 0 && turns < 8 && popups.size() == 1 && popups[0].find( "auto move" ) != std::string::npos &&
           read && interrupt_route.load() == 0, "auto-travel stopped after CDDA's popup",
           std::to_string( turns ) + " turns, route left " + std::to_string( interrupt_route.load() ) + "; " +
           joined() );
    return failed == 0;
}

// Containers and their doors (L8, 2026-11-15): a closed CONTAINER's items are listed from the next square only, a
// SEALED one's never; an action queued for one square (queue_action_at) opens or closes exactly that square, with
// CDDA's rules and words, and never the only other one CDDA would pick by itself.
static bool run_furniture_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  furniture: %-45s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    furniture_setup = 1;
    fp3d::queue_action( "pause" );
    r = fp3d::wait_request( 20000, r->id );
    const std::shared_ptr<const fp3d::snapshot> s0 = fp3d::latest_snapshot();
    if( !r || !r->timed || !s0 || furniture_setup.load() != 0 ) {
        check( false, "scene", "" );
        return false;
    }
    const fp3d::ui_point here = s0->player.pos;
    const auto at = [&]( int dx, int dy ) {
        return fp3d::ui_point{ here.x + dx, here.y + dy, here.z };
    };
    const auto has = []( const std::string & str, const char *part ) {
        return str.find( part ) != std::string::npos;
    };
    const uint32_t t_rock = fp3d::intern_id( fp3d::id_kind::itype, "rock" );
    // `action` queued for square (dx, dy) and run: its messages; `prompts` counts what it asked (cancelled).
    int prompts = 0;
    const auto act = [&]( const char *action, int dx, int dy ) {
        std::vector<fp3d::event> evs;
        fp3d::drain_events( evs );
        evs.clear();
        prompts = 0;
        if( !fp3d::queue_action_at( action, at( dx, dy ) ) ) {
            return std::string( "(refused)" );
        }
        const uint64_t before = r->id;
        const auto t = std::chrono::steady_clock::now();
        while( ms_since( t ) < 20000 * fp3d::slow_build ) {
            std::optional<fp3d::ui_request> next = fp3d::wait_request( 200, r->id );
            if( !next ) {
                continue;
            }
            r = next;
            if( !r->timed ) {
                prompts++;
                fp3d::ui_answer a;
                a.id = r->id;
                a.cancel = true;
                fp3d::answer( a );
                continue;
            }
            if( r->id != before ) {
                break;
            }
        }
        fp3d::drain_events( evs );
        std::string msgs;
        for( const fp3d::event &e : evs ) {
            if( e.kind == fp3d::event_kind::message ) {
                msgs += ( msgs.empty() ? "" : " | " ) + e.text;
            }
        }
        return msgs;
    };
    const auto rocks = [&]( int dx, int dy ) {
        size_t n = 0;
        for( const fp3d::item_handle_view &h : handles_at( at( dx, dy ) ) ) {
            n += h.type == t_rock ? 1 : 0;
        }
        return n;
    };

    check( rocks( 0, 1 ) == 1 && rocks( 0, 2 ) == 0 && rocks( 1, 0 ) == 0 && rocks( -1, 0 ) == 0 &&
           rocks( 0, -1 ) == 0 && furn_at( at( 1, 0 ) ) == "f_safe_c",
           "a dresser's items: next to it yes, 2 away no", "dressers " + std::to_string( rocks( 0, 1 ) ) + " / " +
           std::to_string( rocks( 0, 2 ) ) + ", sealed " + std::to_string( rocks( 1, 0 ) + rocks( -1, 0 ) +
                   rocks( 0, -1 ) ) + ", east " + furn_at( at( 1, 0 ) ) );

    // The safe east is the only square here that opens: CDDA would pick it for any `open` by itself.
    std::string m = act( "open", -1, 0 );
    check( furn_at( at( -1, 0 ) ) == "f_safe_l" && furn_at( at( 1, 0 ) ) == "f_safe_c" && prompts == 0 &&
           has( m, "nothing that can be opened" ), "open a locked safe: nothing opens", "[" + m + "]" );
    m = act( "open", 0, -1 );
    check( furn_at( at( 0, -1 ) ) == "f_crate_c" && furn_at( at( 1, 0 ) ) == "f_safe_c" && prompts == 0 &&
           has( m, "nothing that can be opened" ), "open a nailed crate: nothing opens", "[" + m + "]" );
    m = act( "open", 2, 0 );
    check( furn_at( at( 2, 0 ) ) == "f_safe_c" && furn_at( at( 1, 0 ) ) == "f_safe_c" && prompts == 0 &&
           has( m, "nothing that can be opened" ), "open a safe 2 squares away: nothing opens", "[" + m + "]" );

    m = act( "open", 1, 0 );
    const std::vector<fp3d::item_handle_view> inside = handles_at( at( 1, 0 ) );
    check( furn_at( at( 1, 0 ) ) == "f_safe_o" && inside.size() == 1 && inside[0].type == t_rock && prompts == 0,
           "open the safe next to you: its rock listed", "[" + m + "] " + furn_at( at( 1, 0 ) ) + ", " +
           std::to_string( inside.size() ) + " handles" );
    m = act( "close", 1, 0 );
    const bool shut = furn_at( at( 1, 0 ) ) == "f_safe_c" && handles_at( at( 1, 0 ) ).empty() && prompts == 0;
    const std::string m2 = act( "open", 1, 0 );
    const std::vector<fp3d::item_handle_view> again = handles_at( at( 1, 0 ) );
    check( shut && again.size() == 1 && inside.size() == 1 && again[0].uid == inside[0].uid,
           "close it: hidden, still inside when opened", "[" + m + "] [" + m2 + "]" );

    // The bridge refuses its own answers and squares out of range.
    check( !fp3d::queue_action_at( fp3d::save_action, at( 1, 0 ) ) &&
           !fp3d::queue_action_at( "open", fp3d::ui_point{ 1 << 30, here.y, here.z } ) &&
           !fp3d::queue_action_at( "open", fp3d::ui_point{ here.x, here.y, 1000 } ) && fp3d::queued_actions() == 0,
           "bad squares refused", "" );
    return failed == 0;
}

// Consume (lead-ui's consume op, 2026-11-15): CDDA's eat without its menu, for a carried item, the drink in a carried
// bottle or an item lying next to the player. CDDA's own prompts stay real questions (a "no" eats nothing and the
// result has no text: CDDA said why); the bridge refuses with a reason what CDDA's menu wouldn't offer.
static bool run_consume_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  consume: %-46s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    struct result {
        bool queued = false;
        bool back = false;        // in the main loop again
        int prompts = 0;          // CDDA's questions on the way
        int answers = 0;          // item_result events for the action's tag
        bool done = false;
        uint64_t uid = 0;         // the result's item
        std::string why;          // the result's text
        std::string msgs;         // the messages, " | " between them
        int spent = 0;            // moves
    };
    uint32_t next_tag = 300;
    // Consume `uid` (lying on `pos` when it isn't carried); CDDA's questions answered yes or no.
    const auto run = [&]( uint64_t uid, uint32_t type, const fp3d::ui_point & pos, bool yes ) {
        result res;
        std::vector<fp3d::event> evs;
        std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
        if( !r || !r->timed ) {
            return res;
        }
        const observed o0 = snapshot();
        fp3d::drain_events( evs );
        evs.clear();
        fp3d::item_action ia;
        ia.op = fp3d::item_op::consume;
        ia.item = fp3d::item_ref{ uid, type };
        ia.pos = pos;
        ia.tag = ++next_tag;
        res.queued = fp3d::queue_item_action( ia );
        if( !res.queued ) {
            return res;
        }
        const uint64_t before = r->id;
        const auto t = std::chrono::steady_clock::now();
        while( ms_since( t ) < 60000 * fp3d::slow_build ) {
            std::optional<fp3d::ui_request> next = fp3d::wait_request( 200, r->id );
            if( !next ) {
                continue;
            }
            r = next;
            if( r->timed ) {
                if( r->id != before ) {
                    res.back = true;
                    break;
                }
                continue;
            }
            res.prompts++;
            const auto offers = [&]( const char *act ) {
                return std::find( r->actions.begin(), r->actions.end(), act ) != r->actions.end();
            };
            fp3d::ui_answer a;
            a.id = r->id;
            if( yes && offers( "YES" ) ) {
                a.action = "YES";
            } else if( offers( "NO" ) ) {
                a.action = "NO";
            } else {
                a.cancel = true;
            }
            fp3d::answer( a );
        }
        fp3d::drain_events( evs );
        const observed o1 = snapshot();
        res.spent = static_cast<int>( o1.turn - o0.turn ) * o0.speed + o0.moves - o1.moves;
        for( const fp3d::event &e : evs ) {
            if( e.kind == fp3d::event_kind::message ) {
                res.msgs += ( res.msgs.empty() ? "" : " | " ) + e.text;
            } else if( e.kind == fp3d::event_kind::item_result && e.detail == ia.tag ) {
                res.answers++;
                res.done = e.amount == 1;
                res.uid = e.uid;
                res.why = e.text;
            }
        }
        return res;
    };
    const auto has = []( const std::string & str, const char *part ) {
        return str.find( part ) != std::string::npos;
    };
    const auto show = []( const result & r ) {
        return std::string( r.queued ? "" : "not queued " ) + ( r.done ? "done" : "not done" ) + " [" + r.why +
               "] msgs [" + r.msgs + "] " + std::to_string( r.prompts ) + " prompts, " +
               std::to_string( r.spent ) + " moves, " + std::to_string( r.answers ) + " answers";
    };
    const auto carried_has = []( uint64_t uid ) {
        for( const fp3d::carried_view &c : carried_now() ) {
            if( c.uid == uid ) {
                return true;
            }
        }
        return false;
    };
    const auto charges_of = []( uint64_t uid ) {
        for( const observed::carried_count &c : snapshot().counts ) {
            if( c.uid == uid ) {
                return c.charges;
            }
        }
        return 0;
    };
    const auto lies_at = []( const fp3d::ui_point & p, uint64_t uid ) {
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
    hand_setup = 18;
    fp3d::queue_action( "pause" );
    r = fp3d::wait_request( 20000, r->id );
    const std::shared_ptr<const fp3d::snapshot> s0 = fp3d::latest_snapshot();
    if( !r || !r->timed || !s0 || cs_apple_uid == 0 || cs_water_uid == 0 || cs_spilt_uid == 0 ||
        cs_west_apple_uid == 0 ) {
        check( false, "scene", "" );
        return false;
    }
    const fp3d::ui_point here = s0->player.pos;
    const fp3d::ui_point west{ here.x - 1, here.y, here.z };
    const fp3d::ui_point north{ here.x, here.y - 1, here.z };
    const fp3d::ui_point far_east{ here.x + 2, here.y, here.z };
    const uint32_t t_apple = fp3d::intern_id( fp3d::id_kind::itype, "apple" );
    const uint32_t t_bottle = fp3d::intern_id( fp3d::id_kind::itype, "bottle_plastic" );
    const uint32_t t_rock = fp3d::intern_id( fp3d::id_kind::itype, "rock" );
    const uint32_t t_water = fp3d::intern_id( fp3d::id_kind::itype, "water_clean" );

    result res = run( cs_apple_uid, t_apple, here, true );
    check( res.back && res.answers == 1 && res.uid == cs_apple_uid && res.done && res.why.empty() &&
           res.spent > 0 && has( res.msgs, "apple" ) && !carried_has( cs_apple_uid ), "a carried apple: eaten",
           show( res ) );
    const int water_before = charges_of( cs_water_uid );
    res = run( cs_bottle_uid, t_bottle, here, true );
    check( res.back && res.answers == 1 && res.uid == cs_bottle_uid && res.done && res.why.empty() &&
           res.spent > 0 && carried_has( cs_bottle_uid ) && charges_of( cs_water_uid ) == water_before - 1,
           "the bottle: a drink of the water in it", show( res ) + ", water " + std::to_string( water_before ) +
           " -> " + std::to_string( charges_of( cs_water_uid ) ) );
    res = run( cs_rotten_uid, t_apple, here, false );
    check( res.back && res.answers == 1 && !res.done && res.why.empty() && res.prompts >= 1 &&
           carried_has( cs_rotten_uid ), "rotten: CDDA asks, no eats nothing", show( res ) );
    res = run( cs_rock_uid, t_rock, here, true );
    check( res.back && res.answers == 1 && !res.done && has( res.why, "edible" ) && res.prompts == 0 &&
           res.spent == 0 && carried_has( cs_rock_uid ), "a rock: refused", show( res ) );
    res = run( cs_west_apple_uid, t_apple, west, true );
    check( res.back && res.answers == 1 && res.done && res.spent > 0 && !lies_at( west, cs_west_apple_uid ),
           "an apple next to you: eaten", show( res ) );
    res = run( cs_far_apple_uid, t_apple, far_east, true );
    check( res.back && res.answers == 1 && !res.done && has( res.why, "reach" ) && res.spent == 0 &&
           lies_at( far_east, cs_far_apple_uid ), "an apple 2 squares away: refused", show( res ) );
    res = run( cs_spilt_uid, t_water, north, true );
    check( res.back && res.answers == 1 && !res.done && has( res.why, "spilt" ) && res.spent == 0 &&
           lies_at( north, cs_spilt_uid ), "spilt water: refused", show( res ) );
    return failed == 0;
}

// First aid by hand: consume with the body part named takes CDDA's heal use without its body part menu, and is
// refused with a reason where that menu wouldn't let the part be picked (lead-vr, 2026-11-18).
static bool run_first_aid_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  first aid: %-46s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    struct result {
        bool queued = false;
        bool back = false;        // in the main loop again
        int prompts = 0;          // CDDA's questions on the way
        std::string asked;        // their texts
        int answers = 0;          // item_result events for the action's tag
        bool done = false;
        std::string why;          // the result's text
        std::string msgs;         // the messages, " | " between them
        int spent = 0;            // moves
    };
    struct part_state {
        int hp = -1;
        int hp_max = -1;
        int bleed = 0;
        int bandaged = 0;
    };
    uint32_t next_tag = 700;
    // Consume `uid` with `part` ("" = none named). CDDA's questions: NO when it can be said, else cancelled.
    const auto run = [&]( uint64_t uid, uint32_t type, const fp3d::ui_point & pos, const std::string & part ) {
        result res;
        std::vector<fp3d::event> evs;
        std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
        if( !r || !r->timed ) {
            return res;
        }
        const observed o0 = snapshot();
        fp3d::drain_events( evs );
        evs.clear();
        fp3d::item_action ia;
        ia.op = fp3d::item_op::consume;
        ia.item = fp3d::item_ref{ uid, type };
        ia.pos = pos;
        ia.part = part;
        ia.tag = ++next_tag;
        res.queued = fp3d::queue_item_action( ia );
        if( !res.queued ) {
            return res;
        }
        const uint64_t before = r->id;
        const auto t = std::chrono::steady_clock::now();
        while( ms_since( t ) < 60000 * fp3d::slow_build ) {
            std::optional<fp3d::ui_request> next = fp3d::wait_request( 200, r->id );
            if( !next ) {
                continue;
            }
            r = next;
            if( r->timed ) {
                if( r->id != before ) {
                    res.back = true;
                    break;
                }
                continue;
            }
            res.prompts++;
            res.asked += ( res.asked.empty() ? "" : " | " ) + ( r->text.empty() ? r->title : r->text );
            fp3d::ui_answer a;
            a.id = r->id;
            if( std::find( r->actions.begin(), r->actions.end(), "NO" ) != r->actions.end() ) {
                a.action = "NO";
            } else {
                a.cancel = true;
            }
            fp3d::answer( a );
        }
        fp3d::drain_events( evs );
        const observed o1 = snapshot();
        res.spent = static_cast<int>( o1.turn - o0.turn ) * o0.speed + o0.moves - o1.moves;
        for( const fp3d::event &e : evs ) {
            if( e.kind == fp3d::event_kind::message ) {
                res.msgs += ( res.msgs.empty() ? "" : " | " ) + e.text;
            } else if( e.kind == fp3d::event_kind::item_result && e.detail == ia.tag ) {
                res.answers++;
                res.done = e.amount == 1;
                res.why = e.text;
            }
        }
        return res;
    };
    const auto has = []( const std::string & str, const char *part ) {
        return str.find( part ) != std::string::npos;
    };
    const auto show = []( const result & r ) {
        return std::string( r.queued ? "" : "not queued " ) + ( r.done ? "done" : "not done" ) + " [" + r.why +
               "] asked [" + r.asked + "] " + std::to_string( r.prompts ) + " prompts, " +
               std::to_string( r.spent ) + " moves, " + std::to_string( r.answers ) + " answers";
    };
    const auto carried_has = []( uint64_t uid ) {
        for( const fp3d::carried_view &c : carried_now() ) {
            if( c.uid == uid ) {
                return true;
            }
        }
        return false;
    };
    // What the newest snapshot says of one body part.
    const auto state_of = []( const char *id ) {
        part_state out;
        const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
        const uint32_t want = fp3d::intern_id( fp3d::id_kind::body_part, id );
        if( !s ) {
            return out;
        }
        const fp3d::player_view &pv = s->player;
        for( size_t i = 0; i < pv.body_parts.size(); i++ ) {
            if( pv.body_parts[i] == want && i < pv.hp.size() && i < pv.hp_max.size() && i < pv.bleed.size() &&
                i < pv.bandaged.size() ) {
                out.hp = pv.hp[i];
                out.hp_max = pv.hp_max[i];
                out.bleed = pv.bleed[i];
                out.bandaged = pv.bandaged[i];
            }
        }
        return out;
    };
    const auto scene = [&]( int number ) {
        std::optional<fp3d::ui_request> at = fp3d::wait_request( 5000 );
        if( !at || !at->timed ) {
            return false;
        }
        hand_setup = number;
        fp3d::queue_action( "no_such_action" );   // dropped: the scene runs before the request asked again
        at = fp3d::wait_request( 20000, at->id );
        return at && at->timed;
    };

    const std::shared_ptr<const fp3d::snapshot> s0 = scene( 50 ) ? fp3d::latest_snapshot() : nullptr;
    const part_state arm_l0 = state_of( "arm_l" );
    const part_state arm_r0 = state_of( "arm_r" );
    const part_state leg_r0 = state_of( "leg_r" );
    const part_state torso0 = state_of( "torso" );
    if( !s0 || fa_bandage_a_uid == 0 || fa_bandage_b_uid == 0 || fa_bandage_c_uid == 0 || fa_apple_uid == 0 ||
        arm_l0.hp <= 0 || arm_l0.hp >= arm_l0.hp_max || arm_r0.bleed == 0 || arm_r0.hp != arm_r0.hp_max ||
        leg_r0.hp != 0 || torso0.hp != torso0.hp_max || torso0.bleed != 0 ) {
        check( false, "scene", "arm_l " + std::to_string( arm_l0.hp ) + "/" + std::to_string( arm_l0.hp_max ) +
               ", arm_r bleed " + std::to_string( arm_r0.bleed ) + ", leg_r " + std::to_string( leg_r0.hp ) );
        return false;
    }
    const fp3d::ui_point here = s0->player.pos;
    const uint32_t t_bandage = fp3d::intern_id( fp3d::id_kind::itype, "bandages" );
    const uint32_t t_apple = fp3d::intern_id( fp3d::id_kind::itype, "apple" );
    const uint64_t a_uid = fa_bandage_a_uid;
    const uint64_t b_uid = fa_bandage_b_uid;
    const uint64_t c_uid = fa_bandage_c_uid;

    // What the queue takes: a plain id of a bounded length, and only with consume.
    fp3d::item_action shaped;
    shaped.op = fp3d::item_op::consume;
    shaped.item = fp3d::item_ref{ a_uid, t_bandage };
    shaped.pos = here;
    bool odd_refused = true;
    for( const std::string &odd : { std::string( "arm l" ), std::string( "arm-l" ), std::string( "arm_l\"" ),
                                    std::string( fp3d::max_item_part_bytes + 1, 'a' ), std::string( "\xc3\xa4rm" )
                                  } ) {
        shaped.part = odd;
        odd_refused = odd_refused && !fp3d::queue_item_action( shaped );
    }
    shaped.part = "arm_l";
    shaped.op = fp3d::item_op::drop;
    check( odd_refused && !fp3d::queue_item_action( shaped ), "part: only a plain id, only with consume", "" );

    // A part the avatar doesn't have, and one that is no main part: refused, nothing asked.
    result res = run( a_uid, t_bandage, here, "tentacle_9" );
    const result eyes = run( a_uid, t_bandage, here, "eyes" );
    check( res.back && res.answers == 1 && !res.done && has( res.why, "no such body part" ) && res.prompts == 0 &&
           res.spent == 0 && eyes.back && !eyes.done && has( eyes.why, "no such body part" ) &&
           eyes.prompts == 0 && carried_has( a_uid ), "a part you don't have: refused", show( res ) );
    // A whole part: CDDA's menu wouldn't let it be picked; refused with its words, no menu.
    res = run( a_uid, t_bandage, here, "torso" );
    check( res.back && res.answers == 1 && !res.done && has( res.why, "expect any effect" ) && res.prompts == 0 &&
           res.spent == 0 && carried_has( a_uid ), "a whole part: refused, no menu", show( res ) );
    // A broken leg with nothing else wrong.
    res = run( a_uid, t_bandage, here, "leg_r" );
    check( res.back && res.answers == 1 && !res.done && has( res.why, "leg is broken" ) && res.prompts == 0 &&
           res.spent == 0 && carried_has( a_uid ), "a broken leg: refused, no menu", show( res ) );
    // The hurt arm: no menu, CDDA's first aid runs its time, the arm is bandaged and the bandage used up.
    res = run( a_uid, t_bandage, here, "arm_l" );
    const part_state arm_l1 = state_of( "arm_l" );
    check( res.back && res.answers == 1 && res.done && res.why.empty() && res.prompts == 0 && res.spent > 0 &&
           arm_l1.bandaged > 0 && arm_l0.bandaged == 0 && state_of( "arm_r" ).bandaged == 0 &&
           !carried_has( a_uid ), "the hurt arm: bandaged, no menu",
           show( res ) + ", bandaged " + std::to_string( arm_l1.bandaged ) );
    // The bleeding arm at full health: the bandage stops bleeding, so it may be picked.
    res = run( b_uid, t_bandage, here, "arm_r" );
    const part_state arm_r1 = state_of( "arm_r" );
    check( res.back && res.answers == 1 && res.done && res.prompts == 0 && res.spent > 0 &&
           arm_r1.bleed < arm_r0.bleed && !carried_has( b_uid ), "the bleeding arm: treated, no menu",
           show( res ) + ", bleed " + std::to_string( arm_r0.bleed ) + " -> " + std::to_string( arm_r1.bleed ) );
    // No part named: CDDA's menu comes as before (here: cancelled), also right after actions that named one.
    res = run( c_uid, t_bandage, here, "" );
    check( res.back && res.answers == 1 && !res.done && res.prompts >= 1 && has( res.asked, "Select a body part" ) &&
           carried_has( c_uid ), "no part named: CDDA's menu, cancelled", show( res ) );
    // Food ignores the part.
    res = run( fa_apple_uid, t_apple, here, "torso" );
    check( res.back && res.answers == 1 && res.done && res.spent > 0 && !carried_has( fa_apple_uid ),
           "an apple with a part named: eaten", show( res ) );
    check( scene( 51 ), "after: whole again", "" );
    return failed == 0;
}

// Driving through the bridge (lead-world, 2026-11-19): control_vehicle is let through, the step actions then drive,
// and the snapshot says so (player.driving, player.vehicle; the vehicle's pos, velocity, turn_deg).
static bool run_driving_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  driving: %-46s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    struct result {
        bool queued = false;
        bool back = false;        // in the main loop again
        int menus = 0;
        int popups = 0;
        std::string asked;        // the questions' texts
        std::string msgs;         // the messages, " | " between them
        int64_t turns = 0;
    };
    // What the newest snapshot says of the player and the vehicle it is on.
    struct seen {
        bool driving = false;
        uint64_t vehicle = 0;     // player.vehicle
        fp3d::ui_point player;
        bool car = false;         // that vehicle is listed
        bool on_part = false;     // ... and one of its part rows is on the player's square
        std::optional<fp3d::player_vehicle_view> hud;   // player.vehicle_state
        fp3d::ui_point pos;
        float velocity = 0;
        float face = 0;
        float turn = 0;
    };
    const auto look = []() {
        seen out;
        const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
        if( !s ) {
            return out;
        }
        out.driving = s->player.driving;
        out.vehicle = s->player.vehicle;
        out.hud = s->player.vehicle_state;
        out.player = s->player.pos;
        for( const fp3d::vehicle_view &v : s->vehicles ) {
            if( out.vehicle != 0 && v.uid == out.vehicle ) {
                out.car = true;
                out.pos = v.pos;
                out.velocity = v.velocity;
                out.face = v.face_deg;
                out.turn = v.turn_deg;
                for( const fp3d::vehicle_part_view &row : v.parts ) {
                    out.on_part = out.on_part || ( v.pos.x + row.dx == out.player.x && v.pos.y + row.dy == out.player.y &&
                                                   v.pos.z + row.dz == out.player.z );
                }
            }
        }
        return out;
    };
    const auto told = []( const seen & o ) {
        return std::string( o.driving ? "driving" : "not driving" ) + ", vehicle " + std::to_string( o.vehicle ) +
               ( o.car ? " at " + std::to_string( o.pos.x ) + "," + std::to_string( o.pos.y ) + " " +
                 std::to_string( o.velocity ) + " m/s face " + std::to_string( o.face ) + " turn " +
                 std::to_string( o.turn ) : std::string( " not listed" ) ) + ", player " +
               std::to_string( o.player.x ) + "," + std::to_string( o.player.y ) + ( o.on_part ? " on a part" : "" );
    };
    // One queued main-loop action. CDDA's questions on the way: a menu takes the enabled entry whose label has
    // `pick` (cancelled when none has), a popup YES.
    const auto act = [&]( const std::string & action, const std::string & pick ) {
        result res;
        std::vector<fp3d::event> evs;
        std::optional<fp3d::ui_request> at = fp3d::wait_request( 5000 );
        if( !at || !at->timed ) {
            return res;
        }
        const int64_t turn0 = at->turn;
        fp3d::drain_events( evs );
        evs.clear();
        res.queued = fp3d::queue_action( action );
        if( !res.queued ) {
            return res;
        }
        const uint64_t before = at->id;
        const auto t = std::chrono::steady_clock::now();
        while( ms_since( t ) < 60000 * fp3d::slow_build ) {
            std::optional<fp3d::ui_request> next = fp3d::wait_request( 200, at->id );
            if( !next ) {
                continue;
            }
            at = next;
            if( at->timed ) {
                if( at->id != before ) {
                    res.back = true;
                    res.turns = at->turn - turn0;
                    break;
                }
                continue;
            }
            res.asked += ( res.asked.empty() ? "" : " | " ) + ( at->text.empty() ? at->title : at->text );
            fp3d::ui_answer a;
            a.id = at->id;
            if( at->kind == fp3d::request_kind::menu ) {
                res.menus++;
                a.cancel = true;
                for( const fp3d::ui_entry &e : at->entries ) {
                    if( !pick.empty() && e.enabled && e.label.find( pick ) != std::string::npos ) {
                        a.cancel = false;
                        a.entry = e.id;
                        break;
                    }
                }
            } else {
                res.popups++;
                if( std::find( at->actions.begin(), at->actions.end(), "YES" ) != at->actions.end() ) {
                    a.action = "YES";
                } else {
                    a.cancel = true;
                }
            }
            fp3d::answer( a );
        }
        fp3d::drain_events( evs );
        for( const fp3d::event &e : evs ) {
            if( e.kind == fp3d::event_kind::message ) {
                res.msgs += ( res.msgs.empty() ? "" : " | " ) + e.text;
            }
        }
        return res;
    };
    const auto show = []( const result & r ) {
        return std::string( r.queued ? "" : "not queued " ) + ( r.back ? "" : "not back " ) + "asked [" + r.asked +
               "] msgs [" + r.msgs.substr( 0, 200 ) + "] " + std::to_string( r.menus ) + " menus, " +
               std::to_string( r.popups ) + " popups, " + std::to_string( r.turns ) + " turns";
    };
    const auto scene = [&]( int number ) {
        std::optional<fp3d::ui_request> at = fp3d::wait_request( 5000 );
        if( !at || !at->timed ) {
            return false;
        }
        hand_setup = number;
        fp3d::queue_action( "no_such_action" );   // dropped: the scene runs before the request asked again
        at = fp3d::wait_request( 20000, at->id );
        return at && at->timed;
    };

    check( fp3d::action_allowed( "DEFAULTMODE", "control_vehicle", false ) &&
           !fp3d::action_allowed( "DEFAULTMODE", "toggle_auto_travel_mode", false ) &&
           !fp3d::action_allowed( "DEFAULTMODE", "control_vehicle ", false ),
           "control_vehicle is on the main loop's list", "" );
    const bool set = scene( 53 );
    const seen parked = look();
    if( !set || parked.vehicle == 0 || !parked.car || !parked.on_part || parked.driving ) {
        check( false, "scene: boarded on a parked car's controls", told( parked ) );
        return false;
    }
    // The vehicle's numbers, parked: it is named, the player sits at the controls without holding them, nothing
    // moves, the engine is off, there is fuel in the tank and charge in the battery, and it is whole.
    const auto hud_told = []( const seen & o ) {
        if( !o.hud ) {
            return std::string( "no vehicle_state" );
        }
        const fp3d::player_vehicle_view &h = *o.hud;
        std::string out = "\"" + h.name + "\" " + ( h.in_control ? "in control" : "not in control" ) +
                          ( h.at_controls ? ", at controls" : "" ) + ", v " + std::to_string( h.velocity ) + " cruise " +
                          std::to_string( h.cruise_velocity ) + " safe " + std::to_string( h.safe_velocity ) + " max " +
                          std::to_string( h.max_velocity ) + " " + h.speed_unit + " x" + std::to_string( h.speed_scale ) +
                          ( h.engine_on ? ", engine on" : ", engine off" ) + ", " + std::to_string( h.engines.size() ) +
                          " engines";
        for( const fp3d::vehicle_engine_view &e : h.engines ) {
            out += " [state " + std::to_string( e.state ) + " hp " + std::to_string( e.hp_frac ) + " fuel " +
                   fp3d::id_name( fp3d::id_kind::itype, e.fuel ) + "]";
        }
        for( const fp3d::vehicle_fuel_view &f : h.fuels ) {
            out += " " + fp3d::id_name( fp3d::id_kind::itype, f.type ) + " " + std::to_string( f.left ) + "/" +
                   std::to_string( f.capacity );
        }
        return out + ", battery " + std::to_string( h.battery_left ) + "/" + std::to_string( h.battery_capacity ) +
               ", turn " + std::to_string( h.turn_deg ) + ( h.skidding ? ", skidding" : "" ) + ", " +
               std::to_string( h.mass_kg ) + " kg, health " + std::to_string( h.health_pct );
    };
    const auto fuel_ok = []( const fp3d::player_vehicle_view & h ) {
        bool some = false;
        for( const fp3d::vehicle_fuel_view &f : h.fuels ) {
            some = some || ( f.type != 0 && f.left > 0 && f.left <= f.capacity );
        }
        return some;
    };
    const bool parked_hud = parked.hud && parked.hud->uid == parked.vehicle && !parked.hud->name.empty() &&
                            parked.hud->at_controls && !parked.hud->in_control && parked.hud->velocity == 0 &&
                            parked.hud->cruise_velocity == 0 && parked.hud->safe_velocity > 0 &&
                            parked.hud->max_velocity >= parked.hud->safe_velocity && !parked.hud->engine_on &&
                            !parked.hud->engines.empty() && ( parked.hud->engines[0].state & fp3d::engine_running ) == 0 &&
                            parked.hud->engines[0].fuel != 0 && fuel_ok( *parked.hud ) && parked.hud->battery_capacity > 0 &&
                            parked.hud->battery_left > 0 && parked.hud->battery_left <= parked.hud->battery_capacity &&
                            parked.hud->mass_kg > 300 && parked.hud->health_pct >= 95 && !parked.hud->skidding &&
                            ( parked.hud->speed_unit == "mph" || parked.hud->speed_unit == "km/h" ||
                              parked.hud->speed_unit == "t/t" ) && parked.hud->speed_scale > 0.001f &&
                            parked.hud->speed_scale < 0.02f;
    check( parked_hud, "parked: the vehicle's numbers", hud_told( parked ) );
    // The controls: CDDA starts the engine (an activity) and the character then holds them. An engine can fail to
    // start: asked again, at most three times.
    result res;
    seen now = parked;
    for( int tries = 0; tries < 3 && !now.driving; tries++ ) {
        res = act( "control_vehicle", "" );
        now = look();
    }
    check( res.back && now.driving && now.vehicle == parked.vehicle && res.menus == 0, "control_vehicle: the controls taken",
           show( res ) + "; " + told( now ) );
    check( now.hud && now.hud->in_control && now.hud->engine_on && !now.hud->engines.empty() &&
           ( now.hud->engines[0].state & fp3d::engine_running ) != 0 && now.hud->velocity == 0,
           "the controls taken: in control, the engine runs", hud_told( now ) );
    // Both are in the JSON while they hold, and left out before.
    const auto json_has = []( const char *member ) {
        const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
        if( !s ) {
            return false;
        }
        std::ostringstream out;
        fp3d::write_snapshot_json( out, *s, s->player.pos.z, s->player.pos.z );
        return out.str().find( member ) != std::string::npos;
    };
    const std::string vehicle_member = "\"vehicle\":" + std::to_string( now.vehicle ) + ",";
    check( json_has( "\"driving\":true" ) && json_has( vehicle_member.c_str() ) &&
           json_has( "\"vehicle_state\":{\"uid\":" ) && json_has( "\"speed_unit\":\"" ) &&
           json_has( "\"engines\":[[" ) && json_has( "\"fuels\":[[" ) && json_has( "\"battery\":[" ),
           "the JSON has driving, the vehicle and its numbers", vehicle_member );
    // UP raises the speed the car is set to reach (CDDA's cruise speed, 4 mph a step) and takes no time; the car
    // speeds up as turns pass. The player rides on one of its parts: CDDA moves the vehicle's `pos` to its turning
    // point once it rolls, so the player's offset from `pos` is not a constant, the part rows are what holds.
    const seen held = now;
    int64_t up_turns = 0;
    for( int i = 0; i < 4; i++ ) {
        res = act( "UP", "" );
        up_turns += res.turns;
    }
    const seen sped = look();
    for( int i = 0; i < 6; i++ ) {
        res = act( "pause", "" );
    }
    const seen rolling = look();
    const int moved = std::abs( rolling.pos.x - held.pos.x ) + std::abs( rolling.pos.y - held.pos.y );
    check( res.back && up_turns == 0 && sped.pos.x == held.pos.x && rolling.driving && rolling.car &&
           rolling.velocity > 0.5f && moved >= 2 && rolling.on_part && rolling.vehicle == held.vehicle &&
           ( rolling.player.x != held.player.x || rolling.player.y != held.player.y ),
           "UP four times, then turns pass: the car rolls with the player",
           told( rolling ) + ", moved " + std::to_string( moved ) + ", UP took " + std::to_string( up_turns ) + " turns" );
    // The numbers follow: four steps set 16 mph to reach while it still stands, then it moves at what the
    // vehicle row says, in CDDA's units.
    check( sped.hud && sped.hud->cruise_velocity == 1600 && sped.hud->velocity == 0 && rolling.hud &&
           rolling.hud->cruise_velocity == 1600 && rolling.hud->velocity > 0 &&
           std::fabs( rolling.hud->velocity / 100.0 * 0.44704 - rolling.velocity ) < 0.01 &&
           rolling.hud->velocity <= rolling.hud->max_velocity,
           "the numbers change with acceleration", hud_told( sped ) + "; " + hud_told( rolling ) );
    // Steering: the facing it turns to changes, and back.
    res = act( "RIGHT", "" );
    const seen steered = look();
    const result back_res = act( "LEFT", "" );
    const seen straight = look();
    check( res.back && back_res.back && steered.driving && std::fabs( steered.turn - rolling.turn ) >= 1.0f &&
           std::fabs( straight.turn - rolling.turn ) < 1.0f, "RIGHT, then LEFT: it steers and steers back",
           told( steered ) + "; " + told( straight ) + "; " + show( res ) );
    check( steered.hud && std::fabs( steered.hud->turn_deg - steered.turn ) < 0.01f && straight.hud &&
           std::fabs( straight.hud->turn_deg - straight.turn ) < 0.01f, "the numbers have the steering",
           hud_told( steered ) );
    // DOWN lowers the speed it is set to reach; back at none, the car stands after some turns (at most thirty).
    for( int i = 0; i < 4; i++ ) {
        res = act( "DOWN", "" );
    }
    seen slowing = look();
    for( int i = 0; i < 30 && std::fabs( slowing.velocity ) > 0.01f; i++ ) {
        res = act( "pause", "" );
        slowing = look();
    }
    check( res.back && slowing.driving && std::fabs( slowing.velocity ) <= 0.01f,
           "DOWN four times, then turns pass: it stands", told( slowing ) );
    // The controls again: CDDA's vehicle menu, where the character lets go of them.
    res = act( "control_vehicle", "Let go of controls" );
    const seen let_go = look();
    check( res.back && res.menus >= 1 && !let_go.driving && let_go.vehicle == parked.vehicle,
           "control_vehicle while driving: the menu, let go", show( res ) + "; " + told( let_go ) );
    const bool cleared = scene( 54 );
    const seen after = look();
    check( cleared && !after.driving && after.vehicle == 0 && !after.hud && !json_has( "\"driving\"" ) &&
           !json_has( "\"vehicle\":" ) && !json_has( "\"vehicle_state\"" ),
           "after: off the car, the car gone, nothing of it in the JSON", told( after ) );
    return failed == 0;
}

// Leaning (lead-ui's lean keys, lead-vr's physical lean, 2026-11-19): CDDA's peek on a square the client names with
// queue_action_at, no direction asked; the snapshot of the peek request is the view from the peeked square and says
// where the body is; cancelling the request brings the avatar back.
static bool run_lean_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  lean: %-50s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    // What the newest snapshot says: where the avatar is, whether it peeks and from where, and whether the rock's
    // handle is listed on its square (items are only listed on squares in view).
    struct seen {
        fp3d::ui_point pos;
        bool peeking = false;
        fp3d::ui_point from;
        bool rock = false;
        bool in_json = false;     // "peeking_from" is a member of the player's JSON
    };
    fp3d::ui_point rock_sq;
    const auto look = [&]() {
        seen out;
        const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
        if( !s ) {
            return out;
        }
        out.pos = s->player.pos;
        out.peeking = s->player.peeking;
        out.from = s->player.peeking_from;
        for( const fp3d::item_handle_view &h : handles_at( rock_sq ) ) {
            out.rock = out.rock || h.uid == lean_rock_uid;
        }
        std::ostringstream js;
        fp3d::write_snapshot_json( js, *s, s->player.pos.z, s->player.pos.z );
        out.in_json = js.str().find( "\"peeking_from\":[" ) != std::string::npos;
        return out;
    };
    const auto told = []( const seen & o ) {
        return "at " + std::to_string( o.pos.x ) + "," + std::to_string( o.pos.y ) +
               ( o.peeking ? ", peeking from " + std::to_string( o.from.x ) + "," + std::to_string( o.from.y ) :
                 std::string( ", not peeking" ) ) + ( o.rock ? ", rock in view" : ", rock hidden" ) +
               ( o.in_json ? ", in the JSON" : "" );
    };
    struct result {
        bool queued = false;
        bool back = false;        // in the main loop again
        int directions = 0;       // direction requests on the way
        int targets = 0;          // target requests on the way
        std::string mode;         // the first one's mode
        seen during;              // the snapshot at the first target request
        std::string msgs;
        int64_t turns = 0;
    };
    // Peeks on `square`; the peek's request is cancelled, as a lean that ends.
    const auto peek_at = [&]( const fp3d::ui_point & square ) {
        result res;
        std::vector<fp3d::event> evs;
        std::optional<fp3d::ui_request> at = fp3d::wait_request( 5000 );
        if( !at || !at->timed ) {
            return res;
        }
        const int64_t turn0 = at->turn;
        fp3d::drain_events( evs );
        evs.clear();
        res.queued = fp3d::queue_action_at( "peek", square );
        if( !res.queued ) {
            return res;
        }
        const uint64_t before = at->id;
        const auto t = std::chrono::steady_clock::now();
        while( ms_since( t ) < 30000 * fp3d::slow_build ) {
            std::optional<fp3d::ui_request> next = fp3d::wait_request( 200, at->id );
            if( !next ) {
                continue;
            }
            at = next;
            if( at->timed ) {
                if( at->id != before ) {
                    res.back = true;
                    res.turns = at->turn - turn0;
                    break;
                }
                continue;
            }
            if( at->kind == fp3d::request_kind::direction ) {
                res.directions++;
            } else if( at->kind == fp3d::request_kind::target ) {
                if( res.targets == 0 ) {
                    res.mode = at->mode;
                    res.during = look();
                }
                res.targets++;
            }
            fp3d::ui_answer a;
            a.id = at->id;
            a.cancel = true;
            fp3d::answer( a );
        }
        fp3d::drain_events( evs );
        for( const fp3d::event &e : evs ) {
            if( e.kind == fp3d::event_kind::message ) {
                res.msgs += ( res.msgs.empty() ? "" : " | " ) + e.text;
            }
        }
        return res;
    };
    const auto show = []( const result & r ) {
        return std::string( r.queued ? "" : "not queued " ) + ( r.back ? "" : "not back " ) +
               std::to_string( r.directions ) + " direction, " + std::to_string( r.targets ) + " target [" + r.mode +
               "] msgs [" + r.msgs.substr( 0, 160 ) + "] " + std::to_string( r.turns ) + " turns";
    };
    const auto scene = [&]( int number ) {
        std::optional<fp3d::ui_request> at = fp3d::wait_request( 5000 );
        if( !at || !at->timed ) {
            return false;
        }
        hand_setup = number;
        fp3d::queue_action( "no_such_action" );   // dropped: the scene runs before the request asked again
        at = fp3d::wait_request( 20000, at->id );
        return at && at->timed;
    };

    const bool set = scene( 55 );
    const std::shared_ptr<const fp3d::snapshot> s0 = fp3d::latest_snapshot();
    if( !set || !s0 || lean_rock_uid == 0 ) {
        check( false, "scene", "" );
        return false;
    }
    const fp3d::ui_point body = s0->player.pos;
    rock_sq = fp3d::ui_point{ body.x, body.y - 3, body.z };
    const fp3d::ui_point east{ body.x + 1, body.y, body.z };
    const seen standing = look();
    check( !standing.peeking && !standing.rock && !standing.in_json && standing.pos.x == body.x,
           "standing: the rock behind the wall is not in view", told( standing ) );
    // The square east, named with the action: no direction asked, the peek's request, and its snapshot is the view
    // from there with the body's square in it. Cancelled: back, the rock out of view again, two turns gone.
    result res = peek_at( east );
    const seen after = look();
    check( res.back && res.directions == 0 && res.targets == 1 && res.mode == "peek" && res.during.pos.x == east.x &&
           res.during.pos.y == east.y && res.during.peeking && res.during.from.x == body.x &&
           res.during.from.y == body.y && res.during.from.z == body.z && res.during.rock && res.during.in_json,
           "peek east: no direction asked, the view from there", show( res ) + "; " + told( res.during ) );
    check( after.pos.x == body.x && after.pos.y == body.y && !after.peeking && !after.rock && !after.in_json &&
           res.turns == 2, "the lean ends: back, the rock hidden again, two turns", told( after ) + ", " +
           std::to_string( res.turns ) + " turns" );
    // West of the avatar the wall still hides the rock: a peek shows what CDDA lets be seen, no more.
    res = peek_at( fp3d::ui_point{ body.x - 1, body.y, body.z } );
    check( res.back && res.targets == 1 && res.during.peeking && !res.during.rock && !look().peeking,
           "peek west: the wall still hides the rock", show( res ) + "; " + told( res.during ) );
    // The client's square is checked: two squares off is no peek, the wall's square is none, one far outside is
    // not even queued. None of them costs time.
    const result far_off = peek_at( fp3d::ui_point{ body.x + 2, body.y, body.z } );
    const result wall = peek_at( fp3d::ui_point{ body.x, body.y - 1, body.z } );
    const bool outside_queued = fp3d::queue_action_at( "peek", fp3d::ui_point{ 1 << 29, body.y, body.z } ) ||
                                fp3d::queue_action_at( "peek", fp3d::ui_point{ body.x, body.y, 99 } );
    check( far_off.back && far_off.targets == 0 && far_off.directions == 0 && far_off.turns == 0 &&
           far_off.msgs.find( "Never mind" ) != std::string::npos && wall.back && wall.targets == 0 &&
           wall.directions == 0 && wall.turns == 0 && !outside_queued && !look().peeking,
           "a square not next to the player, a wall, one far outside: no peek", show( far_off ) + "; " + show( wall ) );
    return failed == 0;
}

// Vehicle cargo in the snapshot (lead-world's car interiors, 2026-11-21): the cargo spaces within the avatar's
// reach, each with its part row, its room and its items by handle.
static bool run_cargo_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  cargo: %-50s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    const auto scene = [&]( int number ) {
        std::optional<fp3d::ui_request> at = fp3d::wait_request( 5000 );
        if( !at || !at->timed ) {
            return false;
        }
        hand_setup = number;
        fp3d::queue_action( "no_such_action" );   // dropped: the scene runs before the request asked again
        at = fp3d::wait_request( 20000, at->id );
        return at && at->timed;
    };
    // The cargo rows of the newest snapshot, all vehicles together, and the row on the square (dx, dy) from the
    // player if there is one.
    struct seen {
        size_t spaces = 0;
        bool found = false;
        fp3d::vehicle_cargo_view space;
        bool in_json = false;
        int parts_there = 0;      // part rows of any vehicle on that square
        size_t vehicles = 0;
    };
    const auto look = []( int dx, int dy ) {
        seen out;
        const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
        if( !s ) {
            return out;
        }
        out.vehicles = s->vehicles.size();
        for( const fp3d::vehicle_view &v : s->vehicles ) {
            for( const fp3d::vehicle_part_view &row : v.parts ) {
                out.parts_there += v.pos.x + row.dx == s->player.pos.x + dx && v.pos.y + row.dy == s->player.pos.y + dy &&
                                   v.pos.z + row.dz == s->player.pos.z ? 1 : 0;
            }
            out.spaces += v.cargo.size();
            for( const fp3d::vehicle_cargo_view &c : v.cargo ) {
                if( c.part >= v.parts.size() ) {
                    continue;
                }
                const fp3d::vehicle_part_view &row = v.parts[c.part];
                if( v.pos.x + row.dx == s->player.pos.x + dx && v.pos.y + row.dy == s->player.pos.y + dy &&
                    v.pos.z + row.dz == s->player.pos.z ) {
                    out.found = true;
                    out.space = c;
                }
            }
        }
        std::ostringstream js;
        fp3d::write_snapshot_json( js, *s, s->player.pos.z, s->player.pos.z );
        out.in_json = js.str().find( "\"cargo\":[[" ) != std::string::npos;
        return out;
    };
    const auto told = []( const seen & o ) {
        return std::to_string( o.spaces ) + " spaces listed" + ( o.found ? ", the one there: part row " +
                std::to_string( o.space.part ) + ", " + std::to_string( o.space.used_ml ) + " of " +
                std::to_string( o.space.capacity_ml ) + " ml, " + std::to_string( o.space.items.size() ) +
                " items, " + std::to_string( o.space.more ) + " more" : std::string( ", none there" ) ) +
               ( o.in_json ? ", in the JSON" : "" ) + "; " + std::to_string( o.vehicles ) + " vehicles, " +
               std::to_string( o.parts_there ) + " part rows on that square";
    };

    if( !scene( 56 ) || cargo_items.load() < 33 || ( cargo_dx.load() == 0 && cargo_dy.load() == 0 ) ) {
        check( false, "scene: a car with bottles in its biggest cargo space, the avatar next to it",
               std::to_string( cargo_items.load() ) + " bottles, the space at " + std::to_string( cargo_dx.load() ) + "," +
               std::to_string( cargo_dy.load() ) + " from the avatar" );
        return false;
    }
    const int bottles = cargo_items.load();
    const uint32_t t_bottle = fp3d::intern_id( fp3d::id_kind::itype, "bottle_plastic" );
    const seen by = look( cargo_dx.load(), cargo_dy.load() );
    std::set<uint64_t> uids;
    bool all_bottles = true;
    for( const fp3d::cargo_item_view &it : by.space.items ) {
        all_bottles = all_bottles && it.uid != 0 && it.type == t_bottle;
        uids.insert( it.uid );
    }
    check( by.found && by.space.items.size() == fp3d::max_cargo_items && uids.size() == fp3d::max_cargo_items &&
           all_bottles && static_cast<int>( by.space.more ) == bottles - static_cast<int>( fp3d::max_cargo_items ) &&
           by.space.used_ml > 0 && by.space.used_ml <= by.space.capacity_ml && by.spaces >= 1 &&
           by.spaces <= fp3d::max_cargo_spaces && by.in_json,
           "next to the car: the space, its room, 32 bottles by handle", told( by ) + ", " + std::to_string( bottles ) +
           " bottles in it, the space at " + std::to_string( cargo_dx.load() ) + "," + std::to_string( cargo_dy.load() ) +
           " from the player" );
    // ---- by hand: take an item out of the space, put one in (item_hands.h) ----
    struct result {
        bool queued = false;
        bool back = false;
        int answers = 0;
        bool done = false;
        std::string why;
        int spent = 0;
    };
    uint32_t next_tag = 900;
    const auto act = [&]( fp3d::item_op op, uint64_t uid, uint32_t type, const fp3d::ui_point & pos, bool into_cargo,
    fp3d::item_hand hand ) {
        result res;
        std::vector<fp3d::event> evs;
        std::optional<fp3d::ui_request> at = fp3d::wait_request( 5000 );
        if( !at || !at->timed ) {
            return res;
        }
        const observed o0 = snapshot();
        fp3d::drain_events( evs );
        evs.clear();
        fp3d::item_action ia;
        ia.op = op;
        ia.item = fp3d::item_ref{ uid, type };
        ia.pos = pos;
        ia.hand = hand;
        ia.cargo = into_cargo;
        ia.tag = ++next_tag;
        res.queued = fp3d::queue_item_action( ia );
        if( !res.queued ) {
            return res;
        }
        const uint64_t before = at->id;
        const auto t = std::chrono::steady_clock::now();
        while( ms_since( t ) < 30000 * fp3d::slow_build ) {
            std::optional<fp3d::ui_request> next = fp3d::wait_request( 200, at->id );
            if( !next ) {
                continue;
            }
            at = next;
            if( at->timed ) {
                if( at->id != before ) {
                    res.back = true;
                    break;
                }
                continue;
            }
            fp3d::ui_answer a;
            a.id = at->id;
            a.cancel = true;
            fp3d::answer( a );
        }
        fp3d::drain_events( evs );
        const observed o1 = snapshot();
        res.spent = static_cast<int>( o1.turn - o0.turn ) * o0.speed + o0.moves - o1.moves;
        for( const fp3d::event &e : evs ) {
            if( e.kind == fp3d::event_kind::item_result && e.detail == ia.tag ) {
                res.answers++;
                res.done = e.amount == 1;
                res.why = e.text;
            }
        }
        return res;
    };
    const auto show = []( const result & r ) {
        return std::string( r.queued ? "" : "not queued " ) + ( r.back ? "" : "not back " ) +
               ( r.done ? "done" : "not done" ) + " [" + r.why + "] " + std::to_string( r.spent ) + " moves, " +
               std::to_string( r.answers ) + " answers";
    };
    const auto has = []( const std::string & str, const char *part ) {
        return str.find( part ) != std::string::npos;
    };
    const auto carried_has = []( uint64_t uid ) {
        for( const fp3d::carried_view &c : carried_now() ) {
            if( c.uid == uid ) {
                return true;
            }
        }
        return false;
    };
    const auto in_space = [&]( uint64_t uid ) {
        const seen now = look( cargo_dx.load(), cargo_dy.load() );
        for( const fp3d::cargo_item_view &it : now.space.items ) {
            if( now.found && it.uid == uid ) {
                return true;
            }
        }
        return false;
    };
    const bool handed = scene( 59 );
    const std::shared_ptr<const fp3d::snapshot> s2 = fp3d::latest_snapshot();
    const uint64_t a_uid = cg_bottle_a_uid;
    const uint64_t b_uid = cg_bottle_b_uid;
    const uint64_t theirs_uid = cg_theirs_uid;
    const uint64_t rock_uid = cg_rock_uid;
    const uint64_t big_uid = cg_big_uid;
    const seen loaded = look( cargo_dx.load(), cargo_dy.load() );
    if( !handed || !s2 || a_uid == 0 || b_uid == 0 || theirs_uid == 0 || rock_uid == 0 || big_uid == 0 ||
        !loaded.found || loaded.space.items.size() != 6 || !in_space( a_uid ) || !in_space( theirs_uid ) ||
        !carried_has( rock_uid ) || !carried_has( big_uid ) ) {
        check( false, "by hand: scene: six bottles in the space, a rock and a giant backpack carried", told( loaded ) );
        return false;
    }
    const fp3d::ui_point me = s2->player.pos;
    const fp3d::ui_point space_sq{ me.x + cargo_dx.load(), me.y + cargo_dy.load(), me.z };
    const uint32_t t_rock = fp3d::intern_id( fp3d::id_kind::itype, "rock" );
    const uint32_t t_big = fp3d::intern_id( fp3d::id_kind::itype, "backpack_giant" );
    // What the queue takes: `cargo` only with drop, and never with a spot.
    fp3d::item_action odd;
    odd.op = fp3d::item_op::take;
    odd.item = fp3d::item_ref{ a_uid, t_bottle };
    odd.pos = space_sq;
    odd.cargo = true;
    const bool take_with_cargo = fp3d::queue_item_action( odd );
    odd.op = fp3d::item_op::drop;
    odd.item = fp3d::item_ref{ rock_uid, t_rock };
    odd.has_spot = true;
    odd.spot_x = 500;
    odd.spot_y = 500;
    check( !take_with_cargo && !fp3d::queue_item_action( odd ), "by hand: cargo only with drop, never with a spot", "" );
    // A bottle out of the space into the pack.
    result res = act( fp3d::item_op::take, a_uid, t_bottle, space_sq, false, fp3d::item_hand::pack );
    const seen after_take = look( cargo_dx.load(), cargo_dy.load() );
    check( res.back && res.answers == 1 && res.done && res.spent > 0 && carried_has( a_uid ) && !in_space( a_uid ) &&
           after_take.space.items.size() == 5, "by hand: a bottle out of the space into the pack",
           show( res ) + "; " + told( after_take ) );
    // The same handle again: it isn't there any more.
    res = act( fp3d::item_op::take, a_uid, t_bottle, space_sq, false, fp3d::item_hand::pack );
    check( res.back && res.answers == 1 && !res.done && has( res.why, "isn't there any more" ) && res.spent == 0,
           "by hand: taken once, not there twice", show( res ) );
    // Someone else's bottle stays.
    res = act( fp3d::item_op::take, theirs_uid, t_bottle, space_sq, false, fp3d::item_hand::pack );
    check( res.back && !res.done && has( res.why, "isn't yours" ) && in_space( theirs_uid ) && !carried_has( theirs_uid ),
           "by hand: someone else's bottle is refused", show( res ) );
    // Into a hand that holds something: refused as from the ground.
    res = act( fp3d::item_op::take, b_uid, t_bottle, space_sq, false, fp3d::item_hand::main );
    check( res.back && !res.done && has( res.why, "hand is full" ) && in_space( b_uid ), "by hand: a full hand takes nothing",
           show( res ) );
    // A square two off: out of reach.
    res = act( fp3d::item_op::take, b_uid, t_bottle, fp3d::ui_point{ me.x + 2 * cargo_dx.load(), me.y + 2 * cargo_dy.load(), me.z },
               false, fp3d::item_hand::pack );
    check( res.back && !res.done && has( res.why, "reach" ) && in_space( b_uid ), "by hand: a space two squares off is out of reach",
           show( res ) );
    // The rock into the space.
    res = act( fp3d::item_op::drop, rock_uid, t_rock, space_sq, true, fp3d::item_hand::pack );
    const seen after_put = look( cargo_dx.load(), cargo_dy.load() );
    check( res.back && res.answers == 1 && res.done && res.spent > 0 && !carried_has( rock_uid ) && in_space( rock_uid ) &&
           after_put.space.items.size() == 6 && after_put.space.used_ml > after_take.space.used_ml,
           "by hand: the rock into the space", show( res ) + "; " + told( after_put ) );
    // A square in reach with no cargo space on it.
    if( cg_free.load() != 0 ) {
        res = act( fp3d::item_op::drop, a_uid, t_bottle, fp3d::ui_point{ me.x + cg_free_dx.load(), me.y + cg_free_dy.load(), me.z },
                   true, fp3d::item_hand::pack );
        check( res.back && !res.done && has( res.why, "nothing to put" ) && carried_has( a_uid ) && res.spent == 0,
               "by hand: no cargo space on that square", show( res ) );
    } else {
        std::printf( "  cargo: by hand: every square in reach has a cargo space (not checked: none without)\n" );
    }
    // The space filled up: the giant backpack in hand doesn't fit and stays in the hand.
    const bool filled = scene( 60 );
    res = act( fp3d::item_op::drop, big_uid, t_big, space_sq, true, fp3d::item_hand::pack );
    check( filled && res.back && res.answers == 1 && !res.done && has( res.why, "Unable to fit" ) && carried_has( big_uid ) &&
           res.spent == 0, "by hand: a full space takes no more", show( res ) );
    // Further off nothing of it is listed: the member is left out.
    const bool stepped = scene( 57 );
    const seen off = look( 0, 0 );
    check( stepped && off.spaces == 0 && !off.in_json, "away from the car: no cargo listed", told( off ) );
    check( scene( 58 ), "after: the car gone", "" );
    // ---- the glovebox (fp3d/glovebox.h): a car fresh from its prototype has one, and it is a cargo space ----
    const bool fresh = scene( 61 );
    std::string odd_kinds;
    for( size_t i = 0; i < glove_checked.size(); i++ ) {
        if( ( glove_kinds.load() & ( 1 << i ) ) == 0 ) {
            odd_kinds += std::string( " " ) + glove_checked[i];
        }
    }
    check( fresh && odd_kinds.empty() && glove_protos.load() >= 100,
           "glovebox: cars, trucks and vans yes; bikes, carts no",
           std::to_string( glove_protos.load() ) + " prototypes have one" +
           ( odd_kinds.empty() ? std::string() : "; not as expected:" + odd_kinds ) );
    check( fresh && glove_count.load() == 1 && glove_tile.load() == 7 && glove_bottle_uid.load() != 0,
           "glovebox: a fresh car: one, ahead of the passenger",
           std::to_string( glove_count.load() ) + " on the car, around it " + std::to_string( glove_tile.load() ) +
           " (1 windshield, 2 a seat behind, 4 beside the driver's), at " + std::to_string( glove_dx.load() ) + "," +
           std::to_string( glove_dy.load() ) + " from the driver's seat" );
    // In the snapshot: its part row by vpart id, and its cargo row from the driver's seat.
    const uint32_t v_box = fp3d::intern_id( fp3d::id_kind::vpart, "fp3d_glovebox" );
    const uint64_t in_box_uid = glove_bottle_uid;
    const auto box_now = [&]() {
        struct {
            int rows = 0;
            int cargo_rows = 0;
            int dx = 0;
            int dy = 0;
            fp3d::vehicle_cargo_view space;
            bool has_bottle = false;
        } out;
        const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
        if( !s ) {
            return out;
        }
        for( const fp3d::vehicle_view &v : s->vehicles ) {
            // The car the avatar sits in: a vehicle the map has parked nearby can have a glovebox of its own.
            bool sat_in = false;
            for( const fp3d::vehicle_part_view &row : v.parts ) {
                sat_in = sat_in || ( v.pos.x + row.dx == s->player.pos.x && v.pos.y + row.dy == s->player.pos.y );
            }
            if( !sat_in ) {
                continue;
            }
            for( const fp3d::vehicle_part_view &row : v.parts ) {
                out.rows += row.vpart == v_box ? 1 : 0;
            }
            for( const fp3d::vehicle_cargo_view &c : v.cargo ) {
                if( c.part < v.parts.size() && v.parts[c.part].vpart == v_box ) {
                    out.cargo_rows++;
                    out.space = c;
                    out.dx = v.pos.x + v.parts[c.part].dx - s->player.pos.x;
                    out.dy = v.pos.y + v.parts[c.part].dy - s->player.pos.y;
                }
            }
        }
        for( const fp3d::cargo_item_view &it : out.space.items ) {
            out.has_bottle = out.has_bottle || ( it.uid == in_box_uid && it.type == t_bottle );
        }
        return out;
    };
    const auto box_told = []( const auto & b ) {
        return std::to_string( b.rows ) + " part rows, " + std::to_string( b.cargo_rows ) + " cargo rows, at " +
               std::to_string( b.dx ) + "," + std::to_string( b.dy ) + ", " + std::to_string( b.space.used_ml ) + " of " +
               std::to_string( b.space.capacity_ml ) + " ml, " + std::to_string( b.space.items.size() ) + " items" +
               ( b.has_bottle ? ", the bottle" : "" );
    };
    const auto seated = box_now();
    check( v_box != 0 && seated.rows == 1 && seated.cargo_rows == 1 && seated.dx == glove_dx.load() &&
           seated.dy == glove_dy.load() && seated.space.capacity_ml == 5000 && seated.space.used_ml > 0 &&
           seated.space.items.size() == 1 && seated.has_bottle && seated.space.more == 0,
           "glovebox: from the driver's seat: its row, 5 L, the bottle", box_told( seated ) );
    // By hand, as any cargo space: the bottle out into the pack and back in.
    const std::shared_ptr<const fp3d::snapshot> s3 = fp3d::latest_snapshot();
    const fp3d::ui_point sat = s3 ? s3->player.pos : fp3d::ui_point{};
    const fp3d::ui_point box_sq{ sat.x + glove_dx.load(), sat.y + glove_dy.load(), sat.z };
    res = act( fp3d::item_op::take, in_box_uid, t_bottle, box_sq, false, fp3d::item_hand::pack );
    const auto emptied = box_now();
    check( res.back && res.answers == 1 && res.done && carried_has( in_box_uid ) && emptied.cargo_rows == 1 &&
           emptied.space.items.empty() && emptied.space.used_ml == 0, "glovebox: by hand: the bottle out into the pack",
           show( res ) + "; " + box_told( emptied ) );
    res = act( fp3d::item_op::drop, in_box_uid, t_bottle, box_sq, true, fp3d::item_hand::pack );
    const auto refilled = box_now();
    check( res.back && res.answers == 1 && res.done && !carried_has( in_box_uid ) && refilled.has_bottle &&
           refilled.space.items.size() == 1, "glovebox: by hand: the bottle back in",
           show( res ) + "; " + box_told( refilled ) );
    check( scene( 62 ), "glovebox: after: the car gone", "" );
    return failed == 0;
}

// C6, the vehicle-work read (fp3d/vehicle_work.h, contracts/vehicle_work.md): a car's parts and, for a selected
// part, what CDDA's vehicle screen would say about repairing, removing, mending and refilling it. The part is the
// glovebox: its requirements are this fork's own data (mechanics 1; a screwdriver to remove it, tape to repair it).
static bool run_vehicle_work_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  vehicle work: %-52s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    const auto scene = [&]( int number ) {
        std::optional<fp3d::ui_request> at = fp3d::wait_request( 5000 );
        if( !at || !at->timed ) {
            return false;
        }
        hand_setup = number;
        fp3d::queue_action( "no_such_action" );   // dropped: the scene runs before the request asked again
        at = fp3d::wait_request( 20000, at->id );
        return at && at->timed;
    };
    struct w_line {
        std::string text;
        int met = -1;
    };
    struct w_cmd {
        std::string cmd;
        bool ok = false;
        int reason = -9;
        std::string text;
        int64_t time_s = -1;
        std::vector<w_line> lines;
    };
    struct w_part {
        int row = -1;
        int mx = 0;
        int my = 0;
        int64_t vpart = 0;
        std::string name;
        std::string variant;
        int hp = -1;
        int damage = -1;
        int flags = -1;
        size_t faults = 0;
        bool holds = false;
        std::string fill_what;
        int64_t fill_now = -1;
        int64_t fill_max = -1;
        std::string fill_unit;
    };
    struct parsed {
        bool valid = false;
        bool ok = false;
        int64_t id = 0;
        std::string kind;
        std::string error;
        std::string text;            // CDDA's sentence with an error, "" none
        int64_t vehicle = 0;
        std::string name;
        bool owned = false;
        std::vector<w_part> parts;
        int parts_more = -1;
        bool has_selected = false;
        int sel_row = -1;
        std::vector<int> here;
        std::vector<w_cmd> cmds;
        bool truncated = false;
        bool has_install = false;
        int inst_reason = -9;
        std::string inst_text;
        std::vector<std::pair<int64_t, int>> types;   // the type's id-table index, its bits
        int types_more = -1;
        bool has_picked = false;
        int64_t picked_vpart = 0;
        w_cmd picked;
    };
    const auto parse = []( const std::string & text ) {
        parsed out;
        try {
            const JsonValue root = json_loader::from_string( text );
            JsonObject jo = root.get_object();
            jo.allow_omitted_members();
            out.ok = jo.get_bool( "ok" );
            out.id = jo.get_member( "id" ).get_int64();
            out.kind = jo.get_string( "kind" );
            if( !out.ok ) {
                out.error = jo.get_string( "error" );
                out.text = jo.has_member( "text" ) ? jo.get_string( "text" ) : std::string();
                out.vehicle = jo.has_member( "vehicle" ) ? jo.get_member( "vehicle" ).get_int64() : 0;
                out.valid = true;
                return out;
            }
            out.vehicle = jo.get_member( "vehicle" ).get_int64();
            out.name = jo.get_string( "name" );
            out.owned = jo.get_bool( "owned" );
            for( JsonObject p : jo.get_array( "parts" ) ) {
                p.allow_omitted_members();
                w_part part;
                part.row = p.get_int( "row" );
                JsonArray mount = p.get_array( "mount" );
                part.mx = mount.get_int( 0 );
                part.my = mount.get_int( 1 );
                part.vpart = p.get_member( "vpart" ).get_int64();
                part.name = p.get_string( "name" );
                part.variant = p.get_string( "variant" );
                part.hp = p.get_int( "hp" );
                part.damage = p.get_int( "damage" );
                part.flags = p.get_int( "flags" );
                if( p.has_array( "faults" ) ) {
                    part.faults = p.get_array( "faults" ).size();
                }
                if( p.has_object( "fill" ) ) {
                    JsonObject fill = p.get_object( "fill" );
                    fill.allow_omitted_members();
                    part.holds = true;
                    part.fill_what = fill.get_string( "what" );
                    part.fill_now = fill.get_member( "now" ).get_int64();
                    part.fill_max = fill.get_member( "max" ).get_int64();
                    part.fill_unit = fill.get_string( "unit" );
                }
                out.parts.push_back( part );
            }
            out.parts_more = jo.get_int( "parts_more" );
            out.truncated = jo.get_bool( "truncated" );
            if( jo.has_object( "selected" ) ) {
                JsonObject s = jo.get_object( "selected" );
                s.allow_omitted_members();
                out.has_selected = true;
                out.sel_row = s.get_int( "row" );
                JsonArray rows = s.get_array( "here" );
                for( size_t i = 0; i < rows.size(); i++ ) {
                    out.here.push_back( rows.get_int( i ) );
                }
                for( JsonObject c : s.get_array( "commands" ) ) {
                    c.allow_omitted_members();
                    w_cmd cmd;
                    cmd.cmd = c.get_string( "cmd" );
                    cmd.ok = c.get_bool( "ok" );
                    cmd.reason = c.get_int( "reason" );
                    cmd.text = c.get_string( "text" );
                    cmd.time_s = c.get_member( "time_s" ).get_int64();
                    for( JsonObject l : c.get_array( "lines" ) ) {
                        l.allow_omitted_members();
                        w_line line;
                        line.text = l.get_string( "text" );
                        line.met = l.get_int( "met" );
                        cmd.lines.push_back( line );
                    }
                    out.cmds.push_back( cmd );
                }
            }
            if( jo.has_object( "install" ) ) {
                JsonObject in = jo.get_object( "install" );
                in.allow_omitted_members();
                out.has_install = true;
                out.inst_reason = in.get_int( "reason" );
                out.inst_text = in.get_string( "text" );
                out.types_more = in.get_int( "types_more" );
                for( JsonArray t : in.get_array( "types" ) ) {
                    out.types.emplace_back( t.get_int( 0 ), t.get_int( 1 ) );
                }
                if( in.has_object( "selected" ) ) {
                    JsonObject c = in.get_object( "selected" );
                    c.allow_omitted_members();
                    out.has_picked = true;
                    out.picked_vpart = c.get_int( "vpart" );
                    out.picked.cmd = c.get_string( "cmd" );
                    out.picked.ok = c.get_bool( "ok" );
                    out.picked.reason = c.get_int( "reason" );
                    out.picked.text = c.get_string( "text" );
                    out.picked.time_s = c.get_member( "time_s" ).get_int64();
                    for( JsonObject l : c.get_array( "lines" ) ) {
                        l.allow_omitted_members();
                        w_line line;
                        line.text = l.get_string( "text" );
                        line.met = l.get_int( "met" );
                        out.picked.lines.push_back( line );
                    }
                }
            }
            out.valid = true;
        } catch( const std::exception & ) {
            out.valid = false;
        }
        return out;
    };
    const auto await = []( uint64_t id ) {
        const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        while( id != 0 && ms_since( t0 ) < 20000 * fp3d::slow_build ) {
            if( std::optional<std::string> res = fp3d::take_query_result( id ) ) {
                return *res;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
        }
        return std::string( "timeout" );
    };
    const auto command = []( const parsed & from, const std::string & key ) -> const w_cmd * {
        for( const w_cmd &c : from.cmds ) {
            if( c.cmd == key ) {
                return &c;
            }
        }
        return nullptr;
    };
    // Lines with this `met`; with `needle`, only those holding it.
    const auto lines_with = []( const w_cmd * c, int met, const std::string &needle = std::string() ) {
        int n = 0;
        for( const w_line &l : c ? c->lines : std::vector<w_line>() ) {
            n += l.met == met && ( needle.empty() || l.text.find( needle ) != std::string::npos ) ? 1 : 0;
        }
        return n;
    };
    const auto told = []( const w_cmd * c ) {
        if( c == nullptr ) {
            return std::string( "no such command" );
        }
        std::string out = c->cmd + ( c->ok ? " ok" : " not ok" ) + ", reason " + std::to_string( c->reason ) + ", " +
                          std::to_string( c->time_s ) + " s, \"" + c->text + "\"";
        for( const w_line &l : c->lines ) {
            out += " | " + std::to_string( l.met ) + " " + l.text.substr( 0, 60 );
        }
        return out.substr( 0, 900 );
    };
    const uint32_t v_box = fp3d::intern_id( fp3d::id_kind::vpart, "fp3d_glovebox" );
    // The scene's car in the newest snapshot: its uid, its part rows, the glovebox's row.
    struct car_seen {
        size_t named = 0;
        uint64_t uid = 0;
        std::vector<fp3d::vehicle_part_view> rows;
        size_t own_rows = 0;
        int box_row = -1;
    };
    const auto car_now = [&]() {
        car_seen out;
        const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
        if( !s ) {
            return out;
        }
        for( const fp3d::vehicle_view &v : s->vehicles ) {
            if( v.name != "fp3d work car" ) {
                continue;
            }
            out.named++;
            out.uid = v.uid;
            out.rows = v.parts;
            out.own_rows = 0;
            for( size_t i = 0; i < v.parts.size(); i++ ) {
                out.own_rows += ( v.parts[i].state & fp3d::part_gap_filler ) == 0 ? 1 : 0;
                if( v.parts[i].vpart == v_box ) {
                    out.box_row = static_cast<int>( i );
                }
            }
        }
        return out;
    };

    // The action: queued with a tag, CDDA's questions on the way answered until the main loop asks again: a menu
    // with its first choice, a yes/no question with `agree` (CDDA asks that one again when it is cancelled).
    struct outcome {
        bool queued = false;
        bool back = false;
        int results = 0;
        bool done = false;
        std::string why;
        int prompts = 0;
        std::string asked;           // what CDDA asked on the way
    };
    uint32_t next_tag = 4200;
    // How the helper answers a list of CDDA's (a menu, an item to pick): the entry whose label has `prefer` if
    // any, else the first one; with `choose` off the list is cancelled.
    bool choose = true;
    std::string prefer;
    // The part is named as a client names it: by its row, and by the type and mount the newest snapshot has for
    // that row (`as`: another row's instead, as a client with an older list would send).
    const auto work = [&]( uint64_t uid, int row, char cmd, bool agree = false,
    const fp3d::vehicle_part_view *as = nullptr ) {
        outcome out;
        std::vector<fp3d::event> evs;
        std::optional<fp3d::ui_request> at = fp3d::wait_request( 5000 );
        if( !at || !at->timed ) {
            return out;
        }
        fp3d::drain_events( evs );
        evs.clear();
        fp3d::vehicle_work_action a;
        a.vehicle = uid;
        a.part = row;
        a.vpart = v_box;
        if( const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot() ) {
            for( const fp3d::vehicle_view &v : s->vehicles ) {
                if( v.uid == uid && row >= 0 && static_cast<size_t>( row ) < v.parts.size() ) {
                    a.vpart = v.parts[row].vpart;
                    a.mount_x = v.parts[row].mount_x;
                    a.mount_y = v.parts[row].mount_y;
                }
            }
        }
        if( as != nullptr ) {
            a.vpart = as->vpart;
            a.mount_x = as->mount_x;
            a.mount_y = as->mount_y;
        }
        a.cmd = cmd;
        a.tag = ++next_tag;
        out.queued = fp3d::queue_vehicle_work_action( a );
        if( !out.queued ) {
            return out;
        }
        const uint64_t before = at->id;
        const auto t = std::chrono::steady_clock::now();
        while( ms_since( t ) < 60000 * fp3d::slow_build ) {
            std::optional<fp3d::ui_request> next = fp3d::wait_request( 200, at->id );
            if( !next ) {
                continue;
            }
            at = next;
            if( at->timed ) {
                if( at->id != before ) {
                    out.back = true;
                    break;
                }
                continue;
            }
            out.prompts++;
            out.asked += std::string( " [" ) + fp3d::kind_name( at->kind ) + ": " + at->title.substr( 0, 60 ) + " / " +
                         at->text.substr( 0, 120 ) + "]";
            fp3d::ui_answer reply;
            reply.id = at->id;
            const std::string yes_no = agree ? "YES" : "NO";
            const bool listed = at->kind == fp3d::request_kind::menu ||
                                at->kind == fp3d::request_kind::item_select;
            if( listed && choose && !at->entries.empty() ) {
                reply.entry = at->entries.front().id;
                for( const fp3d::ui_entry &e : at->entries ) {
                    if( !prefer.empty() && e.label.find( prefer ) != std::string::npos ) {
                        reply.entry = e.id;
                        break;
                    }
                }
            } else if( listed ) {
                reply.cancel = true;
            } else if( std::find( at->actions.begin(), at->actions.end(), yes_no ) != at->actions.end() ) {
                reply.action = yes_no;
            } else {
                reply.cancel = true;
            }
            fp3d::answer( reply );
        }
        fp3d::drain_events( evs );
        for( const fp3d::event &e : evs ) {
            if( e.kind == fp3d::event_kind::work_result && e.detail == a.tag ) {
                out.results++;
                out.done = e.amount == 1;
                out.why = e.text;
            }
        }
        return out;
    };
    const auto shown = []( const outcome & o ) {
        return std::string( o.queued ? "" : "not queued " ) + ( o.back ? "" : "not back " ) +
               std::to_string( o.results ) + " results, " + ( o.done ? "done" : "refused" ) + ", \"" + o.why + "\", " +
               std::to_string( o.prompts ) + " prompts" + o.asked;
    };
    // The scene's appliance in the newest snapshot.
    const auto machine_uid = []() {
        uint64_t uid = 0;
        if( const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot() ) {
            for( const fp3d::vehicle_view &v : s->vehicles ) {
                uid = v.name == "fp3d work machine" ? v.uid : uid;
            }
        }
        return uid;
    };

    // What is asking when a scene doesn't come back to the main loop; the prompt is cancelled so the rest can run.
    const auto stuck = []() {
        std::string said;
        std::optional<fp3d::ui_request> at = fp3d::wait_request( 500 );
        for( int i = 0; i < 10 && at && !at->timed; i++ ) {
            said += std::string( " [" ) + fp3d::kind_name( at->kind ) + ": " + at->title.substr( 0, 80 ) + " / " +
                    at->text.substr( 0, 200 ) + "]";
            fp3d::ui_answer cancel;
            cancel.id = at->id;
            cancel.cancel = true;
            fp3d::answer( cancel );
            at = fp3d::wait_request( 5000, at->id );
        }
        return said.empty() ? std::string( at ? " [the main loop waits]" : " [no request]" ) : said;
    };

    const bool placed = scene( 63 );
    const int env = work_env.load();
    const car_seen car = car_now();
    check( placed && env == 15 && car.named == 1 && car.uid != 0 && car.box_row >= 0 && v_box != 0,
           "scene: a car, its glovebox damaged, the avatar beside it",
           std::to_string( env ) + " of 15 (1 light, 2 morale, 4 damaged, 8 beside it), " +
           std::to_string( car.rows.size() ) + " part rows, the glovebox's " + std::to_string( car.box_row ) +
           ( placed ? std::string() : stuck() ) );
    if( !placed || car.uid == 0 || car.box_row < 0 ) {
        scene( 66 );
        return false;
    }
    work_uid = car.uid;
    work_row = car.box_row;

    check( fp3d::post_vehicle_work_query( 0, -1 ) == 0 && fp3d::post_vehicle_work_query( car.uid, -2 ) == 0 &&
           fp3d::post_vehicle_work_query( car.uid, fp3d::max_work_part_row + 1 ) == 0 &&
           fp3d::post_vehicle_work_query( fp3d::max_item_uid + 1, -1 ) == 0,
           "refused unposted: no uid, a part row out of range", "" );

    // The parts alone.
    const uint64_t id_all = fp3d::post_vehicle_work_query( car.uid, -1 );
    const std::string all_text = await( id_all );
    const parsed all = parse( all_text );
    size_t same = 0;
    const w_part *box = nullptr;
    for( const w_part &p : all.parts ) {
        if( p.row >= 0 && static_cast<size_t>( p.row ) < car.rows.size() ) {
            const fp3d::vehicle_part_view &row = car.rows[p.row];
            same += row.mount_x == p.mx && row.mount_y == p.my && row.vpart == static_cast<uint32_t>( p.vpart ) ? 1 : 0;
        }
        if( p.row == car.box_row ) {
            box = &p;
        }
    }
    check( id_all != 0 && all.valid && all.ok && all.id == static_cast<int64_t>( id_all ) &&
           all.kind == "vehicle_work" && all.vehicle == static_cast<int64_t>( car.uid ) &&
           all.name == "fp3d work car" && all.owned && !all.parts.empty() && all.parts.size() == car.own_rows &&
           same == car.own_rows && all.parts_more == 0 && !all.has_selected && !all.truncated,
           "the parts: every part row of the snapshot, none selected",
           std::to_string( all.parts.size() ) + " parts for " + std::to_string( car.own_rows ) + " rows, " +
           std::to_string( same ) + " with the row's mount and type, " + std::to_string( all_text.size() ) + " bytes" +
           ( all.valid ? "" : ": " + all_text.substr( 0, 200 ) ) );
    check( box != nullptr && box->name == "glovebox" && box->hp > 0 && box->hp < 100 && box->damage > 0 &&
           ( box->flags & 3 ) == 2 && box->faults == 0,
           "the damaged glovebox: name, health, damage, repairable",
           box ? box->name + ", hp " + std::to_string( box->hp ) + ", damage " + std::to_string( box->damage ) +
           ", flags " + std::to_string( box->flags ) + ", variant \"" + box->variant + "\"" : "not listed" );

    // What its tanks and batteries hold, as CDDA's overview shows it: a car fresh from its prototype has fuel in
    // its tank and a battery; the glovebox holds nothing of the kind.
    int tanks = 0;
    int stores = 0;
    int odd = 0;
    std::string holding;
    for( const w_part &p : all.parts ) {
        if( !p.holds ) {
            continue;
        }
        const bool tank = p.fill_unit == "ml";
        tanks += tank && p.fill_max > 0 && p.fill_now > 0 && p.fill_now <= p.fill_max && !p.fill_what.empty() ? 1 : 0;
        stores += p.fill_unit == "charges" && p.fill_max > 0 && p.fill_now >= 0 && p.fill_now <= p.fill_max ? 1 : 0;
        odd += ( !tank && p.fill_unit != "charges" ) || p.fill_now < 0 || p.fill_max < 0 ||
               ( p.fill_now == 0 ) != p.fill_what.empty() ? 1 : 0;
        holding += " [" + p.name + ": " + p.fill_what + " " + std::to_string( p.fill_now ) + " of " +
                std::to_string( p.fill_max ) + " " + p.fill_unit + "]";
    }
    check( tanks >= 1 && stores >= 1 && odd == 0 && box != nullptr && !box->holds,
           "fill: fuel in a tank in ml, a battery in charges", holding.substr( 0, 500 ) );

    // The glovebox selected, with no mechanics skill.
    const parsed bare = parse( await( fp3d::post_vehicle_work_query( car.uid, car.box_row ) ) );
    const bool order = bare.cmds.size() == 4 && bare.cmds[0].cmd == "r" && bare.cmds[1].cmd == "o" &&
                       bare.cmds[2].cmd == "m" && bare.cmds[3].cmd == "f";
    check( bare.valid && bare.ok && bare.has_selected && bare.sel_row == car.box_row && order &&
           bare.here.size() >= 2 && std::count( bare.here.begin(), bare.here.end(), car.box_row ) == 1 &&
           bare.parts.size() == all.parts.size(),
           "selected: the rows on its square, and r, o, m, f",
           std::to_string( bare.here.size() ) + " rows there, " + std::to_string( bare.cmds.size() ) + " commands" );
    const w_cmd *r0 = command( bare, "r" );
    const w_cmd *o0 = command( bare, "o" );
    const w_cmd *m0 = command( bare, "m" );
    const w_cmd *f0 = command( bare, "f" );
    check( r0 && !r0->ok && r0->reason == 0 && r0->time_s > 0 && lines_with( r0, 2, "mechanics" ) == 1 &&
           lines_with( r0, 0, "Skills required:" ) == 1 && lines_with( r0, 0, "Time required:" ) == 1,
           "repair without the skill: not ok, the skill line not met", told( r0 ) );
    check( o0 && !o0->ok && o0->reason == 0 && o0->time_s > 0 && o0->time_s <= 300 &&
           lines_with( o0, 2, "mechanics" ) == 1 && lines_with( o0, 0, "yield" ) == 1,
           "remove without the skill: not ok, what it yields", told( o0 ) );
    check( m0 && !m0->ok && m0->reason == 1 && m0->lines.empty() && f0 && !f0->ok && f0->reason == 1 &&
           f0->lines.empty(), "mend and refill: nothing here to mend or fill", told( m0 ) + " ; " + told( f0 ) );

    // The action without the skill: refused in CDDA's words, nothing started.
    const auto bad = []( uint64_t uid, int row, char cmd, uint32_t tag, uint32_t type = 1, int mx = 0,
    int my = 0 ) {
        fp3d::vehicle_work_action a;
        a.vehicle = uid;
        a.part = row;
        a.vpart = type;
        a.mount_x = mx;
        a.mount_y = my;
        a.cmd = cmd;
        a.tag = tag;
        return !fp3d::queue_vehicle_work_action( a );
    };
    check( bad( 0, car.box_row, 'r', 0 ) && bad( car.uid, -1, 'r', 0 ) && bad( car.uid, car.box_row, 'i', 0 ) &&
           bad( car.uid, fp3d::max_work_part_row + 1, 'o', 0 ) && bad( fp3d::max_item_uid + 1, 0, 'o', 0 ) &&
           bad( car.uid, car.box_row, 'r', fp3d::max_item_tag + 1 ) && bad( car.uid, car.box_row, 'r', 0, 0 ) &&
           bad( car.uid, car.box_row, 'r', 0, fp3d::max_work_vpart + 1 ) &&
           bad( car.uid, car.box_row, 'r', 0, 1, fp3d::max_work_mount + 1, 0 ) &&
           bad( car.uid, car.box_row, 'r', 0, 1, 0, -fp3d::max_work_mount - 1 ) &&
           !fp3d::queue_action( fp3d::vehicle_work_action_name ),
           "action refused unqueued: bad uid, row, type, mount, command or tag", "" );
    const outcome unskilled = work( car.uid, car.box_row, 'r' );
    check( unskilled.queued && unskilled.back && unskilled.results == 1 && !unskilled.done &&
           unskilled.why == "You don't meet the requirements to repair the glovebox.",
           "action, repair without the skill: refused in CDDA's words", shown( unskilled ) );
    // An appliance: CDDA has another screen for those.
    const bool machined = scene( 67 );
    const std::string machine_scene = std::to_string( work_machine_state.load() ) +
                                      " of 7 (1 a part type, 2 placed, 4 found)" + ( machined ? std::string() : stuck() );
    const uint64_t machine = machine_uid();
    const parsed fixed = parse( await( fp3d::post_vehicle_work_query( machine, -1 ) ) );
    const outcome on_machine = work( machine, 0, 'o' );
    check( machine != 0 && fixed.valid && !fixed.ok && fixed.error == "an appliance" && on_machine.results == 1 &&
           !on_machine.done && on_machine.why == "That can't be worked on like a vehicle.",
           "an appliance: neither read nor worked on this way",
           machine_scene + "; " + fixed.error + " / " + shown( on_machine ) );

    // Skilled, with a screwdriver and duct tape.
    const bool equipped = scene( 64 );
    const parsed able = parse( await( fp3d::post_vehicle_work_query( car.uid, car.box_row ) ) );
    const w_cmd *r1 = command( able, "r" );
    const w_cmd *o1 = command( able, "o" );
    check( equipped && r1 && r1->ok && r1->reason == 0 && lines_with( r1, 2 ) == 0 &&
           lines_with( r1, 1, "mechanics" ) == 1 && lines_with( r1, 1 ) >= 3 && r1->time_s > 0,
           "repair with the skill and tape: ok, every line met", told( r1 ) );
    check( o1 && o1->ok && o1->reason == 0 && lines_with( o1, 2 ) == 0 && lines_with( o1, 1, "mechanics" ) == 1 &&
           lines_with( o1, 1 ) >= 3 && o1->time_s > 0 && o0 && o1->time_s <= o0->time_s,
           "remove with the skill and a screwdriver: ok, every line met", told( o1 ) );
    // No line is folded: the off-screen screen is given a wide message window.
    size_t longest = 0;
    for( const w_cmd &c : able.cmds ) {
        for( const w_line &l : c.lines ) {
            longest = std::max( longest, l.text.size() );
        }
    }
    check( longest > 40 && lines_with( r1, 0, "Description" ) == 1 && lines_with( r1, 0, "compartment" ) == 1,
           "lines: one requirement a line, the description last", std::to_string( longest ) + " bytes the longest" );

    // The same within 2500 bytes, read on the sim thread in scene 64.
    const parsed tiny = parse( work_tiny );
    check( all_text.size() > 2500 && tiny.valid && tiny.ok && tiny.truncated && work_tiny.size() <= 2500 &&
           tiny.parts.size() < car.own_rows &&
           tiny.parts.size() + static_cast<size_t>( tiny.parts_more ) == car.own_rows && tiny.has_selected &&
           tiny.cmds.size() == 4, "in 2500 bytes: part entries go from the end",
           std::to_string( work_tiny.size() ) + " bytes, " + std::to_string( tiny.parts.size() ) + " parts, " +
           std::to_string( tiny.parts_more ) + " more; the query took " + std::to_string( work_ms.load() ) +
           " ms" );

    // Errors.
    const parsed gone = parse( await( fp3d::post_vehicle_work_query( car.uid + 100000, -1 ) ) );
    const parsed no_row = parse( await( fp3d::post_vehicle_work_query( car.uid, 4000 ) ) );
    check( gone.valid && !gone.ok && gone.kind == "vehicle_work" && gone.error == "no such vehicle" &&
           gone.vehicle == static_cast<int64_t>( car.uid + 100000 ) && no_row.valid && !no_row.ok &&
           no_row.error == "no such part" && no_row.vehicle == static_cast<int64_t>( car.uid ),
           "an unknown uid, a row it doesn't have: errors", gone.error + " / " + no_row.error );

    // One at a time: posted during the wait menu, a second one is refused; the first runs back at the main loop.
    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        scene( 66 );
        return false;
    }
    fp3d::ui_answer wait;
    wait.id = r->id;
    wait.action = "wait";
    fp3d::answer( wait );
    std::optional<fp3d::ui_request> prompt = fp3d::wait_request( 5000, r->id );
    uint64_t first = 0;
    uint64_t second = 1;
    if( prompt && !prompt->timed ) {
        first = fp3d::post_vehicle_work_query( car.uid, -1 );
        second = fp3d::post_vehicle_work_query( car.uid, car.box_row );
    }
    check( prompt && !prompt->timed && first != 0 && second == 0, "a second pending one refused",
           prompt ? std::string( fp3d::kind_name( prompt->kind ) ) + " prompt" : "no prompt" );
    for( int i = 0; i < 10 && prompt && !prompt->timed; i++ ) {
        fp3d::ui_answer cancel;
        cancel.id = prompt->id;
        cancel.cancel = true;
        fp3d::answer( cancel );
        prompt = fp3d::wait_request( 5000, prompt->id );
    }
    const parsed held = parse( await( first ) );
    check( prompt && prompt->timed && held.valid && held.ok && held.id == static_cast<int64_t>( first ),
           "held during a prompt, then answered", std::to_string( first ) );

    // Reads posted without a break, each the moment the last one starts to run: the main loop still takes its
    // turns (here: five actions it drops).
    {
        std::atomic<bool> stop{ false };
        std::atomic<int> posted{ 0 };
        std::vector<uint64_t> left;
        std::thread flood( [&]() {
            while( !stop.load() ) {
                const uint64_t id = fp3d::post_vehicle_work_query( car.uid, car.box_row );
                if( id != 0 ) {
                    posted++;
                    left.push_back( id );
                }
                for( size_t i = 0; i < left.size(); ) {
                    if( fp3d::take_query_result( left[i] ) ) {
                        left.erase( left.begin() + i );
                    } else {
                        i++;
                    }
                }
                std::this_thread::yield();
            }
        } );
        const std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
        while( posted.load() < 5 && ms_since( began ) < 5000 * fp3d::slow_build ) {
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
        const std::chrono::steady_clock::time_point asked = std::chrono::steady_clock::now();
        std::optional<fp3d::ui_request> at = fp3d::wait_request( 5000 );
        int turns = 0;
        for( int i = 0; i < 5 && at && at->timed; i++ ) {
            fp3d::queue_action( "no_such_action" );
            const std::optional<fp3d::ui_request> next = fp3d::wait_request( static_cast<int>( 4000 * fp3d::slow_build ),
                    at->id );
            if( !next || !next->timed ) {
                break;
            }
            at = next;
            turns++;
        }
        const int took = static_cast<int>( ms_since( asked ) );
        const int during = posted.load();
        stop = true;
        flood.join();
        // What the reads left: taken, or given up on (a result can have made room for a newer one).
        const std::chrono::steady_clock::time_point tidy = std::chrono::steady_clock::now();
        while( ( !left.empty() || fp3d::pending_queries() != 0 ) && ms_since( tidy ) < 3000 * fp3d::slow_build ) {
            for( size_t i = 0; i < left.size(); ) {
                if( fp3d::take_query_result( left[i] ) ) {
                    left.erase( left.begin() + i );
                } else {
                    i++;
                }
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
        }
        check( turns == 5 && during >= 5 && fp3d::pending_queries() == 0,
               "reads posted without a break: the main loop takes its turns",
               std::to_string( turns ) + " of 5 turns in " + std::to_string( took ) + " ms, " +
               std::to_string( during ) + " reads posted by then" );
    }

    // Results wait by kind: a character result not taken yet is still there after more vehicle reads than the
    // whole store holds, and of those reads the newest four stay.
    {
        const uint64_t sheet = fp3d::post_character_query();
        std::vector<uint64_t> reads;
        const std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
        while( reads.size() < 18 && ms_since( began ) < 20000 * fp3d::slow_build ) {
            const uint64_t id = fp3d::post_vehicle_work_query( car.uid, -1 );
            if( id != 0 ) {
                reads.push_back( id );
            } else {
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
            }
        }
        const bool all = reads.size() == 18;
        const parsed newest = parse( all ? await( reads.back() ) : std::string() );
        const bool old_gone = all && !fp3d::take_query_result( reads[0] ) && !fp3d::take_query_result( reads[13] );
        int kept = 0;
        for( size_t i = 14; all && i < 17; i++ ) {
            kept += fp3d::take_query_result( reads[i] ) ? 1 : 0;
        }
        const std::optional<std::string> sheet_text = fp3d::take_query_result( sheet );
        check( sheet != 0 && all && newest.valid && newest.ok && old_gone && kept == 3 && sheet_text &&
               !sheet_text->empty(),
               "results wait by kind: 18 reads later the character's is there",
               std::to_string( reads.size() ) + " reads, the newest " + ( newest.ok ? "ok" : "not ok" ) + ", " +
               std::to_string( kept ) + " of 3 older ones kept, the oldest " + ( old_gone ? "gone" : "still there" ) +
               ", the character's " + ( sheet_text ? std::to_string( sheet_text->size() ) + " bytes" : "gone" ) );
    }

    // A part CDDA rolls a yield for (a rotor, smashed off): the read lists it and leaves the game's dice alone.
    const bool rolled = scene( 69 );
    check( rolled && work_gate_state.load() == 15, "a part whose yield CDDA rolls: the dice as before",
           std::to_string( work_gate_state.load() ) +
           " of 15 (1 a rotor put on, 2 its yield listed, 4 the dice as before, 8 the rotor off again)" +
           ( rolled ? std::string() : stuck() ) );

    // A cable to another vehicle: listing what removing its part yields, CDDA touches the battery at the cable's
    // far end; the read leaves it as it was.
    const bool cabled = scene( 74 );
    check( cabled && work_gate_state.load() == 31, "a cable's part read: its far end as before",
           std::to_string( work_gate_state.load() ) +
           " of 31 (1 a second battery, 2 cabled, 4 the yield listed, 8 the far end's mark kept, 16 all gone again)" +
           ( cabled ? std::string() : stuck() ) );

    // CDDA's own gates on the way to its vehicle screen: not from the saddle, not from inside a shell. The avatar
    // could repair the glovebox by now, so only the gate keeps the action from starting.
    const bool riding = scene( 70 );
    const int ride_state = work_gate_state.load();
    const parsed from_saddle = parse( await( fp3d::post_vehicle_work_query( car.uid, car.box_row ) ) );
    const outcome saddle_work = work( car.uid, car.box_row, 'r' );
    const bool afoot = scene( 71 ) && work_gate_state.load() == 1;
    check( riding && ride_state == 3 && from_saddle.valid && !from_saddle.ok && from_saddle.error == "mounted" &&
           from_saddle.vehicle == static_cast<int64_t>( car.uid ) &&
           from_saddle.text == "You cannot interact with a vehicle while mounted." && saddle_work.results == 1 &&
           !saddle_work.done && saddle_work.prompts == 0 && saddle_work.why == from_saddle.text && afoot,
           "riding: neither read nor worked on, in CDDA's words",
           std::to_string( ride_state ) + " of 3 (1 a horse, 2 mounted); " + from_saddle.error + " / " +
           shown( saddle_work ) + ( riding && afoot ? std::string() : stuck() ) );
    const bool tucked = scene( 72 );
    const int shell_state = work_gate_state.load();
    const parsed from_shell = parse( await( fp3d::post_vehicle_work_query( car.uid, car.box_row ) ) );
    const outcome shell_work = work( car.uid, car.box_row, 'r' );
    const bool out_again = scene( 73 ) && work_gate_state.load() == 1;
    const parsed afterwards = parse( await( fp3d::post_vehicle_work_query( car.uid, car.box_row ) ) );
    check( tucked && shell_state == 3 && from_shell.valid && !from_shell.ok && from_shell.error == "in shell" &&
           from_shell.text == "You can't examine your surroundings while you're in your shell." &&
           shell_work.results == 1 && !shell_work.done && shell_work.prompts == 0 &&
           shell_work.why == from_shell.text && out_again && afterwards.valid && afterwards.ok,
           "in a shell: neither read nor worked on; read again once out",
           std::to_string( shell_state ) + " of 3 (1 a shell, 2 inside it); " + from_shell.error + " / " +
           shown( shell_work ) + ( tucked && out_again ? std::string() : stuck() ) );

    // A row from an older list: the type and mount sent with it are another part's. Refused before CDDA is asked
    // anything, and nothing is touched, though the avatar could do both by now.
    const int other_row = car.box_row == 0 ? 1 : 0;
    const bool two = static_cast<size_t>( other_row ) < car.rows.size();
    const outcome moved = two ? work( car.uid, other_row, 'o', false, &car.rows[car.box_row] ) : outcome();
    const outcome swapped = two ? work( car.uid, car.box_row, 'r', false, &car.rows[other_row] ) : outcome();
    const car_seen untouched = car_now();
    check( moved.queued && moved.back && moved.results == 1 && !moved.done && moved.prompts == 0 &&
           moved.why == "That part isn't there any more." && swapped.results == 1 && !swapped.done &&
           swapped.why == moved.why && untouched.own_rows == car.own_rows && untouched.box_row == car.box_row,
           "action, a row with another part's type and mount: refused", shown( moved ) + " / " + shown( swapped ) );

    // The action: repair, then remove. CDDA's own activity does the work.
    const outcome mended = work( car.uid, car.box_row, 'r' );
    const parsed after_r = parse( await( fp3d::post_vehicle_work_query( car.uid, car.box_row ) ) );
    const w_part *box2 = nullptr;
    for( const w_part &p : after_r.parts ) {
        if( p.row == car.box_row ) {
            box2 = &p;
        }
    }
    const w_cmd *r2 = command( after_r, "r" );
    check( mended.queued && mended.back && mended.results == 1 && mended.done && box && box2 != nullptr &&
           box2->hp > box->hp && box2->damage < box->damage && ( box2->flags & 3 ) == 0 && r2 && !r2->ok &&
           r2->reason == 1,
           "action, repair: started, and CDDA's activity mends it",
           shown( mended ) + "; then " + ( box2 ? "hp " + std::to_string( box2->hp ) + ", damage " +
                                           std::to_string( box2->damage ) + ", flags " + std::to_string( box2->flags ) :
                                           std::string( "not listed" ) ) + "; " + told( r2 ) );
    const outcome removed = work( car.uid, car.box_row, 'o' );
    const car_seen stripped = car_now();
    const parsed after_o = parse( await( fp3d::post_vehicle_work_query( car.uid, car.box_row ) ) );
    check( removed.queued && removed.back && removed.results == 1 && removed.done && stripped.box_row == -1 &&
           stripped.own_rows + 1 == car.own_rows && after_o.valid && !after_o.ok && after_o.error == "no such part",
           "action, remove: started, and the glovebox comes off",
           shown( removed ) + "; then " + std::to_string( stripped.own_rows ) + " part rows of " +
           std::to_string( car.own_rows ) + ", the glovebox's " + std::to_string( stripped.box_row ) + ", " +
           after_o.error );
    // A client that hasn't read again still has the glovebox at its old row: whatever has that row now stays on.
    const outcome late = work( car.uid, car.box_row, 'o', false, &car.rows[car.box_row] );
    const car_seen kept = car_now();
    check( late.queued && late.back && late.results == 1 && !late.done && late.prompts == 0 &&
           ( late.why == "That part isn't there any more." || late.why == "That vehicle has no such part." ) &&
           kept.own_rows == stripped.own_rows,
           "action, the glovebox's old row after it came off: refused", shown( late ) + "; " +
           std::to_string( kept.own_rows ) + " part rows" );

    // Install. The glovebox lies where its removal put it; the square it came off is the one to put it back on.
    const fp3d::vehicle_part_view was = car.rows[car.box_row];
    const auto ask_install = [&]( int mx, int my, uint32_t type ) {
        fp3d::vehicle_work_ask ask;
        ask.vehicle = car.uid;
        ask.install = true;
        ask.mount_x = mx;
        ask.mount_y = my;
        ask.vpart = type;
        return ask;
    };
    {
        fp3d::vehicle_work_ask rowed = ask_install( was.mount_x, was.mount_y, v_box );
        rowed.part = 0;
        fp3d::vehicle_work_ask loose;
        loose.vehicle = car.uid;
        loose.vpart = v_box;
        check( fp3d::post_vehicle_work_query( rowed ) == 0 && fp3d::post_vehicle_work_query( loose ) == 0 &&
               fp3d::post_vehicle_work_query( ask_install( fp3d::max_work_mount + 1, 0, 0 ) ) == 0 &&
               fp3d::post_vehicle_work_query( ask_install( 0, 0, fp3d::max_work_vpart + 1 ) ) == 0,
               "install read refused unposted: a row with it, a type alone, out of range", "" );
    }
    const auto bits_of = [&]( const parsed & from, uint32_t type ) {
        int bits = -1;
        for( const std::pair<int64_t, int> &t : from.types ) {
            bits = t.first == static_cast<int64_t>( type ) ? t.second : bits;
        }
        return bits;
    };
    const parsed fit = parse( await( fp3d::post_vehicle_work_query( ask_install( was.mount_x, was.mount_y,
                              v_box ) ) ) );
    bool ordered = true;
    bool lacking = false;
    size_t at_hand = 0;
    for( const std::pair<int64_t, int> &t : fit.types ) {
        const bool has = ( t.second & 1 ) != 0;
        ordered = ordered && !( has && lacking );
        lacking = lacking || !has;
        at_hand += has ? 1 : 0;
    }
    check( fit.valid && fit.ok && fit.has_install && !fit.has_selected && fit.inst_reason == 0 &&
           fit.inst_text.empty() && fit.types.size() > 50 && fit.types_more == 0 && bits_of( fit, v_box ) == 3 &&
           ordered && at_hand >= 1 && at_hand < fit.types.size() && fit.parts.size() == stripped.own_rows,
           "install list, the glovebox's old square: every type, those at hand first",
           std::to_string( fit.types.size() ) + " types, " + std::to_string( at_hand ) + " at hand, the glovebox's bits " +
           std::to_string( bits_of( fit, v_box ) ) + ", " + std::to_string( fit.parts.size() ) + " parts" +
           ( fit.valid ? "" : ": " + fit.error ) );
    check( fit.has_picked && fit.picked.cmd == "i" && fit.picked_vpart == static_cast<int64_t>( v_box ) &&
           fit.picked.ok && fit.picked.reason == 0 && fit.picked.time_s > 0 && lines_with( &fit.picked, 2 ) == 0 &&
           lines_with( &fit.picked, 1, "mechanics" ) == 1 && lines_with( &fit.picked, 1 ) >= 3,
           "install, the glovebox picked: ok, every line met", told( &fit.picked ) );
    // The id tables say which item each part type is made from (what a client goes by that offers to install the
    // item in the player's hand): every listed type names one, the glovebox type the glovebox.
    const std::shared_ptr<const fp3d::id_tables> tables_now = fp3d::latest_id_tables();
    const uint32_t glove_item = fp3d::intern_id( fp3d::id_kind::itype, "fp3d_glovebox" );
    size_t with_item = 0;
    for( const std::pair<int64_t, int> &t : fit.types ) {
        const size_t type = static_cast<size_t>( t.first );
        with_item += tables_now && type < tables_now->vparts.size() && tables_now->vparts[type].item > 0 &&
                     tables_now->vparts[type].item < tables_now->items.size() ? 1 : 0;
    }
    check( tables_now && glove_item != 0 && glove_item < tables_now->items.size() &&
           v_box < tables_now->vparts.size() && tables_now->vparts[v_box].item == glove_item &&
           tables_now->items[glove_item].id == "fp3d_glovebox" && with_item == fit.types.size() &&
           !fit.types.empty(),
           "id tables: each listed type names its item; the glovebox's is the glovebox",
           std::to_string( with_item ) + " of " + std::to_string( fit.types.size() ) + " types, the glovebox item " +
           std::to_string( glove_item ) );
    // The square beside the car's outermost part: CDDA lists the same types, and says why this one can't go there.
    fp3d::vehicle_part_view rim = was;
    for( const fp3d::vehicle_part_view &row : stripped.rows ) {
        if( ( row.state & fp3d::part_gap_filler ) == 0 && row.mount_y >= rim.mount_y ) {
            rim.mount_x = row.mount_x;
            rim.mount_y = row.mount_y;
        }
    }
    rim.mount_y += 1;
    const parsed aside = parse( await( fp3d::post_vehicle_work_query( ask_install( rim.mount_x, rim.mount_y,
                                v_box ) ) ) );
    check( aside.valid && aside.ok && aside.has_install && aside.has_picked && !aside.picked.ok &&
           bits_of( aside, v_box ) == 1 && lines_with( &aside.picked, 0, "Cannot install due to" ) == 1 &&
           lines_with( &aside.picked, 2 ) >= 1,
           "install beside the car: listed, doesn't fit, CDDA says why",
           "bits " + std::to_string( bits_of( aside, v_box ) ) + ", reason " + std::to_string( aside.inst_reason ) +
           " " + aside.inst_text + "; " + told( &aside.picked ) );
    const parsed strange = parse( await( fp3d::post_vehicle_work_query( ask_install( was.mount_x, was.mount_y,
                              fp3d::max_work_vpart ) ) ) );
    check( strange.valid && strange.ok && strange.has_picked && !strange.picked.ok && strange.picked.reason == 1 &&
           strange.picked.text == "That can't be installed here." && strange.picked.lines.empty(),
           "install, a type CDDA doesn't list: not ok", told( &strange.picked ) );

    // The action: a row with it isn't queued; where it doesn't fit and a type CDDA doesn't list are refused.
    const fp3d::vehicle_part_view nowhere = rim;
    fp3d::vehicle_part_view unknown = was;
    unknown.vpart = fp3d::max_work_vpart;
    const outcome misfit = work( car.uid, -1, 'i', false, &nowhere );
    const outcome unlisted = work( car.uid, -1, 'i', false, &unknown );
    const car_seen unfitted = car_now();
    check( bad( car.uid, 0, 'i', 0, v_box, was.mount_x, was.mount_y ) && misfit.queued && misfit.results == 1 &&
           !misfit.done && misfit.prompts == 0 &&
           misfit.why == "You don't meet the requirements to install the glovebox." && unlisted.results == 1 &&
           !unlisted.done && unlisted.why == "That can't be installed here." && unfitted.own_rows == stripped.own_rows,
           "action, install: a row with it, a misfit, an unlisted type refused", shown( misfit ) + " / " +
           shown( unlisted ) );
    // Forty squares off is no square of the car's nor next to one: CDDA's screen isn't sent looking there, by
    // the read or by the action.
    fp3d::vehicle_part_view remote_square = was;
    remote_square.mount_x = 0;
    remote_square.mount_y = 40;
    const parsed remote_read = parse( await( fp3d::post_vehicle_work_query( ask_install( 0, 40, v_box ) ) ) );
    const outcome remote_work = work( car.uid, -1, 'i', false, &remote_square );
    check( remote_read.valid && remote_read.ok && remote_read.has_install && remote_read.inst_reason == 1 &&
           remote_read.inst_text == "Cannot install any part here." && remote_read.types.empty() &&
           remote_read.types_more == 0 && remote_read.has_picked && !remote_read.picked.ok &&
           remote_read.picked.reason == 1 && remote_read.picked.lines.empty() && remote_work.results == 1 &&
           !remote_work.done && remote_work.prompts == 0 && remote_work.why == remote_read.inst_text,
           "install far from the car: nothing listed, the action refused",
           std::to_string( remote_read.types.size() ) + " types, \"" + remote_read.inst_text + "\" / " +
           shown( remote_work ) );
    // CDDA's own activity puts the glovebox back on its square.
    const outcome fitted = work( car.uid, -1, 'i', false, &was );
    const car_seen refitted = car_now();
    const bool home = refitted.box_row >= 0 && refitted.rows[refitted.box_row].mount_x == was.mount_x &&
                      refitted.rows[refitted.box_row].mount_y == was.mount_y;
    check( fitted.queued && fitted.back && fitted.results == 1 && fitted.done && home &&
           refitted.own_rows == stripped.own_rows + 1,
           "action, install: started, and the glovebox is back on its square",
           shown( fitted ) + "; then " + std::to_string( refitted.own_rows ) + " part rows of " +
           std::to_string( stripped.own_rows ) + ", the glovebox's " + std::to_string( refitted.box_row ) );

    // Mend and refill. An engine with an expired air filter, the tank a third down; a filter and a can of gasoline
    // in the pack.
    const bool worn = scene( 75 );
    const int upkeep_state = work_gate_state.load();
    const parsed upkeep = parse( await( fp3d::post_vehicle_work_query( car.uid, -1 ) ) );
    int tank_row = -1;
    int engine_row = -1;
    int64_t tank_had = -1;
    for( const w_part &p : upkeep.parts ) {
        if( tank_row < 0 && p.holds && p.fill_unit == "ml" && p.fill_now > 0 && p.fill_now < p.fill_max &&
            ( p.flags & 8 ) != 0 ) {
            tank_row = p.row;
            tank_had = p.fill_now;
        }
        if( engine_row < 0 && ( p.flags & 4 ) != 0 && p.faults >= 1 ) {
            engine_row = p.row;
        }
    }
    const parsed faulty = parse( await( fp3d::post_vehicle_work_query( car.uid, engine_row ) ) );
    const parsed thirsty = parse( await( fp3d::post_vehicle_work_query( car.uid, tank_row ) ) );
    const w_cmd *m1 = command( faulty, "m" );
    const w_cmd *f1 = command( thirsty, "f" );
    check( worn && upkeep_state == 7 && tank_row >= 0 && engine_row >= 0 && m1 && m1->ok && f1 && f1->ok,
           "scene: a faulty engine, a tank with room; the read says mend and refill",
           std::to_string( upkeep_state ) + " of 7 (1 the tank drained, 2 a fault set, 4 a can filled), tank row " +
           std::to_string( tank_row ) + ", engine row " + std::to_string( engine_row ) + "; " + told( m1 ) + " ; " +
           told( f1 ) + ( worn ? std::string() : stuck() ) );
    // A part with nothing to mend or fill: refused in words, CDDA asks nothing.
    const outcome no_fault = work( car.uid, refitted.box_row, 'm' );
    const outcome no_tank = work( car.uid, refitted.box_row, 'f' );
    check( bad( car.uid, -1, 'm', 0 ) && bad( car.uid, -1, 'f', 0 ) && no_fault.results == 1 && !no_fault.done &&
           no_fault.prompts == 0 && no_fault.why == "The glovebox doesn't have any faults to mend." &&
           no_tank.results == 1 && !no_tank.done && no_tank.prompts == 0 &&
           no_tank.why == "The glovebox can't be refilled.",
           "action, mend and refill on the glovebox: refused", shown( no_fault ) + " / " + shown( no_tank ) );
    // Mend: CDDA's menu of fixes. Backing out starts nothing; the fix picked, CDDA's activity mends it.
    choose = false;
    const outcome mend_left = work( car.uid, engine_row, 'm' );
    choose = true;
    prefer = "Replace expired air filter";
    const outcome mend_done = work( car.uid, engine_row, 'm' );
    prefer.clear();
    const parsed sound = parse( await( fp3d::post_vehicle_work_query( car.uid, -1 ) ) );
    int still_faulty = -1;
    for( const w_part &p : sound.parts ) {
        still_faulty = p.row == engine_row ? ( p.flags & 4 ) : still_faulty;
    }
    check( mend_left.results == 1 && !mend_left.done && mend_left.why.empty() && mend_left.prompts == 1 &&
           mend_left.asked.find( "Mend which fault?" ) != std::string::npos && mend_done.results == 1 &&
           mend_done.done && mend_done.asked.find( "Mend which fault?" ) != std::string::npos && still_faulty == 0,
           "action, mend: CDDA's menu; backed out nothing, picked it mends",
           shown( mend_left ) + " / " + shown( mend_done ) + "; faulty after: " + std::to_string( still_faulty ) );
    // Refill: CDDA asks what from. Nothing picked starts nothing; the can picked, the tank is fuller.
    choose = false;
    const outcome fill_left = work( car.uid, tank_row, 'f' );
    choose = true;
    const outcome fill_done = work( car.uid, tank_row, 'f' );
    const parsed fuller = parse( await( fp3d::post_vehicle_work_query( car.uid, -1 ) ) );
    int64_t tank_has = -1;
    for( const w_part &p : fuller.parts ) {
        tank_has = p.row == tank_row ? p.fill_now : tank_has;
    }
    check( fill_left.results == 1 && !fill_left.done && fill_left.why.empty() && fill_left.prompts >= 1 &&
           fill_left.asked.find( "Refill" ) != std::string::npos && fill_done.results == 1 && fill_done.done &&
           fill_done.asked.find( "item_select" ) != std::string::npos && tank_has > tank_had,
           "action, refill: CDDA asks what from; none nothing, the can fills it",
           shown( fill_left ) + " / " + shown( fill_done ) + "; " + std::to_string( tank_had ) + " -> " +
           std::to_string( tank_has ) + " ml" );

    // Someone else's car: the read says so, and CDDA asks before any work. Here the answer is no.
    const bool taken = scene( 68 );
    const parsed foreign = parse( await( fp3d::post_vehicle_work_query( car.uid, -1 ) ) );
    const outcome theft = work( car.uid, 0, 'o' );
    check( taken && work_machine_state.load() == 1 && foreign.valid && foreign.ok && !foreign.owned &&
           theft.back && theft.results == 1 && !theft.done && theft.why.empty() && theft.prompts == 1 &&
           theft.asked.find( "belongs to" ) != std::string::npos,
           "someone else's car: the read says so, CDDA asks, no means no", shown( theft ) );
    // Yes: CDDA goes on to its own check of the command.
    const outcome brazen = work( car.uid, 0, 'o', true );
    check( brazen.back && brazen.results == 1 && brazen.prompts >= 1 &&
           brazen.asked.find( "belongs to" ) != std::string::npos && ( brazen.done || !brazen.why.empty() ),
           "someone else's car: yes, and CDDA's check answers", shown( brazen ) );

    // From where the avatar stood before: no part of the car within a square.
    const bool back = scene( 65 );
    const parsed away = parse( await( fp3d::post_vehicle_work_query( car.uid, car.box_row ) ) );
    check( back && away.valid && !away.ok && away.error == "too far", "from six squares away: too far", away.error );
    const outcome from_afar = work( car.uid, 0, 'o' );
    const outcome no_car = work( car.uid + 100000, 0, 'o' );
    check( from_afar.results == 1 && !from_afar.done && from_afar.why == "That vehicle is too far away." &&
           no_car.results == 1 && !no_car.done && no_car.why == "There is no such vehicle here.",
           "action from afar, and for an unknown uid: refused", shown( from_afar ) + " / " + shown( no_car ) );
    check( scene( 66 ), "after: the car gone, skill and pack as before", "" );
    return failed == 0;
}

// G5, the map: a page of the overmap as the player knows it (fp3d/overmap_read.h). Cells are told by what CDDA's
// own map screen draws at the vision level the player has of them; a page over ground with no overmap yet reads
// as unseen and creates none.
static bool run_overmap_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  map: %-52s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    const auto scene = [&]( int number ) {
        std::optional<fp3d::ui_request> at = fp3d::wait_request( 5000 );
        if( !at || !at->timed ) {
            return false;
        }
        hand_setup = number;
        fp3d::queue_action( "no_such_action" );   // dropped: the scene runs before the request asked again
        at = fp3d::wait_request( 20000, at->id );
        return at && at->timed;
    };
    struct m_look {
        std::string name;
        std::string sym;
        std::string color;
    };
    struct m_note {
        int x = 0;
        int y = 0;
        std::string sym;
        std::string color;
        std::string text;
        bool danger = false;
        int radius = -1;
    };
    struct m_page {
        bool valid = false;
        bool ok = false;
        int64_t id = 0;
        std::string kind;
        std::string error;
        int origin[3] = { 0, 0, 0 };
        std::vector<m_look> looks;
        std::vector<int> look;
        std::vector<int> vision;
        std::vector<int> flags;
        std::vector<m_note> notes;
        int notes_more = -1;
        bool has_path = false;
        int to[3] = { 0, 0, 0 };
        std::vector<int> cells;
        int path_more = -1;
        bool truncated = false;
    };
    const auto parse = []( const std::string & text ) {
        m_page out;
        try {
            const JsonValue root = json_loader::from_string( text );
            JsonObject jo = root.get_object();
            jo.allow_omitted_members();
            out.ok = jo.get_bool( "ok" );
            out.id = jo.get_member( "id" ).get_int64();
            out.kind = jo.get_string( "kind" );
            if( !out.ok ) {
                out.error = jo.get_string( "error" );
                out.valid = true;
                return out;
            }
            const auto ints = [&jo]( const char *name, std::vector<int> &into ) {
                JsonArray a = jo.get_array( name );
                for( size_t i = 0; i < a.size(); i++ ) {
                    into.push_back( a.get_int( i ) );
                }
            };
            std::vector<int> origin;
            ints( "origin", origin );
            for( size_t i = 0; i < 3 && i < origin.size(); i++ ) {
                out.origin[i] = origin[i];
            }
            for( JsonObject l : jo.get_array( "looks" ) ) {
                l.allow_omitted_members();
                out.looks.push_back( m_look{ l.get_string( "name" ), l.get_string( "sym" ), l.get_string( "color" ) } );
            }
            ints( "look", out.look );
            ints( "vision", out.vision );
            ints( "flags", out.flags );
            for( JsonObject n : jo.get_array( "notes" ) ) {
                n.allow_omitted_members();
                m_note note;
                JsonArray at = n.get_array( "at" );
                note.x = at.get_int( 0 );
                note.y = at.get_int( 1 );
                note.sym = n.get_string( "sym" );
                note.color = n.get_string( "color" );
                note.text = n.get_string( "text" );
                note.danger = n.get_bool( "danger" );
                note.radius = n.get_int( "radius" );
                out.notes.push_back( note );
            }
            out.notes_more = jo.get_int( "notes_more" );
            if( jo.has_object( "path" ) ) {
                JsonObject p = jo.get_object( "path" );
                p.allow_omitted_members();
                out.has_path = true;
                JsonArray to = p.get_array( "to" );
                for( size_t i = 0; i < 3 && i < to.size(); i++ ) {
                    out.to[i] = to.get_int( i );
                }
                JsonArray c = p.get_array( "cells" );
                for( size_t i = 0; i < c.size(); i++ ) {
                    out.cells.push_back( c.get_int( i ) );
                }
                out.path_more = p.get_int( "more" );
            }
            out.truncated = jo.get_bool( "truncated" );
            out.valid = true;
        } catch( const std::exception & ) {
            out.valid = false;
        }
        return out;
    };
    const auto await = []( uint64_t id ) {
        const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        while( id != 0 && ms_since( t0 ) < 20000 * fp3d::slow_build ) {
            if( std::optional<std::string> res = fp3d::take_query_result( id ) ) {
                return *res;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
        }
        return std::string( "timeout" );
    };
    // The page a cell is on starts at the multiple of 32 at or below it.
    const auto origin_of = []( int v ) {
        return v - ( ( v % 32 ) + 32 ) % 32;
    };
    const auto index_of = [&]( int x, int y ) {
        return static_cast<size_t>( ( y - origin_of( y ) ) * 32 + ( x - origin_of( x ) ) );
    };
    const auto read = [&]( int x, int y, int z, bool path ) {
        return parse( await( fp3d::post_overmap_query( fp3d::ui_point{ origin_of( x ), origin_of( y ), z }, path ) ) );
    };
    const auto whole_page = []( const m_page & p ) {
        return p.valid && p.ok && p.look.size() == 1024 && p.vision.size() == 1024 && p.flags.size() == 1024;
    };
    struct m_cell {
        bool ok = false;
        int vision = -1;
        int look = -1;
        int flags = -1;
        m_look drawn;
    };
    const auto cell_at = [&]( int x, int y, int z ) {
        m_cell out;
        const m_page page = read( x, y, z, false );
        const size_t i = index_of( x, y );
        if( whole_page( page ) && page.look[i] >= 0 && static_cast<size_t>( page.look[i] ) < page.looks.size() ) {
            out.ok = true;
            out.vision = page.vision[i];
            out.look = page.look[i];
            out.flags = page.flags[i];
            out.drawn = page.looks[page.look[i]];
        }
        return out;
    };

    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    check( fp3d::post_overmap_query( fp3d::ui_point{ 1, 0, 0 }, false ) == 0 &&
           fp3d::post_overmap_query( fp3d::ui_point{ 0, 33, 0 }, false ) == 0 &&
           fp3d::post_overmap_query( fp3d::ui_point{ 0, 0, fp3d::max_overmap_z + 1 }, false ) == 0 &&
           fp3d::post_overmap_query( fp3d::ui_point{ fp3d::max_overmap_xy + 32, 0, 0 }, false ) == 0 &&
           !fp3d::overmap_origin_ok( -31, 0, 0 ) && fp3d::overmap_origin_ok( -64, 32, -10 ),
           "refused unposted: an origin off the grid or out of range", "" );

    const bool set_up = scene( 76 );
    const int state = map_state.load();
    const int ax = map_at[0];
    const int ay = map_at[1];
    const int az = map_at[2];
    const m_page mine = read( ax, ay, az, false );
    bool indexed = whole_page( mine ) && mine.looks.size() >= 2 && mine.looks.size() <= fp3d::max_overmap_looks;
    for( const int l : mine.look ) {
        indexed = indexed && l >= 0 && static_cast<size_t>( l ) < mine.looks.size();
    }
    const size_t here_i = index_of( ax, ay );
    const bool blank_first = indexed && mine.looks[0].name.empty() && mine.looks[0].sym.empty() &&
                             mine.looks[0].color.empty();
    const bool here_ok = indexed && mine.vision[here_i] >= 1 && mine.looks[mine.look[here_i]].name == map_here[0] &&
                         mine.looks[mine.look[here_i]].sym == map_here[1] &&
                         mine.looks[mine.look[here_i]].color == map_here[2];
    check( set_up && ( state & 14 ) == 14 && indexed && mine.kind == "overmap" && mine.origin[0] == origin_of( ax ) &&
           mine.origin[1] == origin_of( ay ) && mine.origin[2] == az && blank_first && here_ok && !mine.has_path &&
           !mine.truncated,
           "the avatar's page: 1024 cells; its own cell as CDDA draws it",
           std::to_string( state ) + " (1 a cell that looks different when vague, 2 a note on an explored cell, " +
           "4 no overmap under the far page, 8 two unseen cells out of sight), " +
           std::to_string( mine.looks.size() ) + " looks, here \"" +
           map_here[0] + "\" " + map_here[1] + " " + map_here[2] + ( mine.valid ? "" : ": not valid" ) );

    const m_cell vague = cell_at( map_vague_at[0], map_vague_at[1], az );
    const bool differs = ( state & 1 ) != 0;
    check( vague.ok && vague.vision == 1 && vague.drawn.name == map_vague[0] && vague.drawn.sym == map_vague[1] &&
           vague.drawn.color == map_vague[2] && ( !differs || vague.drawn.name != map_full_name ),
           "a vaguely seen cell: its vague look only",
           "\"" + vague.drawn.name + "\" " + vague.drawn.sym + " " + vague.drawn.color +
           ( differs ? ", in full \"" + map_full_name + "\"" : " (no cell near looks different when vague)" ) );
    const m_cell unseen = cell_at( map_unseen_at[0], map_unseen_at[1], az );
    check( unseen.ok && unseen.vision == 0 && unseen.look == 0 && unseen.drawn.name.empty() &&
           unseen.drawn.sym.empty() && ( state & 32 ) != 0 && unseen.flags == 0,
           "an unseen cell: nothing drawn, its explored mark not told",
           "vision " + std::to_string( unseen.vision ) + ", look " + std::to_string( unseen.look ) + ", flags " +
           std::to_string( unseen.flags ) + ( ( state & 32 ) != 0 ? "" : " (the mark wasn't set)" ) );
    // A campsite among forest, all seen vaguely: CDDA draws the campsite as forest until it is seen better.
    const m_cell blended = cell_at( map_blend_at[0], map_blend_at[1], az );
    check( ( state & 16 ) != 0 && blended.ok && blended.vision == 1 && !blended.drawn.name.empty() &&
           blended.drawn.name == map_blend[0] && blended.drawn.sym == map_blend[1] &&
           blended.drawn.color == map_blend[2] && blended.drawn.name == map_woods_name,
           "a cell CDDA blends in when vague: drawn as its neighbours",
           "\"" + blended.drawn.name + "\" " + blended.drawn.sym + " " + blended.drawn.color + ", CDDA's \"" +
           map_blend[0] + "\", a forest's \"" + map_woods_name + "\"" +
           ( ( state & 16 ) != 0 ? "" : " (no campsite placed)" ) );

    const m_page noted = read( map_note_at[0], map_note_at[1], az, false );
    const m_note *n = nullptr;
    for( const m_note &c : noted.notes ) {
        if( c.x == map_note_at[0] && c.y == map_note_at[1] ) {
            n = &c;
        }
    }
    check( whole_page( noted ) && n != nullptr && n->text == "fp3d map note" && n->sym == map_note_sym &&
           n->color == map_note_color && n->danger && n->radius == 3 && noted.notes_more == 0 &&
           ( noted.flags[index_of( map_note_at[0], map_note_at[1] )] & fp3d::overmap_explored ) != 0,
           "a note: CDDA's symbol and colour, its text, its danger; the mark",
           n ? "\"" + n->text + "\" " + n->sym + " " + n->color + ", radius " + std::to_string( n->radius ) :
           std::string( "not on the page, " ) + std::to_string( noted.notes.size() ) + " notes" );

    const m_page routed = read( ax, ay, az, true );
    const bool path_ok = routed.has_path && routed.to[0] == ax && routed.to[1] == ay - 5 && routed.to[2] == az &&
                         routed.cells.size() == 15 && routed.cells[0] == ax && routed.cells[1] == ay - 1 &&
                         routed.cells[2] == az && routed.cells[12] == ax && routed.cells[13] == ay - 5 &&
                         routed.path_more == 0;
    check( whole_page( routed ) && path_ok && !routed.truncated,
           "the travel path when asked for: next cell first, the end last",
           std::to_string( routed.cells.size() / 3 ) + " cells, " + std::to_string( routed.path_more ) + " more" );
    const m_page whole = parse( map_whole );
    const m_page tiny = parse( map_tiny );
    check( whole_page( whole ) && whole.cells.size() == 15 && whole_page( tiny ) && tiny.truncated && tiny.has_path &&
           tiny.path_more >= 1 && tiny.cells.size() + 3 * static_cast<size_t>( tiny.path_more ) == 15 &&
           tiny.to[1] == ay - 5 && map_tiny.size() + 40 <= map_whole.size(),
           "in fewer bytes: the path's far cells go first",
           std::to_string( map_whole.size() ) + " -> " + std::to_string( map_tiny.size() ) + " bytes, " +
           std::to_string( tiny.cells.size() / 3 ) + " cells, " + std::to_string( tiny.path_more ) + " more" );

    // Still too large with the path and the notes gone: looks go from the end, their cells drawn blank.
    const m_page plain = parse( map_plain );
    const m_page fewer = parse( map_fewer );
    bool fewer_ok = whole_page( plain ) && whole_page( fewer ) && fewer.truncated && !fewer.looks.empty() &&
                    fewer.looks.size() < plain.looks.size() && map_fewer.size() + 200 <= map_plain.size();
    size_t blanked = 0;
    for( size_t i = 0; fewer_ok && i < 1024; i++ ) {
        fewer_ok = fewer.look[i] >= 0 && static_cast<size_t>( fewer.look[i] ) < fewer.looks.size();
        blanked += fewer.look[i] == 0 && fewer.vision[i] > 0 ? 1 : 0;
    }
    check( fewer_ok && blanked > 0, "in fewer bytes still: looks go last, their cells drawn blank",
           std::to_string( map_plain.size() ) + " -> " + std::to_string( map_fewer.size() ) + " bytes, " +
           std::to_string( plain.looks.size() ) + " -> " + std::to_string( fewer.looks.size() ) + " looks, " +
           std::to_string( blanked ) + " seen cells blank" );

    const m_page remote = read( map_far_at[0], map_far_at[1], az, false );
    bool blank = whole_page( remote );
    for( size_t i = 0; blank && i < 1024; i++ ) {
        blank = remote.look[i] == 0 && remote.vision[i] == 0 && remote.flags[i] == 0;
    }
    check( blank && remote.looks.size() == 1 && remote.notes.empty() && ( state & 4 ) != 0,
           "a page over ground with no overmap: every cell unseen",
           std::to_string( remote.looks.size() ) + " looks, " + std::to_string( remote.notes.size() ) + " notes" );
    const bool undone = scene( 77 );
    check( undone && map_state.load() == 3, "after: as before, and no overmap has appeared there",
           std::to_string( map_state.load() ) + " of 3 (1 still no overmap under the far page, 2 the note gone)" );
    return failed == 0;
}

// The off hand lets go of an item that has grown too heavy for one hand (here: filled up); it stays in its pocket
// (lead-ui, 2026-11-15).
static bool run_offhand_weight_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  off hand: %-45s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    hand_setup = 19;
    fp3d::queue_action( "pause" );
    r = fp3d::wait_request( 20000, r->id );
    const fp3d::held_view h0 = fp3d::latest_held();
    check( r && r->timed && h0.off.held && h0.off.id == "jerrycan", "an empty jerrycan held", h0.off.id );
    if( !r || !r->timed ) {
        return false;
    }
    std::vector<fp3d::event> evs;
    fp3d::drain_events( evs );
    evs.clear();
    hand_setup = 20;
    // Two turns: the publish after the setup, whichever comes first in a request.
    for( int i = 0; i < 2 && r && r->timed; i++ ) {
        fp3d::queue_action( "pause" );
        r = fp3d::wait_request( 20000, r->id );
    }
    fp3d::drain_events( evs );
    bool told = false;
    for( const fp3d::event &e : evs ) {
        told = told || ( e.kind == fp3d::event_kind::message &&
                         e.text.find( "too heavy to hold in one hand now" ) != std::string::npos );
    }
    const fp3d::held_view h1 = fp3d::latest_held();
    const uint32_t t_can = fp3d::intern_id( fp3d::id_kind::itype, "jerrycan" );
    bool kept = false;
    for( const fp3d::carried_view &c : carried_now() ) {
        kept = kept || ( c.type == t_can && c.parent != 0 );
    }
    check( r && r->timed && !h1.off.held && told && kept, "filled: let go, still in the pack",
           std::string( h1.off.held ? "still held " : "" ) + ( told ? "" : "not told " ) + ( kept ? "" : "gone" ) );
    return failed == 0;
}

// Lying containers and big piles (L8 #3, #4, 2026-11-16): what lies in a container next to the player is listed one
// level deep (never in a sealed pocket, never 2 squares away) and can be taken out by hand; piles next to the player
// list up to 256 handles, others 32 with the rest counted in the JSON's `more`.
static bool run_lying_container_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  lying containers: %-44s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    struct result {
        bool queued = false;
        bool back = false;
        int answers = 0;
        bool done = false;
        std::string why;
        int spent = 0;
    };
    uint32_t next_tag = 400;
    const auto run = [&]( fp3d::item_action ia ) {
        result res;
        std::vector<fp3d::event> evs;
        const std::optional<fp3d::ui_request> r0 = fp3d::wait_request( 5000 );
        if( !r0 || !r0->timed ) {
            return res;
        }
        const observed o0 = snapshot();
        fp3d::drain_events( evs );
        evs.clear();
        ia.tag = ++next_tag;
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
            if( e.kind == fp3d::event_kind::item_result && e.detail == ia.tag ) {
                res.answers++;
                res.done = e.amount == 1;
                res.why = e.text;
            }
        }
        return res;
    };
    const auto show = []( const result & r ) {
        return std::string( r.queued ? "" : "not queued " ) + ( r.done ? "done" : "not done" ) + " [" + r.why +
               "] " + std::to_string( r.spent ) + " moves";
    };
    // What the newest snapshot lists inside the container with handle `box`.
    const auto inside_of = []( uint64_t box ) {
        std::vector<fp3d::inside_view> out;
        if( const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot() ) {
            for( const std::shared_ptr<const fp3d::submap_view> &sm : s->submaps ) {
                for( const fp3d::inside_view &v : sm->inside ) {
                    if( v.container == box ) {
                        out.push_back( v );
                    }
                }
            }
        }
        return out;
    };
    const auto carried_has = []( uint64_t uid ) {
        for( const fp3d::carried_view &c : carried_now() ) {
            if( c.uid == uid ) {
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
    hand_setup = 22;
    fp3d::queue_action( "pause" );
    r = fp3d::wait_request( 20000, r->id );
    const std::shared_ptr<const fp3d::snapshot> s0 = fp3d::latest_snapshot();
    if( !r || !r->timed || !s0 || lc_pack_uid == 0 || lc_rock_uid == 0 || lc_sealed_uid == 0 ||
        lc_sealed_water_uid == 0 || lc_far_pack_uid == 0 ) {
        check( false, "scene", "" );
        return false;
    }
    const fp3d::ui_point here = s0->player.pos;
    const fp3d::ui_point west{ here.x - 1, here.y, here.z };
    const fp3d::ui_point east{ here.x + 1, here.y, here.z };
    const fp3d::ui_point far_east{ here.x + 2, here.y, here.z };
    const uint32_t t_rock = fp3d::intern_id( fp3d::id_kind::itype, "rock" );
    const uint32_t t_pack = fp3d::intern_id( fp3d::id_kind::itype, "backpack" );
    const uint32_t t_bottle = fp3d::intern_id( fp3d::id_kind::itype, "bottle_plastic" );
    const uint32_t t_water = fp3d::intern_id( fp3d::id_kind::itype, "water_clean" );

    const std::vector<fp3d::inside_view> in0 = inside_of( lc_pack_uid );
    size_t rocks = 0;
    size_t bottles = 0;
    bool pockets_ok = true;
    bool has_rock = false;
    for( const fp3d::inside_view &v : in0 ) {
        rocks += v.type == t_rock ? 1 : 0;
        bottles += v.type == t_bottle ? 1 : 0;
        pockets_ok = pockets_ok && v.pocket >= 0;
        has_rock = has_rock || ( v.uid == lc_rock_uid && v.type == t_rock );
    }
    check( in0.size() == 3 && rocks == 1 && bottles == 2 && pockets_ok && has_rock,
           "a backpack next to you: its 3 items listed", std::to_string( in0.size() ) + " listed" );
    check( inside_of( lc_sealed_uid ).empty() && inside_of( lc_far_pack_uid ).empty(),
           "not in a sealed bottle, not 2 squares away", std::to_string( inside_of( lc_sealed_uid ).size() ) + " / " +
           std::to_string( inside_of( lc_far_pack_uid ).size() ) );
    const size_t near_n = handles_at( east ).size();
    const size_t far_n = handles_at( far_east ).size();
    check( near_n == 40 && far_n == fp3d::max_handles_per_tile, "40 rocks: all listed next to you, 32 farther",
           std::to_string( near_n ) + " / " + std::to_string( far_n ) );
    {
        std::ostringstream json;
        fp3d::write_snapshot_json( json, *s0, here.z, here.z );
        const std::string j = json.str();
        const auto floor_div = []( int a ) {
            return a >= 0 ? a / fp3d::sm_size : ( a - fp3d::sm_size + 1 ) / fp3d::sm_size;
        };
        const int far_tile = ( far_east.y - floor_div( far_east.y ) * fp3d::sm_size ) * fp3d::sm_size +
                             ( far_east.x - floor_div( far_east.x ) * fp3d::sm_size );
        const bool more = j.find( "[" + std::to_string( far_tile ) + ",8]" ) != std::string::npos &&
                          j.find( "\"more\":[" ) != std::string::npos;
        const bool inside = j.find( "[" + std::to_string( lc_pack_uid ) + "," + std::to_string( lc_rock_uid ) + "," +
                                    std::to_string( t_rock ) + "," ) != std::string::npos;
        check( more && inside, "JSON: `more` for the far pile, `inside` for the pack", "" );
    }

    // Take one rock out of the backpack next to you.
    fp3d::item_action take;
    take.op = fp3d::item_op::take;
    take.item = fp3d::item_ref{ lc_rock_uid, t_rock };
    take.pos = west;
    take.container = fp3d::item_ref{ lc_pack_uid, t_pack };
    result res = run( take );
    bool still_listed = false;
    for( const fp3d::inside_view &v : inside_of( lc_pack_uid ) ) {
        still_listed = still_listed || v.uid == lc_rock_uid;
    }
    check( res.back && res.answers == 1 && res.done && res.spent > 0 && carried_has( lc_rock_uid ) && !still_listed &&
           inside_of( lc_pack_uid ).size() == 2, "take a rock out of the backpack", show( res ) );
    // The sealed bottle's water: refused, still sealed in there.
    take.item = fp3d::item_ref{ lc_sealed_water_uid, t_water };
    take.container = fp3d::item_ref{ lc_sealed_uid, t_bottle };
    res = run( take );
    check( res.back && res.answers == 1 && !res.done && res.why.find( "sealed" ) != std::string::npos &&
           res.spent == 0, "out of a sealed bottle: refused", show( res ) );
    // A rock lying east isn't in the backpack.
    const std::vector<fp3d::item_handle_view> east_rocks = handles_at( east );
    take.item = fp3d::item_ref{ east_rocks.empty() ? 1 : east_rocks[0].uid, t_rock };
    take.container = fp3d::item_ref{ lc_pack_uid, t_pack };
    take.pos = west;
    res = run( take );
    check( res.back && res.answers == 1 && !res.done && res.why.find( "isn't there" ) != std::string::npos &&
           res.spent == 0, "an item not in that container: refused", show( res ) );
    // A half-named container never queues.
    take.container = fp3d::item_ref{ lc_pack_uid, 0 };
    check( !fp3d::queue_item_action( take ) && fp3d::queued_actions() == 0, "a container without its type: never queued",
           "" );
    return failed == 0;
}

// Character queries (G4, 2026-11-16): the character screen's data. A starting trait and a mutation told apart, a
// bionic and power, an effect's description; with less room descriptions go from the end and the lists stay whole;
// the bridge runs one at a time.
// Peeking (B2): CDDA's peek asks a `target` request in mode "peek" that offers the blind throws as actions:
// throw_blind (CDDA asks what to throw) and, while something is wielded, throw_blind_wielded; then CDDA asks where
// ("throw_blind" target). An action the request didn't offer, or one sent to a plain look, throws nothing.
static bool run_peek_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  peek: %-50s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    hand_setup = 25;
    fp3d::queue_action( "pause" );
    r = fp3d::wait_request( 20000, r->id );
    const std::shared_ptr<const fp3d::snapshot> s0 = fp3d::latest_snapshot();
    if( !r || !r->timed || !s0 || pk_rock_uid == 0 || pk_wielded_uid == 0 ) {
        check( false, "scene", "" );
        return false;
    }
    const fp3d::ui_point here = s0->player.pos;
    struct peeked {
        bool queued = false;
        bool back = false;                   // in the main loop again
        std::vector<std::string> offered;    // the look / peek request's actions
        std::string modes;                   // target modes asked, in order
        int item_menus = 0;
        std::string msgs;
    };
    // Queues `start` ("peek" or "look"), peeks east (`dir` 1) or west (-1), answers the look / peek request with
    // `act` and no square, an item request with a rock, a blind throw's target 3 squares on that way from its
    // origin. The two rocks go opposite ways: rocks stack, so one landing on the other would merge into it.
    const auto run = [&]( const char *start, const std::string & act, int dir ) {
        peeked res;
        std::vector<fp3d::event> evs;
        fp3d::drain_events( evs );
        evs.clear();
        res.queued = fp3d::queue_action( start );
        if( !res.queued ) {
            return res;
        }
        const uint64_t before = r->id;
        bool first_target = true;
        const auto t = std::chrono::steady_clock::now();
        while( ms_since( t ) < 30000 * fp3d::slow_build ) {
            std::optional<fp3d::ui_request> next = fp3d::wait_request( 200, r->id );
            if( !next ) {
                continue;
            }
            r = next;
            if( r->timed ) {
                if( r->id != before ) {
                    res.back = true;
                    break;
                }
                continue;
            }
            fp3d::ui_answer a;
            a.id = r->id;
            if( r->kind == fp3d::request_kind::direction ) {
                a.pos = fp3d::ui_point{ dir, 0, 0 };
            } else if( r->kind == fp3d::request_kind::target && first_target ) {
                first_target = false;
                res.modes += r->mode;
                res.offered = r->actions;
                a.action = act;
            } else if( r->kind == fp3d::request_kind::target ) {
                res.modes += " " + r->mode;
                a.pos = fp3d::ui_point{ r->origin.x + 3 * dir, r->origin.y, r->origin.z };
                a.action = "FIRE";
            } else if( r->kind == fp3d::request_kind::item_select ) {
                res.item_menus++;
                std::string label;
                a.entry = first_entry_with( *r, "rock", label );
                a.cancel = a.entry < 0;
            } else {
                a.cancel = true;
            }
            fp3d::answer( a );
        }
        fp3d::drain_events( evs );
        for( const fp3d::event &e : evs ) {
            if( e.kind == fp3d::event_kind::message ) {
                res.msgs += ( res.msgs.empty() ? "" : " | " ) + e.text;
            }
        }
        return res;
    };
    const auto join = []( const std::vector<std::string> &v ) {
        std::string out;
        for( const std::string &x : v ) {
            out += ( out.empty() ? "" : "," ) + x;
        }
        return out;
    };
    const auto show = [&]( const peeked & p ) {
        return std::string( p.queued ? "" : "not queued " ) + ( p.back ? "" : "not back " ) + "offered [" +
               join( p.offered ) + "] modes [" + p.modes + "] " + std::to_string( p.item_menus ) +
               " item menus, msgs [" + p.msgs + "]";
    };
    const auto carried_has = []( uint64_t uid ) {
        for( const fp3d::carried_view &c : carried_now() ) {
            if( c.uid == uid ) {
                return true;
            }
        }
        return false;
    };
    // Where `uid` lies, 1-10 squares east (`dir` 1) or west (-1) and up to 3 off the row; nullopt if not there.
    const auto lies_on = [&here]( uint64_t uid, int dir ) -> std::optional<fp3d::ui_point> {
        for( int dx = 1; dx <= 10; dx++ ) {
            for( int dy = -3; dy <= 3; dy++ ) {
                const fp3d::ui_point q{ here.x + dx * dir, here.y + dy, here.z };
                for( const fp3d::item_handle_view &h : handles_at( q ) ) {
                    if( h.uid == uid ) {
                        return q;
                    }
                }
            }
        }
        return std::nullopt;
    };
    const auto at = []( const std::optional<fp3d::ui_point> &q ) {
        return q ? "(" + std::to_string( q->x ) + "," + std::to_string( q->y ) + ")" : std::string( "nowhere" );
    };
    const std::vector<std::string> both{ "throw_blind", "throw_blind_wielded" };
    const std::vector<std::string> one{ "throw_blind" };

    // A look offers nothing, and a blind throw sent to it throws nothing.
    peeked p = run( "look", "throw_blind", 1 );
    check( p.back && p.offered.empty() && p.modes == "look" && p.item_menus == 0 && carried_has( pk_wielded_uid ) &&
           carried_has( pk_rock_uid ), "look: no blind throw", show( p ) );
    // The wielded rock, thrown blind from the peek: no item menu.
    p = run( "peek", "throw_blind_wielded", 1 );
    std::optional<fp3d::ui_point> landed = lies_on( pk_wielded_uid, 1 );
    check( p.back && p.offered == both && p.modes == "peek throw_blind" && p.item_menus == 0 &&
           !carried_has( pk_wielded_uid ) && landed, "throw_blind_wielded throws the wielded rock",
           show( p ) + " landed " + at( landed ) );
    // Nothing wielded now: only throw_blind is offered, and throw_blind_wielded is ignored.
    p = run( "peek", "throw_blind_wielded", -1 );
    check( p.back && p.offered == one && p.modes == "peek" && p.item_menus == 0 && carried_has( pk_rock_uid ),
           "not offered: ignored", show( p ) );
    p = run( "peek", "fire_blind", -1 );
    check( p.back && p.offered == one && p.modes == "peek" && p.item_menus == 0 && carried_has( pk_rock_uid ),
           "unknown action: ignored", show( p ) );
    // The carried rock, west: CDDA asks which item first.
    p = run( "peek", "throw_blind", -1 );
    landed = lies_on( pk_rock_uid, -1 );
    check( p.back && p.offered == one && p.modes == "peek throw_blind" && p.item_menus == 1 &&
           !carried_has( pk_rock_uid ) && landed, "throw_blind asks what, then throws it",
           show( p ) + " landed " + at( landed ) );
    return failed == 0;
}

// The number request (E3): CDDA's query_int, query_int64_t and edit( int ) ask the client for a whole number with
// the type's range and the shown default; out of range or no number asks again with a status (a few times, then
// cancel); cancel cancels and edit keeps the value.
static bool run_number_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  number: %-50s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    // How each number request is answered, in order; the hook asks them at the next main-loop request.
    std::vector<std::function<void( fp3d::ui_answer & )>> answers = {
        []( fp3d::ui_answer & a ) { a.number = int64_t( INT_MAX ) + 1; },
        []( fp3d::ui_answer & ) {},
        []( fp3d::ui_answer & a ) { a.number = 42; },
        []( fp3d::ui_answer & a ) { a.cancel = true; },
        []( fp3d::ui_answer & a ) { a.number = -3; },
    };
    for( int i = 0; i < fp3d::max_number_asks; i++ ) {
        answers.emplace_back( []( fp3d::ui_answer & a ) { a.number = int64_t( INT_MIN ) - 1; } );
    }
    number_done.store( false );
    number_setup.store( 1 );
    fp3d::queue_action( "pause" );
    const uint64_t before = r->id;
    std::vector<fp3d::ui_request> asked;
    size_t other = 0;
    const auto t = std::chrono::steady_clock::now();
    while( ms_since( t ) < 30000 * fp3d::slow_build && !( number_done.load() && r->timed && r->id != before ) ) {
        std::optional<fp3d::ui_request> next = fp3d::wait_request( 200, r->id );
        if( !next ) {
            continue;
        }
        r = next;
        if( r->timed ) {
            continue;
        }
        fp3d::ui_answer a;
        a.id = r->id;
        if( r->kind == fp3d::request_kind::number && asked.size() < answers.size() ) {
            answers[asked.size()]( a );
            asked.push_back( *r );
        } else {
            other++;
            a.cancel = true;
        }
        fp3d::answer( a );
    }
    number_results res;
    {
        std::lock_guard<std::mutex> lk( number_mutex );
        res = number_out;
    }
    check( number_done.load() && asked.size() == answers.size() && other == 0, "asked as number requests",
           std::to_string( asked.size() ) + " of " + std::to_string( answers.size() ) + ", " +
           std::to_string( other ) + " others" );
    if( asked.size() != answers.size() ) {
        return false;
    }
    const std::string range_status = "enter a whole number from " + std::to_string( INT_MIN ) + " to " +
                                      std::to_string( INT_MAX );
    check( asked[0].title == "How many?" && asked[0].number_min == INT_MIN && asked[0].number_max == INT_MAX &&
           asked[0].number_default == 7 && asked[0].status.empty(), "query_int: title, int range, shown default",
           std::to_string( asked[0].number_min ) + ".." + std::to_string( asked[0].number_max ) + " default " +
           std::to_string( asked[0].number_default ) );
    check( asked[1].status == range_status && asked[2].status == range_status && asked[1].number_default == 7,
           "out of range, then no number: asked again", asked[1].status + " | " + asked[2].status );
    check( res.got_int && res.int_value == 42, "the third answer taken", std::to_string( res.int_value ) );
    check( asked[3].number_min == -fp3d::max_number_answer && asked[3].number_max == fp3d::max_number_answer &&
           asked[3].number_default == 0 && res.big_cancelled, "query_int64_t: float-exact range, cancelled",
           std::to_string( asked[3].number_max ) );
    check( asked[4].number_default == 5 && asked[4].title == "Edit" && res.edited == -3, "edit( int ): from 5 to -3",
           std::to_string( res.edited ) );
    check( !res.got_stubborn && res.stubborn_value == 9 && asked.back().title == "Again?",
           "always out of range: cancelled after the last ask", std::to_string( res.stubborn_value ) );
    return failed == 0;
}

static bool run_character_query_checks()
{
    int failed = 0;
    const auto check = [&]( bool ok, const char *what, const std::string &detail ) {
        std::printf( "  character query: %-44s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
        failed += ok ? 0 : 1;
    };
    struct entry {
        std::string id;
        std::string name;
        std::string description;
        int rating = 0;
        bool flag = false;   // traits: mutation; bionics: active
    };
    struct parsed {
        bool valid = false;
        bool ok = false;
        int64_t id = 0;
        std::string kind;
        std::string name;
        std::vector<std::string> stat_ids;
        int stats_low = 0;   // stats with cur or base below 1
        int speed = 0;
        size_t encumbrance = 0;
        std::vector<entry> skills;
        std::vector<entry> traits;
        std::vector<entry> bionics;
        size_t known = 0;
        size_t learning = 0;
        int power = -1;
        int max_power = -1;
        int carried[4] = { -1, -1, -1, -1 };   // weight_g, weight_max_g, volume_ml, volume_max_ml
        std::map<std::string, std::string> effects;
        bool truncated = false;
    };
    const auto parse = []( const std::string & text ) {
        parsed out;
        try {
            const JsonValue root = json_loader::from_string( text );
            JsonObject jo = root.get_object();
            jo.allow_omitted_members();
            out.ok = jo.get_bool( "ok" );
            out.id = jo.get_member( "id" ).get_int64();
            out.kind = jo.get_string( "kind" );
            if( out.ok ) {
                out.name = jo.get_string( "name" );
                for( JsonObject s : jo.get_array( "stats" ) ) {
                    s.allow_omitted_members();
                    out.stat_ids.push_back( s.get_string( "id" ) );
                    out.stats_low += s.get_int( "cur" ) < 1 || s.get_int( "base" ) < 1 ? 1 : 0;
                }
                JsonObject sp = jo.get_object( "speed" );
                sp.allow_omitted_members();
                out.speed = sp.get_int( "cur" );
                for( JsonObject e : jo.get_array( "encumbrance" ) ) {
                    e.allow_omitted_members();
                    out.encumbrance++;
                }
                const auto list = []( const JsonObject & from, const char *key, std::vector<entry> &to ) {
                    for( JsonObject o : from.get_array( key ) ) {
                        o.allow_omitted_members();
                        entry e;
                        e.id = o.get_string( "id" );
                        e.name = o.get_string( "name" );
                        e.description = o.get_string( "description" );
                        e.rating = o.get_int( "rating", 0 );
                        e.flag = o.has_member( "mutation" ) ? o.get_bool( "mutation" ) : o.get_bool( "active", false );
                        to.push_back( e );
                    }
                };
                list( jo, "skills", out.skills );
                list( jo, "traits", out.traits );
                list( jo, "bionics", out.bionics );
                JsonObject profs = jo.get_object( "proficiencies" );
                profs.allow_omitted_members();
                for( JsonObject o : profs.get_array( "known" ) ) {
                    o.allow_omitted_members();
                    out.known++;
                }
                for( JsonObject o : profs.get_array( "learning" ) ) {
                    o.allow_omitted_members();
                    out.learning++;
                }
                JsonObject power = jo.get_object( "power" );
                power.allow_omitted_members();
                out.power = power.get_int( "cur_kj" );
                out.max_power = power.get_int( "max_kj" );
                out.carried[0] = jo.get_int( "weight_g" );
                out.carried[1] = jo.get_int( "weight_max_g" );
                out.carried[2] = jo.get_int( "volume_ml" );
                out.carried[3] = jo.get_int( "volume_max_ml" );
                JsonObject eff = jo.get_object( "effect_descriptions" );
                eff.allow_omitted_members();
                for( const JsonMember m : eff ) {
                    out.effects[m.name()] = m.get_string();
                }
                out.truncated = jo.get_bool( "truncated" );
            }
            out.valid = true;
        } catch( const std::exception & ) {
            out.valid = false;
        }
        return out;
    };
    const auto find = []( const std::vector<entry> &in, const std::string & id ) -> const entry * {
        for( const entry &e : in ) {
            if( e.id == id ) {
                return &e;
            }
        }
        return nullptr;
    };
    const auto await = []( uint64_t id ) {
        const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        while( id != 0 && ms_since( t0 ) < 20000 * fp3d::slow_build ) {
            if( std::optional<std::string> res = fp3d::take_query_result( id ) ) {
                return *res;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
        }
        return std::string( "timeout" );
    };

    std::optional<fp3d::ui_request> r = fp3d::wait_request( 5000 );
    if( !r || !r->timed ) {
        check( false, "main loop waiting", "" );
        return false;
    }
    hand_setup = 23;
    fp3d::queue_action( "pause" );
    r = fp3d::wait_request( 20000, r->id );
    const std::shared_ptr<const fp3d::snapshot> s0 = fp3d::latest_snapshot();
    std::string full;
    std::string cut;
    std::string tiny;
    std::string name;
    int carried[4] = {};
    {
        std::lock_guard<std::mutex> lk( cq_m );
        std::copy( cq_carried, cq_carried + 4, carried );
        full = cq_full;
        cut = cq_cut;
        tiny = cq_tiny;
        name = cq_name;
    }
    if( !r || !r->timed || !s0 || full.empty() ) {
        check( false, "scene", "" );
        return false;
    }
    const parsed f = parse( full );
    check( f.valid && f.ok && f.kind == "character" && f.id == 901 && !f.name.empty() && f.name == name &&
           !f.truncated, "the avatar's sheet", f.name + ", " + std::to_string( full.size() ) + " bytes" );
    check( f.stat_ids == std::vector<std::string>( { "str", "dex", "int", "per" } ) && f.stats_low == 0 && f.speed > 0,
           "stats and speed", std::to_string( f.stat_ids.size() ) + " stats, speed " + std::to_string( f.speed ) );
    check( f.encumbrance > 0 && f.encumbrance == s0->player.body_parts.size(), "encumbrance: one per body part",
           std::to_string( f.encumbrance ) + " of " + std::to_string( s0->player.body_parts.size() ) );
    size_t named_skills = 0;
    for( const entry &e : f.skills ) {
        named_skills += !e.id.empty() && !e.name.empty() && !e.description.empty() ? 1 : 0;
    }
    check( f.skills.size() >= 5 && named_skills == f.skills.size(), "skills with names and descriptions",
           std::to_string( f.skills.size() ) + " skills" );
    const entry *bad = find( f.traits, "BADHEARING" );
    const entry *night = find( f.traits, "NIGHTVISION" );
    check( bad && bad->rating == -1 && !bad->flag && !bad->description.empty(), "a starting trait: bad, not a mutation",
           bad ? bad->name : "missing" );
    check( night && night->rating == 1 && night->flag, "a mutation: good, a mutation", night ? night->name : "missing" );
    const entry *light = find( f.bionics, "bio_flashlight" );
    check( light && !light->flag && !light->description.empty() && f.power == 50 && f.max_power == 100,
           "a bionic, off, and 50 of 100 kJ", ( light ? light->name : std::string( "missing" ) ) + ", " +
           std::to_string( f.power ) + "/" + std::to_string( f.max_power ) );
    check( std::equal( carried, carried + 4, f.carried ) && f.carried[0] > 0 && f.carried[1] > 0 &&
           f.carried[3] >= 0, "carried weight and volume, and the limits",
           std::to_string( f.carried[0] ) + "/" + std::to_string( f.carried[1] ) + " g, " +
           std::to_string( f.carried[2] ) + "/" + std::to_string( f.carried[3] ) + " ml" );
    const auto winded = f.effects.find( "winded" );
    check( winded != f.effects.end() && !winded->second.empty() && winded->second.find( '\n' ) == std::string::npos,
           "an effect's description, one line", winded != f.effects.end() ? winded->second : "missing" );
    const parsed c = parse( cut );
    const auto cut_winded = c.effects.find( "winded" );
    check( c.valid && c.ok && c.truncated && cut.size() <= full.size() - 300 && c.skills.size() == f.skills.size() &&
           c.traits.size() == f.traits.size() && c.bionics.size() == f.bionics.size() && c.known == f.known &&
           c.learning == f.learning && !c.skills.empty() && !c.skills.front().description.empty() &&
           cut_winded != c.effects.end() && cut_winded->second.empty(),
           "less room: descriptions go from the end", std::to_string( cut.size() ) + " of " +
           std::to_string( full.size() ) + " bytes" );
    // Far less room: list tails go too, effects first, skills last.
    const parsed t = parse( tiny );
    const size_t full_entries = f.skills.size() + f.traits.size() + f.bionics.size() + f.known + f.learning +
                                f.effects.size();
    const size_t tiny_entries = t.skills.size() + t.traits.size() + t.bionics.size() + t.known + t.learning +
                                t.effects.size();
    check( t.valid && t.ok && t.truncated && tiny.size() <= 3000 && tiny_entries < full_entries && t.effects.empty() &&
           !t.skills.empty() && t.stat_ids.size() == 4 && t.encumbrance == f.encumbrance,
           "in 3000 bytes: list tails go, skills last", std::to_string( tiny.size() ) + " bytes, " +
           std::to_string( tiny_entries ) + " of " + std::to_string( full_entries ) + " entries, " +
           std::to_string( t.skills.size() ) + " skills" );

    // Through the bridge: run by the main loop.
    r = fp3d::wait_request( 5000 );
    const uint64_t id = r && r->timed ? fp3d::post_character_query() : 0;
    const parsed b = parse( await( id ) );
    check( id != 0 && b.valid && b.ok && b.id == static_cast<int64_t>( id ) && b.name == name,
           "posted: answered by the main loop", std::to_string( id ) );
    // One at a time: posted during the wait menu, a second one is refused; the first runs back at the main loop.
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
    uint64_t first = 0;
    uint64_t second = 1;
    if( prompt && !prompt->timed ) {
        first = fp3d::post_character_query();
        second = fp3d::post_character_query();
    }
    check( prompt && !prompt->timed && first != 0 && second == 0, "a second pending one refused",
           prompt ? std::string( fp3d::kind_name( prompt->kind ) ) + " prompt" : "no prompt" );
    for( int i = 0; i < 10 && prompt && !prompt->timed; i++ ) {
        fp3d::ui_answer cancel;
        cancel.id = prompt->id;
        cancel.cancel = true;
        fp3d::answer( cancel );
        prompt = fp3d::wait_request( 5000, prompt->id );
    }
    const parsed p = parse( await( first ) );
    check( prompt && prompt->timed && p.valid && p.ok && p.id == static_cast<int64_t>( first ),
           "held during a prompt, then answered", std::to_string( first ) );
    // Take the scene's traits and effect away again.
    r = fp3d::wait_request( 5000 );
    if( r && r->timed ) {
        hand_setup = 24;
        fp3d::queue_action( "pause" );
        r = fp3d::wait_request( 20000, r->id );
    }
    check( r && r->timed, "back at the main loop", "" );
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
    bad_op.op = static_cast<fp3d::item_op>( 200 );   // past the last op
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
    bad_op.op = static_cast<fp3d::item_op>( 200 );   // past the last op
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
    // This test's own user folder (config, saves, caches): what another test binary leaves behind never
    // reaches it.
    try {
        fp3d::init_paths( "", "./fp3d_user_sim" );
    } catch( const std::exception &e ) {
        std::printf( "error: %s\n", e.what() );
        return EXIT_FAILURE;
    }
    fp3d::set_active( true );
    fp3d::set_events_enabled( true );
    std::thread sim( sim_main );
    // Drained at every request like a client would; counted per kind and checked at the end.
    size_t event_counts[fp3d::event_kind_count] = {};
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
        // The first boot in a new user folder checks the game data once, which takes minutes on a busy machine.
        const int wait_s = requests == 0 ? 600 : 180;
        std::optional<fp3d::ui_request> req = fp3d::wait_request( wait_s * 1000 );
        if( !req && !fp3d::is_shut_down() ) {
            // The sim is stuck outside ask() (a loop that never asks); join() would hang.
            std::printf( "FAIL: no request within %d s; timed-out polls: %s\n", wait_s * fp3d::slow_build,
                         fp3d::poll_report().c_str() );
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
    const bool combat_ok = guns_ok && run_combat_checks() && run_smash_checks() && run_unstoppable_checks();
    const bool range_ok = combat_ok && run_range_op_checks() && run_point_teleport_checks();
    const bool throw_pour_ok = range_ok && run_throw_pour_checks();
    const bool physics_ok = throw_pour_ok && run_item_physics_checks();
    const bool weather_checks_ok = physics_ok && run_weather_mood_checks();
    const bool weather_cheat_ok = weather_checks_ok && run_weather_cheat_checks( [&]() {
        drain();
        return event_counts[static_cast<size_t>( fp3d::event_kind::lightning )];
    } );
    const bool running_ok = weather_cheat_ok && run_running_checks();
    const bool interrupt_ok = running_ok && run_interrupt_checks();
    const bool furniture_ok = interrupt_ok && run_furniture_checks();
    const bool consume_ok = furniture_ok && run_consume_checks();
    const bool offhand_ok = consume_ok && run_offhand_weight_checks();
    const bool lying_ok = offhand_ok && run_lying_container_checks();
    const bool character_ok = lying_ok && run_character_query_checks();
    const bool peek_ok = character_ok && run_peek_checks();
    const bool number_ok = peek_ok && run_number_checks();
    const bool actions_ok = number_ok && run_action_checks();
    const bool gunmods_ok = actions_ok && run_gunmod_checks();
    const bool first_aid_ok = gunmods_ok && run_first_aid_checks();
    const bool driving_ok = first_aid_ok && run_driving_checks();
    const bool lean_ok = driving_ok && run_lean_checks();
    const bool cargo_ok = lean_ok && run_cargo_checks();
    const bool work_ok = cargo_ok && run_vehicle_work_checks();
    const bool map_ok = work_ok && run_overmap_checks();
    const bool names_ok = map_ok && run_name_checks();
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
    // The flash run_weather_mood_checks set off.
    const bool weather_ok = weather_checks_ok && count( event_kind::lightning ) > 0;
    std::printf( "weather and mood: %s (lightning events %zu)\n", weather_ok ? "ok" : "BAD",
                 count( event_kind::lightning ) );
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
    std::printf( "item physics: %s\n", physics_ok ? "ok" : "BAD" );
    std::printf( "weather cheat: %s\n", weather_cheat_ok ? "ok" : "BAD" );
    std::printf( "running: %s\n", running_ok ? "ok" : "BAD" );
    std::printf( "interrupt: %s\n", interrupt_ok ? "ok" : "BAD" );
    std::printf( "furniture: %s\n", furniture_ok ? "ok" : "BAD" );
    std::printf( "consume: %s\n", consume_ok ? "ok" : "BAD" );
    std::printf( "off hand: %s\n", offhand_ok ? "ok" : "BAD" );
    std::printf( "lying containers: %s\n", lying_ok ? "ok" : "BAD" );
    std::printf( "character query: %s\n", character_ok ? "ok" : "BAD" );
    std::printf( "peek: %s\n", peek_ok ? "ok" : "BAD" );
    std::printf( "number requests: %s\n", number_ok ? "ok" : "BAD" );
    std::printf( "actions: %s\n", actions_ok ? "ok" : "BAD" );
    std::printf( "gun mods: %s\n", gunmods_ok ? "ok" : "BAD" );
    std::printf( "first aid: %s\n", first_aid_ok ? "ok" : "BAD" );
    std::printf( "driving: %s\n", driving_ok ? "ok" : "BAD" );
    std::printf( "lean: %s\n", lean_ok ? "ok" : "BAD" );
    std::printf( "cargo: %s\n", cargo_ok ? "ok" : "BAD" );
    std::printf( "vehicle work: %s\n", work_ok ? "ok" : "BAD" );
    std::printf( "map: %s\n", map_ok ? "ok" : "BAD" );
    std::printf( "names: %s\n", names_ok ? "ok" : "BAD" );
    // The real-time turn slot this run settled on (rt_stretch): more than the usual one on a quiet machine means
    // the game's turns got slower.
    std::printf( "real-time slot: %d ms%s\n", 250 * fp3d::slow_build * rt_stretch,
                 rt_stretch > 1 ? " (stretched: turns took longer than the usual slot)" : "" );
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
           resume_ok && throw_pour_ok && worlds_ok && boot_ok && data_check_ok && random_start_ok && weather_ok && physics_ok && weather_cheat_ok && interrupt_ok && furniture_ok && consume_ok && offhand_ok && lying_ok && character_ok &&
           peek_ok && number_ok && actions_ok && names_ok && running_ok && gunmods_ok && first_aid_ok && driving_ok && lean_ok && cargo_ok && work_ok ?
           EXIT_SUCCESS : EXIT_FAILURE;
}
