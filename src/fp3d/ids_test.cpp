// FP3D block B6 test: boots a town and checks the id tables against CDDA's own factories (sizes, null ids,
// wall connect groups, door open/close pairs), that session indices are sorted and survive a rebuild, that
// every id in a snapshot resolves, and that the JSON parses back. Exit code 0 = all checks pass.
//
// usage: fp3d-ids-test   (run from the cdda/ folder)
#include <algorithm>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "cached_options.h"
#include "debug.h"
#include "fp3d/boot.h"
#include "fp3d/id_tables.h"
#include "fp3d/item_hands.h"
#include "fp3d/snapshot.h"
#include "itype.h"
#include "json.h"
#include "json_loader.h"
#include "mapdata.h"
#include "rng.h"
#include "trap.h"
#include "type_id.h"

namespace
{
int failures = 0;

void check( bool ok, const char *what, const std::string &detail = "" )
{
    std::printf( "  %-50s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
    failures += ok ? 0 : 1;
}

bool has( const std::vector<std::string> &v, const std::string &s )
{
    return std::find( v.begin(), v.end(), s ) != v.end();
}

const fp3d::terrain_def &ter_def( const fp3d::id_tables &t, const char *id )
{
    return t.ter[ter_str_id( id ).id().to_i()];
}
} // namespace

int main()
{
    std::setvbuf( stdout, nullptr, _IONBF, 0 );
    test_mode = true;
    setupDebug( DebugOutput::std_err );
    rng_set_engine_seed( 5 );
    // This test's own user folder (config, saves, caches): what another test binary leaves behind never
    // reaches it.
    try {
        fp3d::init_paths( "", "./fp3d_user_ids" );
    } catch( const std::exception &e ) {
        std::printf( "error: %s\n", e.what() );
        return EXIT_FAILURE;
    }
    try {
        fp3d::boot_world( "fp3d-ids", false );
        const std::shared_ptr<const fp3d::id_tables> t = fp3d::latest_id_tables();
        if( !t ) {
            check( false, "boot builds the id tables" );
            return EXIT_FAILURE;
        }
        check( t->ter.size() == ter_t::count() && t->furn.size() == furn_t::count() &&
               t->traps.size() == trap::count() && t->ter[0].id == "t_null" && t->furn[0].id == "f_null" &&
               t->traps[0].id == "tr_null", "ter/furn/trap: CDDA int ids, 0 = null",
               std::to_string( t->ter.size() ) + " ter, " + std::to_string( t->furn.size() ) + " furn" );

        const fp3d::terrain_def &wall = ter_def( *t, "t_wall" );
        check( has( wall.connect_groups, "WALL" ) && has( wall.connects_to, "WALL" ) && wall.move_cost == 0,
               "t_wall: WALL group, joins walls, impassable" );
        const fp3d::terrain_def &door = ter_def( *t, "t_door_c" );
        check( door.open == "t_door_o" && ter_def( *t, "t_door_o" ).close == "t_door_c",
               "doors: open / close pair" );
        const fp3d::terrain_def &floor = ter_def( *t, "t_floor" );
        check( has( floor.flags, "FLAT" ) && floor.move_cost > 0 && !floor.name.empty(), "t_floor: FLAT, walkable, named" );

        const std::vector<std::string> items = fp3d::id_names( fp3d::id_kind::itype );
        check( items.size() > 1000 && items[0].empty() && std::is_sorted( items.begin() + 1, items.end() ) &&
               t->items.size() == items.size(), "itype table pre-filled and sorted",
               std::to_string( items.size() - 1 ) + " items" );
        const uint32_t apple = fp3d::intern_id( fp3d::id_kind::itype, "apple" );
        const uint32_t zombie = fp3d::intern_id( fp3d::id_kind::mtype, "mon_zombie" );
        check( apple < t->items.size() && t->items[apple].id == "apple" && t->items[apple].volume_ml > 0 &&
               zombie < t->monsters.size() && t->monsters[zombie].hp > 0 && t->monsters[zombie].speed > 0 &&
               t->vparts.size() > 100 && !t->vparts[1].id.empty(), "apple, mon_zombie, vehicle parts defined" );
        // The examine action's id goes with terrain and furniture: a fireplace's, none for a floor, an actor's type.
        const auto examine_of = []( const std::vector<fp3d::terrain_def> &defs, const std::string & id ) {
            for( const fp3d::terrain_def &d : defs ) {
                if( d.id == id ) {
                    return d.examine;
                }
            }
            return std::string( "not in the table" );
        };
        check( examine_of( t->furn, "f_fireplace" ) == "fireplace" && examine_of( t->ter, "t_floor" ).empty() &&
               examine_of( t->ter, "t_card_LIXA" ) == "cardreader",
               "examine ids: fireplace, none for a floor, an actor's type",
               examine_of( t->furn, "f_fireplace" ) + " / " + examine_of( t->ter, "t_floor" ) + " / " +
               examine_of( t->ter, "t_card_LIXA" ) );
        // CDDA's phase goes with each item type: water is a liquid, an apple a solid.
        const uint32_t water = fp3d::intern_id( fp3d::id_kind::itype, "water" );
        check( water < t->items.size() && t->items[water].phase == "liquid" && apple < t->items.size() &&
               t->items[apple].phase == "solid", "item phase: water liquid, apple solid",
               water < t->items.size() ? t->items[water].phase : "water not in the table" );

        // Gun mods by hand: where a gun mod goes, and a gun type's own slots.
        const uint32_t glock = fp3d::intern_id( fp3d::id_kind::itype, "glock_19" );
        const uint32_t laser = fp3d::intern_id( fp3d::id_kind::itype, "rail_laser_sight" );
        const auto slots_of = [&]( uint32_t type, const std::string & where ) {
            if( type >= t->items.size() ) {
                return -1;
            }
            for( const std::pair<std::string, int> &slot : t->items[type].mod_locations ) {
                if( slot.first == where ) {
                    return slot.second;
                }
            }
            return 0;
        };
        check( laser < t->items.size() && t->items[laser].gunmod_location == "rail" &&
               t->items[laser].mod_locations.empty() && slots_of( glock, "rail" ) == 1 &&
               slots_of( glock, "mechanism" ) == 2 && slots_of( glock, "stock mount" ) == 1 &&
               slots_of( glock, "underbarrel" ) == 0 && t->items[glock].gunmod_location.empty() &&
               t->items[apple].gunmod_location.empty() && t->items[apple].mod_locations.empty(),
               "gun mods: a mod's location, a gun's slots",
               laser < t->items.size() ? t->items[laser].gunmod_location + ", rail x" +
               std::to_string( slots_of( glock, "rail" ) ) : "laser sight not in the table" );
        check( fp3d::gunmod_location_text( "stock mount" ) == "stock mount" &&
               fp3d::gunmod_location_text( "bad<id>" ).empty() && fp3d::gunmod_location_text( "" ).empty() &&
               fp3d::gunmod_location_text( std::string( fp3d::max_gunmod_location_bytes, 'a' ) ).size() ==
               fp3d::max_gunmod_location_bytes &&
               fp3d::gunmod_location_text( std::string( fp3d::max_gunmod_location_bytes + 1, 'a' ) ).empty(),
               "gun mods: only plain location ids" );

        // First aid: what an item with CDDA's heal use treats; nothing for a pill or an apple.
        const uint32_t bandage = fp3d::intern_id( fp3d::id_kind::itype, "bandages" );
        const uint32_t disinfectant = fp3d::intern_id( fp3d::id_kind::itype, "disinfectant" );
        const uint32_t aspirin = fp3d::intern_id( fp3d::id_kind::itype, "aspirin" );
        const auto heal_of = [&]( uint32_t type ) {
            return type < t->items.size() ? t->items[type].heal : -2;
        };
        check( heal_of( bandage ) == ( fp3d::heal_bandages | fp3d::heal_stops_bleeding ) &&
               heal_of( disinfectant ) == ( fp3d::heal_disinfects | fp3d::heal_treats_bite ) &&
               heal_of( aspirin ) == -1 && heal_of( apple ) == -1,
               "first aid: what a bandage and a disinfectant treat, none for a pill",
               std::to_string( heal_of( bandage ) ) + " / " + std::to_string( heal_of( disinfectant ) ) + " / " +
               std::to_string( heal_of( aspirin ) ) );

        // Pockets per item type: a plastic bottle has one rigid, watertight, sealable 500 ml pocket.
        const uint32_t bottle = fp3d::intern_id( fp3d::id_kind::itype, "bottle_plastic" );
        const bool bottle_ok = bottle < t->items.size() && t->items[bottle].pockets.size() == 1;
        const fp3d::pocket_def bp = bottle_ok ? t->items[bottle].pockets[0] : fp3d::pocket_def();
        const uint16_t want = fp3d::pocket_rigid | fp3d::pocket_watertight | fp3d::pocket_sealable;
        check( bottle_ok && bp.volume_ml == 500 && bp.max_weight_g == 1250 && ( bp.flags & want ) == want &&
               !( bp.flags & fp3d::pocket_ammo_only ) && t->items[apple].pockets.empty(),
               "pockets: bottle_plastic one 500 ml watertight pocket, apple none",
               std::to_string( bp.volume_ml ) + " ml flags " + std::to_string( bp.flags ) );

        const uint32_t zdog = fp3d::intern_id( fp3d::id_kind::mtype, "mon_dog_zombie_cop" );
        check( t->monsters[zombie].bodytype == "human" && zdog < t->monsters.size() &&
               t->monsters[zdog].bodytype == "dog", "monster body types (zombie human, zombie dog dog)",
               zdog < t->monsters.size() ? t->monsters[zdog].bodytype : "no mon_dog_zombie_cop" );

        // A rebuild (another world load) keeps the indices for the same data, with a new generation.
        fp3d::build_id_tables();
        const std::shared_ptr<const fp3d::id_tables> t2 = fp3d::latest_id_tables();
        check( t2->generation == t->generation + 1 &&
               fp3d::intern_id( fp3d::id_kind::itype, "apple" ) == apple &&
               fp3d::intern_id( fp3d::id_kind::mtype, "mon_zombie" ) == zombie, "rebuild: same indices, next generation" );

        fp3d::load_bubble_in_town();
        const std::shared_ptr<const fp3d::snapshot> s = fp3d::capture_snapshot();
        size_t bad = 0;
        for( const auto &sm : s->submaps ) {
            for( int i = 0; i < fp3d::sm_tiles; i++ ) {
                bad += sm->ter[i] >= t2->ter.size() || sm->furn[i] >= t2->furn.size() ||
                       sm->trap[i] >= t2->traps.size() ? 1 : 0;
            }
            for( const fp3d::item_pile_view &p : sm->items ) {
                for( uint32_t top : p.top ) {
                    bad += top >= t2->items.size() || ( top != 0 && t2->items[top].id.empty() ) ? 1 : 0;
                }
            }
            for( const fp3d::field_view &f : sm->fields ) {
                bad += f.type >= t2->fields.size() ? 1 : 0;
            }
        }
        for( const fp3d::creature_view &c : s->creatures ) {
            bad += c.type >= t2->monsters.size() ? 1 : 0;
        }
        check( bad == 0 && s->ids_generation == t2->generation, "every snapshot id resolves",
               std::to_string( bad ) + " unresolved" );

        std::ostringstream os;
        fp3d::write_id_tables_json( os, *t2 );
        const std::string text = os.str();
        check( text.find( "\"pockets\":[{\"volume_ml\":500,\"max_weight_g\":1250," ) != std::string::npos,
               "pockets in the JSON" );
        check( text.find( "\"gunmod_location\":\"rail\"" ) != std::string::npos &&
               text.find( "\"mod_locations\":[[\"barrel\",1],[\"mechanism\",2],[\"rail\",1],"
                          "[\"sights mount\",1],[\"stock mount\",1]]" ) != std::string::npos,
               "gun mod locations in the JSON" );
        // "heal" only where the item has the use: as many times as there are such items, the bandage's among them.
        size_t healing_items = 0;
        for( const fp3d::item_def &d : t2->items ) {
            healing_items += d.heal >= 0 ? 1 : 0;
        }
        size_t heal_members = 0;
        for( size_t at = text.find( "\"heal\":" ); at != std::string::npos; at = text.find( "\"heal\":", at + 1 ) ) {
            heal_members++;
        }
        check( healing_items > 5 && heal_members == healing_items &&
               text.find( "\"id\":\"bandages\"" ) != std::string::npos &&
               text.find( "\"heal\":5,\"volume_ml\":27," ) != std::string::npos, "first aid in the JSON",
               std::to_string( healing_items ) + " items, " + std::to_string( heal_members ) + " members" );
        JsonValue root = json_loader::from_string( text );
        JsonObject jo = root.get_object();
        jo.allow_omitted_members();
        JsonArray ter = jo.get_array( "ter" );
        JsonArray itype = jo.get_array( "itype" );
        JsonObject tables = jo.get_object( "tables" );
        tables.allow_omitted_members();
        check( ter.size() == t2->ter.size() && itype.size() == t2->items.size() &&
               tables.get_array( "itype" ).size() == items.size() &&
               jo.get_int( "generation" ) == static_cast<int>( t2->generation ),
               "JSON parses back with the same sizes", std::to_string( text.size() / 1024 ) + " KB" );
        {
            // Game data is not trusted to stay small (a mod's gun): more slot kinds than the cap, a slot count far
            // beyond it, kinds with no slots and location ids that aren't plain. The tables keep the first 16 plain
            // kinds that have slots, each with 16 slots at most.
            // (`itype` is also a local above, the JSON array: hence `struct`.)
            struct itype &odd = const_cast<struct itype &>( *itype_id( "glock_19" ) );
            const std::map<gunmod_location, int> real = odd.gun->valid_mod_locations;
            odd.gun->valid_mod_locations.clear();
            odd.gun->valid_mod_locations[gunmod_location( "aaa big" )] = 500;
            odd.gun->valid_mod_locations[gunmod_location( "aab<bad>" )] = 1;
            odd.gun->valid_mod_locations[gunmod_location( "aac none" )] = 0;
            odd.gun->valid_mod_locations[gunmod_location( std::string( 40, 'c' ) )] = 1;
            for( int i = 0; i < 20; i++ ) {
                odd.gun->valid_mod_locations[gunmod_location( "slot " + std::to_string( 10 + i ) )] = 1;
            }
            fp3d::build_id_tables();
            const std::shared_ptr<const fp3d::id_tables> t3 = fp3d::latest_id_tables();
            bool plain = glock < t3->items.size();
            int most = 0;
            size_t kinds = 0;
            std::string first;
            if( plain ) {
                const std::vector<std::pair<std::string, int>> &got = t3->items[glock].mod_locations;
                kinds = got.size();
                first = got.empty() ? std::string() : got.front().first;
                for( const std::pair<std::string, int> &slot : got ) {
                    plain = plain && !slot.first.empty() && fp3d::gunmod_location_text( slot.first ) == slot.first &&
                            slot.second >= 1;
                    most = std::max( most, slot.second );
                }
            }
            check( plain && kinds == fp3d::max_mod_locations && most == fp3d::max_mod_slots && first == "aaa big",
                   "gun mods: a gun with oversized data is cut to the caps",
                   std::to_string( kinds ) + " kinds, at most " + std::to_string( most ) + " slots, first " + first );
            odd.gun->valid_mod_locations = real;
            fp3d::build_id_tables();
            const std::shared_ptr<const fp3d::id_tables> t4 = fp3d::latest_id_tables();
            check( glock < t4->items.size() && t4->items[glock].mod_locations.size() == 5,
                   "gun mods: and the real gun's slots are back" );
        }
    } catch( const std::exception &e ) {
        std::printf( "error: %s\n", e.what() );
        return EXIT_FAILURE;
    }
    std::printf( "%s (%d failed)\n", failures == 0 ? "all checks passed" : "FAILED", failures );
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
