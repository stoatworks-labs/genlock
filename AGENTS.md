# genlock — orientation for another LLM (or a newcomer)

**What it is:** an FFGL 2.1 **mixer** for Resolume Arena/Avenue that keys this
layer's colour 0 over the layer below the way an Amiga genlock did — badly, on
purpose. C++17 + GLSL 4.10, CMake, universal macOS `.bundle` and a Windows
`.dll`. MIT. Intended home `github.com/stoatworks-labs/genlock`; **it is not
there yet** — v0.1.0 is local, unreleased and has never been in front of
Resolume.

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
  the fractional frequency error, and the line sync pulls it back a pixel at a
  time — so the fringe walks one Amiga pixel and snaps, over and over.
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

### What Resolume passes with one input — NOT known

The spec said to guard on `numInputTextures < 2` and on either pointer being
null, and this does. **What has actually been verified is only that the guards
work**: `gltest --mixer` calls `ProcessOpenGL` with zero inputs, one input, a
null Dest and a null Src, gets `FF_FAIL` from all four, no crash, and renders
normally afterwards. Whether Resolume really does call a mixer with one input
while the operator is patching is the SDK example's claim, not a measurement —
nothing here has been in front of Resolume.

### Things still unknown about mixers

- **Where Resolume reads them from.** The install prefix here is
  `~/Documents/Resolume Arena/Extra Mixers` rather than `Extra Effects`, on the
  assumption that Resolume separates them. Untested.
- **Whether Resolume binds a parameter by name.** `Add`'s comment says in as
  many words that "Resolume will look for a param named `Opacity` for mix
  value", and `ffglqs::Mixer` declares its own under the name `mixVal`. That is
  why this plugin's master blend is called **`Opacity`** and not `Mix`, which is
  what the spec called it — of the two names, one has evidence behind it. If
  Resolume does bind it, the host's transition control drives the fader, which
  is the right behaviour for a mixer. If it does not, it is an ordinary slider
  with an odd name. **Untested.**
- **Whether a mixer gets `SetTime` at all.** `FF_CAP_SET_TIME` reports 1 here,
  the same as for an effect, but whether Resolume drives a mixer's clock the
  way it drives an effect's is unknown, and this plugin's crawl and roll are
  both functions of time.

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
| `--mixer` guards | none | Four `FFResult` comparisons. No raster, no rasteriser. |
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
| `--roll` whole-row | **zero bytes** | A whole-row advance is a rotation, and a rotation of a sampled image is exact. The frames it applies to are found from the raster (`rate × H / fps`), not listed. |
| `--roll` position | **0.05 rows** | Bound under 0.01 rows: the bilinear reconstruction nulls the first alias (< 0.001), 8-bit weight error on ~4 partial rows moves a centroid by ε/2 ≈ 0.002 **independently of the raster**, and subtexel precision adds 1/256. 0.05 is five times that and twenty times smaller than a one-row error. Measured 0.0000 at both rasters. |
| `--roll` raster | **two rasters** | 640×360 advances a whole 6 rows per frame; 480×270 advances four and a half. This is the check that found the bug: the first version thresholded the band instead of weighting it, so it quantised to whole rows, passed at 360 and failed at 270 by exactly half a row. The *test* was wrong. |
| `--roll` zero-phase anchor | **0.05 rows** vs the card's own centre | Computed from the generator's rounding, not from `centre × height`: at 270 rows those differ by half a row, which is the size of the error being looked for. |
| `--fader` bitwise | **zero bytes** | Stated with its conditions. The arithmetic is `mix(video, video, 1.0)` = `video*(1-1) + video*1`: (1-1) is exactly 0, x*0 is 0, x*1 is x, 0+x is x, and an FMA contraction computes the same because neither factor is rounded. The *fetch* is exact only because the output raster equals Dest's and MaxUV is exactly 1, so every sample is a texel centre. Change either and it becomes a resample — which the check then demonstrates at 400×240 into 640×360 and **does not assert**, because a tolerance wide enough to cover a resampled test card's edges would be wide enough to cover a broken fader. |
| `--fader` at other Opacity | **1 code** | `video*(1-o) + video*o` re-rounds twice and is not an exact cancellation. One code is the readback's floor. Measured 0. |
| `--fader` dissolve halfway | **1 code** | Also the readback's floor. It was 8 at first, which was fitted to this picture. Measured 127 and 128. |
| `--bench` | not asserted | There is no threshold worth asserting on somebody else's GPU. |

**Negative controls actually run**, because a check that cannot fail is not a
check: wrong MaxUV (3 assertions fail), no half-texel inset (1 fails), roll rate
off by 0.1% (4 fail at each raster), one code added to the output (2 fail),
constant bias on the key fetch (the zero-delay anchor fails, and nothing else
does — which is why the anchor is there).

**What this pass does not prove.** Every tolerance is derived from the GL spec's
own guarantees rather than from a measurement — but *derived* is not *proven*,
and none of it has run on a rasteriser other than this Mac's. A llvmpipe run is
what would settle it, and CI has never run.

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

**Resolume's clock overflows a float.** It counts milliseconds from the session
start, and past about 4.99e8 a 32-bit float can no longer represent consecutive
milliseconds. Anything computed from an absolute host time in float stops
moving. `Timing.h` takes the first reading as an epoch, works in double, and
hands the shader a phase already reduced into its own period — under one Amiga
pixel for the crawl, under one picture height for the roll.

**The host's clock unit has to be worked out by observation.** The FFGL header
never says whether `SetTime` is seconds or milliseconds and hosts disagree;
Resolume sends milliseconds. `timing::Clock` votes on the ratio against a real
clock for four frames, exactly as tinsel does. The harness declares its unit
instead of letting the calibration infer one.

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

One pass, three fetches: the video at the fragment's position, the fill at the
overlay's rolled position, and the key at the overlay's rolled position
displaced by the delay. Everything is in picture space, 0..1 across the output,
with each input's `MaxUV` applied once at its own fetch.

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

**The crawl wraps at one Amiga pixel.** A free-running clock at 1 ppm slides
seven pixels a second, which as a picture would slide off the screen. A real
genlock re-locks on every horizontal sync, so only the sub-pixel remainder
survives — the fringe walks a pixel and snaps back. `kCrawlWrapAmigaPx` is that
remainder, and it is the reason the artefact reads as a crawl rather than as a
pan.

**`Clock Error` is geometric, 0.01 to 50 ppm.** The interesting range is all at
the bottom: at 0.01 ppm the fringe takes fourteen seconds to walk one Amiga
pixel, and above about 1 ppm the phase slews faster than seven pixels a second
and stops reading as a crawl at all. 50 ppm is an ordinary crystal's tolerance
and looks like a blur, which is the honest answer rather than a reason to
shorten the range.

**One Amiga pixel is 1/320 of the picture width at every raster.** An Amiga
lores line is 320 pixels across the active picture whatever the monitor is, so
the fringe is the same *fraction* of the frame at 720p and at 4K — which is the
only definition under which an operator's setting means the same thing twice.
PAL is assumed throughout (7.09379 MHz lores pixel clock); there is no NTSC
switch and no hires mode.

**The roll rate does not scale with how far the sync has fallen.** Below the
threshold the overlay rolls at exactly `Roll Rate`; only the *tearing* scales
with the severity. That is a testability choice as much as a physical one — a
rate that varied with the quality would have no closed form to check against.

**No presets.** Every other recent plugin in the fleet ships a preset table and
the machinery that keeps a host from un-setting it. Nineteen controls in four
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
- **The guards hold.** Zero inputs, one input, a null Dest and a null Src all
  return `FF_FAIL` without crashing, and two real inputs render afterwards.
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
- **No dead controls.** All **19** sweepable parameters measurably change the
  picture; the other four are the About buttons, which `tools/sweep.py` skips.
- **The build is universal and exports `plugMain`** — `lipo` reports
  `x86_64 arm64`, `nm -gU` finds `_plugMain`, the plist names a binary that
  exists, and it ad-hoc signs.
- **A host sees `SW Genlock` / `GL01` / mixer / inputs 2..2** through
  `oxbow probe`.
- **The render cost**, by `gltest --bench` (120 frames each, after a 20-frame
  warm-up, `glFinish` on both sides):

  | | ms/frame | % of a 60fps frame |
  | --- | --- | --- |
  | 1280×720 | 0.018 | 0.1% |
  | 1920×1080 | 0.032 | 0.2% |
  | 2560×1440 | 0.049 | 0.3% |
  | 3840×2160 | 0.108 | 0.6% |

  One pass and three texture fetches is about as cheap as an FFGL plugin gets.

**Assumed, or not yet done:**

- **Never loaded into Resolume.** Not once. Everything above was compiled,
  rendered and measured offline against the real plugin class in a headless CGL
  context. Every mixer-specific claim about the *host* — that Resolume reads
  mixers from Extra Mixers, that it binds `Opacity`, that it calls a mixer with
  one input while patching, that it drives a mixer's `SetTime` — is unverified.
- **Never run on another rasteriser.** See "Every number in the harness". The
  tolerances are derived from the GL spec rather than fitted, and the whole
  point of that was to survive llvmpipe, but nothing has proved it.
- **Windows has never been compiled.** CI exists and has never run; there is no
  remote.
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
- **No OpenFX port and no browser demo.** Neither is required for 0.1.0.
- **No user guide**, which is why the About block deliberately carries no guide
  link.

---

## Open questions

1. **Does Resolume drive the fader?** If it binds `Opacity` to the layer's
   transition position, the operator loses it as a manual control — which may be
   right for a mixer and may be surprising. One session in front of Arena
   answers it, and the answer might move the master blend back to `Mix` with a
   separate `Opacity` beside it.
2. **Is `Extra Mixers` the right folder?** If Resolume has no such concept the
   install target is wrong and the plugin will not appear at all.
3. **Is a genlock usable as a transition?** Resolume's mixers are chosen per
   layer as transitions. A genlock is not a crossfade, and what an autopilot
   sweeping `Opacity` through it looks like is unknown.
4. **Should the crawl wrap be an operator control?** It is fixed at one Amiga
   pixel, which is the physical answer for a genlock that re-locks every line.
   A genlock with a worse line sync would let more accumulate, and that is a
   real difference in look with no slider.
5. **Should `Key Delay` scale with a chosen Amiga mode?** Lores is assumed. A
   hires overlay has half-width pixels, so the same nominal delay is half the
   fringe. One dropdown would cover it.

---

## Notes

Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes). The mixer section
above is the part of this file that belongs there.
