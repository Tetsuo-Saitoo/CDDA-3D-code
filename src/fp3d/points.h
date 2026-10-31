#pragma once
#ifndef CATA_SRC_FP3D_POINTS_H
#define CATA_SRC_FP3D_POINTS_H

// FP3D: conversions between CDDA coordinates and the bridge's plain points.

#include "coordinates.h"
#include "fp3d/bridge.h"

namespace fp3d
{
inline ui_point to_ui_point( const tripoint_abs_ms &p )
{
    return ui_point{ p.x(), p.y(), p.z() };
}

inline tripoint_abs_ms to_abs_ms( const ui_point &p )
{
    return tripoint_abs_ms( p.x, p.y, p.z );
}
} // namespace fp3d

#endif // CATA_SRC_FP3D_POINTS_H
