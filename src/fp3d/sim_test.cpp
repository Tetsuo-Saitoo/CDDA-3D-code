// FP3D block B2 test: runs CDDA's real game loop on a worker thread behind the bridge and plays it from the
// main thread like a client would: open a door, pick up an apple, eat it. Every request is printed.
// Exit code 0 only if all three goals are verified from game state.
//
// usage: fp3d-sim-test   (run from the cdda/ folder)
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <iostream>
#if defined(_WIN32)
#include <windows.h>
#endif

#include "avatar.h"
#include "cached_options.h"
#include "calendar.h"
#include "coordinates.h"
#include "debug.h"
#include "do_turn.h"
#include "fp3d/boot.h"
#include "fp3d/bridge.h"
#include "fp3d/game_access.h"
#include "game.h"
#include "item.h"
#include "map.h"
#include "mapdata.h"
#include "messages.h"
#include "rng.h"
#include "type_id.h"

namespace
{
const itype_id itype_apple( "apple" );
const ter_str_id ter_t_door_c( "t_door_c" );
const ter_str_id ter_t_floor( "t_floor" );

// Game state the driver checks; copied on the sim thread right before every request.
struct observed {
    int64_t turn = 0;
    std::string east_ter;
    int apples_here = 0;
    int apples_carried = 0;
    std::vector<std::string> messages;
};
std::mutex observed_mutex;
observed last_observed;
std::string sim_error;

void observe()
{
    const avatar &u = get_avatar();
    map &here = get_map();
    const tripoint_bub_ms p = u.pos_bub();
    observed o;
    o.turn = to_turns<int64_t>( calendar::turn - calendar::turn_zero );
    o.east_ter = here.ter( p + tripoint_rel_ms( 1, 0, 0 ) ).id().str();
    for( const item &it : here.i_at( p ) ) {
        if( it.typeId() == itype_apple ) {
            o.apples_here++;
        }
    }
    o.apples_carried = u.amount_of( itype_apple );
    for( const auto &m : Messages::recent_messages( 6 ) ) {
        o.messages.push_back( m.second );
    }
    std::lock_guard<std::mutex> lk( observed_mutex );
    last_observed = o;
}

observed snapshot()
{
    std::lock_guard<std::mutex> lk( observed_mutex );
    return last_observed;
}

// A 3x3 floor around the avatar, a closed door to the east, an apple underfoot, no monsters.
void build_scene()
{
    map &here = get_map();
    const tripoint_bub_ms p = get_avatar().pos_bub();
    for( int dx = -1; dx <= 1; dx++ ) {
        for( int dy = -1; dy <= 1; dy++ ) {
            const tripoint_bub_ms q = p + tripoint_rel_ms( dx, dy, 0 );
            here.ter_set( q, ter_t_floor );
            here.furn_set( q, furn_str_id::NULL_ID() );
            here.i_clear( q );
        }
    }
    here.ter_set( p + tripoint_rel_ms( 1, 0, 0 ), ter_t_door_c );
    here.add_item( p, item( itype_apple ) );
    g->clear_zombies();
}

void sim_main()
{
    try {
        fp3d::boot_world( "fp3d-sim", true );
        std::printf( "sim: data loaded, starting game\n" );
        // The real new-game path: calendar, start location, starting items, game mode.
        if( !fp3d_game_access::start_game( *g ) ) {
            throw std::runtime_error( "start_game failed" );
        }
        build_scene();
        std::printf( "sim: game started, scene built\n" );
        fp3d::set_before_ask_hook( observe );
        while( !do_turn() ) {
        }
        sim_error = "game ended";
    } catch( const fp3d::shutdown_signal & ) {
        // normal end: the driver shut the bridge down
    } catch( const std::exception &e ) {
        sim_error = e.what();
    }
    fp3d::shutdown();
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

bool has( const std::vector<std::string> &v, const std::string &s )
{
    return std::find( v.begin(), v.end(), s ) != v.end();
}

std::string first_entry_with( const fp3d::ui_request &req, const std::string &needle, int &id )
{
    for( const fp3d::ui_entry &e : req.entries ) {
        if( e.enabled && e.label.find( needle ) != std::string::npos ) {
            id = e.id;
            return e.label;
        }
    }
    id = -1;
    return "";
}

struct goal {
    const char *action;      // main-mode action to issue
    const char *direction;   // answer for a direction prompt
    const char *what;
    bool ( *done )( const observed & );
};

const goal goals[] = {
    { "open", "RIGHT", "open the door east", []( const observed & o ) { return o.east_ter == "t_door_o"; } },
    { "pickup", "pause", "pick up the apple", []( const observed & o ) { return o.apples_here == 0 && o.apples_carried == 1; } },
    { "eat", "pause", "eat the apple", []( const observed & o ) { return o.apples_carried == 0 && o.apples_here == 0; } },
};
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
    rng_set_engine_seed( 42 );
    fp3d::set_active( true );
    std::thread sim( sim_main );

    size_t g_idx = 0;
    bool issued = false;
    int passed = 0;
    int requests = 0;
    while( g_idx < std::size( goals ) && requests < 200 ) {
        const std::optional<fp3d::ui_request> req = fp3d::wait_request( 180000 );
        if( !req && !fp3d::is_shut_down() ) {
            // The sim is stuck outside ask() (a loop that never asks); join() would hang.
            std::printf( "FAIL: no request within 180 s; timed-out polls: %s\n", fp3d::poll_report().c_str() );
            std::_Exit( 2 );
        }
        if( !req ) {
            std::printf( "no request within the time limit (or sim stopped)\n" );
            break;
        }
        requests++;
        const goal &gl = goals[g_idx];
        fp3d::ui_answer a;
        a.id = req->id;
        std::string note;
        const bool main_mode = req->kind == fp3d::request_kind::key && has( req->actions, gl.action );
        if( main_mode && issued ) {
            // Back in main mode after issuing the goal's action: check the result.
            const observed o = snapshot();
            const bool ok = gl.done( o );
            std::printf( "  goal %-20s %s (turn %lld)\n", gl.what, ok ? "PASS" : "FAIL",
                         static_cast<long long>( o.turn ) );
            for( const std::string &m : o.messages ) {
                std::printf( "    msg: %s\n", m.c_str() );
            }
            passed += ok ? 1 : 0;
            issued = false;
            if( ++g_idx == std::size( goals ) ) {
                break;
            }
        }
        const goal &cur = goals[g_idx];
        switch( req->kind ) {
            case fp3d::request_kind::key:
                if( has( req->actions, cur.action ) ) {
                    a.action = cur.action;
                    issued = true;
                } else if( has( req->actions, cur.direction ) ) {
                    a.action = cur.direction;
                } else {
                    a.cancel = true;
                }
                note = a.cancel ? "cancel" : a.action;
                break;
            case fp3d::request_kind::popup:
                a.action = has( req->actions, "YES" ) ? "YES" : req->actions.front();
                note = a.action;
                break;
            case fp3d::request_kind::item_select:
            case fp3d::request_kind::item_multi: {
                int id = -1;
                note = first_entry_with( *req, "apple", id );
                a.entry = id;
                a.cancel = id < 0;
                if( id >= 0 ) {
                    a.picks.emplace_back( id, 0 );
                }
                break;
            }
            case fp3d::request_kind::menu:
            case fp3d::request_kind::string:
                a.cancel = true;
                note = "cancel";
                break;
        }
        std::printf( "[%llu] %-11s ctx=%-12s actions=%zu entries=%zu -> %s\n",
                     static_cast<unsigned long long>( req->id ), fp3d::kind_name( req->kind ),
                     req->context.c_str(), req->actions.size(), req->entries.size(), note.c_str() );
        if( !req->text.empty() ) {
            std::printf( "    text: %.120s\n", req->text.c_str() );
        }
        fp3d::answer( a );
    }

    fp3d::shutdown();
    sim.join();
    if( !sim_error.empty() ) {
        std::printf( "sim error: %s\n", sim_error.c_str() );
    }
    std::printf( "%d/%zu goals passed, %d requests\n", passed, std::size( goals ), requests );
    return passed == static_cast<int>( std::size( goals ) ) ? EXIT_SUCCESS : EXIT_FAILURE;
}
