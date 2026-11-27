#include "fp3d/overmap_read.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "color.h"
#include "coordinates.h"
#include "fp3d/events.h"
#include "fp3d/snapshot.h"
#include "json.h"
#include "omdata.h"
#include "overmap.h"
#include "overmap_ui.h"
#include "overmapbuffer.h"
#include "translations.h"
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

// What CDDA's map screen draws on the cell `p`, whose terrain the player knows this well.
look_view look_of( const tripoint_abs_omt &p, const oter_id &ter, om_vision_level vision )
{
    look_view l;
    if( ter->blends_adjacent( vision ) ) {
        // Some terrains blend into their surroundings until they are seen better (a campsite in a forest, from
        // afar): CDDA draws the commonest of the cell's neighbours instead, or its "unexplored" look when it has
        // seen none of them. This is its own function for that. It asks for a neighbour's terrain only where the
        // player has seen that neighbour, so the neighbour's overmap exists and none is created.
        const oter_vision::blended_omt as = oter_vision::get_blended_omt_info( p, vision );
        l.name = client_text( as.name, max_overmap_name_columns, 4 * max_overmap_name_columns );
        l.sym = client_text( as.sym, 2, 8 );
        l.color = get_all_colors().get_name( as.color );
        return l;
    }
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
    n.radius = n.danger ? std::clamp( overmap_buffer.note_danger_radius( p ), 0, max_overmap_danger_radius ) : 0;
    return n;
}

} // namespace

bool overmap_cell_ok( int x, int y, int z )
{
    return x >= -max_overmap_xy && x <= max_overmap_xy && y >= -max_overmap_xy && y <= max_overmap_xy &&
           z >= -max_overmap_z && z <= max_overmap_z;
}

bool overmap_origin_ok( int x, int y, int z )
{
    return overmap_cell_ok( x, y, z ) && x % overmap_page == 0 && y % overmap_page == 0;
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
                const look_view l = look_of( p, overmap_buffer.ter_existing( p ), vision );
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
            // CDDA's map shows the explored mark and a tracked vehicle only on cells the player has seen, and a
            // camp as its terrain; the mission's target it shows wherever it is.
            const bool seen = vision != om_vision_level::unseen;
            page.flags.push_back( ( seen && overmap_buffer.is_explored( p ) ? overmap_explored : 0 ) |
                                  ( seen && overmap_buffer.has_vehicle( p ) ? overmap_vehicle : 0 ) |
                                  ( seen && overmap_buffer.has_camp( p ) ? overmap_camp : 0 ) |
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
    // Then looks from the end, half of them a round: their cells are drawn as look 0.
    while( out.size() > max_bytes && page.looks.size() > 1 ) {
        const size_t keep = std::max<size_t>( 1, page.looks.size() / 2 );
        page.looks.resize( keep );
        for( int &look : page.look ) {
            look = static_cast<size_t>( look ) < keep ? look : 0;
        }
        page.truncated = true;
        out = write_page( query_id, page );
    }
    return out;
}

bool map_note_text_ok( const std::string &text )
{
    if( text.size() > max_map_note_input_bytes ) {
        return false;
    }
    for( size_t i = 0; i < text.size(); ) {
        const unsigned char lead = static_cast<unsigned char>( text[i] );
        const size_t len = lead < 0x80 ? 1 : lead >= 0xC2 && lead <= 0xDF ? 2 : lead >= 0xE0 && lead <= 0xEF ? 3 :
                           lead >= 0xF0 && lead <= 0xF4 ? 4 : 0;
        if( len == 0 || len > text.size() - i ) {
            return false;
        }
        uint32_t cp = len == 1 ? lead : len == 2 ? lead & 0x1Fu : len == 3 ? lead & 0x0Fu : lead & 0x07u;
        for( size_t k = 1; k < len; k++ ) {
            const unsigned char next = static_cast<unsigned char>( text[i + k] );
            if( ( next & 0xC0 ) != 0x80 ) {
                return false;
            }
            cp = ( cp << 6 ) | ( next & 0x3Fu );
        }
        // Overlong forms, surrogates and code points beyond Unicode are not UTF-8.
        if( ( len == 3 && ( cp < 0x800 || ( cp >= 0xD800 && cp <= 0xDFFF ) ) ) ||
            ( len == 4 && ( cp < 0x10000 || cp > 0x10FFFF ) ) ) {
            return false;
        }
        i += len;
    }
    return true;
}

bool map_command_ok( const map_command &c )
{
    if( ( c.cmd != 'n' && c.cmd != 'u' ) || !overmap_cell_ok( c.omt.x, c.omt.y, c.omt.z ) || c.tag > max_item_tag ||
        !map_note_text_ok( c.text ) || ( !c.has_text && !c.text.empty() ) ) {
        return false;
    }
    if( c.cmd == 'u' ) {
        return !c.has_text && c.danger == -1 && c.radius == 0;
    }
    return c.danger >= -1 && c.danger <= 1 && c.radius >= 0 && c.radius <= max_map_danger_radius &&
           ( c.danger == 1 || c.radius == 0 );
}

std::string map_note_text( const std::string &raw )
{
    // Control characters first, so that none can hide a colour tag from the step after.
    std::string plain;
    const size_t end = std::min( raw.size(), max_map_note_input_bytes );
    plain.reserve( end );
    for( size_t i = 0; i < end; i++ ) {
        const unsigned char b = static_cast<unsigned char>( raw[i] );
        const unsigned char next = i + 1 < end ? static_cast<unsigned char>( raw[i + 1] ) : 0;
        if( b == 0xC2 && next >= 0x80 && next <= 0x9F ) {   // C1, U+0080 .. U+009F
            i++;
        } else if( b >= 0x20 && b != 0x7F ) {
            plain.push_back( raw[i] );
        }
    }
    // Taking a tag out may join two pieces into another one: again until none is left. A round that changes
    // anything makes the text shorter, so this ends.
    std::string untagged = strip_color_tags( plain );
    while( untagged != plain ) {
        plain = std::move( untagged );
        untagged = strip_color_tags( plain );
    }
    return client_text( plain, max_overmap_note_columns, 4 * max_overmap_note_columns );
}

namespace
{

// The note command `c` done as CDDA's map screen does it: "" when it went through, else the sentence why not.
std::string change_note( const map_command &c )
{
    const tripoint_abs_omt p( c.omt.x, c.omt.y, c.omt.z );
    // CDDA's note calls go through get_om_global, which makes an overmap where there is none: never call them
    // there.
    if( !overmap_buffer.has( project_to<coords::om>( p.xy() ) ) ) {
        return _( "Nothing is known of that place yet." );
    }
    const bool had = overmap_buffer.has_note( p );
    if( c.cmd == 'u' ) {
        if( !had ) {
            return _( "There is no note there." );
        }
        overmap_buffer.delete_note( p );
        return std::string();
    }
    const std::string text = c.has_text ? map_note_text( c.text ) : std::string();
    if( c.has_text ? text.empty() : !had ) {
        return _( "The note has no text." );
    }
    if( c.has_text ) {
        overmap_buffer.add_note( p, text );
    }
    if( c.danger >= 0 ) {
        overmap_buffer.mark_note_dangerous( p, c.danger == 1 ? c.radius : 0, c.danger == 1 );
    }
    return std::string();
}

} // namespace

void run_map_command()
{
    const std::optional<map_command> c = take_map_command();
    if( !c ) {
        return;
    }
    const std::string why = map_command_ok( *c ) ? change_note( *c ) : std::string( _( "That can't be done." ) );
    if( c->tag != 0 && events_enabled() ) {
        on_map_result( c->omt, c->tag, why.empty(), why );
    }
}

} // namespace fp3d
