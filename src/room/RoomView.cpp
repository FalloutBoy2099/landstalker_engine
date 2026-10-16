#include "room/RoomView.h"
#include "core/Keyboard.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace
{
// Minimal reader for the flat "key: value" lines of room.yaml (first occurrence wins).
std::unordered_map<std::string, std::string> ReadScalars(const std::filesystem::path& path)
{
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Unable to open " + path.string());
    std::unordered_map<std::string, std::string> kv;
    std::string line;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#') continue;
        const auto first = line.find_first_not_of(' ');
        if (first == std::string::npos || line[first] == '-') continue;   // list items
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto key = line.substr(first, colon - first);
        auto val = line.substr(colon + 1);
        val.erase(0, val.find_first_not_of(' '));
        if (!val.empty()) kv.emplace(key, val);
    }
    return kv;
}

int ToInt(const std::unordered_map<std::string, std::string>& kv, const std::string& key)
{
    auto it = kv.find(key);
    if (it == kv.end()) throw std::runtime_error("room.yaml: missing key '" + key + "'");
    return std::stoi(it->second);
}

// "  - {trigger: 1, mode: 2, map: [1, 2, 3], flag: 4}" -> key -> raw value ("1, 2, 3" for arrays).
std::unordered_map<std::string, std::string> FlowRow(const std::string& line)
{
    std::unordered_map<std::string, std::string> m;
    const auto open = line.find('{'), close = line.rfind('}');
    if (open == std::string::npos || close == std::string::npos) return m;
    const std::string body = line.substr(open + 1, close - open - 1);
    std::size_t i = 0;
    while (i < body.size())
    {
        const auto colon = body.find(':', i);
        if (colon == std::string::npos) break;
        std::string key = body.substr(i, colon - i);
        key.erase(0, key.find_first_not_of(" ,"));
        std::size_t j = colon + 1;
        while (j < body.size() && body[j] == ' ') ++j;
        std::string val;
        if (j < body.size() && body[j] == '[')
        {
            const auto end = body.find(']', j);
            val = body.substr(j + 1, end == std::string::npos ? std::string::npos : end - j - 1);
            i = (end == std::string::npos) ? body.size() : end + 1;
        }
        else
        {
            const auto end = body.find(',', j);
            val = body.substr(j, end == std::string::npos ? std::string::npos : end - j);
            i = (end == std::string::npos) ? body.size() : end + 1;
        }
        while (!val.empty() && val.back() == ' ') val.pop_back();
        m[key] = val;
    }
    return m;
}

std::vector<int> IntList(const std::string& raw)
{
    std::vector<int> v;
    std::string t = raw;
    for (char& c : t) if (c == ',') c = ' ';
    std::istringstream ss(t);
    int x;
    while (ss >> x) v.push_back(x);
    return v;
}

std::string RoomDirName(int number)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "room_%03d", number);
    return buf;
}
}

RoomView::RoomView(std::shared_ptr<Graphics> graphics, const std::filesystem::path& rooms_dir,
                   int room_number, SDL_Rect viewport, float scale)
    : graphics(std::move(graphics)), rooms_dir(rooms_dir), viewport(viewport), scale(scale)
{
    std::ifstream tf(rooms_dir.parent_path() / "text" / "room_transitions.txt");
    std::string tline;
    while (std::getline(tf, tline))
    {
        Transition t;
        if (tline.empty() || tline[0] == '#' || std::sscanf(tline.c_str(), "%d %d %d", &t.from, &t.to, &t.flag) != 3) continue;
        transitions.push_back(t);
    }
    LoadRoom(room_number);
}

// The table is read in order and a replacement can be replaced again, like the original.
int RoomView::Redirect(int room) const
{
    if (!game) return room;
    for (const auto& t : transitions)
        if (t.from == room && game->IsFlagSet(t.flag)) room = t.to;
    return room;
}

int RoomView::OriginalRoom(int room) const
{
    int original = room;
    for (const auto& t : transitions)
        if (t.to == room) original = t.from;
    return original;
}

RoomView::~RoomView()
{
    FreeAnimations();
    FreeEntityImages();
    if (blocks) SDL_DestroyTexture(blocks);
    if (blocks_priority) SDL_DestroyTexture(blocks_priority);
}

std::vector<int> RoomView::LoadCsv(const std::filesystem::path& path, int base, int& cols, int& rows)
{
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Unable to open " + path.string());
    std::vector<int> data;
    std::string line;
    cols = rows = 0;
    while (std::getline(f, line))
    {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string cell;
        int c = 0;
        while (std::getline(ss, cell, ','))
        {
            data.push_back(std::stoi(cell, nullptr, base));
            ++c;
        }
        if (rows == 0) cols = c;
        else if (c != cols) throw std::runtime_error(path.string() + ": ragged row");
        ++rows;
    }
    return data;
}

// Parses lines such as:
//   - {type: normal, room1: 0, room2: 3, x1: 19, y1: 25, x_size: 1, y_size: 2, x2: 34, y2: 25}
std::vector<RoomView::RawWarp> RoomView::LoadRawWarps(const std::filesystem::path& room_yaml)
{
    std::ifstream f(room_yaml);
    if (!f) throw std::runtime_error("Unable to open " + room_yaml.string());
    std::vector<RawWarp> result;
    std::string line;
    bool in_warps = false;
    while (std::getline(f, line))
    {
        if (!line.empty() && line[0] != ' ' && line[0] != '#')
        {
            in_warps = (line.rfind("warps:", 0) == 0);
            continue;
        }
        if (!in_warps) continue;
        const auto open = line.find('{'), close = line.find('}');
        if (open == std::string::npos || close == std::string::npos) continue;
        std::unordered_map<std::string, std::string> kv;
        std::istringstream ss(line.substr(open + 1, close - open - 1));
        std::string item;
        while (std::getline(ss, item, ','))
        {
            const auto colon = item.find(':');
            if (colon == std::string::npos) continue;
            auto key = item.substr(0, colon);
            auto val = item.substr(colon + 1);
            key.erase(0, key.find_first_not_of(' '));
            val.erase(0, val.find_first_not_of(' '));
            kv[key] = val;
        }
        try
        {
            result.push_back({ kv.at("type"), ToInt(kv, "room1"), ToInt(kv, "room2"),
                               ToInt(kv, "x1"), ToInt(kv, "y1"), ToInt(kv, "x_size"), ToInt(kv, "y_size"),
                               ToInt(kv, "x2"), ToInt(kv, "y2") });
        }
        catch (const std::exception&) { /* skip malformed warp */ }
    }
    return result;
}

// Parses the entities: section of room.yaml:
//   - type: 103
//     position: [33.5, 25.5, 2]
//     orientation: "NW"
std::vector<RoomEntity> RoomView::LoadEntities(const std::filesystem::path& room_yaml)
{
    std::ifstream f(room_yaml);
    if (!f) throw std::runtime_error("Unable to open " + room_yaml.string());
    std::vector<RoomEntity> result;
    std::string line;
    bool in_entities = false;
    while (std::getline(f, line))
    {
        if (!line.empty() && line[0] != ' ' && line[0] != '#')
        {
            in_entities = (line.rfind("entities:", 0) == 0);
            continue;
        }
        if (!in_entities) continue;
        const auto first = line.find_first_not_of(' ');
        if (first == std::string::npos) continue;
        std::string body = line.substr(first);
        if (body.rfind("- ", 0) == 0)
        {
            result.emplace_back();
            body = body.substr(2);
        }
        if (result.empty()) continue;
        auto& e = result.back();
        if (body.rfind("type:", 0) == 0)
        {
            e.type = std::stoi(body.substr(5));
        }
        else if (body.rfind("position:", 0) == 0)
        {
            const auto open = body.find('['), close = body.find(']');
            if (open != std::string::npos && close != std::string::npos)
            {
                double v[3] = { 0, 0, 0 };
                std::istringstream ss(body.substr(open + 1, close - open - 1));
                std::string item;
                for (int i = 0; i < 3 && std::getline(ss, item, ','); ++i) v[i] = std::stod(item);
                e.x = v[0]; e.y = v[1]; e.z = v[2];
            }
        }
        else if (body.rfind("images:", 0) == 0)
        {
            // images: {default_file: "x.png", default_w: 32, ..., back_file: ..., front_file: ...}
            const auto open = body.find('{'), close = body.rfind('}');
            if (open == std::string::npos || close == std::string::npos) continue;
            std::unordered_map<std::string, std::string> m;
            std::istringstream ss(body.substr(open + 1, close - open - 1));
            std::string item;
            while (std::getline(ss, item, ','))
            {
                const auto c = item.find(':');
                if (c == std::string::npos) continue;
                auto key = item.substr(0, c);
                auto val = item.substr(c + 1);
                key.erase(0, key.find_first_not_of(' '));
                val.erase(0, val.find_first_not_of(' '));
                if (val.size() >= 2 && val.front() == '"') val = val.substr(1, val.size() - 2);
                m[key] = val;
            }
            static const char* names[3] = { "default", "back", "front" };
            for (int k = 0; k < 3; ++k)
            {
                const std::string n = names[k];
                if (!m.count(n + "_file")) continue;
                auto& im = e.images[k];
                im.file = m[n + "_file"];
                im.w = std::stoi(m[n + "_w"]); im.h = std::stoi(m[n + "_h"]);
                im.ax = std::stoi(m[n + "_ax"]); im.ay = std::stoi(m[n + "_ay"]);
            }
        }
        else if (body.rfind("solid:", 0) == 0)
        {
            e.solid = body.find("true") != std::string::npos;
        }
        else if (body.rfind("talks:", 0) == 0)
        {
            e.talks = body.find("true") != std::string::npos;
        }
        else if (body.rfind("dialogue:", 0) == 0)
        {
            e.dialogue = std::stoi(body.substr(9));
        }
        else if (body.rfind("visible:", 0) == 0)
        {
            e.visible = body.find("true") != std::string::npos;
        }
        else if (body.rfind("hostile:", 0) == 0)
        {
            e.hostile = body.find("true") != std::string::npos;
        }
        else if (body.rfind("speed:", 0) == 0)
        {
            e.speed = std::stoi(body.substr(6));
        }
        else if (body.rfind("behaviour:", 0) == 0)
        {
            e.behaviour = std::stoi(body.substr(10));
        }
        else if (body.rfind("gravity:", 0) == 0)
        {
            e.gravity = body.find("true") != std::string::npos;
        }
        else if (body.rfind("orientation:", 0) == 0)
        {
            const auto q = body.find('"');
            const std::string o = q == std::string::npos ? "" : body.substr(q + 1, 2);
            e.orientation = (o == "NE") ? 0 : (o == "SE") ? 1 : (o == "SW") ? 2 : 3;
        }
    }
    return result;
}

// Entity positions in room.yaml are the centre of the entity's collision box in the same
// frame as the heightmap cells (cell = floor(position)). The editor draws sprites half a
// cell further in +x and back in -y, but measured over every room that offset puts only
// 66% of the entities on walkable cells against 84% for no offset, and it made sprites
// appear 16 px away from their collision boxes. So no offset is applied: drawing,
// collisions, pickups and AI all use the stored position.
void RoomView::CentreEntities()
{
    for (auto& e : entities) e.centred = true;
}

void RoomView::FreeEntityImages()
{
    for (auto& e : entities)
        for (auto& im : e.images)
            if (im.texture) { SDL_DestroyTexture(im.texture); im.texture = nullptr; }
}

void RoomView::LoadEntityImages(const std::filesystem::path& room_dir)
{
    for (auto& e : entities)
    {
        for (auto& im : e.images)
        {
            if (im.file.empty()) continue;
            SDL_Surface* surface = SDL_LoadPNG((room_dir / im.file).string().c_str());
            if (!surface)
            {
                SDL_Log("Room image %s not loaded: %s", im.file.c_str(), SDL_GetError());
                im.file.clear();
                continue;   // fall back to the shared image
            }
            im.texture = SDL_CreateTextureFromSurface(graphics->GetRenderer(), surface);
            SDL_DestroySurface(surface);
            if (!im.texture) { im.file.clear(); continue; }
            SDL_SetTextureScaleMode(im.texture, SDL_SCALEMODE_NEAREST);
            SDL_SetTextureBlendMode(im.texture, SDL_BLENDMODE_BLEND);
        }
    }
}

bool RoomView::RoomExists(int number) const
{
    return number >= 0 && std::filesystem::exists(rooms_dir / RoomDirName(number) / "room.yaml");
}

// A static texture holding exactly the surface's pixels as RGBA, so that parts of it can be
// replaced later (animated blocks).
SDL_Texture* RoomView::MakeTexture(SDL_Renderer* renderer, SDL_Surface* surface)
{
    SDL_Surface* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    if (!rgba) return nullptr;
    SDL_Texture* t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, rgba->w, rgba->h);
    if (t)
    {
        SDL_UpdateTexture(t, nullptr, rgba->pixels, rgba->pitch);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    }
    SDL_DestroySurface(rgba);
    return t;
}

void RoomView::FreeAnimations()
{
    for (auto& a : animations)
        if (a.atlas) SDL_DestroySurface(a.atlas);
    animations.clear();
}

// Water, lava and waterfalls: every `delay` frames of the console (60 Hz) the animated blocks
// take the next picture of their cycle.
void RoomView::UpdateAnimations(double dt)
{
    if (animations.empty() || !blocks) return;
    anim_clock += dt;
    for (auto& a : animations)
    {
        if (!a.atlas || a.frames < 2) continue;
        const int frame = static_cast<int>(anim_clock * 60.0 / a.delay) % a.frames;
        if (frame == a.shown) continue;
        a.shown = frame;
        const int bs = BLOCK_PX;
        for (std::size_t u = 0; u < a.blocks.size(); ++u)
        {
            const int b = a.blocks[u];
            const SDL_Rect dst = { (b % BLOCKS_PER_ROW) * bs, (b / BLOCKS_PER_ROW) * bs, bs, bs };
            const Uint8* src = static_cast<const Uint8*>(a.atlas->pixels) + static_cast<std::size_t>(frame) * bs * a.atlas->pitch + u * bs * 4;
            SDL_UpdateTexture(blocks, &dst, src, a.atlas->pitch);
        }
    }
}

void RoomView::LoadRoom(int number, const SDL_Point* arrival)
{
    const auto dir = rooms_dir / RoomDirName(number);
    const auto kv = ReadScalars(dir / "room.yaml");
    const int w = ToInt(kv, "width"), h = ToInt(kv, "height");
    const int left = ToInt(kv, "heightmap_left"), top = ToInt(kv, "heightmap_top");
    const int hmw = ToInt(kv, "heightmap_width"), hmh = ToInt(kv, "heightmap_height");

    int cols = 0, rows = 0;
    auto new_bg = LoadCsv(dir / "map_bg.csv", 10, cols, rows);
    if (cols != w || rows != h) throw std::runtime_error("map_bg.csv size mismatch");
    auto new_fg = LoadCsv(dir / "map_fg.csv", 10, cols, rows);
    if (cols != w || rows != h) throw std::runtime_error("map_fg.csv size mismatch");
    auto new_hm = LoadCsv(dir / "heightmap.csv", 16, cols, rows);
    if (cols != hmw || rows != hmh) throw std::runtime_error("heightmap.csv size mismatch");
    const int original_room = OriginalRoom(number);
    auto raw = LoadRawWarps(original_room != number && RoomExists(original_room) ? rooms_dir / RoomDirName(original_room) / "room.yaml" : dir / "room.yaml");
    std::vector<FlagRule> new_rules;
    {
        // Lines of the entity_flags: section:  - {kind: onetime, entity: 1, show_flag: 147, ...}
        std::ifstream rf(dir / "room.yaml");
        std::string line;
        bool in_flags = false;
        while (std::getline(rf, line))
        {
            if (!line.empty() && line[0] != ' ' && line[0] != '#')
            {
                in_flags = (line.rfind("entity_flags:", 0) == 0);
                continue;
            }
            if (!in_flags) continue;
            const auto open = line.find('{'), close = line.rfind('}');
            if (open == std::string::npos || close == std::string::npos) continue;
            std::unordered_map<std::string, std::string> m;
            std::istringstream ss(line.substr(open + 1, close - open - 1));
            std::string item;
            while (std::getline(ss, item, ','))
            {
                const auto c = item.find(':');
                if (c == std::string::npos) continue;
                auto key = item.substr(0, c);
                auto val = item.substr(c + 1);
                key.erase(0, key.find_first_not_of(' '));
                val.erase(0, val.find_first_not_of(' '));
                m[key] = val;
            }
            auto num = [&](const char* k) { auto it = m.find(k); return it == m.end() ? 0 : std::stoi(it->second); };
            auto flag = [&](const char* k) { auto it = m.find(k); return it != m.end() && it->second == "true"; };
            FlagRule r;
            r.entity = num("entity");
            if (m["kind"] == "hide") { r.kind = FlagRule::HIDE; r.flag = num("flag"); r.when_set = flag("when_set"); }
            else if (m["kind"] == "onetime")
            {
                r.kind = FlagRule::ONE_TIME;
                r.flag = num("show_flag"); r.when_set = flag("show_set");
                r.hide_flag = num("hide_flag"); r.hide_set = flag("hide_set");
            }
            else if (m["kind"] == "hide_from") { r.kind = FlagRule::HIDE_FROM; r.flag = num("flag"); }
            else continue;
            new_rules.push_back(r);
        }
    }
    auto new_entities = LoadEntities(dir / "room.yaml");
    std::vector<TileSwap> new_swaps;
    std::vector<SwapFlag> new_swap_flags;
    std::vector<Switch> new_switches;
    std::vector<FlagRule> locked_rules;
    std::vector<RoomAnim> new_anims;
    {
        std::ifstream rf(dir / "room.yaml");
        std::string line, section;
        while (std::getline(rf, line))
        {
            if (!line.empty() && line[0] != ' ' && line[0] != '#')
            {
                section = line.substr(0, line.find(':'));
                continue;
            }
            if (line.find('{') == std::string::npos) continue;
            auto row = FlowRow(line);
            auto num = [&](const char* k) { auto it = row.find(k); return it == row.end() ? 0 : std::stoi(it->second); };
            auto flag = [&](const char* k) { auto it = row.find(k); return it != row.end() && it->second == "true"; };
            if (section == "tile_swaps")
            {
                TileSwap t;
                t.trigger = num("trigger");
                t.mode = num("mode");
                const auto m = IntList(row["map"]), h = IntList(row["hm"]);
                if (m.size() == 6) std::copy(m.begin(), m.end(), t.map);
                if (h.size() == 6) std::copy(h.begin(), h.end(), t.hm);
                new_swaps.push_back(t);
            }
            else if (section == "swap_flags")
            {
                new_swap_flags.push_back({ num("trigger"), num("flag"), flag("always"), flag("locked") });
            }
            else if (section == "locked_doors")
            {
                FlagRule r;
                r.kind = FlagRule::HIDE;
                r.entity = num("entity");
                r.flag = num("flag");
                r.when_set = true;     // the door entity is gone once it is unlocked
                locked_rules.push_back(r);
            }
            else if (section == "switches")
            {
                new_switches.push_back({ num("entity"), num("flag") });
            }
            else if (section == "animations")
            {
                // Water, lava, waterfalls: blocks whose picture changes with time (anim_N.png holds
                // one row per frame, one 16x16 image per listed block).
                RoomAnim a;
                a.file = row["file"];
                if (a.file.size() >= 2 && a.file.front() == '"') a.file = a.file.substr(1, a.file.size() - 2);
                a.frames = num("frames");
                a.delay = std::max(1, num("delay"));
                a.blocks = IntList(row["blocks"]);
                if (a.frames >= 2 && !a.blocks.empty()) new_anims.push_back(std::move(a));
            }
        }
    }
    std::vector<int> new_chests;
    {
        auto it = kv.find("chests");
        if (it != kv.end())
        {
            std::string text = it->second;
            for (char& c : text) if (c == '[' || c == ']' || c == ',') c = ' ';
            std::istringstream ss(text);
            int v;
            while (ss >> v) new_chests.push_back(v);
        }
    }
    std::vector<int> new_characters;
    {
        auto it = kv.find("characters");
        if (it != kv.end())
        {
            std::string text = it->second;
            for (char& c : text) if (c == '[' || c == ']' || c == ',') c = ' ';
            std::istringstream ss(text);
            int v;
            while (ss >> v) new_characters.push_back(v);
        }
    }
    const bool new_chests_enabled = kv.count("chests_enabled") && kv.at("chests_enabled") == "true";

    SDL_Surface* surface = SDL_LoadPNG((dir / "blocks.png").string().c_str());
    if (!surface) throw std::runtime_error(std::string("Unable to load blocks.png: ") + SDL_GetError());
    SDL_Texture* tex = MakeTexture(graphics->GetRenderer(), surface);
    SDL_DestroySurface(surface);
    if (!tex) throw std::runtime_error(std::string("Unable to create texture: ") + SDL_GetError());
    // The pictures of the animated blocks, one row per frame.
    for (auto& a : new_anims)
    {
        SDL_Surface* raw = SDL_LoadPNG((dir / a.file).string().c_str());
        if (!raw) { a.frames = 0; continue; }
        a.atlas = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(raw);
        if (!a.atlas) a.frames = 0;
    }
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    // The priority tiles (optional: older exports do not have the file).
    SDL_Texture* priority_tex = nullptr;
    std::vector<char> new_block_priority;
    if (SDL_Surface* ps = SDL_LoadPNG((dir / "blocks_priority.png").string().c_str()))
    {
        const int cols_b = ps->w / BLOCK_PX, rows_b = ps->h / BLOCK_PX;
        new_block_priority.assign(static_cast<std::size_t>(cols_b) * rows_b, 0);
        for (int by = 0; by < rows_b; ++by)
            for (int bx = 0; bx < cols_b; ++bx)
            {
                bool any = false;
                for (int yy = 0; yy < BLOCK_PX && !any; ++yy)
                    for (int xx = 0; xx < BLOCK_PX && !any; ++xx)
                    {
                        Uint8 r, g, b, a;
                        if (SDL_ReadSurfacePixel(ps, bx * BLOCK_PX + xx, by * BLOCK_PX + yy, &r, &g, &b, &a) && a != 0) any = true;
                    }
                new_block_priority[static_cast<std::size_t>(by) * cols_b + bx] = any ? 1 : 0;
            }
        priority_tex = SDL_CreateTextureFromSurface(graphics->GetRenderer(), ps);
        SDL_DestroySurface(ps);
        if (priority_tex)
        {
            SDL_SetTextureScaleMode(priority_tex, SDL_SCALEMODE_NEAREST);
            SDL_SetTextureBlendMode(priority_tex, SDL_BLENDMODE_BLEND);
        }
    }

    // Everything loaded: commit.
    if (blocks) SDL_DestroyTexture(blocks);
    blocks = tex;
    FreeAnimations();
    animations = std::move(new_anims);
    anim_clock = 0.0;
    if (blocks_priority) SDL_DestroyTexture(blocks_priority);
    blocks_priority = priority_tex;
    block_has_priority = std::move(new_block_priority);
    room_number = number;
    fall_dest = kv.count("fall_destination") ? ToInt(kv, "fall_destination") : -1;
    climb_dest = kv.count("climb_destination") ? ToInt(kv, "climb_destination") : -1;
    room_max_height = kv.count("z_end") ? ToInt(kv, "z_end") : 0;
    climbing = false;
    shop_info = shops ? shops->ForRoom(number) : nullptr;
    choice_handler = nullptr;
    width = w; height = h;
    hm_left = left; hm_top = top; hm_width = hmw; hm_height = hmh;
    pixel_w = (width + height + hm_left) * 2 * TILE_PX;
    pixel_h = (width + height + hm_top + 1) * TILE_PX;
    bg = std::move(new_bg); fg = std::move(new_fg); heightmap = std::move(new_hm);
    FreeEntityImages();
    entities = std::move(new_entities);
    for (auto& e : entities) e.base_visible = e.visible;
    flag_rules = std::move(new_rules);
    flag_rules.insert(flag_rules.end(), locked_rules.begin(), locked_rules.end());
    tile_swaps = std::move(new_swaps);
    swap_flags = std::move(new_swap_flags);
    switches = std::move(new_switches);
    base_bg = bg;
    base_fg = fg;
    base_heightmap = heightmap;
    CentreEntities();
    LoadEntityImages(dir);
    ApplyFlagRules();
    chest_contents = std::move(new_chests);
    room_characters = std::move(new_characters);
    dialogue_pages.clear();
    cutscene_ops.clear();
    projectiles.clear();
    camera_focus = -1;
    shake_t = 0.0;
    dialogue_asks = false;
    dialogue_yes = dialogue_no = nullptr;
    if (dialogue_cb) dialogue_cb("", false);
    chests_enabled = new_chests_enabled;

    // A warp entry links (room1, x1,y1) <-> (room2, x2,y2); keep the ends that are in this room.
    warps.clear();
    for (const auto& r : raw)
    {
        if (r.room1 == original_room)
            warps.push_back({ r.x1, r.y1, r.x_size, r.y_size, r.room2, r.x2, r.y2, r.type });
        if (r.room2 == original_room)
            warps.push_back({ r.x2, r.y2, r.x_size, r.y_size, r.room1, r.x1, r.y1, r.type });
    }

    if (arrival)
    {
        cursor_x = arrival->x;
        cursor_y = arrival->y;
    }
    else
    {
        cursor_x = CELL_OFFSET + hm_width / 2;
        cursor_y = CELL_OFFSET + hm_height / 2;
    }
    camera_snap = true;
    player_x = cursor_x + 0.5;
    player_y = cursor_y + 0.5;
    arrival_x = player_x;
    arrival_y = player_y;
    support_entity = -1;
    player_z = FloorHeight(cursor_x, cursor_y);
    {
        int idx = -1;
        const double top = PlatformTop(player_x, player_y, player_z + 1.0 - PLATFORM_TOLERANCE, &idx);
        if (top > player_z) player_z = top;   // arriving on a raft
    }
    vertical_speed = 0.0;
    grounded = true;
    moving = false;
    anim_time = 0.0;
    warp_armed = false;   // re-armed once the cursor leaves every warp
    // The room's "bgm" is an index into the game's RoomMusicLUT (code/maps/mapload.asm).
    {
        static const int room_music_lut[19] = {
            0x25, 0x03, 0x13, 0x01, 0x02, 0x21, 0x23, 0x1D, 0x13, 0x26, 0x27, 0x0A, 0x14, 0x2B,
            0x2A, 0x22, 0x15, 0x29, 0xFD };
        const int idx = kv.count("bgm") ? ToInt(kv, "bgm") : -1;
        room_bgm = (idx >= 0 && idx < 19) ? room_music_lut[idx] : -1;
        // The evil village turns happy once the story flag (byte 2, bit 5) is set.
        if (room_bgm == 0x23 && game && game->IsFlagSet(2 * 8 + 5)) room_bgm = 0x24;
    }
    PlayRoomMusic();
    CheckWarps();
    UpdateStatus();
}

// The music keeps playing across rooms that share the same song (like the original).
void RoomView::PlayRoomMusic()
{
    if (!sound || room_bgm <= 0 || jingle_time > 0.0) return;
    if (intro_active) return;   // the intro scenes choose their own music
    if (room_bgm == playing_bgm) return;
    playing_bgm = room_bgm;
    if (room_bgm == 0xFD) sound->Command(SoundEngine::SND_FADE_OUT);   // this room is silent
    else sound->PlayMusic(room_bgm);
}

// A short fanfare over the room music; the room song is restarted after it.
void RoomView::Jingle(int id, double seconds)
{
    if (!sound) return;
    jingle_time = seconds;
    playing_bgm = -1;
    sound->PlayMusic(id);
}

void RoomView::SetStatusCallback(std::function<void(const std::string&)> cb)
{
    status_cb = std::move(cb);
    UpdateStatus();
}

int RoomView::GetCell(int world_x, int world_y) const
{
    const int x = world_x - CELL_OFFSET, y = world_y - CELL_OFFSET;
    if (x < 0 || y < 0 || x >= hm_width || y >= hm_height) return -1;
    return heightmap[y * hm_width + x];
}

bool RoomView::IsWalkable(int world_x, int world_y) const
{
    const int cell = GetCell(world_x, world_y);
    return cell >= 0 && (cell & 0x4000) == 0;   // props bit 2 = not walkable
}

void RoomView::OnKeyDown(SDL_Scancode scancode, SDL_Keymod)
{
    if (intro_active && (scancode == SDL_SCANCODE_RETURN || scancode == SDL_SCANCODE_ESCAPE))
    {
        SkipIntro();
        return;
    }
    if (input_locked)
    {
        // A scripted scene is playing: the keys only turn the pages of its text boxes.
        if (!dialogue_pages.empty())
        {
            if (scancode == SDL_SCANCODE_LSHIFT || scancode == SDL_SCANCODE_RSHIFT || scancode == SDL_SCANCODE_SPACE) AdvanceDialogue();
            else if (scancode == SDL_SCANCODE_Y) AnswerDialogue(true);
            else if (scancode == SDL_SCANCODE_N) AnswerDialogue(false);
        }
        return;
    }
    if (scancode == SDL_SCANCODE_ESCAPE && pause_cb && !dead && !menu_open && dialogue_pages.empty())
    {
        pause_cb();
        return;
    }
    if (scancode == SDL_SCANCODE_TAB)
    {
        if (menu_open) CloseMenu();
        else if (!dead && dialogue_pages.empty()) OpenMenu();
        return;
    }
    if (menu_open)
    {
        switch (scancode)
        {
        case SDL_SCANCODE_UP:
            if (!menu_items.empty()) menu_index = (menu_index + static_cast<int>(menu_items.size()) - 1) % static_cast<int>(menu_items.size());
            Sfx(SoundEngine::SFX_CURSOR_MOVE);
            RefreshMenu();
            break;
        case SDL_SCANCODE_DOWN:
            if (!menu_items.empty()) menu_index = (menu_index + 1) % static_cast<int>(menu_items.size());
            Sfx(SoundEngine::SFX_CURSOR_MOVE);
            RefreshMenu();
            break;
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT:
        case SDL_SCANCODE_RETURN:
            UseMenuItem();
            break;
        default: break;
        }
        return;   // nothing else reaches the game while the menu is open
    }
    // Screen directions on the isometric grid: +x goes down-right, +y goes down-left.
    switch (scancode)
    {
    case SDL_SCANCODE_UP:
    case SDL_SCANCODE_DOWN:  last_key_was_x_axis = true;  break;
    case SDL_SCANCODE_LEFT:
    case SDL_SCANCODE_RIGHT: last_key_was_x_axis = false; break;
    case SDL_SCANCODE_F1:    show_overlay = !show_overlay; break;
    case SDL_SCANCODE_F2:    show_grid = !show_grid; break;
    case SDL_SCANCODE_F3:    show_blocks = !show_blocks; break;
    case SDL_SCANCODE_KP_PLUS:  SetScale(std::min(6.0f, scale + 1.0f)); break;
    case SDL_SCANCODE_KP_MINUS: SetScale(std::max(1.0f, scale - 1.0f)); break;
    case SDL_SCANCODE_SPACE:
        Attack();
        break;
    case SDL_SCANCODE_K: UseKey(); break;
    case SDL_SCANCODE_F5: SaveGame(); break;
    case SDL_SCANCODE_F9: LoadGame(); break;
    case SDL_SCANCODE_Y: AnswerDialogue(true); break;
    case SDL_SCANCODE_N: AnswerDialogue(false); break;
    case SDL_SCANCODE_R:
        Respawn();
        break;
    case SDL_SCANCODE_E:
        if (game)
        {
            if (game->UseEkeEke()) { Sfx(SoundEngine::SFX_HEALTH); ShowMessage("Used an EkeEke"); }
            else
            {
                Sfx(SoundEngine::SFX_ERROR);
                ShowMessage(game->ItemCount(GameState::ITEM_EKEEKE) <= 0 ? "No EkeEke left" : "Health is already full");
            }
        }
        break;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:
        Interact();
        break;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:
        if (grounded && !(game && game->HasStatus(GameState::STATUS_PARALYSIS)))
        {
            vertical_speed = JUMP_SPEED;
            grounded = false;
            Sfx(SoundEngine::SFX_JUMP);
        }
        break;
    case SDL_SCANCODE_PAGEUP:
    case SDL_SCANCODE_PAGEDOWN:
    {
        const int step = (scancode == SDL_SCANCODE_PAGEDOWN) ? 1 : -1;
        for (int n = room_number + step; n >= 0 && n < 100000; n += step)
        {
            if (RoomExists(n)) { LoadRoom(n); break; }
        }
        break;
    }
    default: break;
    }
}

int RoomView::FloorHeight(int cell_x, int cell_y) const
{
    const int cell = GetCell(cell_x, cell_y);
    return cell < 0 ? 0 : ((cell >> 8) & 0x0F);
}

// True if the player's collision box centred on (x, y) fits on walkable cells that are no
// higher than z + STEP_UP (lower cells are fine: the player will walk off the edge and fall).
bool RoomView::CanStandAt(double x, double y, double z) const
{
    const double r = PLAYER_RADIUS;
    const double xs[2] = { x - r, x + r };
    const double ys[2] = { y - r, y + r };
    for (double cx : xs)
    {
        for (double cy : ys)
        {
            const int ix = static_cast<int>(std::floor(cx)), iy = static_cast<int>(std::floor(cy));
            if (!IsWalkable(ix, iy)) return false;
            if (FloorHeight(ix, iy) > z + STEP_UP + 1e-6) return false;
        }
    }
    return true;
}

// Like the original: the floor is the highest cell under the box that is not above what the
// player can step onto from z.
// Objects the player can stand on (crates, blocks, platforms): the highest top among the solid
// inanimate entities under the player's box that is not above his feet.
double RoomView::PlatformTop(double x, double y, double z, int* index) const
{
    double best = -1.0;
    if (index) *index = -1;
    if (!entity_table) return best;
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        const auto& e = entities[i];
        if (!e.solid || !e.visible || e.hostile || e.talks || e.dying || e.type >= 0xC0 || !entity_table->Has(e.type)) continue;
        if (entity_table->GetEnemy(e.type)) continue;
        if (game && game->IsCollected(room_number, static_cast<int>(i))) continue;
        const double half = entity_table->GetHitboxBase(e.type) / 16.0;
        const double top = e.z + entity_table->GetHitboxHeight(e.type) / 16.0;
        if (top > z + PLATFORM_TOLERANCE || top <= best) continue;
        if (std::abs(x - e.x) < half + PLAYER_RADIUS && std::abs(y - e.y) < half + PLAYER_RADIUS)
        {
            best = top;
            if (index) *index = static_cast<int>(i);
        }
    }
    return best;
}

double RoomView::FloorUnder(double x, double y, double z) const
{
    const double r = PLAYER_RADIUS;
    const double xs[2] = { x - r, x + r };
    const double ys[2] = { y - r, y + r };
    double floor_h = -1.0;
    for (double cx : xs)
    {
        for (double cy : ys)
        {
            const int ix = static_cast<int>(std::floor(cx)), iy = static_cast<int>(std::floor(cy));
            if (!IsWalkable(ix, iy)) continue;
            const double h = FloorHeight(ix, iy);
            if (h <= z + STEP_UP + 1e-6 && h > floor_h) floor_h = h;
        }
    }
    const double platform = PlatformTop(x, y, z);
    if (platform > floor_h) floor_h = platform;
    return floor_h < 0.0 ? z : floor_h;
}

// Indices (into `entities`) of the solid entities overlapping the player's box. Boxes are
// squares of half-size hitbox_base / 16 cells centred on the entity, and extend upwards by
// hitbox_height / 16 height units; z ranges must overlap as well.
std::vector<int> RoomView::OverlappingEntities(double x, double y, double z) const
{
    std::vector<int> hits;
    if (!entity_table) return hits;
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        const auto& e = entities[i];
        if (!e.solid || !e.visible || !entity_table->Has(e.type)) continue;
        if (e.type >= 0xC0) continue;   // items are picked up, not obstacles
        if (game && game->IsCollected(room_number, static_cast<int>(i))) continue;
        const double half = entity_table->GetHitboxBase(e.type) / 16.0;
        const double top = e.z + entity_table->GetHitboxHeight(e.type) / 16.0;
        if (std::abs(x - e.x) < half + PLAYER_RADIUS &&
            std::abs(y - e.y) < half + PLAYER_RADIUS &&
            z < top - 1e-3 && z + PLAYER_HEIGHT > e.z)
        {
            hits.push_back(static_cast<int>(i));
        }
    }
    return hits;
}

std::string RoomView::ExplainBlock(double x, double y, double z) const
{
    char buf[128];
    const double r = PLAYER_RADIUS;
    const double xs[2] = { x - r, x + r };
    const double ys[2] = { y - r, y + r };
    for (double cx : xs)
    {
        for (double cy : ys)
        {
            const int ix = static_cast<int>(std::floor(cx)), iy = static_cast<int>(std::floor(cy));
            const int cell = GetCell(ix, iy);
            if (cell < 0)
            {
                std::snprintf(buf, sizeof(buf), "outside the map at (%d,%d)", ix, iy);
                return buf;
            }
            if (cell & 0x4000)
            {
                std::snprintf(buf, sizeof(buf), "wall cell (%d,%d) hm %04X", ix, iy, cell);
                return buf;
            }
            if (FloorHeight(ix, iy) > z + STEP_UP + 1e-6)
            {
                std::snprintf(buf, sizeof(buf), "step too high at (%d,%d): %d vs z %.1f", ix, iy, FloorHeight(ix, iy), z);
                return buf;
            }
        }
    }
    return "";
}

// Moves by (dx, dy) along a single axis; returns whether the player actually moved.
bool RoomView::TryMove(double dx, double dy)
{
    const double h = player_z;
    const double nx = player_x + dx, ny = player_y + dy;
    if (!CanStandAt(nx, ny, h))
    {
        blocked_text = ExplainBlock(nx, ny, h);
        return false;
    }

    // Entities: refuse to enter a solid one. If we already overlap one (spawned or warped
    // next to it) we may only move so as to get out, never deeper.
    const auto already = OverlappingEntities(player_x, player_y, h);
    for (int i : OverlappingEntities(nx, ny, h))
    {
        const auto& e = entities[i];
        if (std::find(already.begin(), already.end(), i) == already.end())
        {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "entity #%d type %d at (%.1f,%.1f)", i, e.type, e.x, e.y);
            blocked_text = buf;
            return false;
        }
        // Moves are single-axis, so "getting deeper" means getting closer along that axis.
        const bool approaching = (dx != 0.0 && std::abs(nx - e.x) < std::abs(player_x - e.x)) ||
                                 (dy != 0.0 && std::abs(ny - e.y) < std::abs(player_y - e.y));
        if (approaching)
        {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "entity #%d type %d at (%.1f,%.1f)", i, e.type, e.x, e.y);
            blocked_text = buf;
            return false;
        }
    }
    player_x = nx;
    player_y = ny;
    return true;
}

bool RoomView::Nudge(double fx, double fy, double max_step)
{
    // Perpendicular axis to the intended movement.
    const double px = (fx != 0.0) ? 0.0 : 1.0, py = (fx != 0.0) ? 1.0 : 0.0;
    for (double d = 0.05; d <= NUDGE_RANGE + 1e-9; d += 0.05)
    {
        for (double sign : { 1.0, -1.0 })
        {
            const double nx = player_x + fx + px * sign * d, ny = player_y + fy + py * sign * d;
            if (!CanStandAt(nx, ny, player_z)) continue;
            // A gap exists `d` to the side: slide towards it (without moving forward yet).
            const double step = std::min(d, max_step);
            const std::string saved = blocked_text;
            if (TryMove(px * sign * step, py * sign * step)) return true;
            blocked_text = saved;
            return false;
        }
    }
    return false;
}

// An enemy is a hostile entity the table has stats for that has not been killed yet.
bool RoomView::IsAliveEnemy(std::size_t index) const
{
    if (!game || !entity_table || index >= entities.size()) return false;
    const auto& e = entities[index];
    return e.hostile && !e.dormant && !e.dying && e.visible && entity_table->GetEnemy(e.type) != nullptr &&
           !game->IsCollected(room_number, static_cast<int>(index));
}

void RoomView::Die()
{
    dead = true;
    death_time = 0.0;
    playing_bgm = -1;   // the room music restarts after the revival
    if (sound) sound->Stop();
    Sfx(SoundEngine::SFX_NIGEL_DIE);
    ShowMessage("GAME OVER");
    message_time = 1e9;   // keep it on screen
}

// Sword swing: hits enemies in a box in front of the player.
void RoomView::SpawnSparks(double x, double y, double z, bool big, bool hurt_player)
{
    sparks.push_back({ x, y, z, 0.0, big, hurt_player });
}

// Starts the sword animation. The blow itself lands when the arc is out (SWING_HIT_TIME):
// see ApplySwingHit(). Nigel cannot walk while swinging.
void RoomView::Attack()
{
    if (dead || chest_open.active || !dialogue_pages.empty() || attack_cooldown > 0.0 || swinging ||
        !game || !entity_table) return;
    if (game->HasStatus(GameState::STATUS_PARALYSIS))
    {
        // Struggling: each press has a growing chance of breaking free (1/3, 1/2, then sure).
        static const int odds[3] = { 3, 2, 1 };
        const int n = std::min(2, paralysis_presses++);
        if (std::rand() % odds[n] == 0) { CureStatus(GameState::STATUS_PARALYSIS); ShowMessage("You break free!"); }
        else Sfx(SoundEngine::SFX_ERROR);
        attack_cooldown = 0.25;
        return;
    }
    if (game->HasStatus(GameState::STATUS_CURSE))
    {
        Sfx(SoundEngine::SFX_ERROR);
        ShowMessage("You are cursed! You cannot use your sword.");
        attack_cooldown = 0.5;
        return;
    }
    attack_cooldown = ATTACK_COOLDOWN;
    swing_time = 0.15;
    swinging = true;
    swing_hit_pending = true;
    swing_anim = 0.0;
    swing_facing = facing;
    Sfx(SoundEngine::SFX_SWORD_SWING);
}

// The blow: hits every enemy in front of the player (the arc is wider than the player),
// damages them, pushes survivors back and makes them blink.
void RoomView::ApplySwingHit()
{
    swing_hit_pending = false;
    if (!game || !entity_table) return;

    static const double dir_x[4] = { -1, 0, 1, 0 };   // facing: NW NE SE SW
    static const double dir_y[4] = { 0, -1, 0, 1 };
    const double fx = dir_x[swing_facing], fy = dir_y[swing_facing];
    const double cx = player_x + fx * ATTACK_REACH;
    const double cy = player_y + fy * ATTACK_REACH;
    // Longer along the blow than across it is not needed: the arc spreads to both sides.
    const double half_reach = 0.8;

    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        if (!IsAliveEnemy(i)) continue;
        auto& e = entities[i];
        const auto* st = entity_table->GetEnemy(e.type);
        const double half = entity_table->GetHitboxBase(e.type) / 16.0;
        const double top = e.z + entity_table->GetHitboxHeight(e.type) / 16.0;
        if (std::abs(cx - e.x) >= half + half_reach || std::abs(cy - e.y) >= half + half_reach) continue;
        if (player_z >= top || player_z + PLAYER_HEIGHT <= e.z) continue;

        // Health 255 means indestructible: the sword just bounces off.
        if (st->health >= 255)
        {
            SpawnSparks(e.x, e.y, e.z + 1.0, false, false);
            Sfx(SoundEngine::SFX_SWORD_HIT);
            continue;
        }
        int& hp = game->EnemyHealth(room_number, static_cast<int>(i), st->health);
        hp -= static_cast<int>(game->RollPlayerDamage(st->defence) * 256.0);
        if (hp <= 0)
        {
            KillEnemy(i);
        }
        else
        {
            e.hit_flash = HIT_FLASH_TIME;
            e.ai_alert = 6.0;   // it chases for a while, whether or not it saw the player
            // Knockback: slide the enemy away along the blow while the way is free. Big
            // enemies (large hitbox) are pushed back less, bosses hardly at all.
            const int base = entity_table->GetHitboxBase(e.type);
            double knock = KNOCKBACK * 8.0 / std::max(8, base);
            if (base >= 16) knock *= 0.5;
            SpawnSparks(e.x, e.y, e.z + 1.0, false, false);
            Sfx(SoundEngine::SFX_ENEMY_HIT);
            for (double moved = 0.0; moved < knock; moved += 0.1)
            {
                const double nx = e.x + fx * 0.1, ny = e.y + fy * 0.1;
                if (!CanEnemyStandAt(i, nx, ny, e.z)) break;
                e.x = nx;
                e.y = ny;
            }
            ShowMessage("Hit! " + std::to_string((hp + 255) / 256) + " HP left");
        }
    }
}

// Can the enemy's box (half size from its hitbox) stand at (x, y, z)? Cells must be walkable
// for NPCs (bit 0x2000 blocks them), no higher than one step, and the box may not overlap
// the player or another solid entity.
bool RoomView::CanEnemyStandAt(std::size_t index, double x, double y, double z) const
{
    const auto& self = entities[index];
    const double half = entity_table->GetHitboxBase(self.type) / 16.0 - 0.02;
    const double xs[2] = { x - half, x + half };
    const double ys[2] = { y - half, y + half };
    for (double cx : xs)
    {
        for (double cy : ys)
        {
            const int ix = static_cast<int>(std::floor(cx)), iy = static_cast<int>(std::floor(cy));
            const int cell = GetCell(ix, iy);
            if (cell < 0 || (cell & 0x6000) != 0) return false;   // out of map, or blocked
            if (FloorHeight(ix, iy) > z + STEP_UP + 1e-6) return false;
        }
    }
    const double top = z + entity_table->GetHitboxHeight(self.type) / 16.0;

    // Not through the player.
    if (std::abs(x - player_x) < half + PLAYER_RADIUS &&
        std::abs(y - player_y) < half + PLAYER_RADIUS &&
        player_z < top && player_z + PLAYER_HEIGHT > z)
        return false;

    // Not through other solid entities.
    for (std::size_t j = 0; j < entities.size(); ++j)
    {
        if (j == index) continue;
        const auto& o = entities[j];
        if (!o.solid || !o.visible || o.type >= 0xC0 || !entity_table->Has(o.type)) continue;
        if (game && game->IsCollected(room_number, static_cast<int>(j))) continue;
        const double oh = entity_table->GetHitboxBase(o.type) / 16.0;
        const double otop = o.z + entity_table->GetHitboxHeight(o.type) / 16.0;
        if (std::abs(x - o.x) < half + oh && std::abs(y - o.y) < half + oh && z < otop && top > o.z)
            return false;
    }
    return true;
}

namespace { constexpr double LUNGE_TIME_ANIM = 0.7; }   // as LUNGE_TIME below

// ---- Enemy AI ----------------------------------------------------------------------------
// The original gives every enemy family its own state machine (code/gamelogic/ai/enemies).
// They share one idiom, reproduced here: an enemy PATROLS with a small movement script (walk
// until something is in the way, turn at random), SEES the player inside a rectangle in
// front of it (6 cells ahead, 2 behind, 3 to each side for the orcs) and then CHASES along
// the grid axes. Some families add their own trick (see AiKindFor): a jumping LUNGE, floating
// through walls, hopping, bouncing, lying in wait. The exact distances and odds of every
// family are not ported: the numbers below are common values.
namespace
{
enum class AiKind { WALKER, LUNGER, FLOATER, HOPPER, BOUNCER, MIMIC, ROLLER };

AiKind AiKindFor(int type)
{
    switch (type)
    {
    case 4: case 5: case 6:          // orcs
    case 26: case 27: case 28:       // lizards
    case 23: case 24: case 25:       // ninjas
    case 38: case 39: case 40:       // unicorns
        return AiKind::LUNGER;
    case 32: case 33: case 34:       // ghosts
    case 149: case 150: case 151:    // spectres
        return AiKind::FLOATER;
    case 54: case 55: case 56:       // mushrooms
        return AiKind::HOPPER;
    case 60: case 61: case 62:       // bubbles
    case 129: case 130: case 131:
        return AiKind::BOUNCER;
    case 44: case 45: case 46:       // mimics
        return AiKind::MIMIC;
    case 73: case 74: case 141: case 142:   // spiked balls
        return AiKind::ROLLER;
    case 159:                        // Miro: an orc that jumps much more
    case 112:                        // Duke
        return AiKind::LUNGER;
    default:
        return AiKind::WALKER;
    }
}

constexpr double SIGHT_AHEAD = 6.0, SIGHT_BEHIND = 2.0, SIGHT_SIDE = 3.0;   // cells
constexpr double SIGHT_NEAR = 2.2;       // closer than this, an enemy always notices the player
constexpr double LOSE_DISTANCE = 11.0;   // a chase is given up beyond this
constexpr double LOSE_TIME = 5.0;        // ... or after this long without seeing the player
constexpr double ALERT_TIME = 6.0;       // an enemy that was hit keeps chasing for this long
constexpr double LUNGE_MIN = 1.3, LUNGE_MAX = 3.8, LUNGE_TIME = 0.7, LUNGE_HEIGHT = 1.2;
constexpr double LUNGE_COOLDOWN = 1.6, RECOVER_TIME = 0.5;
}

double RoomView::EnemySpeed(const RoomEntity& e) const
{
    // Miro is fast, the stone warriors slow.
    const double factor = e.type == 159 ? 1.5 : e.type == 112 ? 1.3 : (e.type == 167 || e.type == 171) ? 0.8 : 1.0;
    return factor * (ENEMY_BASE_SPEED + 0.6 * std::max(0, e.speed - 1));
}

// Moves an enemy by (dx, dy) cells if its box fits there. Floating enemies only need the map.
bool RoomView::EnemyMove(std::size_t index, double dx, double dy, bool ghost)
{
    auto& e = entities[index];
    const double nx = e.x + dx, ny = e.y + dy;
    if (ghost)
    {
        if (GetCell(static_cast<int>(std::floor(nx)), static_cast<int>(std::floor(ny))) < 0) return false;
    }
    else if (!CanEnemyStandAt(index, nx, ny, e.z))
    {
        return false;
    }
    e.x = nx;
    e.y = ny;
    return true;
}

// ---- Projectiles and bosses --------------------------------------------------------------
// Fireballs, axes and boomerangs are short-lived objects that fly straight (along a grid axis)
// and hurt the player on contact. The bosses of the game each have their own way of using
// them; the patterns here follow the way they play (Ifrit keeps his distance and shoots,
// Gola fires rings of fireballs, Nole walks up and throws returning axes...), not the code.
void RoomView::SpawnProjectile(std::size_t owner, int type, double vx, double vy, int attack, bool returns, double lateral)
{
    if (owner >= entities.size()) return;
    const auto& o = entities[owner];
    Projectile p;
    p.type = type;
    const double len = std::sqrt(vx * vx + vy * vy);
    const double ux = len > 0 ? vx / len : 0.0, uy = len > 0 ? vy / len : 0.0;
    const double half = entity_table ? entity_table->GetHitboxBase(o.type) / 16.0 : 0.5;
    p.x = o.x + ux * (half + 0.3) - uy * lateral;
    p.y = o.y + uy * (half + 0.3) + ux * lateral;
    p.z = o.z + (entity_table ? entity_table->GetHitboxHeight(o.type) / 32.0 : 1.0);
    p.vx = vx;
    p.vy = vy;
    p.attack = attack;
    p.returns = returns;
    p.owner = static_cast<int>(owner);
    if (std::abs(vx) > std::abs(vy)) p.orientation = vx > 0 ? 1 : 3;
    else p.orientation = vy > 0 ? 2 : 0;
    projectiles.push_back(p);
    if (std::getenv("LS_LOG_AI")) SDL_Log("Projectile type %d from enemy %d at (%.1f, %.1f) v (%.1f, %.1f) attack %d%s", type, static_cast<int>(owner), p.x, p.y, vx, vy, attack, returns ? " (returns)" : "");
}

// Damage to the player from something that is not a body: knocked away from (fx, fy).
void RoomView::HurtPlayer(int attack, double from_x, double from_y, bool knockback)
{
    if (dead || invulnerable_time > 0.0 || !game) return;
    invulnerable_time = HIT_INVULNERABILITY;
    hurt_time = HURT_POSE_TIME;
    const double ax = player_x - from_x, ay = player_y - from_y;
    if (std::abs(ax) >= std::abs(ay)) { knock_vx = (ax >= 0 ? 1 : -1) * PLAYER_KNOCK_SPEED; knock_vy = 0.0; }
    else { knock_vy = (ay >= 0 ? 1 : -1) * PLAYER_KNOCK_SPEED; knock_vx = 0.0; }
    knock_time = knockback ? PLAYER_KNOCK_TIME : 0.0;
    SpawnSparks(player_x, player_y, player_z + 1.0, false, true);
    const double dmg = game->RollEnemyDamage(attack);
    const bool killed = game->Damage(dmg);
    if (!killed) Sfx(SoundEngine::SFX_NIGEL_HIT);
    if (killed) Die();
    else
    {
        char text[32];
        std::snprintf(text, sizeof(text), "Ouch! -%.1f HP", dmg);
        ShowMessage(text);
    }
}

void RoomView::UpdateProjectiles(double dt)
{
    if (projectiles.empty()) return;
    for (auto& p : projectiles)
    {
        p.age += dt;
        if (p.returns)
        {
            // A boomerang: out for 0.9 s, then back to whoever threw it.
            if (p.age > 0.9 && p.owner >= 0 && static_cast<std::size_t>(p.owner) < entities.size())
            {
                const auto& o = entities[p.owner];
                const double dx = o.x - p.x, dy = o.y - p.y, d = std::sqrt(dx * dx + dy * dy);
                const double speed = std::sqrt(p.vx * p.vx + p.vy * p.vy);
                if (d < 0.6) { p.age = 1e9; continue; }
                p.vx = dx / d * speed;
                p.vy = dy / d * speed;
            }
        }
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        if (std::abs(p.vx) > std::abs(p.vy)) p.orientation = p.vx > 0 ? 1 : 3;
        else p.orientation = p.vy > 0 ? 2 : 0;
        // Walls stop it (a returning axe flies on: it is going home).
        const int cell = GetCell(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)));
        if (cell < 0 || ((cell & 0x6000) != 0 && !(p.returns && p.age > 0.9)))
        {
            SpawnSparks(p.x, p.y, p.z, false, false);
            p.age = 1e9;
            continue;
        }
        // The player: the projectile is about 0.4 cells wide.
        if (!dead && std::abs(p.x - player_x) < PLAYER_RADIUS + 0.25 && std::abs(p.y - player_y) < PLAYER_RADIUS + 0.25 &&
            p.z + 0.3 > player_z && p.z - 0.3 < player_z + PLAYER_HEIGHT)
        {
            if (invulnerable_time <= 0.0)
            {
                HurtPlayer(p.attack, p.x - p.vx, p.y - p.vy);
                p.age = 1e9;
            }
        }
    }
    projectiles.erase(std::remove_if(projectiles.begin(), projectiles.end(),
                                     [](const Projectile& p) { return p.age > 6.0; }), projectiles.end());
}

// Aims along the grid axis that separates the boss from the player.
void RoomView::BossFire(std::size_t i, int type, double speed, int attack, int count, bool returns)
{
    const auto& e = entities[i];
    const double dx = player_x - e.x, dy = player_y - e.y;
    double vx = 0.0, vy = 0.0;
    if (std::abs(dx) >= std::abs(dy)) vx = (dx >= 0 ? 1 : -1) * speed;
    else vy = (dy >= 0 ? 1 : -1) * speed;
    for (int k = 0; k < count; ++k)
        SpawnProjectile(i, type, vx, vy, attack, returns, (k - (count - 1) / 2.0) * 0.9);
    Sfx(0x71);   // SND_Fireball1
}

// Boss patterns. Returns true if the entity was handled here.
bool RoomView::UpdateBoss(std::size_t i, double dt)
{
    auto& e = entities[i];
    const auto* st = entity_table->GetEnemy(e.type);
    if (!st) return false;
    const double dx = player_x - e.x, dy = player_y - e.y;
    const double dist = std::sqrt(dx * dx + dy * dy);
    const double speed = EnemySpeed(e);
    const int shot = std::max(1, st->attack / 2);
    bool moved = false;
    auto face_player = [&]() {
        if (std::abs(dx) >= std::abs(dy)) e.orientation = dx > 0 ? 1 : 3;
        else e.orientation = dy > 0 ? 2 : 0;
    };
    auto away_step = [&](double step) {
        // Backs off along the axis that separates them most.
        if (std::abs(dx) >= std::abs(dy)) return EnemyMove(i, dx > 0 ? -step : step, 0.0, false);
        return EnemyMove(i, 0.0, dy > 0 ? -step : step, false);
    };
    auto toward_step = [&](double step) {
        if (std::abs(dx) >= std::abs(dy)) return EnemyMove(i, dx > 0 ? step : -step, 0.0, false);
        return EnemyMove(i, 0.0, dy > 0 ? step : -step, false);
    };
    e.ai_cooldown -= dt;
    // The attack pose starts as the telegraph begins and runs 0.9 s (the shot is in the middle).
    if (e.act_t >= 0.0)
    {
        e.act_t += dt;
        if (e.act_t >= 0.9) e.act_t = -1.0;
    }
    else if ((e.type == 124 || e.type == 133 || e.type == 160 || e.type == 162 || e.type == 165) &&
             e.ai_cooldown > 0.0 && e.ai_cooldown < 0.45)
    {
        e.act_t = 0.0;
    }

    switch (e.type)
    {
    case 160:   // Ifrit: stays 4 to 6 cells away and shoots, three at a time now and then
        if (dist > 16.0) break;
        face_player();
        if (dist < 3.5) moved = away_step(speed * dt);
        else if (dist > 6.0) moved = toward_step(speed * dt);
        if (e.ai_cooldown <= 0.0)
        {
            e.ai_shots++;
            BossFire(i, 161, 4.5, shot, (e.ai_shots % 3 == 0) ? 3 : 1, false);
            e.ai_cooldown = 1.4 + (std::rand() % 100) / 100.0;
        }
        break;
    case 162:   // Gola: stands still; rings of fireballs (all four directions) and aimed ones
        if (dist > 18.0) break;
        face_player();
        if (e.ai_cooldown <= 0.0)
        {
            e.ai_shots++;
            if (e.ai_shots % 2 == 0)
            {
                static const double vx[4] = { 0, 1, 0, -1 }, vy[4] = { -1, 0, 1, 0 };
                for (int d = 0; d < 4; ++d) SpawnProjectile(i, 172, vx[d] * 3.5, vy[d] * 3.5, shot, false, 0.0);
                Sfx(0x71);
            }
            else
            {
                BossFire(i, 172, 4.0, shot, 2, false);
            }
            e.ai_cooldown = 2.0 + (std::rand() % 100) / 100.0;
        }
        break;
    case 165:   // Nole: walks at the player and throws axes that come back
        if (dist > 16.0) break;
        face_player();
        if (dist > 1.6) moved = toward_step(speed * 0.8 * dt);
        if (e.ai_cooldown <= 0.0 && dist > 2.0)
        {
            BossFire(i, 166, 5.0, shot, 1, true);
            e.ai_cooldown = 3.0 + (std::rand() % 150) / 100.0;
        }
        break;
    case 133:   // Zak: closes in and throws boomerangs
        if (dist > 16.0) break;
        face_player();
        if (dist > 2.5) moved = toward_step(speed * dt);
        else if (dist < 1.5) moved = away_step(speed * 0.6 * dt);
        if (e.ai_cooldown <= 0.0 && dist > 1.8)
        {
            BossFire(i, 168, 4.5, shot, 1, true);
            e.ai_cooldown = 2.4 + (std::rand() % 120) / 100.0;
        }
        break;
    case 124:   // Mir: keeps away and casts small fireballs
        if (dist > 16.0) break;
        face_player();
        if (dist < 3.0) moved = away_step(speed * dt);
        else if (dist > 5.5) moved = toward_step(speed * dt);
        if (e.ai_cooldown <= 0.0)
        {
            BossFire(i, 135, 4.0, std::max(4, st->attack * 2), 1, false);
            e.ai_cooldown = 1.3 + (std::rand() % 100) / 100.0;
        }
        break;
    case 157: case 170:   // spinners: dash at the player along an axis, bounce off whatever is in the way
        if (dist > 18.0) break;
        if (e.ai_state == RoomEntity::AI_LUNGE)
        {
            e.ai_time += dt;
            if (EnemyMove(i, e.ai_vx * dt, e.ai_vy * dt, false)) moved = true;
            if (!moved || e.ai_time > 1.6) { e.ai_state = RoomEntity::AI_RECOVER; e.ai_time = 0.0; }
        }
        else
        {
            e.ai_time += dt;
            if (e.ai_time > 0.5)
            {
                const double sp = speed * 2.6;
                if (std::abs(dx) >= std::abs(dy)) { e.ai_vx = (dx > 0 ? 1 : -1) * sp; e.ai_vy = 0.0; }
                else { e.ai_vx = 0.0; e.ai_vy = (dy > 0 ? 1 : -1) * sp; }
                e.ai_state = RoomEntity::AI_LUNGE;
                e.ai_time = 0.0;
            }
        }
        break;
    default:
        return false;
    }
    if (std::getenv("LS_LOG_AI"))
        SDL_Log("AI boss %d type %d state %d at (%.2f, %.2f) dist %.1f", static_cast<int>(i), e.type, e.ai_state, e.x, e.y, dist);
    e.walking = moved;
    e.walk_time = moved ? e.walk_time + dt : 0.0;
    if (e.gravity) e.z = static_cast<double>(FloorHeight(static_cast<int>(std::floor(e.x)), static_cast<int>(std::floor(e.y))));
    return true;
}

void RoomView::UpdateEnemies(double dt)
{
    if (dead || !game || !entity_table) return;
    static const double dir_x[4] = { 0, 1, 0, -1 };   // 0=NE 1=SE 2=SW 3=NW
    static const double dir_y[4] = { -1, 0, 1, 0 };
    static const bool log_ai = std::getenv("LS_LOG_AI") != nullptr;
    UpdateEntityAnimations(dt);

    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        if (!IsAliveEnemy(i)) continue;
        auto& e = entities[i];
        if (e.hit_flash > 0.0) { e.walking = false; continue; }
        if (UpdateBoss(i, dt)) continue;
        if (e.speed <= 0) { e.walking = false; continue; }

        // Enemies swing when the player is within reach (the shooters, handled above, have
        // their own clock).
        if (e.act_t >= 0.0)
        {
            e.act_t += dt;
            if (e.act_t >= AttackAnimTime(e.type)) e.act_t = -1.0;
        }
        else if (std::hypot(player_x - e.x, player_y - e.y) < 1.8 && e.ai_state != RoomEntity::AI_LUNGE &&
                 !(e.type >= 60 && e.type <= 62))
        {
            e.act_t = 0.0;
        }

        const AiKind kind = AiKindFor(e.type);
        const double speed = EnemySpeed(e);
        const double dx = player_x - e.x, dy = player_y - e.y;
        const double dist = std::sqrt(dx * dx + dy * dy);
        const int dir = e.orientation & 3;
        e.ai_cooldown = std::max(0.0, e.ai_cooldown - dt);
        e.ai_alert = std::max(0.0, e.ai_alert - dt);
        bool moved = false;

        // Does the enemy see the player? (a rectangle in front of it, or very close)
        const double ahead = dx * dir_x[dir] + dy * dir_y[dir];
        const double side = std::abs(dx * -dir_y[dir] + dy * dir_x[dir]);
        const bool arena_boss = e.type == 159 || e.type == 167 || e.type == 171 || e.type == 112;   // always know where the player is
        const bool sees = dist < SIGHT_NEAR || e.ai_alert > 0.0 || (arena_boss && dist < 16.0) ||
                          (ahead > -SIGHT_BEHIND && ahead < SIGHT_AHEAD && side < SIGHT_SIDE);

        // Chasing step along one grid axis at a time, like the original (no diagonals).
        auto chase_step = [&](double step) {
            const double adx = std::abs(dx), ady = std::abs(dy);
            constexpr double HYSTERESIS = 0.4;
            int axis = e.move_axis;
            if (axis == 0 && ady > adx + HYSTERESIS) axis = 1;
            else if (axis == 1 && adx > ady + HYSTERESIS) axis = 0;
            auto try_axis = [&](int a) {
                const double d = (a == 0) ? dx : dy;
                if (std::abs(d) < 0.02) return false;
                const double m = std::copysign(std::min(step, std::abs(d)), d);
                return a == 0 ? EnemyMove(i, m, 0.0, false) : EnemyMove(i, 0.0, m, false);
            };
            int moved_axis = -1;
            if (try_axis(axis)) moved_axis = axis;
            else if (try_axis(1 - axis)) moved_axis = 1 - axis;
            if (moved_axis >= 0)
            {
                e.move_axis = moved_axis;
                if (moved_axis == 0) e.orientation = dx > 0 ? 1 : 3;
                else e.orientation = dy > 0 ? 2 : 0;
                return true;
            }
            return false;
        };
        auto floor_z = [&]() {
            return static_cast<double>(FloorHeight(static_cast<int>(std::floor(e.x)), static_cast<int>(std::floor(e.y))));
        };

        switch (e.ai_state)
        {
        case RoomEntity::AI_LUNGE:
        {
            // A jump towards the player along one axis: it keeps going whatever happens.
            e.ai_time += dt;
            const double t = std::min(1.0, e.ai_time / LUNGE_TIME);
            if (EnemyMove(i, e.ai_vx * dt, e.ai_vy * dt, false)) moved = true;
            e.z = floor_z() + std::sin(3.14159265 * t) * LUNGE_HEIGHT;
            if (e.ai_time >= LUNGE_TIME)
            {
                e.z = floor_z();
                e.ai_state = RoomEntity::AI_RECOVER;
                e.ai_time = 0.0;
            }
            break;
        }
        case RoomEntity::AI_RECOVER:
            e.ai_time += dt;
            if (e.gravity) e.z = floor_z();
            if (e.ai_time >= RECOVER_TIME)
            {
                e.ai_state = RoomEntity::AI_CHASE;
                e.ai_cooldown = e.type == 159 ? 0.9 : LUNGE_COOLDOWN;
                e.ai_time = 0.0;
            }
            break;
        case RoomEntity::AI_CHASE:
        {
            e.ai_time += sees ? -e.ai_time : dt;   // time since the player was last seen
            if (dist > LOSE_DISTANCE || e.ai_time > LOSE_TIME)
            {
                e.ai_state = RoomEntity::AI_IDLE;
                e.ai_time = 0.0;
                break;
            }
            if (kind == AiKind::FLOATER)
            {
                // Floats straight at the player through walls and furniture.
                if (dist > 0.1)
                {
                    const double m = std::min(speed * dt, dist);
                    moved = EnemyMove(i, dx / dist * m, dy / dist * m, true);
                    if (std::abs(dx) > std::abs(dy)) e.orientation = dx > 0 ? 1 : 3;
                    else e.orientation = dy > 0 ? 2 : 0;
                }
                e.z = floor_z() + 1.0 + 0.25 * std::sin(e.ai_phase += dt * 3.0);
                break;
            }
            if (kind == AiKind::HOPPER)
            {
                // Short hops towards the player, a pause between two of them.
                e.ai_time2 += dt;
                const double hop = 0.55, pause = 0.8;
                const double cycle = std::fmod(e.ai_time2, hop + pause);
                if (cycle < hop && dist > 0.6)
                {
                    moved = chase_step(speed * 2.0 * dt);
                    e.z = floor_z() + std::sin(3.14159265 * cycle / hop) * 0.8;
                }
                else if (e.gravity)
                {
                    e.z = floor_z();
                }
                break;
            }
            if (dist > 0.3) moved = chase_step(speed * dt);
            if (e.gravity && e.ai_state == RoomEntity::AI_CHASE) e.z = floor_z();
            if ((kind == AiKind::LUNGER || kind == AiKind::MIMIC) && e.ai_cooldown <= 0.0 &&
                dist > LUNGE_MIN && dist < LUNGE_MAX && (std::rand() % 1000) < (e.type == 159 ? 35 : 12))
            {
                // Jump at the player along the axis that separates them most.
                const double adx = std::abs(dx), ady = std::abs(dy);
                const double reach = std::min(dist + 0.4, 3.2);
                const double speed_l = reach / LUNGE_TIME;
                if (adx >= ady) { e.ai_vx = (dx > 0 ? 1 : -1) * speed_l; e.ai_vy = 0.0; e.orientation = dx > 0 ? 1 : 3; }
                else { e.ai_vx = 0.0; e.ai_vy = (dy > 0 ? 1 : -1) * speed_l; e.orientation = dy > 0 ? 2 : 0; }
                e.ai_state = RoomEntity::AI_LUNGE;
                e.ai_time = 0.0;
            }
            break;
        }
        default:   // AI_IDLE
        {
            if (kind == AiKind::FLOATER && !sees)
            {
                e.z = floor_z() + 1.0 + 0.25 * std::sin(e.ai_phase += dt * 3.0);   // bobs on the spot
                break;
            }
            if (sees && kind != AiKind::BOUNCER && kind != AiKind::ROLLER &&
                (kind != AiKind::MIMIC || dist < 3.0) && (kind != AiKind::HOPPER || dist < 5.0))
            {
                e.ai_state = RoomEntity::AI_CHASE;
                e.ai_time = 0.0;
                e.ai_time2 = 0.0;
                if (kind == AiKind::LUNGER) e.ai_cooldown = std::max(e.ai_cooldown, 0.4);
                break;
            }
            if (kind == AiKind::MIMIC || kind == AiKind::HOPPER)
            {
                if (e.gravity) e.z = floor_z();
                break;   // waits where it is
            }
            // Patrol script: walk along the facing direction until blocked, then turn at
            // random; now and then stop for a moment. Rollers and bubbles never stop.
            const double patrol_speed = (kind == AiKind::ROLLER || kind == AiKind::BOUNCER) ? speed : speed * 0.5;
            e.ai_time -= dt;
            if (e.ai_time <= 0.0)
            {
                const bool pause = kind == AiKind::WALKER || kind == AiKind::LUNGER ? (std::rand() % 100) < 40 : false;
                e.ai_walking = !pause;
                e.ai_time = pause ? 0.8 + (std::rand() % 100) / 100.0 : 1.0 + (std::rand() % 200) / 100.0;
                if (std::rand() % 100 < 50) e.orientation = std::rand() & 3;
            }
            if (e.ai_walking)
            {
                const int d = e.orientation & 3;
                if (EnemyMove(i, dir_x[d] * patrol_speed * dt, dir_y[d] * patrol_speed * dt, false)) moved = true;
                else
                {
                    // Blocked: a roller turns back, the others turn at random.
                    e.orientation = kind == AiKind::ROLLER ? (d + 2) & 3 : std::rand() & 3;
                    if (kind != AiKind::ROLLER) e.ai_time = std::min(e.ai_time, 0.2);
                }
            }
            if (e.gravity) e.z = floor_z();
            break;
        }
        }

        if (log_ai)
            SDL_Log("AI enemy %d type %d state %d at (%.2f, %.2f) dist %.1f", static_cast<int>(i), e.type, e.ai_state, e.x, e.y, dist);
        e.walking = moved;
        e.walk_time = moved ? e.walk_time + dt : 0.0;
    }
}

void RoomView::KillEnemy(std::size_t i)
{
    auto& e = entities[i];
    const auto* st = entity_table ? entity_table->GetEnemy(e.type) : nullptr;
    if (!st || !game) return;
            // Dead for good right away (flags, no coming back), but the body stays for a death
            // animation: a flicker, long and shaking the screen for a boss.
            game->MarkCollected(room_number, static_cast<int>(i));
            OnEntityGone(i);
            const bool boss = IsBossType(e.type);
            e.dying = true;
            e.die_t = 0.0;
            e.die_dur = boss ? 1.8 : 0.45;
            e.die_spark = 0.0;
            e.pending_gold = st->gold_drop;
            SpawnSparks(e.x, e.y, e.z + 1.0, true, false);
            Sfx(boss ? 0x6D : SoundEngine::SFX_ENEMY_DIE);   // SND_Explosion for a boss
            if (boss) StartShake(1.8, 3.0);
            ShowMessage("Enemy defeated");
}

// What an enemy is doing when it is not simply walking or standing: swinging (act_t, or the
// jump of a lunge) or hurt (just hit, or dying). Gives the frame to draw, or false.
bool RoomView::EnemyPoseFrame(const RoomEntity& e, EntityTable::Image& out, bool& flip)
{
    if (!entity_table || !entity_table->GetEnemy(e.type)) return false;
    const auto kind = entity_table->GetKind(e.type, e.orientation, flip);
    if (kind == EntityTable::DEFAULT) return false;
    EntityTable::Pose pose;
    double progress = 0.0;
    bool loop = false;
    if (e.dying || e.hit_flash > 0.0) pose = EntityTable::HURT;
    else if (e.act_t >= 0.0) { pose = EntityTable::ATTACK; progress = e.act_t / AttackAnimTime(e.type); }
    else if (e.ai_state == RoomEntity::AI_LUNGE)
    {
        pose = EntityTable::ATTACK;
        loop = e.type == 157 || e.type == 170;   // spinners keep spinning
        progress = loop ? e.ai_time * 2.0 : e.ai_time / LUNGE_TIME_ANIM;
    }
    else return false;
    const auto& frames = entity_table->GetPoseFrames(e.type, pose, kind);
    if (frames.empty()) return false;
    std::size_t k;
    if (loop) k = static_cast<std::size_t>(progress * frames.size()) % frames.size();
    else k = std::min(frames.size() - 1, static_cast<std::size_t>(std::max(0.0, progress) * frames.size()));
    out = frames[k];
    return out.texture != nullptr;
}

bool RoomView::IsBossType(int type)
{
    switch (type)
    {
    case 112: case 124: case 133: case 157: case 159: case 160: case 162: case 165: case 167: case 170: case 171:
        return true;
    default:
        return false;
    }
}

// Dying entities flicker and, for a boss, burst into sparks; materialising ones fade in.
void RoomView::UpdateEntityAnimations(double dt)
{
    std::vector<std::size_t> finished;
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        auto& e = entities[i];
        if (e.appear_t >= 0.0)
        {
            e.appear_t += dt;
            if (e.appear_t > 0.9) e.appear_t = -1.0;
        }
        if (!e.dying) continue;
        e.die_t += dt;
        e.die_spark -= dt;
        if (e.die_dur > 1.0 && e.die_spark <= 0.0)
        {
            const double ox = (std::rand() % 100 - 50) / 100.0, oy = (std::rand() % 100 - 50) / 100.0;
            SpawnSparks(e.x + ox, e.y + oy, e.z + 0.5 + (std::rand() % 100) / 100.0 * 1.5, true, false);
            if (std::rand() % 3 == 0) Sfx(0x6D);
            e.die_spark = 0.18;
        }
        if (e.die_t >= e.die_dur) finished.push_back(i);
    }
    for (std::size_t i : finished) FinishDying(i);
}

void RoomView::FinishDying(std::size_t index)
{
    auto& e = entities[index];
    const double x = e.x, y = e.y, z = e.z, gold = e.pending_gold;
    const int type = e.type;
    e.dying = false;
    e.visible = false;
    e.base_visible = false;
    if (IsBossType(type)) Sfx(SoundEngine::SFX_ENEMY_DIE);
    SpawnDrop(x, y, z, type, static_cast<int>(gold));   // may move the vector: `e` is not used after this
}

// ---- Behaviour scripts -------------------------------------------------------------------
// The original drives every entity with a small script (game/Behaviours.h). The commands
// that move, turn, wait, show / hide, play sounds and handle flags are interpreted here for
// the friendly characters and objects; enemies keep the chase AI above. A command the
// interpreter does not know ends the script (the entity stays where it is).

// Moves the entity one tick along its facing direction. False if something is in the way.
bool RoomView::BehaviourStep(std::size_t index, bool noclip)
{
    auto& e = entities[index];
    static const double dir_x[4] = { 0, 1, 0, -1 };   // 0=NE 1=SE 2=SW 3=NW
    static const double dir_y[4] = { -1, 0, 1, 0 };
    const double d = e.bh_speed / 16.0;
    const double nx = e.x + dir_x[e.orientation & 3] * d, ny = e.y + dir_y[e.orientation & 3] * d;
    // Never out of the map. The intro's actors fly over everything for timed moves: their
    // paths were made for another collision model.
    if (GetCell(static_cast<int>(std::floor(nx)), static_cast<int>(std::floor(ny))) < 0) return false;
    // Rafts (type 53) glide over the water of their river, and a rolling boulder (type 20) follows
    // its corridor whatever its big box says: the heightmap does not allow either.
    if (!noclip && e.type != 53 && e.type != 20 && entity_table && entity_table->Has(e.type) && !CanEnemyStandAt(index, nx, ny, e.z)) return false;
    e.x = nx;
    e.y = ny;
    if (e.gravity) e.z = FloorHeight(static_cast<int>(std::floor(e.x)), static_cast<int>(std::floor(e.y)));
    return true;
}

void RoomView::BehaviourTick(std::size_t index, bool& moved)
{
    auto& e = entities[index];
    const auto* script = behaviours->Script(e.behaviour);
    if (!script) { e.bh_pc = -1; return; }
    const int len = static_cast<int>(script->size());
    auto byte_at = [&](int at) { return at >= 0 && at < len ? static_cast<int>((*script)[at]) : 0; };
    auto turn = [&](int dir) { e.orientation = dir & 3; };

    for (int guard = 0; guard < 32; ++guard)
    {
        if (e.bh_pc < 0 || e.bh_pc >= len) { e.bh_pc = -1; return; }
        const int cmd = byte_at(e.bh_pc), par = byte_at(e.bh_pc + 1);
        static const bool trace = std::getenv("LS_TRACE_BH") != nullptr;
        if (trace) SDL_Log("BH trace entity %d type %d pc %d cmd %02X par %02X", static_cast<int>(index), e.type, e.bh_pc, cmd, par);
        if (cmd == 0 && par == 0 && e.bh_pc == 0 && len == 2) { e.bh_pc = -1; return; }   // idle script

        switch (cmd)
        {
        case 0x00:   // Pause
            if (!e.bh_busy) { e.bh_busy = true; e.bh_count = par ? par : 256; }
            if (--e.bh_count > 0) return;
            e.bh_busy = false;
            e.bh_pc += 2;
            return;
        case 0x01:   // MoveTimed: moves for `par` ticks, blocked or not
        case 0x11:   // MoveRelative: waits while blocked
        {
            if (!e.bh_busy) { e.bh_busy = true; e.bh_count = par ? par : 256; }
            const bool ok = BehaviourStep(index, intro_active);
            if (ok) moved = true;
            if (cmd == 0x11 && !ok) return;
            if (--e.bh_count > 0) return;
            e.bh_busy = false;
            e.bh_pc += 2;
            if (cmd == 0x11) return;
            continue;
        }
        case 0x4F:   // MoveNoClip: walks through anything for `par` ticks
        {
            if (!e.bh_busy) { e.bh_busy = true; e.bh_count = par ? par : 256; }
            static const double dir_x[4] = { 0, 1, 0, -1 };
            static const double dir_y[4] = { -1, 0, 1, 0 };
            e.x += dir_x[e.orientation & 3] * e.bh_speed / 16.0;
            e.y += dir_y[e.orientation & 3] * e.bh_speed / 16.0;
            moved = true;
            if (--e.bh_count > 0) return;
            e.bh_busy = false;
            e.bh_pc += 2;
            continue;
        }
        case 0x30: case 0x31:   // (unknown timed commands): only the time is kept
            if (!e.bh_busy) { e.bh_busy = true; e.bh_count = par ? par : 256; }
            if (--e.bh_count > 0) return;
            e.bh_busy = false;
            e.bh_pc += 2;
            return;
        case 0x3A: case 0x3B:   // MoveUpTimed / MoveDownTimed: elevators, platforms (1/16 unit per tick at speed 1)
            if (!e.bh_busy) { e.bh_busy = true; e.bh_count = par ? par : 256; }
            e.z += (cmd == 0x3A ? 1.0 : -1.0) * e.bh_speed / 16.0;
            if (--e.bh_count > 0) return;
            e.bh_busy = false;
            e.bh_pc += 2;
            return;
        case 0x3C: case 0x3D:   // MoveUpAbsolute / MoveDownAbsolute(z in 1/16 units)
        {
            const double target = par / 16.0, step = e.bh_speed / 16.0;
            if (cmd == 0x3C) e.z = std::min(target, e.z + step);
            else e.z = std::max(target, e.z - step);
            if (e.z != target) return;
            e.bh_pc += 2;
            return;
        }
        case 0x13:   // MoveUntilCollision
            if (BehaviourStep(index)) { moved = true; return; }
            e.bh_pc += 1;
            continue;
        case 0x2A:   // MoveRandomTime(range, max)
            if (!e.bh_busy)
            {
                e.bh_busy = true;
                const int range = std::max(1, par);
                e.bh_count = std::max(1, byte_at(e.bh_pc + 2) - (std::rand() % range) + 1);
            }
            if (BehaviourStep(index, intro_active)) moved = true;
            if (--e.bh_count > 0) return;
            e.bh_busy = false;
            e.bh_pc += 3;
            continue;
        case 0x02: case 0x08: turn(e.orientation + 1); e.bh_pc += 1; return;
        case 0x03: case 0x09: turn(e.orientation + 3); e.bh_pc += 1; return;
        case 0x04: case 0x0A: turn(0); e.bh_pc += 1; return;
        case 0x05: case 0x0B: turn(1); e.bh_pc += 1; return;
        case 0x06: case 0x0C: turn(2); e.bh_pc += 1; return;
        case 0x07: case 0x0D: turn(3); e.bh_pc += 1; return;
        case 0x14: case 0x15: turn(std::rand() & 3); e.bh_pc += 1; return;
        case 0x23: case 0x24: turn(e.orientation + 2); e.bh_pc += 1; return;
        // the "immediate" variants carry on with the next command in the same tick
        case 0x17: turn(std::rand() & 3); e.bh_pc += 1; continue;
        case 0x18: turn(e.orientation + 1); e.bh_pc += 1; continue;
        case 0x19: turn(e.orientation + 3); e.bh_pc += 1; continue;
        case 0x1A: turn(0); e.bh_pc += 1; continue;
        case 0x1B: turn(1); e.bh_pc += 1; continue;
        case 0x1C: turn(2); e.bh_pc += 1; continue;
        case 0x1D: turn(3); e.bh_pc += 1; continue;
        case 0x25: turn(e.orientation + 2); e.bh_pc += 1; continue;
        case 0x12:   // GoTo: signed offset from this command
            e.bh_pc += static_cast<int8_t>(par);
            continue;
        case 0x1F: e.bh_speed = 1; e.bh_pc += 1; continue;
        case 0x20: e.bh_speed = 2; e.bh_pc += 1; continue;
        case 0x21: e.bh_speed = 4; e.bh_pc += 1; continue;
        case 0x22: e.bh_speed = 8; e.bh_pc += 1; continue;
        case 0x0E: e.visible = e.base_visible = true; e.bh_pc += 1; continue;
        case 0x0F: e.visible = e.base_visible = false; e.bh_pc += 1; continue;
        case 0x44: e.visible = false; e.bh_pc += 1; continue;   // Hide
        case 0x45: e.visible = true; e.bh_pc += 1; continue;    // ShowWhenCollisionClear
        case 0x3E:   // HideSprite: the entity is gone for good
            e.visible = false;
            e.bh_pc = -1;
            return;
        case 0x41: case 0x43:   // SetFlag / ClearFlag (flag byte, bit)
        {
            const int flag = par * 8 + byte_at(e.bh_pc + 2);
            if (game)
            {
                if (cmd == 0x41) SetStoryFlag(flag);
                else game->ClearFlag(flag);
                ApplyFlagRules();
            }
            e.bh_pc += 3;
            continue;
        }
        case 0x42: case 0x46:   // WaitForFlagSet / WaitForFlagClear
        {
            const int flag = par * 8 + byte_at(e.bh_pc + 2);
            const bool set = game && game->IsFlagSet(flag);
            if (set != (cmd == 0x42)) return;
            e.bh_pc += 3;
            continue;
        }
        case 0x62:   // PlaySound
            Sfx(par);
            e.bh_pc += 2;
            continue;
        case 0x4B:   // MoveToXYPosImmediate(x, y): placed on the cell
            e.x = par + 0.5;
            e.y = byte_at(e.bh_pc + 2) + 0.5;
            if (e.gravity) e.z = FloorHeight(par, byte_at(e.bh_pc + 2));
            e.bh_pc += 3;
            continue;
        case 0x50:   // RotatePlayer(direction: 0 NE 1 SE 2 SW 3 NW)
        {
            static const int to_facing[4] = { 1, 2, 3, 0 };   // entity direction -> player facing
            facing = to_facing[par & 3];
            e.bh_pc += 2;
            continue;
        }
        case 0x57:   // TriggerTileSwap(trigger)
            DoTileSwapTrigger(par);
            e.bh_pc += 2;
            continue;
        case 0x4C: case 0x47: case 0x5A:   // z position / wait on another entity / ...: not ported
            e.bh_pc += 2;
            continue;
        case 0x5D:   // RepeatBegin(count)
            if (par != 0) e.bh_repeat_count = par;
            e.bh_pc += 2;
            e.bh_repeat_pc = e.bh_pc;
            continue;
        case 0x5E:   // RepeatEnd
            if (--e.bh_repeat_count > 0) e.bh_pc = e.bh_repeat_pc;
            else e.bh_pc += 1;
            continue;
        case 0x40:   // MoveDownUntilCollision: down to the floor under it
        {
            const double half = entity_table && entity_table->Has(e.type) ? entity_table->GetHitboxBase(e.type) / 16.0 : 0.5;
            double floor_h = 0.0;
            for (int cx = static_cast<int>(std::floor(e.x - half + 0.01)); cx <= static_cast<int>(std::floor(e.x + half - 0.01)); ++cx)
                for (int cy = static_cast<int>(std::floor(e.y - half + 0.01)); cy <= static_cast<int>(std::floor(e.y + half - 0.01)); ++cy)
                    if (IsWalkable(cx, cy) && FloorHeight(cx, cy) <= e.z + 1e-6) floor_h = std::max(floor_h, static_cast<double>(FloorHeight(cx, cy)));
            const double step = e.bh_speed / 16.0;
            if (e.z - step <= floor_h) { e.z = floor_h; e.bh_pc += 1; continue; }
            e.z -= step;
            return;
        }
        case 0x3F: case 0x56:   // nudge up / back to start: not ported
            e.bh_pc += 1;
            continue;
        case 0x5F: case 0x60: case 0x61: case 0x63: case 0x65: case 0x66:   // flashes, effects (2 bytes)
            e.bh_pc += 2;
            continue;
        case 0x34:   // PlaybackInput(sequence): the player follows a recorded input
            StartPlayback(par);
            e.bh_pc += 2;
            continue;
        case 0x64:   // StartHiCutscene(n): action 0x100 + n
            e.bh_pc += 2;
            cutscene_source = static_cast<int>(index);
            TriggerAction(0x100 + par);
            continue;
        case 0x36:   // WaitForCondition(trigger)
            if (!TriggerHolds(par, index)) return;
            e.bh_pc += 2;
            continue;
        case 0x38:   // EnableGravity: the entity drops until it rests on something
            e.falls = true;
            e.fall_v = 0.0;
            e.bh_pc += 1;
            continue;
        case 0x4E:   // TriggerCutscene(action id)
        {
            // A scene that waited for a flag plays once: entering the room again must not start
            // it again (its scripts restart with the room). Remembered as a private flag.
            const bool waits_first = (*script)[0] == 0x42 || (*script)[0] == 0x46;
            const int played = 4096 + par;
            if (waits_first && game && game->IsFlagSet(played)) { e.bh_pc = -1; return; }
            if (waits_first && game) game->SetFlag(played);
            e.bh_pc += 2;
            cutscene_source = static_cast<int>(index);
            TriggerAction(par);
            continue;
        }
        // Commands with no effect here (jump, rotation, gravity, hostility, facing refresh...)
        case 0x51:   // MakeHostile
            e.hostile = true;
            e.dormant = false;
            if (std::getenv("LS_LOG_BH")) SDL_Log("BH entity %d type %d made hostile (visible %d)", static_cast<int>(index), e.type, e.visible);
            e.bh_pc += 1;
            continue;
        case 0x52:   // MakeNonHostile
            e.hostile = false;
            e.bh_pc += 1;
            continue;
        case 0x2B:   // LoadSpecialAI: from here the enemy AI drives the entity
            e.dormant = false;
            e.bh_pc = -1;
            return;
        case 0x37:   // "Pause4s": plays an idle recording that keeps the player still for 254 frames
            input_hold = 254.0 / 60.0;
            e.bh_pc += 1;
            continue;
        case 0x1E: case 0x27: case 0x28: case 0x29: case 0x35: case 0x39:
        case 0x53: case 0x54: case 0x58:
            e.bh_pc += 1;
            continue;
        default:   // unknown (LoadSpecialAI, climbing, following the player...): stop
            if (std::getenv("LS_LOG_BH"))
                SDL_Log("BH entity %d type %d behaviour %d stops at command %02X (offset %d)", static_cast<int>(index), e.type, e.behaviour, cmd, e.bh_pc);
            e.bh_pc = -1;
            return;
        }
    }
}

// An enemy that can be killed (not a boulder) and is still there: what the "room cleared" and
// "enemies about" conditions look at.
bool RoomView::KillableEnemyExists() const
{
    if (!entity_table || !game) return false;
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        const auto& e = entities[i];
        if (!e.hostile || e.dying || game->IsCollected(room_number, static_cast<int>(i))) continue;
        const auto* st = entity_table->GetEnemy(e.type);
        if (st && st->health != 255) return true;
    }
    return false;
}

// Conditions of WaitForCondition (code/scripthandling/triggeractions.asm). Only the ones the
// first dungeon uses are ported; the others are taken as true.
bool RoomView::TriggerHolds(int trigger, std::size_t index)
{
    switch (trigger)
    {
    case 0:   // First dungeon: boulder trap. Once one of the six blocks has been set falling (somebody
              // stood on it), all of them fall, with a rumble, and the boulder is released.
    {
        bool any = false;
        const std::size_t n = std::min<std::size_t>(6, entities.size());
        for (std::size_t i = 0; i < n; ++i) any = any || entities[i].falls;
        if (!any) return false;
        for (std::size_t i = 0; i < n; ++i)
            if (!entities[i].falls) { entities[i].falls = true; entities[i].fall_v = 0.0; }
        Sfx(0x59);   // SND_Rumble
        StartShake(0.8, 2.0);
        return true;
    }
    case 1:   // somebody (the player) stands on this entity
        return support_entity == static_cast<int>(index);
    case 6: case 0x1E:   // nobody stands on this entity (a pressure plate that was released)
        return support_entity != static_cast<int>(index);
    case 2:   // no killable enemy is left in the room (doors that open once it is cleared)
        return !KillableEnemyExists();
    case 0x62:   // at least one killable enemy is there
        return KillableEnemyExists();
    case 3: case 4: case 5:   // the 6th / 7th / 8th entity is no longer an enemy (or flag 83 is set)
    {
        const std::size_t i = static_cast<std::size_t>(trigger) + 2;
        const bool hostile = i < entities.size() && entities[i].hostile && !entities[i].dying &&
                             !(game && game->IsCollected(room_number, static_cast<int>(i)));
        return !hostile || (game && game->IsFlagSet(0x0A * 8 + 3));
    }
    case 7:   // the player is on row 15
        return cursor_y == 15;
    case 0x0D:   // row 34, floor of height 2
        return cursor_y == 34 && FloorHeight(cursor_x, cursor_y) == 2;
    case 0x0E:   // on the platform at (28..30, 19..21), height 4, not falling
        return cursor_x >= 28 && cursor_x <= 30 && cursor_y >= 19 && cursor_y <= 21 && grounded && std::abs(player_z - 4.0) < 0.05;
    case 0x5F:   // flags 8 to 11 are all set
        return game && game->IsFlagSet(8) && game->IsFlagSet(9) && game->IsFlagSet(10) && game->IsFlagSet(11);
    default:
        return true;
    }
}

// Entities whose gravity was switched on drop until they rest on the floor or on another object.
void RoomView::UpdateFallingEntities(double dt)
{
    if (!entity_table) return;
    for (auto& e : entities)
    {
        if (!e.falls) continue;
        const double half = entity_table->Has(e.type) ? entity_table->GetHitboxBase(e.type) / 16.0 : 0.5;
        double floor_h = 0.0;
        const int x0 = static_cast<int>(std::floor(e.x - half + 0.01)), x1 = static_cast<int>(std::floor(e.x + half - 0.01));
        const int y0 = static_cast<int>(std::floor(e.y - half + 0.01)), y1 = static_cast<int>(std::floor(e.y + half - 0.01));
        for (int cx = x0; cx <= x1; ++cx)
            for (int cy = y0; cy <= y1; ++cy)
                if (IsWalkable(cx, cy)) floor_h = std::max(floor_h, static_cast<double>(FloorHeight(cx, cy)));
        if (e.z <= floor_h + 1e-6) { e.z = floor_h; e.fall_v = 0.0; continue; }
        e.fall_v = std::min(MAX_FALL_SPEED, e.fall_v + GRAVITY * dt);
        e.z = std::max(floor_h, e.z - e.fall_v * dt);
        if (e.z <= floor_h) { Sfx(SoundEngine::SFX_JUMP_LAND); e.fall_v = 0.0; }
    }
}

void RoomView::UpdateBehaviours(double dt)
{
    if (!behaviours || !entity_table || dead || !dialogue_pages.empty() || cutscene_busy > 0.0) return;
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        auto& e = entities[i];
        if (e.type == 0 || e.type >= 0xC0 || e.behaviour <= 0) continue;
        if (!e.bh_init)
        {
            // Enemies that wait for a story event (an ambush, a boss) sleep until their script
            // turns them hostile; the others are handled by the enemy AI.
            e.bh_init = true;
            if (e.hostile && behaviours->IsScripted(e.behaviour)) e.dormant = true;
            // A director whose scene was played (its flag is set) has nothing more to do.
            const int director_flag = behaviours->DirectorFlag(e.behaviour);
            if (!e.hostile && director_flag >= 0 && game && game->IsFlagSet(director_flag)) { e.bh_pc = -1; continue; }
        }
        if ((e.hostile && !e.dormant) || e.bh_pc < 0) continue;
        // Entities the table does not know (the invisible "directors" of cutscenes) still run.
        if (game && game->IsCollected(room_number, static_cast<int>(i))) continue;
        if (e.bh_speed <= 0) e.bh_speed = 1;
        e.bh_acc = std::min(e.bh_acc + dt * BEHAVIOUR_TICK_HZ, 8.0);
        bool moved = false;
        const double before_x = e.x, before_y = e.y, before_z = e.z;
        while (e.bh_acc >= 1.0 && e.bh_pc >= 0)
        {
            e.bh_acc -= 1.0;
            BehaviourTick(i, moved);
        }
        static const bool log_bh = std::getenv("LS_LOG_BH_MOVE") != nullptr;
        if (log_bh && moved)
            SDL_Log("BH entity %d type %d behaviour %d at (%.2f, %.2f) dir %d pc %d", static_cast<int>(i), e.type, e.behaviour, e.x, e.y, e.orientation, e.bh_pc);
        // Whoever stands on a moving platform (a raft) is carried along.
        if (support_entity == static_cast<int>(i) && (e.x != before_x || e.y != before_y || e.z != before_z))
        {
            player_x += e.x - before_x;
            player_y += e.y - before_y;
            player_z += e.z - before_z;
        }
        e.walking = moved;
        e.walk_time = moved ? e.walk_time + dt : 0.0;
        static const bool log_pc = std::getenv("LS_LOG_BH_PC") != nullptr;
        static double log_clock = 0.0;
        if (log_pc && i == 0) log_clock += dt;
        if (log_pc && log_clock > 2.0)
        {
            SDL_Log("BH pc entity %d type %d beh %d pc %d busy %d count %d pos (%.2f, %.2f) dir %d vis %d", static_cast<int>(i), e.type, e.behaviour, e.bh_pc, e.bh_busy, e.bh_count, e.x, e.y, e.orientation, e.visible);
            if (i + 1 >= entities.size()) log_clock = 0.0;
        }
    }
}

// ---- Status ailments ---------------------------------------------------------------------
void RoomView::InflictStatus(int status)
{
    if (!game || game->HasStatus(status) || dead) return;
    game->AddStatus(status);
    Sfx(0x7A);   // SND_Poison
    switch (status)
    {
    case GameState::STATUS_POISON:    poison_walk = 0.0; poison_ticks = 8; ShowMessage("You are poisoned!"); break;
    case GameState::STATUS_CONFUSION: ShowMessage("You are confused! The controls are reversed."); break;
    case GameState::STATUS_PARALYSIS: paralysis_time = 20.0; paralysis_presses = 0; ShowMessage("You are paralysed! Press the sword button to struggle."); break;
    case GameState::STATUS_CURSE:     ShowMessage("You are cursed! You cannot use your sword."); break;
    default: break;
    }
}

void RoomView::CureStatus(int status)
{
    if (!game) return;
    game->ClearStatus(status);
    if (status == GameState::STATUS_PARALYSIS) paralysis_time = 0.0;
    if (status == GameState::STATUS_POISON) poison_ticks = 0;
}

// Some enemies add an ailment to the damage they do (worms and mushrooms in the original).
void RoomView::TryStatusAttack(int enemy_type)
{
    if (!game || dead) return;
    if (std::rand() % 100 >= 40) return;
    switch (enemy_type)
    {
    case 8: case 55:   // second worm / second mushroom: poison
        InflictStatus(GameState::STATUS_POISON);
        break;
    case 9:            // third worm: poison, paralysis or confusion
        InflictStatus(std::rand() % 3 == 0 ? GameState::STATUS_POISON : (std::rand() & 1) ? GameState::STATUS_PARALYSIS : GameState::STATUS_CONFUSION);
        break;
    case 56:           // third mushroom: any of the four
        InflictStatus(std::rand() % 4);
        break;
    default: break;
    }
}

// Poison costs 1 HP every couple of seconds of walking, 8 times in all. Paralysis wears off
// by itself after 20 s, or sooner by struggling (see Attack()).
void RoomView::UpdateAilments(double dt)
{
    if (!game || dead) return;
    if (game->HasStatus(GameState::STATUS_POISON) && moving)
    {
        poison_walk += dt;
        if (poison_walk >= 2.1)
        {
            poison_walk = 0.0;
            if (game->Damage(1)) { Die(); return; }
            if (--poison_ticks <= 0) { CureStatus(GameState::STATUS_POISON); ShowMessage("The poison has worn off."); }
        }
    }
    if (game->HasStatus(GameState::STATUS_PARALYSIS))
    {
        paralysis_time -= dt;
        if (paralysis_time <= 0.0) { CureStatus(GameState::STATUS_PARALYSIS); ShowMessage("You can move again."); }
    }
}

// Cure items: Detox Grass / Detox Book (poison), Mind Repair (confusion), Anti-Paralyze,
// Anti-Curse. Returns whether the item did something (it is then used up by the caller).
bool RoomView::CureAilmentsWith(int item)
{
    if (!game) return false;
    int status = -1;
    switch (item)
    {
    case 18: case 33: status = GameState::STATUS_POISON; break;
    case 21: status = GameState::STATUS_CONFUSION; break;
    case 28: status = GameState::STATUS_PARALYSIS; break;
    case 34: status = GameState::STATUS_CURSE; break;
    default: return false;
    }
    if (!game->HasStatus(status)) return false;
    CureStatus(status);
    return true;
}

// ---- Screen effects and Friday -----------------------------------------------------------
double RoomView::StartFx(const std::string& name)
{
    auto to_level = [&](double target, double dur, SDL_Color color) {
        if (std::abs(fx_level - target) < 0.01) return 0.0;
        fx_mode = FxMode::TO_LEVEL;
        fx_from = fx_level;
        fx_to = target;
        fx_t = 0.0;
        fx_dur = dur;
        fx_color = color;
        return dur;
    };
    auto pulse = [&](double dur, SDL_Color color) {
        fx_mode = FxMode::PULSE;
        fx_t = 0.0;
        fx_dur = dur;
        fx_color = color;
        return dur;
    };
    const SDL_Color black{ 0, 0, 0, 255 }, white{ 255, 255, 255, 255 }, red{ 255, 30, 30, 255 };
    if (name == "fade_out") return to_level(1.0, 0.4, black);
    if (name == "fade_in") return to_level(0.0, 0.4, fx_color);
    if (name == "black_flash") return pulse(0.4, black);
    if (name == "white_flash") return pulse(0.4, white);
    if (name == "red_flash") return pulse(0.5, red);
    if (name == "warp_pad" || name == "tree_warp" || name == "nole_warp") return pulse(0.9, white);
    return 0.0;
}

void RoomView::UpdateFx(double dt)
{
    if (fx_mode == FxMode::NONE) return;
    fx_t += dt;
    const double k = fx_dur > 0.0 ? std::min(1.0, fx_t / fx_dur) : 1.0;
    if (fx_mode == FxMode::TO_LEVEL)
    {
        fx_level = fx_from + (fx_to - fx_from) * k;
        if (k >= 1.0) { fx_level = fx_to; fx_mode = FxMode::NONE; }
    }
    else
    {
        fx_level = k < 0.5 ? k * 2.0 : (1.0 - k) * 2.0;   // up, then down
        if (k >= 1.0) { fx_level = 0.0; fx_mode = FxMode::NONE; }
    }
}

void RoomView::DrawFriday(SDL_Renderer* renderer)
{
    if (friday_state == 0 || !entity_table) return;
    const double t = friday_t;
    const double cx = viewport.x + viewport.w * 0.5 + std::sin(t * 2.2) * 10.0 * scale;
    const double hover_y = viewport.y + viewport.h * 0.22 + std::sin(t * 3.1) * 3.0 * scale;
    double y = hover_y;
    if (friday_state == 1) y = hover_y + (1.0 - std::min(1.0, t / 0.5)) * viewport.h * 0.15;
    else if (friday_state == 3) y = hover_y + std::min(1.0, t / 0.7) * viewport.h * 0.35;
    bool flip = false;
    const auto kind = entity_table->GetKind(97, 2, flip);
    const auto& frames = entity_table->GetWalkFrames(97, kind);
    const EntityTable::Image* img = nullptr;
    EntityTable::Image frame;
    if (!frames.empty()) { frame = frames[static_cast<std::size_t>(t * 8.0) % frames.size()]; img = &frame; }
    else img = entity_table->GetImage(97, 2, flip);
    if (!img || !img->texture) return;
    const float ax = flip ? (img->width - img->anchor_x) : img->anchor_x;
    const SDL_FRect dst = { static_cast<float>(cx) - ax * scale, static_cast<float>(y) - img->anchor_y * scale,
                            img->width * scale, img->height * scale };
    SDL_RenderTextureRotated(renderer, img->texture, nullptr, &dst, 0.0, nullptr,
                             flip ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
}

// ---- Cutscenes ---------------------------------------------------------------------------
// A custom script action is a list of operations (game/Cutscenes.h). Texts open the dialogue
// box and the rest of the action goes on when it is dismissed; flags, sounds and tile swaps
// happen in between.
void RoomView::TriggerAction(int id)
{
    if (id == 0) { StartFaint(); return; }   // CSA_0000: the player faints
    if (!cutscenes || !game) return;
    // A cutscene started by a boss (or a dormant enemy) looks at it.
    camera_focus = -1;
    if (cutscene_source >= 0 && static_cast<std::size_t>(cutscene_source) < entities.size())
    {
        const auto& src = entities[cutscene_source];
        if (IsBossType(src.type) || src.dormant) camera_focus = cutscene_source;
    }
    const auto ops = cutscenes->Action(id);
    static const bool log_bh = std::getenv("LS_LOG_BH") != nullptr;
    if (log_bh) SDL_Log("Cutscene action %d: %d ops", id, static_cast<int>(ops.size()));
    if (ops.empty()) { camera_focus = -1; return; }
    for (const auto& op : ops) cutscene_ops.push_back(op);
    cutscene_answer_valid = false;
    if (dialogue_pages.empty()) ContinueCutscene();
}

void RoomView::StartFaint()
{
    if (fainted || dead) return;
    fainted = true;
    faint_t = 0.0;
}

// 150 frames of the original's faint animation, then the story goes on where the faint was
// the first one of the adventure (room 426, flag 4 not set yet): the mayor's house in Massan.
void RoomView::UpdateFaint(double dt)
{
    if (!fainted) return;
    faint_t += dt;
    if (faint_t < FAINT_TIME) return;
    fainted = false;
    if (room_number == 426 && game && !game->IsFlagSet(4) && RoomExists(593))
    {
        faint_warp = true;
        pending_warp = { 593, 16, 15, 2 };
        cutscene_busy = StartFx("fade_out");
        if (cutscene_busy > 0.0) busy_then_warp = true;
        else warp_pending = true;
    }
}

void RoomView::ApplyPendingWarp()
{
    warp_pending = false;
    PendingWarp w = pending_warp;
    if (w.room < 0) w.room = room_number;   // the same room again, the player moved
    if (std::getenv("LS_LOG_BH")) SDL_Log("Cutscene warp to room %d (%d, %d) dir %d", w.room, w.x, w.y, w.dir);
    if (!RoomExists(w.room)) { ContinueCutscene(); return; }
    if (!faint_warp) player_hidden = false;
    auto saved = std::move(cutscene_ops);   // LoadRoom ends a cutscene: this one goes on
    const SDL_Point arrival{ w.x, w.y };
    try
    {
        LoadRoom(w.room, &arrival);
    }
    catch (const std::exception& e)
    {
        SDL_Log("Warp to room %d failed: %s", w.room, e.what());
    }
    cutscene_ops = std::move(saved);
    int px = w.x, py = w.y;
    if (w.climb_kind)
    {
        // The ladder / elevator of the room above: the nearest cell of the same kind.
        double best = 1e18;
        for (int cy = CELL_OFFSET; cy < CELL_OFFSET + hm_height; ++cy)
            for (int cx = CELL_OFFSET; cx < CELL_OFFSET + hm_width; ++cx)
            {
                const int t = GetCell(cx, cy) & 0x3F;
                const bool match = w.climb_kind == 1 ? (t == 0x0B || t == 0x0C) : t == 0x0F;
                const double d = (cx - w.x) * (cx - w.x) + (cy - w.y) * (cy - w.y);
                if (match && IsWalkable(cx, cy) && d < best) { best = d; px = cx; py = cy; }
            }
    }
    if (w.fall)
    {
        // Falling keeps the place; if the room below has no floor there, land on the nearest floor.
        const bool on_object = PlatformTop(px + 0.5, py + 0.5, 99.0) >= 0.0;   // e.g. a raft on a river
        for (int r = 0; r < 40 && !on_object && !IsWalkable(px, py); ++r)
        {
            bool found = false;
            for (int dx = -r; dx <= r && !found; ++dx)
                for (int dy = -r; dy <= r && !found; ++dy)
                    if (std::max(std::abs(dx), std::abs(dy)) == r && IsWalkable(w.x + dx, w.y + dy)) { px = w.x + dx; py = w.y + dy; found = true; }
            if (found) break;
        }
    }
    SetPlayerPosition(px + 0.5, py + 0.5);
    if (faint_warp)
    {
        faint_warp = false;
        player_hidden = true;        // he is in bed, under the blanket
        input_hold = 254.0 / 60.0;   // PlaybackInput(9): the player lies in bed for a moment
        friday_state = 2;            // Friday hovers beside him
        friday_t = 0.0;
    }
    static const int to_facing[4] = { 1, 2, 3, 0 };   // entity direction -> player facing
    if (w.dir >= 0) facing = to_facing[w.dir & 3];
    fx_level = std::max(fx_level, 1.0);   // still black: the new room fades in
    cutscene_busy = StartFx("fade_in");
    if (cutscene_busy <= 0.0) ContinueCutscene();
}

// New game: the intro is a chain of scripted rooms (139...), driven by the "director"
// entities of each room, ending in room 148 where the adventure begins.
void RoomView::BeginNewGame()
{
    intro_active = true;
    input_locked = true;
    fx_level = 0.0; fx_mode = FxMode::NONE; cutscene_busy = 0.0; busy_then_warp = false; friday_state = 0;
    fainted = false; player_hidden = false; input_hold = 0.0; faint_warp = false;
    if (game) game->Reset();
    church_room = -1;
    dead = false;
    game_over_shown = false;
    poison_ticks = 0;
    paralysis_time = 0.0;
    projectiles.clear();
    const SDL_Point arrival{ 62, 24 };
    LoadRoom(139, &arrival);
    SetPlayerPosition(62.5, 24.5);
    Sfx(0x2B);   // the intro's music (Labyrinth)
}

void RoomView::SkipIntro()
{
    intro_active = false;
    input_locked = false;
    fx_level = 0.0; fx_mode = FxMode::NONE; cutscene_busy = 0.0; busy_then_warp = false; friday_state = 0;
    fainted = false; player_hidden = false; input_hold = 0.0; faint_warp = false;
    playback = nullptr;
    cutscene_ops.clear();
    dialogue_pages.clear();
    dialogue_asks = false;
    choice_handler = nullptr;
    page_wait = 0.0;
    if (dialogue_cb) dialogue_cb("", false);
    playing_bgm = -1;
    const SDL_Point arrival{ 32, 38 };
    LoadRoom(148, &arrival);
    SetPlayerPosition(32.5, 38.5);
    facing = 3;
}

void RoomView::StartPlayback(int sequence)
{
    playback = behaviours ? behaviours->PlaybackSequence(sequence) : nullptr;
    playback_index = 0;
    playback_timer = playback && !playback->empty() ? (*playback)[0].second / 60.0 : 0.0;
}

void RoomView::DoTileSwapTrigger(int trigger)
{
    if (!game) return;
    for (const auto& sf : swap_flags)
    {
        if (sf.trigger != trigger || game->IsFlagSet(sf.flag)) continue;
        SetStoryFlag(sf.flag);
        ApplyFlagRules();
        break;
    }
}

void RoomView::ContinueCutscene()
{
    if (cutscene_ops.empty()) camera_focus = -1;
    while (!cutscene_ops.empty())
    {
        const auto op = cutscene_ops.front();
        cutscene_ops.pop_front();
        switch (op.kind)
        {
        case Cutscenes::Op::FLAG:
            if (game)
            {
                if (op.set) SetStoryFlag(op.value);
                else game->ClearFlag(op.value);
                ApplyFlagRules();
            }
            break;
        case Cutscenes::Op::SOUND:
            Sfx(op.value);
            break;
        case Cutscenes::Op::SWAP:
            DoTileSwapTrigger(op.value);
            break;
        case Cutscenes::Op::TEXT:
        {
            dialogue_pages.clear();
            for (const auto& text : cutscenes->Text(op.value))
                dialogue_pages.push_back({ text, -1 });
            if (dialogue_pages.empty()) break;
            page_wait = op.wait / 60.0;
            dialogue_asks = false;
            dialogue_yes = dialogue_no = nullptr;
            choice_handler = nullptr;
            // A text followed by a branch on the answer ends with a yes/no question.
            if (!cutscene_ops.empty() && cutscene_ops.front().kind == Cutscenes::Op::IF &&
                cutscene_ops.front().on_answer && !cutscene_answer_valid)
            {
                const auto branch = cutscene_ops.front();
                cutscene_ops.pop_front();
                dialogue_asks = true;
                choice_handler = [this, branch](bool yes) {
                    cutscene_answer_valid = true;
                    cutscene_answer = yes;
                    const auto& list = yes ? branch.then_ops : branch.else_ops;
                    if (list)
                        cutscene_ops.insert(cutscene_ops.begin(), list->begin(), list->end());
                    ContinueCutscene();
                };
            }
            ShowDialoguePage();
            return;   // resumed when the last box is dismissed (or the question answered)
        }
        case Cutscenes::Op::HOSTILE:
        case Cutscenes::Op::VISIBLE:
        case Cutscenes::Op::HIDE:
        {
            const int idx = op.value < 0 ? cutscene_source : op.value;
            if (idx < 0 || static_cast<std::size_t>(idx) >= entities.size()) break;
            auto& t = entities[idx];
            if (op.kind == Cutscenes::Op::HOSTILE)
            {
                t.hostile = op.set;
                if (op.set) t.dormant = false;
            }
            else if (op.kind == Cutscenes::Op::VISIBLE)
            {
                const bool was_visible = t.visible;
                t.visible = t.base_visible = op.set;
                if (op.set && !was_visible && !intro_active)
                {
                    // Appears with a rumble, sparks and a fade-in.
                    t.appear_t = 0.0;
                    StartShake(0.7, 2.5);
                    Sfx(0x59);   // SND_Rumble
                    SpawnSparks(t.x, t.y, t.z + 1.0, true, false);
                }
            }
            else
            {
                t.visible = t.base_visible = false;
                if (game) game->MarkCollected(room_number, idx);   // gone for good
            }
            break;
        }
        case Cutscenes::Op::BLOCK:
            for (const auto& [x, y] : op.cells)
            {
                const int hx = x - CELL_OFFSET, hy = y - CELL_OFFSET;
                if (hx < 0 || hy < 0 || hx >= hm_width || hy >= hm_height) continue;
                heightmap[hy * hm_width + hx] |= 0x4000;
                if (base_heightmap.size() == heightmap.size()) base_heightmap[hy * hm_width + hx] |= 0x4000;
            }
            break;
        case Cutscenes::Op::LOCK:
            input_locked = op.set;
            if (!op.set)
            {
                intro_active = false;
                input_hold = 0.0;
                player_hidden = false;
                friday_state = 0;
                PlayRoomMusic();
            }
            break;
        case Cutscenes::Op::WARP:
            // Fade out first; the warp itself is done at the end of a frame (entities are
            // being iterated right now) and the new room fades in.
            pending_warp = { op.value, op.x, op.y, op.dir };
            cutscene_busy = StartFx("fade_out");
            if (cutscene_busy > 0.0) busy_then_warp = true;
            else warp_pending = true;
            return;
        case Cutscenes::Op::FX:
        {
            const double dur = StartFx(op.name);
            if (dur > 0.0) { cutscene_busy = dur; return; }
            break;
        }
        case Cutscenes::Op::CREDITS:
            cutscene_ops.clear();
            if (credits_cb) credits_cb();
            return;
        case Cutscenes::Op::WAIT:
            if (op.value > 0) { cutscene_busy = op.value / 60.0; return; }
            break;
        case Cutscenes::Op::FRIDAY:
            if (op.name == "appear") { friday_state = 1; friday_t = 0.0; }
            else if (op.name == "hover") { if (friday_state == 0) { friday_state = 2; friday_t = 0.0; } }
            else if (op.name == "leave") { if (friday_state != 0) { friday_state = 3; friday_t = 0.0; } }
            else friday_state = 0;
            break;
        case Cutscenes::Op::IF:
        {
            // A branch on a flag, or on an answer nobody asked for (taken as "no").
            bool yes = false;
            if (op.on_answer)
            {
                yes = cutscene_answer_valid && cutscene_answer;
                cutscene_answer_valid = false;
            }
            else
            {
                yes = game && game->IsFlagSet(op.value);
            }
            const auto& list = yes ? op.then_ops : op.else_ops;
            if (list) cutscene_ops.insert(cutscene_ops.begin(), list->begin(), list->end());
            break;
        }
        }
    }
}

// Timers, enemy movement, and contact damage from enemies touching the player.
void RoomView::UpdateCombat(double dt)
{
    UpdateChestOpening(dt);
    for (auto& sp : sparks) sp.t += dt;
    sparks.erase(std::remove_if(sparks.begin(), sparks.end(),
                                [](const Spark& sp) { return sp.t >= SPARK_TIME; }), sparks.end());
    UpdateAilments(dt);
    UpdateBehaviours(dt);
    UpdateFallingEntities(dt);
    UpdateEnemies(dt);
    UpdateProjectiles(dt);
    attack_cooldown = std::max(0.0, attack_cooldown - dt);
    swing_time = std::max(0.0, swing_time - dt);
    if (swinging)
    {
        swing_anim += dt;
        if (swing_hit_pending && swing_anim >= SWING_HIT_TIME) ApplySwingHit();
        if (swing_anim >= SWING_DURATION) swinging = false;
    }
    for (auto& e : entities) e.hit_flash = std::max(0.0, e.hit_flash - dt);
    invulnerable_time = std::max(0.0, invulnerable_time - dt);
    hurt_time = std::max(0.0, hurt_time - dt);
    if (dead)
    {
        death_time += dt;
        if (!game_over_shown && death_time > 1.8 && game_over_cb)
        {
            game_over_shown = true;
            game_over_cb();
        }
    }
    if (dead || invulnerable_time > 0.0 || !game || !entity_table) return;

    const double margin = 0.1;   // enemies are solid: touching means being a hair away
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        // A boulder rolling along its script crushes whoever it meets (it is not an enemy to fight).
        const bool rolling = entities[i].type == 20 && entities[i].visible && entities[i].walking;
        if (!IsAliveEnemy(i) && !rolling) continue;
        const auto& e = entities[i];
        const auto* st = entity_table->GetEnemy(e.type);
        const double half = entity_table->GetHitboxBase(e.type) / 16.0;
        const double top = e.z + entity_table->GetHitboxHeight(e.type) / 16.0;
        if (std::abs(player_x - e.x) < half + PLAYER_RADIUS + margin &&
            std::abs(player_y - e.y) < half + PLAYER_RADIUS + margin &&
            player_z < top && player_z + PLAYER_HEIGHT > e.z)
        {
            HurtPlayer(st->attack, e.x, e.y);
            TryStatusAttack(e.type);
            return;
        }
    }
}

void RoomView::ShowMessage(const std::string& text)
{
    message_time = 4.0;
    if (message_cb) message_cb(text);
}

// A dead enemy leaves an item box with its item (one chance in N, see Drops) or else a
// money bag with its gold, where it stood.
void RoomView::SpawnDrop(double x, double y, double z, int enemy_type, int gold)
{
    int item = -1;
    if (drops)
        if (const auto* d = drops->ForEnemy(enemy_type))
            if (d->one_in <= 1 || std::rand() % d->one_in == 0) item = d->item;
    if (item < 0 && gold <= 0) return;
    RoomEntity e{};
    e.type = item >= 0 ? 96 : 10;   // item box / money bag
    e.x = x;
    e.y = y;
    e.z = z;
    e.centred = true;
    e.orientation = 2;
    e.solid = false;
    e.hostile = false;
    e.visible = true;
    e.base_visible = true;
    e.speed = 0;
    e.gravity = true;
    e.pickup_gold = item >= 0 ? 0 : gold;
    e.pickup_item = item;
    entities.push_back(e);
}

// Entity types 0xC0 and above are items lying on the floor (type - 0xC0 = item index).
// Touching one puts it in the inventory.
void RoomView::CheckPickups()
{
    if (!game || !entity_table) return;
    // Drops: touching one takes it; they vanish after a while.
    for (auto& e : entities)
    {
        if (e.pickup_gold <= 0 && e.pickup_item < 0) continue;
        if (!e.visible) continue;
        e.pickup_age += frame_dt;
        if (e.pickup_age > 25.0) { e.visible = false; continue; }
        if (std::abs(player_x - e.x) < 0.6 + PLAYER_RADIUS && std::abs(player_y - e.y) < 0.6 + PLAYER_RADIUS &&
            player_z < e.z + 1.5 && player_z + PLAYER_HEIGHT > e.z)
        {
            e.visible = false;
            if (e.pickup_gold > 0)
            {
                game->AddGold(e.pickup_gold);
                Sfx(SoundEngine::SFX_PICKUP_MONEY);
                ShowMessage("Got " + std::to_string(e.pickup_gold) + " gold");
            }
            else
            {
                game->AddItem(e.pickup_item);
                Jingle(SoundEngine::MUSIC_FANFARE, 2.6);
                ShowMessage("Got " + game->ItemName(e.pickup_item));
            }
        }
    }
    if (shop_info && shop_info->kind == "shop") return;   // goods are bought, not picked up
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        const auto& e = entities[i];
        if (e.type < 0xC0 || !e.visible || !entity_table->Has(e.type)) continue;
        if (game->IsCollected(room_number, static_cast<int>(i))) continue;
        const double half = std::max(entity_table->GetHitboxBase(e.type) / 16.0, 0.3);
        const double top = e.z + std::max(entity_table->GetHitboxHeight(e.type), 16) / 16.0;
        if (std::abs(player_x - e.x) < half + PLAYER_RADIUS &&
            std::abs(player_y - e.y) < half + PLAYER_RADIUS &&
            player_z < top && player_z + PLAYER_HEIGHT > e.z)
        {
            const int item = e.type - 0xC0;
            game->MarkCollected(room_number, static_cast<int>(i));
            OnEntityGone(i);
            game->AddItem(item);
            Jingle(SoundEngine::MUSIC_FANFARE, 2.6);
            std::string text = "Got " + game->ItemName(item);
            if (game->ItemCount(item) > 0) text += " (x" + std::to_string(game->ItemCount(item)) + ")";
            else if (item == GameState::ITEM_LIFE_STOCK) text += ": max health up";
            ShowMessage(text);
        }
    }
}

void RoomView::GiveChestContent(int value)
{
    // Chest contents 58..61 are not items: they stand for 5, 20, 50 and 200 gold, and 63 for
    // nothing (GoldAmounts and CheckOpenChest in the original).
    if (value >= 58 && value <= 61)
    {
        static const int amounts[4] = { 5, 20, 50, 200 };
        const int gold = amounts[value - 58];
        Sfx(SoundEngine::SFX_PICKUP_MONEY);
        game->AddGold(gold);
        ShowMessage("Found " + std::to_string(gold) + " gold");
    }
    else if (value == 63)
    {
        ShowMessage("The chest is empty.");
    }
    else if (value >= 0x40)
    {
        Sfx(SoundEngine::SFX_PICKUP_MONEY);
        game->AddGold(value - 0x40);
        ShowMessage("Found " + std::to_string(value - 0x40) + " gold");
    }
    else
    {
        Jingle(SoundEngine::MUSIC_CHEST_OPEN, 2.6);
        game->AddItem(value);
        std::string text = "Opened the chest: " + game->ItemName(value);
        if (value == GameState::ITEM_LIFE_STOCK) text += " (max health up)";
        ShowMessage(text);
    }
}

// Opens the chest right in front of the player (entity 18 is the chest). Chest
// contents come from the room's chest list, in the order the chests appear.
// Entity visibility from the story flags:
//   hide       the entity is hidden while the flag is in the given state
//   one-time   the entity is shown while show flag == show state and hide flag != hide state;
//              several rows for one entity are alternatives (any of them shows it)
//   hide_from  every entity from `entity` on is hidden while the flag is set
// Rebuilds bg / fg / heightmap from the exported room plus every swap whose flag is set
// (same copy rules as the editor's TileSwap::DrawSwap / DrawHeightmapSwap).
void RoomView::ApplyTileSwaps()
{
    if (base_bg.size() != bg.size() || base_fg.size() != fg.size() || base_heightmap.size() != heightmap.size()) return;
    bg = base_bg;
    fg = base_fg;
    heightmap = base_heightmap;
    if (!game) return;

    for (const auto& sf : swap_flags)
    {
        if (!sf.always && !game->IsFlagSet(sf.flag)) continue;
        const TileSwap* sw = nullptr;
        for (const auto& t : tile_swaps)
            if (t.trigger == sf.trigger) { sw = &t; break; }
        if (!sw) continue;

        const int dst_x = sw->map[2], dst_y = sw->map[3], src_x = sw->map[0], src_y = sw->map[1];
        const int mw = sw->map[4], mh = sw->map[5];
        for (int layer = 0; layer < 2; ++layer)   // 0 = background, 1 = foreground
        {
            const std::vector<int> before = (layer == 0) ? bg : fg;
            auto& dst = (layer == 0) ? bg : fg;
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    const int xo = x - dst_x + hm_left, yo = y - dst_y + hm_top;
                    bool in = false;
                    switch (sw->mode)
                    {
                    case 0:
                        in = xo >= 0 && xo < mw && yo >= 0 && yo < mh;
                        break;
                    case 1:   // wall NE
                        in = (layer == 0)
                            ? (xo - yo >= 0 && xo - yo < mw && yo >= 0 && yo < mh)
                            : (xo - yo - 1 >= 0 && xo - yo - 1 < mw && yo >= 0 && yo < mh);
                        break;
                    case 2:   // wall NW
                        in = (layer == 0)
                            ? (xo >= 0 && xo < mh && yo > xo && yo <= xo + mw)
                            : (xo >= 0 && xo < mh && yo >= xo && yo < xo + mw);
                        break;
                    }
                    if (!in) continue;
                    const int sx = src_x - dst_x + x, sy = src_y - dst_y + y;
                    dst[y * width + x] = (sx >= 0 && sx < width && sy >= 0 && sy < height) ? before[sy * width + sx] : 0;
                }
            }
        }

        const std::vector<int> hm_before = heightmap;
        for (int y = 0; y < sw->hm[5]; ++y)
        {
            for (int x = 0; x < sw->hm[4]; ++x)
            {
                const int sx = x + sw->hm[0], sy = y + sw->hm[1];
                const int dx = x + sw->hm[2], dy = y + sw->hm[3];
                if (sx < 0 || sy < 0 || sx >= hm_width || sy >= hm_height) continue;
                if (dx < 0 || dy < 0 || dx >= hm_width || dy >= hm_height) continue;
                heightmap[dy * hm_width + dx] = hm_before[sy * hm_width + sx];
            }
        }
    }
}

void RoomView::ApplyFlagRules()
{
    ApplyTileSwaps();
    if (!game) return;
    const std::size_t n = entities.size();
    std::vector<char> has_onetime(n, 0), onetime_shows(n, 0), hidden(n, 0);
    for (const auto& r : flag_rules)
    {
        const std::size_t i = static_cast<std::size_t>(r.entity);
        switch (r.kind)
        {
        case FlagRule::HIDE:
            if (i < n && game->IsFlagSet(r.flag) == r.when_set) hidden[i] = 1;
            break;
        case FlagRule::ONE_TIME:
            if (i < n)
            {
                has_onetime[i] = 1;
                const bool show = game->IsFlagSet(r.flag) == r.when_set;
                const bool hide = game->IsFlagSet(r.hide_flag) == r.hide_set;
                if (show && !hide) onetime_shows[i] = 1;
            }
            break;
        case FlagRule::HIDE_FROM:
            if (game->IsFlagSet(r.flag))
                for (std::size_t k = i; k < n; ++k) hidden[k] = 1;
            break;
        }
    }
    for (std::size_t i = 0; i < n; ++i)
    {
        bool v = has_onetime[i] ? (onetime_shows[i] != 0) : entities[i].base_visible;
        if (hidden[i]) v = false;
        entities[i].visible = v;
    }
}

// Talks to the visible character right in front of the player (same probe as chests).
bool RoomView::StartDialogue()
{
    if (!dialogue_data || !entity_table) return false;
    static const double dir_x[4] = { -1, 0, 1, 0 };   // facing: NW NE SE SW
    static const double dir_y[4] = { 0, -1, 0, 1 };
    const double reach = PLAYER_RADIUS + 0.4;
    const double qx = player_x + dir_x[facing] * reach, qy = player_y + dir_y[facing] * reach;
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        const auto& e = entities[i];
        if (!e.talks || !e.visible || e.type == 0 || !entity_table->Has(e.type)) continue;
        if (game && game->IsCollected(room_number, static_cast<int>(i))) continue;
        const double half = entity_table->GetHitboxBase(e.type) / 16.0 + 0.3;
        if (std::abs(qx - e.x) > half || std::abs(qy - e.y) > half) continue;
        const double top = e.z + entity_table->GetHitboxHeight(e.type) / 16.0;
        if (player_z >= top || player_z + PLAYER_HEIGHT <= e.z) continue;
        if (shop_info && shop_info->kind == "inn" && game) return OfferInn();
        if (shop_info && shop_info->kind == "church" && game) return OfferChurch();
        if (!game || e.dialogue < 0 || e.dialogue >= static_cast<int>(room_characters.size())) continue;
        auto conv = dialogue_data->Begin(room_characters[e.dialogue], *game);
        if (conv.pages.empty()) { for (int item : conv.give_now) GiveDialogueItem(item); continue; }
        for (int item : conv.give_now) GiveDialogueItem(item);
        dialogue_pages = std::move(conv.pages);
        dialogue_yes = conv.yes;
        dialogue_no = conv.no;
        dialogue_asks = conv.asks;
        ShowDialoguePage();
        ApplyFlagRules();   // talking may have set a flag
        return true;
    }
    return false;
}

// ---- Shops and inns ----------------------------------------------------------------------
void RoomView::ShowText(const std::string& text)
{
    dialogue_pages.clear();
    dialogue_pages.push_back({ text, -1 });
    dialogue_yes = dialogue_no = nullptr;
    dialogue_asks = false;
    ShowDialoguePage();
}

void RoomView::AskChoice(const std::string& text, std::function<void(bool)> handler)
{
    dialogue_pages.clear();
    dialogue_pages.push_back({ text, -1 });
    dialogue_yes = dialogue_no = nullptr;
    dialogue_asks = true;
    choice_handler = std::move(handler);
    ShowDialoguePage();
}

// In a shop room: asks to buy the item in front of the player. The price is the item's
// base price times the shop's markup / 16.
bool RoomView::TryShop()
{
    if (!shops || !shop_info || shop_info->kind != "shop" || !game || !entity_table) return false;
    static const double dir_x[4] = { -1, 0, 1, 0 };
    static const double dir_y[4] = { 0, -1, 0, 1 };
    const double qx = player_x + dir_x[facing] * (PLAYER_RADIUS + 0.4);
    const double qy = player_y + dir_y[facing] * (PLAYER_RADIUS + 0.4);
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        const auto& e = entities[i];
        if (e.type < 0xC0 || !e.visible || game->IsCollected(room_number, static_cast<int>(i))) continue;
        const double half = std::max(entity_table->GetHitboxBase(e.type) / 16.0, 0.5) + 0.35;
        if (std::abs(qx - e.x) > half || std::abs(qy - e.y) > half) continue;
        if (std::abs(player_z - e.z) > 2.0) continue;

        const int item = e.type - 0xC0;
        const int price = shops->ItemPrice(*shop_info, item);
        if (price < 0) continue;
        const std::string name = game->ItemName(item);
        const std::size_t index = i;
        AskChoice(name + ": " + std::to_string(price) + " golds.\nDo you want to buy it?",
            [this, item, price, index](bool yes) {
                if (!yes) { ShowText("Are you just browsing?"); return; }
                if (!game->SpendGold(price))
                {
                    Sfx(SoundEngine::SFX_ERROR);
                    ShowText("You can't afford that now.\nMake more golds somewhere!");
                    return;
                }
                game->AddItem(item);
                Sfx(SoundEngine::SFX_PICKUP_MONEY);
                game->MarkCollected(room_number, static_cast<int>(index));
                OnEntityGone(index);
                ShowText("Thank you!\nDon't waste it.");
            });
        return true;
    }
    return false;
}

// Talking to the innkeeper: one night restores all the player's health.
bool RoomView::OfferInn()
{
    if (!shops || !shop_info || shop_info->kind != "inn" || !game) return false;
    const int price = shops->InnPrice(*shop_info);
    AskChoice("Would you like to stay the night?\n" + std::to_string(price) + " golds.",
        [this, price](bool yes) {
            if (!yes) { ShowText("Take it easy!"); return; }
            if (!game->SpendGold(price))
            {
                Sfx(SoundEngine::SFX_ERROR);
                ShowText("You can't afford that now.\nMake more golds somewhere!");
                return;
            }
            game->HealFull();
            Jingle(SoundEngine::MUSIC_INN_REST, 4.5);
            ShowText("Good morning!\nYou feel fully rested.");
        });
    return true;
}

// The priest records the adventure (the only service this port offers: there are no
// status ailments yet). The game is saved to the save file and the player will wake up here
// after dying.
bool RoomView::OfferChurch()
{
    if (!shop_info || shop_info->kind != "church" || !game) return false;
    if (game->HasAnyStatus())
    {
        int count = 0;
        for (int st = 0; st < 4; ++st) count += game->HasStatus(st) ? 1 : 0;
        const int price = shops ? shops->InnPrice(*shop_info) * count : 20 * count;
        AskChoice("My child, you are afflicted.\nA prayer costs " + std::to_string(price) + " golds.\nShall I pray for you?",
            [this, price](bool yes) {
                if (!yes) { ShowText("May God be with you."); return; }
                if (!game->SpendGold(price))
                {
                    Sfx(SoundEngine::SFX_ERROR);
                    ShowText("You can't afford that now.\nCome back when you can.");
                    return;
                }
                for (int st = 0; st < 4; ++st) CureStatus(st);
                Sfx(SoundEngine::SFX_HEALTH);
                ShowText("You are cured.\nGod bless you!");
            });
        return true;
    }
    AskChoice("Welcome, my child.\nShall I record your adventure?",
        [this](bool yes) {
            if (!yes) { ShowText("May God be with you."); return; }
            church_room = room_number;
            church_x = player_x;
            church_y = player_y;
            const bool ok = SaveGame();
            if (ok) Jingle(SoundEngine::MUSIC_SAVE_GAME, 3.5);
            game->HealFull();
            ShowText(ok ? "Your adventure is recorded.\nGod bless you!"
                        : "Something went wrong...\nYour adventure could not be recorded.");
        });
    return true;
}

// ---- Item menu -------------------------------------------------------------------------
void RoomView::OpenMenu()
{
    if (!game) return;
    menu_items.clear();
    for (const auto& [item, count] : game->GetItems())
        if (count > 0) menu_items.push_back(item);
    menu_index = 0;
    menu_open = true;
    Sfx(SoundEngine::SFX_MENU_OPEN);
    RefreshMenu();
}

void RoomView::CloseMenu()
{
    menu_open = false;
    Sfx(SoundEngine::SFX_CURSOR_SELECT);
    if (menu_cb) menu_cb("", false);
}

void RoomView::RefreshMenu()
{
    if (!menu_cb || !game) return;
    std::string text = "ITEMS     HP " + std::to_string(game->GetHealth()) + "/" + std::to_string(game->GetMaxHealth()) +
                       "     Gold " + std::to_string(game->GetGold()) + "\n\n";
    if (menu_items.empty())
    {
        text += "  (nothing)\n";
    }
    else
    {
        const int visible_rows = 8;
        const int total = static_cast<int>(menu_items.size());
        int first = std::clamp(menu_index - visible_rows / 2, 0, std::max(0, total - visible_rows));
        for (int row = first; row < std::min(total, first + visible_rows); ++row)
        {
            const int item = menu_items[row];
            text += (row == menu_index) ? "> " : "  ";
            text += game->ItemName(item);
            if (game->ItemCount(item) > 1) text += " x" + std::to_string(game->ItemCount(item));
            if (item == game->EquippedSword() || item == game->EquippedArmour() || item == game->EquippedBoots()) text += "  (on)";
            text += "\n";
        }
        if (total > visible_rows) text += "  ... " + std::to_string(total) + " items\n";
    }
    text += "\nShift: use   Tab: close";
    menu_cb(text, true);
}

// Effects: EkeEke heals, Key opens the door you stand in front of, swords and breasts are
// worn (the other items have no effect yet).
void RoomView::UseMenuItem()
{
    if (!game || menu_items.empty() || menu_index >= static_cast<int>(menu_items.size())) return;
    const int item = menu_items[menu_index];
    Sfx(SoundEngine::SFX_CURSOR_SELECT);
    if (item == GameState::ITEM_EKEEKE)
    {
        if (game->UseEkeEke()) { Sfx(SoundEngine::SFX_HEALTH); ShowMessage("Used an EkeEke"); }
        else { Sfx(SoundEngine::SFX_ERROR); ShowMessage("Health is already full"); }
    }
    else if (item == GameState::ITEM_KEY)
    {
        CloseMenu();
        UseKey();
        return;
    }
    else if (item == 18 || item == 21 || item == 28 || item == 33 || item == 34)
    {
        if (CureAilmentsWith(item))
        {
            game->UseItem(item);
            Sfx(SoundEngine::SFX_HEALTH);
            ShowMessage("You feel better.");
        }
        else
        {
            Sfx(SoundEngine::SFX_ERROR);
            ShowMessage("It has no effect.");
        }
    }
    else if (GameState::IsSword(item) || GameState::IsArmour(item) || GameState::IsBoots(item))
    {
        const bool on = game->ToggleEquip(item);
        ShowMessage(std::string(on ? "Equipped " : "Removed ") + game->ItemName(item));
    }
    else
    {
        Sfx(SoundEngine::SFX_ERROR);
        ShowMessage("Nothing happens.");
    }
    // The list may have changed (an item used up).
    menu_items.clear();
    for (const auto& [it, count] : game->GetItems())
        if (count > 0) menu_items.push_back(it);
    menu_index = std::clamp(menu_index, 0, std::max(0, static_cast<int>(menu_items.size()) - 1));
    RefreshMenu();
}

// Using a Key on a locked door. As in the original the player stands on the door's floor
// cell: type 26 + n opens the locked-door tile swap n; types 38 / 39 (side doors) open the
// nearest door entity (it disappears).
void RoomView::UseKey()
{
    if (!game || dead || !dialogue_pages.empty()) return;
    const int type = GetCell(cursor_x, cursor_y) & 0xFF;
    const bool tile_door = type >= 26 && type < 26 + 15 && type != 38 && type != 39;
    const bool side_door = (type == 38 || type == 39);
    if (!tile_door && !side_door)
    {
        Sfx(SoundEngine::SFX_ERROR);
        ShowMessage("Nothing to unlock here.");
        return;
    }
    if (game->ItemCount(GameState::ITEM_KEY) <= 0)
    {
        Sfx(SoundEngine::SFX_ERROR);
        ShowMessage("The door is locked. You need a Key.");
        return;
    }

    int flag = -1;
    if (tile_door)
    {
        const int trigger = type - 26;
        for (const auto& sf : swap_flags)
            if (sf.locked && sf.trigger == trigger) { flag = sf.flag; break; }
    }
    else
    {
        // The door entity closest to the player whose flag is not set yet.
        double best = 1e9;
        for (const auto& r : flag_rules)
        {
            if (r.kind != FlagRule::HIDE || !r.when_set || static_cast<std::size_t>(r.entity) >= entities.size()) continue;
            if (game->IsFlagSet(r.flag)) continue;
            // Only the locked-door rules count (they are appended after the others).
            const auto& e = entities[r.entity];
            const double d = std::abs(e.x - player_x) + std::abs(e.y - player_y);
            if (d < best) { best = d; flag = r.flag; }
        }
        if (best > 4.0) flag = -1;
    }

    if (flag < 0 || game->IsFlagSet(flag))
    {
        Sfx(SoundEngine::SFX_ERROR);
        ShowMessage(flag < 0 ? "The Key does not fit here." : "This door is already open.");
        return;
    }
    game->UseItem(GameState::ITEM_KEY);
    Sfx(SoundEngine::SFX_DOOR_LOCK);
    SetStoryFlag(flag);
    ApplyFlagRules();
    ShowMessage("The door is unlocked.");
}

// Permanent switches: stepping on the switch entity sets its flag for good.
void RoomView::CheckSwitches()
{
    if (!game || !entity_table) return;
    for (const auto& sw : switches)
    {
        if (static_cast<std::size_t>(sw.entity) >= entities.size() || game->IsFlagSet(sw.flag)) continue;
        const auto& e = entities[sw.entity];
        if (!entity_table->Has(e.type)) continue;
        const double half = entity_table->GetHitboxBase(e.type) / 16.0 + 0.15;
        const double top = e.z + std::max(entity_table->GetHitboxHeight(e.type), 16) / 16.0;
        if (std::abs(player_x - e.x) < half + PLAYER_RADIUS && std::abs(player_y - e.y) < half + PLAYER_RADIUS &&
            player_z < top && player_z + PLAYER_HEIGHT > e.z - 0.5)
        {
            Sfx(0x74);   // SND_PermanentSwitch
            SetStoryFlag(sw.flag);
            ApplyFlagRules();
            ShowMessage("A switch clicks.");
        }
    }
}

void RoomView::GiveDialogueItem(int item)
{
    if (item < 0 || !game) return;
    game->AddItem(item);
    std::string text = "Got " + game->ItemName(item);
    if (game->ItemCount(item) > 0) text += " (x" + std::to_string(game->ItemCount(item)) + ")";
    ShowMessage(text);
}

void RoomView::SetStoryFlag(int flag)
{
    if (!game || game->IsFlagSet(flag)) return;
    game->SetFlag(flag);
    SDL_Log("Story flag %d set (room %d)", flag, room_number);
    UpdateStatus();
}

// Killing or taking an entity sets the flags the game ties to it:
//  - hide rules "hide when flag set": the entity's own persistence flag, so it does not
//    come back next time;
//  - hide_from rules: when every enemy from the rule's start entity on is dead the room is
//    "cleared" and the flag is set.
void RoomView::OnEntityGone(std::size_t index)
{
    if (!game) return;
    for (const auto& r : flag_rules)
    {
        if (r.kind == FlagRule::HIDE && static_cast<std::size_t>(r.entity) == index && r.when_set)
            SetStoryFlag(r.flag);
    }
    for (const auto& r : flag_rules)
    {
        if (r.kind != FlagRule::HIDE_FROM || static_cast<std::size_t>(r.entity) > index) continue;
        bool any = false, all_dead = true;
        for (std::size_t i = static_cast<std::size_t>(r.entity); i < entities.size(); ++i)
        {
            if (!entities[i].hostile || !entity_table || entity_table->GetEnemy(entities[i].type) == nullptr) continue;
            any = true;
            if (!game->IsCollected(room_number, static_cast<int>(i))) { all_dead = false; break; }
        }
        if (any && all_dead) SetStoryFlag(r.flag);
    }
    ApplyFlagRules();
}

// Current box. On the last box of a question the choices are appended.
void RoomView::ShowDialoguePage()
{
    if (!dialogue_cb || dialogue_pages.empty()) return;
    std::string text = dialogue_pages.front().text;
    if (dialogue_asks && dialogue_pages.size() == 1) text += "\n\n(Y) Yes     (N) No";
    page_timer = 0.0;
    dialogue_cb(text, true);
}

void RoomView::AdvanceDialogue()
{
    if (dialogue_pages.empty()) return;
    if (dialogue_asks && dialogue_pages.size() == 1) return;   // waiting for Y / N
    GiveDialogueItem(dialogue_pages.front().give_item);
    dialogue_pages.erase(dialogue_pages.begin());
    if (dialogue_pages.empty())
    {
        if (dialogue_cb) dialogue_cb("", false);
        page_wait = 0.0;
        ApplyFlagRules();
        ContinueCutscene();
    }
    else
    {
        ShowDialoguePage();
    }
}

void RoomView::AnswerDialogue(bool yes)
{
    if (choice_handler)
    {
        auto handler = std::move(choice_handler);
        choice_handler = nullptr;
        dialogue_asks = false;
        dialogue_pages.clear();
        if (dialogue_cb) dialogue_cb("", false);
        handler(yes);
        return;
    }
    if (dialogue_pages.empty() || !dialogue_asks || dialogue_pages.size() != 1 || !dialogue_data || !game) return;
    const Json* next = yes ? dialogue_yes : dialogue_no;
    GiveDialogueItem(dialogue_pages.front().give_item);
    dialogue_asks = false;
    dialogue_yes = dialogue_no = nullptr;
    auto conv = dialogue_data->Follow(next, *game);
    dialogue_pages = std::move(conv.pages);
    dialogue_yes = conv.yes;
    dialogue_no = conv.no;
    dialogue_asks = conv.asks;
    if (dialogue_pages.empty())
    {
        if (dialogue_cb) dialogue_cb("", false);
        ApplyFlagRules();
    }
    else
    {
        ShowDialoguePage();
    }
}

void RoomView::Interact()
{
    if (!dialogue_pages.empty()) { AdvanceDialogue(); return; }
    if (StartDialogue()) return;
    if (TryShop()) return;
    if (!game || !entity_table) return;
    static const double dir_x[4] = { -1, 0, 1, 0 };   // facing: NW NE SE SW
    static const double dir_y[4] = { 0, -1, 0, 1 };
    const double reach = PLAYER_RADIUS + 0.4;
    const double qx = player_x + dir_x[facing] * reach, qy = player_y + dir_y[facing] * reach;

    int chest_ordinal = 0;
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        const auto& e = entities[i];
        if (e.type != 18 || !e.visible) continue;   // entity 18 = chest
        const int ordinal = chest_ordinal++;
        const double half = entity_table->GetHitboxBase(e.type) / 16.0;
        const double top = e.z + entity_table->GetHitboxHeight(e.type) / 16.0;
        if (std::abs(qx - e.x) > half || std::abs(qy - e.y) > half) continue;
        if (player_z >= top || player_z + PLAYER_HEIGHT <= e.z) continue;

        if (chest_open.active) return;   // already opening one
        if (game->IsChestOpened(room_number, static_cast<int>(i)))
        {
            ShowMessage("The chest is empty.");
            return;
        }
        game->MarkChestOpened(room_number, static_cast<int>(i));
        chest_open = {};
        chest_open.active = true;
        chest_open.entity = i;
        chest_open.content = (chests_enabled && ordinal < static_cast<int>(chest_contents.size()))
                                 ? chest_contents[ordinal] : -1;
        return;
    }
}

// Timeline: 0 - CHEST_OPEN_TIME the lid opens; at CHEST_OPEN_TIME the content is given and
// the item rises above the chest for CHEST_REVEAL_TIME while Nigel holds it up.
void RoomView::UpdateChestOpening(double dt)
{
    if (!chest_open.active) return;
    chest_open.t += dt;
    if (!chest_open.given && chest_open.t >= CHEST_OPEN_TIME)
    {
        chest_open.given = true;
        if (chest_open.content < 0) ShowMessage("The chest is empty.");
        else GiveChestContent(chest_open.content);
    }
    if (chest_open.t >= CHEST_OPEN_TIME + CHEST_REVEAL_TIME) chest_open.active = false;
}

// Follows the floor: climbs steps smoothly, falls (with gravity) off ledges, handles jumps.
void RoomView::UpdateVertical(double dt)
{
    if (climbing) { vertical_speed = 0.0; grounded = true; return; }   // on a ladder: no gravity
    const double floor_h = FloorUnder(player_x, player_y, player_z);
    if (grounded)
    {
        if (floor_h > player_z)
            player_z = std::min(floor_h, player_z + CLIMB_SPEED * dt);
        else if (floor_h < player_z - 1e-6)
        {
            grounded = false;          // walked off an edge
            vertical_speed = 0.0;
            if (player_z - floor_h > 2.5) Sfx(SoundEngine::SFX_FALL);
        }
        else
            player_z = floor_h;
    }
    if (!grounded)
    {
        vertical_speed = std::max(-MAX_FALL_SPEED, vertical_speed - GRAVITY * dt);
        player_z += vertical_speed * dt;
        air_peak = std::max(air_peak, player_z);
        if (player_z <= floor_h && vertical_speed <= 0.0)
        {
            // Landing: a short thud after a jump or a fall (not when stepping down a stair).
            if (air_peak - floor_h > 1.2) Sfx(SoundEngine::SFX_JUMP_LAND);
            air_peak = -1e9;
            player_z = floor_h;        // landed
            vertical_speed = 0.0;
            grounded = true;
        }
        else if (vertical_speed > 0.0 && player_z < floor_h)
        {
            player_z = floor_h;        // rose into a ledge within step range: stand on it
        }
    }
}

void RoomView::Step(int dir_x, int dir_y, double dt)
{
    const bool was_moving = moving;
    moving = false;
    blocked_text.clear();
    if (dead || swinging || chest_open.active || knock_time > 0.0 || !dialogue_pages.empty() || cutscene_busy > 0.0 || input_hold > 0.0 || fainted ||
        (game && game->HasStatus(GameState::STATUS_PARALYSIS)))
        dir_x = dir_y = 0;
    // Ladders (floor 0x0B: against the NW wall, 0x0C: against the NE wall): pressing towards the wall climbs
    // while the way ahead is too high to step onto; the player hangs there when no key is pressed.
    climbing = false;
    {
        const int cell_now = GetCell(cursor_x, cursor_y);
        const int ft = cell_now < 0 ? 0 : (cell_now & 0x3F);
        if ((ft == 0x0B || ft == 0x0C) && !dead && !fainted && !swinging && knock_time <= 0.0 && vertical_speed <= 0.0 &&
            dialogue_pages.empty() && cutscene_busy <= 0.0)
        {
            const int ldx = ft == 0x0B ? -1 : 0, ldy = ft == 0x0C ? -1 : 0;
            const double floor_here = FloorHeight(cursor_x, cursor_y);
            const bool hanging = player_z > floor_here + 0.05;
            if (dir_x == ldx && dir_y == ldy && !CanStandAt(player_x + ldx * 0.25, player_y + ldy * 0.25, player_z))
            {
                player_z += LADDER_SPEED * dt;
                facing = ft == 0x0B ? 0 : 1;
                climbing = true;
                dir_x = dir_y = 0;
                moving = true;
            }
            else if (hanging && dir_x == -ldx && dir_y == -ldy && (ldx != 0 || ldy != 0))
            {
                player_z = std::max(floor_here, player_z - LADDER_SPEED * dt);
                climbing = player_z > floor_here + 0.001;
                dir_x = dir_y = 0;
            }
            else if (hanging && dir_x == 0 && dir_y == 0) climbing = true;
        }
    }
    if (!dead && !swinging && !chest_open.active && knock_time <= 0.0 && dialogue_pages.empty() && cutscene_busy <= 0.0 && !fainted)
        IceSlide(dir_x, dir_y);
    if (dir_x != 0 || dir_y != 0)
    {
        // Facing: 0=NW(-x) 1=NE(-y) 2=SE(+x) 3=SW(+y). On a diagonal keep the current
        // facing if it is one of the two directions held, otherwise pick the x one.
        int candidates[2];
        int n = 0;
        if (dir_x < 0) candidates[n++] = 0;
        if (dir_x > 0) candidates[n++] = 2;
        if (dir_y < 0) candidates[n++] = 1;
        if (dir_y > 0) candidates[n++] = 3;
        bool keep = false;
        for (int i = 0; i < n; ++i) keep = keep || (candidates[i] == facing);
        if (!keep) facing = candidates[0];

        const double len = std::sqrt(static_cast<double>(dir_x * dir_x + dir_y * dir_y));
        const double dist = WALK_SPEED * dt;
        const double mx = dir_x / len * dist, my = dir_y / len * dist;
        // Axis by axis so the player slides along walls.
        bool a = TryMove(mx, 0.0);
        bool b = TryMove(0.0, my);
        // Doorways are one cell wide and the box is almost as wide: when the way is blocked
        // but would be free a few hundredths of a cell to the side, slide towards it.
        if (!a && mx != 0.0 && my == 0.0) a = Nudge(mx, 0.0, dist);
        if (!b && my != 0.0 && mx == 0.0) b = Nudge(0.0, my, dist);
        moving = a || b;
    }
    if (knock_time > 0.0)
    {
        // Thrown back: one axis at a time so walls stop it without sticking.
        const double step = std::min(dt, knock_time);
        TryMove(knock_vx * step, 0.0);
        TryMove(0.0, knock_vy * step);
        knock_time -= dt;
        moving = false;
    }
    UpdateVertical(dt);
    {
        int idx = -1;
        const double top = PlatformTop(player_x, player_y, player_z, &idx);
        support_entity = (grounded && idx >= 0 && std::abs(player_z - top) < 2 * PLATFORM_TOLERANCE) ? idx : -1;
    }
    CheckPickups();
    CheckSwitches();
    CheckPit();
    UpdateFloorEffects(dt);
    UpdateCombat(dt);
    anim_time = moving ? anim_time + dt : 0.0;

    // On a raft the raft's cell is what counts for the warps (the player's place on it depends on
    // where he arrived), and its first cells are not armed: the way back is right there.
    const bool riding = support_entity >= 0 && entities[support_entity].type == 53;
    const double wx = riding ? entities[support_entity].x : player_x, wy = riding ? entities[support_entity].y : player_y;
    if (riding && std::hypot(wx - arrival_x, wy - arrival_y) < 3.0) warp_armed = false;
    const int cx = static_cast<int>(std::floor(wx)), cy = static_cast<int>(std::floor(wy));
    if (cx != cursor_x || cy != cursor_y || (riding && !warp_armed))
    {
        cursor_x = cx;
        cursor_y = cy;
        CheckWarps();   // may load another room and reposition the player
    }
    if (moving) blocked_text.clear();   // it moved on at least one axis: not stuck
    if (moving || was_moving || !grounded || blocked_text != shown_blocked_text) UpdateStatus();
}

// ---- Save / load -----------------------------------------------------------------------
// The game state (inventory, flags, chests...) plus where the player stands.
bool RoomView::SaveGame()
{
    if (!game || save_path.empty()) return false;
    if (dead || chest_open.active || !dialogue_pages.empty() || menu_open)
    {
        Sfx(SoundEngine::SFX_ERROR);
        ShowMessage("Cannot save right now.");
        return false;
    }
    std::ostringstream os;
    os << "room " << room_number << "\nposition " << player_x << ' ' << player_y << "\nfacing " << facing << "\n";
    if (church_room >= 0) os << "church " << church_room << ' ' << church_x << ' ' << church_y << "\n";
    const bool ok = game->Save(save_path, os.str());
    ShowMessage(ok ? "Game saved." : "Saving failed.");
    if (ok) Sfx(SoundEngine::SFX_CURSOR_SELECT);
    return ok;
}

bool RoomView::LoadGame()
{
    if (!game || save_path.empty()) return false;
    std::map<std::string, std::string> extra;
    if (!game->Load(save_path, extra))
    {
        ShowMessage("No saved game.");
        Sfx(SoundEngine::SFX_ERROR);
        return false;
    }
    int room = room_number;
    double x = player_x, y = player_y;
    if (extra.count("room")) room = std::atoi(extra["room"].c_str());
    if (extra.count("position")) std::sscanf(extra["position"].c_str(), "%lf %lf", &x, &y);
    church_room = -1;
    if (extra.count("church"))
        std::sscanf(extra["church"].c_str(), "%d %lf %lf", &church_room, &church_x, &church_y);
    if (extra.count("facing")) facing = std::clamp(std::atoi(extra["facing"].c_str()), 0, 3);
    if (!RoomExists(room)) room = room_number;

    // Drop everything in flight.
    fx_level = 0.0; fx_mode = FxMode::NONE; cutscene_busy = 0.0; busy_then_warp = false; friday_state = 0;
    fainted = false; player_hidden = false; input_hold = 0.0; faint_warp = false;
    cutscene_ops.clear();
    dead = false;
    death_time = hurt_time = knock_time = swing_time = attack_cooldown = 0.0;
    swinging = swing_hit_pending = false;
    invulnerable_time = HIT_INVULNERABILITY;
    chest_open = {};
    sparks.clear();
    if (menu_open) CloseMenu();
    jingle_time = 0.0;
    playing_bgm = -1;
    game_over_shown = false;
    poison_ticks = game->HasStatus(GameState::STATUS_POISON) ? 8 : 0;
    paralysis_time = game->HasStatus(GameState::STATUS_PARALYSIS) ? 10.0 : 0.0;
    paralysis_presses = 0;
    const SDL_Point arrival{ static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)) };
    LoadRoom(room, &arrival);
    SetPlayerPosition(x, y);
    ShowMessage("Game loaded.");
    return true;
}

void RoomView::Respawn()
{
    if (!dead || !game) return;
    game->Revive();
    dead = false;
    game_over_shown = false;
    death_time = 0.0;
    hurt_time = 0.0;
    invulnerable_time = HIT_INVULNERABILITY;
    fx_level = 0.0; fx_mode = FxMode::NONE; cutscene_busy = 0.0; busy_then_warp = false; friday_state = 0;
    fainted = false; player_hidden = false; input_hold = 0.0; faint_warp = false;
    playing_bgm = -1;
    poison_ticks = 0;
    paralysis_time = 0.0;
    message_time = 0.0;
    if (message_cb) message_cb("");
    if (church_room >= 0 && RoomExists(church_room))
    {
        const SDL_Point arrival{ static_cast<int>(std::floor(church_x)), static_cast<int>(std::floor(church_y)) };
        LoadRoom(church_room, &arrival);
        SetPlayerPosition(church_x, church_y);
        ShowMessage("You wake up in the church.");
    }
    else
    {
        LoadRoom(room_number);
        ShowMessage("Back on your feet");
    }
}

std::string RoomView::SlotSummary(int slot) const
{
    std::ifstream f(SlotPath(slot));
    std::string line;
    if (!f || !std::getline(f, line) || line.rfind("landstalker-save 1", 0) != 0) return "";
    std::string room_text = "?", gold_text = "0", health_text;
    while (std::getline(f, line))
    {
        if (line.rfind("room ", 0) == 0) room_text = line.substr(5);
        else if (line.rfind("gold ", 0) == 0) gold_text = line.substr(5);
        else if (line.rfind("maxhealth256 ", 0) == 0) health_text = std::to_string(std::atoi(line.c_str() + 13) / 256);
    }
    return "Room " + room_text + ", " + gold_text + " gold" + (health_text.empty() ? "" : ", " + health_text + " HP");
}

void RoomView::SetPlayerPosition(double x, double y)
{
    player_x = x;
    player_y = y;
    cursor_x = static_cast<int>(std::floor(x));
    cursor_y = static_cast<int>(std::floor(y));
    player_z = FloorHeight(cursor_x, cursor_y);
    {
        // Arriving on a raft or a block: stand on it (up to one unit above the floor).
        int idx = -1;
        const double top = PlatformTop(x, y, player_z + 1.0 - PLATFORM_TOLERANCE, &idx);
        if (top > player_z) player_z = top;
    }
    vertical_speed = 0.0;
    grounded = true;
    camera_snap = true;
    warp_armed = false;   // do not warp immediately if placed on a warp
    UpdateStatus();
}

void RoomView::SimulateMove(int dir_x, int dir_y, double seconds)
{
    const double slice = 1.0 / 60.0;
    for (double t = 0.0; t < seconds; t += slice)
        Step(dir_x, dir_y, slice);
}

void RoomView::OnUpdate(double delta)
{
    if (delta > 0.1) delta = 0.1;   // e.g. after a stall or while dragging the window
    frame_dt = delta;
    if (paused) return;
    UpdateAnimations(delta);
    input_hold = std::max(0.0, input_hold - delta);
    UpdateFaint(delta);
    if (jingle_time > 0.0)
    {
        jingle_time -= delta;
        if (jingle_time <= 0.0) { jingle_time = 0.0; PlayRoomMusic(); }
    }
    if (menu_open) return;   // the game is paused behind the menu
    UpdateFx(delta);
    shake_t = std::max(0.0, shake_t - delta);
    friday_t += delta;
    if (friday_state == 1 && friday_t >= 0.5) friday_state = 2;
    if (friday_state == 3 && friday_t >= 0.8) friday_state = 0;
    if (cutscene_busy > 0.0)
    {
        cutscene_busy -= delta;
        if (cutscene_busy <= 0.0)
        {
            cutscene_busy = 0.0;
            if (busy_then_warp) { busy_then_warp = false; warp_pending = true; }
            else ContinueCutscene();
        }
    }
    if (page_wait > 0.0 && !dialogue_pages.empty())
    {
        page_timer += delta;
        if (page_timer >= page_wait)
        {
            page_timer = 0.0;
            AdvanceDialogue();
        }
    }
    if (message_time > 0.0)
    {
        message_time -= delta;
        if (message_time <= 0.0 && message_cb) message_cb("");
    }
    auto& kb = Keyboard::GetInstance();
    int dx = 0, dy = 0;
    if (kb.IsKeyPressed(SDL_SCANCODE_UP))    dx -= 1;   // north-west
    if (kb.IsKeyPressed(SDL_SCANCODE_DOWN))  dx += 1;   // south-east
    if (kb.IsKeyPressed(SDL_SCANCODE_LEFT))  dy += 1;   // south-west
    if (kb.IsKeyPressed(SDL_SCANCODE_RIGHT)) dy -= 1;   // north-east
    if (bot_input) { dx = bot_dx; dy = bot_dy; }
    // No diagonals: with one key on each axis, keep the one pressed last.
    if (dx != 0 && dy != 0)
    {
        if (last_key_was_x_axis) dy = 0;
        else dx = 0;
    }
    if (game && game->HasStatus(GameState::STATUS_CONFUSION) && !input_locked) { dx = -dx; dy = -dy; }
    if (input_locked)
    {
        // Scripted scenes: only the recorded input moves the player (diagonals allowed).
        dx = dy = 0;
        if (playback && playback_index < playback->size())
        {
            const int buttons = (*playback)[playback_index].first;
            if (buttons & 1) dx -= 1;
            if (buttons & 2) dx += 1;
            if (buttons & 4) dy += 1;
            if (buttons & 8) dy -= 1;
            const int frames = (*playback)[playback_index].second;
            if (frames != 254)
            {
                playback_timer -= delta;
                if (playback_timer <= 0.0 && ++playback_index < playback->size())
                    playback_timer += (*playback)[playback_index].second / 60.0;
            }
        }
    }
    Step(dx, dy, delta);
    if (warp_pending) ApplyPendingWarp();
}

// A pit cell (floor type 7): whoever stands on it falls to the room below, if the room has one.
void RoomView::CheckPit()
{
    if (fall_dest < 0 || !grounded || dead || cutscene_busy > 0.0 || warp_pending || !RoomExists(Redirect(fall_dest))) return;
    const int cell = GetCell(cursor_x, cursor_y);
    if (cell < 0 || (cell & 0x3F) != 7) return;
    if (std::abs(player_z - FloorHeight(cursor_x, cursor_y)) > 0.5) return;
    Sfx(SoundEngine::SFX_FALL);
    pending_warp = { Redirect(fall_dest), cursor_x, cursor_y, -1, true };
    cutscene_busy = StartFx("fade_out");
    if (cutscene_busy > 0.0) busy_then_warp = true;
    else warp_pending = true;
}

// What the floor under the player does (code/gamelogic/gamelogic4.asm, ProcessFloorType): spikes and
// swamp hurt (1), lava burns (5); Iron Boots protect from spikes, Fireproof boots from lava, Healing
// Boots mend a little while walking; ice slides the player (Snow Spikes grip it).
// Leaving the room by a ladder or an elevator once the player is above the room's highest point.
void RoomView::ClimbOut(int kind)
{
    pending_warp = { Redirect(climb_dest), cursor_x, cursor_y, -1, true, kind };
    cutscene_busy = StartFx("fade_out");
    if (cutscene_busy > 0.0) busy_then_warp = true;
    else warp_pending = true;
}

void RoomView::UpdateFloorEffects(double dt)
{
    if (!game || dead) return;
    {
        const int c = GetCell(cursor_x, cursor_y);
        const int t = c < 0 ? 0 : (c & 0x3F);
        if (climb_dest >= 0 && !warp_pending && cutscene_busy <= 0.0 && RoomExists(Redirect(climb_dest)) &&
            player_z + 0.0625 > room_max_height)
        {
            if (t == 0x0B || t == 0x0C) { ClimbOut(1); return; }
            if (t == 0x0F && support_entity >= 0) { ClimbOut(2); return; }
        }
    }
    if (!grounded) return;
    const int cell = GetCell(cursor_x, cursor_y);
    if (cell < 0 || std::abs(player_z - FloorHeight(cursor_x, cursor_y)) > 0.5) { heal_acc = 0.0; return; }
    const int type = cell & 0x3F;
    const int boots = game->EquippedBoots();
    if ((type == 0x10 && boots != GameState::BOOTS_IRON) || type == 0x19) HurtPlayer(1, player_x, player_y, false);
    else if (type == 0x29 && boots != GameState::BOOTS_FIREPROOF) HurtPlayer(5, player_x, player_y, false);
    if (boots == GameState::BOOTS_HEALING && moving && game->GetHealth() < game->GetMaxHealth())
    {
        heal_acc += dt;
        if (heal_acc >= 256.0 / 60.0) { heal_acc = 0.0; game->Heal(1.0); }   // one point every 256 steps
    }
}

// Forced direction on an ice cell (0x2A NE, 0x2B SE, 0x2C SW, 0x2D NW) unless wearing Snow Spikes.
bool RoomView::IceSlide(int& dir_x, int& dir_y) const
{
    if (!game || !grounded || game->EquippedBoots() == GameState::BOOTS_SNOW) return false;
    const int cell = GetCell(cursor_x, cursor_y);
    if (cell < 0) return false;
    switch (cell & 0x3F)
    {
    case 0x2A: dir_x = 0; dir_y = -1; return true;
    case 0x2B: dir_x = 1; dir_y = 0; return true;
    case 0x2C: dir_x = 0; dir_y = 1; return true;
    case 0x2D: dir_x = -1; dir_y = 0; return true;
    default: return false;
    }
}

void RoomView::CheckWarps()
{
    const RoomWarp* hit = nullptr;
    for (const auto& w : warps)
    {
        if (cursor_x >= w.x && cursor_x < w.x + w.w && cursor_y >= w.y && cursor_y < w.y + w.h)
        {
            hit = &w;
            break;
        }
    }
    if (!hit) { warp_armed = true; return; }
    if (!warp_armed) return;
    // A locked door cannot be walked through (opening it swaps its cells for an open doorway).
    {
        const int floor_type = GetCell(cursor_x, cursor_y) & 0x3F;
        if (floor_type == 0x1A || floor_type == 0x26 || floor_type == 0x27) return;
    }
    const int dest_room = Redirect(hit->dest_room);
    if (!RoomExists(dest_room)) return;
    // A warp pad (floor type 8): a flash of light, then the other pad (no door sound).
    if ((GetCell(cursor_x, cursor_y) & 0x3F) == 8 && !intro_active && cutscene_busy <= 0.0 && !warp_pending)
    {
        Sfx(0x4D);   // SND_WarpPad
        pending_warp = { dest_room, hit->dest_x, hit->dest_y, -1 };
        cutscene_busy = StartFx("warp_pad");
        if (cutscene_busy > 0.0) busy_then_warp = true;
        else warp_pending = true;
        return;
    }
    const SDL_Point arrival{ hit->dest_x, hit->dest_y };
    if (!intro_active) Sfx(SoundEngine::SFX_DOOR);
    try
    {
        LoadRoom(dest_room, &arrival);
    }
    catch (const std::exception& e)
    {
        SDL_Log("Warp to room %d failed: %s", hit->dest_room, e.what());
    }
}

void RoomView::UpdateStatus()
{
    if (!status_cb) return;
    const int cell = GetCell(cursor_x, cursor_y);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Room %d  pos (%.2f,%.2f) z %.2f  hm %04X  warps %d",
                  room_number, player_x, player_y, player_z, cell < 0 ? 0 : cell, static_cast<int>(warps.size()));
    std::string text = buf;
    if (game && !game->GetFlags().empty()) text += "  flags " + std::to_string(game->GetFlags().size());
    if (game && game->HasAnyStatus())
    {
        text += "  [";
        if (game->HasStatus(GameState::STATUS_POISON)) text += "POISON ";
        if (game->HasStatus(GameState::STATUS_CONFUSION)) text += "CONFUSED ";
        if (game->HasStatus(GameState::STATUS_PARALYSIS)) text += "PARALYSED ";
        if (game->HasStatus(GameState::STATUS_CURSE)) text += "CURSED ";
        text.back() = ']';
    }
    if (!blocked_text.empty()) text += "  BLOCKED: " + blocked_text;
    shown_blocked_text = blocked_text;
    status_cb(text);
}

// height_px is in unscaled room pixels (it is multiplied by the zoom like the rest).
SDL_FPoint RoomView::ToScreen(float world_x, float world_y, float height_px) const
{
    // Same projection as the original map (see Tilemap3D::Iso3DToPixel), in pixels.
    // The heightmap / entity frame sits half a cell off the block frame: the art for the
    // cell at (x, y) is drawn at (x + 0.5, y - 0.5), i.e. 16 px to the right. Measured on
    // room 0's floor tip and on the wooden platforms of room 1 (see GRID_SHIFT_*).
    const float xx = world_x + GRID_SHIFT_X - hm_left, yy = world_y + GRID_SHIFT_Y - hm_top;
    const float px = ((xx - yy + (height - 1)) * 2 + hm_left) * TILE_PX;
    const float py = (xx + yy + hm_top) * TILE_PX - height_px;
    return { origin.x + px * scale, origin.y + py * scale };
}

void RoomView::DrawLayer(SDL_Renderer* renderer, const std::vector<int>& layer, int layer_offset_tiles,
                         SDL_Texture* texture) const
{
    if (!texture) texture = blocks;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const int index = layer[y * width + x];
            const SDL_FRect src = {
                static_cast<float>((index % BLOCKS_PER_ROW) * BLOCK_PX),
                static_cast<float>((index / BLOCKS_PER_ROW) * BLOCK_PX),
                static_cast<float>(BLOCK_PX), static_cast<float>(BLOCK_PX)
            };
            const int px = ((x - y + (height - 1)) * 2 + hm_left + layer_offset_tiles) * TILE_PX;
            const int py = (x + y + hm_top) * TILE_PX;
            const SDL_FRect dst = {
                origin.x + px * scale, origin.y + py * scale,
                BLOCK_PX * scale, BLOCK_PX * scale
            };
            SDL_RenderTexture(renderer, texture, &src, &dst);
        }
    }
}

void RoomView::DrawCell(SDL_Renderer* renderer, int wx, int wy, float height_px) const
{
    const SDL_FPoint n = ToScreen(static_cast<float>(wx), static_cast<float>(wy), height_px);
    const SDL_FPoint e = ToScreen(static_cast<float>(wx + 1), static_cast<float>(wy), height_px);
    const SDL_FPoint s = ToScreen(static_cast<float>(wx + 1), static_cast<float>(wy + 1), height_px);
    const SDL_FPoint w = ToScreen(static_cast<float>(wx), static_cast<float>(wy + 1), height_px);
    const SDL_FPoint pts[5] = { n, e, s, w, n };
    SDL_RenderLines(renderer, pts, 5);
}

void RoomView::DrawEntities(SDL_Renderer* renderer)
{
    // Painter's algorithm, same ordering rule as the editor: farther (x + y + base) first.
    struct Item
    {
        double dist, left, top;
        std::function<void()> draw;
        // Sprites only: what the priority tiles need to know to hide them.
        bool body = false;
        double cell_depth = 0.0, z = 0.0;
        SDL_FPoint feet{};
    };
    std::vector<Item> items;

    // Entities and the player are already in the heightmap-cell frame (see CentreEntities).
    auto screen_pos = [this](double x, double y, double z) {
        return ToScreen(static_cast<float>(x), static_cast<float>(y),
                        static_cast<float>(z * HEIGHT_UNIT_PX));
    };

    if (entity_table)
    {
        for (std::size_t ei = 0; ei < entities.size(); ++ei)
        {
            const auto& e = entities[ei];
            if (!entity_table->Has(e.type) || !e.visible) continue;
            if (game && game->IsCollected(room_number, static_cast<int>(ei)) && !e.dying) continue;
            if (e.dying && static_cast<int>(e.die_t * 14.0) % 2 == 1) continue;   // flicker
            if (e.hit_flash > 0.0 && static_cast<int>(e.hit_flash * 20.0) % 2 == 0) continue;   // blink
            if (e.type == 18 && chest_sprite)
            {
                // Chest: closed frame, the 4 opening frames while it opens, then the open one.
                const auto& frames = chest_sprite->GetAnimation(0u);
                if (!frames.empty())
                {
                    std::size_t k = 0;
                    const bool opening = chest_open.active && chest_open.entity == ei;
                    if (opening)
                        k = 1 + std::min<std::size_t>(3, static_cast<std::size_t>(chest_open.t / (CHEST_OPEN_TIME / 4)));
                    else if (game && game->IsChestOpened(room_number, static_cast<int>(ei)))
                        k = frames.size() - 1;
                    k = std::min(k, frames.size() - 1);
                    const std::string frame = frames[k];
                    const SDL_FPoint cp = screen_pos(e.x, e.y, e.z);
                    items.push_back({ e.x + e.y + 8 / 256.0, e.y - 8 / 256.0, e.z + 1.0,
                        [this, renderer, frame, cp]() { chest_sprite->Draw(renderer, frame, cp.x, cp.y, scale, false); },
                        true, std::floor(e.x) + std::floor(e.y), e.z, cp });

                    // The content rising above the chest, once the lid is open.
                    if (opening && chest_open.t >= CHEST_OPEN_TIME && chest_open.content >= 0 &&
                        chest_open.content < 0x40 && entity_table)
                    {
                        bool f = false;
                        const EntityTable::Image* item_img = entity_table->GetImage(0xC0 + chest_open.content, 0, f);
                        if (item_img)
                        {
                            const double rise = std::min(1.0, (chest_open.t - CHEST_OPEN_TIME) / 0.4) * 1.0;
                            const EntityTable::Image image = *item_img;
                            const SDL_FPoint ip = screen_pos(e.x, e.y, e.z + 1.2 + rise);
                            items.push_back({ e.x + e.y + 0.5, e.y, e.z + 2.0,
                                [this, renderer, image, ip]() {
                                    const SDL_FRect dst = { ip.x - image.anchor_x * scale, ip.y - image.anchor_y * scale,
                                                            image.width * scale, image.height * scale };
                                    SDL_RenderTexture(renderer, image.texture, nullptr, &dst);
                                } });
                        }
                    }
                }
                continue;
            }
            bool flip = false;
            const EntityTable::Image* img = entity_table->GetImage(e.type, e.orientation, flip);
            EntityTable::Image walk_image;
            if (e.walking && !e.images[0].texture)
            {
                bool f = false;
                const auto kind = entity_table->GetKind(e.type, e.orientation, f);
                if (kind != EntityTable::DEFAULT)
                {
                    const auto& frames = entity_table->GetWalkFrames(e.type, kind);
                    if (!frames.empty())
                    {
                        walk_image = frames[static_cast<std::size_t>(e.walk_time / ENEMY_WALK_FRAME_TIME) % frames.size()];
                        img = &walk_image;
                        flip = f;
                    }
                }
            }
            EntityTable::Image pose_image;
            if (!e.images[0].texture)
            {
                bool f = false;
                if (EnemyPoseFrame(e, pose_image, f)) { img = &pose_image; flip = f; }
            }
            EntityTable::Image room_image;
            {
                bool f = false;
                const auto kind = entity_table->GetKind(e.type, e.orientation, f);
                const auto& ri = e.images[kind].texture ? e.images[kind] : e.images[0];
                if (ri.texture)
                {
                    room_image = { ri.texture, ri.w, ri.h, ri.ax, ri.ay };
                    img = &room_image;
                    flip = f;
                }
            }
            if (!img) continue;
            const int base = entity_table->GetHitboxBase(e.type);
            const int hbh = entity_table->GetHitboxHeight(e.type);
            const SDL_FPoint p = screen_pos(e.x, e.y, e.z);
            // Copy: `img` may point at a local of this iteration (walk / room image), and the
            // draw callback runs after the loop.
            const EntityTable::Image image = *img;
            Uint8 alpha = 255;
            SDL_Color tint{ 255, 255, 255, 255 };
            if (e.appear_t >= 0.0) alpha = static_cast<Uint8>(255.0 * std::min(1.0, e.appear_t / 0.9));
            if (e.dying) tint = { 255, 200, 120, 255 };
            // A boss about to shoot flashes red.
            if ((e.type == 124 || e.type == 133 || e.type == 160 || e.type == 162 || e.type == 165) &&
                e.ai_cooldown > 0.0 && e.ai_cooldown < 0.45 && static_cast<int>(e.ai_cooldown * 16.0) % 2 == 0)
                tint = { 255, 110, 110, 255 };
            items.push_back({ e.x + e.y + base / 256.0, e.y - base / 256.0, e.z + hbh / 256.0,
                [this, renderer, image, flip, p, alpha, tint]() {
                    const float ax = flip ? (image.width - image.anchor_x) : image.anchor_x;
                    const SDL_FRect dst = { p.x - ax * scale, p.y - image.anchor_y * scale,
                                            image.width * scale, image.height * scale };
                    const bool tinted = alpha != 255 || tint.r != 255 || tint.g != 255 || tint.b != 255;
                    if (tinted)
                    {
                        SDL_SetTextureAlphaMod(image.texture, alpha);
                        SDL_SetTextureColorMod(image.texture, tint.r, tint.g, tint.b);
                    }
                    SDL_RenderTextureRotated(renderer, image.texture, nullptr, &dst, 0.0, nullptr,
                                             flip ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
                    if (tinted)
                    {
                        SDL_SetTextureAlphaMod(image.texture, 255);
                        SDL_SetTextureColorMod(image.texture, 255, 255, 255);
                    }
                },
                true, std::floor(e.x) + std::floor(e.y), e.z, p });
        }
    }

    if (entity_table)
    {
        for (const auto& pr : projectiles)
        {
            bool flip = false;
            const EntityTable::Image* img = entity_table->GetImage(pr.type, pr.orientation, flip);
            if (!img) continue;
            const EntityTable::Image image = *img;
            const SDL_FPoint p = screen_pos(pr.x, pr.y, pr.z);
            items.push_back({ pr.x + pr.y + 0.5, pr.y, pr.z + 1.0,
                [this, renderer, image, flip, p]() {
                    const float ax = flip ? (image.width - image.anchor_x) : image.anchor_x;
                    const SDL_FRect dst = { p.x - ax * scale, p.y - image.anchor_y * scale,
                                            image.width * scale, image.height * scale };
                    SDL_RenderTextureRotated(renderer, image.texture, nullptr, &dst, 0.0, nullptr,
                                             flip ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
                } });
        }
    }

    for (const auto& sp : sparks)
    {
        const double t = sp.t / SPARK_TIME;                 // 0..1
        const SDL_FPoint c = screen_pos(sp.x, sp.y, sp.z);
        const int rays = sp.big ? 12 : 8;
        const float inner = static_cast<float>((4.0 + 10.0 * t) * scale);
        const float outer = static_cast<float>((10.0 + (sp.big ? 26.0 : 16.0) * t) * scale);
        items.push_back({ sp.x + sp.y + 1.0, sp.y, sp.z + 3.0, [this, renderer, sp, c, rays, inner, outer, t]() {
            const Uint8 alpha = static_cast<Uint8>(255.0 * (1.0 - t));
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            for (int k = 0; k < rays; ++k)
            {
                const double a = 6.2831853 * k / rays + sp.t * 3.0;
                // Rays alternate between two colours.
                if (sp.hurt_player) SDL_SetRenderDrawColor(renderer, 255, (k & 1) ? 80 : 160, 40, alpha);
                else SDL_SetRenderDrawColor(renderer, 255, 255, (k & 1) ? 120 : 255, alpha);
                // Squashed vertically: the burst is drawn on the isometric screen.
                const float cx = static_cast<float>(std::cos(a)), cy = static_cast<float>(std::sin(a)) * 0.6f;
                SDL_RenderLine(renderer, c.x + cx * inner, c.y + cy * inner, c.x + cx * outer, c.y + cy * outer);
            }
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        } });
    }

    // Blink while protected after a hit.
    const bool blink_off = invulnerable_time > 0.0 && !dead &&
                           (static_cast<int>(invulnerable_time * 10.0) % 2 == 0);
    if (player && !blink_off && !player_hidden)
    {
        // Nigel's animations (sprite_000/sprite.yaml), each as back (NE/NW) / front (SE/SW):
        //   0/1 idle   2/3 walk   8/9 jump   12/13 hit   14/15 sword slash (4 frames, with arc)
        //   17/18 death (5 frames)
        // facing: 0=NW 1=NE 2=SE 3=SW; front for SE/SW, mirrored for NW and SE.
        const bool back = (facing == 0 || facing == 1);
        const bool flip = (facing == 0 || facing == 2);
        auto anim = [&](unsigned back_index) -> const std::vector<std::string>& {
            return player->GetAnimation(back ? back_index : back_index + 1u);
        };
        std::string frame;
        auto pick = [&](const std::vector<std::string>& a, double t, double duration) {
            if (a.empty()) return;
            const std::size_t k = std::min(a.size() - 1,
                static_cast<std::size_t>(std::max(0.0, t) / (duration / a.size())));
            frame = a[k];
        };
        if (dead)
        {
            pick(anim(17), death_time, DEATH_ANIM_TIME);
        }
        else if (fainted)
        {
            pick(anim(17), faint_t, DEATH_ANIM_TIME);   // lying on the ground
        }
        else if (chest_open.active && chest_open.t >= CHEST_OPEN_TIME)
        {
            // Holding the item up (animation 16).
            pick(player->GetAnimation(16u), chest_open.t - CHEST_OPEN_TIME, CHEST_REVEAL_TIME);
        }
        else if (swinging)
        {
            pick(anim(14), swing_anim, SWING_DURATION);
        }
        else if (hurt_time > 0.0)
        {
            const auto& a = anim(12);
            if (!a.empty()) frame = a[hurt_time > HURT_POSE_TIME / 2 ? 0 : std::min<std::size_t>(1, a.size() - 1)];
        }
        else if (!grounded)
        {
            const auto& a = anim(8);   // rising: first frame, falling: second
            if (!a.empty()) frame = a[vertical_speed > 0.0 ? 0 : std::min<std::size_t>(1, a.size() - 1)];
        }
        else if (moving)
        {
            const auto& walk = anim(2);
            if (!walk.empty())
            {
                const std::size_t cycle = std::min<std::size_t>(4, walk.size());
                frame = walk[static_cast<std::size_t>(anim_time / WALK_FRAME_TIME) % cycle];
            }
        }
        if (frame.empty())
        {
            const auto& idle = anim(0);
            if (!idle.empty()) frame = idle.front();
        }
        if (!frame.empty())
        {
            const double z = player_z;
            const double px = player_x, py = player_y;
            const SDL_FPoint p = screen_pos(px, py, z);
            // The ailments tint the sprite: green poison, blue paralysis, violet curse, yellow confusion.
            SDL_Color tint = { 255, 255, 255, 255 };
            if (game)
            {
                if (game->HasStatus(GameState::STATUS_PARALYSIS)) tint = { 150, 160, 255, 255 };
                else if (game->HasStatus(GameState::STATUS_POISON)) tint = { 150, 255, 150, 255 };
                else if (game->HasStatus(GameState::STATUS_CURSE)) tint = { 210, 140, 255, 255 };
                else if (game->HasStatus(GameState::STATUS_CONFUSION)) tint = { 255, 255, 130, 255 };
            }
            items.push_back({ px + py + 8 / 256.0, py - 8 / 256.0, z,
                [this, renderer, frame, flip, p, tint]() { player->Draw(renderer, frame, p.x, p.y, scale, flip, tint); },
                true, std::floor(px) + std::floor(py), z, p });
        }
    }

    // Tiles with the priority bit are drawn over the sprites that stand behind them (a wall
    // hides the character walking behind it); see PriorityHides().
    std::vector<PriorityBlock> priority_blocks;
    if (blocks_priority && !std::getenv("LS_NO_PRIORITY"))
    {
        BuildPriorityBlocks(bg, 2, priority_blocks);
        BuildPriorityBlocks(fg, 0, priority_blocks);
    }

    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.dist != b.dist) return a.dist < b.dist;
        if (a.left != b.left) return a.left < b.left;
        return a.top < b.top;
    });
    for (auto& it : items)
    {
        it.draw();
        if (!it.body) continue;
        const SDL_FRect rect = { it.feet.x - 24 * scale, it.feet.y - 64 * scale, 48 * scale, 70 * scale };
        for (const auto& pb : priority_blocks)
            if (PriorityHides(pb, it.cell_depth, it.z, rect))
                SDL_RenderTexture(renderer, blocks_priority, &pb.src, &pb.dst);
    }
}

void RoomView::BuildPriorityBlocks(const std::vector<int>& layer, int layer_offset_tiles,
                                   std::vector<PriorityBlock>& out) const
{
    if (block_has_priority.empty()) return;
    // Inverse of ToScreen / DrawLayer: the block at (bx, by) shows, at height z, the cell
    // corner (bx + t/4 + z, by - t/4 + z) of the grid frame.
    const double t = layer_offset_tiles;
    for (int by = 0; by < height; ++by)
    {
        for (int bx = 0; bx < width; ++bx)
        {
            const int index = layer[by * width + bx];
            if (index < 0 || index >= static_cast<int>(block_has_priority.size()) || !block_has_priority[index]) continue;
            PriorityBlock pb{ -1.0, 0, false, {}, {} };
            for (int z = 0; z < 16; ++z)
            {
                const double fx = bx + t / 4.0 + z - GRID_SHIFT_X + hm_left;
                const double fy = by - t / 4.0 + z - GRID_SHIFT_Y + hm_top;
                const int cx = static_cast<int>(std::floor(fx)), cy = static_cast<int>(std::floor(fy));
                const int cell = GetCell(cx, cy);
                if (cell < 0) continue;
                const int h = (cell >> 8) & 0x0F;
                if (h < z) continue;
                const double depth = cx + cy;
                if (depth >= pb.depth)
                {
                    pb.depth = depth;
                    pb.z = z;
                    pb.wall = h > z;
                }
            }
            if (pb.depth < 0.0) continue;
            pb.src = { static_cast<float>((index % BLOCKS_PER_ROW) * BLOCK_PX),
                       static_cast<float>((index / BLOCKS_PER_ROW) * BLOCK_PX),
                       static_cast<float>(BLOCK_PX), static_cast<float>(BLOCK_PX) };
            const int px = ((bx - by + (height - 1)) * 2 + hm_left + layer_offset_tiles) * TILE_PX;
            const int py = (bx + by + hm_top) * TILE_PX;
            pb.dst = { origin.x + px * scale, origin.y + py * scale, BLOCK_PX * scale, BLOCK_PX * scale };
            out.push_back(pb);
        }
    }
}

// A block covers a sprite only if it is in front of it (a larger x + y; the same cell is
// not), reaches above its feet (a floor tile at the sprite's own level hides nothing) and
// overlaps it on screen.
bool RoomView::PriorityHides(const PriorityBlock& b, double sprite_depth, double sprite_z, const SDL_FRect& r)
{
    if (b.depth <= sprite_depth) return false;
    const double top = b.z + (b.wall ? 1.0 : 0.0);
    if (top <= sprite_z + 0.01) return false;
    return b.dst.x < r.x + r.w && b.dst.x + b.dst.w > r.x && b.dst.y < r.y + r.h && b.dst.y + b.dst.h > r.y;
}

void RoomView::ScanOcclusion()
{
    std::vector<PriorityBlock> blocks_pri;
    BuildPriorityBlocks(bg, 2, blocks_pri);
    BuildPriorityBlocks(fg, 0, blocks_pri);
    int found = 0;
    for (int cy = CELL_OFFSET; cy < CELL_OFFSET + hm_height; ++cy)
    {
        for (int cx = CELL_OFFSET; cx < CELL_OFFSET + hm_width; ++cx)
        {
            const int cell = GetCell(cx, cy);
            if (cell < 0 || (cell & 0x6000) != 0) continue;
            const double z = FloorHeight(cx, cy);
            const SDL_FPoint feet = ToScreen(static_cast<float>(cx + 0.5), static_cast<float>(cy + 0.5),
                                             static_cast<float>(z * HEIGHT_UNIT_PX));
            const SDL_FRect body = { feet.x - 16 * scale, feet.y - 44 * scale, 32 * scale, 44 * scale };
            int hits = 0;
            for (const auto& b : blocks_pri) if (PriorityHides(b, cx + cy, z, body)) ++hits;
            if (hits > 0 && found < 12)
            {
                SDL_Log("OCC room %d cell (%d, %d) z %.0f: %d priority blocks over the player", room_number, cx, cy, z, hits);
                ++found;
            }
        }
    }
    SDL_Log("OCC scan done: %d cells listed (max 12)", found);
}

void RoomView::SetScale(float new_scale)
{
    scale = new_scale;
    camera_snap = true;
}

SDL_FPoint RoomView::CameraTarget() const
{
    // Same projection as ToScreen(), in unscaled room pixels, aimed at the player's torso
    // (or at the boss whose cutscene is playing).
    double tx = player_x, ty = player_y, tz = player_z;
    if (camera_focus >= 0 && static_cast<std::size_t>(camera_focus) < entities.size())
    {
        const auto& f = entities[camera_focus];
        tx = f.x; ty = f.y; tz = f.z;
    }
    const double xx = tx + GRID_SHIFT_X - hm_left, yy = ty + GRID_SHIFT_Y - hm_top;
    const double px = ((xx - yy + (height - 1)) * 2 + hm_left) * TILE_PX;
    const double py = (xx + yy + hm_top) * TILE_PX - tz * HEIGHT_UNIT_PX - 16.0;
    return { static_cast<float>(px), static_cast<float>(py) };
}

void RoomView::UpdateCamera(double dt)
{
    const double view_w = viewport.w / scale, view_h = viewport.h / scale;
    const SDL_FPoint t = CameraTarget();
    auto axis = [](double target, double view, double room) {
        if (room <= view) return (room - view) / 2.0;          // smaller than the view: centre
        return std::clamp(target - view / 2.0, 0.0, room - view);
    };
    const double tx = axis(t.x, view_w, pixel_w), ty = axis(t.y, view_h, pixel_h);
    if (camera_snap)
    {
        camera_x = tx;
        camera_y = ty;
        camera_snap = false;
    }
    else
    {
        const double k = 1.0 - std::exp(-10.0 * dt);   // ease towards the target
        camera_x += (tx - camera_x) * k;
        camera_y += (ty - camera_y) * k;
    }
}

void RoomView::Draw(SDL_Renderer* renderer)
{
    UpdateCamera(frame_dt);
    // Whole screen pixels so tiles do not shimmer while the camera eases.
    origin.x = std::round(viewport.x - static_cast<float>(camera_x) * scale);
    origin.y = std::round(viewport.y - static_cast<float>(camera_y) * scale);
    if (shake_t > 0.0)
    {
        // Screen shake: the amplitude dies away with the time left.
        const double k = shake_total > 0.0 ? shake_t / shake_total : 0.0;
        origin.x += static_cast<float>(std::sin(shake_t * 63.0) * shake_amp * k * scale);
        origin.y += static_cast<float>(std::cos(shake_t * 51.0) * shake_amp * k * scale);
    }

    SDL_SetRenderClipRect(renderer, &viewport);
    DrawWorld(renderer);
    DrawFriday(renderer);
    if (fx_level > 0.0)
    {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, fx_color.r, fx_color.g, fx_color.b, static_cast<Uint8>(std::min(1.0, fx_level) * 255.0));
        const SDL_FRect all = { static_cast<float>(viewport.x), static_cast<float>(viewport.y),
                                static_cast<float>(viewport.w), static_cast<float>(viewport.h) };
        SDL_RenderFillRect(renderer, &all);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }
    SDL_SetRenderClipRect(renderer, nullptr);
}

// Fills the square of half-size `half` centred on (wx, wy) with a translucent colour.
void RoomView::FillCell(SDL_Renderer* renderer, double wx, double wy, double half, float height_px,
                        SDL_FColor colour) const
{
    const SDL_FPoint n = ToScreen(static_cast<float>(wx - half), static_cast<float>(wy - half), height_px);
    const SDL_FPoint e = ToScreen(static_cast<float>(wx + half), static_cast<float>(wy - half), height_px);
    const SDL_FPoint s = ToScreen(static_cast<float>(wx + half), static_cast<float>(wy + half), height_px);
    const SDL_FPoint w = ToScreen(static_cast<float>(wx - half), static_cast<float>(wy + half), height_px);
    const SDL_Vertex v[4] = {
        { n, colour, { 0, 0 } }, { e, colour, { 0, 0 } }, { s, colour, { 0, 0 } }, { w, colour, { 0, 0 } } };
    const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    SDL_RenderGeometry(renderer, nullptr, v, 4, idx, 6);
}

// F3: everything that stops the player, drawn on the boundary of the walkable floor:
//  red     = edge towards a wall / void cell (heightmap bit 0x4000), with a translucent curtain
//  orange  = edge towards a cell closed to NPCs only (0x2000): enemies stop there, the player does not
//  magenta = step edge higher than the player can climb (more than STEP_UP)
//  yellow  = collision box of a solid entity
// The player's centre stays half a box (PLAYER_RADIUS) inside these lines.
void RoomView::DrawBlockers(SDL_Renderer* renderer) const
{
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    const SDL_FColor red = { 1.0f, 0.1f, 0.1f, 0.30f };
    const SDL_FColor yellow = { 1.0f, 0.9f, 0.0f, 0.45f };
    const float curtain_px = 20.0f;

    auto walkable = [this](int wx, int wy) { return IsWalkable(wx, wy); };

    // Boundary edges of the walkable floor, at the floor height of the walkable cell.
    struct Side { int dx, dy; double x0, y0, x1, y1; };   // offsets within the cell
    static const Side sides[4] = {
        { +1, 0, 1, 0, 1, 1 },   // towards +x
        { -1, 0, 0, 0, 0, 1 },   // towards -x
        { 0, +1, 0, 1, 1, 1 },   // towards +y
        { 0, -1, 0, 0, 1, 0 },   // towards -y
    };
    for (int hy = 0; hy < hm_height; ++hy)
    {
        for (int hx = 0; hx < hm_width; ++hx)
        {
            const int wx = hx + CELL_OFFSET, wy = hy + CELL_OFFSET;
            if (!walkable(wx, wy)) continue;
            const float base = static_cast<float>(FloorHeight(wx, wy) * HEIGHT_UNIT_PX);
            for (const auto& sd : sides)
            {
                const int nx = wx + sd.dx, ny = wy + sd.dy;
                const int ncell = GetCell(nx, ny);
                const bool wall = (ncell < 0) || (ncell & 0x4000) != 0;
                const bool npc_only = !wall && (ncell & 0x2000) != 0;
                if (!wall && !npc_only) continue;

                const SDL_FPoint a = ToScreen(static_cast<float>(wx + sd.x0), static_cast<float>(wy + sd.y0), base);
                const SDL_FPoint b = ToScreen(static_cast<float>(wx + sd.x1), static_cast<float>(wy + sd.y1), base);
                if (wall)
                {
                    const SDL_FPoint at = ToScreen(static_cast<float>(wx + sd.x0), static_cast<float>(wy + sd.y0), base + curtain_px);
                    const SDL_FPoint bt = ToScreen(static_cast<float>(wx + sd.x1), static_cast<float>(wy + sd.y1), base + curtain_px);
                    const SDL_Vertex v[4] = { { a, red, { 0, 0 } }, { b, red, { 0, 0 } },
                                              { bt, red, { 0, 0 } }, { at, red, { 0, 0 } } };
                    const int idx[6] = { 0, 1, 2, 0, 2, 3 };
                    SDL_RenderGeometry(renderer, nullptr, v, 4, idx, 6);
                    SDL_SetRenderDrawColor(renderer, 255, 40, 40, 255);
                }
                else
                {
                    SDL_SetRenderDrawColor(renderer, 255, 160, 0, 255);
                }
                for (int k = -1; k <= 1; ++k)   // 3 px thick
                    SDL_RenderLine(renderer, a.x, a.y + k, b.x, b.y + k);
            }
        }
    }

    // Steps too high to climb: draw the shared edge on the lower side, in magenta.
    SDL_SetRenderDrawColor(renderer, 255, 0, 255, 255);
    for (int hy = 0; hy < hm_height; ++hy)
    {
        for (int hx = 0; hx < hm_width; ++hx)
        {
            const int wx = hx + CELL_OFFSET, wy = hy + CELL_OFFSET;
            if (!walkable(wx, wy)) continue;
            const int h = FloorHeight(wx, wy);
            const int nbr[2][2] = { { 1, 0 }, { 0, 1 } };
            for (const auto& d : nbr)
            {
                const int nx = wx + d[0], ny = wy + d[1];
                if (!walkable(nx, ny)) continue;
                const int nh = FloorHeight(nx, ny);
                if (std::abs(nh - h) <= STEP_UP) continue;
                const float edge_h = static_cast<float>(std::max(h, nh) * HEIGHT_UNIT_PX);
                // The edge between (wx,wy) and its neighbour.
                const SDL_FPoint a = d[0] ? ToScreen(static_cast<float>(wx + 1), static_cast<float>(wy), edge_h)
                                          : ToScreen(static_cast<float>(wx), static_cast<float>(wy + 1), edge_h);
                const SDL_FPoint b = ToScreen(static_cast<float>(wx + 1), static_cast<float>(wy + 1), edge_h);
                SDL_RenderLine(renderer, a.x, a.y, b.x, b.y);
                SDL_RenderLine(renderer, a.x, a.y + 1, b.x, b.y + 1);
            }
        }
    }

    // Collision boxes of the solid entities.
    if (entity_table)
    {
        for (std::size_t i = 0; i < entities.size(); ++i)
        {
            const auto& e = entities[i];
            if (!e.solid || !e.visible || e.type >= 0xC0 || !entity_table->Has(e.type)) continue;
            if (game && game->IsCollected(room_number, static_cast<int>(i))) continue;
            const double half = entity_table->GetHitboxBase(e.type) / 16.0;
            FillCell(renderer, e.x, e.y, half, static_cast<float>(e.z * HEIGHT_UNIT_PX), yellow);
        }
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

void RoomView::DrawWorld(SDL_Renderer* renderer)
{
    DrawLayer(renderer, bg, 2);   // background layer is shifted by 2 grid units
    DrawLayer(renderer, fg, 0);
    DrawEntities(renderer);

    if (swing_time > 0.0 && show_overlay)
    {
        // Sword swing: a short bright arc in front of the player.
        static const double dir_x[4] = { -1, 0, 1, 0 };
        static const double dir_y[4] = { 0, -1, 0, 1 };
        const double hpx = player_z * HEIGHT_UNIT_PX + 16.0;
        const double fx = dir_x[facing], fy = dir_y[facing];
        const double px = -fy, py = fx;   // perpendicular
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        for (int k = -1; k <= 1; ++k)
        {
            const double off = k * 0.5;
            const SDL_FPoint a = ToScreen(static_cast<float>(player_x + fx * 0.5 + px * off),
                                          static_cast<float>(player_y + fy * 0.5 + py * off), static_cast<float>(hpx));
            const SDL_FPoint b = ToScreen(static_cast<float>(player_x + fx * 1.4 + px * off),
                                          static_cast<float>(player_y + fy * 1.4 + py * off), static_cast<float>(hpx));
            SDL_RenderLine(renderer, a.x, a.y, b.x, b.y);
        }
    }

    if (show_blocks || std::getenv("LS_BLOCKS"))
        DrawBlockers(renderer);

    if (show_grid || std::getenv("LS_GRID"))
    {
        // Every non-blocked heightmap cell, at its floor height.
        SDL_SetRenderDrawColor(renderer, 0, 255, 255, 255);
        for (int hy = 0; hy < hm_height; ++hy)
        {
            for (int hx = 0; hx < hm_width; ++hx)
            {
                const int wx = hx + CELL_OFFSET, wy = hy + CELL_OFFSET;
                if (!IsWalkable(wx, wy)) continue;
                DrawCell(renderer, wx, wy, static_cast<float>(FloorHeight(wx, wy) * HEIGHT_UNIT_PX));
            }
        }
    }

    if (!show_overlay) return;

    auto cell_height_px = [this](int wx, int wy) {
        const int c = GetCell(wx, wy);
        return c < 0 ? 0.0f : static_cast<float>(((c >> 8) & 0x0F) * HEIGHT_UNIT_PX);
    };

    // Warps: red outlines on each covered cell.
    SDL_SetRenderDrawColor(renderer, 255, 40, 40, 255);
    for (const auto& wp : warps)
        for (int yy = 0; yy < wp.h; ++yy)
            for (int xx = 0; xx < wp.w; ++xx)
                DrawCell(renderer, wp.x + xx, wp.y + yy, cell_height_px(wp.x + xx, wp.y + yy));

    // Cursor: yellow cell outline with a cross.
    SDL_SetRenderDrawColor(renderer, 255, 230, 0, 255);
    const float hpx = cell_height_px(cursor_x, cursor_y);
    DrawCell(renderer, cursor_x, cursor_y, hpx);
    const SDL_FPoint a = ToScreen(cursor_x + 0.0f, cursor_y + 0.0f, hpx);
    const SDL_FPoint b = ToScreen(cursor_x + 1.0f, cursor_y + 1.0f, hpx);
    const SDL_FPoint c = ToScreen(cursor_x + 1.0f, cursor_y + 0.0f, hpx);
    const SDL_FPoint d = ToScreen(cursor_x + 0.0f, cursor_y + 1.0f, hpx);
    SDL_RenderLine(renderer, a.x, a.y, b.x, b.y);
    SDL_RenderLine(renderer, c.x, c.y, d.x, d.y);
}
