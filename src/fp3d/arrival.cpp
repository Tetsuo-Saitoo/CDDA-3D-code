#include "fp3d/arrival.h"

#include <vector>

#include "avatar.h"
#include "coordinates.h"
#include "creature_tracker.h"
#include "enums.h"
#include "field.h"
#include "fp3d/bridge.h"
#include "fp3d/test_range.h"
#include "game.h"
#include "map.h"
#include "mapdata.h"
#include "monster.h"
#include "npc.h"
#include "trap.h"
#include "vpart_position.h"

namespace fp3d
{
void stop_moving( avatar &u )
{
    if( u.get_grab_type() != object_type::NONE ) {
        u.grab( object_type::NONE );
    }
    u.stop_hauling();
    u.clear_destination();
}

void leave_square( avatar &u )
{
    stop_moving( u );
    clear_actions();
}

std::optional<tripoint_abs_ms> arrival_square( const tripoint_abs_ms &centre )
{
    map &here = get_map();
    creature_tracker &tracker = get_creature_tracker();
    for( const tripoint_bub_ms &p : closest_points_first( here.get_bub( centre ), arrival_search_squares ) ) {
        if( !here.inbounds( p ) || !here.passable( p ) || !here.has_floor( p ) ||
            here.has_flag( ter_furn_flag::TFLAG_DEEP_WATER, p ) || here.field_at( p ).field_count() > 0 ||
            !here.tr_at( p ).is_null() || here.veh_at( p ) || tracker.creature_at<monster>( p, true ) != nullptr ||
            tracker.creature_at<npc>( p ) != nullptr ) {
            continue;
        }
        return here.get_abs( p );
    }
    return std::nullopt;
}

void settle_avatar()
{
    avatar &u = get_avatar();
    map &here = get_map();
    g->update_map( u );
    here.invalidate_map_cache( u.posz() );
    here.build_map_cache( u.posz() );
    g->update_overmap_seen();
}
} // namespace fp3d
