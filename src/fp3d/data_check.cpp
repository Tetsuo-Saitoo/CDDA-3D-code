#include "fp3d/data_check.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string_view>
#include <system_error>

#include "cata_path.h"
#include "debug.h"
#include "get_version.h"
#include "mod_manager.h"
#include "options.h"
#include "path_info.h"
#include "string_formatter.h"

namespace fp3d
{

namespace
{

namespace fs = std::filesystem;

constexpr std::string_view marker_header = "fp3d data check 1";
constexpr size_t max_marker_bytes = 4096;
constexpr size_t max_fingerprints = 8;
constexpr size_t fingerprint_chars = 32;
// Files looked at per fingerprint (CDDA's data is ~3000 files with every mod folder): more means check every time.
constexpr size_t max_files = 100000;

std::string pending;    // fingerprint to remember once the check has passed
bool skipped = false;   // this boot skipped the check

uint64_t splitmix( uint64_t x )
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

// Two independent 64-bit hashes (FNV-1a and a splitmix chain): 128 bits against chance matches between data sets.
struct hash128 {
    uint64_t a = 1469598103934665603ULL;
    uint64_t b = 0x9e3779b97f4a7c15ULL;
    void add( std::string_view s ) {
        for( const char ch : s ) {
            const uint64_t c = static_cast<unsigned char>( ch );
            a = ( a ^ c ) * 1099511628211ULL;
            b = splitmix( b ^ ( c + 0x100 ) );
        }
    }
    std::string hex() const {
        return string_format( "%016llx%016llx", static_cast<unsigned long long>( a ),
                              static_cast<unsigned long long>( b ) );
    }
};

// `p` inside `root` (both absolute or both relative, compared lexically): the path below root, else nullopt.
std::optional<fs::path> inside( const fs::path &p, const fs::path &root )
{
    const fs::path rel = p.lexically_normal().lexically_relative( root.lexically_normal() );
    if( rel.empty() ) {
        return std::nullopt;
    }
    const std::string first = rel.begin()->u8string();
    if( first == ".." ) {
        return std::nullopt;
    }
    return rel;
}

// Appends "path<TAB>size<TAB>mtime" for every regular file below `dir` (paths relative to `data_root`). False when
// the folder can't be read completely or holds too many files.
bool list_files( const fs::path &dir, const fs::path &data_root, std::vector<std::string> &out )
{
    std::error_code ec;
    if( !fs::is_directory( dir, ec ) || ec ) {
        return false;
    }
    fs::recursive_directory_iterator it( dir, fs::directory_options::none, ec );
    for( ; !ec && it != fs::recursive_directory_iterator(); it.increment( ec ) ) {
        if( out.size() >= max_files ) {
            return false;
        }
        const fs::directory_entry &e = *it;
        std::error_code fec;
        if( !e.is_regular_file( fec ) || fec ) {
            if( fec ) {
                return false;
            }
            continue;
        }
        const uintmax_t size = e.file_size( fec );
        const fs::file_time_type mtime = e.last_write_time( fec );
        const std::optional<fs::path> rel = inside( e.path(), data_root );
        if( fec || !rel ) {
            return false;
        }
        out.push_back( string_format( "%s\t%llu\t%lld", rel->generic_u8string(),
                                      static_cast<unsigned long long>( size ),
                                      static_cast<long long>( mtime.time_since_epoch().count() ) ) );
    }
    return !ec;
}

bool is_fingerprint( std::string_view s )
{
    return s.size() == fingerprint_chars && std::all_of( s.begin(), s.end(), []( char c ) {
        return ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' );
    } );
}

// The fingerprints in the marker; empty when it is missing or anything in it is off.
std::vector<std::string> read_marker()
{
    std::vector<std::string> out;
    std::ifstream f( fs::u8path( data_check_marker_path() ), std::ios::binary );
    if( !f ) {
        return out;
    }
    std::string text( max_marker_bytes + 1, '\0' );
    f.read( text.data(), static_cast<std::streamsize>( text.size() ) );
    text.resize( static_cast<size_t>( f.gcount() ) );
    if( text.size() > max_marker_bytes ) {
        return out;
    }
    std::vector<std::string> lines;
    size_t start = 0;
    while( start < text.size() ) {
        size_t end = text.find( '\n', start );
        if( end == std::string::npos ) {
            end = text.size();
        }
        std::string line = text.substr( start, end - start );
        if( !line.empty() && line.back() == '\r' ) {
            line.pop_back();
        }
        lines.push_back( line );
        start = end + 1;
    }
    if( lines.empty() || lines.front() != marker_header || lines.size() > max_fingerprints + 1 ) {
        return out;
    }
    for( size_t i = 1; i < lines.size(); i++ ) {
        if( !is_fingerprint( lines[i] ) ) {
            return {};
        }
        out.push_back( lines[i] );
    }
    return out;
}

void write_marker( const std::string &fingerprint )
{
    std::vector<std::string> kept{ fingerprint };
    for( const std::string &old : read_marker() ) {
        if( old != fingerprint && kept.size() < max_fingerprints ) {
            kept.push_back( old );
        }
    }
    const fs::path path = fs::u8path( data_check_marker_path() );
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f( tmp, std::ios::binary | std::ios::trunc );
        f << marker_header << '\n';
        for( const std::string &k : kept ) {
            f << k << '\n';
        }
        if( !f ) {
            DebugLog( D_WARNING, DC_ALL ) << "[fp3d] data check: cannot write " << tmp.u8string();
            return;
        }
    }
    std::error_code ec;
    fs::rename( tmp, path, ec );
    if( ec ) {
        fs::remove( tmp, ec );
        DebugLog( D_WARNING, DC_ALL ) << "[fp3d] data check: cannot write " << path.u8string();
    }
}

} // namespace

std::string data_check_marker_path()
{
    return PATH_INFO::config_dir() + "fp3d_data_checked.txt";
}

std::optional<std::string> data_fingerprint( const std::vector<mod_id> &mods_in )
{
    // As game::load_world_modfiles: duplicates dropped (first kept), dda first when no core mod is listed.
    std::vector<mod_id> mods;
    std::set<mod_id> seen;
    for( const mod_id &m : mods_in ) {
        if( seen.insert( m ).second ) {
            mods.push_back( m );
        }
    }
    if( std::none_of( mods.begin(), mods.end(), []( const mod_id & m ) {
    return m.is_valid() && m->core;
} ) ) {
        mods.insert( mods.begin(), mod_id( "dda" ) );
    }
    const fs::path data_root = PATH_INFO::datadir_path().get_unrelative_path();
    // The world's own mods folder (load_world_modfiles loads it too) is the player's: check every time.
    // An empty one is fine; anything else (files in it, not a folder, unreadable) means check every time.
    const fs::path world_mods = ( PATH_INFO::world_base_save_path() / "mods" ).get_unrelative_path();
    std::error_code ec;
    const fs::file_status st = fs::symlink_status( world_mods, ec );
    if( ec && st.type() != fs::file_type::not_found ) {
        return std::nullopt;
    }
    if( st.type() != fs::file_type::not_found ) {
        if( st.type() != fs::file_type::directory ) {
            return std::nullopt;
        }
        const fs::directory_iterator first( world_mods, ec );
        if( ec || first != fs::directory_iterator() ) {
            return std::nullopt;
        }
    }
    hash128 h;
    h.add( marker_header );
    h.add( "\n" );
    h.add( getVersionString() );
    h.add( "\n" );
    std::vector<std::string> files;
    if( !list_files( PATH_INFO::jsondir().get_unrelative_path(), data_root, files ) ) {
        return std::nullopt;
    }
    for( const mod_id &m : mods ) {
        if( !m.is_valid() ) {
            return std::nullopt;
        }
        const fs::path dir = m->path.get_unrelative_path();
        if( !inside( dir, data_root ) ) {
            return std::nullopt;   // a mod from the user folder (or elsewhere)
        }
        h.add( m.str() );
        h.add( "\n" );
        if( !list_files( dir, data_root, files ) ) {
            return std::nullopt;
        }
    }
    std::sort( files.begin(), files.end() );
    files.erase( std::unique( files.begin(), files.end() ), files.end() );
    for( const std::string &f : files ) {
        h.add( f );
        h.add( "\n" );
    }
    return h.hex();
}

void prepare_data_check( const std::vector<mod_id> &mods )
{
    pending.clear();
    skipped = false;
    if( get_option<bool>( "SKIP_VERIFICATION" ) ) {
        return;   // the player's own choice
    }
    const std::optional<std::string> fp = data_fingerprint( mods );
    if( !fp ) {
        DebugLog( D_INFO, DC_ALL ) << "[fp3d] data check: runs every boot for these mods";
        return;
    }
    const std::vector<std::string> passed = read_marker();
    if( std::find( passed.begin(), passed.end(), *fp ) != passed.end() ) {
        get_options().get_option( "SKIP_VERIFICATION" ).setValue( "true" );   // in memory only
        skipped = true;
        DebugLog( D_INFO, DC_ALL ) << "[fp3d] data check: skipped, this data passed before";
        return;
    }
    pending = *fp;
}

void finish_data_check()
{
    if( skipped ) {
        // Back to the player's setting for anything that loads data later in this game.
        get_options().get_option( "SKIP_VERIFICATION" ).setValue( "false" );
    }
    if( pending.empty() ) {
        return;
    }
    const std::string fp = pending;
    pending.clear();
    if( debug_has_error_been_observed() ) {
        DebugLog( D_INFO, DC_ALL ) << "[fp3d] data check: CDDA reported errors, so it runs again next boot";
        return;
    }
    write_marker( fp );
    DebugLog( D_INFO, DC_ALL ) << "[fp3d] data check: passed, remembered for the next boots";
}

bool data_check_skipped()
{
    return skipped;
}

} // namespace fp3d
