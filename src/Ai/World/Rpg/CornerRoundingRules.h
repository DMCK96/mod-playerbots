/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CORNERROUNDINGRULES_H
#define PLAYERBOTS_CORNERROUNDINGRULES_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// Replaces the sharp corners of a walking path with short arcs. Kept free of core headers so the unit tests can pin
// the geometry; the navmesh check is passed in as a SegmentCheck.
namespace CornerRoundingRules
{
struct Point
{
    float x;
    float y;
    float z;
};

using SegmentCheck = std::function<bool(Point const&, Point const&)>;

struct Shaped
{
    std::vector<Point> path;
    std::vector<bool> synthesized;  // per path point: added by an arc, not taken from the input
    size_t rounded = 0;             // corners replaced by an arc
    size_t sharp = 0;    // corners that qualified but no radius passed the check
};

constexpr float MinTurnRadians = 0.2618f;  // 15 degrees
constexpr float MaxTangentShare = 0.45f;   // of each adjacent segment, so neighbouring arcs never meet
constexpr float MinRadiusYards = 1.5f;
constexpr float MaxRadiusYards = 4.0f;
constexpr float MaxTurnForTangent = 3.1f;  // tan(pi / 2) is huge, and negative in float, at a full reversal
constexpr int RadiusAttempts = 3;          // radius, half, quarter
// The client receives the inner points of a linear spline truncated to 0.25 yd, so an arc needs chords well above
// that, and a corner that cannot give them stays sharp.
constexpr float MinChordYards = 1.0f;
constexpr float MinTangentYards = 1.0f;
constexpr float MaxSnapYards = 0.1f;
constexpr float MaxSnapHeightYards = 1.2f;

inline float RadiusYards(uint32_t seed)
{
    seed ^= seed >> 16;
    seed *= 0x45d9f3bu;
    seed ^= seed >> 16;
    return MinRadiusYards + (MaxRadiusYards - MinRadiusYards) * float(seed % 1000u) / 999.0f;
}

inline float Length2d(Point const& a, Point const& b)
{
    return std::hypot(b.x - a.x, b.y - a.y);
}

inline float TurnRadians(Point const& a, Point const& b, Point const& c)
{
    float const ax = b.x - a.x;
    float const ay = b.y - a.y;
    float const bx = c.x - b.x;
    float const by = c.y - b.y;
    float const la = std::hypot(ax, ay);
    float const lb = std::hypot(bx, by);
    if (la < 0.01f || lb < 0.01f)
        return 0.0f;
    float const cosine = (ax * bx + ay * by) / (la * lb);
    return std::acos(std::clamp(cosine, -1.0f, 1.0f));
}

inline float TangentYards(float radius, float turnRadians, float inYards, float outYards)
{
    float tangent = radius * std::tan(std::min(turnRadians, MaxTurnForTangent) * 0.5f);
    tangent = std::min(tangent, MaxTangentShare * inYards);
    tangent = std::min(tangent, MaxTangentShare * outYards);
    return std::max(tangent, 0.0f);
}

inline size_t ArcPointCount(float turnRadians)
{
    if (turnRadians < 0.7854f)
        return 3;
    if (turnRadians < 1.5708f)
        return 4;
    return 5;
}

// A point the mesh query snapped to is the point itself, not a near neighbour: the ray is cast between snapped
// points, so a point that snapped far would otherwise pass for walkable.
inline bool SnapIsClose(Point const& wanted, Point const& snapped)
{
    return Length2d(wanted, snapped) <= MaxSnapYards && std::fabs(wanted.z - snapped.z) <= MaxSnapHeightYards;
}

inline Point Toward(Point const& from, Point const& to, float fraction)
{
    return Point{from.x + (to.x - from.x) * fraction, from.y + (to.y - from.y) * fraction,
                 from.z + (to.z - from.z) * fraction};
}

// The corner as it replaces itself: entry point, arc points, exit point. A corner under MinTurnRadians, or with no
// room for an arc of chords above MinChordYards, comes back alone.
inline std::vector<Point> RoundCorner(Point const& prev, Point const& corner, Point const& next, float radius)
{
    float const turn = TurnRadians(prev, corner, next);
    if (turn < MinTurnRadians)
        return {corner};
    float const inYards = Length2d(prev, corner);
    float const outYards = Length2d(corner, next);
    float const tangent = TangentYards(radius, turn, inYards, outYards);
    if (tangent + 0.001f < MinTangentYards)
        return {corner};

    Point const entry = Toward(corner, prev, tangent / inYards);
    Point const exit = Toward(corner, next, tangent / outYards);
    float const arcYards = (2.0f * tangent + Length2d(entry, exit)) * 0.5f;
    size_t const count = size_t(std::clamp(int(arcYards / MinChordYards) - 1, 1, int(ArcPointCount(turn))));

    std::vector<Point> arc;
    arc.reserve(count + 2);
    arc.push_back(entry);
    for (size_t i = 1; i <= count; ++i)
    {
        float const u = float(i) / float(count + 1);
        float const a = (1.0f - u) * (1.0f - u);
        float const b = 2.0f * u * (1.0f - u);
        float const c = u * u;
        arc.push_back(Point{a * entry.x + b * corner.x + c * exit.x, a * entry.y + b * corner.y + c * exit.y,
                            a * entry.z + b * corner.z + c * exit.z});
    }
    arc.push_back(exit);
    return arc;
}

inline bool ArcIsClear(std::vector<Point> const& arc, SegmentCheck const& check)
{
    for (size_t i = 1; i < arc.size(); ++i)
        if (!check(arc[i - 1], arc[i]))
            return false;
    return true;
}

inline Shaped Shape(std::vector<Point> const& path, float radius, SegmentCheck const& check)
{
    Shaped shaped;
    if (path.size() < 3)
    {
        shaped.path = path;
        shaped.synthesized.assign(path.size(), false);
        return shaped;
    }

    shaped.path.reserve(path.size() * 3);
    shaped.path.push_back(path.front());
    shaped.synthesized.push_back(false);
    for (size_t i = 1; i + 1 < path.size(); ++i)
    {
        Point const& corner = path[i];
        if (TurnRadians(path[i - 1], corner, path[i + 1]) < MinTurnRadians)
        {
            shaped.path.push_back(corner);
            shaped.synthesized.push_back(false);
            continue;
        }

        bool rounded = false;
        float tryRadius = radius;
        for (int attempt = 0; attempt < RadiusAttempts && !rounded; ++attempt, tryRadius *= 0.5f)
        {
            std::vector<Point> const arc = RoundCorner(path[i - 1], corner, path[i + 1], tryRadius);
            if (arc.size() > 1 && ArcIsClear(arc, check))
            {
                shaped.path.insert(shaped.path.end(), arc.begin(), arc.end());
                shaped.synthesized.insert(shaped.synthesized.end(), arc.size(), true);
                rounded = true;
            }
        }
        if (rounded)
        {
            ++shaped.rounded;
        }
        else
        {
            shaped.path.push_back(corner);
            shaped.synthesized.push_back(false);
            ++shaped.sharp;
        }
    }
    shaped.path.push_back(path.back());
    shaped.synthesized.push_back(false);
    return shaped;
}
}  // namespace CornerRoundingRules

#endif
