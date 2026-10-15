#define SDL_MAIN_USE_CALLBACKS 1  /* use the callbacks instead of main() */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <vector>
#include <cstdlib>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <iterator>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <format>

#include "graphics/Graphics.h"
#include "core/Timer.h"
#include "text/VarWidthFont.h"
#include "text/TextLabel.h"
#include "text/TextInput.h"
#include "core/Keyboard.h"
#include "graphics/Palette.h"
#include "ui/TextBox.h"
#include "ui/InputBox.h"
#include "ui/Hud.h"
#include "ui/FpsBox.h"
#include "room/RoomView.h"
#include "sprite/SpriteSet.h"
#include "game/GameState.h"
#include "game/Dialogue.h"

const std::string CHARSET = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz*.,?!/<>:-'\"%#&()=[]{}@";
std::shared_ptr<Graphics> graphics;
std::vector<std::shared_ptr<IDrawable>> drawables;
std::shared_ptr<TextBox> fps;
std::shared_ptr<Hud> hud;
std::shared_ptr<GameState> game_state;

/* This function runs once at startup. */
SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    /* Create the window */
    graphics = std::make_shared<Graphics>(1280, 1024, PROJECT_NAME);
    if (!graphics) {
        return SDL_APP_FAILURE;
    }
    auto main_font = std::make_shared<VarWidthFont>(
        std::make_shared<Texture>(graphics, std::filesystem::path(ASSETS_PATH "/graphics/fonts/mainfont.1bpp"), 16, 15, 1),
        CHARSET, std::unordered_map<char, Font::CONTROL_CHAR>{{'\n', Font::CONTROL_CHAR::NEWLINE}}, 8, 2, 1);
    auto menu_font = std::make_shared<Font>(
        std::make_shared<Texture>(graphics, std::filesystem::path(ASSETS_PATH "/graphics/fonts/menufont.1bpp"), 8, 8, 1),
        CHARSET, std::unordered_map<char, Font::CONTROL_CHAR>{{'\n', Font::CONTROL_CHAR::NEWLINE}});
    auto ui_tiles = std::make_shared<Texture>(graphics, std::filesystem::path(ASSETS_PATH "/graphics/tilesets/ui.bin"), 8, 8, 4);
    auto ui_pal = std::make_shared<Palette>(std::filesystem::path(ASSETS_PATH "/graphics/palettes/ui.pal"), 10, true);
    ui_pal->SetSdlColour(15, {0, 0, 0, 0}); // Make index 15 fully transparent
    ui_tiles->SetColours(ui_pal->GetSdlColours());
    menu_font->SetColours(ui_pal->GetSdlColours());
    main_font->SetColours(ui_pal->GetSdlColours());

    fps = std::make_shared<FpsBox>(ui_tiles, ui_pal, menu_font, 1120, 0);
    hud = std::make_shared<Hud>(ui_tiles, ui_pal, 0, 0, 1088, 8);
    SDL_Rect main_viewport = {
        0,
        static_cast<int>(hud->GetExternalHeightPixels()),
        static_cast<int>(graphics->GetWidth()),
        static_cast<int>(graphics->GetHeight() - hud->GetExternalHeightPixels())
    };
    // Optional argument: starting room number (rooms are in ASSETS_PATH/rooms/room_NNN).
    int start_room = 0;
    if (argc > 1) start_room = std::atoi(argv[1]);
    const std::filesystem::path rooms_dir = ASSETS_PATH "/rooms";
    auto status = std::make_shared<TextBox>(ui_tiles, ui_pal, menu_font, "", 8, main_viewport.y + 8, 720, 16, false, 2.0f, 2.0f);
    // Dialogue box: shown only while a conversation is open (added to / removed from drawables).
    auto dialogue_box = std::make_shared<TextBox>(ui_tiles, ui_pal, main_font, "", 48,
        graphics->GetHeight() - 220, graphics->GetWidth() - 96, 160, false, 2.0f, 2.0f);
    // Item menu: like the dialogue box, only in drawables while it is open.
    auto menu_box = std::make_shared<TextBox>(ui_tiles, ui_pal, main_font, "", 240, 120, 800, 440, false, 2.0f, 2.0f);
    auto message = std::make_shared<TextBox>(ui_tiles, ui_pal, menu_font, "", 8, main_viewport.y + 48, 720, 16, false, 2.0f, 2.0f);
    try {
        auto room = std::make_shared<RoomView>(graphics, rooms_dir, start_room, main_viewport, 3.0f);
        game_state = std::make_shared<GameState>(std::filesystem::path(ASSETS_PATH "/text/items.txt"));
        room->SetGameState(game_state);
        // Debug aid: LS_ITEMS="50,0,0" starts with these items (item indices) in the inventory.
        if (const char* items = std::getenv("LS_ITEMS"))
        {
            std::string list = items;
            for (char& c : list) if (c == ',') c = ' ';
            std::istringstream is(list);
            int it;
            while (is >> it) game_state->AddItem(it);
        }
        // Debug aid: LS_GOLD=500 starts with that much gold.
        if (const char* gold = std::getenv("LS_GOLD")) game_state->AddGold(std::atoi(gold));
        // Debug aid: LS_FLAGS="16,20,193" starts with these story flags set.
        if (const char* flags = std::getenv("LS_FLAGS"))
        {
            std::string list = flags;
            for (char& c : list) if (c == ',') c = ' ';
            std::istringstream fs(list);
            int fl;
            while (fs >> fl) game_state->SetFlag(fl);
            room->ApplyFlagRules();
        }
        room->SetDialogue(std::make_shared<Dialogue>(
            std::filesystem::path(ASSETS_PATH "/text/dialogue.json"), CHARSET + "\n"));
        room->SetDialogueCallback([dialogue_box](const std::string& text, bool visible) {
            auto it = std::find(drawables.begin(), drawables.end(), dialogue_box);
            if (visible)
            {
                dialogue_box->SetLabelText(text);
                if (it == drawables.end()) drawables.push_back(dialogue_box);
            }
            else if (it != drawables.end())
            {
                drawables.erase(it);
            }
        });
        room->SetShops(std::make_shared<Shops>(std::filesystem::path(ASSETS_PATH "/text/shops.json"),
                                               std::filesystem::path(ASSETS_PATH "/text/item_prices.txt")));
        try {
            room->SetChestSprite(std::make_shared<SpriteSet>(graphics, std::filesystem::path(ASSETS_PATH "/sprites/sprite_036")));
        } catch (const std::exception& e) {
            SDL_Log("Chest sprite not loaded: %s", e.what());
        }
        room->SetMenuCallback([menu_box](const std::string& text, bool visible) {
            auto it = std::find(drawables.begin(), drawables.end(), menu_box);
            if (visible)
            {
                menu_box->SetLabelText(text);
                if (it == drawables.end()) drawables.push_back(menu_box);
            }
            else if (it != drawables.end())
            {
                drawables.erase(it);
            }
        });
        room->SetMessageCallback([message](const std::string& text) { message->SetLabelText(text); });
        room->SetStatusCallback([status](const std::string& text) { status->SetLabelText(text); });
        try {
            room->SetEntityTable(std::make_shared<EntityTable>(graphics, std::filesystem::path(ASSETS_PATH "/sprites")));
        } catch (const std::exception& e) {
            SDL_Log("Entity table not loaded: %s", e.what());
        }
        try {
            room->SetPlayerSprite(std::make_shared<SpriteSet>(graphics, std::filesystem::path(ASSETS_PATH "/sprites/sprite_000")));
        } catch (const std::exception& e) {
            SDL_Log("Player sprite not loaded: %s", e.what());
        }
        // Optional arguments 2 and 3: starting position (world cell coordinates).
        if (argc > 3) room->SetPlayerPosition(std::atof(argv[2]), std::atof(argv[3]));
        Keyboard::GetInstance().RegisterKeyInputHandler(room);
        drawables.push_back(room);   // before scripted keys, so a dialogue box is drawn above it
        // Debug aid: LS_KEYS="UDLR..." walks the player 0.25 s per letter in that direction
        // (U/D/L/R = arrow keys, N/P = next/previous room), before the first frame.
        if (const char* keys = std::getenv("LS_KEYS"))
        {
            for (const char* k = keys; *k; ++k)
            {
                switch (*k)
                {
                case 'U': room->SimulateMove(-1, 0, 0.25); break;
                case 'D': room->SimulateMove(1, 0, 0.25); break;
                case 'L': room->SimulateMove(0, 1, 0.25); break;
                case 'R': room->SimulateMove(0, -1, 0.25); break;
                case 'J': room->OnKeyDown(SDL_SCANCODE_LCTRL, SDL_KMOD_NONE); room->SimulateMove(0, 0, 0.1); break;
                case 'W': room->SimulateMove(0, 0, 0.25); break;   // wait
                case 'S': room->OnKeyDown(SDL_SCANCODE_LSHIFT, SDL_KMOD_NONE); break;   // action key
                case 'c': room->OnKeyDown(SDL_SCANCODE_LSHIFT, SDL_KMOD_NONE); room->SimulateMove(0, 0, 0.25); break;   // action, lid half open
                case 'C': room->OnKeyDown(SDL_SCANCODE_LSHIFT, SDL_KMOD_NONE); room->SimulateMove(0, 0, 0.7); break;    // action, item held up
                case 'E': room->OnKeyDown(SDL_SCANCODE_E, SDL_KMOD_NONE); break;   // use EkeEke
                case 'Y': room->OnKeyDown(SDL_SCANCODE_Y, SDL_KMOD_NONE); break;   // answer yes
                case 'K': room->OnKeyDown(SDL_SCANCODE_K, SDL_KMOD_NONE); break;   // use a key
                case 'T': room->OnKeyDown(SDL_SCANCODE_TAB, SDL_KMOD_NONE); break;   // item menu
                case 'V': room->OnKeyDown(SDL_SCANCODE_DOWN, SDL_KMOD_NONE); break;   // (menu) down
                case 'H': room->OnKeyDown(SDL_SCANCODE_LSHIFT, SDL_KMOD_NONE); break;   // (menu) use
                case 'X': room->OnKeyDown(SDL_SCANCODE_N, SDL_KMOD_NONE); break;   // answer no
                case 'A': room->OnKeyDown(SDL_SCANCODE_SPACE, SDL_KMOD_NONE); room->SimulateMove(0, 0, 0.45); break;   // swing
                case 'a': room->OnKeyDown(SDL_SCANCODE_SPACE, SDL_KMOD_NONE); room->SimulateMove(0, 0, 0.12); break;   // swing, stop mid-way
                case 'b': room->OnKeyDown(SDL_SCANCODE_SPACE, SDL_KMOD_NONE); room->SimulateMove(0, 0, 0.05); break;   // swing, before the blow lands
                case 'N': room->OnKeyDown(SDL_SCANCODE_PAGEDOWN, SDL_KMOD_NONE); break;
                case 'P': room->OnKeyDown(SDL_SCANCODE_PAGEUP, SDL_KMOD_NONE); break;
                default: break;
                }
            }
        }
    } catch (const std::exception& e) {
        SDL_Log("Unable to load room %d from '%s': %s", start_room, rooms_dir.string().c_str(), e.what());
        status->SetLabelText(std::string("Room load error: ") + e.what());
    }
    drawables.push_back(status);
    drawables.push_back(message);
    drawables.push_back(hud);
    drawables.push_back(fps);
    return SDL_APP_CONTINUE;
}

/* This function runs when a new event (mouse input, keypresses, etc) occurs. */
SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    if (event->type == SDL_EVENT_QUIT)
    {
        return SDL_APP_SUCCESS;  /* end the program, reporting success to the OS. */
    }
    else if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP)
    {
        auto keycode = SDL_GetKeyFromScancode(event->key.scancode, event->key.mod, false);
        if(event->type == SDL_EVENT_KEY_DOWN && keycode == SDLK_ESCAPE)
        {
            return SDL_APP_SUCCESS;
        }
        Keyboard::GetInstance().OnKeyEvent(event->key);
    }
    return SDL_APP_CONTINUE;
}

/* This function runs once per frame, and is the heart of the program. */
SDL_AppResult SDL_AppIterate(void *appstate)
{
    const double delta = Timer::GetDelta();
    if (game_state)
    {
        hud->SetGold(static_cast<unsigned int>(std::max(0, game_state->GetGold())));
        hud->SetCurrentEkeeke(static_cast<unsigned int>(game_state->ItemCount(GameState::ITEM_EKEEKE)));
        hud->SetMaxHealth(static_cast<unsigned int>(game_state->GetMaxHealth()));
        hud->SetCurrentHealth(static_cast<unsigned int>(game_state->GetHealth()));
    }
    std::for_each(drawables.begin(), drawables.end(), [delta](auto& d){ d->OnUpdate(delta); });
    Timer::ProcessTicks([](unsigned int tick_count)
    {
        std::for_each(drawables.begin(), drawables.end(), [tick_count](auto& d){ d->OnTick(tick_count); });
    });
    graphics->Render(drawables);

    return SDL_APP_CONTINUE;
}

/* This function runs once at shutdown. */
void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
}
