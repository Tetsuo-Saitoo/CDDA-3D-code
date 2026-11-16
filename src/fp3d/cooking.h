#pragma once
#ifndef CATA_SRC_FP3D_COOKING_H
#define CATA_SRC_FP3D_COOKING_H

// FP3D L9: heat sources, cookware on them, and matching a container's contents to CDDA's recipes.
//
// Heat tools are the tools CDDA's cooking heat requirements list (`surface_heat`, `water_boiling_heat`, after
// CDDA inlined them and added subtype replacements such as `fake_fireplace` for `hotplate`), read from the loaded
// data, never a fixed list. A heat source is a square that gives one to CDDA's crafting inventory: a fire
// (fd_fire, or terrain/furniture flagged USABLE_FIRE: CDDA's `fire` pseudo tool), furniture whose
// `crafting_pseudo_item` is a heat tool (fireplaces, wood stoves, ovens, smokers), or a vehicle part with one as a
// pseudo tool (kitchen unit hotplate, oven appliance). Charges and "lit" come from CDDA's own crafting inventory
// of that one square (inventory::form_from_map, range 0), built only for such squares.
//
// Cookware: items lying on a heat source whose itype has the COOK or BOIL quality, with what their CONTAINER
// pockets hold (one level) and an in-progress craft among it.
//
// Recipe queries: what a container's contents can become. Components come only from the contents; tools and
// qualities from the avatar's crafting inventory (CDDA's crafting_inventory(), PICKUP_RANGE with a clear path)
// plus the container itself emptied (CDDA's BOIL quality counts only on an empty container). Read-only: it
// never changes game state (only CDDA's own caches). Runs on the sim thread; the bridge calls it (bridge.h).

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>

#include "coords_fwd.h"
#include "fp3d/bridge.h"

class Character;
class avatar;
class item;
class map;

namespace fp3d
{
struct submap_view;

constexpr size_t max_heat_per_submap = 32;
constexpr size_t max_cookware_per_submap = 16;
constexpr size_t max_cookware_contents = 16;
constexpr size_t max_recipe_matches = 32;
constexpr int max_recipe_batch = max_cook_batch;
constexpr size_t max_match_missing = 8;
constexpr size_t max_match_name_bytes = 128;

// Sim side, from capture_submap once its tiles, piles and handles are filled: adds `heat` and `cookware` for the
// tiles of `sm` seen now (vis == seen lit). `corner`: the submap's (0,0) square in the bubble.
void capture_heat_and_cookware( map &here, const avatar &u, const tripoint_bub_ms &corner, submap_view &sm,
                                std::unordered_set<uint64_t> &seen );
// Sim side. Time spent in capture_heat_and_cookware since the last call (ms), and the squares it built a
// crafting inventory for; for tests and measurements.
double take_heat_capture_ms( size_t *candidates = nullptr );

// Sim side. The recipe match for container `c` at absolute square `pos` as JSON:
// {"id":N,"ok":true,"error":"","container":uid,"matches":[...]} (see contracts/bridge_api.md); refusals have
// ok:false and error "not found", "not a container" or "out of reach". Read-only.
std::string match_recipes_json( avatar &u, uint64_t query_id, const item_ref &c, const tripoint_abs_ms &pos );
// Sim side, for the bridge: match_recipes_json for the avatar.
std::string run_recipe_query( uint64_t query_id, const item_ref &c, const ui_point &pos );
// The refusal JSON with `error` (also used by the bridge for "too large" and "internal").
std::string recipe_query_error_json( uint64_t query_id, uint64_t container, const std::string &error );
// Sim side, for the `cook` item action (bridge.h): starts CDDA's craft `a.recipe` x `a.batch` from what the
// container `a.item` lying on `a.pos` (the avatar's square or next to it) holds. The recipe must be one
// match_recipes_json lists for it that can start, for this batch. The components are taken out of the container
// only; the tools are the heat under it if it serves, else CDDA's pick from the crafting inventory (it may ask).
// The craft lies at the avatar's feet (never on the fire, never in the hands) with item var `fp3d_cook_pot` = the
// container's handle (the pot's cookware entry shows its progress), and CDDA's craft activity runs. The container
// must lie on a lit heat source. When a craft it started for this container lies by the avatar (paused), that
// craft goes on instead (`a.recipe` must name its recipe; `a.batch` isn't used). "" when started, else why not
// (CDDA's words where it has them); nothing is used up then.
std::string start_cooking( avatar &u, const item_action &a );
// Sim side, from CDDA's craft activity (start and every turn): why the avatar's craft `craft` can't go on now,
// "" when it can. A craft the `cook` action started goes on only while its container lies on a lit heat source,
// on the avatar's square or one next to it; any other craft is never held up.
std::string cook_craft_blocked( const Character &who, const item &craft );
// Sim side, from CDDA's craft completion: the finished `result` of the avatar's craft `craft` goes into the
// container the craft names, when that lies on the avatar's square or one next to it, as much as CDDA's pocket
// rules let it hold. True when all of it went in; else `result` holds what is left for CDDA's own placement.
bool cooked_into_pot( Character &who, const item &craft, item &result );
// Sim side. How long the last match_recipes_json took (ms) and how many recipes it checked.
double last_recipe_query_ms( size_t *checked = nullptr );
} // namespace fp3d

#endif // CATA_SRC_FP3D_COOKING_H
