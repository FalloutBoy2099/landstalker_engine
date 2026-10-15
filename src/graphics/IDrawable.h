#ifndef _IDRAWABLE_H_
#define _IDRAWABLE_H_

#include "core/IUpdatable.h"
#include "core/ITickable.h"
#include <SDL3/SDL.h>

class IDrawable : public ITickable, public IUpdatable
{
public:
    virtual ~IDrawable() = default;
    virtual void Draw(SDL_Renderer* renderer) = 0;
};

#endif // _IDRAWABLE_H_