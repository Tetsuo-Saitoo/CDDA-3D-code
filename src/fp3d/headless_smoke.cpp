// FP3D block B1: headless smoke test.
// Boots CDDA with no window, generates a world, loads the reality bubble around the nearest city and dumps
// a snapshot (block B3 format, contracts/snapshot.md) of a few z-levels as JSON: the contracts/mock data.
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
#include "fp3d/snapshot.h"
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

static void dump_bubble( const std::string &path, int z_below, int z_above )
{
    const int pz = get_avatar().posz();
    const std::shared_ptr<const fp3d::snapshot> snap = fp3d::capture_snapshot();
    std::ofstream out( path, std::ios::binary );
    if( !out ) {
        throw std::runtime_error( "cannot write " + path );
    }
    fp3d::write_snapshot_json( out, *snap, std::max( -OVERMAP_DEPTH, pz - z_below ),
                               std::min( OVERMAP_HEIGHT, pz + z_above ) );
    out.close();
    if( !out ) {
        throw std::runtime_error( "write failed: " + path );
    }

    std::map<uint32_t, int> ter_counts;
    for( const std::shared_ptr<const fp3d::submap_view> &sm : snap->submaps ) {
        if( sm->pos.z >= pz - z_below && sm->pos.z <= pz + z_above ) {
            for( uint32_t t : sm->ter ) {
                ter_counts[t]++;
            }
        }
    }
    std::printf( "wrote %s: %zu submaps in the bubble, %zu terrain ids in z %d..%d\n", path.c_str(),
                 snap->submaps.size(), ter_counts.size(), pz - z_below, pz + z_above );
    std::vector<std::pair<int, uint32_t>> top;
    for( const auto &[id, n] : ter_counts ) {
        top.emplace_back( n, id );
    }
    std::sort( top.rbegin(), top.rend() );
    for( size_t i = 0; i < std::min<size_t>( 8, top.size() ); i++ ) {
        std::printf( "  %-28s %d\n", fp3d::cdda_id_name( "ter", top[i].second ).c_str(), top[i].first );
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
        // Noon, so the mock has daylight: seen tiles, item piles, creatures.
        calendar::turn = calendar::turn_zero + 12_hours;
        fp3d::load_bubble_in_town();
        dump_bubble( out_path, z_below, z_above );
    } catch( const std::exception &e ) {
        std::fprintf( stderr, "fp3d-headless failed: %s\n", e.what() );
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
