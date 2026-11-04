/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CornerRoundingRules.h"
#include "HumanMovement.h"
#include "Map.h"
#include "MoveSplinePathShaper.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "WorldSession.h"

#include <cmath>
#include <vector>

namespace
{
using CornerRoundingRules::Point;

constexpr float MeshSnapHorizontal = 1.0f;
constexpr float MeshSnapVertical = 2.5f;
constexpr float MaxArcHeightError = 1.2f;

bool ShapesPathOf(Unit* unit)
{
    if (!sPlayerbotAIConfig.coaCornerRounding || !unit || !unit->IsPlayer())
        return false;
    Player* player = unit->ToPlayer();
    WorldSession* session = player->GetSession();
    if (!session || !session->IsBot())
        return false;
    return !player->IsFlying() && !player->isSwimming() && !player->GetVehicle() &&
           !player->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
}

G3D::Vector3 ToVector(Point const& p)
{
    return G3D::Vector3(p.x, p.y, p.z);
}

void SettleOnMesh(Map* map, std::vector<Point> const& original, std::vector<Point>& shaped)
{
    size_t next = 0;
    for (size_t i = 1; i + 1 < shaped.size(); ++i)
    {
        Point& p = shaped[i];
        if (next < original.size() && p.x == original[next].x && p.y == original[next].y && p.z == original[next].z)
        {
            ++next;
            continue;
        }
        G3D::Vector3 onMesh;
        if (HumanMovement::NearestMeshPoint(map, p.x, p.y, p.z, MeshSnapHorizontal, MeshSnapVertical, onMesh) &&
            std::fabs(onMesh.z - p.z) <= MaxArcHeightError)
            p.z = onMesh.z;
    }
}

void ShapeBotPath(Unit* unit, Movement::PointsArray& path)
{
    if (!ShapesPathOf(unit))
        return;
    Map* map = unit->GetMap();
    if (!map)
        return;

    HumanMovement::CostTimer timer(HumanMovement::CostSlotRound);
    std::vector<Point> original;
    original.reserve(path.size());
    for (G3D::Vector3 const& v : path)
        original.push_back(Point{v.x, v.y, v.z});

    CornerRoundingRules::Shaped shaped = CornerRoundingRules::Shape(
        original, CornerRoundingRules::RadiusYards(unit->GetGUID().GetCounter()),
        [map](Point const& a, Point const& b) { return HumanMovement::MeshChordClear(map, ToVector(a), ToVector(b)); });
    HumanMovement::NoteCornersRounded(shaped.rounded, shaped.sharp);
    if (!shaped.rounded)
        return;

    SettleOnMesh(map, original, shaped.path);
    path.clear();
    path.reserve(shaped.path.size());
    for (Point const& p : shaped.path)
        path.push_back(ToVector(p));
}
}  // namespace

void AddSC_coa_corner_rounding()
{
    Movement::SetPathShaper(&ShapeBotPath);
}
