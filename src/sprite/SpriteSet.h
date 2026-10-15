#ifndef _SPRITE_SET_H_
#define _SPRITE_SET_H_

#include <SDL3/SDL.h>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "graphics/Graphics.h"

// One exported sprite (<assets>/sprites/sprite_NNN): its frames as textures and its
// animations as ordered lists of frame names. See landstalker_tools' sprite_export.
class SpriteSet
{
public:
    struct Frame
    {
        SDL_Texture* texture = nullptr;
        int width = 0, height = 0;
        int anchor_x = 0, anchor_y = 0;   // pixel of the image that sits on the entity position
    };

    SpriteSet(std::shared_ptr<Graphics> graphics, const std::filesystem::path& sprite_dir);
    ~SpriteSet();

    SpriteSet(const SpriteSet&) = delete;
    SpriteSet& operator=(const SpriteSet&) = delete;

    const std::string& GetName() const { return name; }
    int GetId() const { return id; }
    // Animations in file order (SpriteNNNAnim00, Anim01, ...).
    const std::vector<std::string>& GetAnimationNames() const { return animation_order; }
    const std::vector<std::string>& GetAnimation(const std::string& anim) const;
    const std::vector<std::string>& GetAnimation(std::size_t index) const;
    const Frame* GetFrame(const std::string& frame) const;

    // Draws a frame with its anchor at (x, y) on screen.
    void Draw(SDL_Renderer* renderer, const std::string& frame, float x, float y,
              float scale = 1.0f, bool hflip = false) const;

private:
    std::string name;
    int id = -1;
    std::map<std::string, Frame> frames;
    std::map<std::string, std::vector<std::string>> animations;
    std::vector<std::string> animation_order;
};

#endif // _SPRITE_SET_H_
