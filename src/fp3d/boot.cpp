#include "fp3d/boot.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>

#include "avatar.h"
#include "cached_options.h"
#include "calendar.h"
#include "color.h"
#include "cursesdef.h"
#include "debug.h"
#include "filesystem.h"
#include "fp3d/game_access.h"
#include "fp3d/id_tables.h"
#include "game.h"
#include "game_constants.h"
#include "map.h"
#include "map_scale_constants.h"
#include "options.h"
#include "output.h"
#include "overmapbuffer.h"
#include "path_info.h"
#include "type_id.h"
#include "weather.h"
#include "worldfactory.h"

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
    init_colors();

    g = std::make_unique<game>();
    g->new_game = true;
    g->load_static_data();

    world_generator->set_active_world( nullptr );
    world_generator->init();
}
} // namespace

void boot_new_game_data( const std::string &world_name, bool fresh_world, bool headless )
{
    boot_common( headless );
    WORLD *world = nullptr;
    if( fresh_world ) {
        if( world_generator->has_world( world_name ) ) {
            world_generator->delete_world( world_name, true );
        }
        world = world_generator->make_new_world( world_name, { mod_id( "dda" ) } );
        if( world == nullptr ) {
            throw std::runtime_error( "make_new_world failed" );
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

    g->load_core_data();
    g->load_world_modfiles();
    build_id_tables();

    get_avatar() = avatar();
    get_map() = map();
}

void boot_world( const std::string &world_name, bool headless )
{
    boot_new_game_data( world_name, true, headless );
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
    g->setup();
    build_id_tables();
    if( !fp3d_game_access::load_game( *g, save ) ) {
        throw std::runtime_error( "cannot load " + save_name + " from world " + world_name );
    }
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
