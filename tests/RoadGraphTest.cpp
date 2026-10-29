/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RoadGraph.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace RoadRules;

namespace
{
std::vector<uint8_t> Encode(std::vector<RoadNode> const& nodes, std::vector<RoadEdge> const& edges,
                            uint32_t version = 1, char const* magic = "CRDS")
{
    std::vector<uint8_t> out;
    auto put = [&out](void const* p, size_t n)
    {
        auto const* b = static_cast<uint8_t const*>(p);
        out.insert(out.end(), b, b + n);
    };
    put(magic, 4);
    uint32_t const nodeCount = uint32_t(nodes.size());
    uint32_t const edgeCount = uint32_t(edges.size());
    put(&version, 4);
    put(&nodeCount, 4);
    put(&edgeCount, 4);
    for (RoadNode const& n : nodes)
    {
        put(&n.x, 4);
        put(&n.y, 4);
        put(&n.z, 4);
    }
    for (RoadEdge const& e : edges)
    {
        put(&e.a, 4);
        put(&e.b, 4);
        uint8_t const tail[4] = {e.kind, 0, 0, 0};
        put(tail, 4);
    }
    return out;
}

// Nodes every `step` yards from (x0, y) to (x1, y), joined in order; returns the first node id.
uint32_t AddLine(std::vector<RoadNode>& nodes, std::vector<RoadEdge>& edges, float x0, float x1, float y,
                 float step = 40.0f)
{
    uint32_t const first = uint32_t(nodes.size());
    int const count = int(std::round((x1 - x0) / step)) + 1;
    for (int i = 0; i < count; ++i)
    {
        nodes.push_back({x0 + (x1 - x0) * i / (count - 1), y, 0.0f});
        if (i)
            edges.push_back({first + i - 1, first + uint32_t(i), 1});
    }
    return first;
}

RoadGraph Make(std::vector<RoadNode> const& nodes, std::vector<RoadEdge> const& edges)
{
    std::vector<uint8_t> const bytes = Encode(nodes, edges);
    std::optional<RoadGraph> graph = RoadGraph::Parse(bytes.data(), bytes.size());
    EXPECT_TRUE(graph.has_value());
    return *graph;
}

RoadGraph StraightRoad()
{
    std::vector<RoadNode> nodes;
    std::vector<RoadEdge> edges;
    AddLine(nodes, edges, 0.0f, 1000.0f, 0.0f);
    return Make(nodes, edges);
}
}  // namespace

TEST(RoadGraph, ParsesNodesAndEdges)
{
    std::vector<uint8_t> const bytes = Encode({{1, 2, 3}, {4, 5, 6}}, {{0, 1, 2}});
    std::optional<RoadGraph> graph = RoadGraph::Parse(bytes.data(), bytes.size());
    ASSERT_TRUE(graph.has_value());
    EXPECT_EQ(graph->NodeCount(), 2u);
    EXPECT_EQ(graph->EdgeCount(), 1u);
    EXPECT_FLOAT_EQ(graph->Node(1).z, 6.0f);
    EXPECT_EQ(graph->Edge(0).kind, 2);
    EXPECT_EQ(graph->EdgeBetween(1, 0), 0u);
    EXPECT_EQ(graph->EdgeBetween(0, 0), NoEdge);
}

TEST(RoadGraph, RejectsBadFiles)
{
    std::vector<RoadNode> const nodes{{0, 0, 0}, {1, 1, 1}};
    std::vector<uint8_t> magic = Encode(nodes, {{0, 1, 1}}, 1, "XRDS");
    std::vector<uint8_t> version = Encode(nodes, {{0, 1, 1}}, 2);
    std::vector<uint8_t> truncated = Encode(nodes, {{0, 1, 1}});
    truncated.pop_back();
    std::vector<uint8_t> range = Encode(nodes, {{0, 7, 1}});
    EXPECT_FALSE(RoadGraph::Parse(magic.data(), magic.size()).has_value());
    EXPECT_FALSE(RoadGraph::Parse(version.data(), version.size()).has_value());
    EXPECT_FALSE(RoadGraph::Parse(truncated.data(), truncated.size()).has_value());
    EXPECT_FALSE(RoadGraph::Parse(range.data(), range.size()).has_value());
    EXPECT_FALSE(RoadGraph::Parse(nullptr, 0).has_value());
}

TEST(RoadGraph, NearestIsSortedAndBounded)
{
    RoadGraph const graph = StraightRoad();
    std::vector<uint32_t> const near = graph.Nearest(85.0f, 10.0f, 60.0f, 3);
    ASSERT_EQ(near.size(), 3u);
    EXPECT_FLOAT_EQ(graph.Node(near[0]).x, 80.0f);
    EXPECT_FLOAT_EQ(graph.Node(near[1]).x, 120.0f);
    EXPECT_FLOAT_EQ(graph.Node(near[2]).x, 40.0f);
    EXPECT_TRUE(graph.Nearest(85.0f, 500.0f, 60.0f, 3).empty());
}

TEST(RoadGraph, TripAlongARoadUsesIt)
{
    RoadGraph const graph = StraightRoad();
    RoadPlan const plan = graph.Plan({0, 30, 0}, {1000, 30, 0}, PlanParams{});
    ASSERT_EQ(plan.result, PlanResult::Used);
    ASSERT_GE(plan.nodes.size(), 20u);
    EXPECT_LE(graph.Node(plan.nodes.front()).x, 80.0f);
    EXPECT_GE(graph.Node(plan.nodes.back()).x, 920.0f);
    EXPECT_NEAR(plan.directYards, 1000.0f, 0.1f);
    EXPECT_GE(plan.roadYards, 840.0f);
    EXPECT_LE(plan.walkYards, 1.4f * plan.directYards);
}

TEST(RoadGraph, DestinationFarFromAnyRoadGoesDirect)
{
    RoadGraph const graph = StraightRoad();
    EXPECT_EQ(graph.Plan({0, 30, 0}, {1000, 300, 0}, PlanParams{}).result, PlanResult::NoRoad);
}

TEST(RoadGraph, RoadThatSavesNothingLosesToTheDirectLine)
{
    RoadGraph const graph = Make({{0, 0, 0}, {0, 500, 0}, {100, 500, 0}, {100, 0, 0}},
                                 {{0, 1, 1}, {1, 2, 1}, {2, 3, 1}});
    EXPECT_EQ(graph.Plan({0, -10, 0}, {100, -10, 0}, PlanParams{}).result, PlanResult::NoRoad);
}

TEST(RoadGraph, CheaperButLongRoadIsTooLong)
{
    RoadGraph const graph = Make({{0, 0, 0}, {500, 560, 0}, {1000, 0, 0}}, {{0, 1, 1}, {1, 2, 1}});
    RoadPlan const plan = graph.Plan({0, 0, 0}, {1000, 0, 0}, PlanParams{});
    EXPECT_EQ(plan.result, PlanResult::TooLong);
}

TEST(RoadGraph, MostlyOffRoadRouteIsNotUsed)
{
    RoadGraph const graph = Make({{50, 0, 0}, {110, 0, 0}}, {{0, 1, 1}});
    EXPECT_EQ(graph.Plan({0, 0, 0}, {160, 0, 0}, PlanParams{}).result, PlanResult::NoRoad);
}

TEST(RoadGraph, BlockedEdgeIsAvoided)
{
    std::vector<RoadNode> nodes;
    std::vector<RoadEdge> edges;
    AddLine(nodes, edges, 0.0f, 1000.0f, 0.0f);
    uint32_t const bend = uint32_t(nodes.size());
    nodes.push_back({500, 100, 0});
    edges.push_back({0, bend, 1});
    edges.push_back({bend, 25, 1});
    RoadGraph const graph = Make(nodes, edges);
    std::unordered_set<uint32_t> const blocked{graph.EdgeBetween(12, 13)};
    RoadPlan const plan = graph.Plan({0, 0, 0}, {1000, 0, 0}, PlanParams{}, &blocked);
    ASSERT_EQ(plan.result, PlanResult::Used);
    EXPECT_NE(std::find(plan.nodes.begin(), plan.nodes.end(), bend), plan.nodes.end());
}

TEST(RoadGraph, ExpansionCapGivesUp)
{
    RoadGraph const graph = StraightRoad();
    PlanParams params;
    params.maxExpansions = 2;
    EXPECT_EQ(graph.Plan({0, 30, 0}, {1000, 30, 0}, params).result, PlanResult::NoRoad);
}

TEST(RoadRules, OffsetIsPerpendicularToTheRoad)
{
    Point const left = OffsetWaypoint({0, 0, 5}, {10, 0, 5}, 1, 2.0f);
    Point const right = OffsetWaypoint({0, 0, 5}, {10, 0, 5}, -1, 2.0f);
    EXPECT_NEAR(left.x, 10.0f, 1e-4f);
    EXPECT_NEAR(left.y, 2.0f, 1e-4f);
    EXPECT_NEAR(right.y, -2.0f, 1e-4f);
    EXPECT_FLOAT_EQ(left.z, 5.0f);
    Point const same = OffsetWaypoint({3, 3, 0}, {3, 3, 0}, 1, 2.0f);
    EXPECT_FLOAT_EQ(same.x, 3.0f);
    EXPECT_FLOAT_EQ(same.y, 3.0f);
}

TEST(RoadRules, AdvanceWhenCloseOrPastTheWaypoint)
{
    Point const current{0, 0, 0};
    Point const next{40, 0, 0};
    EXPECT_TRUE(ShouldAdvance({5, 5, 0}, current, &next));
    EXPECT_TRUE(ShouldAdvance({25, 0, 0}, current, &next));
    EXPECT_FALSE(ShouldAdvance({-20, 0, 0}, current, &next));
    EXPECT_FALSE(ShouldAdvance({-20, 0, 0}, current, nullptr));
    EXPECT_TRUE(ShouldAdvance({7, 0, 0}, current, nullptr));
}

TEST(RoadRules, EligibilityNeedsLengthAndRoll)
{
    EXPECT_TRUE(RoadTripEligible(200.0f, 150.0f, 75, 0.74f));
    EXPECT_FALSE(RoadTripEligible(200.0f, 150.0f, 75, 0.75f));
    EXPECT_FALSE(RoadTripEligible(149.0f, 150.0f, 75, 0.0f));
    EXPECT_FALSE(RoadTripEligible(500.0f, 150.0f, 0, 0.0f));
}

TEST(RoadRules, BlocklistExpires)
{
    EdgeBlocklist list;
    list.Block(5, 1000);
    EXPECT_TRUE(list.Contains(5, 1000 + BlockMs - 1));
    EXPECT_FALSE(list.Contains(5, 1000 + BlockMs + 1));
    EXPECT_FALSE(list.Contains(6, 1000));
    list.Block(8, UINT32_MAX - 10);
    EXPECT_TRUE(list.Contains(8, 100));
    list.Block(7, 1000 + BlockMs);
    std::unordered_set<uint32_t> const live = list.Snapshot(1000 + BlockMs + 1);
    EXPECT_EQ(live.count(5), 0u);
    EXPECT_EQ(live.count(7), 1u);
}
