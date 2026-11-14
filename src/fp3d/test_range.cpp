// K7 test range (PLAN.md K7). See test_range.h.
#include "fp3d/test_range.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

#include "achievement.h"
#include "auto_note.h"
#include "auto_pickup.h"
#include "avatar.h"
#include "bodypart.h"
#include "calendar.h"
#include "character.h"
#include "coordinates.h"
#include "creature.h"
#include "creature_tracker.h"
#include "damage.h"
#include "effect.h"
#include "effect_on_condition.h"
#include "enums.h"
#include "event.h"
#include "event_bus.h"
#include "field.h"
#include "field_type.h"
#include "flag.h"
#include "fp3d/arrival.h"
#include "fp3d/boot.h"
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
// Avatar values (test_range.h): kept in saves, read back by resume_test_range.
const std::string range_var = "fp3d_range";
const std::string built_prefix = "fp3d_range_built_";
const std::string seed_var = "fp3d_range_seed";
const std::string frozen_var = "fp3d_range_ai_frozen";
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
// Planted in the gallery's plant furniture (any seed will do).
const itype_id itype_plant_seed( "seed_wheat" );
// Zone builders: armory tables, hall racks, lot paint, pen glass and water, lane paint.
const furn_str_id furn_table( "f_table" );
const furn_str_id furn_rack( "f_rack" );
const ter_str_id ter_pavement( "t_pavement" );
const ter_str_id ter_paint( "t_pavement_y" );
const ter_str_id ter_pen_glass( "t_reinforced_glass" );
const ter_str_id ter_pen_water( "t_water_dp" );
const ter_str_id ter_pen_dirt( "t_dirt" );
const ter_str_id ter_pen_shade( "t_thconc_floor" );   // INDOORS: no sunlight
// Over the pens: a floor to flyers (they can't rise through it) that lets the daylight in (TRANSPARENT_FLOOR).
const ter_str_id ter_pen_roof( "t_glass_roof" );
const item_category_id item_category_weapons_range( "weapons" );
const flag_id flag_grenade_range( "GRENADE" );
const damage_type_id damage_bash_range( "bash" );
// A liquid without a default container goes into one of these to lie on a shelf.
const itype_id itype_liquid_fallback( "jerrycan" );

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
    // The lists the other kinds slice (zone first/count), ids sorted.
    std::vector<itype_id> armory;             // guns, then melee weapons, then throwables
    size_t armory_guns = 0;
    size_t armory_melee = 0;
    std::map<std::string, std::vector<itype_id>> halls;   // by category id
    std::vector<vproto_id> vehicles;
    std::vector<mtype_id> monsters;           // by species (the first by id), then id
};

registry &reg()
{
    static registry r;
    return r;
}

// boot_test_range's seed (or the saved one resume_test_range read back): zone builds are seeded from it (game thread).
unsigned range_seed = 0;

// CDDA's RNG during a build: seeded from the boot seed and the zone id, so a zone comes out the same each time it is
// built (item variants, vehicle contents, ...); the game's own stream is put back afterwards, untouched.
class build_rng
{
    public:
        explicit build_rng( const std::string &zone_id ) : saved( rng_get_engine() ) {
            uint32_t h = 2166136261u ^ range_seed;   // FNV-1a
            for( const char c : zone_id ) {
                h ^= static_cast<unsigned char>( c );
                h *= 16777619u;
            }
            rng_get_engine().seed( h );
        }
        ~build_rng() {
            rng_get_engine() = saved;
        }
        build_rng( const build_rng & ) = delete;
        build_rng &operator=( const build_rng & ) = delete;
    private:
        cata_default_random_engine saved;
};

bool by_id( const itype_id &a, const itype_id &b )
{
    return a.str() < b.str();
}

// The armory's thrown weapons: bombs, grenades and anything whose data gives it thrown damage (item_factory fills a
// missing one with 3/4 of the melee bash: that one doesn't count).
bool throwable( const itype &t )
{
    if( t.has_flag( flag_BOMB ) || t.has_flag( flag_grenade_range ) ) {
        return true;
    }
    const std::vector<damage_unit> &units = t.thrown_damage.damage_units;
    if( units.size() != 1 ) {
        return !units.empty();
    }
    const auto bash = t.melee.damage_map.find( damage_bash_range );
    const float melee_bash = bash == t.melee.damage_map.end() ? 0.0f : bash->second;
    return units.front().type != damage_bash_range ||
           units.front().amount != static_cast<float>( melee_bash * 0.75 );
}

std::atomic<bool> frozen{ false };
// Set by boot_test_range for the game it starts and by resume_test_range for a saved range it resumes; cleared by every
// boot (reset_test_range).
std::atomic<bool> session{ false };

std::mutex info_mutex;
fp3d::range_info info_latest;
uint64_t info_seq = 0;

// Lower-case letters, digits and '_' only, at most `cap` characters: zone ids end up in avatar value names and the
// client.
std::string id_part( const std::string &s, size_t cap )
{
    std::string out;
    for( const char c : s ) {
        if( out.size() >= cap ) {
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

// The AI freeze, also in the avatar's values: a saved range comes back as it was (resume_test_range).
void store_frozen( bool on )
{
    frozen.store( on );
    get_avatar().set_value( frozen_var, on ? 1.0 : 0.0 );
}

// The range is for trying things, not for surviving the weather: a fixed temperature (as CDDA's tutorial), a clear
// sky and no wind, so a cold, wet or windy day never stops a wait with "You are freezing! Stop waiting?" and snow or
// fog never hides a target. None of it is in a save (the temperature is the range's, not the world's: savegame.cpp
// leaves it out for a range game; CDDA saves neither override), so resume_test_range sets all three again. CDDA's
// first turn of a game clears the sky's override, and do_turn keeps it for a range game (range_active). Each boot
// makes a new game (and weather), so a later normal game never inherits it. Game thread, once the avatar is on the
// map.
void keep_range_weather()
{
    weather_manager &w = get_weather();
    w.forced_temperature = fp3d::range_temperature;
    w.weather_override = WEATHER_CLEAR;
    w.windspeed_override = 0;
    w.set_nextweather( calendar::turn );
}

// What a save says (hostile: bounded). The boot seed: an integer 0 .. UINT_MAX, else (missing, not a number, out of
// range, a fraction) 1.
unsigned saved_seed()
{
    const diag_value *v = get_avatar().maybe_get_value( seed_var );
    if( v == nullptr || !v->is_dbl() ) {
        return 1;
    }
    const double d = v->dbl();
    if( !std::isfinite( d ) || d < 0.0 || d > static_cast<double>( UINT_MAX ) || d != std::floor( d ) ) {
        return 1;
    }
    return static_cast<unsigned>( d );
}

// The AI freeze: off only for 0, else (missing, anything else) frozen.
bool saved_frozen()
{
    const diag_value *v = get_avatar().maybe_get_value( frozen_var );
    return !( v != nullptr && v->is_dbl() && v->dbl() == 0.0 );
}

// Builds the registry from the loaded data (mods included); ids sorted, so the layout is stable.
void build_registry()
{
    registry &r = reg();
    const uint64_t version = r.version + 1;
    r = registry();
    r.version = version;
    fp3d::range_counts &c = r.counts;

    std::map<std::string, size_t> per_category;
    std::vector<itype_id> guns;
    std::vector<itype_id> melee;
    std::vector<itype_id> thrown;
    for( const itype *t : item_controller->all() ) {
        if( t == nullptr || t->get_id().is_null() || t->has_flag( range_pseudo ) ) {
            continue;
        }
        if( t->gun && !t->gunmod ) {
            c.guns++;
            guns.push_back( t->get_id() );
            continue;
        }
        c.items++;
        per_category[t->category_force.str()]++;
        r.halls[t->category_force.str()].push_back( t->get_id() );
        if( t->category_force == item_category_weapons_range ) {
            ( throwable( *t ) ? thrown : melee ).push_back( t->get_id() );
        }
    }
    c.items_per_category.assign( per_category.begin(), per_category.end() );
    for( auto &cat_list : r.halls ) {
        std::sort( cat_list.second.begin(), cat_list.second.end(), by_id );
    }
    for( std::vector<itype_id> *list : {
             &guns, &melee, &thrown
         } ) {
        std::sort( list->begin(), list->end(), by_id );
        r.armory.insert( r.armory.end(), list->begin(), list->end() );
    }
    r.armory_guns = guns.size();
    r.armory_melee = melee.size();
    c.melee = melee.size();
    c.throwables = thrown.size();
    for( const vehicle_prototype &p : vehicles::get_all_prototypes() ) {
        if( p.id.str() != "none" ) {
            r.vehicles.push_back( p.id );
        }
    }
    std::sort( r.vehicles.begin(), r.vehicles.end(), []( const vproto_id & a, const vproto_id & b ) {
        return a.str() < b.str();
    } );
    c.vehicles = r.vehicles.size();
    std::vector<std::pair<std::string, mtype_id>> mons;
    for( const mtype &m : MonsterGenerator::generator().get_all_mtypes() ) {
        if( m.id.is_null() ) {
            continue;
        }
        std::string species = "~";   // none: last
        for( const species_id &sp : m.species ) {
            species = std::min( species, sp.str() );
        }
        mons.emplace_back( species, m.id );
    }
    std::sort( mons.begin(), mons.end(), []( const auto & a, const auto & b ) {
        return a.first != b.first ? a.first < b.first : a.second.str() < b.second.str();
    } );
    for( const auto &sp_id : mons ) {
        r.monsters.push_back( sp_id.second );
    }
    c.monsters = r.monsters.size();
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
    add_parts( zones, fp3d::zone_kind::armory, "armory", "Armory", r.armory.size(), fp3d::armory_items_per_zone );
    std::vector<std::string> cats;
    cats.reserve( c.items_per_category.size() );
    for( const auto &cat_n : c.items_per_category ) {
        cats.push_back( cat_n.first );
    }
    // Categories whose ids differ only in case or punctuation would share an id: the bases are made unique.
    const std::vector<std::string> bases = fp3d::hall_id_bases( cats );
    for( size_t i = 0; i < cats.size() && i < bases.size(); i++ ) {
        const std::string &cat = cats[i];
        const item_category_id cid( cat );
        const std::string label = cid.is_valid() ? cid->name_header() : cat;
        add_parts( zones, fp3d::zone_kind::halls, bases[i], "Halls: " + label, c.items_per_category[i].second,
                   fp3d::hall_items_per_zone, cat );
    }
    add_parts( zones, fp3d::zone_kind::lots, "lots", "Vehicle lot", c.vehicles, fp3d::lot_vehicles_per_zone );
    add_parts( zones, fp3d::zone_kind::pens, "pens", "Monster pens", c.monsters, fp3d::pen_monsters_per_zone );
    fp3d::range_zone lanes;
    lanes.kind = fp3d::zone_kind::lanes;
    lanes.id = "lanes";
    lanes.name = "Target lanes";
    lanes.count = fp3d::lane_targets().size();
    zones.push_back( lanes );
    fp3d::range_zone kitchen;
    kitchen.kind = fp3d::zone_kind::kitchen;
    kitchen.id = "kitchen";
    kitchen.name = "Kitchen";
    kitchen.count = fp3d::kitchen_props().size();
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

// Whether `p` is in the area of the zone round `centre` (the squares clear_area clears): x and y centre - 60 ..
// centre + 59, z -1 and up.
bool in_area( const tripoint_abs_ms &centre, const tripoint_abs_ms &p )
{
    const int h = fp3d::zone_half_squares;
    const int dx = p.x() - centre.x();
    const int dy = p.y() - centre.y();
    return dx >= -h && dx < h && dy >= -h && dy < h && p.z() - centre.z() >= -1;
}

// Clears the zone's area: the NPCs and monsters in it gone (the loaded bubble reaches 12 squares further east and
// south: whoever is there stays), then per square from the top down: open air above z0, the floor at z0, solid ground
// at z -1; no furniture, items, vehicles, traps, fields, graffiti or radiation.
void clear_area( const tripoint_abs_ms &centre )
{
    map &here = get_map();
    g->reload_npcs();
    for( npc &guy : g->all_npcs() ) {
        if( in_area( centre, guy.pos_abs() ) ) {
            guy.die( &here, nullptr );
        }
    }
    g->cleanup_dead();
    std::vector<monster *> gone;
    for( monster &m : g->all_monsters() ) {
        if( in_area( centre, m.pos_abs() ) ) {
            gone.push_back( &m );
        }
    }
    // The tracker keeps a removed monster alive until the turn ends: the other pointers stay good.
    for( monster *m : gone ) {
        g->remove_zombie( *m );
    }
    // Mapgen's monster spawn points would spawn at the next update_map. Loading the bubble spawned them already
    // (place_player_overmap, update_map), so this drops at most a few new ones; map has no call for part of the
    // bubble.
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

// Places `props` around `s` (hub_prop: terrain, furniture, items with what is inside, monsters, vehicles, fields);
// returns how many were placed.
size_t place_props( const tripoint_abs_ms &s, const std::vector<fp3d::hub_prop> &props )
{
    map &here = get_map();
    size_t placed = 0;
    for( const fp3d::hub_prop &prop : props ) {
        const tripoint_abs_ms abs = at( s, prop.at );
        const tripoint_bub_ms p = here.get_bub( abs );
        if( !here.inbounds( p ) ) {
            continue;
        }
        bool ok = false;
        switch( prop.kind ) {
            case fp3d::prop_kind::terrain:
                here.ter_set( p, ter_str_id( prop.id ).id() );
                ok = here.ter( p ).id().str() == prop.id;
                break;
            case fp3d::prop_kind::furniture:
                here.furn_set( p, furn_str_id( prop.id ).id() );
                ok = here.furn( p ).id().str() == prop.id;
                break;
            case fp3d::prop_kind::field:
                ok = here.add_field( p, field_type_str_id( prop.id ).id(), prop.count, 0_turns, false );
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
                    ok = !here.add_item( p, it ).is_null();
                } else {
                    ok = true;
                    for( int i = 0; i < prop.count; i++ ) {
                        ok = !here.add_item( p, it ).is_null() && ok;
                    }
                }
                break;
            }
            case fp3d::prop_kind::monster: {
                const mtype_id type( prop.id );
                ok = type.is_valid() && g->place_critter_at( type, p ) != nullptr;
                break;
            }
            case fp3d::prop_kind::vehicle: {
                const vproto_id type( prop.id );
                // Intact, no faults or security; `count` = fuel percent.
                ok = type.is_valid() && here.add_vehicle( type, p, 0_degrees, prop.count, 2, false, true ) != nullptr;
                break;
            }
        }
        placed += ok ? 1 : 0;
    }
    return placed;
}

void build_hub( const tripoint_abs_ms &s )
{
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
    place_props( s, fp3d::hub_props() );
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
            const furn_id f = furn_str_id( piece.id ).id();
            // A plant needs its seed under it, as CDDA's planted plots have (mapgen's sealed_item): without one the
            // next load of the submap (map::grow_plant) logs an error and removes the plant.
            if( f->has_flag( ter_furn_flag::TFLAG_PLANT ) && itype_plant_seed.is_valid() ) {
                here.add_item( p, item( itype_plant_seed, calendar::turn ) );
            }
            here.furn_set( p, f );
        } else {
            const ter_id t = ter_str_id( piece.id ).id();
            // Open air over natural rock becomes a rock floor whenever the submap loads again (map::add_tree_tops):
            // a wall under it keeps it open.
            if( t->has_flag( "EMPTY_SPACE" ) ) {
                const tripoint_bub_ms below = p + tripoint_rel_ms::below;
                if( here.inbounds( below ) ) {
                    here.ter_set( below, ter_wall.id() );
                }
            }
            here.ter_set( p, t );
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

// Items an armory or hall lays out: not the armed bombs (DANGEROUS), nothing that never lies on a square by itself
// (NO_DROP), no corpse that would rise again.
bool layable( const itype_id &type )
{
    if( !type.is_valid() || type.is_null() ) {
        return false;
    }
    const itype &t = *type;
    if( t.has_flag( flag_DANGEROUS ) || t.has_flag( flag_NO_DROP ) ) {
        return false;
    }
    return t.source_monster.is_null() || !t.source_monster.is_valid() ||
           !t.source_monster->has_flag( mon_flag_REVIVES );
}

// An item as it can lie on a shelf: a liquid or gas in its default container (a liquid without one in a jerrycan);
// nullopt when there is nothing to hold it.
std::optional<item> ground_form( const item &it )
{
    if( it.made_of( phase_id::SOLID ) ) {
        return it;
    }
    item held = it.in_its_container();
    if( held.typeId() == it.typeId() && it.made_of( phase_id::LIQUID ) && itype_liquid_fallback.is_valid() ) {
        held = it.in_container( itype_liquid_fallback );
    }
    if( held.typeId() == it.typeId() || held.all_items_top().empty() ) {
        return std::nullopt;
    }
    return held;
}

// On that square only, if it fits (volume, item count).
bool put_item( const tripoint_bub_ms &p, const item &it )
{
    return !get_map().add_item_or_charges( p, it, false ).is_null();
}

// Whether `gun` can be loaded with `ammo` by ammo_set: an integral magazine, or a default magazine to fill.
bool loadable( const item &gun, const itype_id &ammo )
{
    if( ammo.is_null() || !ammo.is_valid() || !ammo->ammo ) {
        return false;
    }
    if( gun.magazine_integral() ) {
        return true;
    }
    const itype_id mag = gun.magazine_default();
    return !mag.is_null() && mag.is_valid() && mag->magazine;
}

// What the current build left out (the first few, for the log); game thread.
std::vector<std::string> build_skips;
void skip( const std::string &id )
{
    if( build_skips.size() < 64 ) {
        build_skips.push_back( id );
    }
}

// Armory: each item type on its own table; a gun loaded (its default magazine, or its own ammo) with a stack of its
// default ammo next to it on the table.
size_t build_armory( size_t index, const tripoint_abs_ms &s )
{
    map &here = get_map();
    size_t placed = 0;
    for( const fp3d::zone_piece &piece : fp3d::zone_pieces( index ) ) {
        const tripoint_bub_ms p = here.get_bub( at( s, piece.at ) );
        if( !here.inbounds( p ) ) {
            continue;
        }
        here.furn_set( p, furn_table.id() );
        const itype_id type( piece.id );
        if( !layable( type ) ) {
            skip( piece.id );
            continue;
        }
        item it( type, calendar::turn );
        std::optional<item> stack;
        if( piece.what == "gun" ) {
            const itype_id ammo = it.ammo_default();
            if( loadable( it, ammo ) ) {
                it.ammo_set( ammo, -1 );
                stack = ground_form( item( ammo, calendar::turn ) );
            }
        }
        if( !put_item( p, it ) ) {
            skip( piece.id );
            continue;
        }
        placed++;
        if( stack ) {
            put_item( p, *stack );
        }
    }
    return placed;
}

// Halls: one item type per display rack (on the floor when the rack can't take it).
size_t build_halls( size_t index, const tripoint_abs_ms &s )
{
    map &here = get_map();
    size_t placed = 0;
    for( const fp3d::zone_piece &piece : fp3d::zone_pieces( index ) ) {
        const tripoint_bub_ms p = here.get_bub( at( s, piece.at ) );
        if( !here.inbounds( p ) ) {
            continue;
        }
        here.furn_set( p, furn_rack.id() );
        const itype_id type( piece.id );
        if( !layable( type ) ) {
            skip( piece.id );
            continue;
        }
        const std::optional<item> it = ground_form( item( type, calendar::turn ) );
        if( !it ) {
            skip( piece.id );
            continue;
        }
        if( !put_item( p, *it ) ) {
            here.furn_set( p, furn_str_id::NULL_ID() );
            if( !put_item( p, *it ) ) {
                skip( piece.id );
                continue;
            }
        }
        placed++;
    }
    return placed;
}

// Lots: each bay paved and painted round, its vehicle facing east, intact, half a tank.
size_t build_lots( size_t index, const tripoint_abs_ms &s )
{
    map &here = get_map();
    size_t placed = 0;
    for( const fp3d::zone_piece &piece : fp3d::zone_pieces( index ) ) {
        for( int y = piece.min.y - 1; y <= piece.max.y + 1; y++ ) {
            for( int x = piece.min.x - 1; x <= piece.max.x + 1; x++ ) {
                const bool border = x < piece.min.x || x > piece.max.x || y < piece.min.y || y > piece.max.y;
                set_ter( at( s, { x, y, 0 } ), border ? ter_paint : ter_pavement );
            }
        }
        const tripoint_bub_ms p = here.get_bub( at( s, piece.at ) );
        const vproto_id type( piece.id );
        if( here.inbounds( p ) && type.is_valid() &&
            here.add_vehicle( type, p, 0_degrees, 50, 2, false, true ) != nullptr ) {
            placed++;
        } else {
            skip( piece.id );
        }
    }
    return placed;
}

// Pens: each monster in its reinforced glass cell under a glass roof, on the floor it can stand on (water, dirt to dig
// in, an indoor floor out of the sun). A new monster's timers start at the beginning of time, so the first reload
// would catch up (dung on the floor, ...): caught up here, the cell swept. The cells are laid first and the map caches
// rebuilt, so a monster the sun kills sees the indoor floor as shelter.
size_t build_pens( size_t index, const tripoint_abs_ms &s )
{
    map &here = get_map();
    size_t placed = 0;
    const std::vector<fp3d::zone_piece> pieces = fp3d::zone_pieces( index );
    for( const fp3d::zone_piece &piece : pieces ) {
        const ter_str_id &inside = piece.what == "water" ? ter_pen_water : piece.what == "dirt" ? ter_pen_dirt :
                                   piece.what == "shade" ? ter_pen_shade : ter_floor;
        for( int y = piece.min.y - 1; y <= piece.max.y + 1; y++ ) {
            for( int x = piece.min.x - 1; x <= piece.max.x + 1; x++ ) {
                const bool wall = x < piece.min.x || x > piece.max.x || y < piece.min.y || y > piece.max.y;
                set_ter( at( s, { x, y, 0 } ), wall ? ter_pen_glass : inside );
                // A glass roof over the cell and its walls: a flyer stays in once the AI runs.
                set_ter( at( s, { x, y, 1 } ), ter_pen_roof );
            }
        }
    }
    here.build_map_cache( here.get_bub( s ).z(), true );
    for( const fp3d::zone_piece &piece : pieces ) {
        const tripoint_bub_ms p = here.get_bub( at( s, piece.at ) );
        const mtype_id type( piece.id );
        monster *m = here.inbounds( p ) && type.is_valid() ? g->place_critter_at( type, p ) : nullptr;
        if( m == nullptr ) {
            skip( piece.id );
            continue;
        }
        // Evolution is off in the range world (quiet_world_options): on_load can't change its type.
        m->on_load();
        for( int y = piece.min.y; y <= piece.max.y; y++ ) {
            for( int x = piece.min.x; x <= piece.max.x; x++ ) {
                const tripoint_bub_ms q = here.get_bub( at( s, { x, y, 0 } ) );
                if( here.inbounds( q ) ) {
                    here.i_clear( q );
                }
            }
        }
        placed++;
    }
    return placed;
}

// Lanes: the shooting line, lane lines, the wall behind, then the targets.
size_t build_lanes( const tripoint_abs_ms &s )
{
    map &here = get_map();
    const std::vector<fp3d::lane_target> &lanes = fp3d::lane_targets();
    int min_x = 0;
    int max_x = 0;
    for( const fp3d::lane_target &t : lanes ) {
        min_x = std::min( min_x, t.shooter.x - 2 );
        max_x = std::max( max_x, t.shooter.x + 2 );
    }
    for( int x = min_x; x <= max_x; x++ ) {
        set_ter( at( s, { x, 0, 0 } ), ter_paint );
        set_ter( at( s, { x, fp3d::lane_backstop_y, 0 } ), ter_wall );
        if( ( x - min_x ) % 4 == 0 ) {
            for( int y = fp3d::lane_backstop_y + 1; y < 0; y++ ) {
                set_ter( at( s, { x, y, 0 } ), ter_paint );
            }
        }
    }
    size_t placed = 0;
    for( const fp3d::lane_target &t : lanes ) {
        const tripoint_bub_ms p = here.get_bub( at( s, t.target ) );
        if( !here.inbounds( p ) ) {
            continue;
        }
        if( t.monster ) {
            const mtype_id type( t.id );
            placed += type.is_valid() && g->place_critter_at( type, p ) != nullptr ? 1 : 0;
        } else {
            here.furn_set( p, furn_str_id( t.id ).id() );
            placed += here.furn( p ).id().str() == t.id ? 1 : 0;
        }
    }
    return placed;
}

// Builds zone `index` around its centre (which must be in the bubble) and marks it built.
void build_zone( size_t index )
{
    registry &r = reg();
    fp3d::range_zone &z = r.zones[index];
    const tripoint_abs_ms s = fp3d::zone_centre( z );
    const auto t0 = std::chrono::steady_clock::now();
    const build_rng seeded( z.id );
    build_skips.clear();
    clear_area( s );
    size_t placed = z.count;
    switch( z.kind ) {
        case fp3d::zone_kind::hub:
            build_hub( s );
            break;
        case fp3d::zone_kind::gallery:
            build_gallery( index, s );
            break;
        case fp3d::zone_kind::armory:
            placed = build_armory( index, s );
            break;
        case fp3d::zone_kind::halls:
            placed = build_halls( index, s );
            break;
        case fp3d::zone_kind::lots:
            placed = build_lots( index, s );
            break;
        case fp3d::zone_kind::pens:
            placed = build_pens( index, s );
            break;
        case fp3d::zone_kind::lanes:
            placed = build_lanes( s );
            break;
        case fp3d::zone_kind::kitchen:
            placed = place_props( s, fp3d::kitchen_props() );
            break;
    }
    map &here = get_map();
    for( int zl = -1; zl <= 1; zl++ ) {
        here.invalidate_map_cache( zl );
    }
    here.rebuild_vehicle_level_caches();
    z.built = true;
    z.placed = std::min( placed, z.count );
    z.skipped = z.count - z.placed;
    z.build_ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
    get_avatar().set_value( built_prefix + z.id, 1.0 );
    r.version++;
    std::string skipped;
    for( size_t i = 0; i < build_skips.size() && i < 8; i++ ) {
        skipped += " " + build_skips[i];
    }
    if( z.skipped > std::min<size_t>( build_skips.size(), 8 ) ) {
        skipped += " ...";
    }
    std::printf( "range: built zone %s in %.0f ms (%zu of %zu placed%s%s)\n", z.id.c_str(), z.build_ms, z.placed,
                 z.count, skipped.empty() ? "" : "; skipped:", skipped.c_str() );
}

// Loads the bubble round `omt` (CDDA's overmap teleport: the creatures of the old bubble are saved, a vehicle the
// player is in is left) and puts the avatar on `square` in it: it is still where it was, outside the new bubble, and
// must be in before anything looks at what it sees. settle_avatar() before the turn goes on.
void load_bubble_at( const tripoint_abs_omt &omt, const tripoint_abs_ms &square )
{
    g->place_player_overmap( omt, false );
    get_avatar().setpos( square, false );
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
    // Pen monsters keep their type: no evolution.
    if( has( "EVOLUTION_INVERSE_MULTIPLIER" ) ) {
        opts.get_option( "EVOLUTION_INVERSE_MULTIPLIER" ).setValue( 0.0f );
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
    session.store( false );
    range_seed = 0;
}

bool range_active()
{
    return session.load() && g != nullptr && avatar_flag( range_var );
}

bool range_world( const std::string &world_name )
{
    // The names boot_new_game_data( "fp3d-range", ..., replace ) gives (replacing_world_name: "-2" .. "-99"), digits
    // capped; then FP3D's marker in the folder.
    static const std::string base = "fp3d-range";
    if( world_name.compare( 0, base.size(), base ) != 0 ) {
        return false;
    }
    const std::string rest = world_name.substr( base.size() );
    const auto digit = []( char c ) {
        return c >= '0' && c <= '9';
    };
    if( !rest.empty() && ( rest.size() < 2 || rest.size() > 10 || rest[0] != '-' ||
                           !std::all_of( rest.begin() + 1, rest.end(), digit ) ) ) {
        return false;
    }
    return owned_world( world_name );
}

unsigned range_boot_seed()
{
    return range_seed;
}

std::vector<std::string> hall_id_bases( const std::vector<std::string> &categories )
{
    static const std::string prefix = "halls_";
    static_assert( max_hall_id_base > 8 && max_hall_id_base + 10 <= max_zone_id, "room for '_NN' parts" );
    std::vector<std::string> out;
    out.reserve( categories.size() );
    std::set<std::string> used;
    for( const std::string &cat : categories ) {
        std::string base = prefix + id_part( cat, max_hall_id_base - prefix.size() );
        if( !used.insert( base ).second ) {
            // Taken: the first free "-k", the base cut to make room (ends in a digit, so no base made from a
            // category id, which has no '-', can match it later).
            for( size_t k = 2; ; k++ ) {
                const std::string suffix = "-" + std::to_string( k );
                std::string candidate = base.substr( 0, max_hall_id_base - suffix.size() ) + suffix;
                if( used.insert( candidate ).second ) {
                    base = std::move( candidate );
                    break;
                }
            }
        }
        out.push_back( std::move( base ) );
    }
    return out;
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

namespace
{
// The armory's and halls' item squares: rows y % 3 != 0 (aisles between pairs of rows, the centre row among them),
// columns x % 14 != 0 (cross aisles, the centre column among them), within x -56..55, y -58..58. The rows nearest
// the centre are used, read top-down and left to right; each group starts a new row. At most what fits.
std::vector<range_offset> grid_slots( const std::vector<size_t> &groups )
{
    const auto mod = []( int v, int m ) {
        return ( ( v % m ) + m ) % m;
    };
    std::vector<int> columns;
    for( int x = -56; x <= 55; x++ ) {
        if( mod( x, 14 ) != 0 ) {
            columns.push_back( x );
        }
    }
    std::vector<int> rows;
    for( int y = -58; y <= 58; y++ ) {
        if( mod( y, 3 ) != 0 ) {
            rows.push_back( y );
        }
    }
    size_t rows_needed = 0;
    for( const size_t n : groups ) {
        rows_needed += ( n + columns.size() - 1 ) / columns.size();
    }
    std::stable_sort( rows.begin(), rows.end(), []( int a, int b ) {
        return std::abs( a ) < std::abs( b );
    } );
    rows.resize( std::min( rows.size(), rows_needed ) );
    std::sort( rows.begin(), rows.end() );
    std::vector<range_offset> out;
    size_t row = 0;
    for( const size_t n : groups ) {
        size_t col = 0;
        for( size_t i = 0; i < n && row < rows.size(); i++ ) {
            out.push_back( { columns[col], rows[row], 0 } );
            if( ++col == columns.size() ) {
                col = 0;
                row++;
            }
        }
        if( col > 0 ) {
            row++;
        }
    }
    return out;
}

// Lot bays, shelf-packed from the north-west corner: each bay is the vehicle's mount rectangle with a painted border
// (bays side by side share it), a free row between rows of bays, the centre's 5x5 kept free.
void lot_layout( const std::vector<vproto_id> &list, std::vector<zone_piece> &out )
{
    constexpr int lo = -58;
    constexpr int hi = 57;
    constexpr int plaza = 2;
    int x = lo;
    int y = lo;
    int row_h = 0;
    for( const vproto_id &id : list ) {
        if( !id.is_valid() || id->parts.empty() ) {
            continue;
        }
        int min_x = INT_MAX;
        int min_y = INT_MAX;
        int max_x = INT_MIN;
        int max_y = INT_MIN;
        for( const vehicle_prototype::part_def &pt : id->parts ) {
            min_x = std::min( min_x, pt.pos.x() );
            min_y = std::min( min_y, pt.pos.y() );
            max_x = std::max( max_x, pt.pos.x() );
            max_y = std::max( max_y, pt.pos.y() );
        }
        // With the border; a prototype's parts are close to its origin, but its data is a mod's: bounded.
        const int64_t w64 = int64_t{ max_x } - min_x + 3;
        const int64_t h64 = int64_t{ max_y } - min_y + 3;
        if( w64 > hi - lo + 1 || h64 > hi - lo + 1 ) {
            continue;
        }
        const int w = static_cast<int>( w64 );
        const int h = static_cast<int>( h64 );
        while( true ) {
            if( x + w - 1 > hi ) {
                x = lo;
                y += row_h + 1;
                row_h = 0;
            }
            if( y + h - 1 > hi ) {
                break;
            }
            if( x <= plaza && x + w - 1 >= -plaza && y <= plaza && y + h - 1 >= -plaza ) {
                x = plaza + 1;
                continue;
            }
            zone_piece piece;
            piece.id = id.str();
            piece.min = { x + 1, y + 1, 0 };
            piece.max = { x + w - 2, y + h - 2, 0 };
            piece.at = { x + 1 - min_x, y + 1 - min_y, 0 };
            out.push_back( std::move( piece ) );
            x += w - 1;
            row_h = std::max( row_h, h );
            break;
        }
    }
}

// Pen cells in strips south of the centre: a glass wall row, 3 rows inside, a glass wall row, then 2 free rows;
// cells side by side share their glass walls.
void pen_layout( const std::vector<mtype_id> &list, std::vector<zone_piece> &out )
{
    constexpr int lo_x = -56;
    constexpr int hi_x = 55;
    constexpr int hi_y = 58;
    int x = lo_x;
    int y = 2;
    for( const mtype_id &id : list ) {
        if( !id.is_valid() ) {
            continue;
        }
        const mtype &t = *id;
        const int w = t.size >= creature_size::huge ? 3 : t.size >= creature_size::large ? 2 : 1;
        if( x + w + 1 > hi_x ) {
            x = lo_x;
            y += 7;
        }
        if( y + 4 > hi_y ) {
            break;
        }
        zone_piece piece;
        piece.id = id.str();
        piece.min = { x + 1, y + 1, 0 };
        piece.max = { x + w, y + 3, 0 };
        piece.at = { x + 1 + ( w - 1 ) / 2, y + 2, 0 };
        piece.what = t.has_flag( mon_flag_AQUATIC ) ? "water" : t.has_flag( mon_flag_DIGS ) ? "dirt" :
                     t.has_flag( mon_flag_SUNDEATH ) ? "shade" : "";
        out.push_back( std::move( piece ) );
        x += w + 1;
    }
}
} // namespace

std::vector<zone_piece> zone_pieces( size_t zone )
{
    std::vector<zone_piece> out;
    const std::vector<range_zone> &zones = range_zones();
    if( zone >= zones.size() ) {
        return out;
    }
    const registry &r = reg();
    const range_zone &z = zones[zone];
    // The zone's slice of a list (clamped: the zone came from the same data, but stay inside).
    const auto slice = [&z]( const auto & list ) {
        const size_t first = std::min( z.first, list.size() );
        const size_t end = std::min( list.size(), first + std::min( z.count, list.size() - first ) );
        return std::make_pair( first, end );
    };
    switch( z.kind ) {
        case zone_kind::armory: {
            const auto [first, end] = slice( r.armory );
            std::vector<size_t> groups( 3, 0 );
            for( size_t i = first; i < end; i++ ) {
                groups[i < r.armory_guns ? 0 : i < r.armory_guns + r.armory_melee ? 1 : 2]++;
            }
            const std::vector<range_offset> slots = grid_slots( groups );
            for( size_t i = first; i < end && i - first < slots.size(); i++ ) {
                const range_offset &o = slots[i - first];
                const char *what = i < r.armory_guns ? "gun" : i < r.armory_guns + r.armory_melee ? "melee" : "thrown";
                out.push_back( { r.armory[i].str(), o, o, o, what } );
            }
            break;
        }
        case zone_kind::halls: {
            const auto it = r.halls.find( z.category );
            if( it == r.halls.end() ) {
                break;
            }
            const auto [first, end] = slice( it->second );
            const std::vector<range_offset> slots = grid_slots( { end - first } );
            for( size_t i = first; i < end && i - first < slots.size(); i++ ) {
                const range_offset &o = slots[i - first];
                out.push_back( { it->second[i].str(), o, o, o, "" } );
            }
            break;
        }
        case zone_kind::lots: {
            const auto [first, end] = slice( r.vehicles );
            lot_layout( std::vector<vproto_id>( r.vehicles.begin() + static_cast<std::ptrdiff_t>( first ),
                                                r.vehicles.begin() + static_cast<std::ptrdiff_t>( end ) ), out );
            break;
        }
        case zone_kind::pens: {
            const auto [first, end] = slice( r.monsters );
            pen_layout( std::vector<mtype_id>( r.monsters.begin() + static_cast<std::ptrdiff_t>( first ),
                                               r.monsters.begin() + static_cast<std::ptrdiff_t>( end ) ), out );
            break;
        }
        case zone_kind::hub:
        case zone_kind::gallery:
        case zone_kind::lanes:
        case zone_kind::kitchen:
            break;
    }
    return out;
}

const std::vector<lane_target> &lane_targets()
{
    // Eight lanes 4 squares apart (none on the centre column, where the player arrives): zombies at 5, 10, 25 and
    // 50 squares, then practice targets at the same distances.
    static const std::vector<lane_target> lanes = [] {
        std::vector<lane_target> v;
        const int distances[] = { 5, 10, 25, 50 };
        for( int i = 0; i < 8; i++ )
        {
            const int x = -14 + 4 * i;
            const int d = distances[i % 4];
            const bool monster = i < 4;
            v.push_back( { { x, 0, 0 }, { x, -d, 0 }, d, monster, monster ? "mon_zombie" : "f_target" } );
        }
        return v;
    }();
    return lanes;
}

const std::vector<hub_prop> &kitchen_props()
{
    // Along a counter row north of the centre: two lit wood stoves, an oven, a fridge with food, a sink with water,
    // counters with cookware, food and water; a lit fireplace and brazier to the south, apart.
    static const std::vector<hub_prop> props = {
        { "wood stove", { -6, -5, 0 }, prop_kind::furniture, "f_woodstove" },
        { "firewood in the wood stove", { -6, -5, 0 }, prop_kind::item, "2x4", 6 },
        { "fire in the wood stove", { -6, -5, 0 }, prop_kind::field, "fd_fire", 1 },
        { "second wood stove", { -4, -5, 0 }, prop_kind::furniture, "f_woodstove" },
        { "firewood in the second stove", { -4, -5, 0 }, prop_kind::item, "2x4", 6 },
        { "fire in the second stove", { -4, -5, 0 }, prop_kind::field, "fd_fire", 1 },
        { "oven", { -2, -5, 0 }, prop_kind::furniture, "f_oven" },
        { "refrigerator", { 0, -5, 0 }, prop_kind::furniture, "f_fridge" },
        { "jug of milk in the fridge", { 0, -5, 0 }, prop_kind::item, "jug_plastic", 1, "milk", 8 },
        { "cheese in the fridge", { 0, -5, 0 }, prop_kind::item, "cheese", 2 },
        { "raw meat in the fridge", { 0, -5, 0 }, prop_kind::item, "meat", 2 },
        { "eggs in the fridge", { 0, -5, 0 }, prop_kind::item, "egg_chicken", 6 },
        { "sink", { 2, -5, 0 }, prop_kind::furniture, "f_sink" },
        { "bucket of clean water in the sink", { 2, -5, 0 }, prop_kind::item, "bucket", 1, "water_clean", 20 },
        { "counter by the sink", { 4, -5, 0 }, prop_kind::furniture, "f_counter" },
        { "cooking pot", { 4, -5, 0 }, prop_kind::item, "pot" },
        { "frying pan", { 4, -5, 0 }, prop_kind::item, "pan" },
        { "counter", { 5, -5, 0 }, prop_kind::furniture, "f_counter" },
        { "kettle", { 5, -5, 0 }, prop_kind::item, "kettle" },
        { "large kitchen knife", { 5, -5, 0 }, prop_kind::item, "knife_huge" },
        { "spatula", { 5, -5, 0 }, prop_kind::item, "spatula" },
        { "ladle", { 5, -5, 0 }, prop_kind::item, "ladle" },
        { "third counter", { 6, -5, 0 }, prop_kind::furniture, "f_counter" },
        { "bread", { 6, -5, 0 }, prop_kind::item, "bread", 2 },
        { "apples", { 6, -5, 0 }, prop_kind::item, "apple", 3 },
        { "fourth counter", { 7, -5, 0 }, prop_kind::furniture, "f_counter" },
        { "jug of clean water", { 7, -5, 0 }, prop_kind::item, "jug_plastic", 1, "water_clean", 15 },
        { "bottle of clean water", { 7, -5, 0 }, prop_kind::item, "bottle_plastic", 1, "water_clean", 2 },
        { "fifth counter", { 8, -5, 0 }, prop_kind::furniture, "f_counter" },
        { "empty jerrycan", { 8, -5, 0 }, prop_kind::item, "jerrycan" },
        { "empty plastic bottles", { 8, -5, 0 }, prop_kind::item, "bottle_plastic", 2 },
        { "fireplace", { -8, 4, 0 }, prop_kind::furniture, "f_fireplace" },
        { "firewood in the fireplace", { -8, 4, 0 }, prop_kind::item, "2x4", 8 },
        { "fire in the fireplace", { -8, 4, 0 }, prop_kind::field, "fd_fire", 1 },
        { "brazier", { 8, 4, 0 }, prop_kind::furniture, "f_brazier" },
        { "firewood in the brazier", { 8, 4, 0 }, prop_kind::item, "2x4", 6 },
        { "fire in the brazier", { 8, 4, 0 }, prop_kind::field, "fd_fire", 1 },
    };
    return props;
}

void boot_test_range( unsigned seed )
{
    if( g == nullptr || world_generator->active_world == nullptr ) {
        throw std::runtime_error( "test range: load the game data first (boot_new_game_data)" );
    }
    // The range changes the world's options and clears land: only in the throwaway world boot made for it.
    const std::string &world = world_generator->active_world->world_name;
    if( !range_world( world ) ) {
        throw std::runtime_error( "test range: needs its own fp3d-range world (boot_new_game_data with replace)" );
    }
    const auto t0 = std::chrono::steady_clock::now();
    rng_set_engine_seed( seed );
    reset_test_range();
    range_seed = seed;
    session.store( true );
    quiet_world_options();
    calendar::start_of_cataclysm = calendar::turn_zero;
    calendar::start_of_game = calendar::turn_zero + 1_days + 10_hours;
    calendar::turn = calendar::start_of_game;
    calendar::initial_season = SPRING;

    make_avatar();
    avatar &u = get_avatar();
    u.set_value( range_var, 1.0 );
    u.set_value( seed_var, static_cast<double>( seed ) );
    const std::vector<range_zone> &zones = range_zones();
    if( zones.empty() ) {
        throw std::runtime_error( "test range: no zones" );
    }
    const tripoint_abs_ms hub = zone_centre( zones.front() );
    fp3d_game_access::start_game_at( *g, hub );
    build_zone( 0 );
    u.setpos( hub, false );
    settle_avatar();
    keep_range_weather();
    store_frozen( true );
    // As CDDA's debug menu: a game with cheats earns no achievements.
    get_achievements().set_enabled( false );
    std::printf( "range: booted with seed %u, %zu zones, in %.1f s\n", seed, zones.size(),
                 std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count() );
}

bool resume_test_range()
{
    // Whatever the save turns out to be, nothing of an earlier range lingers.
    reset_test_range();
    // A save is hostile: its avatar values alone never make a range game, only in one of FP3D's own range worlds.
    if( g == nullptr || world_generator == nullptr || world_generator->active_world == nullptr ||
        !avatar_flag( range_var ) || !range_world( world_generator->active_world->world_name ) ) {
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    avatar &u = get_avatar();
    range_seed = saved_seed();
    u.set_value( seed_var, static_cast<double>( range_seed ) );
    session.store( true );
    // The same zones from the same data, each zone's built flag from the save: built zones are not built again
    // (what they hold is in the save).
    build_registry();
    store_frozen( saved_frozen() );
    keep_range_weather();
    get_achievements().set_enabled( false );
    size_t built = 0;
    for( const range_zone &z : reg().zones ) {
        built += z.built ? 1 : 0;
    }
    std::printf( "range: resumed a saved range, seed %u, %zu zones (%zu built), AI %s, in %.1f s\n", range_seed,
                 reg().zones.size(), built, ai_frozen() ? "frozen" : "running",
                 std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count() );
    return true;
}

namespace
{
// The operations change the world: only on the sim thread, right before a main-loop (timed) request, never
// in the middle of an action (a popup's or menu's request) and never from another thread.
bool between_turns()
{
    return in_timed_hook();
}
constexpr const char *not_now = "Only between turns.";

// Why an operation can't run now, "" if it can.
std::string op_refusal()
{
    if( !between_turns() ) {
        return not_now;
    }
    if( !range_active() ) {
        return "Only in the test range.";
    }
    if( g->uquit != QUIT_NO || get_avatar().is_dead_state() ) {
        return "Not now.";
    }
    return "";
}

// Whether moving the clock `ahead` would pass the range clock's end, max_range_days after the start of the game.
// In 64 bits: never overflows, whatever the clock says.
bool past_clock_end( const time_duration &ahead )
{
    const int64_t end = int64_t{ to_turn<int>( calendar::start_of_game ) } + int64_t{ max_range_days } *
                        to_turns<int64_t>( 1_days );
    return int64_t{ to_turn<int>( calendar::turn ) } + to_turns<int64_t>( ahead ) > end;
}

std::string clock_end_refusal()
{
    return string_format( "The range clock stops %d days after the start.", max_range_days );
}

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
    if( const std::string why = op_refusal(); !why.empty() ) {
        return why;
    }
    if( count < 1 || count > max_spawn_count ) {
        return string_format( "Count must be 1 to %d.", max_spawn_count );
    }
    const std::string name = id_name( id_kind::itype, itype );
    if( name.empty() ) {
        return "No such item type.";
    }
    const itype_id type( name );
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
    // On that square only (no overflow onto its neighbours): as many as fit.
    item it( type, calendar::turn );
    int placed = 0;
    if( it.count_by_charges() ) {
        it.charges = count;
        placed = here.add_item_or_charges( p, it, false ).is_null() ? 0 : count;
    } else {
        while( placed < count && !here.add_item_or_charges( p, it, false ).is_null() ) {
            placed++;
        }
    }
    if( placed == 0 ) {
        return "Nothing was placed.";
    }
    return "";
}

std::string spawn_monster( const std::string &mtype, const ui_point &square )
{
    if( const std::string why = op_refusal(); !why.empty() ) {
        return why;
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
    // Looked up among the loaded types: an id made from the client's text would stay interned for good.
    mtype_id type = mtype_id::NULL_ID();
    for( const ::mtype &m : MonsterGenerator::generator().get_all_mtypes() ) {
        if( m.id.str() == mtype ) {
            type = m.id;
            break;
        }
    }
    if( type.is_null() ) {
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
    if( const std::string why = op_refusal(); !why.empty() ) {
        return why;
    }
    const std::vector<range_zone> &zones = range_zones();
    if( index >= zones.size() ) {
        return "No such zone.";
    }
    avatar &u = get_avatar();
    // The overmap teleport leaves the mount behind, and the way back below would put rider and mount on one square.
    if( u.is_mounted() ) {
        return "Not while riding.";
    }
    leave_square( u );
    const tripoint_abs_ms back = u.pos_abs();
    const bool was_in_vehicle = u.in_vehicle;
    const tripoint_abs_ms dest = zone_centre( zones[index] );
    load_bubble_at( zones[index].centre, dest );
    if( !zones[index].built ) {
        build_zone( index );
    }
    const std::optional<tripoint_abs_ms> arrive = arrival_square( dest );
    if( !arrive ) {
        // Back where it was (its square was its own: still free), in the vehicle it was in.
        load_bubble_at( project_to<coords::omt>( back ), back );
        map &here = get_map();
        if( was_in_vehicle && here.veh_at( u.pos_bub() ).part_with_feature( "BOARDABLE", true ) ) {
            here.board_vehicle( u.pos_bub(), &u );
        }
        settle_avatar();
        reg().version++;
        return "No free square to arrive on there.";
    }
    u.setpos( *arrive, false );
    settle_avatar();
    reg().version++;
    return "";
}

std::string rebuild_zone( size_t index )
{
    if( const std::string why = op_refusal(); !why.empty() ) {
        return why;
    }
    const std::vector<range_zone> &zones = range_zones();
    if( index >= zones.size() ) {
        return "No such zone.";
    }
    if( current_zone() == static_cast<int>( index ) ) {
        return "Not while you are in that zone.";
    }
    avatar &u = get_avatar();
    // The overmap teleport below would leave the vehicle or the mount behind.
    if( u.in_vehicle || u.is_mounted() ) {
        return "Not while riding or in a vehicle.";
    }
    // The bubble is loaded elsewhere and back: the route and the hauled items' references would point at nothing, and
    // a grabbed thing in the zone is cleared away.
    stop_moving( u );
    const tripoint_abs_ms back = u.pos_abs();
    load_bubble_at( zones[index].centre, zone_centre( zones[index] ) );
    build_zone( index );
    load_bubble_at( project_to<coords::omt>( back ), back );
    settle_avatar();
    reg().version++;
    return "";
}

std::string set_time_of_day( int minute )
{
    if( const std::string why = op_refusal(); !why.empty() ) {
        return why;
    }
    if( minute < 0 || minute > 1439 ) {
        return "The minute of the day must be 0 to 1439.";
    }
    // Ahead of now to that minute: today if still ahead, tomorrow if already past, nothing if now.
    time_duration ahead = time_duration::from_minutes( minute ) - time_past_midnight( calendar::turn );
    if( ahead < 0_turns ) {
        ahead += 1_days;
    }
    if( ahead == 0_turns ) {
        return "";
    }
    if( past_clock_end( ahead ) ) {
        return clock_end_refusal();
    }
    calendar::turn += ahead;
    get_weather().update_weather();
    g->reset_light_level();
    get_map().invalidate_map_cache( get_avatar().posz() );
    return "";
}

std::string advance( int minutes )
{
    if( const std::string why = op_refusal(); !why.empty() ) {
        return why;
    }
    if( minutes < 1 || minutes > max_advance_minutes ) {
        return string_format( "Minutes must be 1 to %d.", max_advance_minutes );
    }
    if( past_clock_end( time_duration::from_minutes( minutes ) ) ) {
        return clock_end_refusal();
    }
    avatar &u = get_avatar();
    if( u.activity ) {
        return "Finish what you are doing first.";
    }
    // As CDDA's wait menu (handle_action.cpp): not while controlling a vehicle that moves, or would.
    if( u.controlling_vehicle ) {
        map &here = get_map();
        if( const optional_vpart_position vp = here.veh_at( u.pos_bub() ) ) {
            const vehicle &veh = vp->vehicle();
            if( !veh.can_use_rails( here ) && ( veh.is_flying_in_air() || veh.is_falling || veh.velocity ||
                                                ( veh.cruise_velocity && ( ( veh.is_watercraft() && veh.can_float( here ) ) ||
                                                        veh.sufficient_wheel_config() ) ) ||
                                                ( veh.is_in_water( true ) && !veh.can_float( here ) ) ) ) {
                return "You can't pass time while controlling a moving vehicle.";
            }
        }
    }
    u.assign_activity( player_activity( act_wait, 100 * to_turns<int>( time_duration::from_minutes( minutes ) ),
                                        0 ) );
    return "";
}

std::string set_ai_frozen( bool on )
{
    if( const std::string why = op_refusal(); !why.empty() ) {
        return why;
    }
    // capture_range_info sees the change through ai_frozen(); saves keep it (resume_test_range).
    store_frozen( on );
    return "";
}

std::string heal()
{
    if( const std::string why = op_refusal(); !why.empty() ) {
        return why;
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
