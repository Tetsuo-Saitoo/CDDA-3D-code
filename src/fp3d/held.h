#pragma once
#ifndef CATA_SRC_FP3D_HELD_H
#define CATA_SRC_FP3D_HELD_H

// FP3D: what the hands hold, and context actions that use it (client actions, only while the bridge is active).
// CDDA has one hand slot (the wielded item). The off hand (left) holds one carried item: it is marked with an
// item var holding a token the avatar keeps too, so it stays in its pocket for CDDA's rules (weight, volume),
// saves keep it, and once it leaves the avatar (dropped, wielded, traded) it isn't held again on return. The off
// hand is empty while the wielded item needs both hands or the left arm is broken; an off-hand strike (hands.h)
// hits with the held item.
//   fp3d_hold_off        pick a carried one-handed item to hold in the off hand (an item_select prompt; a
//                        message, not CDDA's popup, when nothing fits)
//   fp3d_stow_off        put it away
//   fp3d_use_<DIR>       use what the hands hold on the square in direction DIR (as hands.h), first that fits:
//                        a hose + a container in the other hand + a vehicle: siphon its tank into the container;
//                        a held container with liquid + a vehicle with a tank that takes it: pour it in (refuel);
//                        a held container + liquid there (river, lake, keg, barrel, puddle): fill it;
//                        else CDDA's examine of the square. No menus unless CDDA must ask (which tank).
// A liquid fill runs one turn per second of wall time (watched_activity), so the client can show it filling.

#include <cstdint>
#include <string>

class avatar;
class input_context;
class item;
class item_location;

namespace fp3d
{
void register_held_actions( input_context &ctxt );
bool is_held_action( const std::string &action );
// Runs a held action; true when something happened (moves spent or an activity started).
bool held_action( avatar &u, const std::string &action );
// The item held in the off hand, or an empty location.
item_location offhand_item( avatar &u );
// True while the avatar runs an activity the client shows as it happens (a liquid fill): paced in real time.
bool watched_activity( const avatar &u );

struct hand_view {
    bool held = false;
    std::string id;              // itype id
    std::string name;            // display name, no colour tags
    std::string liquid;          // the liquid inside ("" none)
    int64_t ml = 0;              // volume inside a container
    int64_t capacity_ml = 0;     // container capacity (0: not a container)
};
struct held_view {
    uint64_t seq = 0;            // bumps on every publish
    int64_t turn = 0;
    hand_view main;              // wielded (right hand)
    hand_view off;               // off hand (left)
    std::string activity;        // the avatar's activity id ("" none)
};
// Sim side: copies the hands for the client. Called before every request and once per turn.
void publish_held();
// Client side.
held_view latest_held();
} // namespace fp3d

#endif // CATA_SRC_FP3D_HELD_H
