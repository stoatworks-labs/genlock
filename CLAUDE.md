# genlock

An Amiga genlock as an FFGL **mixer** for Resolume Arena/Avenue. C++/GLSL, CMake
MODULE → universal `.bundle` (macOS) + Windows `.dll`. MIT. Not yet public, not
yet released, never loaded into Resolume.

Read `AGENTS.md` before changing `ProcessOpenGL`, the timing, or any tolerance
in the harness. It carries the fleet's only account of how an FFGL mixer
behaves, and the reasoning behind every number `gltest` asserts.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install into Arena: `cmake --install build` → `~/Documents/Resolume Arena/Extra Mixers`
  (**not** Extra Effects — this is a mixer. Untested: nothing here has been in
  front of Resolume.)
- Render a frame offline: `./build/gltest --out /tmp/f.png --size 1920x1080`
- Choose the two inputs: `--input-a video --input-b amiga`
  (a is **Dest**, the layer below; b is **Src**, this layer. Also `quads-a`,
  `quads-b`, `edge`, `bar`, `colour0`, `flat`.)
- Set anything by name: `--set "Key Delay=0.3" --set "Sync Quality=0.2"`
- List parameters: `./build/gltest --list`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + every check, ~8 s)
- No name over 16 characters: `./build/gltest --names`
- Two inputs, two sizes, two MaxUVs, and the guards: `./build/gltest --mixer`
- The key lags the fill by exactly the stated delay: `./build/gltest --delay`
- The delay walks at the rate the clock error predicts: `./build/gltest --crawl`
- The overlay rolls and wraps, at two rasters: `./build/gltest --roll`
- At Video the output IS Dest: `./build/gltest --fader`
- ms/frame, 720p through 4K: `./build/gltest --bench`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)

## Notes
- **This is the fleet's first `FF_MIXER`.** The type is the eighth argument of
  `CFFGLPluginInfo` and nothing else enforces it. `SetMinInputs`/`SetMaxInputs`
  are a **separate** declaration the host reads through different function
  codes — a plugin can be a mixer and still ask for one input. `verify.sh`
  asserts both through `oxbow probe`.
- **`inputTextures[0]` is Dest (the layer below), `[1]` is Src (this layer).**
  They can be different resolutions, so each needs its own `MaxUV` and its own
  half-texel inset. This is the single easiest thing to get silently wrong.
- **`GetShortName` is optional**, not required — the base class returns null and
  the SDK passes that straight to the host. Overridden here anyway.
- **Every displacement is in picture space**, 0..1 across the output, with each
  input's `MaxUV` applied once at its own fetch. The SDK examples fold `MaxUV`
  into the vertex shader; that cannot work here, because a displacement in one
  input's texture space is the wrong size in the other's.
- **The `ffglex::Scoped*` bindings must be declared interleaved** — activate(0),
  bind(0), activate(1), bind(1) — because each clears to 0 on exit rather than
  restoring.
- **The shader never sees a clock.** Phases are reduced on the CPU, in double,
  from a frame-relative time: Resolume counts milliseconds and a float stops
  resolving consecutive ones at about 4.99e8.
- **One Amiga pixel is 1/320 of the picture width** at every raster. PAL lores
  is assumed throughout.
- `SetParamInfo` clamps a STANDARD default into 0..1, so every ranged parameter
  is 0..1 and the conversions live in `Controls.cpp`.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no
  host can instantiate the plugin at all.
- `genlock_core` is an OBJECT library, not STATIC — the plugin registers itself
  from a file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- The master blend is called **`Opacity`**, not `Mix`. See AGENTS.md.
- FFGL id is `GL01`. Display name `SW Genlock`.

## Not done yet
- Never loaded into Resolume, on any platform. Never run on any rasteriser but
  this Mac's.
- No release tag, no remote, not registered on the website. `StoatworksAbout.h`
  and `ATTRIBUTIONS.md` are provisional hand copies.
- No presets, no OpenFX port, no browser demo, no user guide.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume).

    ~/Library/Logs/genlock/genlock.YYYY-MM-DD.log
