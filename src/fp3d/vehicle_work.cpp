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
#include "fp3d/snapshot.h"
#include "item.h"
#include "json.h"
#include "map.h"
#include "point.h"
#include "translations.h"
#include "type_id.h"
#include "veh_interact.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"

static const trait_id trait_DEBUG_HS( "DEBUG_HS" );

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
};

struct work_view {
    uint64_t vehicle = 0;
    std::string name;
    int64_t turn = 0;
    std::vector<part_view> parts;
    size_t parts_more = 0;
    bool has_selected = false;
    int selected_row = -1;
    std::vector<int> here;
    std::vector<command_view> commands;
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
    return v;
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
            jo.start_object();
            jo.member( "cmd", std::string( 1, c.cmd ) );
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
        jo.end_array();
        jo.end_object();
    }
    jo.member( "truncated", truncated );
    jo.end_object();
    return os.str();
}

} // namespace

// The friend veh_interact names: everything that touches CDDA's vehicle screen is in here.
struct vehicle_work_access {
    // The commands for the part `part_index` of `veh`, and the rows of the parts on its square. `rows`: CDDA's
    // part index of each snapshot part row.
    static void fill_selected( map &here, vehicle &veh, int part_index, const std::vector<int> &rows,
                               work_view &out ) {
        avatar &u = get_avatar();
        vehicle_part &pt = veh.part( part_index );
        // CDDA's screen with its cursor on the part's square, never shown: the cursor's offset is minus the mount
        // (veh_interact::move_cursor).
        veh_interact vi( here, veh, -pt.mount );
        // The checks fold their text to the message window's width. A wide one, never drawn, keeps each
        // requirement on its own line.
        vi.w_msg = catacurses::newwin( 4, static_cast<int>( max_work_line_columns ) + 4, point::zero );
        for( const int index : vi.parts_here ) {
            const auto row = std::find( rows.begin(), rows.end(), index );
            if( row != rows.end() && out.here.size() < max_work_here ) {
                out.here.push_back( static_cast<int>( row - rows.begin() ) );
            }
        }
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

        {
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
            out.commands.push_back( std::move( c ) );
        }

        {
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
            out.commands.push_back( std::move( c ) );
        }

        {
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
            out.commands.push_back( std::move( c ) );
        }

        {
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
            out.commands.push_back( std::move( c ) );
        }
        for( command_view &c : out.commands ) {
            c.text = line_text( c.text );
        }
    }
};

std::string vehicle_work_query_error_json( uint64_t query_id, const std::string &error )
{
    std::ostringstream os;
    JsonOut jo( os );
    jo.start_object();
    jo.member( "id", static_cast<int64_t>( query_id ) );
    jo.member( "ok", false );
    jo.member( "kind", std::string( "vehicle_work" ) );
    jo.member( "error", error );
    jo.end_object();
    return os.str();
}

std::string run_vehicle_work_query( uint64_t query_id, uint64_t vehicle_uid, int part_row, size_t max_bytes )
{
    map &here = get_map();
    avatar &u = get_avatar();
    vehicle *const veh = vehicle_by_uid( here, vehicle_uid );
    if( veh == nullptr ) {
        return vehicle_work_query_error_json( query_id, "no such vehicle" );
    }
    // CDDA's rule for examining a vehicle: a part of it on the avatar's square or one next to it.
    bool beside = false;
    for( const vpart_reference &vp : veh->get_all_parts() ) {
        const tripoint_bub_ms p = vp.pos_bub( here );
        if( p.z() == u.posz() && square_dist( p, u.pos_bub() ) <= 1 ) {
            beside = true;
            break;
        }
    }
    if( !beside ) {
        return vehicle_work_query_error_json( query_id, "too far" );
    }
    const std::vector<int> rows = vehicle_part_rows( *veh );
    if( part_row < -1 || ( part_row >= 0 && static_cast<size_t>( part_row ) >= rows.size() ) ) {
        return vehicle_work_query_error_json( query_id, "no such part" );
    }

    work_view w;
    w.vehicle = vehicle_uid;
    w.name = client_text( veh->name, max_work_name_columns, 4 * max_work_name_columns );
    w.turn = to_turns<int64_t>( calendar::turn - calendar::turn_zero );
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
        out = write_work( query_id, w, true );
    }
    return out;
}

} // namespace fp3d
