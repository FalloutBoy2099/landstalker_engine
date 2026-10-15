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
    LoadRoom(room_number);
}

RoomView::~RoomView()
{
    FreeEntityImages();
    if (blocks) SDL_DestroyTexture(blocks);
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
    auto raw = LoadRawWarps(dir / "room.yaml");
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
    SDL_Texture* tex = SDL_CreateTextureFromSurface(graphics->GetRenderer(), surface);
    SDL_DestroySurface(surface);
    if (!tex) throw std::runtime_error(std::string("Unable to create texture: ") + SDL_GetError());
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    // Everything loaded: commit.
    if (blocks) SDL_DestroyTexture(blocks);
    blocks = tex;
    room_number = number;
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
    dialogue_asks = false;
    dialogue_yes = dialogue_no = nullptr;
    if (dialogue_cb) dialogue_cb("", false);
    chests_enabled = new_chests_enabled;

    // A warp entry links (room1, x1,y1) <-> (room2, x2,y2); keep the ends that are in this room.
    warps.clear();
    for (const auto& r : raw)
    {
        if (r.room1 == number)
            warps.push_back({ r.x1, r.y1, r.x_size, r.y_size, r.room2, r.x2, r.y2, r.type });
        if (r.room2 == number)
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
    player_z = FloorHeight(cursor_x, cursor_y);
    vertical_speed = 0.0;
    grounded = true;
    moving = false;
    anim_time = 0.0;
    warp_armed = false;   // re-armed once the cursor leaves every warp
    CheckWarps();
    UpdateStatus();
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
            RefreshMenu();
            break;
        case SDL_SCANCODE_DOWN:
            if (!menu_items.empty()) menu_index = (menu_index + 1) % static_cast<int>(menu_items.size());
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
    case SDL_SCANCODE_Y: AnswerDialogue(true); break;
    case SDL_SCANCODE_N: AnswerDialogue(false); break;
    case SDL_SCANCODE_R:
        if (dead && game)
        {
            game->Revive();
            dead = false;
            death_time = 0.0;
            hurt_time = 0.0;
            invulnerable_time = HIT_INVULNERABILITY;
            LoadRoom(room_number);
            ShowMessage("Back on your feet");
        }
        break;
    case SDL_SCANCODE_E:
        if (game)
        {
            if (game->UseEkeEke()) ShowMessage("Used an EkeEke");
            else if (game->ItemCount(GameState::ITEM_EKEEKE) <= 0) ShowMessage("No EkeEke left");
            else ShowMessage("Health is already full");
        }
        break;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:
        Interact();
        break;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:
        if (grounded)
        {
            vertical_speed = JUMP_SPEED;
            grounded = false;
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
            z < top && z + PLAYER_HEIGHT > e.z)
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
    return e.hostile && e.visible && entity_table->GetEnemy(e.type) != nullptr &&
           !game->IsCollected(room_number, static_cast<int>(index));
}

void RoomView::Die()
{
    dead = true;
    death_time = 0.0;
    ShowMessage("GAME OVER - press R to restart the room");
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
    attack_cooldown = ATTACK_COOLDOWN;
    swing_time = 0.15;
    swinging = true;
    swing_hit_pending = true;
    swing_anim = 0.0;
    swing_facing = facing;
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

        int& hp = game->EnemyHealth(room_number, static_cast<int>(i), st->health);
        const int damage = std::max(1, game->Attack() - st->defence);
        hp -= damage;
        if (hp <= 0)
        {
            game->MarkCollected(room_number, static_cast<int>(i));
            OnEntityGone(i);
            SpawnSparks(e.x, e.y, e.z + 1.0, true, false);
            std::string text = "Enemy defeated";
            if (st->gold_drop > 0)
            {
                game->AddGold(st->gold_drop);
                text += " (+" + std::to_string(st->gold_drop) + " gold)";
            }
            ShowMessage(text);
        }
        else
        {
            e.hit_flash = HIT_FLASH_TIME;
            // Knockback: slide the enemy away along the blow while the way is free. Big
            // enemies (large hitbox) are pushed back less, bosses hardly at all.
            const int base = entity_table->GetHitboxBase(e.type);
            double knock = KNOCKBACK * 8.0 / std::max(8, base);
            if (base >= 16) knock *= 0.5;
            SpawnSparks(e.x, e.y, e.z + 1.0, false, false);
            for (double moved = 0.0; moved < knock; moved += 0.1)
            {
                const double nx = e.x + fx * 0.1, ny = e.y + fy * 0.1;
                if (!CanEnemyStandAt(i, nx, ny, e.z)) break;
                e.x = nx;
                e.y = ny;
            }
            ShowMessage("Hit! " + std::to_string(hp) + " HP left");
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

// Simple chase AI: an enemy with a speed walks straight at the player once he is close
// enough, sliding along walls, and faces the way it walks. This is a generic stand-in for
// the per-enemy behaviour scripts of the original game.
void RoomView::UpdateEnemies(double dt)
{
    if (dead || !game || !entity_table) return;
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        if (!IsAliveEnemy(i)) continue;
        auto& e = entities[i];
        if (e.speed <= 0 || e.hit_flash > 0.0) continue;

        const double dx = player_x - e.x, dy = player_y - e.y;
        const double dist = std::sqrt(dx * dx + dy * dy);
        if (dist > AGGRO_RADIUS || dist < 0.05) { e.walking = false; e.walk_time = 0.0; continue; }

        // Like the original, walk along one grid axis at a time (no diagonals): the axis
        // with the larger remaining distance, kept until the other one is clearly larger
        // so the enemy does not zig-zag. If that way is blocked, try the other axis.
        const double step = (ENEMY_BASE_SPEED + 0.6 * (e.speed - 1)) * dt;
        const double adx = std::abs(dx), ady = std::abs(dy);
        constexpr double HYSTERESIS = 0.4;
        int axis = e.move_axis;
        if (axis == 0 && ady > adx + HYSTERESIS) axis = 1;
        else if (axis == 1 && adx > ady + HYSTERESIS) axis = 0;

        auto try_axis = [&](int a) {
            const double d = (a == 0) ? dx : dy;
            if (std::abs(d) < 0.02) return false;
            const double move = std::copysign(std::min(step, std::abs(d)), d);
            if (a == 0 && CanEnemyStandAt(i, e.x + move, e.y, e.z)) { e.x += move; return true; }
            if (a == 1 && CanEnemyStandAt(i, e.x, e.y + move, e.z)) { e.y += move; return true; }
            return false;
        };
        int moved_axis = -1;
        if (try_axis(axis)) moved_axis = axis;
        else if (try_axis(1 - axis)) moved_axis = 1 - axis;

        static const bool log_ai = std::getenv("LS_LOG_AI") != nullptr;
        if (log_ai)
            SDL_Log("AI enemy %d type %d at (%.3f, %.3f) axis %d", static_cast<int>(i), e.type, e.x, e.y, moved_axis);
        e.walking = moved_axis >= 0;
        e.walk_time = e.walking ? e.walk_time + dt : 0.0;
        if (moved_axis >= 0)
        {
            e.move_axis = moved_axis;
            // 0=NE 1=SE 2=SW 3=NW
            if (moved_axis == 0) e.orientation = dx > 0 ? 1 : 3;
            else e.orientation = dy > 0 ? 2 : 0;
        }
        if (e.gravity)
            e.z = FloorHeight(static_cast<int>(std::floor(e.x)), static_cast<int>(std::floor(e.y)));
    }
}

// Timers, enemy movement, and contact damage from enemies touching the player.
void RoomView::UpdateCombat(double dt)
{
    UpdateChestOpening(dt);
    for (auto& sp : sparks) sp.t += dt;
    sparks.erase(std::remove_if(sparks.begin(), sparks.end(),
                                [](const Spark& sp) { return sp.t >= SPARK_TIME; }), sparks.end());
    UpdateEnemies(dt);
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
    if (dead) death_time += dt;
    if (dead || invulnerable_time > 0.0 || !game || !entity_table) return;

    const double margin = 0.1;   // enemies are solid: touching means being a hair away
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        if (!IsAliveEnemy(i)) continue;
        const auto& e = entities[i];
        const auto* st = entity_table->GetEnemy(e.type);
        const double half = entity_table->GetHitboxBase(e.type) / 16.0;
        const double top = e.z + entity_table->GetHitboxHeight(e.type) / 16.0;
        if (std::abs(player_x - e.x) < half + PLAYER_RADIUS + margin &&
            std::abs(player_y - e.y) < half + PLAYER_RADIUS + margin &&
            player_z < top && player_z + PLAYER_HEIGHT > e.z)
        {
            invulnerable_time = HIT_INVULNERABILITY;
            hurt_time = HURT_POSE_TIME;
            {
                // Thrown away from the enemy along the axis that separates them most.
                const double ax = player_x - e.x, ay = player_y - e.y;
                if (std::abs(ax) >= std::abs(ay)) { knock_vx = (ax >= 0 ? 1 : -1) * PLAYER_KNOCK_SPEED; knock_vy = 0.0; }
                else { knock_vy = (ay >= 0 ? 1 : -1) * PLAYER_KNOCK_SPEED; knock_vx = 0.0; }
                knock_time = PLAYER_KNOCK_TIME;
                SpawnSparks(player_x, player_y, player_z + 1.0, false, true);
            }
            const int dmg = std::max(1, st->attack - game->Defence());
            const bool killed = game->Damage(dmg);
            if (killed) Die();
            else ShowMessage("Ouch! -" + std::to_string(dmg) + " HP");
            return;
        }
    }
}

void RoomView::ShowMessage(const std::string& text)
{
    message_time = 4.0;
    if (message_cb) message_cb(text);
}

// Entity types 0xC0 and above are items lying on the floor (type - 0xC0 = item index).
// Touching one puts it in the inventory.
void RoomView::CheckPickups()
{
    if (!game || !entity_table) return;
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
            std::string text = "Got " + game->ItemName(item);
            if (game->ItemCount(item) > 0) text += " (x" + std::to_string(game->ItemCount(item)) + ")";
            else if (item == GameState::ITEM_LIFE_STOCK) text += ": max health up";
            ShowMessage(text);
        }
    }
}

void RoomView::GiveChestContent(int value)
{
    if (value >= 0x40)
    {
        game->AddGold(value - 0x40);
        ShowMessage("Found " + std::to_string(value - 0x40) + " gold");
    }
    else
    {
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
                    ShowText("You can't afford that now.\nMake more golds somewhere!");
                    return;
                }
                game->AddItem(item);
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
                ShowText("You can't afford that now.\nMake more golds somewhere!");
                return;
            }
            game->HealFull();
            ShowText("Good morning!\nYou feel fully rested.");
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
    RefreshMenu();
}

void RoomView::CloseMenu()
{
    menu_open = false;
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
            if (item == game->EquippedSword() || item == game->EquippedArmour()) text += "  (on)";
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
    if (item == GameState::ITEM_EKEEKE)
    {
        if (game->UseEkeEke()) ShowMessage("Used an EkeEke");
        else ShowMessage("Health is already full");
    }
    else if (item == GameState::ITEM_KEY)
    {
        CloseMenu();
        UseKey();
        return;
    }
    else if (GameState::IsSword(item) || GameState::IsArmour(item))
    {
        const bool on = game->ToggleEquip(item);
        ShowMessage(std::string(on ? "Equipped " : "Removed ") + game->ItemName(item));
    }
    else
    {
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
        ShowMessage("Nothing to unlock here.");
        return;
    }
    if (game->ItemCount(GameState::ITEM_KEY) <= 0)
    {
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
        ShowMessage(flag < 0 ? "The Key does not fit here." : "This door is already open.");
        return;
    }
    game->UseItem(GameState::ITEM_KEY);
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
        ApplyFlagRules();
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
    const double floor_h = FloorUnder(player_x, player_y, player_z);
    if (grounded)
    {
        if (floor_h > player_z)
            player_z = std::min(floor_h, player_z + CLIMB_SPEED * dt);
        else if (floor_h < player_z - 1e-6)
        {
            grounded = false;          // walked off an edge
            vertical_speed = 0.0;
        }
        else
            player_z = floor_h;
    }
    if (!grounded)
    {
        vertical_speed = std::max(-MAX_FALL_SPEED, vertical_speed - GRAVITY * dt);
        player_z += vertical_speed * dt;
        if (player_z <= floor_h && vertical_speed <= 0.0)
        {
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
    if (dead || swinging || chest_open.active || knock_time > 0.0 || !dialogue_pages.empty())
        dir_x = dir_y = 0;
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
    CheckPickups();
    CheckSwitches();
    UpdateCombat(dt);
    anim_time = moving ? anim_time + dt : 0.0;

    const int cx = static_cast<int>(std::floor(player_x)), cy = static_cast<int>(std::floor(player_y));
    if (cx != cursor_x || cy != cursor_y)
    {
        cursor_x = cx;
        cursor_y = cy;
        CheckWarps();   // may load another room and reposition the player
    }
    if (moving) blocked_text.clear();   // it moved on at least one axis: not stuck
    if (moving || was_moving || !grounded || blocked_text != shown_blocked_text) UpdateStatus();
}

void RoomView::SetPlayerPosition(double x, double y)
{
    player_x = x;
    player_y = y;
    cursor_x = static_cast<int>(std::floor(x));
    cursor_y = static_cast<int>(std::floor(y));
    player_z = FloorHeight(cursor_x, cursor_y);
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
    if (menu_open) return;   // the game is paused behind the menu
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
    // No diagonals: with one key on each axis, keep the one pressed last.
    if (dx != 0 && dy != 0)
    {
        if (last_key_was_x_axis) dy = 0;
        else dx = 0;
    }
    Step(dx, dy, delta);
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
    if (!RoomExists(hit->dest_room)) return;
    const SDL_Point arrival{ hit->dest_x, hit->dest_y };
    try
    {
        LoadRoom(hit->dest_room, &arrival);
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

void RoomView::DrawLayer(SDL_Renderer* renderer, const std::vector<int>& layer, int layer_offset_tiles) const
{
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
            SDL_RenderTexture(renderer, blocks, &src, &dst);
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
            if (game && game->IsCollected(room_number, static_cast<int>(ei))) continue;
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
                        [this, renderer, frame, cp]() { chest_sprite->Draw(renderer, frame, cp.x, cp.y, scale, false); } });

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
            items.push_back({ e.x + e.y + base / 256.0, e.y - base / 256.0, e.z + hbh / 256.0,
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
    if (player && !blink_off)
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
            items.push_back({ px + py + 8 / 256.0, py - 8 / 256.0, z,
                [this, renderer, frame, flip, p]() { player->Draw(renderer, frame, p.x, p.y, scale, flip); } });
        }
    }

    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.dist != b.dist) return a.dist < b.dist;
        if (a.left != b.left) return a.left < b.left;
        return a.top < b.top;
    });
    for (auto& it : items) it.draw();
}

void RoomView::SetScale(float new_scale)
{
    scale = new_scale;
    camera_snap = true;
}

SDL_FPoint RoomView::CameraTarget() const
{
    // Same projection as ToScreen(), in unscaled room pixels, aimed at the player's torso.
    const double xx = player_x + GRID_SHIFT_X - hm_left, yy = player_y + GRID_SHIFT_Y - hm_top;
    const double px = ((xx - yy + (height - 1)) * 2 + hm_left) * TILE_PX;
    const double py = (xx + yy + hm_top) * TILE_PX - player_z * HEIGHT_UNIT_PX - 16.0;
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

    SDL_SetRenderClipRect(renderer, &viewport);
    DrawWorld(renderer);
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
