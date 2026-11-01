#include "fp3d/boot.h"

#include <cstdio>
#include <memory>
#include <stdexcept>

#include "avatar.h"
#include "calendar.h"
#include "color.h"
#include "cursesdef.h"
#include "filesystem.h"
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

namespace fp3d
{

void boot_world( const std::string &world_name, bool headless )
{
    PATH_INFO::init_base_path( "" );
    PATH_INFO::init_user_dir( "./fp3d_user" );
    PATH_INFO::set_standard_filenames();
    for( const std::string &dir : { PATH_INFO::config_dir(), PATH_INFO::savedir(), PATH_INFO::templatedir() } ) {
        if( !assure_dir_exist( dir ) ) {
            throw std::runtime_error( "cannot create " + dir );
        }
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
    if( world_generator->has_world( world_name ) ) {
        world_generator->delete_world( world_name, true );
    }
    WORLD *world = world_generator->make_new_world( world_name, { mod_id( "dda" ) } );
    if( world == nullptr ) {
        throw std::runtime_error( "make_new_world failed" );
    }
    world_generator->set_active_world( world );
    calendar::set_eternal_season( get_option<bool>( "ETERNAL_SEASON" ) );
    calendar::set_season_length( get_option<int>( "SEASON_LENGTH" ) );

    g->load_core_data();
    g->load_world_modfiles();

    get_avatar() = avatar();
    get_avatar().create( character_type::NOW );
    get_map() = map();
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
