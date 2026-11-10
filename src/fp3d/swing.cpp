#include "fp3d/swing.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>
#include <string>

#include "bodypart.h"
#include "character.h"
#include "coordinates.h"
#include "creature.h"
#include "fp3d/bridge.h"
#include "fp3d/hands.h"
#include "fp3d/points.h"
#include "fp3d/shot.h"
#include "fp3d/snapshot.h"
#include "line.h"
#include "monster.h"
#include "weakpoint.h"

namespace fp3d
{
namespace
{
// The client's part for the swing under way: the creature struck and what it aimed at.
struct swing_plan {
    uint64_t uid = 0;
    std::string part;
};

int aimed_depth = 0;            // aimed_strike guards alive
bool aimed_asked = false;       // this aimed strike asked already (one request per strike)
int swing_depth = 0;            // swing_scope nesting (a technique's extra hits swing inside the first)
std::optional<swing_plan> plan;
std::optional<double> goodhit;  // set by swing_rolled for the current swing

std::atomic<uint64_t> n_asked{ 0 };
std::atomic<uint64_t> n_accepted{ 0 };
std::atomic<uint64_t> n_refused{ 0 };
std::atomic<uint64_t> n_parts{ 0 };

// The plan's part for `target`, while the current swing has rolled a hit and the plan names that creature.
const std::string *planned( const Creature &target )
{
    if( swing_depth == 0 || !plan || !goodhit || plan->part.empty() || creature_uid( target ) != plan->uid ) {
        return nullptr;
    }
    return &plan->part;
}
} // namespace

aimed_strike::aimed_strike()
{
    if( aimed_depth++ == 0 ) {
        aimed_asked = false;
    }
}

aimed_strike::~aimed_strike()
{
    if( --aimed_depth == 0 ) {
        aimed_asked = false;
    }
}

swing_scope::swing_scope( const Character &attacker, const Creature &target )
{
    if( swing_depth++ > 0 ) {
        return;     // a swing inside a swing (a technique's extra hit) keeps the outer swing's answer
    }
    plan.reset();
    goodhit.reset();
    if( aimed_depth == 0 || aimed_asked || !active() || !attacker.is_avatar() ) {
        return;
    }
    aimed_asked = true;
    const tripoint_abs_ms from = attacker.pos_abs();
    const tripoint_abs_ms at = target.pos_abs();
    ui_request req;
    req.kind = request_kind::swing;
    req.origin = to_ui_point( from );
    req.cursor = to_ui_point( at );
    req.range = rl_dist( from, at );
    req.mode = offhand_strike() ? "off" : "main";
    n_asked++;
    ui_answer a;
    try {
        a = ask( req );
    } catch( ... ) {
        swing_depth--;  // the destructor won't run for a constructor that throws
        throw;
    }
    const uint64_t uid = creature_uid( target );
    if( !a.cancel && a.shot && uid != 0 && a.shot->hit_uid == uid && !a.shot->part.empty() &&
        a.shot->end.x == req.cursor.x && a.shot->end.y == req.cursor.y && a.shot->end.z == req.cursor.z ) {
        plan = swing_plan{ uid, a.shot->part };
        n_accepted++;
    } else {
        n_refused++;
    }
}

swing_scope::~swing_scope()
{
    if( --swing_depth == 0 ) {
        plan.reset();
        goodhit.reset();
    }
}

double swing_goodhit( int hit_spread, bool critical )
{
    if( critical ) {
        return 0.0;
    }
    return 1.0 / ( 1.0 + std::max( hit_spread, 0 ) / 10.0 );
}

void swing_rolled( int hit_spread, bool critical )
{
    if( swing_depth == 0 || !plan ) {
        return;
    }
    if( hit_spread < 0 ) {
        goodhit.reset();    // a miss lands nowhere
        return;
    }
    goodhit = swing_goodhit( hit_spread, critical );
}

std::optional<bodypart_id> swing_body_part( const Creature &target )
{
    const std::string *part = planned( target );
    if( part == nullptr ) {
        return std::nullopt;
    }
    std::optional<bodypart_id> id = client_body_part( target, *part, *goodhit );
    if( id ) {
        n_parts++;
    }
    return id;
}

const weakpoint *swing_weakpoint( const monster &target, const weakpoint_attack &attack )
{
    if( attack.source == nullptr || !attack.source->is_avatar() ) {
        return nullptr;
    }
    const std::string *part = planned( target );
    if( part == nullptr ) {
        return nullptr;
    }
    const weakpoint *w = client_weakpoint( target, attack, *part, *goodhit );
    if( w != nullptr ) {
        n_parts++;
    }
    return w;
}

swing_counts get_swing_counts()
{
    swing_counts c;
    c.asked = n_asked.load();
    c.accepted = n_accepted.load();
    c.refused = n_refused.load();
    c.parts = n_parts.load();
    return c;
}

} // namespace fp3d
