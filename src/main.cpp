#define SDL_MAIN_USE_CALLBACKS 1  /* use the callbacks instead of main() */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <vector>
#include <cstdlib>
#include <cstdio>
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
#include "audio/SoundEngine.h"
#include "ui/Menus.h"
#include "debug/Bot.h"
#include "debug/CrashLog.h"
#include "game/Dialogue.h"

const std::string CHARSET = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz*.,?!/<>:-'\"%#&()=[]{}@";
std::shared_ptr<Graphics> graphics;
std::vector<std::shared_ptr<IDrawable>> drawables;
std::shared_ptr<TextBox> fps;
std::shared_ptr<Hud> hud;
std::shared_ptr<GameState> game_state;
std::shared_ptr<SoundEngine> sound_engine;
std::shared_ptr<RoomView> room_view;
std::shared_ptr<TextBox> status_box;   // the debug line at the top
std::shared_ptr<Menus> menus;   // title, pause, game over, slots, options
bool want_title = false;
std::filesystem::path pref_dir;

/* This function runs once at startup. */
SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    InstallCrashLog();
    // Debug aid: LS_SOUND_DUMP="id:seconds:file.wav" renders one sound offline and exits.
    if (const char* dump = std::getenv("LS_SOUND_DUMP"))
    {
        int id = 1;
        double seconds = 5.0;
        char path[512] = "sound.wav";
        std::sscanf(dump, "%i:%lf:%511s", &id, &seconds, path);
        SoundEngine engine(ASSETS_PATH "/sound/rom.bin");
        const bool ok = engine.IsValid() && engine.DumpWav(path, id, seconds);
        SDL_Log("Sound dump %s: %s", path, ok ? "ok" : "failed");
        return SDL_APP_SUCCESS;
    }

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
    // Without an argument the game starts like a new game (the intro, room 139).
    int start_room = 139;
    if (argc > 1) start_room = std::atoi(argv[1]);
    const std::filesystem::path rooms_dir = ASSETS_PATH "/rooms";
    status_box = std::make_shared<TextBox>(ui_tiles, ui_pal, menu_font, "", 8, main_viewport.y + 8, 720, 16, false, 2.0f, 2.0f);
    // Dialogue box: shown only while a conversation is open (added to / removed from drawables).
    auto dialogue_box = std::make_shared<TextBox>(ui_tiles, ui_pal, main_font, "", 48,
        graphics->GetHeight() - 220, graphics->GetWidth() - 96, 160, false, 2.0f, 2.0f);
    // Item menu: like the dialogue box, only in drawables while it is open.
    auto menu_box = std::make_shared<TextBox>(ui_tiles, ui_pal, main_font, "", 240, 120, 800, 440, false, 2.0f, 2.0f);
    auto message = std::make_shared<TextBox>(ui_tiles, ui_pal, menu_font, "", 8, main_viewport.y + 48, 720, 16, false, 2.0f, 2.0f);
    try {
        auto room = std::make_shared<RoomView>(graphics, rooms_dir, start_room, main_viewport, 3.0f);
        room_view = room;
        game_state = std::make_shared<GameState>(std::filesystem::path(ASSETS_PATH "/text/items.txt"));
        room->SetGameState(game_state);
        try {
            room->SetDrops(std::make_shared<Drops>(std::filesystem::path(ASSETS_PATH "/text/enemy_drops.txt")));
        } catch (const std::exception& ex) {
            SDL_Log("Drops not loaded: %s", ex.what());
        }
        std::shared_ptr<Behaviours> room_behaviours;
        try {
            room_behaviours = std::make_shared<Behaviours>(std::filesystem::path(ASSETS_PATH "/text/behaviours.txt"));
            room->SetBehaviours(room_behaviours);
        } catch (const std::exception& ex) {
            SDL_Log("Behaviours not loaded: %s", ex.what());
        }
        try {
            room->SetCutscenes(std::make_shared<Cutscenes>(
                std::filesystem::path(ASSETS_PATH "/text/cutscenes.json"), CHARSET + "\n"));
        } catch (const std::exception& ex) {
            SDL_Log("Cutscenes not loaded: %s", ex.what());
        }
        try {
            if (room_behaviours) room_behaviours->LoadPlayback(std::filesystem::path(ASSETS_PATH "/text/playback.txt"));
        } catch (const std::exception&) {}
        // F5 saves, F9 loads (the active slot); three slots live in the user's pref dir.
        // LS_SAVE=path overrides the file (tests).
        {
            if (char* pref = SDL_GetPrefPath("landstalker", "dds520"))
            {
                pref_dir = pref;
                SDL_free(pref);
            }
            room->SetSaveDir(pref_dir);
            room->SetActiveSlot(1);
        }
        // Music and sound effects: the original Z80 sound driver and the YM2612 / PSG are
        // emulated. LS_NOSOUND=1 turns it off.
        if (!std::getenv("LS_NOSOUND"))
        {
            auto engine = std::make_shared<SoundEngine>(std::filesystem::path(ASSETS_PATH "/sound/rom.bin"));
            if (engine->IsValid() && engine->StartOutput())
            {
                sound_engine = engine;
                room->SetSound(sound_engine);
            }
            else
            {
                SDL_Log("Sound disabled (no ROM at assets/sound/rom.bin or no audio device)");
            }
        }
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
        // Debug aid: LS_STATUS="0,2" starts with these ailments (0 poison, 1 confusion, 2 paralysis, 3 curse).
        if (const char* st = std::getenv("LS_STATUS"))
        {
            std::string list = st;
            for (char& c : list) if (c == ',') c = ' ';
            std::istringstream ss(list);
            int v;
            while (ss >> v) room->InflictStatus(v);
        }
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
        // The message line is only in the drawables while it has something to say.
        room->SetMessageCallback([message](const std::string& text) {
            message->SetLabelText(text);
            auto it = std::find(drawables.begin(), drawables.end(), std::static_pointer_cast<IDrawable>(message));
            if (text.empty() && it != drawables.end()) drawables.erase(it);
            else if (!text.empty() && it == drawables.end())
            {
                auto at = std::find(drawables.begin(), drawables.end(), std::static_pointer_cast<IDrawable>(hud));
                drawables.insert(at, message);
            }
        });
        room->SetStatusCallback([](const std::string& text) { status_box->SetLabelText(text); });
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
        // No arguments: the title screen (LS_NOTITLE=1 goes straight to the intro).
        // LS_LOAD loads the save game instead, LS_NOINTRO stays in the given room.
        if (argc <= 1 && !std::getenv("LS_LOAD") && !std::getenv("LS_NOINTRO"))
        {
            if (std::getenv("LS_NOTITLE")) room->BeginNewGame();
            else want_title = true;
        }
        // Debug aid: LS_SCAN_OCC=1 lists cells where priority tiles hide the player.
        if (std::getenv("LS_SCAN_OCC")) room->ScanOcclusion();
        // Debug aid: LS_POSE=t puts every boss of the room t seconds into its attack animation.
        if (const char* pose = std::getenv("LS_POSE")) room->DebugPose(std::atof(pose));
        // Debug aid: LS_KILL=n kills entity n before the first frame (death animation test).
        if (const char* kill = std::getenv("LS_KILL")) room->DebugKill(static_cast<std::size_t>(std::atoi(kill)));
        // Debug aid: LS_ACTION=n runs custom script action n (a cutscene) before the first frame.
        if (const char* act = std::getenv("LS_ACTION")) room->TriggerAction(std::atoi(act));
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
                case 'F': room->OnKeyDown(SDL_SCANCODE_F5, SDL_KMOD_NONE); break;   // save game
                case 'G': room->OnKeyDown(SDL_SCANCODE_F9, SDL_KMOD_NONE); break;   // load game
                case 'N': room->OnKeyDown(SDL_SCANCODE_PAGEDOWN, SDL_KMOD_NONE); break;
                case 'P': room->OnKeyDown(SDL_SCANCODE_PAGEUP, SDL_KMOD_NONE); break;
                default: break;
                }
            }
        }
    } catch (const std::exception& e) {
        SDL_Log("Unable to load room %d from '%s': %s", start_room, rooms_dir.string().c_str(), e.what());
        status_box->SetLabelText(std::string("Room load error: ") + e.what());
    }
    drawables.push_back(status_box);
    drawables.push_back(hud);
    drawables.push_back(fps);
    if (room_view)
    {
        Menus::Context ctx;
        ctx.renderer = graphics->GetRenderer();
        ctx.ui_tiles = ui_tiles;
        ctx.ui_palette = ui_pal;
        ctx.font = main_font;
        ctx.viewport = SDL_Rect{ 0, 0, static_cast<int>(graphics->GetWidth()), static_cast<int>(graphics->GetHeight()) };
        ctx.room = room_view;
        ctx.sound = sound_engine;
        ctx.drawables = &drawables;
        ctx.window = graphics->GetWindow();
        // The debug line and the frame rate box can be hidden (options). Both start in the drawables.
        auto toggle = [](std::shared_ptr<IDrawable> d, bool on) {
            auto it = std::find(drawables.begin(), drawables.end(), d);
            if (on && it == drawables.end()) drawables.insert(drawables.begin() + (drawables.empty() ? 0 : 1), d);
            else if (!on && it != drawables.end()) drawables.erase(it);
        };
        ctx.set_info_bar = [toggle](bool on) { toggle(status_box, on); };
        ctx.set_fps = [toggle](bool on) { toggle(fps, on); };
        ctx.settings_path = pref_dir.empty() ? std::filesystem::path() : pref_dir / "settings.txt";
        ctx.title_picture = std::filesystem::path(ASSETS_PATH "/graphics/title.png");
        ctx.climax_picture = std::filesystem::path(ASSETS_PATH "/graphics/climax.png");
        ctx.credits_list = std::filesystem::path(ASSETS_PATH "/text/credits.txt");
        menus = std::make_shared<Menus>(ctx);
        room_view->SetPauseCallback([]() { if (menus) menus->ShowPause(); });
        room_view->SetGameOverCallback([]() { if (menus) menus->ShowGameOver(); });
        room_view->SetCreditsCallback([]() { if (menus) menus->ShowCredits(); });
        // LS_SAVE overrides the slot files (tests); the settings may have changed the slot.
        if (const char* custom = std::getenv("LS_SAVE")) room_view->SetSavePath(custom);
        if (want_title) menus->ShowTitle(true);   // last: above the HUD
        if (const char* what = std::getenv("LS_MENU")) menus->Debug(what);
        // Debug aid: LS_BOT=<room> plays the game (new game, no title) at high speed towards that
        // room's boss and logs the way to stderr; LS_BOT_TIME = game seconds allowed (default 7200).
        if (const char* target = std::getenv("LS_BOT"))
        {
            const char* limit = std::getenv("LS_BOT_TIME");
            Bot bot(*room_view, game_state, std::filesystem::path(ASSETS_PATH "/rooms"));
            bot.Run(std::atoi(target), limit ? std::atof(limit) : 7200.0);
        }
    }
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
    sound_engine.reset();   // stops the audio callback before SDL shuts down
}
