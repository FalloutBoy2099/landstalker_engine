#ifndef _CREDITS_SCREEN_H_
#define _CREDITS_SCREEN_H_

#include <SDL3/SDL.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "core/IKeyInput.h"
#include "graphics/IDrawable.h"
#include "text/Font.h"
#include "text/TextLabel.h"

// The end credits: the game's own list (assets/text/credits.txt, written by
// landstalker_tools/scripts/credits_export.py) scrolling up over a black screen. Names come in
// two columns where the original has them. When "THE END" has reached the middle the scroll
// stops; after a few seconds (or Enter / Space / Escape once the credits have run for a
// while) it calls on_done.
class CreditsScreen : public IDrawable, public IKeyInput
{
public:
    CreditsScreen(SDL_Rect viewport, const std::filesystem::path& list, std::shared_ptr<Font> font, float scale,
                  std::function<void()> on_done)
        : viewport(viewport), on_done(std::move(on_done))
    {
        std::ifstream f(list);
        std::string line;
        double y = 0.0;
        const double unit = 16.0 * scale;   // the original scrolls in units of 16 pixels
        while (std::getline(f, line))
        {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ls(line);
            int advance = 0;
            char align = 'C';
            if (!(ls >> advance >> align)) continue;
            std::string text;
            std::getline(ls, text);
            if (!text.empty() && text[0] == ' ') text.erase(0, 1);
            y += advance * unit;
            Line l;
            l.y = y;
            l.align = align;
            l.text = text;
            const unsigned width = font->GetExtent(text, scale).first;
            unsigned x = 0;
            if (align == 'C') x = (viewport.w > static_cast<int>(width)) ? (viewport.w - width) / 2 : 0;
            else if (align == 'L') x = static_cast<unsigned>(viewport.w * 0.2);
            else x = static_cast<unsigned>(viewport.w * 0.55);
            l.label = std::make_shared<TextLabel>(font, x, 0, text, scale);
            lines.push_back(std::move(l));
        }
        total = y;
    }

    void Draw(SDL_Renderer* renderer) override
    {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        const SDL_FRect all = { static_cast<float>(viewport.x), static_cast<float>(viewport.y),
                                static_cast<float>(viewport.w), static_cast<float>(viewport.h) };
        SDL_RenderFillRect(renderer, &all);
        for (auto& l : lines)
        {
            const double sy = viewport.h - scroll + l.y;
            if (sy < 0.0 || sy > viewport.h - 40.0) continue;   // not (entirely) on screen
            l.label->SetY(static_cast<unsigned>(sy) + viewport.y);
            l.label->Draw(renderer);
        }
    }

    void OnUpdate(double delta) override
    {
        if (finished) return;
        elapsed += delta;
        // The last line stops in the middle of the screen.
        const double target = total + viewport.h * 0.5;
        if (scroll < target) scroll += SPEED * delta;
        else
        {
            end_wait += delta;
            if (end_wait > 4.0) Finish();
        }
    }
    void OnTick(unsigned int) override {}

    void OnKeyDown(SDL_Scancode scancode, SDL_Keymod) override
    {
        if (elapsed < 2.0) return;
        if (scancode == SDL_SCANCODE_RETURN || scancode == SDL_SCANCODE_SPACE || scancode == SDL_SCANCODE_ESCAPE ||
            scancode == SDL_SCANCODE_LSHIFT || scancode == SDL_SCANCODE_RSHIFT)
            Finish();
    }
    void OnKeyUp(SDL_Scancode, SDL_Keymod) override {}

private:
    struct Line
    {
        double y = 0.0;
        char align = 'C';
        std::string text;
        std::shared_ptr<TextLabel> label;
    };

    void Finish()
    {
        if (finished) return;
        finished = true;
        if (on_done) on_done();
    }

    static constexpr double SPEED = 110.0;   // pixels / s (the original: 60 on a 320 x 224 screen)

    SDL_Rect viewport;
    std::function<void()> on_done;
    std::vector<Line> lines;
    double total = 0.0, scroll = 0.0, elapsed = 0.0, end_wait = 0.0;
    bool finished = false;
};

#endif // _CREDITS_SCREEN_H_
