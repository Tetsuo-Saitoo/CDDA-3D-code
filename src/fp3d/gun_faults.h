#pragma once
#ifndef CATA_SRC_FP3D_GUN_FAULTS_H
#define CATA_SRC_FP3D_GUN_FAULTS_H

// Gun jams in FP3D games (combat block 3): CDDA's own jam faults and fixes, without its menus or its random quick fix.
// - A gun with a gun_mechanical_simple fault (failure to feed, stovepipe, double feed, spent case in the chamber) is
//   jammed. Aiming it stops with a message saying how to clear it, instead of CDDA's random quick fix at aim start;
//   a burst stops at a jam set by an earlier round (CDDA's failure to cycle doesn't stop it).
// - clear_jam starts CDDA's own mend of that fault with the first fix the character can do: its skills, tools and
//   time (with the time-saving flags and proficiencies), as Character::mend_item does after its menu. The reload
//   action (the client's R / rack) does that when the wielded gun is jammed, instead of reloading.
// Only for the avatar while the bridge is active; NPCs and plain CDDA keep CDDA's behaviour. Sim thread only.

#include <string>

#include "type_id.h"

class Character;
class item;
class item_location;

namespace fp3d
{

// The gun's jam fault, else fault_id::NULL_ID(). Not a gun: NULL_ID.
fault_id jam_of( const item &gun );

// Aim start (aim_activity_actor): true, after a message, when `who` is the avatar in an FP3D game and `gun` is
// jammed, so the aim stops instead of CDDA's random quick fix.
bool refuse_jammed_aim( Character &who, const item &gun );

// Each round of the avatar's trigger pull in an FP3D game (Character::fire_gun): true, after a message, when `gun`
// is already jammed, so the burst stops there.
bool stop_jammed_burst( const Character &who, const item &gun );

// Starts clearing the jam of `gun`, which must be `who`'s wielded gun: CDDA's mend activity with the first fix of
// its jam fault that `who` can do. "" when started, else why not (shown to the player as is).
std::string clear_jam( Character &who, const item_location &gun );

// The reload action (ACTION_RELOAD_WEAPON): when `who` is the avatar in an FP3D game and the wielded gun is jammed,
// starts clearing it (or says why not) and returns true, so nothing is reloaded this time.
bool rack_jammed( Character &who );

} // namespace fp3d

#endif // CATA_SRC_FP3D_GUN_FAULTS_H
