#pragma once
#ifndef CATA_SRC_FP3D_BRIDGE_H
#define CATA_SRC_FP3D_BRIDGE_H

// FP3D block B2: the channel between the sim thread (CDDA) and a client (the 3D frontend or a test driver).
// When active, CDDA's blocking input points (input_context, uilist, popups, string input, item pickers) post a
// ui_request here and wait for the client's ui_answer instead of reading the keyboard, and drawing is skipped.
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
};
const char *kind_name( request_kind k );

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
    std::vector<std::string> actions;    // key, popup
    bool allow_cancel = true;
    int max_len = 0;                     // string
};

struct ui_answer {
    uint64_t id = 0;
    bool cancel = false;
    std::string action;                  // key, popup
    int entry = -1;                      // menu, item_select
    std::string text;                    // string
    std::vector<std::pair<int, int>> picks;  // item_multi: (entry id, count; 0 = all)
};

// Thrown on the sim thread by ask() when the client shuts the bridge down. Deliberately not a
// std::exception, so CDDA's own catch( std::exception & ) handlers let it unwind to the runner.
struct shutdown_signal {};

// Hard caps on client-supplied data (answers are untrusted).
constexpr size_t max_answer_text = 1024;
constexpr size_t max_answer_picks = 4096;

bool active();
void set_active( bool on );

// Sim side. Blocks until the client answers request `req` (its id is assigned here).
// Throws shutdown_signal after shutdown().
ui_answer ask( ui_request req );
// Sim side. Called on the sim thread right before every request is posted, so the client can copy game
// state (snapshot, block B3) while the sim is parked and consistent.
void set_before_ask_hook( std::function<void()> hook );

// Client side. Returns the pending request, waiting up to timeout_ms; nullopt on timeout or shutdown.
std::optional<ui_request> wait_request( int timeout_ms );
// Client side. Answers the pending request; ignored if `a.id` doesn't match it.
void answer( ui_answer a );
// Either side. Wakes everyone; the sim thread unwinds with shutdown_signal at its next ask().
void shutdown();
bool is_shut_down();

// Sim side. Text for the next request that has none (e.g. "Open where?" for a direction prompt).
void set_next_prompt( const std::string &text );

// Diagnostics: input polls answered with a timeout (they never reach the client), counted per place.
void note_poll( const std::string &where );
std::string poll_report();

// Strips control characters and caps the length; for any text coming from the client.
std::string sanitize_text( const std::string &s, size_t max_len );

} // namespace fp3d

#endif // CATA_SRC_FP3D_BRIDGE_H
