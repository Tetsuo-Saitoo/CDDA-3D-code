// K7 test range test: boots the range (fixed seed) with CDDA's game loop on a worker thread behind the bridge and
// drives it like a client. Checks the zone registry (unique ids), every hub prop, the gallery (every terrain and
// furniture id, working stairs), the range operations (and that bad input is refused, the clock's end, a full square),
// teleport arrival next to a taken or unsafe centre (fields, traps, deep water, open air) and with no free square (from
// a car seat: back in it), teleport and rebuild refused in a vehicle or riding, rebuilding a zone (creatures just
// outside it stay), frozen AI (and that unfreezing lets the hub zombie act),
// the median turn time in the hub and the gallery, and one zone of each other kind (armory, halls, lots, pens, lanes,
// kitchen: what is placed against the zone's slice, and that a rebuild gives the same zone), then a saved range loaded
// again in this process and resumed as the bridge does (resume_test_range), and saves or worlds that must not resume.
// Range operations run on the sim thread, in the hook that runs before each request. Exit code 0 only if every check
// passed.
//
// usage: fp3d-range-test [--all-zones]   (run from the cdda/ folder)
//   --all-zones   also builds every zone once (rebuild_zone from the hub) and checks placed + skipped == count
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif

#include "achievement.h"
#include "avatar.h"
#include "bodypart.h"
#include "cached_options.h"
#include "calendar.h"
#include "coordinates.h"
#include "creature_tracker.h"
#include "debug.h"
#include "do_turn.h"
#include "enums.h"
#include "field_type.h"
#include "fp3d/boot.h"
#include "fp3d/bridge.h"
#include "fp3d/game_access.h"
#include "fp3d/snapshot.h"
#include "fp3d/test_range.h"
#include "game.h"
#include "item.h"
#include "itype.h"
#include "map.h"
#include "mapdata.h"
#include "math_parser_diag_value.h"
#include "monster.h"
#include "mtype.h"
#include "npc.h"
#include "path_info.h"
#include "point.h"
#include "string_formatter.h"
#include "trap.h"
#include "type_id.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "vpart_range.h"
#include "worldfactory.h"

namespace
{
std::mutex job_mutex;
std::function<void()> job;     // run by the sim right before its next request
std::vector<double> snapshot_ms;   // capture + publish time per request (job_mutex)
std::string sim_error;
int failed = 0;
int passed = 0;
// check_reload: the save sim_main loads once the game has quit (set on the sim thread), and what resume_test_range
// said after that load (1 resumed, 0 not, -1 not called).
std::string reload_save;
std::atomic<int> resumed{ -1 };

void check( bool ok, const std::string &what, const std::string &detail = "" )
{
    std::printf( "  %-56s %s %s\n", what.c_str(), ok ? "PASS" : "FAIL", detail.c_str() );
    ( ok ? passed : failed )++;
}

void before_ask()
{
    std::function<void()> j;
    {
        std::lock_guard<std::mutex> lk( job_mutex );
        j.swap( job );
    }
    if( j ) {
        j();
    }
    fp3d::capture_range_info();
    // As the client's hook: the turn times below include a snapshot per request.
    const auto t = std::chrono::steady_clock::now();
    fp3d::publish_snapshot( fp3d::capture_snapshot() );
    const double ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t ).count();
    std::lock_guard<std::mutex> lk( job_mutex );
    snapshot_ms.push_back( ms );
}

void sim_main()
{
    try {
        // A throwaway fp3d- world: replaces the last run's (boot.h, replaceable_world).
        fp3d::boot_new_game_data( "fp3d-range", true, true, false, true );
        fp3d::boot_test_range( 1 );
        fp3d::set_before_ask_hook( before_ask );
        while( !do_turn() ) {
        }
        // check_reload saved and quit: load that save the way the main menu does (as boot_saved_game: the range
        // forgotten, g->setup(), game::load), then resume the range as the bridge does.
        if( g->uquit == QUIT_SAVED && !reload_save.empty() ) {
            std::printf( "sim: saved and quit, loading the save again\n" );
            fp3d::reset_test_range();
            g->setup();
            if( !fp3d_game_access::load_game( *g, save_t::from_save_id( reload_save ) ) ) {
                throw std::runtime_error( "load failed" );
            }
            resumed.store( fp3d::resume_test_range() ? 1 : 0 );
            fp3d::reset_snapshots();
            while( !do_turn() ) {
            }
        }
        sim_error = "game ended";
    } catch( const fp3d::shutdown_signal & ) {
        // normal end: the driver shut the bridge down
    } catch( const std::exception &e ) {
        sim_error = e.what();
    }
    fp3d::shutdown();
    fp3d::shutdown_world();
}

#if defined(_WIN32)
LONG WINAPI on_fatal_exception( EXCEPTION_POINTERS *info )
{
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if( code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO ) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    std::fprintf( stderr, "FATAL: exception 0x%08lx at %p\n", static_cast<unsigned long>( code ),
                  info->ExceptionRecord->ExceptionAddress );
    debug_write_backtrace( std::cerr );
    std::cerr.flush();
    std::_Exit( 3 );
}
#endif

double ms_since( std::chrono::steady_clock::time_point t )
{
    return std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t ).count();
}

// The main loop's next request (id above `newer_than`); anything else asked meanwhile is cancelled and printed.
std::optional<fp3d::ui_request> main_loop( uint64_t newer_than, int timeout_ms )
{
    for( int i = 0; i < 20; i++ ) {
        const std::optional<fp3d::ui_request> r = fp3d::wait_request( timeout_ms, newer_than );
        if( !r || r->timed ) {
            return r;
        }
        std::printf( "    (cancelled a %s request: %.100s%.100s)\n", fp3d::kind_name( r->kind ), r->title.c_str(),
                     r->text.c_str() );
        fp3d::ui_answer a;
        a.id = r->id;
        a.cancel = true;
        if( r->kind == fp3d::request_kind::popup && !r->actions.empty() ) {
            a.action = std::find( r->actions.begin(), r->actions.end(), "NO" ) != r->actions.end() ? "NO" :
                       r->actions.back();
        }
        fp3d::answer( a );
        newer_than = r->id;
    }
    return std::nullopt;
}

// One main-loop action; the next main-loop request (after any activity it started), nullopt if none came.
std::optional<fp3d::ui_request> act( const std::string &action, int timeout_ms = 60000 )
{
    const std::optional<fp3d::ui_request> r0 = main_loop( 0, 20000 );
    if( !r0 ) {
        return std::nullopt;
    }
    fp3d::queue_action( action );
    return main_loop( r0->id, timeout_ms );
}

// Runs `f` on the sim thread right before its next request (a turn of waiting gets it there). False if the sim
// didn't come back to the main loop.
bool on_sim( std::function<void()> f, int timeout_ms = 300000 )
{
    const std::optional<fp3d::ui_request> r0 = main_loop( 0, 20000 );
    if( !r0 ) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lk( job_mutex );
        job = std::move( f );
    }
    fp3d::queue_action( "pause" );
    return main_loop( r0->id, timeout_ms ).has_value();
}

// Median wall time of `n` one-turn waits.
double median_of( std::vector<double> ms )
{
    if( ms.empty() ) {
        return -1.0;
    }
    std::sort( ms.begin(), ms.end() );
    return ( ms[ms.size() / 2] + ms[( ms.size() - 1 ) / 2] ) / 2.0;
}

// Median wall time of `n` one-turn waits, from queuing the wait to the next turn's request: the whole turn, and
// the turn without the snapshot capture (the bridge hook's, the same for any game; not range work).
struct turn_times {
    double total = -1.0;
    double sim = -1.0;
    double snapshot = -1.0;
};
turn_times median_turn_ms( int n )
{
    std::vector<double> total;
    std::vector<double> sim;
    std::vector<double> snap;
    for( int i = 0; i < n; i++ ) {
        const std::optional<fp3d::ui_request> r0 = main_loop( 0, 20000 );
        if( !r0 ) {
            break;
        }
        {
            std::lock_guard<std::mutex> lk( job_mutex );
            snapshot_ms.clear();
        }
        const auto t = std::chrono::steady_clock::now();
        fp3d::queue_action( "pause" );
        if( !main_loop( r0->id, 60000 ) ) {
            break;
        }
        const double ms = ms_since( t );
        double snaps = 0.0;
        {
            std::lock_guard<std::mutex> lk( job_mutex );
            for( const double v : snapshot_ms ) {
                snaps += v;
            }
        }
        total.push_back( ms );
        sim.push_back( ms - snaps );
        snap.push_back( snaps );
    }
    turn_times out;
    if( total.size() == static_cast<size_t>( n ) ) {
        out.total = median_of( total );
        out.sim = median_of( sim );
        out.snapshot = median_of( snap );
    }
    return out;
}

// Budgets: the turn itself 100 ms (the range adds no per-turn work); with the snapshot, the live turn median
// budget of client/tests/snapshot_bench.gd (400 ms).
void check_turn_time( const char *where )
{
    const turn_times t = median_turn_ms( 20 );
    std::printf( "turn time: %s %.1f ms, of which snapshot %.1f ms, the turn without it %.1f ms (medians of 20)\n",
                 where, t.total, t.snapshot, t.sim );
    check( t.sim >= 0 && t.sim < 100.0, std::string( "turn time in the " ) + where + " under 100 ms",
           std::to_string( t.sim ) + " ms without the snapshot" );
    check( t.total >= 0 && t.total < 400.0, std::string( "turn time in the " ) + where +
           " with the snapshot under 400 ms", std::to_string( t.total ) + " ms" );
}

// Zone centres, copied on the sim thread by check_registry (the registry is game state: never read it from here).
std::vector<tripoint_abs_ms> zone_centres;
tripoint_abs_ms centre_of( size_t zone )
{
    return zone < zone_centres.size() ? zone_centres[zone] : tripoint_abs_ms();
}

fp3d::ui_point ui( const tripoint_abs_ms &p )
{
    return fp3d::ui_point{ p.x(), p.y(), p.z() };
}

tripoint_abs_ms offset( const tripoint_abs_ms &c, int x, int y, int z = 0 )
{
    return c + tripoint_rel_ms( x, y, z );
}

// Monsters in the bubble: type, square, hp (sim thread).
using mon_state = std::vector<std::tuple<std::string, tripoint_abs_ms, int>>;
mon_state monsters_now()
{
    mon_state out;
    for( monster &m : g->all_monsters() ) {
        out.emplace_back( m.type->id.str(), m.pos_abs(), m.get_hp() );
    }
    std::sort( out.begin(), out.end() );
    return out;
}

int items_of( const tripoint_abs_ms &p, const std::string &id )
{
    map &here = get_map();
    int n = 0;
    for( const item &it : here.i_at( here.get_bub( p ) ) ) {
        n += it.typeId().str() == id ? it.count() : 0;
    }
    return n;
}

std::string ok_or( const std::string &why )
{
    return why.empty() ? "ok" : "[" + why + "]";
}

// ---- the checks ----

size_t check_registry()
{
    std::printf( "registry:\n" );
    fp3d::range_counts counts;
    std::vector<fp3d::range_zone> zones;
    bool active = false;
    bool frozen = false;
    int zone = -2;
    check( on_sim( [&] {
        counts = fp3d::range_content_counts();
        zones = fp3d::range_zones();
        for( const fp3d::range_zone &z : zones ) {
            zone_centres.push_back( fp3d::zone_centre( z ) );
        }
        active = fp3d::range_active();
        frozen = fp3d::ai_frozen();
        zone = fp3d::current_zone();
    } ), "sim ran a job" );
    std::printf( "    content: %zu guns, %zu other items in %zu categories (%zu melee weapons, %zu throwables), %zu "
                 "vehicle prototypes, %zu monsters, %zu terrain, %zu furniture (%zu emit fields)\n", counts.guns,
                 counts.items, counts.items_per_category.size(), counts.melee, counts.throwables, counts.vehicles,
                 counts.monsters, counts.terrain, counts.furniture, counts.emitters );
    std::string cats;
    for( const auto &[cat, n] : counts.items_per_category ) {
        cats += cat + "=" + std::to_string( n ) + " ";
    }
    std::printf( "    items per category: %s\n", cats.c_str() );
    std::map<std::string, int> per_kind;
    for( const fp3d::range_zone &z : zones ) {
        per_kind[fp3d::zone_kind_name( z.kind )]++;
    }
    std::string kinds;
    for( const auto &[k, n] : per_kind ) {
        kinds += k + "=" + std::to_string( n ) + " ";
    }
    std::printf( "    %zu zones: %s\n", zones.size(), kinds.c_str() );
    check( active && frozen && zone == 0, "range game, AI frozen, player in the hub",
           "zone " + std::to_string( zone ) );
    check( counts.guns > 0 && counts.items > 0 && counts.vehicles > 0 && counts.monsters > 0 && counts.terrain > 0 &&
           counts.furniture > 0, "content counted from the loaded data" );
    bool sorted = zones.size() > 1 && zones[0].id == "hub" && zones[0].built;
    bool spaced = true;
    std::set<std::string> ids;
    for( size_t i = 0; i < zones.size(); i++ ) {
        ids.insert( zones[i].id );
        if( i >= 2 && !( zones[i - 1].id < zones[i].id ) ) {
            sorted = false;
        }
        if( i >= 1 && ( zones[i].centre.x() - zones[i - 1].centre.x() != fp3d::zone_spacing_omt ||
                        zones[i].centre.y() != zones[0].centre.y() ) ) {
            spaced = false;
        }
        if( i >= 1 && zones[i].built ) {
            sorted = false;   // only the hub is built at boot
        }
    }
    check( sorted && ids.size() == zones.size(), "hub first and built, the rest sorted by id, unique" );
    bool ids_ok = true;
    std::string bad_id;
    for( const fp3d::range_zone &z : zones ) {
        bool ok = !z.id.empty() && z.id.size() <= fp3d::max_zone_id;
        for( const char c : z.id ) {
            ok = ok && ( ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '_' || c == '-' );
        }
        if( !ok && bad_id.empty() ) {
            bad_id = z.id;
        }
        ids_ok = ids_ok && ok;
    }
    check( ids_ok, "zone ids: a-z 0-9 _ -, at most max_zone_id characters", bad_id );
    check( spaced, "zones 8 OMT apart on one row" );
    const auto has_kind = [&]( fp3d::zone_kind k, size_t want_at_least ) {
        return static_cast<size_t>( per_kind[fp3d::zone_kind_name( k )] ) >= want_at_least;
    };
    check( has_kind( fp3d::zone_kind::gallery, 1 ) && has_kind( fp3d::zone_kind::armory, 1 ) &&
           has_kind( fp3d::zone_kind::halls, counts.items_per_category.size() ) &&
           has_kind( fp3d::zone_kind::lots, ( counts.vehicles + 24 ) / 25 ) &&
           has_kind( fp3d::zone_kind::pens, ( counts.monsters + 99 ) / 100 ) && has_kind( fp3d::zone_kind::lanes, 1 ) &&
           has_kind( fp3d::zone_kind::kitchen, 1 ), "every kind registered, counts from the data" );
    for( size_t i = 0; i < zones.size(); i++ ) {
        if( zones[i].kind == fp3d::zone_kind::gallery ) {
            return i;
        }
    }
    return 0;
}

// Halls id bases from categories that collide (case, punctuation, past the cap): unique, capped, stable.
void check_zone_ids()
{
    std::printf( "zone ids:\n" );
    const std::string longer( 80, 'a' );
    const std::vector<std::string> cats = {
        "food", "Food", "FOOD", "fo-od", "fo_od", "fo od", "", "!", longer, longer + "b", longer + "c",
        std::string( fp3d::max_hall_id_base, 'z' ), std::string( fp3d::max_hall_id_base, 'z' ) + "Z"
    };
    const std::vector<std::string> bases = fp3d::hall_id_bases( cats );
    std::set<std::string> unique( bases.begin(), bases.end() );
    bool capped = bases.size() == cats.size();
    std::string list;
    for( const std::string &b : bases ) {
        capped = capped && b.size() <= fp3d::max_hall_id_base && b.compare( 0, 6, "halls_" ) == 0;
        list += b.substr( 0, 14 ) + ( b.size() > 14 ? "..(" + std::to_string( b.size() ) + ")" : "" ) + " ";
    }
    check( unique.size() == cats.size() && capped, "hall id bases: colliding categories get unique, capped ids",
           list.substr( 0, 160 ) );
    check( bases.size() >= 3 && bases[0] == "halls_food" && bases[1] == "halls_food-2" && bases[2] == "halls_food-3",
           "hall id bases: the first keeps its id, the rest -2, -3",
           bases.size() >= 3 ? bases[1] + " " + bases[2] : "" );
    check( fp3d::hall_id_bases( cats ) == bases, "hall id bases: the same categories give the same ids" );
}

struct prop_result {
    std::string what;
    bool ok = false;
    std::string detail;
};

// Each prop where it should be (hub_props, kitchen_props) around `s` (sim thread).
std::vector<prop_result> props_at( const tripoint_abs_ms &s, const std::vector<fp3d::hub_prop> &props )
{
    std::vector<prop_result> results;
    map &here = get_map();
    for( const fp3d::hub_prop &p : props ) {
        const tripoint_abs_ms abs = offset( s, p.at.x, p.at.y, p.at.z );
        const tripoint_bub_ms b = here.get_bub( abs );
        prop_result r;
        r.what = p.what;
        switch( p.kind ) {
            case fp3d::prop_kind::terrain:
                r.detail = here.ter( b ).id().str();
                r.ok = r.detail == p.id;
                break;
            case fp3d::prop_kind::furniture:
                r.detail = here.furn( b ).id().str();
                r.ok = r.detail == p.id;
                break;
            case fp3d::prop_kind::field:
                r.ok = here.has_field_at( b, field_type_str_id( p.id ).id() );
                break;
            case fp3d::prop_kind::item: {
                int n = 0;
                bool inside_ok = *p.contents == '\0';
                for( const item &it : here.i_at( b ) ) {
                    if( it.typeId().str() != p.id ) {
                        continue;
                    }
                    n += it.count();
                    if( it.is_gun() ) {
                        inside_ok = inside_ok || it.ammo_remaining() == p.contents_count;
                    } else {
                        for( const item *c : it.all_items_top() ) {
                            inside_ok = inside_ok || ( c->typeId().str() == p.contents &&
                                                       c->charges == p.contents_count );
                        }
                    }
                }
                r.ok = n >= p.count && inside_ok;
                r.detail = std::to_string( n ) + "x " + p.id + ( *p.contents ? std::string( " + " ) + p.contents : "" );
                break;
            }
            case fp3d::prop_kind::monster: {
                const monster *m = get_creature_tracker().creature_at<monster>( b );
                r.ok = m != nullptr && m->type->id.str() == p.id;
                r.detail = m != nullptr ? m->type->id.str() : "nobody";
                break;
            }
            case fp3d::prop_kind::vehicle: {
                const optional_vpart_position vp = here.veh_at( b );
                const int fuel = vp ? vp->vehicle().fuel_left( here, itype_id( "gasoline" ) ) : -1;
                r.ok = vp && vp->vehicle().type.str() == p.id && fuel > 0;
                r.detail = vp ? vp->vehicle().type.str() + ", gasoline " + std::to_string( fuel ) : "none";
                break;
            }
        }
        results.push_back( r );
    }
    return results;
}

void check_hub()
{
    std::printf( "hub:\n" );
    std::vector<prop_result> results;
    bool at_centre = false;
    size_t npcs = 99;
    mon_state mons;
    on_sim( [&] {
        const tripoint_abs_ms s = centre_of( 0 );
        at_centre = get_avatar().pos_abs() == s;
        npcs = 0;
        for( const npc &n : g->all_npcs() ) {
            npcs += n.is_dead() ? 0 : 1;
        }
        mons = monsters_now();
        results = props_at( s, fp3d::hub_props() );
    } );
    for( const prop_result &r : results ) {
        check( r.ok, "hub: " + r.what, r.detail );
    }
    check( results.size() == fp3d::hub_props().size() && results.size() >= 20, "hub: every prop checked",
           std::to_string( results.size() ) );
    check( at_centre && npcs == 0 && mons.size() == 1, "hub: player on the start square, no NPCs, one monster",
           std::to_string( npcs ) + " NPCs, " + std::to_string( mons.size() ) + " monsters" );
}

// Monsters stay put over 10 turns while frozen.
void check_frozen( const char *where )
{
    mon_state before;
    mon_state after;
    on_sim( [&] {
        before = monsters_now();
    } );
    int turns = 0;
    for( int i = 0; i < 10; i++ ) {
        turns += act( "pause" ) ? 1 : 0;
    }
    on_sim( [&] {
        after = monsters_now();
    } );
    check( turns == 10 && !before.empty() && before == after, std::string( where ) +
           ": frozen monsters don't move over 10 turns", std::to_string( before.size() ) + " monsters" );
}

void check_operations( size_t gallery )
{
    std::printf( "operations:\n" );
    const tripoint_abs_ms s = centre_of( 0 );
    const uint32_t rock = fp3d::intern_id( fp3d::id_kind::itype, "rock" );
    const uint32_t ammo = fp3d::intern_id( fp3d::id_kind::itype, "9mm" );
    const uint32_t table_size = static_cast<uint32_t>( fp3d::id_names( fp3d::id_kind::itype ).size() );
    const tripoint_abs_ms rock_sq = offset( s, 2, 2 );
    const tripoint_abs_ms ammo_sq = offset( s, 3, 2 );
    const tripoint_abs_ms outside_sq = offset( s, 500, 0 );

    // Off the main loop (here: this thread, which never asks a timed request) every operation but the freeze is
    // refused before it reads anything.
    std::string off_loop;
    for( const std::string &r : {
             fp3d::spawn_item( rock, 1, ui( rock_sq ) ), fp3d::spawn_monster( "mon_zombie", ui( offset( s, 5, 5 ) ) ),
             fp3d::teleport_to_zone( 0 ), fp3d::set_time_of_day( 600 ), fp3d::advance( 1 ), fp3d::heal(),
             fp3d::rebuild_zone( 1 )
         } ) {
        off_loop += r.empty() ? "[accepted]" : "[" + r + "]";
    }
    check( off_loop.find( "accepted" ) == std::string::npos, "operations refused off the main loop",
           off_loop.substr( 0, 60 ) );

    // spawn_item
    std::vector<std::string> bad;
    std::string good;
    std::string good_ammo;
    int rocks = 0;
    int rounds = 0;
    on_sim( [&] {
        good = fp3d::spawn_item( rock, 5, ui( rock_sq ) );
        good_ammo = fp3d::spawn_item( ammo, 30, ui( ammo_sq ) );
        rocks = items_of( rock_sq, "rock" );
        rounds = items_of( ammo_sq, "9mm" );
        bad.push_back( fp3d::spawn_item( 0, 1, ui( rock_sq ) ) );
        bad.push_back( fp3d::spawn_item( table_size, 1, ui( rock_sq ) ) );
        bad.push_back( fp3d::spawn_item( 0xffffffffu, 1, ui( rock_sq ) ) );
        bad.push_back( fp3d::spawn_item( rock, 0, ui( rock_sq ) ) );
        bad.push_back( fp3d::spawn_item( rock, fp3d::max_spawn_count + 1, ui( rock_sq ) ) );
        bad.push_back( fp3d::spawn_item( rock, -3, ui( rock_sq ) ) );
        bad.push_back( fp3d::spawn_item( rock, 1, ui( outside_sq ) ) );
        bad.push_back( fp3d::spawn_item( rock, 1, fp3d::ui_point{ s.x(), s.y(), 11 } ) );
        bad.push_back( fp3d::spawn_item( rock, 1, fp3d::ui_point{ 1 << 30, -( 1 << 30 ), 0 } ) );
        bad.push_back( fp3d::spawn_item( rock, 1, fp3d::ui_point{ INT32_MIN, INT32_MIN, INT32_MIN } ) );
        // nothing more appeared
        rocks = std::min( rocks, items_of( rock_sq, "rock" ) == rocks ? rocks : -1 );
    } );
    check( good.empty() && rocks == 5, "spawn_item: 5 rocks", ok_or( good ) + " " + std::to_string( rocks ) );
    check( good_ammo.empty() && rounds == 30, "spawn_item: 30 rounds of 9mm in one stack", ok_or( good_ammo ) +
           " " + std::to_string( rounds ) );
    bool all_refused = bad.size() == 10;
    std::string why;
    for( const std::string &b : bad ) {
        all_refused = all_refused && !b.empty();
        why += "[" + b + "]";
    }
    check( all_refused, "spawn_item: bad index, count or square refused", why.substr( 0, 120 ) );

    // A square that holds no more: as many as fit, then "Nothing was placed." (nothing spills next door).
    const uint32_t can = fp3d::intern_id( fp3d::id_kind::itype, "jerrycan" );
    const tripoint_abs_ms full_sq = offset( s, -3, 4 );
    std::string fill;
    std::string full;
    int cans = 0;
    int cans_after = -1;
    int next_door = -1;
    on_sim( [&] {
        fill = fp3d::spawn_item( can, fp3d::max_spawn_count, ui( full_sq ) );
        cans = items_of( full_sq, "jerrycan" );
        full = fp3d::spawn_item( can, 1, ui( full_sq ) );
        cans_after = items_of( full_sq, "jerrycan" );
        next_door = 0;
        for( int dy = -2; dy <= 2; dy++ ) {
            for( int dx = -2; dx <= 2; dx++ ) {
                next_door += dx == 0 && dy == 0 ? 0 : items_of( offset( full_sq, dx, dy ), "jerrycan" );
            }
        }
        map &here = get_map();
        here.i_clear( here.get_bub( full_sq ) );
    } );
    check( fill.empty() && cans > 0 && cans < fp3d::max_spawn_count && next_door == 0,
           "spawn_item: as many as fit, on that square only", ok_or( fill ) + " " + std::to_string( cans ) +
           " jerrycans, " + std::to_string( next_door ) + " next door" );
    check( full == "Nothing was placed." && cans_after == cans, "spawn_item: into a full square refused",
           ok_or( full ) );

    // spawn_monster
    const tripoint_abs_ms mon_sq = offset( s, 4, 4 );
    bad.clear();
    good.clear();
    std::string placed;
    on_sim( [&] {
        good = fp3d::spawn_monster( "mon_zombie", ui( mon_sq ) );
        map &here = get_map();
        const monster *m = get_creature_tracker().creature_at<monster>( here.get_bub( mon_sq ) );
        placed = m != nullptr ? m->type->id.str() : "";
        bad.push_back( fp3d::spawn_monster( "mon_nope", ui( offset( s, 5, 4 ) ) ) );
        bad.push_back( fp3d::spawn_monster( std::string( fp3d::max_monster_id + 1, 'a' ), ui( offset( s, 5, 4 ) ) ) );
        bad.push_back( fp3d::spawn_monster( "mon_null", ui( offset( s, 5, 4 ) ) ) );
        bad.push_back( fp3d::spawn_monster( "../mon_zombie", ui( offset( s, 5, 4 ) ) ) );
        bad.push_back( fp3d::spawn_monster( "", ui( offset( s, 5, 4 ) ) ) );
        bad.push_back( fp3d::spawn_monster( "mon_zombie", ui( outside_sq ) ) );
        bad.push_back( fp3d::spawn_monster( "mon_zombie", ui( offset( s, 15, 3 ) ) ) );   // the wall
        bad.push_back( fp3d::spawn_monster( "mon_zombie", ui( mon_sq ) ) );               // taken
        bad.push_back( fp3d::spawn_monster( "mon_zombie", fp3d::ui_point{ s.x(), s.y(), 99 } ) );
    } );
    check( good.empty() && placed == "mon_zombie", "spawn_monster: a zombie", ok_or( good ) + " " + placed );
    all_refused = bad.size() == 9;
    why.clear();
    for( const std::string &b : bad ) {
        all_refused = all_refused && !b.empty();
        why += "[" + b + "]";
    }
    check( all_refused, "spawn_monster: unknown id, bad square, wall, taken refused", why.substr( 0, 120 ) );
    check_frozen( "hub with a spawned zombie" );
    // The spawned zombie isn't caged: gone before the AI wakes up.
    on_sim( [&] {
        map &here = get_map();
        if( monster *m = get_creature_tracker().creature_at<monster>( here.get_bub( mon_sq ) ) ) {
            g->remove_zombie( *m );
        }
    } );

    // set_time_of_day
    int64_t before_turn = 0;
    int64_t after_turn = 0;
    int64_t same_turn = 0;
    int minute_now = -1;
    std::string same;
    bad.clear();
    good.clear();
    on_sim( [&] {
        before_turn = to_turns<int64_t>( calendar::turn - calendar::turn_zero );
        good = fp3d::set_time_of_day( 600 );
        after_turn = to_turns<int64_t>( calendar::turn - calendar::turn_zero );
        minute_now = to_minutes<int>( time_past_midnight( calendar::turn ) );
        // Again at the same minute, three times: nothing moves (it used to add a day each time).
        for( int i = 0; i < 3; i++ ) {
            const std::string r = fp3d::set_time_of_day( 600 );
            same += r.empty() ? "" : "[" + r + "]";
        }
        same_turn = to_turns<int64_t>( calendar::turn - calendar::turn_zero );
        bad.push_back( fp3d::set_time_of_day( -1 ) );
        bad.push_back( fp3d::set_time_of_day( 1440 ) );
        bad.push_back( fp3d::set_time_of_day( INT32_MIN ) );
        if( to_turns<int64_t>( calendar::turn - calendar::turn_zero ) != after_turn ) {
            bad.push_back( "" );   // a refused call moved the clock
        }
    } );
    check( good.empty() && minute_now == 600 && after_turn > before_turn && after_turn - before_turn <= 24 * 3600,
           "set_time_of_day: 10:00, forward", ok_or( good ) + " minute " + std::to_string( minute_now ) );
    all_refused = bad.size() == 3 && !bad[0].empty() && !bad[1].empty() && !bad[2].empty();
    check( all_refused, "set_time_of_day: out of range refused, clock untouched" );
    check( same.empty() && same_turn == after_turn, "set_time_of_day: the current minute leaves the clock alone",
           same + " " + std::to_string( same_turn - after_turn ) + " turns moved" );

    // The clock's end: half an hour before it, an hour ahead is refused (both ways), the current minute is fine. The
    // clock is put back afterwards (only refusals ran: nothing else saw the far future).
    std::string end_tod;
    std::string end_adv;
    std::string end_same;
    std::string end_past;
    bool end_untouched = false;
    on_sim( [&] {
        const time_point saved = calendar::turn;
        const time_point end = calendar::start_of_game + time_duration::from_days( fp3d::max_range_days );
        calendar::turn = end - 30_minutes;
        const int m = to_minutes<int>( time_past_midnight( calendar::turn ) );
        end_tod = fp3d::set_time_of_day( ( m + 60 ) % 1440 );
        end_adv = fp3d::advance( 60 );
        end_same = fp3d::set_time_of_day( m );
        end_untouched = calendar::turn == end - 30_minutes;
        calendar::turn = end + 1_hours;
        end_past = fp3d::advance( 1 );
        end_untouched = end_untouched && calendar::turn == end + 1_hours;
        calendar::turn = saved;
    } );
    check( !end_tod.empty() && !end_adv.empty() && !end_past.empty() && end_same.empty() && end_untouched,
           "clock: past max_range_days refused, clock untouched", "[" + end_tod + "][" + end_adv + "]" );

    // advance: real turns through CDDA's wait activity
    bad.clear();
    good.clear();
    std::optional<fp3d::ui_request> started;
    on_sim( [&] {
        good = fp3d::advance( 10 );
        bad.push_back( fp3d::advance( 0 ) );
        bad.push_back( fp3d::advance( -5 ) );
        bad.push_back( fp3d::advance( fp3d::max_advance_minutes + 1 ) );
    } );
    started = main_loop( 0, 20000 );
    const auto t0 = std::chrono::steady_clock::now();
    fp3d::queue_action( "pause" );
    const std::optional<fp3d::ui_request> ended = started ? main_loop( started->id, 600000 ) : std::nullopt;
    const double secs = ms_since( t0 ) / 1000.0;
    const int64_t waited = started && ended ? ended->turn - started->turn : -1;
    check( good.empty() && waited >= 600 && waited <= 620, "advance: 10 minutes as real turns",
           ok_or( good ) + " " + std::to_string( waited ) + " turns in " + std::to_string( secs ) + " s" );
    if( waited > 0 ) {
        std::printf( "    advance: %.2f ms per turn; a week (10080 min) would take about %.0f s\n",
                     secs * 1000.0 / waited, secs / waited * 604800.0 );
    }
    all_refused = bad.size() == 3 && !bad[0].empty() && !bad[1].empty() && !bad[2].empty();
    check( all_refused, "advance: 0, negative, more than a week refused" );

    // heal
    bool hurt = false;
    bool healed = false;
    std::string detail;
    on_sim( [&] {
        avatar &u = get_avatar();
        const bodypart_id torso( "torso" );
        u.set_part_hp_cur( torso, 5 );
        u.set_pain( 40 );
        u.set_hunger( 300 );
        u.set_thirst( 300 );
        u.add_effect( efftype_id( "bleed" ), 10_minutes, bodypart_id( "arm_l" ) );
        hurt = u.get_part_hp_cur( torso ) == 5 && u.has_effect( efftype_id( "bleed" ) );
        good = fp3d::heal();
        healed = true;
        for( const bodypart_id &bp : u.get_all_body_parts() ) {
            healed = healed && u.get_part_hp_cur( bp ) == u.get_part_hp_max( bp );
        }
        healed = healed && u.get_pain() == 0 && u.get_hunger() <= 0 && u.get_thirst() <= 0 &&
                 !u.has_effect( efftype_id( "bleed" ) ) && u.get_sleepiness() <= 0;
        detail = "pain " + std::to_string( u.get_pain() ) + " hunger " + std::to_string( u.get_hunger() ) +
                 " thirst " + std::to_string( u.get_thirst() );
    } );
    check( hurt && good.empty() && healed, "heal: hit points, pain, bleeding, hunger, thirst", detail );

    // ai freeze toggles (the unfreeze check below shows it acts)
    bool off = true;
    bool on = false;
    on_sim( [&] {
        fp3d::set_ai_frozen( false );
        off = fp3d::ai_frozen();
        fp3d::set_ai_frozen( true );
        on = fp3d::ai_frozen();
    } );
    check( !off && on, "set_ai_frozen: off and on" );

    // teleport refusals
    bad.clear();
    on_sim( [&] {
        const size_t n = fp3d::range_zones().size();
        bad.push_back( fp3d::teleport_to_zone( n ) );
        bad.push_back( fp3d::teleport_to_zone( static_cast<size_t>( -1 ) ) );
        avatar &u = get_avatar();
        u.remove_value( "fp3d_range" );
        bad.push_back( fp3d::teleport_to_zone( gallery ) );
        u.set_value( "fp3d_range", 1.0 );
        bad.push_back( fp3d::current_zone() == 0 ? "" : "moved" );
    } );
    check( bad.size() == 4 && !bad[0].empty() && !bad[1].empty() && !bad[2].empty() && bad[3].empty(),
           "teleport: index past the end and outside a range refused" );
}

void check_unfreeze()
{
    std::printf( "unfreeze:\n" );
    const tripoint_abs_ms s = centre_of( 0 );
    // The cell's glass ring, as text, to see a bash.
    const auto ring = [&]() {
        map &here = get_map();
        std::string out;
        for( int y = -7; y <= -5; y++ ) {
            for( int x = 6; x <= 10; x++ ) {
                out += here.ter( here.get_bub( offset( s, x, y ) ) ).id().str() + ",";
            }
        }
        return out;
    };
    mon_state before;
    mon_state after;
    std::string ring_before;
    std::string ring_after;
    bool in_view = false;
    on_sim( [&] {
        before = monsters_now();
        ring_before = ring();
        map &here = get_map();
        for( monster &m : g->all_monsters() ) {
            in_view = m.sees( here, get_avatar() ) && get_avatar().sees( here, m );
        }
        fp3d::set_ai_frozen( false );
    } );
    check( in_view, "hub: the caged zombie and the player see each other" );
    for( int i = 0; i < 10; i++ ) {
        act( "pause" );
    }
    on_sim( [&] {
        after = monsters_now();
        ring_after = ring();
        fp3d::set_ai_frozen( true );
    } );
    std::string where;
    for( const auto &m : after ) {
        const tripoint_abs_ms p = std::get<1>( m );
        where += std::get<0>( m ) + " at hub+(" + std::to_string( p.x() - s.x() ) + "," + std::to_string(
                     p.y() - s.y() ) + ") ";
    }
    check( before.size() == 1 && ( before != after || ring_before != ring_after ),
           "unfrozen: the hub zombie acts (moves or bashes)", where + ( ring_before != ring_after ? "glass hit" : "" ) );
}

void check_gallery( size_t gallery )
{
    std::printf( "gallery:\n" );
    std::string tp;
    int zone = -1;
    bool built = false;
    on_sim( [&] {
        tp = fp3d::teleport_to_zone( gallery );
        zone = fp3d::current_zone();
        built = fp3d::range_zones()[gallery].built;
    } );
    check( tp.empty() && zone == static_cast<int>( gallery ) && built, "teleport to the gallery, built",
           ok_or( tp ) + " zone " + std::to_string( zone ) );

    size_t pieces = 0;
    size_t missing = 0;
    std::string missing_ids;
    std::set<std::string> union_ids;
    std::set<std::string> all_ids;
    bool stairs = false;
    on_sim( [&] {
        map &here = get_map();
        const tripoint_abs_ms s = centre_of( gallery );
        const std::vector<fp3d::gallery_piece> list = fp3d::gallery_pieces( gallery );
        pieces = list.size();
        for( const fp3d::gallery_piece &p : list )
        {
            const tripoint_bub_ms b = here.get_bub( offset( s, p.at.x, p.at.y, p.at.z ) );
            const std::string there = p.furniture ? here.furn( b ).id().str() : here.ter( b ).id().str();
            if( there != p.id ) {
                missing++;
                if( missing <= 12 ) {
                    missing_ids += p.id + "(" + there + ") ";
                }
            }
        }
        // Every gallery part together shows every id.
        for( size_t i = 0; i < fp3d::range_zones().size(); i++ )
        {
            for( const fp3d::gallery_piece &p : fp3d::gallery_pieces( i ) ) {
                union_ids.insert( ( p.furniture ? "f:" : "t:" ) + p.id );
            }
        }
        for( int i = 1; i < static_cast<int>( ter_t::count() ); i++ )
        {
            all_ids.insert( "t:" + ter_id( i ).id().str() );
        }
        for( int i = 1; i < static_cast<int>( furn_t::count() ); i++ )
        {
            if( furn_id( i ).id().str() != "f_clear" ) {
                all_ids.insert( "f:" + furn_id( i ).id().str() );
            }
        }
        const fp3d::range_offset st = fp3d::gallery_stairs;
        stairs = here.ter( here.get_bub( offset( s, st.x, st.y, 0 ) ) ).id().str() == "t_stairs_up" &&
                 here.ter( here.get_bub( offset( s, st.x, st.y, 1 ) ) ).id().str() == "t_stairs_down";
    } );
    check( pieces > 0 && missing == 0, "gallery: every piece of this part in place",
           std::to_string( pieces ) + " pieces, " + std::to_string( missing ) + " missing " + missing_ids );
    check( !all_ids.empty() && union_ids == all_ids, "gallery: the parts show every terrain and furniture id",
           std::to_string( union_ids.size() ) + " of " + std::to_string( all_ids.size() ) );
    check( stairs, "gallery: stair pair z0/z1" );

    // Walk onto the stairs and climb them.
    const std::optional<fp3d::ui_request> r1 = act( "RIGHT" );
    std::shared_ptr<const fp3d::snapshot> snap = fp3d::latest_snapshot();
    const tripoint_abs_ms s = centre_of( gallery );
    const fp3d::range_offset st = fp3d::gallery_stairs;
    const bool on_stairs = r1 && snap && snap->player.pos.x == s.x() + st.x && snap->player.pos.y == s.y() + st.y &&
                           snap->player.pos.z == 0;
    check( on_stairs, "gallery: walked onto the stairs", snap ? std::to_string( snap->player.pos.x - s.x() ) + "," +
           std::to_string( snap->player.pos.y - s.y() ) + "," + std::to_string( snap->player.pos.z ) : "" );
    const std::optional<fp3d::ui_request> r2 = act( "LEVEL_UP" );
    snap = fp3d::latest_snapshot();
    check( r2 && snap && snap->player.pos.z == 1, "gallery: LEVEL_UP on the stairs reaches z 1",
           snap ? "z " + std::to_string( snap->player.pos.z ) : "" );
    const std::optional<fp3d::ui_request> r3 = act( "LEVEL_DOWN" );
    snap = fp3d::latest_snapshot();
    check( r3 && snap && snap->player.pos.z == 0, "gallery: LEVEL_DOWN back to z 0",
           snap ? "z " + std::to_string( snap->player.pos.z ) : "" );
}

// Teleport onto a zone whose centre is taken (a monster there): the player lands next to it and lets go of its grab
// and route. The player stands on the gallery's stairs (check_gallery). Leaves rocks on the gallery floor for
// check_rebuild to see cleared.
void check_teleport_arrival( size_t gallery )
{
    std::printf( "teleport arrival:\n" );
    const tripoint_abs_ms c = centre_of( gallery );
    const uint32_t rock = fp3d::intern_id( fp3d::id_kind::itype, "rock" );
    std::string spawned;
    std::string tp;
    std::string mon;
    std::string marker;
    bool grabbed = false;
    bool routed = false;
    bool let_go = false;
    bool no_route = false;
    tripoint_abs_ms at;
    int zone = -1;
    int rocks = -1;
    on_sim( [&] {
        map &here = get_map();
        avatar &u = get_avatar();
        spawned = fp3d::spawn_monster( "mon_zombie", ui( c ) );
        u.grab( object_type::FURNITURE, tripoint_rel_ms( 0, 1, 0 ) );
        grabbed = u.get_grab_type() == object_type::FURNITURE;
        u.set_destination( { here.get_bub( offset( c, 3, 3 ) ) } );
        routed = u.has_destination();
        tp = fp3d::teleport_to_zone( gallery );
        at = u.pos_abs();
        let_go = u.get_grab_type() == object_type::NONE;
        no_route = !u.has_destination();
        zone = fp3d::current_zone();
        map &now = get_map();
        if( monster *m = get_creature_tracker().creature_at<monster>( now.get_bub( c ) ) ) {
            mon = m->type->id.str();
            g->remove_zombie( *m );
        }
        marker = fp3d::spawn_item( rock, 3, ui( offset( c, 2, -2 ) ) );
        rocks = items_of( offset( c, 2, -2 ), "rock" );
    } );
    const int dist = std::max( std::abs( at.x() - c.x() ), std::abs( at.y() - c.y() ) );
    check( spawned.empty() && tp.empty() && mon == "mon_zombie" && dist == 1 && at.z() == c.z() &&
           zone == static_cast<int>( gallery ), "teleport: centre taken, lands on a free square next to it",
           ok_or( spawned ) + " " + ok_or( tp ) + " " + mon + " at centre+(" + std::to_string( at.x() - c.x() ) + "," +
           std::to_string( at.y() - c.y() ) + "," + std::to_string( at.z() - c.z() ) + ")" );
    check( grabbed && routed && let_go && no_route, "teleport: lets go of the grab, drops the auto-move route" );
    check( marker.empty() && rocks == 3, "gallery: rocks left on the floor", ok_or( marker ) );
}

// rebuild_zone: refused for the zone the player is in (the hub) and bad indices; from the hub the gallery is built
// again (its rocks gone, its pieces in place) while the hub and the player stay as they were.
void check_rebuild( size_t gallery )
{
    std::printf( "rebuild:\n" );
    const tripoint_abs_ms s = centre_of( 0 );
    const tripoint_abs_ms c = centre_of( gallery );
    std::vector<std::string> bad;
    std::string good;
    tripoint_abs_ms before;
    tripoint_abs_ms after;
    mon_state mons_before;
    mon_state mons_after;
    int zone = -1;
    int hub_rocks = -1;
    bool built = false;
    on_sim( [&] {
        const size_t n = fp3d::range_zones().size();
        bad.push_back( fp3d::rebuild_zone( 0 ) );
        bad.push_back( fp3d::rebuild_zone( n ) );
        bad.push_back( fp3d::rebuild_zone( static_cast<size_t>( -1 ) ) );
        before = get_avatar().pos_abs();
        mons_before = monsters_now();
        good = fp3d::rebuild_zone( gallery );
        after = get_avatar().pos_abs();
        mons_after = monsters_now();
        zone = fp3d::current_zone();
        hub_rocks = items_of( offset( s, 2, 2 ), "rock" );
        built = fp3d::range_zones()[gallery].built;
    } );
    bool refused = bad.size() == 3;
    std::string why;
    for( const std::string &b : bad ) {
        refused = refused && !b.empty();
        why += "[" + b + "]";
    }
    check( refused, "rebuild_zone: the zone you are in and bad indices refused", why.substr( 0, 120 ) );
    check( good.empty() && built && before == after && zone == 0 && hub_rocks == 5,
           "rebuild_zone: from the hub, the player and the hub untouched", ok_or( good ) + " zone " +
           std::to_string( zone ) + " hub rocks " + std::to_string( hub_rocks ) );
    check( mons_before == mons_after && !mons_after.empty(), "rebuild_zone: the hub's monsters back in place",
           std::to_string( mons_before.size() ) + " before, " + std::to_string( mons_after.size() ) + " after" );

    std::string tp;
    int rocks = -1;
    size_t pieces = 0;
    size_t missing = 0;
    std::string missing_ids;
    bool stairs = false;
    on_sim( [&] {
        tp = fp3d::teleport_to_zone( gallery );
        map &here = get_map();
        rocks = items_of( offset( c, 2, -2 ), "rock" );
        const std::vector<fp3d::gallery_piece> list = fp3d::gallery_pieces( gallery );
        pieces = list.size();
        for( const fp3d::gallery_piece &p : list ) {
            const tripoint_bub_ms b = here.get_bub( offset( c, p.at.x, p.at.y, p.at.z ) );
            const std::string there = p.furniture ? here.furn( b ).id().str() : here.ter( b ).id().str();
            if( there != p.id ) {
                missing++;
                if( missing <= 8 ) {
                    missing_ids += p.id + "(" + there + ") ";
                }
            }
        }
        const fp3d::range_offset st = fp3d::gallery_stairs;
        stairs = here.ter( here.get_bub( offset( c, st.x, st.y, 0 ) ) ).id().str() == "t_stairs_up" &&
                 here.ter( here.get_bub( offset( c, st.x, st.y, 1 ) ) ).id().str() == "t_stairs_down";
    } );
    check( tp.empty() && rocks == 0 && pieces > 0 && missing == 0 && stairs,
           "rebuild_zone: the gallery cleared and built again", ok_or( tp ) + " rocks " + std::to_string( rocks ) +
           ", " + std::to_string( missing ) + " of " + std::to_string( pieces ) + " pieces missing " + missing_ids );
}

void check_info( size_t gallery )
{
    std::printf( "range info:\n" );
    const fp3d::range_info info = fp3d::latest_range_info();
    size_t built = 0;
    for( const fp3d::range_zone_info &z : info.zones ) {
        built += z.built ? 1 : 0;
    }
    check( info.seq > 0 && info.range && info.ai_frozen && info.zone == static_cast<int>( gallery ) &&
           info.zones.size() > gallery && info.zones[0].built && info.zones[gallery].built && built == 2 &&
           info.zones[gallery].kind == "gallery", "range info: range, frozen, zones, current zone, built",
           "seq " + std::to_string( info.seq ) + ", zone " + std::to_string( info.zone ) + ", " +
           std::to_string( info.zones.size() ) + " zones, " + std::to_string( built ) + " built" );
}

// Teleport onto a centre that isn't safe, with every square round it unsafe too but the last one arrival_square looks
// at: fire on the centre, smoke, acid, deep water (still and flowing), open air and traps round it. The player lands on
// that last square. The player is in the gallery (check_teleport_arrival); the squares are put back afterwards.
void check_arrival_hazards( size_t gallery )
{
    std::printf( "arrival hazards:\n" );
    const tripoint_abs_ms c = centre_of( gallery );
    std::string setup;
    std::string tp;
    tripoint_abs_ms want;
    tripoint_abs_ms at;
    on_sim( [&] {
        map &here = get_map();
        // The order arrival_square tries them in: the centre, then ring 1.
        std::vector<tripoint_abs_ms> order;
        for( const tripoint_bub_ms &p : closest_points_first( here.get_bub( c ), 1 ) ) {
            order.push_back( here.get_abs( p ) );
        }
        if( order.size() != 9 ) {
            setup = "ring of " + std::to_string( order.size() );
            return;
        }
        want = order.back();
        std::vector<ter_id> before;
        for( const tripoint_abs_ms &p : order ) {
            before.push_back( here.ter( here.get_bub( p ) ) );
        }
        const tripoint_bub_ms air_below = here.get_bub( order[5] + tripoint_rel_ms::below );
        const ter_id below_before = here.ter( air_below );
        const auto field = [&]( size_t i, const char *id ) {
            if( !here.add_field( here.get_bub( order[i] ), field_type_str_id( id ).id(), 1, 0_turns, false ) ) {
                setup += std::string( "[no " ) + id + "]";
            }
        };
        const auto ter = [&]( size_t i, const char *id ) {
            here.ter_set( here.get_bub( order[i] ), ter_str_id( id ).id() );
        };
        field( 0, "fd_fire" );
        field( 1, "fd_smoke" );
        field( 2, "fd_acid" );
        ter( 3, "t_water_dp" );
        ter( 4, "t_water_moving_dp" );
        // Open air over natural rock turns into a floor when the submap loads again: a wall under it.
        here.ter_set( air_below, ter_str_id( "t_concrete_wall" ).id() );
        ter( 5, "t_open_air" );
        here.trap_set( here.get_bub( order[6] ), trap_str_id( "tr_bubblewrap" ).id() );
        here.trap_set( here.get_bub( order[7] ), trap_str_id( "tr_beartrap" ).id() );
        tp = fp3d::teleport_to_zone( gallery );
        at = get_avatar().pos_abs();
        map &now = get_map();
        for( size_t i = 0; i < order.size(); i++ ) {
            const tripoint_bub_ms b = now.get_bub( order[i] );
            now.clear_fields( b );
            now.remove_trap( b );
            now.ter_set( b, before[i] );
        }
        now.ter_set( now.get_bub( order[5] + tripoint_rel_ms::below ), below_before );
    } );
    check( setup.empty() && tp.empty() && at == want,
           "teleport: avoids fire, smoke, acid, deep water, open air, traps", ok_or( setup ) + " " + ok_or( tp ) +
           " at centre+(" + std::to_string( at.x() - c.x() ) + "," + std::to_string( at.y() - c.y() ) + "), want (" +
           std::to_string( want.x() - c.x() ) + "," + std::to_string( want.y() - c.y() ) + ")" );
}

// A boardable seat of the hub's car (sim thread); nullopt if there is none.
std::optional<tripoint_abs_ms> car_seat( const tripoint_abs_ms &hub )
{
    map &here = get_map();
    const optional_vpart_position vp = here.veh_at( here.get_bub( offset( hub, 6, 9 ) ) );
    if( !vp ) {
        return std::nullopt;
    }
    for( const vpart_reference &seat : vp->vehicle().get_avail_parts( VPFLAG_BOARDABLE ) ) {
        return here.get_abs( seat.pos_bub( here ) );
    }
    return std::nullopt;
}

// Whether someone sits on the seat at `p` of a "car" (sim thread); nullopt if no car seat is there.
std::optional<bool> seat_taken( const tripoint_abs_ms &p )
{
    map &here = get_map();
    const std::optional<vpart_reference> seat = here.veh_at( here.get_bub( p ) ).part_with_feature( VPFLAG_BOARDABLE,
            true );
    if( !seat || seat->vehicle().type.str() != "car" ) {
        return std::nullopt;
    }
    return seat->part().has_flag( vp_flag::passenger_flag );
}

// Teleport to a zone with no free square round its centre (walls over the whole search radius): refused, the player
// back where it was. Done from a seat in the hub's car: rebuild_zone is refused there, the refused teleport puts the
// player back in its seat, and a teleport that works leaves the car where it stands. The player starts in the gallery
// and ends in the hub; the gallery keeps its walls until check_rebuild builds it again.
void check_crowded_arrival( size_t gallery )
{
    std::printf( "crowded arrival, from a vehicle:\n" );
    const tripoint_abs_ms c = centre_of( gallery );
    const tripoint_abs_ms s = centre_of( 0 );
    int walls = 0;
    std::string to_hub;
    on_sim( [&] {
        map &here = get_map();
        const int r = fp3d::arrival_search_squares;
        for( int y = -r; y <= r; y++ ) {
            for( int x = -r; x <= r; x++ ) {
                const tripoint_bub_ms b = here.get_bub( offset( c, x, y ) );
                here.ter_set( b, ter_str_id( "t_concrete_wall" ).id() );
                walls += here.passable( b ) ? 0 : 1;
            }
        }
        to_hub = fp3d::teleport_to_zone( 0 );
    } );
    const int side = 2 * fp3d::arrival_search_squares + 1;
    check( walls == side * side && to_hub.empty(), "gallery walled round its centre, player to the hub",
           std::to_string( walls ) + " walls " + ok_or( to_hub ) );

    std::optional<tripoint_abs_ms> seat;
    bool boarded = false;
    std::string rebuild;
    bool still_in = false;
    std::string tp;
    bool back_in = false;
    std::optional<bool> taken_back;
    tripoint_abs_ms back_at;
    std::string out;
    bool left = false;
    std::optional<bool> taken_after;
    int zone = -1;
    on_sim( [&] {
        avatar &u = get_avatar();
        seat = car_seat( s );
        if( !seat ) {
            return;
        }
        get_map().board_vehicle( get_map().get_bub( *seat ), &u );
        boarded = u.in_vehicle && u.pos_abs() == *seat && seat_taken( *seat ).value_or( false );
        rebuild = fp3d::rebuild_zone( gallery );
        still_in = u.in_vehicle && u.pos_abs() == *seat;
        tp = fp3d::teleport_to_zone( gallery );
        back_at = u.pos_abs();
        back_in = u.in_vehicle;
        taken_back = seat_taken( *seat );
        out = fp3d::teleport_to_zone( 0 );
        left = !u.in_vehicle && !get_map().veh_at( u.pos_bub() );
        taken_after = seat_taken( *seat );
        zone = fp3d::current_zone();
    } );
    check( seat && boarded, "hub: the player boards the car" );
    check( rebuild == "Not while riding or in a vehicle." && still_in, "rebuild_zone: refused in a vehicle",
           ok_or( rebuild ) );
    check( tp == "No free square to arrive on there." && seat && back_at == *seat && back_in &&
           taken_back.value_or( false ), "teleport: no free square, refused, back in the car seat", ok_or( tp ) +
           ( back_in ? " in the car" : " not in the car" ) );
    check( out.empty() && left && taken_after.has_value() && !*taken_after && zone == 0,
           "teleport from the car: works, the car stays, the seat empty", ok_or( out ) );
}

// Riding: teleport_to_zone and rebuild_zone refused, the player and its mount stay. The mount: a horse next to the
// player (mount_creature checks nothing; the menus that would). The player is in the hub; it gets off and the horse
// is taken away afterwards.
void check_riding( size_t gallery )
{
    std::printf( "riding:\n" );
    bool mounted = false;
    std::string tp;
    std::string rebuild;
    bool stayed = false;
    bool off = false;
    on_sim( [&] {
        map &here = get_map();
        avatar &u = get_avatar();
        monster *horse = nullptr;
        for( const tripoint_bub_ms &p : here.points_in_radius( u.pos_bub(), 1 ) ) {
            if( p != u.pos_bub() && here.passable( p ) && !here.veh_at( p ) &&
                get_creature_tracker().creature_at( p ) == nullptr ) {
                horse = g->place_critter_at( mtype_id( "mon_horse" ), p );
                if( horse != nullptr ) {
                    break;
                }
            }
        }
        if( horse == nullptr ) {
            return;
        }
        u.mount_creature( *horse );
        const tripoint_abs_ms where = u.pos_abs();
        mounted = u.is_mounted() && horse->pos_abs() == where;
        tp = fp3d::teleport_to_zone( gallery );
        rebuild = fp3d::rebuild_zone( gallery );
        stayed = u.is_mounted() && u.pos_abs() == where && horse->pos_abs() == where;
        // Off again (as Character::dismount, without its square menu), the horse gone.
        u.remove_effect( efftype_id( "riding" ) );
        horse->remove_effect( efftype_id( "ridden" ) );
        horse->mounted_player_id = character_id();
        horse->mounted_player = nullptr;
        u.mounted_creature = nullptr;
        g->remove_zombie( *horse );
        off = !u.is_mounted();
    } );
    check( mounted && tp == "Not while riding." && stayed, "teleport: refused while riding", ok_or( tp ) );
    check( rebuild == "Not while riding or in a vehicle." && stayed && off, "rebuild_zone: refused while riding",
           ok_or( rebuild ) );
}

// A rebuild clears the zone's own area only: the bubble it loads reaches 12 squares further east and south. The
// player stands just east of the gallery (in no zone) with a monster next to it, outside the gallery too: the
// monster stays; one just inside the gallery is gone. The player is in the gallery (check_rebuild).
void check_rebuild_edge( size_t gallery )
{
    std::printf( "rebuild edge:\n" );
    const tripoint_abs_ms c = centre_of( gallery );
    const int h = fp3d::zone_half_squares;
    const tripoint_abs_ms stand = offset( c, h + 5, 0 );
    const tripoint_abs_ms outside = offset( c, h + 6, 0 );
    const tripoint_abs_ms inside = offset( c, h - 1, 0 );
    std::string spawned;
    int zone = -2;
    std::string rebuild;
    tripoint_abs_ms at;
    std::string out_after = "?";
    std::string in_after = "?";
    on_sim( [&] {
        avatar &u = get_avatar();
        // Outside the zone is the generated world: a floor to stand on.
        const auto pave = []( const tripoint_abs_ms & p ) {
            map &here = get_map();
            const tripoint_bub_ms b = here.get_bub( p );
            if( here.inbounds( b ) ) {
                here.ter_set( b, ter_str_id( "t_concrete" ).id() );
                here.furn_set( b, furn_str_id::NULL_ID() );
            }
        };
        pave( stand );
        u.setpos( stand, false );
        g->update_map( u );
        pave( outside );
        pave( inside );
        spawned = ok_or( fp3d::spawn_monster( "mon_zombie", ui( outside ) ) ) + ok_or( fp3d::spawn_monster(
                      "mon_zombie", ui( inside ) ) );
        zone = fp3d::current_zone();
        rebuild = fp3d::rebuild_zone( gallery );
        at = u.pos_abs();
        creature_tracker &tracker = get_creature_tracker();
        const monster *mo = tracker.creature_at<monster>( outside );
        const monster *mi = tracker.creature_at<monster>( inside );
        out_after = mo != nullptr ? mo->type->id.str() : "nobody";
        in_after = mi != nullptr ? mi->type->id.str() : "nobody";
        if( mo != nullptr ) {
            g->remove_zombie( *mo );
        }
    } );
    check( spawned == "okok" && zone == -1 && rebuild.empty() && at == stand,
           "rebuild_zone: from just outside the zone", spawned + " zone " + std::to_string( zone ) + " " +
           ok_or( rebuild ) );
    check( out_after == "mon_zombie" && in_after == "nobody",
           "rebuild_zone: a monster just outside the zone stays", "outside: " + out_after + ", inside: " + in_after );
}

// ---- the other zone kinds ----

bool in_zone( const tripoint_abs_ms &c, const tripoint_abs_ms &p )
{
    const int h = fp3d::zone_half_squares;
    return p.x() - c.x() >= -h && p.x() - c.x() < h && p.y() - c.y() >= -h && p.y() - c.y() < h &&
           p.z() - c.z() >= -1 && p.z() - c.z() <= 1;
}

// Everything a zone holds, as one hash: terrain, furniture, items (type, count, ammo, contents) but on burning
// squares (the fire eats its fuel), fire, monsters and vehicles (type and square). Two builds of a zone give the same
// (sim thread). `things` counts what went in.
uint64_t zone_fingerprint( const tripoint_abs_ms &c, size_t &things, std::string *what = nullptr )
{
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&h]( const std::string & s ) {
        for( const char ch : s ) {
            h ^= static_cast<unsigned char>( ch );
            h *= 1099511628211ull;
        }
        h ^= 0xff;
        h *= 1099511628211ull;
    };
    things = 0;
    map &here = get_map();
    const int half = fp3d::zone_half_squares;
    for( int z = -1; z <= 1; z++ ) {
        for( int y = -half; y < half; y++ ) {
            for( int x = -half; x < half; x++ ) {
                const tripoint_bub_ms b = here.get_bub( offset( c, x, y, z ) );
                mix( here.ter( b ).id().str() );
                mix( here.furn( b ).id().str() );
                const bool fire = here.has_field_at( b, field_type_str_id( "fd_fire" ).id() );
                mix( fire ? "fire" : "-" );
                if( fire ) {
                    continue;
                }
                for( const item &it : here.i_at( b ) ) {
                    things++;
                    if( what != nullptr && what->size() < 2000 ) {
                        *what += it.typeId().str() + "@" + std::to_string( x ) + "," + std::to_string( y ) + " ";
                    }
                    std::string s = it.typeId().str() + "x" + std::to_string( it.count() );
                    if( it.is_gun() || it.is_magazine() ) {
                        s += "/" + std::to_string( it.ammo_remaining() );
                    }
                    for( const item *in : it.all_items_top() ) {
                        s += "+" + in->typeId().str() + "x" + std::to_string( in->count() );
                    }
                    mix( s );
                }
            }
        }
    }
    std::vector<std::string> mons;
    for( monster &m : g->all_monsters() ) {
        if( in_zone( c, m.pos_abs() ) ) {
            const tripoint_abs_ms p = m.pos_abs();
            mons.push_back( m.type->id.str() + "@" + std::to_string( p.x() - c.x() ) + "," +
                            std::to_string( p.y() - c.y() ) );
        }
    }
    std::vector<std::string> vehs;
    for( const wrapped_vehicle &w : here.get_vehicles() ) {
        const tripoint_abs_ms p = w.v->pos_abs();
        if( in_zone( c, p ) ) {
            vehs.push_back( w.v->type.str() + "@" + std::to_string( p.x() - c.x() ) + "," +
                            std::to_string( p.y() - c.y() ) + "#" + std::to_string( w.v->part_count() ) );
        }
    }
    std::sort( mons.begin(), mons.end() );
    std::sort( vehs.begin(), vehs.end() );
    for( const std::string &s : mons ) {
        mix( s );
    }
    for( const std::string &s : vehs ) {
        mix( s );
    }
    things += mons.size() + vehs.size();
    if( what != nullptr ) {
        *what = std::to_string( mons.size() ) + " monsters, " + std::to_string( vehs.size() ) + " vehicles; items: " +
                *what;
    }
    return h;
}

struct kind_result {
    std::vector<prop_result> checks;   // the kind's own checks
    std::string summary;               // counts for the log
};

bool has_item( const tripoint_bub_ms &b, const std::string &id )
{
    for( const item &it : get_map().i_at( b ) ) {
        if( it.typeId().str() == id || it.has_item_with( [&id]( const item & e ) {
        return e.typeId().str() == id;
        } ) ) {
            return true;
        }
    }
    return false;
}

// Armory: every type on its table (or skipped), guns loaded with a stack of their ammo.
kind_result verify_armory( size_t index, const fp3d::range_zone &z, const tripoint_abs_ms &c )
{
    kind_result out;
    map &here = get_map();
    const std::vector<fp3d::zone_piece> pieces = fp3d::zone_pieces( index );
    size_t present = 0;
    size_t tables = 0;
    size_t guns = 0;
    size_t guns_present = 0;
    size_t loadable = 0;
    size_t loaded = 0;
    size_t need_mag = 0;
    size_t with_mag = 0;
    size_t stacks = 0;
    size_t want_stacks = 0;
    size_t melee = 0;
    size_t thrown = 0;
    std::string unloaded;
    std::string skipped_ids;
    for( const fp3d::zone_piece &p : pieces ) {
        const tripoint_bub_ms b = here.get_bub( offset( c, p.at.x, p.at.y, p.at.z ) );
        tables += here.furn( b ).id().str() == "f_table" ? 1 : 0;
        guns += p.what == "gun" ? 1 : 0;
        melee += p.what == "melee" ? 1 : 0;
        thrown += p.what == "thrown" ? 1 : 0;
        const item *found = nullptr;
        for( const item &it : here.i_at( b ) ) {
            if( it.typeId().str() == p.id ) {
                found = &it;
                break;
            }
        }
        if( found == nullptr ) {
            if( skipped_ids.size() < 300 ) {
                skipped_ids += p.id + " ";
            }
            continue;
        }
        present++;
        if( p.what != "gun" ) {
            continue;
        }
        guns_present++;
        const itype_id ammo = found->ammo_default();
        const itype_id mag = found->magazine_default();
        const bool can_load = !ammo.is_null() && ammo->ammo && ( found->magazine_integral() ||
                              ( !mag.is_null() && mag->magazine ) );
        if( !can_load ) {
            continue;
        }
        loadable++;
        loaded += found->ammo_remaining() > 0 ? 1 : 0;
        if( found->ammo_remaining() <= 0 && unloaded.size() < 120 ) {
            unloaded += p.id + " ";
        }
        if( !found->magazine_integral() ) {
            need_mag++;
            with_mag += found->magazine_current() != nullptr ? 1 : 0;
        }
        // Ammo that never lies loose (NO_DROP: only inside its gun) gets no stack.
        if( ammo->has_flag( flag_id( "NO_DROP" ) ) ) {
            continue;
        }
        want_stacks++;
        bool stack = false;
        for( const item &it : here.i_at( b ) ) {
            if( &it != found && ( it.typeId() == ammo || it.has_item_with( [&ammo]( const item & e ) {
            return e.typeId() == ammo;
        } ) ) ) {
                stack = true;
                break;
            }
        }
        stacks += stack ? 1 : 0;
        if( !stack && unloaded.size() < 120 ) {
            unloaded += "(no stack: " + p.id + " " + ammo.str() + ") ";
        }
    }
    out.summary = string_format( "%zu of %zu placed (%zu guns, %zu melee, %zu thrown), %zu skipped; guns: %zu "
                                 "loadable, %zu loaded, %zu of %zu with a magazine, %zu of %zu ammo stacks", present,
                                 z.count, guns, melee, thrown, z.skipped, loadable, loaded, with_mag, need_mag, stacks,
                                 want_stacks ) + "; skipped: " + skipped_ids;
    out.checks.push_back( { "every item type on its table (or counted skipped)", pieces.size() == z.count &&
                            tables == pieces.size() && present == z.placed && present + z.skipped == z.count && present > 0,
                            std::to_string( present ) + " + " + std::to_string( z.skipped ) + " of " + std::to_string( z.count ) } );
    out.checks.push_back( { "guns loaded (default magazine or own ammo), a stack of ammo each", guns_present > 0 &&
                            loadable > guns_present / 2 && loaded == loadable && with_mag == need_mag &&
                            stacks == want_stacks && want_stacks + 5 >= loadable,
                            std::to_string( loaded ) + "/" + std::to_string( loadable ) + " loaded, " +
                            std::to_string( stacks ) + " stacks " + unloaded } );
    return out;
}

// Halls: every type on its rack (or the floor), each item square next to an aisle.
kind_result verify_halls( size_t index, const fp3d::range_zone &z, const tripoint_abs_ms &c )
{
    kind_result out;
    map &here = get_map();
    const std::vector<fp3d::zone_piece> pieces = fp3d::zone_pieces( index );
    size_t present = 0;
    size_t racks = 0;
    size_t reachable = 0;
    size_t most = 0;
    std::set<std::pair<int, int>> squares;
    for( const fp3d::zone_piece &p : pieces ) {
        const tripoint_bub_ms b = here.get_bub( offset( c, p.at.x, p.at.y, p.at.z ) );
        squares.emplace( p.at.x, p.at.y );
        racks += here.furn( b ).id().str() == "f_rack" ? 1 : 0;
        present += has_item( b, p.id ) ? 1 : 0;
        most = std::max( most, here.i_at( b ).size() );
        bool aisle = false;
        for( const int dy : { -1, 1 } ) {
            const tripoint_bub_ms n = here.get_bub( offset( c, p.at.x, p.at.y + dy, p.at.z ) );
            aisle = aisle || ( here.passable( n ) && here.furn( n ) == furn_str_id::NULL_ID() );
        }
        reachable += aisle ? 1 : 0;
    }
    out.summary = string_format( "%s: %zu of %zu placed, %zu skipped, %zu on racks, at most %zu items a square",
                                 z.category, present, z.count, z.skipped, racks, most );
    out.checks.push_back( { "every item type on its own square (or counted skipped)", pieces.size() == z.count &&
                            squares.size() == pieces.size() && present == z.placed && present + z.skipped == z.count &&
                            present > 0 && most <= 1, std::to_string( present ) + " + " + std::to_string( z.skipped ) +
                            " of " + std::to_string( z.count ) } );
    out.checks.push_back( { "every item square next to a walkable aisle", reachable == pieces.size(),
                            std::to_string( reachable ) + " of " + std::to_string( pieces.size() ) } );
    return out;
}

// Lots: each vehicle whole in its painted bay, none overlapping; placed + skipped == count.
kind_result verify_lots( size_t index, const fp3d::range_zone &z, const tripoint_abs_ms &c )
{
    kind_result out;
    map &here = get_map();
    const std::vector<fp3d::zone_piece> pieces = fp3d::zone_pieces( index );
    size_t found = 0;
    size_t whole = 0;
    size_t painted = 0;
    std::string missing;
    std::string outside;
    std::set<const vehicle *> seen;
    for( const fp3d::zone_piece &p : pieces ) {
        const tripoint_bub_ms b = here.get_bub( offset( c, p.at.x, p.at.y, p.at.z ) );
        bool border_ok = true;
        for( int y = p.min.y - 1; y <= p.max.y + 1; y++ ) {
            for( int x = p.min.x - 1; x <= p.max.x + 1; x++ ) {
                if( x < p.min.x || x > p.max.x || y < p.min.y || y > p.max.y ) {
                    border_ok = border_ok && here.ter( here.get_bub( offset( c, x, y ) ) ).id().str() == "t_pavement_y";
                }
            }
        }
        painted += border_ok ? 1 : 0;
        const optional_vpart_position vp = here.veh_at( b );
        if( !vp || vp->vehicle().type.str() != p.id ) {
            missing += p.id + " ";
            continue;
        }
        const vehicle &v = vp->vehicle();
        found++;
        seen.insert( &v );
        bool inside = true;
        int lo_x = INT_MAX;
        int lo_y = INT_MAX;
        int hi_x = INT_MIN;
        int hi_y = INT_MIN;
        for( const tripoint_abs_ms &q : v.get_points( true, true ) ) {
            lo_x = std::min( lo_x, q.x() - c.x() );
            lo_y = std::min( lo_y, q.y() - c.y() );
            hi_x = std::max( hi_x, q.x() - c.x() );
            hi_y = std::max( hi_y, q.y() - c.y() );
        }
        inside = lo_x >= p.min.x && hi_x <= p.max.x && lo_y >= p.min.y && hi_y <= p.max.y;
        whole += inside ? 1 : 0;
        if( !inside && outside.size() < 200 ) {
            outside += string_format( "%s (%d,%d)-(%d,%d) in bay (%d,%d)-(%d,%d) ", p.id, lo_x, lo_y, hi_x, hi_y,
                                      p.min.x, p.min.y, p.max.x, p.max.y );
        }
    }
    size_t in_zone_vehicles = 0;
    for( const wrapped_vehicle &w : here.get_vehicles() ) {
        in_zone_vehicles += in_zone( c, w.v->pos_abs() ) ? 1 : 0;
    }
    out.summary = string_format( "%zu of %zu placed, %zu skipped (%zu bays laid out), %zu vehicles in the zone; "
                                 "missing: %s", found, z.count, z.skipped, pieces.size(), in_zone_vehicles,
                                 missing.empty() ? "none" : missing.substr( 0, 200 ) );
    out.checks.push_back( { "vehicles placed + skipped == count", found == z.placed && found + z.skipped == z.count &&
                            found > 0 && seen.size() == found && in_zone_vehicles == found,
                            std::to_string( found ) + " + " + std::to_string( z.skipped ) + " of " + std::to_string( z.count ) } );
    out.checks.push_back( { "each vehicle whole inside its painted bay", whole == found && painted == pieces.size(),
                            std::to_string( whole ) + " whole, " + std::to_string( painted ) + " bays painted " + outside } );
    return out;
}

// Pens: each monster in its glass cell; placed + skipped == count.
kind_result verify_pens( size_t index, const fp3d::range_zone &z, const tripoint_abs_ms &c )
{
    kind_result out;
    map &here = get_map();
    const std::vector<fp3d::zone_piece> pieces = fp3d::zone_pieces( index );
    size_t present = 0;
    size_t enclosed = 0;
    size_t water = 0;
    size_t wide = 0;
    std::string missing;
    for( const fp3d::zone_piece &p : pieces ) {
        bool glass = true;
        for( int y = p.min.y - 1; y <= p.max.y + 1; y++ ) {
            for( int x = p.min.x - 1; x <= p.max.x + 1; x++ ) {
                if( x < p.min.x || x > p.max.x || y < p.min.y || y > p.max.y ) {
                    glass = glass && here.ter( here.get_bub( offset( c, x, y ) ) ).id().str() == "t_reinforced_glass";
                }
                glass = glass && here.ter( here.get_bub( offset( c, x, y, 1 ) ) ).id().str() == "t_glass_roof";
            }
        }
        water += p.what == "water" ? 1 : 0;
        wide += p.max.x > p.min.x ? 1 : 0;
        const tripoint_bub_ms b = here.get_bub( offset( c, p.at.x, p.at.y, p.at.z ) );
        const monster *m = get_creature_tracker().creature_at<monster>( b );
        if( m == nullptr || m->type->id.str() != p.id ) {
            missing += p.id + " ";
            continue;
        }
        present++;
        enclosed += glass ? 1 : 0;
    }
    size_t in_zone_monsters = 0;
    for( monster &m : g->all_monsters() ) {
        in_zone_monsters += in_zone( c, m.pos_abs() ) ? 1 : 0;
    }
    out.summary = string_format( "%zu of %zu placed, %zu skipped, %zu monsters in the zone, %zu wide cells, %zu "
                                 "flooded; missing: %s", present, z.count, z.skipped, in_zone_monsters, wide, water,
                                 missing.empty() ? "none" : missing.substr( 0, 200 ) );
    out.checks.push_back( { "monsters placed + skipped == count, no strays", present == z.placed &&
                            present + z.skipped == z.count && present > 0 && in_zone_monsters == present,
                            std::to_string( present ) + " + " + std::to_string( z.skipped ) + " of " + std::to_string( z.count ) } );
    out.checks.push_back( { "every monster inside reinforced glass, under a glass roof", enclosed == present,
                            std::to_string( enclosed ) + " of " + std::to_string( present ) } );
    return out;
}

// Lanes: each target at its distance from its shooting square, in plain view; zombies and practice targets at 5,
// 10, 25 and 50.
kind_result verify_lanes( const fp3d::range_zone &z, const tripoint_abs_ms &c )
{
    kind_result out;
    map &here = get_map();
    size_t present = 0;
    size_t in_view = 0;
    std::set<int> mon_d;
    std::set<int> furn_d;
    std::string detail;
    for( const fp3d::lane_target &t : fp3d::lane_targets() ) {
        const tripoint_abs_ms from = offset( c, t.shooter.x, t.shooter.y, t.shooter.z );
        const tripoint_abs_ms to = offset( c, t.target.x, t.target.y, t.target.z );
        const tripoint_bub_ms b = here.get_bub( to );
        bool there = false;
        if( t.monster ) {
            const monster *m = get_creature_tracker().creature_at<monster>( b );
            there = m != nullptr && m->type->id.str() == t.id;
        } else {
            there = here.furn( b ).id().str() == t.id;
        }
        const bool line = here.ter( here.get_bub( from ) ).id().str() == "t_pavement_y";
        if( there && line && rl_dist( from, to ) == t.distance ) {
            present++;
            ( t.monster ? mon_d : furn_d ).insert( t.distance );
        } else {
            detail += string_format( "[%s at %d missing] ", t.id, t.distance );
        }
        in_view += here.sees( here.get_bub( from ), b, 60 ) ? 1 : 0;
    }
    const std::set<int> want = { 5, 10, 25, 50 };
    out.summary = string_format( "%zu of %zu targets, %zu in view; %s", present, z.count, in_view, detail );
    out.checks.push_back( { "targets at 5, 10, 25 and 50 from the shooting line (zombies and practice targets)",
                            present == z.count && present == z.placed && z.skipped == 0 && mon_d == want && furn_d == want,
                            std::to_string( present ) + " of " + std::to_string( z.count ) + " " + detail } );
    out.checks.push_back( { "every target in view from its shooting square", in_view == z.count,
                            std::to_string( in_view ) } );
    return out;
}

// Kitchen: every prop (stoves, oven, fridge, sink, counters, fires, food, water).
kind_result verify_kitchen( const fp3d::range_zone &z, const tripoint_abs_ms &c )
{
    kind_result out;
    const std::vector<prop_result> results = props_at( c, fp3d::kitchen_props() );
    size_t ok = 0;
    size_t fires = 0;
    std::string bad;
    for( size_t i = 0; i < results.size(); i++ ) {
        ok += results[i].ok ? 1 : 0;
        if( results[i].ok && fp3d::kitchen_props()[i].kind == fp3d::prop_kind::field ) {
            fires++;
        }
        if( !results[i].ok ) {
            bad += "[" + results[i].what + ": " + results[i].detail + "] ";
        }
    }
    out.summary = string_format( "%zu of %zu props, %zu fires lit", ok, z.count, fires );
    out.checks.push_back( { "every prop: stoves, oven, fridge, sink, counters, food, water", ok == results.size() &&
                            ok == z.count && z.placed == z.count, std::to_string( ok ) + " of " +
                            std::to_string( results.size() ) + " " + bad } );
    out.checks.push_back( { "stoves, fireplace and brazier lit", fires == 4, std::to_string( fires ) + " fires" } );
    return out;
}

kind_result verify_kind( size_t index, const fp3d::range_zone &z, const tripoint_abs_ms &c )
{
    switch( z.kind ) {
        case fp3d::zone_kind::armory:
            return verify_armory( index, z, c );
        case fp3d::zone_kind::halls:
            return verify_halls( index, z, c );
        case fp3d::zone_kind::lots:
            return verify_lots( index, z, c );
        case fp3d::zone_kind::pens:
            return verify_pens( index, z, c );
        case fp3d::zone_kind::lanes:
            return verify_lanes( z, c );
        case fp3d::zone_kind::kitchen:
            return verify_kitchen( z, c );
        case fp3d::zone_kind::hub:
        case fp3d::zone_kind::gallery:
            break;
    }
    return {};
}

// One zone of a kind: teleport there (built on arrival), check what it holds, then from the hub rebuild it and
// check the same again, and that both builds give the same zone.
void check_zone_kind( size_t index )
{
    const tripoint_abs_ms c = centre_of( index );
    std::string tp;
    int zone = -1;
    fp3d::range_zone z;
    kind_result first;
    uint64_t print1 = 0;
    size_t things1 = 0;
    std::string what1;
    on_sim( [&] {
        tp = fp3d::teleport_to_zone( index );
        zone = fp3d::current_zone();
        z = fp3d::range_zones()[index];
        first = verify_kind( index, z, c );
        print1 = zone_fingerprint( c, things1, &what1 );
    } );
    const std::string kind = fp3d::zone_kind_name( z.kind );
    std::printf( "%s (%s): built in %.0f ms; %s\n", kind.c_str(), z.id.c_str(), z.build_ms, first.summary.c_str() );
    check( tp.empty() && zone == static_cast<int>( index ) && z.built, kind + ": teleport there, built",
           ok_or( tp ) + " zone " + std::to_string( zone ) + " " + z.id );
    for( const prop_result &r : first.checks ) {
        check( r.ok, kind + ": " + r.what, r.detail );
    }
    check( z.build_ms > 0.0 && z.build_ms < 5000.0, kind + ": build under 5 s",
           std::to_string( static_cast<int>( z.build_ms ) ) + " ms" );

    std::string back;
    std::string rebuilt;
    std::string again;
    fp3d::range_zone z2;
    kind_result second;
    uint64_t print2 = 0;
    size_t things2 = 0;
    std::string what2;
    on_sim( [&] {
        back = fp3d::teleport_to_zone( 0 );
        rebuilt = fp3d::rebuild_zone( index );
        again = fp3d::teleport_to_zone( index );
        z2 = fp3d::range_zones()[index];
        second = verify_kind( index, z2, c );
        print2 = zone_fingerprint( c, things2, &what2 );
    } );
    bool second_ok = !second.checks.empty();
    for( const prop_result &r : second.checks ) {
        second_ok = second_ok && r.ok;
    }
    check( back.empty() && rebuilt.empty() && again.empty() && second_ok && z2.placed == z.placed &&
           z2.skipped == z.skipped, kind + ": rebuilt from the hub, checks pass again", ok_or( back ) + ok_or( rebuilt ) +
           ok_or( again ) + " " + second.summary.substr( 0, 100 ) );
    check( print1 == print2 && things1 == things2 && things1 > 0, kind + ": the rebuild gives the same zone",
           std::to_string( things1 ) + " vs " + std::to_string( things2 ) + " things" );
    if( print1 != print2 ) {
        std::printf( "    first: %.600s\n    again: %.600s\n", what1.c_str(), what2.c_str() );
    }
}

void check_zone_kinds()
{
    std::printf( "zone kinds:\n" );
    std::vector<fp3d::range_zone> zones;
    on_sim( [&] {
        zones = fp3d::range_zones();
    } );
    // The first zone of each kind; for halls the biggest (the slowest build).
    std::map<fp3d::zone_kind, size_t> pick;
    for( size_t i = 0; i < zones.size(); i++ ) {
        const fp3d::zone_kind k = zones[i].kind;
        if( k == fp3d::zone_kind::hub || k == fp3d::zone_kind::gallery ) {
            continue;
        }
        const auto it = pick.find( k );
        if( it == pick.end() || ( k == fp3d::zone_kind::halls && zones[i].count > zones[it->second].count ) ) {
            pick[k] = i;
        }
    }
    check( pick.size() == 6, "a zone of each kind to check", std::to_string( pick.size() ) );
    for( const fp3d::zone_kind k : {
             fp3d::zone_kind::armory, fp3d::zone_kind::halls, fp3d::zone_kind::lots, fp3d::zone_kind::pens,
             fp3d::zone_kind::lanes, fp3d::zone_kind::kitchen
         } ) {
        if( pick.count( k ) ) {
            check_zone_kind( pick[k] );
        }
    }
}

// Every zone built once (rebuild_zone from the hub): placed + skipped == count, the build times and what each kind
// left out.
void check_all_zones()
{
    std::printf( "all zones:\n" );
    std::string tp;
    std::vector<std::string> refused;
    std::vector<fp3d::range_zone> zones;
    double secs = 0.0;
    on_sim( [&] {
        tp = fp3d::teleport_to_zone( 0 );
        const auto t = std::chrono::steady_clock::now();
        const size_t n = fp3d::range_zones().size();
        for( size_t i = 1; i < n; i++ ) {
            const std::string why = fp3d::rebuild_zone( i );
            if( !why.empty() ) {
                refused.push_back( fp3d::range_zones()[i].id + ": " + why );
            }
        }
        secs = ms_since( t ) / 1000.0;
        zones = fp3d::range_zones();
    }, 900000 );
    struct per_kind {
        size_t zones = 0;
        size_t count = 0;
        size_t placed = 0;
        size_t skipped = 0;
        double max_ms = 0.0;
        std::string slowest;
    };
    std::map<std::string, per_kind> kinds;
    bool sums = !zones.empty();
    for( size_t i = 1; i < zones.size(); i++ ) {
        const fp3d::range_zone &z = zones[i];
        per_kind &k = kinds[fp3d::zone_kind_name( z.kind )];
        k.zones++;
        k.count += z.count;
        k.placed += z.placed;
        k.skipped += z.skipped;
        if( z.build_ms > k.max_ms ) {
            k.max_ms = z.build_ms;
            k.slowest = z.id;
        }
        sums = sums && z.built && z.placed + z.skipped == z.count;
    }
    for( const auto &[name, k] : kinds ) {
        std::printf( "    %-8s %2zu zones: %zu of %zu placed, %zu skipped; slowest build %s %.0f ms\n", name.c_str(),
                     k.zones, k.placed, k.count, k.skipped, k.slowest.c_str(), k.max_ms );
    }
    std::printf( "    every zone built in %.1f s\n", secs );
    check( tp.empty() && refused.empty() && sums, "every zone built: placed + skipped == count",
           ok_or( tp ) + " " + ( refused.empty() ? "" : refused.front() ) );
}

void check_back_to_hub()
{
    std::printf( "back to the hub:\n" );
    std::string tp;
    int rocks = -1;
    int zone = -1;
    std::string zombie;
    on_sim( [&] {
        tp = fp3d::teleport_to_zone( 0 );
        zone = fp3d::current_zone();
        const tripoint_abs_ms s = centre_of( 0 );
        rocks = items_of( offset( s, 2, 2 ), "rock" );
        for( const auto &m : monsters_now() ) {
            zombie += std::get<0>( m ) + " ";
        }
    } );
    check( tp.empty() && zone == 0 && rocks == 5, "teleport back: the hub kept (not rebuilt)",
           ok_or( tp ) + " rocks " + std::to_string( rocks ) + " " + zombie );
}

// ---- a saved range loaded again (resume_test_range) ----

struct zone_row {
    std::string id;
    std::string name;
    std::string kind;
    bool built = false;
    bool operator==( const zone_row &o ) const {
        return id == o.id && name == o.name && kind == o.kind && built == o.built;
    }
};

std::vector<zone_row> zone_rows()
{
    std::vector<zone_row> out;
    for( const fp3d::range_zone &z : fp3d::range_zones() ) {
        out.push_back( { z.id, z.name, fp3d::zone_kind_name( z.kind ), z.built } );
    }
    return out;
}

// An avatar value as a number, `none` when it is missing or not a number (sim thread).
double avatar_number( const std::string &name, double none = -12345.0 )
{
    const diag_value *v = get_avatar().maybe_get_value( name );
    return v != nullptr && v->is_dbl() ? v->dbl() : none;
}

// The range saved in a zone that isn't the hub (the smallest halls zone: items only, nothing there moves; built on
// arrival unless --all-zones built it), with rocks dropped there and the AI running, then that save loaded in this
// process (as boot_saved_game) and resumed as the bridge does: a range game again, the same zones, that zone built and
// not built again (rocks and contents as saved), the seed and AI freeze as saved, achievements off, operations work.
// Returns the zone, 0 if none.
size_t check_reload()
{
    std::printf( "reload:\n" );
    const uint32_t rock = fp3d::intern_id( fp3d::id_kind::itype, "rock" );
    size_t pick = 0;
    bool was_built = true;
    std::string tp;
    int zone_before = -1;
    tripoint_abs_ms marker;
    std::string spawned;
    int rocks_before = -1;
    uint64_t print_before = 0;
    size_t things_before = 0;
    std::string unfreeze;
    std::vector<zone_row> rows_before;
    double frozen_value = -1.0;
    double built_value = -1.0;
    bool saved = false;
    on_sim( [&] {
        const std::vector<fp3d::range_zone> &zones = fp3d::range_zones();
        for( size_t i = 1; i < zones.size(); i++ ) {
            if( zones[i].kind == fp3d::zone_kind::halls && ( pick == 0 || zones[i].built < zones[pick].built ||
                    ( zones[i].built == zones[pick].built && zones[i].count < zones[pick].count ) ) ) {
                pick = i;
            }
        }
        if( pick == 0 ) {
            return;
        }
        was_built = zones[pick].built;
        tp = fp3d::teleport_to_zone( pick );
        zone_before = fp3d::current_zone();
        avatar &u = get_avatar();
        marker = u.pos_abs();
        spawned = fp3d::spawn_item( rock, 7, ui( marker ) );
        rocks_before = items_of( marker, "rock" );
        print_before = zone_fingerprint( centre_of( pick ), things_before );
        unfreeze = fp3d::set_ai_frozen( false );
        frozen_value = avatar_number( "fp3d_range_ai_frozen" );
        built_value = avatar_number( "fp3d_range_built_" + fp3d::range_zones()[pick].id );
        // As if booted with a seed other than the default (1): resume must read it from the save.
        u.set_value( "fp3d_range_seed", 4242.0 );
        rows_before = zone_rows();
        saved = fp3d_game_access::save_game( *g );
        if( saved ) {
            // As CDDA's save-and-quit: the game ends on its next turn, and sim_main loads it again.
            reload_save = u.get_save_id();
            u.set_moves( 0 );
            g->uquit = QUIT_SAVED;
        }
    } );
    const std::string zone_id = pick < rows_before.size() ? rows_before[pick].id : "?";
    check( pick > 0 && tp.empty() && zone_before == static_cast<int>( pick ) && spawned.empty() && rocks_before >= 7 &&
           unfreeze.empty() && saved, "reload: in " + zone_id + ", rocks dropped, AI running, saved",
           ok_or( tp ) + ok_or( spawned ) + ok_or( unfreeze ) +
           ( was_built ? " (built before)" : " (built on arrival)" ) );
    check( frozen_value == 0.0 && built_value == 1.0, "reload: the AI freeze and the built zone in avatar values",
           "frozen " + std::to_string( frozen_value ) + ", built " + std::to_string( built_value ) );

    // That turn ends the game; the next main-loop request comes after the load.
    const auto t = std::chrono::steady_clock::now();
    const std::optional<fp3d::ui_request> back_in = act( "pause", 600000 );
    std::printf( "    loaded and resumed in %.1f s\n", ms_since( t ) / 1000.0 );
    check( back_in.has_value() && resumed.load() == 1, "reload: resume_test_range says yes after the load",
           "resumed " + std::to_string( resumed.load() ) );

    bool active = false;
    bool frozen = true;
    unsigned seed = 0;
    bool achievements = true;
    int zone_after = -1;
    std::vector<zone_row> rows_after;
    fp3d::range_zone z;
    int rocks_after = -1;
    uint64_t print_after = 0;
    size_t things_after = 0;
    std::string healed;
    std::string to_hub;
    int zone_hub = -1;
    std::string again;
    int zone_again = -1;
    fp3d::range_zone z_again;
    int rocks_again = -1;
    uint64_t print_again = 0;
    size_t things_again = 0;
    on_sim( [&] {
        active = fp3d::range_active();
        frozen = fp3d::ai_frozen();
        seed = fp3d::range_boot_seed();
        achievements = get_achievements().is_enabled();
        rows_after = zone_rows();
        zone_after = fp3d::current_zone();
        if( pick < fp3d::range_zones().size() ) {
            z = fp3d::range_zones()[pick];
        }
        rocks_after = items_of( marker, "rock" );
        print_after = zone_fingerprint( centre_of( pick ), things_after );
        get_avatar().set_pain( 20 );
        healed = fp3d::heal();
        healed += get_avatar().get_pain() == 0 ? "" : " (still in pain)";
        // To the hub and back: the zone is built, so arriving doesn't build it again.
        to_hub = fp3d::teleport_to_zone( 0 );
        zone_hub = fp3d::current_zone();
        again = fp3d::teleport_to_zone( pick );
        zone_again = fp3d::current_zone();
        if( pick < fp3d::range_zones().size() ) {
            z_again = fp3d::range_zones()[pick];
        }
        rocks_again = items_of( marker, "rock" );
        print_again = zone_fingerprint( centre_of( pick ), things_again );
    } );
    check( active && zone_after == static_cast<int>( pick ), "reload: a range game, the player in " + zone_id,
           "zone " + std::to_string( zone_after ) );
    check( !rows_after.empty() && rows_after == rows_before, "reload: the same zones, the same built flags",
           std::to_string( rows_before.size() ) + " vs " + std::to_string( rows_after.size() ) + " zones" );
    check( z.built && z.placed == 0 && z.build_ms == 0.0 && rocks_after == rocks_before &&
           print_after == print_before && things_after == things_before,
           "reload: " + zone_id + " built, not built again, as saved",
           "rocks " + std::to_string( rocks_before ) + " -> " + std::to_string( rocks_after ) + ", " +
           std::to_string( things_before ) + " -> " + std::to_string( things_after ) + " things" );
    check( !frozen && seed == 4242 && !achievements, "reload: AI running and seed as saved, achievements off",
           "seed " + std::to_string( seed ) );
    check( healed.empty() && to_hub.empty() && zone_hub == 0, "reload: heal and teleport to the hub work",
           ok_or( healed ) + ok_or( to_hub ) );
    check( again.empty() && zone_again == static_cast<int>( pick ) && z_again.placed == 0 &&
           rocks_again == rocks_before && print_again == print_before, "reload: back in " + zone_id + ", not rebuilt",
           ok_or( again ) + " rocks " + std::to_string( rocks_again ) );
    const fp3d::range_info info = fp3d::latest_range_info();
    check( info.range && !info.ai_frozen && info.zones.size() == rows_before.size() &&
           info.zone == static_cast<int>( pick ), "reload: range info for the client",
           std::to_string( info.zones.size() ) + " zones, zone " + std::to_string( info.zone ) );
    return pick;
}

namespace fs = std::filesystem;

// The marker file boot puts in the worlds it makes (fp3d/boot.h), here or a folder named like it.
bool write_marker( const fs::path &dir, bool as_folder )
{
    std::error_code ec;
    fs::create_directories( dir, ec );
    if( as_folder ) {
        fs::create_directories( dir / fp3d::owned_world_marker, ec );
        return !ec;
    }
    std::ofstream mark( dir / fp3d::owned_world_marker, std::ios::binary | std::ios::trunc );
    mark << "fp3d-range-test scratch world\n";
    return static_cast<bool>( mark );
}

// Only what write_marker leaves in a folder: safe to remove (a scratch world from an earlier, interrupted run).
bool only_markers( const fs::path &dir )
{
    std::error_code ec;
    size_t n = 0;
    for( fs::directory_iterator it( dir, ec ); !ec && it != fs::directory_iterator(); it.increment( ec ) ) {
        if( ++n > 1 || it->path().filename() != fs::u8path( fp3d::owned_world_marker ) ||
            ( fs::is_directory( it->symlink_status( ec ) ) && !fs::is_empty( it->path(), ec ) ) ||
            fs::is_symlink( it->symlink_status( ec ) ) ) {
            return false;
        }
    }
    return !ec;
}

// Saves and worlds that must not resume: without `fp3d_range` (or a crafted one), in a range world FP3D didn't make (no
// marker), in a world of FP3D's with another name; each refused, the game left a normal one (no zones, operations
// refused). Then the saved seed and AI freeze read back bounded, and range_world's names.
void check_resume_refusals( size_t pick )
{
    std::printf( "resume refusals:\n" );
    const fs::path saves = fs::u8path( PATH_INFO::savedir() );
    // Scratch worlds: name, marker (0 none, 1 file, 2 a folder of that name), whether range_world says yes.
    struct scratch {
        const char *name;
        int marker;
        bool range;
    };
    const std::vector<scratch> scratches = {
        { "fp3d-range-987654321", 1, true },
        { "fp3d-range-987654320", 0, false },
        { "fp3d-range-987654322", 2, false },
        { "fp3d-rangex", 1, false },
        { "fp3d-range-", 1, false },
        { "fp3d-range-9x", 1, false },
        { "fp3d-range--9", 1, false },
        { "fp3d-range-1234567890", 1, false },
        { "fp3d-plain-world", 1, false },
    };
    std::vector<fs::path> made;
    std::string setup;
    for( const scratch &w : scratches ) {
        const fs::path dir = saves / fs::u8path( w.name );
        std::error_code ec;
        if( fs::exists( dir, ec ) ) {
            if( !only_markers( dir ) ) {
                setup += std::string( "[" ) + w.name + " exists] ";
                continue;
            }
            fs::remove_all( dir, ec );
        }
        if( !( w.marker == 0 ? fs::create_directories( dir, ec ) : write_marker( dir, w.marker == 2 ) ) ) {
            setup += std::string( "[" ) + w.name + " not made] ";
        }
        made.push_back( dir );
    }

    // range_world on names (any thread).
    std::string world;
    on_sim( [&] {
        world = world_generator->active_world->world_name;
    } );
    std::string wrong;
    const auto expect = [&wrong]( const std::string & name, bool want ) {
        if( fp3d::range_world( name ) != want ) {
            wrong += "[" + name + "] ";
        }
    };
    expect( world, true );
    for( const scratch &w : scratches ) {
        expect( w.name, w.range );
    }
    for( const std::string &name : {
             std::string(), std::string( "fp3d" ), std::string( "fp3d-ran" ), std::string( "FP3D-range" ),
             std::string( "Fp3d-Range" ), std::string( "fp3d-range/../fp3d-range" ),
             std::string( "fp3d-range-1/../../fp3d-range" ), std::string( "fp3d-range" ) + '\0',
             std::string( "fp3d-range-99999" ) + std::string( 1, '\0' ) + "9", std::string( "fp3d-range-55555" )
         } ) {
        expect( name, false );
    }
    check( setup.empty() && wrong.empty(), "range_world: fp3d-range(-digits) with FP3D's marker only",
           "active " + world + " " + setup + wrong );

    struct outcome {
        bool resumed = true;
        bool active = true;
        size_t zones = 1;
        std::string heal;
        bool frozen = true;
    };
    const auto try_resume = []() {
        outcome o;
        o.resumed = fp3d::resume_test_range();
        o.active = fp3d::range_active();
        o.zones = fp3d::range_zones().size();
        o.heal = fp3d::heal();
        o.frozen = fp3d::ai_frozen();
        return o;
    };
    const auto refused = []( const outcome & o ) {
        return !o.resumed && !o.active && o.zones == 0 && o.heal == "Only in the test range." && !o.frozen;
    };
    std::vector<std::pair<std::string, outcome>> refusals;
    outcome back;
    std::vector<std::pair<std::string, unsigned>> seeds;
    std::vector<std::pair<std::string, bool>> freezes;
    double seed_written = -1.0;
    bool marker_moved = false;
    bool marker_back = false;
    on_sim( [&] {
        avatar &u = get_avatar();
        u.remove_value( "fp3d_range" );
        refusals.emplace_back( "no fp3d_range", try_resume() );
        u.set_value( "fp3d_range", std::string( "1" ) );
        refusals.emplace_back( "fp3d_range a string", try_resume() );
        u.set_value( "fp3d_range", 0.0 );
        refusals.emplace_back( "fp3d_range 0", try_resume() );
        u.set_value( "fp3d_range", 1.0 );
        // A range world FP3D didn't make: its marker gone.
        const fs::path mark = saves / fs::u8path( world ) / fp3d::owned_world_marker;
        fs::path moved = mark;
        moved += ".moved";
        std::error_code ec;
        fs::rename( mark, moved, ec );
        marker_moved = !ec;
        refusals.emplace_back( "no marker", try_resume() );
        fs::rename( moved, mark, ec );
        marker_back = !ec;
        // Another world of FP3D's (its marker in place), not a range one: the active world swapped for a stand-in of
        // that name for the call (resume reads only its name).
        WORLD plain( "fp3d-plain-world" );
        WORLD *const active = world_generator->active_world;
        world_generator->active_world = &plain;
        refusals.emplace_back( "fp3d-plain-world", try_resume() );
        world_generator->active_world = active;
        back = try_resume();

        // The saved seed, bounded.
        const auto seed_of = [&]( const std::string & what, const std::optional<diag_value> &v ) {
            if( v ) {
                u.set_value( "fp3d_range_seed", *v );
            } else {
                u.remove_value( "fp3d_range_seed" );
            }
            const bool ok = fp3d::resume_test_range();
            seeds.emplace_back( what, ok ? fp3d::range_boot_seed() : 999u );
        };
        seed_of( "missing", std::nullopt );
        seed_of( "-1", diag_value( -1.0 ) );
        seed_of( "2^32", diag_value( 4294967296.0 ) );
        seed_of( "2.5", diag_value( 2.5 ) );
        seed_of( "NaN", diag_value( std::numeric_limits<double>::quiet_NaN() ) );
        seed_of( "inf", diag_value( std::numeric_limits<double>::infinity() ) );
        seed_of( "\"7\"", diag_value( std::string( "7" ) ) );
        seed_written = avatar_number( "fp3d_range_seed" );
        seed_of( "0", diag_value( 0.0 ) );
        seed_of( "2^32-1", diag_value( 4294967295.0 ) );
        seed_of( "4242", diag_value( 4242.0 ) );

        // The saved AI freeze: off only for 0.
        const auto freeze_of = [&]( const std::string & what, const std::optional<diag_value> &v ) {
            if( v ) {
                u.set_value( "fp3d_range_ai_frozen", *v );
            } else {
                u.remove_value( "fp3d_range_ai_frozen" );
            }
            const bool ok = fp3d::resume_test_range();
            freezes.emplace_back( what, ok && fp3d::ai_frozen() );
        };
        freeze_of( "missing", std::nullopt );
        freeze_of( "\"0\"", diag_value( std::string( "0" ) ) );
        freeze_of( "0.5", diag_value( 0.5 ) );
        freeze_of( "NaN", diag_value( std::numeric_limits<double>::quiet_NaN() ) );
        freeze_of( "1", diag_value( 1.0 ) );
        freeze_of( "0", diag_value( 0.0 ) );
        // Left as the reload left it: a range, AI running, in the zone.
        fp3d::resume_test_range();
    } );
    for( const fs::path &dir : made ) {
        std::error_code ec;
        fs::remove_all( dir, ec );
    }
    std::string bad;
    for( const auto &[what, o] : refusals ) {
        if( !refused( o ) ) {
            bad += "[" + what + ( o.resumed ? " resumed" : "" ) + ( o.active ? " active" : "" ) + " " + o.heal + "] ";
        }
    }
    check( refusals.size() == 5 && bad.empty() && marker_moved && marker_back,
           "resume refused: no or crafted fp3d_range, no marker, not a range world", bad );
    check( back.resumed && back.active && back.zones > pick && back.heal.empty(),
           "resume: yes again once the save and world are a range's" );
    std::string seed_text;
    bool seeds_ok = seeds.size() == 10;
    for( size_t i = 0; i < seeds.size(); i++ ) {
        const unsigned want = i < 7 ? 1u : i == 7 ? 0u : i == 8 ? 4294967295u : 4242u;
        seeds_ok = seeds_ok && seeds[i].second == want;
        seed_text += seeds[i].first + "=" + std::to_string( seeds[i].second ) + " ";
    }
    check( seeds_ok && seed_written == 1.0, "resume: a bad saved seed reads as 1", seed_text );
    std::string freeze_text;
    bool freezes_ok = freezes.size() == 6;
    for( size_t i = 0; i < freezes.size(); i++ ) {
        freezes_ok = freezes_ok && freezes[i].second == ( i < 5 );
        freeze_text += freezes[i].first + "=" + ( freezes[i].second ? "frozen " : "running " );
    }
    check( freezes_ok, "resume: the saved AI freeze is off only for 0", freeze_text );
}
} // namespace

int main( int argc, char **argv )
{
    std::setvbuf( stdout, nullptr, _IONBF, 0 );
    bool all_zones = false;
    for( int i = 1; i < argc; i++ ) {
        all_zones = all_zones || std::strcmp( argv[i], "--all-zones" ) == 0;
    }
#if defined(_WIN32)
    // CDDA's crash handler shows a message box; a test must print the stack and exit instead.
    AddVectoredExceptionHandler( 1, on_fatal_exception );
#endif
    test_mode = true;
    setupDebug( DebugOutput::std_err );
    fp3d::set_active( true );
    const auto boot_start = std::chrono::steady_clock::now();
    std::thread sim( sim_main );

    const std::optional<fp3d::ui_request> first = main_loop( 0, 600000 );
    if( !first ) {
        std::printf( "FAIL: no main-loop request after boot (%s)\n", sim_error.c_str() );
        fp3d::shutdown();
        sim.join();
        return EXIT_FAILURE;
    }
    std::printf( "boot: first request after %.1f s\n", ms_since( boot_start ) / 1000.0 );

    check_zone_ids();
    const size_t gallery = check_registry();
    check_hub();
    check_frozen( "hub" );
    check_operations( gallery );
    check_turn_time( "hub" );
    check_unfreeze();
    check_gallery( gallery );
    check_teleport_arrival( gallery );
    check_turn_time( "gallery" );
    check_info( gallery );
    check_arrival_hazards( gallery );
    check_crowded_arrival( gallery );
    check_riding( gallery );
    check_back_to_hub();
    check_rebuild( gallery );
    check_rebuild_edge( gallery );
    check_zone_kinds();
    if( all_zones ) {
        check_all_zones();
    }
    check_resume_refusals( check_reload() );

    fp3d::shutdown();
    sim.join();
    if( !sim_error.empty() ) {
        std::printf( "sim error: %s\n", sim_error.c_str() );
        failed++;
    }
    std::printf( "%d passed, %d failed\n", passed, failed );
    if( failed == 0 ) {
        std::printf( "all checks passed\n" );
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
