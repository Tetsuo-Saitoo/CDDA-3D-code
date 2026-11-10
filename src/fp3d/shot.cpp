#include "fp3d/shot.h"

#include <optional>

#include "creature.h"

namespace fp3d
{
namespace
{
std::optional<shot_plan> plan;
} // namespace

void set_shot_plan( const shot_plan &p )
{
    plan = p;
}

void clear_shot_plan()
{
    plan.reset();
}

const shot_plan *shot_plan_for( const Creature *shooter )
{
    return plan && shooter != nullptr && shooter->is_avatar() ? &*plan : nullptr;
}

} // namespace fp3d
