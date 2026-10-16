#ifndef _GAME_STATE_H_
#define _GAME_STATE_H_

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <cmath>
#include <cstdlib>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// What the player has done so far: inventory, gold, items picked up and chests opened.
// Lives for the whole session (it is not reset when changing room).
class GameState
{
public:
    // items_txt: one item name per line, as written by landstalker_tools' strings tool.
    explicit GameState(const std::filesystem::path& items_txt)
    {
        std::ifstream f(items_txt);
        std::string line;
        while (std::getline(f, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            names.push_back(Clean(line));
        }
    }

    std::string ItemName(int index) const
    {
        if (index >= 0 && index < static_cast<int>(names.size()) && !names[index].empty())
            return names[index];
        return "Item " + std::to_string(index);
    }

    // Item indices with an immediate or special effect (see Item Names.txt).
    static constexpr int ITEM_EKEEKE = 0;
    static constexpr int ITEM_LIFE_STOCK = 62;
    static constexpr int ITEM_KEY = 50;
    static constexpr int LIFE_STOCK_HP = 1;      // max health gained per Life Stock (the original adds 0x0100)
    // Health is kept in 1/256 of a point, like the original (8.8 fixed point). Nigel starts with 4
    // points (the HUD's "4/ 4"), a Life Stock adds one, an enemy hits for its attack value.
    static constexpr int START_HEALTH = 4;
    static constexpr int MAX_HEALTH_CAP = 99;

    // Equipment. Swords: Magic Sword (1), Sword of Ice (2), Thunder Sword (3), Sword of Gaia (4).
    // Armour: Steel (9), Chrome (10), Shell (11), Hyper (12) Breast. The stat values are
    // assumptions (the original's damage formulas are not ported).
    static bool IsSword(int item) { return item >= 1 && item <= 4; }
    static bool IsArmour(int item) { return item >= 9 && item <= 12; }
    // Boots: Fireproof (5, lava), Iron (6, spikes), Healing (7, slowly heals), Snow Spikes (8, ice).
    static bool IsBoots(int item) { return item >= 5 && item <= 8; }
    static constexpr int BOOTS_FIREPROOF = 5, BOOTS_IRON = 6, BOOTS_HEALING = 7, BOOTS_SNOW = 8;
    int EquippedBoots() const { return equipped_boots; }
    static constexpr int BASE_ATTACK = 8;
    int EquippedSword() const { return equipped_sword; }
    int EquippedArmour() const { return equipped_armour; }
    // Toggles the item: equips it, or unequips it if it is the one worn. Returns true if worn now.
    bool ToggleEquip(int item)
    {
        int& slot = IsSword(item) ? equipped_sword : IsBoots(item) ? equipped_boots : equipped_armour;
        if (slot == item) { slot = -1; return false; }
        slot = item;
        return true;
    }
    // ---- Combat formulas of the original (code/gamelogic/combat.asm, 8.8 fixed point) ----
    // Armour scales the damage taken: none 1.0, Steel 0.9, Chrome 0.8, Shell 0.7, Hyper 0.5.
    double ArmourMultiplier() const
    {
        static const double mult[5] = { 1.0, 0.9, 0.8, 0.7, 0.5 };
        return equipped_armour >= 9 && equipped_armour <= 12 ? mult[equipped_armour - 8] : 1.0;
    }
    // Damage an enemy attack of strength `attack` does to the player: the strength times the
    // armour factor, plus its integer part times a random 0..95/256.
    double RollEnemyDamage(int attack) const
    {
        const double a = (attack & 0x7F) * ArmourMultiplier();
        const double dmg = a + std::floor(a) * (std::rand() % 96) / 256.0;
        return std::round(dmg * 256.0) / 256.0;
    }
    // Damage the player's sword does to an enemy of defence byte `defence` (the original keeps
    // half of it): a quarter of the maximum health minus that, at least 1, plus the same
    // random bonus. The sword type does not enter (only the charged strike differs).
    double RollPlayerDamage(int defence) const
    {
        const double d = std::max(1.0, max_health / 256.0 / 4.0 - defence / 2.0);
        return d + std::floor(d) * (std::rand() % 96) / 256.0;
    }

    // Life Stock is consumed on pickup: it raises max health and refills it.
    void AddItem(int index)
    {
        if (index == ITEM_LIFE_STOCK)
        {
            max_health = std::min(MAX_HEALTH_CAP * 256, max_health + LIFE_STOCK_HP * 256);
            health = std::min(max_health, health + LIFE_STOCK_HP * 256);
            return;
        }
        ++items[index];
    }
    bool UseItem(int index)
    {
        auto it = items.find(index);
        if (it == items.end() || it->second <= 0) return false;
        --it->second;
        return true;
    }
    // Uses one EkeEke if there is one and health is not full. Returns whether it was used. It
    // restores half the maximum health, at least 10 points (EkeEkeHealthRecover).
    bool UseEkeEke()
    {
        if (ItemCount(ITEM_EKEEKE) <= 0 || health >= max_health) return false;
        --items[ITEM_EKEEKE];
        health = std::min(max_health, health + std::max(max_health / 2, 10 * 256));
        return true;
    }
    // Damage to the player, in points. Returns true if this killed him.
    bool Damage(double amount)
    {
        health = std::max(0, health - std::max(0, static_cast<int>(std::lround(amount * 256.0))));
        return health == 0;
    }
    bool IsDead() const { return health <= 0; }
    void Revive() { health = max_health; status = 0; }

    // Back to the start of a new adventure (the item names are kept).
    void Reset()
    {
        items.clear();
        flags.clear();
        collected.clear();
        enemy_health.clear();
        opened.clear();
        gold = 0;
        progress = 0;
        health = max_health = START_HEALTH * 256;
        equipped_sword = equipped_armour = equipped_boots = -1;
        status = 0;
    }

    // Status ailments (bit numbers of the original's g_PlayerStatus).
    static constexpr int STATUS_POISON = 0, STATUS_CONFUSION = 1, STATUS_PARALYSIS = 2, STATUS_CURSE = 3;
    bool HasStatus(int s) const { return (status >> s) & 1; }
    bool HasAnyStatus() const { return (status & 0x0F) != 0; }
    void AddStatus(int s) { status |= static_cast<unsigned char>(1u << s); }
    void ClearStatus(int s) { status &= static_cast<unsigned char>(~(1u << s)); }
    void ClearAllStatus() { status = 0; }

    // Remaining health of an enemy (room, entity index) in 1/256 of a point, like the original
    // (8.8 fixed point); initialised on first use from `initial` points.
    int& EnemyHealth(int room, int entity, int initial)
    {
        auto it = enemy_health.find({ room, entity });
        if (it == enemy_health.end()) it = enemy_health.emplace(std::make_pair(room, entity), initial * 256).first;
        return it->second;
    }

    // Whole points as the HUD shows them (a started point counts: (health + 256) >> 8).
    int GetHealth() const { return health <= 0 ? 0 : (health + 255) / 256; }
    int GetMaxHealth() const { return max_health / 256; }
    int GetHealthRaw() const { return health; }
    int GetMaxHealthRaw() const { return max_health; }
    int ItemCount(int index) const { auto it = items.find(index); return it == items.end() ? 0 : it->second; }
    const std::map<int, int>& GetItems() const { return items; }
    void AddGold(int amount) { gold += amount; }
    bool SpendGold(int amount)
    {
        if (amount < 0 || gold < amount) return false;
        gold -= amount;
        return true;
    }
    void HealFull() { health = max_health; }
    void Heal(double amount) { health = std::min(max_health, health + static_cast<int>(std::lround(amount * 256.0))); }
    int GetGold() const { return gold; }

    // Story flags: bit indices as in the original game (byte * 8 + bit). A new game has none set.
    bool IsFlagSet(int flag) const { return flags.count(flag) > 0; }
    void SetFlag(int flag) { flags.insert(flag); }
    void ClearFlag(int flag) { flags.erase(flag); }
    const std::set<int>& GetFlags() const { return flags; }
    // Story progress: the highest value reached (it never goes back).
    int GetProgress() const { return progress; }
    void RaiseProgress(int value) { progress = std::max(progress, value); }

    // (room, entity index within the room's entity list)
    bool IsCollected(int room, int entity) const { return collected.count({ room, entity }) > 0; }
    void MarkCollected(int room, int entity) { collected.insert({ room, entity }); }
    bool IsChestOpened(int room, int entity) const { return opened.count({ room, entity }) > 0; }
    void MarkChestOpened(int room, int entity) { opened.insert({ room, entity }); }

    // ---- Save games ----------------------------------------------------------------------
    // Plain text, one "key value..." line per field. `extra` lines (room, position...) come
    // from the caller and are written verbatim after the header.
    bool Save(const std::filesystem::path& path, const std::string& extra) const
    {
        std::ofstream f(path);
        if (!f) return false;
        f << "landstalker-save 1\n";
        f << extra;
        f << "status " << static_cast<int>(status) << "\n";
        f << "gold " << gold << "\nhealth " << health << "\nmaxhealth " << max_health
          << "\nprogress " << progress << "\nsword " << equipped_sword << "\narmour " << equipped_armour << "\n";
        f << "items";
        for (const auto& [i, c] : items) f << ' ' << i << ':' << c;
        f << "\nflags";
        for (int fl : flags) f << ' ' << fl;
        f << "\ncollected";
        for (const auto& [r, e] : collected) f << ' ' << r << ':' << e;
        f << "\nopened";
        for (const auto& [r, e] : opened) f << ' ' << r << ':' << e;
        f << "\nenemies";
        for (const auto& [k, hp] : enemy_health) f << ' ' << k.first << ':' << k.second << ':' << hp;
        f << "\n";
        return static_cast<bool>(f);
    }

    // Replaces the whole state with the file's. Lines this class does not know (the caller's
    // `extra`) are returned in `other`. On failure the state is left untouched.
    bool Load(const std::filesystem::path& path, std::map<std::string, std::string>& other)
    {
        std::ifstream f(path);
        std::string line;
        if (!f || !std::getline(f, line) || line.rfind("landstalker-save 1", 0) != 0) return false;
        GameState s(*this);   // work on a copy
        s.status = 0;
        s.items.clear(); s.flags.clear(); s.collected.clear(); s.opened.clear(); s.enemy_health.clear();
        auto pairs = [](const std::string& rest, auto&& fn) {
            std::istringstream is(rest);
            std::string tok;
            while (is >> tok)
            {
                for (char& c : tok) if (c == ':') c = ' ';
                std::istringstream ts(tok);
                int a = 0, b = 0, c = 0;
                ts >> a >> b >> c;
                fn(a, b, c);
            }
        };
        while (std::getline(f, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto sp = line.find(' ');
            const std::string key = line.substr(0, sp);
            const std::string rest = sp == std::string::npos ? "" : line.substr(sp + 1);
            if (key == "status") s.status = static_cast<unsigned char>(std::atoi(rest.c_str()) & 0x0F);
            else if (key == "gold") s.gold = std::atoi(rest.c_str());
            else if (key == "health256") s.health = std::atoi(rest.c_str());
            else if (key == "maxhealth256") s.max_health = std::atoi(rest.c_str());
            else if (key == "progress") s.progress = std::atoi(rest.c_str());
            else if (key == "sword") s.equipped_sword = std::atoi(rest.c_str());
            else if (key == "armour") s.equipped_armour = std::atoi(rest.c_str());
            else if (key == "boots") s.equipped_boots = std::atoi(rest.c_str());
            else if (key == "items") pairs(rest, [&](int a, int b, int) { s.items[a] = b; });
            else if (key == "flags") pairs(rest, [&](int a, int, int) { s.flags.insert(a); });
            else if (key == "collected") pairs(rest, [&](int a, int b, int) { s.collected.insert({ a, b }); });
            else if (key == "opened") pairs(rest, [&](int a, int b, int) { s.opened.insert({ a, b }); });
            else if (key == "enemies") pairs(rest, [&](int a, int b, int c) { s.enemy_health[{ a, b }] = c; });
            else if (!key.empty()) other[key] = rest;
        }
        s.max_health = std::clamp(s.max_health, START_HEALTH * 256, MAX_HEALTH_CAP * 256);
        s.health = std::clamp(s.health, 1, s.max_health);
        s.health = std::min(s.health, s.max_health);
        *this = std::move(s);
        return true;
    }

private:
    // Item names contain control codes such as {6A} (line break) or {6B} (soft hyphen).
    static std::string Clean(const std::string& s)
    {
        std::string r;
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '{')
            {
                const auto end = s.find('}', i);
                if (end == std::string::npos) break;
                const std::string code = s.substr(i + 1, end - i - 1);
                if (code == "6A") r += ' ';
                i = end;
            }
            else
            {
                r += s[i];
            }
        }
        return r;
    }

    std::vector<std::string> names;
    std::map<int, int> items;
    int gold = 0;
    unsigned char status = 0;   // bit per ailment (poison, confusion, paralysis, curse)
    std::set<int> flags;
    int progress = 0;
    int health = START_HEALTH * 256;       // 1/256 of a point
    int equipped_sword = -1;
    int equipped_armour = -1;
    int equipped_boots = -1;
    int max_health = START_HEALTH * 256;
    std::set<std::pair<int, int>> collected;
    std::map<std::pair<int, int>, int> enemy_health;
    std::set<std::pair<int, int>> opened;
};

#endif // _GAME_STATE_H_
