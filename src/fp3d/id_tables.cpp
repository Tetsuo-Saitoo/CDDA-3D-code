#include "fp3d/id_tables.h"

#include <algorithm>
#include <bitset>
#include <mutex>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>

#include "bodypart.h"
#include "catacharset.h"
#include "color.h"
#include "damage.h"
#include "field_type.h"
#include "fp3d/snapshot.h"
#include "item_factory.h"
#include "itype.h"
#include "json.h"
#include "mapdata.h"
#include "monstergenerator.h"
#include "move_mode.h"
#include "mtype.h"
#include "trap.h"
#include "units.h"
#include "veh_type.h"
#include "weakpoint.h"
#include "weather_type.h"

namespace fp3d
{
namespace
{
std::mutex tables_mutex;
std::shared_ptr<const id_tables> latest;
uint32_t generation = 0;

// Classic symbols are code points; CDDA's line-drawing pseudo codes and invalid values become "".
std::string symbol_text( int sym )
{
    if( sym <= 0 || sym > 0x10FFFF || ( sym >= 0xD800 && sym <= 0xDFFF ) ) {
        return "";
    }
    return utf32_to_utf8( static_cast<uint32_t>( sym ) );
}

// nc_color value -> CDDA colour name. color_manager::get_name() raises a debug message for colours it can't
// invert (a popup outside tests), so the tables use this map and send "" for those.
std::unordered_map<int, std::string> color_names;

void fill_color_names()
{
    color_names.clear();
    const color_manager &cm = get_all_colors();
    for( int i = 0; i < static_cast<int>( num_colors ); i++ ) {
        const color_id id = static_cast<color_id>( i );
        color_names.emplace( cm.get( id ).to_int(), cm.id_to_name( id ) );
    }
}

std::string color_name( const nc_color &c )
{
    const auto it = color_names.find( c.to_int() );
    return it == color_names.end() ? "" : it->second;
}

std::vector<std::string> group_names( const std::bitset<NUM_TERCONN> &bits, const std::vector<std::string> &names )
{
    std::vector<std::string> out;
    for( size_t i = 0; i < names.size() && i < bits.size(); i++ ) {
        if( bits[i] && !names[i].empty() ) {
            out.push_back( names[i] );
        }
    }
    return out;
}

template<typename T>
void fill_common( terrain_def &d, const T &t, const std::vector<std::string> &groups )
{
    d.id = t.id.str();
    d.name = t.name();
    d.symbol = symbol_text( t.symbol() );
    d.color = color_name( t.color() );
    d.looks_like = t.looks_like;
    d.move_cost = t.movecost;
    d.coverage = t.coverage;
    d.light_emitted = t.light_emitted;
    d.transparent = t.transparent;
    d.flags.assign( t.get_flags().begin(), t.get_flags().end() );
    d.connect_groups = group_names( t.connect_groups, groups );
    d.connects_to = group_names( t.connect_to_groups, groups );
    d.rotates_to = group_names( t.rotate_to_groups, groups );
    d.open = t.open.is_null() ? "" : t.open.str();
    d.close = t.close.is_null() ? "" : t.close.str();
}

// Interns `ids` sorted, so the session indices depend only on the loaded data.
void prefill( id_kind kind, std::vector<std::string> ids )
{
    std::sort( ids.begin(), ids.end() );
    for( const std::string &id : ids ) {
        intern_id( kind, id );
    }
}

void write_strings( JsonOut &jo, const char *name, const std::vector<std::string> &v )
{
    jo.member( name );
    jo.start_array();
    for( const std::string &s : v ) {
        jo.write( s );
    }
    jo.end_array();
}

void write_terrain( JsonOut &jo, const char *name, const std::vector<terrain_def> &defs, bool is_ter )
{
    jo.member( name );
    jo.start_array();
    for( const terrain_def &d : defs ) {
        jo.start_object();
        jo.member( "id", d.id );
        jo.member( "name", d.name );
        jo.member( "symbol", d.symbol );
        jo.member( "color", d.color );
        jo.member( "looks_like", d.looks_like );
        jo.member( "move_cost", d.move_cost );
        jo.member( "coverage", d.coverage );
        jo.member( "light_emitted", d.light_emitted );
        jo.member( "transparent", d.transparent );
        write_strings( jo, "flags", d.flags );
        write_strings( jo, "connect_groups", d.connect_groups );
        write_strings( jo, "connects_to", d.connects_to );
        write_strings( jo, "rotates_to", d.rotates_to );
        jo.member( "open", d.open );
        jo.member( "close", d.close );
        if( is_ter ) {
            jo.member( "roof", d.roof );
        }
        jo.end_object();
    }
    jo.end_array();
}
} // namespace

void build_id_tables()
{
    // Old indices die here, and with them anything the snapshot state still holds.
    reset_id_names();
    reset_snapshots();
    fill_color_names();

    std::vector<std::string> ids;
    for( const itype *t : item_controller->all() ) {
        ids.push_back( t->get_id().str() );
    }
    prefill( id_kind::itype, std::move( ids ) );
    ids.clear();
    for( const mtype &m : MonsterGenerator::generator().get_all_mtypes() ) {
        ids.push_back( m.id.str() );
    }
    prefill( id_kind::mtype, std::move( ids ) );
    ids.clear();
    for( const vpart_info &vp : vehicles::parts::get_all() ) {
        ids.push_back( vp.id.str() );
    }
    prefill( id_kind::vpart, std::move( ids ) );
    ids.clear();
    for( const weather_type &w : weather_types::get_all() ) {
        ids.push_back( w.id.str() );
    }
    prefill( id_kind::weather, std::move( ids ) );
    ids.clear();
    for( const body_part_type &bp : body_part_type::get_all() ) {
        ids.push_back( bp.id.str() );
    }
    prefill( id_kind::body_part, std::move( ids ) );
    ids.clear();
    for( const move_mode_id &mm : move_modes_by_speed() ) {
        ids.push_back( mm.str() );
    }
    prefill( id_kind::move_mode, std::move( ids ) );
    ids.clear();
    for( const damage_type &dt : damage_type::get_all() ) {
        ids.push_back( dt.id.str() );
    }
    prefill( id_kind::damage_type, std::move( ids ) );

    auto t = std::make_shared<id_tables>();
    const std::vector<std::string> groups = fp3d_connect_group_names();
    t->ter.resize( ter_t::count() );
    for( size_t i = 0; i < t->ter.size(); i++ ) {
        const ter_t &ter = ter_id( static_cast<int>( i ) ).obj();
        fill_common( t->ter[i], ter, groups );
        t->ter[i].roof = ter.roof.is_null() ? "" : ter.roof.str();
    }
    t->furn.resize( furn_t::count() );
    for( size_t i = 0; i < t->furn.size(); i++ ) {
        fill_common( t->furn[i], furn_id( static_cast<int>( i ) ).obj(), groups );
    }
    t->traps.resize( trap::count() );
    for( size_t i = 0; i < t->traps.size(); i++ ) {
        const trap &tr = trap_id( static_cast<int>( i ) ).obj();
        trap_def &d = t->traps[i];
        d.id = tr.id.str();
        d.name = tr.name();
        d.symbol = symbol_text( tr.sym );
        d.color = color_name( tr.color );
    }
    for( const field_type &ft : field_types::get_all() ) {
        field_def d;
        d.id = ft.id.str();
        d.looks_like = ft.looks_like;
        d.priority = ft.priority;
        for( const field_intensity_level &lv : ft.intensity_levels ) {
            d.level_names.push_back( lv.name.translated() );
            d.level_colors.push_back( color_name( lv.color ) );
        }
        t->fields.push_back( std::move( d ) );
    }

    // Session-indexed definitions follow the tables just filled.
    const std::vector<std::string> item_names = id_names( id_kind::itype );
    t->items.resize( item_names.size() );
    for( size_t i = 1; i < item_names.size(); i++ ) {
        const itype_id iid( item_names[i] );
        if( !iid.is_valid() ) {
            continue;
        }
        const itype &it = *iid;
        item_def &d = t->items[i];
        d.id = item_names[i];
        d.name = it.nname( 1 );
        d.symbol = it.sym;
        d.color = color_name( it.color );
        d.looks_like = it.looks_like.str();
        d.category = it.category_force.str();
        if( it.gun ) {
            d.gun = it.gun->skill_used.str();
            if( !it.gun->ammo.empty() ) {
                d.gun_ammo = it.gun->ammo.begin()->str();
            }
        }
        d.volume_ml = static_cast<int>( units::to_milliliter( it.volume ) );
        d.weight_g = static_cast<int>( units::to_gram( it.weight ) );
    }
    const std::vector<std::string> mon_names = id_names( id_kind::mtype );
    t->monsters.resize( mon_names.size() );
    for( size_t i = 1; i < mon_names.size(); i++ ) {
        const mtype_id mid( mon_names[i] );
        if( !mid.is_valid() ) {
            continue;
        }
        const mtype &m = mid.obj();
        monster_def &d = t->monsters[i];
        d.id = mon_names[i];
        d.name = m.nname();
        d.symbol = m.sym;
        d.color = color_name( m.color );
        d.looks_like = m.looks_like;
        d.size = static_cast<int>( m.size );
        d.hp = m.hp;
        d.speed = m.speed;
        d.volume_ml = static_cast<int>( units::to_milliliter( m.volume ) );
        d.weight_g = static_cast<int>( units::to_gram( m.weight ) );
        for( const species_id &s : m.species ) {
            d.species.push_back( s.str() );
        }
        d.bodytype = m.bodytype;
        for( const weakpoint &w : m.weakpoints.weakpoint_list ) {
            if( d.weakpoints.size() < 32 && !w.id.empty() ) {
                d.weakpoints.push_back( w.id );
            }
        }
    }
    const std::vector<std::string> vp_names = id_names( id_kind::vpart );
    t->vparts.resize( vp_names.size() );
    for( size_t i = 1; i < vp_names.size(); i++ ) {
        const vpart_id vid( vp_names[i] );
        if( !vid.is_valid() ) {
            continue;
        }
        const vpart_info &vp = vid.obj();
        vpart_def &d = t->vparts[i];
        d.id = vp_names[i];
        d.name = vp.name();
        d.color = color_name( vp.color );
        d.looks_like = vp.looks_like;
        d.location = vp.location;
        d.flags.assign( vp.get_flags().begin(), vp.get_flags().end() );
    }

    std::lock_guard<std::mutex> lk( tables_mutex );
    t->generation = ++generation;
    latest = std::move( t );
}

std::shared_ptr<const id_tables> latest_id_tables()
{
    std::lock_guard<std::mutex> lk( tables_mutex );
    return latest;
}

void write_id_tables_json( std::ostream &out, const id_tables &t )
{
    JsonOut jo( out, false );
    jo.start_object();
    jo.member( "generation", static_cast<int64_t>( t.generation ) );
    write_terrain( jo, "ter", t.ter, true );
    write_terrain( jo, "furn", t.furn, false );
    jo.member( "trap" );
    jo.start_array();
    for( const trap_def &d : t.traps ) {
        jo.start_object();
        jo.member( "id", d.id );
        jo.member( "name", d.name );
        jo.member( "symbol", d.symbol );
        jo.member( "color", d.color );
        jo.end_object();
    }
    jo.end_array();
    jo.member( "field" );
    jo.start_array();
    for( const field_def &d : t.fields ) {
        jo.start_object();
        jo.member( "id", d.id );
        jo.member( "looks_like", d.looks_like );
        jo.member( "priority", d.priority );
        write_strings( jo, "level_names", d.level_names );
        write_strings( jo, "level_colors", d.level_colors );
        jo.end_object();
    }
    jo.end_array();
    jo.member( "itype" );
    jo.start_array();
    for( const item_def &d : t.items ) {
        jo.start_object();
        jo.member( "id", d.id );
        jo.member( "name", d.name );
        jo.member( "symbol", d.symbol );
        jo.member( "color", d.color );
        jo.member( "looks_like", d.looks_like );
        jo.member( "category", d.category );
        if( !d.gun.empty() ) {
            jo.member( "gun", d.gun );
            jo.member( "gun_ammo", d.gun_ammo );
        }
        jo.member( "volume_ml", d.volume_ml );
        jo.member( "weight_g", d.weight_g );
        jo.end_object();
    }
    jo.end_array();
    jo.member( "mtype" );
    jo.start_array();
    for( const monster_def &d : t.monsters ) {
        jo.start_object();
        jo.member( "id", d.id );
        jo.member( "name", d.name );
        jo.member( "symbol", d.symbol );
        jo.member( "color", d.color );
        jo.member( "looks_like", d.looks_like );
        jo.member( "size", d.size );
        jo.member( "hp", d.hp );
        jo.member( "speed", d.speed );
        jo.member( "volume_ml", d.volume_ml );
        jo.member( "weight_g", d.weight_g );
        write_strings( jo, "species", d.species );
        jo.member( "bodytype", d.bodytype );
        jo.member( "weakpoints", d.weakpoints );
        jo.end_object();
    }
    jo.end_array();
    jo.member( "vpart" );
    jo.start_array();
    for( const vpart_def &d : t.vparts ) {
        jo.start_object();
        jo.member( "id", d.id );
        jo.member( "name", d.name );
        jo.member( "color", d.color );
        jo.member( "looks_like", d.looks_like );
        jo.member( "location", d.location );
        write_strings( jo, "flags", d.flags );
        jo.end_object();
    }
    jo.end_array();
    // Every session table by name; the ones without definitions are plain name lists.
    jo.member( "tables" );
    jo.start_object();
    const std::pair<const char *, id_kind> session_tables[] = {
        { "itype", id_kind::itype }, { "mtype", id_kind::mtype }, { "vpart", id_kind::vpart },
        { "weather", id_kind::weather }, { "body_part", id_kind::body_part }, { "move_mode", id_kind::move_mode },
        { "damage_type", id_kind::damage_type }, { "activity", id_kind::activity }, { "sound", id_kind::sound },
        { "explosion", id_kind::explosion }
    };
    for( const auto &[name, kind] : session_tables ) {
        write_strings( jo, name, id_names( kind ) );
    }
    jo.end_object();
    jo.end_object();
}

} // namespace fp3d
