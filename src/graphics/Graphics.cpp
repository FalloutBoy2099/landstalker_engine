// filepath: c:\PROJECTS\sdl3_ls_engine\Graphics.cpp
#include "graphics/Graphics.h"
#include <SDL3/SDL.h>
#include <stdexcept>
#include <cstdlib>

Graphics::Graphics(unsigned int width, unsigned int height, const std::string& title)
    : width(width), height(height), window(nullptr), renderer(nullptr)
{
    // Create the window
    if (!SDL_CreateWindowAndRenderer(title.c_str(), width, height, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        SDL_Log("Couldn't create window and renderer: %s", SDL_GetError());
        throw std::runtime_error(SDL_GetError());
    }

    // The picture is made for width x height: scaled to the window (bars if the shape differs).
    SDL_SetRenderLogicalPresentation(renderer, static_cast<int>(width), static_cast<int>(height), SDL_LOGICAL_PRESENTATION_LETTERBOX);

    const SDL_Rect textarea = {0, 0, 800, 600};
    SDL_SetTextInputArea(window, &textarea, 0);
    SDL_StartTextInput(window);
}

Graphics::~Graphics()
{
    if (renderer) {
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
    }
    if (window) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    // Quit SDL subsystems initialized by this object
    SDL_Quit();
}

void Graphics::Render(const std::vector<std::shared_ptr<IDrawable>> drawables)
{
    if (!renderer) return;

    // Clear to black and present (simple stub)
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    Draw(drawables);

    // Debug aid: LS_SCREENSHOT=<file.bmp> saves the first frame and quits.
    static bool shot_done = false;
    static int shot_frames = 0;
    if (!shot_done)
    {
        // LS_SCREENSHOT_FRAME=n waits for the n-th frame (default: the first one).
        static const char* frame_env = std::getenv("LS_SCREENSHOT_FRAME");
        const int wanted = frame_env ? std::atoi(frame_env) : 1;
        if (const char* path = (++shot_frames >= wanted) ? std::getenv("LS_SCREENSHOT") : nullptr)
        {
            shot_done = true;
            if (SDL_Surface* surf = SDL_RenderReadPixels(renderer, nullptr))
            {
                SDL_SaveBMP(surf, path);
                SDL_DestroySurface(surf);
            }
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
    }

    SDL_RenderPresent(renderer);
}

SDL_Renderer* Graphics::GetRenderer()
{
    return renderer;
}

SDL_Window* Graphics::GetWindow()
{
    return window;
}

void Graphics::Draw(const std::vector<std::shared_ptr<IDrawable>> drawables)
{
    for(const auto& drawable : drawables)
    {
        drawable->Draw(renderer);
    }
}