#pragma once
#ifndef CATA_SRC_FP3D_EVENTS_H
#define CATA_SRC_FP3D_EVENTS_H

// FP3D block B4: things that happened, for sound, one-shot animations, effects and the message feed. See
// contracts/events.md. Hooks in CDDA's code emit on the sim thread, only while events are enabled and only what
// the player perceives; the client drains the queue. Each event carries the seq of the snapshot that shows its
// outcome, so the client plays it when it shows that snapshot.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "coordinates.h"
#include "fp3d/bridge.h"

class Character;
class Creature;
class map;
struct dealt_damage_instance;

namespace fp3d
{

enum class event_kind : uint8_t { sound, message, attack, damage, projectile, explosion, death, anim, turn_end };

struct event {
    event_kind kind = event_kind::turn_end;
    int64_t turn = 0;              // game turn it happened in
    uint64_t seq = 0;              // the snapshot that shows its outcome
    uint64_t uid = 0;              // actor: attacker, shooter, damaged / dying / busy creature (0 none)
    uint64_t other = 0;            // attack target, creature a projectile hit (0 none)
    ui_point pos;                  // sound source, attack target, explosion centre, death place (absolute)
    int32_t amount = 0;            // sound volume, damage dealt, explosion radius
    int32_t heard = 0;             // sound: volume at the player's ears
    uint32_t type = 0;             // session table index, see contracts/events.md
    uint32_t detail = 0;           // sound category, message type, body part index, attack hit (1) / miss (0)
    std::string text;              // message text without colour tags, sound description
    std::vector<ui_point> path;    // projectile path from the shooter (cap 256), explosion area (cap 1024)
};

constexpr size_t max_events = 4096;        // queued; overflow drops the oldest sound / anim / turn_end first
constexpr size_t max_event_text = 1024;    // bytes
constexpr size_t max_projectile_path = 256;
constexpr size_t max_explosion_area = 1024;

extern std::atomic<bool> events_on;
inline bool events_enabled()
{
    return events_on.load( std::memory_order_relaxed );
}
void set_events_enabled( bool on );
// Client side: appends all queued events to `out`. Returns how many were dropped since the last drain.
size_t drain_events( std::vector<event> &out );
void reset_events();

// Hooks, sim thread. Callers check events_enabled() first so a disabled bus costs one load.
void on_sound( const tripoint_bub_ms &p, int volume, int heard, int category, const std::string &description,
               const std::string &id, const std::string &variant );
void on_message( const std::string &text, int type );
void on_attack( const Creature &attacker, const Creature &target, const std::string &weapon, bool hit );
void on_damage( const Creature &target, const std::string &body_part, int amount,
                const dealt_damage_instance &dealt );
// `path` from the shooter's square; the projectile got as far as `end`.
void on_projectile( const map &here, const Creature *shooter, const std::vector<tripoint_bub_ms> &path,
                    const tripoint_bub_ms &end, const Creature *hit, const std::string &thrown );
// Radius 0 = computed from `area`.
void on_explosion( const tripoint_bub_ms &centre, int radius, const std::vector<tripoint_bub_ms> &area,
                   const std::string &kind );
// The item behind explosions started in this scope (a grenade, C4, a rocket's ammo); explosion events carry
// its itype index in `detail` (0 = unknown, e.g. a vehicle or a monster).
class explosion_item_scope
{
    public:
        explicit explosion_item_scope( const std::string &itype );
        ~explosion_item_scope();
        explosion_item_scope( const explosion_item_scope & ) = delete;
        explosion_item_scope &operator=( const explosion_item_scope & ) = delete;
    private:
        std::string prev;
};
void on_death( const Creature &c );
void on_activity( const Character &ch, const std::string &activity );
void on_turn_end();

} // namespace fp3d

#endif // CATA_SRC_FP3D_EVENTS_H
