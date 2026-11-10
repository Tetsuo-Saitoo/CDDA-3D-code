#include "fp3d/shot.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>
#include <utility>

#include "avatar.h"
#include "ballistics.h"
#include "bodypart.h"
#include "character.h"
#include "creature.h"
#include "creature_tracker.h"
#include "dispersion.h"
#include "fp3d/bridge.h"
#include "fp3d/points.h"
#include "fp3d/snapshot.h"
#include "game.h"
#include "item.h"
#include "item_location.h"
#include "line.h"
#include "map.h"
#include "monster.h"
#include "mtype.h"
#include "projectile.h"
#include "ranged.h"
#include "rng.h"
#include "units.h"
#include "weakpoint.h"

static const ammo_effect_str_id ammo_effect_WIDE( "WIDE" );

static const json_character_flag json_flag_HARDTOHIT( "HARDTOHIT" );

namespace fp3d
{
namespace
{
std::optional<shot_plan> plan;
bool firing = false;                    // in the avatar's fire_gun with its wielded gun
bool first_used = false;                // this trigger pull's first projectile has been decided
int asks_this_pull = 0;                 // `shot` requests sent in this trigger pull
int depth = 0;                          // projectile_attack nesting
std::optional<shot_outcome> current;    // the outermost projectile's outcome

std::atomic<uint64_t> n_asked{ 0 };
std::atomic<uint64_t> n_accepted{ 0 };
std::atomic<uint64_t> n_refused{ 0 };
std::atomic<uint64_t> n_capped{ 0 };
std::atomic<int> asks_cap{ default_asks_per_pull };
std::atomic<uint64_t> n_planned{ 0 };
std::atomic<uint64_t> n_parts{ 0 };
std::atomic<uint64_t> n_spared{ 0 };

constexpr double fire_line_tiles = 1.0;     // a FIRE answer's outcome: off the line to the aimed square
constexpr double shot_line_tiles = 1.5;     // a shot answer: off CDDA's deviated line
constexpr size_t max_part_bytes = 64;

const shot_plan *plan_for( const Creature *shooter )
{
    return plan && firing && shooter != nullptr && shooter->is_avatar() ? &*plan : nullptr;
}

// Where the client says one projectile went, after the checks and before CDDA's roll is applied.
struct claim {
    uint64_t hit_uid = 0;
    const Creature *hit = nullptr;      // the creature of hit_uid, alive and seen now
    tripoint_bub_ms end;
    double quality = 0.99;
    std::string part;
};

// `p` in front of `source` along the unit direction (ux, uy) and at most `max_off` squares off that line.
bool near_line( const tripoint_bub_ms &source, const tripoint_bub_ms &p, double ux, double uy, double max_off )
{
    const double px = p.x() - source.x();
    const double py = p.y() - source.y();
    return px * ux + py * uy > 0.0 && std::abs( px * uy - py * ux ) <= max_off;
}

// A hit must name a creature the request offered (the cursor creature or a hostile candidate) that is alive, seen
// and, unless it is the cursor creature, still hostile now; it stops at that creature's square. A miss's
// `end_abs` is only checked (CDDA keeps the path). Either square must be on the shooter's level, in the bubble,
// within the gun's range and near the line.
std::optional<claim> check_claim( map &here, const Character &you, const shot_plan &p,
                                  const tripoint_bub_ms &source, double ux, double uy, double max_off,
                                  uint64_t hit_uid, const tripoint_abs_ms &end_abs, double quality,
                                  const std::string &part )
{
    if( !std::isfinite( quality ) ) {
        return std::nullopt;
    }
    claim c;
    c.quality = std::clamp( quality, 0.0, 0.99 );
    tripoint_bub_ms end;
    if( hit_uid != 0 ) {
        if( std::find( p.targets.begin(), p.targets.end(), hit_uid ) == p.targets.end() ) {
            return std::nullopt;
        }
        const Creature *hit = g->get_creature_if( [hit_uid]( const Creature & cr ) {
            return !cr.is_avatar() && creature_uid( cr ) == hit_uid;
        } );
        if( hit == nullptr || hit->is_dead_state() || !you.sees( here, *hit ) ||
            ( hit_uid != p.cursor_uid && hit->attitude_to( you ) != Creature::Attitude::HOSTILE ) ) {
            return std::nullopt;
        }
        end = hit->pos_bub( here );
        c.hit_uid = hit_uid;
        c.hit = hit;
        c.part = part.size() <= max_part_bytes ? part : std::string();
    } else {
        end = here.get_bub( end_abs );
    }
    if( !here.inbounds( end ) || end.z() != source.z() || end == source || rl_dist( source, end ) > p.range ||
        !near_line( source, end, ux, uy, max_off ) ) {
        return std::nullopt;
    }
    c.end = end;
    return c;
}

// CDDA's line from `source` along the unit direction (vx, vy): how far `at` lies along it and how far off it.
struct line_offset {
    double along = 0.0;
    double off = 0.0;
};
line_offset offset_from( const tripoint_bub_ms &source, const tripoint_bub_ms &at, double vx, double vy )
{
    const double px = at.x() - source.x();
    const double py = at.y() - source.y();
    return { px * vx + py * vy, std::abs( px * vy - py * vx ) };
}

// The roll projectile_attack makes for the aimed creature, made for `c` at `at` from what is known of CDDA's
// projectile, which flies the line (vx, vy): the square's distance off that line combined with the error's vertical
// part `vert_arcmin` over the distance along it (shot answers: CDDA's deviated line), plus `unknown_arcmin` over that
// distance in the worst direction (a FIRE answer: the aim line, the error's direction unknown), against its size
// (WIDE ammo takes a monster's plain size); the worse of two rolls for a HARDTOHIT character. 1 = a miss. Never
// better than the creature's square standing exactly where the projectile went, nor than CDDA's roll for an
// unintended hit (`aimed_missed_by`: CDDA's roll for the aimed square).
double own_missed_by( const Creature &c, const tripoint_bub_ms &source, const tripoint_bub_ms &at, double vx,
                      double vy, double vert_arcmin, double unknown_arcmin, const projectile &proj,
                      const dispersion_sources &dispersion, double aimed_missed_by )
{
    const monster *mon = c.as_monster();
    const double size = mon != nullptr && proj.proj_effects.count( ammo_effect_WIDE ) ?
                        occupied_tile_fraction( mon->get_size() ) : c.ranged_target_size();
    const line_offset lo = offset_from( source, at, vx, vy );
    const double along_line = std::max( lo.along, 0.0 );
    double missed_by = 1.0;
    if( size > 0.0 && std::isfinite( vert_arcmin ) && std::isfinite( unknown_arcmin ) ) {
        const double vert = iso_tangent( along_line, units::from_arcmin( std::abs( vert_arcmin ) ) );
        const double unknown = iso_tangent( along_line, units::from_arcmin( std::abs( unknown_arcmin ) ) );
        missed_by = std::min( 1.0, ( std::hypot( lo.off, vert ) + unknown ) / size );
    }
    const Character *ch = c.as_character();
    if( ch != nullptr && ch->has_flag( json_flag_HARDTOHIT ) ) {
        missed_by = std::max( missed_by, projectile_attack_roll( dispersion, rl_dist( source, at ), size ).missed_by );
    }
    // never better than CDDA's roll for a creature the projectile meets on the way (ballistics.cpp: unintended hit)
    const double cdda_size = c.ranged_target_size();
    const double unintended = cdda_size > 0.0 && std::isfinite( aimed_missed_by ) ?
                              std::max( rng_float( 0.1, 1.5 - std::clamp( aimed_missed_by, 0.0, 1.0 ) ) / cdda_size,
                                        0.4 ) : 1.0;
    missed_by = std::max( missed_by, unintended );
    return std::isfinite( missed_by ) ? std::min( missed_by, 1.0 ) : 1.0;
}

// A claim that passed the checks. A hit counts only if the creature's own roll `own` hits, goodhit = max(own,
// quality); when it misses, CDDA keeps the path and that creature, its roll made, isn't rolled again. A miss leaves
// the path to CDDA.
shot_outcome outcome_of( const claim &c, double own )
{
    shot_outcome o;
    if( c.hit_uid == 0 ) {
        return o;
    }
    if( !( own < 1.0 ) ) {
        o.spared_uid = c.hit_uid;
        return o;
    }
    o.end = c.end;
    o.hit_uid = c.hit_uid;
    o.goodhit = std::max( own, c.quality );
    o.part = c.part;
    return o;
}

// The current projectile's client part for a hit on `target` by `source`, else null.
const std::string *planned_part( const Creature &target, const Creature *source )
{
    if( depth != 1 || !current || current->hit_uid == 0 || current->part.empty() || source == nullptr ||
        !source->is_avatar() || creature_uid( target ) != current->hit_uid ) {
        return nullptr;
    }
    return &current->part;
}

// The client's part only weights CDDA's pick: rolled with the part's own chance (0..1) times 1 + 3 * (1 - goodhit)
// (4 for a dead-centre roll, 1 for a graze), at most 1; CDDA's own pick stands otherwise. A part that can't be hit
// never lands.
bool part_lands( double chance, double goodhit )
{
    if( !std::isfinite( chance ) || !( chance > 0.0 ) || !std::isfinite( goodhit ) ) {
        return false;
    }
    const double weight = 1.0 + ( aim_weight_max - 1.0 ) * std::clamp( 1.0 - goodhit, 0.0, 1.0 );
    return rng_float( 0.0, 1.0 ) < std::min( chance * weight, 1.0 );
}
} // namespace

void set_shot_plan( const shot_plan &p )
{
    plan = p;
}

void clear_shot_plan()
{
    plan.reset();
}

bool has_shot_plan()
{
    return plan.has_value();
}

void set_shot_targets( shot_plan &p, const Creature *cursor, const std::vector<tripoint_bub_ms> &squares )
{
    std::vector<uint64_t> out;
    p.cursor_uid = 0;
    if( cursor != nullptr && !cursor->is_avatar() ) {
        p.cursor_uid = creature_uid( *cursor );
        out.push_back( p.cursor_uid );
    }
    creature_tracker &creatures = get_creature_tracker();
    for( const tripoint_bub_ms &t : squares ) {
        if( out.size() >= max_candidates ) {
            break;
        }
        if( const Creature *c = creatures.creature_at( t ) ) {
            if( !c->is_avatar() && c != cursor && c->attitude_to( get_avatar() ) == Creature::Attitude::HOSTILE ) {
                out.push_back( creature_uid( *c ) );
            }
        }
    }
    p.targets = std::move( out );
}

firing_scope::firing_scope( const Character &shooter, const item &gun, const map &here,
                            const tripoint_bub_ms &target )
{
    if( !shooter.is_avatar() ) {
        return;
    }
    avatar_ = true;
    const item_location wielded = shooter.get_wielded_item();
    // the plan's gun at the plan's square, being the wielded gun firing itself (not an attached gunmod)
    firing = plan && plan->gun == &gun && plan->target == here.get_abs( target ) && wielded &&
             wielded.get_item() == &gun && gun.gun_current_mode().target == &gun;
    first_used = false;
    asks_this_pull = 0;
}

firing_scope::~firing_scope()
{
    if( avatar_ ) {
        firing = false;
        first_used = false;
        asks_this_pull = 0;
        plan.reset();
    }
}

projectile_scope::projectile_scope( map &here, const Creature *origin, const tripoint_bub_ms &source,
                                    const tripoint_bub_ms &target, const projectile &proj,
                                    const dispersion_sources &dispersion, const projectile_attack_aim &aim )
{
    if( ++depth != 1 ) {
        return;
    }
    outer_ = true;
    current.reset();
    const shot_plan *p = plan_for( origin );
    if( p == nullptr || source == target || origin->as_character() == nullptr ) {
        return;
    }
    const Character &you = *origin->as_character();
    const bool first = !std::exchange( first_used, true );
    const double dx = target.x() - source.x();
    const double dy = target.y() - source.y();
    const double len = std::hypot( dx, dy );
    if( !( len > 0.0 ) || target.z() != source.z() ) {
        return;     // up or down: CDDA's own path
    }
    const double ux = dx / len;
    const double uy = dy / len;
    const double rolled_arcmin = std::isfinite( aim.dispersion ) ? aim.dispersion : 1800.0;
    const double dispersion_max = dispersion.max();
    // A hit on the aimed square's creature uses CDDA's roll for it; on any other creature that creature's own roll
    // from its square's place off the projectile's line (vx, vy).
    const auto own_roll = [&]( const claim & c, double vx, double vy, double vert_arcmin, double unknown_arcmin ) {
        if( c.end == target ) {
            return aim.missed_by;
        }
        return own_missed_by( *c.hit, source, c.end, vx, vy, vert_arcmin, unknown_arcmin, proj, dispersion,
                              aim.missed_by );
    };
    try {
        std::optional<shot_outcome> out;
        if( p->fly && asks_this_pull >= asks_cap.load() ) {
            n_capped++;     // past the cap: CDDA's own path, nothing asked (its coin-flip side for a wild miss)
        } else if( p->fly ) {
            // 30 degrees at most, as the client clamps it, so both sides fly the same line
            const double err = std::isfinite( rolled_arcmin ) ? std::clamp( rolled_arcmin, 0.0, 1800.0 ) : 0.0;
            double angle = rng_float( 0.0, 360.0 );
            if( !( angle >= 0.0 && angle < 360.0 ) ) {
                angle = 0.0;
            }
            ui_request req;
            req.kind = request_kind::shot;
            req.shot_error = static_cast<float>( err );
            req.shot_angle = static_cast<float>( angle );
            req.dispersion = static_cast<float>( std::isfinite( dispersion_max ) ? dispersion_max : 0.0 );
            req.origin = to_ui_point( here.get_abs( source ) );
            req.cursor = to_ui_point( here.get_abs( target ) );
            req.range = p->range;
            std::optional<ui_answer> a;
            if( active() ) {
                n_asked++;
                asks_this_pull++;
                a = ask( req );
            }
            // CDDA's deviated line: the aim line turned by the error's horizontal part, err * sin(angle), to the
            // shooter's right. CDDA's map has x east and y south, so the right of (ux, uy) is (-uy, ux).
            const double side = std::sin( units::to_radians( units::from_degrees( angle ) ) );
            const double h = units::to_radians( units::from_arcmin( err * side ) );
            const double vx = ux * std::cos( h ) - uy * std::sin( h );
            const double vy = uy * std::cos( h ) + ux * std::sin( h );
            side_ = h >= 0.0 ? 1 : -1;
            bool used = false;
            if( a && !a->cancel && a->shot ) {
                const std::optional<claim> c = check_claim( here, you, *p, source, vx, vy, shot_line_tiles,
                                               a->shot->hit_uid, to_abs_ms( a->shot->end ), a->shot->quality,
                                               a->shot->part );
                if( c ) {
                    // the error's vertical part, err * cos(angle), is known: the line itself is the horizontal part
                    const double vert = err * std::cos( units::to_radians( units::from_degrees( angle ) ) );
                    out = outcome_of( *c, c->hit != nullptr ? own_roll( *c, vx, vy, vert, 0.0 ) : 1.0 );
                    used = true;
                    n_accepted++;
                }
            }
            if( a && !used ) {
                n_refused++;     // this projectile takes CDDA's own path
            }
        } else if( first && p->turn == calendar::turn ) {
            const std::optional<claim> c = check_claim( here, you, *p, source, ux, uy, fire_line_tiles, p->hit_uid,
                                           p->end, p->quality, p->part );
            // A hit's creature off the aimed square: the error's direction is unknown, so the worst.
            if( c ) {
                out = outcome_of( *c, c->hit != nullptr ? own_roll( *c, ux, uy, 0.0, rolled_arcmin ) : 1.0 );
                n_planned++;     // passed the checks, as n_accepted (CDDA's roll may still leave it to its own path)
            }
        }
        current = std::move( out );
    } catch( ... ) {
        --depth;
        current.reset();
        throw;
    }
}

projectile_scope::~projectile_scope()
{
    if( --depth == 0 ) {
        current.reset();
    }
}

const shot_outcome *projectile_scope::outcome() const
{
    return outer_ && current ? &*current : nullptr;
}

std::optional<int> projectile_scope::wild_side() const
{
    return outer_ ? side_ : std::nullopt;
}

double outcome_missed_by( const shot_outcome &o, const Creature &critter, double cur, bool at_target,
                          double cdda_missed_by, double spread_add )
{
    const uint64_t uid = creature_uid( critter );
    if( o.hit_uid != 0 && uid == o.hit_uid ) {
        return std::min( 1.0, o.goodhit + spread_add );     // goodhit: the creature's own roll at best
    }
    if( o.spared_uid != 0 && uid == o.spared_uid ) {
        n_spared++;
        return 1.0;     // its own roll for the claim missed; one roll per creature
    }
    if( at_target ) {
        // The aimed square's creature, which the client's bullet didn't hit: CDDA's unintended-hit roll, never
        // better than CDDA's own roll for it (`cur`), so CDDA's miss stays a miss.
        return std::max( cur, std::max( rng_float( 0.1, 1.5 - cdda_missed_by ) / critter.ranged_target_size(), 0.4 ) );
    }
    return cur;
}

const weakpoint *planned_weakpoint( const monster &target, const weakpoint_attack &attack, double goodhit )
{
    const std::string *part = planned_part( target, attack.source );
    if( part == nullptr ) {
        return nullptr;
    }
    for( const weakpoint &w : target.type->weakpoints.weakpoint_list ) {
        if( w.id == *part ) {
            if( part_lands( w.hit_chance( attack ) / 100.0, goodhit ) ) {
                n_parts++;
                return &w;
            }
            return nullptr;
        }
    }
    return nullptr;
}

std::optional<bodypart_id> planned_body_part( const Creature &target, const Creature *source, double goodhit )
{
    const std::string *part = planned_part( target, source );
    if( part == nullptr ) {
        return std::nullopt;
    }
    // Found by name among the loaded body parts, so the client's string never becomes an id itself.
    const body_part_type *found = nullptr;
    for( const body_part_type &bp : body_part_type::get_all() ) {
        if( bp.id.str() == *part ) {
            found = &bp;
            break;
        }
    }
    if( found == nullptr ) {
        return std::nullopt;
    }
    const bodypart_id id = found->id.id();
    if( !target.has_part( id ) ) {
        return std::nullopt;
    }
    double total = 0.0;
    for( const bodypart_id &bp : target.get_all_body_parts() ) {
        total += bp->hit_size;
    }
    if( !( total > 0.0 ) || !part_lands( id->hit_size / total, goodhit ) ) {
        return std::nullopt;
    }
    n_parts++;
    return id;
}

void set_asks_per_pull( int n )
{
    asks_cap = std::clamp( n, 1, 64 );
}

shot_counts get_shot_counts()
{
    shot_counts c;
    c.asked = n_asked.load();
    c.accepted = n_accepted.load();
    c.refused = n_refused.load();
    c.capped = n_capped.load();
    c.planned = n_planned.load();
    c.parts = n_parts.load();
    c.spared = n_spared.load();
    return c;
}

} // namespace fp3d
