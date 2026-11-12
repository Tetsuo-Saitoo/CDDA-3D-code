// FP3D block B3 test: boots a town and checks snapshots against CDDA's map, read through a different path
// (the submap buffer, by absolute submap). Also checks unchanged submaps are shared, an edit bumps exactly one
// revision, a zombie keeps its uid, and a map shift keeps everything matching. Exit code 0 = all checks pass.
//
// usage: fp3d-snapshot-test   (run from the cdda/ folder)
#include <algorithm>
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
#include "weather.h"
#include "weather_type.h"

namespace
{
const mtype_id mon_dog( "mon_dog" );
const mtype_id mon_zombie( "mon_zombie" );
const ter_str_id ter_t_floor( "t_floor" );
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
            // A fault the gun type can't get (a save or a mod can add one) is listed too.
            u.get_wielded_item()->set_fault( fault_id( "fault_engine_starter" ), true, false );
            const std::shared_ptr<const fp3d::snapshot> odd = timed_capture( gun_ms );
            const std::optional<fp3d::gun_view> &g2 = odd->player.wielded_gun;
            const auto lists = [&]( const std::string & f ) {
                return g2 && std::find( g2->faults.begin(), g2->faults.end(), f ) != g2->faults.end();
            };
            check( g2 && g2->faults.size() == 2 && lists( fault ) && lists( "fault_engine_starter" ),
                   "wielded_gun lists every fault it has", g2 ? std::to_string( g2->faults.size() ) : "none" );
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
    } catch( const std::exception &e ) {
        std::printf( "error: %s\n", e.what() );
        return EXIT_FAILURE;
    }
    std::printf( "%s (%d failed)\n", failures == 0 ? "all checks passed" : "FAILED", failures );
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
