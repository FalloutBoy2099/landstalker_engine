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
            Log("state: locked %d intro %d busy %.2f fx %.2f warp_pending %d dialogue %zu cutscene_ops %zu", r.input_locked, r.intro_active,
                r.cutscene_busy, r.fx_level, r.warp_pending, r.dialogue_pages.size(), r.cutscene_ops.size());
            Log("player (%.2f,%.2f) z %.2f grounded %d support %d cell %04X floor %d blocked: %s", r.player_x, r.player_y, r.player_z, r.grounded,
                r.support_entity, r.GetCell(r.cursor_x, r.cursor_y), r.FloorHeight(r.cursor_x, r.cursor_y), r.blocked_text.c_str());
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
        if (!r.entity_table) return out;
        for (std::size_t i = 0; i < r.entities.size(); ++i)
        {
            const auto& e = r.entities[i];
            if (static_cast<int>(i) == ignore) continue;
            if (!e.solid || !e.visible || !r.entity_table->Has(e.type) || e.type >= 0xC0) continue;
            if (r.IsAliveEnemy(i)) continue;
            if (g->IsCollected(r.room_number, static_cast<int>(i))) continue;
            const bool platform = !e.hostile && !e.talks && !e.dying && !r.entity_table->GetEnemy(e.type);
            const double half = r.entity_table->GetHitboxBase(e.type) / 16.0;
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
                        if (platform) { auto& t = plat[{ x, y }]; t = std::max(t, top); }
                        else if (H(x, y) < top && H(x, y) + RoomView::PLAYER_HEIGHT > e.z) out.insert({ x, y });
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
    int StepKind(int cx, int cy, int nx, int ny, const std::map<std::pair<int, int>, double>& plat) const
    {
        const double from = Eff(cx, cy, plat), to = Eff(nx, ny, plat);
        const bool on_object = plat.count({ nx, ny }) && to > H(nx, ny) + 1e-6;
        const double rise = to - from;
        if (rise <= 1e-6) return 1;
        if (!on_object) return rise <= 1.0 + 1e-6 ? 1 : rise <= 2.0 + 1e-6 ? 2 : 0;
        return rise <= 1.3 ? 2 : 0;   // objects are not stepped on: jump
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
        const int W = r.hm_width, Hh = r.hm_height;
        std::vector<int> dist(static_cast<std::size_t>(W) * Hh, -1);
        std::vector<int> from(static_cast<std::size_t>(W) * Hh, -1);
        auto idx = [&](int x, int y) { return (y - OFF) * W + (x - OFF); };
        // Dijkstra-lite: jumps cost more, so use a small priority queue.
        using Item = std::pair<int, int>;   // cost, cell index
        std::vector<Item> heap;
        auto cmp = [](const Item& a, const Item& b) { return a.first > b.first; };
        std::vector<int> best(static_cast<std::size_t>(W) * Hh, 1 << 30);
        best[idx(sx, sy)] = 0;
        heap.push_back({ 0, idx(sx, sy) });
        int found = -1;
        static const int ddx[4] = { -1, 0, 1, 0 }, ddy[4] = { 0, -1, 0, 1 };
        while (!heap.empty())
        {
            std::pop_heap(heap.begin(), heap.end(), cmp);
            const auto [cost, ci] = heap.back();
            heap.pop_back();
            if (cost > best[ci]) continue;
            const int cx = OFF + ci % W, cy = OFF + ci / W;
            if (goal(cx, cy)) { found = ci; break; }
            for (int d = 0; d < 4; ++d)
            {
                const int nx = cx + ddx[d], ny = cy + ddy[d];
                if (!InRoom(nx, ny) || !r.IsWalkable(nx, ny)) continue;
                const int floor_type = r.GetCell(nx, ny) & 0x3F;
                if (floor_type == 7 && !goal(nx, ny)) continue;   // a pit: only to fall on purpose
                if ((floor_type == 0x10 || floor_type == 0x19 || floor_type == 0x29) && !goal(nx, ny)) continue;   // spikes, swamp, lava
                if ((floor_type == 0x1A || floor_type == 0x26 || floor_type == 0x27) && goal(nx, ny) && !allow_locked_goal) continue;   // a locked door   // a pit: only to fall on purpose
                if (blocked.count({ nx, ny }) || (time < temp_block_until && temp_block.count({ nx, ny }))) continue;
                const int kind = StepKind(cx, cy, nx, ny, plat);
                if (kind == 0) continue;
                const int step = kind == 1 ? 1 : 8;   // a jump costs more
                const int ni = idx(nx, ny);
                if (cost + step < best[ni])
                {
                    best[ni] = cost + step;
                    from[ni] = ci;
                    heap.push_back({ cost + step, ni });
                    std::push_heap(heap.begin(), heap.end(), cmp);
                }
            }
        }
        if (found < 0) return none;
        std::vector<std::pair<int, int>> cells;
        for (int c = found; c >= 0; c = from[c]) cells.push_back({ OFF + c % W, OFF + c / W });
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
            ++recent_visits[seen_room];
            entry_key = std::get<1>(cur_hop) == seen_room ? cur_hop : std::make_tuple(-1, -1, 0, 0);
            hunt = false;
            if (!room_talkables.count(seen_room) && !(r.shop_info && r.shop_info->kind == "inn"))
            {
                auto& list = room_talkables[seen_room];
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
            if (!seen_flags.count(f)) { seen_flags.insert(f); if (f < 4096) recent_visits.clear(); Log("FLAG %d (byte %d bit %d) set", f, f / 8, f % 8); }
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
            const double d = std::hypot(e.x - r.player_x, e.y - r.player_y);
            if (d < dist) { dist = d; best = static_cast<int>(i); }
        }
        return best;
    }

    void Decide(int& dx, int& dy)
    {
        dx = dy = 0;
        if (!g || !r.entity_table) return;

        // ---- finished? ----
        if (r.room_number == goal_room)
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
            Log("DEAD (hp 0). Respawn #%d", deaths);
            r.Respawn();
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
                it = std::get<0>(it->first) == r.room_number ? dead_edges.erase(it) : std::next(it);
            Log("room cleared");
        }
        if (enemy >= 0 && (ed < 5.0 || hunt))
        {
            Fight(enemy, ed, dx, dy);
            return;
        }

        // ---- pick up what lies around ----
        if (TryPickup(dx, dy)) return;

        // ---- open chests and talk ----
        if (TryInteract(dx, dy)) return;
        if (TryUnlock(dx, dy)) return;

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
        std::map<int, int> dist{ { r.room_number, 0 } };
        std::deque<int> q{ r.room_number };
        while (!q.empty())
        {
            const int u = q.front();
            q.pop_front();
            if (u != r.room_number && !tour_done.count({ u, FlagSignature() }) && HasPendingTalk(u)) return u;
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
        if (tour_room < 0 && r.room_number != goal_room && (!NextHop(r.room_number, goal_room) || recent_visits[r.room_number] >= 4))
        {
            tour_room = PickTourRoom();
            if (tour_room >= 0) Log("the way on is closed: goes back to room %d to talk again", tour_room);
        }
        if (tour_room >= 0 && r.room_number == tour_room)
        {
            // nothing left to say here (TryInteract found nothing): the tour of this room is over
            tour_done.insert({ tour_room, FlagSignature() });
            tour_room = -1;
            recent_visits.clear();
        }
        const int target_room = tour_room >= 0 ? tour_room : goal_room;
        if (pending_aim.active && path.size() < 2) { aim = pending_aim; pending_aim.active = false; Aiming(dx, dy); return; }
        const Edge* hop = NextHop(r.room_number, target_room);
        if (!hop)
        {
            if (r.room_number == goal_room && tour_room < 0)
            {
                // Inside the goal room: look for the boss and wait for it (it fights us).
                if (plan_what != "wait") { plan_what = "wait"; Log("in the goal room, waiting"); }
                WanderToBoss(dx, dy);
                return;
            }
            if (plan_what != "noroute") { plan_what = "noroute"; Log("NO ROUTE from room %d to %d (dead edges %zu)", r.room_number, goal_room, dead_edges.size()); }
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
                Search([&](int x, int y) { ++reach; if (reach % 7 == 0 || reach < 40) far_cells += "(" + std::to_string(x) + "," + std::to_string(y) + ")"; return false; }, -1);
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
            else if (KillableEnemyExists())
            {
                hunt = true;
                Log("the way is shut and enemies are about: hunts them");
            }
            return;
        }
        cur_hop = std::make_tuple(hop->from, hop->to, hop->x, hop->y);
        if (plan_what != "goto" + std::to_string(hop->to)) { plan_what = "goto" + std::to_string(hop->to); Log("heads for room %d via %s (%d,%d)", hop->to, hop->fall ? "a pit" : "the warp", hop->x, hop->y); }
        path = cells;
        if (path.size() < 2 && hop->fall)
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
