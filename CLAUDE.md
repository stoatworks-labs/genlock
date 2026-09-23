# genlock

An Amiga genlock as an FFGL **mixer** for Resolume Arena/Avenue. C++/GLSL, CMake
MODULE → universal `.bundle` (macOS) + Windows `.dll`. MIT. Public at
github.com/stoatworks-labs/genlock, released at v0.1.0 on 2026-09-23. Never
loaded into Resolume on macOS; on Windows Arena 7.27.1 loads it as a layer
**Blend Mode** and drives it every frame (measured 2026-09-23, see AGENTS.md).
**Known limitation: Arena does not expose Key Source, so in Resolume the key is
always Colour 0.**

Read `AGENTS.md` before changing `ProcessOpenGL`, the timing, or any tolerance
in the harness. It carries the fleet's only account of how an FFGL mixer
behaves, and the reasoning behind every number `gltest` asserts.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install into Arena: `cmake --install build` → `~/Documents/Resolume Arena/Extra Effects`
  (yes, Extra Effects, although this is a mixer: Arena has one FFGL folder and
  no `Extra Mixers`, on macOS or Windows. See AGENTS.md.)
- Render a frame offline: `./build/gltest --out /tmp/f.png --size 1920x1080`
- Choose the two inputs: `--input-a video --input-b amiga`
  (a is **Dest**, the layer below; b is **Src**, this layer. Also `quads-a`,
  `quads-b`, `edge`, `bar`, `colour0`, `flat`.)
- Set anything by name: `--set "Key Delay=0.3" --set "Sync Quality=0.2"`
- List parameters: `./build/gltest --list`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + every check, ~8 s)
- No name over 16 characters, and no two alike: `./build/gltest --names`
- Two inputs, two sizes, two MaxUVs, and the guards: `./build/gltest --mixer`
- The key lags the fill by exactly the stated delay: `./build/gltest --delay`
- The delay walks at the rate the clock error predicts: `./build/gltest --crawl`
- The overlay rolls and wraps, at two rasters: `./build/gltest --roll`
- At Video the output IS Dest: `./build/gltest --fader`
- What Amiga Mode and Crawl Wrap scale and what they do not, at 640x360 and
  320x180, each with a negative control that must fail: `./build/gltest --modes`
- The new controls' defaults ARE the old behaviour, plus names, order and the
  parameter count: `./build/gltest --defaults`
- One character of the shipped GLSL, changed, fails a check: `./build/gltest --mutation`
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
- **One Amiga pixel is 1/320, 1/640 or 1/1280 of the picture width** at every
  raster, by `Amiga Mode`. What is a count of pixel clocks (Key Delay, Crawl
  Wrap) scales with the mode; what is a time error (the crawl's speed across
  the picture, the tear's throw, which is always in LORES pixels) does not.
  `Controls.h` says which beside each constant. PAL is assumed throughout.
- **Amiga Mode and Crawl Wrap default to lores and one pixel**, which renders
  byte-identically to the plugin before they existed. `--defaults` holds that.
- `Genlock::SetFaultForTest` / `SetFragmentShaderForTest` are harness-only hooks
  for the negative controls and the mutation test. Nothing a host does reaches
  them.
- `SetParamInfo` clamps a STANDARD default into 0..1, so every ranged parameter
  is 0..1 and the conversions live in `Controls.cpp`.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no
  host can instantiate the plugin at all.
- `genlock_core` is an OBJECT library, not STATIC — the plugin registers itself
  from a file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- The master blend is called **`Opacity`**, not `Mix`, and Resolume binds it to
  the **layer's** Opacity fader (measured on Arena 7.27.1, 2026-09-23): the
  mixer panel's own slider is overridden. See AGENTS.md.
- **Arena does not expose Key Source** (id 0, the first parameter): 25 of 26
  parameters appear in its mixer panel, and the key is stuck at Colour 0 in
  Resolume. Why is not known. The harness and `oxbow probe` cannot see this —
  only Arena can. No code changed for it.
- FFGL id is `GL01`. Display name `SW Genlock`. Arena registers it as
  category 2, beside its own blend modes and transitions: an operator picks it
  as a layer's **Blend Mode**.
- `source/StoatworksAbout.h` is generated by the backend's sync-about.py and
  `ATTRIBUTIONS.md` by sync-attributions.py; do not hand-edit either. The user
  guide is `docs/USER-GUIDE.md` (the only copy anyone edits).

## Not done yet
- Never loaded into Resolume on macOS. On Windows (Arena 7.27.1 build 15990,
  win-lab, Mesa llvmpipe, 2026-09-23, from the plugin's log) it is instantiated
  as a blend mode at the composition size, gets both inputs padded
  (`1280x720 of 1280x768`), gets `SetTime` every frame in milliseconds and
  `SetBeatInfo` every frame, and takes `Opacity` from the layer's opacity fader.
  No one-input call was seen when the layer below was cleared and refilled;
  whether the transition/autopilot moves `Opacity` is untested; no pixels were
  captured, so a correct render in Arena is not claimed. Key Source is not
  exposed there (above). The fleet's Arena gate cannot gate a mixer — that run
  was a manual probe. The universal build has never run on an Intel
  Mac. (CI ran the checks on the macOS runner's software renderer and passed;
  MSVC compiled the Windows DLL.)
- No presets and no OpenFX port.
- `demo/` is the browser demo at genlock-demo.stoatworks-labs.com: the plugin's
  two shaders, unedited, over a JS port of `Controls.cpp`, `Timing.cpp` and
  `ProcessOpenGL`'s uniforms. Dest is the kit's clip; Src is one of gltest's
  `amiga`/`bar`/`edge` cards, ported. `demo/vendor/` is the shared kit — do not
  edit it; `stoatworks-backend/resolume-demo/sync.sh genlock` copies it in.
  After changing `source/Shaders.cpp`, copy it into `demo/plugin.js` too —
  `python3 demo/tools/check_shaders.py` (run by `verify.sh`) fails on drift.
  Deploy from the repo root: `cf-run npx wrangler deploy`, then check the
  `<title>` of `https://genlock-demo.stoatworks-labs.com/?cb=1`.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume).

    ~/Library/Logs/genlock/genlock.YYYY-MM-DD.log

The first line is written at LOAD time and names the file the host loaded.
Host callbacks, the input guards, the clock and Opacity changes are logged
(on Windows: `%LOCALAPPDATA%\genlock\logs\`); the first Arena run read them
on 2026-09-23 — AGENTS.md, "Reading the log after an Arena run", says what
each line answers. `GENLOCK_LOG_DIR` moves the log; `verify.sh` sets it so the
harness never writes into the one an Arena run is read from.
