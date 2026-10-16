#ifndef _CUTSCENES_H_
#define _CUTSCENES_H_

#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>
#include <sstream>
#include <string>
#include <vector>
#include "core/Json.h"

// Cutscene texts and custom script actions (assets/text/cutscenes.json, written by
// landstalker_tools/scripts/cutscene_export.py).
//
// An action is what the game's "TriggerCutscene" command or a character script runs: a short
// list of operations, read linearly from the original routine (see the exporter for what
// is and is not kept).
class Cutscenes
{
public:
    struct Op;
    using OpList = std::vector<Op>;

    struct Op
    {
        enum Kind { TEXT, FLAG, SOUND, SWAP, IF, WARP, LOCK, HOSTILE, VISIBLE, HIDE, BLOCK, FX, WAIT, FRIDAY, CREDITS } kind = TEXT;
        int value = 0;       // cutscene text id / flag / sound id / swap trigger
        bool set = true;     // FLAG: set or clear
        // IF: the condition (a story flag, or the answer to the yes/no question just asked)
        // and the two branches.
        bool on_answer = false;
        int wait = 0;        // TEXT: frames (1/60 s) on screen before it goes away by itself
        int x = 0, y = 0;    // WARP: arrival cell
        int dir = -1;        // WARP: orientation 0 NE 1 SE 2 SW 3 NW, -1 unchanged
        // HOSTILE / VISIBLE / HIDE: value = entity index (-1: the entity that triggered the
        // action), set = the new state. BLOCK: cells to make impassable.
        std::vector<std::pair<int, int>> cells;
        std::string name;    // FX: effect name (fade_out, white_flash...), FRIDAY: appear / hover / leave / hide
        std::shared_ptr<OpList> then_ops, else_ops;
    };

    // allowed: the characters the engine font can draw; anything else becomes '?'.
    Cutscenes(const std::filesystem::path& file, const std::string& allowed) : allowed(allowed)
    {
        std::ifstream f(file, std::ios::binary);
        if (!f) throw std::runtime_error("Unable to open " + file.string());
        std::stringstream ss;
        ss << f.rdbuf();
        root = Json::Parse(ss.str());
    }

    bool HasAction(int id) const
    {
        const Json* a = root.Find("actions");
        return a && a->Has(std::to_string(id));
    }

    // The operations of an action with the tail calls into other actions expanded.
    std::vector<Op> Action(int id) const
    {
        std::vector<Op> ops;
        Expand(id, ops, 0);
        return ops;
    }


    // The boxes of a cutscene text, formatted for the engine's font.
    std::vector<std::string> Text(int id) const
    {
        std::vector<std::string> boxes;
        const Json* t = root.Find("texts");
        const Json* node = t ? t->Find(std::to_string(id)) : nullptr;
        if (node)
            for (const auto& s : node->array)
                if (s.type == Json::Type::String) boxes.push_back(Format(s.string));
        return boxes;
    }

private:
    void Expand(int id, std::vector<Op>& out, int depth) const
    {
        if (depth > 6) return;
        const Json* a = root.Find("actions");
        const Json* list = a ? a->Find(std::to_string(id)) : nullptr;
        if (list) ExpandList(*list, out, depth);
    }

    void ExpandList(const Json& list, std::vector<Op>& out, int depth) const
    {
        for (const auto& o : list.array)
        {
            Op op;
            if (const Json* v = o.Find("text"))
            {
                // A cutscene id that is a routine of the original: run its ops in place.
                const Json* sc = root.Find("scripts");
                const Json* routine = sc ? sc->Find(std::to_string(v->Int())) : nullptr;
                if (routine && depth < 8) { ExpandList(*routine, out, depth + 1); continue; }
                op.kind = Op::TEXT;
                op.value = v->Int();
                if (const Json* w = o.Find("wait")) op.wait = w->Int();
                out.push_back(op);
            }
            else if (const Json* v = o.Find("flag"))
            {
                op.kind = Op::FLAG;
                op.value = v->Int();
                op.set = o.Find("set") ? o.Find("set")->boolean : true;
                out.push_back(op);
            }
            else if (const Json* v = o.Find("sound")) { op.kind = Op::SOUND; op.value = v->Int(); out.push_back(op); }
            else if (const Json* v = o.Find("swap")) { op.kind = Op::SWAP; op.value = v->Int(); out.push_back(op); }
            else if (const Json* v = o.Find("call")) Expand(v->Int(), out, depth + 1);
            else if (const Json* v = o.Find("warp"))
            {
                op.kind = Op::WARP;
                op.value = v->Int();
                if (const Json* x = o.Find("x")) op.x = x->Int();
                if (const Json* y = o.Find("y")) op.y = y->Int();
                if (const Json* d = o.Find("dir")) op.dir = d->Int(-1);
                out.push_back(op);
            }
            else if (const Json* v = o.Find("lock")) { op.kind = Op::LOCK; op.set = v->boolean; out.push_back(op); }
            else if (const Json* v = o.Find("hostile")) { op.kind = Op::HOSTILE; op.value = v->Int(-1); op.set = o.Find("set") ? o.Find("set")->boolean : true; out.push_back(op); }
            else if (const Json* v = o.Find("visible")) { op.kind = Op::VISIBLE; op.value = v->Int(-1); op.set = o.Find("set") ? o.Find("set")->boolean : true; out.push_back(op); }
            else if (o.Find("credits")) { op.kind = Op::CREDITS; out.push_back(op); }
            else if (const Json* v = o.Find("fx")) { op.kind = Op::FX; op.name = v->string; out.push_back(op); }
            else if (const Json* v = o.Find("wait")) { op.kind = Op::WAIT; op.value = v->Int(); out.push_back(op); }
            else if (const Json* v = o.Find("friday")) { op.kind = Op::FRIDAY; op.name = v->string; out.push_back(op); }
            else if (const Json* v = o.Find("hide")) { op.kind = Op::HIDE; op.value = v->Int(-1); out.push_back(op); }
            else if (const Json* v = o.Find("block"))
            {
                op.kind = Op::BLOCK;
                for (const auto& cell : v->array)
                    if (cell.array.size() == 2) op.cells.push_back({ cell.array[0].Int(), cell.array[1].Int() });
                out.push_back(op);
            }
            else if (const Json* c = o.Find("if"))
            {
                op.kind = Op::IF;
                if (const Json* f = c->Find("flag")) op.value = f->Int();
                else op.on_answer = true;
                op.then_ops = std::make_shared<OpList>();
                op.else_ops = std::make_shared<OpList>();
                if (const Json* t = o.Find("then")) ExpandList(*t, *op.then_ops, depth + 1);
                if (const Json* e = o.Find("else")) ExpandList(*e, *op.else_ops, depth + 1);
                out.push_back(op);
            }
        }
    }

    // Same control codes as the dialogues: {57} line break, {5F} the player's name.
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
            else if (c == '\t') r += ' ';
            else if (c == '\n' || allowed.find(c) != std::string::npos) r += c;
            else r += '?';
        }
        return r;
    }

    std::string allowed;
    Json root;
};

#endif // _CUTSCENES_H_
