#include "fp3d/bridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>

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
};

channel &chan()
{
    static channel c;
    return c;
}

std::atomic<bool> bridge_active{ false };
thread_local std::string next_prompt;
std::mutex poll_mutex;
std::map<std::string, uint64_t> poll_counts;
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

void set_next_prompt( const std::string &text )
{
    next_prompt = text;
}

void set_before_ask_hook( std::function<void()> hook )
{
    std::lock_guard<std::mutex> lk( chan().m );
    chan().before_ask = std::move( hook );
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
    if( req.text.empty() ) {
        req.text = std::move( next_prompt );
    }
    next_prompt.clear();
    std::unique_lock<std::mutex> lk( c.m );
    req.id = c.next_id++;
    const uint64_t id = req.id;
    c.reply.reset();
    c.pending = std::move( req );
    c.cv.notify_all();
    c.cv.wait( lk, [&] {
        return c.shut_down || ( c.reply && c.reply->id == id );
    } );
    if( c.shut_down ) {
        c.pending.reset();
        throw shutdown_signal();
    }
    ui_answer a = std::move( *c.reply );
    c.reply.reset();
    c.pending.reset();
    a.text = sanitize_text( a.text, max_answer_text );
    a.action = sanitize_text( a.action, 128 );
    if( a.picks.size() > max_answer_picks ) {
        a.picks.resize( max_answer_picks );
    }
    return a;
}

std::optional<ui_request> wait_request( int timeout_ms )
{
    channel &c = chan();
    std::unique_lock<std::mutex> lk( c.m );
    c.cv.wait_for( lk, std::chrono::milliseconds( timeout_ms ), [&] {
        return c.shut_down || ( c.pending && !c.reply );
    } );
    if( c.shut_down || !c.pending || c.reply ) {
        return std::nullopt;
    }
    return c.pending;
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
