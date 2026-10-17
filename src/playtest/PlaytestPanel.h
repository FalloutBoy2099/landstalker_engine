#ifndef _PLAYTEST_PANEL_H_
#define _PLAYTEST_PANEL_H_

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "core/Json.h"
#include "graphics/IDrawable.h"

// The playtest build (dds520_playtest.exe): the game is on the left, this panel on the right.
//  - notes for every room, kept in playtest_notes.json next to the exe ({"<room>": "text"});
//    click in the panel to type (Esc or a click on the game gives the keyboard back to the game);
//  - a Capture button (F12): the game freezes, drag a rectangle (Enter: the whole game, Esc: cancel)
//    and the picture goes to screenshots/ next to the exe; its name is added to the room's notes.
// Text is drawn with SDL's built-in 8x8 font (ASCII only: other characters show as '?').
class PlaytestPanel : public IDrawable
{
public:
    PlaytestPanel(SDL_Renderer* renderer, std::function<int()> current_room, int x0, int width, int height, int game_width,
                  const std::filesystem::path& dir)
        : renderer(renderer), current_room(std::move(current_room)), x0(x0), w(width), h(height), game_w(game_width), dir(dir)
    {
        notes_file = dir / "playtest_notes.json";
        Load();
    }

    ~PlaytestPanel() override
    {
        Commit();
        Save();
        if (frozen_tex) SDL_DestroyTexture(frozen_tex);
        if (frozen) SDL_DestroySurface(frozen);
    }

    // What the panel can ask of the game (playtest aids).
    struct Hooks
    {
        std::function<bool(int)> go_room;       // false when the room does not exist
        std::function<void(int)> hp_delta;      // change the maximum health by n hearts (and fill it)
        std::function<void()> hp_full;          // refill the health
        std::function<std::string()> hp_text;   // "8/11"
        std::function<std::string()> control_lock;   // why the player cannot move ("": he is in control)
    };
    void SetHooks(Hooks h) { hooks = std::move(h); }
    void Flush() { Commit(); Save(); }
    bool IsSelecting() const { return selecting; }
    bool IsFocused() const { return focused; }

    void OnTick(unsigned int) override {}

    void OnUpdate(double delta) override
    {
        blink += delta;
        const int room = current_room ? current_room() : -1;
        if (room != cur_room)
        {
            Commit();
            if (dirty) Save();
            cur_room = room;
            auto it = notes.find(room);
            text = it == notes.end() ? std::string() : it->second;
            cursor = text.size();
            scroll = 0;
            if (!field_focus) room_field = std::to_string(room);
        }
        if (dirty && (save_timer += delta) > 1.0) { Commit(); Save(); }
    }

    // Returns true when the event was used by the panel (the game must not see it).
    bool HandleEvent(SDL_Event& e)
    {
        switch (e.type)
        {
        case SDL_EVENT_KEY_DOWN:
        {
            const SDL_Keycode key = e.key.key;
            if (key == SDLK_F12 && !e.key.repeat) { RequestCapture(); return true; }
            if (selecting)
            {
                if (key == SDLK_ESCAPE) { CancelSelection(); }
                else if (key == SDLK_RETURN) { Finish(SDL_Rect{ 0, 0, game_w, h }); }
                return true;
            }
            if (field_focus)
            {
                if (key == SDLK_ESCAPE) { field_focus = false; room_field = std::to_string(cur_room); }
                else if (key == SDLK_BACKSPACE) { if (!room_field.empty()) room_field.pop_back(); }
                else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) GoField();
                return true;
            }
            if (!focused) return false;
            if (key == SDLK_ESCAPE) { focused = false; return true; }
            EditKey(e.key);
            return true;
        }
        case SDL_EVENT_KEY_UP:
            return selecting;
        case SDL_EVENT_TEXT_INPUT:
            if (selecting) return false;
            if (field_focus)
            {
                for (const char* c = e.text.text; *c; ++c) if (*c >= '0' && *c <= '9' && room_field.size() < 4) room_field += *c;
                return true;
            }
            if (!focused) return false;
            Insert(e.text.text);
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        {
            SDL_ConvertEventToRenderCoordinates(renderer, &e);
            const float mx = e.button.x, my = e.button.y;
            if (selecting)
            {
                dragging = true;
                sel_a = { std::clamp(mx, 0.0f, static_cast<float>(game_w)), std::clamp(my, 0.0f, static_cast<float>(h)) };
                sel_b = sel_a;
                return true;
            }
            if (mx < x0) { focused = false; LeaveField(); return false; }
            if (InRect(mx, my, ButtonRect())) { RequestCapture(); return true; }
            if (InRect(mx, my, FieldRect())) { field_focus = true; focused = false; room_field.clear(); return true; }
            if (InRect(mx, my, SmallBtn(0))) { StepRoom(-1); return true; }
            if (InRect(mx, my, SmallBtn(1))) { StepRoom(1); return true; }
            if (InRect(mx, my, SmallBtn(2))) { GoField(); return true; }
            if (InRect(mx, my, HpBtn(0))) { if (hooks.hp_delta) hooks.hp_delta(-1); return true; }
            if (InRect(mx, my, HpBtn(1))) { if (hooks.hp_delta) hooks.hp_delta(1); return true; }
            if (InRect(mx, my, HpBtn(2))) { if (hooks.hp_full) hooks.hp_full(); return true; }
            if (InRect(mx, my, NotesRect())) { focused = true; LeaveField(); PlaceCursor(mx, my); blink = 0; return true; }
            focused = false;
            LeaveField();
            return true;
        }
        case SDL_EVENT_MOUSE_MOTION:
            if (selecting && dragging)
            {
                SDL_ConvertEventToRenderCoordinates(renderer, &e);
                sel_b = { std::clamp(e.motion.x, 0.0f, static_cast<float>(game_w)), std::clamp(e.motion.y, 0.0f, static_cast<float>(h)) };
                return true;
            }
            return selecting;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (selecting && dragging)
            {
                dragging = false;
                SDL_ConvertEventToRenderCoordinates(renderer, &e);
                sel_b = { std::clamp(e.button.x, 0.0f, static_cast<float>(game_w)), std::clamp(e.button.y, 0.0f, static_cast<float>(h)) };
                const int x = static_cast<int>(std::min(sel_a.x, sel_b.x)), y = static_cast<int>(std::min(sel_a.y, sel_b.y));
                const int rw = static_cast<int>(std::abs(sel_a.x - sel_b.x)), rh = static_cast<int>(std::abs(sel_a.y - sel_b.y));
                if (rw >= 6 && rh >= 6) Finish(SDL_Rect{ x, y, rw, rh });
                return true;
            }
            return selecting;
        case SDL_EVENT_MOUSE_WHEEL:
            if (!selecting) { scroll = std::max(0, scroll - static_cast<int>(e.wheel.y)); }
            return false;
        default:
            return false;
        }
    }

    void Draw(SDL_Renderer* r) override
    {
        if (capture_requested && !selecting) TakeFrozen();
        SDL_SetRenderViewport(r, nullptr);
        SDL_SetRenderClipRect(r, nullptr);
        SDL_SetRenderScale(r, 1.0f, 1.0f);
        if (selecting) { DrawSelection(r); return; }

        // panel
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(r, 22, 22, 34, 255);
        const SDL_FRect bg = { static_cast<float>(x0), 0.0f, static_cast<float>(w), static_cast<float>(h) };
        SDL_RenderFillRect(r, &bg);
        SDL_SetRenderDrawColor(r, 90, 90, 130, 255);
        SDL_RenderLine(r, static_cast<float>(x0), 0.0f, static_cast<float>(x0), static_cast<float>(h));

        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        SDL_SetRenderScale(r, 3.0f, 3.0f);
        char buf[64];
        std::snprintf(buf, sizeof buf, "ROOM %d", cur_room);
        SDL_RenderDebugText(r, (x0 + 16) / 3.0f, 14 / 3.0f, buf);

        // control marker: green when the player plays, red while a scene or a script drives him
        {
            const std::string why = hooks.control_lock ? hooks.control_lock() : std::string();
            const SDL_FRect m = { static_cast<float>(x0 + w - 150), 10.0f, 134.0f, 44.0f };
            SDL_SetRenderScale(r, 1.0f, 1.0f);
            if (why.empty()) SDL_SetRenderDrawColor(r, 40, 140, 70, 255);
            else SDL_SetRenderDrawColor(r, 200, 50, 50, 255);
            SDL_RenderFillRect(r, &m);
            SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
            SDL_SetRenderScale(r, 1.5f, 1.5f);
            SDL_RenderDebugText(r, (m.x + 8) / 1.5f, (m.y + 6) / 1.5f, why.empty() ? "YOU PLAY" : "NO CONTROL");
            SDL_SetRenderScale(r, 1.0f, 1.0f);
            if (!why.empty()) SDL_RenderDebugText(r, m.x + 8, m.y + 28, Ascii(why.substr(0, 15)).c_str());
        }
        SDL_SetRenderScale(r, 3.0f, 3.0f);

        // capture button
        const SDL_FRect b = ButtonRect();
        SDL_SetRenderScale(r, 1.0f, 1.0f);
        SDL_SetRenderDrawColor(r, 50, 90, 160, 255);
        SDL_RenderFillRect(r, &b);
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        SDL_SetRenderScale(r, 2.0f, 2.0f);
        SDL_RenderDebugText(r, (b.x + 14) / 2.0f, (b.y + 12) / 2.0f, "CAPTURE (F12)");

        // room and health controls
        auto button = [&](const SDL_FRect& b2, const char* label, Uint8 cr, Uint8 cg, Uint8 cb) {
            SDL_SetRenderScale(r, 1.0f, 1.0f);
            SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
            SDL_RenderFillRect(r, &b2);
            SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
            SDL_SetRenderScale(r, 2.0f, 2.0f);
            const float tw = static_cast<float>(std::strlen(label)) * 8.0f;
            SDL_RenderDebugText(r, (b2.x + b2.w / 2.0f) / 2.0f - tw / 2.0f, (b2.y + 10) / 2.0f, label);
        };
        button(SmallBtn(0), "<", 60, 60, 90);
        button(SmallBtn(1), ">", 60, 60, 90);
        button(SmallBtn(2), "GO", 50, 130, 80);
        {
            const SDL_FRect f = FieldRect();
            button(f, (room_field + (field_focus && std::fmod(blink, 1.0) < 0.55 ? "_" : "")).c_str(), field_focus ? 70 : 38, field_focus ? 70 : 38, field_focus ? 100 : 54);
        }
        button(HpBtn(0), "HP-", 140, 60, 60);
        button(HpBtn(1), "HP+", 60, 120, 70);
        button(HpBtn(2), "FULL", 60, 90, 150);
        if (hooks.hp_text)
        {
            SDL_SetRenderScale(r, 2.0f, 2.0f);
            SDL_SetRenderDrawColor(r, 255, 160, 160, 255);
            SDL_RenderDebugText(r, (x0 + 240) / 2.0f, 176 / 2.0f, hooks.hp_text().c_str());
        }

        // notes
        const SDL_FRect nr = NotesRect();
        SDL_SetRenderScale(r, 1.0f, 1.0f);
        SDL_SetRenderDrawColor(r, focused ? 36 : 28, focused ? 36 : 28, focused ? 52 : 40, 255);
        SDL_RenderFillRect(r, &nr);
        SDL_SetRenderDrawColor(r, focused ? 255 : 90, focused ? 220 : 90, focused ? 80 : 130, 255);
        SDL_RenderRect(r, &nr);

        const auto lines = Layout();
        const int cl = CursorLine(lines);
        const int rows = VisibleRows();
        if (cl < scroll) scroll = cl;
        if (cl >= scroll + rows) scroll = cl - rows + 1;
        scroll = std::clamp(scroll, 0, std::max(0, static_cast<int>(lines.size()) - 1));
        SDL_SetRenderScale(r, SCALE, SCALE);
        SDL_SetRenderDrawColor(r, 225, 225, 235, 255);
        for (int i = scroll; i < static_cast<int>(lines.size()) && i < scroll + rows; ++i)
        {
            const std::string line = Ascii(text.substr(lines[i].first, lines[i].second - lines[i].first));
            SDL_RenderDebugText(r, (nr.x + PAD) / SCALE, (nr.y + PAD + (i - scroll) * LINE_H) / SCALE, line.c_str());
        }
        if (focused && std::fmod(blink, 1.0) < 0.55 && cl >= scroll && cl < scroll + rows)
        {
            const int col = Codepoints(text, lines[cl].first, cursor);
            SDL_SetRenderDrawColor(r, 255, 220, 80, 255);
            SDL_RenderDebugText(r, (nr.x + PAD + col * CHAR_W) / SCALE, (nr.y + PAD + (cl - scroll) * LINE_H + 2) / SCALE, "_");
        }

        SDL_SetRenderScale(r, 1.0f, 1.0f);
        SDL_SetRenderDrawColor(r, 150, 150, 170, 255);
        SDL_SetRenderScale(r, 1.5f, 1.5f);
        SDL_RenderDebugText(r, (x0 + 16) / 1.5f, (h - 58) / 1.5f, focused ? "Esc: back to the game" : "Click the box to type");
        SDL_SetRenderScale(r, 1.0f, 1.0f);
        if (message_t > 0 && !message.empty())
        {
            SDL_SetRenderDrawColor(r, 120, 230, 140, 255);
            SDL_RenderDebugText(r, static_cast<float>(x0 + 16), static_cast<float>(h - 30), message.c_str());
            message_t -= 1.0 / 60.0;
        }
        SDL_SetRenderScale(r, 1.0f, 1.0f);
    }

private:
    static constexpr float SCALE = 1.5f, CHAR_W = 12.0f, LINE_H = 15.0f, PAD = 8.0f;

    SDL_Renderer* renderer;
    std::function<int()> current_room;
    int x0, w, h, game_w;
    std::filesystem::path dir, notes_file;
    Hooks hooks;
    std::string room_field;
    bool field_focus = false;
    std::map<int, std::string> notes;
    int cur_room = -2;
    std::string text;
    std::size_t cursor = 0;
    int scroll = 0;
    bool focused = false, dirty = false;
    double blink = 0.0, save_timer = 0.0;
    std::string message;
    double message_t = 0.0;

    // capture
    bool capture_requested = false, selecting = false, dragging = false;
    SDL_Surface* frozen = nullptr;
    SDL_Texture* frozen_tex = nullptr;
    SDL_FPoint sel_a{}, sel_b{};

    static bool InRect(float x, float y, const SDL_FRect& r) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
    SDL_FRect ButtonRect() const { return { static_cast<float>(x0 + 16), 64.0f, static_cast<float>(w - 32), 44.0f }; }
    SDL_FRect NotesRect() const { return { static_cast<float>(x0 + 16), 214.0f, static_cast<float>(w - 32), static_cast<float>(h - 214 - 70) }; }
    // row of room controls: < [field] > GO ; row of health controls: HP- HP+ FULL
    SDL_FRect FieldRect() const { return { static_cast<float>(x0 + 62), 120.0f, 128.0f, 36.0f }; }
    SDL_FRect SmallBtn(int i) const
    {
        static const float xs[3] = { 16.0f, 198.0f, 244.0f }, ws[3] = { 38.0f, 38.0f, 70.0f };
        return { x0 + xs[i], 120.0f, ws[i], 36.0f };
    }
    SDL_FRect HpBtn(int i) const
    {
        static const float xs[3] = { 16.0f, 78.0f, 140.0f }, ws[3] = { 54.0f, 54.0f, 84.0f };
        return { x0 + xs[i], 166.0f, ws[i], 36.0f };
    }
    void LeaveField() { if (field_focus) { field_focus = false; room_field = std::to_string(cur_room); } }
    void GoField()
    {
        const int n = room_field.empty() ? cur_room : std::atoi(room_field.c_str());
        field_focus = false;
        if (hooks.go_room && hooks.go_room(n)) Say("Room " + std::to_string(n));
        else { Say("No room " + std::to_string(n)); room_field = std::to_string(cur_room); }
    }
    void StepRoom(int dir)
    {
        for (int k = 1; k <= 80; ++k)
        {
            const int n = cur_room + dir * k;
            if (n < 0) break;
            if (hooks.go_room && hooks.go_room(n)) { Say("Room " + std::to_string(n)); return; }
        }
    }
    int Columns() const { return static_cast<int>((w - 32 - 2 * PAD) / CHAR_W); }
    int VisibleRows() const { return static_cast<int>((NotesRect().h - 2 * PAD) / LINE_H); }

    // ---- text ----
    static bool IsCont(unsigned char c) { return (c & 0xC0) == 0x80; }
    static int Codepoints(const std::string& s, std::size_t from, std::size_t to)
    {
        int n = 0;
        for (std::size_t i = from; i < to && i < s.size(); ++i) if (!IsCont(static_cast<unsigned char>(s[i]))) ++n;
        return n;
    }
    static std::string Ascii(const std::string& s)
    {
        std::string out;
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x80) out += (c >= 32 ? static_cast<char>(c) : ' ');
            else if (!IsCont(c)) out += '?';
        }
        return out;
    }
    std::size_t Prev(std::size_t i) const { if (i == 0) return 0; --i; while (i > 0 && IsCont(static_cast<unsigned char>(text[i]))) --i; return i; }
    std::size_t Next(std::size_t i) const { if (i >= text.size()) return text.size(); ++i; while (i < text.size() && IsCont(static_cast<unsigned char>(text[i]))) ++i; return i; }

    // visual lines: [begin, end) byte offsets, hard-wrapped at the panel width
    std::vector<std::pair<std::size_t, std::size_t>> Layout() const
    {
        std::vector<std::pair<std::size_t, std::size_t>> lines;
        const int cols = std::max(8, Columns());
        std::size_t begin = 0, i = 0, last_space = std::string::npos;
        int col = 0;
        while (i < text.size())
        {
            if (text[i] == '\n') { lines.push_back({ begin, i }); begin = ++i; col = 0; last_space = std::string::npos; continue; }
            if (col >= cols)
            {
                // wrap at the last space of the line when there is one
                const std::size_t cut = (last_space != std::string::npos && last_space >= begin) ? last_space + 1 : i;
                lines.push_back({ begin, cut });
                begin = i = cut;
                col = 0;
                last_space = std::string::npos;
                continue;
            }
            if (text[i] == ' ') last_space = i;
            i = Next(i);
            ++col;
        }
        lines.push_back({ begin, text.size() });
        return lines;
    }
    int CursorLine(const std::vector<std::pair<std::size_t, std::size_t>>& lines) const
    {
        for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i)
            if (cursor >= lines[i].first) return i;
        return 0;
    }
    void PlaceCursor(float mx, float my)
    {
        const auto lines = Layout();
        const SDL_FRect nr = NotesRect();
        int row = scroll + static_cast<int>((my - nr.y - PAD) / LINE_H);
        row = std::clamp(row, 0, static_cast<int>(lines.size()) - 1);
        int col = std::max(0, static_cast<int>((mx - nr.x - PAD) / CHAR_W + 0.5f));
        std::size_t i = lines[row].first;
        while (col > 0 && i < lines[row].second) { i = Next(i); --col; }
        cursor = i;
    }
    void Insert(const char* s)
    {
        text.insert(cursor, s);
        cursor += std::strlen(s);
        Touched();
    }
    void Touched() { dirty = true; save_timer = 0.0; blink = 0.0; }
    void EditKey(const SDL_KeyboardEvent& k)
    {
        switch (k.key)
        {
        case SDLK_BACKSPACE:
            if (cursor > 0) { const std::size_t p = Prev(cursor); text.erase(p, cursor - p); cursor = p; Touched(); }
            break;
        case SDLK_DELETE:
            if (cursor < text.size()) { text.erase(cursor, Next(cursor) - cursor); Touched(); }
            break;
        case SDLK_RETURN: case SDLK_KP_ENTER: Insert("\n"); break;
        case SDLK_LEFT: cursor = Prev(cursor); blink = 0; break;
        case SDLK_RIGHT: cursor = Next(cursor); blink = 0; break;
        case SDLK_HOME: { const auto l = Layout(); cursor = l[CursorLine(l)].first; blink = 0; break; }
        case SDLK_END: { const auto l = Layout(); cursor = l[CursorLine(l)].second; blink = 0; break; }
        case SDLK_UP: case SDLK_DOWN:
        {
            const auto l = Layout();
            const int cl = CursorLine(l), target = cl + (k.key == SDLK_UP ? -1 : 1);
            if (target < 0 || target >= static_cast<int>(l.size())) break;
            int col = Codepoints(text, l[cl].first, cursor);
            std::size_t i = l[target].first;
            while (col > 0 && i < l[target].second) { i = Next(i); --col; }
            cursor = i;
            blink = 0;
            break;
        }
        case SDLK_V:
            if (k.mod & SDL_KMOD_CTRL)
            {
                if (char* clip = SDL_GetClipboardText())
                {
                    std::string s = clip;
                    SDL_free(clip);
                    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
                    text.insert(cursor, s);
                    cursor += s.size();
                    Touched();
                }
            }
            break;
        default: break;
        }
    }

    // ---- notes file ----
    void Commit()
    {
        if (cur_room < -1) return;
        if (text.empty()) notes.erase(cur_room);
        else notes[cur_room] = text;
    }
    static std::string Escape(const std::string& s)
    {
        std::string o;
        for (unsigned char c : s)
        {
            switch (c)
            {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 32) o += ' ';
                else o += static_cast<char>(c);
            }
        }
        return o;
    }
    void Save()
    {
        std::ofstream f(notes_file, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << "{\n";
        bool first = true;
        for (const auto& [room, t] : notes)
        {
            f << (first ? "" : ",\n") << "  \"" << room << "\": \"" << Escape(t) << "\"";
            first = false;
        }
        f << "\n}\n";
        dirty = false;
        save_timer = 0.0;
    }
    void Load()
    {
        std::ifstream f(notes_file, std::ios::binary);
        if (!f) return;
        std::stringstream ss;
        ss << f.rdbuf();
        try
        {
            const Json root = Json::Parse(ss.str());
            if (root.IsObject())
                for (const auto& [k, v] : root.object)
                    if (v.type == Json::Type::String) notes[std::atoi(k.c_str())] = v.string;
        }
        catch (const std::exception&) {}
    }

    // ---- capture ----
    void RequestCapture() { if (!selecting) capture_requested = true; }

    // Called from Draw() before the panel is painted: the game as it is on screen right now.
    void TakeFrozen()
    {
        capture_requested = false;
        if (frozen_tex) { SDL_DestroyTexture(frozen_tex); frozen_tex = nullptr; }
        if (frozen) { SDL_DestroySurface(frozen); frozen = nullptr; }
        SDL_Surface* all = SDL_RenderReadPixels(renderer, nullptr);
        if (!all) { Say("Capture failed"); return; }
        SDL_FRect lr{};
        SDL_Rect crop{ 0, 0, all->w, all->h };
        if (SDL_GetRenderLogicalPresentationRect(renderer, &lr))
            crop = SDL_Rect{ static_cast<int>(lr.x + 0.5f), static_cast<int>(lr.y + 0.5f), static_cast<int>(lr.w + 0.5f), static_cast<int>(lr.h + 0.5f) };
        crop.x = std::clamp(crop.x, 0, all->w - 1);
        crop.y = std::clamp(crop.y, 0, all->h - 1);
        crop.w = std::min(crop.w, all->w - crop.x);
        crop.h = std::min(crop.h, all->h - crop.y);
        SDL_Surface* part = SDL_CreateSurface(crop.w, crop.h, SDL_PIXELFORMAT_RGBA32);
        if (part) SDL_BlitSurface(all, &crop, part, nullptr);
        SDL_DestroySurface(all);
        if (!part) { Say("Capture failed"); return; }
        frozen = part;
        frozen_tex = SDL_CreateTextureFromSurface(renderer, frozen);
        selecting = frozen_tex != nullptr;
        dragging = false;
        sel_a = sel_b = { 0, 0 };
    }

    void CancelSelection()
    {
        selecting = dragging = false;
        if (frozen_tex) { SDL_DestroyTexture(frozen_tex); frozen_tex = nullptr; }
        if (frozen) { SDL_DestroySurface(frozen); frozen = nullptr; }
    }

    void DrawSelection(SDL_Renderer* r)
    {
        const float W = static_cast<float>(x0 + w), H = static_cast<float>(h);
        const SDL_FRect whole = { 0, 0, W, H };
        SDL_RenderTexture(r, frozen_tex, nullptr, &whole);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 150);
        const float rx = std::min(sel_a.x, sel_b.x), ry = std::min(sel_a.y, sel_b.y);
        const float rw = std::abs(sel_a.x - sel_b.x), rh = std::abs(sel_a.y - sel_b.y);
        if (dragging || rw > 0)
        {
            const SDL_FRect shades[4] = { { 0, 0, W, ry }, { 0, ry + rh, W, H - ry - rh }, { 0, ry, rx, rh }, { rx + rw, ry, W - rx - rw, rh } };
            for (const auto& s : shades) SDL_RenderFillRect(r, &s);
            SDL_SetRenderDrawColor(r, 255, 220, 60, 255);
            const SDL_FRect box = { rx, ry, rw, rh };
            SDL_RenderRect(r, &box);
        }
        else
        {
            const SDL_FRect all = { 0, 0, W, H };
            SDL_RenderFillRect(r, &all);
        }
        SDL_SetRenderDrawColor(r, 20, 20, 30, 220);
        const SDL_FRect banner = { 0, 0, W, 34 };
        SDL_RenderFillRect(r, &banner);
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        SDL_SetRenderScale(r, 2.0f, 2.0f);
        SDL_RenderDebugText(r, 12 / 2.0f, 9 / 2.0f, "Drag to choose the area   Enter: whole game   Esc: cancel");
        SDL_SetRenderScale(r, 1.0f, 1.0f);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    }

    void Finish(SDL_Rect logical)
    {
        if (!frozen) { CancelSelection(); return; }
        const double sx = static_cast<double>(frozen->w) / (x0 + w), sy = static_cast<double>(frozen->h) / h;
        SDL_Rect src{ static_cast<int>(logical.x * sx), static_cast<int>(logical.y * sy), std::max(1, static_cast<int>(logical.w * sx)),
                      std::max(1, static_cast<int>(logical.h * sy)) };
        src.w = std::min(src.w, frozen->w - src.x);
        src.h = std::min(src.h, frozen->h - src.y);
        SDL_Surface* out = SDL_CreateSurface(src.w, src.h, SDL_PIXELFORMAT_RGBA32);
        bool ok = false;
        std::string name;
        if (out && SDL_BlitSurface(frozen, &src, out, nullptr))
        {
            std::error_code ec;
            std::filesystem::create_directories(dir / "screenshots", ec);
            const std::time_t t = std::time(nullptr);
            char stamp[32];
            std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", std::localtime(&t));
            char file[96];
            std::snprintf(file, sizeof file, "room_%03d_%s.png", cur_room, stamp);
            name = std::string("screenshots/") + file;
            ok = SDL_SavePNG(out, (dir / "screenshots" / file).string().c_str());
        }
        if (out) SDL_DestroySurface(out);
        CancelSelection();
        if (ok)
        {
            if (!text.empty() && text.back() != '\n') text += '\n';
            text += "[img " + name + "]\n";
            cursor = text.size();
            Touched();
            Commit();
            Save();
            Say("Saved " + name);
        }
        else Say("Saving the picture failed");
    }

    void Say(const std::string& s) { message = Ascii(s); message_t = 6.0; }
};

#endif // _PLAYTEST_PANEL_H_
