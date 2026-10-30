/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HumanMovementRules.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace HumanMovementRules;

TEST(HumanMovementRules, KeysAreDistinctAcrossKindsAndSigns)
{
    EXPECT_NE(NpcKey(5), PlaceKey(0, 5));
    EXPECT_NE(PlaceKey(-1, 0), PlaceKey(1, 0));
    EXPECT_NE(PlaceKey(0, -1), PlaceKey(-1, 0));
    EXPECT_EQ(PlaceKey(-37, 12), PlaceKey(-37, 12));
    EXPECT_NE(NpcKey(0), NoTarget);
    EXPECT_NE(PlaceKey(0, 0), NoTarget);
}

TEST(HumanMovementRules, DistanceFactorHalvesAtSixtyYards)
{
    EXPECT_FLOAT_EQ(DistanceFactor(0.0f), 1.0f);
    EXPECT_FLOAT_EQ(DistanceFactor(60.0f), 0.5f);
    EXPECT_FLOAT_EQ(DistanceFactor(-5.0f), 1.0f);
}

TEST(HumanMovementRules, HubsPullUntilFourAndPushPastTwelve)
{
    EXPECT_FLOAT_EQ(OccupancyFactor(TargetKind::Hub, 0), 1.0f);
    EXPECT_FLOAT_EQ(OccupancyFactor(TargetKind::Hub, 2), 1.3f);
    EXPECT_FLOAT_EQ(OccupancyFactor(TargetKind::Hub, 4), 1.6f);
    EXPECT_FLOAT_EQ(OccupancyFactor(TargetKind::Hub, 12), 1.6f);
    EXPECT_FLOAT_EQ(OccupancyFactor(TargetKind::Hub, 16), 0.8f);
}

TEST(HumanMovementRules, SpreadAndPlacesFallSteeplyFromTheFirstBot)
{
    for (TargetKind kind : {TargetKind::Spread, TargetKind::Place})
    {
        EXPECT_FLOAT_EQ(OccupancyFactor(kind, 0), 1.0f);
        EXPECT_FLOAT_EQ(OccupancyFactor(kind, 1), 0.25f);
        EXPECT_FLOAT_EQ(OccupancyFactor(kind, 3), 1.0f / 16.0f);
    }
}

TEST(HumanMovementRules, RecencyIsAPenaltyNotABan)
{
    float const fresh = TargetWeight(TargetKind::Spread, 0, false, 0.0f);
    float const recent = TargetWeight(TargetKind::Spread, 0, true, 0.0f);
    EXPECT_FLOAT_EQ(recent, fresh * RecencyFactor);
    EXPECT_GT(recent, 0.0f);
}

TEST(HumanMovementRules, WeightMultipliesItsFactors)
{
    EXPECT_FLOAT_EQ(TargetWeight(TargetKind::Hub, 4, false, 60.0f), 1.6f * 0.5f);
    EXPECT_FLOAT_EQ(TargetWeight(TargetKind::Place, 1, true, 60.0f), 0.25f * 0.5f * RecencyFactor);
}

TEST(HumanMovementRules, OccupancyCountsABotOnce)
{
    EXPECT_EQ(Occupancy(3, 1), 3u);
    EXPECT_EQ(Occupancy(0, 2), 2u);
    EXPECT_EQ(Occupancy(0, 0), 0u);
}

TEST(HumanMovementRules, PickWeightedRejectsEmptyAndZero)
{
    EXPECT_EQ(PickWeighted({}, 0.5f), -1);
    EXPECT_EQ(PickWeighted({0.0f, 0.0f}, 0.5f), -1);
    EXPECT_EQ(PickWeighted({0.0f, -1.0f}, 0.5f), -1);
}

TEST(HumanMovementRules, PickWeightedWalksTheCumulativeSum)
{
    std::vector<float> const weights = {1.0f, 0.0f, 3.0f};
    EXPECT_EQ(PickWeighted(weights, 0.0f), 0);
    EXPECT_EQ(PickWeighted(weights, 0.24f), 0);
    EXPECT_EQ(PickWeighted(weights, 0.26f), 2);
    EXPECT_EQ(PickWeighted(weights, 0.999f), 2);
    EXPECT_EQ(PickWeighted(weights, 1.0f), 2);
}

TEST(HumanMovementRules, PickWeightedFallsBackWhenOnlyRecentRemain)
{
    std::vector<float> const weights = {TargetWeight(TargetKind::Spread, 0, true, 10.0f),
                                        TargetWeight(TargetKind::Spread, 0, true, 20.0f)};
    EXPECT_GE(PickWeighted(weights, 0.7f), 0);
}

TEST(HumanMovementRules, TopIndicesIsStableDescending)
{
    std::vector<float> const weights = {0.5f, 2.0f, 0.5f, 1.0f};
    EXPECT_EQ(TopIndices(weights, 3), (std::vector<size_t>{1, 3, 0}));
    EXPECT_EQ(TopIndices(weights, 10), (std::vector<size_t>{1, 3, 0, 2}));
    EXPECT_TRUE(TopIndices({}, 8).empty());
}

TEST(HumanMovementRules, RecentTargetsKeepsTheLastFourDistinct)
{
    RecentTargets recent;
    for (TargetKey key = 1; key <= 5; ++key)
        recent.Push(key);
    EXPECT_FALSE(recent.Contains(1));
    EXPECT_TRUE(recent.Contains(2));
    EXPECT_TRUE(recent.Contains(5));

    recent.Push(2);
    recent.Push(6);
    EXPECT_TRUE(recent.Contains(2));
    EXPECT_FALSE(recent.Contains(3));

    recent.Push(NoTarget);
    EXPECT_FALSE(recent.Contains(NoTarget));
    EXPECT_TRUE(recent.Contains(4));

    recent.Clear();
    EXPECT_FALSE(recent.Contains(6));
}

TEST(HumanMovementRules, ShortLegsPauseAsOftenPerYardAsLongOnes)
{
    constexpr float tripYards = 1200.0f;
    constexpr uint32_t routePauseYards = 400;
    auto const rolls = [&](float legYards)
    {
        float accrued = 0.0f;
        float expected = 0.0f;
        for (float walked = 0.0f; walked < tripYards; walked += legYards)
        {
            accrued = AccrueWalked(accrued, legYards);
            if (accrued < MinPauseWalkYards)
                continue;
            expected += PauseChance(tripYards, accrued, routePauseYards);
            accrued = 0.0f;
        }
        return expected;
    };
    EXPECT_NEAR(rolls(20.0f), rolls(60.0f), 0.35f);
    EXPECT_NEAR(rolls(20.0f), tripYards / routePauseYards, 0.35f);
}

TEST(HumanMovementRules, TeleportsAndBackwardStepsAddNoWalkedYards)
{
    EXPECT_FLOAT_EQ(AccrueWalked(10.0f, 25.0f), 35.0f);
    EXPECT_FLOAT_EQ(AccrueWalked(10.0f, 500.0f), 10.0f);
    EXPECT_FLOAT_EQ(AccrueWalked(10.0f, -3.0f), 10.0f);
    EXPECT_FLOAT_EQ(AccrueWalked(10.0f, MaxWalkStepYards), 10.0f + MaxWalkStepYards);
}

TEST(HumanMovementRules, PauseChanceNeedsALongTripAndALongLeg)
{
    EXPECT_FLOAT_EQ(PauseChance(200.0f, 100.0f, 400), 0.25f);
    EXPECT_FLOAT_EQ(PauseChance(200.0f, 300.0f, 400), 0.5f);
    EXPECT_FLOAT_EQ(PauseChance(80.0f, 100.0f, 400), 0.0f);
    EXPECT_FLOAT_EQ(PauseChance(200.0f, 29.9f, 400), 0.0f);
    EXPECT_FLOAT_EQ(PauseChance(200.0f, 100.0f, 0), 0.0f);
}

TEST(HumanMovementRules, PauseCutIndexNeedsAnInteriorPoint)
{
    EXPECT_EQ(PauseCutIndex(0, 0.5f), 0u);
    EXPECT_EQ(PauseCutIndex(2, 0.5f), 0u);
    EXPECT_EQ(PauseCutIndex(3, 0.0f), 1u);
    EXPECT_EQ(PauseCutIndex(3, 1.0f), 1u);
    EXPECT_EQ(PauseCutIndex(11, 0.0f), 3u);
    EXPECT_EQ(PauseCutIndex(11, 1.0f), 7u);
    for (float u = 0.0f; u <= 1.0f; u += 0.05f)
    {
        size_t const cut = PauseCutIndex(40, u);
        EXPECT_GE(cut, 12u);
        EXPECT_LE(cut, 27u);
    }
}

TEST(HumanMovementRules, LaneKeepsOneSideAndAModestWidth)
{
    bool left = false, right = false;
    for (uint32_t seed = 0; seed < 64; ++seed)
    {
        float const lane = LaneYards(seed);
        EXPECT_EQ(lane, LaneYards(seed));
        EXPECT_GE(std::fabs(lane), MinLaneYards);
        EXPECT_LE(std::fabs(lane), MaxLaneYards);
        left = left || lane > 0.0f;
        right = right || lane < 0.0f;
    }
    EXPECT_TRUE(left);
    EXPECT_TRUE(right);
}

TEST(HumanMovementRules, LaneWeightsEaseInAndOutWithoutReversing)
{
    std::vector<float> along;
    for (int i = 0; i <= 10; ++i)
        along.push_back(4.0f * i);
    std::vector<float> const weights = LaneWeights(along, 8.0f);
    ASSERT_EQ(weights.size(), along.size());
    EXPECT_FLOAT_EQ(weights.front(), 0.0f);
    EXPECT_FLOAT_EQ(weights.back(), 0.0f);
    EXPECT_FLOAT_EQ(weights[5], 1.0f);
    EXPECT_GT(weights[1], 0.0f);
    EXPECT_LT(weights[1], weights[2]);
    for (float w : weights)
    {
        EXPECT_GE(w, 0.0f);
        EXPECT_LE(w, 1.0f);
    }

    std::vector<float> const shortPath = LaneWeights({0.0f, 3.0f, 6.0f}, 8.0f);
    EXPECT_GT(shortPath[1], 0.0f);
    EXPECT_LT(shortPath[1], 1.0f);
}

TEST(HumanMovementRules, FloorMissIsCloseAcrossButAFloorAway)
{
    EXPECT_TRUE(IsFloorMiss(0.0f, 0.0f, 0.0f, 2.0f, 2.0f, 6.0f));
    EXPECT_TRUE(IsFloorMiss(0.0f, 0.0f, 9.0f, 1.0f, 0.0f, 3.0f));
    EXPECT_FALSE(IsFloorMiss(0.0f, 0.0f, 0.0f, 2.0f, 2.0f, 2.0f));
    EXPECT_FALSE(IsFloorMiss(20.0f, 0.0f, 0.0f, 0.0f, 0.0f, 6.0f));
}

TEST(HumanMovementIntent, ClaimCountsAndReleaseClears)
{
    IntentTable table;
    table.Claim(1, NpcKey(10), 1000);
    table.Claim(2, NpcKey(10), 1000);
    EXPECT_EQ(table.Count(NpcKey(10), 1000), 2u);
    table.Release(1);
    EXPECT_EQ(table.Count(NpcKey(10), 1000), 1u);
    table.Release(1);
    EXPECT_EQ(table.Count(NpcKey(10), 1000), 1u);
    table.Release(2);
    EXPECT_TRUE(table.Empty());
}

TEST(HumanMovementIntent, ReclaimMovesTheCount)
{
    IntentTable table;
    table.Claim(1, NpcKey(10), 0);
    table.Claim(1, PlaceKey(3, 4), 0);
    EXPECT_EQ(table.Count(NpcKey(10), 0), 0u);
    EXPECT_EQ(table.Count(PlaceKey(3, 4), 0), 1u);
    EXPECT_EQ(table.ClaimOf(1, 0), PlaceKey(3, 4));
}

TEST(HumanMovementIntent, SameTargetTwiceCountsOnce)
{
    IntentTable table;
    table.Claim(1, NpcKey(10), 0);
    table.Claim(1, NpcKey(10), 5);
    EXPECT_EQ(table.Count(NpcKey(10), 5), 1u);
}

TEST(HumanMovementIntent, ClaimsExpireUnlessRefreshed)
{
    IntentTable table;
    table.Claim(1, NpcKey(10), 0);
    table.Claim(2, NpcKey(10), 0);
    table.Refresh(2, ClaimTtlMs - 1);
    EXPECT_EQ(table.Count(NpcKey(10), ClaimTtlMs + 1), 1u);
    EXPECT_EQ(table.ClaimOf(1, ClaimTtlMs + 1), NoTarget);
    EXPECT_EQ(table.ClaimOf(2, ClaimTtlMs + 1), NpcKey(10));
    table.Prune(2 * ClaimTtlMs);
    EXPECT_TRUE(table.Empty());
}

TEST(HumanMovementIntent, ExpiryHandlesClockWrap)
{
    IntentTable table;
    uint32_t const nearWrap = 0xFFFFFF00u;
    table.Claim(1, NpcKey(10), nearWrap);
    EXPECT_EQ(table.Count(NpcKey(10), nearWrap + 0x200u), 1u);
    EXPECT_EQ(table.Count(NpcKey(10), nearWrap + ClaimTtlMs + 1), 0u);
}

TEST(HumanMovementIntent, RefreshWithoutClaimDoesNothing)
{
    IntentTable table;
    table.Refresh(9, 100);
    EXPECT_TRUE(table.Empty());
    EXPECT_EQ(table.ClaimOf(9, 100), NoTarget);
}
