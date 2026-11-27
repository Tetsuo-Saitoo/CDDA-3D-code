#pragma once
#ifndef CATA_SRC_FP3D_OVERMAP_READ_H
#define CATA_SRC_FP3D_OVERMAP_READ_H

// G5, the map: one page of the overmap as the player knows it, for a client that draws its own map screen
// (contracts/overmap.md). A page is overmap_page x overmap_page cells (overmap terrains) on one level. A cell is
// told by its look, which is what CDDA's own map screen draws for it at the vision level the player has of it:
// its name, symbol and colour there. The terrain's id is not told, so two cells CDDA draws alike can't be told
// apart.
//
// The read is a query (fp3d/bridge.h, post_overmap_query): run on the sim thread at the main loop's request,
// read-only, never a prompt. It looks only at overmaps that exist already: ground nobody has been near has no
// overmap yet and reads as unseen, and reading it creates none.
//
// The notes are commands (bridge.h, queue_map_command): a note added, changed or deleted on a cell, run in the
// main loop and answered by a map_result event. They are refused on ground with no overmap yet, so they create
// none either.

#include <cstddef>
#include <cstdint>
#include <string>

#include "fp3d/bridge.h"

namespace fp3d
{

constexpr int overmap_page = 32;                    // cells along a page's side
constexpr int max_overmap_xy = 1 << 24;             // a page origin's x and y, either way
constexpr int max_overmap_z = 10;                   // ... and its level, either way (CDDA's OVERMAP_DEPTH)
constexpr size_t max_overmap_looks = 256;           // looks a page, the unseen one included
constexpr size_t max_overmap_notes = 256;           // notes a page
constexpr size_t max_overmap_path = 2048;           // cells of the avatar's path
constexpr size_t max_overmap_name_columns = 60;
constexpr size_t max_overmap_note_columns = 450;    // CDDA's own width for a note
constexpr int max_overmap_danger_radius = 180;      // a note's danger radius as told: an overmap's side

// A cell's flags.
constexpr int overmap_explored = 1;                 // the player's explored mark
constexpr int overmap_vehicle = 2;                  // a vehicle the player tracks
constexpr int overmap_camp = 4;                     // a camp
constexpr int overmap_mission = 8;                  // the active mission's target

// Whether a page may start at this cell: x and y multiples of overmap_page, all three in range.
bool overmap_origin_ok( int x, int y, int z );

// Sim side. The page whose north-west cell is the absolute overmap terrain (x, y) on level z, as JSON: each
// cell's look, vision level and flags, the looks, the notes on the page and, with `with_path`, the avatar's
// travel path. At most about `max_bytes`: the path's far cells go first, then notes from the end, then looks
// from the end (their cells are then drawn as look 0).
std::string run_overmap_query( uint64_t query_id, int x, int y, int z, bool with_path, size_t max_bytes );
std::string overmap_query_error_json( uint64_t query_id, const std::string &error );

constexpr int max_map_danger_radius = 20;           // CDDA's own range for a danger note (overmap_ui.cpp)
constexpr size_t max_map_note_input_bytes = 8192;   // a note's text as a command may carry it

// Whether a cell may be named: x and y within max_overmap_xy, the level within max_overmap_z.
bool overmap_cell_ok( int x, int y, int z );
// Whether a note's text may be sent: at most max_map_note_input_bytes, and whole UTF-8 (no stray or overlong
// bytes, no surrogates).
bool map_note_text_ok( const std::string &text );
// Whether a map command is well formed: 'n' or 'u', a cell overmap_cell_ok takes, text map_note_text_ok takes;
// 'n' a danger of -1, 0 or 1 and a radius in 0 .. max_map_danger_radius only with 1; 'u' no text, danger or
// radius; a tag up to INT32_MAX. Checked when it is queued and again when it runs.
bool map_command_ok( const map_command &c );
// The text a note is kept with: control characters (C0, DEL, C1) removed, then colour tags until none is left,
// at most max_overmap_note_columns wide and 4 bytes a column. CDDA's "symbol:colour;" prefix stays.
std::string map_note_text( const std::string &raw );
// Sim side: runs the command the bridge answered `fp3d_map` with (take_map_command), as CDDA's map screen would
// change the note, and answers it with a map_result event when it carried a tag.
void run_map_command();

} // namespace fp3d

#endif // CATA_SRC_FP3D_OVERMAP_READ_H
