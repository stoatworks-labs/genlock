# Attributions

Genlock is built on other people's work. This file lists what that work is, who
did it, and what it is doing here.

> **Provisional.** Across the fleet this file is generated from master lists in
> `stoatworks-backend` by `scripts/sync-attributions.py`. Genlock is not in that
> script's lists at all yet — it is a new repo — so this copy is hand-written,
> adapted from rosette's; v0.1.0 ships that way. Registering the project and
> re-running the sync is the fix — and note that the script's `--only` flag
> truncates the file rather than filtering it.

## Third-party code this project uses

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>
Licence: BSD-3-Clause
Copyright: FreeFrame

Vendored as a git submodule at `external/ffgl`, pinned to `b1afaf9`.

The plugin ABI itself. An FFGL plugin is defined by this SDK's headers — there
is no other way to be loadable by Resolume Arena and Avenue.

The SDK's `source/plugins/Add` example is also the only `FF_MIXER` that exists
anywhere reachable, and it is what the mixer mechanics here were read off. The
shader is ours; the shape of `ProcessOpenGL` — guard on the input count, guard
on each pointer, one `MaxUV` per input, interleaved scoped bindings — follows
it.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Windows only, from vcpkg, statically linked. The SDK's headers pull it in for
the OpenGL function pointers; macOS uses the system OpenGL framework instead.

### zlib

<https://zlib.net>
Licence: zlib
Copyright: Jean-loup Gailly and Mark Adler

Ships with macOS. Linked by the offline harness only, which writes its PNGs
with it rather than carrying an image library.

## Work from elsewhere in the fleet

### tinsel, rosette, graticule

<https://github.com/stoatworks-labs>
Licence: MIT
Copyright: Stoatworks Labs

`source/Diag.*` is tinsel's, renamed into this namespace. The CMake shape, the
offline-harness skeleton (the CGL context, the PNG writer, `--list`/`--set`),
`tools/sweep.py` and the release-job-locally checks in `tools/verify.sh` follow
rosette's and graticule's. The About header and `StoatworksAboutLinks.h` are
hand copies of the generated ones, with `guide=""` because no user guide
exists.

## Method

The Amiga genlock behaviour modelled here is implemented from the published
description of how a genlock works — an external key derived from the
computer's colour 0, cut on the computer's own pixel clock rather than the
incoming video's, and a three-position video/overlay/dissolve fader. The PAL
lores pixel clock (7.09379 MHz, twice the 3.546895 MHz colour carrier) is a
published figure. No vendor's firmware, schematic or captured footage was used,
and nothing was measured off real hardware — there is none here. The look is
a model, not a characterisation of anybody's box.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or
you would rather not be listed — open an issue and it will be fixed.
