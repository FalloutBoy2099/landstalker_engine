#ifndef _TRIGGERS_H_
#define _TRIGGERS_H_

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// The "trigger actions" of the original (WaitForCondition in the sprite scripts): small routines that
// compare where the player stands, or test story flags, and say whether the condition holds yet.
// assets/text/triggers.txt holds them as tiny programs (written by landstalker_tools/scripts/triggers_export.py);
// this class runs them against a view of the game given as a function:
//     get(variable, size) -> value, with variable one of the Var enum (flags: FLAG0 + byte).
class Triggers
{
public:
    enum Var { PX, PY, PZ, SX, SY, FH, ROT, FALL, PLAYBACK, FLAG0 = 100 };
    using Getter = std::function<int(int var, char size)>;

    explicit Triggers(const std::filesystem::path& file)
    {
        std::ifstream f(file);
        std::string line;
        int current = -1;
        while (std::getline(f, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            if (line.rfind("TA ", 0) == 0) { current = std::stoi(line.substr(3)); programs[current]; continue; }
            if (current < 0) continue;
            std::istringstream ss(line);
            Ins ins;
            std::string op, a, b, c;
            ss >> op;
            if (!op.empty() && op.back() == ':') { ins.op = LABEL; ins.target = op.substr(0, op.size() - 1); programs[current].push_back(ins); continue; }
            if (op == "cmp") { ins.op = CMP; ss >> a >> b >> c; ins.size = a[0]; ins.var = ParseVar(b); ins.imm = std::stoi(c); }
            else if (op == "tst") { ins.op = TST; ss >> a >> b; ins.size = a[0]; ins.var = ParseVar(b); }
            else if (op == "btst") { ins.op = BTST; ss >> a >> b; ins.imm = std::stoi(a); ins.var = ParseVar(b); ins.size = 'b'; }
            else if (op == "load") { ins.op = LOAD; ss >> a; ins.var = ParseVar(a); ins.size = 'b'; }
            else if (op == "andd0") { ins.op = ANDD0; ss >> a; ins.imm = std::stoi(a); }
            else if (op == "cmpd0") { ins.op = CMPD0; ss >> a; ins.imm = std::stoi(a); }
            else { ins.op = BRANCH; ins.cond = op; ss >> ins.target; }
            programs[current].push_back(ins);
        }
    }

    bool Has(int id) const { return programs.count(id) > 0; }

    // Does the condition hold? (Routines that fall off their end do not hold.)
    bool Evaluate(int id, const Getter& get) const
    {
        const auto it = programs.find(id);
        if (it == programs.end()) return true;
        const auto& prog = it->second;
        bool N = false, Z = false, C = false;
        int d0 = 0;
        auto size_mask = [](char size) { return size == 'w' ? 0xFFFF : 0xFF; };
        auto sign_bit = [](char size) { return size == 'w' ? 0x8000 : 0x80; };
        std::size_t pc = 0;
        for (int guard = 0; guard < 500 && pc < prog.size(); ++guard)
        {
            const Ins& i = prog[pc];
            switch (i.op)
            {
            case LABEL: break;
            case CMP:
            {
                const int mask = size_mask(i.size);
                const int v = get(i.var, i.size) & mask, imm = i.imm & mask;
                const int res = (v - imm) & mask;
                Z = res == 0; C = v < imm; N = (res & sign_bit(i.size)) != 0;
                break;
            }
            case TST:
            {
                const int v = get(i.var, i.size) & size_mask(i.size);
                Z = v == 0; N = (v & sign_bit(i.size)) != 0; C = false;
                break;
            }
            case BTST: Z = ((get(i.var, 'b') >> i.imm) & 1) == 0; break;
            case LOAD: d0 = get(i.var, 'b') & 0xFF; Z = d0 == 0; N = (d0 & 0x80) != 0; C = false; break;
            case ANDD0: d0 &= i.imm & 0xFF; Z = d0 == 0; N = (d0 & 0x80) != 0; C = false; break;
            case CMPD0:
            {
                const int imm = i.imm & 0xFF;
                const int res = (d0 - imm) & 0xFF;
                Z = res == 0; C = d0 < imm; N = (res & 0x80) != 0;
                break;
            }
            case BRANCH:
            {
                bool take = false;
                const std::string& c = i.cond;
                if (c == "bra") take = true;
                else if (c == "beq") take = Z;
                else if (c == "bne") take = !Z;
                else if (c == "bcc") take = !C;
                else if (c == "bcs") take = C;
                else if (c == "bhi") take = !C && !Z;
                else if (c == "bls") take = C || Z;
                else if (c == "bmi") take = N;
                else if (c == "bpl") take = !N;
                if (!take) break;
                if (i.target == "done") return true;
                if (i.target == "notdone") return false;
                bool found = false;
                for (std::size_t k = 0; k < prog.size(); ++k)
                    if (prog[k].op == LABEL && prog[k].target == i.target) { pc = k; found = true; break; }
                if (!found) return false;
                break;
            }
            }
            ++pc;
        }
        return false;
    }

private:
    enum Op { CMP, TST, BTST, LOAD, ANDD0, CMPD0, BRANCH, LABEL };
    struct Ins { Op op = LABEL; char size = 'b'; int var = 0; int imm = 0; std::string cond, target; };

    static int ParseVar(const std::string& s)
    {
        if (s == "PX") return PX;
        if (s == "PY") return PY;
        if (s == "PZ") return PZ;
        if (s == "SX") return SX;
        if (s == "SY") return SY;
        if (s == "FH") return FH;
        if (s == "ROT") return ROT;
        if (s == "FALL") return FALL;
        if (s == "PLAYBACK") return PLAYBACK;
        if (s.rfind("FLAG", 0) == 0) return FLAG0 + std::stoi(s.substr(4));
        return PX;
    }

    std::map<int, std::vector<Ins>> programs;
};

#endif // _TRIGGERS_H_
