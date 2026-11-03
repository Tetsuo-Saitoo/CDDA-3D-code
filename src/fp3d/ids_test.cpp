// FP3D block B6 test: boots a town and checks the id tables against CDDA's own factories (sizes, null ids,
// wall connect groups, door open/close pairs), that session indices are sorted and survive a rebuild, that
// every id in a snapshot resolves, and that the JSON parses back. Exit code 0 = all checks pass.
//
// usage: fp3d-ids-test   (run from the cdda/ folder)
#include <algorithm>
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
#include "fp3d/snapshot.h"
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
    } catch( const std::exception &e ) {
        std::printf( "error: %s\n", e.what() );
        return EXIT_FAILURE;
    }
    std::printf( "%s (%d failed)\n", failures == 0 ? "all checks passed" : "FAILED", failures );
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
