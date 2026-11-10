#include "fp3d/bullet_paths.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>

#include "creature.h"
#include "damage.h"
#include "fp3d/bridge.h"
#include "game_constants.h"
#include "map.h"
#include "messages.h"
#include "projectile.h"
#include "rng.h"
#include "translations.h"

namespace fp3d
{
namespace
{
std::atomic<uint64_t> n_through{ 0 };
std::atomic<uint64_t> n_ricochets{ 0 };

double body_stop( creature_size size )
{
    switch( size ) {
        case creature_size::tiny:
            return body_stop_tiny;
        case creature_size::small:
            return body_stop_small;
        case creature_size::medium:
            return body_stop_medium;
        case creature_size::large:
            return body_stop_large;
        case creature_size::huge:
            return body_stop_huge;
        default:
            return body_stop_huge;
    }
}
} // namespace

double through_body( const projectile &bullet, const Creature &critter )
{
    if( !active() ) {
        return 0.0;
    }
    const double damage = bullet.impact.total_damage();
    if( !std::isfinite( damage ) || !( damage > 0.0 ) ) {
        return 0.0;
    }
    const double left = damage - body_stop( critter.get_size() );
    if( !( left >= min_through_damage ) ) {
        return 0.0;
    }
    n_through++;
    return left / damage;
}

bool ricochet( map &here, projectile &bullet, std::vector<tripoint_bub_ms> &path, size_t i,
               const tripoint_bub_ms &from, int range_left )
{
    if( !active() || i == 0 || i >= path.size() || range_left <= 0 ) {
        return false;
    }
    const tripoint_bub_ms wall = path[i];
    if( !here.inbounds( wall ) || !here.inbounds( from ) || wall.z() != from.z() || !here.impassable( wall ) ) {
        return false;
    }
    const int sx = wall.x() - from.x();
    const int sy = wall.y() - from.y();
    if( std::abs( sx ) > 1 || std::abs( sy ) > 1 || ( sx == 0 && sy == 0 ) ) {
        return false;
    }
    // The face met: the wall square's side toward `from`. A diagonal step met the wall beside it along x or y; a
    // corner (both) or a lone post's edge (neither) has no face to glance off.
    double nx = -sx;
    double ny = -sy;
    if( sx != 0 && sy != 0 ) {
        const tripoint_bub_ms beside_x( from.x() + sx, from.y(), from.z() );
        const tripoint_bub_ms beside_y( from.x(), from.y() + sy, from.z() );
        const bool wall_x = here.inbounds( beside_x ) && here.impassable( beside_x );
        const bool wall_y = here.inbounds( beside_y ) && here.impassable( beside_y );
        if( wall_x == wall_y ) {
            return false;
        }
        if( wall_x ) {
            ny = 0.0;
        } else {
            nx = 0.0;
        }
    }
    // The bullet's way: its whole line, from the shooter to the line's end.
    const double dx = path.back().x() - path.front().x();
    const double dy = path.back().y() - path.front().y();
    const double len = std::hypot( dx, dy );
    if( !( len > 0.0 ) ) {
        return false;
    }
    const double ux = dx / len;
    const double uy = dy / len;
    const double into = -( ux * nx + uy * ny );     // sin of the angle off the face
    if( !( into > 0.0 ) ) {
        return false;
    }
    constexpr double deg_per_rad = 57.29577951308232;
    const double glance_deg = std::asin( std::min( into, 1.0 ) ) * deg_per_rad;
    if( !( glance_deg < ricochet_max_deg ) ||
        !( rng_float( 0.0, 1.0 ) < ricochet_chance_max * ( 1.0 - glance_deg / ricochet_max_deg ) ) ) {
        return false;
    }
    // reflected off the face: u - 2 (u . n) n
    const double rx = ux + 2.0 * into * nx;
    const double ry = uy + 2.0 * into * ny;
    const int reach = std::min( range_left, ricochet_reach );
    const tripoint_bub_ms end( std::clamp<int>( from.x() + std::lround( rx * reach ), 0, MAPSIZE_X - 1 ),
                               std::clamp<int>( from.y() + std::lround( ry * reach ), 0, MAPSIZE_Y - 1 ), from.z() );
    if( end == from ) {
        return false;
    }
    const std::vector<tripoint_bub_ms> bounce = here.find_clear_path( from, end );
    if( bounce.empty() || bounce.front() == wall ) {
        return false;
    }
    path.resize( i );
    path.insert( path.end(), bounce.begin(), bounce.end() );
    bullet.impact.mult_damage( ricochet_keep );
    n_ricochets++;
    //~ %s: what the bullet glanced off (a wall's name)
    add_msg_if_player_sees( wall, _( "The bullet glances off the %s!" ), here.tername( wall ) );
    return true;
}

path_counts get_path_counts()
{
    path_counts c;
    c.through = n_through.load();
    c.ricochets = n_ricochets.load();
    return c;
}

} // namespace fp3d
