#pragma once
#ifndef CATA_SRC_FP3D_HANDS_H
#define CATA_SRC_FP3D_HANDS_H

// FP3D: hand attacks for first person and VR (client actions, only while the bridge is active).
// Main-loop actions "fp3d_attack_main_<DIR>" and "fp3d_attack_off_<DIR>", DIR one of the eight move actions
// (UP is north, RIGHTUP, RIGHT, RIGHTDOWN, DOWN, LEFTDOWN, LEFT, LEFTUP). The main hand (right) is CDDA's
// melee attack with what is wielded. The off hand (left) strikes with what it holds (fp3d/held.h), else unarmed,
// as CDDA fights unarmed (strength, unarmed skill, the style's unarmed techniques, gloves); it is refused while the
// wielded item needs both hands or the left arm is broken. Nobody there (or someone friendly): a message, no time.
// Nobody next to you that way: the main hand's weapon with reach strikes the first creature along it (CDDA's
// reach attack, up to its reach, 3 at most). "fp3d_attack_<main|off>_aim_<DIR>" is the same strike, aimed: the
// client's crosshair is on that creature, so the swing asks a `swing` request (fp3d/swing.h).

#include <string>
#include <vector>

#include "item_location.h"

class avatar;
class input_context;

namespace fp3d
{
void register_hand_actions( input_context &ctxt );
bool is_hand_action( const std::string &action );
// Runs a hand action; true when the attack happened (it spent the avatar's moves).
bool hand_attack( avatar &u, const std::string &action );
// True while an off-hand strike runs: Character::used_weapon() then gives offhand_weapon() (empty = unarmed).
bool offhand_strike();
item_location offhand_weapon();
// The eight direction names (UP ... LEFTUP) and the step of one; false for an unknown name.
std::vector<std::string> hand_dir_names();
bool hand_dir( const std::string &name, int &dx, int &dy );
} // namespace fp3d

#endif // CATA_SRC_FP3D_HANDS_H
