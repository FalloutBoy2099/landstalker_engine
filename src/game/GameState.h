#ifndef _GAME_STATE_H_
#define _GAME_STATE_H_

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
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
    static constexpr int LIFE_STOCK_HP = 10;     // max health gained per Life Stock (assumed)
    static constexpr int EKEEKE_HEAL_HP = 50;    // health restored by an EkeEke (assumed)
    static constexpr int START_HEALTH = 100;
    static constexpr int MAX_HEALTH_CAP = 200;

    // Equipment. Swords: Magic Sword (1), Sword of Ice (2), Thunder Sword (3), Sword of Gaia (4).
    // Armour: Steel (9), Chrome (10), Shell (11), Hyper (12) Breast. The stat values are
    // assumptions (the original's damage formulas are not ported).
    static bool IsSword(int item) { return item >= 1 && item <= 4; }
    static bool IsArmour(int item) { return item >= 9 && item <= 12; }
    static constexpr int BASE_ATTACK = 8;
    int EquippedSword() const { return equipped_sword; }
    int EquippedArmour() const { return equipped_armour; }
    // Toggles the item: equips it, or unequips it if it is the one worn. Returns true if worn now.
    bool ToggleEquip(int item)
    {
        int& slot = IsSword(item) ? equipped_sword : equipped_armour;
        if (slot == item) { slot = -1; return false; }
        slot = item;
        return true;
    }
    int Attack() const { return BASE_ATTACK + (equipped_sword >= 0 ? 4 * equipped_sword : 0); }
    int Defence() const { return equipped_armour >= 0 ? 2 * (equipped_armour - 8) : 0; }

    // Life Stock is consumed on pickup: it raises max health and refills it.
    void AddItem(int index)
    {
        if (index == ITEM_LIFE_STOCK)
        {
            max_health = std::min(MAX_HEALTH_CAP, max_health + LIFE_STOCK_HP);
            health = max_health;
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
    // Uses one EkeEke if there is one and health is not full. Returns whether it was used.
    bool UseEkeEke()
    {
        if (ItemCount(ITEM_EKEEKE) <= 0 || health >= max_health) return false;
        --items[ITEM_EKEEKE];
        health = std::min(max_health, health + EKEEKE_HEAL_HP);
        return true;
    }
    // Damage to the player. Returns true if this killed him.
    bool Damage(int amount)
    {
        health = std::max(0, health - std::max(0, amount));
        return health == 0;
    }
    bool IsDead() const { return health <= 0; }
    void Revive() { health = max_health; }

    // Remaining health of an enemy (room, entity index); initialised on first use.
    int& EnemyHealth(int room, int entity, int initial)
    {
        auto it = enemy_health.find({ room, entity });
        if (it == enemy_health.end()) it = enemy_health.emplace(std::make_pair(room, entity), initial).first;
        return it->second;
    }

    int GetHealth() const { return health; }
    int GetMaxHealth() const { return max_health; }
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
    std::set<int> flags;
    int progress = 0;
    int health = START_HEALTH;
    int equipped_sword = -1;
    int equipped_armour = -1;
    int max_health = START_HEALTH;
    std::set<std::pair<int, int>> collected;
    std::map<std::pair<int, int>, int> enemy_health;
    std::set<std::pair<int, int>> opened;
};

#endif // _GAME_STATE_H_
