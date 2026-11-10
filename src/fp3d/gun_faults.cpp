#include "fp3d/gun_faults.h"

#include <algorithm>
#include <string>

#include "calendar.h"
#include "character.h"
#include "fault.h"
#include "fp3d/bridge.h"
#include "inventory.h"
#include "item.h"
#include "item_location.h"
#include "messages.h"
#include "player_activity.h"
#include "requirements.h"
#include "string_formatter.h"
#include "translations.h"

static const activity_id ACT_MEND_ITEM( "ACT_MEND_ITEM" );

namespace fp3d
{
namespace
{
const std::string gun_mechanical_simple = "gun_mechanical_simple";

bool applies( const Character &who )
{
    return who.is_avatar() && active();
}
} // namespace

fault_id jam_of( const item &gun )
{
    if( !gun.is_gun() ) {
        return fault_id::NULL_ID();
    }
    return faults::random_of_type_item_has( gun, gun_mechanical_simple );   // the first in load order, not random
}

bool refuse_jammed_aim( Character &who, const item &gun )
{
    if( !applies( who ) ) {
        return false;
    }
    const fault_id jam = jam_of( gun );
    if( jam.is_null() ) {
        return false;
    }
    //~ %1$s: gun name, %2$s: fault name (stovepipe, double feed...)
    who.add_msg_if_player( m_bad, _( "Your %1$s is jammed (%2$s).  Clear it first." ), gun.tname(),
                           jam->name() );
    return true;
}

bool stop_jammed_burst( const Character &who, const item &gun )
{
    if( !applies( who ) || jam_of( gun ).is_null() ) {
        return false;
    }
    who.add_msg_if_player( m_bad, _( "Your %s is jammed." ), gun.tname() );
    return true;
}

std::string clear_jam( Character &who, const item_location &gun )
{
    if( !gun || !gun->is_gun() ) {
        return _( "That isn't a gun." );
    }
    const item_location wielded = who.get_wielded_item();
    if( !wielded || wielded.get_item() != gun.get_item() ) {
        return _( "Hold the gun to clear it." );
    }
    const fault_id jam = jam_of( *gun );
    if( jam.is_null() ) {
        return string_format( _( "Your %s isn't jammed." ), gun->tname() );
    }
    if( who.activity ) {
        return _( "You are busy." );
    }
    // No light check on purpose: CDDA's mend command refuses when it's too dark (fine_detail_vision_mod() > 4),
    // but its own quick fix at aim start never did; a stovepipe or a double feed is cleared by feel.
    const inventory &inv = who.crafting_inventory();
    for( const fault_fix_id &fix_id : jam->get_fixes() ) {
        const fault_fix &fix = *fix_id;
        bool doable = true;
        for( const auto &[skill, level] : fix.skills ) {
            if( who.get_greater_skill_or_knowledge_level( skill ) < level ) {
                doable = false;
                break;
            }
        }
        if( !doable || !fix.get_requirements().can_make_with_inventory( inv, is_crafting_component ) ) {
            continue;
        }
        // as Character::mend_item after its menu
        time_duration time = fix.time;
        for( const auto &[flag, mult] : fix.time_save_flags ) {
            if( gun->has_flag( flag ) ) {
                time *= mult;
            }
        }
        for( const auto &[prof, mult] : fix.time_save_profs ) {
            if( who.has_proficiency( prof ) ) {
                time *= mult;
            }
        }
        who.assign_activity( ACT_MEND_ITEM, std::max( to_moves<int>( time ), 1 ) );
        who.activity.name = jam.str();
        who.activity.str_values.emplace_back( fix.id.str() );
        who.activity.targets.push_back( gun );
        //~ %1$s: what clearing it is called (Manual cycle...), %2$s: gun name
        who.add_msg_if_player( _( "%1$s: %2$s." ), fix.name.translated(), gun->tname() );
        return std::string();
    }
    return string_format( _( "You can't clear the %1$s of your %2$s here." ), jam->name(), gun->tname() );
}

bool rack_jammed( Character &who )
{
    if( !applies( who ) ) {
        return false;
    }
    const item_location gun = who.get_wielded_item();
    if( !gun || jam_of( *gun ).is_null() ) {
        return false;
    }
    const std::string why = clear_jam( who, gun );
    if( !why.empty() ) {
        who.add_msg_if_player( m_info, why );
    }
    return true;
}

} // namespace fp3d
