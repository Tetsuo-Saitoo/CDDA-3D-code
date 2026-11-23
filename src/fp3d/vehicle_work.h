#pragma once
#ifndef CATA_SRC_FP3D_VEHICLE_WORK_H
#define CATA_SRC_FP3D_VEHICLE_WORK_H

// FP3D C6: working on a vehicle by hand (repair, remove, mend, refill; install later), for a client that shows
// its own view instead of CDDA's vehicle screen. CDDA's screen (veh_interact) is a curses loop a client can't
// answer, but its checks are its own member functions: this builds one off-screen, with its cursor on the
// selected part's square, and asks it. So what can be done, why not, how long and what it needs are CDDA's.
// The JSON and its limits: contracts/vehicle_work.md.
//
// The read is a query (fp3d/bridge.h, post_vehicle_work_query): run on the sim thread at the main loop's request,
// read-only, never a prompt.
//
// The action (fp3d/bridge.h, vehicle_work_action) starts the work: CDDA's own question when the vehicle is
// someone else's, the same check as the read, its question when the work would ground an aircraft, then the
// activity veh_interact::serialize_activity builds,
// given to the avatar as game::exam_vehicle does. CDDA's complete_vehicle carries it out when the time is up.

#include <cstddef>
#include <cstdint>
#include <string>

class avatar;

namespace fp3d
{

constexpr size_t max_work_parts = 512;          // part entries listed
constexpr size_t max_work_here = 32;            // rows listed for the selected part's square
constexpr size_t max_work_faults = 8;           // fault names a part
constexpr size_t max_work_lines = 64;           // requirement lines a command
constexpr size_t max_work_name_columns = 60;
constexpr size_t max_work_line_columns = 200;
constexpr int max_work_part_row = 4095;         // the highest part row a query may name

// Sim side. The vehicle-work read for the vehicle a snapshot gave `vehicle` to, as JSON: its parts, and for the
// part row `part_row` (-1: none) each command with CDDA's reason, time and requirement lines. An error object
// when there is no such vehicle in the reality bubble, no part of it is on the avatar's square or one next to it,
// or the row isn't one of its parts. At most about `max_bytes`: part entries go from the end first, then the lines.
std::string run_vehicle_work_query( uint64_t query_id, uint64_t vehicle, int part_row, size_t max_bytes );
std::string vehicle_work_query_error_json( uint64_t query_id, const std::string &error );

// Sim side, from the main loop (fp3d/held.h held_action): runs the vehicle-work action the bridge just answered
// with. True when the work was started. Refused with a message, nothing done: no such vehicle in the reality
// bubble, an appliance, no part of it within a square of the avatar, a row it doesn't have, or CDDA's own check
// says no (its sentence, or that the requirements aren't met). An action with a tag is answered by one
// `work_result` event (fp3d/events.h): done when the activity was started, not when the work is finished.
bool run_vehicle_work( avatar &u );

} // namespace fp3d

#endif // CATA_SRC_FP3D_VEHICLE_WORK_H
