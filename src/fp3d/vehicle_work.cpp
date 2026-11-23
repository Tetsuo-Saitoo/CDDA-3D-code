// FP3D: see vehicle_work.h.
#include "fp3d/vehicle_work.h"

#include <algorithm>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "character.h"
#include "coordinates.h"
#include "cursesdef.h"
#include "fault.h"
#include "fp3d/bridge.h"
#include "fp3d/events.h"
#include "fp3d/snapshot.h"
#include "item.h"
#include "itype.h"
#include "json.h"
#include "map.h"
#include "messages.h"
#include "monster.h"
#include "mtype.h"
#include "output.h"
#include "player_activity.h"
#include "point.h"
#include "rng.h"
#include "string_formatter.h"
#include "translations.h"
#include "type_id.h"
#include "veh_interact.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"

static const ammotype ammo_battery( "battery" );

static const trait_id trait_DEBUG_HS( "DEBUG_HS" );
static const trait_id trait_SHELL2( "SHELL2" );
static const trait_id trait_SHELL3( "SHELL3" );

namespace fp3d
{
namespace
{

struct line_view {
    std::string text;
    int met = 0;                     // 0 a heading or a note, 1 a requirement that is met, 2 one that is not
};

struct command_view {
    char cmd = ' ';
    bool ok = false;
    int reason = 0;                  // CDDA's task_reason
    std::string text;                // CDDA's sentence when it refuses outright
    int64_t time_s = 0;
    std::vector<line_view> lines;
};

struct part_view {
    int row = 0;
    int mount_x = 0;
    int mount_y = 0;
    uint32_t vpart = 0;
    std::string name;
    std::string variant;
    int hp = 0;
    int damage = 0;
    int flags = 0;
    std::vector<std::string> faults;
    // What a tank, a battery or another fuel store holds, as CDDA's vehicle overview shows it.
    bool holds = false;
    std::string fill_what;           // the name of what is in it, "" when empty
    int64_t fill_now = 0;
    int64_t fill_max = 0;            // 0 when CDDA can't tell (an empty fuel store that takes several kinds)
    bool fill_ml = false;            // a tank: millilitres; else CDDA's charges
};

// A part type CDDA's screen lists for a square.
struct type_view {
    uint32_t vpart = 0;
    int bits = 0;                    // type_have, type_fits
};
constexpr int type_have = 1;         // the avatar has its components and tools (can_potentially_install)
constexpr int type_fits = 2;         // it can go on the square as the vehicle is now (vehicle::can_mount)

struct work_view {
    uint64_t vehicle = 0;
    std::string name;
    int64_t turn = 0;
    bool owned = true;               // the avatar's or nobody's: CDDA asks nothing before work on it
    std::vector<part_view> parts;
    size_t parts_more = 0;
    bool has_selected = false;
    int selected_row = -1;
    std::vector<int> here;
    std::vector<command_view> commands;
    // The install list for a square, in place of a part's commands.
    bool has_install = false;
    int install_x = 0;
    int install_y = 0;
    int install_reason = 0;          // CDDA's task_reason for installing anything there
    std::string install_text;
    std::vector<type_view> types;
    size_t types_more = 0;
    bool has_type = false;           // a type was picked
    uint32_t type_vpart = 0;
    command_view type_command;
};

constexpr int part_broken = 1;
constexpr int part_repairable = 2;
constexpr int part_faulty = 4;
constexpr int part_refillable = 8;
constexpr int part_racked = 16;

std::string line_text( const std::string &raw )
{
    return client_text( raw, max_work_line_columns, 4 * max_work_line_columns );
}

// A message of CDDA's up to its first line break, as the client gets it.
std::string first_line( const std::string &raw )
{
    return line_text( raw.substr( 0, raw.find( '\n' ) ) );
}

// What CDDA's colours on a line say: 1 something on it is green, 2 something is red or brown and nothing green.
int colour_of( const std::string &line )
{
    bool good = false;
    bool bad = false;
    size_t at = 0;
    while( ( at = line.find( "<color_", at ) ) != std::string::npos ) {
        at += 7;
        const size_t end = line.find( '>', at );
        if( end == std::string::npos ) {
            break;
        }
        std::string name = line.substr( at, end - at );
        if( name.rfind( "c_", 0 ) == 0 ) {
            name.erase( 0, 2 );
        }
        good = good || name == "green" || name == "light_green";
        bad = bad || name == "red" || name == "light_red" || name == "brown";
        at = end;
    }
    return good ? 1 : bad ? 2 : 0;
}

// CDDA's requirement text, one entry a line. Under its requirement headings (skills, components, tools, the
// additional ones) each line is one requirement and its colour says whether it is met: green for an alternative
// that is there, red for none (brown: not enough of it). Anywhere else a line is a note (the time, what a removal
// yields, the description: an item's name there carries its own condition colours), except CDDA's red sentences
// that refuse the work outright.
std::vector<line_view> lines_of( const std::string &colored )
{
    static const std::set<std::string> requirement_headings = {
        first_line( _( "<color_white>Skills required:</color>\n" ) ),
        first_line( _( "Components required:" ) ),
        first_line( _( "Tools required:" ) ),
        first_line( _( "<color_white>Additional requirements:</color>\n" ) ),
        first_line( _( "<color_white>Cannot install due to:</color>\n> " ) )
    };
    std::vector<line_view> out;
    std::istringstream in( colored );
    std::string line;
    bool requirements = false;
    while( std::getline( in, line ) && out.size() < max_work_lines ) {
        line_view v;
        v.text = line_text( line );
        if( v.text.find_first_not_of( ' ' ) == std::string::npos ) {
            continue;
        }
        const bool listed = v.text.rfind( "> ", 0 ) == 0 || v.text.rfind( "  ", 0 ) == 0;
        if( requirement_headings.count( v.text ) > 0 ) {
            requirements = true;
        } else if( !listed ) {
            requirements = false;
            v.met = colour_of( line ) == 2 ? 2 : 0;
        } else if( requirements ) {
            v.met = colour_of( line );
        }
        out.push_back( std::move( v ) );
    }
    return out;
}

int64_t seconds_of( const time_duration &d, const avatar &u )
{
    // CDDA's debug trait makes any vehicle work take a second (veh_interact::serialize_activity).
    return u.has_trait( trait_DEBUG_HS ) ? 1 : std::max<int64_t>( 0, to_seconds<int64_t>( d ) );
}

bool plain_id( const std::string &s )
{
    return s.size() <= 64 && std::all_of( s.begin(), s.end(), []( unsigned char ch ) {
        return ch > 0x20 && ch < 0x7f && ch != '"' && ch != '\\';
    } );
}

// CDDA's rule for examining a vehicle: a part of it on the avatar's square or one next to it.
bool within_reach( map &here, const avatar &u, const vehicle &veh )
{
    for( const vpart_reference &vp : veh.get_all_parts() ) {
        const tripoint_bub_ms p = vp.pos_bub( here );
        if( p.z() == u.posz() && square_dist( p, u.pos_bub() ) <= 1 ) {
            return true;
        }
    }
    return false;
}

// CDDA's own gates on the way to its vehicle screen: nothing is examined from inside a shell
// (game::do_regular_action), and a rider gets no vehicle, a mech's aside (game::examine). The read's error name
// and CDDA's sentence.
struct gate {
    const char *error;
    std::string text;
};
std::optional<gate> examine_gate( const Character &u )
{
    if( u.has_active_mutation( trait_SHELL2 ) || u.has_active_mutation( trait_SHELL3 ) ) {
        return gate{ "in shell", _( "You can't examine your surroundings while you're in your shell." ) };
    }
    if( u.is_mounted() && !u.mounted_creature->has_flag( mon_flag_RIDEABLE_MECH ) ) {
        return gate{ "mounted", _( "You cannot interact with a vehicle while mounted." ) };
    }
    return std::nullopt;
}

// CDDA opens another screen for an appliance (game::exam_appliance) and none for a summoned vehicle.
bool workable( const vehicle &veh )
{
    return !veh.is_appliance() && !veh.magic;
}

part_view part_of( const vehicle_part &pt, int row )
{
    part_view v;
    v.row = row;
    v.mount_x = pt.mount.x();
    v.mount_y = pt.mount.y();
    v.vpart = intern_id( id_kind::vpart, pt.info().id.str() );
    v.name = client_text( pt.name( false ), max_work_name_columns, 4 * max_work_name_columns );
    v.variant = plain_id( pt.variant ) ? pt.variant : std::string();
    v.hp = std::clamp( static_cast<int>( 100.0 * pt.health_percent() ), 0, 100 );
    v.damage = std::clamp( pt.damage_level(), 0, 100 );
    v.flags = ( pt.is_broken() ? part_broken : 0 ) | ( pt.is_repairable() ? part_repairable : 0 ) |
              ( pt.faults().empty() ? 0 : part_faulty ) | ( pt.can_reload() ? part_refillable : 0 ) |
              ( pt.has_flag( vp_flag::carried_flag ) ? part_racked : 0 );
    for( const fault_id &f : pt.faults() ) {
        if( v.faults.size() >= max_work_faults ) {
            break;
        }
        if( f.is_valid() ) {
            v.faults.push_back( client_text( f->name(), max_work_name_columns, 4 * max_work_name_columns ) );
        }
    }
    // veh_interact::calc_overview: a tank by the volume in it, a battery and other fuel stores by their charges;
    // a turret's ammunition is not a fill.
    const itype_id in_it = pt.ammo_current();
    if( pt.is_tank() ) {
        v.holds = true;
        v.fill_ml = true;
        v.fill_now = units::to_milliliter( pt.get_base().get_total_contained_volume() );
        v.fill_max = units::to_milliliter( pt.get_base().get_total_capacity() );
    } else if( pt.is_battery() || ( pt.is_fuel_store() && !pt.is_turret() ) ) {
        v.holds = true;
        v.fill_now = pt.ammo_remaining();
        if( pt.is_battery() ) {
            v.fill_max = pt.ammo_capacity( ammo_battery );
        } else if( !in_it.is_null() && item::find_type( in_it )->ammo ) {
            v.fill_max = pt.ammo_capacity( item::find_type( in_it )->ammo->type );
        }
    }
    if( v.holds ) {
        v.fill_now = std::max<int64_t>( 0, v.fill_now );
        v.fill_max = std::max<int64_t>( 0, v.fill_max );
        if( !in_it.is_null() && v.fill_now > 0 ) {
            v.fill_what = client_text( item::nname( in_it ), max_work_name_columns, 4 * max_work_name_columns );
        }
    }
    return v;
}

// One command as JSON. `vpart`: the type it is about (install), 0 none.
void write_command( JsonOut &jo, const command_view &c, uint32_t vpart )
{
    jo.start_object();
    jo.member( "cmd", std::string( 1, c.cmd ) );
    if( vpart != 0 ) {
        jo.member( "vpart", static_cast<int64_t>( vpart ) );
    }
    jo.member( "ok", c.ok );
    jo.member( "reason", c.reason );
    jo.member( "text", c.text );
    jo.member( "time_s", c.time_s );
    jo.member( "lines" );
    jo.start_array();
    for( const line_view &l : c.lines ) {
        jo.start_object();
        jo.member( "text", l.text );
        jo.member( "met", l.met );
        jo.end_object();
    }
    jo.end_array();
    jo.end_object();
}

std::string write_work( uint64_t query_id, const work_view &w, bool truncated )
{
    std::ostringstream os;
    JsonOut jo( os );
    jo.start_object();
    jo.member( "id", static_cast<int64_t>( query_id ) );
    jo.member( "ok", true );
    jo.member( "kind", std::string( "vehicle_work" ) );
    jo.member( "vehicle", static_cast<int64_t>( w.vehicle ) );
    jo.member( "name", w.name );
    jo.member( "turn", w.turn );
    jo.member( "owned", w.owned );
    jo.member( "parts" );
    jo.start_array();
    for( const part_view &p : w.parts ) {
        jo.start_object();
        jo.member( "row", p.row );
        jo.member( "mount" );
        jo.start_array();
        jo.write( p.mount_x );
        jo.write( p.mount_y );
        jo.end_array();
        jo.member( "vpart", static_cast<int64_t>( p.vpart ) );
        jo.member( "name", p.name );
        jo.member( "variant", p.variant );
        jo.member( "hp", p.hp );
        jo.member( "damage", p.damage );
        jo.member( "flags", p.flags );
        if( !p.faults.empty() ) {
            jo.member( "faults" );
            jo.start_array();
            for( const std::string &f : p.faults ) {
                jo.write( f );
            }
            jo.end_array();
        }
        if( p.holds ) {
            jo.member( "fill" );
            jo.start_object();
            jo.member( "what", p.fill_what );
            jo.member( "now", p.fill_now );
            jo.member( "max", p.fill_max );
            jo.member( "unit", std::string( p.fill_ml ? "ml" : "charges" ) );
            jo.end_object();
        }
        jo.end_object();
    }
    jo.end_array();
    jo.member( "parts_more", static_cast<int64_t>( w.parts_more ) );
    if( w.has_selected ) {
        jo.member( "selected" );
        jo.start_object();
        jo.member( "row", w.selected_row );
        jo.member( "here" );
        jo.start_array();
        for( const int row : w.here ) {
            jo.write( row );
        }
        jo.end_array();
        jo.member( "commands" );
        jo.start_array();
        for( const command_view &c : w.commands ) {
            write_command( jo, c, 0 );
        }
        jo.end_array();
        jo.end_object();
    }
    if( w.has_install ) {
        jo.member( "install" );
        jo.start_object();
        jo.member( "mount" );
        jo.start_array();
        jo.write( w.install_x );
        jo.write( w.install_y );
        jo.end_array();
        jo.member( "reason", w.install_reason );
        jo.member( "text", w.install_text );
        jo.member( "types" );
        jo.start_array();
        for( const type_view &t : w.types ) {
            jo.start_array();
            jo.write( static_cast<int64_t>( t.vpart ) );
            jo.write( t.bits );
            jo.end_array();
        }
        jo.end_array();
        jo.member( "types_more", static_cast<int64_t>( w.types_more ) );
        if( w.has_type ) {
            jo.member( "selected" );
            write_command( jo, w.type_command, w.type_vpart );
        }
        jo.end_object();
    }
    jo.member( "truncated", truncated );
    jo.end_object();
    return os.str();
}

} // namespace

// The friend veh_interact names: everything that touches CDDA's vehicle screen is in here.
struct vehicle_work_access {
    // The checks fold their text to the message window's width. A wide one, never drawn, keeps each requirement
    // on its own line.
    static void widen( veh_interact &vi ) {
        vi.w_msg = catacurses::newwin( 4, static_cast<int>( max_work_line_columns ) + 4, point::zero );
    }

    // The commands for the part `part_index` of `veh`, and the rows of the parts on its square. `rows`: CDDA's
    // part index of each snapshot part row.
    static void fill_selected( map &here, vehicle &veh, int part_index, const std::vector<int> &rows,
                               work_view &out ) {
        // CDDA's screen with its cursor on the part's square, never shown: the cursor's offset is minus the mount
        // (veh_interact::move_cursor).
        veh_interact vi( here, veh, -veh.part( part_index ).mount );
        widen( vi );
        for( const int index : vi.parts_here ) {
            const auto row = std::find( rows.begin(), rows.end(), index );
            if( row != rows.end() && out.here.size() < max_work_here ) {
                out.here.push_back( static_cast<int>( row - rows.begin() ) );
            }
        }
        commands_for( vi, here, veh, part_index, "romf", out.commands );
    }

    // What CDDA's screen `vi`, its cursor on the part's square, says about each command in `which` (of r, o, m,
    // f) for the part `part_index`, in that order.
    static void commands_for( veh_interact &vi, map &here, vehicle &veh, int part_index, const std::string &which,
                              std::vector<command_view> &out ) {
        avatar &u = get_avatar();
        vehicle_part &pt = veh.part( part_index );
        // CDDA works on the parts of the cursor's square; a part it doesn't count there is no target at all.
        const bool there = std::find( vi.parts_here.begin(), vi.parts_here.end(),
                                      part_index ) != vi.parts_here.end();
        const std::string dark = _( "It's too dark to see what you are doing…" );
        const int no_target = static_cast<int>( task_reason::INVALID_TARGET );
        const auto begin = []( char cmd, task_reason reason ) {
            command_view c;
            c.cmd = cmd;
            c.reason = static_cast<int>( reason );
            return c;
        };

        if( which.find( 'r' ) != std::string::npos ) {
            // Repair, or replace when broken: do_repair.
            const task_reason reason = vi.cant_do( here, 'r' );
            command_view c = begin( 'r', reason );
            switch( reason ) {
                case task_reason::LOW_MORALE:
                    c.text = _( "Your morale is too low to repair…" );
                    break;
                case task_reason::LOW_LIGHT:
                    c.text = dark;
                    break;
                case task_reason::MOVING_VEHICLE:
                    c.text = _( "You can't repair stuff while driving." );
                    break;
                default:
                    break;
            }
            if( !there || !( pt.is_repairable() || pt.is_broken() ) ) {
                // This part needs no repair, whatever else on the vehicle does.
                if( reason == task_reason::CAN_DO || reason == task_reason::INVALID_TARGET ) {
                    c.reason = no_target;
                }
            } else {
                std::string nmsg;
                const bool met = vi.repair_reqs( here, pt, nmsg );
                c.lines = lines_of( nmsg );
                c.ok = met && reason == task_reason::CAN_DO;
                // The time veh_interact::serialize_activity gives the activity.
                const vpart_info &vp = pt.info();
                c.time_s = seconds_of( pt.is_broken() ? vp.install_time( u ) :
                                       vp.repair_time( u ) * pt.get_base().repairable_levels(), u );
            }
            out.push_back( std::move( c ) );
        }

        if( which.find( 'o' ) != std::string::npos ) {
            // Remove: do_remove.
            const task_reason reason = vi.cant_do( here, 'o' );
            command_view c = begin( 'o', reason );
            if( !there || reason == task_reason::INVALID_TARGET ) {
                c.reason = no_target;
                c.text = _( "No parts here." );
            } else {
                vi.msg.reset();
                const bool can = vi.can_remove_part( here, part_index, u );
                const std::string said = vi.msg.value_or( std::string() );
                vi.msg.reset();
                // CDDA either refuses with one sentence, or lists what the removal yields and needs.
                const bool outright = !can && std::count( said.begin(), said.end(), '\n' ) <= 1;
                if( outright ) {
                    c.text = first_line( said );
                } else {
                    c.lines = lines_of( said );
                    c.time_s = seconds_of( pt.info().removal_time( u ), u );
                    switch( reason ) {
                        case task_reason::LOW_MORALE:
                            c.text = _( "Your morale is too low to construct…" );
                            break;
                        case task_reason::LOW_LIGHT:
                            c.text = dark;
                            break;
                        case task_reason::NOT_FREE:
                            c.text = _( "You cannot remove that part while something is attached to it." );
                            break;
                        case task_reason::MOVING_VEHICLE:
                            c.text = _( "Better not remove something while driving." );
                            break;
                        default:
                            break;
                    }
                }
                c.ok = can && reason == task_reason::CAN_DO;
            }
            out.push_back( std::move( c ) );
        }

        if( which.find( 'm' ) != std::string::npos ) {
            // Mend: do_mend. The fix and what it needs are chosen in CDDA's own menu (Character::mend_item).
            const task_reason reason = vi.cant_do( here, 'm' );
            command_view c = begin( 'm', reason );
            switch( reason ) {
                case task_reason::LOW_MORALE:
                    c.text = _( "Your morale is too low to mend…" );
                    break;
                case task_reason::LOW_LIGHT:
                    c.text = dark;
                    break;
                case task_reason::INVALID_TARGET:
                    c.text = _( "No faulty parts require mending." );
                    break;
                case task_reason::MOVING_VEHICLE:
                    c.text = _( "You can't mend stuff while driving." );
                    break;
                default:
                    break;
            }
            const bool faulty = u.has_trait( trait_DEBUG_HS ) ? !pt.faults_potential().empty() :
                                !pt.faults().empty();
            if( reason == task_reason::CAN_DO && !faulty ) {
                c.reason = no_target;
            }
            c.ok = reason == task_reason::CAN_DO && faulty;
            out.push_back( std::move( c ) );
        }

        if( which.find( 'f' ) != std::string::npos ) {
            // Refill: do_refill. What to refill from is asked when the work is started.
            const task_reason reason = vi.cant_do( here, 'f' );
            command_view c = begin( 'f', reason );
            switch( reason ) {
                case task_reason::MOVING_VEHICLE:
                    c.text = _( "You can't refill a moving vehicle." );
                    break;
                case task_reason::INVALID_TARGET:
                    c.text = _( "No parts can currently be refilled." );
                    break;
                default:
                    break;
            }
            const bool takes = pt.can_reload();
            if( reason == task_reason::CAN_DO && !takes ) {
                c.reason = no_target;
            }
            c.ok = reason == task_reason::CAN_DO && takes;
            out.push_back( std::move( c ) );
        }
        for( command_view &c : out ) {
            c.text = line_text( c.text );
        }
    }

    // Starts repairing ('r') or removing ('o') the part, as CDDA's screen does once the player confirms the
    // command. False when it isn't started: `why` is CDDA's sentence, or "" when the player said no to its
    // question.
    static bool start( map &here, vehicle &veh, int part_index, char cmd, std::string &why ) {
        avatar &u = get_avatar();
        // veh_interact::do_main_loop: before any work on someone else's vehicle CDDA asks, and its owners who see
        // it take it as theft. An ownerless vehicle becomes the worker's.
        if( !veh.handle_potential_theft( u ) ) {
            why.clear();
            return false;
        }
        vehicle_part &pt = veh.part( part_index );
        veh_interact vi( here, veh, -pt.mount );
        widen( vi );
        std::vector<command_view> said;
        commands_for( vi, here, veh, part_index, std::string( 1, cmd ), said );
        if( said.size() != 1 || !said.front().ok ) {
            const std::string name = pt.name( false );
            why = said.size() != 1 ? std::string() :
                  !said.front().text.empty() ? said.front().text :
                  said.front().reason == static_cast<int>( task_reason::INVALID_TARGET ) ?
                  string_format( _( "The %s needs no repair." ), name ) :
                  cmd == 'r' ? string_format( _( "You don't meet the requirements to repair the %s." ), name ) :
                  string_format( _( "You don't meet the requirements to remove the %1$s." ), name );
            return false;
        }
        // do_repair and do_remove: work that grounds an aircraft is asked about first.
        if( cmd == 'r' ? veh.would_repair_prevent_flyable( pt, u ) : veh.would_removal_prevent_flyable( pt, u ) ) {
            if( !query_yn( cmd == 'r' ?
                           _( "Repairing this part will mean that this vehicle is no longer flightworthy.  Continue?" ) :
                           _( "Removing this part will mean that this vehicle is no longer flightworthy.  Continue?" ) ) ) {
                why.clear();
                return false;
            }
            veh.set_flyable( false );
            // The question took a while: CDDA looks once more.
            if( vi.cant_do( here, cmd ) != task_reason::CAN_DO ) {
                why.clear();
                return false;
            }
        }
        for( const Character *helper : u.get_crafting_helpers() ) {
            add_msg( m_info, _( "%s helps with this task…" ), helper->get_name() );
        }
        vi.sel_vehicle_part = &pt;
        vi.sel_vpart_info = &pt.info();
        vi.sel_cmd = cmd;
        const player_activity act = vi.serialize_activity( here );
        if( cmd == 'o' ) {
            veh.recalculate_enchantment_cache();
        }
        if( !act ) {
            why.clear();
            return false;
        }
        // game::exam_vehicle.
        u.set_moves( 0 );
        u.assign_activity( act );
        return true;
    }

    // CDDA's sentence when nothing is installed whatever the type: do_install.
    static std::string install_refusal( task_reason reason ) {
        switch( reason ) {
            case task_reason::INVALID_TARGET:
                return _( "Cannot install any part here." );
            case task_reason::LOW_MORALE:
                return _( "Your morale is too low to construct…" );
            case task_reason::LOW_LIGHT:
                return _( "It's too dark to see what you are doing…" );
            case task_reason::MOVING_VEHICLE:
                return _( "You can't install parts while driving." );
            default:
                return std::string();
        }
    }

    // The part type with the id-table index `type`, if CDDA's screen `vi` lists it for its cursor's square; else
    // null (no such type, one CDDA hides from the list, an appliance).
    static const vpart_info *listed( const veh_interact &vi, uint32_t type ) {
        const vpart_id id( id_name( id_kind::vpart, type ) );
        if( !id.is_valid() ) {
            return nullptr;
        }
        const auto at = std::find_if( vi.can_mount.begin(), vi.can_mount.end(), [&id]( const vpart_info * vpi ) {
            return vpi->id == id;
        } );
        return at == vi.can_mount.end() ? nullptr : *at;
    }

    // What CDDA's screen `vi`, its cursor on a square, says about installing `vpi` there: do_install and
    // update_part_requirements. `vpi` null: not a type CDDA lists for the square.
    static command_view install_of( veh_interact &vi, map &here, const vpart_info *vpi ) {
        avatar &u = get_avatar();
        const task_reason reason = vi.cant_do( here, 'i' );
        command_view c;
        c.cmd = 'i';
        c.reason = static_cast<int>( reason );
        c.text = install_refusal( reason );
        if( reason == task_reason::INVALID_TARGET ) {
            return c;
        }
        if( vpi == nullptr ) {
            c.reason = static_cast<int>( task_reason::INVALID_TARGET );
            c.text = _( "That can't be installed here." );
            return c;
        }
        vi.sel_vpart_info = vpi;
        vi.msg.reset();
        const bool met = vi.update_part_requirements( here );
        const std::string said = vi.msg.value_or( std::string() );
        vi.msg.reset();
        // CDDA either refuses with one sentence, or lists what installing it takes.
        if( !met && std::count( said.begin(), said.end(), '\n' ) <= 1 ) {
            c.text = first_line( said );
        } else {
            c.lines = lines_of( said );
            c.time_s = seconds_of( vpi->install_time( u ), u );
        }
        c.ok = met && reason == task_reason::CAN_DO;
        return c;
    }

    // The install list for the square at `mount` of `veh`: every type CDDA's screen lists there, in its order (the
    // ones the avatar has the parts for first), and what installing the type `picked` (0 none) there takes.
    static void fill_install( map &here, vehicle &veh, const point_rel_ms &mount, uint32_t picked,
                              work_view &out ) {
        // CDDA's screen with its cursor on the square, never shown.
        veh_interact vi( here, veh, -mount );
        widen( vi );
        const task_reason reason = vi.cant_do( here, 'i' );
        out.install_reason = static_cast<int>( reason );
        out.install_text = install_refusal( reason );
        for( const vpart_info *vpi : vi.can_mount ) {
            if( out.types.size() >= max_work_types ) {
                out.types_more++;
                continue;
            }
            type_view t;
            t.vpart = intern_id( id_kind::vpart, vpi->id.str() );
            t.bits = ( vi.can_potentially_install( *vpi ) ? type_have : 0 ) |
                     ( veh.can_mount( mount, *vpi ).success() ? type_fits : 0 );
            out.types.push_back( t );
        }
        if( picked != 0 ) {
            out.has_type = true;
            out.type_vpart = picked;
            out.type_command = install_of( vi, here, listed( vi, picked ) );
        }
    }

    // Starts installing the type `type` on the square at `mount`, as CDDA's screen does: its question when the
    // vehicle is someone else's, the same check as the read, its questions when the part would ground an aircraft
    // or keep the vehicle from folding, then its activity. False when it isn't started: `why` as for start().
    static bool start_install( map &here, vehicle &veh, const point_rel_ms &mount, uint32_t type,
                               std::string &why ) {
        avatar &u = get_avatar();
        if( !veh.handle_potential_theft( u ) ) {
            why.clear();
            return false;
        }
        veh_interact vi( here, veh, -mount );
        widen( vi );
        const vpart_info *const vpi = listed( vi, type );
        const command_view said = install_of( vi, here, vpi );
        if( !said.ok || vpi == nullptr ) {
            why = !said.text.empty() || vpi == nullptr ? said.text :
                  string_format( _( "You don't meet the requirements to install the %s." ), vpi->name() );
            return false;
        }
        // do_install: a part that grounds an aircraft or keeps the vehicle from folding is asked about first.
        if( veh.would_install_prevent_flyable( *vpi, u ) ) {
            if( !query_yn( _( "Installing this part will mean that this vehicle is no longer "
                              "flightworthy.  Continue?" ) ) ) {
                why.clear();
                return false;
            }
            veh.set_flyable( false );
        }
        if( veh.is_foldable() && !vpi->folded_volume &&
            !query_yn( _( "Installing this part will make the vehicle unfoldable. "
                          " Continue?" ) ) ) {
            why.clear();
            return false;
        }
        for( const Character *helper : u.get_crafting_helpers() ) {
            add_msg( m_info, _( "%s helps with this task…" ), helper->get_name() );
        }
        vi.sel_vehicle_part = nullptr;
        vi.sel_vpart_info = vpi;
        vi.sel_cmd = 'i';
        const player_activity act = vi.serialize_activity( here );
        if( !act ) {
            why.clear();
            return false;
        }
        // game::exam_vehicle.
        u.set_moves( 0 );
        u.assign_activity( act );
        return true;
    }
};

std::string vehicle_work_query_error_json( uint64_t query_id, const std::string &error, uint64_t vehicle,
        const std::string &text )
{
    std::ostringstream os;
    JsonOut jo( os );
    jo.start_object();
    jo.member( "id", static_cast<int64_t>( query_id ) );
    jo.member( "ok", false );
    jo.member( "kind", std::string( "vehicle_work" ) );
    if( vehicle != 0 ) {
        jo.member( "vehicle", static_cast<int64_t>( vehicle ) );
    }
    jo.member( "error", error );
    if( !text.empty() ) {
        jo.member( "text", client_text( text, max_work_line_columns, 4 * max_work_line_columns ) );
    }
    jo.end_object();
    return os.str();
}

namespace
{
// The read: with a part row the commands for that part; with `install` the install list for that square and what
// installing the type `picked` (0 none) there takes.
std::string read_work( uint64_t query_id, uint64_t vehicle_uid, int part_row,
                       const std::optional<point_rel_ms> &install, uint32_t picked, size_t max_bytes )
{
    // A read leaves the game's dice as they were: CDDA rolls what removing some parts may yield just to list it.
    struct dice_kept {
        cata_default_random_engine was = rng_get_engine();
        ~dice_kept() {
            rng_get_engine() = was;
        }
    } dice;
    map &here = get_map();
    avatar &u = get_avatar();
    if( const std::optional<gate> shut = examine_gate( u ) ) {
        return vehicle_work_query_error_json( query_id, shut->error, vehicle_uid, shut->text );
    }
    vehicle *const veh = vehicle_by_uid( here, vehicle_uid );
    if( veh == nullptr ) {
        return vehicle_work_query_error_json( query_id, "no such vehicle", vehicle_uid );
    }
    if( !workable( *veh ) ) {
        return vehicle_work_query_error_json( query_id, "an appliance", vehicle_uid );
    }
    if( !within_reach( here, u, *veh ) ) {
        return vehicle_work_query_error_json( query_id, "too far", vehicle_uid );
    }
    const std::vector<int> rows = vehicle_part_rows( *veh );
    if( part_row < -1 || ( part_row >= 0 && static_cast<size_t>( part_row ) >= rows.size() ) ) {
        return vehicle_work_query_error_json( query_id, "no such part", vehicle_uid );
    }

    work_view w;
    w.vehicle = vehicle_uid;
    w.name = client_text( veh->name, max_work_name_columns, 4 * max_work_name_columns );
    w.turn = to_turns<int64_t>( calendar::turn - calendar::turn_zero );
    // Looked at only: CDDA's own check (vehicle::handle_potential_theft) also hands an ownerless vehicle to
    // whoever works on it, which a read must not do.
    w.owned = !veh->has_owner() || veh->is_owned_by( u );
    for( size_t row = 0; row < rows.size(); row++ ) {
        if( w.parts.size() >= max_work_parts ) {
            w.parts_more++;
            continue;
        }
        w.parts.push_back( part_of( veh->part( rows[row] ), static_cast<int>( row ) ) );
    }
    if( part_row >= 0 ) {
        w.has_selected = true;
        w.selected_row = part_row;
        vehicle_work_access::fill_selected( here, *veh, rows[part_row], rows, w );
    }
    if( install ) {
        w.has_install = true;
        w.install_x = install->x();
        w.install_y = install->y();
        vehicle_work_access::fill_install( here, *veh, *install, picked, w );
    }

    std::string out = write_work( query_id, w, false );
    // Over the limit: part entries go from the end, half of them a round; then the selected part's lines.
    while( out.size() > max_bytes && !w.parts.empty() ) {
        const size_t drop = std::max<size_t>( 1, w.parts.size() / 2 );
        w.parts.resize( w.parts.size() - drop );
        w.parts_more += drop;
        out = write_work( query_id, w, true );
    }
    if( out.size() > max_bytes ) {
        for( command_view &c : w.commands ) {
            c.lines.clear();
        }
        w.type_command.lines.clear();
        out = write_work( query_id, w, true );
    }
    // Then the part types, from the end.
    while( out.size() > max_bytes && !w.types.empty() ) {
        const size_t drop = std::max<size_t>( 1, w.types.size() / 2 );
        w.types.resize( w.types.size() - drop );
        w.types_more += drop;
        out = write_work( query_id, w, true );
    }
    return out;
}
} // namespace

std::string run_vehicle_work_query( uint64_t query_id, uint64_t vehicle_uid, int part_row, size_t max_bytes )
{
    return read_work( query_id, vehicle_uid, part_row, std::nullopt, 0, max_bytes );
}

std::string run_vehicle_install_query( uint64_t query_id, uint64_t vehicle_uid, int mount_x, int mount_y,
                                       uint32_t vpart, size_t max_bytes )
{
    return read_work( query_id, vehicle_uid, -1, point_rel_ms( mount_x, mount_y ), vpart, max_bytes );
}

bool run_vehicle_work( avatar &u )
{
    const std::optional<vehicle_work_action> a = take_vehicle_work_action();
    if( !a ) {
        return false;
    }
    const auto answer = [&]( bool done, const std::string & why ) {
        if( !done && !why.empty() ) {
            u.add_msg_if_player( m_info, "%s", why );
        }
        if( a->tag != 0 ) {
            on_work_result( a->vehicle, a->tag, done, why );
        }
        return done;
    };
    if( a->cmd != 'r' && a->cmd != 'o' && a->cmd != 'i' ) {
        return answer( false, _( "That can't be done to a vehicle this way." ) );
    }
    if( const std::optional<gate> shut = examine_gate( u ) ) {
        return answer( false, shut->text );
    }
    map &here = get_map();
    vehicle *const veh = vehicle_by_uid( here, a->vehicle );
    if( veh == nullptr ) {
        return answer( false, _( "There is no such vehicle here." ) );
    }
    if( !workable( *veh ) ) {
        return answer( false, _( "That can't be worked on like a vehicle." ) );
    }
    if( !within_reach( here, u, *veh ) ) {
        return answer( false, _( "That vehicle is too far away." ) );
    }
    if( a->cmd == 'i' ) {
        std::string why;
        const bool started = vehicle_work_access::start_install( here, *veh, point_rel_ms( a->mount_x, a->mount_y ),
                             a->vpart, why );
        return answer( started, started ? std::string() : why );
    }
    const std::vector<int> rows = vehicle_part_rows( *veh );
    if( a->part < 0 || static_cast<size_t>( a->part ) >= rows.size() ) {
        return answer( false, _( "That vehicle has no such part." ) );
    }
    // The row must still hold the part the client meant: rows move up when a part comes off.
    const vehicle_part &named = veh->part( rows[a->part] );
    if( named.mount.x() != a->mount_x || named.mount.y() != a->mount_y ||
        intern_id( id_kind::vpart, named.info().id.str() ) != a->vpart ) {
        return answer( false, _( "That part isn't there any more." ) );
    }
    std::string why;
    const bool started = vehicle_work_access::start( here, *veh, rows[a->part], a->cmd, why );
    return answer( started, started ? std::string() : why );
}

} // namespace fp3d
