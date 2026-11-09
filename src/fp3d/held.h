#pragma once
#ifndef CATA_SRC_FP3D_HELD_H
#define CATA_SRC_FP3D_HELD_H

// FP3D: what the hands hold, and context actions that use it (client actions, only while the bridge is active).
// CDDA has one hand slot (the wielded item). The off hand (left) holds one carried item: it is marked with an
// item var, so it stays in its pocket for CDDA's rules (weight, volume) and saves keep it. The off hand is empty
// while the wielded item needs both hands or the left arm is broken.
//   fp3d_hold_off        pick a carried item to hold in the off hand (an item_select prompt)
//   fp3d_stow_off        put it away
//   fp3d_use_<DIR>       use what the hands hold on the square in direction DIR (as hands.h): a hose in one hand
//                        and a container in the other siphon a vehicle's tank there straight into that
//                        container; anything else examines the square (CDDA's examine).
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
