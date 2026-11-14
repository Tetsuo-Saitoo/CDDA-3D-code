#pragma once
#ifndef CATA_SRC_FP3D_ITEM_HANDS_H
#define CATA_SRC_FP3D_ITEM_HANDS_H

// FP3D L8/E6: handles for single items and menu-free item actions (bridge.h `item_action`): take one item from a
// square (into the inventory, the main hand or the off hand), put a carried item into a container (a worn or held
// one, or one lying in reach, into CDDA's best pocket or a given one), drop or place it on a square, unload a gun or
// magazine (carried or in reach; what comes out goes to the inventory, the main hand or the off hand), reload a
// carried gun or magazine with given ammo or a given magazine (carried or in reach), clear the wielded gun's jam
// (gun_faults.h), throw a carried item at a square (CDDA's throw without its target menu), pour the liquid out of a
// carried container onto a square or into a container (CDDA's liquid rules without its liquid menu). Each action
// finds its items again by handle, square and itype, uses CDDA's rules (pockets, weight, volume, wielding, what
// loads into what, throw range) and move costs, and refuses with a message saying why. Nothing is ever asked: where
// CDDA would show a menu, the choice is fixed (unload: the item itself, then its gunmods in order; reload: the first
// part CDDA's reload menu lists that takes the ammo now; pour: the client names where) or the action is refused
// (liquids out of a magazine, a loaded item outgrowing its pocket, throwing at a non-hostile NPC).
//
// Handle: a uid in the item var `fp3d_uid` (decimal text, 1 .. max_item_uid), given the first time a snapshot
// lists the item. Saves keep it and CDDA's stacking ignores it (item.cpp). A copy made by splitting a stack
// carries the same uid; the snapshot gives the second one listed a new uid. A uid alone is never trusted: actions
// also check the square (or that the avatar carries it) and the itype.
// Spot: `fp3d_spot` = "x,y" in permille of the square, set when the client places an item; erased when the item
// is picked up. Pose: `fp3d_pose` = "yaw,side" (whole degrees 0..359, 0 or 1), only next to a spot: how the client's
// physics left the item lying. Vars come from saves, which are hostile input like the client: all are parsed strictly.
//
// Three actions are for a client that simulates lying items (VR item physics): `place` moves a lying item one
// square, `spot` stores how it rests, both without time; `hold` takes a carried item into a hand. Free actions are
// bounded per turn (max_free_item_ops, one move per item) and reach no further than physics_reach from the avatar.
// An action with a tag is answered by an `item_result` event (events.h).

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>

class avatar;
class input_context;
class item;

namespace fp3d
{
constexpr size_t max_handles_per_tile = 32;   // items listed per square, in CDDA's stack order
constexpr size_t max_carried_listed = 256;    // carried items listed, depth first
constexpr int max_carried_depth = 8;          // levels listed: top level + 7 levels of containers inside
constexpr size_t max_body_parts_listed = 32;  // player.body_parts, worn covers (client's MAX_BODY_PARTS)

// The item's handle, given now if it has none or an unreadable one. `seen` (optional): uids already listed in
// this capture; a repeat gets a new uid. Sim side.
uint64_t item_handle( item &it, std::unordered_set<uint64_t> *seen = nullptr );
// The item's handle, 0 if it has none (or a malformed one).
uint64_t read_item_handle( const item &it );
// The spot the client placed the item on (permille), false if none or malformed.
bool item_spot( const item &it, uint16_t &x, uint16_t &y );
// How the item lies on its spot (yaw in whole degrees), false if none, malformed or without a spot.
bool item_pose( const item &it, uint16_t &yaw, bool &side );
// place, spot: how far from the avatar's square the item may lie (squares, same level).
constexpr int physics_reach = 2;
// place, spot: how many run in one turn (they cost no time); more are refused until the next turn.
constexpr int max_free_item_ops = 16;

void register_item_actions( input_context &ctxt );
bool is_item_action( const std::string &action );
// Takes the item action the bridge answered with (bridge.h) and runs it; true when it took time.
bool run_item_action( avatar &u );
} // namespace fp3d

#endif // CATA_SRC_FP3D_ITEM_HANDS_H
