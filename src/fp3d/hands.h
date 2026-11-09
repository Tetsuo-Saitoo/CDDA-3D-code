#pragma once
#ifndef CATA_SRC_FP3D_HANDS_H
#define CATA_SRC_FP3D_HANDS_H

// FP3D: hand attacks for first person and VR (client actions, only while the bridge is active).
// Main-loop actions "fp3d_attack_main_<DIR>" and "fp3d_attack_off_<DIR>", DIR one of the eight move actions
// (UP is north, RIGHTUP, RIGHT, RIGHTDOWN, DOWN, LEFTDOWN, LEFT, LEFTUP). The main hand (right) is CDDA's
// melee attack with what is wielded. The off hand (left) is an unarmed strike, as CDDA fights unarmed
// (strength, unarmed skill, the style's unarmed techniques, gloves); it is refused while the wielded item
// needs both hands or the left arm is broken. Nobody there (or someone friendly): a message, no time.

#include <string>
#include <vector>

class avatar;
class input_context;

namespace fp3d
{
void register_hand_actions( input_context &ctxt );
bool is_hand_action( const std::string &action );
// Runs a hand action; true when the attack happened (it spent the avatar's moves).
bool hand_attack( avatar &u, const std::string &action );
// True while an off-hand strike runs: Character::used_weapon() then gives the avatar no weapon.
bool offhand_strike();
// The eight direction names (UP ... LEFTUP) and the step of one; false for an unknown name.
std::vector<std::string> hand_dir_names();
bool hand_dir( const std::string &name, int &dx, int &dy );
} // namespace fp3d

#endif // CATA_SRC_FP3D_HANDS_H
