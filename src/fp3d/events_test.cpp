// FP3D block B4 test: boots a town, makes things happen next to the player (message, sound, melee, damage, a
// shot, deaths, an activity, explosions) and checks the events the bus queues for each, including what it must
// leave out (a death the player can't see) and the overflow rule. Exit code 0 = all checks pass.
//
// usage: fp3d-events-test   (run from the cdda/ folder)
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "avatar.h"
#include "ballistics.h"
#include "bodypart.h"
#include "cached_options.h"
#include "calendar.h"
#include "coordinates.h"
#include "damage.h"
#include "debug.h"
#include "dialogue.h"
#include "dispersion.h"
#include "effect_on_condition.h"
#include "explosion.h"
#include "fp3d/boot.h"
#include "fp3d/bridge.h"
#include "fp3d/events.h"
#include "fp3d/points.h"
#include "fp3d/snapshot.h"
#include "fp3d/swing.h"
#include "game.h"
#include "map.h"
#include "martialarts.h"
#include "messages.h"
#include "monster.h"
#include "mtype.h"
#include "projectile.h"
#include "rng.h"
#include "sounds.h"
#include "talker.h"
#include "translation.h"
#include "type_id.h"
#include "weakpoint.h"

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
        // A hit's order, as the client confirms it (HitFeedback): the player's attack event, then the target's
        // damage from the player, only messages and sounds between. Swung until a hit does damage.
        {
            bool ordered = false;
            bool one_each = true;       // every swing sent exactly one attack event
            bool unnamed = true;        // a plain hit (no special attack) names no technique
            int swings = 0;
            std::string seen_order;
            for( ; swings < 20 && !ordered; swings++ ) {
                zed->set_hp( zed->get_hp_max() );
                u.melee_attack( *zed, false );
                std::vector<event> all;
                fp3d::drain_events( all );
                int attacks = 0;
                for( const event &e : all ) {
                    attacks += e.kind == event_kind::attack ? 1 : 0;
                    unnamed = unnamed && ( e.kind != event_kind::attack || ( e.text.empty() && e.amount == 0 ) );
                }
                one_each = one_each && attacks == 1;
                size_t i = 0;
                while( i < all.size() && !( all[i].kind == event_kind::attack && all[i].uid == 1 &&
                                             all[i].other == zed_uid ) ) {
                    i++;
                }
                if( i == all.size() || all[i].detail != 1 ) {
                    continue;   // a miss
                }
                size_t j = i + 1;
                while( j < all.size() && ( all[j].kind == event_kind::message || all[j].kind == event_kind::sound ) ) {
                    j++;
                }
                seen_order = j < all.size() ? std::to_string( static_cast<int>( all[j].kind ) ) : "end";
                ordered = j < all.size() && all[j].kind == event_kind::damage && all[j].uid == zed_uid &&
                          all[j].other == 1;
            }
            zed->set_hp( zed->get_hp_max() );
            check( ordered && one_each && unnamed, "melee: a hit's attack event, then its damage from the player",
                   std::to_string( swings ) + " swings, after the hit: " + seen_order +
                   ( one_each ? "" : ", not one attack event a swing" ) +
                   ( unnamed ? "" : ", a plain strike named a technique" ) );
        }
        // A hit names its technique (fp3d/swing.h swing_note): its translated name and its family; a miss none.
        {
            const matec_id rapid( "RAPID" );
            const std::string rapid_name = rapid.is_valid() ? rapid->name.translated() : std::string( "?" );
            bool one_each = true;
            bool misses_plain = true;
            bool hit = false;
            bool named = false;
            int swings = 0;
            std::string got = "no hit";
            for( ; swings < 20 && !hit && rapid.is_valid(); swings++ ) {
                zed->set_hp( zed->get_hp_max() );
                u.melee_attack( *zed, false, rapid );
                const std::vector<event> atks = drain( event_kind::attack );
                one_each = one_each && atks.size() == 1 && atks[0].uid == 1 && atks[0].other == zed_uid;
                if( atks.size() != 1 ) {
                    continue;
                }
                if( atks[0].detail == 0 ) {
                    misses_plain = misses_plain && atks[0].text.empty() && atks[0].amount == 0;
                    continue;
                }
                hit = true;
                got = "\"" + atks[0].text + "\" family " + std::to_string( atks[0].amount );
                named = atks[0].detail == 1 && atks[0].text == rapid_name && atks[0].amount == fp3d::technique_strike;
            }
            zed->set_hp( zed->get_hp_max() );
            check( named && one_each && misses_plain && rapid_name == "Rapid Strike",
                   "melee: a forced technique names itself", std::to_string( swings ) + " swings, " + got +
                   ( one_each ? "" : ", not one attack event a swing" ) +
                   ( misses_plain ? "" : ", a miss named a technique" ) );
        }
        {
            struct family_of {
                const char *id;
                int family;
            };
            const family_of families[] = {
                { "SWEEP", fp3d::technique_down }, { "BRUTAL", fp3d::technique_stun },
                { "PRECISE", fp3d::technique_stun }, { "WRAP", fp3d::technique_stun },
                { "RAPID", fp3d::technique_strike }, { "PENETRATE", fp3d::technique_strike },
                { "WHIP_DISARM", fp3d::technique_disarm }, { "DEF_DISARM", fp3d::technique_disarm },
                { "tec_none", fp3d::technique_none }, { "WBLOCK_1", fp3d::technique_none },
                { "tec_feint", fp3d::technique_none }
            };
            std::string wrong;
            for( const family_of &f : families ) {
                const matec_id id( f.id );
                const int family = id.is_valid() ? fp3d::technique_family( *id ) : -1;
                if( family != f.family ) {
                    wrong += std::string( wrong.empty() ? "" : ", " ) + f.id + " " + std::to_string( family );
                }
            }
            check( wrong.empty(), "technique families", wrong.empty() ? std::to_string( std::size( families ) ) +
                   " techniques" : "wrong: " + wrong );
        }
        {
            // colour tags, two control characters and more characters than fit: 7 bytes, then 100 of 2 bytes
            std::string raw = "<color_red>Swe\x01" "e\np</color>";
            std::string clean = "Sweep";
            for( int i = 0; i < 100; i++ ) {
                raw += "\xC3\xA9";
                clean += i < 28 ? "\xC3\xA9" : "";
            }
            const std::string text = fp3d::technique_text( raw );
            // bytes that are no UTF-8 character are left out: a stray byte, a cut-off character, an overlong form,
            // a surrogate, one past U+10FFFF; a 4-byte character stays whole
            const std::string odd = fp3d::technique_text( std::string( "\xFF\xC3" "ok\xE2\x82" ) );
            const std::string forms = fp3d::technique_text( std::string( "\xC0\x80\xED\xA0\x80\xF4\x90\x80\x80"
                                      "a\xF0\x9F\x98\x80" ) );
            // a tag that one pass over the name leaves, or that a dropped byte puts together, goes too: an opener
            // inside an opener, a control character inside a tag, a stray byte inside one
            const std::string nested = fp3d::technique_text( "<col<color_red>or_red>X" );
            const std::string joined = fp3d::technique_text( std::string( "<co\x01lor_red>X</co\nlor>" ) );
            const std::string stray = fp3d::technique_text( std::string( "<col\xFFor_red>X" ) );
            // a name is read only so far (CDDA's tag pass repeats the tail once per opener that lies before one
            // '>'): this one's '>' and tail are never reached, and its openers are no text
            std::string hostile;
            for( int i = 0; i < 2000; i++ ) {
                hostile += "<color_";
            }
            hostile += ">" + std::string( 20000, 'a' );
            const std::string cut_short = fp3d::technique_text( hostile );
            const bool tags_gone = nested == "X" && joined == "X" && stray == "X" && cut_short.empty();
            const fp3d::technique_note unseen = fp3d::swing_note( u, matec_id( "SWEEP" ), false );
            const fp3d::technique_note none = fp3d::swing_note( u, tec_none, true );
            const fp3d::technique_note unknown = fp3d::swing_note( u, matec_id( "fp3d_no_such_technique" ), true );
            const fp3d::technique_note sweep = fp3d::swing_note( u, matec_id( "SWEEP" ), true );
            const auto empty = []( const fp3d::technique_note & n ) {
                return n.name.empty() && n.family == 0;
            };
            // a monster's hit and the avatar's miss never name a technique; the family is clamped
            drain( event_kind::attack );
            fp3d::on_attack( *zed, u, "", true, "Sweep Attack", fp3d::technique_down );
            fp3d::on_attack( u, *zed, "", false, "Sweep Attack", fp3d::technique_down );
            fp3d::on_attack( u, *zed, "", true, "Sweep Attack", 99 );
            fp3d::on_attack( u, *zed, "", true, "Sweep Attack", -3 );
            const std::vector<event> sent = drain( event_kind::attack );
            const bool sent_ok = sent.size() == 4 && sent[0].text.empty() && sent[0].amount == 0 &&
                                 sent[1].text.empty() && sent[1].amount == 0 && sent[2].text == "Sweep Attack" &&
                                 sent[2].amount == fp3d::max_technique_family && sent[3].amount == 0;
            check( text == clean && text.size() == 61 && text.size() <= fp3d::max_technique_name && odd == "ok" &&
                   forms == "a\xF0\x9F\x98\x80" && empty( unseen ) && empty( none ) && empty( unknown ) &&
                   sweep.name == matec_id( "SWEEP" )->name.translated() && sweep.family == fp3d::technique_down &&
                   sent_ok && tags_gone, "technique text is cleaned", std::to_string( text.size() ) + " bytes \"" +
                   text + "\", odd \"" + odd + "\", sweep \"" + sweep.name + "\" " + std::to_string( sweep.family ) +
                   ", " + std::to_string( sent.size() ) + " events" + ( sent_ok ? "" : " (wrong)" ) +
                   ( tags_gone ? "" : ", tags left: \"" + nested + "\" \"" + joined + "\" \"" + stray + "\", " +
                     std::to_string( cut_short.size() ) + " bytes of the long name" ) );
        }
        // A swing inside a swing (a technique's extra hit on another creature) rolls for itself: the roll the
        // outer swing's part is weighted by stays the outer swing's (fp3d/swing.h swing_rolled). The outer swing
        // asks as an aimed strike does; a second thread answers it as a client would, for the zombie's leg.
        {
            monster *other = g->place_critter_at( mon_zombie, at + tripoint_rel_ms( 0, 1, 0 ) );
            const fp3d::swing_counts c0 = fp3d::get_swing_counts();
            fp3d::swing_counts c1 = c0;
            int inner_landed = 0;
            int outer_tries = 0;
            bool outer_landed = false;
            if( other != nullptr ) {
                const fp3d::aimed_strike aimed;
                std::atomic<bool> stop{ false };
                std::thread client( [&stop, zed_uid]() {
                    while( !stop ) {
                        const std::optional<fp3d::ui_request> r = fp3d::wait_request( 100 );
                        if( !r ) {
                            continue;
                        }
                        fp3d::shot_answer leg;
                        leg.end = r->cursor;
                        leg.hit_uid = zed_uid;
                        leg.part = "leg";
                        fp3d::ui_answer a;
                        a.id = r->id;
                        a.shot = leg;
                        fp3d::answer( a );
                        return;
                    }
                } );
                std::optional<fp3d::swing_scope> outer;
                fp3d::set_active( true );    // only a live bridge asks
                try {
                    outer.emplace( u, *zed );
                } catch( ... ) {
                    std::printf( "  the swing request threw\n" );
                }
                fp3d::set_active( false );
                stop = true;
                client.join();
                c1 = fp3d::get_swing_counts();
                weakpoint_attack hit;
                hit.source = &u;
                hit.target = zed;
                hit.type = weakpoint_attack::attack_type::MELEE_BASH;
                hit.compute_wp_skill();
                // The outer swing has rolled no hit yet. The inner one's critical hit is not the outer swing's:
                // the leg never lands. (Were it taken, each try would land with the leg's chance times 4.)
                {
                    const fp3d::swing_scope inner( u, *other );
                    fp3d::swing_rolled( 0, true );
                }
                for( int i = 0; i < 200; i++ ) {
                    inner_landed += fp3d::swing_weakpoint( *zed, hit ) != nullptr ? 1 : 0;
                }
                // The outer swing's critical hit stays through an inner swing's roll (a miss, to tell them apart):
                // the leg lands, at its chance times 4 a try (about 1 in 5 here: all 2000 failing is 0.8^2000).
                fp3d::swing_rolled( 0, true );
                {
                    const fp3d::swing_scope inner( u, *other );
                    fp3d::swing_rolled( -1, false );
                }
                for( ; outer_tries < 2000 && !outer_landed; outer_tries++ ) {
                    outer_landed = fp3d::swing_weakpoint( *zed, hit ) != nullptr;
                }
                outer.reset();
                g->remove_zombie( *other );
                refresh_caches();
            }
            const fp3d::swing_counts c2 = fp3d::get_swing_counts();
            check( other != nullptr && c1.asked - c0.asked == 1 && c1.accepted - c0.accepted == 1 &&
                   inner_landed == 0 && outer_landed && c2.parts - c0.parts == 1,
                   "a swing inside a swing keeps the outer roll", "asked +" + std::to_string( c1.asked - c0.asked ) +
                   " accepted +" + std::to_string( c1.accepted - c0.accepted ) + ", the inner roll landed " +
                   std::to_string( inner_landed ) + " of 200, the outer roll " +
                   ( outer_landed ? "landed at try " + std::to_string( outer_tries ) : std::string( "never landed" ) ) +
                   ", parts +" + std::to_string( c2.parts - c0.parts ) );
        }

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
        // The source: the player's uid, 0 without one, and 0 for a monster the player doesn't see (under the floor)
        check( dmg.size() == 1 && dmg[0].other == 1, "damage: the player as its source",
               dmg.empty() ? "none" : "other " + std::to_string( dmg[0].other ) );
        zed->deal_damage( nullptr, bodypart_id( "torso" ), damage_instance( damage_bash, 5 ) );
        std::vector<event> unsourced = drain( event_kind::damage );
        check( unsourced.size() == 1 && unsourced[0].other == 0, "damage: no source, other 0",
               unsourced.empty() ? "none" : "other " + std::to_string( unsourced[0].other ) );
        {
            // the weak point the hit landed on: a monster's names it in `text`; none, or an id that is no plain id,
            // sends ""
            const weakpoint *head = nullptr;
            for( const weakpoint &w : zed->type->weakpoints.weakpoint_list ) {
                head = w.id == "head" ? &w : head;
            }
            weakpoint_attack aimed;
            aimed.accuracy = 1.0;   // not -1: absorb_hit takes the weak point given, as for an aimed hit
            if( head != nullptr ) {
                zed->deal_damage( &u, bodypart_id( "torso" ), damage_instance( damage_bash, 10 ), aimed, *head );
            }
            const std::vector<event> on_head = drain( event_kind::damage );
            zed->deal_damage( &u, bodypart_id( "torso" ), damage_instance( damage_bash, 5 ), aimed, weakpoint() );
            const std::vector<event> on_body = drain( event_kind::damage );
            const bool ids_ok = fp3d::weakpoint_text( "leg_joint" ) == "leg_joint" &&
                                fp3d::weakpoint_text( "Eye2" ) == "Eye2" && fp3d::weakpoint_text( "" ).empty() &&
                                fp3d::weakpoint_text( "the head" ).empty() && fp3d::weakpoint_text( "9th" ).empty() &&
                                fp3d::weakpoint_text( "<color_red>head" ).empty() &&
                                fp3d::weakpoint_text( "h\xC3\xA9" "ad" ).empty() &&
                                fp3d::weakpoint_text( std::string( fp3d::max_weakpoint_id, 'a' ) ).size() ==
                                fp3d::max_weakpoint_id &&
                                fp3d::weakpoint_text( std::string( fp3d::max_weakpoint_id + 1, 'a' ) ).empty();
            check( head != nullptr && on_head.size() == 1 && on_head[0].text == "head" && on_body.size() == 1 &&
                   on_body[0].text.empty() && ids_ok, "damage: a monster's weak point is named",
                   std::string( head == nullptr ? "no head weak point" : "" ) + ( on_head.empty() ? " none" :
                           " \"" + on_head[0].text + "\"" ) + ( on_body.empty() ? " none" : " \"" + on_body[0].text + "\"" ) +
                   ( ids_ok ? "" : " ids wrong" ) );
            zed->set_hp( zed->get_hp_max() );   // the checks after this one want it alive
        }
        {
            const tripoint_bub_ms under = at + tripoint_rel_ms( 0, 0, -1 );
            here.ter_set( under, ter_t_floor );
            monster *lurker = g->place_critter_at( mon_zombie, under );
            refresh_caches();
            const bool lurker_unseen = lurker != nullptr && !u.sees( here, *lurker ) && u.sees( here, *zed );
            if( lurker != nullptr ) {
                zed->deal_damage( lurker, bodypart_id( "torso" ), damage_instance( damage_bash, 5 ) );
            }
            std::vector<event> hidden_source = drain( event_kind::damage );
            check( lurker_unseen && hidden_source.size() == 1 && hidden_source[0].uid == zed_uid &&
                   hidden_source[0].other == 0, "damage: an unseen source is 0",
                   hidden_source.empty() ? "none" : "other " + std::to_string( hidden_source[0].other ) );
            if( lurker != nullptr ) {
                g->remove_zombie( *lurker );
            }
            refresh_caches();
        }

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

        {
            // CDDA's lightning flash (the weather's `lightning` effect): one event at the player's square, and
            // none while events are off.
            const effect_on_condition_id lightning( "lightning" );
            drain( event_kind::lightning );
            dialogue d( get_talker_for( u ), nullptr );
            lightning->apply_true_effects( d );
            std::vector<event> flash = drain( event_kind::lightning );
            const tripoint_abs_ms where = u.pos_abs();
            check( flash.size() == 1 && flash[0].pos.x == where.x() && flash[0].pos.y == where.y() &&
                   flash[0].pos.z == where.z() && flash[0].uid == 0, "lightning: one event at the player",
                   std::to_string( flash.size() ) + " events" );
            fp3d::set_events_enabled( false );
            dialogue d2( get_talker_for( u ), nullptr );
            lightning->apply_true_effects( d2 );
            fp3d::set_events_enabled( true );
            check( drain( event_kind::lightning ).empty(), "lightning: nothing while events are off" );
        }

        const tripoint_bub_ms blast_at = at + tripoint_rel_ms( -3, 2, 0 );
        {
            const fp3d::explosion_item_scope grenade( "grenade_act" );
            explosion_handler::explosion( nullptr, blast_at, 2.0f );
        }
        {
            // Set off later, inside another item's scope: still the grenade's blast.
            const fp3d::explosion_item_scope other( "c4armed" );
            explosion_handler::process_explosions();
        }
        explosion_handler::flashbang( blast_at, true );
        std::vector<event> boom = drain( event_kind::explosion );
        check( boom.size() == 2 && name_of( fp3d::id_kind::explosion, boom[0].type ) == "blast" &&
               !boom[0].path.empty() && same( boom[0].pos, abs_of( blast_at ) ) &&
               name_of( fp3d::id_kind::explosion, boom[1].type ) == "flashbang" && boom[1].amount == 8 &&
               name_of( fp3d::id_kind::itype, boom[0].detail ) == "grenade_act" && boom[1].detail == 0,
               "explosions: blast area and item, flashbang radius", std::to_string( boom.size() ) + " events" +
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
