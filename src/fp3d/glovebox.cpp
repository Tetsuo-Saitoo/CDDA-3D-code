// FP3D: see glovebox.h.
#include "fp3d/glovebox.h"

#include <array>
#include <string>

#include "point.h"
#include "ret_val.h"
#include "veh_type.h"
#include "vehicle.h"

static const vpart_id vpart_fp3d_glovebox( "fp3d_glovebox" );

namespace fp3d
{
namespace
{
bool tile_has( const vehicle &veh, const point_rel_ms &at, const std::string &flag )
{
    for( int i = 0; i < veh.part_count(); i++ ) {
        const vehicle_part &vp = veh.part( i );
        if( vp.mount == at && !vp.removed && vp.info().has_flag( flag ) ) {
            return true;
        }
    }
    return false;
}
} // namespace

const vpart_id &glovebox_part()
{
    return vpart_fp3d_glovebox;
}

std::optional<point_rel_ms> add_glovebox( map &here, vehicle &blueprint )
{
    if( !vpart_fp3d_glovebox.is_valid() ) {
        return std::nullopt;
    }
    const vpart_info &box = *vpart_fp3d_glovebox;
    bool wheels = false;
    for( int i = 0; i < blueprint.part_count(); i++ ) {
        const vpart_info &vpi = blueprint.part( i ).info();
        if( vpi.id == vpart_fp3d_glovebox || vpi.has_flag( "ROTOR" ) ) {
            return std::nullopt;
        }
        wheels = wheels || vpi.has_flag( "WHEEL" );
    }
    if( !wheels ) {
        return std::nullopt;
    }
    // The cab: the first controls that are on a seat under a roof.
    std::optional<point_rel_ms> cab;
    for( int i = 0; i < blueprint.part_count() && !cab; i++ ) {
        const vehicle_part &vp = blueprint.part( i );
        if( vp.info().has_flag( "CONTROLS" ) && tile_has( blueprint, vp.mount, "SEAT" ) &&
            tile_has( blueprint, vp.mount, "ROOF" ) ) {
            cab = vp.mount;
        }
    }
    if( !cab ) {
        return std::nullopt;
    }
    // Ahead of the seat beside the driver's, the nearest first; last, ahead of the driver's own.
    static constexpr std::array<int, 5> beside = { 1, -1, 2, -2, 0 };
    for( const int dy : beside ) {
        const point_rel_ms seat = *cab + point( 0, dy );
        if( dy != 0 && !tile_has( blueprint, seat, "SEAT" ) ) {
            continue;
        }
        const point_rel_ms at = seat + point::east;
        if( !tile_has( blueprint, at, "WINDSHIELD" ) || !blueprint.can_mount( at, box ).success() ) {
            continue;
        }
        const int index = blueprint.install_part( here, at, vpart_fp3d_glovebox );
        if( index < 0 ) {
            return std::nullopt;
        }
        // As finalize_prototypes does for a part with no variant named: the part's first one.
        if( !box.variants.empty() ) {
            blueprint.part( index ).variant = box.variants.begin()->first;
        }
        return at;
    }
    return std::nullopt;
}
} // namespace fp3d
