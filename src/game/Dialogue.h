#ifndef _DIALOGUE_H_
#define _DIALOGUE_H_

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "core/Json.h"
#include "game/GameState.h"

// Character dialogues, loaded from assets/text/dialogue.json (written by
// landstalker_tools/scripts/dialogue_export.py).
//
// A character is a tree of nodes that mirror the helpers of the original scripts:
//   messages          boxes to show
//   progress          the last entry whose story progress >= min wins
//   set_flag_on_talk  first talk sets the flag and shows `clear`, later talks show `set`
//   check_flag        shows `set` or `clear` depending on a flag (does not change it)
//   yesno             prompt, then `yes` or `no` depending on the player's answer
class Dialogue
{
public:
    // The boxes of a conversation, and the follow-ups if it ended on a yes/no question.
    struct Page
    {
        std::string text;
        int give_item = -1;   // item received when this box is dismissed (-1: none)
    };

    struct Conversation
    {
        std::vector<Page> pages;
        const Json* yes = nullptr;
        const Json* no = nullptr;
        bool asks = false;
        std::vector<int> give_now;   // items given as soon as the conversation starts
    };

    // allowed: the characters the engine font can draw; anything else becomes '?'.
    Dialogue(const std::filesystem::path& file, const std::string& allowed) : allowed(allowed)
    {
        std::ifstream f(file, std::ios::binary);
        if (!f) throw std::runtime_error("Unable to open " + file.string());
        std::stringstream ss;
        ss << f.rdbuf();
        root = Json::Parse(ss.str());
        if (const Json* pf = root.Find("progress_flags"))
            for (const auto& p : pf->array)
                if (p.array.size() == 2) progress_table.push_back({ p.array[0].Int(), p.array[1].Int() });
    }

    bool Has(int character) const
    {
        const Json* chars = root.Find("characters");
        return chars && chars->Has(std::to_string(character));
    }

    // Story progress: value of the first flag of the progress table that is set. It only
    // ever goes up, like the game's progress byte.
    int Progress(GameState& gs) const
    {
        for (const auto& [flag, value] : progress_table)
        {
            if (gs.IsFlagSet(flag))
            {
                gs.RaiseProgress(value);
                break;
            }
        }
        return gs.GetProgress();
    }

    // Starts talking to a character; may set story flags (set_flag_on_talk).
    Conversation Begin(int character, GameState& gs) const
    {
        Conversation c;
        const Json* chars = root.Find("characters");
        const Json* node = chars ? chars->Find(std::to_string(character)) : nullptr;
        if (node) Resolve(*node, gs, c, 0);
        return c;
    }

    // Continues with a node returned in Conversation::yes / ::no.
    Conversation Follow(const Json* node, GameState& gs) const
    {
        Conversation c;
        if (node) Resolve(*node, gs, c, 0);
        return c;
    }

private:
    void Resolve(const Json& n, GameState& gs, Conversation& c, int depth) const
    {
        if (depth > 16 || !n.IsObject()) return;
        if (const Json* m = n.Find("messages"))
        {
            for (const auto& s : m->array)
            {
                if (s.type == Json::Type::String)
                {
                    c.pages.push_back({ Format(s.string), -1 });
                }
                else if (const Json* g = s.Find("give_item"))
                {
                    // Received after the box before it; before any box: right away.
                    if (!c.pages.empty()) c.pages.back().give_item = g->Int();
                    else c.give_now.push_back(g->Int());
                }
            }
        }
        else if (const Json* pr = n.Find("progress"))
        {
            const int progress = Progress(gs);
            const Json* chosen = nullptr;
            for (const auto& e : pr->array)
            {
                const int index = e.Find("index") ? e.Find("index")->Int() : 0;
                const int min = e.Find("min") ? e.Find("min")->Int() : 0;
                const int value = (index == 0) ? progress : 0;   // only the story progress is tracked
                if (value >= min) chosen = e.Find("node");
            }
            // Nothing matched (should not happen: the first entry has min 0): use the first.
            if (!chosen && !pr->array.empty()) chosen = pr->array.front().Find("node");
            if (chosen) Resolve(*chosen, gs, c, depth + 1);
        }
        else if (const Json* f = n.Find("set_flag_on_talk"))
        {
            const int flag = f->Int();
            if (!gs.IsFlagSet(flag))
            {
                gs.SetFlag(flag);
                if (const Json* sub = n.Find("clear")) Resolve(*sub, gs, c, depth + 1);
            }
            else if (const Json* sub = n.Find("set"))
            {
                Resolve(*sub, gs, c, depth + 1);
            }
        }
        else if (const Json* f = n.Find("check_flag"))
        {
            const Json* sub = n.Find(gs.IsFlagSet(f->Int()) ? "set" : "clear");
            if (sub) Resolve(*sub, gs, c, depth + 1);
        }
        else if (const Json* yn = n.Find("yesno"))
        {
            if (const Json* prompt = yn->Find("prompt")) Resolve(*prompt, gs, c, depth + 1);
            c.asks = true;
            c.yes = yn->Find("yes");
            c.no = yn->Find("no");
        }
    }

    // Control codes seen in dialogue: {57} line break, {5F} player name, {5A} a number;
    // {5B} {5E} {58} {62} {59} are box start / end / prompt / pause: nothing to draw.
    std::string Format(const std::string& s) const
    {
        std::string r;
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            const char c = s[i];
            if (c == '{')
            {
                const auto end = s.find('}', i);
                if (end == std::string::npos) break;
                const std::string code = s.substr(i + 1, end - i - 1);
                if (code == "57") r += '\n';
                else if (code == "5F") r += "Nigel";
                else if (code == "5A") r += "?";
                i = end;
            }
            else if (c == '\t')
            {
                r += ' ';
            }
            else if (c == '\n' || allowed.find(c) != std::string::npos)
            {
                r += c;
            }
            else
            {
                r += '?';
            }
        }
        return r;
    }

    std::string allowed;
    Json root;
    std::vector<std::pair<int, int>> progress_table;   // (flag, progress value), first set wins
};

#endif // _DIALOGUE_H_
