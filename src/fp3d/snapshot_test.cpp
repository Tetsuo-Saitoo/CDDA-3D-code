// FP3D block B3 test: boots a town and checks snapshots against CDDA's map, read through a different path
// (the submap buffer, by absolute submap). Also checks unchanged submaps are shared, an edit bumps exactly one
// revision, a zombie keeps its uid, and a map shift keeps everything matching. Exit code 0 = all checks pass.
//
// usage: fp3d-snapshot-test   (run from the cdda/ folder)
#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "avatar.h"
#include "cached_options.h"
#include "coordinates.h"
#include "calendar.h"
#include "debug.h"
#include "item.h"
#include "flag.h"
#include "fp3d/boot.h"
#include "fp3d/points.h"
#include "fp3d/id_tables.h"
#include "fp3d/item_hands.h"
#include "fp3d/snapshot.h"
#include "game.h"
#include "map.h"
#include "map_scale_constants.h"
#include "mapbuffer.h"
#include "mapdata.h"
#include "memory_fast.h"
#include "monster.h"
#include "npc.h"
#include "overmapbuffer.h"
#include "pocket_type.h"
#include "rng.h"
#include "submap.h"
#include "trap.h"
#include "type_id.h"
#include "units.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "vpart_range.h"
#include "weather.h"
#include "weather_type.h"

namespace
{
const mtype_id mon_dog( "mon_dog" );
const mtype_id mon_zombie( "mon_zombie" );
const ter_str_id ter_t_floor( "t_floor" );
const ter_str_id ter_t_wall( "t_wall" );
const vproto_id vehicle_prototype_none( "none" );
const vpart_id vpart_frame( "frame" );
const trait_id trait_DEBUG_NIGHTVISION( "DEBUG_NIGHTVISION" );

int failures = 0;

void check( bool ok, const char *what, const std::string &detail = "" )
{
    std::printf( "  %-52s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
    failures += ok ? 0 : 1;
}

using key = std::tuple<int, int, int>;
key key_of( const fp3d::submap_view &sm )
{
    return key{ sm.pos.x, sm.pos.y, sm.pos.z };
}

std::shared_ptr<const fp3d::snapshot> timed_capture( double &ms )
{
    const auto t0 = std::chrono::steady_clock::now();
    std::shared_ptr<const fp3d::snapshot> s = fp3d::capture_snapshot();
    ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
    return s;
}

// Every tile's ter/furn against the submap buffer, which CDDA's map reads from.
void check_against_mapbuffer( const fp3d::snapshot &s, const char *what )
{
    int missing = 0;
    int mismatched = 0;
    for( const std::shared_ptr<const fp3d::submap_view> &smp : s.submaps ) {
        const fp3d::submap_view &sm = *smp;
        submap *real = MAPBUFFER.lookup_submap( tripoint_abs_sm( sm.pos.x, sm.pos.y, sm.pos.z ) );
        if( real == nullptr ) {
            missing++;
            continue;
        }
        for( int y = 0; y < fp3d::sm_size; y++ ) {
            for( int x = 0; x < fp3d::sm_size; x++ ) {
                const int i = y * fp3d::sm_size + x;
                const point_sm_ms p( x, y );
                if( sm.ter[i] != static_cast<uint32_t>( real->get_ter( p ).to_i() ) ||
                    sm.furn[i] != static_cast<uint32_t>( real->get_furn( p ).to_i() ) ) {
                    mismatched++;
                }
            }
        }
    }
    const size_t expected = static_cast<size_t>( MAPSIZE ) * MAPSIZE * OVERMAP_LAYERS;
    check( s.submaps.size() == expected && missing == 0 && mismatched == 0, what,
           std::to_string( s.submaps.size() ) + " submaps, " + std::to_string( missing ) + " missing, " +
           std::to_string( mismatched ) + " tiles differ" );
}

// Every tile's terrain, furniture, light, trap and floor / sign flags against the map's own calls. The capture
// reads each submap directly and repeats what those calls do, so this is what says it still agrees with them
// (after a merge from upstream too). Counts how many tiles had each, so an empty scene can't pass.
void check_against_map_calls( const fp3d::snapshot &s, const char *what, int min_traps )
{
    map &here = get_map();
    const avatar &u = get_avatar();
    int differ = 0;
    int traps = 0;
    int holes = 0;
    int floors = 0;
    int signs = 0;
    int lit = 0;
    for( const std::shared_ptr<const fp3d::submap_view> &smp : s.submaps ) {
        const fp3d::submap_view &sm = *smp;
        const tripoint_bub_ms corner = here.get_bub( project_to<coords::ms>(
                                           tripoint_abs_sm( sm.pos.x, sm.pos.y, sm.pos.z ) ) );
        for( int y = 0; y < fp3d::sm_size; y++ ) {
            for( int x = 0; x < fp3d::sm_size; x++ ) {
                const int i = y * fp3d::sm_size + x;
                const tripoint_bub_ms p = corner + tripoint_rel_ms( x, y, 0 );
                const uint8_t light = static_cast<uint8_t>( std::clamp( std::lround( here.ambient_light_at( p ) ),
                                      0L, 255L ) );
                const trap &tr = here.tr_at( p );
                const uint32_t trap_shown = !tr.is_null() && ( u.knows_trap( p ) ||
                                            ( sm.vis[i] == fp3d::vis_seen_lit && here.can_see_trap_at( p, u ) ) ) ?
                                            static_cast<uint32_t>( tr.loadid.to_i() ) : 0;
                const bool hole = !here.has_floor( p );
                const bool sign = here.has_graffiti_at( p );
                if( sm.ter[i] != static_cast<uint32_t>( here.ter( p ).to_i() ) ||
                    sm.furn[i] != static_cast<uint32_t>( here.furn( p ).to_i() ) || sm.light[i] != light ||
                    sm.trap[i] != trap_shown || ( ( sm.flags[i] & fp3d::tile_open_air_below ) != 0 ) != hole ||
                    ( ( sm.flags[i] & fp3d::tile_has_sign ) != 0 ) != sign ) {
                    differ++;
                }
                traps += sm.trap[i] != 0 ? 1 : 0;
                holes += hole ? 1 : 0;
                floors += hole ? 0 : 1;
                signs += sign ? 1 : 0;
                lit += light > 0 ? 1 : 0;
            }
        }
    }
    check( differ == 0 && traps >= min_traps && holes > 0 && floors > 0 && signs >= 1 && lit > 0, what,
           std::to_string( differ ) + " tiles differ; " + std::to_string( traps ) + " traps shown, " +
           std::to_string( holes ) + " without a floor, " + std::to_string( signs ) + " signs, " +
           std::to_string( lit ) + " lit" );
}

// Caches as fresh as after a game turn: the test edits the map between turns, and CDDA's outside cache
// (the roof flag) is only rebuilt when marked dirty. Vehicles also need the refresh map::actualize()
// gives them (their "fake" diagonal parts and roofed insides), which a map shift triggers and the
// test's first load doesn't.
void refresh_caches()
{
    map &here = get_map();
    for( const wrapped_vehicle &wv : here.get_vehicles() ) {
        wv.v->refresh();
    }
    here.rebuild_vehicle_level_caches();
    for( int z = 0; z <= OVERMAP_HEIGHT; z++ ) {
        here.set_outside_cache_dirty( z );
    }
    here.build_map_cache( get_avatar().posz() );
}

// ---- write_client_snapshot_json, read the way a client would ----

// A snapshot JSON cut at its "submaps" array: the text before it, each object's text, the text from its "]" on.
struct cut_json {
    bool ok = false;
    std::string before;
    std::vector<std::string> bodies;
    std::string after;
};

cut_json cut_at_submaps( const std::string &j )
{
    cut_json out;
    const std::string tag = "\"submaps\":[";
    const size_t at = j.find( tag );
    if( at == std::string::npos ) {
        return out;
    }
    size_t i = at + tag.size();
    while( i < j.size() && j[i] != ']' ) {
        if( j[i] == ',' ) {
            i++;
            continue;
        }
        if( j[i] != '{' ) {
            return out;
        }
        const size_t start = i;
        int depth = 0;
        bool in_string = false;
        for( ; i < j.size(); i++ ) {
            const char c = j[i];
            if( in_string ) {
                if( c == '\\' ) {
                    i++;
                } else if( c == '"' ) {
                    in_string = false;
                }
            } else if( c == '"' ) {
                in_string = true;
            } else if( c == '{' || c == '[' ) {
                depth++;
            } else if( ( c == '}' || c == ']' ) && --depth == 0 ) {
                i++;
                break;
            }
        }
        out.bodies.push_back( j.substr( start, i - start ) );
    }
    out.ok = i < j.size();
    out.before = j.substr( 0, at );
    out.after = j.substr( std::min( i, j.size() ) );
    return out;
}

uint32_t fnv1a( const std::string &text )
{
    uint32_t hash = 2166136261u;
    for( const char c : text ) {
        hash = ( hash ^ static_cast<uint8_t>( c ) ) * 16777619u;
    }
    return hash;
}

using key_row = std::array<long long, 6>;         // x, y, z, revision, detail_revision, hash

// The first five numbers of a submap object's text: its position and revisions.
bool body_head( const std::string &body, key &pos, long long &revision, long long &detail )
{
    int x = 0;
    int y = 0;
    int z = 0;
    if( std::sscanf( body.c_str(), "{\"pos\":[%d,%d,%d],\"revision\":%lld,\"detail_revision\":%lld", &x, &y, &z,
                     &revision, &detail ) != 5 ) {
        return false;
    }
    pos = key{ x, y, z };
    return true;
}

// What write_client_snapshot_json wrote, taken apart.
struct client_json {
    bool ok = false;
    long long base = -1;                          // -1: no "base", a full snapshot
    std::vector<key_row> rows;
    cut_json cut;
    std::string head;                             // cut.before without "base" and "submap_keys"
};

client_json read_client_json( const std::string &j )
{
    client_json out;
    out.cut = cut_at_submaps( j );
    const std::string keys_tag = "\"submap_keys\":[";
    const size_t keys_at = out.cut.before.rfind( keys_tag );
    if( !out.cut.ok || keys_at == std::string::npos ) {
        return out;
    }
    size_t head_end = keys_at;
    const std::string base_tag = "\"base\":";
    const size_t base_at = out.cut.before.rfind( base_tag, keys_at );
    if( base_at != std::string::npos ) {
        // Only a "base" member right before the key rows counts: digits, then the comma.
        const std::string number = out.cut.before.substr( base_at + base_tag.size(),
                                   keys_at - base_at - base_tag.size() );
        if( number.size() >= 2 && number.size() <= 19 && number.back() == ',' &&
            number.find_first_not_of( "0123456789" ) == number.size() - 1 ) {
            out.base = std::atoll( number.c_str() );
            head_end = base_at;
        }
    }
    out.head = out.cut.before.substr( 0, head_end );
    const std::string list = out.cut.before.substr( keys_at + keys_tag.size() );
    std::vector<long long> numbers;
    for( size_t i = 0; i < list.size(); ) {
        const char c = list[i];
        if( c == '-' || ( c >= '0' && c <= '9' ) ) {
            char *end = nullptr;
            numbers.push_back( std::strtoll( list.c_str() + i, &end, 10 ) );
            i = static_cast<size_t>( end - list.c_str() );
        } else if( c == '[' || c == ']' || c == ',' ) {
            i++;
        } else {
            return out;
        }
    }
    if( numbers.size() % 6 != 0 ) {
        return out;
    }
    for( size_t n = 0; n < numbers.size(); n += 6 ) {
        out.rows.push_back( key_row{ numbers[n], numbers[n + 1], numbers[n + 2], numbers[n + 3], numbers[n + 4],
                                     numbers[n + 5] } );
    }
    out.ok = true;
    return out;
}

// A client that keeps the last snapshot's submap objects and puts the next one together from a delta, with the
// checks a real one makes. `why` says what didn't add up.
struct test_client {
    uint64_t seq = 0;
    std::map<key, std::pair<std::string, key_row>> held;

    bool take( const client_json &j, uint64_t new_seq, std::vector<std::string> &bodies, std::string &why ) {
        bodies.clear();
        if( !j.ok ) {
            why = "not the expected JSON";
            return false;
        }
        if( j.base >= 0 && ( seq == 0 || static_cast<uint64_t>( j.base ) != seq ) ) {
            why = "base " + std::to_string( j.base ) + " is not the held seq " + std::to_string( seq );
            return false;
        }
        std::map<key, const std::string *> sent;
        for( const std::string &body : j.cut.bodies ) {
            key pos;
            long long revision = 0;
            long long detail = 0;
            if( !body_head( body, pos, revision, detail ) || !sent.emplace( pos, &body ).second ) {
                why = "a body without a position, or a position twice";
                return false;
            }
        }
        std::map<key, std::pair<std::string, key_row>> next;
        size_t used = 0;
        for( const key_row &row : j.rows ) {
            const key pos{ static_cast<int>( row[0] ), static_cast<int>( row[1] ), static_cast<int>( row[2] ) };
            if( next.count( pos ) != 0 || row[5] < 0 || row[5] > 0xffffffffLL ) {
                why = "a key row twice, or a hash out of range";
                return false;
            }
            const auto body = sent.find( pos );
            if( body != sent.end() ) {
                key at;
                long long revision = 0;
                long long detail = 0;
                body_head( *body->second, at, revision, detail );
                if( revision != row[3] || detail != row[4] || fnv1a( *body->second ) != row[5] ) {
                    why = "a body's revisions or hash differ from its key row";
                    return false;
                }
                next.emplace( pos, std::make_pair( *body->second, row ) );
                used++;
            } else {
                const auto old = held.find( pos );
                if( j.base < 0 || old == held.end() || old->second.second != row ) {
                    why = "no body, and the held one is missing or has another key row";
                    return false;
                }
                next.emplace( pos, old->second );
            }
            bodies.push_back( next.at( pos ).first );
        }
        if( used != sent.size() ) {
            why = "a body without a key row";
            return false;
        }
        held = std::move( next );
        seq = new_seq;
        return true;
    }
};

// write_client_snapshot_json over a short walk through what changes between snapshots: whatever it leaves out,
// the client ends up with exactly the full snapshot.
void check_client_snapshots()
{
    map &here = get_map();
    avatar &u = get_avatar();
    test_client client;
    struct round_result {
        bool ok = false;                          // written, taken by the client, and equal to the full JSON
        fp3d::client_snapshot_info info;
        std::string why;
        size_t bytes = 0;
        size_t full_bytes = 0;
        std::set<key> sent;                       // positions of the bodies in the JSON
    };
    // Asks as a client holding `since`, takes the answer and compares with write_snapshot_json's text.
    const auto ask = [&]( uint64_t since ) {
        round_result r;
        std::ostringstream out;
        if( !fp3d::write_client_snapshot_json( out, 1, 1, since, r.info ) ) {
            r.why = "nothing written";
            return r;
        }
        const std::shared_ptr<const fp3d::snapshot> s = fp3d::latest_snapshot();
        const int pz = s->player.pos.z;
        std::ostringstream full_out;
        fp3d::write_snapshot_json( full_out, *s, std::max( -OVERMAP_DEPTH, pz - 1 ), std::min( OVERMAP_HEIGHT,
                                   pz + 1 ) );
        const cut_json full = cut_at_submaps( full_out.str() );
        const client_json got = read_client_json( out.str() );
        r.bytes = out.str().size();
        r.full_bytes = full_out.str().size();
        for( const std::string &body : got.cut.bodies ) {
            key pos;
            long long revision = 0;
            long long detail = 0;
            if( body_head( body, pos, revision, detail ) ) {
                r.sent.insert( pos );
            }
        }
        std::vector<std::string> bodies;
        if( s->seq != r.info.seq || static_cast<int>( got.rows.size() ) != r.info.submaps ||
            static_cast<int>( got.cut.bodies.size() ) != r.info.bodies ||
            got.base != ( r.info.base == 0 ? -1 : static_cast<long long>( r.info.base ) ) ) {
            r.why = "the counts or the base differ from what the call reported";
        } else if( !client.take( got, s->seq, bodies, r.why ) ) {
            // r.why is set
        } else if( !full.ok || bodies != full.bodies ) {
            r.why = "the submaps put together differ from the full snapshot's";
        } else if( got.head != full.before || got.cut.after != full.after ) {
            r.why = "the rest of the JSON differs from the full snapshot's";
        } else {
            r.ok = true;
        }
        return r;
    };
    const auto capture = [&]() {
        refresh_caches();
        const std::shared_ptr<const fp3d::snapshot> s = fp3d::capture_snapshot();
        fp3d::publish_snapshot( s );
        return s;
    };
    const auto told = []( const round_result & r ) {
        return ( r.why.empty() ? std::string() : r.why + "; " ) + ( r.info.base == 0 ? "full" : "delta" ) + ", " +
               std::to_string( r.info.bodies ) + " of " + std::to_string( r.info.submaps ) + " submaps, " +
               std::to_string( r.bytes / 1024 ) + " of " + std::to_string( r.full_bytes / 1024 ) + " KB";
    };
    const auto submap_of = []( int square ) {
        return square >= 0 ? square / fp3d::sm_size : -( ( fp3d::sm_size - 1 - square ) / fp3d::sm_size );
    };
    // The submaps that hold the squares on and around the player's.
    const auto around = [&]( const fp3d::snapshot & s, std::set<key> &out ) {
        for( int dy = -1; dy <= 1; dy++ ) {
            for( int dx = -1; dx <= 1; dx++ ) {
                out.insert( key{ submap_of( s.player.pos.x + dx ), submap_of( s.player.pos.y + dy ),
                                 s.player.pos.z } );
            }
        }
    };

    fp3d::publish_snapshot( nullptr );
    fp3d::client_snapshot_info none;
    std::ostringstream nothing;
    const bool refused = !fp3d::write_client_snapshot_json( nothing, 1, 1, 0, none ) && nothing.str().empty();
    check( refused, "client snapshot: nothing before the first publish" );

    // Nothing held: a full snapshot, with a key row for every submap.
    const std::shared_ptr<const fp3d::snapshot> first = capture();
    const round_result full = ask( 0 );
    check( full.ok && full.info.base == 0 && full.info.submaps > 0 && full.info.bodies == full.info.submaps,
           "client snapshot: nothing held, a full one with key rows", told( full ) );

    // The same snapshot again: a delta with only the submaps around the player.
    const round_result same = ask( first->seq );
    std::set<key> surroundings;
    around( *first, surroundings );
    check( same.ok && same.info.base == first->seq && same.sent == surroundings && same.info.submaps == full.info.submaps,
           "client snapshot: nothing changed, only the player's surroundings", told( same ) );

    // A wall changes a few submaps away: its submap comes, most others don't.
    const tripoint_bub_ms away = u.pos_bub() + tripoint_rel_ms( 3 * SEEX, 0, 0 );
    const ter_id was = here.ter( away );
    here.ter_set( away, was == ter_t_wall.id() ? ter_t_floor.id() : ter_t_wall.id() );
    const tripoint_abs_sm away_sm = project_to<coords::sm>( here.get_abs( away ) );
    const std::shared_ptr<const fp3d::snapshot> walled = capture();
    const round_result wall = ask( client.seq );
    check( wall.ok && wall.info.base == first->seq &&
           wall.sent.count( key{ away_sm.x(), away_sm.y(), away_sm.z() } ) == 1 &&
           wall.info.bodies < wall.info.submaps, "client snapshot: a changed submap comes with the delta",
           told( wall ) );

    // A step: the submaps around the player before and after it come, changed or not.
    u.setpos( here, u.pos_bub() + tripoint_rel_ms( 1, 0, 0 ) );
    const std::shared_ptr<const fp3d::snapshot> stepped = capture();
    const round_result step = ask( client.seq );
    std::set<key> both;
    around( *walled, both );
    around( *stepped, both );
    bool both_sent = true;
    for( const key &k : both ) {
        both_sent = both_sent && step.sent.count( k ) == 1;
    }
    check( step.ok && step.info.base == walled->seq && both_sent,
           "client snapshot: a step, the player's surroundings before and after", told( step ) );

    // Half a day later: whatever the light and the clock changed, the client still ends up right.
    calendar::turn += 12_hours;
    capture();
    const round_result night = ask( client.seq );
    check( night.ok && night.info.base == stepped->seq, "client snapshot: half a day later", told( night ) );

    // A seq the client can't hold: one never written here, one made up, one written too long ago.
    const std::shared_ptr<const fp3d::snapshot> unseen = capture();
    capture();
    const round_result never = ask( unseen->seq );
    const round_result made_up = ask( fp3d::latest_snapshot()->seq + 1000 );
    check( never.ok && never.info.base == 0 && made_up.ok && made_up.info.base == 0,
           "client snapshot: an unknown seq gets a full one", told( never ) );
    for( size_t i = 0; i < fp3d::served_snapshots_kept; i++ ) {
        capture();
        ask( client.seq );
    }
    const uint64_t held_seq = client.seq;
    const round_result stale = ask( first->seq );
    const round_result current = ask( held_seq );
    check( stale.ok && stale.info.base == 0 && current.ok && current.info.base == held_seq,
           "client snapshot: an old seq gets a full one, the held one a delta", told( stale ) );

    // Other levels asked for than the held snapshot was written with: a full one.
    std::ostringstream wide_out;
    fp3d::client_snapshot_info wide;
    const bool wide_written = fp3d::write_client_snapshot_json( wide_out, 2, 1, client.seq, wide );
    check( wide_written && wide.base == 0 && wide.submaps > 0 && wide.bodies == wide.submaps,
           "client snapshot: other levels than before get a full one",
           std::to_string( wide.bodies ) + " of " + std::to_string( wide.submaps ) );

    // A map shift: a full one, and deltas again after it.
    ask( 0 );
    u.setpos( here, u.pos_bub() + tripoint_rel_ms( 2 * SEEX, 0, 0 ) );
    g->update_map( u );
    const std::shared_ptr<const fp3d::snapshot> shifted = capture();
    const round_result shift = ask( client.seq );
    capture();
    const round_result after_shift = ask( client.seq );
    check( shift.ok && shift.info.base == 0 && shift.info.bodies == shift.info.submaps && after_shift.ok &&
           after_shift.info.base == shifted->seq && after_shift.info.bodies < after_shift.info.submaps,
           "client snapshot: a map shift gets a full one, then deltas again", told( shift ) + "; then " +
           told( after_shift ) );

    // The player on another level: a full one.
    const int level_before = u.posz();
    g->vertical_shift( level_before + 1 );
    const std::shared_ptr<const fp3d::snapshot> upstairs = capture();
    const round_result level = ask( client.seq );
    check( upstairs->player.pos.z == level_before + 1 && level.ok && level.info.base == 0 &&
           level.info.bodies == level.info.submaps, "client snapshot: another level gets a full one",
           told( level ) );
    g->vertical_shift( level_before );
    capture();

    // After the snapshots were cleared nothing from before is a base.
    const uint64_t before_clear = client.seq;
    fp3d::publish_snapshot( nullptr );
    capture();
    const round_result cleared = ask( before_clear );
    check( cleared.ok && cleared.info.base == 0, "client snapshot: nothing from before a clear is a base",
           told( cleared ) );
    fp3d::want_all_snapshot_levels();
}

const fp3d::creature_view *find_monster( const fp3d::snapshot &s, uint32_t type )
{
    for( const fp3d::creature_view &c : s.creatures ) {
        if( c.kind == 2 && c.type == type ) {
            return &c;
        }
    }
    return nullptr;
}
} // namespace

int main()
{
    std::setvbuf( stdout, nullptr, _IONBF, 0 );
    test_mode = true;
    setupDebug( DebugOutput::std_err );
    rng_set_engine_seed( 7 );
    // This test's own user folder (config, saves, caches): what another test binary leaves behind never
    // reaches it.
    try {
        fp3d::init_paths( "", "./fp3d_user_snapshot" );
    } catch( const std::exception &e ) {
        std::printf( "error: %s\n", e.what() );
        return EXIT_FAILURE;
    }
    try {
        fp3d::boot_world( "fp3d-snap", false );
        fp3d::load_bubble_in_town();
        map &here = get_map();
        avatar &u = get_avatar();
        u.set_mutation( trait_DEBUG_NIGHTVISION );
        u.clear_worn();
        u.wear_item( item( itype_id( "tshirt" ) ), false );
        u.wear_item( item( itype_id( "jacket_leather" ) ), false );
        u.add_effect( efftype_id( "bandaged" ), 1_hours, bodypart_id( "arm_l" ) );
        {
            // For check_against_map_calls, west of the player: a cot (a trap anyone sees), a bear trap the player
            // knows about and one they don't, a pit (the terrain's own trap, known), graffiti, and a hole in the
            // ground.
            const tripoint_bub_ms west = u.pos_bub() + tripoint_rel_ms( -3, 0, 0 );
            for( int dx = -2; dx <= 0; dx++ ) {
                for( int dy = -2; dy <= 2; dy++ ) {
                    const tripoint_bub_ms q = west + tripoint_rel_ms( dx, dy, 0 );
                    here.furn_set( q, furn_str_id::NULL_ID() );
                    here.ter_set( q, ter_str_id( "t_dirt" ) );
                    here.i_clear( q );
                }
            }
            here.trap_set( west + tripoint_rel_ms( 0, 1, 0 ), trap_str_id( "tr_cot" ).id() );
            here.trap_set( west, trap_str_id( "tr_beartrap" ).id() );
            u.add_known_trap( west, trap_str_id( "tr_beartrap" ).obj() );
            here.trap_set( west + tripoint_rel_ms( 0, -1, 0 ), trap_str_id( "tr_beartrap" ).id() );
            here.ter_set( west + tripoint_rel_ms( -1, 0, 0 ), ter_str_id( "t_pit" ) );
            u.add_known_trap( west + tripoint_rel_ms( -1, 0, 0 ), trap_str_id( "tr_pit" ).obj() );
            here.set_graffiti( west + tripoint_rel_ms( 0, 2, 0 ), "fp3d" );
            here.ter_set( west + tripoint_rel_ms( -1, 2, 0 ), ter_str_id( "t_open_air" ) );
        }
        refresh_caches();
        check( ter_str_id::NULL_ID().id().to_i() == 0 && furn_str_id::NULL_ID().id().to_i() == 0,
               "index 0 is t_null / f_null" );

        double ms = 0;
        const std::shared_ptr<const fp3d::snapshot> first = timed_capture( ms );
        std::printf( "  capture took %.0f ms, origin %d,%d\n", ms, first->bubble_origin.x, first->bubble_origin.y );
        check_against_mapbuffer( *first, "full capture matches the submap buffer" );
        check_against_map_calls( *first, "every tile as the map's own calls give it", 3 );
        {
            // The unknown bear trap stays hidden; the known one, the cot and the pit show.
            const auto trap_at = [&]( const tripoint_rel_ms & d ) {
                const tripoint_abs_ms abs = here.get_abs( u.pos_bub() + tripoint_rel_ms( -3, 0, 0 ) + d );
                const tripoint_abs_sm sm_pos = project_to<coords::sm>( abs );
                for( const std::shared_ptr<const fp3d::submap_view> &sm : first->submaps ) {
                    if( key_of( *sm ) == key{ sm_pos.x(), sm_pos.y(), sm_pos.z() } ) {
                        const point local = abs.raw().xy() - project_to<coords::ms>( sm_pos ).raw().xy();
                        return sm->trap[local.y * fp3d::sm_size + local.x];
                    }
                }
                return uint32_t{ 0 };
            };
            check( trap_at( tripoint_rel_ms( 0, 0, 0 ) ) ==
                   static_cast<uint32_t>( trap_str_id( "tr_beartrap" ).id().to_i() ) &&
                   trap_at( tripoint_rel_ms( 0, -1, 0 ) ) == 0 &&
                   trap_at( tripoint_rel_ms( 0, 1, 0 ) ) == static_cast<uint32_t>( trap_str_id( "tr_cot" ).id().to_i() ) &&
                   trap_at( tripoint_rel_ms( -1, 0, 0 ) ) == static_cast<uint32_t>( trap_str_id( "tr_pit" ).id().to_i() ),
                   "traps: known, plain to see and built into the terrain show; an unknown one doesn't" );
        }
        check( first->creatures.size() >= 1 && first->creatures[0].kind == 0 &&
               first->creatures[0].pos.x == first->player.pos.x, "player is the first creature" );
        {
            const fp3d::player_view &pv = first->player;
            const size_t n = pv.body_parts.size();
            bool covers_ok = true;
            for( const fp3d::worn_view &w : pv.worn ) {
                covers_ok = covers_ok && w.type != 0 && w.covers.size() <= n;
            }
            check( n > 0 && pv.bleed.size() == n && pv.bandaged.size() == n && pv.disinfected.size() == n &&
                   pv.bite.size() == n && pv.infected.size() == n && covers_ok &&
                   pv.worn.size() == 2 && pv.worn[0].layer <= pv.worn[1].layer &&
                   pv.worn[0].type == fp3d::intern_id( fp3d::id_kind::itype, "tshirt" ),
                   "player's worn items and wounds", std::to_string( pv.worn.size() ) + " worn" );
            int bandaged = 0;
            for( size_t i = 0; i < n; i++ ) {
                if( pv.body_parts[i] == fp3d::intern_id( fp3d::id_kind::body_part, "arm_l" ) ) {
                    bandaged = pv.bandaged[i];
                }
            }
            check( bandaged > 0, "bandage on the left arm", std::to_string( bandaged ) );
            check( !pv.wielded_gun, "no gun wielded: no wielded_gun" );
        }

        const std::shared_ptr<const fp3d::snapshot> again = timed_capture( ms );
        size_t shared = 0;
        for( size_t i = 0; i < again->submaps.size(); i++ ) {
            shared += again->submaps[i] == first->submaps[i] ? 1 : 0;
        }
        check( shared == first->submaps.size() && again->seq == first->seq + 1,
               "nothing changed: every submap shared", std::to_string( shared ) + " shared" );
        {
            // A wielded gun: its type, fouling, loaded rounds and faults.
            item gun( itype_id( "glock_19" ) );
            gun.ammo_set( gun.ammo_default(), 7 );
            gun.set_var( "dirt", 1234 );
            const std::set<fault_id> potential = gun.faults_potential();
            const std::string fault = potential.empty() ? std::string() : potential.begin()->str();
            if( !potential.empty() ) {
                gun.set_fault( *potential.begin(), true, false );
            }
            u.set_wielded_item( gun );
            double gun_ms = 0;
            const std::shared_ptr<const fp3d::snapshot> armed = timed_capture( gun_ms );
            const std::optional<fp3d::gun_view> &g = armed->player.wielded_gun;
            check( g && g->type == fp3d::intern_id( fp3d::id_kind::itype, "glock_19" ) && g->dirt == 1234 &&
                   g->rounds == 7 && !fault.empty() && g->faults == std::vector<std::string> { fault },
                   "wielded gun: type, dirt, rounds, faults", g ? std::to_string( g->rounds ) + " rounds" : "none" );
            std::ostringstream gun_json;
            fp3d::write_snapshot_json( gun_json, *armed, u.posz(), u.posz() );
            check( gun_json.str().find( "\"wielded_gun\":{\"type\":" ) != std::string::npos &&
                   gun_json.str().find( "\"dirt\":1234,\"rounds\":7,\"faults\":[\"" + fault + "\"]" ) !=
                   std::string::npos, "wielded_gun in the JSON" );
            check( g && !g->suppressed && gun_json.str().find( "\"suppressed\"" ) == std::string::npos,
                   "a plain gun isn't suppressed (no suppressed in the JSON)" );
            // A fault the gun type can't get (a save or a mod can add one) is listed too.
            u.get_wielded_item()->set_fault( fault_id( "fault_engine_starter" ), true, false );
            const std::shared_ptr<const fp3d::snapshot> odd = timed_capture( gun_ms );
            const std::optional<fp3d::gun_view> &g2 = odd->player.wielded_gun;
            const auto lists = [&]( const std::string & f ) {
                return g2 && std::find( g2->faults.begin(), g2->faults.end(), f ) != g2->faults.end();
            };
            check( g2 && g2->faults.size() == 2 && lists( fault ) && lists( "fault_engine_starter" ),
                   "wielded_gun lists every fault it has", g2 ? std::to_string( g2->faults.size() ) : "none" );
            // An integral suppressor (a built-in gunmod) counts like one screwed on.
            u.set_wielded_item( item( itype_id( "hk_mp5sd" ) ) );
            const std::shared_ptr<const fp3d::snapshot> quiet = timed_capture( gun_ms );
            const std::optional<fp3d::gun_view> &g3 = quiet->player.wielded_gun;
            std::ostringstream quiet_json;
            fp3d::write_snapshot_json( quiet_json, *quiet, u.posz(), u.posz() );
            check( g3 && g3->suppressed && quiet_json.str().find( "],\"suppressed\":true,\"mods\":[[" ) !=
                   std::string::npos, "an integral suppressor: suppressed, and in the JSON" );
            bool integral = false;
            for( const fp3d::gun_mod_view &m : g3 ? g3->mods : std::vector<fp3d::gun_mod_view>() ) {
                integral = integral || ( ( m.flags & fp3d::gun_mod_irremovable ) != 0 && m.uid != 0 );
            }
            check( integral, "gun mods: an integral mod is listed as one that can't be taken off" );
            u.remove_weapon();
        }
        {
            // Gun mods by hand: what the wielded gun has, its slots as they are now, and which carried mods go on.
            const skill_id pistol( "pistol" );
            const skill_id marksmanship( "gun" );
            const int pistol_was = static_cast<int>( u.get_skill_level( pistol ) );
            const int marksmanship_was = static_cast<int>( u.get_skill_level( marksmanship ) );
            u.set_skill_level( pistol, 5 );
            u.set_skill_level( marksmanship, 5 );
            u.set_wielded_item( item( itype_id( "glock_19" ) ) );
            item_location laser = u.i_add( item( itype_id( "rail_laser_sight" ) ) );
            item_location dot = u.i_add( item( itype_id( "red_dot_sight" ) ) );
            double mod_ms = 0;
            const auto fit_of = []( const std::optional<fp3d::gun_view> &gv, uint64_t uid ) {
                const fp3d::gun_fit_view *found = nullptr;
                for( const fp3d::gun_fit_view &f : gv ? gv->fits : std::vector<fp3d::gun_fit_view>() ) {
                    if( f.uid == uid && gv ) {
                        for( const fp3d::gun_fit_view &real : gv->fits ) {
                            if( real.uid == uid ) {
                                found = &real;
                            }
                        }
                    }
                }
                return found;
            };
            const auto slot_of = []( const std::optional<fp3d::gun_view> &gv, const std::string & where ) {
                const fp3d::gun_slot_view *found = nullptr;
                if( gv ) {
                    for( const fp3d::gun_slot_view &sl : gv->slots ) {
                        if( sl.location == where ) {
                            found = &sl;
                        }
                    }
                }
                return found;
            };
            const std::shared_ptr<const fp3d::snapshot> bare = timed_capture( mod_ms );
            const std::optional<fp3d::gun_view> &gb = bare->player.wielded_gun;
            const uint64_t laser_uid = laser ? fp3d::read_item_handle( *laser ) : 0;
            const uint64_t dot_uid = dot ? fp3d::read_item_handle( *dot ) : 0;
            const fp3d::gun_fit_view *laser_fit = fit_of( gb, laser_uid );
            const fp3d::gun_fit_view *dot_fit = fit_of( gb, dot_uid );
            check( gb && gb->mods.empty() && laser_uid != 0 && dot_uid != 0 && gb->fits.size() == 2 && laser_fit &&
                   laser_fit->ok && laser_fit->chance == 100 && laser_fit->risk == 0 && laser_fit->reason.empty() &&
                   dot_fit && !dot_fit->ok && !dot_fit->reason.empty() &&
                   dot_fit->reason.size() <= fp3d::max_gun_fit_reason_bytes,
                   "gun mods: a carried mod that fits and one that doesn't", dot_fit ? dot_fit->reason : "no fit rows" );
            check( slot_of( gb, "rail" ) && slot_of( gb, "rail" )->slots == 1 && slot_of( gb, "rail" )->free == 1 &&
                   slot_of( gb, "mechanism" ) && slot_of( gb, "mechanism" )->slots == 2 &&
                   !slot_of( gb, "bayonet lug" ), "gun mods: the gun's slots, all free" );
            // Too little skill (CDDA counts what the character knows as well as what they can do): its own words.
            const int knowledge_was = u.get_knowledge_level( pistol );
            u.set_skill_level( pistol, 0 );
            u.set_knowledge_level( pistol, 0 );
            const std::shared_ptr<const fp3d::snapshot> unskilled = timed_capture( mod_ms );
            const fp3d::gun_fit_view *clumsy = fit_of( unskilled->player.wielded_gun, laser_uid );
            check( clumsy && !clumsy->ok && clumsy->reason.rfind( "requires at least", 0 ) == 0,
                   "gun mods: too little skill, in CDDA's words", clumsy ? clumsy->reason : "no fit row" );
            u.set_skill_level( pistol, 5 );
            u.set_knowledge_level( pistol, std::max( knowledge_was, 5 ) );
            // A lug rail on the gun (CDDA's own put_in): listed with its location, the rail is taken, the lug adds a
            // slot of its own, the laser has no room now, and a mod in a gun is not a carried one.
            item lug( itype_id( "rail_bayonet_lug" ) );
            u.get_wielded_item()->put_in( lug, pocket_type::MOD );
            const std::shared_ptr<const fp3d::snapshot> fitted = timed_capture( mod_ms );
            const std::optional<fp3d::gun_view> &gf = fitted->player.wielded_gun;
            const fp3d::gun_fit_view *crowded = fit_of( gf, laser_uid );
            check( gf && gf->mods.size() == 1 && gf->mods[0].location == "rail" && gf->mods[0].uid != 0 &&
                   gf->mods[0].flags == 0 &&
                   gf->mods[0].type == fp3d::intern_id( fp3d::id_kind::itype, "rail_bayonet_lug" ) &&
                   slot_of( gf, "rail" ) && slot_of( gf, "rail" )->free == 0 && slot_of( gf, "bayonet lug" ) &&
                   slot_of( gf, "bayonet lug" )->slots == 1 && slot_of( gf, "bayonet lug" )->free == 1 &&
                   gf->fits.size() == 2 && crowded && !crowded->ok && !crowded->reason.empty(),
                   "gun mods: an installed mod, the slot it takes and the slot it adds",
                   crowded ? crowded->reason : "no fit row" );
            std::ostringstream mods_json;
            fp3d::write_snapshot_json( mods_json, *fitted, u.posz(), u.posz() );
            const std::string mj = mods_json.str();
            check( gf && !gf->mods.empty() &&
                   mj.find( "\"mods\":[[" + std::to_string( gf->mods[0].uid ) + "," +
                            std::to_string( gf->mods[0].type ) + ",\"rail\",0]]" ) != std::string::npos &&
                   mj.find( "\"slots\":[[\"barrel\",1,1]," ) != std::string::npos &&
                   mj.find( "\"fits\":[[" ) != std::string::npos, "gun mods in the JSON" );
            if( laser ) {
                laser.remove_item();
            }
            if( dot ) {
                dot.remove_item();
            }
            u.remove_weapon();
            u.set_skill_level( pistol, pistol_was );
            u.set_skill_level( marksmanship, marksmanship_was );
        }
        {
            // Effects on the player: id, display name, body part (0 = whole body), rating, time left.
            const efftype_id stunned( "stunned" );
            u.add_effect( stunned, 5_minutes );
            double eff_ms = 0;
            const std::shared_ptr<const fp3d::snapshot> dazed = timed_capture( eff_ms );
            const fp3d::effect_view *found = nullptr;
            for( const fp3d::effect_view &e : dazed->player.effects ) {
                found = e.id == "stunned" ? &e : found;
            }
            check( found && found->name == "Stunned" && found->body_part == 0 && found->rating == -1 &&
                   found->intensity == 1 && found->seconds_left > 0 && found->seconds_left <= 300,
                   "stunned effect in the player view", found ? found->name + " " +
                   std::to_string( found->seconds_left ) + " s" : "none" );
            std::ostringstream eff_json;
            fp3d::write_snapshot_json( eff_json, *dazed, u.posz(), u.posz() );
            check( eff_json.str().find( "{\"id\":\"stunned\",\"name\":\"Stunned\",\"body_part\":0,\"intensity\":1,"
                                        "\"rating\":-1,\"seconds_left\":" ) != std::string::npos,
                   "effects in the JSON" );
            u.remove_effect( stunned );
        }
        {
            // Mood: the morale screen's entries, strongest first, with the level, its band and CDDA's face.
            u.clear_morale();
            u.add_morale( morale_type( "morale_craving_caffeine" ), -30, -30 );
            u.add_morale( morale_type( "morale_music" ), 10, 10 );
            double mood_ms = 0;
            const std::shared_ptr<const fp3d::snapshot> moody = timed_capture( mood_ms );
            const fp3d::mood_view &mood = moody->player.mood;
            const int level = u.get_morale_level();
            check( level == -20 && mood.level == level && mood.band == -1 && !mood.face.empty() && mood.total == 2 &&
                   mood.entries.size() == 2 && mood.entries[0].name == "Craving caffeine" &&
                   mood.entries[0].value == -30 && mood.entries[0].percent == 100 &&
                   mood.entries[1].name == "Music" && mood.entries[1].value == 10 && mood.entries[1].percent == 100,
                   "mood: level, band, face and entries, strongest first", "level " + std::to_string( mood.level ) +
                   ", band " + std::to_string( mood.band ) + ", face " + mood.face + ", " +
                   std::to_string( mood.entries.size() ) + " of " + std::to_string( mood.total ) );
            std::ostringstream mood_json;
            fp3d::write_snapshot_json( mood_json, *moody, u.posz(), u.posz() );
            check( mood_json.str().find( "\"mood\":{\"level\":-20,\"band\":-1,\"face\":\"" ) != std::string::npos &&
                   mood_json.str().find( "\"total\":2,\"entries\":[{\"name\":\"Craving caffeine\",\"value\":-30,"
                                         "\"percent\":100},{\"name\":\"Music\",\"value\":10,\"percent\":100}]}" ) !=
                   std::string::npos, "mood in the JSON" );
            check( fp3d::mood_band( 100 ) == 3 && fp3d::mood_band( 99 ) == 2 && fp3d::mood_band( 50 ) == 2 &&
                   fp3d::mood_band( 49 ) == 1 && fp3d::mood_band( 10 ) == 1 && fp3d::mood_band( 9 ) == 0 &&
                   fp3d::mood_band( -10 ) == 0 && fp3d::mood_band( -11 ) == -1 && fp3d::mood_band( -50 ) == -1 &&
                   fp3d::mood_band( -51 ) == -2 && fp3d::mood_band( -100 ) == -2 && fp3d::mood_band( -101 ) == -3 &&
                   fp3d::mood_band( INT_MIN ) == -3 && fp3d::mood_band( INT_MAX ) == 3, "mood bands" );
            // The share is cut to a whole number, as the morale screen prints it: 100 and 225 of 325.
            u.add_morale( morale_type( "morale_honey" ), 15, 15 );
            const std::shared_ptr<const fp3d::snapshot> sweet = timed_capture( mood_ms );
            const fp3d::mood_view &shares = sweet->player.mood;
            check( shares.entries.size() == 3 && shares.entries[1].name == "Enjoyed honey" &&
                   shares.entries[1].percent == 69 && shares.entries[2].name == "Music" &&
                   shares.entries[2].percent == 30 && shares.entries[0].percent == 100,
                   "mood: shares cut like the morale screen's", shares.entries.size() == 3 ?
                   std::to_string( shares.entries[1].percent ) + " and " + std::to_string( shares.entries[2].percent ) :
                   std::to_string( shares.entries.size() ) + " entries" );
            // Text for the client: no color tags or control characters, never cut inside a character.
            check( fp3d::client_text( "<color_red>caf\xC3\xA9</color>\x01!", 40, 160 ) == "caf\xC3\xA9!" &&
                   fp3d::client_text( "ab\xE2\x94\xBB", 10, 4 ) == "ab" &&
                   fp3d::client_text( "ab\xE2\x94\xBB", 10, 5 ) == "ab\xE2\x94\xBB" &&
                   fp3d::client_text( "a\xF0\x9F\x98\x80", 10, 4 ) == "a" &&
                   fp3d::client_text( "a\xC3\xA9\xC3\xA9", 10, 4 ) == "a\xC3\xA9" &&
                   fp3d::client_text( "abcdef", 3, 160 ) == "abc" && fp3d::client_text( "", 3, 3 ).empty(),
                   "client text: plain, bounded, whole characters" );
            // More entries than fit: the strongest max_mood_entries, all counted.
            u.clear_morale();
            const char *const many[] = { "morale_food_hot", "morale_chat", "morale_ate_with_table", "morale_music",
                                         "morale_honey", "morale_game", "morale_marloss", "morale_mutagen",
                                         "morale_feeling_good", "morale_support", "morale_photos",
                                         "morale_craving_nicotine", "morale_craving_caffeine", "morale_craving_alcohol"
                                       };
            int n = 0;
            for( const char *id : many ) {
                n++;
                u.add_morale( morale_type( id ), n % 2 == 0 ? n : -n, n % 2 == 0 ? n : -n );
            }
            const std::shared_ptr<const fp3d::snapshot> crowded = timed_capture( mood_ms );
            const fp3d::mood_view &lots = crowded->player.mood;
            bool ordered = lots.entries.size() == fp3d::max_mood_entries;
            for( size_t i = 0; ordered && i < lots.entries.size(); i++ ) {
                ordered = std::abs( lots.entries[i].value ) == n - static_cast<int>( i ) && lots.entries[i].value != 0 &&
                          !lots.entries[i].name.empty() && lots.entries[i].percent <= 100;
            }
            check( ordered && lots.total == n, "mood: the strongest 12 of 14 entries, all counted",
                   std::to_string( lots.entries.size() ) + " of " + std::to_string( lots.total ) );
            u.clear_morale();
            const std::shared_ptr<const fp3d::snapshot> calm = timed_capture( mood_ms );
            check( calm->player.mood.entries.empty() && calm->player.mood.total == 0 && calm->player.mood.level == 0 &&
                   calm->player.mood.band == 0, "mood: nothing on a blank mind" );
        }
        {
            // Busy and asleep: the activity's id and verb while one runs; asleep only with the sleep effect.
            double busy_ms = 0;
            const std::shared_ptr<const fp3d::snapshot> idle = timed_capture( busy_ms );
            std::ostringstream idle_json;
            fp3d::write_snapshot_json( idle_json, *idle, u.posz(), u.posz() );
            check( !idle->player.activity && !idle->player.asleep &&
                   idle_json.str().find( "\"activity\"" ) == std::string::npos &&
                   idle_json.str().find( "\"asleep\"" ) == std::string::npos, "activity: none and awake, not in the JSON" );
            u.assign_activity( player_activity( activity_id( "ACT_WAIT" ), 100 * 60, 0 ) );
            const std::shared_ptr<const fp3d::snapshot> busy = timed_capture( busy_ms );
            check( busy->player.activity && busy->player.activity->id == "ACT_WAIT" &&
                   busy->player.activity->verb == "waiting" && !busy->player.asleep, "activity: id and verb",
                   busy->player.activity ? busy->player.activity->id + ", " + busy->player.activity->verb : "none" );
            std::ostringstream busy_json;
            fp3d::write_snapshot_json( busy_json, *busy, u.posz(), u.posz() );
            check( busy_json.str().find( "\"activity\":{\"id\":\"ACT_WAIT\",\"verb\":\"waiting\"}" ) !=
                   std::string::npos, "activity in the JSON" );
            u.cancel_activity();
            // Trying to sleep is an activity, not sleep yet.
            u.assign_activity( player_activity( activity_id( "ACT_TRY_SLEEP" ), 100 * 60, 0 ) );
            const std::shared_ptr<const fp3d::snapshot> trying = timed_capture( busy_ms );
            check( trying->player.activity && trying->player.activity->id == "ACT_TRY_SLEEP" &&
                   !trying->player.asleep, "activity: trying to sleep is not asleep",
                   trying->player.activity ? trying->player.activity->verb : "none" );
            u.cancel_activity();
            u.add_effect( efftype_id( "sleep" ), 1_hours );
            const std::shared_ptr<const fp3d::snapshot> sleeping = timed_capture( busy_ms );
            std::ostringstream sleep_json;
            fp3d::write_snapshot_json( sleep_json, *sleeping, u.posz(), u.posz() );
            check( sleeping->player.asleep && !sleeping->player.activity &&
                   sleep_json.str().find( "\"asleep\":true" ) != std::string::npos, "asleep, in the JSON too" );
            u.remove_effect( efftype_id( "sleep" ) );
        }

        // One wall 3 squares east of the player: exactly one submap's shape revision moves.
        const tripoint_bub_ms wall_at = u.pos_bub() + tripoint_rel_ms( 3, 0, 0 );
        here.ter_set( wall_at, ter_t_wall );
        refresh_caches();
        const std::shared_ptr<const fp3d::snapshot> edited = timed_capture( ms );
        int bumped = 0;
        bool wall_seen = false;
        const tripoint_abs_ms wall_abs = here.get_abs( wall_at );
        const tripoint_abs_sm wall_sm = project_to<coords::sm>( wall_abs );
        for( size_t i = 0; i < edited->submaps.size(); i++ ) {
            const fp3d::submap_view &sm = *edited->submaps[i];
            if( sm.revision != again->submaps[i]->revision ) {
                bumped++;
            }
            if( key_of( sm ) == key{ wall_sm.x(), wall_sm.y(), wall_sm.z() } ) {
                const point local = wall_abs.raw().xy() - project_to<coords::ms>( wall_sm ).raw().xy();
                wall_seen = sm.ter[local.y * fp3d::sm_size + local.x] ==
                            static_cast<uint32_t>( ter_t_wall.id().to_i() ) && sm.revision == 2;
            }
        }
        check( bumped == 1 && wall_seen, "a new wall bumps one submap's revision",
               std::to_string( bumped ) + " bumped" );
        {
            // Only the levels a host wants (want_snapshot_levels): what it gets for them is the same as from a full
            // capture, the other levels are left out, and a level that comes back goes on from its old revisions.
            const int pz = u.posz();
            const size_t per_level = static_cast<size_t>( MAPSIZE ) * MAPSIZE;
            double all_ms = 0;
            const std::shared_ptr<const fp3d::snapshot> all = timed_capture( all_ms );
            fp3d::want_snapshot_levels( 1, 1 );
            double few_ms = 0;
            const std::shared_ptr<const fp3d::snapshot> few = timed_capture( few_ms );
            std::map<key, const fp3d::submap_view *> in_all;
            for( const auto &sm : all->submaps ) {
                in_all[key_of( *sm )] = sm.get();
            }
            size_t same_view = 0;
            bool levels_ok = few->z_first == pz - 1 && few->z_last == pz + 1;
            for( const auto &sm : few->submaps ) {
                const auto it = in_all.find( key_of( *sm ) );
                same_view += it != in_all.end() && it->second == sm.get() ? 1 : 0;
                levels_ok = levels_ok && sm->pos.z >= pz - 1 && sm->pos.z <= pz + 1;
            }
            check( all->submaps.size() == per_level * OVERMAP_LAYERS && all->z_first == -OVERMAP_DEPTH &&
                   all->z_last == OVERMAP_HEIGHT && levels_ok && few->submaps.size() == 3 * per_level &&
                   same_view == few->submaps.size(), "wanted levels: 3 of 21 taken, each submap as in a full capture",
                   std::to_string( few->submaps.size() ) + " submaps, " + std::to_string( same_view ) + " the same" );
            std::printf( "  capture took %.0f ms for all levels, %.0f ms for 3\n", all_ms, few_ms );
            // What a client reads of those levels is the same text, but for the seq.
            const auto json_of = [&]( const fp3d::snapshot & s ) {
                std::ostringstream out;
                fp3d::write_snapshot_json( out, s, pz - 1, pz + 1 );
                std::string j = out.str();
                const std::string tag = "\"seq\":" + std::to_string( s.seq );
                const size_t at = j.find( tag );
                if( at != std::string::npos ) {
                    j.replace( at, tag.size(), "\"seq\":0" );
                }
                return j;
            };
            const std::string all_json = json_of( *all );
            {
                // A submap's JSON text is kept with its view and reused: the same bytes as writing every submap
                // anew, the first time (texts made) and the second (texts reused), and far less work.
                const auto timed_json = [&]( const fp3d::snapshot & s, bool reuse, double & took ) {
                    const auto t0 = std::chrono::steady_clock::now();
                    std::ostringstream out;
                    fp3d::write_snapshot_json( out, s, pz - 1, pz + 1, reuse );
                    took = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
                    return out.str();
                };
                double plain_ms = 0;
                double first_ms = 0;
                double again_ms = 0;
                const std::string plain = timed_json( *few, false, plain_ms );
                const std::string first_reuse = timed_json( *few, true, first_ms );
                const std::string again_reuse = timed_json( *few, true, again_ms );
                check( plain.size() > 100000 && plain == first_reuse && plain == again_reuse,
                       "submap text kept and reused: the same JSON, byte for byte",
                       std::to_string( plain.size() / 1024 ) + " KB" );
                std::printf( "  JSON for 3 levels: %.0f ms written anew, %.0f ms with the kept texts\n", plain_ms, again_ms );
            }
            check( all_json == json_of( *few ) && all_json.size() > 1000, "wanted levels: the same JSON for them",
                   std::to_string( all_json.size() ) + " bytes" );
            // A wall two levels up, where no capture looks now; then that level is wanted: its submap's revision
            // goes on by one from the full capture's, the others on that level keep theirs, none runs backwards.
            const tripoint_bub_ms high = u.pos_bub() + tripoint_rel_ms( 3, 0, 2 );
            const ter_id was = here.ter( high );
            here.ter_set( high, ter_t_wall );
            refresh_caches();
            const std::shared_ptr<const fp3d::snapshot> blind = timed_capture( few_ms );
            fp3d::want_snapshot_levels( 2, 0 );   // the wider of each side: 2 below, 1 above
            const std::shared_ptr<const fp3d::snapshot> lopsided = timed_capture( few_ms );
            check( lopsided->z_first == pz - 2 && lopsided->z_last == pz + 1 &&
                   lopsided->submaps.size() == 4 * per_level, "wanted levels: only ever wider, each side on its own",
                   std::to_string( lopsided->z_first ) + ".." + std::to_string( lopsided->z_last ) );
            fp3d::want_snapshot_levels( 0, 2 );
            const std::shared_ptr<const fp3d::snapshot> wider = timed_capture( few_ms );
            const tripoint_abs_sm high_sm = project_to<coords::sm>( here.get_abs( high ) );
            const key high_key{ high_sm.x(), high_sm.y(), high_sm.z() };
            int went_on = 0;
            int kept = 0;
            int backwards = 0;
            for( const auto &sm : wider->submaps ) {
                const auto it = in_all.find( key_of( *sm ) );
                if( it == in_all.end() ) {
                    continue;
                }
                backwards += sm->revision < it->second->revision ||
                             sm->detail_revision < it->second->detail_revision ? 1 : 0;
                if( sm->pos.z == pz + 2 ) {
                    if( key_of( *sm ) == high_key ) {
                        went_on += sm->revision == it->second->revision + 1 ? 1 : 0;
                    } else {
                        kept += sm->revision == it->second->revision ? 1 : 0;
                    }
                }
            }
            check( blind->submaps.size() == 3 * per_level && wider->z_first == pz - 2 && wider->z_last == pz + 2 &&
                   wider->submaps.size() == 5 * per_level && went_on == 1 &&
                   kept == static_cast<int>( per_level ) - 1 && backwards == 0,
                   "wanted levels: a level that comes back goes on from its revisions",
                   std::to_string( wider->submaps.size() ) + " submaps, " + std::to_string( went_on ) + " went on, " +
                   std::to_string( kept ) + " kept, " + std::to_string( backwards ) + " backwards" );
            here.ter_set( high, was );
            refresh_caches();
            fp3d::want_all_snapshot_levels();
            const std::shared_ptr<const fp3d::snapshot> back = timed_capture( all_ms );
            check( back->submaps.size() == per_level * OVERMAP_LAYERS && back->z_first == -OVERMAP_DEPTH &&
                   back->z_last == OVERMAP_HEIGHT, "wanted levels: all of them again" );
            // Published in the order they were captured: an earlier capture never replaces a later one.
            const std::shared_ptr<const fp3d::snapshot> later = timed_capture( all_ms );
            fp3d::publish_snapshot( later );
            fp3d::publish_snapshot( back );
            const bool kept_later = fp3d::latest_snapshot() == later;
            fp3d::publish_snapshot( nullptr );
            const bool cleared = fp3d::latest_snapshot() == nullptr;
            fp3d::publish_snapshot( back );
            check( later->seq > back->seq && kept_later && cleared && fp3d::latest_snapshot() == back,
                   "publish: never back to an earlier capture; nullptr clears" );
            fp3d::publish_snapshot( later );
        }

        // A zombie in view shows up with its type and keeps its uid. A tamed dog comes along for the attitudes.
        const tripoint_bub_ms zed_at = u.pos_bub() + tripoint_rel_ms( 0, 2, 0 );
        here.ter_set( zed_at, ter_str_id( "t_floor" ) );
        g->place_critter_at( mon_zombie, zed_at );
        const tripoint_bub_ms dog_at = u.pos_bub() + tripoint_rel_ms( 0, -2, 0 );
        here.ter_set( dog_at, ter_str_id( "t_floor" ) );
        monster *dog = g->place_critter_at( mon_dog, dog_at );
        if( dog != nullptr ) {
            dog->friendly = -1;
        }
        refresh_caches();
        const uint32_t zed_type = fp3d::intern_id( fp3d::id_kind::mtype, mon_zombie.str() );
        const std::shared_ptr<const fp3d::snapshot> with_zed = timed_capture( ms );
        const std::shared_ptr<const fp3d::snapshot> with_zed2 = timed_capture( ms );
        const fp3d::creature_view *z1 = find_monster( *with_zed, zed_type );
        const fp3d::creature_view *z2 = find_monster( *with_zed2, zed_type );
        check( z1 && z2 && z1->uid == z2->uid && z1->uid >= ( uint64_t( 2 ) << 32 ) &&
               z1->pos.x == here.get_abs( zed_at ).x(), "zombie seen, uid stable across captures" );

        // Attitude to the player: the zombie is hostile, a tamed dog friendly, the player itself neutral.
        const fp3d::creature_view *d1 = find_monster( *with_zed2, fp3d::intern_id( fp3d::id_kind::mtype,
                                        mon_dog.str() ) );
        check( z2 && z2->attitude == 0 && d1 && d1->attitude == 2 && with_zed2->creatures[0].kind == 0 &&
               with_zed2->creatures[0].attitude == 1, "attitude: zombie hostile, tame dog friendly, player neutral",
               std::to_string( z2 ? z2->attitude : -1 ) + " " + std::to_string( d1 ? d1->attitude : -1 ) );
        std::ostringstream creatures_json;
        fp3d::write_snapshot_json( creatures_json, *with_zed2, u.posz(), u.posz() );
        check( creatures_json.str().find( "\"attitude\":0" ) != std::string::npos, "creatures JSON has attitude" );

        // Walk two submaps east: the bubble shifts and the snapshot follows it.
        u.setpos( here, u.pos_bub() + tripoint_rel_ms( 2 * SEEX, 0, 0 ) );
        g->update_map( u );
        refresh_caches();
        const std::shared_ptr<const fp3d::snapshot> shifted = timed_capture( ms );
        check( shifted->bubble_origin.x == with_zed2->bubble_origin.x + 2 &&
               shifted->bubble_origin.y == with_zed2->bubble_origin.y, "bubble origin moved 2 submaps east",
               std::to_string( shifted->bubble_origin.x - with_zed2->bubble_origin.x ) );
        check_against_mapbuffer( *shifted, "after the shift, still matches" );
        // The cot is out of sight from here; the two known traps still show.
        check_against_map_calls( *shifted, "after the shift, every tile as the map's calls give it", 2 );
        std::map<key, const fp3d::submap_view *> before;
        for( const auto &sm : with_zed2->submaps ) {
            before[key_of( *sm )] = sm.get();
        }
        // Submaps in both bubbles keep their terrain, and their revision unless something else changed. (A
        // shift rebuilds CDDA's vehicle caches, which can change vehicle roof flags: a real change, so a bump.)
        int kept = 0;
        int overlap = 0;
        int same_ground = 0;
        for( const auto &sm : shifted->submaps ) {
            const auto it = before.find( key_of( *sm ) );
            if( it == before.end() ) {
                continue;
            }
            overlap++;
            kept += it->second->revision == sm->revision ? 1 : 0;
            same_ground += it->second->ter == sm->ter && it->second->furn == sm->furn ? 1 : 0;
        }
        check( overlap == 9 * MAPSIZE * OVERMAP_LAYERS && same_ground == overlap && kept + 8 >= overlap,
               "submaps in both bubbles keep ter/furn and revision",
               std::to_string( same_ground ) + "/" + std::to_string( overlap ) + " same ground, " +
               std::to_string( kept ) + " same revision" );
        {
            // Weather: what the current weather type does, and the wind and shelter at the player. Last: it puts a
            // floor under the player for a while, which bumps submap revisions the checks above count.
            weather_manager &w = get_weather();
            const tripoint_bub_ms at = u.pos_bub();
            const ter_id ground = here.ter( at );
            const auto rebuild = [&]() {
                here.invalidate_map_cache( at.z() );
                here.build_map_cache( at.z() );
            };
            double w_ms = 0;
            const auto with_weather = [&]( const char *id ) {
                w.weather_override = weather_type_id( id );
                w.set_nextweather( calendar::turn );
                w.update_weather();
                w.windspeed = 20;        // mph
                w.winddirection = 90;
                return timed_capture( w_ms );
            };
            const auto close_to = []( float a, float b ) {
                return std::fabs( a - b ) < 0.01f;
            };
            const std::shared_ptr<const fp3d::snapshot> rain = with_weather( "rain" );
            const fp3d::environment_view &r = rain->env;
            check( r.weather_id == "rain" && r.weather_name == "Rain" && r.precip == 3 && r.precip_kind == 1 &&
                   r.weather_sound == 2 && close_to( r.sight_penalty, 1.1f ) && r.light_modifier == -30 &&
                   close_to( r.sun_multiplier, 0.03f ) && r.sound_attn == 4 && !r.dangerous,
                   "weather: rain", r.weather_id + " / " + r.weather_name + ", precip " + std::to_string( r.precip ) +
                   " kind " + std::to_string( r.precip_kind ) + ", sound " + std::to_string( r.weather_sound ) );
            check( close_to( r.wind_speed, 20 * 0.44704f ) && close_to( r.wind_dir_deg, 90.0f ) && r.humidity <= 100 &&
                   r.sheltered == g->is_sheltered( at ) && r.wind_at_player >= 0.0f &&
                   r.wind_at_player <= r.wind_speed + 0.01f && ( !r.sheltered || r.wind_at_player == 0.0f ),
                   "weather: wind and humidity at the player", std::to_string( r.wind_at_player ) + " of " +
                   std::to_string( r.wind_speed ) + " m/s, humidity " + std::to_string( r.humidity ) +
                   ( r.sheltered ? ", sheltered" : ", in the open" ) );
            const std::shared_ptr<const fp3d::snapshot> snow = with_weather( "flurries" );
            check( snow->env.weather_id == "flurries" && snow->env.precip == 2 && snow->env.precip_kind == 2 &&
                   snow->env.weather_sound == 5, "weather: flurries are snow",
                   std::to_string( snow->env.precip ) + " kind " + std::to_string( snow->env.precip_kind ) );
            const std::shared_ptr<const fp3d::snapshot> fog = with_weather( "fog" );
            check( fog->env.weather_id == "fog" && fog->env.precip == 0 && fog->env.precip_kind == 0 &&
                   fog->env.weather_sound == 0 && close_to( fog->env.sight_penalty, 1.7f ) && fog->env.light_modifier == -40,
                   "weather: fog falls on nothing and hides more", std::to_string( fog->env.sight_penalty ) );
            const std::shared_ptr<const fp3d::snapshot> storm = with_weather( "portal_storm" );
            check( storm->env.dangerous && storm->env.weather_sound == 8, "weather: a portal storm is dangerous" );
            // Under a roof: sheltered, no wind. Then back in the open.
            here.ter_set( at, ter_t_floor );
            rebuild();
            const std::shared_ptr<const fp3d::snapshot> indoors = with_weather( "rain" );
            check( indoors->env.sheltered && indoors->env.wind_at_player == 0.0f && indoors->env.precip == 3,
                   "weather: sheltered under a roof, no wind there",
                   std::to_string( indoors->env.wind_at_player ) + " m/s" );
            std::ostringstream w_json;
            fp3d::write_snapshot_json( w_json, *indoors, u.posz(), u.posz() );
            const std::string wj = w_json.str();
            check( wj.find( "\"weather_id\":\"rain\",\"weather_name\":\"Rain\",\"precip\":3,\"precip_kind\":1,"
                            "\"weather_sound\":2,\"sight_penalty\":" ) != std::string::npos &&
                   wj.find( "\"light_modifier\":-30,\"sun_multiplier\":" ) != std::string::npos &&
                   wj.find( "\"sound_attn\":4,\"dangerous\":false,\"wind_at_player\":" ) != std::string::npos &&
                   wj.find( "\"sheltered\":true}" ) != std::string::npos, "weather in the JSON" );
            here.ter_set( at, ground );
            rebuild();
            w.weather_override = WEATHER_NULL;
            w.set_nextweather( calendar::turn );
            w.update_weather();
        }

        // Live item tiles: what the carried items and those lying in reach show of their state (player.item_states).
        {
            const tripoint_bub_ms beside = u.pos_bub() + tripoint_rel_ms( 1, 0, 0 );
            here.ter_set( beside, ter_t_floor );
            here.furn_set( beside, furn_str_id::NULL_ID() );
            here.i_clear( beside );
            // Carried: a plastic bottle (500 ml, see-through) with 250 ml of hot water.
            item water( itype_id( "water" ), calendar::turn, 1 );
            water.set_flag( flag_HOT );
            item bottle( itype_id( "bottle_plastic" ) );
            bottle.put_in( water, pocket_type::CONTAINER );
            item_location carried = u.i_add( bottle );
            // Lying next to the player: frozen meat well past rotten, a fouled pistol, a pistol whose fouling a save
            // spelled as text, a pot with old meat in it, a rock.
            item meat( itype_id( "meat" ) );
            meat.set_relative_rot( 1.5 );
            meat.set_flag( flag_FROZEN );
            item fouled( itype_id( "glock_19" ) );
            fouled.set_var( "dirt", 5000.0 );
            item odd( itype_id( "glock_19" ) );
            odd.set_var( "dirt", std::string( "lots" ) );
            item old_meat( itype_id( "meat" ) );
            old_meat.set_relative_rot( 0.95 );
            item pot( itype_id( "pot" ) );
            pot.put_in( old_meat, pocket_type::CONTAINER );
            for( const item &it : { meat, fouled, odd, pot, item( itype_id( "rock" ) ) } ) {
                here.add_item( beside, it );
            }
            // For the exact numbers: a stack of 15 rounds lying there too.
            here.add_item( beside, item( itype_id( "9mm" ), calendar::turn, 15 ) );
            const std::shared_ptr<const fp3d::snapshot> live = fp3d::capture_snapshot();
            const auto state_of = [&live]( const item & it ) {
                const uint64_t uid = fp3d::read_item_handle( it );
                for( const fp3d::item_state_view &st : live->player.item_states ) {
                    if( uid != 0 && st.uid == uid ) {
                        return std::optional<fp3d::item_state_view>( st );
                    }
                }
                return std::optional<fp3d::item_state_view>();
            };
            const auto text = []( const std::optional<fp3d::item_state_view> &st ) {
                return st ? "temp " + std::to_string( st->temp ) + " rot " + std::to_string( st->rot ) + " fill " +
                       std::to_string( st->fill ) + " type " + std::to_string( st->fill_type ) +
                       ( st->see_through ? " clear " : " opaque " ) + std::to_string( st->fill_ml ) + " ml dirt " +
                       std::to_string( st->dirt ) : std::string( "not listed" );
            };
            const uint32_t t_water = fp3d::intern_id( fp3d::id_kind::itype, "water" );
            const uint32_t t_meat = fp3d::intern_id( fp3d::id_kind::itype, "meat" );
            std::optional<fp3d::item_state_view> b;
            std::optional<fp3d::item_state_view> w;
            if( carried ) {
                b = state_of( *carried );
                if( !carried->all_items_top( pocket_type::CONTAINER ).empty() ) {
                    w = state_of( *carried->all_items_top( pocket_type::CONTAINER ).front() );
                }
            }
            check( b && b->temp == 0 && b->rot == -1 && b->fill == 500 && b->fill_type == t_water && b->see_through &&
                   b->fill_ml == 250 && b->dirt == -1, "a carried bottle: half full of water, see-through", text( b ) );
            check( w && w->temp == 1 && w->rot == -1 && w->fill == -1 && w->fill_ml == -1 && w->dirt == -1,
                   "the hot water in it", text( w ) );
            std::optional<fp3d::item_state_view> m;
            std::optional<fp3d::item_state_view> g1;
            std::optional<fp3d::item_state_view> g2;
            std::optional<fp3d::item_state_view> p;
            std::optional<fp3d::item_state_view> inside;
            bool rock_listed = false;
            int pistols = 0;
            for( const item &it : here.i_at( beside ) ) {
                if( it.typeId() == itype_id( "meat" ) ) {
                    m = state_of( it );
                } else if( it.typeId() == itype_id( "glock_19" ) ) {
                    ( pistols++ == 0 ? g1 : g2 ) = state_of( it );
                } else if( it.typeId() == itype_id( "pot" ) ) {
                    p = state_of( it );
                    if( !it.all_items_top( pocket_type::CONTAINER ).empty() ) {
                        inside = state_of( *it.all_items_top( pocket_type::CONTAINER ).front() );
                    }
                } else if( it.typeId() == itype_id( "rock" ) ) {
                    rock_listed = state_of( it ).has_value();
                }
            }
            check( m && m->temp == 3 && m->rot == 1500 && m->fill == -1 && m->fill_ml == -1 && m->dirt == -1,
                   "lying meat: frozen, rot 1500", text( m ) );
            check( g1 && g2 && std::max( g1->dirt, g2->dirt ) == 5000 && std::min( g1->dirt, g2->dirt ) == 0 &&
                   g1->fill == -1 && g1->rot == -1, "lying pistols: fouling 5000, and 0 for a fouling that isn't a number",
                   text( g1 ) + " | " + text( g2 ) );
            check( p && p->rot == 950 && p->fill > 0 && p->fill <= 1000 && p->fill_ml > 0 && p->fill_type == t_meat &&
                   !p->see_through && inside && inside->rot == 950,
                   "a lying pot: the rot of the meat inside, not see-through", text( p ) + " | " + text( inside ) );
            check( !rock_listed && live->player.item_states.size() <= fp3d::max_item_states,
                   "a rock shows nothing: not listed" );
            std::ostringstream st_json;
            fp3d::write_snapshot_json( st_json, *live, u.posz(), u.posz() );
            const std::string sj = st_json.str();
            const std::string meat_entry = m ? "[" + std::to_string( m->uid ) + ",3,1500,-1,0,0,-1,-1]" : "?";
            check( sj.find( "\"item_states\":[[" ) != std::string::npos && sj.find( meat_entry ) != std::string::npos,
                   "item states in the JSON", meat_entry );
            // Exact numbers (player.item_amounts): every carried item in the order of `items`, then the lying ones;
            // weight and volume as CDDA counts them.
            const auto amount_of = [&live]( const item & it ) {
                const uint64_t uid = fp3d::read_item_handle( it );
                for( const fp3d::item_amount_view &av : live->player.item_amounts ) {
                    if( uid != 0 && av.uid == uid ) {
                        return std::optional<fp3d::item_amount_view>( av );
                    }
                }
                return std::optional<fp3d::item_amount_view>();
            };
            bool carried_in_order = live->player.item_amounts.size() >= live->player.items.size();
            for( size_t i = 0; carried_in_order && i < live->player.items.size(); i++ ) {
                carried_in_order = live->player.item_amounts[i].uid == live->player.items[i].uid;
            }
            std::optional<fp3d::item_amount_view> ba;
            if( carried ) {
                ba = amount_of( *carried );
            }
            check( carried_in_order && ba && carried && ba->count == 1 &&
                   ba->weight_g == units::to_gram( carried->weight() ) &&
                   ba->volume_ml == units::to_milliliter( carried->volume() ) &&
                   ba->weight_g > units::to_gram( carried->weight( false ) ),
                   "item amounts: every carried item in order; a bottle weighs with its water",
                   ba ? std::to_string( ba->weight_g ) + " g, " + std::to_string( ba->volume_ml ) + " ml" : "not listed" );
            std::optional<fp3d::item_amount_view> rounds;
            int64_t rounds_g = 0;
            for( const item &it : here.i_at( beside ) ) {
                if( it.typeId() == itype_id( "9mm" ) ) {
                    rounds = amount_of( it );
                    rounds_g = units::to_gram( it.weight() );
                }
            }
            check( rounds && rounds->count == 15 && rounds->weight_g == rounds_g && rounds->weight_g > 0 &&
                   rounds->volume_ml > 0 && live->player.item_amounts.size() <= fp3d::max_item_amounts,
                   "item amounts: a lying stack of 15 rounds, its whole weight",
                   rounds ? std::to_string( rounds->count ) + " x, " + std::to_string( rounds->weight_g ) + " g" :
                   "not listed" );
            const std::string rounds_entry = rounds ? "[" + std::to_string( rounds->uid ) + ",15," +
                                             std::to_string( rounds->weight_g ) + "," +
                                             std::to_string( rounds->volume_ml ) + "]" : "?";
            check( sj.find( "\"item_amounts\":[[" ) != std::string::npos && sj.find( rounds_entry ) != std::string::npos,
                   "item amounts in the JSON", rounds_entry );
            here.i_clear( beside );
            if( carried ) {
                carried.remove_item();
            }
        }

        // Names from a save or a mod are untrusted: an NPC's and a vehicle's come cleaned and capped. Last: the
        // floor squares bump revisions.
        {
            const std::string hostile = "<color_red>Ann</color>\x01\x7f\n Lee" + std::string( 300, 'x' );
            const auto plain = []( const std::string & s, size_t max ) {
                return s.size() <= max && s.find( '<' ) == std::string::npos &&
                       std::none_of( s.begin(), s.end(), []( char ch ) {
                    return static_cast<unsigned char>( ch ) < 0x20 || ch == 0x7f;
                } );
            };
            const tripoint_bub_ms npc_at = u.pos_bub() + tripoint_rel_ms( -2, 0, 0 );
            here.ter_set( npc_at, ter_t_floor );
            here.furn_set( npc_at, furn_str_id::NULL_ID() );
            shared_ptr_fast<npc> guy = make_shared_fast<npc>();
            guy->normalize();
            guy->randomize();
            guy->spawn_at_precise( here.get_abs( npc_at ) );
            overmap_buffer.insert_npc( guy );
            g->load_npcs();
            guy->name = hostile;
            const tripoint_bub_ms veh_at = u.pos_bub() + tripoint_rel_ms( 0, 3, 0 );
            here.ter_set( veh_at, ter_t_floor );
            here.furn_set( veh_at, furn_str_id::NULL_ID() );
            vehicle *veh = here.add_vehicle( vehicle_prototype_none, veh_at, 0_degrees, 0, 0 );
            if( veh != nullptr ) {
                veh->install_part( here, point_rel_ms::zero, vpart_frame );
                veh->name = hostile;
                here.add_vehicle_to_cache( veh );
            }
            refresh_caches();
            const std::shared_ptr<const fp3d::snapshot> named = timed_capture( ms );
            const fp3d::creature_view *who = nullptr;
            for( const fp3d::creature_view &c : named->creatures ) {
                if( c.kind == 1 && c.uid == fp3d::creature_uid( *guy ) ) {
                    who = &c;
                }
            }
            check( who != nullptr && who->name.rfind( "Ann Lee", 0 ) == 0 && plain( who->name, 40 ),
                   "an NPC's name, cleaned and capped", who ? who->name.substr( 0, 60 ) : "NPC not seen" );
            bool others_unnamed = true;
            for( const fp3d::creature_view &c : named->creatures ) {
                others_unnamed = others_unnamed && ( c.kind == 1 || c.name.empty() );
            }
            check( others_unnamed, "no name for the player or monsters" );
            const fp3d::vehicle_view *car = nullptr;
            for( const fp3d::vehicle_view &v : named->vehicles ) {
                if( veh != nullptr && v.pos.x == here.get_abs( veh_at ).x() && v.pos.y == here.get_abs( veh_at ).y() ) {
                    car = &v;
                }
            }
            check( car != nullptr && car->name.rfind( "Ann Lee", 0 ) == 0 && plain( car->name, 60 ),
                   "a vehicle's name, cleaned and capped", car ? car->name.substr( 0, 80 ) : "vehicle not seen" );
            std::ostringstream n_json;
            fp3d::write_snapshot_json( n_json, *named, u.posz(), u.posz() );
            const std::string nj = n_json.str();
            const size_t npc_entry = nj.find( "\"kind\":1," );
            const size_t name_at = nj.find( "\"name\":\"Ann Lee", npc_entry );
            check( npc_entry != std::string::npos && name_at != std::string::npos &&
                   nj.find( '}', npc_entry ) > name_at, "the NPC's name in the JSON" );
            if( veh != nullptr ) {
                here.destroy_vehicle( veh );
            }
            guy->die( &here, nullptr );
            g->cleanup_dead();
        }
        {
            // Where a vehicle's parts are. An L of four frames facing 0, 90 and 45 degrees: each row's (dx, dy, dz)
            // is the part's square from the vehicle's position as CDDA has it, and the map finds this vehicle on
            // that square. (The client can't turn the mounts itself: CDDA steps along a ray around a pivot.)
            // A wall of three boards at 0 and 45 degrees: at an angle CDDA fills the diagonal gaps with copies of
            // the boards, which come as extra rows (part_gap_filler) and carry the vehicle flag on their tiles.
            const std::vector<point_rel_ms> ell = { point_rel_ms( 0, 0 ), point_rel_ms( 1, 0 ), point_rel_ms( 2, 0 ), point_rel_ms( 0, 1 ) };
            const std::vector<point_rel_ms> line = { point_rel_ms( 0, 0 ), point_rel_ms( 1, 0 ), point_rel_ms( 2, 0 ) };
            struct test_car {
                const char *name;
                units::angle dir;
                tripoint_rel_ms at;
                bool wall;
                vehicle *veh = nullptr;
            };
            std::array<test_car, 5> cars = { {
                    { "fp3d car 0", 0_degrees, tripoint_rel_ms( 9, 9, 0 ), false },
                    { "fp3d car 90", 90_degrees, tripoint_rel_ms( 9, -9, 0 ), false },
                    { "fp3d car 45", 45_degrees, tripoint_rel_ms( -9, 9, 0 ), false },
                    { "fp3d wall 0", 0_degrees, tripoint_rel_ms( -9, -9, 0 ), true },
                    { "fp3d wall 45", 45_degrees, tripoint_rel_ms( 18, 0, 0 ), true }
                }
            };
            for( test_car &c : cars ) {
                const tripoint_bub_ms mid = u.pos_bub() + c.at;
                for( int dx = -4; dx <= 4; dx++ ) {
                    for( int dy = -4; dy <= 4; dy++ ) {
                        const tripoint_bub_ms q = mid + tripoint_rel_ms( dx, dy, 0 );
                        here.furn_set( q, furn_str_id::NULL_ID() );
                        here.ter_set( q, ter_t_floor );
                        here.i_clear( q );
                    }
                }
                c.veh = here.add_vehicle( vehicle_prototype_none, mid, c.dir, 0, 0 );
                if( c.veh != nullptr ) {
                    for( const point_rel_ms &m : c.wall ? line : ell ) {
                        c.veh->install_part( here, m, vpart_frame );
                        if( c.wall ) {
                            c.veh->install_part( here, m, vpart_id( "board" ) );
                        }
                    }
                    c.veh->name = c.name;
                    here.add_vehicle_to_cache( c.veh );
                }
            }
            refresh_caches();
            const std::shared_ptr<const fp3d::snapshot> parked = timed_capture( ms );
            std::ostringstream parked_json;
            fp3d::write_snapshot_json( parked_json, *parked, u.posz(), u.posz() );
            // The snapshot's vehicle flag on a square of the bubble.
            const auto flagged = [&]( const tripoint_bub_ms & p ) {
                const tripoint_abs_ms abs = here.get_abs( p );
                const tripoint_abs_sm sm_pos = project_to<coords::sm>( abs );
                for( const std::shared_ptr<const fp3d::submap_view> &sm : parked->submaps ) {
                    if( key_of( *sm ) == key{ sm_pos.x(), sm_pos.y(), sm_pos.z() } ) {
                        const point local = abs.raw().xy() - project_to<coords::ms>( sm_pos ).raw().xy();
                        return ( sm->flags[local.y * fp3d::sm_size + local.x] & fp3d::tile_has_vehicle ) != 0;
                    }
                }
                return false;
            };
            for( const test_car &c : cars ) {
                const fp3d::vehicle_view *view = nullptr;
                for( const fp3d::vehicle_view &v : parked->vehicles ) {
                    if( v.name == c.name ) {
                        view = &v;
                    }
                }
                std::string rows;
                int right = 0;
                int found_by_map = 0;
                std::set<std::tuple<int, int, int>> squares;
                size_t real_parts = 0;
                size_t fillers = 0;
                int fillers_right = 0;
                if( view != nullptr && c.veh != nullptr ) {
                    const tripoint_bub_ms origin = c.veh->pos_bub( here );
                    const auto note = [&]( const fp3d::vehicle_part_view & row ) {
                        const optional_vpart_position there = here.veh_at( origin + tripoint_rel_ms( row.dx, row.dy,
                                row.dz ) );
                        found_by_map += there && &there->vehicle() == c.veh &&
                                        flagged( origin + tripoint_rel_ms( row.dx, row.dy, row.dz ) ) ? 1 : 0;
                        rows += std::string( " [" ) + ( row.state & fp3d::part_gap_filler ? "filler " : "" ) +
                                std::to_string( row.mount_x ) + "," + std::to_string( row.mount_y ) + " -> " +
                                std::to_string( row.dx ) + "," + std::to_string( row.dy ) + "," +
                                std::to_string( row.dz ) + "]";
                    };
                    // Its own parts first, in CDDA's order.
                    for( const vpart_reference &vp : c.veh->get_all_parts() ) {
                        if( real_parts < view->parts.size() ) {
                            const fp3d::vehicle_part_view &row = view->parts[real_parts];
                            const tripoint_rel_ms want = vp.pos_bub( here ) - origin;
                            right += row.mount_x == vp.part().mount.x() && row.mount_y == vp.part().mount.y() &&
                                     row.dx == want.x() && row.dy == want.y() && row.dz == want.z() &&
                                     ( row.state & fp3d::part_gap_filler ) == 0 ? 1 : 0;
                            squares.emplace( row.dx, row.dy, row.dz );
                            note( row );
                        }
                        real_parts++;
                    }
                    // Then CDDA's active fake parts: the copied part's mount and vpart, the fake's own square, which
                    // is none of the vehicle's own.
                    for( const vpart_reference &vp : c.veh->get_all_parts_with_fakes() ) {
                        if( !vp.part().is_fake ) {
                            continue;
                        }
                        const size_t at = real_parts + fillers;
                        if( at < view->parts.size() ) {
                            const fp3d::vehicle_part_view &row = view->parts[at];
                            const vehicle_part &copied = c.veh->part( vp.part().fake_part_to );
                            const tripoint_rel_ms want = vp.pos_bub( here ) - origin;
                            fillers_right += ( row.state & fp3d::part_gap_filler ) != 0 &&
                                             row.mount_x == copied.mount.x() && row.mount_y == copied.mount.y() &&
                                             row.vpart == fp3d::intern_id( fp3d::id_kind::vpart, copied.info().id.str() ) &&
                                             row.dx == want.x() && row.dy == want.y() && row.dz == want.z() &&
                                             squares.count( { row.dx, row.dy, row.dz } ) == 0 ? 1 : 0;
                            note( row );
                        }
                        fillers++;
                    }
                }
                const size_t mounts = c.wall ? line.size() : ell.size();
                const size_t want_real = c.wall ? 2 * mounts : mounts;
                const bool fillers_as_expected = c.wall && c.dir != 0_degrees ? fillers >= 1 : fillers == 0;
                check( view != nullptr && real_parts == want_real && view->parts.size() == real_parts + fillers &&
                       right == static_cast<int>( real_parts ) && fillers_right == static_cast<int>( fillers ) &&
                       found_by_map == static_cast<int>( real_parts + fillers ) && squares.size() == mounts &&
                       fillers_as_expected, ( std::string( c.name ) + ": each part's square" ).c_str(),
                       view == nullptr ? "vehicle not seen" : "face " + std::to_string( static_cast<int>( std::lround(
                                   view->face_deg ) ) ) + " deg, mount -> dx,dy,dz:" + rows + "; " +
                       std::to_string( fillers ) + " gap fillers; " + std::to_string( found_by_map ) +
                       " found and flagged there" );
            }
            // The JSON row: the five fields older bridges sent, then dx, dy, dz.
            bool row_written = false;
            for( const fp3d::vehicle_view &v : parked->vehicles ) {
                if( v.name == "fp3d wall 45" && !v.parts.empty() ) {
                    const fp3d::vehicle_part_view &row = v.parts.back();
                    const std::string want = "[" + std::to_string( row.mount_x ) + "," + std::to_string( row.mount_y ) +
                                             "," + std::to_string( row.vpart ) + "," + std::to_string( row.hp_frac ) +
                                             "," + std::to_string( row.state ) + "," + std::to_string( row.dx ) + "," +
                                             std::to_string( row.dy ) + "," + std::to_string( row.dz ) + "]";
                    const size_t named_at = parked_json.str().find( "\"name\":\"fp3d wall 45\"" );
                    row_written = ( row.state & fp3d::part_gap_filler ) != 0 && named_at != std::string::npos &&
                                  parked_json.str().find( want, named_at ) != std::string::npos;
                }
            }
            check( row_written, "a gap filler's JSON row: mount, vpart, hp, state with bit 3, then dx, dy, dz" );
            for( const test_car &c : cars ) {
                if( c.veh != nullptr ) {
                    here.destroy_vehicle( c.veh );
                }
            }
            refresh_caches();
        }
        check_client_snapshots();
    } catch( const std::exception &e ) {
        std::printf( "error: %s\n", e.what() );
        return EXIT_FAILURE;
    }
    std::printf( "%s (%d failed)\n", failures == 0 ? "all checks passed" : "FAILED", failures );
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
