// FP3D character creation test: the chargen catalog parses and lists CDDA's data, valid choices build the
// avatar, and hostile or invalid choices are refused. Run from cdda/ (~40 s): prints "all checks passed".

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <algorithm>
#include <vector>

#include "avatar.h"
#include "cached_options.h"
#include "debug.h"
#include "fp3d/boot.h"
#include "fp3d/chargen.h"
#include "fp3d/worldgen.h"
#include "mod_manager.h"
#include "path_info.h"
#include "player_difficulty.h"
#include "recipe.h"
#include "recipe_dictionary.h"
#include "worldfactory.h"
#include "json.h"
#include "json_loader.h"
#include "mutation.h"
#include "profession.h"
#include "rng.h"
#include "scenario.h"

namespace
{
namespace fs = std::filesystem;

int failures = 0;

// A mod without dependencies, written to the user mods folder before the game data loads.
const char *const nodeps_mod = "fp3d_test_nodeps";

void check( bool ok, const char *what, const std::string &detail = "" )
{
    std::printf( "  %-56s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
    failures += ok ? 0 : 1;
}

std::string choices( const std::string &scen, const std::string &prof, const std::string &extra )
{
    return "{\"scenario\":\"" + scen + "\",\"profession\":\"" + prof + "\"" + extra + "}";
}

fs::path template_file( const std::string &name )
{
    return fs::u8path( PATH_INFO::templatedir() + name + ".template" );
}

std::string read_file( const fs::path &p )
{
    std::ifstream in( p, std::ios::binary );
    return std::string( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
}

void write_file( const fs::path &p, const std::string &text )
{
    std::ofstream out( p, std::ios::binary | std::ios::trunc );
    out << text;
}

std::string replace_all( std::string s, const std::string &from, const std::string &to )
{
    for( size_t at = s.find( from ); at != std::string::npos; at = s.find( from, at + to.size() ) ) {
        s.replace( at, from.size(), to );
    }
    return s;
}

bool listed( const std::string &name )
{
    const std::vector<std::string> tl = fp3d::list_templates();
    return std::find( tl.begin(), tl.end(), name ) != tl.end();
}

// The catalog's template_choices entry for `name`: its "name", or "(none)" if the entry is missing.
std::string catalog_name_of( const std::string &name )
{
    JsonValue r = json_loader::from_string( fp3d::chargen_catalog_json() );
    JsonObject o = r.get_object();
    o.allow_omitted_members();
    JsonObject tc = o.get_object( "template_choices" );
    tc.allow_omitted_members();
    if( !tc.has_object( name ) ) {
        return "(none)";
    }
    JsonObject one = tc.get_object( name );
    one.allow_omitted_members();
    return one.get_string( "name" );
}

// Template names, the template's own character name, transfer templates, the size cap, the cache and failed
// saves.
void template_checks( avatar &u, const std::string &scen, const std::string &prof )
{
    bool refused = true;
    for( const char *bad : { "nul", "Nul", "CON", "con", "aux", "Prn", "com1", "COM9", "lpt0", "Lpt5" } ) {
        refused = refused && !fp3d::apply_chargen( u, choices( scen, prof, std::string( ",\"save_template\":\"" ) +
                                bad + "\"" ) ).empty();
    }
    check( refused, "device names refused as template names, any case" );
    check( !fp3d::apply_chargen( u, choices( scen, prof, ",\"save_template\":\"Jos\\u00e9\"" ) ).empty(),
           "non-ASCII template name refused" );

    // A save that can't write the file is reported.
    const fs::path dir_tmpl = template_file( "fp3d dir" );
    std::error_code ec;
    fs::create_directories( dir_tmpl, ec );
    std::string why = fp3d::apply_chargen( u, choices( scen, prof, ",\"save_template\":\"fp3d dir\"" ) );
    check( why.find( "cannot save" ) != std::string::npos, "failed template save reported", why );
    fs::remove_all( dir_tmpl, ec );
    for( const fs::directory_entry &e : fs::directory_iterator( fs::u8path( PATH_INFO::templatedir() ), ec ) ) {
        if( e.path().filename().u8string().rfind( "fp3d dir.template.", 0 ) == 0 ) {
            std::error_code ec2;
            fs::remove( e.path(), ec2 );
        }
    }

    // The template's own character name: kept when the game could save under it, otherwise replaced.
    why = fp3d::apply_chargen( u, choices( scen, prof, ",\"name\":\"Templ Person\",\"save_template\":\"fp3d names\"" ) );
    const fs::path names = template_file( "fp3d names" );
    const std::string orig = read_file( names );
    check( why.empty() && orig.find( "\"Templ Person\"" ) != std::string::npos && catalog_name_of( "fp3d names" ) ==
           "Templ Person", "a template's own name is listed", why );
    why = fp3d::apply_chargen( u, "{\"template\":\"fp3d names\"}" );
    check( why.empty() && u.name == "Templ Person", "a template's own name is kept", why );
    // The catalog reads a template again only when its file changes.
    std::error_code tec;
    const fs::file_time_type was = fs::last_write_time( names, tec );
    write_file( names, replace_all( orig, "\"Templ Person\"", "\"Tempo Person\"" ) );
    fs::last_write_time( names, was, tec );
    check( !tec && catalog_name_of( "fp3d names" ) == "Templ Person", "catalog reuses a template read before" );
    fs::last_write_time( names, was + std::chrono::seconds( 2 ), tec );
    check( !tec && catalog_name_of( "fp3d names" ) == "Tempo Person", "catalog reads a changed template again" );
    bool replaced = true;
    std::string seen;
    for( const std::string &bad : {
             std::string( 300, 'x' ), std::string( "Bad\\u0007Name" ), std::string( "Bad\\u0085Name" ),
             std::string( "#hash" ), std::string( "a/b" ), std::string( " edge" )
         } ) {
        write_file( names, replace_all( orig, "\"Templ Person\"", "\"" + bad + "\"" ) );
        const std::string listed_name = catalog_name_of( "fp3d names" );
        why = fp3d::apply_chargen( u, "{\"template\":\"fp3d names\"}" );
        const bool ok = listed_name.empty() && why.empty() && !u.name.empty() && u.name.size() <= 64 &&
                        u.name.find_first_of( "\x07/#" ) == std::string::npos &&
                        u.name.find( "\xc2\x85" ) == std::string::npos && u.name.front() != ' ';
        if( !ok ) {
            seen += bad.substr( 0, 16 ) + " -> " + u.name.substr( 0, 16 ) + " (" + why + ") ";
        }
        replaced = replaced && ok;
    }
    check( replaced, "a template's unsafe name: not listed, a random one", seen );
    why = fp3d::apply_chargen( u, "{\"template\":\"fp3d names\",\"name\":\"" + std::string( 100, 'y' ) + "\"}" );
    check( why.empty() && u.name == std::string( 64, 'y' ), "a long typed name is cut to 64", why );

    // Bigger templates than the cap are neither listed nor loaded.
    const fs::path big = template_file( "fp3d big" );
    write_file( big, orig );
    const bool small_listed = listed( "fp3d big" );
    write_file( big, orig + std::string( 5 * 1024 * 1024, ' ' ) );
    check( small_listed && !listed( "fp3d big" ) &&
           !fp3d::apply_chargen( u, "{\"template\":\"fp3d big\"}" ).empty() &&
           catalog_name_of( "fp3d big" ) == "(none)", "oversized template skipped" );

    // A transfer template ("Character to Template") loads as it was saved: no new setup, recipes kept.
    std::string sprof;
    for( const string_id<profession> &p : scenario::generic()->permitted_professions() ) {
        if( !p->skills().empty() && p->ma_choices().empty() ) {
            sprof = p.str();
            break;
        }
    }
    why = fp3d::apply_chargen( u, choices( scen, sprof, ",\"name\":\"Transfer Person\"" ) );
    const skill_id sk = sprof.empty() ? skill_id( "melee" ) : profession_id( sprof )->skills().front().first;
    const int lvl = static_cast<int>( u.get_skill_level( sk ) );
    u.cash = 4242;
    const recipe *rec = nullptr;
    for( const auto &e : recipe_dict ) {
        if( !e.second.is_practice() && !u.knows_recipe( &e.second ) ) {
            rec = &e.second;
            break;
        }
    }
    if( rec != nullptr ) {
        u.learn_recipe( rec, true );
    }
    u.save_template( "fp3d transfer", pool_type::TRANSFER );
    const bool learnt = rec != nullptr && u.knows_recipe( rec );
    std::string why2 = fp3d::apply_chargen( u, "{\"template\":\"fp3d transfer\",\"name\":\"Moved Person\"}" );
    check( why.empty() && why2.empty() && lvl > 0 && learnt && u.name == "Moved Person" && u.cash == 4242 &&
           static_cast<int>( u.get_skill_level( sk ) ) == lvl && u.knows_recipe( rec ),
           "transfer template loads as saved", why + why2 + " " + sprof + " " + sk.str() + " " +
           std::to_string( lvl ) + "/" + std::to_string( static_cast<int>( u.get_skill_level( sk ) ) ) );
    why = fp3d::apply_chargen( u, "{\"template\":\"fp3d transfer\"}" );
    check( why.empty() && u.name == "Transfer Person", "transfer template keeps its name", why );

    for( const char *t : { "fp3d names", "fp3d big", "fp3d transfer" } ) {
        fs::remove( template_file( t ), ec );
    }
}

// The core mod first, exactly one, and every mod after its dependencies.
bool mods_ordered( const std::vector<mod_id> &mods )
{
    if( mods.empty() || !mods.front()->core ) {
        return false;
    }
    for( size_t i = 0; i < mods.size(); i++ ) {
        if( i > 0 && mods[i]->core ) {
            return false;
        }
        for( const mod_id &d : mods[i]->dependencies ) {
            if( std::find( mods.begin(), mods.begin() + i, d ) == mods.begin() + i ) {
                return false;
            }
        }
    }
    return true;
}
} // namespace

int main()
{
    std::setvbuf( stdout, nullptr, _IONBF, 0 );
    test_mode = true;
    setupDebug( DebugOutput::std_err );
    rng_set_engine_seed( 7 );
    std::error_code mec;
    fs::path mod_dir;
    try {
        fp3d::init_paths( "", "./fp3d_user" );
        mod_dir = fs::u8path( PATH_INFO::user_moddir() ) / nodeps_mod;
        fs::create_directories( mod_dir, mec );
        write_file( mod_dir / "modinfo.json", std::string( "[{\"type\":\"MOD_INFO\",\"id\":\"" ) + nodeps_mod +
                    "\",\"name\":\"FP3D test mod\",\"description\":\"chargen test\",\"category\":\"content\","
                    "\"dependencies\":[]}]" );
        fp3d::boot_new_game_data( "fp3d-chargen", true, false, false, /*replace=*/true );
        const std::string catalog = fp3d::chargen_catalog_json();
        JsonValue root = json_loader::from_string( catalog );
        JsonObject jo = root.get_object();
        jo.allow_omitted_members();
        JsonArray scens = jo.get_array( "scenarios" );
        JsonArray traits = jo.get_array( "traits" );
        check( scens.size() > 5 && jo.get_array( "professions" ).size() > 20 && traits.size() > 50 &&
               jo.get_array( "skills" ).size() > 10, "catalog parses and lists CDDA's data",
               std::to_string( catalog.size() / 1024 ) + " KB" );

        // A valid character in the default scenario with its cheapest profession.
        const std::string scen = scenario::generic()->ident().str();
        std::string prof;
        int best = 1 << 30;
        for( const string_id<profession> &p : scenario::generic()->permitted_professions() ) {
            if( p->point_cost() < best && p->ma_choices().empty() ) {
                best = p->point_cost();
                prof = p.str();
            }
        }
        // Two listed starting traits that conflict, and one that conflicts with neither.
        std::string t1;
        std::string t2;
        std::string lone;
        for( const mutation_branch &m : mutation_branch::get_all() ) {
            if( !m.startingtrait || mutation_branch::trait_is_blacklisted( m.id ) ||
                scenario::generic()->is_forbidden_trait( m.id ) ) {
                continue;
            }
            if( t1.empty() && !m.cancels.empty() && m.cancels[0]->startingtrait &&
                !scenario::generic()->is_forbidden_trait( m.cancels[0] ) ) {
                t1 = m.id.str();
                t2 = m.cancels[0].str();
            }
        }
        for( const mutation_branch &m : mutation_branch::get_all() ) {
            if( m.startingtrait && m.cancels.empty() && m.types.empty() && m.points > 0 &&
                !mutation_branch::trait_is_blacklisted( m.id ) && m.id.str() != t1 && m.id.str() != t2 &&
                !scenario::generic()->is_forbidden_trait( m.id ) ) {
                lone = m.id.str();
                break;
            }
        }
        check( !prof.empty() && !t1.empty() && !lone.empty(), "test data found", prof + " " + t1 + "/" + t2 + " " +
               lone );

        avatar &u = get_avatar();
        std::string why = fp3d::apply_chargen( u, choices( scen, prof,
                                               ",\"str\":10,\"dex\":9,\"int\":8,\"per\":7,\"traits\":[\"" + lone +
                                               "\"],\"skills\":{\"melee\":2},\"name\":\"  Test Person \",\"male\":false,"
                                               "\"age\":30,\"height\":170" ) );
        check( why.empty(), "valid choices accepted", why );
        check( u.prof->ident().str() == prof && get_scenario()->ident().str() == scen,
               "scenario and profession applied" );
        check( u.str_max == 10 && u.dex_max == 9 && u.int_max == 8 && u.per_max == 7, "stats applied" );
        check( u.has_trait( trait_id( lone ) ), "trait applied" );
        check( static_cast<int>( u.get_skill_level( skill_id( "melee" ) ) ) >= 2, "skill applied" );
        check( u.name == "Test Person" && !u.male && u.base_age() == 30 && u.base_height() == 170,
               "name trimmed, gender, age, height applied" );

        const std::string ok_base = choices( scen, prof, "" );
        check( fp3d::apply_chargen( u, ok_base ).empty() && !u.name.empty(), "no name: a random one" );
        check( !fp3d::apply_chargen( u, choices( "no_such_scenario", prof, "" ) ).empty(), "unknown scenario refused" );
        check( !fp3d::apply_chargen( u, choices( scen, "no_such_prof", "" ) ).empty(), "unknown profession refused" );
        check( !fp3d::apply_chargen( u, choices( scen, prof, ",\"str\":99" ) ).empty(), "stat out of range refused" );
        check( !fp3d::apply_chargen( u, choices( scen, prof, ",\"str\":\"x\"" ) ).empty(), "wrong type refused" );
        check( !fp3d::apply_chargen( u, choices( scen, prof, ",\"traits\":[\"" + t1 + "\",\"" + t2 +
                                     "\"]" ) ).empty(), "conflicting traits refused" );
        check( !fp3d::apply_chargen( u, choices( scen, prof, ",\"traits\":[\"no_such_trait\"]" ) ).empty(),
               "unknown trait refused" );
        check( !fp3d::apply_chargen( u, choices( scen, prof, ",\"skills\":{\"melee\":11}" ) ).empty(),
               "skill level out of range refused" );
        check( !fp3d::apply_chargen( u, choices( scen, prof, ",\"backgrounds\":[\"" + prof + "\"]" ) ).empty(),
               "a profession as background refused" );
        check( !fp3d::apply_chargen( u, choices( scen, prof, ",\"age\":3" ) ).empty(), "age out of range refused" );
        check( !fp3d::apply_chargen( u, choices( scen, prof, ",\"name\":\"#raw\"" ) ).empty(), "name with # refused" );
        // Templates: saved with the choices, listed, loaded back with another name.
        why = fp3d::apply_chargen( u, choices( scen, prof, ",\"str\":11,\"traits\":[\"" + lone +
                                   "\"],\"name\":\"Templ Person\",\"save_template\":\"fp3d test\"" ) );
        const std::vector<std::string> tl = fp3d::list_templates();
        check( why.empty() && std::find( tl.begin(), tl.end(), "fp3d test" ) != tl.end(), "template saved and listed",
               why );
        why = fp3d::apply_chargen( u, "{\"template\":\"fp3d test\",\"name\":\"From Template\"}" );
        check( why.empty() && u.name == "From Template" && u.str_max == 11 && u.has_trait( trait_id( lone ) ) &&
               u.prof->ident().str() == prof, "template loads with a new name", why );
        check( !fp3d::apply_chargen( u, "{\"template\":\"../fp3d test\"}" ).empty(), "template path refused" );
        check( !fp3d::apply_chargen( u, "{\"template\":\"nope\"}" ).empty(), "unknown template refused" );
        check( !fp3d::apply_chargen( u, choices( scen, prof, ",\"save_template\":\"../evil\"" ) ).empty(),
               "bad template name refused" );
        {
            // The catalog reads the new template (generic scenario); CDDA's current scenario must stay.
            const scenario *other = nullptr;
            for( const scenario &s : scenario::get_all() ) {
                if( !s.scen_is_blacklisted() && &s != scenario::generic() ) {
                    other = &s;
                    break;
                }
            }
            if( other != nullptr ) {
                set_scenario( other );
            }
            JsonValue r2 = json_loader::from_string( fp3d::chargen_catalog_json() );
            check( other != nullptr && get_scenario() == other, "reading templates keeps the scenario" );
            set_scenario( scenario::generic() );
            JsonObject o2 = r2.get_object();
            o2.allow_omitted_members();
            check( o2.get_string_array( "templates" ).size() == tl.size(), "catalog lists templates" );
        }
        {
            JsonValue r3 = json_loader::from_string( fp3d::chargen_catalog_json() );
            JsonObject o3 = r3.get_object();
            o3.allow_omitted_members();
            JsonObject tc = o3.get_object( "template_choices" );
            tc.allow_omitted_members();
            bool described = false;
            if( tc.has_object( "fp3d test" ) ) {
                JsonObject one = tc.get_object( "fp3d test" );
                one.allow_omitted_members();
                described = one.get_int( "str" ) == 11 && one.get_string( "profession" ) == prof &&
                            one.get_string( "name" ) == "Templ Person";
                const std::vector<std::string> tr = one.get_string_array( "traits" );
                described = described && std::find( tr.begin(), tr.end(), lone ) != tr.end();
            }
            check( described, "catalog describes the template for editing" );
        }
        check( !fp3d::apply_chargen( u, "{\"delete_template\":\"../x\"}" ).empty() &&
               fp3d::list_templates().size() == tl.size(), "deleting an unlisted template refused" );
        why = fp3d::apply_chargen( u, "{\"delete_template\":\"fp3d test\"}" );
        const std::vector<std::string> tl2 = fp3d::list_templates();
        check( why.rfind( "deleted", 0 ) == 0 && std::find( tl2.begin(), tl2.end(), "fp3d test" ) == tl2.end(),
               "template deleted", why );
        template_checks( u, scen, prof );
        check( !fp3d::apply_chargen( u, "{\"scenario\":" ).empty(), "broken JSON refused" );
        check( !fp3d::apply_chargen( u, "" ).empty(), "empty answer refused" );
        check( !fp3d::apply_chargen( u, std::string( 20000, ' ' ) ).empty(), "oversized answer refused" );
        // World setup: settings and mods.
        {
            JsonValue wr = json_loader::from_string( fp3d::world_setup_catalog_json() );
            JsonObject wo = wr.get_object();
            wo.allow_omitted_members();
            check( wo.get_array( "sliders" ).size() >= 3 && wo.get_array( "mods" ).size() > 5,
                   "world setup catalog lists settings and mods" );
            std::string dep_mod;
            for( const mod_id &m : world_generator->get_mod_manager().get_usable_mods() ) {
                if( m.is_valid() && !m->core && !m->obsolete && !m->dependencies.empty() &&
                    m->conflicts.empty() ) {
                    dep_mod = m.str();
                    break;
                }
            }
            options_manager::options_container opts;
            std::vector<mod_id> mods;
            // A slider at its top level, and the first shown bool setting flipped.
            JsonObject sl = wo.get_array( "sliders" ).get_object( 0 );
            sl.allow_omitted_members();
            const std::string sl_id = sl.get_string( "id" );
            const int top = static_cast<int>( sl.get_array( "levels" ).size() ) - 1;
            std::string bool_id;
            bool bool_was = false;
            for( JsonObject o : wo.get_array( "options" ) ) {
                o.allow_omitted_members();
                if( bool_id.empty() && o.get_string( "type" ) == "bool" ) {
                    bool_id = o.get_string( "id" );
                    bool_was = o.get_bool( "value" );
                }
            }
            options_manager::options_container plain = get_options().get_world_defaults();
            for( const option_slider &osl : option_slider::get_all() ) {
                if( osl.context() == "WORLDGEN" ) {
                    osl.apply_opts( osl.id.str() == sl_id ? top : osl.default_level(), plain );
                }
            }
            std::string w = fp3d::apply_world_setup( "{\"sliders\":{\"" + sl_id + "\":" + std::to_string( top ) +
                            "},\"options\":{\"" + bool_id + "\":" + ( bool_was ? "false" : "true" ) + "},"
                            "\"mods\":[\"" + dep_mod + "\"]}", opts, mods );
            bool same = true;
            for( const auto &kv : plain ) {
                if( kv.first != bool_id && opts[kv.first].getValue() != kv.second.getValue() ) {
                    same = false;
                }
            }
            check( w.empty() && same && opts[bool_id].value_as<bool>() != bool_was &&
                   !mods.empty() && mods.front()->core && mods.back().str() == dep_mod,
                   "settings applied, core and dependencies added first", w + " " + dep_mod );
            check( !fp3d::apply_world_setup( "{\"sliders\":{\"" + sl_id + "\":99}}", opts, mods ).empty(),
                   "slider level out of range refused" );
            check( !fp3d::apply_world_setup( "{\"sliders\":{\"nope\":0}}", opts, mods ).empty(),
                   "unknown slider refused" );
            check( !fp3d::apply_world_setup( "{\"options\":{\"CITY_SIZE\":4}}", opts, mods ).empty(),
                   "hidden setting refused (sliders set it)" );
            check( !fp3d::apply_world_setup( "{\"options\":{\"" + bool_id + "\":\"x\"}}", opts, mods ).empty(),
                   "setting of the wrong type refused" );
            check( !fp3d::apply_world_setup( "{\"options\":{\"USE_TILES\":true}}", opts, mods ).empty(),
                   "non-world option refused" );
            check( !fp3d::apply_world_setup( "{\"mods\":[\"no_such_mod\"]}", opts, mods ).empty(),
                   "unknown mod refused" );
            check( !fp3d::apply_world_setup( "{\"mods\":[]}", opts, mods ).empty(), "no core mod refused" );
            check( fp3d::apply_world_setup( "{}", opts, mods ).empty() && !mods.empty(), "defaults when omitted" );
            // A mod without dependencies picked first: the core mod still goes first.
            w = fp3d::apply_world_setup( std::string( "{\"mods\":[\"" ) + nodeps_mod + "\",\"" + dep_mod + "\"]}",
                                         opts, mods );
            check( w.empty() && mods_ordered( mods ) && mods.size() >= 3 && mods[1].str() == nodeps_mod &&
                   mods.back().str() == dep_mod, "core mod first, before mods picked ahead of it", w );
        }
    } catch( const std::exception &e ) {
        std::printf( "error: %s\n", e.what() );
        fs::remove_all( mod_dir, mec );
        return EXIT_FAILURE;
    }
    fs::remove_all( mod_dir, mec );
    std::printf( "%s (%d failed)\n", failures == 0 ? "all checks passed" : "FAILED", failures );
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
