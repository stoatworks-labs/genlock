# genlock — orientation for another LLM (or a newcomer)

**What it is:** an FFGL 2.1 **mixer** for Resolume Arena/Avenue that keys this
layer's colour 0 over the layer below the way an Amiga genlock did — badly, on
purpose. C++17 + GLSL 4.10, CMake, universal macOS `.bundle` and a Windows
`.dll`. MIT. Public at `github.com/stoatworks-labs/genlock`, released at
v0.1.0 on 2026-09-23. Never loaded into Resolume on macOS; on Windows, Arena
7.27.1 loads it as a layer blend mode, drives it every frame with both inputs,
`SetTime` in milliseconds and `SetBeatInfo`, and binds its `Opacity` to the
layer's opacity fader — measured 2026-09-23 from the plugin's own log, no pixels
captured. **Arena does not expose Key Source, so in Resolume the key is always
Colour 0.** See "What Resolume does with a mixer". User guide:
`docs/USER-GUIDE.md`.

`CLAUDE.md` is the command reference. This file is the *why*, and it carries two
things worth more than the plugin: **how an FFGL mixer actually behaves**, which
nothing else in the fleet knows, and **the reasoning behind every number in the
harness**, which is the thing four of six plugins got wrong in the last round.

---

## The one idea

**A genlock's key comes from the wrong clock.**

An Amiga genlock keys the computer's colour 0 over incoming video. The video
sets the timing; the computer does not follow it. The key is cut from the
computer's own pixel clock, which free-runs a few parts per million away from
the video's, so **the key edge arrives a pixel or so from the fill it belongs
to — and because the clocks differ, that error walks.**

Everything else is a consequence rather than an effect:

- **A coloured fringe** down one side of every overlay edge. Where the key says
  "computer" and the fill is still colour 0, the palette's background colour
  lands on the video. Where it says "video" and the fill is already the
  graphic, video cuts into the graphic. Which one you get is the *sign* of the
  delay, and both are real.
- **The fringe crawls.** The phase error accumulates at the pixel clock times
  the fractional frequency error, and the line sync pulls it back — so the
  fringe walks as far as `Crawl Wrap` lets it (one Amiga pixel by default) and
  snaps, over and over.
- **Loss of vertical lock.** Below a sync-quality threshold the overlay rolls
  and tears at the seam. The video does not move; the overlay is the thing that
  has lost lock.
- **A three-position fader**, because that is what the hardware had: Video,
  Overlay, Dissolve, and a dissolve pot.

`resolume-luma-keyer` is the keyer that works. Here the flaws are the product,
and the one line in `Shaders.cpp` that fetches the key at a displaced position
is the whole plugin. Everything else is in service of it.

---

## How an FFGL mixer actually behaves

Nothing in the other forty-four repos is an `FF_MIXER`. This section is what was
**measured** at SDK `b1afaf9`, against what the headers implied. Where a claim
here is untested in a host it says so.

### The type is one argument, and nothing else enforces it

`FF_MIXER` is `2`, and it is the **eighth argument of `CFFGLPluginInfo`**. That
is the only thing in the repo that makes this a mixer. It is not checked at
compile time, it does not follow from the base class, and no build step would
notice it being wrong. Verified at the ABI: `plugMain( FF_GET_INFO )` returns a
`PluginInfoStruct` whose `PluginType` is `2` for this bundle and `0` for
rosette's.

`tools/verify.sh` asserts it through `oxbow probe`, which is the only thing in
the repo that reads the plugin the way a host does.

### The input count is a SEPARATE declaration

This is the thing the header does not imply. `SetMinInputs(2)` /
`SetMaxInputs(2)` are nothing to do with the type: the host asks for them
through `FF_GET_PLUGIN_CAPS` with `FF_CAP_MINIMUM_INPUT_FRAMES` (10) and
`FF_CAP_MAXIMUM_INPUT_FRAMES` (11), which are different function codes
entirely. Measured, by calling `plugMain` directly:

| | `FF_GET_INFO` type | cap 10 | cap 11 |
|---|---|---|---|
| this plugin | 2 (mixer) | 2 | 2 |
| rosette (an effect) | 0 (effect) | 1 | 1 |

So **a plugin can declare itself a mixer and still tell the host to give it one
input.** `ffglqs::Mixer` sets the pair in its constructor, which is exactly why
it is easy to assume the type implies it; deriving from `CFFGLPlugin`, as this
does and as the whole fleet does, it has to be said out loud. `verify.sh`
asserts `inputs: 2..2` separately from the type for that reason.

### `GetShortName` is NOT required

The SDK's `Add` example overrides it; `ffglqs::Mixer` does not; the header says
nothing either way. The answer is in `FFGLPluginSDK.h` and `FFGL.cpp`:
`CFFGLPlugin::GetShortName()` returns `0`, and `getPluginShortName()` passes a
null straight back to the host for function code `FF_GET_PLUGIN_SHORT_NAME`
(33). Measured: rosette, which does not override it, returns a **null pointer**
there and has been loaded and instantiated in Arena 7.27.1 regardless. So it is
optional metadata and the `Add` example's override is incidental to it being a
mixer.

This plugin overrides it anyway, with `"GnLk"`, because a host with no room for
"SW Genlock" will show *something* and choosing it is better than letting a
host choose. **Untested in Resolume:** what Resolume does with a short name,
and whether it differs for a mixer, is unknown.

### Which base class, and why not `ffglqs::Mixer`

Two routes exist. `ffglqs::Mixer::createPlugin` passes `FF_MIXER` for you; the
`Add` example ignores that class entirely and declares the type in the info
struct. **This derives from `CFFGLPlugin` directly and declares the type**, for
three reasons:

1. `ffglqs::Mixer` owns the render. Its `Render()` binds the two textures, sets
   four uniforms it names itself (`textureDest`, `textureSrc`, `maxUVDest`,
   `maxUVSrc`) and draws one quad. There is no hook for a second fetch at a
   displaced coordinate, which is the only thing this plugin does.
2. It installs its own vertex shader, which folds `MaxUV` into the varying — the
   one arrangement this plugin must not have (see below).
3. It adds a parameter called `mixVal` whether you want one or not, and the
   fleet's About block, group and parameter machinery all sit on
   `CFFGLPlugin`.

The SDK's own only mixer example took the same route. That is not a coincidence.

### `inputTextures[0]` is Dest, `[1]` is Src, and each needs its own MaxUV

`[0]` is the layer **below** (Dest); `[1]` is **this** layer (Src). For a
genlock that means Dest is the incoming video and Src is the computer's
picture, which is the right way round: the key is cut from Src.

The two are routinely **different resolutions**, and `GetMaxGLTexCoords` is
`Width / HardwareWidth` per texture — so there are two different numbers and
using one for the other is silent. `gltest --mixer` is the first check in the
fleet to assert it: Dest 200×120 used of a 256×256 texture, Src 96×70 used of
128×128, rendered to 320×200. The two ratios differ in both axes on purpose, so
a plugin that used one MaxUV for both fails rather than passing by luck. The
padding is filled with magenta and the check counts how much of it reached the
picture.

**Negative control, run:** replacing `MaxUVSrc` with `MaxUVDest` in the Src
fetch produced 2,634 sentinel pixels, quadrant means 28 codes out of place and a
marker 0.04 of the picture off. Dropping the half-texel inset while leaving
MaxUV correct produced 198 sentinel pixels and nothing else — which is why both
things are checked, and separately.

### Displacements belong in picture space, not texture space

`Add` and `ffglqs::Mixer` both fold `MaxUV` into the vertex shader and hand the
fragment shader two ready-made texture coordinates. That is fine for a plugin
that only ever samples at the fragment's own position, and wrong for this one:
a horizontal displacement expressed in Dest's texture space is a **different
distance** in Src's whenever the two have different padding. So the vertex
shader here passes UV through unscaled and each fetch applies its own MaxUV,
exactly once, inside the fragment shader. Every displacement — key delay,
crawl, roll, tear — is a fraction of the *picture*.

### The scoped bindings must be declared interleaved

Every `ffglex::Scoped*` **clears** its binding on scope exit rather than
restoring it. So the four objects have to unwind as activate(1), bind(1),
activate(0), bind(0), which means declaring them interleaved:

    ScopedSamplerActivation activateDest( 0 );
    Scoped2DTextureBinding  bindDest( dest.Handle );
    ScopedSamplerActivation activateSrc( 1 );
    Scoped2DTextureBinding  bindSrc( src.Handle );

Both activations first would unbind unit 0 twice and leave unit 1 bound. `Add`
gets this right without saying why.

### What Resolume passes with one input — not observed, still open

The spec said to guard on `numInputTextures < 2` and on either pointer being
null, and this does. `gltest --mixer` calls `ProcessOpenGL` with a null input
array, zero inputs, one input, a null Dest and a null Src, gets `FF_FAIL` from
all five, no crash, and renders normally afterwards.

Whether Resolume really does call a mixer with one input while the operator is
patching is the SDK example's claim. **MEASURED 2026-09-23** (Arena 7.27.1 on
win-lab, see below): clearing the layer below while SW Genlock was the upper
layer's blend mode, and then refilling it, produced **no** one-input call — no
`guard:` line in the log. Arena kept passing two inputs, the lower layer's
empty composite. So the claim was not observed in that sequence; it may still
happen in others (an empty bottom layer from the start was not tried). The
guards stay.

### What Resolume does with a mixer

- **Where Resolume reads them from: Extra Effects, the same folder as every
  other FFGL plugin.** This said `Extra Mixers` until the v0.1.0 release, on the
  assumption that Resolume keeps mixers apart. Checked at release: the Arena 7
  binary on the release Mac contains the string `Extra Effects` and an
  `FFGLMixer` class, and no `Extra Mixers` anywhere; the FFGL SDK's README sends
  every plugin -- its `Add` mixer example included -- to Extra Effects. An
  install to Extra Mixers would have put the bundle where Arena never looks.
  **MEASURED on Windows, 2026-09-23** (Resolume Arena 7.27.1, Windows x64,
  Mesa llvmpipe): Arena's `Documents\Resolume Arena` folder has no `Extra
  Mixers` there either; `Genlock.dll` placed in Extra Effects was loaded
  ("Plugin was successfully loaded").
- **How Resolume presents a mixer: as a layer's Blend Mode. MEASURED on
  Windows, 2026-09-23** (same run). Arena registered it as
  `'SW Genlock' uid: GL01 category: 2`, and category 2 is the category of
  Resolume's own blend modes and transitions — Add, Alpha, Cube, Dissolve, Luma
  Key, Wipe Ellipse and the rest all register as category 2. `SW Genlock`
  appears in **every** layer's **Blend Mode** dropdown (REST:
  `/composition/layers/N` → `video.mixer` "Blend Mode" options). So an operator
  chooses a mixer as the blend mode of the upper layer, and the same list
  serves as the transition list. This was a listing, not a render: what the
  host does with the mixer once chosen is below.

**How Arena drives it — MEASURED 2026-09-23** in Resolume Arena 7.27.1 (build
15990) on win-lab (Windows x64, Mesa llvmpipe, no GPU), with a CI (MSVC) build
of the v0.1.0 source. SW Genlock was set as layer 2's Blend Mode over layer 1,
both layers carrying a still picture, driven through Arena's REST API, and the
plugin's own log read back from
`%LOCALAPPDATA%\genlock\logs\genlock.2026-09-23.log`. This was a **manual
probe**: the fleet's Arena gate cannot gate a mixer — it knows registration
categories 1 and 3 only, looks for the plugin in the source list, and can only
mount a clip effect or a source.

- **Load.** Arena scans Extra Effects and loads
  `...\Resolume Arena\Extra Effects\Genlock.dll` — the `plugin loaded` and
  `loaded from` lines are written at scan time — and it registers as category 2.
- **Instantiation as a blend mode.** `instance created`, then InitGL at a
  viewport of **1280×720**, the composition size; `host Resolume Arena version
  7.27.1 15990`; `SetSampleRate 44100`.
- **The two inputs arrive padded.** First frame: `Dest 1280x720 of 1280x768,
  Src 1280x720 of 1280x768` — 768 rows for 720. So MaxUV.y is not 1 in a real
  host, and the per-input MaxUV handling above is exercised, not theoretical.
- **Resolume drives a mixer's clock and transport.** `SetTime` is called before
  frame 1 and on every frame — **412 calls over 412 frames** — in
  **milliseconds** (about 4.1e6, milliseconds since Arena started; the unit
  detector voted ms 4–0). `SetBeatInfo` arrives every frame (128 bpm).
- **One input: not observed.** See the section above.
- **`Opacity` is bound — to the LAYER's Opacity.** `Add`'s comment says "Resolume
  will look for a param named `Opacity` for mix value", which is why the master
  blend carries that name and not the spec's `Mix`. It does: the plugin logged
  `Opacity 1 at frame 1`, `0.42 at frame 78`, `1 at frame 204`, exactly as the
  layer's opacity was set 1 → 0.42 → 1. Writing the mixer's own `Opacity`
  parameter (0.61, then 0.13) through the API never reached the plugin, and
  Arena read it back as the layer's 0.42. So in Resolume the master blend is
  the **layer's opacity fader**, not a slider in the mixer's panel. Whether the
  layer's transition or autopilot also drives it was **not tested**.
- **Parameters: 25 of 26 exposed, and the missing one is Key Source.** Arena's
  mixer panel (REST `video.mixer`) shows 25 of the 26 declared parameters, and
  every name, type and default matches the declaration — except that **Key
  Source** (id 0, the first parameter) is **not exposed at all**: it is absent
  from the layer's JSON. Why is not known; one guess is that Arena treats a
  mixer's first parameter specially. The consequence: **in Resolume the key
  source is stuck at its default, Colour 0**, and Luma and Alpha keying cannot
  be reached. See the trap below. No code was changed for it.
- **No pixels were grabbed.** A mixer's output exists only in the composition,
  and Arena's REST thumbnails do not serve it. So "renders correctly in Arena"
  is **not** claimed. What is claimed is that it initialised and was called
  every frame with no error lines in the log.

Still open after that session: whether a mixer is called with one input in any
other patching sequence, whether the layer's transition or autopilot moves
`Opacity`, why Key Source is hidden, and anything at all on macOS. See "Reading
the log after an Arena run" for what each log line answers.

---

## Every number in the harness

The last fleet round shipped four plugins whose numeric checks passed on this
Mac's GPU and failed on a GPU-less CI runner, and in all four cases the **test**
was wrong. So every check here was gone over with two questions: *would this
hold on a different rasteriser?* and *would this hold at a different raster?*

Two rules came out of it and run through the whole harness:

- **A whole-texel displacement and a fractional one are different physics.** A
  whole-texel offset lands every sample on a texel centre, so the filter weight
  is 0/1 and the image is translated *exactly* — that is a byte-equality claim,
  not a tolerance. A fractional offset exists only in the filter's own
  interpolation, for which GL promises no more than eight bits of subtexel
  precision. Conflating them is what broke rosette. They are never measured by
  the same assertion here.
- **An exactly-zero cancellation is bitwise only while both paths round
  identically.** Where this repo claims bitwise it names the arithmetic. That is
  what broke vocoder.

And one that is less of a rule than a habit: every differential measurement got
an **absolute anchor** beside it, because a difference cancels a constant bias.
A deliberate third-of-a-texel bias on the key fetch passed every differential
check in `--delay` and was caught only by the zero-delay anchor.

| Check | The number | Where it comes from |
|---|---|---|
| `--mixer` guards | none | Five `FFResult` comparisons. No raster, no rasteriser. |
| `--mixer` sentinel | **135** in summed channel difference | The nearest legitimate card colour is 302 (Dest) and 315 (Src) away from the magenta padding, and the check asserts that too. Half of that is "more padding than picture" — unreachable by any blend of two card colours, and scored 0 by a whole leaked pixel. The first version used a per-channel box that sat 55 away from a magenta quadrant of the card itself. |
| `--mixer` quadrant means | **1 of 255** | One 8-bit code: the smallest difference the readback can express. Bilinear interpolation of a *constant* region is the constant on any rasteriser. A wrong MaxUV moves a quadrant boundary by tens of pixels, so it fails by tens of codes. The measured interior is inset by `ceil(out/used) + 1` pixels — the width a boundary can smear over, stated in terms of both rasters. |
| `--mixer` marker position | **one source texel**, in each axis | The quantum the marker's own edges are drawn on. Finer would be asserting something about the filter; coarser would miss a swapped MaxUV. Measured 0.0006 and 0.0000 of the picture against tolerances of 0.005 and 0.010. |
| `--delay` whole-pixel translation | **zero bytes** | −6 and −8 Amiga pixels are 4 output texels apart *at this raster*, which the check asserts rather than assumes (at 641 wide they would be 4.006 apart and the comparison would be meaningless). A whole-texel translation is exact on any conforming rasteriser. Held: 0 of 164 bytes. |
| `--delay` fractional | **0.25 output texels** | Derived in the code from this raster and this card: 2 × (rampPx × 0.5/contrast + 1/256) = **0.031**, and the check asserts the bound is at least three times inside the tolerance, so a wider raster says so instead of failing mysteriously. 0.25 is also half the 0.5-texel error a plugin would make by rounding the fraction away — the failure the check exists for. Measured 0.4941 against 0.5000. |
| `--delay` fill unmoved | **zero bytes**, twice | Past the card's ramp the key is *exactly* 0 — `smoothstep` clamps, and `mix(fill, video, 0)` is `fill*1 + video*0`, exact in IEEE-754 and still exact under FMA contraction. And with the key forced off, all three delays render the same frame including the ramp. |
| `--delay` zero-delay anchor | **zero bytes** | At Key Delay exactly 0 the key is fetched from exactly where the fill is, so the disagreement is exactly 0 and `mix(rgb, tint, Fringe*0)` returns rgb whatever Fringe is. The one absolute statement in the check. Paired with "at a real delay Fringe does something" (157 codes) so it cannot pass on a dead control. |
| `--crawl` phase | **1e-9 Amiga px** | Pure double arithmetic on the CPU, no GPU involved. It checks the *plumbing* — epoch, clock unit, ppm→rate, the parameter path — not the modulo, which both sides share. Measured exactly 0. |
| `--crawl` pixels | **0.25 output texels** | The same derivation, recomputed for this raster (bound 0.055, tolerance asserted to be 3× it). Every frame of a crawl is at a fractional offset, so every one of these is a partial-coverage measurement. Measured worst 0.0176 over 30 frames. |
| `--crawl` wrap count | **half a period**, in texels of this raster | The drop that counts as a wrap is `kCrawlWrapAmigaPx × pxPerAmiga × 0.5`, not a constant. At 320 wide a fixed 1.0-texel threshold would have missed every wrap. |
| `--crawl` signal margin | **8 × the tolerance** | A self-check that the raster gives the measurement enough to work with. One period of travel is 4.0 output texels here, 16× the tolerance. |
| `--roll` locked | **zero bytes** | Losing lock is a branch, not a fade: above the threshold the phase is exactly 0 and `fract(p.y + 0)` returns `p.y`. |
| `--roll` whole-row | **zero bytes** | A whole-row advance is a rotation, and a rotation of a sampled image is exact. The frames it applies to are found from the raster (`rate × H / fps`), not listed — and "there was at least one" is part of the assertion, so a raster where none landed on a whole row would fail rather than quietly stop making the exact claim. |
| `--roll` position | **0.05 rows** | Bound under 0.01 rows: the bilinear reconstruction nulls the first alias (< 0.001), 8-bit weight error on ~4 partial rows moves a centroid by ε/2 ≈ 0.002 **independently of the raster**, and subtexel precision adds 1/256. 0.05 is five times that and twenty times smaller than a one-row error. Measured 0.0000 at both rasters. |
| `--roll` raster | **two rasters** | 640×360 advances a whole 6 rows per frame; 480×270 advances four and a half. This is the check that found the bug: the first version thresholded the band instead of weighting it, so it quantised to whole rows, passed at 360 and failed at 270 by exactly half a row. The *test* was wrong. |
| `--roll` zero-phase anchor | **0.05 rows** vs the card's own centre | Computed from the generator's rounding, not from `centre × height`: at 270 rows those differ by half a row, which is the size of the error being looked for. |
| `--fader` bitwise | **zero bytes** | Stated with its conditions. The arithmetic is `mix(video, video, 1.0)` = `video*(1-1) + video*1`: (1-1) is exactly 0, x*0 is 0, x*1 is x, 0+x is x, and an FMA contraction computes the same because neither factor is rounded. The *fetch* is exact only because the output raster equals Dest's and MaxUV is exactly 1, so every sample is a texel centre. Change either and it becomes a resample — which the check then demonstrates at 400×240 into 640×360 and **does not assert**, because a tolerance wide enough to cover a resampled test card's edges would be wide enough to cover a broken fader. |
| `--fader` at other Opacity | **1 code** | `video*(1-o) + video*o` re-rounds twice and is not an exact cancellation. One code is the readback's floor. Measured 0. |
| `--fader` dissolve halfway | **1 code** | Also the readback's floor. It was 8 at first, which was fitted to this picture. Measured 127 and 128. |
| `--modes` crawl, arithmetic | **bitwise** | `rate / width` for 40 settings of Clock Error × Crawl Rate, each mode against lores. Exact because both the clock and the width are the lores value times the same power of two: scaling a double by 2 or 4 is exact, and IEEE division of the same real quotient rounds the same way. No GPU in it. |
| `--modes` crawl, picture | **zero bytes** across modes, and **0.25 texels** against the physics | Hires and superhires frames are compared to lores byte for byte, frame for frame, over 24 frames of an 8 lores px/s crawl — the same uniform bits through the same program object, so GL's repeatability rule is all it rests on. And each mode's key edge is measured against the closed form **in lores pixels**, not against the other modes, which would pass a plugin wrong the same way in all three. Worst 0.0083 texels at 640×360, 0.0619 at 320×180. |
| `--modes` Key Delay | **0.25 texels**, and **zero bytes** | −8 of the mode's pixels is −16, −8 and −4 texels at 640 wide and −8, −4, −2 at 320 — whole numbers, asserted. Measured −16.0024, −8.0024, −4.0024 and −8.0045, −4.0045, −2.0045 (a constant −0.002/−0.005 that is the card's own quantisation, identical in every mode). And hires at −8 is lores at −4 **byte for byte**, superhires at −8 is lores at −2: (−8)/640 and (−4)/320 are the same float, which is checked on the CPU first. |
| `--modes` tear | **0.25 texels**, and **zero bytes** | The fill's edge (key off) in the two seam rows against the middle row, which leans not at all, at roll phase exactly 0. Predicted from the shader's own lean and the float throw: −28.1333 texels at 640, −13.7333 at 320; measured −28.1294 and −13.7294 in all three modes. The bound uses 85 codes of contrast, not 170, because the fill runs 170→255 in blue. The three torn frames are the same bytes. |
| `--modes` wrap | **1e-9 px**, **0.25 texels**, a **wrap count**, and the **reach** | Crawl Wrap at 4 pixels of the mode, lores and hires, 60 frames: the reduced phase against `PositiveMod(rate·t, 4)`; the key edge against it in texels; 3 wraps (lores) and 6 (hires) predicted and counted with a half-period threshold in texels of THIS raster and mode; and the furthest the key walks before it snaps within one frame's travel of the wrap and never past it — 7.933 of 8.000 texels, 3.933 of 4.000, 3.974 of 4.000, 1.974 of 2.000. The rate (14 lores px/s) puts no wrap on a frame boundary, so no count depends on which way a rounding falls. |
| `--defaults` | **bitwise** | The plugin's reduced crawl and delay over 60 frames against the pre-feature formula restated with its literals (7093790 Hz, a wrap of 1.0), at Clock Error 0.6 so the one-pixel wrap is crossed eleven times. The same double arithmetic in the same order, so equal is the only honest claim. Plus every control's name, place and default, 26 parameters (21 controls and the 5-entry About block), About last. |
| `--mutation` | **fails** | The shipped fragment shader with `uv - vec2( KeyDelay` changed to `uv + vec2( KeyDelay` — one character, asserted — fails the Key Delay claims; the unmutated text through the same test hook passes them and renders the default path's bytes. |
| `--bench` | not asserted | There is no threshold worth asserting on somebody else's GPU. |

**Negative controls actually run**, because a check that cannot fail is not a
check: wrong MaxUV (3 assertions fail), no half-texel inset (1 fails), roll rate
off by 0.1% (4 fail at each raster), one code added to the output (2 fail),
constant bias on the key fetch (the zero-delay anchor fails, and nothing else
does — which is why the anchor is there). Those were run once, by hand.

**The mode checks ship theirs.** `Genlock::SetFaultForTest` builds the wrong
answer to each "does it scale?" question into the real `ProcessOpenGL` — the
crawl on the lores clock in every mode, Key Delay in lores pixels, the tear in
the mode's pixels, Crawl Wrap ignored — and `--modes` re-runs every claim of
the matching check against it, silently, then asserts that **a claim measured
out of the picture** failed. (Not merely any claim: a fault caught only by the
plugin's report of its own phase would prove the plumbing is checked, not the
pixels. Before this requirement the wrap control's negative control reported
its failure through the plugin's phase report, which proves nothing about the
pixels.) All five fail in the picture at both rasters. `--defaults` does the
same with the controls themselves: Crawl Wrap at 16 (54 of 60 frames differ)
and Amiga Mode at Hires (59 of 60) both break the old-formula comparison.

**The GLSL mutation test ships too** (`--mutation`), and verify.sh runs it.

### Would this hold on another rasteriser, at another raster?

One line per check added with Amiga Mode and Crawl Wrap. Every one runs at
640×360 **and** 320×180, CI's raster, and every window, prediction and bound in
it is computed from the raster rather than typed.

- **Crawl, arithmetic** — no rasteriser in it at all. Exact by IEEE-754.
- **Crawl, byte identity across modes** — yes: identical uniform bits through one
  program object on one set of textures, and GL's repeatability rule requires
  the same result. It does NOT depend on the rasteriser computing the right
  answer, only the same one twice.
- **Crawl, edge vs physics** — yes, same derivation as `--crawl`; the bound is
  0.031 texels at 640 and 0.020 at 320 against a 0.25 tolerance. Every frame is
  a fractional offset, so this is a partial-coverage measurement throughout.
- **Key Delay, edge** — yes at both: every predicted shift is a whole number of
  texels at both rasters, asserted, and the measurement is still the
  partial-coverage integral, so it does not rely on that.
- **Key Delay, byte identity with lores** — yes: same float uniform, checked on
  the CPU before the GPU is asked.
- **Tear** — yes: the throw is a fractional shift, measured by integrating the
  fill's own blue ramp, which translates exactly under linear interpolation;
  the bound (0.055 texels at 640, 0.031 at 320) is from an 85-code contrast.
  Lean is computed from the shader's own formula at the row centre, so a
  different raster moves the prediction, not the tolerance.
- **Wrap** — yes, with one margin that is tight by construction: 4 hires pixels
  at 320 wide is exactly 2 texels, exactly the "8 × tolerance" signal floor,
  asserted with `>=`. A narrower raster than 320 would fail that assertion
  rather than the measurement, which is the intended way to fail.
- **Defaults** — no rasteriser: Timings against a double-precision formula.
- **Mutation** — the byte identity between the default path and the hook is two
  compiles of the same text in one context. A compiler that compiled the same
  source differently twice would fail it; none is known to.

**What this pass does not prove** is unchanged from above: derived is not proven
for every rasteriser.

**What this pass does not prove.** Every tolerance is derived from the GL spec's
own guarantees rather than from a measurement — but *derived* is not *proven*.
One second rasteriser has now run it: on 2026-09-23 `ci.yml` ran on GitHub's
macOS runner, which has no GPU, so the harness fell back to Apple's software
renderer, and all 9 ctest suites (names, mixer, delay, crawl, roll, fader,
modes, defaults, mutation) and the sweep at 320×180 passed. That is two
rasterisers, not all of them; llvmpipe has not run the checks.

---

## The traps

Ordered by how much time they cost.

**`Softness` at zero makes a fractional delay unmeasurable.** At zero the key is
a step in colour distance, so however finely the fetch is displaced the key
still snaps to whichever side of the threshold the pixel centre lands on. Every
measurement came back a whole number of texels and a half-texel delay was
reported as a whole one. It looks exactly like the plugin rounding the delay,
and it is the harness. A ramped key is what turns the sub-texel part into
partial coverage there is anything to recover.

**Interpolating a 50% crossing is not translation-invariant.** The first
measurement found the crossing by linear interpolation between the two columns
that straddle it. That has a bias from the curvature of the S-curve, and the
bias *changes with where the sample grid falls relative to the edge* — which is
precisely the difference between a whole-pixel offset and a fractional one, and
so precisely the thing being measured. **Integrating** the key across a window
that holds the whole transition has no such bias: it is a linear functional, so
it translates exactly, every column contributes once, and the error is just the
quantisation of the few partial columns.

**A thresholded centroid quantises to whole rows.** Same failure in a different
costume, in `--roll`. Counting rows whose lit fraction passes 50% gives an
answer on the row lattice, so a roll that advances 4.5 rows per frame reads as 4
or 5. It passed at 360 rows and failed at 270 by exactly half a row. Weighting
each row by how much of the band it holds fixes it, and the weighted circular
vector sum rotates exactly when the picture rotates, whatever shape the band's
edges are.

**`StoatworksAboutParams.h` must be included AFTER `<FFGLSDK.h>`.** It names
`FFUInt32` and does not pull the SDK in itself. The failure is two "unknown type
name" errors pointing at the About header rather than at the include order. The
header says so in its own comment; that comment was read after the error.

**The plugin registers itself from a file-scope constructor.** `CFFGLPluginInfo`
is never referenced by name, so in a **STATIC** archive the linker may drop the
whole translation unit, giving a bundle that loads, exports `plugMain`, and
reports that it contains no plugins. `genlock_core` is an **OBJECT** library for
that reason.

**A ranged parameter cannot have a ranged default.** `SetParamInfo` clamps an
`FF_TYPE_STANDARD` default into 0..1 *before* returning, and `SetParamRange` can
only be called afterwards. So every host parameter here is 0..1 and the
conversions live in `Controls.cpp`.

**Override `SetTextParameter`** to return `FF_SUCCESS` for the About block, or
no host can instantiate the plugin at all: `instantiateGL` pushes every declared
default back through the setters and deletes the instance if one fails, and the
base class's `SetTextParameter` is a stub that returns exactly that failure.

**Arena hides a mixer's first parameter — so Key Source cannot be reached in
Resolume.** Measured 2026-09-23 on Arena 7.27.1 (Windows): the mixer panel
(REST `video.mixer`) exposes 25 of the 26 declared parameters, every one
matching, but Key Source — id 0 — is absent from the layer's JSON. The key is
stuck at the default, Colour 0; Luma and Alpha are unreachable there. Nothing in
the harness or `oxbow probe` can see this, because both read the declaration,
and the declaration is right. Why Arena drops it is not known (one guess: it
treats a mixer's first parameter specially). Not fixed in v0.1.0; if it is
fixed, it is by what is declared at index 0, and it has to be re-checked in
Arena, not in the harness.

**Arena's REST `Opacity` on a mixer is the layer's.** Resolume binds a mixer
parameter named `Opacity` to the layer's Opacity, so writing the mixer's own
`Opacity` through the API is overridden and reads back as the layer's value. A
probe that sets the mixer's slider and waits for the plugin to see it will wait
forever; set the layer's opacity.

**Resolume's clock overflows a float.** It counts milliseconds from the session
start, and past about 4.99e8 a 32-bit float can no longer represent consecutive
milliseconds. Anything computed from an absolute host time in float stops
moving. `Timing.h` takes the first reading as an epoch, works in double, and
hands the shader a phase already reduced into its own period — under `Crawl
Wrap` for the crawl, under one picture height for the roll.

**The host's clock unit has to be worked out by observation.** The FFGL header
never says whether `SetTime` is seconds or milliseconds and hosts disagree;
Resolume sends milliseconds (to a mixer too: measured 2026-09-23 on Arena
7.27.1, about 4.1e6 ms, the detector voting ms 4–0). `timing::Clock` votes on the ratio against a real
clock for four frames, exactly as tinsel does. The harness declares its unit
instead of letting the calibration infer one.

**A key delayed to the RIGHT cannot be read with a flat-fill inversion.**
`keyAcrossRow` recovers the key as `(out - fill) / (video - fill)` with the fill
taken as colour 0 everywhere — true only left of the card's edge, which is why
`--delay` and `--crawl` keep the delay negative. A crawl that starts from a zero
delay walks the key rightwards, into the ramp and the fill, and the recovered
key there is nonsense. `keyOverCard` inverts column by column with the card's
own value, which is valid because the fill fetch is not displaced while locked
and lands on texel centres at matched rasters; read off blue, the contrast
never drops below the 170 codes the old bound assumed.

**Two translation units' file-scope objects construct in no promised order.**
`Diag::init` is now reached from a file-scope constructor in Genlock.cpp, at
load time. If Diag.cpp's own file-scope `std::string` for the log path were
constructed AFTER that, it would reset the path to empty and every later line
would be dropped silently. The state is a function-local static now.

**`fork` inside `dlopen` can hang.** The log directory used to be made with
`std::system( "mkdir -p ..." )`. Harmless at instantiation; not at load time,
on whatever host thread is scanning plugins, where the forked child can inherit
a malloc lock another thread holds. `mkdir(2)` in a loop instead.

**The harness writes to the log an Arena run is read from.** Every `gltest` and
every sweep render instantiates the real plugin. Its lines say `loaded from
.../gltest`, so they can be told apart, but `verify.sh` points
`GENLOCK_LOG_DIR` at a temp directory so they never land there at all. Do the
same by hand before running the harness around an Arena session.

**`cc` on this machine is a shell function**, not a compiler — it changes
directory into `~/Projects`. A scratch C file compiled with `cc` fails with "no
project matching '-o'". Use `/usr/bin/clang`.

**`make` skips a rebuild when the edit lands in the same second as the last
build.** Restoring a file from a backup between two builds inside one second
left a stale object, and a check that had just been made to fail carried on
failing after the fix. `touch` the file, or `sleep 1`.

---

## Shape of the code

    source/Shaders.cpp      the pass. One vertex, one fragment. All of it.
    source/Genlock.*        the plugin: type, parameters, the two inputs.
    source/Controls.*       0..1 host parameters to Amiga pixels, ppm, rolls/s.
    source/Timing.*         the two clocks, the epoch, and the phase reduction.
    source/Diag.*           a log file, for the shader that will not compile.
    tools/gltest/           the offline harness. Two inputs.
    tools/sweep.py          no control is silently dead.
    tools/verify.sh         all of it.
    demo/plugin.js          the browser demo: the two shaders, unedited, and a
                            JS port of the CPU half.
    demo/tools/             check_shaders.py -- the two copies agree.
    demo/vendor/            the shared kit, copied in by sync.sh. Do not edit.

One pass, three fetches: the video at the fragment's position, the fill at the
overlay's rolled position, and the key at the overlay's rolled position
displaced by the delay. Everything is in picture space, 0..1 across the output,
with each input's `MaxUV` applied once at its own fetch.

---

## The browser demo

`demo/` is the page at **genlock-demo.stoatworks-labs.com**, added 2026-09-24
after the release. It is a *port*, not a recording and not the plugin — the
fleet's first demo of a **mixer**, so several of its decisions are new.

- **The shaders are the plugin's.** `VERTEX_SHADER` and `GENLOCK_SHADER` in
  `demo/plugin.js` are `kVertexShader` and `kGenlockShader`, unedited;
  `demo/tools/check_shaders.py` compares them character for character and
  `tools/verify.sh` runs it (step "demo"). Change the C++, copy it across.
- **The CPU half is a port that only a reader checks**: `Controls.cpp`'s
  conversions (with `Math.fround` where the C++ is float), `Timing.cpp`'s
  `CrawlPhase`/`RollPhase`/`PositiveMod`, and `ProcessOpenGL`'s uniform
  arithmetic — the crawl added to the key delay before the division by the
  mode's width, the tear thrown in lores pixels in every mode, the lock
  threshold as a branch. The harness's fault switches are not ported.
- **Two inputs, and the kit makes one.** Dest (`inputTextures[0]`, the layer
  below) is the kit's clip, so the Clip dropdown and "Use my own…" both drive
  it; the page relabels them "Layer below (Dest)" and "Own video below…". Src
  (`inputTextures[1]`, this layer) is one of gltest's own cards — `amigaCard`,
  `barCard`, `edgeCard`, ported pixel for pixel from `tools/gltest/main.cpp`
  and uploaded bottom row first as gltest does — chosen with the kit's
  `demo.variants` dropdown ("This layer (Src)"), because which picture is on
  the other layer is not a parameter. All three are on #0055AA, the Key Colour
  default, so the default key finds them. Chosen over a new generated "Amiga"
  clip because those cards are what the plugin was measured on; a picture
  invented for the page would be a claim nothing backs.
- **Both inputs are the composition's size and unpadded**, so both MaxUVs are
  exactly 1 — the padding Arena really sends (1280x720 of 1280x768) is not
  reproduced. The half-texel insets are each input's own. Disclosed.
- **Key Source is on the panel** although Arena hides it: the plugin declares
  it. Its hint and the disclosure say that in Resolume the key is always
  Colour 0. **Opacity is a slider**, and the hint says in Resolume it is the
  layer's opacity fader.
- **The clock is the page's**: already seconds from zero, so `Clock`'s unit
  vote and epoch are skipped. Restart is a new epoch.
- **Presets are the page's**, labelled as such; the plugin ships none. A
  status line under the picture shows key delay + crawl, the crawl rate and
  the lock state — the numbers the Diag log carries.
- **The About block is absent.** No audio caveat: Genlock has no audio path.
- **The banner says "FFGL mixer"** because `plugin.js` sets `kind: 'mixer'`
  (the kit's closing sentence said "effect" on every page until 2026-09-24).

Deploy from the repo root with `cf-run npx wrangler deploy` and verify by
content: `curl -s 'https://genlock-demo.stoatworks-labs.com/?cb=1' | grep -o
'<title>[^<]*'`. `.github/workflows/deploy.yml`
(the fleet's, from idler) redeploys the Worker on every push to main and checks
the live `<head>` against the build.

---

## Decisions taken without asking

The brief said to decide and write it down. These are the ones that are not
obvious.

**`Opacity`, not `Mix`.** The spec's Look group named the master blend `Mix`.
The SDK's only mixer example says Resolume looks for a parameter named
`Opacity`, and `ffglqs::Mixer` uses `mixVal`. Of the three names, one has
evidence. See the mixer section above.

**`Crawl Rate` is a dimensionless multiplier on the drift rate, and it
duplicates `Clock Error`.** Both scale the same quantity, and that is said
plainly rather than dressed up: `Clock Error` is the physical statement in ppm,
`Crawl Rate` lets an operator freeze or exaggerate the crawl without restating
it, and 0 freezes it *without* claiming the clocks are locked. The alternative
— making `Crawl Rate` the wrap span in Amiga pixels — would have been
non-redundant and physically meaningful, but a control called "Rate" that is a
distance is worse than a redundant multiplier.

**The crawl wraps — by default at one Amiga pixel, and now at up to sixteen.**
A free-running clock at 1 ppm slides seven pixels a second, which as a picture
would slide off the screen. A real genlock re-locks on every horizontal sync, so
only the remainder survives — the fringe walks and snaps back. One pixel is the
remainder of a genlock that re-locks cleanly (the phase is quantised by the
pixel clock, so less is not a thing the hardware can hold), and it is the
default: exactly what this plugin did before `Crawl Wrap` existed. Sixteen is
the top because it is the colour burst: the lores clock is exactly 1.6 × the PAL
subcarrier and the burst is ten cycles, so 16 lores pixels long, and a phase
error bigger than the burst gate is a genlock that has lost lock — `Sync
Quality`'s job, not this control's. That ceiling is reasoning from the burst's
length, not a measurement of any genlock.

**`Crawl Wrap` is linear, and counted in pixels of the current mode.** Linear
because it is a distance the fringe walks and a 16× span does not need a curve
(`Clock Error` is geometric because its span is 5000×). In the mode's pixels
because the remainder the line sync leaves is quantised by the pixel clock, so
it is a count of clocks, exactly like `Key Delay` — 4 hires pixels is 2 lores.
`--modes` measures it in both.

**Amiga Mode: Lores, Hires, Superhires, first in the Timing group.** It sets the
unit every other Timing control is stated in, so it sits above them. A quantity
that is a count of the computer's own pixel clocks scales with it (`Key Delay`,
the wrap); a time error in the incoming video does not (the crawl's speed across
the picture, the tear's throw). `Controls.h` says which is which beside each
constant, and `--modes` holds the plugin to it. Superhires is included because
it completes the set and its clock is exactly 4× lores; most real genlocks
could not resolve a 28 MHz pixel, and that is said in the header rather than
used as a reason to leave the arithmetic out. It is parameter 7, Crawl Wrap is 11, and
the defaults reproduce the pre-feature render byte for byte (below).

**The tear's throw stays in lores pixels in every mode.** A tear is the incoming
sync collapsing, a time error; scaling it with the overlay's resolution would
say the computer's pixel size changes how far a failing sync throws a line.

**The negative controls live in the shipping class.** `SetFaultForTest` and
`SetFragmentShaderForTest` are two setters and five conditionals in
`ProcessOpenGL`/`InitGL`, unreachable from a host (nothing but the setters
changes them, and both start off). The alternative — perturbing the harness's
inputs to imitate each fault — would test the imitation.

**`Clock Error` is geometric, 0.01 to 50 ppm.** The interesting range is all at
the bottom: at 0.01 ppm the fringe takes fourteen seconds to walk one Amiga
pixel, and above about 1 ppm the phase slews faster than seven pixels a second
and stops reading as a crawl at all. 50 ppm is an ordinary crystal's tolerance
and looks like a blur, which is the honest answer rather than a reason to
shorten the range.

**One Amiga pixel is 1/320, 1/640 or 1/1280 of the picture width at every
raster**, by mode. An Amiga line is that many pixels across the active picture
whatever the monitor is, so the fringe is the same *fraction* of the frame at
720p and at 4K — the only definition under which an operator's setting means
the same thing twice. PAL is assumed throughout (7.09379 MHz lores pixel clock);
there is no NTSC switch.

**The roll rate does not scale with how far the sync has fallen.** Below the
threshold the overlay rolls at exactly `Roll Rate`; only the *tearing* scales
with the severity. That is a testability choice as much as a physical one — a
rate that varied with the quality would have no closed form to check against.

**No presets.** Every other recent plugin in the fleet ships a preset table and
the machinery that keeps a host from un-setting it. Twenty-one controls in four
groups did not seem to need one, and the preset machinery is the single largest
source of host-behaviour assumptions in the fleet. It can be added later without
moving anything.

---

## What is genuinely verified, and what is assumed

**Verified, by measurement, on this machine (Apple M4 Max, macOS 26.4.1),
2026-09-22:**

- **Two inputs at two sizes with two MaxUVs resolve independently.** Dest 200×120
  used of 256×256, Src 96×70 used of 128×128, rendered to 320×200: **0 sentinel
  pixels** from either texture's padding, every quadrant within **0.000 of 255**
  of its own colour, and a known marker within **0.0006 and 0.0000** of the
  picture against tolerances of one source texel.
- **The guards hold.** A null input array, zero inputs, one input, a null Dest
  and a null Src all return `FF_FAIL` without crashing, and two real inputs
  render afterwards.
- **A whole-pixel key delay translates the key exactly** — 0 of 164 bytes differ
  across a 4-texel shift — and **a fractional one lands as partial coverage**,
  measured at **0.4941 output texels against a predicted 0.5000**, tolerance
  0.25, derived bound 0.031.
- **The fill does not move**: 0 differing bytes past the edge across three
  delays, 0 differing bytes over the whole frame with the key off, and at zero
  delay the fringe controls provably do nothing (0 bytes) while at a real delay
  they do (157 codes).
- **The crawl matches the closed form.** At 1.127747 ppm the predicted rate is
  7.999999 Amiga px/s; the plugin's own reduced phase matches to **0.000e+00**
  over 30 frames and the key edge is where that phase says to within **0.0176
  output texels**. It wraps at one Amiga pixel, **3 snaps in 30 frames**, as
  predicted.
- **The roll advances at the stated rate and wraps at the raster's height**, at
  **two rasters**: 640×360 (6 whole rows per frame, 23 of 23 frames are exact
  rotations) and 480×270 (4.5 rows per frame, 11 of 23 exact rotations and 12
  fractional). Worst position error **0.0000 rows** at both, tolerance 0.05. The
  picture returns byte-identical after exactly one period, and half a period is
  half the raster to three decimals.
- **Losing lock is a branch.** Locked, two frames a second apart are byte-identical.
- **At Fader = Video the output IS Dest, bitwise** — 0 bytes, at matched
  rasters with no padding, for the stated reason. Overlay with no key is Src
  bitwise; Dissolve reaches both ends bitwise. At an unmatched raster it is a
  resample and no claim is made.
- **Amiga Mode scales what it should and nothing else**, at 640×360 and 320×180
  (2026-09-23): the crawl's speed across the picture is bit-identical in all
  three modes (rate/width for 40 settings; the plugin's reduced phase over 24
  frames; and the frames themselves, byte for byte), and each mode's key edge
  follows the lores physics to 0.0083 / 0.0619 texels. Key Delay −8 moves the
  key −16.0024, −8.0024 and −4.0024 texels at 640 (predicted −16, −8, −4), and
  hires at −8 is lores at −4 byte for byte. The tear throws −28.13 texels at 640
  in every mode, and the torn frames are identical. Each with a shipped
  negative control that fails in the picture.
- **Crawl Wrap wraps where it says**: 4 pixels of the mode, 3 wraps (lores) and 6
  (hires) in 60 frames as predicted, the key reaching within one frame's travel
  of the wrap before it snaps, the phase exactly the closed form.
- **The defaults are the old plugin.** The reduced crawl and delay match the
  pre-feature formula bit for bit over 60 frames. And, once, by hand: cf17b14's
  own `gltest` (built from `git archive`) and this one rendered **byte-identical
  PNGs** for eight scenes — defaults at 1280×720 and 30 frames, a fast crawl, the
  fastest crawl at 320×180, Key Delay −8, a rolling overlay, the edge card, and a
  dissolving torn overlay — while Crawl Wrap 16 and Amiga Mode Hires both
  differed. That comparison is not in the harness (it needs the old binary);
  `--defaults` is its standing proxy.
- **A one-character mutation of the shipped GLSL fails a check** (`--mutation`).
- **No dead controls.** All **21** sweepable parameters measurably change the
  picture, at 480×270 and at CI's 320×180; the other five are the About text and buttons,
  which `tools/sweep.py` skips.
- **The load-time log line works in a real `dlopen`**: `oxbow probe` loads the
  bundle and the log says `loaded from .../Genlock.bundle/Contents/MacOS/Genlock`
  — `verify.sh` asserts it. On macOS that is the only log line exercised by
  anything but the harness; on Windows, Arena 7.27.1 exercised all of them
  except the `guard:` lines (2026-09-23, below).
- **The build is universal and exports `plugMain`** — `lipo` reports
  `x86_64 arm64`, `nm -gU` finds `_plugMain`, the plist names a binary that
  exists, and it ad-hoc signs.
- **A host sees `SW Genlock` / `GL01` / mixer / inputs 2..2 / 26 params** (25 until the
  About block gained its User guide button at registration)
  through `oxbow probe`, with Amiga Mode at index 7 and Crawl Wrap at 11 in the
  Timing group.
- **The checks pass on a second rasteriser** (2026-09-23): `ci.yml` on
  GitHub's GPU-less macOS runner, Apple's software renderer — all 9 ctest suites
  and the sweep at 320×180. See "Every number in the harness".
- **The Windows DLL compiles**: MSVC on GitHub's Windows runner, 2026-09-23.
- **Arena on Windows loads it and lists it as a blend mode** (Resolume Arena
  7.27.1, Windows x64, Mesa llvmpipe, 2026-09-23): loaded from Extra Effects,
  registered as `'SW Genlock' uid: GL01 category: 2`, present in every layer's
  Blend Mode list. See "What Resolume does with a mixer".
- **Arena drives it as a blend mode** (Arena 7.27.1 build 15990 on win-lab,
  Windows x64, Mesa llvmpipe, CI MSVC build of v0.1.0, 2026-09-23; a manual
  probe through the REST API, read from the plugin's own log): instantiated with
  InitGL at 1280×720; both inputs arrive padded (`1280x720 of 1280x768`);
  `SetTime` on every frame (412 of 412) in milliseconds, and `SetBeatInfo` every
  frame; no one-input call when the layer below was cleared and refilled;
  `Opacity` followed the **layer's** opacity 1 → 0.42 → 1 and the mixer's own
  slider was overridden; 25 of 26 parameters exposed, matching the declaration,
  with **Key Source missing**. No error lines. No pixels were captured, so a
  correct render in Arena is not claimed. See "What Resolume does with a
  mixer".
- **The render cost**, by `gltest --bench` (120 frames each, after a 20-frame
  warm-up, `glFinish` on both sides):

  | | ms/frame, worst of five runs | % of a 60fps frame |
  | --- | --- | --- |
  | 1280×720 | 0.020 | 0.1% |
  | 1920×1080 | 0.036 | 0.2% |
  | 2560×1440 | 0.054 | 0.3% |
  | 3840×2160 | 0.118 | 0.7% |

  One pass and three texture fetches is about as cheap as an FFGL plugin gets
  — cheap enough that the measurement is the unreliable part. Repeated runs of
  `--bench --frames 120` vary by a factor of two (4K came back at 0.042 on one
  run and 0.107 on the next), because a tenth of a millisecond of GPU work is
  close to what a `glFinish` round trip costs to observe. **Take the ceiling,
  not the mean, and do not read a 20% change in these as a regression.** A
  plugin whose cost mattered would need a longer run and a quiet machine.

**Assumed, or not yet done:**

- **Never loaded into Resolume on macOS.** Everything above except the Windows
  host lines was compiled, rendered and measured offline against the real
  plugin class in a headless GL context. On Windows, Arena drives it (above);
  still unverified about the *host* are a one-input call in any patching
  sequence but the one tried, and whether the layer's transition or autopilot
  moves `Opacity`.
- **KNOWN LIMITATION: Key Source cannot be reached in Resolume.** Arena 7.27.1
  does not expose it (2026-09-23, Windows), so the key there is always Colour 0;
  Luma and Alpha work in the harness and nowhere an operator can reach. Why is
  not known, and nothing was changed for it. See "The traps".
- **No frame of its output in Resolume has been looked at.** Arena's REST
  thumbnails do not serve a mixer's output, so the Arena run shows it was
  driven, not that it drew the right picture.
- **The universal build has never run on an Intel Mac**, and the render cost is
  macOS-only.
- **Hires and superhires are arithmetic, not observation.** The model — pixel
  clocks scale, time errors do not — is argued in `Controls.h` and measured
  against itself here. Nobody has put a hires Workbench through a real genlock
  and compared, and whether a composite-rate genlock's fringe in superhires is
  anything like a quarter of the lores one is exactly the kind of question the
  real bandwidth would answer differently.
- **The Crawl Wrap ceiling of 16 is a derivation from the burst's length**, not
  a measurement of how far any genlock lets the phase go before it drops lock.
- **Two rasterisers, not all.** See "Every number in the harness". The
  tolerances are derived from the GL spec rather than fitted; they held on this
  Mac's GPU and on the CI runner's software renderer, and llvmpipe has not run
  the checks.
- **Premultiplied alpha is assumed.** The key un-premultiplies before measuring
  a colour and the composite blends premultiplied, following
  `resolume-luma-keyer`. Whether Resolume hands a mixer premultiplied textures
  is inherited belief, not measurement.
- **The tear is a look, not a model.** Rows within 6% of the roll seam are
  thrown sideways by up to 24 Amiga pixels, scaled by how far the sync has
  fallen. Nothing measures it and no real hardware was consulted for the shape.
- **`Edge Tint` is a stylisation.** The fringe itself falls out of the key/fill
  disagreement and needs no help; the tint is an extra layer on top of it,
  because a real genlock's fringe also carries chroma crosstalk. The default is
  a cyan chosen by eye.
- **No OpenFX port.** Not required for 0.1.0. The browser demo came after the
  release; see "The browser demo" below.

---

## Reading the log after an Arena run

The plugin writes one file, `~/Library/Logs/genlock/genlock.YYYY-MM-DD.log`
(`%LOCALAPPDATA%\genlock\logs\` on Windows). Everything below was added so
that **one session in front of Arena** answers the open questions about mixers.
None of it changes a pixel. The first such session was 2026-09-23 on Arena
7.27.1 (Windows, win-lab); the right-hand column says what it measured.

Before the session: run nothing from the harness without `GENLOCK_LOG_DIR`
pointing elsewhere, or move the day's log aside — harness lines say `loaded from
.../gltest` and are noise here. Install with `cmake --install build`, start
Arena, set the upper layer's **Blend Mode** to SW Genlock (the same list is the
transition list), crossfade by hand and by autopilot, patch and unpatch the
layer below, quit. `Opacity` moves with the **layer's** Opacity fader, not the
mixer's own slider (see below).

| Line | What it answers | Measured 2026-09-23, Arena 7.27.1 on Windows |
|---|---|---|
| `plugin loaded build=…` then `loaded from <path>` | Written at **load time**, from a file-scope constructor, before any instance exists. Its presence says Arena scanned the folder and `dlopen`ed the bundle; the path says **which** folder (Extra Effects on Windows, per Arena's own log, 2026-09-23). No file at all means Arena never looked there. This line and nothing after it means it loaded the file and never instantiated it. | Both, at scan time, from `...\Resolume Arena\Extra Effects\Genlock.dll`. |
| `instance created` | A plugin object exists. Hosts often make one at scan time to read the parameters, so one of these alone is not "the operator chose it". | Present. |
| `host <name> version <v>` | `SetHostInfo`: makes the log evidence about a known Arena build, not about "Resolume". | `Resolume Arena version 7.27.1 15990`. |
| `SetSampleRate <n>` | Whether a host sends a mixer the audio rate. | Yes: `44100`. |
| `GL vendor=…` / `initialised, viewport WxH` | `InitGL` ran — the mixer was really put in a layer — and at what size. | Yes, at **1280×720**, the composition size. |
| `guard: called with 1 input(s) -- returned FF_FAIL (logged once)` | **Whether Resolume calls a mixer with one input while patching**, the SDK example's claim. Also `no input array`, `a null Dest`/`Src`, `a zero-sized …`. Each is logged the first time only. Its absence after patching and unpatching is also an answer. | **None.** Clearing and refilling the layer below gave no `guard:` line; Arena kept passing two inputs. An empty bottom layer from the start was not tried. |
| `first SetTime <t> (before frame n)` | That the host calls `SetTime` on a mixer at all, and whether before the first frame. | Yes, before frame 1. |
| `clock at frame 1 / frame 300 / DeInitGL: SetTime called|NEVER called (N calls, last t), unit seconds|milliseconds|undecided, votes s=… ms=…, elapsed …` | **Whether Resolume drives a mixer's clock**, and in which unit. `NEVER called` means the crawl and roll ran on the wall clock (right rate, wrong origin). `undecided` at frame 300 means the host's clock did not advance like a clock for four frames in five seconds — paused, or looping. | Called on every frame (412 of 412), **milliseconds** (about 4.1e6, votes ms 4–0). |
| `first frame: Dest WxH of HWxHH, Src …` | What sizes and paddings a mixer's two inputs really arrive at — the case the two MaxUVs exist for. | `Dest 1280x720 of 1280x768, Src 1280x720 of 1280x768`: both padded. |
| `first SetBeatInfo bpm … bar phase …`, and `… SetBeatInfo calls` at DeInitGL | Whether a mixer gets the transport. | Yes, every frame, 128 bpm. |
| `Opacity <v> at frame <n>` (the first 16 changes) | **Whether Resolume binds a parameter named `Opacity` to the transition.** If these lines appear while the layer's crossfader or autopilot moves and nobody touched the slider, it binds. If they appear only when the slider is dragged, it does not. | **Bound to the layer's Opacity**: `1 at frame 1`, `0.42 at frame 78`, `1 at frame 204` as the layer's opacity was set; the mixer's own slider (0.61, 0.13) never reached the plugin. Transition/autopilot not tested. |
| `DeInitGL after N frames, …` | The instance's life, and the clock's final state. | Not reported from this session. |

Where a line's answer changes a claim in this file, change the claim, date it,
and say it came from the log.

---

## Open questions

1. ~~Does Resolume drive the fader?~~ **Yes, answered 2026-09-23** (Arena
   7.27.1 on Windows, from the log): Resolume binds `Opacity` to the **layer's**
   Opacity fader and overrides the mixer's own slider. The operator does not
   lose a manual control — the layer's fader is it — so the master blend stays
   `Opacity`. Whether the layer's transition or autopilot also moves it was not
   tested.
2. ~~Is `Extra Mixers` the right folder?~~ **No, answered at release:**
   Resolume has no such folder; mixers load from Extra Effects. See above.
3. **Is a genlock usable as a transition?** Partly answered, 2026-09-23 (Arena
   7.27.1 on Windows): a mixer is chosen as a layer's **Blend Mode**, and that
   same list is the transition list, so SW Genlock is offered in both roles.
   `Opacity` is bound to the layer's opacity (question 1). A genlock is not a
   crossfade, and what a transition or autopilot does through it is still
   unknown — not tested, and no pixels have been captured from Arena.
4. ~~Should the crawl wrap be an operator control?~~ **Done, 2026-09-23:**
   `Crawl Wrap`, 1 to 16 pixels of the mode, default 1.
5. ~~Should `Key Delay` scale with a chosen Amiga mode?~~ **Done, 2026-09-23:**
   `Amiga Mode`, and it does. What is still open is whether the result looks
   like a real hires genlock at all — see "Assumed".
6. **NTSC.** Still PAL throughout. An NTSC Amiga's lores clock is 7.15909 MHz
   and its burst is nine cycles, so both the crawl rate and the Crawl Wrap
   ceiling would move. A second dropdown, not attempted.
7. **Why does Arena hide Key Source?** Measured 2026-09-23 (Arena 7.27.1,
   Windows): the mixer panel exposes 25 of 26 parameters and leaves out Key
   Source, id 0, so in Resolume the key is always Colour 0. Not known why; one
   guess is that Arena treats a mixer's first parameter specially, which moving
   another parameter to index 0 would test. Not attempted in v0.1.0.
8. **Does Resolume ever call a mixer with one input?** Not in the one sequence
   tried (clear and refill the layer below, 2026-09-23). An empty bottom layer
   from the start, and other patching orders, are untried.
9. **Does it render correctly in Arena?** Not known: no pixels were captured,
   because Arena's REST thumbnails do not serve a mixer's output. It was driven
   every frame without error lines; nobody has looked at the picture.

---

## Notes

Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes). The mixer section
above is the part of this file that belongs there.
