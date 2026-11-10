// K7 test range test: boots the range (fixed seed) with CDDA's game loop on a worker thread behind the bridge and
// drives it like a client. Checks the zone registry, every hub prop, the gallery (every terrain and furniture id, working
// stairs), the range operations (and that bad input is refused), frozen AI (and that unfreezing lets the hub zombie
// act), and the median turn time in the hub and the gallery. Range operations run on the sim thread, in the hook that
// runs before each request. Exit code 0 only if every check passed.
//
// usage: fp3d-range-test   (run from the cdda/ folder)
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif

#include "avatar.h"
#include "bodypart.h"
#include "cached_options.h"
#include "calendar.h"
#include "coordinates.h"
#include "creature_tracker.h"
#include "debug.h"
#include "do_turn.h"
#include "field_type.h"
#include "fp3d/boot.h"
#include "fp3d/bridge.h"
#include "fp3d/snapshot.h"
#include "fp3d/test_range.h"
#include "game.h"
#include "item.h"
#include "map.h"
#include "mapdata.h"
#include "monster.h"
#include "mtype.h"
#include "npc.h"
#include "type_id.h"
#include "vehicle.h"
#include "vpart_position.h"

namespace
{
std::mutex job_mutex;
std::function<void()> job;     // run by the sim right before its next request
std::vector<double> snapshot_ms;   // capture + publish time per request (job_mutex)
std::string sim_error;
int failed = 0;
int passed = 0;

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
    std::printf( "    content: %zu guns, %zu other items in %zu categories, %zu vehicle prototypes, %zu monsters, "
                 "%zu terrain, %zu furniture (%zu emit fields)\n", counts.guns, counts.items,
                 counts.items_per_category.size(), counts.vehicles, counts.monsters, counts.terrain,
                 counts.furniture, counts.emitters );
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

void check_hub()
{
    std::printf( "hub:\n" );
    struct prop_result {
        std::string what;
        bool ok = false;
        std::string detail;
    };
    std::vector<prop_result> results;
    bool at_centre = false;
    size_t npcs = 99;
    mon_state mons;
    on_sim( [&] {
        map &here = get_map();
        const tripoint_abs_ms s = centre_of( 0 );
        at_centre = get_avatar().pos_abs() == s;
        npcs = 0;
        for( const npc &n : g->all_npcs() ) {
            npcs += n.is_dead() ? 0 : 1;
        }
        mons = monsters_now();
        for( const fp3d::hub_prop &p : fp3d::hub_props() )
        {
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
             fp3d::teleport_to_zone( 0 ), fp3d::set_time_of_day( 600 ), fp3d::advance( 1 ), fp3d::heal()
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
    int minute_now = -1;
    bad.clear();
    good.clear();
    on_sim( [&] {
        before_turn = to_turns<int64_t>( calendar::turn - calendar::turn_zero );
        good = fp3d::set_time_of_day( 600 );
        after_turn = to_turns<int64_t>( calendar::turn - calendar::turn_zero );
        minute_now = to_minutes<int>( time_past_midnight( calendar::turn ) );
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
} // namespace

int main()
{
    std::setvbuf( stdout, nullptr, _IONBF, 0 );
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

    const size_t gallery = check_registry();
    check_hub();
    check_frozen( "hub" );
    check_operations( gallery );
    check_turn_time( "hub" );
    check_unfreeze();
    check_gallery( gallery );
    check_turn_time( "gallery" );
    check_info( gallery );
    check_back_to_hub();

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
