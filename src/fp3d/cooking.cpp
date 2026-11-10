#include "fp3d/cooking.h"

#include <algorithm>
#include <atomic>
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

#include "avatar.h"
#include "calendar.h"
#include "character.h"
#include "coordinates.h"
#include "crafting.h"
#include "field.h"
#include "field_type.h"
#include "fp3d/id_tables.h"
#include "fp3d/item_hands.h"
#include "fp3d/snapshot.h"
#include "game.h"
#include "game_constants.h"
#include "inventory.h"
#include "item.h"
#include "item_location.h"
#include "itype.h"
#include "json.h"
#include "map.h"
#include "mapdata.h"
#include "output.h"
#include "pocket_type.h"
#include "proficiency.h"
#include "recipe.h"
#include "recipe_dictionary.h"
#include "requirements.h"
#include "skill.h"
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
    return cap_utf8( sanitize_text( remove_color_tags( s ), max_bytes * 2 ), max_bytes );
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

void capture_cookware( map &here, const tripoint_bub_ms &p, uint8_t tile, submap_view &sm,
                       std::unordered_set<uint64_t> &seen )
{
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
                const recipe &making = in->get_making();
                if( !making.is_null() ) {
                    cw.craft_recipe = cap_utf8( making.ident().str(), 128 );
                    cw.craft_result = making.result().is_null() ? 0 : intern_id( id_kind::itype,
                                      making.result().str() );
                    // CDDA's craft progress: item_counter of 10,000,000 = done.
                    cw.craft_progress = static_cast<uint16_t>( std::clamp( in->item_counter / 10000, 0, 1000 ) );
                }
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
            capture_cookware( here, p, hv.tile, sm, seen );
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

    // Components: the contents only.
    inventory comps;
    std::vector<itype_id> content_types;
    size_t taken = 0;
    for( const item *in : cont->all_items_top( pocket_type::CONTAINER ) ) {
        if( taken++ >= max_query_contents ) {
            break;
        }
        comps.add_item( *in, true, false );
        in->visit_items( [&]( item * e, item * ) {
            if( content_types.size() >= max_query_content_types ) {
                return VisitResponse::ABORT;
            }
            if( std::find( content_types.begin(), content_types.end(), e->typeId() ) == content_types.end() ) {
                content_types.push_back( e->typeId() );
            }
            return VisitResponse::NEXT;
        } );
    }

    // Recipes that take one of those as a component and need heat.
    const recipe_index &idx = recipes_now();
    std::vector<recipe_id> candidates;
    std::unordered_set<recipe_id> listed;
    for( const itype_id &t : content_types ) {
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

    // Tools and qualities: the avatar's crafting inventory plus the container emptied (as CDDA's
    // crafting_inventory adds a carried non-empty container's empty copy, and BOIL needs an empty one).
    std::optional<inventory> tools;
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
        // The alternative whose components the contents cover (and whose tools are there, if one is).
        const requirement_data *chosen = nullptr;
        std::optional<requirement_data> chosen_comps;
        bool chosen_tools_ok = false;
        for( const requirement_data &alt : r.deduped_requirements().alternatives() ) {
            if( alt.get_components().empty() ) {
                continue;
            }
            // Quick: every component group has a type that is in the container.
            const bool possible = std::all_of( alt.get_components().begin(), alt.get_components().end(),
            [&]( const std::vector<item_comp> &group ) {
                return std::any_of( group.begin(), group.end(), [&]( const item_comp & ic ) {
                    return std::find( content_types.begin(), content_types.end(), ic.type ) != content_types.end();
                } );
            } );
            if( !possible ) {
                continue;
            }
            requirement_data comps_only( requirement_data::alter_tool_comp_vector(),
                                         requirement_data::alter_quali_req_vector(), alt.get_components() );
            if( !comps_only.can_make_with_inventory( comps, filter, 1 ) ) {
                continue;
            }
            if( !tools ) {
                // Item by item, not a copy: a copied inventory keeps the original's quality cache, which
                // add_item doesn't reset (a cached "no BOIL" would hide the emptied pot).
                tools.emplace();
                for( const std::list<item> *stack : u.crafting_inventory().const_slice() ) {
                    for( const item &it : *stack ) {
                        tools->add_item( it, true, false, false );
                    }
                }
                tools->add_item( item( cont->typeId(), cont->birthday() ), true, false, false );
            }
            requirement_data tools_only( alt.get_tools(), alt.get_qualities(),
                                         requirement_data::alter_item_comp_vector() );
            const bool tools_ok = tools_only.can_make_with_inventory( *tools, filter, 1, craft_flags::start_only );
            if( chosen == nullptr || ( tools_ok && !chosen_tools_ok ) ) {
                chosen = &alt;
                chosen_comps = std::move( comps_only );
                chosen_tools_ok = tools_ok;
            }
            if( tools_ok ) {
                break;
            }
        }
        if( chosen == nullptr ) {
            continue;
        }
        match m;
        m.r = &r;
        m.tools_ok = chosen_tools_ok;
        while( m.batch_max < max_recipe_batch &&
               chosen_comps->can_make_with_inventory( comps, filter, m.batch_max + 1 ) ) {
            m.batch_max++;
        }
        for( const std::vector<item_comp> &group : chosen->get_components() ) {
            for( const itype_id &t : content_types ) {
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
            tools_only.can_make_with_inventory( *tools, filter, 1, craft_flags::start_only );
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

} // namespace fp3d
