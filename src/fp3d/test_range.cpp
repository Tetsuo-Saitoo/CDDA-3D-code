// K7 test range (PLAN.md K7). See test_range.h.
#include "fp3d/test_range.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "auto_note.h"
#include "auto_pickup.h"
#include "avatar.h"
#include "bodypart.h"
#include "calendar.h"
#include "character.h"
#include "coordinates.h"
#include "creature_tracker.h"
#include "effect.h"
#include "effect_on_condition.h"
#include "enums.h"
#include "event.h"
#include "event_bus.h"
#include "field_type.h"
#include "flag.h"
#include "fp3d/chargen.h"
#include "fp3d/game_access.h"
#include "fp3d/snapshot.h"
#include "game.h"
#include "game_constants.h"
#include "gamemode.h"
#include "get_version.h"
#include "item.h"
#include "item_category.h"
#include "item_factory.h"
#include "itype.h"
#include "map.h"
#include "map_scale_constants.h"
#include "mapdata.h"
#include "math_parser_diag_value.h"
#include "monster.h"
#include "monstergenerator.h"
#include "mtype.h"
#include "npc.h"
#include "options.h"
#include "overmapbuffer.h"
#include "player_activity.h"
#include "pocket_type.h"
#include "profession.h"
#include "rng.h"
#include "safemode_ui.h"
#include "scenario.h"
#include "string_formatter.h"
#include "trap.h"
#include "type_id.h"
#include "units.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "weather.h"
#include "worldfactory.h"

namespace
{
const std::string range_var = "fp3d_range";
const std::string built_prefix = "fp3d_range_built_";
const activity_id act_wait( "ACT_WAIT" );
const flag_id range_pseudo( "PSEUDO" );
// Outdoor concrete: an INDOORS floor (t_thconc_floor) keeps out the daylight, and nobody could see.
const ter_str_id ter_floor( "t_concrete" );
const ter_str_id ter_wall( "t_concrete_wall" );
const ter_str_id ter_ground( "t_rock" );
const ter_str_id ter_cellar_floor( "t_rock_floor" );
const ter_str_id ter_open_air( "t_open_air" );
const ter_str_id ter_glass_wall( "t_wall_glass" );
const ter_str_id ter_stairs_up( "t_stairs_up" );
const ter_str_id ter_stairs_down( "t_stairs_down" );
const field_type_str_id field_fire( "fd_fire" );
const itype_id itype_gasoline( "gasoline" );

// Overmap square of zone 0; zone i is zone_spacing_omt * i further east.
const tripoint_abs_omt range_origin( 20, 90, 0 );
// A coordinate the client may send: far from int overflow in CDDA's point maths.
constexpr int max_coord = 1 << 28;

// Gallery slots (offsets from the centre square): pitch 2 with a free 9x9 plaza around the centre for the
// arrival square and the stairs; emitters (they spread fields) pitch 4 in two rows at the south edge.
constexpr int gallery_min = -56;
constexpr int gallery_max_x = 54;
constexpr int gallery_max_y = 48;
constexpr int plaza = 4;

const std::vector<fp3d::range_offset> &gallery_slots()
{
    static const std::vector<fp3d::range_offset> slots = [] {
        std::vector<fp3d::range_offset> v;
        for( int y = gallery_min; y <= gallery_max_y; y += 2 ) {
            for( int x = gallery_min; x <= gallery_max_x; x += 2 ) {
                if( std::abs( x ) <= plaza && std::abs( y ) <= plaza ) {
                    continue;
                }
                v.push_back( { x, y, 0 } );
            }
        }
        return v;
    }();
    return slots;
}

const std::vector<fp3d::range_offset> &gallery_emitter_slots()
{
    static const std::vector<fp3d::range_offset> slots = [] {
        std::vector<fp3d::range_offset> v;
        for( int y : { 52, 56 } ) {
            for( int x = gallery_min; x <= gallery_max_x; x += 4 ) {
                v.push_back( { x, y, 0 } );
            }
        }
        return v;
    }();
    return slots;
}

struct piece_id {
    std::string id;
    bool furniture = false;
    bool operator<( const piece_id &o ) const {
        return id != o.id ? id < o.id : furniture < o.furniture;
    }
};

// The zone registry (game thread only).
struct registry {
    bool ready = false;
    std::vector<fp3d::range_zone> zones;
    fp3d::range_counts counts;
    std::vector<piece_id> gallery_normal;     // sorted
    std::vector<piece_id> gallery_emitters;   // sorted
    uint64_t version = 1;                     // bumps on any change the range info shows
};

registry &reg()
{
    static registry r;
    return r;
}

std::atomic<bool> frozen{ false };

std::mutex info_mutex;
fp3d::range_info info_latest;
uint64_t info_seq = 0;

// Lower-case letters, digits and '_' only, capped: zone ids end up in avatar value names and the client.
std::string id_part( const std::string &s )
{
    std::string out;
    for( const char c : s ) {
        if( out.size() >= 48 ) {
            break;
        }
        const bool ok = ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '_';
        out.push_back( ok ? c : ( c >= 'A' && c <= 'Z' ) ? static_cast<char>( c - 'A' + 'a' ) : '_' );
    }
    return out.empty() ? "x" : out;
}

size_t parts_for( size_t n, size_t per )
{
    return n == 0 ? 0 : ( n + per - 1 ) / per;
}

void add_parts( std::vector<fp3d::range_zone> &zones, fp3d::zone_kind kind, const std::string &id,
                const std::string &name, size_t n, size_t per, const std::string &category = "" )
{
    const size_t parts = parts_for( n, per );
    for( size_t p = 0; p < parts; p++ ) {
        fp3d::range_zone z;
        z.kind = kind;
        z.id = string_format( "%s_%02d", id, static_cast<int>( p + 1 ) );
        z.name = parts > 1 ? string_format( "%s %d/%d", name, static_cast<int>( p + 1 ), static_cast<int>( parts ) ) :
                 name;
        z.part = static_cast<int>( p );
        z.parts = static_cast<int>( parts );
        z.category = category;
        z.first = p * per;
        z.count = std::min( per, n - z.first );
        zones.push_back( std::move( z ) );
    }
}

bool avatar_flag( const std::string &name )
{
    const diag_value *v = get_avatar().maybe_get_value( name );
    return v != nullptr && v->is_dbl() && v->dbl() != 0.0;
}

// Builds the registry from the loaded data (mods included); ids sorted, so the layout is stable.
void build_registry()
{
    registry &r = reg();
    r = registry{ false, {}, {}, {}, {}, r.version + 1 };
    fp3d::range_counts &c = r.counts;

    std::map<std::string, size_t> per_category;
    for( const itype *t : item_controller->all() ) {
        if( t == nullptr || t->get_id().is_null() || t->has_flag( range_pseudo ) ) {
            continue;
        }
        if( t->gun && !t->gunmod ) {
            c.guns++;
            continue;
        }
        c.items++;
        per_category[t->category_force.str()]++;
    }
    c.items_per_category.assign( per_category.begin(), per_category.end() );
    for( const vehicle_prototype &p : vehicles::get_all_prototypes() ) {
        c.vehicles += p.id.str() == "none" ? 0 : 1;
    }
    for( const mtype &m : MonsterGenerator::generator().get_all_mtypes() ) {
        c.monsters += m.id.is_null() ? 0 : 1;
    }
    for( int i = 1; i < static_cast<int>( ter_t::count() ); i++ ) {
        const ter_t &t = ter_id( i ).obj();
        ( t.emissions.empty() ? r.gallery_normal : r.gallery_emitters ).push_back( { t.id.str(), false } );
        c.terrain++;
    }
    for( int i = 1; i < static_cast<int>( furn_t::count() ); i++ ) {
        const furn_t &f = furn_id( i ).obj();
        if( f.id.str() == "f_clear" ) {   // a mapgen marker: setting it clears the furniture
            continue;
        }
        ( f.emissions.empty() ? r.gallery_normal : r.gallery_emitters ).push_back( { f.id.str(), true } );
        c.furniture++;
    }
    c.emitters = r.gallery_emitters.size();
    std::sort( r.gallery_normal.begin(), r.gallery_normal.end() );
    std::sort( r.gallery_emitters.begin(), r.gallery_emitters.end() );

    std::vector<fp3d::range_zone> zones;
    const size_t gallery_parts = std::max<size_t>( { size_t{ 1 },
                                 parts_for( r.gallery_normal.size(), gallery_slots().size() ),
                                 parts_for( r.gallery_emitters.size(), gallery_emitter_slots().size() )
                                                   } );
    for( size_t p = 0; p < gallery_parts; p++ ) {
        fp3d::range_zone z;
        z.kind = fp3d::zone_kind::gallery;
        z.id = string_format( "gallery_%02d", static_cast<int>( p + 1 ) );
        z.name = gallery_parts > 1 ? string_format( "Gallery %d/%d", static_cast<int>( p + 1 ),
                 static_cast<int>( gallery_parts ) ) : "Gallery";
        z.part = static_cast<int>( p );
        z.parts = static_cast<int>( gallery_parts );
        z.first = std::min( r.gallery_normal.size(), p * gallery_slots().size() );
        z.count = std::min( gallery_slots().size(), r.gallery_normal.size() - z.first );
        zones.push_back( std::move( z ) );
    }
    add_parts( zones, fp3d::zone_kind::armory, "armory", "Armory", c.guns, fp3d::armory_guns_per_zone );
    for( const auto &[cat, n] : c.items_per_category ) {
        const item_category_id cid( cat );
        const std::string label = cid.is_valid() ? cid->name_header() : cat;
        add_parts( zones, fp3d::zone_kind::halls, "halls_" + id_part( cat ), "Halls: " + label, n,
                   fp3d::hall_items_per_zone, cat );
    }
    add_parts( zones, fp3d::zone_kind::lots, "lots", "Vehicle lot", c.vehicles, fp3d::lot_vehicles_per_zone );
    add_parts( zones, fp3d::zone_kind::pens, "pens", "Monster pens", c.monsters, fp3d::pen_monsters_per_zone );
    fp3d::range_zone lanes;
    lanes.kind = fp3d::zone_kind::lanes;
    lanes.id = "lanes";
    lanes.name = "Target lanes";
    zones.push_back( lanes );
    fp3d::range_zone kitchen;
    kitchen.kind = fp3d::zone_kind::kitchen;
    kitchen.id = "kitchen";
    kitchen.name = "Kitchen";
    zones.push_back( kitchen );
    std::sort( zones.begin(), zones.end(), []( const fp3d::range_zone & a, const fp3d::range_zone & b ) {
        return a.id < b.id;
    } );
    fp3d::range_zone hub;
    hub.kind = fp3d::zone_kind::hub;
    hub.id = "hub";
    hub.name = "Hub";
    zones.insert( zones.begin(), hub );
    for( size_t i = 0; i < zones.size(); i++ ) {
        zones[i].centre = range_origin + tripoint_rel_omt( fp3d::zone_spacing_omt * static_cast<int>( i ), 0, 0 );
        zones[i].built = avatar_flag( built_prefix + zones[i].id );
    }
    r.zones = std::move( zones );
    r.ready = true;
}

registry &ready_registry()
{
    registry &r = reg();
    if( !r.ready && g != nullptr && fp3d::range_active() ) {
        build_registry();
    }
    return r;
}

tripoint_abs_ms at( const tripoint_abs_ms &centre, const fp3d::range_offset &o )
{
    return centre + tripoint_rel_ms( o.x, o.y, o.z );
}

// Clears the zone's area: creatures in the bubble gone, then per square from the top down: open air above z0, the
// floor at z0, solid ground at z -1; no furniture, items, vehicles, traps, fields, graffiti or radiation.
void clear_area( const tripoint_abs_ms &centre )
{
    map &here = get_map();
    g->reload_npcs();
    for( npc &guy : g->all_npcs() ) {
        guy.die( &here, nullptr );
    }
    g->cleanup_dead();
    g->clear_zombies();
    // Mapgen's monster spawn points (the whole bubble) would spawn at the next update_map.
    here.clear_spawns();

    const int half = fp3d::zone_half_squares;
    for( int z = OVERMAP_HEIGHT; z >= -1; z-- ) {
        const ter_id fill = z > 0 ? ter_open_air.id() : z == 0 ? ter_floor.id() : ter_ground.id();
        for( int y = -half; y < half; y++ ) {
            for( int x = -half; x < half; x++ ) {
                const tripoint_bub_ms p = here.get_bub( at( centre, { x, y, z } ) );
                if( !here.inbounds( p ) ) {
                    continue;
                }
                // Up high most squares are open air already: only touch what isn't.
                if( z > 1 && here.ter( p ) == fill && here.furn( p ) == furn_str_id::NULL_ID() &&
                    !here.has_items( p ) && here.tr_at( p ).is_null() && !here.has_field_at( p ) ) {
                    continue;
                }
                if( const optional_vpart_position vp = here.veh_at( p ) ) {
                    here.destroy_vehicle( &vp->vehicle() );
                }
                here.furn_set( p, furn_str_id::NULL_ID() );
                here.i_clear( p );
                if( here.has_field_at( p ) ) {
                    here.clear_fields( p );
                }
                if( here.has_graffiti_at( p ) ) {
                    here.delete_graffiti( p );
                }
                here.ter_set( p, fill );
                if( !here.tr_at( p ).is_null() ) {
                    here.remove_trap( p );
                }
                here.set_radiation( p, 0 );
            }
        }
    }
}

void set_ter( const tripoint_abs_ms &p, const ter_str_id &t )
{
    map &here = get_map();
    const tripoint_bub_ms b = here.get_bub( p );
    if( here.inbounds( b ) ) {
        here.ter_set( b, t.id() );
    }
}

void build_hub( const tripoint_abs_ms &s )
{
    map &here = get_map();
    // Walls round a 29x29 room, the door in the east wall (props). Nothing stands between the start square and the
    // props: the player sees them all, and the zombie sees the player.
    for( int y = -15; y <= 15; y++ ) {
        for( int x = -15; x <= 15; x++ ) {
            if( std::abs( x ) == 15 || std::abs( y ) == 15 ) {
                set_ter( at( s, { x, y, 0 } ), ter_wall );
            }
        }
    }
    // The zombie's glass cell: interior x 7..9, y -6.
    for( int y = -7; y <= -5; y++ ) {
        for( int x = 6; x <= 10; x++ ) {
            if( !( y == -6 && x >= 7 && x <= 9 ) ) {
                set_ter( at( s, { x, y, 0 } ), ter_glass_wall );
            }
        }
    }
    // A landing above the stairs up, a cellar room below the stairs down.
    for( int y = 7; y <= 9; y++ ) {
        for( int x = -11; x <= -9; x++ ) {
            set_ter( at( s, { x, y, 1 } ), ter_floor );
        }
    }
    for( int y = 10; y <= 12; y++ ) {
        for( int x = -11; x <= -9; x++ ) {
            set_ter( at( s, { x, y, -1 } ), ter_cellar_floor );
        }
    }
    for( const fp3d::hub_prop &prop : fp3d::hub_props() ) {
        const tripoint_abs_ms abs = at( s, prop.at );
        const tripoint_bub_ms p = here.get_bub( abs );
        if( !here.inbounds( p ) ) {
            continue;
        }
        switch( prop.kind ) {
            case fp3d::prop_kind::terrain:
                here.ter_set( p, ter_str_id( prop.id ).id() );
                break;
            case fp3d::prop_kind::furniture:
                here.furn_set( p, furn_str_id( prop.id ).id() );
                break;
            case fp3d::prop_kind::field:
                here.add_field( p, field_type_str_id( prop.id ).id(), prop.count, 0_turns, false );
                break;
            case fp3d::prop_kind::item: {
                const itype_id type( prop.id );
                if( !type.is_valid() ) {
                    break;
                }
                item it( type, calendar::turn );
                const itype_id inside( *prop.contents != '\0' ? prop.contents : "null" );
                if( !inside.is_null() && inside.is_valid() ) {
                    if( it.is_gun() ) {
                        it.ammo_set( inside, prop.contents_count );
                    } else {
                        it.put_in( item( inside, calendar::turn, prop.contents_count ), pocket_type::CONTAINER );
                    }
                }
                if( it.count_by_charges() ) {
                    it.charges = prop.count;
                    here.add_item( p, it );
                } else {
                    for( int i = 0; i < prop.count; i++ ) {
                        here.add_item( p, it );
                    }
                }
                break;
            }
            case fp3d::prop_kind::monster: {
                const mtype_id type( prop.id );
                if( type.is_valid() ) {
                    g->place_critter_at( type, p );
                }
                break;
            }
            case fp3d::prop_kind::vehicle: {
                const vproto_id type( prop.id );
                if( type.is_valid() ) {
                    // Intact, no faults or security; `count` = fuel percent.
                    here.add_vehicle( type, p, 0_degrees, prop.count, 2, false, true );
                }
                break;
            }
        }
    }
}

void build_gallery( size_t index, const tripoint_abs_ms &s )
{
    map &here = get_map();
    for( const fp3d::gallery_piece &piece : fp3d::gallery_pieces( index ) ) {
        const tripoint_bub_ms p = here.get_bub( at( s, piece.at ) );
        if( !here.inbounds( p ) ) {
            continue;
        }
        if( piece.furniture ) {
            here.furn_set( p, furn_str_id( piece.id ).id() );
        } else {
            here.ter_set( p, ter_str_id( piece.id ).id() );
        }
    }
    // Working stairs: up one square east of the arrival square, a landing round the top.
    const fp3d::range_offset st = fp3d::gallery_stairs;
    for( int y = -1; y <= 1; y++ ) {
        for( int x = -1; x <= 1; x++ ) {
            set_ter( at( s, { st.x + x, st.y + y, 1 } ), ter_floor );
        }
    }
    set_ter( at( s, st ), ter_stairs_up );
    set_ter( at( s, { st.x, st.y, 1 } ), ter_stairs_down );
}

// Builds zone `index` around its centre (which must be in the bubble) and marks it built.
void build_zone( size_t index )
{
    registry &r = reg();
    fp3d::range_zone &z = r.zones[index];
    const tripoint_abs_ms s = fp3d::zone_centre( z );
    const auto t0 = std::chrono::steady_clock::now();
    clear_area( s );
    switch( z.kind ) {
        case fp3d::zone_kind::hub:
            build_hub( s );
            break;
        case fp3d::zone_kind::gallery:
            build_gallery( index, s );
            break;
        // TODO(K7 slice 2): armory (every gun on a table square with its loaded default magazine and a stack of its
        // ammo, melee and throwables in rows), halls (items of z.category, z.first .. +z.count, in aisles), lots
        // (z.count vehicle prototypes in marked bays), pens (z.count monsters in glass cells, by species), lanes
        // (frozen targets at 5/10/25/50 m) and kitchen (stoves, ovens, a lit fireplace and brazier, fridge, sink,
        // pots and food). Until then they are empty cleared floors.
        case fp3d::zone_kind::armory:
        case fp3d::zone_kind::halls:
        case fp3d::zone_kind::lots:
        case fp3d::zone_kind::pens:
        case fp3d::zone_kind::lanes:
        case fp3d::zone_kind::kitchen:
            break;
    }
    map &here = get_map();
    for( int zl = -1; zl <= 1; zl++ ) {
        here.invalidate_map_cache( zl );
    }
    here.rebuild_vehicle_level_caches();
    z.built = true;
    get_avatar().set_value( built_prefix + z.id, 1.0 );
    r.version++;
    std::printf( "range: built zone %s in %.0f ms\n", z.id.c_str(),
                 std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count() );
}

// After the avatar moved within the loaded bubble: recentre it and refresh what the turn would.
void settle_avatar()
{
    avatar &u = get_avatar();
    map &here = get_map();
    g->update_map( u );
    here.invalidate_map_cache( u.posz() );
    here.build_map_cache( u.posz() );
    g->update_overmap_seen();
}

void quiet_world_options()
{
    options_manager &opts = get_options();
    const auto has = [&opts]( const char *name ) {
        return opts.has_option( name );
    };
    if( has( "CITY_SIZE" ) ) {
        opts.get_option( "CITY_SIZE" ).setValue( 0 );
    }
    if( has( "SPAWN_DENSITY" ) ) {
        opts.get_option( "SPAWN_DENSITY" ).setValue( 0.0f );
    }
    if( has( "NPC_SPAWNTIME" ) ) {
        opts.get_option( "NPC_SPAWNTIME" ).setValue( 0.0f );
    }
    for( const char *name : {
             "WANDER_SPAWNS", "BLACK_ROAD"
         } ) {
        if( has( name ) ) {
            opts.get_option( name ).setValue( std::string( "false" ) );
        }
    }
    if( world_generator->active_world != nullptr ) {
        world_generator->active_world->save();
    }
}

// A fixed character: the generic scenario and profession, average stats, no traits, a fixed name.
void make_avatar()
{
    avatar &u = get_avatar();
    const scenario *scen = scenario::generic();
    std::string prof = profession::generic()->ident().str();
    const std::vector<string_id<profession>> permitted = scen->permitted_professions();
    if( std::none_of( permitted.begin(), permitted.end(), [&prof]( const string_id<profession> &p ) {
    return p.str() == prof;
} ) && !permitted.empty() ) {
        prof = permitted.front().str();
    }
    const std::string why = fp3d::apply_chargen( u, "{\"scenario\":\"" + scen->ident().str() +
                            "\",\"profession\":\"" + prof +
                            "\",\"str\":8,\"dex\":8,\"int\":8,\"per\":8,\"name\":\"Range Tester\",\"male\":true}" );
    if( !why.empty() ) {
        throw std::runtime_error( "range avatar: " + why );
    }
}
} // namespace

// The new-game half of game::start_game() for a given square: no start location, no NPCs, no monsters, no
// missions. The caller sets the calendar first.
void fp3d_game_access::start_game_at( game &g, const tripoint_abs_ms &start )
{
    avatar &u = get_avatar();
    if( !g.gamemode ) {
        g.gamemode = std::make_unique<special_game>();
    }
    g.seed = rng_bits();
    g.new_game = true;
    g.weather.nextweather = calendar::turn;
    g.safe_mode = SAFE_MODE_OFF;
    g.mostseen = 0;
    get_safemode().load_global();
    g.init_autosave();
    g.load_master();
    u.setID( g.assign_npc_id() );
    u.add_profession_items();
    u.migrate_items_to_storage( true );

    const tripoint_abs_omt omt = project_to<coords::omt>( start );
    g.load_map( project_to<coords::sm>( omt ) - point_rel_sm( HALF_MAPSIZE, HALF_MAPSIZE ) );
    u.setpos( start, false );
    map &here = get_map();
    here.invalidate_map_cache( start.z() );
    here.build_map_cache( start.z() );
    overmap_buffer.reveal( omt.xy(), 5, 0 );

    u.set_moves( 0 );
    u.process_turn();
    u.set_stamina( u.get_stamina_max() );
    g.weather.temperature = SPRING_TEMPERATURE;
    g.weather.update_weather();
    u.next_climate_control_check = calendar::before_time_starts;
    u.last_climate_control_ret = false;
    get_auto_pickup().clear_character_rules();
    get_safemode().clear_character_rules();
    get_auto_notes_settings().clear();
    get_auto_notes_settings().default_initialize();
    g.load_npcs();
    u.calc_mutation_levels();
    u.drench_mut_calc();
    for( item *&e : u.inv_dump() ) {
        e->set_owner( u );
    }
    g.update_map( u );
    get_event_bus().send<event_type::game_start>( getVersionString() );
    get_event_bus().send<event_type::game_avatar_new>( /*is_new_game=*/true, /*is_debug=*/false,
            u.getID(), u.name, u.male, u.prof->ident(), u.custom_profession );
    g.time_played_at_last_load = std::chrono::seconds( 0 );
    g.time_of_last_load = std::chrono::steady_clock::now();
    effect_on_conditions::load_new_character( u );
}

namespace fp3d
{

const char *zone_kind_name( zone_kind k )
{
    switch( k ) {
        case zone_kind::hub:
            return "hub";
        case zone_kind::gallery:
            return "gallery";
        case zone_kind::armory:
            return "armory";
        case zone_kind::halls:
            return "halls";
        case zone_kind::lots:
            return "lots";
        case zone_kind::pens:
            return "pens";
        case zone_kind::lanes:
            return "lanes";
        case zone_kind::kitchen:
            return "kitchen";
    }
    return "?";
}

const std::vector<hub_prop> &hub_props()
{
    // The sim test's goals and checks: a door to open, food and water to pick up and eat, a rock to throw, a
    // backpack, a loaded pistol and spare ammo, melee weapons, a hose and an empty jerrycan to siphon the car with,
    // a bag and bottles for the item checks, something too heavy for a bag; plus a window, stairs both ways, a lit
    // fireplace and a frozen zombie behind glass.
    static const std::vector<hub_prop> props = {
        { "apple to pick up and eat", { 0, 0, 0 }, prop_kind::item, "apple" },
        { "closed door", { 15, 0, 0 }, prop_kind::terrain, "t_door_c" },
        { "rock to throw", { -8, -10, 0 }, prop_kind::item, "rock" },
        { "backpack", { -7, -10, 0 }, prop_kind::item, "backpack" },
        { "pistol, loaded magazine", { -6, -10, 0 }, prop_kind::item, "glock_19", 1, "9mm", 15 },
        { "spare 9mm ammo", { -5, -10, 0 }, prop_kind::item, "9mm", 50 },
        { "machete", { -4, -10, 0 }, prop_kind::item, "machete" },
        { "two-handed flail", { -3, -10, 0 }, prop_kind::item, "2h_flail_steel" },
        { "rubber hose", { -2, -10, 0 }, prop_kind::item, "hose" },
        { "empty jerrycan", { -1, -10, 0 }, prop_kind::item, "jerrycan" },
        { "bottle of clean water", { 0, -10, 0 }, prop_kind::item, "bottle_plastic", 1, "water_clean", 2 },
        { "plastic bag", { 1, -10, 0 }, prop_kind::item, "bag_plastic" },
        { "empty plastic bottle", { 2, -10, 0 }, prop_kind::item, "bottle_plastic" },
        { "gold, too heavy for a bag", { 3, -10, 0 }, prop_kind::item, "1l_gold" },
        { "window", { -15, -5, 0 }, prop_kind::terrain, "t_window" },
        { "fireplace", { -10, -5, 0 }, prop_kind::furniture, "f_fireplace" },
        { "firewood in it", { -10, -5, 0 }, prop_kind::item, "2x4", 8 },
        { "fire in it", { -10, -5, 0 }, prop_kind::field, "fd_fire", 1 },
        { "shallow water", { -7, 3, 0 }, prop_kind::terrain, "t_water_sh" },
        { "stairs up", { -10, 8, 0 }, prop_kind::terrain, "t_stairs_up" },
        { "stairs up: the top", { -10, 8, 1 }, prop_kind::terrain, "t_stairs_down" },
        { "stairs down", { -10, 11, 0 }, prop_kind::terrain, "t_stairs_down" },
        { "stairs down: the bottom", { -10, 11, -1 }, prop_kind::terrain, "t_stairs_up" },
        { "glass wall", { 8, -5, 0 }, prop_kind::terrain, "t_wall_glass" },
        { "zombie behind glass", { 9, -6, 0 }, prop_kind::monster, "mon_zombie" },
        { "car with fuel", { 6, 9, 0 }, prop_kind::vehicle, "car", 50 },
    };
    return props;
}

void reset_test_range()
{
    const uint64_t version = reg().version;
    reg() = registry();
    reg().version = version + 1;
    frozen.store( false );
}

bool range_active()
{
    return g != nullptr && avatar_flag( range_var );
}

const std::vector<range_zone> &range_zones()
{
    static const std::vector<range_zone> none;
    registry &r = ready_registry();
    return r.ready && range_active() ? r.zones : none;
}

range_counts range_content_counts()
{
    return ready_registry().counts;
}

tripoint_abs_ms zone_centre( const range_zone &z )
{
    return project_to<coords::ms>( z.centre );
}

int current_zone()
{
    const std::vector<range_zone> &zones = range_zones();
    if( zones.empty() ) {
        return -1;
    }
    const tripoint_abs_ms p = get_avatar().pos_abs();
    for( size_t i = 0; i < zones.size(); i++ ) {
        const tripoint_abs_ms c = zone_centre( zones[i] );
        const int dx = p.x() - c.x();
        const int dy = p.y() - c.y();
        if( dx >= -zone_half_squares && dx < zone_half_squares && dy >= -zone_half_squares &&
            dy < zone_half_squares ) {
            return static_cast<int>( i );
        }
    }
    return -1;
}

std::vector<gallery_piece> gallery_pieces( size_t zone )
{
    std::vector<gallery_piece> out;
    const std::vector<range_zone> &zones = range_zones();
    if( zone >= zones.size() || zones[zone].kind != zone_kind::gallery ) {
        return out;
    }
    const registry &r = reg();
    const range_zone &z = zones[zone];
    const std::vector<range_offset> &slots = gallery_slots();
    for( size_t i = 0; i < z.count && z.first + i < r.gallery_normal.size() && i < slots.size(); i++ ) {
        const piece_id &p = r.gallery_normal[z.first + i];
        out.push_back( { p.id, p.furniture, slots[i] } );
    }
    const std::vector<range_offset> &eslots = gallery_emitter_slots();
    const size_t efirst = static_cast<size_t>( z.part ) * eslots.size();
    for( size_t i = 0; i < eslots.size() && efirst + i < r.gallery_emitters.size(); i++ ) {
        const piece_id &p = r.gallery_emitters[efirst + i];
        out.push_back( { p.id, p.furniture, eslots[i] } );
    }
    return out;
}

void boot_test_range( unsigned seed )
{
    if( g == nullptr || world_generator->active_world == nullptr ) {
        throw std::runtime_error( "test range: load the game data first (boot_new_game_data)" );
    }
    const auto t0 = std::chrono::steady_clock::now();
    rng_set_engine_seed( seed );
    reset_test_range();
    quiet_world_options();
    calendar::start_of_cataclysm = calendar::turn_zero;
    calendar::start_of_game = calendar::turn_zero + 1_days + 10_hours;
    calendar::turn = calendar::start_of_game;
    calendar::initial_season = SPRING;

    make_avatar();
    avatar &u = get_avatar();
    u.set_value( range_var, 1.0 );
    const std::vector<range_zone> &zones = range_zones();
    if( zones.empty() ) {
        throw std::runtime_error( "test range: no zones" );
    }
    const tripoint_abs_ms hub = zone_centre( zones.front() );
    fp3d_game_access::start_game_at( *g, hub );
    build_zone( 0 );
    u.setpos( hub, false );
    settle_avatar();
    frozen.store( true );
    std::printf( "range: booted with seed %u, %zu zones, in %.1f s\n", seed, zones.size(),
                 std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count() );
}

namespace
{
// The operations change the world: only on the sim thread, right before a main-loop (timed) request, never
// in the middle of an action (a popup's or menu's request) and never from another thread.
bool between_turns()
{
    return next_is_timed();
}
constexpr const char *not_now = "Only between turns.";

bool coord_ok( const ui_point &p )
{
    const auto ok = []( int v ) {
        return v >= -max_coord && v <= max_coord;
    };
    return ok( p.x ) && ok( p.y ) && ok( p.z );
}
} // namespace

std::string spawn_item( uint32_t itype, int count, const ui_point &square )
{
    if( !between_turns() ) {
        return not_now;
    }
    if( count < 1 || count > max_spawn_count ) {
        return string_format( "Count must be 1 to %d.", max_spawn_count );
    }
    const std::vector<std::string> names = id_names( id_kind::itype );
    if( itype == 0 || itype >= names.size() ) {
        return "No such item type.";
    }
    const itype_id type( names[itype] );
    if( !type.is_valid() || type.is_null() || type->has_flag( range_pseudo ) ) {
        return "That item type can't be spawned.";
    }
    if( !coord_ok( square ) ) {
        return "That square is outside the reality bubble.";
    }
    map &here = get_map();
    const tripoint_bub_ms p = here.get_bub( tripoint_abs_ms( square.x, square.y, square.z ) );
    if( !here.inbounds( p ) ) {
        return "That square is outside the reality bubble.";
    }
    item it( type, calendar::turn );
    if( it.count_by_charges() ) {
        it.charges = count;
        here.add_item_or_charges( p, it );
    } else {
        for( int i = 0; i < count; i++ ) {
            here.add_item_or_charges( p, it );
        }
    }
    return "";
}

std::string spawn_monster( const std::string &mtype, const ui_point &square )
{
    if( !between_turns() ) {
        return not_now;
    }
    if( mtype.empty() || mtype.size() > max_monster_id ) {
        return "No such monster.";
    }
    for( const char c : mtype ) {
        const bool ok = ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) ||
                        c == '_' || c == '-';
        if( !ok ) {
            return "No such monster.";
        }
    }
    const mtype_id type( mtype );
    if( !type.is_valid() || type.is_null() ) {
        return "No such monster.";
    }
    if( !coord_ok( square ) ) {
        return "That square is outside the reality bubble.";
    }
    map &here = get_map();
    const tripoint_bub_ms p = here.get_bub( tripoint_abs_ms( square.x, square.y, square.z ) );
    if( !here.inbounds( p ) ) {
        return "That square is outside the reality bubble.";
    }
    if( !here.passable( p ) ) {
        return "Something is in the way there.";
    }
    if( get_creature_tracker().creature_at( p ) != nullptr ) {
        return "Someone is already there.";
    }
    if( g->place_critter_at( type, p ) == nullptr ) {
        return "It can't be placed there.";
    }
    return "";
}

std::string teleport_to_zone( size_t index )
{
    if( !between_turns() ) {
        return not_now;
    }
    if( !range_active() ) {
        return "Only in the test range.";
    }
    const std::vector<range_zone> &zones = range_zones();
    if( index >= zones.size() ) {
        return "No such zone.";
    }
    avatar &u = get_avatar();
    const tripoint_abs_ms dest = zone_centre( zones[index] );
    g->place_player_overmap( zones[index].centre, false );
    // The avatar is still where it was (outside the new bubble): put it on the arrival square before anything
    // looks at what it sees.
    u.setpos( dest, false );
    if( !zones[index].built ) {
        build_zone( index );
    }
    u.setpos( dest, false );
    settle_avatar();
    reg().version++;
    return "";
}

std::string set_time_of_day( int minute )
{
    if( !between_turns() ) {
        return not_now;
    }
    if( minute < 0 || minute > 1439 ) {
        return "The minute of the day must be 0 to 1439.";
    }
    const time_point day_start = calendar::turn - time_past_midnight( calendar::turn );
    time_point target = day_start + time_duration::from_minutes( minute );
    if( target <= calendar::turn ) {
        target += 1_days;
    }
    calendar::turn = target;
    get_weather().update_weather();
    g->reset_light_level();
    get_map().invalidate_map_cache( get_avatar().posz() );
    return "";
}

std::string advance( int minutes )
{
    if( !between_turns() ) {
        return not_now;
    }
    if( minutes < 1 || minutes > max_advance_minutes ) {
        return string_format( "Minutes must be 1 to %d.", max_advance_minutes );
    }
    avatar &u = get_avatar();
    if( u.activity ) {
        return "Finish what you are doing first.";
    }
    u.assign_activity( player_activity( act_wait, 100 * to_turns<int>( time_duration::from_minutes( minutes ) ),
                                        0 ) );
    return "";
}

std::string set_ai_frozen( bool on )
{
    if( frozen.exchange( on ) != on ) {
        reg().version++;
    }
    return "";
}

std::string heal()
{
    if( !between_turns() ) {
        return not_now;
    }
    avatar &u = get_avatar();
    u.set_all_parts_hp_to_max();
    u.set_pain( 0 );
    u.set_hunger( 0 );
    u.set_thirst( 0 );
    u.set_stored_kcal( u.get_healthy_kcal() );
    u.set_sleepiness( 0 );
    u.set_sleep_deprivation( 0 );
    u.set_rad( 0 );
    u.set_stamina( u.get_stamina_max() );
    std::vector<std::pair<efftype_id, bodypart_id>> bad;
    for( const effect &e : u.get_effects() ) {
        const effect_type *t = e.get_effect_type();
        if( t != nullptr && t->get_rating( e.get_intensity() ) == m_bad ) {
            bad.emplace_back( e.get_id(), e.get_bp() );
        }
    }
    for( const auto &[id, bp] : bad ) {
        u.remove_effect( id, bp );
    }
    return "";
}

bool ai_frozen()
{
    return frozen.load( std::memory_order_relaxed );
}

void capture_range_info()
{
    if( g == nullptr ) {
        return;
    }
    // Rebuilt only when something it shows changed.
    static bool have = false;
    static bool last_range = false;
    static bool last_frozen = false;
    static int last_zone = -1;
    static uint64_t last_version = 0;
    const bool range = range_active();
    const bool is_frozen = ai_frozen();
    const int zone = range ? current_zone() : -1;
    const uint64_t version = reg().version;
    if( have && range == last_range && is_frozen == last_frozen && zone == last_zone && version == last_version ) {
        return;
    }
    have = true;
    last_range = range;
    last_frozen = is_frozen;
    last_zone = zone;
    last_version = version;
    range_info info;
    info.range = range;
    info.ai_frozen = is_frozen;
    info.zone = zone;
    if( range ) {
        for( const range_zone &z : range_zones() ) {
            info.zones.push_back( { z.id, z.name, zone_kind_name( z.kind ), z.built } );
        }
    }
    std::lock_guard<std::mutex> lk( info_mutex );
    info.seq = ++info_seq;
    info_latest = std::move( info );
}

range_info latest_range_info()
{
    std::lock_guard<std::mutex> lk( info_mutex );
    return info_latest;
}

} // namespace fp3d
