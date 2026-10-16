#include <tuple>
#include "sprite/EntityTable.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

EntityTable::EntityTable(std::shared_ptr<Graphics> graphics, const std::filesystem::path& sprites_dir)
    : graphics(std::move(graphics)), dir(sprites_dir)
{
    std::ifstream f(dir / "entities.yaml");
    if (!f) throw std::runtime_error("Unable to open " + (dir / "entities.yaml").string());

    // Lines look like:   12: {sprite: 5, hitbox_base: 8, default_file: "entities/x.png", ...}
    std::string line;
    while (std::getline(f, line))
    {
        if (line.size() < 4 || line[0] != ' ') continue;
        const auto colon = line.find(':');
        const auto open = line.find('{'), close = line.rfind('}');
        if (colon == std::string::npos || open == std::string::npos || close == std::string::npos) continue;

        int id = 0;
        try { id = std::stoi(line.substr(0, colon)); } catch (const std::exception&) { continue; }

        std::unordered_map<std::string, std::string> kv;
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
            if (val.size() >= 2 && val.front() == '"') val = val.substr(1, val.size() - 2);
            kv[key] = val;
        }
        auto geti = [&](const std::string& k) { auto it = kv.find(k); return it == kv.end() ? 0 : std::stoi(it->second); };
        auto gets = [&](const std::string& k) { auto it = kv.find(k); return it == kv.end() ? std::string() : it->second; };

        Def d;
        d.sprite = geti("sprite");
        d.front_and_back = gets("has_front_and_back") == "true";
        d.is_enemy = gets("enemy") == "true" && geti("health") > 0;
        d.enemy = { geti("health"), geti("defence"), geti("attack"), geti("gold_drop") };
        d.base = geti("hitbox_base");
        d.height = geti("hitbox_height");
        auto fill = [&](Def::Slot& s, const std::string& p) {
            s.file = gets(p + "_file");
            s.w = geti(p + "_w"); s.h = geti(p + "_h");
            s.ax = geti(p + "_ax"); s.ay = geti(p + "_ay");
        };
        // "file|w|h|ax|ay;file|w|h|ax|ay;..."
        auto parse_walk = [&](const std::string& key, std::vector<Def::Slot>& out) {
            const std::string v = gets(key);
            std::size_t pos = 0;
            while (pos < v.size())
            {
                const auto end = v.find(';', pos);
                const std::string item = v.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                std::vector<std::string> f;
                std::size_t q = 0;
                while (q <= item.size())
                {
                    const auto bar = item.find('|', q);
                    f.push_back(item.substr(q, bar == std::string::npos ? std::string::npos : bar - q));
                    if (bar == std::string::npos) break;
                    q = bar + 1;
                }
                if (f.size() == 5)
                {
                    Def::Slot s;
                    s.file = f[0];
                    s.w = std::stoi(f[1]); s.h = std::stoi(f[2]);
                    s.ax = std::stoi(f[3]); s.ay = std::stoi(f[4]);
                    out.push_back(std::move(s));
                }
                if (end == std::string::npos) break;
                pos = end + 1;
            }
        };
        parse_walk("walk_back", d.walk_back);
        parse_walk("walk_front", d.walk_front);
        parse_walk("attack_back", d.attack_back);
        parse_walk("attack_front", d.attack_front);
        parse_walk("hurt_back", d.hurt_back);
        parse_walk("hurt_front", d.hurt_front);
        fill(d.def, "default");
        fill(d.back, "back");
        fill(d.front, "front");
        defs[id] = std::move(d);
    }
}

EntityTable::~EntityTable()
{
    for (auto& [_, d] : defs)
    {
        for (auto* s : { &d.def, &d.back, &d.front })
            if (s->image.texture) SDL_DestroyTexture(s->image.texture);
        for (auto* list : { &d.walk_back, &d.walk_front, &d.attack_back, &d.attack_front, &d.hurt_back, &d.hurt_front })
            for (auto& s : *list)
                if (s.image.texture) SDL_DestroyTexture(s.image.texture);
    }
}

const std::vector<EntityTable::Image>& EntityTable::GetWalkFrames(int entity, Kind kind)
{
    return GetPoseFrames(entity, WALK, kind);
}

const std::vector<EntityTable::Image>& EntityTable::GetPoseFrames(int entity, Pose pose, Kind kind)
{
    static const std::vector<Image> none;
    static std::map<std::tuple<int, int, int>, std::vector<Image>> cache;
    auto it = defs.find(entity);
    if (it == defs.end() || kind == DEFAULT) return none;
    Def& d = it->second;
    auto& slots = pose == WALK ? (kind == BACK ? d.walk_back : d.walk_front)
                : pose == ATTACK ? (kind == BACK ? d.attack_back : d.attack_front)
                                 : (kind == BACK ? d.hurt_back : d.hurt_front);
    if (slots.empty()) return none;
    auto& out = cache[{ entity, static_cast<int>(pose), static_cast<int>(kind) }];
    if (out.size() != slots.size())
    {
        out.clear();
        for (auto& slot : slots)
        {
            const Image* img = Load(slot);
            if (!img) { out.clear(); break; }
            out.push_back(*img);
        }
    }
    return out.empty() ? none : out;
}

int EntityTable::GetHitboxBase(int entity) const
{
    auto it = defs.find(entity);
    return it == defs.end() ? 0 : it->second.base;
}

int EntityTable::GetSprite(int entity) const
{
    auto it = defs.find(entity);
    return it == defs.end() ? -1 : it->second.sprite;
}

int EntityTable::GetHitboxHeight(int entity) const
{
    auto it = defs.find(entity);
    return it == defs.end() ? 0 : it->second.height;
}

const EntityTable::Image* EntityTable::Load(Def::Slot& slot)
{
    if (slot.loaded) return slot.image.texture ? &slot.image : nullptr;
    slot.loaded = true;
    if (slot.file.empty()) return nullptr;
    SDL_Surface* surface = SDL_LoadPNG((dir / slot.file).string().c_str());
    if (!surface)
    {
        SDL_Log("Unable to load %s: %s", slot.file.c_str(), SDL_GetError());
        return nullptr;
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(graphics->GetRenderer(), surface);
    SDL_DestroySurface(surface);
    if (!tex) return nullptr;
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    slot.image = { tex, slot.w, slot.h, slot.ax, slot.ay };
    return &slot.image;
}

EntityTable::Kind EntityTable::GetKind(int entity, int orientation, bool& hflip) const
{
    hflip = false;
    auto it = defs.find(entity);
    if (it == defs.end() || !it->second.front_and_back) return DEFAULT;
    // Same rule as the editor: SW/SE show the front, mirrored for NW and SE.
    hflip = (orientation == 3 || orientation == 1);
    return (orientation == 1 || orientation == 2) ? FRONT : BACK;
}

const EntityTable::Image* EntityTable::GetImage(int entity, int orientation, bool& hflip)
{
    auto it = defs.find(entity);
    if (it == defs.end()) { hflip = false; return nullptr; }
    Def& d = it->second;
    const Kind kind = GetKind(entity, orientation, hflip);
    if (kind == DEFAULT) return Load(d.def);
    const Image* img = Load(kind == FRONT ? d.front : d.back);
    return img ? img : Load(d.def);
}
