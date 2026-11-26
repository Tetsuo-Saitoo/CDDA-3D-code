#include "fp3d/overmap_read.h"

#include <algorithm>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "color.h"
#include "coordinates.h"
#include "fp3d/snapshot.h"
#include "json.h"
#include "omdata.h"
#include "overmap.h"
#include "overmap_ui.h"
#include "overmapbuffer.h"
#include "type_id.h"

namespace fp3d
{
namespace
{

struct look_view {
    std::string name;
    std::string sym;
    std::string color;
};

struct note_view {
    int x = 0;
    int y = 0;
    std::string sym;
    std::string color;
    std::string text;
    bool danger = false;
    int radius = 0;
};

struct page_view {
    int x = 0;
    int y = 0;
    int z = 0;
    int64_t turn = 0;
    std::vector<look_view> looks;
    std::vector<int> look;
    std::vector<int> vision;
    std::vector<int> flags;
    std::vector<note_view> notes;
    size_t notes_more = 0;
    bool has_path = false;
    tripoint_abs_omt path_to;
    std::vector<tripoint_abs_omt> path;      // from the avatar's end
    size_t path_more = 0;
    bool truncated = false;
};

void write_ints( JsonOut &jo, const char *name, const std::vector<int> &values )
{
    jo.member( name );
    jo.start_array();
    for( const int v : values ) {
        jo.write( v );
    }
    jo.end_array();
}

std::string write_page( uint64_t query_id, const page_view &p )
{
    std::ostringstream os;
    JsonOut jo( os );
    jo.start_object();
    jo.member( "id", static_cast<int64_t>( query_id ) );
    jo.member( "ok", true );
    jo.member( "kind", std::string( "overmap" ) );
    jo.member( "origin" );
    jo.start_array();
    jo.write( p.x );
    jo.write( p.y );
    jo.write( p.z );
    jo.end_array();
    jo.member( "turn", p.turn );
    jo.member( "looks" );
    jo.start_array();
    for( const look_view &l : p.looks ) {
        jo.start_object();
        jo.member( "name", l.name );
        jo.member( "sym", l.sym );
        jo.member( "color", l.color );
        jo.end_object();
    }
    jo.end_array();
    write_ints( jo, "look", p.look );
    write_ints( jo, "vision", p.vision );
    write_ints( jo, "flags", p.flags );
    jo.member( "notes" );
    jo.start_array();
    for( const note_view &n : p.notes ) {
        jo.start_object();
        jo.member( "at" );
        jo.start_array();
        jo.write( n.x );
        jo.write( n.y );
        jo.end_array();
        jo.member( "sym", n.sym );
        jo.member( "color", n.color );
        jo.member( "text", n.text );
        jo.member( "danger", n.danger );
        jo.member( "radius", n.radius );
        jo.end_object();
    }
    jo.end_array();
    jo.member( "notes_more", static_cast<int64_t>( p.notes_more ) );
    if( p.has_path ) {
        jo.member( "path" );
        jo.start_object();
        jo.member( "to" );
        jo.start_array();
        jo.write( p.path_to.x() );
        jo.write( p.path_to.y() );
        jo.write( p.path_to.z() );
        jo.end_array();
        jo.member( "cells" );
        jo.start_array();
        for( const tripoint_abs_omt &c : p.path ) {
            jo.write( c.x() );
            jo.write( c.y() );
            jo.write( c.z() );
        }
        jo.end_array();
        jo.member( "more", static_cast<int64_t>( p.path_more ) );
        jo.end_object();
    }
    jo.member( "truncated", p.truncated );
    jo.end_object();
    return os.str();
}

// What CDDA's map screen draws for a terrain the player knows this well.
look_view look_of( const oter_id &ter, om_vision_level vision )
{
    look_view l;
    l.name = client_text( ter->get_name( vision ), max_overmap_name_columns, 4 * max_overmap_name_columns );
    l.sym = client_text( ter->get_symbol( vision ), 2, 8 );
    l.color = get_all_colors().get_name( ter->get_color( vision ) );
    return l;
}

// The note on a cell, as CDDA's map screen shows it: the symbol and colour its text asks for, then the text.
note_view note_of( const tripoint_abs_omt &p )
{
    note_view n;
    n.x = p.x();
    n.y = p.y();
    const std::string &raw = overmap_buffer.note( p );
    const std::tuple<char, nc_color, size_t> shown = overmap_ui::get_note_display_info( raw );
    const char sym = std::get<0>( shown );
    n.sym = sym >= 0x20 && sym < 0x7f ? std::string( 1, sym ) : std::string( "N" );
    n.color = get_all_colors().get_name( std::get<1>( shown ) );
    const size_t from = std::min( std::get<2>( shown ), raw.size() );
    n.text = client_text( raw.substr( from ), max_overmap_note_columns, 4 * max_overmap_note_columns );
    n.danger = overmap_buffer.is_marked_dangerous( p );
    n.radius = n.danger ? std::max( 0, overmap_buffer.note_danger_radius( p ) ) : 0;
    return n;
}

} // namespace

bool overmap_origin_ok( int x, int y, int z )
{
    return x >= -max_overmap_xy && x <= max_overmap_xy && y >= -max_overmap_xy && y <= max_overmap_xy &&
           z >= -max_overmap_z && z <= max_overmap_z && x % overmap_page == 0 && y % overmap_page == 0;
}

std::string overmap_query_error_json( uint64_t query_id, const std::string &error )
{
    std::ostringstream os;
    JsonOut jo( os );
    jo.start_object();
    jo.member( "id", static_cast<int64_t>( query_id ) );
    jo.member( "ok", false );
    jo.member( "kind", std::string( "overmap" ) );
    jo.member( "error", error );
    jo.end_object();
    return os.str();
}

std::string run_overmap_query( uint64_t query_id, int x, int y, int z, bool with_path, size_t max_bytes )
{
    if( !overmap_origin_ok( x, y, z ) ) {
        return overmap_query_error_json( query_id, "internal" );
    }
    const avatar &u = get_avatar();
    page_view page;
    page.x = x;
    page.y = y;
    page.z = z;
    page.turn = to_turns<int64_t>( calendar::turn - calendar::turn_zero );
    const size_t cells = static_cast<size_t>( overmap_page ) * overmap_page;
    page.look.reserve( cells );
    page.vision.reserve( cells );
    page.flags.reserve( cells );
    // Look 0 is the cell the player hasn't seen. Looks are told apart by what is drawn, never by terrain.
    page.looks.emplace_back();
    std::unordered_map<std::string, int> known;
    const tripoint_abs_omt mission = u.get_active_mission_target();

    for( int dy = 0; dy < overmap_page; dy++ ) {
        for( int dx = 0; dx < overmap_page; dx++ ) {
            const tripoint_abs_omt p( x + dx, y + dy, z );
            // Only calls that look at overmaps which exist: overmapbuffer::ter() would create one.
            const om_vision_level vision = overmap_buffer.seen( p );
            int look = 0;
            if( vision != om_vision_level::unseen ) {
                const look_view l = look_of( overmap_buffer.ter_existing( p ), vision );
                const std::string key = l.name + '\x1f' + l.sym + '\x1f' + l.color;
                const auto had = known.find( key );
                if( had != known.end() ) {
                    look = had->second;
                } else if( page.looks.size() < max_overmap_looks ) {
                    look = static_cast<int>( page.looks.size() );
                    known.emplace( key, look );
                    page.looks.push_back( l );
                } else {
                    page.truncated = true;
                }
            }
            page.look.push_back( look );
            page.vision.push_back( static_cast<int>( vision ) );
            page.flags.push_back( ( overmap_buffer.is_explored( p ) ? overmap_explored : 0 ) |
                                  ( overmap_buffer.has_vehicle( p ) ? overmap_vehicle : 0 ) |
                                  ( overmap_buffer.has_camp( p ) ? overmap_camp : 0 ) |
                                  ( p == mission ? overmap_mission : 0 ) );
            if( overmap_buffer.has_note( p ) ) {
                if( page.notes.size() < max_overmap_notes ) {
                    page.notes.push_back( note_of( p ) );
                } else {
                    page.notes_more++;
                    page.truncated = true;
                }
            }
        }
    }

    // The avatar's travel path. CDDA keeps it from the destination back to the next step.
    if( with_path && !u.omt_path.empty() ) {
        page.has_path = true;
        page.path_to = u.omt_path.front();
        const size_t steps = std::min( u.omt_path.size(), max_overmap_path );
        page.path.assign( u.omt_path.rbegin(), u.omt_path.rbegin() + steps );
        page.path_more = u.omt_path.size() - steps;
        page.truncated = page.truncated || page.path_more > 0;
    }

    std::string out = write_page( query_id, page );
    // Over the limit: the path's far cells go, half of them a round; then notes from the end.
    while( out.size() > max_bytes && !page.path.empty() ) {
        const size_t drop = std::max<size_t>( 1, page.path.size() / 2 );
        page.path.resize( page.path.size() - drop );
        page.path_more += drop;
        page.truncated = true;
        out = write_page( query_id, page );
    }
    while( out.size() > max_bytes && !page.notes.empty() ) {
        const size_t drop = std::max<size_t>( 1, page.notes.size() / 2 );
        page.notes.resize( page.notes.size() - drop );
        page.notes_more += drop;
        page.truncated = true;
        out = write_page( query_id, page );
    }
    return out;
}

} // namespace fp3d
