#ifndef _MENUS_H_
#define _MENUS_H_

#include <SDL3/SDL.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "audio/SoundEngine.h"
#include "core/Keyboard.h"
#include "room/RoomView.h"
#include "ui/CreditsScreen.h"
#include "ui/MenuScreen.h"
#include "ui/TitleScreen.h"

// The screens around the game: title, pause menu (Esc), game over, save slots and options.
// Screens stack (the pause menu opens the slots, which open...). While one is open the room
// is paused and the keyboard belongs to the top screen; when the last one closes the room
// takes the keyboard back.
class Menus
{
public:
    struct Context
    {
        SDL_Renderer* renderer = nullptr;
        std::shared_ptr<Texture> ui_tiles;
        std::shared_ptr<Palette> ui_palette;
        std::shared_ptr<Font> font;
        SDL_Rect viewport{};
        std::shared_ptr<RoomView> room;
        std::shared_ptr<SoundEngine> sound;           // may be null
        std::vector<std::shared_ptr<IDrawable>>* drawables = nullptr;
        std::filesystem::path settings_path;
        SDL_Window* window = nullptr;
        std::function<void(bool)> set_info_bar;       // the debug line at the top of the screen
        std::function<void(bool)> set_fps;            // the frame rate box
        std::filesystem::path title_picture;
        std::filesystem::path climax_picture;
        std::filesystem::path credits_list;
    };

    explicit Menus(Context ctx) : ctx(std::move(ctx))
    {
        LoadSettings();
        ApplyAll();
    }

    bool Active() const { return !stack.empty(); }

    // Debug aid (LS_MENU=pause|gameover|save|load|options): opens a screen directly.
    void Debug(const std::string& what)
    {
        if (what == "pause") ShowPause();
        else if (what == "gameover") ShowGameOver();
        else if (what == "save") OpenSlots(true);
        else if (what == "load") OpenSlots(false);
        else if (what == "options") OpenOptions();
        else if (what == "credits") ShowCredits();
    }

    // splash: begin with the Climax logo (only when the game starts).
    void ShowTitle(bool splash = false)
    {
        bool any_save = false;
        for (int s = 1; s <= RoomView::SAVE_SLOTS; ++s) any_save = any_save || !ctx.room->SlotSummary(s).empty();
        const unsigned w = ctx.viewport.w, h = ctx.viewport.h;
        auto box = std::make_shared<TextBox>(ctx.ui_tiles, ctx.ui_palette, ctx.font, "", (w - 460) / 2,
                                             (h - 832) / 2 + 19 * 8 * 4 - 64, 460, 112, false, 2.0f, 2.0f);
        if (ctx.sound) ctx.sound->PlayMusic(0x10);   // title music
        auto title = std::make_shared<TitleScreen>(ctx.renderer, ctx.title_picture, splash ? ctx.climax_picture : std::filesystem::path(), ctx.viewport, box, any_save,
            [this](TitleScreen::Choice c) {
                if (c == TitleScreen::QUIT)
                {
                    SDL_Event quit;
                    SDL_zero(quit);
                    quit.type = SDL_EVENT_QUIT;
                    SDL_PushEvent(&quit);
                }
                else if (c == TitleScreen::NEW_GAME)
                {
                    CloseAll();
                    ctx.room->BeginNewGame();
                }
                else
                {
                    OpenSlots(false);
                }
            });
        Push(title, title);
    }

    void ShowPause()
    {
        if (Active()) return;
        auto menu = MakeMenu("PAUSE", { { "RESUME" }, { "SAVE GAME" }, { "LOAD GAME" }, { "OPTIONS" }, { "QUIT TO TITLE" } }, 460,
            [this](int i) {
                switch (i)
                {
                case 0: CloseAll(); break;
                case 1: OpenSlots(true); break;
                case 2: OpenSlots(false); break;
                case 3: OpenOptions(); break;
                default: CloseAll(); ShowTitle(); break;
                }
            },
            [this]() { CloseAll(); }, true);
        Push(menu, menu);
    }

    // The end of the game: the credits, then back to the title.
    void ShowCredits()
    {
        CloseAll();
        if (ctx.sound) ctx.sound->PlayMusic(0x1B);   // the ending music
        auto credits = std::make_shared<CreditsScreen>(ctx.viewport, ctx.credits_list, ctx.font, 2.0f, [this]() {
            CloseAll();
            ShowTitle();
        });
        Push(credits, credits);
    }

    void ShowGameOver()
    {
        if (Active()) return;
        bool any_save = false;
        for (int s = 1; s <= RoomView::SAVE_SLOTS; ++s) any_save = any_save || !ctx.room->SlotSummary(s).empty();
        auto menu = MakeMenu("GAME OVER", { { "CONTINUE" }, { "LOAD GAME", any_save }, { "QUIT TO TITLE" } }, 460,
            [this](int i) {
                if (i == 0) { CloseAll(); ctx.room->Respawn(); }
                else if (i == 1) OpenSlots(false);
                else { CloseAll(); ShowTitle(); }
            },
            nullptr, true);
        Push(menu, menu);
    }

private:
    struct Screen
    {
        std::shared_ptr<IDrawable> drawable;
        std::shared_ptr<IKeyInput> keys;
    };

    std::shared_ptr<MenuScreen> MakeMenu(const std::string& title, std::vector<MenuScreen::Item> items, unsigned width,
                                         std::function<void(int)> on_choice, std::function<void()> on_cancel, bool dim)
    {
        const unsigned w = ctx.viewport.w, h = ctx.viewport.h;
        const unsigned list_h = 14 + 32 * static_cast<unsigned>(items.size());
        const unsigned x = (w - width) / 2, y = (h - list_h) / 2 + 20;
        auto title_box = std::make_shared<TextBox>(ctx.ui_tiles, ctx.ui_palette, ctx.font, "", x, y - 60, width, 36, false, 2.0f, 2.0f);
        auto list_box = std::make_shared<TextBox>(ctx.ui_tiles, ctx.ui_palette, ctx.font, "", x, y, width, list_h, false, 2.0f, 2.0f);
        return std::make_shared<MenuScreen>(ctx.viewport, title_box, list_box, title, std::move(items),
                                            std::move(on_choice), std::move(on_cancel), dim);
    }

    void Push(std::shared_ptr<IDrawable> d, std::shared_ptr<IKeyInput> k)
    {
        if (stack.empty())
        {
            ctx.room->SetPaused(true);
            Keyboard::GetInstance().UnregisterKeyInputHandler(ctx.room);
        }
        else
        {
            Keyboard::GetInstance().UnregisterKeyInputHandler(stack.back().keys);
        }
        stack.push_back({ d, k });
        ctx.drawables->push_back(d);
        Keyboard::GetInstance().RegisterKeyInputHandler(k);
    }

    // Closes the top screen (the one below gets the keyboard back).
    void Pop()
    {
        if (stack.empty()) return;
        Remove(stack.back());
        stack.pop_back();
        if (stack.empty()) Resume();
        else Keyboard::GetInstance().RegisterKeyInputHandler(stack.back().keys);
    }

    void CloseAll()
    {
        while (!stack.empty())
        {
            Remove(stack.back());
            stack.pop_back();
        }
        Resume();
    }

    void Remove(const Screen& s)
    {
        Keyboard::GetInstance().UnregisterKeyInputHandler(s.keys);
        auto& v = *ctx.drawables;
        v.erase(std::remove(v.begin(), v.end(), s.drawable), v.end());
    }

    void Resume()
    {
        Keyboard::GetInstance().RegisterKeyInputHandler(ctx.room);
        ctx.room->SetPaused(false);
    }

    // Save slots. Saving to a slot makes it the active one (F5 / F9 and the churches use it).
    void OpenSlots(bool saving)
    {
        std::vector<MenuScreen::Item> items;
        for (int s = 1; s <= RoomView::SAVE_SLOTS; ++s)
        {
            const std::string summary = ctx.room->SlotSummary(s);
            items.push_back({ "SLOT " + std::to_string(s) + ": " + (summary.empty() ? "EMPTY" : summary), saving || !summary.empty() });
        }
        items.push_back({ "BACK" });
        auto menu = MakeMenu(saving ? "SAVE GAME" : "LOAD GAME", std::move(items), 900,
            [this, saving](int i) {
                if (i >= RoomView::SAVE_SLOTS) { Pop(); return; }
                ctx.room->SetActiveSlot(i + 1);
                SaveSettings();
                CloseAll();
                if (saving) ctx.room->SaveGame();
                else ctx.room->LoadGame();
            },
            [this]() { Pop(); }, true);
        Push(menu, menu);
    }

    void OpenOptions()
    {
        auto menu_ptr = std::make_shared<std::shared_ptr<MenuScreen>>();
        auto onoff = [](bool v) { return v ? "ON" : "OFF"; };
        auto label = [this, onoff](int i) -> std::string {
            switch (i)
            {
            case 0: return "VOLUME: " + std::to_string(static_cast<int>(volume * 100.0f + 0.5f)) + "%";
            case 1: return std::string("MUSIC: ") + onoff(music);
            case 2: return std::string("SOUND EFFECTS: ") + onoff(sfx);
            case 3: return std::string("FULLSCREEN: ") + onoff(fullscreen);
            case 4: return "ZOOM: " + std::to_string(zoom) + "X";
            case 5: return std::string("INFO LINE: ") + onoff(info_bar);
            case 6: return std::string("FRAME RATE: ") + onoff(show_fps);
            case 7: return "CONTROLS";
            default: return "BACK";
            }
        };
        std::vector<MenuScreen::Item> items;
        for (int i = 0; i <= 8; ++i) items.push_back({ label(i) });
        auto menu = MakeMenu("OPTIONS", std::move(items), 700,
            [this, menu_ptr, label](int i) {
                switch (i)
                {
                case 0: volume = volume <= 0.01f ? 1.0f : std::max(0.0f, volume - 0.25f); break;   // 100 75 50 25 0
                case 1: music = !music; break;
                case 2: sfx = !sfx; break;
                case 3: fullscreen = !fullscreen; break;
                case 4: zoom = zoom >= 5 ? 2 : zoom + 1; break;
                case 5: info_bar = !info_bar; break;
                case 6: show_fps = !show_fps; break;
                case 7: OpenControls(); return;
                default: Pop(); return;
                }
                ApplyAll();
                SaveSettings();
                if (i == 1 && music && ctx.room) ctx.room->RestartMusic();
                if (*menu_ptr) (*menu_ptr)->SetItemLabel(i, label(i));
                if (ctx.sound) ctx.sound->Command(SoundEngine::SFX_CURSOR_SELECT);
            },
            [this]() { Pop(); }, true);
        *menu_ptr = menu;
        Push(menu, menu);
    }

    // What the keys do (fixed: the list is only shown).
    void OpenControls()
    {
        std::vector<MenuScreen::Item> items;
        for (const char* line : { "ARROWS: MOVE", "SHIFT: TALK, OPEN, BUY", "SPACE: SWORD", "CTRL: JUMP", "TAB: ITEM MENU",
                                  "E: USE AN EKEEKE", "K: USE A KEY ON A DOOR", "Y / N: ANSWER A QUESTION",
                                  "ESC: PAUSE MENU", "F5 / F9: QUICK SAVE / LOAD", "KEYPAD + / -: ZOOM" })
            items.push_back({ line, false });
        items.push_back({ "BACK" });
        auto menu = MakeMenu("CONTROLS", std::move(items), 900, [this](int) { Pop(); }, [this]() { Pop(); }, true);
        Push(menu, menu);
    }

    // Puts every setting into effect.
    void ApplyAll()
    {
        ApplyVolume();
        if (ctx.sound)
        {
            ctx.sound->SetSfxEnabled(sfx);
            ctx.sound->SetMusicEnabled(music);
        }
        if (ctx.window) SDL_SetWindowFullscreen(ctx.window, fullscreen);
        if (ctx.room) ctx.room->SetScale(static_cast<float>(zoom));
        if (ctx.set_info_bar) ctx.set_info_bar(info_bar);
        if (ctx.set_fps) ctx.set_fps(show_fps);
    }

    void ApplyVolume()
    {
        if (ctx.sound) ctx.sound->SetVolume(volume);
    }

    void LoadSettings()
    {
        std::ifstream f(ctx.settings_path);
        std::string key;
        while (f >> key)
        {
            if (key == "volume") { f >> volume; volume = std::clamp(volume, 0.0f, 1.0f); }
            else if (key == "music") { int v = 1; f >> v; music = v != 0; }
            else if (key == "sfx") { int v = 1; f >> v; sfx = v != 0; }
            else if (key == "fullscreen") { int v = 0; f >> v; fullscreen = v != 0; }
            else if (key == "zoom") { f >> zoom; zoom = std::clamp(zoom, 2, 5); }
            else if (key == "info") { int v = 0; f >> v; info_bar = v != 0; }
            else if (key == "fps") { int v = 0; f >> v; show_fps = v != 0; }
            else if (key == "slot")
            {
                int s = 1;
                f >> s;
                ctx.room->SetActiveSlot(std::clamp(s, 1, static_cast<int>(RoomView::SAVE_SLOTS)));
            }
            else f.ignore(1 << 10, '\n');
        }
    }

    void SaveSettings()
    {
        if (ctx.settings_path.empty()) return;
        std::ofstream f(ctx.settings_path);
        f << "volume " << volume << "\nslot " << ctx.room->ActiveSlot() << "\nmusic " << music << "\nsfx " << sfx
          << "\nfullscreen " << fullscreen << "\nzoom " << zoom << "\ninfo " << info_bar << "\nfps " << show_fps << "\n";
    }

    Context ctx;
    std::vector<Screen> stack;
    float volume = 1.0f;
    bool music = true, sfx = true, fullscreen = false, info_bar = false, show_fps = false;
    int zoom = 3;
};

#endif // _MENUS_H_
