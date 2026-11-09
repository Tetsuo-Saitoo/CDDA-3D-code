// FP3D character creation test: the chargen catalog parses and lists CDDA's data, valid choices build the
// avatar, and hostile or invalid choices are refused. Run from cdda/ (~40 s): prints "all checks passed".

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <algorithm>
#include <vector>

#include "avatar.h"
#include "cached_options.h"
#include "debug.h"
#include "fp3d/boot.h"
#include "fp3d/chargen.h"
#include "json.h"
#include "json_loader.h"
#include "mutation.h"
#include "profession.h"
#include "rng.h"
#include "scenario.h"

namespace
{
int failures = 0;

void check( bool ok, const char *what, const std::string &detail = "" )
{
    std::printf( "  %-56s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
    failures += ok ? 0 : 1;
}

std::string choices( const std::string &scen, const std::string &prof, const std::string &extra )
{
    return "{\"scenario\":\"" + scen + "\",\"profession\":\"" + prof + "\"" + extra + "}";
}
} // namespace

int main()
{
    std::setvbuf( stdout, nullptr, _IONBF, 0 );
    test_mode = true;
    setupDebug( DebugOutput::std_err );
    rng_set_engine_seed( 7 );
    try {
        fp3d::boot_new_game_data( "fp3d-chargen", true, false );
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
            JsonValue r2 = json_loader::from_string( fp3d::chargen_catalog_json() );
            JsonObject o2 = r2.get_object();
            o2.allow_omitted_members();
            check( o2.get_string_array( "templates" ).size() == tl.size(), "catalog lists templates" );
        }
        check( !fp3d::apply_chargen( u, "{\"scenario\":" ).empty(), "broken JSON refused" );
        check( !fp3d::apply_chargen( u, "" ).empty(), "empty answer refused" );
        check( !fp3d::apply_chargen( u, std::string( 20000, ' ' ) ).empty(), "oversized answer refused" );
    } catch( const std::exception &e ) {
        std::printf( "error: %s\n", e.what() );
        return EXIT_FAILURE;
    }
    std::printf( "%s (%d failed)\n", failures == 0 ? "all checks passed" : "FAILED", failures );
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
