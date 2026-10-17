#ifndef _ROOM_VIEW_H_
#define _ROOM_VIEW_H_

#include <SDL3/SDL.h>
#include <filesystem>
#include <functional>
#include <map>
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
#include "game/Behaviours.h"
#include "game/Cutscenes.h"
#include "game/Drops.h"
#include "game/Triggers.h"
#include <deque>
#include "audio/SoundEngine.h"

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
    int behaviour = 0;     // script number (game/Behaviours.h)
    // A drop left by a dead enemy (a money bag or an item box): what it gives when touched.
    // Animations: dying (flickers, then goes), appearing (fades in)
    bool dying = false;
    double die_t = 0.0, die_dur = 0.0, die_spark = 0.0;
    int pending_gold = 0;     // dropped when the death animation ends
    double appear_t = -1.0;   // >= 0 while the entity materialises
    double act_t = -1.0;      // >= 0 while a boss plays its attack animation (s since it began)
    bool pickable = false;    // Nigel can lift it (crates, vases...)
    bool carried = false;     // Nigel holds it over his head
    bool saved_solid = true;  // `solid` before it was picked up
    bool thrown = false;      // flying after a throw
    double tvx = 0.0, tvy = 0.0, thrown_t = 0.0;
    bool falls = false;       // gravity was switched on by its script: it drops until it rests on something
    double fall_v = 0.0;      // fall speed, height units / s
    bool dormant = false;  // a scripted enemy that has not been "made hostile" yet
    bool bh_init = false;
    int pickup_gold = 0;
    int pickup_item = -1;
    double pickup_age = 0.0;
    // State of the behaviour script interpreter.
    int bh_pc = 0;         // next command, -1 = finished / idle
    int bh_count = 0;      // ticks left of the running Pause / Move command
    bool bh_busy = false;  // a Pause / Move command is running
    int bh_repeat_count = 0;   // RepeatBegin / RepeatEnd
    int bh_repeat_pc = 0;
    int bh_speed = 1;      // units (1/16 cell) per tick
    double bh_acc = 0.0;   // fractional ticks owed
    int move_axis = 0;     // axis an enemy is walking along: 0 = x, 1 = y (never both)
    double walk_time = 0;  // seconds spent walking: drives the walk cycle
    bool walking = false;  // moved during the last update
    double hit_flash = 0;  // s left of the "just hit" blink / stun
    // Enemy AI state (see RoomView::UpdateEnemies)
    enum AiState { AI_IDLE = 0, AI_CHASE, AI_LUNGE, AI_RECOVER };
    int ai_state = AI_IDLE;
    double ai_time = 0.0;      // time in the state (CHASE: since the player was last seen)
    double ai_time2 = 0.0;     // hopping clock
    double ai_cooldown = 0.0;  // s until the next lunge is allowed
    double ai_alert = 0.0;     // s left of "was hit: knows where the player is"
    double ai_vx = 0.0, ai_vy = 0.0;   // lunge velocity, cells / s
    double ai_phase = 0.0;     // bobbing phase of floaters
    bool ai_walking = true;    // patrol: walking or pausing
    int ai_shots = 0;          // bosses: shots fired so far (picks the pattern)
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
class Bot;

class RoomView : public IDrawable, public IKeyInput
{
    friend class Bot;   // the play-test bot (debug/Bot.h) reads and steers the game
public:
    RoomView(std::shared_ptr<Graphics> graphics, const std::filesystem::path& rooms_dir,
             int room_number, SDL_Rect viewport, float scale = 1.0f);
    ~RoomView() override;

    RoomView(const RoomView&) = delete;
    RoomView& operator=(const RoomView&) = delete;

    void Draw(SDL_Renderer* renderer) override;
    void OnUpdate(double delta) override;
    void SetScale(float new_scale);
    // Plays the room music again (after the music was switched back on).
    void RestartMusic() { playing_bgm = -1; PlayRoomMusic(); }
    float GetScale() const { return scale; }
    void OnTick(unsigned int total_ticks) override {}
    void OnKeyDown(SDL_Scancode scancode, SDL_Keymod mod) override;
    void OnKeyUp(SDL_Scancode scancode, SDL_Keymod mod) override {}

    // Loads a room. If arrival is given the cursor is placed there, otherwise at the map centre.
    void LoadRoom(int room_number, const SDL_Point* arrival = nullptr);
    // Playtest aid: goes to a room, arriving by one of its doors (the first warp that leads there).
    // False when the room does not exist.
    bool GoToRoom(int number);
    void SetStatusCallback(std::function<void(const std::string&)> cb);
    // Shown for a few seconds after pickups and interactions ("" clears it).
    void SetMessageCallback(std::function<void(const std::string&)> cb) { message_cb = std::move(cb); }
    void SetGameState(std::shared_ptr<GameState> state) { game = std::move(state); }
    // Music and sound effects (optional: the game is silent without it).
    // Where F5 saves / F9 loads (empty: no saving).
    void SetSavePath(std::filesystem::path path) { save_path = std::move(path); }
    bool SaveGame();
    bool LoadGame();
    // Save slots: files <dir>/save1.txt ... save3.txt. F5 / F9 and the churches use the active slot.
    static constexpr int SAVE_SLOTS = 3;
    void SetSaveDir(std::filesystem::path dir) { save_dir = std::move(dir); }
    std::filesystem::path SlotPath(int slot) const { return save_dir / ("save" + std::to_string(slot) + ".txt"); }
    void SetActiveSlot(int slot) { active_slot = slot; save_path = SlotPath(slot); }
    int ActiveSlot() const { return active_slot; }
    // "Room 21, 123 gold" or "" if the slot is empty.
    std::string SlotSummary(int slot) const;
    // The menus of main.cpp: the pause menu (Esc) and the game over screen.
    void SetPauseCallback(std::function<void()> cb) { pause_cb = std::move(cb); }
    void SetGameOverCallback(std::function<void()> cb) { game_over_cb = std::move(cb); }
    // The end of the game: the credits roll (see ui/CreditsScreen.h).
    void SetCreditsCallback(std::function<void()> cb) { credits_cb = std::move(cb); }
    // After dying: wake up at the last church, or restart the room.
    void Respawn();
    bool HasSavedGame() const { return !save_path.empty() && std::filesystem::exists(save_path); }
    // While paused (behind the title screen) nothing in the room moves.
    void SetPaused(bool p) { paused = p; }
    void SetBehaviours(std::shared_ptr<Behaviours> b) { behaviours = std::move(b); }
    void SetDrops(std::shared_ptr<Drops> d) { drops = std::move(d); }
    void SetCutscenes(std::shared_ptr<Cutscenes> c) { cutscenes = std::move(c); }
    // Runs a custom script action (cutscene) by number: texts, flags, sounds.
    void TriggerAction(int id);
    void DebugPose(double act_time) { for (auto& e : entities) if (entity_table && entity_table->GetEnemy(e.type)) e.act_t = act_time; }   // tests
    void DebugKill(std::size_t index) { if (index < entities.size()) KillEnemy(index); }   // tests
    void InflictStatus(int status);   // an ailment (see GameState::STATUS_*)
    // Debug: lists the walkable cells where a priority tile hides the player.
    void ScanOcclusion();
    // Starts a new game: the intro sequence (rooms 139...) with the controls locked.
    void BeginNewGame();
    void SetSound(std::shared_ptr<SoundEngine> engine) { sound = std::move(engine); PlayRoomMusic(); }
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
    void ApplyCustomRoomActions();
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
    std::string DebugPos() const { char b[96]; std::snprintf(b, sizeof b, "player (%.2f,%.2f) z %.2f grounded %d support %d", player_x, player_y, player_z, grounded, support_entity); return b; }
    // Why the player cannot move right now ("" when he is in control): a scripted scene, the intro,
    // a dialogue, a faint... (shown by the playtest panel).
    std::string ControlLockReason() const
    {
        if (dead) return "DEAD";
        if (intro_active) return "INTRO";
        if (fainted) return "FAINT";
        if (input_locked) return "SCENE (input locked)";
        if (!cutscene_ops.empty() || cutscene_busy > 0.0) return "CUTSCENE";
        if (input_hold > 0.0) return "SCRIPTED (held)";
        if (warp_pending || fx_level > 0.05) return "TRANSITION";
        if (!dialogue_pages.empty()) return "DIALOGUE";
        if (chest_open.active) return "CHEST";
        return "";
    }
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
    double PlatformTop(double x, double y, double z, int* index = nullptr) const;
    bool IsPlatform(std::size_t i) const;
    double EnemyFloor(std::size_t index) const;   // floor or low object under an enemy
    bool SomeoneOn(std::size_t index) const;      // the player or an enemy stands on entity `index`
    static constexpr double PLATFORM_STEP = 0.05;  // the player never steps onto an object: he must jump (only floor cells are climbed)
    static constexpr double ENEMY_PLATFORM_STEP = 0.6;   // enemies do hop onto low objects (a pressure plate)
    static constexpr double LADDER_SPEED = 3.0;   // height units / s on a ladder
    static constexpr double FAINT_TIME = 5.0;   // s lying on the ground
    static constexpr double PLATFORM_TOLERANCE = 0.05;   // an object counts as a floor when its top is this close to the feet
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
    // Lifting crates and vases: Shift next to one lifts it over the head; Shift again on the ground sets it
    // down in front, Shift in mid-air throws it. (The original: ProcessActionButton, code/gamelogic/gamelogic4.asm.)
    static constexpr double LIFT_TIME = 0.4;        // 24 frames
    static constexpr double CARRY_HEIGHT = 2.0;     // above the player's feet
    int carried_entity = -1;
    int lift_phase = 0;        // 0 none, 1 lifting, 2 putting down
    double lift_t = 0.0;
    double drop_x = 0.0, drop_y = 0.0, drop_z = 0.0;
    bool TryPickUp();
    void PutDownCarried();
    void ThrowCarried();
    void UpdateCarry(double dt);
    void DropCarried();   // forgets what was held (room change)
    void UseIdol();
    void UpdateIdol(double dt);
    static constexpr int ITEM_IDOL_STONE = 49;
    std::vector<int> forced_swaps;   // tile swap triggers shown regardless of flags (the door half open)
    double idol_t = 0.0;
    void UpdateChestOpening(double dt);
    void SpawnSparks(double x, double y, double z, bool big, bool hurt_player);
    // Shops and inns: yes / no questions answered with Y / N.
    void ShowText(const std::string& text);
    void AskChoice(const std::string& text, std::function<void(bool)> handler);
    bool TryShop();
    bool OfferInn();
    bool OfferChurch();
    void UpdateBehaviours(double dt);
    // Status ailments: poison (HP drain while walking), paralysis (cannot move), confusion
    // (controls reversed), curse (cannot use the sword).
    void CureStatus(int status);
    void TryStatusAttack(int enemy_type);
    void UpdateAilments(double dt);
    // Boss animations: death throes, materialising, camera on the boss, screen shake.
    static bool IsBossType(int type);
    void KillEnemy(std::size_t index);
    void StartShake(double seconds, double amplitude) { shake_t = shake_total = std::max(shake_t, seconds); shake_amp = amplitude; }
    void UpdateEntityAnimations(double dt);
    void UpdateFallingEntities(double dt);
    bool TriggerHolds(int trigger, std::size_t index);
    bool KillableEnemyExists() const;
    std::shared_ptr<Triggers> triggers;   // position / flag conditions (assets/text/triggers.txt)
    int playback_seq = 0;                 // the recorded input sequence being replayed, 0 = none
    int TriggerVar(int var, char size) const;   // WaitForCondition of the behaviour scripts
    int support_entity = -1;   // entity the player stands on, or -1
    double arrival_x = 0.0, arrival_y = 0.0;   // where the player entered the room (the raft warps wait until it is far from here)
    // The player faints (CSA_0000, the fall down the waterfall): he lies still for a while, then in
    // the Waterfall Shrine Entrance (room 426) he is carried to the Massan mayor's house.
    bool fainted = false;
    double faint_t = 0.0;
    bool player_hidden = false;   // not drawn (asleep in a bed)
    bool faint_warp = false;   // the pending warp is the one to the mayor's house
    void StartFaint();
    void UpdateFaint(double dt);
    double input_hold = 0.0;   // s the player cannot move (a raft ride starts: the original plays an idle recording)
    // Attack and hurt poses of the enemies: the frame to draw instead of the walking image
    // (see EntityTable::GetPoseFrames), or false.
    bool EnemyPoseFrame(const RoomEntity& e, EntityTable::Image& frame, bool& flip);
    static double AttackAnimTime(int type) { return IsBossType(type) ? 0.9 : 0.5; }
    void FinishDying(std::size_t index);
    double shake_t = 0.0, shake_total = 0.0, shake_amp = 0.0;
    int camera_focus = -1;    // entity the camera looks at during a cutscene (-1: the player)
    bool CureAilmentsWith(int item);
    double poison_walk = 0.0;
    int poison_ticks = 0;
    double paralysis_time = 0.0;
    int paralysis_presses = 0;
    void BehaviourTick(std::size_t index, bool& moved);
    bool BehaviourStep(std::size_t index, bool noclip = false);
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
    double EnemySpeed(const RoomEntity& e) const;
    // Projectiles thrown by bosses (fireballs, axes), flying along the grid axes.
    struct Projectile
    {
        int type = 0;                 // entity type whose image it uses
        double x = 0, y = 0, z = 0;
        double vx = 0, vy = 0;        // cells / s
        int attack = 1;
        bool returns = false;         // a boomerang: comes back to its owner
        int owner = -1;
        double age = 0.0;
        int orientation = 0;
    };
    std::vector<Projectile> projectiles;
    void SpawnProjectile(std::size_t owner, int type, double vx, double vy, int attack, bool returns, double lateral);
    void UpdateProjectiles(double dt);
    void HurtPlayer(int attack, double from_x, double from_y, bool knockback = true);
    void UpdateFloorEffects(double dt);
    bool IceSlide(int& dir_x, int& dir_y) const;
    double heal_acc = 0.0;   // Healing Boots
    void BossFire(std::size_t index, int type, double speed, int attack, int count, bool returns);
    bool UpdateBoss(std::size_t index, double dt);
    bool EnemyMove(std::size_t index, double dx, double dy, bool ghost);
    bool CanEnemyStandAt(std::size_t index, double x, double y, double z) const;
    void Die();
    bool IsAliveEnemy(std::size_t index) const;
    static bool IsScriptedBall(int type);
    // Solid entities whose collision box overlaps the player box centred on (x, y).
    std::vector<int> OverlappingEntities(double x, double y, double z) const;
    int FloorHeight(int cell_x, int cell_y) const;
    void CheckWarps();
    void UpdateStatus();
    void DrawLayer(SDL_Renderer* renderer, const std::vector<int>& layer, int layer_offset_tiles,
                   SDL_Texture* texture = nullptr) const;
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
    static constexpr double BEHAVIOUR_TICK_HZ = 30.0;   // script ticks per second (calibrated by feel)
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
    std::vector<int> dialogue_actions;            // custom script actions to run when the conversation ends
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
    std::shared_ptr<SoundEngine> sound;
    std::shared_ptr<Behaviours> behaviours;
    std::shared_ptr<Drops> drops;
    void SpawnDrop(double x, double y, double z, int enemy_type, int gold);
    std::shared_ptr<Cutscenes> cutscenes;
    int cutscene_source = -1;                 // entity whose script started the running cutscene
    std::deque<Cutscenes::Op> cutscene_ops;   // what is left of the running cutscene
    bool cutscene_answer_valid = false;       // the last yes/no answer, until a branch uses it
    bool cutscene_answer = false;
    double page_wait = 0.0;                   // s a cutscene box stays up (0 = until dismissed)
    double page_timer = 0.0;
    bool paused = false;
    // Screen effects of the cutscenes: a coloured veil over the room (fades, flashes).
    enum class FxMode { NONE, TO_LEVEL, PULSE };
    FxMode fx_mode = FxMode::NONE;
    double fx_level = 0.0;      // 0 = nothing, 1 = the veil covers everything
    double fx_from = 0.0, fx_to = 0.0, fx_t = 0.0, fx_dur = 0.0;
    SDL_Color fx_color{ 0, 0, 0, 255 };
    double cutscene_busy = 0.0;   // s the running cutscene waits (an effect, a pause)
    bool busy_then_warp = false;  // ... and then does the warp that was prepared
    double StartFx(const std::string& name);   // returns how long it takes
    void UpdateFx(double dt);
    // Friday: her overlay sprite in the cutscenes (flies in, hovers, flies off).
    // Opening captions of the intro (SEGA ENTERPRISES PRESENTS...): 2 lines of 16 glyphs of the intro font.
    struct IntroString { int x1 = 0, y1 = 0, x2 = 0, y2 = 0, frames = 0; std::vector<int> line1, line2; };
    std::map<int, IntroString> intro_strings;
    bool intro_strings_loaded = false;
    int intro_caption = 0;            // the string on screen (0: none)
    double intro_caption_t = 0.0;     // seconds left
    SDL_Texture* intro_font = nullptr;
    void ShowIntroString(int number);
    void DrawIntroCaption(SDL_Renderer* renderer);
    int friday_state = 0;         // 0 hidden, 1 appearing, 2 hovering, 3 leaving
    double friday_t = 0.0;
    void DrawFriday(SDL_Renderer* renderer);
    std::filesystem::path save_dir;
    int active_slot = 1;
    std::function<void()> pause_cb, game_over_cb, credits_cb;
    bool game_over_shown = false;
    bool input_locked = false;                // scripted scene: the keyboard does nothing
    bool intro_active = false;                // the new-game intro is running (Enter skips it)
    struct PendingWarp { int room, x, y, dir; bool fall = false; int climb_kind = 0; };   // climb_kind: 1 ladder, 2 elevator
    // Rooms replaced by another version once a story flag is set (assets/text/room_transitions.txt).
    struct Transition { int from, to, flag; };
    std::vector<Transition> transitions;
    int Redirect(int room) const;         // the version of `room` to enter now (CheckForRoomTransition)
    int OriginalRoom(int room) const;     // the room whose warps `room` uses (GetOriginalRoomNum)
    int climb_dest = -1;       // room above, reached by a ladder or an elevator (room.yaml climb_destination)
    int room_max_height = 0;   // room.yaml z_end: going higher takes the ladder / elevator out of the room
    bool climbing = false;     // hanging on a ladder this frame
    void ClimbOut(int kind);
    // Animated blocks of the room (water, lava...): see UpdateAnimations.
    struct RoomAnim
    {
        std::string file;
        int frames = 0, delay = 1, shown = -1;
        std::vector<int> blocks;
        SDL_Surface* atlas = nullptr;
    };
    std::vector<RoomAnim> animations;
    double anim_clock = 0.0;
    static SDL_Texture* MakeTexture(SDL_Renderer* renderer, SDL_Surface* surface);
    void FreeAnimations();
    void UpdateAnimations(double dt);
    int fall_dest = -1;        // room entered by falling into a pit cell (room.yaml fall_destination)
    bool support_is_raft_ok() const;
    void CheckPit();           // standing on a pit: fall to the room below
    bool warp_pending = false;
    PendingWarp pending_warp{};
    void ApplyPendingWarp();
    void SkipIntro();
    // Scripted controller input (the "playback" of the original)
    const Behaviours::Playback* playback = nullptr;
    std::size_t playback_index = 0;
    double playback_timer = 0.0;
    void StartPlayback(int sequence);
    void ContinueCutscene();
    void DoTileSwapTrigger(int trigger);
    std::filesystem::path save_path;
    // Last church where the adventure was recorded: where the player wakes up after dying.
    int church_room = -1;
    double church_x = 0.0, church_y = 0.0;
    void Sfx(int id) { if (sound) sound->Command(id); }
    void Jingle(int id, double seconds);
    void PlayRoomMusic();
    int room_bgm = -1;                   // music id of the current room
    int playing_bgm = -1;                // music id last sent to the driver
    double jingle_time = 0.0;            // s left of a fanfare; the room music resumes after it
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
    SDL_Texture* blocks_priority = nullptr;   // only the priority tiles: drawn over the sprites
    std::vector<char> block_has_priority;     // per block index: blocks_priority has pixels for it
    // A block with priority tiles and what it shows: the nearest cell (x + y) and its height.
    // Such a block hides a sprite standing behind it (a smaller x + y) lower than its top.
    struct PriorityBlock
    {
        double depth;        // x + y of the cell it shows
        int z;               // height slice it shows
        bool wall;           // a vertical face (the cell is higher than the slice), not a top
        SDL_FRect src, dst;  // in blocks_priority, on screen
    };
    void BuildPriorityBlocks(const std::vector<int>& layer, int layer_offset_tiles,
                             std::vector<PriorityBlock>& out) const;
    static bool PriorityHides(const PriorityBlock& b, double sprite_depth, double sprite_z,
                              const SDL_FRect& sprite_rect);

    int cursor_x = 0, cursor_y = 0;      // world cell containing the player
    double player_x = 0, player_y = 0;   // player position, world cells (cell centre = +0.5)
    double player_z = 0;                 // height units; 1 unit = HEIGHT_UNIT_PX pixels
    double vertical_speed = 0;           // + up
    double air_peak = -1e9;              // highest z of the current jump / fall (for the landing sound)
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
    // Play-test bot: when set, these directions replace the keyboard.
    bool bot_input = false;
    int bot_dx = 0, bot_dy = 0;
};

#endif // _ROOM_VIEW_H_
