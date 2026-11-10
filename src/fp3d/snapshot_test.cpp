// FP3D block B3 test: boots a town and checks snapshots against CDDA's map, read through a different path
// (the submap buffer, by absolute submap). Also checks unchanged submaps are shared, an edit bumps exactly one
// revision, a zombie keeps its uid, and a map shift keeps everything matching. Exit code 0 = all checks pass.
//
// usage: fp3d-snapshot-test   (run from the cdda/ folder)
#include <algorithm>
#include <chrono>
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

#include "avatar.h"
#include "cached_options.h"
#include "coordinates.h"
#include "calendar.h"
#include "debug.h"
#include "item.h"
#include "fp3d/boot.h"
#include "fp3d/points.h"
#include "fp3d/snapshot.h"
#include "game.h"
#include "map.h"
#include "map_scale_constants.h"
#include "mapbuffer.h"
#include "mapdata.h"
#include "monster.h"
#include "rng.h"
#include "submap.h"
#include "type_id.h"

namespace
{
const mtype_id mon_dog( "mon_dog" );
const mtype_id mon_zombie( "mon_zombie" );
const ter_str_id ter_t_wall( "t_wall" );
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
        refresh_caches();
        check( ter_str_id::NULL_ID().id().to_i() == 0 && furn_str_id::NULL_ID().id().to_i() == 0,
               "index 0 is t_null / f_null" );

        double ms = 0;
        const std::shared_ptr<const fp3d::snapshot> first = timed_capture( ms );
        std::printf( "  capture took %.0f ms, origin %d,%d\n", ms, first->bubble_origin.x, first->bubble_origin.y );
        check_against_mapbuffer( *first, "full capture matches the submap buffer" );
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
            u.remove_weapon();
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
    } catch( const std::exception &e ) {
        std::printf( "error: %s\n", e.what() );
        return EXIT_FAILURE;
    }
    std::printf( "%s (%d failed)\n", failures == 0 ? "all checks passed" : "FAILED", failures );
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
