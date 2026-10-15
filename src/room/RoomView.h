#ifndef _ROOM_VIEW_H_
#define _ROOM_VIEW_H_

#include <SDL3/SDL.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "graphics/IDrawable.h"
#include "graphics/Graphics.h"
#include "core/IKeyInput.h"
#include "sprite/SpriteSet.h"
#include "sprite/EntityTable.h"
#include "game/GameState.h"
#include "game/Dialogue.h"
#include "game/Shops.h"

// A warp as seen from the room currently loaded: the cells it covers and where it leads.
struct RoomWarp
{
    int x, y;              // first covered cell, world cell coordinates
    int w, h;              // size in cells
    int dest_room;
    int dest_x, dest_y;    // arrival cell in the destination room
    std::string type;      // normal / stair_se / stair_sw / unknown
};

// An entity placed in the room (from room.yaml). Coordinates are in cells.
// x, y, z are the centre of the entity's collision box, in the heightmap-cell frame
// (see RoomView::CentreEntities); its sprite is drawn there too.
struct RoomEntity
{
    int type;
    double x, y, z;
    bool centred;
    int orientation;       // 0=NE 1=SE 2=SW 3=NW
    bool solid;            // blocks the player
    bool hostile;          // an enemy if the entity table has stats for it
    bool visible = true;   // current visibility: base flag, then the room's flag rules
    bool base_visible = true;   // the "visible" bit stored with the entity
    bool talks = false;    // has a dialogue (Shift next to it)
    int dialogue = 0;      // index into the room's character list
    int speed;             // 0 = stationary, 1..4 = how fast it moves
    bool gravity;          // follows the floor height
    int move_axis = 0;     // axis an enemy is walking along: 0 = x, 1 = y (never both)
    double walk_time = 0;  // seconds spent walking: drives the walk cycle
    bool walking = false;  // moved during the last update
    double hit_flash = 0;  // s left of the "just hit" blink / stun
    // Room-specific images (default / back / front), present for entities drawn with the
    // room palette. Empty file = use the shared entity image.
    struct Image { std::string file; int w = 0, h = 0, ax = 0, ay = 0; SDL_Texture* texture = nullptr; };
    Image images[3];
};

// Loads rooms exported by landstalker_tools' room_export (<rooms_dir>/room_NNN/...)
// and draws them isometrically, with a debug overlay (warps, cursor).
// Cursor keys walk the player (held keys, smooth movement, 4 isometric directions, wall
// and step collisions from the heightmap). If two perpendicular arrows are held the most
// recently pressed one wins: there is no diagonal movement. Entering a warp loads the destination room.
// PageUp / PageDown browse rooms, keypad + / - zoom.
// Debug toggles: F1 overlay (warps, cursor), F2 heightmap grid, F3 what blocks the player.
// Walking over an item picks it up; Shift opens the chest in front of the player;
// E uses an EkeEke; Space swings the sword; R restarts the room after dying.
// Tab opens the item menu (arrows to choose, Shift to use / equip, Tab to close).
// In a shop, Shift in front of an item asks to buy it; in an inn, Shift in front of the
// innkeeper offers a night's rest.
// K uses a Key on the locked door the player is standing in front of;
// Shift talks to the character in front (and turns the pages of a dialogue);
// Y / N answer a yes/no question.
// Enemies with a speed chase the player once he is within AGGRO_RADIUS cells.
// The camera follows the player and is clamped to the room; rooms smaller than the
// viewport are centred. Drawing is clipped to the viewport.
class RoomView : public IDrawable, public IKeyInput
{
public:
    RoomView(std::shared_ptr<Graphics> graphics, const std::filesystem::path& rooms_dir,
             int room_number, SDL_Rect viewport, float scale = 1.0f);
    ~RoomView() override;

    RoomView(const RoomView&) = delete;
    RoomView& operator=(const RoomView&) = delete;

    void Draw(SDL_Renderer* renderer) override;
    void OnUpdate(double delta) override;
    void SetScale(float new_scale);
    float GetScale() const { return scale; }
    void OnTick(unsigned int total_ticks) override {}
    void OnKeyDown(SDL_Scancode scancode, SDL_Keymod mod) override;
    void OnKeyUp(SDL_Scancode scancode, SDL_Keymod mod) override {}

    // Loads a room. If arrival is given the cursor is placed there, otherwise at the map centre.
    void LoadRoom(int room_number, const SDL_Point* arrival = nullptr);
    void SetStatusCallback(std::function<void(const std::string&)> cb);
    // Shown for a few seconds after pickups and interactions ("" clears it).
    void SetMessageCallback(std::function<void(const std::string&)> cb) { message_cb = std::move(cb); }
    void SetGameState(std::shared_ptr<GameState> state) { game = std::move(state); }
    // Dialogues: the callback shows (visible = true) or hides the dialogue box with `text`.
    void SetDialogue(std::shared_ptr<Dialogue> d) { dialogue_data = std::move(d); }
    void SetDialogueCallback(std::function<void(const std::string& text, bool visible)> cb)
    {
        dialogue_cb = std::move(cb);
    }
    bool IsDialogueActive() const { return !dialogue_pages.empty(); }
    // Frames of the chest sprite (closed, then 4 opening frames): drawn for entity type 18.
    void SetChestSprite(std::shared_ptr<SpriteSet> sprite) { chest_sprite = std::move(sprite); }
    void SetShops(std::shared_ptr<Shops> s) { shops = std::move(s); shop_info = shops ? shops->ForRoom(room_number) : nullptr; }
    // The item menu: the callback shows (visible = true) or hides the menu box with `text`.
    void SetMenuCallback(std::function<void(const std::string& text, bool visible)> cb) { menu_cb = std::move(cb); }
    bool IsMenuOpen() const { return menu_open; }
    // Re-evaluates which entities are shown after a story flag changed.
    void ApplyFlagRules();
    void ApplyTileSwaps();
    // Sprite drawn at the cursor cell (the player). Without it only the debug cursor is shown.
    void SetPlayerSprite(std::shared_ptr<SpriteSet> sprite) { player = std::move(sprite); }
    // Drawing information for the entities listed in room.yaml.
    void SetEntityTable(std::shared_ptr<EntityTable> table)
    {
        entity_table = std::move(table);
        CentreEntities();
    }
    const std::vector<RoomEntity>& GetEntities() const { return entities; }

    // Moves the player as if the direction (world axes: +x = south-east, +y = south-west)
    // were held for `seconds`. Used by the keyboard update and by scripted tests.
    void SimulateMove(int dir_x, int dir_y, double seconds);
    // Puts the player at a world position (cell centre = +0.5), standing on the floor.
    void SetPlayerPosition(double x, double y);
    double GetPlayerX() const { return player_x; }
    double GetPlayerY() const { return player_y; }
    double GetPlayerZ() const { return player_z; }
    bool IsPlayerGrounded() const { return grounded; }

    int GetRoomNumber() const { return room_number; }
    const std::vector<RoomWarp>& GetWarps() const { return warps; }
    // Raw heightmap cell for a world cell (-1 outside the heightmap).
    int GetCell(int world_x, int world_y) const;
    bool IsWalkable(int world_x, int world_y) const;

    static constexpr int CELL_OFFSET = 12;   // world cell = heightmap index + 12

private:
    struct RawWarp
    {
        std::string type;
        int room1, room2, x1, y1, x_size, y_size, x2, y2;
    };

    static std::vector<int> LoadCsv(const std::filesystem::path& path, int base, int& cols, int& rows);
    static std::vector<RawWarp> LoadRawWarps(const std::filesystem::path& room_yaml);
    static std::vector<RoomEntity> LoadEntities(const std::filesystem::path& room_yaml);
    void LoadEntityImages(const std::filesystem::path& room_dir);
    void FreeEntityImages();
    // Documents/marks the entity position frame (see the comment on the definition).
    void CentreEntities();
    bool RoomExists(int number) const;
    void Step(int dir_x, int dir_y, double dt);
    bool TryMove(double dx, double dy);
    // Blocked while walking (fx, fy): slides sideways towards a gap the player almost fits through.
    bool Nudge(double fx, double fy, double max_step);
    // Describes why the box at (x, y, z) cannot stand there ("" if it can).
    std::string ExplainBlock(double x, double y, double z) const;
    bool CanStandAt(double x, double y, double z) const;
    // Highest floor under the player's box at (x, y) that can be reached from height z.
    double FloorUnder(double x, double y, double z) const;
    void UpdateVertical(double dt);
    void CheckPickups();
    void Interact();
    bool StartDialogue();       // talks to the entity in front, if any
    void AdvanceDialogue();     // next box, or closes the dialogue
    void ShowDialoguePage();    // current box, with the Yes / No hint on a question
    void AnswerDialogue(bool yes);
    void GiveDialogueItem(int item);
    // Something that was in the room is gone for good (killed, picked up): sets the story
    // flags the game ties to it. See room.yaml entity_flags:.
    void OnEntityGone(std::size_t index);
    void SetStoryFlag(int flag);
    void UseKey();
    void UpdateChestOpening(double dt);
    void SpawnSparks(double x, double y, double z, bool big, bool hurt_player);
    // Shops and inns: yes / no questions answered with Y / N.
    void ShowText(const std::string& text);
    void AskChoice(const std::string& text, std::function<void(bool)> handler);
    bool TryShop();
    bool OfferInn();
    void OpenMenu();
    void CloseMenu();
    void RefreshMenu();
    void UseMenuItem();
    void CheckSwitches();
    void ShowMessage(const std::string& text);
    void GiveChestContent(int value);
    void Attack();
    void ApplySwingHit();
    void UpdateCombat(double dt);
    void UpdateEnemies(double dt);
    bool CanEnemyStandAt(std::size_t index, double x, double y, double z) const;
    void Die();
    bool IsAliveEnemy(std::size_t index) const;
    // Solid entities whose collision box overlaps the player box centred on (x, y).
    std::vector<int> OverlappingEntities(double x, double y, double z) const;
    int FloorHeight(int cell_x, int cell_y) const;
    void CheckWarps();
    void UpdateStatus();
    void DrawLayer(SDL_Renderer* renderer, const std::vector<int>& layer, int layer_offset_tiles) const;
    SDL_FPoint ToScreen(float world_x, float world_y, float height_px = 0.0f) const;
    void DrawCell(SDL_Renderer* renderer, int wx, int wy, float height_px) const;
    void FillCell(SDL_Renderer* renderer, double wx, double wy, double half, float height_px, SDL_FColor colour) const;
    void DrawBlockers(SDL_Renderer* renderer) const;
    void DrawEntities(SDL_Renderer* renderer);
    void DrawWorld(SDL_Renderer* renderer);
    // Room pixel (unscaled) the camera should be centred on: the player's torso.
    SDL_FPoint CameraTarget() const;
    void UpdateCamera(double dt);

    // Screen shift, in cells, of everything expressed in heightmap coordinates (cells,
    // entities, player, warps) relative to the map blocks.
    static constexpr float GRID_SHIFT_X = 0.5f;
    static constexpr float GRID_SHIFT_Y = -0.5f;

    static constexpr int TILE_PX = 8;     // iso grid unit, in pixels
    static constexpr int BLOCK_PX = 16;   // block size, in pixels
    static constexpr int BLOCKS_PER_ROW = 32;
    static constexpr double WALK_SPEED = 4.5;       // cells per second
    static constexpr double WALK_FRAME_TIME = 0.12; // seconds per walk frame
    static constexpr double PLAYER_RADIUS = 0.40;   // half size of the collision box, cells
    static constexpr double NUDGE_RANGE = 0.45;     // max sideways correction towards a gap, cells
    static constexpr double PLAYER_HEIGHT = 2.0;    // collision box height, height units (32 px)
    static constexpr int PLAYER_ATTACK = 8;           // sword damage before defence (assumed)
    static constexpr double AGGRO_RADIUS = 6.0;       // cells: enemies chase inside this
    static constexpr double ENEMY_BASE_SPEED = 0.6;   // cells/s at speed 1, +0.6 per level
    static constexpr double ATTACK_COOLDOWN = 0.4;
    static constexpr double HURT_POSE_TIME = 0.3;
    static constexpr double DEATH_ANIM_TIME = 1.0;
    static constexpr double CHEST_OPEN_TIME = 0.4;     // lid animation, then the content is given
    static constexpr double CHEST_REVEAL_TIME = 0.8;   // item held up
    static constexpr double SPARK_TIME = 0.25;
    static constexpr double PLAYER_KNOCK_SPEED = 6.0;  // cells / s
    static constexpr double PLAYER_KNOCK_TIME = 0.14;  // => about 0.85 cells
    static constexpr double SWING_DURATION = 0.3;
    static constexpr double SWING_HIT_TIME = 0.10;    // second frame of 4: the arc is out
    static constexpr double HIT_FLASH_TIME = 0.25;    // blink / stun of an enemy that was hit
    static constexpr double KNOCKBACK = 0.6;          // cells an enemy is pushed back      // seconds the attack frames are shown
    static constexpr double ENEMY_WALK_FRAME_TIME = 0.18;
    static constexpr double ATTACK_REACH = 0.9;       // cells in front of the player
    static constexpr double HIT_INVULNERABILITY = 1.0;
    static constexpr double STEP_UP = 1.0;          // max floor rise walked over (stairs)
    static constexpr double CLIMB_SPEED = 8.0;      // height units / s while walking up
    static constexpr double GRAVITY = 30.0;         // height units / s^2
    static constexpr double MAX_FALL_SPEED = 20.0;  // height units / s
    static constexpr double JUMP_SPEED = 9.0;       // height units / s (peak ~1.35 units)
    static constexpr int HEIGHT_UNIT_PX = 16;  // pixels per height unit (same unit as entity z)

    std::shared_ptr<Graphics> graphics;
    std::filesystem::path rooms_dir;
    SDL_Rect viewport;
    float scale;
    int room_number = -1;
    int width = 0, height = 0;           // map size in blocks
    int pixel_w = 0, pixel_h = 0;
    int hm_left = 0, hm_top = 0;
    int hm_width = 0, hm_height = 0;
    std::vector<int> bg, fg, heightmap;
    std::vector<RoomWarp> warps;
    std::vector<RoomEntity> entities;
    std::vector<int> room_characters;    // character ids; an entity's `dialogue` indexes this
    std::shared_ptr<Dialogue> dialogue_data;
    std::function<void(const std::string&, bool)> dialogue_cb;
    std::function<void(const std::string&, bool)> menu_cb;
    std::shared_ptr<Shops> shops;

    // Chest opening: the lid opens (4 frames), then the content is given and shown rising
    // above the chest while Nigel holds it up.
    struct ChestOpening
    {
        bool active = false;
        std::size_t entity = 0;
        int content = -1;          // item index / gold code, -1 = empty chest
        double t = 0.0;
        bool given = false;
    };
    ChestOpening chest_open;
    std::shared_ptr<SpriteSet> chest_sprite;

    // Hit effects: short bursts of sparks, and the knockback the player suffers.
    struct Spark
    {
        double x, y, z;
        double t;                  // seconds since it was spawned
        bool big;
        bool hurt_player;          // red / orange instead of white / yellow
    };
    std::vector<Spark> sparks;
    double knock_vx = 0.0, knock_vy = 0.0;   // player knockback velocity, cells / s
    double knock_time = 0.0;                 // s of knockback left
    const Shops::Info* shop_info = nullptr;   // set when this room is a shop / inn / church
    std::function<void(bool)> choice_handler; // pending yes / no question (shops, inns)
    bool menu_open = false;
    int menu_index = 0;                  // selected row
    std::vector<int> menu_items;         // item indices listed, in order
    std::vector<Dialogue::Page> dialogue_pages;   // boxes left to show, current one first
    const Json* dialogue_yes = nullptr;        // follow-ups when the last box is a question
    const Json* dialogue_no = nullptr;
    bool dialogue_asks = false;
    // Flag rules for this room's entities (see room.yaml entity_flags:).
    struct FlagRule
    {
        enum Kind { HIDE, ONE_TIME, HIDE_FROM } kind;
        int entity = 0;
        int flag = 0;          // HIDE / HIDE_FROM: the flag; ONE_TIME: show flag
        bool when_set = false; // HIDE: hide while the flag state equals this; ONE_TIME: show state
        int hide_flag = 0;     // ONE_TIME
        bool hide_set = false;
    };
    std::vector<FlagRule> flag_rules;

    // Tile swaps: blocks / heightmap cells copied from one region to another when a flag is
    // set (or a key opens a locked door). base_* is the room as exported; bg / fg /
    // heightmap are base with the active swaps applied.
    struct TileSwap
    {
        int trigger = 0;
        int mode = 0;                 // 0 floor, 1 wall NE, 2 wall NW
        int map[6] = { 0, 0, 0, 0, 1, 1 };   // src_x src_y dst_x dst_y width height (blocks)
        int hm[6] = { 0, 0, 0, 0, 1, 1 };    // same, in heightmap cells
    };
    struct SwapFlag { int trigger; int flag; bool always; bool locked; };
    struct Switch { int entity; int flag; };
    std::vector<TileSwap> tile_swaps;
    std::vector<SwapFlag> swap_flags;
    std::vector<Switch> switches;
    std::vector<int> base_bg, base_fg, base_heightmap;
    std::vector<int> chest_contents;     // values from room.yaml, one per chest entity in order
    bool chests_enabled = false;
    std::shared_ptr<GameState> game;
    std::function<void(const std::string&)> message_cb;
    double message_time = 0.0;
    // Combat
    double attack_cooldown = 0.0;        // s until the next swing is allowed
    double swing_time = 0.0;             // s left of the swing effect
    double swing_anim = 0.0;             // s since the swing started (Nigel's attack frames)
    bool swinging = false;
    bool swing_hit_pending = false;      // the blow lands at SWING_HIT_TIME into the animation
    int swing_facing = 3;                // direction the blow was started in
    double hurt_time = 0.0;              // s left of Nigel's "hit" pose
    double death_time = 0.0;             // s since he died (drives the fall animation)
    double invulnerable_time = 0.0;      // s of protection after being hit
    bool dead = false;
    std::shared_ptr<EntityTable> entity_table;
    SDL_Texture* blocks = nullptr;

    int cursor_x = 0, cursor_y = 0;      // world cell containing the player
    double player_x = 0, player_y = 0;   // player position, world cells (cell centre = +0.5)
    double player_z = 0;                 // height units; 1 unit = HEIGHT_UNIT_PX pixels
    double vertical_speed = 0;           // + up
    bool grounded = true;
    bool moving = false;
    std::string blocked_text;            // why the last move attempt failed (shown in the status)
    std::string shown_blocked_text;
    bool last_key_was_x_axis = true;     // most recent arrow pressed: UP/DOWN (x) or LEFT/RIGHT (y)
    double anim_time = 0.0;              // seconds spent walking, drives the walk cycle
    bool warp_armed = true;              // false right after arriving on a warp
    bool show_overlay = true;
    bool show_grid = false;              // F2: outline every heightmap cell (alignment check)
    bool show_blocks = false;            // F3: show what blocks the player (cells, steps, entities)
    std::shared_ptr<SpriteSet> player;
    int facing = 3;                      // 0=NW 1=NE 2=SE 3=SW (screen directions)
    std::function<void(const std::string&)> status_cb;
    mutable SDL_FPoint origin{0, 0};     // screen position of the room's top-left pixel
    double camera_x = 0, camera_y = 0;   // room pixel (unscaled) shown at the viewport's top-left
    bool camera_snap = true;             // jump to the target instead of easing (room load, zoom)
    double frame_dt = 0.0;
};

#endif // _ROOM_VIEW_H_
