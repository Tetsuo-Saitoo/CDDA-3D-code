#include "fp3d/events.h"

#include <algorithm>
#include <deque>
#include <mutex>
#include <utility>

#include "avatar.h"
#include "calendar.h"
#include "character.h"
#include "creature.h"
#include "damage.h"
#include "fp3d/points.h"
#include "fp3d/snapshot.h"
#include "line.h"
#include "map.h"
#include "monster.h"
#include "mtype.h"
#include "output.h"

namespace fp3d
{

std::atomic<bool> events_on{ false };

namespace
{
std::mutex queue_mutex;
std::deque<event> queue;
size_t dropped = 0;

bool low_priority( const event &e )
{
    return e.kind == event_kind::sound || e.kind == event_kind::anim || e.kind == event_kind::turn_end;
}

void push( event &&e )
{
    e.turn = to_turns<int64_t>( calendar::turn - calendar::turn_zero );
    e.seq = next_snapshot_seq();
    std::lock_guard<std::mutex> lk( queue_mutex );
    if( queue.size() >= max_events ) {
        // Nobody is draining (or not fast enough): lose a sound or anim before a message, damage or death.
        const auto victim = std::find_if( queue.begin(), queue.end(), low_priority );
        queue.erase( victim != queue.end() ? victim : queue.begin() );
        dropped++;
    }
    queue.push_back( std::move( e ) );
}

// Cut to max_event_text bytes without splitting a UTF-8 sequence.
std::string bounded_text( std::string s )
{
    if( s.size() > max_event_text ) {
        size_t n = max_event_text;
        while( n > 0 && ( static_cast<unsigned char>( s[n] ) & 0xC0 ) == 0x80 ) {
            n--;
        }
        s.resize( n );
    }
    return s;
}

bool player_sees( const Creature &c )
{
    return c.is_avatar() || get_avatar().sees( get_map(), c );
}

bool player_sees( const tripoint_bub_ms &p )
{
    return get_avatar().sees( get_map(), p );
}

ui_point abs_of( const tripoint_bub_ms &p )
{
    return to_ui_point( get_map().get_abs( p ) );
}

uint32_t item_index( const std::string &itype )
{
    return itype == "null" ? 0 : intern_id( id_kind::itype, itype );
}
} // namespace

void set_events_enabled( bool on )
{
    events_on.store( on );
}

size_t drain_events( std::vector<event> &out )
{
    std::lock_guard<std::mutex> lk( queue_mutex );
    out.insert( out.end(), std::make_move_iterator( queue.begin() ), std::make_move_iterator( queue.end() ) );
    queue.clear();
    return std::exchange( dropped, 0 );
}

void reset_events()
{
    std::lock_guard<std::mutex> lk( queue_mutex );
    queue.clear();
    dropped = 0;
}

void on_sound( const tripoint_bub_ms &p, int volume, int heard, int category, const std::string &description,
               const std::string &id, const std::string &variant )
{
    event e;
    e.kind = event_kind::sound;
    e.pos = abs_of( p );
    e.amount = volume;
    e.heard = heard;
    e.detail = static_cast<uint32_t>( category );
    e.type = intern_id( id_kind::sound, variant.empty() || id.empty() ? id : id + "/" + variant );
    e.text = bounded_text( description );
    push( std::move( e ) );
}

void on_message( const std::string &text, int type )
{
    event e;
    e.kind = event_kind::message;
    e.detail = static_cast<uint32_t>( type );
    e.text = bounded_text( remove_color_tags( text ) );
    push( std::move( e ) );
}

void on_attack( const Creature &attacker, const Creature &target, const std::string &weapon, bool hit )
{
    if( !player_sees( attacker ) && !player_sees( target ) ) {
        return;
    }
    event e;
    e.kind = event_kind::attack;
    e.uid = creature_uid( attacker );
    e.other = creature_uid( target );
    e.pos = to_ui_point( target.pos_abs() );
    e.type = weapon.empty() ? 0 : item_index( weapon );
    e.detail = hit ? 1 : 0;
    push( std::move( e ) );
}

void on_damage( const Creature &target, const std::string &body_part, int amount,
                const dealt_damage_instance &dealt )
{
    if( !player_sees( target ) ) {
        return;
    }
    // The damage type that did the most.
    std::string worst;
    int most = 0;
    for( const auto &[type, dam] : dealt.dealt_dams ) {
        if( dam > most ) {
            most = dam;
            worst = type.str();
        }
    }
    event e;
    e.kind = event_kind::damage;
    e.uid = creature_uid( target );
    e.pos = to_ui_point( target.pos_abs() );
    e.amount = amount;
    e.type = intern_id( id_kind::damage_type, worst );
    e.detail = intern_id( id_kind::body_part, body_part );
    push( std::move( e ) );
}

void on_projectile( const map &here, const Creature *shooter, const std::vector<tripoint_bub_ms> &path,
                    const tripoint_bub_ms &end, const Creature *hit, const std::string &thrown )
{
    if( &here != &get_map() ) {
        return;   // a projectile on another loaded map (overlapping maps aren't handled by CDDA either)
    }
    event e;
    e.kind = event_kind::projectile;
    bool seen = ( shooter != nullptr && player_sees( *shooter ) ) || ( hit != nullptr && player_sees( *hit ) );
    for( const tripoint_bub_ms &p : path ) {
        if( e.path.size() >= max_projectile_path ) {
            break;
        }
        seen = seen || player_sees( p );
        e.path.push_back( abs_of( p ) );
        if( p == end ) {
            break;
        }
    }
    if( !seen ) {
        return;
    }
    e.uid = shooter != nullptr ? creature_uid( *shooter ) : 0;
    e.other = hit != nullptr ? creature_uid( *hit ) : 0;
    e.pos = abs_of( end );
    e.type = thrown.empty() ? 0 : item_index( thrown );
    push( std::move( e ) );
}

namespace
{
thread_local std::string explosion_item;
} // namespace

explosion_item_scope::explosion_item_scope( const std::string &itype ) : prev( explosion_item )
{
    explosion_item = itype;
}

explosion_item_scope::~explosion_item_scope()
{
    explosion_item = prev;
}

void on_explosion( const tripoint_bub_ms &centre, int radius, const std::vector<tripoint_bub_ms> &area,
                   const std::string &kind )
{
    event e;
    e.kind = event_kind::explosion;
    bool seen = player_sees( centre );
    for( const tripoint_bub_ms &p : area ) {
        if( e.path.size() >= max_explosion_area ) {
            break;
        }
        seen = seen || player_sees( p );
        radius = std::max( radius, rl_dist( centre, p ) );
        e.path.push_back( abs_of( p ) );
    }
    if( !seen ) {
        return;
    }
    e.pos = abs_of( centre );
    e.amount = radius;
    e.type = intern_id( id_kind::explosion, kind );
    e.text = kind;
    if( !explosion_item.empty() ) {
        e.detail = intern_id( id_kind::itype, explosion_item );
    }
    push( std::move( e ) );
}

void on_death( const Creature &c )
{
    if( !player_sees( c ) ) {
        return;
    }
    event e;
    e.kind = event_kind::death;
    e.uid = creature_uid( c );
    e.pos = to_ui_point( c.pos_abs() );
    if( const monster *m = c.as_monster() ) {
        e.type = intern_id( id_kind::mtype, m->type->id.str() );
    }
    push( std::move( e ) );
}

void on_activity( const Character &ch, const std::string &activity )
{
    if( !player_sees( ch ) ) {
        return;
    }
    event e;
    e.kind = event_kind::anim;
    e.uid = creature_uid( ch );
    e.pos = to_ui_point( ch.pos_abs() );
    e.type = intern_id( id_kind::activity, activity );
    push( std::move( e ) );
}

void on_turn_end()
{
    event e;
    e.kind = event_kind::turn_end;
    push( std::move( e ) );
}

} // namespace fp3d
