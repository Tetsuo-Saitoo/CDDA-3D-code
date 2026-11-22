#pragma once
#ifndef CATA_SRC_FP3D_GLOVEBOX_H
#define CATA_SRC_FP3D_GLOVEBOX_H

// FP3D: a glovebox, which CDDA 0.I doesn't have. The part and its item are data
// (data/json/vehicleparts/fp3d_glovebox.json: vehicle part and item `fp3d_glovebox`, a covered 5 L cargo space).
// No prototype lists it: add_glovebox puts one into the blueprint CDDA builds for each prototype, so every vehicle
// spawned from then on has it, and CDDA's own vehicle files stay as upstream has them. A vehicle in a save keeps
// the parts it was saved with; one saved before this simply has no glovebox.
//
// Which vehicles: those with wheels and no rotor whose controls are on a seat under a roof (cars, trucks, vans),
// which leaves out bikes, carts, trailers, tractors, boats and helicopters.
// Where: CDDA allows one cargo part per tile and every seat is one, so the glovebox sits on the tile ahead (+x) of
// the seat beside the driver's (the nearest within two tiles to either side), else ahead of the driver's; only on
// a tile with a windshield, and only where CDDA's own rule lets it be mounted. A vehicle with no such tile gets
// none.

#include <optional>

#include "coordinates.h"
#include "type_id.h"

class map;
class vehicle;

namespace fp3d
{
// The glovebox's vehicle part id.
const vpart_id &glovebox_part();
// For vehicles::finalize_prototypes, once CDDA has installed a prototype's own parts: adds the glovebox to this
// blueprint if the vehicle is one that gets it. Returns the mount it went to.
std::optional<point_rel_ms> add_glovebox( map &here, vehicle &blueprint );
} // namespace fp3d

#endif // CATA_SRC_FP3D_GLOVEBOX_H
