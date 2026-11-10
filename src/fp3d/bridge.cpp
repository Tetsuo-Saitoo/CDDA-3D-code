#include "fp3d/bridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <map>
#include <mutex>

#include "calendar.h"
#include "fp3d/cooking.h"
#include "map_scale_constants.h"

namespace fp3d
{
namespace
{
// A queued main-loop action; item actions carry their details and go out as `fp3d_item`.
struct queued_action {
    std::string action;
    std::optional<item_action> item;
    std::optional<stairs_action> stairs;
};

// A recipe query (L9) waiting for the sim.
struct recipe_query {
    uint64_t id = 0;
    item_ref container;
    ui_point pos;
};

// Thrown by ask() when CDDA code run by a query wants to ask the client something (a debug popup): queries
// never prompt. Not a std::exception, so CDDA's handlers let it unwind to run_queries().
struct query_prompt {};

struct channel {
    std::mutex m;
    std::condition_variable cv;
    std::optional<ui_request> pending;
    std::optional<ui_answer> reply;
    uint64_t next_id = 1;
    bool shut_down = false;
    std::function<void()> before_ask;
    std::deque<queued_action> actions;   // queued main-loop actions (block B5)
    std::optional<item_action> item_now; // the item action last answered as `fp3d_item`, until the sim takes it
    std::optional<stairs_action> stairs_now; // ... and the stairs action answered as `fp3d_stairs`
    bool save_wanted = false;            // request_save() until the sim takes it
    bool job_wanted = false;             // want_job() until the main loop takes it
    bool save_if_changed = false;
    save_status saves;
    clock_state clock;
    // Wall time the current game turn started; real time ends it turn_length() later.
    std::chrono::steady_clock::time_point turn_start = std::chrono::steady_clock::now();
    std::deque<recipe_query> queries;    // posted, not run yet (L9)
    std::deque<std::pair<uint64_t, std::string>> query_results;   // run, not taken yet
    uint64_t next_query_id = 1;
};

channel &chan()
{
    static channel c;
    return c;
}

std::atomic<bool> bridge_active{ false };
std::atomic<bool> interrupt_requested{ false };
thread_local bool next_timed = false;
thread_local bool hook_timed = false;
thread_local bool in_query = false;
std::mutex poll_mutex;
std::map<std::string, uint64_t> poll_counts;
// Answer coordinates feed CDDA's int point maths (absolute -> bubble); keep them far from overflow.
constexpr int max_answer_coord = 1 << 28;

bool coord_ok( int v )
{
    return v >= -max_answer_coord && v <= max_answer_coord;
}

std::chrono::steady_clock::duration turn_length( double timescale )
{
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
               std::chrono::duration<double, std::milli>( 1000.0 / timescale ) );
}

// Sim side, with `lk` holding c.m: runs the pending recipe queries one by one, the lock released while CDDA code
// runs. Returns with the lock held; stops early on shutdown (the caller then unwinds).
void run_queries( channel &c, std::unique_lock<std::mutex> &lk )
{
    while( !c.queries.empty() && !c.shut_down ) {
        const recipe_query q = c.queries.front();
        c.queries.pop_front();
        lk.unlock();
        std::string result;
        {
            struct query_scope {
                query_scope() {
                    in_query = true;
                }
                ~query_scope() {
                    in_query = false;
                }
            } scope;
            try {
                result = run_recipe_query( q.id, q.container, q.pos );
            } catch( const query_prompt & ) {
                result = recipe_query_error_json( q.id, q.container.uid, "internal" );
            } catch( const std::exception & ) {
                result = recipe_query_error_json( q.id, q.container.uid, "internal" );
            }
        }
        if( result.size() > max_query_result_bytes ) {
            result = recipe_query_error_json( q.id, q.container.uid, "too large" );
        }
        lk.lock();
        while( c.query_results.size() >= max_query_results ) {
            c.query_results.pop_front();
        }
        c.query_results.emplace_back( q.id, std::move( result ) );
        c.cv.notify_all();
    }
}
} // namespace

const char *kind_name( request_kind k )
{
    switch( k ) {
        case request_kind::key:
            return "key";
        case request_kind::menu:
            return "menu";
        case request_kind::popup:
            return "popup";
        case request_kind::string:
            return "string";
        case request_kind::item_select:
            return "item_select";
        case request_kind::item_multi:
            return "item_multi";
        case request_kind::direction:
            return "direction";
        case request_kind::chargen:
            return "chargen";
        case request_kind::world_setup:
            return "world_setup";
        case request_kind::target:
            return "target";
        case request_kind::shot:
            return "shot";
        case request_kind::swing:
            return "swing";
    }
    return "?";
}

bool active()
{
    return bridge_active.load( std::memory_order_relaxed );
}

void set_active( bool on )
{
    bridge_active.store( on );
}

void note_poll( const std::string &where )
{
    std::lock_guard<std::mutex> lk( poll_mutex );
    // bounded: one counter per distinct place, and places are a small fixed set of contexts
    if( poll_counts.size() < 256 || poll_counts.count( where ) ) {
        poll_counts[where]++;
    }
}

std::string poll_report()
{
    std::lock_guard<std::mutex> lk( poll_mutex );
    std::string out;
    for( const auto &[where, n] : poll_counts ) {
        out += where + "=" + std::to_string( n ) + " ";
    }
    return out.empty() ? "none" : out;
}

void set_before_ask_hook( std::function<void()> hook )
{
    std::lock_guard<std::mutex> lk( chan().m );
    chan().before_ask = std::move( hook );
}

void set_next_timed()
{
    next_timed = true;
}

bool next_is_timed()
{
    return next_timed;
}

bool in_timed_hook()
{
    return hook_timed;
}

ui_answer ask( ui_request req )
{
    if( in_query ) {
        throw query_prompt();
    }
    channel &c = chan();
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lk( c.m );
        if( c.shut_down ) {
            throw shutdown_signal();
        }
        hook = c.before_ask;
    }
    // Taken before the hook, so a request asked inside it isn't posted as the main loop's.
    const bool timed = std::exchange( next_timed, false );
    // Not again from a request asked inside the hook itself (CDDA's debug message prompt, say). A shot request
    // skips it too: the client flies the shot against the figures it already shows, and a burst asks once per
    // round while the game waits. So does a swing request (the strike lands on what the client shows).
    static thread_local bool in_hook = false;
    if( hook && !in_hook && req.kind != request_kind::shot && req.kind != request_kind::swing ) {
        in_hook = true;
        hook_timed = timed;
        try {
            hook();
        } catch( ... ) {
            in_hook = false;
            hook_timed = false;
            throw;
        }
        in_hook = false;
        hook_timed = false;
    }
    req.timed = timed;
    req.turn = to_turns<int64_t>( calendar::turn - calendar::turn_zero );
    if( req.candidates.size() > max_candidates ) {
        req.candidates.resize( max_candidates );
    }
    std::unique_lock<std::mutex> lk( c.m );
    if( timed ) {
        run_queries( c, lk );   // the main loop answers recipe queries before it waits
    }
    req.id = c.next_id++;
    const request_kind kind = req.kind;
    req.paused = c.clock.paused;
    req.pause_reason = c.clock.pause_reason;
    const uint64_t id = req.id;
    c.reply.reset();
    c.item_now.reset();   // only the answer to this request may carry one (a client can't send `fp3d_item` itself)
    c.stairs_now.reset();
    c.pending = std::move( req );
    c.cv.notify_all();
    bool expired = false;
    for( ;; ) {
        if( c.shut_down ) {
            c.pending.reset();
            throw shutdown_signal();
        }
        if( c.reply && c.reply->id == id ) {
            break;
        }
        // A save goes first and doesn't wait for pause to end; the sim asks again afterwards.
        if( timed && c.save_wanted ) {
            ui_answer save;
            save.id = id;
            save.action = save_action;
            save.queued = true;
            c.reply = std::move( save );
            break;
        }
        // Cheats and the test kit (cheats.h), like a save: in the main loop, also while paused.
        if( timed && c.job_wanted ) {
            c.job_wanted = false;
            ui_answer job;
            job.id = id;
            job.action = job_action;
            job.queued = true;
            c.reply = std::move( job );
            break;
        }
        // Checked under the same lock that posted the request, so a client never sees it as waiting while
        // a queued action is about to answer it.
        if( timed && !c.actions.empty() && !c.clock.paused ) {
            ui_answer queued;
            queued.id = id;
            queued.action = std::move( c.actions.front().action );
            c.item_now = std::move( c.actions.front().item );
            c.stairs_now = std::move( c.actions.front().stairs );
            queued.queued = true;
            c.actions.pop_front();
            c.reply = std::move( queued );
            break;
        }
        // Recipe queries posted while the main loop waits (paused too); then everything is checked again.
        if( timed && !c.queries.empty() ) {
            run_queries( c, lk );
            continue;
        }
        // Re-evaluated on every wake-up: the client may switch mode, timescale or pause meanwhile.
        if( timed && c.clock.mode == clock_mode::real_time && !c.clock.paused ) {
            const std::chrono::steady_clock::time_point deadline = c.turn_start + turn_length(
                        c.clock.timescale );
            if( std::chrono::steady_clock::now() >= deadline ) {
                expired = true;
                break;
            }
            c.cv.wait_until( lk, deadline );
        } else {
            c.cv.wait( lk );
        }
    }
    c.pending.reset();
    if( expired ) {
        // A late answer to this id is ignored by answer(); the client re-sends it to the next request.
        c.reply.reset();
        ui_answer a;
        a.id = id;
        a.timed_out = true;
        return a;
    }
    ui_answer a = std::move( *c.reply );
    c.reply.reset();
    a.timed_out = false;
    a.text = sanitize_text( a.text, kind == request_kind::chargen || kind == request_kind::world_setup ? max_answer_json : max_answer_text );
    a.action = sanitize_text( a.action, 128 );
    if( a.picks.size() > max_answer_picks ) {
        a.picks.resize( max_answer_picks );
    }
    if( a.pos && !( coord_ok( a.pos->x ) && coord_ok( a.pos->y ) && coord_ok( a.pos->z ) ) ) {
        a.pos.reset();
    }
    // A client shot only answers a target, shot or swing request, lands somewhere real, has a finite quality and
    // names a short id. Only a target answer can ask for shot requests (`fly`); a swing answer has no pellets.
    if( a.shot && ( ( kind != request_kind::target && kind != request_kind::shot && kind != request_kind::swing ) ||
                    !( coord_ok( a.shot->end.x ) && coord_ok( a.shot->end.y ) && coord_ok( a.shot->end.z ) ) ||
                    !std::isfinite( a.shot->quality ) ) ) {
        a.shot.reset();
    }
    if( a.shot ) {
        a.shot->part = sanitize_text( a.shot->part, 64 );
        // Not a number is not a quality: the worst one.
        a.shot->quality = std::isfinite( a.shot->quality ) ? std::clamp( a.shot->quality, 0.0f, 0.99f ) : 0.99f;
        a.shot->fly = a.shot->fly && kind == request_kind::target;
        if( kind == request_kind::swing ) {
            a.shot->pellets.clear();
        }
    }
    return a;
}

std::optional<ui_request> wait_request( int timeout_ms, uint64_t newer_than )
{
    channel &c = chan();
    std::unique_lock<std::mutex> lk( c.m );
    c.cv.wait_for( lk, std::chrono::milliseconds( timeout_ms ), [&] {
        return c.shut_down || ( c.pending && !c.reply && c.pending->id > newer_than );
    } );
    if( c.shut_down || !c.pending || c.reply || c.pending->id <= newer_than ) {
        return std::nullopt;
    }
    return c.pending;
}

clock_state get_clock()
{
    std::lock_guard<std::mutex> lk( chan().m );
    return chan().clock;
}

void set_clock_mode( clock_mode m )
{
    channel &c = chan();
    std::lock_guard<std::mutex> lk( c.m );
    c.clock.mode = m;
    c.turn_start = std::chrono::steady_clock::now();
    c.cv.notify_all();
}

void set_timescale( double scale )
{
    if( !( scale == scale ) ) {
        scale = 1.0;
    }
    channel &c = chan();
    std::lock_guard<std::mutex> lk( c.m );
    c.clock.timescale = std::clamp( scale, min_timescale, max_timescale );
    c.cv.notify_all();
}

void set_paused( bool paused )
{
    channel &c = chan();
    std::lock_guard<std::mutex> lk( c.m );
    if( c.clock.paused && !paused ) {
        // A resumed turn gets its full length.
        c.turn_start = std::chrono::steady_clock::now();
    }
    c.clock.paused = paused;
    c.clock.pause_reason.clear();
    c.cv.notify_all();
}

void auto_pause( const std::string &reason )
{
    channel &c = chan();
    std::lock_guard<std::mutex> lk( c.m );
    if( c.clock.mode != clock_mode::real_time || c.clock.paused ) {
        return;
    }
    c.clock.paused = true;
    c.clock.pause_reason = sanitize_text( reason, 128 );
    c.actions.clear();   // don't walk on into what made us stop
    c.cv.notify_all();
}

bool want_job()
{
    channel &c = chan();
    std::lock_guard<std::mutex> lk( c.m );
    if( c.shut_down ) {
        return false;
    }
    c.job_wanted = true;
    c.cv.notify_all();
    return true;
}

void set_auto_pause_hostiles( bool on )
{
    std::lock_guard<std::mutex> lk( chan().m );
    chan().clock.auto_pause_hostiles = on;
}

void hostile_spotted()
{
    bool on = false;
    {
        std::lock_guard<std::mutex> lk( chan().m );
        on = chan().clock.auto_pause_hostiles;
    }
    if( on ) {
        auto_pause( "hostile spotted" );
    }
}

bool queue_action( const std::string &action )
{
    // The bridge's own answers (a save, cheats) only come from the bridge.
    if( action == save_action || action == job_action || action == item_action_name ||
        action == stairs_action_name ) {
        return false;
    }
    channel &c = chan();
    std::lock_guard<std::mutex> lk( c.m );
    if( c.shut_down || c.actions.size() >= max_queued_actions ) {
        return false;
    }
    c.actions.push_back( queued_action{ sanitize_text( action, 128 ), std::nullopt, std::nullopt } );
    c.cv.notify_all();
    return true;
}

bool queue_item_action( const item_action &a )
{
    const auto ref_ok = []( const item_ref & r ) {
        return r.uid >= 1 && r.uid <= max_item_uid && r.type >= 1 && r.type < ( 1u << 24 );
    };
    const bool op_ok = a.op == item_op::take || a.op == item_op::put || a.op == item_op::drop ||
                       a.op == item_op::unload || a.op == item_op::reload || a.op == item_op::clear ||
                       a.op == item_op::throw_ || a.op == item_op::pour;
    const bool hand_ok = a.hand == item_hand::pack || a.hand == item_hand::main || a.hand == item_hand::off;
    // pour: exactly one target. Into a container: a whole ref, and no square (pos left at 0, 0, 0). Onto a square:
    // no container at all (uid 0, type 0) and no pocket.
    const bool no_container = a.container.uid == 0 && a.container.type == 0;
    const bool no_pos = a.pos.x == 0 && a.pos.y == 0 && a.pos.z == 0;
    const bool pour_ok = a.op != item_op::pour ||
                         ( no_container ? a.pocket == -1 : ref_ok( a.container ) && no_pos );
    if( !op_ok || !hand_ok || !ref_ok( a.item ) || !coord_ok( a.pos.x ) || !coord_ok( a.pos.y ) ||
        !coord_ok( a.pos.z ) || a.pocket < -1 || a.pocket > max_item_pocket || !pour_ok ||
        ( ( a.op == item_op::put || a.op == item_op::reload ) && !ref_ok( a.container ) ) ||
        ( a.has_spot && ( a.spot_x >= spot_scale || a.spot_y >= spot_scale ) ) ) {
        return false;
    }
    channel &c = chan();
    std::lock_guard<std::mutex> lk( c.m );
    if( c.shut_down || c.actions.size() >= max_queued_actions ) {
        return false;
    }
    c.actions.push_back( queued_action{ item_action_name, a, std::nullopt } );
    c.cv.notify_all();
    return true;
}

std::optional<item_action> take_item_action()
{
    channel &c = chan();
    std::lock_guard<std::mutex> lk( c.m );
    std::optional<item_action> a = std::move( c.item_now );
    c.item_now.reset();
    return a;
}

bool queue_stairs_action( const stairs_action &a )
{
    if( ( a.dir != 1 && a.dir != -1 ) || !coord_ok( a.pos.x ) || !coord_ok( a.pos.y ) ||
        a.pos.z < -OVERMAP_DEPTH || a.pos.z > OVERMAP_HEIGHT ) {
        return false;
    }
    channel &c = chan();
    std::lock_guard<std::mutex> lk( c.m );
    if( c.shut_down || c.actions.size() >= max_queued_actions ) {
        return false;
    }
    c.actions.push_back( queued_action{ stairs_action_name, std::nullopt, a } );
    c.cv.notify_all();
    return true;
}

std::optional<stairs_action> take_stairs_action()
{
    channel &c = chan();
    std::lock_guard<std::mutex> lk( c.m );
    std::optional<stairs_action> a = std::move( c.stairs_now );
    c.stairs_now.reset();
    return a;
}

uint64_t post_recipe_query( const item_ref &container, const ui_point &pos )
{
    if( container.uid < 1 || container.uid > max_item_uid || container.type < 1 || container.type >= ( 1u << 24 ) ||
        !coord_ok( pos.x ) || !coord_ok( pos.y ) || !coord_ok( pos.z ) ) {
        return 0;
    }
    channel &c = chan();
    std::lock_guard<std::mutex> lk( c.m );
    if( c.shut_down || c.queries.size() >= max_pending_queries ) {
        return 0;
    }
    const uint64_t id = c.next_query_id++;
    c.queries.push_back( recipe_query{ id, container, pos } );
    c.cv.notify_all();
    return id;
}

std::optional<std::string> take_query_result( uint64_t id )
{
    channel &c = chan();
    std::lock_guard<std::mutex> lk( c.m );
    for( auto it = c.query_results.begin(); it != c.query_results.end(); ++it ) {
        if( it->first == id ) {
            std::string result = std::move( it->second );
            c.query_results.erase( it );
            return result;
        }
    }
    return std::nullopt;
}

size_t pending_queries()
{
    std::lock_guard<std::mutex> lk( chan().m );
    return chan().queries.size();
}

bool request_save( bool only_if_changed )
{
    channel &c = chan();
    std::lock_guard<std::mutex> lk( c.m );
    if( c.shut_down ) {
        return false;
    }
    // A plain request wins over a pending autosave's "only if changed".
    c.save_if_changed = c.save_wanted ? c.save_if_changed && only_if_changed : only_if_changed;
    c.save_wanted = true;
    c.saves.pending = true;
    c.cv.notify_all();
    return true;
}

save_status get_save_status()
{
    std::lock_guard<std::mutex> lk( chan().m );
    return chan().saves;
}

std::optional<bool> take_save_request()
{
    channel &c = chan();
    std::lock_guard<std::mutex> lk( c.m );
    if( !c.save_wanted ) {
        return std::nullopt;
    }
    c.save_wanted = false;
    return c.save_if_changed;
}

void note_saved( bool ok )
{
    channel &c = chan();
    std::lock_guard<std::mutex> lk( c.m );
    c.saves.done++;
    c.saves.ok = ok;
    c.saves.pending = c.save_wanted;   // asked again while this one ran
}

void clear_actions()
{
    std::lock_guard<std::mutex> lk( chan().m );
    chan().actions.clear();
}

size_t queued_actions()
{
    std::lock_guard<std::mutex> lk( chan().m );
    return chan().actions.size();
}

void pace_turn( bool fast_forward, bool watched )
{
    channel &c = chan();
    std::unique_lock<std::mutex> lk( c.m );
    std::optional<std::chrono::steady_clock::time_point> paced_until;
    for( ;; ) {
        if( c.shut_down ) {
            throw shutdown_signal();
        }
        // Recipe queries also get answered during activities and pauses.
        if( !in_query && !c.queries.empty() ) {
            run_queries( c, lk );
            continue;
        }
        if( c.clock.paused ) {
            c.cv.wait( lk );
            continue;
        }
        if( !watched && ( c.clock.mode == clock_mode::turn_based || fast_forward ) ) {
            paced_until.reset();
            break;
        }
        const std::chrono::steady_clock::time_point deadline = c.turn_start + turn_length(
                    c.clock.timescale );
        paced_until = deadline;
        if( std::chrono::steady_clock::now() >= deadline ) {
            break;
        }
        c.cv.wait_until( lk, deadline );
    }
    // Chain turns from the deadline so per-turn work doesn't add up to drift, unless the sim fell
    // a whole turn behind (then start fresh instead of rushing to catch up).
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if( paced_until && now - *paced_until < turn_length( c.clock.timescale ) ) {
        c.turn_start = *paced_until;
    } else {
        c.turn_start = now;
    }
}

void interrupt()
{
    interrupt_requested.store( true );
}

bool take_interrupt()
{
    return interrupt_requested.exchange( false );
}

void answer( ui_answer a )
{
    channel &c = chan();
    std::lock_guard<std::mutex> lk( c.m );
    if( !c.pending || c.pending->id != a.id || c.reply ) {
        return;
    }
    a.queued = false;   // the bridge's flag, not the client's
    c.reply = std::move( a );
    c.cv.notify_all();
}

void shutdown()
{
    channel &c = chan();
    std::lock_guard<std::mutex> lk( c.m );
    c.shut_down = true;
    c.cv.notify_all();
}

bool is_shut_down()
{
    std::lock_guard<std::mutex> lk( chan().m );
    return chan().shut_down;
}

std::string sanitize_text( const std::string &s, size_t max_len )
{
    std::string out;
    out.reserve( std::min( s.size(), max_len ) );
    for( const char ch : s ) {
        if( out.size() >= max_len ) {
            break;
        }
        const unsigned char u = static_cast<unsigned char>( ch );
        if( u >= 0x20 && u != 0x7f ) {
            out.push_back( ch );
        }
    }
    return out;
}

namespace
{
struct boot_state {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point phase_start = start;
    std::string phase;
    std::string step;
    uint64_t steps = 0;
    std::string last_error;
    uint64_t errors = 0;
};
std::mutex boot_mutex;
boot_state boot;

// sanitize_text to max_boot_text bytes, without a UTF-8 character cut in half at the end.
std::string boot_text( const std::string &s )
{
    std::string out = sanitize_text( s, max_boot_text );
    size_t lead = out.size();
    while( lead > 0 && ( static_cast<unsigned char>( out[lead - 1] ) & 0xC0 ) == 0x80 ) {
        lead--;
    }
    if( lead > 0 ) {
        const unsigned char b = static_cast<unsigned char>( out[lead - 1] );
        const size_t len = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
        if( lead - 1 + len > out.size() ) {
            out.resize( lead - 1 );
        }
    }
    return out;
}

double seconds_between( std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b )
{
    return std::chrono::duration<double>( b - a ).count();
}

void print_line( const std::string &line )
{
    std::fprintf( stdout, "%s\n", line.c_str() );
    std::fflush( stdout );
}
} // namespace

void begin_boot()
{
    std::lock_guard<std::mutex> lk( boot_mutex );
    boot = boot_state();
}

void note_boot_step( const std::string &phase, const std::string &step )
{
    const auto now = std::chrono::steady_clock::now();
    std::string line;
    {
        std::lock_guard<std::mutex> lk( boot_mutex );
        boot.steps++;
        boot.step = boot_text( step );
        std::string p = boot_text( phase );
        if( p != boot.phase ) {
            char buf[64];
            std::snprintf( buf, sizeof( buf ), "fp3d boot %7.1f s: ", seconds_between( boot.start, now ) );
            line = buf + p;
            if( !boot.phase.empty() ) {
                std::snprintf( buf, sizeof( buf ), " (%.1f s)", seconds_between( boot.phase_start, now ) );
                line += "   [after " + boot.phase + buf + "]";
            }
            boot.phase = std::move( p );
            boot.phase_start = now;
        }
    }
    if( !line.empty() ) {
        print_line( line );
    }
}

void note_debug_error( const std::string &text )
{
    std::string t = boot_text( text );
    bool print = false;
    {
        std::lock_guard<std::mutex> lk( boot_mutex );
        boot.errors++;
        print = boot.errors <= max_printed_errors;
        boot.last_error = t;
    }
    if( print ) {
        print_line( "fp3d: CDDA error: " + t );
    }
}

boot_progress latest_boot_progress()
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk( boot_mutex );
    boot_progress p;
    p.phase = boot.step.empty() ? boot.phase : boot.phase + ": " + boot.step;
    p.seconds = seconds_between( boot.start, now );
    p.phase_seconds = seconds_between( boot.phase_start, now );
    p.steps = boot.steps;
    p.last_error = boot.last_error;
    p.errors = boot.errors;
    return p;
}

} // namespace fp3d
