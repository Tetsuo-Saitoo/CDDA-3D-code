#include "fp3d/bridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>

#include "calendar.h"

namespace fp3d
{
namespace
{
struct channel {
    std::mutex m;
    std::condition_variable cv;
    std::optional<ui_request> pending;
    std::optional<ui_answer> reply;
    uint64_t next_id = 1;
    bool shut_down = false;
    std::function<void()> before_ask;
    clock_state clock;
    // Wall time the current game turn started; real time ends it turn_length() later.
    std::chrono::steady_clock::time_point turn_start = std::chrono::steady_clock::now();
};

channel &chan()
{
    static channel c;
    return c;
}

std::atomic<bool> bridge_active{ false };
std::atomic<bool> interrupt_requested{ false };
thread_local bool next_timed = false;
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
        case request_kind::target:
            return "target";
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

ui_answer ask( ui_request req )
{
    channel &c = chan();
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lk( c.m );
        if( c.shut_down ) {
            throw shutdown_signal();
        }
        hook = c.before_ask;
    }
    if( hook ) {
        hook();
    }
    const bool timed = std::exchange( next_timed, false );
    req.timed = timed;
    req.turn = to_turns<int64_t>( calendar::turn - calendar::turn_zero );
    if( req.candidates.size() > max_candidates ) {
        req.candidates.resize( max_candidates );
    }
    std::unique_lock<std::mutex> lk( c.m );
    req.id = c.next_id++;
    req.paused = c.clock.paused;
    req.pause_reason = c.clock.pause_reason;
    const uint64_t id = req.id;
    c.reply.reset();
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
    a.text = sanitize_text( a.text, max_answer_text );
    a.action = sanitize_text( a.action, 128 );
    if( a.picks.size() > max_answer_picks ) {
        a.picks.resize( max_answer_picks );
    }
    if( a.pos && !( coord_ok( a.pos->x ) && coord_ok( a.pos->y ) && coord_ok( a.pos->z ) ) ) {
        a.pos.reset();
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
    c.cv.notify_all();
}

void pace_turn( bool fast_forward )
{
    channel &c = chan();
    std::unique_lock<std::mutex> lk( c.m );
    std::optional<std::chrono::steady_clock::time_point> paced_until;
    for( ;; ) {
        if( c.shut_down ) {
            throw shutdown_signal();
        }
        if( c.clock.paused ) {
            c.cv.wait( lk );
            continue;
        }
        if( c.clock.mode == clock_mode::turn_based || fast_forward ) {
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

} // namespace fp3d
