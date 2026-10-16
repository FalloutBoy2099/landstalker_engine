#include "sprite/SpriteSet.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace
{
// All "double quoted" strings of a line, in order.
std::vector<std::string> QuotedStrings(const std::string& line)
{
    std::vector<std::string> out;
    std::size_t pos = 0;
    while ((pos = line.find('"', pos)) != std::string::npos)
    {
        const auto end = line.find('"', pos + 1);
        if (end == std::string::npos) break;
        out.push_back(line.substr(pos + 1, end - pos - 1));
        pos = end + 1;
    }
    return out;
}

// "key: value" pairs of a {...} flow mapping; quotes are stripped from values.
std::unordered_map<std::string, std::string> FlowMapping(const std::string& line)
{
    std::unordered_map<std::string, std::string> kv;
    const auto open = line.find('{'), close = line.rfind('}');
    if (open == std::string::npos || close == std::string::npos) return kv;
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
        if (!val.empty() && val.front() == '"') val = val.substr(1, val.size() - 2);
        kv[key] = val;
    }
    return kv;
}
}

SpriteSet::SpriteSet(std::shared_ptr<Graphics> graphics, const std::filesystem::path& sprite_dir)
{
    std::ifstream f(sprite_dir / "sprite.yaml");
    if (!f) throw std::runtime_error("Unable to open " + (sprite_dir / "sprite.yaml").string());

    std::string line, section;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#') continue;
        if (line[0] != ' ')
        {
            section = line.substr(0, line.find(':'));
            continue;
        }
        const auto first = line.find_first_not_of(' ');
        if (section == "sprite")
        {
            if (line.compare(first, 3, "id:") == 0) id = std::stoi(line.substr(first + 3));
            else if (line.compare(first, 5, "name:") == 0)
            {
                const auto q = QuotedStrings(line);
                if (!q.empty()) name = q[0];
            }
        }
        else if (section == "frames")
        {
            const auto q = QuotedStrings(line);
            const auto kv = FlowMapping(line);
            if (q.empty() || !kv.count("file")) continue;
            SDL_Surface* surface = SDL_LoadPNG((sprite_dir / kv.at("file")).string().c_str());
            if (!surface) throw std::runtime_error("Unable to load " + kv.at("file") + ": " + SDL_GetError());
            Frame fr;
            fr.texture = SDL_CreateTextureFromSurface(graphics->GetRenderer(), surface);
            SDL_DestroySurface(surface);
            if (!fr.texture) throw std::runtime_error(std::string("Unable to create texture: ") + SDL_GetError());
            SDL_SetTextureScaleMode(fr.texture, SDL_SCALEMODE_NEAREST);
            SDL_SetTextureBlendMode(fr.texture, SDL_BLENDMODE_BLEND);
            fr.width = std::stoi(kv.at("width"));
            fr.height = std::stoi(kv.at("height"));
            fr.anchor_x = std::stoi(kv.at("anchor_x"));
            fr.anchor_y = std::stoi(kv.at("anchor_y"));
            frames[q[0]] = fr;
        }
        else if (section == "animations")
        {
            auto q = QuotedStrings(line);
            if (q.empty()) continue;
            const std::string anim = q.front();
            q.erase(q.begin());
            animations[anim] = std::move(q);
            animation_order.push_back(anim);
        }
    }
}

SpriteSet::~SpriteSet()
{
    for (auto& [_, fr] : frames)
        if (fr.texture) SDL_DestroyTexture(fr.texture);
}

const std::vector<std::string>& SpriteSet::GetAnimation(const std::string& anim) const
{
    static const std::vector<std::string> empty;
    auto it = animations.find(anim);
    return it == animations.end() ? empty : it->second;
}

const std::vector<std::string>& SpriteSet::GetAnimation(std::size_t index) const
{
    static const std::vector<std::string> empty;
    return index < animation_order.size() ? GetAnimation(animation_order[index]) : empty;
}

const SpriteSet::Frame* SpriteSet::GetFrame(const std::string& frame) const
{
    auto it = frames.find(frame);
    return it == frames.end() ? nullptr : &it->second;
}

void SpriteSet::Draw(SDL_Renderer* renderer, const std::string& frame, float x, float y,
                     float scale, bool hflip, SDL_Color tint) const
{
    const Frame* fr = GetFrame(frame);
    if (!fr) return;
    // When mirrored, the anchor moves to the opposite side of the image.
    const float ax = hflip ? (fr->width - fr->anchor_x) : fr->anchor_x;
    const SDL_FRect dst = { x - ax * scale, y - fr->anchor_y * scale,
                            fr->width * scale, fr->height * scale };
    const bool tinted = tint.r != 255 || tint.g != 255 || tint.b != 255;
    if (tinted) SDL_SetTextureColorMod(fr->texture, tint.r, tint.g, tint.b);
    SDL_RenderTextureRotated(renderer, fr->texture, nullptr, &dst, 0.0, nullptr,
                             hflip ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
    if (tinted) SDL_SetTextureColorMod(fr->texture, 255, 255, 255);
}
