// FP3D block B4 test: boots a town, makes things happen next to the player (message, sound, melee, damage, a
// shot, deaths, an activity, explosions) and checks the events the bus queues for each, including what it must
// leave out (a death the player can't see) and the overflow rule. Exit code 0 = all checks pass.
//
// usage: fp3d-events-test   (run from the cdda/ folder)
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

#include "avatar.h"
#include "ballistics.h"
#include "bodypart.h"
#include "cached_options.h"
#include "calendar.h"
#include "coordinates.h"
#include "damage.h"
#include "debug.h"
#include "dispersion.h"
#include "explosion.h"
#include "fp3d/boot.h"
#include "fp3d/events.h"
#include "fp3d/points.h"
#include "fp3d/snapshot.h"
#include "game.h"
#include "map.h"
#include "messages.h"
#include "monster.h"
#include "projectile.h"
#include "rng.h"
#include "sounds.h"
#include "type_id.h"

namespace
{
const activity_id ACT_WAIT( "ACT_WAIT" );
const damage_type_id damage_bash( "bash" );
const furn_str_id furn_f_null( "f_null" );
const mtype_id mon_zombie( "mon_zombie" );
const ter_str_id ter_t_floor( "t_floor" );
const trait_id trait_DEBUG_NIGHTVISION( "DEBUG_NIGHTVISION" );

using fp3d::event;
using fp3d::event_kind;

int failures = 0;

void check( bool ok, const char *what, const std::string &detail = "" )
{
    std::printf( "  %-44s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str() );
    failures += ok ? 0 : 1;
}

std::vector<event> drain( event_kind kind )
{
    std::vector<event> all;
    std::vector<event> out;
    fp3d::drain_events( all );
    for( event &e : all ) {
        if( e.kind == kind ) {
            out.push_back( std::move( e ) );
        }
    }
    return out;
}

std::string name_of( fp3d::id_kind kind, uint32_t index )
{
    const std::vector<std::string> names = fp3d::id_names( kind );
    return index < names.size() ? names[index] : "?";
}

fp3d::ui_point abs_of( const tripoint_bub_ms &p )
{
    return fp3d::to_ui_point( get_map().get_abs( p ) );
}

bool same( const fp3d::ui_point &a, const fp3d::ui_point &b )
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

void refresh_caches()
{
    get_map().build_map_cache( get_avatar().posz() );
}
} // namespace

int main()
{
    std::setvbuf( stdout, nullptr, _IONBF, 0 );
    test_mode = true;
    setupDebug( DebugOutput::std_err );
    rng_set_engine_seed( 11 );
    try {
        fp3d::boot_world( "fp3d-events", false );
        fp3d::load_bubble_in_town();
        map &here = get_map();
        avatar &u = get_avatar();
        u.set_mutation( trait_DEBUG_NIGHTVISION );
        // An open 9x9 floor around the player, so lines of sight and the shot's path are clear.
        const tripoint_bub_ms at = u.pos_bub();
        for( int dy = -4; dy <= 4; dy++ ) {
            for( int dx = -4; dx <= 4; dx++ ) {
                here.ter_set( at + tripoint_rel_ms( dx, dy, 0 ), ter_t_floor );
                here.furn_set( at + tripoint_rel_ms( dx, dy, 0 ), furn_f_null );
            }
        }
        refresh_caches();

        fp3d::set_events_enabled( false );
        add_msg( "quiet" );
        std::vector<event> none;
        fp3d::drain_events( none );
        check( none.empty(), "disabled bus queues nothing", std::to_string( none.size() ) );
        fp3d::set_events_enabled( true );

        const uint64_t seq = fp3d::capture_snapshot()->seq;
        add_msg( m_good, "<color_red>Hello</color> world" );
        std::vector<event> msgs = drain( event_kind::message );
        check( msgs.size() == 1 && msgs[0].text == "Hello world" && msgs[0].detail == m_good &&
               msgs[0].seq == seq + 1, "message: colour tags gone, next snapshot's seq",
               msgs.empty() ? "none" : "\"" + msgs[0].text + "\" seq " + std::to_string( msgs[0].seq ) );

        sounds::sound( at + tripoint_rel_ms( 3, 0, 0 ), 40, sounds::sound_t::combat, "a bang", false, "explosion",
                       "default" );
        sounds::process_sound_markers( &u );
        sounds::reset_sounds();
        std::vector<event> snd = drain( event_kind::sound );
        check( snd.size() == 1 && snd[0].amount == 40 && snd[0].heard > 0 && snd[0].heard < 40 &&
               name_of( fp3d::id_kind::sound, snd[0].type ) == "explosion/default" &&
               same( snd[0].pos, abs_of( at + tripoint_rel_ms( 3, 0, 0 ) ) ), "sound: volume, heard, id, place",
               snd.empty() ? "none" : "heard " + std::to_string( snd[0].heard ) );

        monster *zed = g->place_critter_at( mon_zombie, at + tripoint_rel_ms( 1, 0, 0 ) );
        refresh_caches();
        const uint64_t zed_uid = fp3d::creature_uid( *zed );
        u.melee_attack( *zed, false );
        std::vector<event> atk = drain( event_kind::attack );
        check( atk.size() == 1 && atk[0].uid == 1 && atk[0].other == zed_uid, "melee: attacker and target uids",
               atk.empty() ? "none" : std::string( atk[0].detail ? "hit" : "miss" ) );

        bool zed_in_snapshot = false;
        const std::shared_ptr<const fp3d::snapshot> snap = fp3d::capture_snapshot();
        for( const fp3d::creature_view &c : snap->creatures ) {
            zed_in_snapshot = zed_in_snapshot || c.uid == zed_uid;
        }
        check( zed_in_snapshot, "the snapshot uses the same uid" );

        zed->deal_damage( &u, bodypart_id( "torso" ), damage_instance( damage_bash, 5 ) );
        std::vector<event> dmg = drain( event_kind::damage );
        check( dmg.size() == 1 && dmg[0].uid == zed_uid && dmg[0].amount > 0 &&
               name_of( fp3d::id_kind::damage_type, dmg[0].type ) == "bash" &&
               name_of( fp3d::id_kind::body_part, dmg[0].detail ) == "torso", "damage: amount, type, body part",
               dmg.empty() ? "none" : std::to_string( dmg[0].amount ) );

        projectile proj;
        proj.speed = 1000;
        proj.range = 10;
        proj.impact = damage_instance( damage_bash, 1 );
        dealt_projectile_attack shot;
        projectile_attack( shot, proj, &here, at, at + tripoint_rel_ms( -4, 0, 0 ), dispersion_sources( 0 ), &u );
        std::vector<event> prj = drain( event_kind::projectile );
        check( prj.size() == 1 && prj[0].uid == 1 && prj[0].path.size() >= 2 &&
               same( prj[0].path[0], abs_of( at ) ) &&
               same( prj[0].path[1], abs_of( at + tripoint_rel_ms( -1, 0, 0 ) ) ),
               "projectile: path from the shooter", prj.empty() ? "none" :
               std::to_string( prj[0].path.size() ) + " squares" );

        zed->die( &here, &u );
        std::vector<event> dead = drain( event_kind::death );
        check( dead.size() == 1 && dead[0].uid == zed_uid &&
               name_of( fp3d::id_kind::mtype, dead[0].type ) == "mon_zombie", "death in view: uid and type" );
        g->cleanup_dead();

        // Under the player's floor: out of sight, so its death is not sent.
        const tripoint_bub_ms below = at + tripoint_rel_ms( 0, 0, -1 );
        here.ter_set( below, ter_t_floor );
        monster *hidden = g->place_critter_at( mon_zombie, below );
        refresh_caches();
        const bool hidden_unseen = hidden != nullptr && !u.sees( here, *hidden );
        if( hidden != nullptr ) {
            hidden->die( &here, &u );
        }
        std::vector<event> unseen = drain( event_kind::death );
        check( hidden_unseen && unseen.empty(), "death out of sight: no event",
               std::to_string( unseen.size() ) + " events" );
        g->cleanup_dead();

        u.assign_activity( ACT_WAIT, 1000 );
        std::vector<event> anim = drain( event_kind::anim );
        check( anim.size() == 1 && anim[0].uid == 1 && name_of( fp3d::id_kind::activity, anim[0].type ) == "ACT_WAIT",
               "activity: anim event" );
        u.cancel_activity();

        const tripoint_bub_ms blast_at = at + tripoint_rel_ms( -3, 2, 0 );
        explosion_handler::explosion( nullptr, blast_at, 2.0f );
        explosion_handler::process_explosions();
        explosion_handler::flashbang( blast_at, true );
        std::vector<event> boom = drain( event_kind::explosion );
        check( boom.size() == 2 && name_of( fp3d::id_kind::explosion, boom[0].type ) == "blast" &&
               !boom[0].path.empty() && same( boom[0].pos, abs_of( blast_at ) ) &&
               name_of( fp3d::id_kind::explosion, boom[1].type ) == "flashbang" && boom[1].amount == 8,
               "explosions: blast area, flashbang radius", std::to_string( boom.size() ) + " events" +
               ( boom.empty() ? "" : ", blast covers " + std::to_string( boom[0].path.size() ) ) );

        // Overflow: 10 sounds then a full queue of messages. The sounds go first; every message stays.
        fp3d::reset_events();
        for( int i = 0; i < 10; i++ ) {
            fp3d::on_sound( at, 10, 10, 0, "tick", "", "" );
        }
        for( size_t i = 0; i < fp3d::max_events; i++ ) {
            fp3d::on_message( "m" + std::to_string( i ), 0 );
        }
        std::vector<event> full;
        const size_t dropped = fp3d::drain_events( full );
        size_t sounds_left = 0;
        for( const event &e : full ) {
            sounds_left += e.kind == event_kind::sound ? 1 : 0;
        }
        check( full.size() == fp3d::max_events && dropped == 10 && sounds_left == 0 && full[0].text == "m0",
               "overflow drops sounds before messages", std::to_string( dropped ) + " dropped" );

        // "€" is 3 bytes: the cut must not split one.
        std::string euros;
        for( int i = 0; i < 500; i++ ) {
            euros += "\xE2\x82\xAC";
        }
        fp3d::on_message( euros, 0 );
        std::vector<event> longmsg = drain( event_kind::message );
        check( longmsg.size() == 1 && longmsg[0].text.size() == 1023, "long text cut on a UTF-8 boundary",
               longmsg.empty() ? "none" : std::to_string( longmsg[0].text.size() ) + " bytes" );
    } catch( const std::exception &e ) {
        std::printf( "error: %s\n", e.what() );
        return EXIT_FAILURE;
    }
    std::printf( "%s (%d failed)\n", failures == 0 ? "all checks passed" : "FAILED", failures );
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
