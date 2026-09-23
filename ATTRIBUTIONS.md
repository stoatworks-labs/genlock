# Attributions

Genlock is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Diagnostics log — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Diag.* is tinsel's log, renamed into this namespace.

### Build and offline-harness shape — Stoatworks rosette

<https://github.com/stoatworks-labs/rosette>  
Licence: MIT  
Copyright: Stoatworks Labs

The CMake shape, the offline-harness skeleton (the CGL context, the PNG writer, --list/--set), tools/sweep.py and the release-job-locally checks in tools/verify.sh follow rosette's.

### Build and offline-harness shape — Stoatworks graticule

<https://github.com/stoatworks-labs/graticule>  
Licence: MIT  
Copyright: Stoatworks Labs

The same CMake, harness, sweep and verify shape, followed from graticule alongside rosette.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### The Amiga genlock

Implemented from the published description of how a genlock works: an external key derived from the computer's colour 0, cut on the computer's own pixel clock rather than the incoming video's, and a three-position video/overlay/dissolve fader. No vendor firmware, schematic or captured footage was used and nothing was measured off real hardware; the look is a model, not a characterisation of anybody's box.

## Standards and published specifications

What the implementation is measured against.

- **PAL colour television (ITU-R BT.470)** — The 4.43361875 MHz subcarrier and ten-cycle colour burst: the Amiga's PAL lores pixel clock (7.09379 MHz, 1.6 times the subcarrier) sets the crawl rate, and the burst's 16-lores-pixel length sets the Crawl Wrap ceiling.
- **ITU-R BT.709** — Luma coefficients for the Luma key source.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
