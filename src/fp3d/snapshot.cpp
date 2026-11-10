#include "fp3d/snapshot.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <ostream>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "avatar.h"
#include "calendar.h"
#include "character_id.h"
#include "coordinates.h"
#include "creature.h"
#include "enums.h"
#include "type_id.h"
#include "field.h"
#include "fp3d/held.h"
#include "fp3d/id_tables.h"
#include "fp3d/item_hands.h"
#include "fp3d/points.h"
#include "game.h"
#include "flag.h"
#include "item.h"
#include "item_location.h"
#include "item_pocket.h"
#include "json.h"
#include "lightmap.h"
#include "map.h"
#include "map_memory.h"
#include "map_scale_constants.h"
#include "mapdata.h"
#include "memory_fast.h"
#include "monster.h"
#include "mtype.h"
#include "npc.h"
#include "trap.h"
#include "units.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "vpart_range.h"
#include "weather.h"

static_assert( SEEX == fp3d::sm_size && SEEY == fp3d::sm_size, "snapshot submaps are 12x12" );

namespace fp3d
{
namespace
{
constexpr size_t max_creatures = 1024;
constexpr size_t max_vehicles = 256;
constexpr size_t max_vehicle_parts = 4096;
constexpr size_t max_fields_per_tile = 4;

// ---- id tables ----
struct id_table {
    std::vector<std::string> names{ "" };
    std::unordered_map<std::string, uint32_t> index;
};
std::mutex ids_mutex;
std::array<id_table, static_cast<size_t>( id_kind::count )> session_tables;

// ---- capture state (sim thread only) ----
using sm_key = std::tuple<int, int, int>;
struct capture_state {
    uint64_t seq = 0;
    std::map<sm_key, std::shared_ptr<const submap_view>> previous;
    struct tracked_monster {
        weak_ptr_fast<monster> ptr;
        uint64_t uid;
    };
    std::unordered_map<const monster *, tracked_monster> monsters;
    std::unordered_map<const vehicle *, uint64_t> vehicles;
    uint64_t next_uid = uint64_t( 2 ) << 32;
};
capture_state state;

std::mutex latest_mutex;
std::shared_ptr<const snapshot> latest;

uint8_t clamp_u8( double v )
{
    return static_cast<uint8_t>( std::clamp( std::lround( v ), 0L, 255L ) );
}

int16_t clamp_i16( int v )
{
    return static_cast<int16_t>( std::clamp( v, -32768, 32767 ) );
}

uint8_t hp_frac( int hp, int hp_max )
{
    return clamp_u8( 255.0 * hp / std::max( 1, hp_max ) );
}

uint32_t wielded_index( const Character &ch )
{
    const item_location wielded = ch.get_wielded_item();
    return wielded ? intern_id( id_kind::itype, wielded->typeId().str() ) : 0;
}

uint64_t monster_uid( const monster &m )
{
    auto it = state.monsters.find( &m );
    if( it != state.monsters.end() ) {
        const shared_ptr_fast<monster> alive = it->second.ptr.lock();
        if( alive.get() == &m ) {
            return it->second.uid;
        }
        state.monsters.erase( it );   // the address now belongs to another monster
    }
    const uint64_t uid = state.next_uid++;
    state.monsters.emplace( &m, capture_state::tracked_monster{ g->shared_from( m ), uid } );
    return uid;
}
} // namespace

uint64_t creature_uid( const Creature &c )
{
    if( c.is_avatar() ) {
        return 1;
    }
    if( const npc *guy = c.as_npc() ) {
        return ( uint64_t( 1 ) << 32 ) | static_cast<uint32_t>( guy->getID().get_value() );
    }
    if( const monster *m = c.as_monster() ) {
        return monster_uid( *m );
    }
    return 0;
}

uint64_t next_snapshot_seq()
{
    return state.seq + 1;
}

namespace
{
uint64_t vehicle_uid( const vehicle *v )
{
    auto it = state.vehicles.find( v );
    if( it != state.vehicles.end() ) {
        return it->second;
    }
    const uint64_t uid = state.next_uid++;
    state.vehicles.emplace( v, uid );
    return uid;
}

// Index into a flat bubble-sized array; z offset so -OVERMAP_DEPTH maps to 0.
size_t bubble_index( const tripoint_bub_ms &p )
{
    constexpr int side = MAPSIZE_X;
    return ( static_cast<size_t>( p.z() + OVERMAP_DEPTH ) * side + p.y() ) * side + p.x();
}

// `items_seen`: CDDA's could_see_items (never inside sealed containers, others only from next to them).
void capture_items_and_fields( map &here, const tripoint_bub_ms &p, uint8_t tile, submap_view &sm,
                               std::unordered_set<uint64_t> &seen, bool items_seen )
{
    map_stack stack = here.i_at( p );
    if( items_seen && !stack.empty() ) {
        std::vector<std::pair<units::volume, const item *>> by_volume;
        for( const item &it : stack ) {
            by_volume.emplace_back( it.volume(), &it );
        }
        const size_t n = std::min<size_t>( 4, by_volume.size() );
        std::partial_sort( by_volume.begin(), by_volume.begin() + n, by_volume.end(),
        []( const auto & a, const auto & b ) {
            return a.first > b.first;
        } );
        item_pile_view pile;
        pile.tile = tile;
        pile.count = static_cast<uint16_t>( std::min<size_t>( stack.size(), 65535 ) );
        for( size_t i = 0; i < n; i++ ) {
            pile.top[i] = intern_id( id_kind::itype, by_volume[i].second->typeId().str() );
        }
        units::volume biggest = -1_ml;
        for( const auto &[vol, it] : by_volume ) {
            if( it->is_corpse() && it->get_mtype() != nullptr && vol > biggest ) {
                biggest = vol;
                pile.corpse = intern_id( id_kind::mtype, it->get_mtype()->id.str() );
                pile.corpse_state = ( it->has_flag( flag_BLED ) ? corpse_bled : 0 ) |
                                    ( it->has_flag( flag_FIELD_DRESS ) ? corpse_field_dressed : 0 ) |
                                    ( it->has_flag( flag_FIELD_DRESS_FAILED ) ? corpse_dress_failed : 0 ) |
                                    ( it->has_flag( flag_SKINNED ) ? corpse_skinned : 0 ) |
                                    ( it->has_flag( flag_QUARTERED ) ? corpse_quartered : 0 ) |
                                    ( it->has_flag( flag_PULPED ) || it->damage() >= it->max_damage() ? corpse_pulped : 0 ) |
                                    ( it->has_flag( flag_GIBBED ) ? corpse_gibbed : 0 );
            }
        }
        // Handles for the client to point at one item (fp3d/item_hands.h).
        for( item &it : stack ) {
            if( pile.handles.size() >= max_handles_per_tile ) {
                break;
            }
            item_handle_view h;
            h.uid = item_handle( it, &seen );
            h.type = intern_id( id_kind::itype, it.typeId().str() );
            uint16_t sx = 0;
            uint16_t sy = 0;
            if( item_spot( it, sx, sy ) ) {
                h.spot_x = static_cast<int16_t>( sx );
                h.spot_y = static_cast<int16_t>( sy );
            }
            pile.handles.push_back( h );
        }
        sm.items.push_back( std::move( pile ) );
    }
    size_t fields_here = 0;
    for( const auto &fd : here.field_at( p ) ) {
        if( fields_here++ >= max_fields_per_tile ) {
            break;
        }
        field_view f;
        f.tile = tile;
        f.type = static_cast<uint32_t>( fd.first.to_i() );
        f.intensity = clamp_u8( fd.second.get_field_intensity() );
        f.age_turns = static_cast<uint16_t>( std::clamp<int64_t>( to_turns<int64_t>
                                             ( fd.second.get_field_age() ), 0, 65535 ) );
        sm.fields.push_back( f );
    }
}

std::shared_ptr<const submap_view> capture_submap( map &here, const avatar &u,
        const visibility_variables &cache, const std::vector<uint8_t> &vehicle_tiles, const point &grid,
        int z, std::unordered_set<uint64_t> &seen )
{
    auto sm = std::make_shared<submap_view>();
    const tripoint_bub_ms corner( grid.x * SEEX, grid.y * SEEY, z );
    sm->pos = to_ui_point( project_to<coords::sm>( here.get_abs( corner ) ) );
    for( int y = 0; y < SEEY; y++ ) {
        for( int x = 0; x < SEEX; x++ ) {
            const int i = y * SEEX + x;
            const tripoint_bub_ms p = corner + tripoint_rel_ms( x, y, 0 );
            sm->ter[i] = static_cast<uint32_t>( here.ter( p ).to_i() );
            sm->furn[i] = static_cast<uint32_t>( here.furn( p ).to_i() );
            sm->light[i] = clamp_u8( here.ambient_light_at( p ) );

            uint8_t vis = vis_never;
            switch( here.get_visibility( here.apparent_light_at( p, cache ), cache ) ) {
                case visibility_type::CLEAR:
                case visibility_type::LIT:
                case visibility_type::BOOMER:
                    vis = vis_seen_lit;
                    break;
                case visibility_type::DARK:
                case visibility_type::BOOMER_DARK:
                    vis = vis_seen_dark;
                    break;
                case visibility_type::HIDDEN:
                    break;
            }
            if( vis == vis_never ) {
                const memorized_tile &mem = u.get_memorized_tile( here.get_abs( p ) );
                if( mem.symbol != 0 || !mem.get_ter_id().empty() || !mem.get_dec_id().empty() ) {
                    vis = vis_remembered;
                }
            }
            sm->vis[i] = vis;

            const trap &tr = here.tr_at( p );
            if( !tr.is_null() && ( u.knows_trap( p ) || ( vis == vis_seen_lit && here.can_see_trap_at( p, u ) ) ) ) {
                sm->trap[i] = static_cast<uint32_t>( tr.loadid.to_i() );
            }

            uint8_t flags = 0;
            if( !here.is_outside( p ) || ( vehicle_tiles[bubble_index( p )] & 2 ) ) {
                flags |= tile_has_roof;
            }
            if( !here.has_floor( p ) ) {
                flags |= tile_open_air_below;
            }
            if( here.has_graffiti_at( p ) ) {
                flags |= tile_has_sign;
            }
            if( vehicle_tiles[bubble_index( p )] & 1 ) {
                flags |= tile_has_vehicle;
            }
            sm->flags[i] = flags;

            // Hidden information stays hidden: piles and fields only where the player looks now.
            if( vis == vis_seen_lit ) {
                capture_items_and_fields( here, p, static_cast<uint8_t>( i ), *sm, seen,
                                          here.could_see_items( p, u.pos_bub() ) );
            }
        }
    }
    return sm;
}

// `loc` and what's inside it, depth first, into `out` (capped).
void list_carried( item_location loc, uint64_t parent, int pocket, uint8_t flags, int depth,
                   const item *offhand, std::unordered_set<uint64_t> &seen, std::vector<carried_view> &out )
{
    if( out.size() >= max_carried_listed || !loc ) {
        return;
    }
    item &it = *loc.get_item();
    carried_view cv;
    cv.uid = item_handle( it, &seen );
    cv.type = intern_id( id_kind::itype, it.typeId().str() );
    cv.parent = parent;
    cv.pocket = static_cast<int16_t>( pocket );
    cv.flags = static_cast<uint8_t>( flags | ( &it == offhand ? carried_offhand : 0 ) );
    out.push_back( cv );
    if( depth + 1 >= max_carried_depth ) {
        return;
    }
    int index = 0;
    for( item_pocket *pk : it.get_all_contained_pockets() ) {
        if( index > max_item_pocket ) {
            break;
        }
        for( item *child : pk->all_items_top() ) {
            list_carried( item_location( loc, child ), cv.uid, index, 0, depth + 1, offhand, seen, out );
        }
        index++;
    }
}

void capture_player( avatar &u, player_view &pv, std::unordered_set<uint64_t> &seen )
{
    pv.uid = 1;
    pv.pos = to_ui_point( u.pos_abs() );
    pv.moves = u.get_moves();
    pv.move_mode = intern_id( id_kind::move_mode, u.move_mode.str() );
    for( const bodypart_id &bp : u.get_all_body_parts( get_body_part_flags::only_main ) ) {
        if( pv.body_parts.size() >= max_body_parts_listed ) {
            break;
        }
        pv.body_parts.push_back( intern_id( id_kind::body_part, bp.id().str() ) );
        pv.hp.push_back( clamp_i16( u.get_part_hp_cur( bp ) ) );
        pv.hp_max.push_back( clamp_i16( u.get_part_hp_max( bp ) ) );
    }
    pv.stamina = u.get_stamina();
    pv.stamina_max = u.get_stamina_max();
    pv.hunger = clamp_i16( u.get_hunger() );
    pv.thirst = clamp_i16( u.get_thirst() );
    pv.sleepiness = clamp_i16( u.get_sleepiness() );
    pv.pain = clamp_i16( u.get_perceived_pain() );
    pv.morale = clamp_i16( u.get_morale_level() );
    pv.wielded = wielded_index( u );
    for( const bodypart_id &bp : u.get_all_body_parts( get_body_part_flags::only_main ) ) {
        const auto eff = [&]( const char *id ) {
            return clamp_u8( u.get_effect_int( efftype_id( id ), bp ) );
        };
        pv.bleed.push_back( eff( "bleed" ) );
        pv.bandaged.push_back( eff( "bandaged" ) );
        pv.disinfected.push_back( eff( "disinfected" ) );
        pv.bite.push_back( eff( "bite" ) > 0 ? 1 : 0 );
        pv.infected.push_back( eff( "infected" ) );
    }
    // CDDA keeps worn items sorted inner to outer; invisible ones (e.g. integrated) are skipped.
    for( const item_location &it : u.get_visible_worn_items() ) {
        if( pv.worn.size() >= 64 ) {
            break;
        }
        worn_view wv;
        wv.type = intern_id( id_kind::itype, it->typeId().str() );
        int layer = 0;
        for( const layer_level l : it->get_layer() ) {
            layer = std::max( layer, static_cast<int>( l ) );
        }
        wv.layer = clamp_u8( layer );
        wv.damage = clamp_u8( std::max( 0, it->damage_level() ) );
        wv.degradation = clamp_u8( 255.0 * std::max( 0, it->degradation() ) / std::max( 1, it->max_damage() ) );
        for( const bodypart_id &bp : u.get_all_body_parts( get_body_part_flags::only_main ) ) {
            if( it->covers( bp ) && wv.covers.size() < max_body_parts_listed ) {
                wv.covers.push_back( intern_id( id_kind::body_part, bp.id().str() ) );
            }
        }
        pv.worn.push_back( std::move( wv ) );
    }
    const item_location held = offhand_item( u );
    const item *offhand = held ? held.get_item() : nullptr;
    if( const item_location weapon = u.get_wielded_item() ) {
        list_carried( weapon, 0, -1, carried_wielded, 0, offhand, seen, pv.items );
    }
    for( const item_location &worn : u.top_items_loc() ) {
        list_carried( worn, 0, -1, carried_worn, 0, offhand, seen, pv.items );
    }
}

void capture_environment( const avatar &u, environment_view &env )
{
    weather_manager &weather = get_weather();
    env.turn_of_day_s = to_seconds<int64_t>( time_past_midnight( calendar::turn ) );
    env.season = static_cast<uint8_t>( season_of_year( calendar::turn ) );
    const std::pair<units::angle, units::angle> sun = sun_azimuth_altitude( calendar::turn );
    env.sun_az_deg = static_cast<float>( units::to_degrees( sun.first ) );
    env.sun_alt_deg = static_cast<float>( units::to_degrees( sun.second ) );
    env.moon_phase = static_cast<uint8_t>( get_moon_phase( calendar::turn ) );
    env.weather = intern_id( id_kind::weather, weather.weather_id.str() );
    env.temperature_c = static_cast<float>( units::to_celsius( weather.get_temperature( u.pos_bub() ) ) );
    env.wind_speed = static_cast<float>( weather.windspeed * 0.44704 );   // mph -> m/s
    env.wind_dir_deg = static_cast<float>( weather.winddirection );
    env.ambient_light = clamp_u8( g->natural_light_level( u.posz() ) );
}

// vehicle_tiles: bit0 a part is here, bit1 the part is inside the vehicle (roofed).
void capture_vehicles( map &here, snapshot &s, std::vector<uint8_t> &vehicle_tiles )
{
    std::set<const vehicle *> present;
    for( const wrapped_vehicle &wv : here.get_vehicles() ) {
        vehicle *v = wv.v;
        present.insert( v );
        // CDDA computes "inside" lazily (only enclosed_at() does), so the outside cache can lack
        // vehicle roofs; refresh it and set the roof flag from the parts directly.
        v->refresh_insides();
        for( const vpart_reference &vp : v->get_all_parts() ) {
            const tripoint_bub_ms p = vp.pos_bub( here );
            if( here.inbounds( p ) ) {
                vehicle_tiles[bubble_index( p )] |= 1 | ( vp.is_inside() ? 2 : 0 );
            }
        }
        if( s.vehicles.size() >= max_vehicles ) {
            continue;
        }
        vehicle_view vv;
        vv.uid = vehicle_uid( v );
        vv.pos = to_ui_point( here.get_abs( wv.pos ) );
        vv.face_deg = static_cast<float>( units::to_degrees( v->face.dir() ) );
        vv.turn_deg = static_cast<float>( units::to_degrees( v->turn_dir ) );
        vv.velocity = static_cast<float>( v->velocity / 100.0 * 0.44704 );   // vmiph (mph * 100) -> m/s
        vv.name = v->name;
        for( const vpart_reference &vp : v->get_all_parts() ) {
            if( vv.parts.size() >= max_vehicle_parts ) {
                break;
            }
            const vehicle_part &part = vp.part();
            vehicle_part_view pv;
            pv.mount_x = part.mount.x();
            pv.mount_y = part.mount.y();
            pv.vpart = intern_id( id_kind::vpart, vp.info().id.str() );
            pv.hp_frac = clamp_u8( 255.0 * part.health_percent() );
            pv.state = ( part.open ? 1 : 0 ) | ( part.is_broken() ? 2 : 0 ) | ( part.enabled ? 4 : 0 );
            vv.parts.push_back( pv );
        }
        s.vehicles.push_back( std::move( vv ) );
    }
    for( auto it = state.vehicles.begin(); it != state.vehicles.end(); ) {
        it = present.count( it->first ) ? std::next( it ) : state.vehicles.erase( it );
    }
}

// Creature::Attitude as the contract's 0 hostile, 1 neutral, 2 friendly (ANY counts as neutral).
uint8_t attitude_index( Creature::Attitude a )
{
    switch( a ) {
        case Creature::Attitude::HOSTILE:
            return 0;
        case Creature::Attitude::FRIENDLY:
            return 2;
        case Creature::Attitude::NEUTRAL:
        case Creature::Attitude::ANY:
            break;
    }
    return 1;
}

void capture_creatures( map &here, const avatar &u, snapshot &s )
{
    std::set<const monster *> seen_monsters;
    std::vector<const Creature *> list{ &u };
    for( const Creature *c : u.get_visible_creatures( MAX_VIEW_DISTANCE ) ) {
        list.push_back( c );
    }
    for( const Creature *c : list ) {
        if( s.creatures.size() >= max_creatures ) {
            break;
        }
        creature_view cv;
        cv.uid = creature_uid( *c );
        cv.pos = to_ui_point( here.get_abs( c->pos_bub( here ) ) );
        cv.hp_frac = hp_frac( c->get_hp(), c->get_hp_max() );
        cv.attitude = c->is_avatar() ? 1 : attitude_index( c->attitude_to( u ) );
        if( const monster *m = c->as_monster() ) {
            seen_monsters.insert( m );
            cv.kind = 2;
            cv.type = intern_id( id_kind::mtype, m->type->id.str() );
        } else if( const Character *ch = c->as_character() ) {
            cv.kind = c->is_avatar() ? 0 : 1;
            cv.move_mode = intern_id( id_kind::move_mode, ch->move_mode.str() );
            cv.wielded = wielded_index( *ch );
        }
        s.creatures.push_back( cv );
    }
    // Forget monsters that left the view; seen again later they keep their uid only while tracked.
    for( auto it = state.monsters.begin(); it != state.monsters.end(); ) {
        it = seen_monsters.count( it->first ) ? std::next( it ) : state.monsters.erase( it );
    }
}

void write_point( JsonOut &jo, const ui_point &p )
{
    jo.start_array();
    jo.write( p.x );
    jo.write( p.y );
    jo.write( p.z );
    jo.end_array();
}

template<typename Array>
void write_array( JsonOut &jo, const std::string &name, const Array &a )
{
    jo.member( name );
    jo.start_array();
    for( const auto &v : a ) {
        jo.write( static_cast<int64_t>( v ) );
    }
    jo.end_array();
}
} // namespace

bool item_pile_view::operator==( const item_pile_view &o ) const
{
    return tile == o.tile && count == o.count && top == o.top && corpse == o.corpse && corpse_state == o.corpse_state &&
           handles == o.handles;
}

bool item_handle_view::operator==( const item_handle_view &o ) const
{
    return uid == o.uid && type == o.type && spot_x == o.spot_x && spot_y == o.spot_y;
}

bool field_view::operator==( const field_view &o ) const
{
    return tile == o.tile && type == o.type && intensity == o.intensity && age_turns == o.age_turns;
}

bool submap_view::same_shape( const submap_view &o ) const
{
    return ter == o.ter && furn == o.furn && trap == o.trap && flags == o.flags;
}

bool submap_view::same_detail( const submap_view &o ) const
{
    return light == o.light && vis == o.vis && items == o.items && fields == o.fields;
}

uint32_t intern_id( id_kind kind, const std::string &id )
{
    if( id.empty() ) {
        return 0;
    }
    std::lock_guard<std::mutex> lk( ids_mutex );
    id_table &t = session_tables[static_cast<size_t>( kind )];
    const auto it = t.index.find( id );
    if( it != t.index.end() ) {
        return it->second;
    }
    const uint32_t i = static_cast<uint32_t>( t.names.size() );
    t.names.push_back( id );
    t.index.emplace( id, i );
    return i;
}

std::vector<std::string> id_names( id_kind kind )
{
    std::lock_guard<std::mutex> lk( ids_mutex );
    return session_tables[static_cast<size_t>( kind )].names;
}

void reset_id_names()
{
    std::lock_guard<std::mutex> lk( ids_mutex );
    for( id_table &t : session_tables ) {
        t = id_table();
    }
}

std::string cdda_id_name( const std::string &table, uint32_t index )
{
    const int i = static_cast<int>( index );
    if( table == "ter" && i < static_cast<int>( ter_t::count() ) ) {
        return ter_id( i ).id().str();
    }
    if( table == "furn" && i < static_cast<int>( furn_t::count() ) ) {
        return furn_id( i ).id().str();
    }
    if( table == "trap" && i < static_cast<int>( trap::count() ) ) {
        return trap_id( i ).id().str();
    }
    if( table == "field" && i < static_cast<int>( field_types::get_all().size() ) ) {
        return field_type_id( i ).id().str();
    }
    return "";
}

std::shared_ptr<const snapshot> capture_snapshot()
{
    map &here = get_map();
    avatar &u = get_avatar();
    auto s = std::make_shared<snapshot>();
    s->turn = to_turns<int64_t>( calendar::turn - calendar::turn_zero );
    s->seq = ++state.seq;
    if( const std::shared_ptr<const id_tables> tables = latest_id_tables() ) {
        s->ids_generation = tables->generation;
    }
    const tripoint_abs_sm origin = here.get_abs_sub();
    s->bubble_origin = ui_point{ origin.x(), origin.y(), 0 };
    // Uids listed in this capture: a copy carrying an earlier item's uid (a split stack) gets its own.
    std::unordered_set<uint64_t> seen;
    capture_player( u, s->player, seen );
    capture_environment( u, s->env );

    std::vector<uint8_t> vehicle_tiles( static_cast<size_t>( MAPSIZE_X ) * MAPSIZE_Y * OVERMAP_LAYERS, 0 );
    capture_vehicles( here, *s, vehicle_tiles );
    capture_creatures( here, u, *s );

    here.update_visibility_cache( u.posz() );
    const visibility_variables &cache = here.get_visibility_variables_cache();
    std::map<sm_key, std::shared_ptr<const submap_view>> current;
    s->submaps.reserve( static_cast<size_t>( MAPSIZE ) * MAPSIZE * OVERMAP_LAYERS );
    for( int z = -OVERMAP_DEPTH; z <= OVERMAP_HEIGHT; z++ ) {
        for( int gy = 0; gy < MAPSIZE; gy++ ) {
            for( int gx = 0; gx < MAPSIZE; gx++ ) {
                std::shared_ptr<const submap_view> fresh = capture_submap( here, u, cache, vehicle_tiles,
                        point( gx, gy ), z, seen );
                const sm_key key{ fresh->pos.x, fresh->pos.y, fresh->pos.z };
                const auto prev = state.previous.find( key );
                if( prev == state.previous.end() ) {
                    auto first = std::const_pointer_cast<submap_view>( fresh );
                    first->revision = 1;
                    first->detail_revision = 1;
                } else {
                    const bool shape = fresh->same_shape( *prev->second );
                    const bool detail = fresh->same_detail( *prev->second );
                    if( shape && detail ) {
                        fresh = prev->second;
                    } else {
                        auto changed = std::const_pointer_cast<submap_view>( fresh );
                        changed->revision = prev->second->revision + ( shape ? 0 : 1 );
                        changed->detail_revision = prev->second->detail_revision + ( detail ? 0 : 1 );
                    }
                }
                current.emplace( key, fresh );
                s->submaps.push_back( std::move( fresh ) );
            }
        }
    }
    // Submaps that left the bubble are forgotten; if they come back they start over at revision 1.
    state.previous = std::move( current );
    return s;
}

void reset_snapshots()
{
    // seq and uids keep counting, so nothing from before the reset is mistaken for something after it.
    capture_state fresh;
    fresh.seq = state.seq;
    fresh.next_uid = state.next_uid;
    state = std::move( fresh );
}

void publish_snapshot( std::shared_ptr<const snapshot> s )
{
    std::lock_guard<std::mutex> lk( latest_mutex );
    latest = std::move( s );
}

std::shared_ptr<const snapshot> latest_snapshot()
{
    std::lock_guard<std::mutex> lk( latest_mutex );
    return latest;
}

void write_snapshot_json( std::ostream &out, const snapshot &s, int z_min, int z_max )
{
    std::set<uint32_t> used_ter;
    std::set<uint32_t> used_furn;
    std::set<uint32_t> used_trap;
    std::set<uint32_t> used_field;
    JsonOut jo( out );
    jo.start_object();
    jo.member( "contract_version", static_cast<int64_t>( s.contract_version ) );
    jo.member( "turn", s.turn );
    jo.member( "seq", static_cast<int64_t>( s.seq ) );
    jo.member( "bubble_origin" );
    write_point( jo, s.bubble_origin );

    const player_view &pv = s.player;
    jo.member( "player" );
    jo.start_object();
    jo.member( "uid", static_cast<int64_t>( pv.uid ) );
    jo.member( "pos" );
    write_point( jo, pv.pos );
    jo.member( "moves", pv.moves );
    jo.member( "move_mode", static_cast<int64_t>( pv.move_mode ) );
    write_array( jo, "body_parts", pv.body_parts );
    write_array( jo, "hp", pv.hp );
    write_array( jo, "hp_max", pv.hp_max );
    jo.member( "stamina", pv.stamina );
    jo.member( "stamina_max", pv.stamina_max );
    jo.member( "hunger", static_cast<int>( pv.hunger ) );
    jo.member( "thirst", static_cast<int>( pv.thirst ) );
    jo.member( "sleepiness", static_cast<int>( pv.sleepiness ) );
    jo.member( "pain", static_cast<int>( pv.pain ) );
    jo.member( "morale", static_cast<int>( pv.morale ) );
    jo.member( "wielded", static_cast<int64_t>( pv.wielded ) );
    write_array( jo, "bleed", pv.bleed );
    write_array( jo, "bandaged", pv.bandaged );
    write_array( jo, "disinfected", pv.disinfected );
    write_array( jo, "bite", pv.bite );
    write_array( jo, "infected", pv.infected );
    jo.member( "worn" );
    jo.start_array();
    for( const worn_view &w : pv.worn ) {
        jo.start_object();
        jo.member( "type", static_cast<int64_t>( w.type ) );
        jo.member( "layer", static_cast<int>( w.layer ) );
        jo.member( "damage", static_cast<int>( w.damage ) );
        jo.member( "degradation", static_cast<int>( w.degradation ) );
        write_array( jo, "covers", w.covers );
        jo.end_object();
    }
    jo.end_array();
    jo.member( "items" );
    jo.start_array();
    for( const carried_view &c : pv.items ) {
        jo.start_array();
        jo.write( static_cast<int64_t>( c.uid ) );
        jo.write( static_cast<int64_t>( c.type ) );
        jo.write( static_cast<int64_t>( c.parent ) );
        jo.write( static_cast<int>( c.pocket ) );
        jo.write( static_cast<int>( c.flags ) );
        jo.end_array();
    }
    jo.end_array();
    jo.end_object();

    const environment_view &env = s.env;
    jo.member( "env" );
    jo.start_object();
    jo.member( "turn_of_day_s", env.turn_of_day_s );
    jo.member( "season", static_cast<int>( env.season ) );
    jo.member( "sun_alt_deg", env.sun_alt_deg );
    jo.member( "sun_az_deg", env.sun_az_deg );
    jo.member( "moon_phase", static_cast<int>( env.moon_phase ) );
    jo.member( "weather", static_cast<int64_t>( env.weather ) );
    jo.member( "temperature_c", env.temperature_c );
    jo.member( "wind_speed", env.wind_speed );
    jo.member( "wind_dir_deg", env.wind_dir_deg );
    jo.member( "ambient_light", static_cast<int>( env.ambient_light ) );
    jo.end_object();

    jo.member( "submaps" );
    jo.start_array();
    for( const std::shared_ptr<const submap_view> &smp : s.submaps ) {
        const submap_view &sm = *smp;
        if( sm.pos.z < z_min || sm.pos.z > z_max ) {
            continue;
        }
        used_ter.insert( sm.ter.begin(), sm.ter.end() );
        used_furn.insert( sm.furn.begin(), sm.furn.end() );
        used_trap.insert( sm.trap.begin(), sm.trap.end() );
        jo.start_object();
        jo.member( "pos" );
        write_point( jo, sm.pos );
        jo.member( "revision", static_cast<int64_t>( sm.revision ) );
        jo.member( "detail_revision", static_cast<int64_t>( sm.detail_revision ) );
        write_array( jo, "ter", sm.ter );
        write_array( jo, "furn", sm.furn );
        write_array( jo, "trap", sm.trap );
        write_array( jo, "light", sm.light );
        write_array( jo, "vis", sm.vis );
        write_array( jo, "flags", sm.flags );
        jo.member( "items" );
        jo.start_array();
        for( const item_pile_view &p : sm.items ) {
            jo.start_array();
            jo.write( static_cast<int>( p.tile ) );
            jo.write( static_cast<int>( p.count ) );
            jo.start_array();
            for( uint32_t t : p.top ) {
                jo.write( static_cast<int64_t>( t ) );
            }
            jo.end_array();
            if( p.corpse != 0 ) {
                jo.write( static_cast<int64_t>( p.corpse ) );
                jo.write( static_cast<int>( p.corpse_state ) );
            }
            jo.end_array();
        }
        jo.end_array();
        jo.member( "handles" );
        jo.start_array();
        for( const item_pile_view &p : sm.items ) {
            for( const item_handle_view &h : p.handles ) {
                jo.start_array();
                jo.write( static_cast<int>( p.tile ) );
                jo.write( static_cast<int64_t>( h.uid ) );
                jo.write( static_cast<int64_t>( h.type ) );
                jo.write( static_cast<int>( h.spot_x ) );
                jo.write( static_cast<int>( h.spot_y ) );
                jo.end_array();
            }
        }
        jo.end_array();
        jo.member( "fields" );
        jo.start_array();
        for( const field_view &f : sm.fields ) {
            used_field.insert( f.type );
            jo.start_array();
            jo.write( static_cast<int>( f.tile ) );
            jo.write( static_cast<int64_t>( f.type ) );
            jo.write( static_cast<int>( f.intensity ) );
            jo.write( static_cast<int>( f.age_turns ) );
            jo.end_array();
        }
        jo.end_array();
        jo.end_object();
    }
    jo.end_array();

    jo.member( "creatures" );
    jo.start_array();
    for( const creature_view &c : s.creatures ) {
        jo.start_object();
        jo.member( "uid", static_cast<int64_t>( c.uid ) );
        jo.member( "kind", static_cast<int>( c.kind ) );
        jo.member( "type", static_cast<int64_t>( c.type ) );
        jo.member( "pos" );
        write_point( jo, c.pos );
        jo.member( "move_mode", static_cast<int64_t>( c.move_mode ) );
        jo.member( "hp_frac", static_cast<int>( c.hp_frac ) );
        jo.member( "wielded", static_cast<int64_t>( c.wielded ) );
        jo.member( "attitude", static_cast<int>( c.attitude ) );
        jo.end_object();
    }
    jo.end_array();

    jo.member( "vehicles" );
    jo.start_array();
    for( const vehicle_view &v : s.vehicles ) {
        jo.start_object();
        jo.member( "uid", static_cast<int64_t>( v.uid ) );
        jo.member( "name", v.name );
        jo.member( "pos" );
        write_point( jo, v.pos );
        jo.member( "face_deg", v.face_deg );
        jo.member( "turn_deg", v.turn_deg );
        jo.member( "velocity", v.velocity );
        jo.member( "parts" );
        jo.start_array();
        for( const vehicle_part_view &p : v.parts ) {
            jo.start_array();
            jo.write( p.mount_x );
            jo.write( p.mount_y );
            jo.write( static_cast<int64_t>( p.vpart ) );
            jo.write( static_cast<int>( p.hp_frac ) );
            jo.write( static_cast<int>( p.state ) );
            jo.end_array();
        }
        jo.end_array();
        jo.end_object();
    }
    jo.end_array();

    // Names of the CDDA ids used above: {"ter": {"12": "t_floor", ...}, ...}. The session tables and all
    // definitions are in the id tables file (write_id_tables_json) of the same generation.
    jo.member( "ids_generation", static_cast<int64_t>( s.ids_generation ) );
    jo.member( "ids" );
    jo.start_object();
    const std::pair<const char *, const std::set<uint32_t> *> cdda_tables[] = {
        { "ter", &used_ter }, { "furn", &used_furn }, { "trap", &used_trap }, { "field", &used_field }
    };
    for( const auto &[name, used] : cdda_tables ) {
        jo.member( name );
        jo.start_object();
        for( uint32_t i : *used ) {
            jo.member( std::to_string( i ), cdda_id_name( name, i ) );
        }
        jo.end_object();
    }
    jo.end_object();
    jo.end_object();
}

} // namespace fp3d
