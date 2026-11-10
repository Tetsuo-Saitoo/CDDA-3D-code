// FP3D: fills CDDA's JSON parse cache for a read-only install (the Linux AppImage), so the game doesn't parse
// every JSON file again on each start. Every .json file under <cdda>/data goes into <cdda>/data/cache and every one
// under <cdda>/gfx into <cdda>/cache/gfx, the places CDDA itself caches them.
// CDDA uses a cache entry only while its JSON file still has the modification time it was cached with (to the
// millisecond), so run this after the files have their final times (squashfs keeps whole seconds: round them first).
// A stale or missing entry is parsed from the JSON as usual. Existing cache folders are replaced. Fails when any
// file can't be cached.
//
// usage: fp3d-json-cache <cdda folder>      (the folder that holds data/ and gfx/)
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "cata_path.h"
#include "debug.h"
#include "json_loader.h"
#include "path_info.h"

namespace fs = std::filesystem;

namespace
{

constexpr size_t max_files = 100000;

// .json files below `dir`, relative to `root`; false if the folder can't be read or holds too many.
bool json_files( const fs::path &dir, const fs::path &root, std::vector<fs::path> &out )
{
    std::error_code ec;
    if( !fs::is_directory( dir, ec ) ) {
        return !ec;   // no gfx folder: nothing to cache there
    }
    fs::recursive_directory_iterator it( dir, fs::directory_options::none, ec );
    for( ; !ec && it != fs::recursive_directory_iterator(); it.increment( ec ) ) {
        if( it->is_directory( ec ) && it->path().filename() == "cache" && it->path().parent_path() == dir ) {
            it.disable_recursion_pending();
            continue;
        }
        if( it->is_regular_file( ec ) && it->path().extension() == ".json" ) {
            if( out.size() >= max_files ) {
                return false;
            }
            out.push_back( it->path().lexically_relative( root ) );
        }
    }
    return !ec;
}

} // namespace

int main( int argc, char **argv )
{
    if( argc != 2 ) {
        std::fprintf( stderr, "usage: fp3d-json-cache <cdda folder>\n" );
        return 2;
    }
    std::error_code ec;
    const fs::path base = fs::absolute( fs::u8path( argv[1] ), ec ).lexically_normal();
    if( ec || !fs::is_directory( base / "data", ec ) ) {
        std::fprintf( stderr, "fp3d-json-cache: no data folder in %s\n", argv[1] );
        return 2;
    }
    // A fresh cache: entries left from other file times would only be found stale and deleted at the next start.
    for( const fs::path &old : { base / "data" / "cache", base / "cache" } ) {
        fs::remove_all( old, ec );
        if( ec ) {
            std::fprintf( stderr, "fp3d-json-cache: cannot clear the old cache %s: %s\n",
                          old.generic_u8string().c_str(), ec.message().c_str() );
            return 1;
        }
    }
    // CDDA's path setup wants a user folder. Nothing is written there; it is inside <cdda> anyway and removed at
    // the end, so nothing ever lands outside <cdda>.
    const fs::path user = base / ".fp3d-json-cache-user";
    std::string base_s = base.generic_u8string();
    if( base_s.back() != '/' ) {
        base_s += '/';
    }
    PATH_INFO::init_base_path( base_s );
    PATH_INFO::init_user_dir( user.generic_u8string() + "/" );
    PATH_INFO::set_standard_filenames();
    setupDebug( DebugOutput::std_err );

    std::vector<fs::path> data_files;
    std::vector<fs::path> gfx_files;
    if( !json_files( base / "data", base / "data", data_files ) || !json_files( base / "gfx", base, gfx_files ) ) {
        std::fprintf( stderr, "fp3d-json-cache: cannot list %s\n", base_s.c_str() );
        return 1;
    }
    size_t cached = 0;
    size_t bad = 0;
    const auto cache = [&]( const cata_path & p ) {
        try {
            json_loader::from_path( p );
            cached++;
        } catch( const std::exception &e ) {
            bad++;
            std::fprintf( stderr, "  not cached (CDDA will parse it at start): %s: %s\n",
                          p.generic_u8string().c_str(), e.what() );
        }
    };
    for( const fs::path &f : data_files ) {
        cache( PATH_INFO::datadir_path() / f );
    }
    for( const fs::path &f : gfx_files ) {
        cache( cata_path( cata_path::root_path::base, f ) );
    }
    fs::remove_all( user, ec );
    std::printf( "fp3d-json-cache: %zu files cached, %zu not\n", cached, bad );
    // Every shipped JSON file should parse: one that doesn't is a data problem to fix before release.
    return cached > 0 && bad == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
