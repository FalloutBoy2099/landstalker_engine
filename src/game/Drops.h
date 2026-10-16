#ifndef _DROPS_H_
#define _DROPS_H_

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

// What the enemies leave behind (assets/text/enemy_drops.txt, written by
// landstalker_tools/scripts/drops_export.py): an item with one chance in N, or gold.
class Drops
{
public:
    struct Drop { int item = 0; int one_in = 0; };

    explicit Drops(const std::filesystem::path& txt)
    {
        std::ifstream f(txt);
        std::string line;
        while (std::getline(f, line))
        {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ls(line);
            int type = 0;
            Drop d;
            if (ls >> type >> d.item >> d.one_in) table[type] = d;
        }
    }

    const Drop* ForEnemy(int type) const
    {
        auto it = table.find(type);
        return it == table.end() ? nullptr : &it->second;
    }

private:
    std::map<int, Drop> table;
};

#endif // _DROPS_H_
