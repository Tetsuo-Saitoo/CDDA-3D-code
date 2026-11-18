#include "fp3d/snapshot.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <ostream>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "avatar.h"
#include "bionics.h"
#include "bodypart.h"
#include "calendar.h"
#include "catacharset.h"
#include "character.h"
#include "character_id.h"
#include "coordinates.h"
#include "creature.h"
#include "display.h"
#include "effect.h"
#include "enums.h"
#include "mutation.h"
#include "profession.h"
#include "proficiency.h"
#include "skill.h"
#include "type_id.h"
#include "field.h"
#include "fp3d/bridge.h"
#include "fp3d/cooking.h"
#include "fp3d/held.h"
#include "fp3d/id_tables.h"
#include "fp3d/item_hands.h"
#include "fp3d/points.h"
#include "game.h"
#include "flag.h"
#include "item.h"
#include "item_location.h"
#include "item_pocket.h"
#include "itype.h"
#include "json.h"
#include "level_cache.h"
#include "lightmap.h"
#include "line.h"
#include "map.h"
#include "map_memory.h"
#include "map_scale_constants.h"
#include "mapdata.h"
#include "math_parser_diag_value.h"
#include "memory_fast.h"
#include "monster.h"
#include "mood_face.h"
#include "morale.h"
#include "output.h"
#include "overmapbuffer.h"
#include "mtype.h"
#include "npc.h"
#include "shadowcasting.h"
#include "submap.h"
#include "trap.h"
#include "units.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "vpart_range.h"
#include "translation.h"
#include "weather.h"
#include "weather_type.h"

static_assert( SEEX == fp3d::sm_size && SEEY == fp3d::sm_size, "snapshot submaps are 12x12" );

static const efftype_id effect_sleep( "sleep" );

namespace fp3d
{
namespace
{
constexpr size_t max_creatures = 1024;
// Names come from saves and mods: untrusted text, cleaned and capped like the sheet's.
constexpr size_t max_creature_name_columns = 40;
constexpr size_t max_vehicle_name_columns = 60;
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

// want_snapshot_levels: levels below and above the player's that a capture takes; -1 = all of them.
std::atomic<int> want_below{ -1 };
std::atomic<int> want_above{ -1 };

void widen_levels( std::atomic<int> &side, int want )
{
    int cur = side.load();
    while( ( cur < 0 || want > cur ) && !side.compare_exchange_weak( cur, want ) ) {
    }
}

uint8_t clamp_u8( double v )
{
    return static_cast<uint8_t>( std::clamp( std::lround( v ), 0L, 255L ) );
}

} // namespace

std::string client_text( const std::string &raw, size_t max_columns, size_t max_bytes )
{
    std::string s = sanitize_text( utf8_truncate( remove_color_tags( raw ), max_columns ), max_bytes );
    // The byte cap may have cut a character: drop a last sequence that is not whole.
    size_t lead = s.size();
    while( lead > 0 && s.size() - lead < 3 && ( static_cast<unsigned char>( s[lead - 1] ) & 0xC0 ) == 0x80 ) {
        lead--;
    }
    if( lead > 0 ) {
        const unsigned char c = static_cast<unsigned char>( s[lead - 1] );
        const size_t want = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if( c >= 0x80 && s.size() - ( lead - 1 ) < want ) {
            s.resize( lead - 1 );
        }
    }
    return s;
}

int8_t mood_band( int level )
{
    return level >= 100 ? 3 : level >= 50 ? 2 : level >= 10 ? 1 : level >= -10 ? 0 : level >= -50 ? -1 :
           level >= -100 ? -2 : -3;
}

namespace
{
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
// What this square adds to CDDA's lightmap, as map::build_lightmap gathers it. Nothing when it adds none.
std::optional<light_view> light_source_at( map &here, const tripoint_bub_ms &p, uint8_t tile,
        const ter_t &ter, const furn_t &furn )
{
    light_view best;
    float lum = 0.0f;
    const auto offer = [&]( float l, uint8_t kind ) {
        if( l > lum ) {
            lum = l;
            best.kind = kind;
        }
    };
    offer( static_cast<float>( ter.light_emitted ), light_fixed );
    offer( static_cast<float>( furn.light_emitted ), light_fixed );
    size_t n = 0;
    for( const item &it : here.i_at( p ) ) {
        if( n++ >= 64 ) {
            break;
        }
        float l = 0.0f;
        units::angle width = 0_degrees;
        units::angle dir = 0_degrees;
        if( it.getlight( l, width, dir ) ) {
            offer( l, light_item );
        }
    }
    for( const auto &fd : here.field_at( p ) ) {
        offer( static_cast<float>( fd.second.get_intensity_level().light_emitted ), light_fire );
    }
    if( !( lum >= 0.5f ) ) {
        return std::nullopt;
    }
    best.tile = tile;
    best.luminance = static_cast<uint8_t>( std::min( 255.0f, std::round( lum ) ) );
    return best;
}

// L8 #3: what lies in `it` (a container lying next to the player, handle `container`), one level, into sm.inside;
// sealed and forbidden pockets stay closed. Capped per container and per submap; what's left out is counted.
void capture_inside( item &it, uint64_t container, submap_view &sm, std::unordered_set<uint64_t> &seen )
{
    size_t listed = 0;
    uint32_t left_out = 0;
    int index = 0;
    for( item_pocket *pk : it.get_all_contained_pockets() ) {
        if( index > max_item_pocket ) {
            break;
        }
        if( !pk->sealed() && !pk->is_forbidden() ) {
            for( item *child : pk->all_items_top() ) {
                if( listed >= max_inside_per_container || sm.inside.size() >= max_inside_per_submap ) {
                    left_out++;
                    continue;
                }
                inside_view v;
                v.container = container;
                v.uid = item_handle( *child, &seen );
                v.type = intern_id( id_kind::itype, child->typeId().str() );
                v.pocket = static_cast<int16_t>( index );
                sm.inside.push_back( v );
                listed++;
            }
        }
        index++;
    }
    if( left_out > 0 ) {
        sm.inside_more.emplace_back( container, left_out );
    }
}

// `by_player`: the player's square or one next to it, same level, items reachable there (L8: more handles, and what
// lies in containers).
void capture_items_and_fields( map &here, const tripoint_bub_ms &p, uint8_t tile, submap_view &sm,
                               std::unordered_set<uint64_t> &seen, bool items_seen, bool by_player )
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
        const size_t max_handles = by_player ? max_handles_near_player : max_handles_per_tile;
        for( item &it : stack ) {
            if( pile.handles.size() >= max_handles ) {
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
                uint16_t yaw = 0;
                bool side = false;
                if( item_pose( it, yaw, side ) ) {
                    h.yaw = static_cast<int16_t>( yaw );
                    h.side = side;
                }
                uint8_t shelf = 0;
                if( item_shelf( it, shelf ) ) {
                    h.shelf = static_cast<int8_t>( shelf );
                }
            }
            pile.handles.push_back( h );
            if( by_player ) {
                capture_inside( it, h.uid, sm, seen );
            }
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
    // CDDA's submap is looked up once for its 144 tiles, and the level's light table once. map::ter, furn, tr_at,
    // has_floor, has_graffiti_at and ambient_light_at each find the submap again for every tile, which was half
    // of a capture's time; what they return is worked out here the same way (fp3d-snapshot-test compares every
    // tile of the bubble with the map's own calls). A submap that isn't loaded goes through those calls, as before.
    const submap *const tiles = std::as_const( here ).maptile_at( corner ).wrapped_submap();
    const level_cache &level = here.get_cache_ref( z );
    // map::has_floor: a map without levels, and the lowest level, have a floor everywhere.
    const bool floor_everywhere = !here.supports_zlevels() || z < -OVERMAP_DEPTH + 1 || z > OVERMAP_HEIGHT;
    for( int y = 0; y < SEEY; y++ ) {
        for( int x = 0; x < SEEX; x++ ) {
            const int i = y * SEEX + x;
            const tripoint_bub_ms p = corner + tripoint_rel_ms( x, y, 0 );
            const point_sm_ms in_submap( x, y );
            const ter_id ter = tiles != nullptr ? tiles->get_ter( in_submap ) : here.ter( p );
            const furn_id furn = tiles != nullptr ? tiles->get_furn( in_submap ) : here.furn( p );
            const ter_t &ter_obj = ter.obj();
            const furn_t &furn_obj = furn.obj();
            sm->ter[i] = static_cast<uint32_t>( ter.to_i() );
            sm->furn[i] = static_cast<uint32_t>( furn.to_i() );
            sm->light[i] = clamp_u8( level.lm[p.x()][p.y()].max() );

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

            // map::tr_at: the terrain's built-in trap, else the one set on the square.
            const trap_id trap_here = tiles == nullptr ? here.tr_at( p ).loadid :
                                      ter_obj.trap != tr_null ? ter_obj.trap : tiles->get_trap( in_submap );
            if( trap_here != tr_null &&
                ( u.knows_trap( p ) || ( vis == vis_seen_lit && here.can_see_trap_at( p, u ) ) ) ) {
                sm->trap[i] = static_cast<uint32_t>( trap_here.to_i() );
            }

            uint8_t flags = 0;
            if( !here.is_outside( p ) || ( vehicle_tiles[bubble_index( p )] & 2 ) ) {
                flags |= tile_has_roof;
            }
            // map::has_floor: no NO_FLOOR or NO_FLOOR_WATER flag on the terrain or the furniture.
            if( !floor_everywhere && ( ter_obj.has_flag( ter_furn_flag::TFLAG_NO_FLOOR ) ||
                                       furn_obj.has_flag( ter_furn_flag::TFLAG_NO_FLOOR ) ||
                                       ter_obj.has_flag( ter_furn_flag::TFLAG_NO_FLOOR_WATER ) ||
                                       furn_obj.has_flag( ter_furn_flag::TFLAG_NO_FLOOR_WATER ) ) ) {
                flags |= tile_open_air_below;
            }
            if( tiles != nullptr ? tiles->has_graffiti( in_submap ) : here.has_graffiti_at( p ) ) {
                flags |= tile_has_sign;
            }
            if( vehicle_tiles[bubble_index( p )] & 1 ) {
                flags |= tile_has_vehicle;
            }
            sm->flags[i] = flags;

            // Hidden information stays hidden: piles and fields only where the player looks now.
            if( vis == vis_seen_lit ) {
                const bool by_player = p.z() == u.posz() && square_dist( u.pos_bub(), p ) <= 1 &&
                                  here.accessible_items( p );
                capture_items_and_fields( here, p, static_cast<uint8_t>( i ), *sm, seen,
                                          here.could_see_items( p, u.pos_bub() ), by_player );
                if( const std::optional<light_view> l = light_source_at( here, p, static_cast<uint8_t>( i ),
                        ter_obj, furn_obj ) ) {
                    sm->lights.push_back( *l );
                }
            }
        }
    }
    // C6: brightest first, capped (ties by tile, so the order is stable).
    std::sort( sm->lights.begin(), sm->lights.end(), []( const light_view & a, const light_view & b ) {
        return a.luminance != b.luminance ? a.luminance > b.luminance : a.tile < b.tile;
    } );
    if( sm->lights.size() > max_lights_per_submap ) {
        sm->lights.resize( max_lights_per_submap );
    }
    // L9: heat sources and the cookware on them, after the piles gave their items handles.
    capture_heat_and_cookware( here, u, corner, *sm, seen );
    return sm;
}

// A gun's fouling as CDDA keeps it (item var `dirt`), 0..10000; the var comes from a save: anything else is 0.
int16_t gun_dirt( const item &gun )
{
    const diag_value *v = gun.maybe_get_value( "dirt" );
    const double dirt = v != nullptr && v->is_dbl() ? v->dbl() : 0.0;
    return static_cast<int16_t>( std::isfinite( dirt ) ? std::clamp( dirt, 0.0, 10000.0 ) : 0.0 );
}

// What `it` shows of its state (live item tiles) into `out`; nothing is added when it shows nothing or the list is
// full.
void add_item_state( const item &it, uint64_t uid, std::vector<item_state_view> &out )
{
    if( uid == 0 || out.size() >= max_item_states ) {
        return;
    }
    const auto rot_of = []( const item & food ) {
        const double r = food.get_relative_rot();
        return static_cast<int16_t>( std::isfinite( r ) ? std::clamp( r * 1000.0, 0.0, 2000.0 ) : 2000.0 );
    };
    item_state_view st;
    st.uid = uid;
    st.temp = it.has_own_flag( flag_FROZEN ) ? 3 : it.has_own_flag( flag_COLD ) ? 2 :
              it.has_own_flag( flag_HOT ) ? 1 : 0;
    if( it.goes_bad() ) {
        st.rot = rot_of( it );
    }
    bool has_pocket = false;
    int64_t cap_ml = 0;
    int64_t held_ml = 0;
    units::volume biggest = -1_ml;
    size_t looked = 0;
    for( const item_pocket *pk : it.get_all_contained_pockets() ) {
        has_pocket = true;
        cap_ml += std::max<int64_t>( 0, units::to_milliliter( pk->volume_capacity() ) );
        held_ml += std::max<int64_t>( 0, units::to_milliliter( pk->contains_volume() ) );
        // The pocket's material (glass, clear plastic), not CDDA's transparent(), which also counts any open
        // container.
        const pocket_data *pd = pk->get_pocket_data();
        st.see_through = st.see_through || ( pd != nullptr && pd->transparent );
        for( const item *c : pk->all_items_top() ) {
            if( looked++ >= max_state_contents ) {
                break;
            }
            if( c->goes_bad() ) {
                st.rot = std::max( st.rot, rot_of( *c ) );
            }
            const units::volume v = c->volume();
            if( v > biggest ) {
                biggest = v;
                st.fill_type = intern_id( id_kind::itype, c->typeId().str() );
            }
        }
    }
    if( has_pocket ) {
        st.fill_ml = static_cast<int32_t>( std::min<int64_t>( held_ml, INT32_MAX ) );
        st.fill = cap_ml > 0 ? static_cast<int16_t>( std::clamp<int64_t>( held_ml * 1000 / cap_ml, 0, 1000 ) ) : 0;
    }
    if( it.is_gun() && !it.has_flag( flag_NON_FOULING ) ) {
        st.dirt = gun_dirt( it );
    }
    if( st.temp != 0 || st.rot >= 0 || st.fill >= 0 || st.dirt >= 0 ) {
        out.push_back( st );
    }
}

// The item's exact numbers (G2) into `out`; nothing is added when the list is full.
void add_item_amount( const item &it, uint64_t uid, std::vector<item_amount_view> &out )
{
    if( uid == 0 || out.size() >= max_item_amounts ) {
        return;
    }
    const auto clamped = []( int64_t v ) {
        return static_cast<int32_t>( std::clamp<int64_t>( v, 0, max_item_amount ) );
    };
    item_amount_view av;
    av.uid = uid;
    av.count = it.count_by_charges() ? std::max( it.charges, 1 ) : 1;
    av.weight_g = clamped( units::to_gram( it.weight() ) );
    av.volume_ml = clamped( units::to_milliliter( it.volume() ) );
    out.push_back( av );
}

// The states and the exact numbers of the items lying on the avatar's square and the eight around it, and of what
// is directly inside them: those this capture listed handles for (`seen`), after the carried ones.
void capture_near_item_states( map &here, const avatar &u, const std::unordered_set<uint64_t> &seen,
                               std::vector<item_state_view> &out, std::vector<item_amount_view> &amounts )
{
    for( const tripoint_bub_ms &p : here.points_in_radius( u.pos_bub(), 1 ) ) {
        size_t listed = 0;
        for( const item &it : here.i_at( p ) ) {
            if( listed++ >= max_handles_near_player || out.size() >= max_item_states ) {
                break;
            }
            const uint64_t uid = read_item_handle( it );
            if( uid == 0 || seen.count( uid ) == 0 ) {
                continue;
            }
            add_item_state( it, uid, out );
            add_item_amount( it, uid, amounts );
            size_t inside = 0;
            for( const item *c : it.all_items_top( pocket_type::CONTAINER ) ) {
                if( inside++ >= max_inside_per_container ) {
                    break;
                }
                const uint64_t cuid = read_item_handle( *c );
                if( cuid != 0 && seen.count( cuid ) != 0 ) {
                    add_item_state( *c, cuid, out );
                    add_item_amount( *c, cuid, amounts );
                }
            }
        }
    }
}

// `loc` and what's inside it, depth first, into `out` (capped), with what each shows of its state into `states`.
void list_carried( item_location loc, uint64_t parent, int pocket, uint8_t flags, int depth,
                   const item *offhand, std::unordered_set<uint64_t> &seen, std::vector<carried_view> &out,
                   std::vector<item_state_view> &states, std::vector<item_amount_view> &amounts )
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
    add_item_state( it, cv.uid, states );
    add_item_amount( it, cv.uid, amounts );
    if( depth + 1 >= max_carried_depth ) {
        return;
    }
    int index = 0;
    for( item_pocket *pk : it.get_all_contained_pockets() ) {
        if( index > max_item_pocket ) {
            break;
        }
        for( item *child : pk->all_items_top() ) {
            list_carried( item_location( loc, child ), cv.uid, index, 0, depth + 1, offhand, seen, out, states,
                          amounts );
        }
        index++;
    }
}

// CDDA's morale screen: the level and the strongest entries. Names come from game data (mods, item names) and are
// cut to max_mood_name_columns; an entry whose name is empty after that is still counted in `total`.
void capture_mood( const avatar &u, mood_view &mood )
{
    const int level = u.get_morale_level();
    mood.level = clamp_i16( level );
    mood.band = mood_band( level );
    // CDDA's face lookup raises a debug message for a face with no steps (a mod's): no face then.
    const mood_face_id &face = u.character_mood_face();
    if( face.is_valid() && !face->values().empty() ) {
        mood.face = client_text( display::morale_face_color( u ).first, max_mood_face_bytes, max_mood_face_bytes );
    }
    std::vector<std::pair<std::string, int>> all = u.morale->fp3d_entries();
    mood.total = static_cast<uint16_t>( std::min<size_t>( all.size(), UINT16_MAX ) );
    // The morale screen's share: an entry's square over the sum of squares on its side (good or bad).
    double good = 0;
    double bad = 0;
    for( const std::pair<std::string, int> &e : all ) {
        const double sq = static_cast<double>( e.second ) * e.second;
        ( e.second > 0 ? good : bad ) += sq;
    }
    std::sort( all.begin(), all.end(), []( const std::pair<std::string, int> &a,
    const std::pair<std::string, int> &b ) {
        const int64_t ma = std::abs( static_cast<int64_t>( a.second ) );
        const int64_t mb = std::abs( static_cast<int64_t>( b.second ) );
        return ma != mb ? ma > mb : a.first < b.first;
    } );
    for( const std::pair<std::string, int> &e : all ) {
        if( mood.entries.size() >= max_mood_entries ) {
            break;
        }
        mood_entry m;
        m.name = client_text( e.first, max_mood_name_columns, 4 * max_mood_name_columns );
        if( m.name.empty() ) {
            continue;
        }
        m.value = clamp_i16( e.second );
        const double side = e.second > 0 ? good : bad;
        const double sq = static_cast<double>( e.second ) * e.second;
        // Cut, not rounded: what the morale screen prints.
        m.percent = side > 0 ? static_cast<uint8_t>( std::clamp( sq / side * 100.0, 0.0, 100.0 ) ) : 0;
        mood.entries.push_back( std::move( m ) );
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
    capture_mood( u, pv.mood );
    if( u.activity ) {
        activity_view a;
        const std::string &id = u.activity.id().str();
        const bool plain = !id.empty() && id.size() <= max_activity_id_bytes &&
        std::all_of( id.begin(), id.end(), []( const char ch ) {
            return ( ch >= 'A' && ch <= 'Z' ) || ( ch >= 'a' && ch <= 'z' ) || ( ch >= '0' && ch <= '9' ) || ch == '_';
        } );
        if( plain ) {
            a.id = id;
        }
        a.verb = client_text( u.activity.get_verb().translated(), max_activity_verb_columns,
                              max_activity_verb_columns * 4 );
        pv.activity = std::move( a );
    }
    pv.asleep = u.has_effect( effect_sleep );
    pv.wielded = wielded_index( u );
    if( const item_location w = u.get_wielded_item(); w && w->is_gun() ) {
        gun_view g;
        g.type = pv.wielded;
        if( !w->has_flag( flag_NON_FOULING ) ) {
            g.dirt = gun_dirt( *w );
        }
        if( !w->ammo_types().empty() ) {
            g.rounds = std::clamp( w->ammo_remaining(), 0, 100000 );
        }
        // Every fault it has, not only the type's potential ones (a save or a mod can add others).
        for( const fault_id &f : w->faults ) {
            if( g.faults.size() >= max_gun_faults ) {
                break;
            }
            if( !f.str().empty() && f.str().size() <= max_fault_id_bytes ) {
                g.faults.push_back( f.str() );
            }
        }
        // item::gun_noise's own test: a mod that takes noise off (loudness_modifier < 0) or scales it down
        // (loudness_multiplier < 1). gunmods() lists built-in mods too (an integral suppressor, hk_mp5sd).
        for( const item *mod : w->gunmods() ) {
            if( mod != nullptr && mod->type->gunmod &&
                ( mod->type->gunmod->loudness < 0 || mod->type->gunmod->loudness_multiplier < 1.0f ) ) {
                g.suppressed = true;
                break;
            }
        }
        pv.wielded_gun = std::move( g );
    }
    for( const effect &e : u.get_effects() ) {
        if( pv.effects.size() >= max_player_effects ) {
            break;
        }
        const std::string &id = e.get_id().str();
        const std::string name = client_text( e.disp_name(), max_effect_name_columns, 4 * max_effect_name_columns );
        if( name.empty() || id.empty() || id.size() > max_effect_id_bytes ) {
            continue;
        }
        effect_view ev;
        ev.id = id;
        ev.name = name;
        const bodypart_id bp = e.get_bp();
        ev.body_part = bp.id().is_null() ? 0 : intern_id( id_kind::body_part, bp.id().str() );
        ev.intensity = clamp_u8( e.get_intensity() );
        const effect_type *t = e.get_effect_type();
        switch( t ? t->get_rating( e.get_intensity() ) : m_neutral ) {
            case m_good:
                ev.rating = 1;
                break;
            case m_bad:
                ev.rating = -1;
                break;
            case m_mixed:
                ev.rating = 2;
                break;
            default:
                ev.rating = 0;
                break;
        }
        if( !e.is_permanent() ) {
            ev.seconds_left = static_cast<int32_t>( std::clamp<int64_t>( to_seconds<int64_t>( e.get_duration() ), 0,
                                                    INT32_MAX ) );
        }
        pv.effects.push_back( std::move( ev ) );
    }
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
        list_carried( weapon, 0, -1, carried_wielded, 0, offhand, seen, pv.items, pv.item_states,
                      pv.item_amounts );
    }
    for( const item_location &worn : u.top_items_loc() ) {
        list_carried( worn, 0, -1, carried_worn, 0, offhand, seen, pv.items, pv.item_states, pv.item_amounts );
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

    // The weather type's numbers come from game data (mods too): bounded, and never NaN or infinite in the JSON.
    const auto bounded = []( float v, float lo, float hi, float fallback ) {
        return std::isfinite( v ) ? std::clamp( v, lo, hi ) : fallback;
    };
    const weather_type_id &wid = weather.weather_id;
    if( wid.is_valid() ) {
        const weather_type &w = *wid;
        env.weather_id = wid.str().size() <= max_weather_id_bytes ? sanitize_text( wid.str(),
                         max_weather_id_bytes ) : std::string();
        env.weather_name = client_text( w.name.translated(), max_weather_name_columns,
                                        4 * max_weather_name_columns );
        env.precip = static_cast<uint8_t>( std::clamp( static_cast<int>( w.precip ), 0,
                                           static_cast<int>( precip_class::heavy ) ) );
        // Snow is what CDDA draws and sounds as snow; `rains` only says whether it wets (a mod's snow can).
        const bool snow = w.tiles_animation == "weather_snowflake" ||
                          w.sound_category == weather_sound_category::flurries ||
                          w.sound_category == weather_sound_category::snow ||
                          w.sound_category == weather_sound_category::snowstorm;
        env.precip_kind = env.precip == 0 ? 0 : snow ? 2 : 1;
        env.weather_sound = static_cast<uint8_t>( std::clamp( static_cast<int>( w.sound_category ), 0,
                            static_cast<int>( weather_sound_category::last ) - 1 ) );
        env.sight_penalty = bounded( w.sight_penalty, 0.0f, 100.0f, 1.0f );
        env.light_modifier = static_cast<int16_t>( std::clamp( w.light_modifier, -1000, 1000 ) );
        env.sun_multiplier = bounded( w.sun_multiplier, 0.0f, 10.0f, 1.0f );
        env.sound_attn = static_cast<int16_t>( std::clamp( w.sound_attn, 0, 1000 ) );
        env.dangerous = w.dangerous;
    }
    env.sheltered = g->is_sheltered( u.pos_bub() );
    // As CDDA works out the wind on the player (character_body.cpp), without a vehicle's own speed.
    // Wind and humidity can be set by game data (effects on conditions): bounded before CDDA's integer maths.
    const int local_wind = get_local_windpower( std::clamp( weather.windspeed, 0, 1000 ),
                           overmap_buffer.ter( u.pos_abs_omt() ), u.pos_abs(), weather.winddirection, env.sheltered );
    env.wind_at_player = bounded( static_cast<float>( local_wind * 0.44704 ), 0.0f, 1000.0f, 0.0f );
    const double humidity = weather.weather_precise->humidity;
    env.humidity = static_cast<uint8_t>( std::clamp( get_local_humidity( std::isfinite( humidity ) ?
                                         std::clamp( humidity, 0.0, 100.0 ) : 0.0, wid.is_valid() ? wid : WEATHER_CLEAR,
                                         env.sheltered ), 0, 100 ) );
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
        vv.name = client_text( v->name, max_vehicle_name_columns, 4 * max_vehicle_name_columns );
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
            const tripoint_rel_ms offset = vp.pos_bub( here ) - wv.pos;
            pv.dx = static_cast<int8_t>( std::clamp( offset.x(), -max_part_offset, max_part_offset ) );
            pv.dy = static_cast<int8_t>( std::clamp( offset.y(), -max_part_offset, max_part_offset ) );
            pv.dz = static_cast<int8_t>( std::clamp( offset.z(), -max_part_offset, max_part_offset ) );
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
            if( !c->is_avatar() ) {
                cv.name = client_text( ch->get_name(), max_creature_name_columns, 4 * max_creature_name_columns );
            }
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

bool inside_view::operator==( const inside_view &o ) const
{
    return container == o.container && uid == o.uid && type == o.type && pocket == o.pocket;
}

bool item_handle_view::operator==( const item_handle_view &o ) const
{
    return uid == o.uid && type == o.type && spot_x == o.spot_x && spot_y == o.spot_y && yaw == o.yaw &&
           side == o.side && shelf == o.shelf;
}

bool light_view::operator==( const light_view &o ) const
{
    return tile == o.tile && luminance == o.luminance && kind == o.kind;
}

bool field_view::operator==( const field_view &o ) const
{
    return tile == o.tile && type == o.type && intensity == o.intensity && age_turns == o.age_turns;
}

bool heat_view::operator==( const heat_view &o ) const
{
    return tile == o.tile && source == o.source && tool == o.tool && fire == o.fire && charges == o.charges &&
           lit == o.lit;
}

bool cookware_item_view::operator==( const cookware_item_view &o ) const
{
    return uid == o.uid && type == o.type && count == o.count;
}

bool cookware_view::operator==( const cookware_view &o ) const
{
    return tile == o.tile && uid == o.uid && type == o.type && contents == o.contents &&
           craft_result == o.craft_result && craft_recipe == o.craft_recipe && craft_progress == o.craft_progress;
}

bool submap_view::same_shape( const submap_view &o ) const
{
    return ter == o.ter && furn == o.furn && trap == o.trap && flags == o.flags;
}

bool submap_view::same_detail( const submap_view &o ) const
{
    return light == o.light && vis == o.vis && items == o.items && fields == o.fields && heat == o.heat &&
           cookware == o.cookware && lights == o.lights && inside == o.inside && inside_more == o.inside_more;
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

std::string id_name( id_kind kind, uint32_t index )
{
    const size_t k = static_cast<size_t>( kind );
    std::lock_guard<std::mutex> lk( ids_mutex );
    if( k >= session_tables.size() ) {
        return "";
    }
    const std::vector<std::string> &names = session_tables[k].names;
    return index > 0 && index < names.size() ? names[index] : std::string();
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
    // The levels the host wants (want_snapshot_levels), around the player's.
    const int below = want_below.load();
    const int above = want_above.load();
    const int z_first = below < 0 ? -OVERMAP_DEPTH : std::max( -OVERMAP_DEPTH, u.posz() - below );
    const int z_last = above < 0 ? OVERMAP_HEIGHT : std::min( OVERMAP_HEIGHT, u.posz() + above );
    s->z_first = static_cast<int8_t>( z_first );
    s->z_last = static_cast<int8_t>( z_last );
    s->submaps.reserve( static_cast<size_t>( MAPSIZE ) * MAPSIZE * ( z_last - z_first + 1 ) );
    for( int z = z_first; z <= z_last; z++ ) {
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
    // Live item tiles: the lying items in reach, now that the piles gave them their handles.
    capture_near_item_states( here, u, seen, s->player.item_states, s->player.item_amounts );
    // Submaps that left the bubble are forgotten; if they come back they start over at revision 1. Ones on a
    // level this capture left out keep their last view: their revisions go on from it when the level is taken again.
    if( z_first > -OVERMAP_DEPTH || z_last < OVERMAP_HEIGHT ) {
        for( auto &old : state.previous ) {
            const int x = std::get<0>( old.first );
            const int y = std::get<1>( old.first );
            const int z = std::get<2>( old.first );
            if( ( z < z_first || z > z_last ) && x >= origin.x() && x < origin.x() + MAPSIZE &&
                y >= origin.y() && y < origin.y() + MAPSIZE ) {
                current.emplace( old.first, std::move( old.second ) );
            }
        }
    }
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

void want_snapshot_levels( int below, int above )
{
    widen_levels( want_below, std::clamp( below, 0, OVERMAP_LAYERS ) );
    widen_levels( want_above, std::clamp( above, 0, OVERMAP_LAYERS ) );
}

void want_all_snapshot_levels()
{
    want_below.store( -1 );
    want_above.store( -1 );
}

void publish_snapshot( std::shared_ptr<const snapshot> s )
{
    std::lock_guard<std::mutex> lk( latest_mutex );
    if( s && latest && s->seq < latest->seq ) {
        return;
    }
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
    // The z range CDDA has (constant per build).
    jo.member( "z_levels" );
    jo.start_array();
    jo.write( -OVERMAP_DEPTH );
    jo.write( OVERMAP_HEIGHT );
    jo.end_array();
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
    if( pv.activity ) {
        jo.member( "activity" );
        jo.start_object();
        jo.member( "id", pv.activity->id );
        jo.member( "verb", pv.activity->verb );
        jo.end_object();
    }
    if( pv.asleep ) {
        jo.member( "asleep", true );
    }
    jo.member( "wielded", static_cast<int64_t>( pv.wielded ) );
    if( pv.wielded_gun ) {
        const gun_view &g = *pv.wielded_gun;
        jo.member( "wielded_gun" );
        jo.start_object();
        jo.member( "type", static_cast<int64_t>( g.type ) );
        if( g.dirt >= 0 ) {
            jo.member( "dirt", static_cast<int>( g.dirt ) );
        }
        if( g.rounds >= 0 ) {
            jo.member( "rounds", g.rounds );
        }
        jo.member( "faults" );
        jo.start_array();
        for( const std::string &f : g.faults ) {
            jo.write( f );
        }
        jo.end_array();
        if( g.suppressed ) {
            jo.member( "suppressed", true );
        }
        jo.end_object();
    }
    jo.member( "mood" );
    jo.start_object();
    jo.member( "level", static_cast<int>( pv.mood.level ) );
    jo.member( "band", static_cast<int>( pv.mood.band ) );
    jo.member( "face", pv.mood.face );
    jo.member( "total", static_cast<int>( pv.mood.total ) );
    jo.member( "entries" );
    jo.start_array();
    for( const mood_entry &m : pv.mood.entries ) {
        jo.start_object();
        jo.member( "name", m.name );
        jo.member( "value", static_cast<int>( m.value ) );
        jo.member( "percent", static_cast<int>( m.percent ) );
        jo.end_object();
    }
    jo.end_array();
    jo.end_object();
    jo.member( "effects" );
    jo.start_array();
    for( const effect_view &e : pv.effects ) {
        jo.start_object();
        jo.member( "id", e.id );
        jo.member( "name", e.name );
        jo.member( "body_part", static_cast<int64_t>( e.body_part ) );
        jo.member( "intensity", static_cast<int>( e.intensity ) );
        jo.member( "rating", static_cast<int>( e.rating ) );
        jo.member( "seconds_left", e.seconds_left );
        jo.end_object();
    }
    jo.end_array();
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
    jo.member( "item_states" );
    jo.start_array();
    for( const item_state_view &st : pv.item_states ) {
        jo.start_array();
        jo.write( static_cast<int64_t>( st.uid ) );
        jo.write( static_cast<int>( st.temp ) );
        jo.write( static_cast<int>( st.rot ) );
        jo.write( static_cast<int>( st.fill ) );
        jo.write( static_cast<int64_t>( st.fill_type ) );
        jo.write( st.see_through ? 1 : 0 );
        jo.write( static_cast<int>( st.fill_ml ) );
        jo.write( static_cast<int>( st.dirt ) );
        jo.end_array();
    }
    jo.end_array();
    jo.member( "item_amounts" );
    jo.start_array();
    for( const item_amount_view &av : pv.item_amounts ) {
        jo.start_array();
        jo.write( static_cast<int64_t>( av.uid ) );
        jo.write( static_cast<int>( av.count ) );
        jo.write( static_cast<int>( av.weight_g ) );
        jo.write( static_cast<int>( av.volume_ml ) );
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
    jo.member( "weather_id", env.weather_id );
    jo.member( "weather_name", env.weather_name );
    jo.member( "precip", static_cast<int>( env.precip ) );
    jo.member( "precip_kind", static_cast<int>( env.precip_kind ) );
    jo.member( "weather_sound", static_cast<int>( env.weather_sound ) );
    jo.member( "sight_penalty", env.sight_penalty );
    jo.member( "light_modifier", static_cast<int>( env.light_modifier ) );
    jo.member( "sun_multiplier", env.sun_multiplier );
    jo.member( "sound_attn", static_cast<int>( env.sound_attn ) );
    jo.member( "dangerous", env.dangerous );
    jo.member( "wind_at_player", env.wind_at_player );
    jo.member( "humidity", static_cast<int>( env.humidity ) );
    jo.member( "sheltered", env.sheltered );
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
        // How placed items lie, only for those with a stored pose: [uid, yaw, side]. Its own list, so a handle
        // keeps its five numbers.
        jo.member( "poses" );
        jo.start_array();
        for( const item_pile_view &p : sm.items ) {
            for( const item_handle_view &h : p.handles ) {
                if( h.yaw >= 0 ) {
                    jo.start_array();
                    jo.write( static_cast<int64_t>( h.uid ) );
                    jo.write( static_cast<int>( h.yaw ) );
                    jo.write( h.side ? 1 : 0 );
                    jo.end_array();
                }
            }
        }
        jo.end_array();
        // The furniture shelf placed items were put on, only for those with one stored: [uid, shelf].
        jo.member( "shelves" );
        jo.start_array();
        for( const item_pile_view &p : sm.items ) {
            for( const item_handle_view &h : p.handles ) {
                if( h.shelf >= 0 ) {
                    jo.start_array();
                    jo.write( static_cast<int64_t>( h.uid ) );
                    jo.write( static_cast<int>( h.shelf ) );
                    jo.end_array();
                }
            }
        }
        jo.end_array();
        // L8: piles with more items than handles listed: [tile, items not listed].
        jo.member( "more" );
        jo.start_array();
        for( const item_pile_view &p : sm.items ) {
            if( p.count > p.handles.size() ) {
                jo.start_array();
                jo.write( static_cast<int>( p.tile ) );
                jo.write( static_cast<int64_t>( p.count - p.handles.size() ) );
                jo.end_array();
            }
        }
        jo.end_array();
        // L8: what lies in containers next to the player: [container, uid, type, pocket]; containers cut short:
        // [container, items left out].
        jo.member( "inside" );
        jo.start_array();
        for( const inside_view &v : sm.inside ) {
            jo.start_array();
            jo.write( static_cast<int64_t>( v.container ) );
            jo.write( static_cast<int64_t>( v.uid ) );
            jo.write( static_cast<int64_t>( v.type ) );
            jo.write( static_cast<int>( v.pocket ) );
            jo.end_array();
        }
        jo.end_array();
        jo.member( "inside_more" );
        jo.start_array();
        for( const std::pair<uint64_t, uint32_t> &m : sm.inside_more ) {
            jo.start_array();
            jo.write( static_cast<int64_t>( m.first ) );
            jo.write( static_cast<int64_t>( m.second ) );
            jo.end_array();
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
        // C6: [tile, luminance, kind (0 terrain/furniture, 1 item, 2 field)]
        jo.member( "lights" );
        jo.start_array();
        for( const light_view &l : sm.lights ) {
            jo.start_array();
            jo.write( static_cast<int>( l.tile ) );
            jo.write( static_cast<int>( l.luminance ) );
            jo.write( static_cast<int>( l.kind ) );
            jo.end_array();
        }
        jo.end_array();
        // L9: [tile, source, tool, fire, charges, lit (0/1)]
        jo.member( "heat" );
        jo.start_array();
        for( const heat_view &h : sm.heat ) {
            jo.start_array();
            jo.write( static_cast<int>( h.tile ) );
            jo.write( static_cast<int>( h.source ) );
            jo.write( static_cast<int64_t>( h.tool ) );
            jo.write( static_cast<int>( h.fire ) );
            jo.write( static_cast<int64_t>( h.charges ) );
            jo.write( h.lit ? 1 : 0 );
            jo.end_array();
        }
        jo.end_array();
        jo.member( "cookware" );
        jo.start_array();
        for( const cookware_view &cw : sm.cookware ) {
            jo.start_object();
            jo.member( "tile", static_cast<int>( cw.tile ) );
            jo.member( "uid", static_cast<int64_t>( cw.uid ) );
            jo.member( "type", static_cast<int64_t>( cw.type ) );
            jo.member( "contents" );
            jo.start_array();
            for( const cookware_item_view &c : cw.contents ) {
                jo.start_array();
                jo.write( static_cast<int64_t>( c.uid ) );
                jo.write( static_cast<int64_t>( c.type ) );
                jo.write( static_cast<int64_t>( c.count ) );
                jo.end_array();
            }
            jo.end_array();
            jo.member( "craft_result", static_cast<int64_t>( cw.craft_result ) );
            jo.member( "craft_recipe", cw.craft_recipe );
            jo.member( "craft_progress", static_cast<int>( cw.craft_progress ) );
            jo.end_object();
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
        if( c.kind == 1 ) {
            jo.member( "name", c.name );
        }
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
            jo.write( static_cast<int>( p.dx ) );
            jo.write( static_cast<int>( p.dy ) );
            jo.write( static_cast<int>( p.dz ) );
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


// ---- Character sheet (G4): the character screen's data for a character query (bridge.h). ----

namespace
{
struct speed_mod {
    std::string name;
    int value = 0;
    bool percent = false;
    bool move_cost = false;   // a move cost change (CDDA's MOVE COST list), not a speed change
};
struct enc_view {
    int enc = 0;
    int layer = 0;
    int warmth = 0;
};
struct skill_view {
    std::string id;
    std::string name;
    std::string description;
    int level = 0;
    int knowledge = 0;
    int progress = 0;
    int knowledge_progress = 0;
};
struct prof_view {
    std::string id;
    std::string name;
    std::string description;
    int percent = 0;
};
struct trait_view {
    std::string id;
    std::string name;
    std::string description;
    int rating = 0;
    bool mutation = false;
};
struct bionic_view {
    std::string id;
    std::string name;
    std::string description;
    bool active = false;
};
struct sheet {
    std::string name;
    std::string profession;
    int age = 0;
    int height_cm = 0;
    int stats[4][2] = {};
    int speed = 0;
    int move_cost = 0;
    std::vector<speed_mod> speed_mods;
    std::vector<enc_view> encumbrance;
    std::vector<skill_view> skills;
    std::vector<prof_view> known;
    std::vector<prof_view> learning;
    std::vector<trait_view> traits;
    std::vector<bionic_view> bionics;
    int power_kj = 0;
    int max_power_kj = 0;
    // What the character carries and can carry, as CDDA's inventory screen counts it.
    int weight_g = 0;
    int weight_max_g = 0;
    int volume_ml = 0;
    int volume_max_ml = 0;
    std::vector<std::pair<std::string, std::string>> effects;   // id, description
};

// An id from game data the client gets as is: printable ASCII, at most max_sheet_id_bytes; empty if not.
std::string clean_id( const std::string &id )
{
    if( id.empty() || id.size() > max_sheet_id_bytes ) {
        return std::string();
    }
    for( const char ch : id ) {
        const unsigned char u = static_cast<unsigned char>( ch );
        if( u < 0x20 || u >= 0x7f || ch == '<' || ch == '>' ) {
            return std::string();
        }
    }
    return id;
}

std::string name_text( const std::string &raw )
{
    return client_text( raw, max_sheet_name_columns, 4 * max_sheet_name_columns );
}

// Line breaks and tabs become spaces (client_text drops control characters), runs of spaces one.
std::string description_text( const std::string &raw )
{
    std::string flat;
    flat.reserve( std::min<size_t>( raw.size(), 8 * max_sheet_description_columns ) );
    for( const char ch : raw ) {
        if( flat.size() >= 8 * max_sheet_description_columns ) {
            break;
        }
        const char c = ch == '\n' || ch == '\r' || ch == '\t' ? ' ' : ch;
        if( c == ' ' && ( flat.empty() || flat.back() == ' ' ) ) {
            continue;
        }
        flat.push_back( c );
    }
    while( !flat.empty() && flat.back() == ' ' ) {
        flat.pop_back();
    }
    return client_text( flat, max_sheet_description_columns, 4 * max_sheet_description_columns );
}

int clamp_int( double v, int lo, int hi )
{
    if( !std::isfinite( v ) ) {
        return lo;
    }
    return static_cast<int>( std::clamp( v, static_cast<double>( lo ), static_cast<double>( hi ) ) );
}

// CDDA's speed tab (player_display.cpp get_speedlist_entries): effects' SPEED mods by speed name, the other speed
// bonuses, then the move cost changes.
void read_speed( const avatar &u, sheet &s )
{
    std::map<std::string, int> by_effect;
    for( const effect &e : u.get_effects() ) {
        const int adjust = e.get_mod( "SPEED", u.resists_effect( e ) );
        if( adjust != 0 ) {
            by_effect[e.get_speed_name()] += adjust;
        }
    }
    const auto add = [&s]( const std::string & raw, int value, bool percent, bool move_cost ) {
        const std::string name = name_text( raw );
        if( value == 0 || name.empty() || s.speed_mods.size() >= max_sheet_speed_modifiers ) {
            return;
        }
        s.speed_mods.push_back( speed_mod{ name, value, percent, move_cost } );
    };
    for( const std::pair<const std::string, int> &e : by_effect ) {
        add( e.first, e.second, false, false );
    }
    for( const speed_bonus_effect &b : u.get_speed_bonus_effects() ) {
        if( by_effect.find( b.description ) == by_effect.end() ) {
            add( b.description, b.bonus, false, false );
        }
    }
    float movecost = 100;
    for( const run_cost_effect &r : u.run_cost_effects( movecost ) ) {
        const int percent = clamp_int( std::trunc( r.times * 100 - 100 ), -10000, 10000 );
        const int constant = clamp_int( std::trunc( r.plus ), -10000, 10000 );
        if( ( r.times != 1.0f && percent == 0 ) || ( r.plus != 0 && constant == 0 ) ) {
            continue;
        }
        add( r.description, percent != 0 ? percent : constant, percent != 0, true );
    }
    s.speed = u.get_speed();
    s.move_cost = clamp_int( movecost, 0, 100000 );
}

void read_skills( const avatar &u, sheet &s )
{
    const std::vector<const Skill *> sorted = Skill::get_skills_sorted_by(
    []( const Skill & a, const Skill & b ) {
        return a.get_sort_rank() < b.get_sort_rank();
    } );
    for( const Skill *sk : sorted ) {
        if( s.skills.size() >= max_sheet_skills ) {
            break;
        }
        skill_view v;
        v.id = clean_id( sk->ident().str() );
        v.name = name_text( sk->name() );
        if( v.id.empty() || v.name.empty() ) {
            continue;
        }
        const SkillLevel &level = u.get_skill_level_object( sk->ident() );
        v.level = level.level();
        v.knowledge = level.knowledgeLevel();
        v.progress = std::clamp( level.exercise(), 0, 100 );
        v.knowledge_progress = std::clamp( level.knowledgeExperience(), 0, 100 );
        v.description = description_text( sk->description() );
        s.skills.push_back( std::move( v ) );
    }
}

void read_proficiencies( const avatar &u, sheet &s )
{
    for( const display_proficiency &p : u.display_proficiencies() ) {
        const bool learning = !p.known && p.id->can_learn();
        std::vector<prof_view> &list = p.known ? s.known : s.learning;
        if( ( !p.known && !learning ) || list.size() >= max_sheet_proficiencies ) {
            continue;
        }
        prof_view v;
        v.id = clean_id( p.id.str() );
        v.name = name_text( p.id->name() );
        if( v.id.empty() || v.name.empty() ) {
            continue;
        }
        v.percent = p.known ? 100 : clamp_int( std::floor( p.practice * 100 ), 0, 99 );
        v.description = description_text( p.id->description() );
        list.push_back( std::move( v ) );
    }
}

void read_traits( const avatar &u, sheet &s )
{
    std::vector<trait_and_var> traits = u.get_mutations_variants( false );
    std::sort( traits.begin(), traits.end(), trait_var_display_sort );
    for( const trait_and_var &t : traits ) {
        if( s.traits.size() >= max_sheet_traits ) {
            break;
        }
        trait_view v;
        v.id = clean_id( t.trait.str() );
        v.name = name_text( t.name() );
        if( v.id.empty() || v.name.empty() ) {
            continue;
        }
        const int points = t.trait->points;
        v.rating = points > 0 ? 1 : points < 0 ? -1 : 0;
        v.mutation = !u.has_base_trait( t.trait );
        v.description = description_text( u.mutation_desc( t.trait ) );
        s.traits.push_back( std::move( v ) );
    }
}

void read_bionics( const avatar &u, sheet &s )
{
    for( const bionic_id &b : u.get_bionics() ) {
        if( s.bionics.size() >= max_sheet_bionics ) {
            break;
        }
        bionic_view v;
        v.id = clean_id( b.str() );
        v.name = name_text( b->name.translated() );
        if( v.id.empty() || v.name.empty() ) {
            continue;
        }
        v.active = u.has_active_bionic( b );
        v.description = description_text( b->description.translated() );
        s.bionics.push_back( std::move( v ) );
    }
    s.power_kj = clamp_int( units::to_kilojoule( u.get_power_level() ), 0, INT32_MAX );
    s.max_power_kj = clamp_int( units::to_kilojoule( u.get_max_power_level() ), 0, INT32_MAX );
}

// The effects the snapshot lists (same filter), each id once.
void read_effects( const avatar &u, sheet &s )
{
    std::unordered_set<std::string> seen;
    for( const effect &e : u.get_effects() ) {
        if( s.effects.size() >= max_sheet_effects ) {
            break;
        }
        const std::string &id = e.get_id().str();
        if( id.empty() || id.size() > max_effect_id_bytes ||
            client_text( e.disp_name(), max_effect_name_columns, 4 * max_effect_name_columns ).empty() ||
            !seen.insert( id ).second ) {
            continue;
        }
        const std::string clean = sanitize_text( id, max_effect_id_bytes );
        if( clean != id ) {
            continue;
        }
        s.effects.emplace_back( id, description_text( e.disp_desc() ) );
    }
}

sheet read_sheet( const avatar &u )
{
    sheet s;
    s.name = name_text( u.get_name() );
    s.profession = u.prof != nullptr ? name_text( u.prof->gender_appropriate_name( u.male ) ) : std::string();
    s.age = clamp_int( u.age(), 0, 100000 );
    s.height_cm = clamp_int( u.height(), 0, 100000 );
    s.weight_g = clamp_int( static_cast<double>( units::to_gram( u.weight_carried() ) ), 0, INT32_MAX );
    s.weight_max_g = clamp_int( static_cast<double>( units::to_gram( u.weight_capacity() ) ), 0, INT32_MAX );
    s.volume_ml = clamp_int( static_cast<double>( units::to_milliliter( u.volume_carried() ) ), 0, INT32_MAX );
    s.volume_max_ml = clamp_int( static_cast<double>( units::to_milliliter( u.volume_capacity() ) ), 0, INT32_MAX );
    const int stats[4][2] = {
        { u.get_str(), u.get_str_base() }, { u.get_dex(), u.get_dex_base() },
        { u.get_int(), u.get_int_base() }, { u.get_per(), u.get_per_base() }
    };
    std::copy( &stats[0][0], &stats[0][0] + 8, &s.stats[0][0] );
    read_speed( u, s );
    // Same parts and order as the snapshot's body_parts.
    for( const bodypart_id &bp : u.get_all_body_parts( get_body_part_flags::only_main ) ) {
        if( s.encumbrance.size() >= max_body_parts_listed ) {
            break;
        }
        enc_view e;
        e.layer = u.get_part_layer_penalty( bp );
        e.enc = u.get_part_encumbrance( bp ) - e.layer;
        // CDDA's warmth number (player_display.cpp temperature_print_rescaling).
        e.warmth = clamp_int( units::to_legacy_bodypart_temp( u.get_part_temp_conv( bp ) ) / 100.0 * 2 - 100,
                              -1000, 1000 );
        s.encumbrance.push_back( e );
    }
    read_skills( u, s );
    read_proficiencies( u, s );
    read_traits( u, s );
    read_bionics( u, s );
    read_effects( u, s );
    return s;
}

std::string write_sheet( uint64_t query_id, const sheet &s, bool truncated )
{
    static const char *const stat_ids[4] = { "str", "dex", "int", "per" };
    std::ostringstream os;
    JsonOut jo( os );
    jo.start_object();
    jo.member( "id", static_cast<int64_t>( query_id ) );
    jo.member( "ok", true );
    jo.member( "kind", std::string( "character" ) );
    jo.member( "name", s.name );
    jo.member( "profession", s.profession );
    jo.member( "age", s.age );
    jo.member( "height_cm", s.height_cm );
    jo.member( "weight_g", s.weight_g );
    jo.member( "weight_max_g", s.weight_max_g );
    jo.member( "volume_ml", s.volume_ml );
    jo.member( "volume_max_ml", s.volume_max_ml );
    jo.member( "stats" );
    jo.start_array();
    for( int i = 0; i < 4; i++ ) {
        jo.start_object();
        jo.member( "id", std::string( stat_ids[i] ) );
        jo.member( "cur", s.stats[i][0] );
        jo.member( "base", s.stats[i][1] );
        jo.end_object();
    }
    jo.end_array();
    jo.member( "speed" );
    jo.start_object();
    jo.member( "cur", s.speed );
    jo.member( "move_cost", s.move_cost );
    jo.member( "modifiers" );
    jo.start_array();
    for( const speed_mod &m : s.speed_mods ) {
        jo.start_object();
        jo.member( "name", m.name );
        jo.member( "value", m.value );
        jo.member( "percent", m.percent );
        jo.member( "move_cost", m.move_cost );
        jo.end_object();
    }
    jo.end_array();
    jo.end_object();
    jo.member( "encumbrance" );
    jo.start_array();
    for( const enc_view &e : s.encumbrance ) {
        jo.start_object();
        jo.member( "enc", e.enc );
        jo.member( "layer", e.layer );
        jo.member( "warmth", e.warmth );
        jo.end_object();
    }
    jo.end_array();
    jo.member( "skills" );
    jo.start_array();
    for( const skill_view &v : s.skills ) {
        jo.start_object();
        jo.member( "id", v.id );
        jo.member( "name", v.name );
        jo.member( "level", v.level );
        jo.member( "knowledge", v.knowledge );
        jo.member( "progress", v.progress );
        jo.member( "knowledge_progress", v.knowledge_progress );
        jo.member( "description", v.description );
        jo.end_object();
    }
    jo.end_array();
    const auto write_profs = [&jo]( const char *key, const std::vector<prof_view> &list, bool percent ) {
        jo.member( key );
        jo.start_array();
        for( const prof_view &v : list ) {
            jo.start_object();
            jo.member( "id", v.id );
            jo.member( "name", v.name );
            if( percent ) {
                jo.member( "percent", v.percent );
            }
            jo.member( "description", v.description );
            jo.end_object();
        }
        jo.end_array();
    };
    jo.member( "proficiencies" );
    jo.start_object();
    write_profs( "known", s.known, false );
    write_profs( "learning", s.learning, true );
    jo.end_object();
    jo.member( "traits" );
    jo.start_array();
    for( const trait_view &v : s.traits ) {
        jo.start_object();
        jo.member( "id", v.id );
        jo.member( "name", v.name );
        jo.member( "rating", v.rating );
        jo.member( "mutation", v.mutation );
        jo.member( "description", v.description );
        jo.end_object();
    }
    jo.end_array();
    jo.member( "bionics" );
    jo.start_array();
    for( const bionic_view &v : s.bionics ) {
        jo.start_object();
        jo.member( "id", v.id );
        jo.member( "name", v.name );
        jo.member( "active", v.active );
        jo.member( "description", v.description );
        jo.end_object();
    }
    jo.end_array();
    jo.member( "power" );
    jo.start_object();
    jo.member( "cur_kj", s.power_kj );
    jo.member( "max_kj", s.max_power_kj );
    jo.end_object();
    jo.member( "effect_descriptions" );
    jo.start_object();
    for( const std::pair<std::string, std::string> &e : s.effects ) {
        jo.member( e.first, e.second );
    }
    jo.end_object();
    jo.member( "truncated", truncated );
    jo.end_object();
    return os.str();
}
} // namespace

std::string run_character_query( uint64_t query_id, size_t max_bytes )
{
    sheet s = read_sheet( get_avatar() );
    std::string out = write_sheet( query_id, s, false );
    if( out.size() <= max_bytes ) {
        return out;
    }
    // Empty descriptions from the end. Each saves at least its raw size (JSON escaping only adds), so this drops
    // enough; the margin covers "truncated": true.
    std::vector<std::string *> descriptions;
    for( skill_view &v : s.skills ) {
        descriptions.push_back( &v.description );
    }
    for( prof_view &v : s.known ) {
        descriptions.push_back( &v.description );
    }
    for( prof_view &v : s.learning ) {
        descriptions.push_back( &v.description );
    }
    for( trait_view &v : s.traits ) {
        descriptions.push_back( &v.description );
    }
    for( bionic_view &v : s.bionics ) {
        descriptions.push_back( &v.description );
    }
    for( std::pair<std::string, std::string> &e : s.effects ) {
        descriptions.push_back( &e.second );
    }
    size_t excess = out.size() - max_bytes + 16;
    for( auto it = descriptions.rbegin(); it != descriptions.rend() && excess > 0; ++it ) {
        const size_t n = ( *it )->size();
        ( *it )->clear();
        excess = n >= excess ? 0 : excess - n;
    }
    out = write_sheet( query_id, s, true );
    // Still over (a long-modded character): entries go from the ends of the lists, least needed first. Each saves its
    // strings' raw size plus `keys`: the bytes write_sheet puts around them with empty strings and one-digit numbers
    // (close to the real size, not a strict bound: `true` and a last entry without its comma differ by a few
    // bytes), so one round drops enough and little more; more rounds guard against a miscount.
    for( int round = 0; round < 4 && out.size() > max_bytes; round++ ) {
        size_t over = out.size() - max_bytes + 16;
        const auto cut = [&over]( auto & list, size_t keys ) {
            while( over > 0 && !list.empty() ) {
                const size_t n = keys + list.back().id.size() + list.back().name.size();
                list.pop_back();
                over = n >= over ? 0 : over - n;
            }
        };
        while( over > 0 && !s.effects.empty() ) {
            const size_t n = 6 + s.effects.back().first.size() + s.effects.back().second.size();
            s.effects.pop_back();
            over = n >= over ? 0 : over - n;
        }
        // {"id":"","name":"","percent":0,"description":""}, and the same without percent.
        cut( s.learning, 49 );
        cut( s.known, 37 );
        // {"id":"","name":"","rating":0,"mutation":false,"description":""},
        cut( s.traits, 65 );
        // {"id":"","name":"","active":false,"description":""},
        cut( s.bionics, 52 );
        // {"name":"","value":0,"percent":false,"move_cost":false},
        while( over > 0 && !s.speed_mods.empty() ) {
            const size_t n = 56 + s.speed_mods.back().name.size();
            s.speed_mods.pop_back();
            over = n >= over ? 0 : over - n;
        }
        // {"id":"","name":"","level":0,"knowledge":0,"progress":0,"knowledge_progress":0,"description":""},
        cut( s.skills, 97 );
        out = write_sheet( query_id, s, true );
    }
    return out;
}

std::string character_query_error_json( uint64_t query_id, const std::string &error )
{
    std::ostringstream os;
    JsonOut jo( os );
    jo.start_object();
    jo.member( "id", static_cast<int64_t>( query_id ) );
    jo.member( "ok", false );
    jo.member( "kind", std::string( "character" ) );
    jo.member( "error", error );
    jo.end_object();
    return os.str();
}

} // namespace fp3d
