// FP3D block B1: headless smoke test.
// Boots CDDA with no window, generates a world, loads the reality bubble around the nearest city and dumps
// terrain/furniture ids for a few z-levels as JSON. The output is the first contracts/mock data.
//
// usage: fp3d-headless [out.json] [z_below] [z_above]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "avatar.h"
#include "cached_options.h"
#include "calendar.h"
#include "color.h"
#include "coordinates.h"
#include "debug.h"
#include "fp3d/boot.h"
#include "game.h"
#include "json.h"
#include "map.h"
#include "map_scale_constants.h"
#include "mapdata.h"
#include "options.h"
#include "overmapbuffer.h"
#include "path_info.h"
#include "rng.h"
#include "type_id.h"
#include "weather.h"
#include "worldfactory.h"

static void dump_bubble( const std::string &path, const tripoint_abs_sm &origin, int z_below, int z_above )
{
    map &here = get_map();
    const int size = MAPSIZE * SEEX;
    const int pz = get_avatar().pos_bub().z();
    std::map<std::string, int> ter_index;
    std::map<std::string, int> furn_index;
    std::vector<std::string> ter_names;
    std::vector<std::string> furn_names;
    auto index_of = []( std::map<std::string, int> &idx, std::vector<std::string> &names, const std::string & id ) {
        auto it = idx.find( id );
        if( it != idx.end() ) {
            return it->second;
        }
        names.push_back( id );
        return idx[id] = static_cast<int>( names.size() ) - 1;
    };

    std::ofstream out( path, std::ios::binary );
    if( !out ) {
        throw std::runtime_error( "cannot write " + path );
    }
    JsonOut jo( out );
    jo.start_object();
    jo.member( "contract_version", 0 );
    jo.member( "turn", to_turns<int>( calendar::turn - calendar::turn_zero ) );
    jo.member( "bubble_origin" );
    jo.start_array();
    jo.write( origin.x() );
    jo.write( origin.y() );
    jo.write( origin.z() );
    jo.end_array();
    jo.member( "size", size );
    jo.member( "levels" );
    jo.start_array();
    std::map<std::string, int> ter_counts;
    for( int z = std::max( -OVERMAP_DEPTH, pz - z_below ); z <= std::min( OVERMAP_HEIGHT, pz + z_above ); z++ ) {
        jo.start_object();
        jo.member( "z", z );
        for( const char *layer : { "ter", "furn" } ) {
            const bool is_ter = layer[0] == 't';
            jo.member( layer );
            jo.start_array();
            for( int y = 0; y < size; y++ ) {
                for( int x = 0; x < size; x++ ) {
                    const tripoint_bub_ms p( x, y, z );
                    if( is_ter ) {
                        const std::string &id = here.ter( p ).id().str();
                        ter_counts[id]++;
                        jo.write( index_of( ter_index, ter_names, id ) );
                    } else {
                        jo.write( index_of( furn_index, furn_names, here.furn( p ).id().str() ) );
                    }
                }
            }
            jo.end_array();
        }
        jo.end_object();
    }
    jo.end_array();
    jo.member( "ter_ids", ter_names );
    jo.member( "furn_ids", furn_names );
    jo.end_object();

    std::printf( "wrote %s: %zu terrain ids, %zu furniture ids\n", path.c_str(), ter_names.size(),
                 furn_names.size() );
    std::vector<std::pair<int, std::string>> top;
    for( const auto &[id, n] : ter_counts ) {
        top.emplace_back( n, id );
    }
    std::sort( top.rbegin(), top.rend() );
    for( size_t i = 0; i < std::min<size_t>( 8, top.size() ); i++ ) {
        std::printf( "  %-28s %d\n", top[i].second.c_str(), top[i].first );
    }
}

int main( int argc, char *argv[] )
{
    const std::string out_path = argc > 1 ? argv[1] : "fp3d_bubble.json";
    // Bounded: at most the full z range.
    const int z_below = argc > 2 ? std::clamp( std::atoi( argv[2] ), 0, OVERMAP_DEPTH * 2 ) : 1;
    const int z_above = argc > 3 ? std::clamp( std::atoi( argv[3] ), 0, OVERMAP_HEIGHT * 2 ) : 2;

    test_mode = true;
    setupDebug( DebugOutput::std_err );
    rng_set_engine_seed( 42 );
    try {
        fp3d::boot_world( "fp3d-smoke", false );
        const tripoint_abs_sm origin = fp3d::load_bubble_in_town();
        dump_bubble( out_path, origin, z_below, z_above );
    } catch( const std::exception &e ) {
        std::fprintf( stderr, "fp3d-headless failed: %s\n", e.what() );
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
