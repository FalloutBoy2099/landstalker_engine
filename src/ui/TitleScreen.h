#ifndef _TITLE_SCREEN_H_
#define _TITLE_SCREEN_H_

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "core/IKeyInput.h"
#include "graphics/IDrawable.h"
#include "ui/TextBox.h"

// The title sequence: the Climax logo fades in and out, then the game's own title picture
// (assets/graphics/title.png, written by landstalker_tools/scripts/title_export.py) is wiped
// in from the left with a bright edge, and glints from time to time. The menu (New game /
// Load game / Quit, where the original says "PUSH START BUTTON") appears when the logo is in
// place. Any key skips ahead to the menu. Up / Down choose, Enter or Shift confirm.
class TitleScreen : public IDrawable, public IKeyInput
{
public:
    enum Choice { NEW_GAME = 0, LOAD_GAME = 1, QUIT = 2 };

    // viewport: the area to fill (the whole window); menu: a text box the screen positions
    // and fills with the menu lines. `climax` may be empty (no splash).
    TitleScreen(SDL_Renderer* renderer, const std::filesystem::path& picture, const std::filesystem::path& climax,
                SDL_Rect viewport, std::shared_ptr<TextBox> menu_box, bool can_load, std::function<void(Choice)> on_choice)
        : viewport(viewport), menu_box(std::move(menu_box)), on_choice(std::move(on_choice))
    {
        texture = Load(renderer, picture, pic_w, pic_h);
        if (!texture) throw std::runtime_error(std::string("Unable to load ") + picture.string() + ": " + SDL_GetError());
        if (!climax.empty()) climax_texture = Load(renderer, climax, climax_w, climax_h);
        if (!climax_texture) time = CLIMAX_END;   // no splash: straight to the title

        items = { { "NEW GAME", true }, { "LOAD GAME", can_load }, { "QUIT", true } };
        Refresh();
        // Debug aid: LS_TITLE_T=seconds starts the sequence that far in (screenshots).
        if (const char* t = std::getenv("LS_TITLE_T")) time = std::atof(t);
    }

    ~TitleScreen() override
    {
        if (texture) SDL_DestroyTexture(texture);
        if (climax_texture) SDL_DestroyTexture(climax_texture);
    }

    void Draw(SDL_Renderer* renderer) override
    {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        const SDL_FRect all = { static_cast<float>(viewport.x), static_cast<float>(viewport.y),
                                static_cast<float>(viewport.w), static_cast<float>(viewport.h) };
        SDL_RenderFillRect(renderer, &all);

        if (time < CLIMAX_END)
        {
            // Climax logo: fade in, hold, fade out.
            double a = 1.0;
            if (time < 0.7) a = time / 0.7;
            else if (time > CLIMAX_END - 0.7) a = (CLIMAX_END - time) / 0.7;
            Place(climax_w, climax_h, 4);
            SDL_SetTextureAlphaMod(climax_texture, static_cast<Uint8>(std::clamp(a, 0.0, 1.0) * 255.0));
            const SDL_FRect dst = { static_cast<float>(px), static_cast<float>(py), static_cast<float>(pw), static_cast<float>(ph) };
            SDL_RenderTexture(renderer, climax_texture, nullptr, &dst);
            return;
        }

        const int scale = std::max(1, std::min(viewport.w / pic_w, viewport.h / pic_h));
        Place(pic_w, pic_h, scale);
        const double since = time - CLIMAX_END;
        const double reveal = std::min(1.0, since / REVEAL_TIME);
        const float edge = static_cast<float>(px + pw * reveal);
        SDL_FRect dst = { static_cast<float>(px), static_cast<float>(py), static_cast<float>(pw), static_cast<float>(ph) };

        // The part of the picture the wipe has reached.
        SDL_Rect clip = { px, py, static_cast<int>(std::lround(edge - px)), ph };
        if (clip.w > 0)
        {
            SDL_SetRenderClipRect(renderer, &clip);
            SDL_RenderTexture(renderer, texture, nullptr, &dst);
            SDL_SetRenderClipRect(renderer, nullptr);
        }
        // A bright edge on the front of the wipe, then a glint crossing the logo every few seconds.
        if (reveal < 1.0) Band(renderer, dst, edge - 36.0f * scale, 36.0f * scale, 200, px, edge);
        else
        {
            const double cycle = std::fmod(since - REVEAL_TIME, GLINT_PERIOD);
            if (cycle < GLINT_TIME)
            {
                const double k = cycle / GLINT_TIME;
                const float x = static_cast<float>(px - 50.0f * scale + (pw + 100.0f * scale) * k);
                Band(renderer, dst, x, 50.0f * scale, 170, px, px + pw);
            }
        }
        if (Ready() && menu_box) menu_box->Draw(renderer);
    }

    void OnUpdate(double delta) override { time += delta; }
    void OnTick(unsigned int) override {}

    void OnKeyDown(SDL_Scancode scancode, SDL_Keymod) override
    {
        if (!Ready())
        {
            // Any key skips the splash and the wipe.
            time = CLIMAX_END + REVEAL_TIME + 0.2;
            return;
        }
        switch (scancode)
        {
        case SDL_SCANCODE_UP:   Move(-1); break;
        case SDL_SCANCODE_DOWN: Move(1); break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT:
        case SDL_SCANCODE_SPACE:
            if (items[selected].enabled && on_choice) on_choice(static_cast<Choice>(selected));
            break;
        default: break;
        }
    }
    void OnKeyUp(SDL_Scancode, SDL_Keymod) override {}

    bool Ready() const { return time >= CLIMAX_END + REVEAL_TIME; }

private:
    struct Item { const char* label; bool enabled; };

    static constexpr double CLIMAX_END = 3.8;      // s the splash lasts
    static constexpr double REVEAL_TIME = 2.0;     // s the wipe takes
    static constexpr double GLINT_PERIOD = 4.5;    // s between two glints
    static constexpr double GLINT_TIME = 0.9;      // s a glint takes to cross

    static SDL_Texture* Load(SDL_Renderer* renderer, const std::filesystem::path& file, int& w, int& h)
    {
        SDL_Surface* surface = SDL_LoadPNG(file.string().c_str());
        if (!surface) return nullptr;
        w = surface->w;
        h = surface->h;
        SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, surface);
        SDL_DestroySurface(surface);
        if (t)
        {
            SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        }
        return t;
    }

    // Where a w x h picture goes at this scale: centred in the viewport.
    void Place(int w, int h, int scale)
    {
        pw = w * scale;
        ph = h * scale;
        px = viewport.x + (viewport.w - pw) / 2;
        py = viewport.y + (viewport.h - ph) / 2;
    }

    // Brightens the picture inside a vertical band by drawing it again additively.
    void Band(SDL_Renderer* renderer, const SDL_FRect& dst, float x, float width, Uint8 alpha, float min_x, float max_x)
    {
        const float x0 = std::max(x, min_x), x1 = std::min(x + width, max_x);
        if (x1 <= x0) return;
        const SDL_Rect clip = { static_cast<int>(x0), static_cast<int>(dst.y), static_cast<int>(x1 - x0), static_cast<int>(dst.h) };
        SDL_SetRenderClipRect(renderer, &clip);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_ADD);
        SDL_SetTextureAlphaMod(texture, alpha);
        SDL_RenderTexture(renderer, texture, nullptr, &dst);
        SDL_SetTextureAlphaMod(texture, 255);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetRenderClipRect(renderer, nullptr);
    }

    void Move(int step)
    {
        for (int tries = 0; tries < static_cast<int>(items.size()); ++tries)
        {
            selected = (selected + step + static_cast<int>(items.size())) % static_cast<int>(items.size());
            if (items[selected].enabled) break;
        }
        Refresh();
    }

    void Refresh()
    {
        std::string text;
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            text += (static_cast<int>(i) == selected ? "> " : "  ");
            text += items[i].label;
            if (!items[i].enabled) text += " (NO SAVE)";
            if (i + 1 < items.size()) text += "\n";
        }
        if (menu_box) menu_box->SetLabelText(text);
    }

    SDL_Rect viewport;
    std::shared_ptr<TextBox> menu_box;
    std::function<void(Choice)> on_choice;
    SDL_Texture* texture = nullptr;
    SDL_Texture* climax_texture = nullptr;
    int pic_w = 1, pic_h = 1, climax_w = 1, climax_h = 1;
    int px = 0, py = 0, pw = 0, ph = 0;
    double time = 0.0;
    std::vector<Item> items;
    int selected = 0;
};

#endif // _TITLE_SCREEN_H_
