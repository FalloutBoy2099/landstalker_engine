#ifndef _MENU_SCREEN_H_
#define _MENU_SCREEN_H_

#include <SDL3/SDL.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "core/IKeyInput.h"
#include "graphics/IDrawable.h"
#include "ui/TextBox.h"

// A vertical list of choices in a box, with an optional title box above it: the building
// block of the pause menu, the game over screen, the save slots and the options. Up / Down
// move, Enter / Shift / Space choose, Escape cancels. It can dim what is behind it.
class MenuScreen : public IDrawable, public IKeyInput
{
public:
    struct Item
    {
        std::string label;
        bool enabled = true;
    };

    MenuScreen(SDL_Rect viewport, std::shared_ptr<TextBox> title_box, std::shared_ptr<TextBox> list_box,
               std::string title, std::vector<Item> items, std::function<void(int)> on_choice,
               std::function<void()> on_cancel, bool dim)
        : viewport(viewport), title_box(std::move(title_box)), list_box(std::move(list_box)),
          items(std::move(items)), on_choice(std::move(on_choice)), on_cancel(std::move(on_cancel)), dim(dim)
    {
        if (this->title_box) this->title_box->SetLabelText(title);
        for (std::size_t i = 0; i < this->items.size(); ++i)
            if (this->items[i].enabled) { selected = static_cast<int>(i); break; }
        Refresh();
    }

    void SetItemLabel(int index, const std::string& label)
    {
        if (index >= 0 && index < static_cast<int>(items.size())) items[index].label = label;
        Refresh();
    }

    void Draw(SDL_Renderer* renderer) override
    {
        if (dim)
        {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
            const SDL_FRect all = { static_cast<float>(viewport.x), static_cast<float>(viewport.y),
                                    static_cast<float>(viewport.w), static_cast<float>(viewport.h) };
            SDL_RenderFillRect(renderer, &all);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        }
        if (title_box) title_box->Draw(renderer);
        if (list_box) list_box->Draw(renderer);
    }
    void OnUpdate(double) override {}
    void OnTick(unsigned int) override {}

    void OnKeyDown(SDL_Scancode scancode, SDL_Keymod) override
    {
        switch (scancode)
        {
        case SDL_SCANCODE_UP:   Move(-1); break;
        case SDL_SCANCODE_DOWN: Move(1); break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT:
        case SDL_SCANCODE_SPACE:
            if (!items.empty() && items[selected].enabled && on_choice) on_choice(selected);
            break;
        case SDL_SCANCODE_ESCAPE:
            if (on_cancel) on_cancel();
            break;
        default: break;
        }
    }
    void OnKeyUp(SDL_Scancode, SDL_Keymod) override {}

private:
    void Move(int step)
    {
        const int n = static_cast<int>(items.size());
        for (int tries = 0; tries < n; ++tries)
        {
            selected = (selected + step + n) % n;
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
            if (i + 1 < items.size()) text += "\n";
        }
        if (list_box) list_box->SetLabelText(text);
    }

    SDL_Rect viewport;
    std::shared_ptr<TextBox> title_box, list_box;
    std::vector<Item> items;
    std::function<void(int)> on_choice;
    std::function<void()> on_cancel;
    bool dim;
    int selected = 0;
};

#endif // _MENU_SCREEN_H_
