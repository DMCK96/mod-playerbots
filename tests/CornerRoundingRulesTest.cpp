/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CornerRoundingRules.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <set>

using namespace CornerRoundingRules;

namespace
{
Point P(float x, float y, float z = 0.0f)
{
    return Point{x, y, z};
}

float Dist(Point const& a, Point const& b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

bool Finite(std::vector<Point> const& path)
{
    return std::all_of(path.begin(), path.end(), [](Point const& p)
                       { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); });
}

float MaxTurn(std::vector<Point> const& path)
{
    float worst = 0.0f;
    for (size_t i = 1; i + 1 < path.size(); ++i)
        worst = std::max(worst, TurnRadians(path[i - 1], path[i], path[i + 1]));
    return worst;
}

float Length(std::vector<Point> const& path)
{
    float total = 0.0f;
    for (size_t i = 1; i < path.size(); ++i)
        total += Dist(path[i - 1], path[i]);
    return total;
}

bool Same(std::vector<Point> const& a, std::vector<Point> const& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z)
            return false;
    return true;
}

SegmentCheck const AlwaysClear = [](Point const&, Point const&) { return true; };
float const Pi = 3.14159265f;
}  // namespace

TEST(CornerRounding, RadiusIsStableInRangeAndVaries)
{
    std::set<float> seen;
    for (uint32_t seed = 0; seed < 100; ++seed)
    {
        float const radius = RadiusYards(seed);
        EXPECT_GE(radius, MinRadiusYards);
        EXPECT_LE(radius, MaxRadiusYards);
        EXPECT_EQ(radius, RadiusYards(seed));
        seen.insert(radius);
    }
    EXPECT_GE(seen.size(), 20u);
}

TEST(CornerRounding, TurnIsTheDeflectionInThePlaneAndIgnoresHeight)
{
    EXPECT_NEAR(TurnRadians(P(0, 0), P(10, 0), P(20, 0)), 0.0f, 1e-4f);
    EXPECT_NEAR(TurnRadians(P(0, 0), P(10, 0), P(10, 10)), Pi / 2.0f, 1e-4f);
    EXPECT_NEAR(TurnRadians(P(0, 0), P(10, 0), P(0, 0)), Pi, 1e-4f);
    EXPECT_NEAR(TurnRadians(P(0, 0, 0), P(10, 0, 5), P(10, 10, 0)), Pi / 2.0f, 1e-4f);
    EXPECT_EQ(TurnRadians(P(0, 0), P(0, 0), P(10, 0)), 0.0f);
}

TEST(CornerRounding, TangentFollowsTheRadiusUntilASegmentCapsIt)
{
    EXPECT_NEAR(TangentYards(2.0f, Pi / 2.0f, 20.0f, 20.0f), 2.0f, 1e-3f);
    EXPECT_NEAR(TangentYards(2.0f, Pi / 2.0f, 4.0f, 20.0f), 1.8f, 1e-3f);
    EXPECT_NEAR(TangentYards(2.0f, Pi / 2.0f, 20.0f, 3.0f), 1.35f, 1e-3f);
    EXPECT_NEAR(TangentYards(2.0f, Pi / 2.0f, 20.0f, 6.0f), 2.0f, 1e-3f);
}

TEST(CornerRounding, TangentYardsIsCappedAtAHairpin)
{
    EXPECT_NEAR(TangentYards(2.0f, Pi, 20.0f, 20.0f), 9.0f, 1e-3f);
    EXPECT_NEAR(TangentYards(3.0f, Pi - 0.001f, 10.0f, 30.0f), 4.5f, 1e-3f);
    EXPECT_GE(TangentYards(2.0f, 3.1415927f, 10.0f, 10.0f), 0.0f);
}

TEST(CornerRounding, SharperTurnsGetMoreArcPoints)
{
    EXPECT_EQ(ArcPointCount(0.3f), 3u);
    EXPECT_EQ(ArcPointCount(0.78f), 3u);
    EXPECT_EQ(ArcPointCount(1.0f), 4u);
    EXPECT_EQ(ArcPointCount(1.5f), 4u);
    EXPECT_EQ(ArcPointCount(1.6f), 5u);
    EXPECT_EQ(ArcPointCount(Pi), 5u);
}

TEST(CornerRounding, AGentleTurnIsNotRounded)
{
    auto const out = RoundCorner(P(0, 0), P(10, 0), P(20, 1), 2.0f);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].x, 10.0f);
    EXPECT_EQ(out[0].y, 0.0f);
}

TEST(CornerRounding, ASixtyDegreeCornerBecomesEntryFourArcPointsAndExit)
{
    Point const corner = P(10, 0);
    auto const out = RoundCorner(P(0, 0), corner, P(15, 8.660254f), 2.0f);
    ASSERT_EQ(out.size(), 6u);
    EXPECT_NEAR(out.front().x, 8.8453f, 1e-3f);
    EXPECT_NEAR(out.front().y, 0.0f, 1e-3f);
    EXPECT_NEAR(out.back().x, 10.5774f, 1e-3f);
    EXPECT_NEAR(out.back().y, 1.0f, 1e-3f);
    for (size_t i = 1; i + 1 < out.size(); ++i)
    {
        EXPECT_LT(Dist(out[i], corner), 1.1547f);
        EXPECT_GT(Dist(out[i], corner), 0.01f);
    }
    EXPECT_LT(MaxTurn(out), 0.3f);
}

TEST(CornerRounding, HairpinStaysFiniteAndInsideTheSegments)
{
    Point const corner = P(10, 0);
    auto const out = RoundCorner(P(0, 0), corner, P(0, 0.5f), 2.0f);
    ASSERT_EQ(out.size(), 7u);
    ASSERT_TRUE(Finite(out));
    EXPECT_NEAR(Dist(out.front(), corner), 4.5f, 0.05f);
    for (Point const& p : out)
    {
        EXPECT_GE(p.x, -0.01f);
        EXPECT_LE(p.x, 10.01f);
    }
}

TEST(CornerRounding, RepeatedPointsAreLeftAlone)
{
    std::vector<Point> const path = {P(0, 0), P(10, 0), P(10, 0), P(10, 10)};
    Shaped const shaped = Shape(path, 2.0f, AlwaysClear);
    EXPECT_TRUE(Finite(shaped.path));
    EXPECT_TRUE(Same(shaped.path, path));
    EXPECT_EQ(shaped.rounded, 0u);
    EXPECT_EQ(shaped.sharp, 0u);
}

TEST(CornerRounding, ShortAndStraightPathsAreReturnedUnchanged)
{
    EXPECT_TRUE(Shape({}, 2.0f, AlwaysClear).path.empty());
    std::vector<Point> const two = {P(0, 0), P(5, 5)};
    EXPECT_TRUE(Same(Shape(two, 2.0f, AlwaysClear).path, two));
    std::vector<Point> const straight = {P(0, 0), P(10, 0), P(20, 0)};
    Shaped const shaped = Shape(straight, 2.0f, AlwaysClear);
    EXPECT_TRUE(Same(shaped.path, straight));
    EXPECT_EQ(shaped.rounded, 0u);
    EXPECT_EQ(shaped.sharp, 0u);
}

TEST(CornerRounding, ShapeKeepsFirstAndLastPointsAndCountsTheCorner)
{
    std::vector<Point> const path = {P(0, 0, 1), P(20, 0, 3), P(30, 17.320508f, 6)};
    Shaped const shaped = Shape(path, 2.0f, AlwaysClear);
    ASSERT_EQ(shaped.path.size(), 8u);
    EXPECT_EQ(shaped.path.front().x, 0.0f);
    EXPECT_EQ(shaped.path.front().z, 1.0f);
    EXPECT_EQ(shaped.path.back().x, 30.0f);
    EXPECT_EQ(shaped.path.back().z, 6.0f);
    EXPECT_EQ(shaped.rounded, 1u);
    EXPECT_EQ(shaped.sharp, 0u);
    for (Point const& p : shaped.path)
    {
        EXPECT_GE(p.z, 1.0f - 1e-3f);
        EXPECT_LE(p.z, 6.0f + 1e-3f);
    }
}

TEST(CornerRounding, RejectingEveryChordKeepsThePathSharp)
{
    std::vector<Point> const path = {P(0, 0), P(20, 0), P(30, 17.320508f)};
    Shaped const shaped = Shape(path, 2.0f, [](Point const&, Point const&) { return false; });
    EXPECT_TRUE(Same(shaped.path, path));
    EXPECT_EQ(shaped.rounded, 0u);
    EXPECT_EQ(shaped.sharp, 1u);
}

TEST(CornerRounding, RadiusHalvesBeforeGivingUp)
{
    Point const corner = P(20, 0);
    std::vector<Point> const path = {P(0, 0), corner, P(30, 17.320508f)};
    SegmentCheck const nearCorner = [corner](Point const& a, Point const& b)
    { return Dist(a, corner) <= 0.6f && Dist(b, corner) <= 0.6f; };
    Shaped const shaped = Shape(path, 2.0f, nearCorner);
    ASSERT_GT(shaped.path.size(), path.size());
    EXPECT_EQ(shaped.rounded, 1u);
    EXPECT_EQ(shaped.sharp, 0u);
    EXPECT_NEAR(Dist(shaped.path[1], corner), 0.5774f, 2e-3f);
}

TEST(CornerRounding, AdjacentCornersOnShortSegmentsDoNotOverlap)
{
    std::vector<Point> const path = {P(0, 0), P(4, 0), P(4, 4), P(0, 4)};
    Shaped const shaped = Shape(path, 4.0f, AlwaysClear);
    EXPECT_EQ(shaped.rounded, 2u);
    EXPECT_TRUE(Finite(shaped.path));
    EXPECT_LT(MaxTurn(shaped.path), 0.6f);
    EXPECT_LT(Length(shaped.path), Length(path));
    EXPECT_GT(Length(shaped.path), 0.8f * Length(path));
}
