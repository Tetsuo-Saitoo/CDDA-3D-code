#include "fp3d/chargen.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include "avatar.h"
#include "character.h"
#include "fp3d/bridge.h"
#include "game_constants.h"
#include "json.h"
#include "json_loader.h"
#include "mutation.h"
#include "options.h"
#include "path_info.h"
#include "player_difficulty.h"
#include "profession.h"
#include "scenario.h"
#include "skill.h"
#include "worldfactory.h"

namespace fp3d
{
namespace
{
// As newcharacter.cpp (its helpers are file-local there).
constexpr std::array<int, 11> skill_costs = { 0, 1, 1, 2, 4, 6, 9, 12, 16, 20, 25 };
constexpr int min_stat = 4;
constexpr int freeform_max_stat = 20;
constexpr size_t max_name_bytes = 64;
constexpr size_t max_list = 512;
constexpr int min_age = 16;
constexpr int max_age = 55;

bool multi_pool()
{
    return get_option<std::string>( "CHARACTER_POINT_POOLS" ) == "multi_pool";
}

int stat_pool()
{
    return 4 * 8 + get_option<int>( "INITIAL_STAT_POINTS" );
}

bool scenario_listed( const scenario &s )
{
    return !s.scen_is_blacklisted();
}

const scenario *find_scenario( const std::string &id )
{
    for( const scenario &s : scenario::get_all() ) {
        if( s.ident().str() == id && scenario_listed( s ) ) {
            return &s;
        }
    }
    return nullptr;
}

bool trait_listed( const mutation_branch &m, const scenario &s )
{
    return !mutation_branch::trait_is_blacklisted( m.id ) && ( m.startingtrait || s.traitquery( m.id ) );
}

void write_strings( JsonOut &jo, const char *name, const std::vector<std::string> &v )
{
    jo.member( name );
    jo.start_array();
    for( const std::string &s : v ) {
        jo.write( s );
    }
    jo.end_array();
}

void write_profession( JsonOut &jo, const profession &p )
{
    jo.start_object();
    jo.member( "id", p.ident().str() );
    jo.member( "name", p.gender_appropriate_name( true ) );
    jo.member( "name_female", p.gender_appropriate_name( false ) );
    jo.member( "description", p.description( true ) );
    jo.member( "points", p.point_cost() );
    std::vector<std::string> locked;
    for( const trait_and_var &t : p.get_locked_traits() ) {
        locked.push_back( t.trait.str() );
    }
    write_strings( jo, "locked_traits", locked );
    jo.member( "skills" );
    jo.start_object();
    for( const profession::StartingSkill &sk : p.skills() ) {
        jo.member( sk.first.str(), sk.second );
    }
    jo.end_object();
    jo.end_object();
}

int stat_points_used( const Character &u )
{
    int used = 0;
    for( int stat : { u.str_max, u.dex_max, u.int_max, u.per_max } ) {
        used += stat + std::max( 0, stat - HIGH_STAT );
    }
    return used;
}

bool locked_for( const Character &u, const trait_id &t )
{
    bool locked = get_scenario()->is_locked_trait( t ) || u.prof->is_locked_trait( t );
    for( const profession *h : u.hobbies ) {
        locked = locked || h->is_locked_trait( t );
    }
    return locked;
}

int trait_points_used( const Character &u )
{
    int used = 0;
    for( const trait_id &t : u.get_mutations( true ) ) {
        if( !locked_for( u, t ) ) {
            used += t->points;
        }
    }
    return used;
}

int skill_points_used( const Character &u )
{
    int used = get_scenario()->point_cost() + u.prof->point_cost();
    for( const profession *h : u.hobbies ) {
        used += h->point_cost();
    }
    for( const Skill &sk : Skill::skills ) {
        const int lvl = std::clamp<int>( u.get_skill_level( sk.ident() ), 0, skill_costs.size() - 1 );
        used += skill_costs[lvl];
    }
    return used;
}

// newcharacter.cpp's reset_scenario (file-local there).
void reset_to_scenario( avatar &u, const scenario &scen, const profession &prof )
{
    u.random_start_location = true;
    u.str_max = 8;
    u.dex_max = 8;
    u.int_max = 8;
    u.per_max = 8;
    set_scenario( &scen );
    u.prof = &prof;
    u.hobbies.clear();
    u.clear_mutations();
    u.recalc_hp();
    u.empty_skills();
    u.add_traits();
}

constexpr size_t max_templates = 256;
// Bigger template files are skipped (not listed, not loaded). Transfer templates hold a played character with
// all it carries, so this leaves room for those.
constexpr std::uintmax_t max_template_bytes = 4 * 1024 * 1024;

bool ascii_alnum( char ch )
{
    return ( ch >= 'a' && ch <= 'z' ) || ( ch >= 'A' && ch <= 'Z' ) || ( ch >= '0' && ch <= '9' );
}

// A Windows device name (CON, PRN, AUX, NUL, COM0-9, LPT0-9, the console ones) in any case, also with an
// extension or trailing spaces: Windows opens the device instead of a file of that name.
bool device_name( const std::string &n )
{
    std::string base = n.substr( 0, n.find( '.' ) );
    while( !base.empty() && base.back() == ' ' ) {
        base.pop_back();
    }
    for( char &ch : base ) {
        if( ch >= 'A' && ch <= 'Z' ) {
            ch = static_cast<char>( ch - 'A' + 'a' );
        }
    }
    if( base == "con" || base == "prn" || base == "aux" || base == "nul" || base == "conin$" ||
        base == "conout$" || base == "clock$" ) {
        return true;
    }
    return base.size() == 4 && ( base.compare( 0, 3, "com" ) == 0 || base.compare( 0, 3, "lpt" ) == 0 ) &&
           base[3] >= '0' && base[3] <= '9';
}

// A template name the client may save under: 1-64 of ASCII letters, digits, space, _ -, not starting or ending
// with a space and not a device name, so it stays one plain file in the templates folder.
bool template_name_ok( const std::string &n )
{
    if( n.empty() || n.size() > max_name_bytes || n.front() == ' ' || n.back() == ' ' || device_name( n ) ) {
        return false;
    }
    return std::all_of( n.begin(), n.end(), []( char ch ) {
        return ascii_alnum( ch ) || ch == ' ' || ch == '_' || ch == '-';
    } );
}

// 1..max_bytes of valid UTF-8 (no overlong forms, surrogates or code points past U+10FFFF) and no control
// characters (C0, DEL, C1). The bridge checks save names the same way.
bool plain_text( const std::string &s, size_t max_bytes )
{
    if( s.empty() || s.size() > max_bytes ) {
        return false;
    }
    static const std::array<uint32_t, 5> min_cp = { 0, 0, 0x80, 0x800, 0x10000 };
    size_t i = 0;
    while( i < s.size() ) {
        const unsigned char c = static_cast<unsigned char>( s[i] );
        if( c < 0x80 ) {
            if( c < 0x20 || c == 0x7f ) {
                return false;
            }
            i++;
            continue;
        }
        size_t len = 0;
        uint32_t cp = 0;
        if( ( c & 0xe0 ) == 0xc0 ) {
            len = 2;
            cp = c & 0x1f;
        } else if( ( c & 0xf0 ) == 0xe0 ) {
            len = 3;
            cp = c & 0x0f;
        } else if( ( c & 0xf8 ) == 0xf0 ) {
            len = 4;
            cp = c & 0x07;
        } else {
            return false;
        }
        if( i + len > s.size() ) {
            return false;
        }
        for( size_t k = 1; k < len; k++ ) {
            const unsigned char cc = static_cast<unsigned char>( s[i + k] );
            if( ( cc & 0xc0 ) != 0x80 ) {
                return false;
            }
            cp = ( cp << 6 ) | ( cc & 0x3f );
        }
        if( cp < min_cp[len] || cp < 0xa0 || cp > 0x10ffff || ( cp >= 0xd800 && cp <= 0xdfff ) ) {
            return false;
        }
        i += len;
    }
    return true;
}

// A character name the game saves under, as the bridge's save_name_ok: plain text of at most max_name_bytes,
// not starting with '#' (CDDA keeps such a name unencoded as the file name).
bool name_ok( const std::string &n )
{
    return plain_text( n, max_name_bytes ) && n.front() != '#';
}

// A character name read from a template file: name_ok, and no path characters or edge spaces either.
bool stored_name_ok( const std::string &n )
{
    return name_ok( n ) && n.front() != ' ' && n.back() != ' ' &&
           n.find_first_of( "/\\:*?\"<>|" ) == std::string::npos;
}

pool_type chosen_pool()
{
    return multi_pool() ? pool_type::MULTI_POOL : pool_type::FREEFORM;
}

// A name typed in the client: control characters dropped, cut to max_name_bytes on a character boundary and
// trimmed. The result still needs name_ok.
std::string checked_name( const std::string &raw )
{
    std::string name = sanitize_text( raw, raw.size() );
    if( name.size() > max_name_bytes ) {
        size_t cut = max_name_bytes;
        while( cut > 0 && ( static_cast<unsigned char>( name[cut] ) & 0xc0 ) == 0x80 ) {
            --cut;
        }
        name.resize( cut );
    }
    // Trim; the name becomes a save file name (CDDA encodes it), so keep it plain and non-empty.
    while( !name.empty() && name.back() == ' ' ) {
        name.pop_back();
    }
    size_t start = 0;
    while( start < name.size() && name[start] == ' ' ) {
        ++start;
    }
    return name.substr( start );
}

std::filesystem::path template_path( const std::string &name )
{
    return std::filesystem::u8path( PATH_INFO::templatedir() + name + ".template" );
}

// Template `name`'s file if it is a regular file (not a link) of at most max_template_bytes: its time and size.
bool template_file_ok( const std::string &name, std::filesystem::file_time_type &mtime, std::uintmax_t &size )
{
    namespace fs = std::filesystem;
    const fs::path p = template_path( name );
    std::error_code ec;
    if( fs::symlink_status( p, ec ).type() != fs::file_type::regular || ec ) {
        return false;
    }
    size = fs::file_size( p, ec );
    if( ec || size > max_template_bytes ) {
        return false;
    }
    mtime = fs::last_write_time( p, ec );
    return !ec;
}

// Saves `u` as template `name`. False unless the file is there and new afterwards: avatar::save_template
// reports nothing.
bool save_template_checked( avatar &u, const std::string &name )
{
    namespace fs = std::filesystem;
    const fs::path p = template_path( name );
    std::error_code ec;
    const fs::file_time_type before = fs::last_write_time( p, ec );
    const bool existed = !ec;
    u.save_template( name, chosen_pool() );
    std::error_code ec2;
    if( fs::symlink_status( p, ec2 ).type() != fs::file_type::regular || ec2 ) {
        return false;
    }
    const fs::file_time_type after = fs::last_write_time( p, ec2 );
    return !ec2 && ( !existed || after != before );
}

// What a template holds, as template_choices lists it.
struct template_info {
    std::string scenario;
    std::string profession;
    std::vector<std::string> backgrounds;
    std::array<int, 4> stats = {};
    std::vector<std::string> traits;
    std::vector<std::pair<std::string, int>> skills;
    std::string name;
    bool male = true;
    int age = 0;
    int height = 0;
};

// Loads template `name` into a scratch avatar; nothing if it doesn't load. load_template sets CDDA's global
// scenario: the old one is put back.
std::optional<template_info> read_template( const std::string &name )
{
    const scenario *before = get_scenario();
    avatar t;
    pool_type pool = pool_type::FREEFORM;
    bool loaded = false;
    try {
        loaded = t.load_template( name, pool );
    } catch( const std::exception & ) {
        loaded = false;
    }
    const scenario *scen = get_scenario();
    set_scenario( before );
    if( !loaded || scen == nullptr || t.prof == nullptr ) {
        return std::nullopt;
    }
    template_info ti;
    ti.scenario = scen->ident().str();
    ti.profession = t.prof->ident().str();
    for( const profession *h : t.hobbies ) {
        if( h != nullptr && h->ident().is_valid() && ti.backgrounds.size() < max_list ) {
            ti.backgrounds.push_back( h->ident().str() );
        }
    }
    ti.stats = { t.str_max, t.dex_max, t.int_max, t.per_max };
    for( const trait_id &m : t.get_mutations( true ) ) {
        bool locked = scen->is_locked_trait( m ) || t.prof->is_locked_trait( m );
        for( const profession *h : t.hobbies ) {
            locked = locked || ( h != nullptr && h->is_locked_trait( m ) );
        }
        if( !locked && m.is_valid() && ti.traits.size() < max_list ) {
            ti.traits.push_back( m.str() );
        }
    }
    for( const Skill &sk : Skill::skills ) {
        const int lvl = static_cast<int>( t.get_skill_level( sk.ident() ) );
        if( lvl > 0 && !sk.is_contextual_skill() ) {
            ti.skills.emplace_back( sk.ident().str(), std::min<int>( lvl, skill_costs.size() - 1 ) );
        }
    }
    // The name is file data: sent only if the game could save under it.
    ti.name = stored_name_ok( t.name ) ? t.name : std::string();
    ti.male = t.male;
    ti.age = t.base_age();
    ti.height = t.base_height();
    return ti;
}

struct cached_template {
    std::filesystem::file_time_type mtime;
    std::uintmax_t size = 0;
    std::optional<template_info> info;
};

// Read templates by name, for the game data `key` names. Chargen runs on the sim thread only.
struct template_cache_t {
    std::string key;
    std::map<std::string, cached_template> entries;
};

template_cache_t &template_cache()
{
    static template_cache_t cache;
    return cache;
}

// The game data the cached templates were read with: the active world and its mods.
std::string game_data_key()
{
    std::string key;
    if( world_generator->active_world != nullptr ) {
        key = world_generator->active_world->world_name;
        for( const mod_id &m : world_generator->active_world->active_mod_order ) {
            key += '\n' + m.str();
        }
    }
    return key;
}

// Drops cached templates that aren't `listed`, or all of them if the game data changed.
void prune_template_cache( const std::vector<std::string> &listed )
{
    template_cache_t &cache = template_cache();
    const std::string key = game_data_key();
    if( key != cache.key ) {
        cache.entries.clear();
        cache.key = key;
    }
    for( auto it = cache.entries.begin(); it != cache.entries.end(); ) {
        if( std::find( listed.begin(), listed.end(), it->first ) == listed.end() ) {
            it = cache.entries.erase( it );
        } else {
            ++it;
        }
    }
}

// Template `name` as read_template reads it, read again only when its file changed. Null if it doesn't load.
const template_info *cached_template_info( const std::string &name )
{
    template_cache_t &cache = template_cache();
    std::filesystem::file_time_type mtime;
    std::uintmax_t size = 0;
    if( !template_file_ok( name, mtime, size ) ) {
        cache.entries.erase( name );
        return nullptr;
    }
    auto it = cache.entries.find( name );
    if( it == cache.entries.end() || it->second.mtime != mtime || it->second.size != size ) {
        cached_template c;
        c.mtime = mtime;
        c.size = size;
        c.info = read_template( name );
        it = cache.entries.insert_or_assign( name, std::move( c ) ).first;
    }
    return it->second.info ? &*it->second.info : nullptr;
}

// Writes `name: {choices}` for template `name` (as apply_chargen reads them).
void write_template_choices( JsonOut &jo, const std::string &name, const template_info &ti )
{
    jo.member( name );
    jo.start_object();
    jo.member( "scenario", ti.scenario );
    jo.member( "profession", ti.profession );
    write_strings( jo, "backgrounds", ti.backgrounds );
    jo.member( "str", ti.stats[0] );
    jo.member( "dex", ti.stats[1] );
    jo.member( "int", ti.stats[2] );
    jo.member( "per", ti.stats[3] );
    write_strings( jo, "traits", ti.traits );
    jo.member( "skills" );
    jo.start_object();
    for( const std::pair<std::string, int> &sk : ti.skills ) {
        jo.member( sk.first, sk.second );
    }
    jo.end_object();
    jo.member( "name", ti.name );
    jo.member( "male", ti.male );
    jo.member( "age", ti.age );
    jo.member( "height", ti.height );
    jo.end_object();
}
} // namespace

std::vector<std::string> list_templates()
{
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path dir = fs::u8path( PATH_INFO::templatedir() );
    fs::directory_iterator it( dir, ec );
    size_t seen = 0;
    for( ; !ec && it != fs::directory_iterator() && seen < 4096; it.increment( ec ), ++seen ) {
        std::error_code ec2;
        if( it->symlink_status( ec2 ).type() != fs::file_type::regular || ec2 ) {
            continue;
        }
        const std::string file = it->path().filename().u8string();
        const std::string ext = ".template";
        if( file.size() <= ext.size() || file.compare( file.size() - ext.size(), ext.size(), ext ) != 0 ) {
            continue;
        }
        const std::string name = file.substr( 0, file.size() - ext.size() );
        const std::uintmax_t size = it->file_size( ec2 );
        if( template_name_ok( name ) && !ec2 && size <= max_template_bytes && out.size() < max_templates ) {
            out.push_back( name );
        }
    }
    std::sort( out.begin(), out.end() );
    return out;
}

std::string chargen_catalog_json()
{
    std::ostringstream os;
    JsonOut jo( os );
    jo.start_object();
    jo.member( "version", 1 );
    jo.member( "pool", get_option<std::string>( "CHARACTER_POINT_POOLS" ) );
    jo.member( "enforce_points", multi_pool() );
    jo.member( "stat_pool", stat_pool() );
    jo.member( "trait_pool", get_option<int>( "INITIAL_TRAIT_POINTS" ) );
    jo.member( "skill_pool", get_option<int>( "INITIAL_SKILL_POINTS" ) );
    jo.member( "max_trait_points", get_option<int>( "MAX_TRAIT_POINTS" ) );
    jo.member( "min_stat", min_stat );
    jo.member( "max_stat", multi_pool() ? MAX_STAT : freeform_max_stat );
    jo.member( "high_stat", HIGH_STAT );
    jo.member( "max_skill", static_cast<int>( skill_costs.size() ) - 1 );
    jo.member( "skill_costs" );
    jo.start_array();
    for( int c : skill_costs ) {
        jo.write( c );
    }
    jo.end_array();
    jo.member( "min_age", min_age );
    jo.member( "max_age", max_age );
    jo.member( "min_height", Character::min_height() );
    jo.member( "max_height", Character::max_height() );
    jo.member( "max_name", static_cast<int>( max_name_bytes ) );

    // Default selection: the generic scenario and its cheapest profession, as CDDA's own screen starts.
    jo.member( "default_scenario", scenario::generic()->ident().str() );
    const std::vector<std::string> templates = list_templates();
    write_strings( jo, "templates", templates );
    // What each template holds, in the answer's own format, so the client can open one for editing.
    // Templates that don't load are left out.
    prune_template_cache( templates );
    jo.member( "template_choices" );
    jo.start_object();
    for( const std::string &t : templates ) {
        if( const template_info *ti = cached_template_info( t ) ) {
            write_template_choices( jo, t, *ti );
        }
    }
    jo.end_object();

    jo.member( "scenarios" );
    jo.start_array();
    for( const scenario &s : scenario::get_all() ) {
        if( !scenario_listed( s ) ) {
            continue;
        }
        jo.start_object();
        jo.member( "id", s.ident().str() );
        jo.member( "name", s.gender_appropriate_name( true ) );
        jo.member( "description", s.description( true ) );
        jo.member( "points", s.point_cost() );
        jo.member( "start_name", s.start_name() );
        jo.member( "default_background", !s.has_flag( "SKIP_DEFAULT_BACKGROUND" ) );
        std::vector<std::string> profs;
        for( const string_id<profession> &p : s.permitted_professions() ) {
            profs.push_back( p.str() );
        }
        write_strings( jo, "professions", profs );
        std::vector<std::string> hobbies;
        for( const string_id<profession> &p : s.permitted_hobbies() ) {
            hobbies.push_back( p.str() );
        }
        write_strings( jo, "backgrounds", hobbies );
        std::vector<std::string> extra;
        std::vector<std::string> forbidden;
        for( const mutation_branch &m : mutation_branch::get_all() ) {
            if( mutation_branch::trait_is_blacklisted( m.id ) ) {
                continue;
            }
            if( !m.startingtrait && s.traitquery( m.id ) ) {
                extra.push_back( m.id.str() );
            }
            if( s.is_forbidden_trait( m.id ) ) {
                forbidden.push_back( m.id.str() );
            }
        }
        write_strings( jo, "extra_traits", extra );
        write_strings( jo, "forbidden_traits", forbidden );
        std::vector<std::string> locked;
        for( const trait_id &t : s.get_locked_traits() ) {
            locked.push_back( t.str() );
        }
        write_strings( jo, "locked_traits", locked );
        jo.end_object();
    }
    jo.end_array();

    jo.member( "professions" );
    jo.start_array();
    for( const profession &p : profession::get_all() ) {
        if( !p.is_hobby() ) {
            write_profession( jo, p );
        }
    }
    jo.end_array();

    jo.member( "backgrounds" );
    jo.start_array();
    for( const string_id<profession> &p : profession::get_all_hobbies() ) {
        write_profession( jo, p.obj() );
    }
    jo.end_array();

    // Every trait some scenario can list; scenario.extra_traits says which non-starting ones it adds.
    jo.member( "traits" );
    jo.start_array();
    for( const mutation_branch &m : mutation_branch::get_all() ) {
        bool listed = false;
        for( const scenario &s : scenario::get_all() ) {
            if( scenario_listed( s ) && trait_listed( m, s ) ) {
                listed = true;
                break;
            }
        }
        if( !listed ) {
            continue;
        }
        jo.start_object();
        jo.member( "id", m.id.str() );
        jo.member( "name", m.name() );
        jo.member( "description", m.desc() );
        jo.member( "points", m.points );
        jo.member( "starting", m.startingtrait );
        std::vector<std::string> cancels;
        for( const trait_id &c : m.cancels ) {
            cancels.push_back( c.str() );
        }
        write_strings( jo, "cancels", cancels );
        write_strings( jo, "types", std::vector<std::string>( m.types.begin(), m.types.end() ) );
        jo.end_object();
    }
    jo.end_array();

    jo.member( "skills" );
    jo.start_array();
    for( const Skill &sk : Skill::skills ) {
        if( sk.is_contextual_skill() ) {
            continue;
        }
        jo.start_object();
        jo.member( "id", sk.ident().str() );
        jo.member( "name", sk.name() );
        jo.member( "description", sk.description() );
        jo.end_object();
    }
    jo.end_array();
    jo.end_object();
    return os.str();
}

std::string apply_chargen( avatar &u, const std::string &choices_json )
{
    if( choices_json.empty() || choices_json.size() > max_answer_json ) {
        return "no choices";
    }
    try {
        const JsonValue jv = json_loader::from_string( choices_json );
        const JsonObject jo = jv.get_object();
        jo.allow_omitted_members();

        const std::string del = jo.get_string( "delete_template", "" );
        if( !del.empty() ) {
            const std::vector<std::string> listed = list_templates();
            if( std::find( listed.begin(), listed.end(), del ) == listed.end() ) {
                return "no template " + sanitize_text( del, 64 );
            }
            std::error_code ec;
            std::filesystem::remove( template_path( del ), ec );
            template_cache().entries.erase( del );
            return ec ? "cannot delete template " + del : "deleted template " + del;
        }
        const std::string tmpl = jo.get_string( "template", "" );
        if( !tmpl.empty() ) {
            const std::vector<std::string> listed = list_templates();
            if( std::find( listed.begin(), listed.end(), tmpl ) == listed.end() ) {
                return "no template " + sanitize_text( tmpl, 64 );
            }
            u = avatar();
            u.prof = profession::generic();
            set_scenario( scenario::generic() );
            pool_type pool = pool_type::FREEFORM;
            if( !u.load_template( tmpl, pool ) ) {
                return "cannot load template " + tmpl;
            }
            if( get_scenario() == nullptr || find_scenario( get_scenario()->ident().str() ) == nullptr ) {
                return "the template's scenario isn't available";
            }
            // As avatar::create( TEMPLATE ): a transfer template ("Character to Template") is a played
            // character and keeps its recipes and setup; other templates relearn recipes in game.
            const bool transfer = pool == pool_type::TRANSFER;
            if( !transfer ) {
                u.forget_all_recipes();
            }
            const std::string name = checked_name( jo.get_string( "name", "" ) );
            if( !name.empty() ) {
                u.name = name;
            } else if( !stored_name_ok( u.name ) ) {
                // The template's own name is file data: a random one if the game couldn't save under it.
                u.pick_name();
            }
            if( !name_ok( u.name ) ) {
                return "bad name";
            }
            if( world_generator->active_world != nullptr &&
                world_generator->active_world->save_exists( save_t::from_save_id( u.name ) ) ) {
                return "a character named " + u.name + " already exists in this world";
            }
            u.set_body();
            if( !transfer ) {
                u.initialize( character_type::CUSTOM );
            }
            return "";
        }

        const scenario *scen = find_scenario( jo.get_string( "scenario", "" ) );
        if( scen == nullptr ) {
            return "unknown scenario";
        }
        const std::string prof_id = jo.get_string( "profession", "" );
        const std::vector<string_id<profession>> permitted = scen->permitted_professions();
        auto pit = std::find_if( permitted.begin(), permitted.end(), [&]( const string_id<profession> &p ) {
            return p.str() == prof_id;
        } );
        if( pit == permitted.end() ) {
            return "that profession isn't allowed in this scenario";
        }

        u = avatar();
        u.prof = profession::generic();
        set_scenario( scenario::generic() );
        u.male = jo.get_bool( "male", true );
        u.randomize_cosmetics();
        reset_to_scenario( u, *scen, pit->obj() );

        // Backgrounds (hobbies). Omitted: CDDA's default background where the scenario allows one.
        if( jo.has_array( "backgrounds" ) ) {
            const std::vector<std::string> ids = jo.get_string_array( "backgrounds" );
            if( ids.size() > max_list ) {
                return "too many backgrounds";
            }
            const std::vector<string_id<profession>> allowed = scen->permitted_hobbies();
            for( const std::string &id : ids ) {
                auto hit = std::find_if( allowed.begin(), allowed.end(), [&]( const string_id<profession> &p ) {
                    return p.str() == id;
                } );
                if( hit == allowed.end() ) {
                    return "background not allowed: " + id;
                }
                u.hobbies.insert( &hit->obj() );
            }
            for( const profession *h : u.hobbies ) {
                for( const trait_and_var &t : h->get_locked_traits() ) {
                    if( !u.has_trait( t.trait ) ) {
                        u.toggle_trait_deps( t.trait );
                    }
                }
            }
        } else if( !scen->has_flag( "SKIP_DEFAULT_BACKGROUND" ) ) {
            u.add_default_background();
        }

        // Stats.
        const int max_stat = multi_pool() ? MAX_STAT : freeform_max_stat;
        const std::array<std::pair<const char *, int *>, 4> stats = { {
                { "str", &u.str_max }, { "dex", &u.dex_max }, { "int", &u.int_max }, { "per", &u.per_max }
            }
        };
        for( const auto &st : stats ) {
            const int v = jo.get_int( st.first, 8 );
            if( v < min_stat || v > max_stat ) {
                return std::string( "stat out of range: " ) + st.first;
            }
            *st.second = v;
        }

        // Traits the player picked (locked ones are already there).
        if( jo.has_array( "traits" ) ) {
            const std::vector<std::string> ids = jo.get_string_array( "traits" );
            if( ids.size() > max_list ) {
                return "too many traits";
            }
            int good = 0;
            int bad = 0;
            for( const std::string &id : ids ) {
                const trait_id t( id );
                if( !t.is_valid() || !trait_listed( t.obj(), *scen ) ) {
                    return "unknown trait: " + id;
                }
                if( u.has_trait( t ) ) {
                    continue;
                }
                if( scen->is_forbidden_trait( t ) ) {
                    return "the scenario forbids " + t->name();
                }
                if( u.prof->is_forbidden_trait( t ) ) {
                    return "the profession forbids " + t->name();
                }
                if( u.has_conflicting_trait( t ) ) {
                    return t->name() + " conflicts with another trait";
                }
                ( t->points > 0 ? good : bad ) += t->points;
                u.toggle_trait_deps( t );
            }
            const int max_trait = get_option<int>( "MAX_TRAIT_POINTS" );
            if( multi_pool() && ( good > max_trait || bad < -max_trait ) ) {
                return "too many trait points";
            }
        }

        // Skills.
        if( jo.has_object( "skills" ) ) {
            const JsonObject sk = jo.get_object( "skills" );
            sk.allow_omitted_members();
            size_t n = 0;
            for( const JsonMember m : sk ) {
                if( ++n > max_list ) {
                    return "too many skills";
                }
                const skill_id id( m.name() );
                if( !id.is_valid() || id->is_contextual_skill() ) {
                    return "unknown skill: " + m.name();
                }
                const int lvl = m.get_int();
                if( lvl < 0 || lvl >= static_cast<int>( skill_costs.size() ) ) {
                    return "skill level out of range: " + m.name();
                }
                u.set_skill_level( id, lvl );
            }
        }

        if( multi_pool() ) {
            const int ps = stat_pool() - stat_points_used( u );
            const int pt = get_option<int>( "INITIAL_TRAIT_POINTS" ) - trait_points_used( u );
            const int pk = get_option<int>( "INITIAL_SKILL_POINTS" ) - skill_points_used( u );
            // newcharacter.cpp's multi_pool: lower pools may borrow from higher ones, not the other way.
            if( ps + std::min( 0, pt + std::min( 0, pk ) ) < 0 || ps + pt + std::min( 0, pk ) < 0 ||
                ps + pt + pk < 0 ) {
                return "not enough points";
            }
        }

        // Description.
        const int age = jo.get_int( "age", 25 );
        if( age < min_age || age > max_age ) {
            return "age out of range";
        }
        u.set_base_age( age );
        const int height = jo.get_int( "height", Character::default_height() );
        if( height < Character::min_height() || height > Character::max_height() ) {
            return "height out of range";
        }
        u.set_base_height( height );
        std::string name = checked_name( jo.get_string( "name", "" ) );
        if( name.empty() ) {
            u.pick_name();
            name = u.name;
        }
        if( !name_ok( name ) ) {
            return "bad name";
        }
        u.name = name;
        if( world_generator->active_world != nullptr &&
            world_generator->active_world->save_exists( save_t::from_save_id( name ) ) ) {
            return "a character named " + name + " already exists in this world";
        }
    } catch( const std::exception &e ) {
        return std::string( "bad choices: " ) + sanitize_text( e.what(), 200 );
    }
    u.set_body();
    {
        // As avatar::create: the template is saved before initialize() adds the profession's items.
        std::string save_as;
        try {
            const JsonValue jv = json_loader::from_string( choices_json );
            const JsonObject jo = jv.get_object();
            jo.allow_omitted_members();
            save_as = jo.get_string( "save_template", "" );
        } catch( const std::exception & ) {
            save_as.clear();
        }
        if( !save_as.empty() ) {
            if( !template_name_ok( save_as ) ) {
                return "bad template name";
            }
            const bool saved = save_template_checked( u, save_as );
            template_cache().entries.erase( save_as );
            if( !saved ) {
                return "cannot save template " + save_as;
            }
        }
    }
    u.initialize( character_type::CUSTOM );
    return "";
}

bool run_chargen()
{
    std::string status;
    for( ;; ) {
        // Rebuilt every time: a deleted or saved template changes it.
        const std::string catalog = chargen_catalog_json();
        ui_request req;
        req.kind = request_kind::chargen;
        req.context = "CHARGEN";
        req.title = "New character";
        req.text = catalog;
        req.status = status;
        req.allow_cancel = true;
        const ui_answer a = ask( req );
        if( a.cancel ) {
            return false;
        }
        status = apply_chargen( get_avatar(), a.text );
        if( status.empty() ) {
            return true;
        }
    }
}
} // namespace fp3d
