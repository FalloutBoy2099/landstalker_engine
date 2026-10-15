#ifndef _SHOPS_H_
#define _SHOPS_H_

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include "core/Json.h"

// Shops, inns and churches (assets/text/shops.json, written by landstalker_tools/scripts/
// shop_export.py) and the base price of every item (assets/text/item_prices.txt, written
// by sprite_export).
//
// A shop's price is base * modifier / 16: the modifier is `markup` for ordinary items and
// services and `lifestock_markup` for Life Stock.
class Shops
{
public:
    struct Info
    {
        std::string name;
        std::string kind;       // shop / inn / church / other
        int markup = 16;
        int lifestock_markup = 16;
    };

    Shops(const std::filesystem::path& shops_json, const std::filesystem::path& prices_txt)
    {
        {
            std::ifstream f(shops_json, std::ios::binary);
            if (!f) throw std::runtime_error("Unable to open " + shops_json.string());
            std::stringstream ss;
            ss << f.rdbuf();
            const Json root = Json::Parse(ss.str());
            if (const Json* rooms = root.Find("rooms"))
            {
                for (const auto& [key, v] : rooms->object)
                {
                    Info info;
                    if (const Json* n = v.Find("name")) info.name = n->string;
                    if (const Json* k = v.Find("kind")) info.kind = k->string;
                    if (const Json* m = v.Find("markup")) info.markup = m->Int(16);
                    if (const Json* l = v.Find("lifestock_markup")) info.lifestock_markup = l->Int(16);
                    by_room[std::stoi(key)] = info;
                }
            }
        }
        std::ifstream pf(prices_txt);
        std::string line;
        while (std::getline(pf, line))
        {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ls(line);
            int index = 0, price = 0;
            if (ls >> index >> price) prices[index] = price;
        }
    }

    const Info* ForRoom(int room) const
    {
        auto it = by_room.find(room);
        return it == by_room.end() ? nullptr : &it->second;
    }

    // Base price of an item, -1 if the game does not give it one.
    int BasePrice(int item) const
    {
        auto it = prices.find(item);
        return it == prices.end() ? -1 : it->second;
    }

    static int Apply(int base, int modifier) { return base * modifier / 16; }

    static constexpr int LIFE_STOCK = 62;
    // What the shop of this room asks for an item.
    int ItemPrice(const Info& info, int item) const
    {
        const int base = BasePrice(item);
        if (base < 0) return -1;
        return Apply(base, item == LIFE_STOCK ? info.lifestock_markup : info.markup);
    }

    // One night at an inn: the original derives it from a table this port does not read
    // yet, so a base price is assumed and the inn's markup applied.
    static constexpr int INN_BASE_PRICE = 20;
    int InnPrice(const Info& info) const { return Apply(INN_BASE_PRICE, info.markup); }

private:
    std::map<int, Info> by_room;
    std::map<int, int> prices;
};

#endif // _SHOPS_H_
