#include "fp3d/chargen.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include "avatar.h"
#include "character.h"
#include "fp3d/bridge.h"
#include "game_constants.h"
#include "json.h"
#include "json_loader.h"
#include "mutation.h"
#include "options.h"
#include "path_info.h"
#include "player_difficulty.h"
#include "profession.h"
#include "scenario.h"
#include "skill.h"
#include "worldfactory.h"

namespace fp3d
{
namespace
{
// As newcharacter.cpp (its helpers are file-local there).
constexpr std::array<int, 11> skill_costs = { 0, 1, 1, 2, 4, 6, 9, 12, 16, 20, 25 };
constexpr int min_stat = 4;
constexpr int freeform_max_stat = 20;
constexpr size_t max_name_bytes = 64;
constexpr size_t max_list = 512;
constexpr int min_age = 16;
constexpr int max_age = 55;

bool multi_pool()
{
    return get_option<std::string>( "CHARACTER_POINT_POOLS" ) == "multi_pool";
}

int stat_pool()
{
    return 4 * 8 + get_option<int>( "INITIAL_STAT_POINTS" );
}

bool scenario_listed( const scenario &s )
{
    return !s.scen_is_blacklisted();
}

const scenario *find_scenario( const std::string &id )
{
    for( const scenario &s : scenario::get_all() ) {
        if( s.ident().str() == id && scenario_listed( s ) ) {
            return &s;
        }
    }
    return nullptr;
}

bool trait_listed( const mutation_branch &m, const scenario &s )
{
    return !mutation_branch::trait_is_blacklisted( m.id ) && ( m.startingtrait || s.traitquery( m.id ) );
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

void write_profession( JsonOut &jo, const profession &p )
{
    jo.start_object();
    jo.member( "id", p.ident().str() );
    jo.member( "name", p.gender_appropriate_name( true ) );
    jo.member( "name_female", p.gender_appropriate_name( false ) );
    jo.member( "description", p.description( true ) );
    jo.member( "points", p.point_cost() );
    std::vector<std::string> locked;
    for( const trait_and_var &t : p.get_locked_traits() ) {
        locked.push_back( t.trait.str() );
    }
    write_strings( jo, "locked_traits", locked );
    jo.member( "skills" );
    jo.start_object();
    for( const profession::StartingSkill &sk : p.skills() ) {
        jo.member( sk.first.str(), sk.second );
    }
    jo.end_object();
    jo.end_object();
}

int stat_points_used( const Character &u )
{
    int used = 0;
    for( int stat : { u.str_max, u.dex_max, u.int_max, u.per_max } ) {
        used += stat + std::max( 0, stat - HIGH_STAT );
    }
    return used;
}

bool locked_for( const Character &u, const trait_id &t )
{
    bool locked = get_scenario()->is_locked_trait( t ) || u.prof->is_locked_trait( t );
    for( const profession *h : u.hobbies ) {
        locked = locked || h->is_locked_trait( t );
    }
    return locked;
}

int trait_points_used( const Character &u )
{
    int used = 0;
    for( const trait_id &t : u.get_mutations( true ) ) {
        if( !locked_for( u, t ) ) {
            used += t->points;
        }
    }
    return used;
}

int skill_points_used( const Character &u )
{
    int used = get_scenario()->point_cost() + u.prof->point_cost();
    for( const profession *h : u.hobbies ) {
        used += h->point_cost();
    }
    for( const Skill &sk : Skill::skills ) {
        const int lvl = std::clamp<int>( u.get_skill_level( sk.ident() ), 0, skill_costs.size() - 1 );
        used += skill_costs[lvl];
    }
    return used;
}

// newcharacter.cpp's reset_scenario (file-local there).
void reset_to_scenario( avatar &u, const scenario &scen, const profession &prof )
{
    u.random_start_location = true;
    u.str_max = 8;
    u.dex_max = 8;
    u.int_max = 8;
    u.per_max = 8;
    set_scenario( &scen );
    u.prof = &prof;
    u.hobbies.clear();
    u.clear_mutations();
    u.recalc_hp();
    u.empty_skills();
    u.add_traits();
}

constexpr size_t max_templates = 256;

// A template name the client may save under: 1-64 of letters, digits, space, _ - and not starting with a
// space or dot, so it stays one plain file in the templates folder.
bool template_name_ok( const std::string &n )
{
    if( n.empty() || n.size() > max_name_bytes || n.front() == ' ' || n.back() == ' ' ) {
        return false;
    }
    for( const char ch : n ) {
        const unsigned char u = static_cast<unsigned char>( ch );
        if( !( std::isalnum( u ) || ch == ' ' || ch == '_' || ch == '-' ) ) {
            return false;
        }
    }
    return true;
}

pool_type chosen_pool()
{
    return multi_pool() ? pool_type::MULTI_POOL : pool_type::FREEFORM;
}

std::string checked_name( const std::string &raw )
{
    std::string name = sanitize_text( raw, max_name_bytes );
    // Trim; the name becomes a save file name (CDDA encodes it), so keep it plain and non-empty.
    while( !name.empty() && name.back() == ' ' ) {
        name.pop_back();
    }
    size_t start = 0;
    while( start < name.size() && name[start] == ' ' ) {
        ++start;
    }
    return name.substr( start );
}
} // namespace

std::vector<std::string> list_templates()
{
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path dir = fs::u8path( PATH_INFO::templatedir() );
    fs::directory_iterator it( dir, ec );
    size_t seen = 0;
    for( ; !ec && it != fs::directory_iterator() && seen < 4096; it.increment( ec ), ++seen ) {
        std::error_code ec2;
        if( it->symlink_status( ec2 ).type() != fs::file_type::regular || ec2 ) {
            continue;
        }
        const std::string file = it->path().filename().u8string();
        const std::string ext = ".template";
        if( file.size() <= ext.size() || file.compare( file.size() - ext.size(), ext.size(), ext ) != 0 ) {
            continue;
        }
        const std::string name = file.substr( 0, file.size() - ext.size() );
        if( template_name_ok( name ) && out.size() < max_templates ) {
            out.push_back( name );
        }
    }
    std::sort( out.begin(), out.end() );
    return out;
}

std::string chargen_catalog_json()
{
    std::ostringstream os;
    JsonOut jo( os );
    jo.start_object();
    jo.member( "version", 1 );
    jo.member( "pool", get_option<std::string>( "CHARACTER_POINT_POOLS" ) );
    jo.member( "enforce_points", multi_pool() );
    jo.member( "stat_pool", stat_pool() );
    jo.member( "trait_pool", get_option<int>( "INITIAL_TRAIT_POINTS" ) );
    jo.member( "skill_pool", get_option<int>( "INITIAL_SKILL_POINTS" ) );
    jo.member( "max_trait_points", get_option<int>( "MAX_TRAIT_POINTS" ) );
    jo.member( "min_stat", min_stat );
    jo.member( "max_stat", multi_pool() ? MAX_STAT : freeform_max_stat );
    jo.member( "high_stat", HIGH_STAT );
    jo.member( "max_skill", static_cast<int>( skill_costs.size() ) - 1 );
    jo.member( "skill_costs" );
    jo.start_array();
    for( int c : skill_costs ) {
        jo.write( c );
    }
    jo.end_array();
    jo.member( "min_age", min_age );
    jo.member( "max_age", max_age );
    jo.member( "min_height", Character::min_height() );
    jo.member( "max_height", Character::max_height() );
    jo.member( "max_name", static_cast<int>( max_name_bytes ) );

    // Default selection: the generic scenario and its cheapest profession, as CDDA's own screen starts.
    jo.member( "default_scenario", scenario::generic()->ident().str() );
    write_strings( jo, "templates", list_templates() );

    jo.member( "scenarios" );
    jo.start_array();
    for( const scenario &s : scenario::get_all() ) {
        if( !scenario_listed( s ) ) {
            continue;
        }
        jo.start_object();
        jo.member( "id", s.ident().str() );
        jo.member( "name", s.gender_appropriate_name( true ) );
        jo.member( "description", s.description( true ) );
        jo.member( "points", s.point_cost() );
        jo.member( "start_name", s.start_name() );
        jo.member( "default_background", !s.has_flag( "SKIP_DEFAULT_BACKGROUND" ) );
        std::vector<std::string> profs;
        for( const string_id<profession> &p : s.permitted_professions() ) {
            profs.push_back( p.str() );
        }
        write_strings( jo, "professions", profs );
        std::vector<std::string> hobbies;
        for( const string_id<profession> &p : s.permitted_hobbies() ) {
            hobbies.push_back( p.str() );
        }
        write_strings( jo, "backgrounds", hobbies );
        std::vector<std::string> extra;
        std::vector<std::string> forbidden;
        for( const mutation_branch &m : mutation_branch::get_all() ) {
            if( mutation_branch::trait_is_blacklisted( m.id ) ) {
                continue;
            }
            if( !m.startingtrait && s.traitquery( m.id ) ) {
                extra.push_back( m.id.str() );
            }
            if( s.is_forbidden_trait( m.id ) ) {
                forbidden.push_back( m.id.str() );
            }
        }
        write_strings( jo, "extra_traits", extra );
        write_strings( jo, "forbidden_traits", forbidden );
        std::vector<std::string> locked;
        for( const trait_id &t : s.get_locked_traits() ) {
            locked.push_back( t.str() );
        }
        write_strings( jo, "locked_traits", locked );
        jo.end_object();
    }
    jo.end_array();

    jo.member( "professions" );
    jo.start_array();
    for( const profession &p : profession::get_all() ) {
        if( !p.is_hobby() ) {
            write_profession( jo, p );
        }
    }
    jo.end_array();

    jo.member( "backgrounds" );
    jo.start_array();
    for( const string_id<profession> &p : profession::get_all_hobbies() ) {
        write_profession( jo, p.obj() );
    }
    jo.end_array();

    // Every trait some scenario can list; scenario.extra_traits says which non-starting ones it adds.
    jo.member( "traits" );
    jo.start_array();
    for( const mutation_branch &m : mutation_branch::get_all() ) {
        bool listed = false;
        for( const scenario &s : scenario::get_all() ) {
            if( scenario_listed( s ) && trait_listed( m, s ) ) {
                listed = true;
                break;
            }
        }
        if( !listed ) {
            continue;
        }
        jo.start_object();
        jo.member( "id", m.id.str() );
        jo.member( "name", m.name() );
        jo.member( "description", m.desc() );
        jo.member( "points", m.points );
        jo.member( "starting", m.startingtrait );
        std::vector<std::string> cancels;
        for( const trait_id &c : m.cancels ) {
            cancels.push_back( c.str() );
        }
        write_strings( jo, "cancels", cancels );
        write_strings( jo, "types", std::vector<std::string>( m.types.begin(), m.types.end() ) );
        jo.end_object();
    }
    jo.end_array();

    jo.member( "skills" );
    jo.start_array();
    for( const Skill &sk : Skill::skills ) {
        if( sk.is_contextual_skill() ) {
            continue;
        }
        jo.start_object();
        jo.member( "id", sk.ident().str() );
        jo.member( "name", sk.name() );
        jo.member( "description", sk.description() );
        jo.end_object();
    }
    jo.end_array();
    jo.end_object();
    return os.str();
}

std::string apply_chargen( avatar &u, const std::string &choices_json )
{
    if( choices_json.empty() || choices_json.size() > max_answer_json ) {
        return "no choices";
    }
    try {
        const JsonValue jv = json_loader::from_string( choices_json );
        const JsonObject jo = jv.get_object();
        jo.allow_omitted_members();

        const std::string tmpl = jo.get_string( "template", "" );
        if( !tmpl.empty() ) {
            const std::vector<std::string> listed = list_templates();
            if( std::find( listed.begin(), listed.end(), tmpl ) == listed.end() ) {
                return "no template " + sanitize_text( tmpl, 64 );
            }
            u = avatar();
            pool_type pool = pool_type::FREEFORM;
            if( !u.load_template( tmpl, pool ) ) {
                return "cannot load template " + tmpl;
            }
            if( get_scenario() == nullptr || find_scenario( get_scenario()->ident().str() ) == nullptr ) {
                return "the template's scenario isn't available";
            }
            // As avatar::create( TEMPLATE ): recipes are relearnt in game.
            u.forget_all_recipes();
            const std::string name = checked_name( jo.get_string( "name", "" ) );
            if( !name.empty() ) {
                u.name = name;
            }
            if( u.name.empty() ) {
                u.pick_name();
            }
            if( u.name.empty() || u.name.front() == '#' ) {
                return "bad name";
            }
            if( world_generator->active_world != nullptr &&
                world_generator->active_world->save_exists( save_t::from_save_id( u.name ) ) ) {
                return "a character named " + u.name + " already exists in this world";
            }
            u.set_body();
            u.initialize( character_type::CUSTOM );
            return "";
        }

        const scenario *scen = find_scenario( jo.get_string( "scenario", "" ) );
        if( scen == nullptr ) {
            return "unknown scenario";
        }
        const std::string prof_id = jo.get_string( "profession", "" );
        const std::vector<string_id<profession>> permitted = scen->permitted_professions();
        auto pit = std::find_if( permitted.begin(), permitted.end(), [&]( const string_id<profession> &p ) {
            return p.str() == prof_id;
        } );
        if( pit == permitted.end() ) {
            return "that profession isn't allowed in this scenario";
        }

        u = avatar();
        u.prof = profession::generic();
        set_scenario( scenario::generic() );
        u.male = jo.get_bool( "male", true );
        u.randomize_cosmetics();
        reset_to_scenario( u, *scen, pit->obj() );

        // Backgrounds (hobbies). Omitted: CDDA's default background where the scenario allows one.
        if( jo.has_array( "backgrounds" ) ) {
            const std::vector<std::string> ids = jo.get_string_array( "backgrounds" );
            if( ids.size() > max_list ) {
                return "too many backgrounds";
            }
            const std::vector<string_id<profession>> allowed = scen->permitted_hobbies();
            for( const std::string &id : ids ) {
                auto hit = std::find_if( allowed.begin(), allowed.end(), [&]( const string_id<profession> &p ) {
                    return p.str() == id;
                } );
                if( hit == allowed.end() ) {
                    return "background not allowed: " + id;
                }
                u.hobbies.insert( &hit->obj() );
            }
            for( const profession *h : u.hobbies ) {
                for( const trait_and_var &t : h->get_locked_traits() ) {
                    if( !u.has_trait( t.trait ) ) {
                        u.toggle_trait_deps( t.trait );
                    }
                }
            }
        } else if( !scen->has_flag( "SKIP_DEFAULT_BACKGROUND" ) ) {
            u.add_default_background();
        }

        // Stats.
        const int max_stat = multi_pool() ? MAX_STAT : freeform_max_stat;
        const std::array<std::pair<const char *, int *>, 4> stats = { {
                { "str", &u.str_max }, { "dex", &u.dex_max }, { "int", &u.int_max }, { "per", &u.per_max }
            }
        };
        for( const auto &st : stats ) {
            const int v = jo.get_int( st.first, 8 );
            if( v < min_stat || v > max_stat ) {
                return std::string( "stat out of range: " ) + st.first;
            }
            *st.second = v;
        }

        // Traits the player picked (locked ones are already there).
        if( jo.has_array( "traits" ) ) {
            const std::vector<std::string> ids = jo.get_string_array( "traits" );
            if( ids.size() > max_list ) {
                return "too many traits";
            }
            int good = 0;
            int bad = 0;
            for( const std::string &id : ids ) {
                const trait_id t( id );
                if( !t.is_valid() || !trait_listed( t.obj(), *scen ) ) {
                    return "unknown trait: " + id;
                }
                if( u.has_trait( t ) ) {
                    continue;
                }
                if( scen->is_forbidden_trait( t ) ) {
                    return "the scenario forbids " + t->name();
                }
                if( u.prof->is_forbidden_trait( t ) ) {
                    return "the profession forbids " + t->name();
                }
                if( u.has_conflicting_trait( t ) ) {
                    return t->name() + " conflicts with another trait";
                }
                ( t->points > 0 ? good : bad ) += t->points;
                u.toggle_trait_deps( t );
            }
            const int max_trait = get_option<int>( "MAX_TRAIT_POINTS" );
            if( multi_pool() && ( good > max_trait || bad < -max_trait ) ) {
                return "too many trait points";
            }
        }

        // Skills.
        if( jo.has_object( "skills" ) ) {
            const JsonObject sk = jo.get_object( "skills" );
            sk.allow_omitted_members();
            size_t n = 0;
            for( const JsonMember m : sk ) {
                if( ++n > max_list ) {
                    return "too many skills";
                }
                const skill_id id( m.name() );
                if( !id.is_valid() || id->is_contextual_skill() ) {
                    return "unknown skill: " + m.name();
                }
                const int lvl = m.get_int();
                if( lvl < 0 || lvl >= static_cast<int>( skill_costs.size() ) ) {
                    return "skill level out of range: " + m.name();
                }
                u.set_skill_level( id, lvl );
            }
        }

        if( multi_pool() ) {
            const int ps = stat_pool() - stat_points_used( u );
            const int pt = get_option<int>( "INITIAL_TRAIT_POINTS" ) - trait_points_used( u );
            const int pk = get_option<int>( "INITIAL_SKILL_POINTS" ) - skill_points_used( u );
            // newcharacter.cpp's multi_pool: lower pools may borrow from higher ones, not the other way.
            if( ps + std::min( 0, pt + std::min( 0, pk ) ) < 0 || ps + pt + std::min( 0, pk ) < 0 ||
                ps + pt + pk < 0 ) {
                return "not enough points";
            }
        }

        // Description.
        const int age = jo.get_int( "age", 25 );
        if( age < min_age || age > max_age ) {
            return "age out of range";
        }
        u.set_base_age( age );
        const int height = jo.get_int( "height", Character::default_height() );
        if( height < Character::min_height() || height > Character::max_height() ) {
            return "height out of range";
        }
        u.set_base_height( height );
        std::string name = checked_name( jo.get_string( "name", "" ) );
        if( name.empty() ) {
            u.pick_name();
            name = u.name;
        }
        if( name.empty() || name.front() == '#' ) {
            return "bad name";
        }
        u.name = name;
        if( world_generator->active_world != nullptr &&
            world_generator->active_world->save_exists( save_t::from_save_id( name ) ) ) {
            return "a character named " + name + " already exists in this world";
        }
    } catch( const std::exception &e ) {
        return std::string( "bad choices: " ) + sanitize_text( e.what(), 200 );
    }
    u.set_body();
    {
        // As avatar::create: the template is saved before initialize() adds the profession's items.
        std::string save_as;
        try {
            const JsonValue jv = json_loader::from_string( choices_json );
            const JsonObject jo = jv.get_object();
            jo.allow_omitted_members();
            save_as = jo.get_string( "save_template", "" );
        } catch( const std::exception & ) {
            save_as.clear();
        }
        if( !save_as.empty() ) {
            if( !template_name_ok( save_as ) ) {
                return "bad template name";
            }
            u.save_template( save_as, chosen_pool() );
        }
    }
    u.initialize( character_type::CUSTOM );
    return "";
}

bool run_chargen()
{
    const std::string catalog = chargen_catalog_json();
    std::string status;
    for( ;; ) {
        ui_request req;
        req.kind = request_kind::chargen;
        req.context = "CHARGEN";
        req.title = "New character";
        req.text = catalog;
        req.status = status;
        req.allow_cancel = true;
        const ui_answer a = ask( req );
        if( a.cancel ) {
            return false;
        }
        status = apply_chargen( get_avatar(), a.text );
        if( status.empty() ) {
            return true;
        }
    }
}
} // namespace fp3d
