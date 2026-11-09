#pragma once
#ifndef CATA_SRC_FP3D_BRIDGE_H
#define CATA_SRC_FP3D_BRIDGE_H

// FP3D block B2: the channel between the sim thread (CDDA) and a client (the 3D frontend or a test driver).
// When active, CDDA's blocking input points (input_context, uilist, popups, string input, item pickers,
// direction and target prompts) post a ui_request here and wait for the client's ui_answer instead of reading
// the keyboard, and drawing is skipped. The bridge also owns the real-time clock (block D3).
// See contracts/ui_requests.md.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fp3d
{

enum class request_kind : int {
    key,          // pick one of `actions` (an input_context with no richer hook)
    menu,         // pick one enabled entry (uilist)
    popup,        // pick one of `actions` (query_popup, query_yn)
    string,       // type a string (string_input_popup)
    item_select,  // pick one entry (inventory_pick_selector)
    item_multi,   // pick entries with counts (pickup / drop selectors)
    direction,    // pick an adjacent square (choose_direction / choose_adjacent)
    target,       // pick a square, plus an action when aiming (look_around, target_ui)
    chargen,      // create a character: `text` is the catalog JSON, the answer's `text` the choices JSON (chargen.h)
};
const char *kind_name( request_kind k );

// An absolute map square (tripoint_abs_ms), or a relative vector for direction answers.
struct ui_point {
    int x = 0;
    int y = 0;
    int z = 0;
};

struct ui_entry {
    int id = 0;
    std::string label;
    bool enabled = true;
    std::string description;
};

struct ui_request {
    uint64_t id = 0;
    request_kind kind = request_kind::key;
    std::string context;                 // input category, e.g. DEFAULTMODE, YESNO
    std::string title;
    std::string text;                    // CDDA colour-tagged text
    std::vector<ui_entry> entries;       // menu, item_select, item_multi
    std::vector<std::string> actions;    // key, popup, target
    bool allow_cancel = true;
    int max_len = 0;                     // string
    std::string mode;                    // target: look, zone, peek, fire, throw, reach, spell, ...
    std::string status;                  // target: "" = ok, bad_target, out_of_ammo, out_of_range
    ui_point origin;                     // direction, target: where the prompt is centred
    ui_point cursor;                     // target: suggested square
    int range = 0;                       // target: max distance in squares (0 = no limit)
    bool allow_vertical = false;         // direction
    std::vector<ui_point> candidates;    // target: visible creatures (hostiles when aiming)
    bool timed = false;                  // real time: unanswered by the end of the turn, the sim waits a turn
    int64_t turn = 0;                    // game turns since the cataclysm
    bool paused = false;                 // clock paused (pause_reason says why if the sim paused it)
    std::string pause_reason;
};

struct ui_answer {
    uint64_t id = 0;
    bool cancel = false;
    std::string action;                  // key, popup, target
    int entry = -1;                      // menu, item_select
    std::string text;                    // string
    std::vector<std::pair<int, int>> picks;  // item_multi: (entry id, count; 0 = all)
    std::optional<ui_point> pos;         // direction: relative (-1..1); target: absolute square
    bool timed_out = false;              // set by the bridge only: a timed request expired
    bool queued = false;                 // set by the bridge only: taken from the action queue
};

// Thrown on the sim thread by ask() when the client shuts the bridge down. Deliberately not a
// std::exception, so CDDA's own catch( std::exception & ) handlers let it unwind to the runner.
struct shutdown_signal {};

// Hard caps on client-supplied data (answers are untrusted) and on lists the sim sends.
constexpr size_t max_answer_text = 1024;
// The chargen answer is JSON (traits, skills, name ...): a larger cap for that kind only.
constexpr size_t max_answer_json = 16384;
constexpr size_t max_answer_picks = 4096;
constexpr size_t max_candidates = 256;

bool active();
void set_active( bool on );

// Sim side. Blocks until the client answers request `req` (its id is assigned here).
// Throws shutdown_signal after shutdown().
ui_answer ask( ui_request req );
// Sim side. Called on the sim thread right before every request is posted, so the client can copy game
// state (snapshot, block B3) while the sim is parked and consistent.
void set_before_ask_hook( std::function<void()> hook );
// Sim side. Makes the next request timed (the main game loop's): in real time it expires at the end of
// the current turn and comes back with `timed_out` set.
void set_next_timed();
bool next_is_timed();

// Client side. Returns the pending request with an id above `newer_than`, waiting up to timeout_ms;
// nullopt on timeout or shutdown.
std::optional<ui_request> wait_request( int timeout_ms, uint64_t newer_than = 0 );
// Client side. Answers the pending request; ignored if `a.id` doesn't match it.
void answer( ui_answer a );
// Either side. Wakes everyone; the sim thread unwinds with shutdown_signal at its next ask().
void shutdown();
bool is_shut_down();

// Real-time clock (block D3). Turn-based (the default) waits for the player like CDDA. Real time gives each
// turn 1 s / timescale of wall time: the sim waits out the rest of the turn, and a player who hasn't acted
// by then waits a turn. Long activities and sleep fast-forward. Pause stops turns in both modes.
enum class clock_mode : int { turn_based, real_time };
struct clock_state {
    clock_mode mode = clock_mode::turn_based;
    double timescale = 1.0;
    bool paused = false;
    std::string pause_reason;
};
constexpr double min_timescale = 0.25;
constexpr double max_timescale = 16.0;
clock_state get_clock();
void set_clock_mode( clock_mode m );
void set_timescale( double scale );      // clamped to min..max; NaN means 1
void set_paused( bool paused );          // client side; clears the reason
// Sim side. Pauses with a reason (e.g. a monster came into view); only in real time.
void auto_pause( const std::string &reason );
// Sim side, at every turn advance: blocks while paused and paces real time. Throws shutdown_signal.
void pace_turn( bool fast_forward );

// Action queue (block B5). The client can send main-loop actions (names from a timed request's `actions`) at
// any time instead of catching each timed request: the sim takes one per timed request, before waiting for an
// answer, and none while paused. An action that isn't valid there is dropped with a `message` event. Auto-pause
// clears the queue, like CDDA's safe mode stopping you. Client side; queue_action returns false when full.
constexpr size_t max_queued_actions = 32;
bool queue_action( const std::string &action );
void clear_actions();
size_t queued_actions();

// Client side. Asks the sim to stop the current activity (it confirms with a popup first, like CDDA).
void interrupt();
// Sim side. True once per interrupt().
bool take_interrupt();

// Diagnostics: input polls answered with a timeout (they never reach the client), counted per place.
void note_poll( const std::string &where );
std::string poll_report();

// Strips control characters and caps the length; for any text coming from the client.
std::string sanitize_text( const std::string &s, size_t max_len );

} // namespace fp3d

#endif // CATA_SRC_FP3D_BRIDGE_H
