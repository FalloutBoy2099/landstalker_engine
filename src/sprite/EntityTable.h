#ifndef _ENTITY_TABLE_H_
#define _ENTITY_TABLE_H_

#include <SDL3/SDL.h>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "graphics/Graphics.h"

// Entity id -> how to draw it, loaded from <assets>/sprites/entities.yaml
// (written by landstalker_tools' sprite_export). Images are loaded on first use.
class EntityTable
{
public:
    struct Image
    {
        SDL_Texture* texture = nullptr;
        int width = 0, height = 0;
        int anchor_x = 0, anchor_y = 0;   // pixel sitting on the entity position
    };

    EntityTable(std::shared_ptr<Graphics> graphics, const std::filesystem::path& sprites_dir);
    ~EntityTable();

    EntityTable(const EntityTable&) = delete;
    EntityTable& operator=(const EntityTable&) = delete;

    bool Has(int entity) const { return defs.count(entity) > 0; }

    struct Enemy { int health = 0, defence = 0, attack = 0, gold_drop = 0; };
    // Stats if the entity is an enemy (health > 0), otherwise nullptr.
    const Enemy* GetEnemy(int entity) const
    {
        auto it = defs.find(entity);
        return (it != defs.end() && it->second.is_enemy) ? &it->second.enemy : nullptr;
    }
    int GetHitboxBase(int entity) const;
    int GetSprite(int entity) const;   // -1 if unknown
    int GetHitboxHeight(int entity) const;

    // Image for an entity facing `orientation` (0=NE 1=SE 2=SW 3=NW, as in room.yaml).
    // `hflip` tells whether it must be mirrored. Returns nullptr if there is none.
    const Image* GetImage(int entity, int orientation, bool& hflip);

    // Which of the three images applies: 0 = default, 1 = back (NE/NW), 2 = front (SE/SW).
    // Also tells whether to mirror. Used with room-specific images (see RoomView).
    enum Kind { DEFAULT = 0, BACK = 1, FRONT = 2 };

    // Walk cycle frames for a kind (BACK or FRONT); empty if the entity has none.
    // Each frame carries its own anchor.
    const std::vector<Image>& GetWalkFrames(int entity, Kind kind);
    // The same for the attack swing and the hurt pose of the enemies.
    enum Pose { WALK = 0, ATTACK = 1, HURT = 2 };
    const std::vector<Image>& GetPoseFrames(int entity, Pose pose, Kind kind);
    Kind GetKind(int entity, int orientation, bool& hflip) const;

private:
    struct Def
    {
        int sprite = -1;
        bool front_and_back = false;
        bool is_enemy = false;
        Enemy enemy;
        int base = 0, height = 0;
        struct Slot { std::string file; int w = 0, h = 0, ax = 0, ay = 0; Image image; bool loaded = false; };
        Slot def, back, front;
        std::vector<Slot> walk_back, walk_front;   // one slot per frame
        std::vector<Slot> attack_back, attack_front, hurt_back, hurt_front;
    };

    const Image* Load(Def::Slot& slot);

    std::shared_ptr<Graphics> graphics;
    std::filesystem::path dir;
    std::map<int, Def> defs;
};

#endif // _ENTITY_TABLE_H_
