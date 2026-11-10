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
    world_setup,  // new world settings + mods: `text` is the catalog JSON, the answer's `text` the choices (worldgen.h)
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

// A shot the client flew itself (target requests, with FIRE): see fp3d/shot.h.
struct shot_answer {
    ui_point end;                        // absolute square where the bullet stopped
    uint64_t hit_uid = 0;                // creature hit, 0 = miss
    std::string part;                    // body part / weak point id, "" = CDDA picks
    float quality = 0.3f;                // 0 dead centre .. <1 grazing
};

struct ui_answer {
    uint64_t id = 0;
    bool cancel = false;
    std::string action;                  // key, popup, target
    int entry = -1;                      // menu, item_select
    std::string text;                    // string
    std::vector<std::pair<int, int>> picks;  // item_multi: (entry id, count; 0 = all)
    std::optional<ui_point> pos;         // direction: relative (-1..1); target: absolute square
    std::optional<shot_answer> shot;     // target: where the client's own bullet went
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
// Sim side. True while the before-ask hook runs for a main-loop (timed) request: between turns, nothing half done.
// A request asked from inside the hook (a popup) is never the timed one.
bool in_timed_hook();

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
    bool auto_pause_hostiles = true;     // real time pauses when a new hostile comes into view
};
constexpr double min_timescale = 0.25;
constexpr double max_timescale = 16.0;
clock_state get_clock();
void set_clock_mode( clock_mode m );
void set_timescale( double scale );      // clamped to min..max; NaN means 1
void set_paused( bool paused );          // client side; clears the reason
// Sim side. Pauses with a reason (e.g. a monster came into view); only in real time.
void auto_pause( const std::string &reason );
// Auto-pause when a new hostile comes into view (game::mon_info_update, where CDDA's safe mode would stop you).
// Its own setting, on by default: FP3D games start with CDDA's safe mode off (boot.cpp), and real time still
// stops for a hostile unless the client turns this off. Client side.
void set_auto_pause_hostiles( bool on );
// Sim side: a new hostile came into view; pauses real time ("hostile spotted") if the setting is on.
void hostile_spotted();
// Sim side, at every turn advance: blocks while paused and paces real time. Throws shutdown_signal.
// `watched`: paced in real time in both modes, for an activity the player watches happen (a liquid fill).
void pace_turn( bool fast_forward, bool watched = false );

// Action queue (block B5). The client can send main-loop actions (names from a timed request's `actions`) at
// any time instead of catching each timed request: the sim takes one per timed request, before waiting for an
// answer, and none while paused. An action that isn't valid there is dropped with a `message` event. Auto-pause
// clears the queue, like CDDA's safe mode stopping you. Client side; queue_action returns false when full.
constexpr size_t max_queued_actions = 32;
bool queue_action( const std::string &action );
void clear_actions();
size_t queued_actions();

// Item hands (L8/E6): one CDDA action on one given item, with no menu. Items are named by the handles snapshots
// list (fp3d/item_hands.h): a uid plus the itype index from the same snapshot; the sim finds them again before
// acting and refuses with a message when one is gone, out of reach or doesn't fit. Item actions go into the action
// queue above (same order, same rules); the sim runs them as the main-loop action `fp3d_item`.
// unload: empty a gun or magazine (CDDA's unload): a gun's magazine comes out whole, a magazine's rounds come out.
// reload: load a carried gun or magazine with the ammo or magazine `container` names (CDDA's reload with that ammo
// chosen, as much as fits); a gun holding a magazine or a gunmod is loaded where CDDA's reload menu would load it.
enum class item_op : uint8_t { take, put, drop, unload, reload };
// take, unload: where the item (take) or what comes out (unload) goes: the inventory (CDDA drops what doesn't
// fit), wielded, the off hand.
enum class item_hand : uint8_t { pack, main, off };
struct item_ref {
    uint64_t uid = 0;                    // 1 .. max_item_uid
    uint32_t type = 0;                   // itype index (session table); 0 = invalid
};
constexpr uint64_t max_item_uid = ( uint64_t{ 1 } << 53 ) - 1;   // exact in a double (Godot's JSON)
constexpr uint16_t spot_scale = 1000;    // spots are permille of the square, 0 .. spot_scale - 1
struct item_action {
    item_op op = item_op::take;
    item_ref item;                       // the item to take, put, drop, unload or reload
    // take: the square the item lies on. drop: the square to put it on. put: the container's square, used
    // when the container isn't carried. unload: the item's square, used when it isn't carried. reload: the ammo's
    // square, used when the ammo isn't carried.
    ui_point pos;
    item_hand hand = item_hand::pack;    // take, unload
    item_ref container;                  // put: the container. reload: the ammo or magazine to load (required)
    int pocket = -1;                     // put: index into the container's pockets; -1 = CDDA's best pocket
    bool has_spot = false;               // drop: keep where on the square it was placed
    uint16_t spot_x = 0;                 // permille of the square, east
    uint16_t spot_y = 0;                 // permille of the square, south
};
constexpr int max_item_pocket = 255;
// Client side. False if a field is out of range or the queue is full.
bool queue_item_action( const item_action &a );
constexpr const char *item_action_name = "fp3d_item";
// Sim side: the item action the bridge just answered `fp3d_item` with (once), nullopt if none.
std::optional<item_action> take_item_action();

// D2: use the stairs at an absolute square, dir +1 up or -1 down. The player stands on that square or next to it
// on the same level (next to it, they step onto it first); CDDA's vertical_move does the rest, with its messages.
struct stairs_action {
    ui_point pos;
    int dir = 0;
};
// Client side. False if a field is out of range or the queue is full.
bool queue_stairs_action( const stairs_action &a );
constexpr const char *stairs_action_name = "fp3d_stairs";
// Sim side: the stairs action the bridge just answered `fp3d_stairs` with (once), nullopt if none.
std::optional<stairs_action> take_stairs_action();

// Recipe queries (L9, fp3d/cooking.h): "what can this container's contents become, and how long?" without
// blocking. The client posts a query and later takes its result (JSON text). The sim runs pending queries at the
// main loop's request (right after the before-ask hook, and whenever one is posted while it waits there, paused
// too) and once per turn in pace_turn (during activities); a query posted during another prompt waits for the
// main loop. Queries are read-only. Results wait until taken; the oldest is dropped beyond max_query_results.
constexpr size_t max_pending_queries = 8;
constexpr size_t max_query_results = 16;
constexpr size_t max_query_result_bytes = 64 * 1024;
// Client side. The query's id (from 1 up), or 0 if refused: a malformed ref (uid 0 or above max_item_uid, type
// 0), a position out of range, max_pending_queries pending already, or after shutdown.
uint64_t post_recipe_query( const item_ref &container, const ui_point &pos );
// Client side. The result (once), nullopt while it is pending or for an unknown or already taken id.
std::optional<std::string> take_query_result( uint64_t id );
size_t pending_queries();

// Client side. Asks the sim to stop the current activity (it confirms with a popup first, like CDDA).
void interrupt();
// Sim side. True once per interrupt().
bool take_interrupt();

// Saving (M1). Client side: asks the sim to save the game where it stands and play on (CDDA's quicksave without
// its popup). The sim saves at the main loop's next request, also while paused; during a long activity or another
// prompt it waits for that. `only_if_changed` (autosave) skips the save when nothing happened since the last one;
// a plain request overrides it. False after shutdown.
bool request_save( bool only_if_changed );
struct save_status {
    bool pending = false;      // asked for and not finished
    uint64_t done = 0;         // saves finished (skipped autosaves too, with ok = true)
    bool ok = false;           // how the last one went
};
save_status get_save_status();
// What ask() answers a timed request with while a save is wanted; input_context then runs it (run_save, boot.h)
// and asks again. Only the bridge sets `queued`, so a client can't fake it.
constexpr const char *save_action = "fp3d_save";
// Sim side. Takes the wanted save: nullopt if none, else its only_if_changed. note_saved() records the result.
std::optional<bool> take_save_request();
void note_saved( bool ok );

// Main-loop jobs: changes the client asks for that must run between turns (cheats and the test kit, cheats.h).
// The client side keeps the details and calls want_job(); ask() answers the main loop's next timed request with
// job_action (also while paused, after a wanted save) and input_context runs them, then asks again. Only the
// bridge sets `queued`, so a client can't send it itself. Client side; false after shutdown.
constexpr const char *job_action = "fp3d_job";
bool want_job();

// Diagnostics: input polls answered with a timeout (they never reach the client), counted per place.
void note_poll( const std::string &where );
std::string poll_report();

// Strips control characters and caps the length; for any text coming from the client.
std::string sanitize_text( const std::string &s, size_t max_len );

} // namespace fp3d

#endif // CATA_SRC_FP3D_BRIDGE_H
