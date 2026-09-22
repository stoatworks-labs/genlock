#pragma once

/**
    The pass, as GLSL. There is only one.

    A genlock is a single decision per pixel -- video or computer -- so
    nothing here needs a buffer of its own, and this repo carries no
    `PassBuffer`. What it does need is **two inputs**, which is the whole
    reason this plugin exists in the fleet: `TextureDest` is the layer below
    (the incoming video) and `TextureSrc` is this layer (the computer's
    picture, fill and key together).

    The two can be **different resolutions**, and each therefore has its own
    `MaxUV` and its own half-texel inset. Getting one of those wrong is
    silent: the picture still appears, at the wrong scale, or with a band of
    a neighbouring clip's texture memory down one side.

    Everything in here is addressed in **picture space**, 0..1 across the
    output, with MaxUV applied once at each fetch. Displacements -- the key
    delay, the crawl, the roll, the tear -- are therefore in the same units
    at every raster, which is the only way the artefact is the same size at
    720p and at 4K.
*/

namespace genlock
{

extern const char* const kVertexShader;
extern const char* const kGenlockShader;

} // namespace genlock
