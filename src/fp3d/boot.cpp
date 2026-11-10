#include "fp3d/boot.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>

#include "avatar.h"
#include "cached_options.h"
#include "calendar.h"
#include "color.h"
#include "cursesdef.h"
#include "debug.h"
#include "enums.h"
#include "filesystem.h"
#include "fp3d/bridge.h"
#include "fp3d/data_check.h"
#include "fp3d/game_access.h"
#include "fp3d/id_tables.h"
#include "fp3d/test_range.h"
#include "game.h"
#include "game_constants.h"
#include "map.h"
#include "map_scale_constants.h"
#include "messages.h"
#include "options.h"
#include "output.h"
#include "overmapbuffer.h"
#include "path_info.h"
#include "type_id.h"
#include "weather.h"
#include "worldfactory.h"
#include "fp3d/worldgen.h"

#if defined(TILES)
#include "sdl_wrappers.h"
#endif

namespace fp3d
{

static bool paths_set = false;

void init_paths( const std::string &base_path, const std::string &user_dir )
{
    PATH_INFO::init_base_path( base_path );
    PATH_INFO::init_user_dir( user_dir );
    PATH_INFO::set_standard_filenames();
    for( const std::string &dir : { PATH_INFO::user_dir(), PATH_INFO::config_dir(), PATH_INFO::savedir(),
                                    PATH_INFO::templatedir()
                                  } ) {
        if( !assure_dir_exist( dir ) ) {
            throw std::runtime_error( "cannot create " + dir );
        }
    }
    paths_set = true;
}

namespace
{
// Options (headless: CDDA's hidden window first), colours, the game object and its static data.
void boot_common( bool headless )
{
    if( !paths_set ) {
        init_paths( "", "./fp3d_user" );
    }
    if( headless ) {
        // Game code calls the tile renderer directly, so create CDDA's window as usual (with the bridge
        // active it is hidden and nothing is drawn to it). Like main(), init_interface() starts SDL first
        // and then initialises and loads the options itself.
        FULL_SCREEN_WIDTH = EVEN_MINIMUM_TERM_WIDTH;
        FULL_SCREEN_HEIGHT = EVEN_MINIMUM_TERM_HEIGHT;
        note_boot_step( "Starting SDL, options and tilesets" );
        catacurses::init_interface();
        // With animations on, the main input loop polls on a 125 ms timer instead of blocking.
        for( const char *opt : { "ANIMATIONS", "ANIMATION_RAIN", "ANIMATION_SCT", "AUTOSAVE" } ) {
            if( get_options().has_option( opt ) ) {
                get_options().get_option( opt ).setValue( "false" );
            }
        }
    } else {
        get_options().init();
        get_options().load();
    }
    // CDDA's safe mode is off in FP3D games (new and loaded: game::start_game and game::load read SAFEMODE);
    // the `safemode` action (Shift+1) still toggles it. Set in memory only, like the options above.
    for( const char *opt : { "SAFEMODE", "AUTOSAFEMODE" } ) {
        if( get_options().has_option( opt ) ) {
            get_options().get_option( opt ).setValue( "false" );
        }
    }
    // A JSON parse cache entry older than its file (a cache shipped with the data, or data updated in place) is
    // parsed again either way; keep it to a log line, not a reported error.
    if( get_options().has_option( "WARN_ON_MODIFIED" ) ) {
        get_options().get_option( "WARN_ON_MODIFIED" ).setValue( "false" );
    }
    note_boot_step( "Colours and static data" );
    init_colors();
    // A range (and its AI freeze) belongs to the game booted with it, not to the next one.
    reset_test_range();

    g = std::make_unique<game>();
    g->new_game = true;
    g->load_static_data();

    note_boot_step( "Finding worlds" );
    world_generator->set_active_world( nullptr );
    world_generator->init();
}
} // namespace

namespace
{
std::string ascii_lower( std::string s )
{
    for( char &c : s ) {
        if( c >= 'A' && c <= 'Z' ) {
            c = static_cast<char>( c - 'A' + 'a' );
        }
    }
    return s;
}
} // namespace

std::optional<std::string> world_name_taken( const std::string &world_name )
{
    const std::string want = ascii_lower( world_name );
    for( const std::string &w : world_generator->all_worldnames() ) {
        if( ascii_lower( w ) == want ) {
            return w;
        }
    }
    // Folders CDDA didn't list as worlds (no options file, say) count too.
    constexpr size_t max_entries = 100000;
    std::error_code ec;
    std::filesystem::directory_iterator it( std::filesystem::u8path( PATH_INFO::savedir() ), ec );
    for( size_t n = 0; !ec && it != std::filesystem::directory_iterator() && n < max_entries; n++ ) {
        const std::string name = it->path().filename().u8string();
        if( ascii_lower( name ) == want ) {
            return name;
        }
        it.increment( ec );
    }
    return std::nullopt;
}

bool replaceable_world( const std::string &world_name )
{
    return world_name.size() > 5 && world_name.compare( 0, 5, "fp3d-" ) == 0;
}

bool reserved_world_name( const std::string &world_name )
{
    return ascii_lower( world_name.substr( 0, 5 ) ) == "fp3d-";
}

std::string replacing_world_name( const std::string &world_name )
{
    // "<name>-99" at most: a folder full of others' fp3d- worlds is someone's choice, not FP3D's to clear.
    for( int n = 1; n <= 99; n++ ) {
        const std::string name = n == 1 ? world_name : world_name + "-" + std::to_string( n );
        const std::optional<std::string> taken = world_name_taken( name );
        if( !taken || ( *taken == name && world_generator->has_world( name ) && owned_world( name ) ) ) {
            return name;
        }
    }
    throw std::runtime_error( "no free world name like " + world_name );
}

bool owned_world( const std::string &world_name )
{
    namespace fs = std::filesystem;
    if( !replaceable_world( world_name ) ) {
        return false;
    }
    const fs::path dir = fs::u8path( PATH_INFO::savedir() ) / fs::u8path( world_name );
    std::error_code ec;
    if( fs::symlink_status( dir, ec ).type() != fs::file_type::directory || ec ) {
        return false;
    }
    return fs::symlink_status( dir / owned_world_marker, ec ).type() == fs::file_type::regular && !ec;
}

void boot_new_game_data( const std::string &world_name, bool fresh_world, bool headless, bool world_setup,
                         bool replace )
{
    boot_common( headless );
    // As CDDA's main menu: core data (option sliders among it) before a world is made or picked.
    g->load_core_data();
    WORLD *world = nullptr;
    if( fresh_world ) {
        // A new world gets a folder of its own; only FP3D's throwaway worlds are replaced: by exact name, and
        // only one this code made (owned_world).
        const bool replacing = replace && replaceable_world( world_name );
        if( reserved_world_name( world_name ) && !replacing ) {
            throw std::runtime_error( "world names starting with fp3d- are kept for quick starts and tests" );
        }
        // A replacing start whose world someone else made (or an older FP3D, before the marker) leaves it alone
        // and takes the next free or own "<name>-N".
        const std::string name = replacing ? replacing_world_name( world_name ) : world_name;
        if( const std::optional<std::string> taken = world_name_taken( name ) ) {
            if( !replacing || *taken != name || !world_generator->has_world( name ) || !owned_world( name ) ) {
                throw std::runtime_error( "world " + *taken + " exists" );
            }
            world_generator->delete_world( name, true );
        }
        note_boot_step( "Making world " + name );
        if( world_setup ) {
            world = run_world_setup( name );
            if( world == nullptr ) {
                throw std::runtime_error( "world creation cancelled" );
            }
        } else {
            world = world_generator->make_new_world( name, { mod_id( "dda" ) } );
            if( world == nullptr ) {
                throw std::runtime_error( "make_new_world failed" );
            }
        }
        if( replacing ) {
            namespace fs = std::filesystem;
            const fs::path dir = fs::u8path( PATH_INFO::savedir() ) / fs::u8path( name );
            std::error_code ec;
            fs::create_directories( dir, ec );
            std::ofstream mark( dir / owned_world_marker, std::ios::binary | std::ios::trunc );
            mark << "Made by an FP3D quick start or test: FP3D may delete and remake this world.\n";
            if( !mark ) {
                throw std::runtime_error( "cannot mark world " + name );
            }
        }
    } else {
        // A new character in a world world_generator->init() found; ask for nothing it didn't find.
        if( !world_generator->has_world( world_name ) ) {
            throw std::runtime_error( "no world " + world_name );
        }
        world = world_generator->get_world( world_name );
        if( world == nullptr ) {
            throw std::runtime_error( "no world " + world_name );
        }
    }
    world_generator->set_active_world( world );
    calendar::set_eternal_season( get_option<bool>( "ETERNAL_SEASON" ) );
    calendar::set_season_length( get_option<int>( "SEASON_LENGTH" ) );

    prepare_data_check( world->active_mod_order );
    g->load_world_modfiles();
    finish_data_check();
    note_boot_step( "Id tables" );
    build_id_tables();

    get_avatar() = avatar();
    get_map() = map();
}

void boot_world( const std::string &world_name, bool headless )
{
    boot_new_game_data( world_name, true, headless, false, /*replace=*/true );
    get_avatar().create( character_type::NOW );
    get_map() = map();
}

void boot_saved_game( const std::string &world_name, const std::string &save_name, bool headless )
{
    boot_common( headless );
    // world_generator->init() found the worlds and their saves; ask for nothing it didn't find.
    if( !world_generator->has_world( world_name ) ) {
        throw std::runtime_error( "no world " + world_name );
    }
    WORLD *world = world_generator->get_world( world_name );
    const save_t save = save_t::from_save_id( save_name );
    if( world == nullptr ||
        std::find( world->world_saves.begin(), world->world_saves.end(), save ) == world->world_saves.end() ) {
        throw std::runtime_error( "no save " + save_name + " in world " + world_name );
    }
    // As main_menu::load_game: setup() loads the core data, the world's mods and resets the game state.
    world_generator->set_active_world( world );
    prepare_data_check( world->active_mod_order );
    g->setup();
    finish_data_check();
    note_boot_step( "Id tables" );
    build_id_tables();
    note_boot_step( "Loading the save" );
    if( !fp3d_game_access::load_game( *g, save ) ) {
        throw std::runtime_error( "cannot load " + save_name + " from world " + world_name );
    }
}

void run_save()
{
    const std::optional<bool> only_if_changed = take_save_request();
    if( !only_if_changed ) {
        return;
    }
    if( *only_if_changed && g->get_moves_since_last_save() == 0 ) {
        note_saved( true );
        return;
    }
    const bool ok = fp3d_game_access::save_game( *g );
    add_msg( ok ? m_good : m_bad, ok ? "Game saved." : "The game could not be saved." );
    note_saved( ok );
}

void shutdown_world()
{
    test_mode = true;
    deinitDebug();
    g.reset();
    catacurses::endwin();
#if defined(TILES)
    SDL_Quit();
#endif
}

tripoint_abs_sm load_bubble_in_town()
{
    // closest_city only searches overmaps that exist, so generate the one at the origin first.
    overmap_buffer.get( point_abs_om() );
    const city_reference city = overmap_buffer.closest_city( tripoint_abs_sm::zero );
    const tripoint_abs_sm centre = city ? city.abs_sm_pos : tripoint_abs_sm::zero;
    const tripoint_abs_sm origin = centre - point_rel_sm( MAPSIZE / 2, MAPSIZE / 2 );
    // start_game() assigns the id itself; this path doesn't call it.
    get_avatar().setID( g->assign_npc_id(), false );
    get_map().load( origin, false );
    get_avatar().move_to( project_to<coords::ms>( centre ) );
    get_weather().update_weather();
    // Vehicle and vision/light caches, as a map shift and do_turn build them (snapshots read them;
    // without the vehicle rebuild, vehicle roofs are missing from the outside cache).
    get_map().rebuild_vehicle_level_caches();
    get_map().build_map_cache( get_avatar().posz() );
    std::printf( "city: %s at submap %s\n", city ? "found" : "none (using origin)",
                 centre.to_string().c_str() );
    return origin;
}

} // namespace fp3d
