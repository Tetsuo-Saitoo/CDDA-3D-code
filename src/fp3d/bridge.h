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
    shot,         // one projectile of a client-flown trigger pull: CDDA's rolled error; answered with `shot` (shot.h)
    swing,        // an aimed melee strike (origin: attacker, cursor: the creature struck); answered with `shot` (swing.h)
    number,       // type a whole number from number_min to number_max (query_int, edit); answered with `number`
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
    int selected = -1;                   // menu: index into entries of CDDA's highlighted entry, -1 = none
    std::vector<std::string> actions;    // key, popup, target
    bool allow_cancel = true;
    int max_len = 0;                     // string
    int64_t number_min = 0;              // number: the range (never beyond +-max_number_answer) and the default
    int64_t number_max = 0;
    int64_t number_default = 0;
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
    float shot_error = 0.0f;             // shot: the error CDDA rolled for this projectile, arcmin
    float shot_angle = 0.0f;             // shot: which way off the sight line, degrees (0 up, 90 the shooter's right)
    std::vector<float> pellets;          // shot: a shell's pellets, each one's turn off that line, arcmin (+ = right)
    float dispersion = 0.0f;             // target (fire): max error after aiming in the current mode; shot: max, arcmin
    float dispersion_typical = 0.0f;     // target (fire): the average error, arcmin (0 = not sent)
    bool weapon_wielded = false;         // target (fire): the gun aimed is the avatar's wielded item
};

// Pellets a shot request names and a shot answer may carry, at most.
constexpr size_t max_shot_pellets = 32;

// Where one pellet of a shell went (shot answers to a request with `pellets`).
struct pellet_answer {
    ui_point end;                        // absolute square where the pellet stopped
    uint64_t hit_uid = 0;                // creature hit, 0 = miss
    std::string part;                    // body part / weak point id, "" = CDDA picks
    float quality = 0.3f;                // 0 dead centre .. <1 grazing
};

// A shot the client flew itself (target requests with FIRE, and shot requests): see fp3d/shot.h.
struct shot_answer {
    ui_point end;                        // absolute square where the bullet stopped
    uint64_t hit_uid = 0;                // creature hit, 0 = miss
    std::string part;                    // body part / weak point id, "" = CDDA picks
    float quality = 0.3f;                // 0 dead centre .. <1 grazing
    bool fly = false;                    // target: send a `shot` request for each projectile of this trigger pull
    // shot: one per pellet of the request, in its order (nullopt: that pellet takes CDDA's own path); none = the
    // whole shell spreads around this answer's outcome
    std::vector<std::optional<pellet_answer>> pellets;
};

struct ui_answer {
    uint64_t id = 0;
    bool cancel = false;
    std::string action;                  // key, popup, target
    int entry = -1;                      // menu, item_select
    std::string text;                    // string
    std::optional<int64_t> number;       // number (any other kind: dropped); the asker checks the range
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
// A number request's range never goes beyond this (2^53 - 1: exact in a float64, as the client's numbers are); an
// answer outside the range is asked again with a status, this many times at most, then taken as cancel.
constexpr int64_t max_number_answer = 9007199254740991;
constexpr int max_number_asks = 8;
constexpr size_t max_candidates = 256;

bool active();
// Action allow-list. The main loop (context "DEFAULTMODE") takes from the client only the actions a client is meant
// to send: movement, the plain game actions the client uses, FP3D's own (`fp3d_...`), and `debug` while `cheats`
// (cheats.h cheats_allowed). Every other context takes what it registered.
bool action_allowed( const std::string &context, const std::string &action, bool cheats );
void set_active( bool on );

// Sim side. Blocks until the client answers request `req` (its id is assigned here).
// Throws shutdown_signal after shutdown().
ui_answer ask( ui_request req );
// Sim side. Called on the sim thread right before every request is posted, so the client can copy game
// state (snapshot, block B3) while the sim is parked and consistent. Not for a request asked from inside this
// hook or the running hook.
void set_before_ask_hook( std::function<void()> hook );
// Sim side. Makes the next request timed (the main game loop's): in real time it expires at the end of
// the current turn and comes back with `timed_out` set.
void set_next_timed();
// Whether the next ask() is timed. Inside the before-ask hook it's already taken: use in_timed_hook() there.
bool next_is_timed();
// Sim side. True while the before-ask hook runs for a main-loop (timed) request: between turns, nothing half done.
// A request asked from inside the hook (a popup) is never the timed one.
bool in_timed_hook();
// Sim side. While the game runs on by itself (an activity, sleep, a watched fill: turns pass with no request, so
// with no before-ask hook), `hook` is called at a turn's end once neither hook has run for running_hook_gap_ms of
// wall time, so the client still gets a snapshot now and then. Other turns cost one clock read.
constexpr int running_hook_gap_ms = 500;
void set_running_hook( std::function<void()> hook );

// How many times longer this build runs: ThreadSanitizer slows the sim 5 to 15 times (B5's TSan run of the fork
// tests), so wait_request and the tests' wall-clock limits stretch by this much there; 1 in every other build.
#if defined( __SANITIZE_THREAD__ )
constexpr int slow_build = 15;
#elif defined( __has_feature )
#if __has_feature( thread_sanitizer )
constexpr int slow_build = 15;
#else
constexpr int slow_build = 1;
#endif
#else
constexpr int slow_build = 1;
#endif

// Client side. Returns the pending request with an id above `newer_than`, waiting up to timeout_ms (times
// slow_build); nullopt on timeout or shutdown.
std::optional<ui_request> wait_request( int timeout_ms, uint64_t newer_than = 0 );
// Client side. Answers the pending request; ignored if `a.id` doesn't match it.
void answer( ui_answer a );
// Either side. Wakes everyone; the sim thread unwinds with shutdown_signal at its next ask().
void shutdown();
bool is_shut_down();
// Between two games of one process (no game is booting or running), client side: the channel as a fresh process
// has it (no request, answer, queued action, query or save wanted; the default clock; not shut down), except that
// request and query ids keep counting, so nothing from the last game is taken for something in the next.
void reset_channel();
// Game thread, between two games: its own per-request flags back to a fresh thread's.
void reset_sim_thread_state();

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
// Like queue_action, for an action that asks for a square next to the player (open, close, examine...: CDDA's
// choose_adjacent_highlight): it takes exactly `square` (absolute), with no prompt, and never another square CDDA
// would pick by itself (its AUTOSELECT_SINGLE_VALID_TARGET). A square CDDA doesn't allow for it there (not next to
// the player, locked, nothing to open) does nothing, with CDDA's own message for none ("There is nothing that can be
// opened nearby."). An action that asks for no square ignores it; it never carries over to a later action. False as
// for queue_action, or for a square out of range.
bool queue_action_at( const std::string &action, const ui_point &square );
// Sim side: the square the main-loop action now running was queued with (queue_action_at), once.
std::optional<ui_point> take_action_square();
void clear_actions();
size_t queued_actions();

// Item hands (L8/E6): one CDDA action on one given item, with no menu. Items are named by the handles snapshots
// list (fp3d/item_hands.h): a uid plus the itype index from the same snapshot; the sim finds them again before
// acting and refuses with a message when one is gone, out of reach or doesn't fit. Item actions go into the action
// queue above (same order, same rules); the sim runs them as the main-loop action `fp3d_item`.
// unload: empty a gun or magazine (CDDA's unload): a gun's magazine comes out whole, a magazine's rounds come out.
// reload: load a carried gun or magazine with the ammo or magazine `container` names (CDDA's reload with that ammo
// chosen, as much as fits); a gun holding a magazine or a gunmod is loaded where CDDA's reload menu would load it.
// clear: clear the jam of the wielded gun `item` itself (fp3d/gun_faults.h clear_jam: CDDA's mend with the jam's
// first fix the avatar can do, its time); pos, hand and container aren't used.
// throw_ ("throw"): throw the carried item `item` at the square `pos` (CDDA's throw without its target menu: one
// unit of a stack counted by charges, CDDA's range, moves and dispersion). The square is one the avatar sees, on its
// level, within its throw range, not its own. Worn items aren't thrown; one held flies from its hand, anything else
// carried from an empty main hand or a free off hand. hand, container and pocket aren't used.
// pour: pour the liquid out of the container `item`, carried or lying on the avatar's square or one next to it (a pot
// on its burner), or the carried liquid `item` out of its container, with CDDA's liquid rules, to exactly one place: onto the square `pos` (the avatar's or one next to it; all of it)
// or into the container `container` (carried or on the avatar's square or one next to it, found by handle there;
// `pocket` as for put; as much as fits, the rest stays). With `container` set, `pos` stays at (0, 0, 0) (not sent);
// without it, `pocket` stays -1. hand isn't used.
// place: the client's physics moved the lying item `item` from the square `pos` onto the square `to` next to it
// (it rolled or was knocked there): no time spent, CDDA's rules for what a square takes; the item must lie near the
// avatar (item_hands.h physics_reach) on a square it sees, and be one the avatar could lift. An item moves once a
// turn. `spot` and the pose are stored when given, forgotten otherwise.
// spot: only how the lying item `item` rests on its square `pos` changes (`spot` required, the pose with it).
// hold: the carried item `item` goes into the main hand (CDDA's wield and its cost) or the off hand (held.h), `hand`
// main or off; refused for a worn item or a hand that holds something else: nothing is ever put away to make room.
// consume: CDDA's eat, drink or first aid for the item `item` (carried, or lying where `take` could get it at `pos`),
// or for the first thing in it CDDA would consume (a drink in its bottle): CDDA's own prompts, time and messages.
// First aid (an item with CDDA's heal use) may name the body part it goes on, `part`: then CDDA's body part menu
// doesn't show, and the action is refused with a reason when that menu wouldn't let the part be picked (nothing
// there the item treats, a broken or bionic limb). Without `part` the menu comes as a request.
// cook: start CDDA's craft `recipe` (batch `batch`) from what the container `item` lying on `pos` (the avatar's
// square or one next to it) holds: the recipe must be one match_recipes lists for it that can start; the components
// come out of the container only, the tools from CDDA's crafting inventory (the heat under the pot first) plus the
// emptied container. The craft lies at the avatar's feet, linked to the container (cooking.h), and CDDA's craft
// activity runs. The container must lie on a lit heat source: the craft stops when it leaves it or the heat goes out,
// and `cook` with the same recipe makes it go on (one craft a container). What it makes goes into the container.
// light: CDDA's fire starting on the square `pos` next to the avatar (the firestarter item use: its questions, time
// and charges) with the carried item `item`, or with no item (uid 0, type 0) the quickest carried one that works now.
// extinguish: CDDA's "Extinguish fire" of a fireplace: the fire in the fire furniture on `pos` (the avatar's square or
// one next to it) is beaten out; needs something that bashes, carried or lying near. `item` stays empty (uid 0,
// type 0). A fire anywhere else isn't put out by hand.
enum class item_op : uint8_t {
    take, put, drop, unload, reload, clear, throw_, pour, place, spot, hold, consume, cook, light, extinguish,
    mod_add, mod_remove
};
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
    item_ref item;                       // the item to take, put, drop, unload, reload, clear, throw or pour from;
    // light: the firestarter or empty; extinguish: empty
    // take: the square the item lies on. drop: the square to put it on. put: the container's square, used
    // when the container isn't carried. unload: the item's square, used when it isn't carried. reload: the ammo's
    // square, used when the ammo isn't carried. throw: the square thrown at (required). pour: the square poured
    // onto, only without `container`.
    ui_point pos;
    item_hand hand = item_hand::pack;    // take, unload
    // mod_add, mod_remove: the wielded gun (required); `item` is the gun mod, carried (mod_add) or one the gun
    // has (mod_remove).
    // put: the container. reload: the ammo or magazine to load (required). pour: the container poured into, or
    // empty (uid 0, type 0) to pour onto `pos`. take: the container lying on `pos` that holds `item` (one level,
    // L8), or empty for an item lying there itself.
    item_ref container;
    int pocket = -1;                     // put, pour: index into the container's pockets; -1 = CDDA's best pocket
    bool has_spot = false;               // drop, place, spot: keep where on the square it was placed
    uint16_t spot_x = 0;                 // permille of the square, east
    uint16_t spot_y = 0;                 // permille of the square, south
    ui_point to;                         // place: the square the item comes to rest on
    // drop, place, spot, only with a spot: how the item lies there, as the client's physics left it. Stored and
    // given back in the snapshot; the sim never reads a meaning into it.
    bool has_pose = false;
    uint16_t yaw = 0;                    // whole degrees, 0 .. max_item_yaw
    bool side = false;                   // lies on its side
    // drop, place, spot, only with a spot: the furniture shelf (tier, drawer, surface) it rests on, in the client's
    // own numbering 0 .. max_item_shelf. Stored and given back in the snapshot; the sim never reads a meaning into it.
    bool has_shelf = false;
    uint8_t shelf = 0;
    // 0 = none. Else the client's own number for this action: an `item_result` event (events.h) says how it went.
    uint32_t tag = 0;
    // cook only (empty and 1 for every other op): the recipe id (plain, 1 .. max_cook_recipe_bytes) and the batch.
    std::string recipe;
    int batch = 1;
    // consume only (empty for every other op), optional: the body part first aid goes on, a body part id of
    // 1 .. max_item_part_bytes letters, digits and _. It must be one of the avatar's own main parts (the ones the
    // snapshot's player.body_parts lists), which the action checks when it runs.
    std::string part;
    // mod_add only (uid 0, type 0 for every other op), optional: the vise the gun is clamped in, an item with tool
    // quality VISE 3 lying on the avatar's square or one next to it (FP3D rule, item_hands.h vise_odds: better
    // odds). The action checks it when it runs and is refused when it isn't one or isn't there.
    item_ref vise;
    // drop only (false for every other op), optional: the item goes into the vehicle's cargo space on `pos` (the
    // snapshot's vehicles[].cargo) instead of onto the ground there, by CDDA's own fit rule. Never with a spot, a
    // pose or a shelf: an item in a cargo space has no place on a square. take needs no such word: it finds the
    // item lying on `pos` or, failing that, in the cargo space there.
    bool cargo = false;
};
constexpr size_t max_item_part_bytes = 32;
constexpr size_t max_cook_recipe_bytes = 128;
constexpr int max_cook_batch = 20;
constexpr int max_item_pocket = 255;
constexpr uint16_t max_item_yaw = 359;
constexpr uint8_t max_item_shelf = 15;
constexpr uint32_t max_item_tag = INT32_MAX;
// Client side. False if a field is out of range, a pour names no target or two (container and pos), an item is
// missing (only light may leave it out, extinguish must), a vise is named for another op than mod_add or the queue
// is full.
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

// C6: work on a vehicle next to the avatar (fp3d/vehicle_work.h): repair or remove one of its parts, started as
// CDDA's vehicle screen starts it. Everything is checked again when it runs.
struct vehicle_work_action {
    uint64_t vehicle = 0;                // the snapshot's vehicle uid, 1 .. max_item_uid
    int part = -1;                       // a row of its parts, 0 .. max_work_part_row (vehicle_work.h); -1 for 'i'
    // Which part the client means by that row, as the read gave it: rows move up when a part comes off, so a row
    // from an older read can be another part's by now. The action is refused when they don't fit the row's part.
    // For 'i': the type to install and the square to install it on.
    uint32_t vpart = 0;                  // its type, the vpart id-table index, 1 .. max_work_vpart
    int mount_x = 0;                     // its mount, each -max_work_mount .. max_work_mount
    int mount_y = 0;
    char cmd = ' ';                      // 'r' repair (replace when broken), 'o' remove, 'i' install,
                                         // 'm' mend a fault, 'f' refill
    uint32_t tag = 0;                    // the client's number for it, 0 none; answered by a work_result event
};
// Client side. False if a field is out of range, the command isn't one of the above, or the queue is full.
bool queue_vehicle_work_action( const vehicle_work_action &a );
constexpr const char *vehicle_work_action_name = "fp3d_vehicle_work";
// Sim side: the action the bridge just answered `fp3d_vehicle_work` with (once), nullopt if none.
std::optional<vehicle_work_action> take_vehicle_work_action();

// Recipe queries (L9, fp3d/cooking.h): "what can this container's contents become, and how long?" without
// blocking. The client posts a query and later takes its result (JSON text). The sim runs pending queries at the
// main loop's request (right after the before-ask hook, and whenever one is posted while it waits there, paused
// too) and once per turn in pace_turn (during activities); a query posted during another prompt waits for the
// main loop. Queries are read-only. Results wait until taken; each kind keeps its own newest ones (recipe 8,
// character 4, vehicle work 4, overmap 4: max_query_results in all), the oldest of that kind dropped beyond.
constexpr size_t max_pending_queries = 8;
constexpr size_t max_query_results = 20;
constexpr size_t max_query_result_bytes = 64 * 1024;
// Client side. The query's id (from 1 up), or 0 if refused: a malformed ref (uid 0 or above max_item_uid, type
// 0), a position out of range, max_pending_queries pending already, or after shutdown.
uint64_t post_recipe_query( const item_ref &container, const ui_point &pos );
// Character queries (G4, fp3d/snapshot.h): the character screen's data, run like recipe queries. Client side.
// The query's id, or 0 if refused: a character query pending already (one at a time), max_pending_queries pending,
// or after shutdown.
uint64_t post_character_query();
// Vehicle-work queries (C6, fp3d/vehicle_work.h): what can be done to a vehicle next to the avatar, run like
// recipe queries. Client side. `vehicle`: the snapshot's vehicle uid; `part`: a row of its parts, -1 for none.
// The query's id, or 0 if refused: a uid of 0 or above max_item_uid, a part below -1 or above
// max_work_part_row, a vehicle-work query pending already (one at a time), max_pending_queries pending, or
// after shutdown.
uint64_t post_vehicle_work_query( uint64_t vehicle, int part );
// The same with everything a vehicle-work query can ask. `install`: the install list for the square at the mount
// mount_x, mount_y (each within max_work_mount) instead of a part's commands; `vpart`: the type picked there (the
// id-table index, 0 none, up to max_work_vpart). Also refused: `install` with a part row, a mount or a type
// without `install`.
struct vehicle_work_ask {
    uint64_t vehicle = 0;
    int part = -1;
    bool install = false;
    int mount_x = 0;
    int mount_y = 0;
    uint32_t vpart = 0;
};
uint64_t post_vehicle_work_query( const vehicle_work_ask &ask );
// Overmap queries (G5, fp3d/overmap_read.h): one page of the map as the player knows it, run like recipe
// queries. Client side. `origin`: the page's north-west cell in absolute overmap terrains; `with_path`: also the
// avatar's travel path. The query's id, or 0 if refused: an origin overmap_origin_ok() refuses, an overmap
// query pending already (one at a time), max_pending_queries pending, or after shutdown.
uint64_t post_overmap_query( const ui_point &origin, bool with_path );
// Client side. The result (once), nullopt while it is pending or for an unknown or already taken id.
std::optional<std::string> take_query_result( uint64_t id );
size_t pending_queries();

// Client side. Asks the sim to stop the current activity or auto-travel (it confirms with a popup first, like
// CDDA's key during one). False, and nothing happens, while the main loop waits for input with nothing queued (or
// paused): nothing runs then. One not taken by the time the main loop next waits for input is dropped, so it never
// stops something started later.
bool interrupt();
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
// Bounded polling. An input context polled with a timeout never waits for the client, so a screen that only polls
// would run the sim thread without end. Each such poll is counted here; the count starts again at every request
// and whenever the game turn changes. At every max_idle_polls polls the poll is answered `close` (the screen is
// told QUIT); at 4 x max_idle_polls it is answered `ask` (a blocking request, as an input context without a
// timeout gets) and the count starts again. `turn`: the game turn now.
enum class poll_verdict : int { go_on, close, ask };
constexpr int max_idle_polls = 256;
poll_verdict idle_poll( int64_t turn );

// Strips control characters and caps the length; for any text coming from the client.
std::string sanitize_text( const std::string &s, size_t max_len );

// Boot progress, for the console, debug.log, the client and the bridge's stall check (either thread). The game
// thread names its boot phases and CDDA's loading screen its steps (note_boot_step); each new phase is printed
// with the time since begin_boot and how long the one before took. CDDA's debug messages are never a prompt in an
// FP3D game (nobody could answer it during a boot): CDDA logs them, note_debug_error keeps the last one and
// prints the first max_printed_errors. Texts are cleaned and cut to max_boot_text bytes (whole UTF-8 characters).
constexpr size_t max_boot_text = 300;
constexpr uint64_t max_printed_errors = 50;
void begin_boot();
void note_boot_step( const std::string &phase, const std::string &step = "" );
void note_debug_error( const std::string &text );
struct boot_progress {
    std::string phase;           // "<phase>" or "<phase>: <step>", "" before the first
    double seconds = 0.0;        // since begin_boot
    double phase_seconds = 0.0;  // in this phase
    uint64_t steps = 0;          // bumps on every note_boot_step
    std::string last_error;      // the last debug message, "" none
    uint64_t errors = 0;         // debug messages since begin_boot
};
boot_progress latest_boot_progress();

} // namespace fp3d

#endif // CATA_SRC_FP3D_BRIDGE_H
