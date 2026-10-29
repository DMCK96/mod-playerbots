/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROADGRAPH_H
#define PLAYERBOTS_ROADGRAPH_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Road graphs built offline from the client's terrain textures (tools/roads/build_roads.py) and the route
// planning bots do over them. Kept free of core headers so the unit tests can pin it.
namespace RoadRules
{
struct RoadNode
{
    float x, y, z;
};

struct RoadEdge
{
    uint32_t a, b;
    uint8_t kind;  // 1 road, 2 trail, 3 link across a gap the textures miss (bridge, gate, faded paint)
};

constexpr uint8_t LinkKind = 3;
constexpr float LinkCostFactor = 1.25f;  // links are unverified: a road of the same length wins

struct Point
{
    float x, y, z;
};

constexpr uint32_t NoEdge = UINT32_MAX;
constexpr uint32_t BlockMs = 1800000;
constexpr float MaxCoordinate = 100000.0f;
constexpr float AdvanceYards = 16.0f;  // past the 10 yd at which MoveFarTo starts the next leg, plus the offset
constexpr float EntryBackslide = 0.25f;  // a road node may be this share of its distance farther from the goal
constexpr float LegYards = 60.0f;      // road walked per leg before asking the navmesh again

struct PlanParams
{
    float entryRadius = 80.0f;   // how far off the road a trip may join or leave it
    size_t entryCount = 4;       // road nodes tried at each end
    float offRoadFactor = 1.6f;  // cost of a yard off the road against a yard on it
    float maxDetour = 1.4f;      // longest accepted walk against the direct distance
    float minRoadShare = 0.0f;   // road yards needed against the direct distance
    size_t maxExpansions = 20000;
};

// Longer trips may walk further to reach a road: a quarter of the trip, between 80 and 250 yards.
inline PlanParams ParamsForTrip(float directYards)
{
    PlanParams params;
    params.entryRadius = std::clamp(0.25f * directYards, 80.0f, 250.0f);
    return params;
}

enum class PlanResult : uint8_t
{
    Used,
    NoRoad,
    TooLong
};

struct RoadPlan
{
    PlanResult result = PlanResult::NoRoad;
    std::vector<uint32_t> nodes;
    float roadYards = 0.0f;
    float walkYards = 0.0f;
    float directYards = 0.0f;
};

inline float Distance2d(float ax, float ay, float bx, float by)
{
    return std::hypot(ax - bx, ay - by);
}

class RoadGraph
{
public:
    static std::optional<RoadGraph> Parse(uint8_t const* data, size_t size)
    {
        constexpr size_t header = 16, nodeSize = 12, edgeSize = 12;
        if (!data || size < header || std::memcmp(data, "CRDS", 4) != 0)
            return std::nullopt;
        uint32_t version, nodeCount, edgeCount;
        std::memcpy(&version, data + 4, 4);
        std::memcpy(&nodeCount, data + 8, 4);
        std::memcpy(&edgeCount, data + 12, 4);
        if (version != 1 || size != header + uint64_t(nodeSize) * nodeCount + uint64_t(edgeSize) * edgeCount)
            return std::nullopt;

        RoadGraph graph;
        graph.nodes.resize(nodeCount);
        graph.edges.resize(edgeCount);
        uint8_t const* cursor = data + header;
        for (RoadNode& node : graph.nodes)
        {
            std::memcpy(&node.x, cursor, 4);
            std::memcpy(&node.y, cursor + 4, 4);
            std::memcpy(&node.z, cursor + 8, 4);
            cursor += nodeSize;
            for (float v : {node.x, node.y, node.z})
                if (!std::isfinite(v) || std::fabs(v) > MaxCoordinate)
                    return std::nullopt;
        }
        for (RoadEdge& edge : graph.edges)
        {
            std::memcpy(&edge.a, cursor, 4);
            std::memcpy(&edge.b, cursor + 4, 4);
            edge.kind = cursor[8];
            cursor += edgeSize;
            if (edge.a >= nodeCount || edge.b >= nodeCount)
                return std::nullopt;
        }
        graph.Index();
        return graph;
    }

    size_t NodeCount() const { return nodes.size(); }
    size_t EdgeCount() const { return edges.size(); }
    RoadNode const& Node(uint32_t id) const { return nodes[id]; }
    RoadEdge const& Edge(uint32_t id) const { return edges[id]; }

    uint32_t EdgeBetween(uint32_t a, uint32_t b) const
    {
        if (a >= nodes.size() || b >= nodes.size())
            return NoEdge;
        auto const& links = adjacency[a].size() <= adjacency[b].size() ? adjacency[a] : adjacency[b];
        uint32_t const other = adjacency[a].size() <= adjacency[b].size() ? b : a;
        for (auto const& [neighbour, edge] : links)
            if (neighbour == other)
                return edge;
        return NoEdge;
    }

    std::vector<uint32_t> Nearest(float x, float y, float radius, size_t max) const
    {
        std::vector<std::pair<float, uint32_t>> found;
        int32_t const x0 = CellOf(x - radius), x1 = CellOf(x + radius);
        int32_t const y0 = CellOf(y - radius), y1 = CellOf(y + radius);
        for (int32_t cx = x0; cx <= x1; ++cx)
        {
            for (int32_t cy = y0; cy <= y1; ++cy)
            {
                auto const cell = grid.find(CellKey(cx, cy));
                if (cell == grid.end())
                    continue;
                for (uint32_t id : cell->second)
                {
                    float const d = Distance2d(x, y, nodes[id].x, nodes[id].y);
                    if (d <= radius)
                        found.emplace_back(d, id);
                }
            }
        }
        std::sort(found.begin(), found.end());
        if (found.size() > max)
            found.resize(max);
        std::vector<uint32_t> ids;
        ids.reserve(found.size());
        for (auto const& entry : found)
            ids.push_back(entry.second);
        return ids;
    }

    // Cheapest way to join a road near `from` (only at a node closer to `to` than `from` is, give or take a quarter of the way to the road) and ride it as far as it pays, then leave it for `to`: every road
    // node is a place to step off, at offRoadFactor per yard of the way still to go. The direct line at the same
    // rate is the bar a road route has to beat. Past the expansion cap the best route found so far is kept.
    RoadPlan Plan(Point from, Point to, PlanParams const& params,
                  std::unordered_set<uint32_t> const* blocked = nullptr) const
    {
        RoadPlan plan;
        plan.directYards = Distance2d(from.x, from.y, to.x, to.y);
        std::vector<uint32_t> entries;
        for (uint32_t id : Nearest(from.x, from.y, params.entryRadius, params.entryCount * 8))
            if (entries.size() < params.entryCount &&
                Distance2d(nodes[id].x, nodes[id].y, to.x, to.y) <=
                    plan.directYards + EntryBackslide * Distance2d(nodes[id].x, nodes[id].y, from.x, from.y))
                entries.push_back(id);
        if (entries.empty())
            return plan;

        uint32_t const start = uint32_t(nodes.size());
        float best = params.offRoadFactor * plan.directYards;
        uint32_t bestNode = start;
        float const ellipse = params.maxDetour * plan.directYards + 2.0f * params.entryRadius;

        auto heuristic = [&](uint32_t id) { return Distance2d(nodes[id].x, nodes[id].y, to.x, to.y); };

        using Entry = std::pair<float, uint32_t>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
        std::unordered_map<uint32_t, float> cost;
        std::unordered_map<uint32_t, uint32_t> parent;
        cost.reserve(1024);
        parent.reserve(1024);

        auto relax = [&](uint32_t from_, uint32_t id, float g)
        {
            auto const known = cost.find(id);
            if ((known != cost.end() && known->second <= g) || g + heuristic(id) >= best)
                return;
            cost[id] = g;
            parent[id] = from_;
            open.emplace(g + heuristic(id), id);
        };

        for (uint32_t id : entries)
            relax(start, id, params.offRoadFactor * Distance2d(from.x, from.y, nodes[id].x, nodes[id].y));

        size_t expansions = 0;
        while (!open.empty())
        {
            auto const [f, id] = open.top();
            open.pop();
            float const g = cost[id];
            if (f > g + heuristic(id) + 1e-3f)
                continue;
            if (f >= best)
                break;
            RoadNode const& node = nodes[id];
            if (Distance2d(from.x, from.y, node.x, node.y) + Distance2d(node.x, node.y, to.x, to.y) > ellipse)
                continue;
            float const total = g + params.offRoadFactor * heuristic(id);
            if (total < best)
            {
                best = total;
                bestNode = id;
            }
            if (++expansions > params.maxExpansions)
                break;
            for (auto const& [neighbour, edge] : adjacency[id])
            {
                if (blocked && blocked->count(edge))
                    continue;
                RoadNode const& next = nodes[neighbour];
                float const factor = edges[edge].kind == LinkKind ? LinkCostFactor : 1.0f;
                relax(id, neighbour, g + factor * Distance2d(node.x, node.y, next.x, next.y));
            }
        }
        if (bestNode == start)
            return plan;

        for (uint32_t id = bestNode; id != start && plan.nodes.size() <= nodes.size(); id = parent[id])
            plan.nodes.push_back(id);
        if (plan.nodes.size() > nodes.size())
        {
            plan.nodes.clear();
            return plan;
        }
        std::reverse(plan.nodes.begin(), plan.nodes.end());
        for (size_t i = 1; i < plan.nodes.size(); ++i)
        {
            RoadNode const& a = nodes[plan.nodes[i - 1]];
            RoadNode const& b = nodes[plan.nodes[i]];
            plan.roadYards += Distance2d(a.x, a.y, b.x, b.y);
        }
        RoadNode const& first = nodes[plan.nodes.front()];
        RoadNode const& last = nodes[plan.nodes.back()];
        plan.walkYards = Distance2d(from.x, from.y, first.x, first.y) + plan.roadYards +
                         Distance2d(last.x, last.y, to.x, to.y);
        if (plan.roadYards < params.minRoadShare * plan.directYards)
            plan.result = PlanResult::NoRoad;
        else if (plan.walkYards > params.maxDetour * plan.directYards)
            plan.result = PlanResult::TooLong;
        else
            plan.result = PlanResult::Used;
        return plan;
    }

private:
    static constexpr float CellYards = 64.0f;

    static int32_t CellOf(float v) { return int32_t(std::floor(v / CellYards)); }
    static uint64_t CellKey(int32_t x, int32_t y) { return (uint64_t(uint32_t(x)) << 32) | uint32_t(y); }

    void Index()
    {
        adjacency.assign(nodes.size(), {});
        for (uint32_t id = 0; id < edges.size(); ++id)
        {
            adjacency[edges[id].a].emplace_back(edges[id].b, id);
            adjacency[edges[id].b].emplace_back(edges[id].a, id);
        }
        for (uint32_t id = 0; id < nodes.size(); ++id)
            grid[CellKey(CellOf(nodes[id].x), CellOf(nodes[id].y))].push_back(id);
    }

    std::vector<RoadNode> nodes;
    std::vector<RoadEdge> edges;
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> adjacency;  // per node: (neighbour, edge id)
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
};

// `node` moved `yards` to the left (side 1) or right (side -1) of the road arriving from `previous`.
inline Point OffsetWaypoint(Point previous, Point node, int side, float yards)
{
    float const dx = node.x - previous.x;
    float const dy = node.y - previous.y;
    float const length = std::hypot(dx, dy);
    if (length <= 0.0f)
        return node;
    float const scale = float(side) * yards / length;
    return {node.x - dy * scale, node.y + dx * scale, node.z};
}

inline bool ShouldAdvance(Point bot, Point current, Point const* next)
{
    float const toCurrent = Distance2d(bot.x, bot.y, current.x, current.y);
    if (toCurrent <= AdvanceYards)
        return true;
    return next && Distance2d(bot.x, bot.y, next->x, next->y) < toCurrent;
}

inline bool RoadTripEligible(float tripYards, float minTripYards, uint32_t sharePercent, float roll)
{
    return sharePercent > 0 && tripYards >= minTripYards && roll * 100.0f < float(sharePercent);
}

// Road edges bots failed to walk, skipped by later plans until BlockMs has passed. Not thread-safe.
class EdgeBlocklist
{
public:
    void Block(uint32_t edge, uint32_t nowMs) { stamps[edge] = nowMs; }

    bool Contains(uint32_t edge, uint32_t nowMs) const
    {
        auto const found = stamps.find(edge);
        return found != stamps.end() && uint32_t(nowMs - found->second) <= BlockMs;
    }

    std::unordered_set<uint32_t> Snapshot(uint32_t nowMs)
    {
        std::unordered_set<uint32_t> live;
        for (auto it = stamps.begin(); it != stamps.end();)
        {
            if (uint32_t(nowMs - it->second) > BlockMs)
                it = stamps.erase(it);
            else
                live.insert((it++)->first);
        }
        return live;
    }

private:
    std::unordered_map<uint32_t, uint32_t> stamps;
};
}  // namespace RoadRules

#endif
