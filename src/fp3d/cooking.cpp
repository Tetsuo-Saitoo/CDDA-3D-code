#include "fp3d/cooking.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <climits>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "activity_actor_definitions.h"
#include "avatar.h"
#include "calendar.h"
#include "character.h"
#include "coordinates.h"
#include "craft_command.h"
#include "crafting.h"
#include "enums.h"
#include "field.h"
#include "field_type.h"
#include "fp3d/id_tables.h"
#include "fp3d/item_hands.h"
#include "fp3d/snapshot.h"
#include "game.h"
#include "game_constants.h"
#include "inventory.h"
#include "item.h"
#include "item_components.h"
#include "item_location.h"
#include "itype.h"
#include "json.h"
#include "map.h"
#include "map_selector.h"
#include "mapdata.h"
#include "math_parser_diag_value.h"
#include "output.h"
#include "pocket_type.h"
#include "proficiency.h"
#include "recipe.h"
#include "recipe_dictionary.h"
#include "requirements.h"
#include "skill.h"
#include "string_formatter.h"
#include "translations.h"
#include "type_id.h"
#include "veh_type.h"
#include "vehicle.h"
#include "visitable.h"
#include "vpart_position.h"

namespace fp3d
{
namespace
{
const quality_id qual_COOK( "COOK" );
const quality_id qual_BOIL( "BOIL" );
const itype_id itype_fire( "fire" );
// The requirements CDDA's cooking recipes take their heat from (data/json/requirements/cooking_tools.json); the
// tools are read from them, after CDDA's finalize inlined nested lists and added subtype replacements.
const char *const heat_requirements[] = { "surface_heat", "water_boiling_heat" };
// Contents taken into account per query (a pot holds a handful; saves are hostile).
constexpr size_t max_query_contents = 64;
constexpr size_t max_query_content_types = 256;
constexpr size_t max_query_candidates = 2048;
constexpr size_t max_missing_text = 96;

// What the loaded data looked like when a table was built: rebuilt when it changes (a different world's mods).
struct data_signature {
    uint32_t ids_generation = 0;
    size_t furn = 0;
    size_t requirements = 0;
    size_t recipes = 0;
    bool operator==( const data_signature &o ) const {
        return ids_generation == o.ids_generation && furn == o.furn && requirements == o.requirements &&
               recipes == o.recipes;
    }
};

data_signature current_signature()
{
    data_signature s;
    if( const std::shared_ptr<const id_tables> t = latest_id_tables() ) {
        s.ids_generation = t->generation;
    }
    s.furn = furn_t::count();
    s.requirements = requirement_data::all().size();
    s.recipes = recipe_dict.size();
    return s;
}

// ---- tables from the loaded data (sim thread only) ----
struct heat_tables {
    bool built = false;
    data_signature sig;
    // heat tool -> true when CDDA accepts it without charges (a count <= 0 in a heat requirement, e.g. `fire`)
    std::unordered_map<itype_id, bool> tools;
    std::vector<itype_id> furn_tool;              // by furn int id: its heat pseudo tool, null if none
};
heat_tables heat;

struct recipe_index {
    bool built = false;
    data_signature sig;
    std::unordered_map<itype_id, std::vector<recipe_id>> by_component;   // cooking recipes only
};
recipe_index recipes;

const heat_tables &heat_now()
{
    const data_signature sig = current_signature();
    if( heat.built && heat.sig == sig ) {
        return heat;
    }
    heat = heat_tables();
    heat.built = true;
    heat.sig = sig;
    for( const char *name : heat_requirements ) {
        const requirement_id rid( name );
        if( !rid.is_valid() ) {
            continue;
        }
        for( const std::vector<tool_comp> &group : rid->get_tools() ) {
            for( const tool_comp &t : group ) {
                if( !t.type.is_valid() ) {
                    continue;
                }
                bool &presence_enough = heat.tools.emplace( t.type, false ).first->second;
                presence_enough = presence_enough || t.count <= 0;
            }
        }
    }
    heat.furn_tool.assign( sig.furn, itype_id::NULL_ID() );
    for( size_t i = 0; i < sig.furn; i++ ) {
        const itype_id &tool = furn_id( static_cast<int>( i ) ).obj().crafting_pseudo_item;
        if( !tool.is_null() && heat.tools.count( tool ) != 0 ) {
            heat.furn_tool[i] = tool;
        }
    }
    return heat;
}

bool uses_heat( const requirement_data &req, const heat_tables &h )
{
    for( const std::vector<tool_comp> &group : req.get_tools() ) {
        for( const tool_comp &t : group ) {
            if( h.tools.count( t.type ) != 0 ) {
                return true;
            }
        }
    }
    return false;
}

const recipe_index &recipes_now()
{
    const heat_tables &h = heat_now();
    if( recipes.built && recipes.sig == h.sig ) {
        return recipes;
    }
    recipes = recipe_index();
    recipes.built = true;
    recipes.sig = h.sig;
    for( const auto &[rid, r] : recipe_dict ) {
        if( r.obsolete || r.is_blacklisted() || r.is_practice() || r.is_nested() || r.is_blueprint() ||
            r.result().is_null() || !uses_heat( r.simple_requirements(), h ) ) {
            continue;
        }
        std::vector<itype_id> comps;
        for( const std::vector<item_comp> &group : r.simple_requirements().get_components() ) {
            for( const item_comp &c : group ) {
                if( std::find( comps.begin(), comps.end(), c.type ) == comps.end() ) {
                    comps.push_back( c.type );
                }
            }
        }
        for( const itype_id &t : comps ) {
            recipes.by_component[t].push_back( rid );
        }
    }
    return recipes;
}

uint32_t type_index( const item &it )
{
    return intern_id( id_kind::itype, it.typeId().str() );
}

bool matches( const item &it, const item_ref &r )
{
    return !it.is_null() && read_item_handle( it ) == r.uid && type_index( it ) == r.type;
}

int32_t clamp_i32( int64_t v )
{
    return static_cast<int32_t>( std::clamp<int64_t>( v, INT32_MIN, INT32_MAX ) );
}

// At most `max_bytes` of `s`, cut on a UTF-8 character boundary.
std::string cap_utf8( std::string s, size_t max_bytes )
{
    if( s.size() <= max_bytes ) {
        return s;
    }
    size_t n = max_bytes;
    while( n > 0 && ( static_cast<unsigned char>( s[n] ) & 0xC0 ) == 0x80 ) {
        n--;
    }
    s.resize( n );
    return s;
}

// Plain text for the client: no colour tags, no control characters, capped.
std::string plain( const std::string &s, size_t max_bytes )
{
    return cap_utf8( sanitize_text( strip_color_tags( s ), max_bytes * 2 ), max_bytes );
}

bool is_cookware( const item &it )
{
    for( const quality_id &q : { qual_COOK, qual_BOIL } ) {
        const auto found = it.type->qualities.find( q );
        if( found != it.type->qualities.end() && found->second > 0 ) {
            return true;
        }
    }
    return false;
}

// The heat tool a vehicle part on bubble square `p` gives (its pseudo tools), null if none.
itype_id vehicle_heat_tool( map &here, const tripoint_bub_ms &p, const heat_tables &h )
{
    const optional_vpart_position vp = here.veh_at( p );
    if( !vp ) {
        return itype_id::NULL_ID();
    }
    for( const int idx : vp->vehicle().parts_at_relative( vp->mount_pos(), true ) ) {
        const vehicle_part &part = vp->vehicle().part( idx );
        if( part.is_broken() ) {
            continue;
        }
        for( const auto &tool : part.info().get_pseudo_tools() ) {
            if( h.tools.count( tool.first ) != 0 ) {
                return tool.first;
            }
        }
    }
    return itype_id::NULL_ID();
}

// A craft the `cook` item action started names its pot (the pot's handle) in this item var.
const std::string cook_pot_var = "fp3d_cook_pot";

// The pot handle a craft names, read strictly (saves are hostile); 0 if none.
uint64_t read_cook_pot( const item &it )
{
    const diag_value *v = it.maybe_get_value( cook_pot_var );
    if( v == nullptr || !v->is_str() ) {
        return 0;
    }
    const std::string &s = v->str();
    uint64_t n = 0;
    if( s.empty() || s.size() > 16 ) {
        return 0;
    }
    const std::from_chars_result r = std::from_chars( s.data(), s.data() + s.size(), n );
    return r.ec == std::errc() && r.ptr == s.data() + s.size() && n <= max_item_uid ? n : 0;
}

// Whether bubble square `p` gives CDDA's crafting a heat that works now (a fire burns, or the heat tool there has
// charges): the snapshot's `lit`, for one square.
bool heat_lit_at( map &here, const Character &who, const tripoint_bub_ms &p )
{
    const heat_tables &h = heat_now();
    const bool fire_here = here.get_field_intensity( p, fd_fire ) > 0 ||
                           here.has_flag_ter_or_furn( ter_furn_flag::TFLAG_USABLE_FIRE, p );
    const size_t furn = static_cast<size_t>( here.furn( p ).to_i() );
    const itype_id tool = furn < h.furn_tool.size() && !h.furn_tool[furn].is_null() ? h.furn_tool[furn] :
                          vehicle_heat_tool( here, p, h );
    if( tool.is_null() && !fire_here ) {
        return false;
    }
    inventory there;
    there.form_from_map( &here, p, 0, &who, false, false );
    if( there.has_tools( itype_fire, 1 ) ) {
        return true;
    }
    if( tool.is_null() || !there.has_tools( tool, 1 ) ) {
        return false;
    }
    const auto presence = h.tools.find( tool );
    return there.charges_of( tool ) > 0 || ( presence != h.tools.end() && presence->second );
}

// Items looked at on one square for a pot or its craft.
constexpr size_t max_cook_scan = 256;

// The item with handle `uid` lying on `me` or a square next to it (top level), and where; null if none.
item *find_cook_pot( map &here, const tripoint_bub_ms &me, uint64_t uid, tripoint_bub_ms &at )
{
    for( const tripoint_bub_ms &q : here.points_in_radius( me, 1 ) ) {
        size_t looked = 0;
        for( item &it : here.i_at( q ) ) {
            if( looked++ >= max_cook_scan ) {
                break;
            }
            if( read_item_handle( it ) == uid ) {
                at = q;
                return &it;
            }
        }
    }
    return nullptr;
}

// The craft the `cook` action started for the pot `uid`, lying on `me` or a square next to it.
item_location find_cook_craft( map &here, const tripoint_bub_ms &me, uint64_t uid )
{
    for( const tripoint_bub_ms &q : here.points_in_radius( me, 1 ) ) {
        size_t looked = 0;
        for( item &it : here.i_at( q ) ) {
            if( looked++ >= max_cook_scan ) {
                break;
            }
            if( it.is_craft() && read_cook_pot( it ) == uid ) {
                return item_location( map_cursor( q ), &it );
            }
        }
    }
    return item_location();
}

void capture_cookware( map &here, const tripoint_bub_ms &p, uint8_t tile, submap_view &sm,
                       std::unordered_set<uint64_t> &seen, const avatar &u )
{
    const tripoint_bub_ms me = u.pos_bub();
    size_t index = 0;
    for( item &it : here.i_at( p ) ) {
        const size_t at = index++;
        if( sm.cookware.size() >= max_cookware_per_submap ) {
            return;
        }
        if( !is_cookware( it ) ) {
            continue;
        }
        cookware_view cw;
        cw.tile = tile;
        // The pile listed the first max_handles_per_tile items in this capture already: same uid.
        const uint64_t listed = at < max_handles_per_tile ? read_item_handle( it ) : 0;
        cw.uid = listed != 0 ? listed : item_handle( it, &seen );
        cw.type = type_index( it );
        const auto take_craft = [&cw]( const item & c ) {
            const recipe &making = c.get_making();
            if( making.is_null() ) {
                return;
            }
            cw.craft_recipe = cap_utf8( making.ident().str(), 128 );
            cw.craft_result = making.result().is_null() ? 0 : intern_id( id_kind::itype, making.result().str() );
            // CDDA's craft progress: item_counter of 10,000,000 = done.
            cw.craft_progress = static_cast<uint16_t>( std::clamp( c.item_counter / 10000, 0, 1000 ) );
        };
        size_t visited = 0;
        for( item *in : it.all_items_top( pocket_type::CONTAINER ) ) {
            if( visited++ >= 256 ) {
                break;
            }
            if( cw.contents.size() < max_cookware_contents ) {
                cookware_item_view c;
                c.uid = item_handle( *in, &seen );
                c.type = type_index( *in );
                c.count = in->count_by_charges() ? clamp_i32( in->charges ) : 1;
                cw.contents.push_back( c );
            }
            if( cw.craft_recipe.empty() && in->is_craft() ) {
                take_craft( *in );
            }
        }
        // A craft the `cook` action started lies by the avatar (its square or one next to it), next to its pot,
        // naming it.
        if( cw.craft_recipe.empty() && cw.uid != 0 && square_dist( me, p ) <= 1 ) {
            if( const item_location linked = find_cook_craft( here, me, cw.uid ) ) {
                take_craft( *linked );
            }
        }
        sm.cookware.push_back( std::move( cw ) );
    }
}

double heat_ms_total = 0;
size_t heat_candidates_total = 0;
std::atomic<double> last_query_ms_value{ 0 };
std::atomic<size_t> last_query_checked{ 0 };

struct match {
    const recipe *r = nullptr;
    int batch_max = 1;
    int64_t time_s = 0;
    bool known = false;
    bool skills_ok = false;
    bool tools_ok = false;
    bool can_start = false;
    size_t used = 0;                              // distinct content types the recipe takes
    std::vector<std::string> missing;
};

void add_missing( std::vector<std::string> &missing, const std::string &text )
{
    const std::string t = plain( text, max_missing_text );
    if( !t.empty() && missing.size() < max_match_missing &&
        std::find( missing.begin(), missing.end(), t ) == missing.end() ) {
        missing.push_back( t );
    }
}

// "fire or hotplate or ..." for a tool group none of which is there.
std::string tool_group_text( const std::vector<tool_comp> &group )
{
    std::string text;
    size_t shown = 0;
    for( const tool_comp &t : group ) {
        if( shown == 3 ) {
            text += " or ...";
            break;
        }
        text += ( shown++ == 0 ? "" : " or " ) + item::nname( t.type );
    }
    return text;
}

void write_match( JsonOut &jo, const match &m )
{
    jo.start_object();
    jo.member( "recipe", m.r->ident().str() );
    jo.member( "result", m.r->result().str() );
    jo.member( "name", plain( m.r->result_name(), max_match_name_bytes ) );
    jo.member( "batch_max", m.batch_max );
    jo.member( "time_s", m.time_s );
    jo.member( "known", m.known );
    jo.member( "skills_ok", m.skills_ok );
    jo.member( "tools_ok", m.tools_ok );
    jo.member( "missing" );
    jo.start_array();
    for( const std::string &s : m.missing ) {
        jo.write( s );
    }
    jo.end_array();
    jo.member( "can_start", m.can_start );
    jo.end_object();
}

std::string result_json( uint64_t query_id, uint64_t container, const std::vector<match> &found, size_t n )
{
    std::ostringstream os;
    JsonOut jo( os );
    jo.start_object();
    jo.member( "id", static_cast<int64_t>( query_id ) );
    jo.member( "ok", true );
    jo.member( "error", std::string() );
    jo.member( "container", static_cast<int64_t>( container ) );
    jo.member( "matches" );
    jo.start_array();
    for( size_t i = 0; i < n && i < found.size(); i++ ) {
        write_match( jo, found[i] );
    }
    jo.end_array();
    jo.end_object();
    return os.str();
}

// What a container holds, as recipe components: its first max_query_contents items (one level), and every type in
// them.
struct contents_view {
    inventory comps;
    std::vector<itype_id> types;
};

contents_view read_contents( const item &cont )
{
    contents_view out;
    size_t taken = 0;
    for( const item *in : cont.all_items_top( pocket_type::CONTAINER ) ) {
        if( taken++ >= max_query_contents ) {
            break;
        }
        out.comps.add_item( *in, true, false );
        in->visit_items( [&]( item * e, item * ) {
            if( out.types.size() >= max_query_content_types ) {
                return VisitResponse::ABORT;
            }
            if( std::find( out.types.begin(), out.types.end(), e->typeId() ) == out.types.end() ) {
                out.types.push_back( e->typeId() );
            }
            return VisitResponse::NEXT;
        } );
    }
    return out;
}

// The cooking recipes that take one of `types` as a component.
std::vector<recipe_id> candidates_for( const std::vector<itype_id> &types )
{
    const recipe_index &idx = recipes_now();
    std::vector<recipe_id> candidates;
    std::unordered_set<recipe_id> listed;
    for( const itype_id &t : types ) {
        const auto found = idx.by_component.find( t );
        if( found == idx.by_component.end() ) {
            continue;
        }
        for( const recipe_id &rid : found->second ) {
            if( candidates.size() < max_query_candidates && listed.insert( rid ).second ) {
                candidates.push_back( rid );
            }
        }
    }
    return candidates;
}

// Tools and qualities: the avatar's crafting inventory plus the container emptied (as CDDA's crafting_inventory
// adds a carried non-empty container's empty copy, and BOIL needs an empty one). Built once, when first needed.
struct tool_source {
    const avatar &u;
    const item &cont;
    std::optional<inventory> inv;
    inventory &get() {
        if( !inv ) {
            // Item by item, not a copy: a copied inventory keeps the original's quality cache, which add_item
            // doesn't reset (a cached "no BOIL" would hide the emptied pot).
            inv.emplace();
            for( const std::list<item> *stack : u.crafting_inventory().const_slice() ) {
                for( const item &it : *stack ) {
                    inv->add_item( it, true, false, false );
                }
            }
            inv->add_item( item( cont.typeId(), cont.birthday() ), true, false, false );
        }
        return *inv;
    }
};

struct alt_choice {
    const requirement_data *alt = nullptr;
    std::optional<requirement_data> comps;        // its components only
    bool tools_ok = false;
};

// The alternative of `r` whose components the contents cover `batch` times, one whose tools are there if any.
alt_choice choose_alternative( const recipe &r, const contents_view &cv,
                               const std::function<bool( const item & )> &filter, tool_source &tools, int batch )
{
    alt_choice chosen;
    for( const requirement_data &alt : r.deduped_requirements().alternatives() ) {
        if( alt.get_components().empty() ) {
            continue;
        }
        // Quick: every component group has a type that is in the container.
        const bool possible = std::all_of( alt.get_components().begin(), alt.get_components().end(),
        [&]( const std::vector<item_comp> &group ) {
            return std::any_of( group.begin(), group.end(), [&]( const item_comp & ic ) {
                return std::find( cv.types.begin(), cv.types.end(), ic.type ) != cv.types.end();
            } );
        } );
        if( !possible ) {
            continue;
        }
        requirement_data comps_only( requirement_data::alter_tool_comp_vector(),
                                     requirement_data::alter_quali_req_vector(), alt.get_components() );
        if( !comps_only.can_make_with_inventory( cv.comps, filter, batch ) ) {
            continue;
        }
        requirement_data tools_only( alt.get_tools(), alt.get_qualities(),
                                     requirement_data::alter_item_comp_vector() );
        const bool tools_ok = tools_only.can_make_with_inventory( tools.get(), filter, batch,
                              craft_flags::start_only );
        if( chosen.alt == nullptr || ( tools_ok && !chosen.tools_ok ) ) {
            chosen.alt = &alt;
            chosen.comps = std::move( comps_only );
            chosen.tools_ok = tools_ok;
        }
        if( tools_ok ) {
            break;
        }
    }
    return chosen;
}
} // namespace

void capture_heat_and_cookware( map &here, const avatar &u, const tripoint_bub_ms &corner, submap_view &sm,
                                std::unordered_set<uint64_t> &seen )
{
    const auto t0 = std::chrono::steady_clock::now();
    const heat_tables &h = heat_now();
    for( int i = 0; i < sm_tiles && sm.heat.size() < max_heat_per_submap; i++ ) {
        if( sm.vis[i] != vis_seen_lit ) {
            continue;
        }
        // Cheap tests first: a fire, a heat furniture, a vehicle part with a heat tool.
        const tripoint_bub_ms p = corner + tripoint_rel_ms( i % sm_size, i / sm_size, 0 );
        const int fire = here.get_field_intensity( p, fd_fire );
        const bool fire_here = fire > 0 || here.has_flag_ter_or_furn( ter_furn_flag::TFLAG_USABLE_FIRE, p );
        itype_id tool = itype_id::NULL_ID();
        uint8_t source = heat_from_fire;
        if( sm.furn[i] < h.furn_tool.size() && !h.furn_tool[sm.furn[i]].is_null() ) {
            tool = h.furn_tool[sm.furn[i]];
            source = heat_from_furniture;
        } else if( sm.flags[i] & tile_has_vehicle ) {
            tool = vehicle_heat_tool( here, p, h );
            source = tool.is_null() ? heat_from_fire : heat_from_vehicle;
        }
        if( tool.is_null() && !fire_here ) {
            continue;
        }
        // CDDA's crafting inventory of this square alone: what crafting would get from it.
        heat_candidates_total++;
        inventory there;
        there.form_from_map( &here, p, 0, &u, false, false );
        heat_view hv;
        hv.tile = static_cast<uint8_t>( i );
        hv.source = source;
        hv.fire = static_cast<uint8_t>( std::clamp( fire, 0, 3 ) );
        bool tool_usable = false;
        if( !tool.is_null() ) {
            hv.tool = intern_id( id_kind::itype, tool.str() );
            hv.charges = clamp_i32( there.charges_of( tool ) );
            const auto presence = h.tools.find( tool );
            tool_usable = there.has_tools( tool, 1 ) &&
                          ( hv.charges > 0 || ( presence != h.tools.end() && presence->second ) );
        }
        hv.lit = there.has_tools( itype_fire, 1 ) || tool_usable;
        sm.heat.push_back( hv );
        // What is in or on it only as CDDA shows it (an oven is a container: seen into from next to it).
        if( here.could_see_items( p, u.pos_bub() ) ) {
            capture_cookware( here, p, hv.tile, sm, seen, u );
        }
    }
    heat_ms_total += std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
}

double take_heat_capture_ms( size_t *candidates )
{
    const double ms = heat_ms_total;
    if( candidates != nullptr ) {
        *candidates = heat_candidates_total;
    }
    heat_ms_total = 0;
    heat_candidates_total = 0;
    return ms;
}

std::string recipe_query_error_json( uint64_t query_id, uint64_t container, const std::string &error )
{
    std::ostringstream os;
    JsonOut jo( os );
    jo.start_object();
    jo.member( "id", static_cast<int64_t>( query_id ) );
    jo.member( "ok", false );
    jo.member( "error", error );
    jo.member( "container", static_cast<int64_t>( container ) );
    jo.member( "matches" );
    jo.start_array();
    jo.end_array();
    jo.end_object();
    return os.str();
}

double last_recipe_query_ms( size_t *checked )
{
    if( checked != nullptr ) {
        *checked = last_query_checked.load();
    }
    return last_query_ms_value.load();
}

std::string match_recipes_json( avatar &u, uint64_t query_id, const item_ref &c, const tripoint_abs_ms &pos )
{
    const auto t0 = std::chrono::steady_clock::now();
    const auto refuse = [&]( const char *why ) {
        last_query_ms_value = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
        last_query_checked = 0;
        return recipe_query_error_json( query_id, c.uid, why );
    };
    map &here = get_map();
    const tripoint_bub_ms p = here.get_bub( pos );
    const tripoint_bub_ms me = u.pos_bub( here );
    // Reach first (as CDDA's crafting inventory: PICKUP_RANGE, a clear path), so a query can't probe squares the
    // avatar can't reach for items.
    if( !here.inbounds( p ) || p.z() != me.z() ) {
        return refuse( "out of reach" );
    }
    if( p != me ) {
        const std::vector<tripoint_bub_ms> reach = here.reachable_flood_steps( me, PICKUP_RANGE, 1, 100 );
        if( std::find( reach.begin(), reach.end(), p ) == reach.end() || !here.accessible_items( p ) ) {
            return refuse( "out of reach" );
        }
    }
    const item *cont = nullptr;
    for( const item &it : here.i_at( p ) ) {
        if( matches( it, c ) ) {
            cont = &it;
            break;
        }
    }
    if( cont == nullptr && p == me ) {
        for( const item_location &loc : u.all_items_loc() ) {
            if( loc && matches( *loc, c ) ) {
                cont = loc.get_item();
                break;
            }
        }
    }
    if( cont == nullptr ) {
        return refuse( "not found" );
    }
    if( !cont->is_container() ) {
        return refuse( "not a container" );
    }

    // Components: the contents only. Recipes: those that take one of them as a component and need heat.
    const contents_view cv = read_contents( *cont );
    const std::vector<recipe_id> candidates = candidates_for( cv.types );
    tool_source tools{ u, *cont, std::nullopt };
    std::optional<std::unordered_set<const recipe *>> available;
    std::vector<match> found;
    size_t checked = 0;
    for( const recipe_id &rid : candidates ) {
        if( !rid.is_valid() ) {
            continue;
        }
        const recipe &r = rid.obj();
        if( r.never_learn ) {
            continue;
        }
        checked++;
        const std::function<bool( const item & )> filter = r.get_component_filter();
        const alt_choice choice = choose_alternative( r, cv, filter, tools, 1 );
        const requirement_data *chosen = choice.alt;
        if( chosen == nullptr ) {
            continue;
        }
        match m;
        m.r = &r;
        m.tools_ok = choice.tools_ok;
        while( m.batch_max < max_recipe_batch &&
               choice.comps->can_make_with_inventory( cv.comps, filter, m.batch_max + 1 ) ) {
            m.batch_max++;
        }
        for( const std::vector<item_comp> &group : chosen->get_components() ) {
            for( const itype_id &t : cv.types ) {
                if( std::any_of( group.begin(), group.end(), [&t]( const item_comp & ic ) {
                return ic.type == t;
            } ) ) {
                    m.used++;
                }
            }
        }
        if( !available ) {
            const recipe_subset &group = u.get_group_available_recipes();
            available.emplace( group.begin(), group.end() );
        }
        m.known = available->count( &r ) != 0;
        // Skills as CDDA's craft menu shows them; lacking them CDDA still lets you craft (a failure chance).
        m.skills_ok = true;
        if( !r.skill_used.is_null() && u.get_skill_level( r.skill_used ) < r.get_difficulty( u ) ) {
            m.skills_ok = false;
            add_missing( m.missing, r.skill_used->name() + " " + std::to_string( r.get_difficulty( u ) ) );
        }
        for( const auto &[skill, level] : r.required_skills ) {
            if( u.get_skill_level( skill ) < level ) {
                m.skills_ok = false;
                add_missing( m.missing, skill->name() + " " + std::to_string( level ) );
            }
        }
        // Required proficiencies do block a craft in CDDA.
        const bool profs_ok = r.character_has_required_proficiencies( u );
        if( !profs_ok ) {
            for( const proficiency_id &prof : r.required_proficiencies() ) {
                if( !u.has_proficiency( prof ) ) {
                    add_missing( m.missing, prof->name() );
                }
            }
        }
        if( !m.tools_ok ) {
            // The availability CDDA's check left on this copy says which groups are missing.
            requirement_data tools_only( chosen->get_tools(), chosen->get_qualities(),
                                         requirement_data::alter_item_comp_vector() );
            tools_only.can_make_with_inventory( tools.get(), filter, 1, craft_flags::start_only );
            for( const std::vector<tool_comp> &group : tools_only.get_tools() ) {
                if( std::none_of( group.begin(), group.end(), []( const tool_comp & t ) {
                return t.available == available_status::a_true;
            } ) ) {
                    add_missing( m.missing, tool_group_text( group ) );
                }
            }
            for( const std::vector<quality_requirement> &group : tools_only.get_qualities() ) {
                for( const quality_requirement &q : group ) {
                    if( q.available != available_status::a_true && q.type.is_valid() ) {
                        add_missing( m.missing, q.type->name.translated() + " " + std::to_string( q.level ) );
                    }
                }
            }
        }
        m.can_start = m.known && m.tools_ok && profs_ok;
        // CDDA's time for this avatar now: crafting speed (light, morale, limbs, pain) and helpers included.
        m.time_s = ( u.expected_time_to_craft( r, 1 ) + 99 ) / 100;
        found.push_back( std::move( m ) );
    }

    std::sort( found.begin(), found.end(), []( const match & a, const match & b ) {
        if( a.can_start != b.can_start ) {
            return a.can_start;
        }
        if( a.used != b.used ) {
            return a.used > b.used;
        }
        if( a.batch_max != b.batch_max ) {
            return a.batch_max > b.batch_max;
        }
        return a.r->ident().str() < b.r->ident().str();
    } );
    size_t n = std::min( found.size(), max_recipe_matches );
    std::string out = result_json( query_id, c.uid, found, n );
    while( out.size() > max_query_result_bytes && n > 0 ) {
        n--;
        out = result_json( query_id, c.uid, found, n );
    }
    last_query_ms_value = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
    last_query_checked = checked;
    return out;
}

std::string run_recipe_query( uint64_t query_id, const item_ref &c, const ui_point &pos )
{
    return match_recipes_json( get_avatar(), query_id, c, tripoint_abs_ms( pos.x, pos.y, pos.z ) );
}

std::string start_cooking( avatar &u, const item_action &a )
{
    map &here = get_map();
    const tripoint_bub_ms p = here.get_bub( tripoint_abs_ms( a.pos.x, a.pos.y, a.pos.z ) );
    const tripoint_bub_ms me = u.pos_bub( here );
    // The pot lies on the avatar's square or one next to it (where CDDA works on a craft from).
    if( !here.inbounds( p ) || p.z() != me.z() || square_dist( p, me ) > 1 || !here.accessible_items( p ) ) {
        return _( "You can't reach that from here." );
    }
    item *pot = nullptr;
    for( item &it : here.i_at( p ) ) {
        if( matches( it, a.item ) ) {
            pot = &it;
            break;
        }
    }
    if( pot == nullptr ) {
        return _( "That isn't there any more." );
    }
    if( !pot->is_container() ) {
        return _( "Nothing can be cooked in that." );
    }
    // One craft a pot: the one it was cooking (paused, lying by the avatar) goes on, as CDDA's own way on
    // (iuse::craft), instead of a new one.
    if( item_location linked = find_cook_craft( here, me, a.item.uid ) ) {
        const recipe &making = linked->get_making();
        if( making.ident().str() != a.recipe ) {
            return string_format( _( "Something else is cooking in the %s already." ), pot->type_name() );
        }
        const std::string blocked = cook_craft_blocked( u, *linked );
        if( !blocked.empty() ) {
            return blocked;
        }
        if( u.is_driving() ) {
            return _( "You can't craft while driving!" );
        }
        if( !u.can_continue_craft( *linked ) ) {
            return _( "You can't go on with that now." );
        }
        if( !u.has_recipe( &making ) ) {
            return string_format( _( "You don't know the recipe for the %s and can't continue crafting." ),
                                  making.result_name() );
        }
        u.add_msg_if_player( pgettext( "in progress craft", "You start working on the %s." ), linked->tname() );
        u.assign_activity( craft_activity_actor( linked, false ) );
        return std::string();
    }
    // Only a cooking recipe match_recipes would list for these contents. The client's text is compared, never
    // made an id itself.
    const contents_view cv = read_contents( *pot );
    const std::vector<recipe_id> candidates = candidates_for( cv.types );
    const auto named = std::find_if( candidates.begin(), candidates.end(), [&a]( const recipe_id & c ) {
        return c.str() == a.recipe;
    } );
    if( named == candidates.end() || !named->is_valid() || ( *named )->never_learn ) {
        return string_format( _( "Nothing in the %s makes that." ), pot->type_name() );
    }
    const recipe_id rid = *named;
    const recipe &r = rid.obj();
    // CDDA's own checks before a craft (crafting_allowed, craft_command::execute).
    if( u.is_driving() ) {
        return _( "You can't craft while driving!" );
    }
    if( u.morale_crafting_speed_multiplier( r ) <= 0.0f ) {
        return _( "Your morale is too low to craft such a difficult thing…" );
    }
    if( u.lighting_craft_speed_multiplier( r ) <= 0.0f ) {
        return _( "You can't see to craft!" );
    }
    const recipe_subset &known = u.get_group_available_recipes();
    if( std::find( known.begin(), known.end(), &r ) == known.end() ) {
        return _( "You don't know how to make that." );
    }
    if( !r.character_has_required_proficiencies( u ) ) {
        return _( "You don't have the required proficiencies to craft this!" );
    }
    // The pot cooks on its heat only (the craft stops when it leaves it, cook_craft_blocked).
    if( !heat_lit_at( here, u, p ) ) {
        return string_format( _( "The %s has to be on a lit heat source." ), pot->type_name() );
    }
    const std::function<bool( const item & )> filter = r.get_component_filter();
    tool_source tools{ u, *pot, std::nullopt };
    const alt_choice choice = choose_alternative( r, cv, filter, tools, a.batch );
    if( choice.alt == nullptr ) {
        return string_format( _( "There isn't enough in the %s for that." ), pot->type_name() );
    }
    if( !choice.tools_ok ) {
        return _( "You don't have the tools to make that here." );
    }
    // Each component group: the first kind the contents have enough of (never the pot itself).
    std::vector<item_comp> picks;
    for( const std::vector<item_comp> &group : choice.alt->get_components() ) {
        const auto pick = std::find_if( group.begin(), group.end(), [&]( const item_comp & ic ) {
            return ic.type != pot->typeId() && ic.has( cv.comps, filter, a.batch );
        } );
        if( pick == group.end() ) {
            return string_format( _( "There isn't enough in the %s for that." ), pot->type_name() );
        }
        picks.push_back( *pick );
    }
    // Tools before anything is used up: the heat under the pot first, else CDDA's pick (it may ask).
    inventory square_inv;
    square_inv.form_from_map( &here, p, 0, &u, false, false );
    inventory map_inv;
    map_inv.form_from_map( me, PICKUP_RANGE, &u );
    std::vector<comp_selection<tool_comp>> tool_picks;
    for( const std::vector<tool_comp> &group : choice.alt->get_tools() ) {
        comp_selection<tool_comp> sel;
        for( const tool_comp &tc : group ) {
            if( tc.has( square_inv, return_true<item>, a.batch, craft_flags::start_only ) ) {
                sel.use_from = usage_from::map;
                sel.comp = tc;
                break;
            }
        }
        if( sel.use_from == usage_from::none ) {
            sel = u.select_tool_component( group, a.batch, map_inv, true, true, false, []( int charges ) {
                return charges / 20 + charges % 20;
            } );
        }
        if( sel.use_from == usage_from::none || sel.use_from == usage_from::cancel ) {
            return _( "You don't have the tools to make that here." );
        }
        tool_picks.push_back( sel );
    }
    // The components come out of the pot only (CDDA's consume_items would take them from anywhere near).
    const auto take_components = [&]( item & from, item_components & used, std::vector<item_comp> &comps_used ) {
        for( const item_comp &ic : picks ) {
            int qty = ic.count * a.batch;
            std::list<item> taken;
            if( item::count_by_charges( ic.type ) ) {
                from.use_charges( ic.type, qty, taken, p, filter );
            } else {
                from.use_amount( ic.type, qty, taken, filter );
            }
            for( item &it : taken ) {
                used.add( it );
            }
            item_comp total = ic;
            total.count *= a.batch;
            const auto same = std::find_if( comps_used.begin(), comps_used.end(), [&]( const item_comp & c ) {
                return c.type == total.type;
            } );
            if( same != comps_used.end() ) {
                same->count += total.count;
            } else {
                comps_used.push_back( total );
            }
        }
    };
    // The craft is made from a copy of the pot and laid down first: nothing is used up unless it has its place.
    item_components used;
    std::vector<item_comp> comps_used;
    {
        item trial = *pot;
        take_components( trial, used, comps_used );
    }
    // As craft_command::create_in_progress_craft and Character::start_craft.
    item craft( &r, a.batch, used, comps_used );
    craft.set_cached_tool_selections( tool_picks );
    craft.set_tools_to_continue( true );
    craft.set_next_failure_point( u );
    craft.set_owner( u );
    craft.set_var( cook_pot_var, std::to_string( a.item.uid ) );
    // At the avatar's feet only, as CDDA's crafting without a workbench or a free hand: never into the hands (they
    // stay as they are) and never spilled over to a square next to it (the fire would burn it).
    item_location craft_loc = here.add_item_or_charges_ret_loc( me, craft, false );
    if( !craft_loc ) {
        return _( "There's no room for that here." );
    }
    // Placed: the same out of the pot itself (found again, the craft may share its square), then the tools'
    // start charges.
    pot = nullptr;
    for( item &it : here.i_at( p ) ) {
        if( matches( it, a.item ) ) {
            pot = &it;
            break;
        }
    }
    if( pot == nullptr ) {
        craft_loc.remove_item();
        return _( "That isn't there any more." );
    }
    {
        item_components taken;
        std::vector<item_comp> taken_comps;
        take_components( *pot, taken, taken_comps );
    }
    pot->on_contents_changed();
    u.craft_consume_tools( *craft_loc, 1, true );
    u.last_batch = a.batch;
    u.lastrecipe = rid;
    u.assign_activity( craft_activity_actor( craft_loc, false ) );
    u.add_msg_if_player( pgettext( "in progress craft", "You start working on the %s." ), craft_loc->tname() );
    return std::string();
}

std::string cook_craft_blocked( const Character &who, const item &craft )
{
    if( !active() || !who.is_avatar() ) {
        return std::string();
    }
    const uint64_t uid = read_cook_pot( craft );
    if( uid == 0 ) {
        return std::string();
    }
    map &here = get_map();
    tripoint_bub_ms at;
    const item *pot = find_cook_pot( here, who.pos_bub( here ), uid, at );
    if( pot == nullptr ) {
        return _( "What that was cooking in isn't on the heat next to you." );
    }
    if( !heat_lit_at( here, who, at ) ) {
        return string_format( _( "The %s has to be on a lit heat source." ), pot->type_name() );
    }
    return std::string();
}

bool cooked_into_pot( Character &who, const item &craft, item &result )
{
    if( !active() || !who.is_avatar() ) {
        return false;
    }
    const uint64_t uid = read_cook_pot( craft );
    if( uid == 0 ) {
        return false;
    }
    map &here = get_map();
    tripoint_bub_ms at;
    item *pot = find_cook_pot( here, who.pos_bub( here ), uid, at );
    if( pot == nullptr || !pot->is_container() ) {
        return false;
    }
    // CDDA's own filling: its pockets' rules (a liquid never joins another kind) and their room.
    const bool by_charges = result.count_by_charges();
    const int wanted = by_charges ? result.charges : 1;
    const int filled = pot->fill_with( result, wanted );
    if( filled <= 0 ) {
        return false;
    }
    pot->on_contents_changed();
    //~ %1$s: what was cooked, %2$s: the pot
    who.add_msg_if_player( m_good, _( "The %1$s is ready in the %2$s." ), result.type_name( filled ),
                           pot->type_name() );
    if( by_charges ) {
        result.charges -= filled;
        return result.charges <= 0;
    }
    return true;
}

} // namespace fp3d
