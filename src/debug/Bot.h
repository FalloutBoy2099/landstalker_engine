#ifndef _BOT_H_
#define _BOT_H_

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include "room/RoomView.h"

// A play-test bot: plays the game through the engine's own rules (keys, collisions, scripts) at
// many times real speed, to check that the story can be followed from one place to another.
// It walks the warp graph towards a target room, talks to everyone, opens the chests, picks up
// what lies around, fights what attacks it and uses EkeEkes. Everything noteworthy is logged
// (rooms, flags, items, dialogues, deaths, places where it gets stuck). Used with LS_BOT=<room>.
class Bot
{
public:
    Bot(RoomView& room, std::shared_ptr<GameState> game, std::filesystem::path rooms_dir)
        : r(room), g(std::move(game)), rooms_dir(std::move(rooms_dir))
    {
        BuildGraph();
    }

    // Plays until the target room's boss is dead (or the time is up). Returns true on success.
    // `targets`: rooms to reach one after the other, e.g. "180,592" (a room that holds a boss is
    // finished when the boss is dead, any other when the bot has had its say there).
    bool Run(const std::string& targets, double max_seconds)
    {
        for (std::size_t i = 0; i < targets.size();)
        {
            goals.push_back(std::atoi(targets.c_str() + i));
            const auto comma = targets.find(',', i);
            if (comma == std::string::npos) break;
            i = comma + 1;
        }
        if (goals.empty()) goals.push_back(-1);
        return Run(goals[0], max_seconds);
    }

    bool Run(int target, double max_seconds)
    {
        goal_room = target;
        r.bot_input = true;
        Snapshot();
        while (time < max_seconds)
        {
            Tick();
            if (finished) break;
        }
        r.bot_input = false;
        r.bot_dx = r.bot_dy = 0;
        if (!finished)
        {
            Log("state: locked %d intro %d busy %.2f fx %.2f warp_pending %d dialogue %zu cutscene_ops %zu fainted %d hold %.1f dead %d menu %d chest %d swinging %d", r.input_locked, r.intro_active,
                r.cutscene_busy, r.fx_level, r.warp_pending, r.dialogue_pages.size(), r.cutscene_ops.size(), r.fainted, r.input_hold, r.dead, r.menu_open, r.chest_open.active, r.swinging);
            Log("player (%.2f,%.2f) z %.2f grounded %d support %d cell %04X floor %d blocked: %s", r.player_x, r.player_y, r.player_z, r.grounded,
                r.support_entity, r.GetCell(r.cursor_x, r.cursor_y), r.FloorHeight(r.cursor_x, r.cursor_y), r.blocked_text.c_str());
            int cxd = r.cursor_x, cyd = r.cursor_y;
            if (const char* dump = std::getenv("LS_BOT_DUMPXY")) std::sscanf(dump, "%d,%d", &cxd, &cyd);
            {
                std::map<std::pair<int, int>, double> plat0;
                const auto blocked0 = EntityCells(-1, plat0);
                if (std::getenv("LS_BOT_DUMPXY"))
                {
                    auto pth = Search([&](int x, int y) { return x == cxd && y == cyd; }, -1);
                    std::string ps;
                    for (const auto& c : pth) ps += "(" + std::to_string(c.first) + "," + std::to_string(c.second) + ")";
                    Log("  search to dump cell: %zu cells %s", pth.size(), ps.c_str());
                }
                Log("  dump cell (%d,%d): blocked %d plat %d; steps from N/S/E/W neighbours:", cxd, cyd, (int)blocked0.count({ cxd, cyd }), (int)plat0.count({ cxd, cyd }));
                static const int ndx0[4] = { -1, 0, 1, 0 }, ndy0[4] = { 0, -1, 0, 1 };
                for (int k = 0; k < 4; ++k)
                    Log("    from (%d,%d) kind %d walkable %d type %02X dest blocked %d", cxd + ndx0[k], cyd + ndy0[k],
                        StepKind(cxd + ndx0[k], cyd + ndy0[k], cxd, cyd, plat0), (int)r.IsWalkable(cxd + ndx0[k], cyd + ndy0[k]),
                        r.GetCell(cxd + ndx0[k], cyd + ndy0[k]) & 0x3F, (int)blocked0.count({ cxd + ndx0[k], cyd + ndy0[k] }));
            }
            for (int ddy = -2; ddy <= 3; ++ddy)
            {
                std::string row;
                for (int ddx2 = -5; ddx2 <= 2; ++ddx2)
                {
                    const int qx = cxd + ddx2, qy = cyd + ddy;
                    char b[24];
                    std::snprintf(b, sizeof b, "%s%d.%02X ", InRoom(qx, qy) && r.IsWalkable(qx, qy) ? "" : "#", InRoom(qx, qy) ? r.FloorHeight(qx, qy) : -1, InRoom(qx, qy) ? r.GetCell(qx, qy) & 0x3F : 0);
                    row += b;
                }
                Log("  y%d: %s", cyd + ddy, row.c_str());
            }
            for (std::size_t i = 0; i < r.entities.size(); ++i)
            {
                const auto& e = r.entities[i];
                Log("  entity %zu type %d at (%.1f,%.1f,z %.1f) visible %d hostile %d behaviour %d pc %d busy %d", i, e.type, e.x, e.y, e.z, e.visible,
                    e.hostile, e.behaviour, e.bh_pc, e.bh_busy);
            }
        }
        Log("END after %.0f s: room %d, hp %d/%d, gold %d, deaths %d, %s", time, r.room_number, g->GetHealth(),
            g->GetMaxHealth(), g->GetGold(), deaths, finished ? "GOAL REACHED" : "goal not reached");
        return finished;
    }

private:
    struct Edge { int from, to, x, y, w, h; bool fall = false; };

    RoomView& r;
    std::shared_ptr<GameState> g;
    std::filesystem::path rooms_dir;
    std::map<int, std::vector<Edge>> graph;
    std::map<std::tuple<int, int, int, int>, double> dead_edges;   // (from, to, x, y) that could not be walked -> when (retried after a while)
    std::map<std::tuple<int, int, int, int>, int> edge_failures;

    static constexpr double DT = 1.0 / 60.0;
    double time = 0.0;
    int goal_room = -1;
    std::vector<int> goals;
    std::size_t goal_idx = 0;
    double goal_arrived = -1.0;

    // The current goal is done: on to the next one, or finish.
    void AdvanceGoal()
    {
        Log("goal room %d done", goal_room);
        goal_arrived = -1.0;
        boss_seen = false;
        if (goal_idx + 1 < goals.size()) { goal_room = goals[++goal_idx]; tour_room = -1; Log("next goal: room %d", goal_room); }
        else finished = true;
    }
    bool finished = false;
    int deaths = 0;

    // ---- perception of the last tick, to log changes ----
    int seen_room = -1;
    std::set<int> seen_flags;
    std::map<int, int> seen_items;
    int seen_gold = 0, seen_maxhp = 0;
    std::string seen_dialogue;
    double room_entered = 0.0;
    std::set<int> visited;

    // ---- task memory ----
    std::set<std::tuple<int, int, int>> talked;            // (room, entity, flags signature)
    std::map<std::tuple<int, int, int>, int> talk_tries;
    std::set<std::pair<int, int>> bad_targets;               // (room, entity) that cannot be reached
    double next_key = 0.0;       // earliest time of the next key press
    std::pair<int, int> step_off{ -1, -1 };   // where to step to leave a doorway the player stands in
    int step_off_room = -1;
    int nudge_ticks = 0, nudge_dx = 0, nudge_dy = 0;   // a few ticks of sliding sideways before opening a chest
    double last_tick_log = 0.0;
    double last_progress = 0.0;  // last time the player moved or something happened
    double last_x = 0, last_y = 0;
    int stuck_count = 0;
    std::set<std::pair<int, int>> temp_block;                // cells to avoid for a while
    double temp_block_until = 0.0;

    // Interaction in progress (turning towards what is to be talked to / opened).
    struct Aim { bool active = false; int entity = -1; int facing = 0; int ticks = 0; bool chest = false; };
    Aim aim;
    std::vector<std::pair<int, int>> path;   // cells, current one first
    std::map<std::pair<int, int>, double> plat_cache;   // object tops of the last search
    int plan_age = 1000;
    int plan_room = -1;
    std::string plan_what;

    // ---- logging ----
    void Log(const char* fmt, ...)
    {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        std::fprintf(stderr, "[BOT %6.1f r%03d] %s\n", time, r.room_number, buf);
        std::fflush(stderr);
    }

    static std::string Flat(const std::string& s)
    {
        std::string out;
        for (char c : s) out += (c == '\n') ? ' ' : c;
        return out.size() > 150 ? out.substr(0, 150) + "..." : out;
    }

    // The room whose warps the current one uses (a flag-dependent version of a room counts as the original).
    int Cur() const { return r.OriginalRoom(r.room_number); }

    // ---- the warp graph ----
    void BuildGraph()
    {
        for (const auto& entry : std::filesystem::directory_iterator(rooms_dir))
        {
            const auto name = entry.path().filename().string();
            if (name.rfind("room_", 0) != 0) continue;
            std::vector<RoomView::RawWarp> raw;
            try { raw = RoomView::LoadRawWarps(entry.path() / "room.yaml"); }
            catch (const std::exception&) { continue; }
            {
                // falling into a pit cell leads to fall_destination
                std::ifstream f(entry.path() / "room.yaml");
                std::string line;
                int number = -1, fall = -1;
                while (std::getline(f, line) && (number < 0 || fall < 0))
                {
                    if (line.find("  number:") == 0) number = std::atoi(line.c_str() + 9);
                    if (line.find("  fall_destination:") == 0) fall = std::atoi(line.c_str() + 19);
                }
                if (number >= 0 && fall >= 0) graph[number].push_back({ number, fall, -1, -1, 0, 0, true });
            }
            for (const auto& w : raw)
            {
                graph[w.room1].push_back({ w.room1, w.room2, w.x1, w.y1, w.x_size, w.y_size });
                graph[w.room2].push_back({ w.room2, w.room1, w.x2, w.y2, w.x_size, w.y_size });
            }
        }
    }

    // First warp of the shortest way from `from` to `to`, avoiding the dead edges.
    const Edge* NextHop(int from, int to)
    {
        if (from == to) return nullptr;
        std::map<int, std::pair<int, const Edge*>> prev;   // room -> (previous room, edge used from it)
        std::deque<int> q{ from };
        prev[from] = { -1, nullptr };
        while (!q.empty())
        {
            const int u = q.front();
            q.pop_front();
            if (u == to) break;
            for (const auto& e : graph[u])
            {
                const auto dead = dead_edges.find({ e.from, e.to, e.x, e.y });
                if ((dead != dead_edges.end() && time - dead->second < 90.0) || prev.count(e.to)) continue;
                prev[e.to] = { u, &e };
                q.push_back(e.to);
            }
        }
        if (!prev.count(to)) return nullptr;
        const Edge* first = nullptr;
        for (int x = to; prev[x].first != -1; x = prev[x].first) first = prev[x].second;
        return first;
    }

    // ---- cells ----
    static constexpr int OFF = RoomView::CELL_OFFSET;
    bool InRoom(int x, int y) const { return x >= OFF && y >= OFF && x < OFF + r.hm_width && y < OFF + r.hm_height; }
    int H(int x, int y) const { return r.FloorHeight(x, y); }

    // Cells a solid entity (not an enemy) stands in. Inanimate objects the player can stand on
    // (see RoomView::PlatformTop) do not block: their top is returned in `plat` instead.
    std::set<std::pair<int, int>> EntityCells(int ignore, std::map<std::pair<int, int>, double>& plat) const
    {
        std::set<std::pair<int, int>> out;
        over_cache.clear();
        if (!r.entity_table) return out;
        for (std::size_t i = 0; i < r.entities.size(); ++i)
        {
            const auto& e = r.entities[i];
            if (static_cast<int>(i) == ignore) continue;
            if (!e.solid || !e.visible || !r.entity_table->Has(e.type) || e.type >= 0xC0) continue;
            const auto* stats0 = r.entity_table->GetEnemy(e.type);
            const bool still_hazard = r.IsAliveEnemy(i) && stats0 && stats0->health == 255 && e.behaviour == 0;   // a motionless spiked ball: walked around
            if (r.IsAliveEnemy(i) && !still_hazard) continue;
            if (g->IsCollected(r.room_number, static_cast<int>(i))) continue;
            const auto* stats = r.entity_table->GetEnemy(e.type);
            const bool platform = !e.hostile && !e.talks && !e.dying && (!stats || stats->health == 255);
            const double half = r.entity_table->GetHitboxBase(e.type) / 16.0 + (still_hazard ? 0.25 : 0.0);
            const double top = e.z + r.entity_table->GetHitboxHeight(e.type) / 16.0;
            const int x0 = static_cast<int>(std::floor(e.x - half - RoomView::PLAYER_RADIUS));
            const int x1 = static_cast<int>(std::floor(e.x + half + RoomView::PLAYER_RADIUS));
            const int y0 = static_cast<int>(std::floor(e.y - half - RoomView::PLAYER_RADIUS));
            const int y1 = static_cast<int>(std::floor(e.y + half + RoomView::PLAYER_RADIUS));
            for (int x = x0; x <= x1; ++x)
                for (int y = y0; y <= y1; ++y)
                {
                    if (!InRoom(x, y)) continue;
                    // the player's box centred in the cell would overlap the entity's box
                    if (std::abs(x + 0.5 - e.x) < half + RoomView::PLAYER_RADIUS &&
                        std::abs(y + 0.5 - e.y) < half + RoomView::PLAYER_RADIUS)
                    {
                        if (e.z < H(x, y) + RoomView::PLAYER_HEIGHT)   // an object overhead (a raised block) is no obstacle
                        {
                            if (platform) { auto& t = plat[{ x, y }]; t = std::max(t, top); }
                            else if (H(x, y) < top && H(x, y) + RoomView::PLAYER_HEIGHT > e.z) out.insert({ x, y });
                        }
                        else if (platform && top > H(x, y)) { auto& t = over_cache[{ x, y }]; t = std::max(t, top); }   // a bridge over the floor
                    }
                }
        }
        return out;
    }

    // Standing height of a cell: the floor, or the top of an object on it.
    double Eff(int x, int y, const std::map<std::pair<int, int>, double>& plat) const
    {
        const auto it = plat.find({ x, y });
        return std::max(static_cast<double>(H(x, y)), it == plat.end() ? -1.0 : it->second);
    }

    // The kind of step from one cell to a neighbour: 0 impossible, 1 walk, 2 jump.
    mutable std::map<std::pair<int, int>, double> over_cache;   // platforms hovering over the floor of a cell (bridges): their top

    int StepKind(int cx, int cy, int nx, int ny, const std::map<std::pair<int, int>, double>& plat, double from_at_least = -1e9) const
    {
        return StepKind1(std::max(Eff(cx, cy, plat), from_at_least), cx, cy, nx, ny, Eff(nx, ny, plat), plat, -1e9);
    }

    // Standing height of cell (x, y) on layer 0 (the floor or an object on it) or 1 (a bridge hovering over the floor).
    double LayerH(int x, int y, int layer, const std::map<std::pair<int, int>, double>& plat) const
    {
        if (layer == 0) return Eff(x, y, plat);
        const auto it = over_cache.find({ x, y });
        return it == over_cache.end() ? Eff(x, y, plat) : it->second;
    }

    int StepKind1(double from, int cx, int cy, int nx, int ny, double to, const std::map<std::pair<int, int>, double>& plat, double) const
    {
        (void)cx; (void)cy;
        const bool on_object = plat.count({ nx, ny }) && to > H(nx, ny) + 1e-6;
        const double rise = to - from;
        if (rise <= 1e-6) return 1;
        if (!on_object) return rise <= 1.0 + 1e-6 ? 1 : rise <= 2.0 + 1e-6 ? 2 : 0;
        if (rise <= 0.06) return 1;   // (the player no longer steps onto an object: a button needs a jump)
        return rise <= 1.3 ? 2 : 0;   // higher ones need a jump
    }

    // Breadth-first search from the player's cell to the nearest cell satisfying `goal`.
    // Walks up steps of one, jumps up two, drops freely. Returns the cells (start first), or empty.
    template <class Goal>
    std::vector<std::pair<int, int>> Search(Goal goal, int ignore_entity)
    {
        const int sx = search_start.first >= 0 ? search_start.first : r.cursor_x, sy = search_start.first >= 0 ? search_start.second : r.cursor_y;
        std::vector<std::pair<int, int>> none;
        if (!InRoom(sx, sy)) return none;
        std::map<std::pair<int, int>, double> plat;
        const auto blocked = EntityCells(ignore_entity, plat);
        plat_cache = plat;
        // doorways are only entered on purpose
        std::set<std::pair<int, int>> doorway;
        for (const auto& w : r.warps)
            for (int wx = w.x; wx < w.x + w.w; ++wx)
                for (int wy = w.y; wy < w.y + w.h; ++wy) doorway.insert({ wx, wy });
        const int W = r.hm_width, Hh = r.hm_height;
        const std::size_t N = static_cast<std::size_t>(W) * Hh;
        std::vector<int> from(N * 2, -1);
        auto idx = [&](int x, int y) { return (y - OFF) * W + (x - OFF); };
        // Dijkstra-lite: jumps cost more, so use a small priority queue. Nodes are (cell, layer): layer 1 is
        // a bridge (a platform hovering over the floor of the cell), so a corridor under it stays a corridor.
        using Item = std::pair<int, int>;   // cost, node (layer * N + cell index)
        std::vector<Item> heap;
        auto cmp = [](const Item& a, const Item& b) { return a.first > b.first; };
        std::vector<int> best(N * 2, 1 << 30);
        int start_layer = 0;
        {
            const auto ov = over_cache.find({ sx, sy });
            if (ov != over_cache.end() && r.player_z >= ov->second - 0.3) start_layer = 1;
        }
        const int start_node = static_cast<int>(N) * start_layer + idx(sx, sy);
        best[start_node] = 0;
        heap.push_back({ 0, start_node });
        int found = -1;
        static const int ddx[4] = { -1, 0, 1, 0 }, ddy[4] = { 0, -1, 0, 1 };
        while (!heap.empty())
        {
            std::pop_heap(heap.begin(), heap.end(), cmp);
            const auto [cost, node] = heap.back();
            heap.pop_back();
            if (cost > best[node]) continue;
            const int layer = node / static_cast<int>(N), ci = node % static_cast<int>(N);
            const int cx = OFF + ci % W, cy = OFF + ci / W;
            if (goal(cx, cy)) { found = node; break; }
            for (int d = 0; d < 4; ++d)
            {
                const int nx = cx + ddx[d], ny = cy + ddy[d];
                if (!InRoom(nx, ny) || !r.IsWalkable(nx, ny)) continue;
                if (doorway.count({ nx, ny }) && !goal(nx, ny)) continue;
                const int floor_type = r.GetCell(nx, ny) & 0x3F;
                if (floor_type == 7 && !goal(nx, ny)) continue;   // a pit: only to fall on purpose
                const int hazard = (floor_type == 0x10 || floor_type == 0x19) ? 40 : floor_type == 0x29 ? 150 : 0;   // spikes, swamp, lava: crossed only when there is no other way
                if ((floor_type == 0x1A || floor_type == 0x26 || floor_type == 0x27) && goal(nx, ny) && !allow_locked_goal) continue;   // a locked door
                if (blocked.count({ nx, ny }) || (time < temp_block_until && temp_block.count({ nx, ny }))) continue;
                // the first step starts from where the player really is (he may hover over a ledge's edge)
                const bool at_start = cx == sx && cy == sy && search_start.first < 0;
                const bool has_over = over_cache.count({ nx, ny }) > 0;
                for (int nl = 0; nl <= (has_over ? 1 : 0); ++nl)
                {
                    const double from_h = std::max(LayerH(cx, cy, layer, plat), at_start ? r.player_z : -1e9);
                    const double to_h = LayerH(nx, ny, nl, plat);
                    if (nl == 1 && std::abs(from_h - to_h) > 2.0 + 1e-6) continue;   // a bridge is entered from its own level only (a jump up is fine)
                    const int kind = StepKind1(from_h, cx, cy, nx, ny, to_h, plat, -1e9);
                    if (kind == 0) continue;
                    const int step = (kind == 1 ? 1 : 8) + hazard;   // a jump costs more
                    const int ni = static_cast<int>(N) * nl + idx(nx, ny);
                    if (cost + step < best[ni])
                    {
                        best[ni] = cost + step;
                        from[ni] = node;
                        heap.push_back({ cost + step, ni });
                        std::push_heap(heap.begin(), heap.end(), cmp);
                    }
                }
            }
        }
        if (found < 0) return none;
        std::vector<std::pair<int, int>> cells;
        for (int c = found; c >= 0; c = from[c]) { const int ci = c % static_cast<int>(N); cells.push_back({ OFF + ci % W, OFF + ci / W }); }
        std::reverse(cells.begin(), cells.end());
        return cells;
    }

    // ---- steering ----
    // Follows `path` one tick: sets the direction keys, jumping where the path climbs.
    void Follow(const std::vector<std::pair<int, int>>& cells, int& dx, int& dy)
    {
        dx = dy = 0;
        if (cells.size() < 2) return;
        const auto [cx, cy] = cells[0];
        const auto [nx, ny] = cells[1];
        const int ex = nx - cx, ey = ny - cy;
        const double tx = nx + 0.5, ty = ny + 0.5;
        if (ex != 0)
        {
            // along x: first line up on y
            if (std::abs(ty - r.player_y) > 0.12) dy = ty > r.player_y ? 1 : -1;
            else dx = ex;
        }
        else
        {
            if (std::abs(tx - r.player_x) > 0.12) dx = tx > r.player_x ? 1 : -1;
            else dy = ey;
        }
        // jump when the next cell is more than a step higher
        if (StepKind(cx, cy, nx, ny, plat_cache) == 2 && r.grounded && Key(SDL_SCANCODE_LCTRL, 0.5)) {}
    }

    bool Key(SDL_Scancode k, double cooldown)
    {
        if (time < next_key) return false;
        next_key = time + cooldown;
        r.OnKeyDown(k, SDL_KMOD_NONE);
        return true;
    }

    static int FacingToDir(int f, int& dx, int& dy)
    {
        static const int fx[4] = { -1, 0, 1, 0 }, fy[4] = { 0, -1, 0, 1 };
        dx = fx[f];
        dy = fy[f];
        return f;
    }

    // ---- the decision of one tick ----
    void Tick()
    {
        Observe();
        TrackPlatforms();
        static const char* trace = std::getenv("LS_BOT_TRACE");   // "<room>:<entity>": log its position every second
        if (trace && std::fmod(time, 1.0) < DT)
        {
            int tr = 0, te = 0;
            if (std::sscanf(trace, "%d:%d", &tr, &te) == 2 && r.room_number == tr && static_cast<std::size_t>(te) < r.entities.size())
                Log("trace entity %d: (%.2f,%.2f,z %.2f) pc %d; player (%.2f,%.2f,z %.2f)", te, r.entities[te].x, r.entities[te].y, r.entities[te].z,
                    r.entities[te].bh_pc, r.player_x, r.player_y, r.player_z);
        }
        int dx = 0, dy = 0;
        Decide(dx, dy);
        Dodge(dx, dy);
        if (trace && time > 497.0 && time < 497.2) Log("decision dx %d dy %d, ferry stage %d, path %zu, hunt %d, player (%.3f,%.3f) knock %.2f hold %.2f", dx, dy, ferry.stage, path.size(), hunt, r.player_x, r.player_y, r.knock_time, r.input_hold);
        if (g->HasStatus(GameState::STATUS_CONFUSION) && !r.input_locked) { dx = -dx; dy = -dy; }
        r.bot_dx = dx;
        r.bot_dy = dy;
        r.OnUpdate(DT);
        time += DT;
        // Stuck detection: nothing moved for a while although we wanted to.
        if (std::hypot(r.player_x - last_x, r.player_y - last_y) > 0.05 || !r.dialogue_pages.empty() || r.input_locked ||
            r.cutscene_busy > 0.0 || r.swinging || r.chest_open.active || r.menu_open)
        {
            last_x = r.player_x;
            last_y = r.player_y;
            last_progress = time;
        }
    }

    // Logs what changed since the last tick.
    void Observe()
    {
        if (std::getenv("LS_BOT_TICKS") && time - last_tick_log >= 10.0)
        {
            last_tick_log = time;
            double ed0 = 0.0;
            const int en = NearestEnemy(ed0);
            Log("tick: pos (%.2f,%.2f) z %.2f hp %d/%d plan %s hunt %d enemy %d dist %.1f path %zu", r.player_x, r.player_y, r.player_z,
                g->GetHealth(), g->GetMaxHealth(), plan_what.c_str(), hunt, en, ed0, path.size());
        }
        if (r.room_number != seen_room)
        {
            seen_room = r.room_number;
            room_entered = time;
            visited.insert(seen_room);
            path.clear();
            aim.active = false;
            pending_aim.active = false;
            plan_age = 1000;
            last_progress = time;
            extents.clear();
            extents_since.clear();
            ferry = Ferry();
            ++recent_visits[Cur()];
            entry_key = std::get<1>(cur_hop) == Cur() ? cur_hop : std::make_tuple(-1, -1, 0, 0);
            hunt = false;
            if (!room_talkables.count(Cur()) && !(r.shop_info && r.shop_info->kind == "inn"))
            {
                auto& list = room_talkables[Cur()];
                for (std::size_t i = 0; i < r.entities.size(); ++i)
                {
                    const auto& e = r.entities[i];
                    if (e.talks && e.type != 0 && r.entity_table->Has(e.type) && e.dialogue >= 0 &&
                        e.dialogue < static_cast<int>(r.room_characters.size()))
                        list.push_back(static_cast<int>(i));
                }
            }
            Log("ENTER room %d at (%.1f,%.1f)  [%zu warps, %zu entities]", seen_room, r.player_x, r.player_y, r.warps.size(),
                r.entities.size());
        }
        for (int f : g->GetFlags())
            if (!seen_flags.count(f)) { seen_flags.insert(f); if (f < 4096) { recent_visits.clear(); dead_edges.clear(); } Log("FLAG %d (byte %d bit %d) set", f, f / 8, f % 8); }
        for (const auto& [item, n] : g->GetItems())
            if (seen_items[item] != n) { Log("ITEM %d '%s': %d -> %d", item, g->ItemName(item).c_str(), seen_items[item], n); seen_items[item] = n; }
        if (g->GetGold() != seen_gold) { Log("GOLD %d -> %d", seen_gold, g->GetGold()); seen_gold = g->GetGold(); }
        if (g->GetMaxHealth() != seen_maxhp) { Log("MAX HP %d -> %d", seen_maxhp, g->GetMaxHealth()); seen_maxhp = g->GetMaxHealth(); }
        if (!r.dialogue_pages.empty())
        {
            const std::string t = r.dialogue_pages.front().text;
            if (t != seen_dialogue) { seen_dialogue = t; Log("SAYS: %s%s", Flat(t).c_str(), r.dialogue_asks && r.dialogue_pages.size() == 1 ? "  [QUESTION]" : ""); }
        }
        else seen_dialogue.clear();
    }

    int FlagSignature() const { return static_cast<int>(g->GetFlags().size()); }

    bool Dead() { return r.dead; }

    // The player's facing and the spots from which an entity can be talked to / opened.
    // Same geometry as RoomView::StartDialogue and Interact.
    bool SpotFor(const RoomEntity& e, int cx, int cy, bool chest, int& facing_out) const
    {
        static const double fx[4] = { -1, 0, 1, 0 }, fy[4] = { 0, -1, 0, 1 };
        const double half = r.entity_table->GetHitboxBase(e.type) / 16.0 + (chest ? 0.0 : 0.3);
        const double top = e.z + r.entity_table->GetHitboxHeight(e.type) / 16.0;
        const double z = H(cx, cy);
        if (z >= top || z + RoomView::PLAYER_HEIGHT <= e.z) return false;
        for (int f = 0; f < 4; ++f)
        {
            const double reach = RoomView::PLAYER_RADIUS + 0.4;
            const double qx = cx + 0.5 + fx[f] * reach, qy = cy + 0.5 + fy[f] * reach;
            if (std::abs(qx - e.x) <= half && std::abs(qy - e.y) <= half) { facing_out = f; return true; }
        }
        return false;
    }

    // Nearest living enemy that is awake and visible.
    int NearestEnemy(double& dist) const
    {
        int best = -1;
        dist = 1e9;
        for (std::size_t i = 0; i < r.entities.size(); ++i)
        {
            if (!r.IsAliveEnemy(i)) continue;
            const auto& e = r.entities[i];
            const auto* st = r.entity_table->GetEnemy(e.type);
            if (st && st->health == 255) continue;   // indestructible (spiked balls): dodged, not fought
            const double d = std::hypot(e.x - r.player_x, e.y - r.player_y);
            if (d < dist) { dist = d; best = static_cast<int>(i); }
        }
        return best;
    }

    // ---- indestructible hazards (the spiked balls of the Swamp Shrine): keep out of their way ----
    struct Mover { double x = 0, y = 0, t = -1, vx = 0, vy = 0; };
    std::map<int, Mover> movers;
    double dodge_since = -1.0, dodge_off_until = -1.0;
    int dodge_room = -1;

    void Dodge(int& dx, int& dy)
    {
        if (!r.entity_table || r.input_locked || r.cutscene_busy > 0.0 || r.dead) return;
        if (dodge_room != r.room_number) { dodge_room = r.room_number; movers.clear(); dodge_since = -1.0; }
        struct Hz { double x, y, vx, vy, rad; };
        std::vector<Hz> hz;
        for (std::size_t i = 0; i < r.entities.size(); ++i)
        {
            if (!r.IsAliveEnemy(i)) continue;
            const auto* st = r.entity_table->GetEnemy(r.entities[i].type);
            if (!st || st->health != 255 || r.entities[i].behaviour == 0) continue;   // (motionless ones are planned around)
            const auto& e = r.entities[i];
            auto& m = movers[static_cast<int>(i)];
            if (m.t < 0) { m.x = e.x; m.y = e.y; m.t = time; }
            else if (time - m.t >= 0.1)
            {
                const double dt = time - m.t;
                m.vx = (e.x - m.x) / dt;
                m.vy = (e.y - m.y) / dt;
                m.x = e.x; m.y = e.y; m.t = time;
            }
            if (std::hypot(e.x - r.player_x, e.y - r.player_y) > 6.0) continue;
            hz.push_back({ e.x, e.y, m.vx, m.vy, r.entity_table->GetHitboxBase(e.type) / 16.0 + RoomView::PLAYER_RADIUS + 0.3 });
        }
        if (hz.empty()) { dodge_since = -1.0; return; }
        auto danger = [&](int mx, int my) {
            double worst = 1e9;
            for (const auto& h : hz)
                for (double t : { 0.0, 0.2, 0.4, 0.6 })
                {
                    const double len = (mx != 0 && my != 0) ? 0.7071 : 1.0;
                    const double px = r.player_x + mx * len * RoomView::WALK_SPEED * t, py = r.player_y + my * len * RoomView::WALK_SPEED * t;
                    worst = std::min(worst, std::hypot(px - (h.x + h.vx * t), py - (h.y + h.vy * t)) - h.rad);
                }
            return worst;   // < 0: would be touched
        };
        if (time < dodge_off_until) return;
        if (danger(dx, dy) >= 0) { dodge_since = -1.0; return; }
        if (dodge_since < 0) dodge_since = time;
        if (time - dodge_since > 6.0) { dodge_off_until = time + 2.5; dodge_since = -1.0; return; }   // waited long enough: push through
        static const int opt[9][2] = { { 0, 0 }, { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 } };
        double best = -1e9;
        int bx = 0, by = 0;
        for (const auto& o : opt)
        {
            if (o[0] != 0 || o[1] != 0)
            {
                // do not walk into a wall / off a ledge
                const double nx = r.player_x + o[0] * 0.6, ny = r.player_y + o[1] * 0.6;
                const int cx = static_cast<int>(std::floor(nx)), cy = static_cast<int>(std::floor(ny));
                if (!InRoom(cx, cy) || !r.IsWalkable(cx, cy)) continue;
                const int ft = r.GetCell(cx, cy) & 0x3F;
                if (ft == 7 || ft == 0x10 || ft == 0x19 || ft == 0x29) continue;
                if (H(cx, cy) > r.player_z + 0.8) continue;
            }
            double score = std::min(danger(o[0], o[1]), 1.5) * 4.0 + (o[0] * dx + o[1] * dy) * 0.8 - ((o[0] != 0 || o[1] != 0) ? 0.0 : 0.3);
            if (score > best) { best = score; bx = o[0]; by = o[1]; }
        }
        dx = bx; dy = by;
    }

    void Decide(int& dx, int& dy)
    {
        dx = dy = 0;
        if (!g || !r.entity_table) return;
        if (std::getenv("LS_BOT_STAND")) return;   // test aid: the player just stands where he arrived

        // ---- finished? ----
        if (Cur() == goal_room)
        {
            bool boss_alive = false;
            for (std::size_t i = 0; i < r.entities.size(); ++i)
                if (r.IsAliveEnemy(i) && RoomView::IsBossType(r.entities[i].type)) boss_alive = true;
            if (boss_alive) { boss_seen = true; boss_dead_t = time; }
            else if (boss_seen && time - boss_dead_t > 3.0) { Log("The boss of room %d is dead.", goal_room); finished = true; }
        }

        if (r.dead)
        {
            ++deaths;
            Log("DEAD (hp 0) at (%.2f,%.2f) z %.2f. Respawn #%d", r.player_x, r.player_y, r.player_z, deaths);
            for (std::size_t i = 0; i < r.entities.size(); ++i)
            {
                const auto& e = r.entities[i];
                if (!e.hostile || !e.visible || std::hypot(e.x - r.player_x, e.y - r.player_y) > 3.5) continue;
                Log("  near #%zu type %d at (%.2f,%.2f,z %.1f) dist %.2f behaviour %d dying %d", i, e.type, e.x, e.y, e.z,
                    std::hypot(e.x - r.player_x, e.y - r.player_y), e.behaviour, e.dying);
            }
            const double ax0 = r.arrival_x, ay0 = r.arrival_y;
            r.Respawn();
            Log("respawned in room %d: arrival (%.2f,%.2f) -> player (%.2f,%.2f) z %.2f cursor (%d,%d) church %d", r.room_number, ax0, ay0, r.player_x, r.player_y, r.player_z, r.cursor_x, r.cursor_y, r.church_room);
            aim.active = false;
            path.clear();
            return;
        }

        // ---- scripted scenes: nothing to do but wait (Enter skips the intro when it takes too long) ----
        if (r.input_locked || r.cutscene_busy > 0.0 || r.fx_level > 0.9)
        {
            if (!r.dialogue_pages.empty())
            {
                if (r.dialogue_asks && r.dialogue_pages.size() == 1) Key(SDL_SCANCODE_Y, 0.3);
                else Key(SDL_SCANCODE_LSHIFT, 0.3);
            }
            if (r.intro_active && time - last_progress > 400.0) { Log("intro takes too long: skipping it"); r.OnKeyDown(SDL_SCANCODE_RETURN, SDL_KMOD_NONE); last_progress = time; }
            return;
        }
        if (r.menu_open) { r.OnKeyDown(SDL_SCANCODE_TAB, SDL_KMOD_NONE); return; }
        if (r.chest_open.active || r.swinging) return;

        // ---- dialogues ----
        if (!r.dialogue_pages.empty())
        {
            aim.active = false;
            if (r.dialogue_asks && r.dialogue_pages.size() == 1)
            {
                // Questions in an inn or a shop cost money: refuse; everything else: yes.
                const bool pay = r.shop_info != nullptr;
                Key(pay ? SDL_SCANCODE_N : SDL_SCANCODE_Y, 0.3);
            }
            else Key(SDL_SCANCODE_LSHIFT, 0.3);
            return;
        }

        // ---- health ----
        if (g->GetHealth() * 100 < g->GetMaxHealth() * 40 && g->ItemCount(GameState::ITEM_EKEEKE) > 0)
        {
            if (Key(SDL_SCANCODE_E, 0.5)) Log("uses an EkeEke (hp %d/%d)", g->GetHealth(), g->GetMaxHealth());
        }

        // ---- fight ----
        double ed = 0.0;
        const int enemy = NearestEnemy(ed);
        if (hunt && (enemy < 0 || !KillableEnemyExists()))
        {
            hunt = false;   // the room is cleared: the ways out may be open now
            for (auto it = dead_edges.begin(); it != dead_edges.end();)
                it = std::get<0>(it->first) == Cur() ? dead_edges.erase(it) : std::next(it);
            Log("room cleared");
            temp_block.clear();
            temp_block_until = time + 5.0;   // gates take a moment to open: wait before judging the way shut
        }
        if (enemy >= 0 && ed < 5.0)
        {
            Fight(enemy, ed, dx, dy);
            return;
        }
        // floor buttons first: they are usually what opens the way
        if (TryButtons(dx, dy)) return;
        if (enemy >= 0 && hunt)
        {
            Fight(enemy, ed, dx, dy);
            return;
        }

        // ---- pick up what lies around ----
        if (TryPickup(dx, dy)) return;

        // ---- open chests and talk ----
        if (TryInteract(dx, dy)) return;
        if (TryUnlock(dx, dy)) return;
        if (TryIdol(dx, dy)) return;

        // ---- go on towards the goal ----
        GoToGoal(dx, dy);
    }

    bool boss_seen = false;
    double boss_dead_t = 0.0;

    void Fight(int enemy, double dist, int& dx, int& dy)
    {
        const auto& e = r.entities[enemy];
        const double ex = e.x - r.player_x, ey = e.y - r.player_y;
        // face the dominant axis
        int fdx = 0, fdy = 0;
        if (std::abs(ex) >= std::abs(ey)) fdx = ex > 0 ? 1 : -1; else fdy = ey > 0 ? 1 : -1;
        const bool other_level = std::abs(r.player_z - e.z) > 1.2;   // a blow only lands at about the same height
        if (dist > 1.5 || other_level)
        {
            // walk towards it by path (so that walls are respected)
            auto cells = Search([&](int x, int y) { return std::hypot(x + 0.5 - e.x, y + 0.5 - e.y) <= 1.2 && std::abs(H(x, y) - e.z) <= 1.2; }, enemy);
            if (cells.size() >= 2) { path = cells; MoveAlong(dx, dy, "to an enemy"); return; }
            if (cells.size() == 1 && other_level)
            {
                // already on the right cell but still up on the ledge's edge: walk to the cell's middle and drop
                const double tx = cells[0].first + 0.5, ty = cells[0].second + 0.5;
                if (std::abs(tx - r.player_x) > 0.1) dx = tx > r.player_x ? 1 : -1;
                else if (std::abs(ty - r.player_y) > 0.1) dy = ty > r.player_y ? 1 : -1;
                return;
            }
            if (cells.empty())
            {
                // No way to it (it floats over a gap, or lives on another level): go to the nearest
                // place we can reach and let it come.
                std::pair<int, int> best{ -1, -1 };
                double best_d = 1e18;
                Search([&](int x, int y) {
                    const double d = std::hypot(x + 0.5 - e.x, y + 0.5 - e.y);
                    if (d < best_d && (r.GetCell(x, y) & 0x3F) != 7) { best_d = d; best = { x, y }; }
                    return false;
                }, enemy);
                if (best.first >= 0 && (best.first != r.cursor_x || best.second != r.cursor_y))
                {
                    auto to_it = Search([&](int x, int y) { return x == best.first && y == best.second; }, enemy);
                    if (to_it.size() >= 2) { path = to_it; MoveAlong(dx, dy, "towards an enemy"); return; }
                }
                return;   // already as close as it gets: wait for it
            }
            dx = fdx; dy = fdy;
            return;
        }
        // in reach: face it, strike
        dx = fdx; dy = fdy;
        const int want = fdx != 0 ? (fdx < 0 ? 0 : 2) : (fdy < 0 ? 1 : 3);
        if (r.facing == want) Key(SDL_SCANCODE_SPACE, 0.12);
        if (r.facing == want && dist < 1.0) dx = dy = 0;   // no need to push into it
    }

    bool TryPickup(int& dx, int& dy)
    {
        if (r.shop_info && r.shop_info->kind == "shop") return false;
        int best = -1;
        std::vector<std::pair<int, int>> best_cells;
        for (std::size_t i = 0; i < r.entities.size(); ++i)
        {
            const auto& e = r.entities[i];
            if (!e.visible) continue;
            const bool drop = e.pickup_gold > 0 || e.pickup_item >= 0;
            const bool item = e.type >= 0xC0 && r.entity_table->Has(e.type) && !g->IsCollected(r.room_number, static_cast<int>(i));
            if (!drop && !item) continue;
            if (bad_targets.count({ r.room_number, static_cast<int>(i) * 2 })) continue;
            const int tx = static_cast<int>(std::floor(e.x)), ty = static_cast<int>(std::floor(e.y));
            auto cells = Search([&](int x, int y) { return x == tx && y == ty; }, -1);
            if (cells.empty())
            {
                bad_targets.insert({ r.room_number, static_cast<int>(i) * 2 });
                if (!drop) Log("cannot reach the item #%d (type %d) at (%.1f,%.1f)", (int)i, e.type, e.x, e.y);
                continue;
            }
            if (best < 0 || cells.size() < best_cells.size()) { best = static_cast<int>(i); best_cells = cells; }
        }
        if (best < 0) return false;
        if (best_cells.size() < 2)
        {
            // standing on it without taking it (height?): give up on this one
            bad_targets.insert({ r.room_number, best * 2 });
            return false;
        }
        path = best_cells;
        MoveAlong(dx, dy, "pickup");
        return true;
    }

    // Continues along `path` with stuck handling; returns false if stuck (a replan is asked).
    void MoveAlong(int& dx, int& dy, const char* what)
    {
        Follow(path, dx, dy);
        if (time - last_progress > 1.5)
        {
            ++stuck_count;
            const auto nxt = path.size() > 1 ? path[1] : std::make_pair(r.cursor_x, r.cursor_y);
            Log("STUCK (%s) at (%.2f,%.2f) wanting (%d,%d): %s", what, r.player_x, r.player_y, nxt.first, nxt.second,
                r.blocked_text.c_str());
            temp_block.clear();
            temp_block.insert(nxt);
            temp_block_until = time + 4.0;
            last_progress = time;
            path.clear();
        }
    }

    // ---- ferries: a moving platform that links two banks (the pit of room 179) ----
    std::pair<int, int> search_start{ -1, -1 };
    std::map<int, std::array<double, 4>> extents;   // entity -> min x, max x, min y, max y seen in this room
    std::map<int, double> extents_since;
    struct Ferry
    {
        int stage = 0, ent = -1;
        bool axis_x = true;
        double lo = 0, hi = 0, fixed = 0, since = 0;
        std::pair<int, int> board{ -1, -1 }, land{ -1, -1 };
        bool board_at_lo = true;
        int dir_x = 0, dir_y = 0;
    } ferry;

    void TrackPlatforms()
    {
        for (std::size_t i = 0; i < r.entities.size(); ++i)
        {
            const auto& e = r.entities[i];
            if (!e.solid || !e.visible || e.hostile || e.talks || e.type >= 0xC0 || !r.entity_table->Has(e.type) || r.entity_table->GetEnemy(e.type)) continue;
            if (e.bh_pc < 0 && !e.walking) continue;
            auto it = extents.find(static_cast<int>(i));
            if (it == extents.end()) { extents[static_cast<int>(i)] = { e.x, e.x, e.y, e.y }; extents_since[static_cast<int>(i)] = time; continue; }
            it->second[0] = std::min(it->second[0], e.x); it->second[1] = std::max(it->second[1], e.x);
            it->second[2] = std::min(it->second[2], e.y); it->second[3] = std::max(it->second[3], e.y);
        }
    }

    // Rides a moving platform to a bank the walking paths do not reach. Returns true while it is at work.
    template <class Goal>
    bool TryFerry(int& dx, int& dy, Goal goal)
    {
        dx = dy = 0;
        if (ferry.stage == 0 || ferry.stage == 2 || ferry.stage == 4) last_progress = time;   // waiting on purpose is not being stuck
        if (ferry.stage == 0)
        {
            int best = -1;
            for (const auto& [i, ex] : extents)
            {
                const double spread = std::max(ex[1] - ex[0], ex[3] - ex[2]);
                if (spread > 1.5 && time - extents_since[i] > 3.0 && static_cast<std::size_t>(i) < r.entities.size()) { best = i; break; }
            }
            if (best < 0) return !extents.empty() && time - room_entered < 25.0;   // still watching it move
            const auto& e = r.entities[best];
            const auto ex = extents[best];
            ferry.ent = best;
            ferry.axis_x = (ex[1] - ex[0]) >= (ex[3] - ex[2]);
            ferry.lo = ferry.axis_x ? ex[0] : ex[2];
            ferry.hi = ferry.axis_x ? ex[1] : ex[3];
            ferry.fixed = ferry.axis_x ? e.y : e.x;
            const double top = e.z + r.entity_table->GetHitboxHeight(e.type) / 16.0;
            const double half_p = r.entity_table->GetHitboxBase(e.type) / 16.0;
            std::set<std::pair<int, int>> region;
            Search([&](int x, int y) { region.insert({ x, y }); return false; }, -1);
            auto seg_dist = [&](int cx, int cy, double axis_at) {
                const double ca = ferry.axis_x ? cx + 0.5 : cy + 0.5, co = ferry.axis_x ? cy + 0.5 : cx + 0.5;
                return std::hypot(ca - axis_at, co - ferry.fixed);
            };
            double best_b = 1e9, best_l = 1e9;
            for (int cy = OFF; cy < OFF + r.hm_height; ++cy)
                for (int cx = OFF; cx < OFF + r.hm_width; ++cx)
                {
                    if (!r.IsWalkable(cx, cy) || std::abs(H(cx, cy) - top) > 1.2 || (r.GetCell(cx, cy) & 0x3F) == 7) continue;
                    const bool mine = region.count({ cx, cy }) > 0;
                    for (int end = 0; end < 2; ++end)
                    {
                        const double at = end == 0 ? ferry.lo : ferry.hi;
                        const double d = seg_dist(cx, cy, at);
                        if (d - half_p > 2.4) continue;   // from the platform's edge: a jump of about two cells
                        if (mine && d < best_b) { best_b = d; ferry.board = { cx, cy }; ferry.board_at_lo = end == 0; }
                        if (!mine && d < best_l)
                        {
                            // the far bank must lead to where we want to go
                            // the far bank must be somewhere new: a region of its own, with room to move
                            search_start = { cx, cy };
                            int size = 0;
                            const auto onward = Search([&](int x, int y) { ++size; return goal(x, y); }, -1);
                            search_start = { -1, -1 };
                            if (!onward.empty() || size >= 8) { best_l = d; ferry.land = { cx, cy }; }
                        }
                    }
                }
            if (ferry.board.first < 0 || ferry.land.first < 0)
            {
                Log("ferry: no crossing found (platform #%d, %s %.1f..%.1f at %.1f, top %.1f; boarding %s, landing %s)", best, ferry.axis_x ? "x" : "y", ferry.lo, ferry.hi, ferry.fixed, top,
                    ferry.board.first < 0 ? "none" : "ok", ferry.land.first < 0 ? "none" : "ok");
                ferry.stage = -1;
                return false;
            }
            Log("ferry: platform #%d along %s between %.1f and %.1f; boards at (%d,%d), lands at (%d,%d)", best, ferry.axis_x ? "x" : "y", ferry.lo, ferry.hi,
                ferry.board.first, ferry.board.second, ferry.land.first, ferry.land.second);
            ferry.stage = 1;
        }
        if (ferry.stage < 0) return false;
        const auto& e = r.entities[ferry.ent];
        const double axis_now = ferry.axis_x ? e.x : e.y;
        switch (ferry.stage)
        {
        case 1:   // walk to the boarding cell
        {
            if (r.cursor_x == ferry.board.first && r.cursor_y == ferry.board.second && r.grounded) { ferry.stage = 2; ferry.since = time; return true; }
            auto cells = Search([&](int x, int y) { return x == ferry.board.first && y == ferry.board.second; }, -1);
            if (cells.size() < 2) { ferry.stage = -1; return false; }
            path = cells;
            MoveAlong(dx, dy, "to the ferry");
            if (std::getenv("LS_BOT_TRACE") && time > 497.0 && time < 497.05) Log("ferry walk: %zu cells, first (%d,%d) next (%d,%d), dx %d dy %d", cells.size(), cells[0].first, cells[0].second, cells[1].first, cells[1].second, dx, dy);
            return true;
        }
        case 2:   // wait for the platform to come alongside, then jump onto it
        {
            const double bank_axis = ferry.axis_x ? ferry.board.first + 0.5 : ferry.board.second + 0.5;
            const double end_at = std::min(ferry.hi, std::max(ferry.lo, bank_axis));
            if (std::abs(axis_now - end_at) < 0.6 && r.grounded)
            {
                // towards the platform: the larger of the two distances decides the direction
                const double ex = e.x - r.player_x, ey = e.y - r.player_y;
                ferry.dir_x = std::abs(ex) >= std::abs(ey) ? (ex > 0 ? 1 : -1) : 0;
                ferry.dir_y = std::abs(ex) >= std::abs(ey) ? 0 : (ey > 0 ? 1 : -1);
                r.OnKeyDown(SDL_SCANCODE_LCTRL, SDL_KMOD_NONE);
                ferry.stage = 3; ferry.since = time;
            }
            return true;
        }
        case 3:   // in the air / just landed on it
            dx = ferry.dir_x; dy = ferry.dir_y;
            if (r.support_entity == ferry.ent && r.grounded) { ferry.stage = 4; ferry.since = time; dx = dy = 0; return true; }
            if (time - ferry.since > 1.6) { ferry.stage = 2; ferry.since = time; dx = dy = 0; }
            return true;
        case 4:   // ride it to the far end
        {
            const double land_axis = ferry.axis_x ? ferry.land.first + 0.5 : ferry.land.second + 0.5;
            const double far_end = std::min(ferry.hi, std::max(ferry.lo, land_axis));
            if (r.support_entity != ferry.ent && r.grounded) { ferry.stage = 2; return true; }
            if (std::abs(axis_now - far_end) < 0.7)
            {
                const double ex = ferry.land.first + 0.5 - r.player_x, ey = ferry.land.second + 0.5 - r.player_y;
                ferry.dir_x = std::abs(ex) >= std::abs(ey) ? (ex > 0 ? 1 : -1) : 0;
                ferry.dir_y = std::abs(ex) >= std::abs(ey) ? 0 : (ey > 0 ? 1 : -1);
                r.OnKeyDown(SDL_SCANCODE_LCTRL, SDL_KMOD_NONE);
                ferry.stage = 5; ferry.since = time;
            }
            return true;
        }
        case 5:   // jump towards the far bank
            dx = ferry.dir_x; dy = ferry.dir_y;
            if (r.grounded && r.support_entity < 0 && time - ferry.since > 0.2)
            {
                Log("ferry: crossed, now at (%d,%d)", r.cursor_x, r.cursor_y);
                ferry.stage = -1;   // done: normal walking takes over from the new bank
                return false;
            }
            if (time - ferry.since > 2.0) { ferry.stage = 2; ferry.since = time; dx = dy = 0; }
            return true;
        }
        return false;
    }

    // ---- floor buttons: low objects whose script waits for somebody standing on them ----
    std::set<std::pair<int, int>> pressed;   // (room, entity)
    double pressing_since = -1.0;

    bool IsButton(std::size_t i) const
    {
        if (!r.IsPlatform(i) || !r.behaviours) return false;
        const auto& e = r.entities[i];
        const auto* script = r.behaviours->Script(e.behaviour);
        if (!script || script->size() < 2 || (*script)[0] != 0x36 || (*script)[1] != 1) return false;
        return r.entity_table->GetHitboxHeight(e.type) / 16.0 <= 0.6;
    }

    bool TryButtons(int& dx, int& dy)
    {
        if (std::getenv("LS_BOT_TRACE") && std::fmod(time, 20.0) < DT)
            for (std::size_t i = 0; i < r.entities.size(); ++i) if (IsButton(i)) Log("button candidate #%zu pressed=%d", i, static_cast<int>(pressed.count({ r.room_number, static_cast<int>(i) })));
        for (std::size_t i = 0; i < r.entities.size(); ++i)
        {
            if (!IsButton(i) || pressed.count({ r.room_number, static_cast<int>(i) })) continue;
            const auto& e = r.entities[i];
            if (r.support_entity == static_cast<int>(i))
            {
                if (pressing_since < 0) { pressing_since = time; Log("steps on the button #%d (type %d)", static_cast<int>(i), e.type); }
                last_progress = time;
                if (time - pressing_since > 1.2) { pressed.insert({ r.room_number, static_cast<int>(i) }); pressing_since = -1.0; }
                return true;
            }
            const int tx = static_cast<int>(std::floor(e.x)), ty = static_cast<int>(std::floor(e.y));
            auto cells = Search([&](int x, int y) { return x == tx && y == ty; }, static_cast<int>(i));
            if (cells.size() < 2) { if (cells.empty()) { if (logged_unreachable.insert({ r.room_number, 1000 + static_cast<int>(i) }).second) Log("cannot reach the button #%d at (%.1f,%.1f)", static_cast<int>(i), e.x, e.y); continue; } 
                // on its cell already: step to the middle and wait for the player to settle on it
                const double mx = tx + 0.5 - r.player_x, my = ty + 0.5 - r.player_y;
                if (std::abs(mx) > 0.08) dx = mx > 0 ? 1 : -1; else if (std::abs(my) > 0.08) dy = my > 0 ? 1 : -1;
                if (pressing_since < 0) { pressing_since = time; Log("on the button's cell: player (%.2f,%.2f,z %.3f) grounded %d support %d; button (%.2f,%.2f,z %.3f)", r.player_x, r.player_y, r.player_z, r.grounded, r.support_entity, e.x, e.y, e.z); }
                last_progress = time;
                if (time - pressing_since > 2.5) { pressed.insert({ r.room_number, static_cast<int>(i) }); pressing_since = -1.0; }
                return true; }
            path = cells;
            MoveAlong(dx, dy, "to a button");
            return true;
        }
        return false;
    }

    // The Idol Stone is held up in front of the Swamp Shrine's door (room 433, x <= 15, y 25..27).
    double idol_wait = -1.0;
    bool TryIdol(int& dx, int& dy)
    {
        if (g->ItemCount(RoomView::ITEM_IDOL_STONE) <= 0 || g->IsFlagSet(6) || r.room_number != 433) return false;
        if (r.cursor_x <= 15 && r.cursor_y >= 25 && r.cursor_y <= 27 && r.grounded)
        {
            if (idol_wait < 0 || time - idol_wait > 6.0)
            {
                idol_wait = time;
                Log("uses the Idol Stone at (%d,%d)", r.cursor_x, r.cursor_y);
                Key(SDL_SCANCODE_I, 0.5);
            }
            return true;
        }
        auto cells = Search([&](int x, int y) { return x <= 15 && y >= 25 && y <= 27 && !(r.GetCell(x, y) & 0x4000); }, -1);
        if (cells.size() < 2) return false;
        path = cells;
        MoveAlong(dx, dy, "to the shrine door");
        return true;
    }

    // With a Key in the bag: go and use it on the locked doors of the room (the player stands on the door's floor).
    bool TryUnlock(int& dx, int& dy)
    {
        if (g->ItemCount(GameState::ITEM_KEY) <= 0) return false;
        const int here = r.GetCell(r.cursor_x, r.cursor_y) & 0x3F;
        if (here >= 26 && here <= 40)
        {
            auto& tries = unlock_tries[{ r.room_number, r.cursor_x, r.cursor_y }];
            if (tries < 2 && r.grounded && Key(SDL_SCANCODE_K, 0.6))
            {
                ++tries;
                Log("uses a Key at (%d,%d), floor type %d", r.cursor_x, r.cursor_y, here);
                return true;
            }
            if (tries < 2) return true;
        }
        // the door entity itself does not block the way to its floor cell
        int door = -1;
        for (const auto& fr : r.flag_rules)
            if (fr.kind == RoomView::FlagRule::HIDE && fr.when_set && !g->IsFlagSet(fr.flag) &&
                static_cast<std::size_t>(fr.entity) < r.entities.size())
                door = fr.entity;
        allow_locked_goal = true;
        auto cells = Search([&](int x, int y) {
            const int t = r.GetCell(x, y) & 0x3F;
            if (t < 26 || t > 40) return false;
            const auto it = unlock_tries.find({ r.room_number, x, y });
            return it == unlock_tries.end() || it->second < 2;
        }, door);
        allow_locked_goal = false;
        if (cells.size() < 2) return false;
        path = cells;
        MoveAlong(dx, dy, "to a locked door");
        return true;
    }

    bool TryInteract(int& dx, int& dy)
    {
        if (aim.active) { return Aiming(dx, dy); }
        const bool in_shop = r.shop_info && r.shop_info->kind == "shop";
        int best = -1;
        bool best_chest = false;
        std::vector<std::pair<int, int>> best_cells;
        int best_facing = 0;
        for (std::size_t i = 0; i < r.entities.size(); ++i)
        {
            const auto& e = r.entities[i];
            if (!e.visible || !r.entity_table->Has(e.type) || g->IsCollected(r.room_number, static_cast<int>(i))) continue;
            const bool chest = e.type == 18;
            if (chest)
            {
                if (g->IsChestOpened(r.room_number, static_cast<int>(i))) continue;
            }
            else
            {
                if (!e.talks || e.type == 0 || in_shop) continue;
                if (r.shop_info && (r.shop_info->kind == "inn")) continue;   // costs money
                if (e.dialogue < 0 || e.dialogue >= static_cast<int>(r.room_characters.size())) continue;
                const auto key = std::make_tuple(r.room_number, static_cast<int>(i), FlagSignature());
                if (talked.count(key) || talk_tries[key] >= 3) continue;
            }
            if (bad_targets.count({ r.room_number, static_cast<int>(i) * 2 + (chest ? 1 : 0) })) continue;
            // nearest spot
            std::map<std::pair<int, int>, int> spot_facing;
            auto cells = Search([&](int x, int y) {
                int f = 0;
                if (SpotFor(e, x, y, chest, f)) { spot_facing[{ x, y }] = f; return true; }
                return false;
            }, static_cast<int>(i));
            if (cells.empty())
            {
                bad_targets.insert({ r.room_number, static_cast<int>(i) * 2 + (chest ? 1 : 0) });
                if (logged_unreachable.insert({ r.room_number, static_cast<int>(i) }).second)
                Log("cannot reach %s #%d (type %d) at (%.1f,%.1f)", chest ? "the chest" : "the character", (int)i, e.type, e.x, e.y);
                continue;
            }
            if (best < 0 || cells.size() < best_cells.size())
            {
                best = static_cast<int>(i);
                best_chest = chest;
                best_cells = cells;
                best_facing = spot_facing[cells.back()];
            }
        }
        if (best < 0) return false;
        if (best_cells.size() < 2)
        {
            aim = { true, best, best_facing, 0, best_chest };
            return Aiming(dx, dy);
        }
        path = best_cells;
        pending_aim = { true, best, best_facing, 0, best_chest };
        MoveAlong(dx, dy, best_chest ? "to a chest" : "to a character");
        return true;
    }

    Aim pending_aim;
    std::tuple<int, int, int, int> cur_hop{ -1, -1, 0, 0 };     // the warp being walked to
    std::tuple<int, int, int, int> entry_key{ -1, -1, 0, 0 };   // the warp this room was entered by
    bool hunt = false;                                          // the way is shut: kill the enemies to open it
    bool allow_locked_goal = false;
    std::map<std::tuple<int, int, int>, int> unlock_tries;
    std::map<std::pair<int, int>, int> aim_retries;
    // ---- story tours: when the way on is closed, go and talk again to people met earlier ----
    std::map<int, std::vector<int>> room_talkables;   // room -> talking entities seen there
    std::map<int, int> recent_visits;                  // room -> entries since the last new flag
    std::set<std::pair<int, int>> tour_done;           // (room, flag signature) already revisited
    int tour_room = -1;
    std::set<std::pair<int, int>> logged_unreachable;

    // Standing at a spot: turn towards the entity and press the action key.
    bool Aiming(int& dx, int& dy)
    {
        if (aim.entity < 0 || static_cast<std::size_t>(aim.entity) >= r.entities.size()) { aim.active = false; return false; }
        if (nudge_ticks > 0) { --nudge_ticks; dx = nudge_dx; dy = nudge_dy; if (nudge_ticks % 3 == 0) Log("  nudge pos (%.2f,%.2f)", r.player_x, r.player_y); return true; }
        FacingToDir(aim.facing, dx, dy);
        ++aim.ticks;
        if (aim.ticks > 12 && r.facing == aim.facing && r.grounded && r.knock_time <= 0.0)
        {
            const auto& e = r.entities[aim.entity];
            const auto key = std::make_tuple(r.room_number, aim.entity, FlagSignature());
            if (!aim.chest) { talked.insert(key); ++talk_tries[key]; }
            Log("%s #%d (type %d)", aim.chest ? "opens the chest" : "talks to", aim.entity, e.type);
            dx = dy = 0;
            r.OnKeyDown(SDL_SCANCODE_LSHIFT, SDL_KMOD_NONE);
            if (aim.chest && r.dialogue_pages.empty() && !r.chest_open.active)
            {
                Log("the chest did not open: player (%.2f,%.2f,z %.1f) facing %d, chest (%.2f,%.2f,z %.1f)", r.player_x, r.player_y, r.player_z,
                    r.facing, e.x, e.y, e.z);
                if (++aim_retries[{ r.room_number, aim.entity }] < 4)
                {
                    // Not where the geometry said: look at where we really stand, turn, and try again.
                    static const double fx[4] = { -1, 0, 1, 0 }, fy[4] = { 0, -1, 0, 1 };
                    const double half = r.entity_table->GetHitboxBase(e.type) / 16.0;
                    int f2 = -1;
                    for (int f = 0; f < 4 && f2 < 0; ++f)
                    {
                        const double reach = RoomView::PLAYER_RADIUS + 0.4;
                        if (std::abs(r.player_x + fx[f] * reach - e.x) <= half && std::abs(r.player_y + fy[f] * reach - e.y) <= half) f2 = f;
                    }
                    if (f2 < 0)
                    {
                        const double ex = e.x - r.player_x, ey = e.y - r.player_y;
                        f2 = std::abs(ex) >= std::abs(ey) ? (ex < 0 ? 0 : 2) : (ey < 0 ? 1 : 3);
                    }
                    // (the chest may be off to one side of where we stand: slide towards it first)
                    const double lat = (f2 % 2 == 0) ? e.y - r.player_y : e.x - r.player_x;
                    if (std::abs(lat) > 0.3)
                    {
                        nudge_ticks = static_cast<int>((std::abs(lat) - 0.1) / (RoomView::WALK_SPEED * DT)) + 1;
                        nudge_dx = (f2 % 2 == 0) ? 0 : (lat > 0 ? 1 : -1);
                        nudge_dy = (f2 % 2 == 0) ? (lat > 0 ? 1 : -1) : 0;
                        Log("nudges %d ticks (%d,%d), lateral %.2f", nudge_ticks, nudge_dx, nudge_dy, lat);
                    }
                    aim = { true, aim.entity, f2, 0, true };
                    return true;
                }
                bad_targets.insert({ r.room_number, aim.entity * 2 + 1 });
            }
            aim.active = false;
            return true;
        }
        if (aim.ticks > 90)
        {
            Log("could not face entity #%d", aim.entity);
            bad_targets.insert({ r.room_number, aim.entity * 2 + (aim.chest ? 1 : 0) });
            aim.active = false;
        }
        return true;
    }

    bool KillableEnemyExists() const
    {
        for (std::size_t i = 0; i < r.entities.size(); ++i)
        {
            if (!r.IsAliveEnemy(i)) continue;
            const auto* st = r.entity_table->GetEnemy(r.entities[i].type);
            if (st && st->health != 255) return true;
        }
        return false;
    }

    // Does the room hold people to talk to again, now that the story has moved?
    bool HasPendingTalk(int room) const
    {
        const auto it = room_talkables.find(room);
        if (it == room_talkables.end()) return false;
        for (int i : it->second)
        {
            const auto key = std::make_tuple(room, i, FlagSignature());
            const auto tries = talk_tries.find(key);
            if (!talked.count(key) && (tries == talk_tries.end() || tries->second < 3)) return true;
        }
        return false;
    }

    // The nearest other room (by warps) where somebody is worth talking to again.
    int PickTourRoom()
    {
        std::map<int, int> dist{ { Cur(), 0 } };
        std::deque<int> q{ Cur() };
        while (!q.empty())
        {
            const int u = q.front();
            q.pop_front();
            if (u != Cur() && !tour_done.count({ u, FlagSignature() }) && HasPendingTalk(u)) return u;
            for (const auto& e : graph[u])
            {
                if (dist.count(e.to) || !visited.count(e.to)) continue;
                dist[e.to] = dist[u] + 1;
                q.push_back(e.to);
            }
        }
        return -1;
    }

    void GoToGoal(int& dx, int& dy)
    {
        // The way on is closed, or we keep going round in circles: revisit people.
        if (tour_room < 0 && Cur() != goal_room && (!NextHop(Cur(), goal_room) || recent_visits[Cur()] >= 4))
        {
            tour_room = PickTourRoom();
            if (tour_room >= 0) Log("the way on is closed: goes back to room %d to talk again", tour_room);
        }
        if (tour_room >= 0 && Cur() == tour_room)
        {
            // nothing left to say here (TryInteract found nothing): the tour of this room is over
            tour_done.insert({ tour_room, FlagSignature() });
            tour_room = -1;
            recent_visits.clear();
        }
        const int target_room = tour_room >= 0 ? tour_room : goal_room;
        if (pending_aim.active && path.size() < 2) { aim = pending_aim; pending_aim.active = false; Aiming(dx, dy); return; }
        const Edge* hop = NextHop(Cur(), target_room);
        if (!hop)
        {
            if (Cur() == goal_room && tour_room < 0)
            {
                // Inside the goal room: look for the boss and wait for it (it fights us).
                if (plan_what != "wait") { plan_what = "wait"; Log("in the goal room, waiting"); }
                if (goal_arrived < 0) goal_arrived = time;
                if (!boss_seen && time - goal_arrived > 8.0) { AdvanceGoal(); return; }
                WanderToBoss(dx, dy);
                return;
            }
            if (plan_what != "noroute") { plan_what = "noroute"; Log("NO ROUTE from room %d to %d (dead edges %zu)", Cur(), goal_room, dead_edges.size()); }
            Idle(dx, dy);
            return;
        }
        auto cells = Search([&](int x, int y) {
            if (hop->fall) return (r.GetCell(x, y) & 0x3F) == 7;
            return x >= hop->x && x < hop->x + hop->w && y >= hop->y && y < hop->y + hop->h; }, -1);
        if (cells.empty() && time < temp_block_until) return;   // a cell was blocked a moment ago: wait
        if (cells.empty() && ferry.stage >= 0)
        {
            const bool fall_goal = hop->fall;
            auto goal_fn = [&](int x, int y) {
                if (fall_goal) return (r.GetCell(x, y) & 0x3F) == 7;
                return x >= hop->x && x < hop->x + hop->w && y >= hop->y && y < hop->y + hop->h; };
            if (TryFerry(dx, dy, goal_fn)) return;
        }
        if (cells.empty())
        {
            {
                std::map<std::pair<int, int>, double> plat;
                const auto blocked = EntityCells(-1, plat);
                std::string cells_blocked;
                for (const auto& c : blocked) cells_blocked += "(" + std::to_string(c.first) + "," + std::to_string(c.second) + ")";
                int reach = 0;
                std::string far_cells;
                static const bool all_cells = std::getenv("LS_BOT_TICKS") != nullptr;
                Search([&](int x, int y) { ++reach; if (all_cells || reach % 7 == 0 || reach < 40) far_cells += "(" + std::to_string(x) + "," + std::to_string(y) + ")"; return false; }, -1);
                if (reach < 10)
                    for (int ddy = -2; ddy <= 2; ++ddy)
                    {
                        std::string row;
                        for (int ddx2 = -3; ddx2 <= 3; ++ddx2)
                        {
                            const int qx = r.cursor_x + ddx2, qy = r.cursor_y + ddy;
                            char b[40];
                            std::snprintf(b, sizeof b, "%s%04X ", InRoom(qx, qy) && r.IsWalkable(qx, qy) ? "" : "#", InRoom(qx, qy) ? r.GetCell(qx, qy) & 0xFFFF : 0);
                            row += b;
                        }
                        Log("  y%d: %s", r.cursor_y + ddy, row.c_str());
                    }
                Log("reachable cells: %d %s; temp blocks %zu until %.1f (now %.1f)", reach, far_cells.c_str(), temp_block.size(), temp_block_until, time);
                Log("cannot reach the warp to room %d at (%d,%d): giving the edge up [player cell (%d,%d) z %.2f; blocked by entities: %s]", hop->to,
                    hop->x, hop->y, r.cursor_x, r.cursor_y, r.player_z, cells_blocked.c_str());
            }
            dead_edges[{ hop->from, hop->to, hop->x, hop->y }] = time;
            // Is there any way out of the region we can walk in, other than back where we came from?
            bool other_exit = false;
            {
                std::set<std::pair<int, int>> region;
                Search([&](int x, int y) { region.insert({ x, y }); return false; }, -1);
                for (const auto& w : r.warps)
                {
                    if (w.dest_room == std::get<0>(entry_key)) continue;
                    for (int cx = w.x; cx < w.x + w.w && !other_exit; ++cx)
                        for (int cy = w.y; cy < w.y + w.h && !other_exit; ++cy)
                            if (region.count({ cx, cy })) other_exit = true;
                }
            }
            if (time - room_entered < 5.0 && std::get<0>(entry_key) >= 0 && !other_exit)
            {
                // Just arrived and already stuck: this was the wrong one of two ways in.
                dead_edges[entry_key] = time;
                Log("this entrance leads to a dead end: avoids it");
            }
            else if (KillableEnemyExists() && (!NextHop(Cur(), goal_room) || [&] {
                         for (const auto& e : r.entities) if ((e.type == 103 || e.type == 104) && e.visible) return true;
                         return false; }()))   // no other way, or gates that open when the room is cleared: clear the room
            {
                hunt = true;
                Log("the way is shut and enemies are about: hunts them");
            }
            return;
        }
        cur_hop = std::make_tuple(hop->from, hop->to, hop->x, hop->y);
        if (plan_what != "goto" + std::to_string(hop->to)) { plan_what = "goto" + std::to_string(hop->to); Log("heads for room %d via %s (%d,%d)", hop->to, hop->fall ? "a pit" : "the warp", hop->x, hop->y); }
        path = cells;
        const bool centred = std::abs(r.cursor_x + 0.5 - r.player_x) <= 0.05 && std::abs(r.cursor_y + 0.5 - r.player_y) <= 0.05 &&
                             std::abs(r.player_z - H(r.cursor_x, r.cursor_y)) <= 0.75;   // really standing in the doorway
        if (path.size() < 2 && (hop->fall || (time - room_entered > 3.0 && !centred)))
        {
            // on the pit's edge: walk right in, to the cell's centre
            const double tx = r.cursor_x + 0.5, ty = r.cursor_y + 0.5;
            if (std::abs(tx - r.player_x) > 0.05) dx = tx > r.player_x ? 1 : -1;
            else if (std::abs(ty - r.player_y) > 0.05) dy = ty > r.player_y ? 1 : -1;
            return;
        }
        if (path.size() < 2)
        {
            // already in the rectangle: step off and on again (the warp is armed by leaving)
            // (to a walkable neighbour outside the rectangle, the one behind us first)
            static const int ndx[4] = { -1, 0, 1, 0 }, ndy[4] = { 0, -1, 0, 1 };
            std::map<std::pair<int, int>, double> plat;
            const auto blocked = EntityCells(-1, plat);
            if (step_off_room != r.room_number || (step_off.first == r.cursor_x && step_off.second == r.cursor_y)) step_off = { -1, -1 };
            if (step_off.first >= 0 && std::abs(step_off.first - r.cursor_x) + std::abs(step_off.second - r.cursor_y) == 1)
            {
                path = { { r.cursor_x, r.cursor_y }, step_off };
                MoveAlong(dx, dy, "off the warp");
                return;
            }
            for (int k = 0; k < 4; ++k)
            {
                const int d = (((r.facing + 2) & 3) + k) & 3;
                const int nx = r.cursor_x + ndx[d], ny = r.cursor_y + ndy[d];
                if (!InRoom(nx, ny) || !r.IsWalkable(nx, ny) || blocked.count({ nx, ny })) continue;
                if (nx >= hop->x && nx < hop->x + hop->w && ny >= hop->y && ny < hop->y + hop->h) continue;
                if (StepKind(r.cursor_x, r.cursor_y, nx, ny, plat) != 1 || H(nx, ny) != H(r.cursor_x, r.cursor_y)) continue;   // level ground only
                const int ft = r.GetCell(nx, ny) & 0x3F;
                if (ft == 7 || ft == 0x10 || ft == 0x19 || ft == 0x29) continue;
                path = { { r.cursor_x, r.cursor_y }, { nx, ny } };
                step_off = { nx, ny };
                step_off_room = r.room_number;
                MoveAlong(dx, dy, "off the warp");
                return;
            }
            int fx = 0, fy = 0;
            FacingToDir((r.facing + 2) & 3, fx, fy);
            dx = fx; dy = fy;
            return;
        }
        MoveAlong(dx, dy, "to the warp");
        if (path.empty() && stuck_count > 6)
        {
            // The way is blocked for good: avoid this warp
            const auto k = std::make_tuple(hop->from, hop->to, hop->x, hop->y);
            if (++edge_failures[k] >= 3)
            {
                Log("giving up the warp to room %d", hop->to);
                dead_edges[k] = time;
            }
            stuck_count = 0;
        }
    }

    void WanderToBoss(int& dx, int& dy)
    {
        for (std::size_t i = 0; i < r.entities.size(); ++i)
        {
            const auto& e = r.entities[i];
            if (!RoomView::IsBossType(e.type) || !e.visible || e.dying) continue;
            auto cells = Search([&](int x, int y) { return std::hypot(x + 0.5 - e.x, y + 0.5 - e.y) <= 1.4; }, static_cast<int>(i));
            if (cells.size() >= 2) { path = cells; MoveAlong(dx, dy, "to the boss"); }
            return;
        }
        Idle(dx, dy);
    }

    void Idle(int& dx, int& dy)
    {
        // Keep still; try talking again from time to time (a new flag may have changed what people say).
        dx = dy = 0;
        if (time - last_progress > 30.0)
        {
            talked.clear();
            bad_targets.clear();
            last_progress = time;
        }
    }

    void Snapshot() { seen_room = -1; }
};

#endif // _BOT_H_
