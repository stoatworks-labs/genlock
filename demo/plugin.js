/**
 * Genlock — browser demo.
 *
 * An Amiga genlock as an FFGL MIXER. The one idea, from `AGENTS.md`: **a
 * genlock's key comes from the wrong clock.** The key is cut from the
 * computer's picture on the computer's own pixel clock, which free-runs a few
 * parts per million away from the video's, so the key edge lands a pixel or so
 * from the fill it belongs to — and because the clocks differ, that error walks.
 *
 * What is the plugin's here, and what is not:
 *
 *   The shader is the plugin's. `VERTEX_SHADER` and `GENLOCK_SHADER` below are
 *   `kVertexShader` and `kGenlockShader` from `source/Shaders.cpp`, copied
 *   across unedited. `demo/tools/check_shaders.py` compares them to the C++
 *   character for character and `tools/verify.sh` runs it, because two copies
 *   of a shader is exactly the arrangement that drifts. There is one pass, as
 *   in the plugin.
 *
 *   The CPU half is a port, and a small one: `Controls.cpp`'s conversions, the
 *   crawl and roll phases from `Timing.cpp`, and the uniform arithmetic of
 *   `Genlock::ProcessOpenGL` — the mode's width, the crawl added to the key
 *   delay BEFORE the division, the tear's throw in lores pixels in every mode,
 *   the lock threshold as a branch. Nothing checks that port but a reader.
 *
 * ---------------------------------------------------------------------------
 * Decisions this page made, and why
 * ---------------------------------------------------------------------------
 *
 * **A mixer needs two inputs, and the kit makes one.** Dest — `inputTextures[0]`,
 * the layer BELOW, the incoming video — is the kit's clip, so the Clip dropdown
 * and "Use my own…" both drive it, and they are relabelled to say so. Src —
 * `inputTextures[1]`, THIS layer, the computer's picture — is the second clip,
 * and it is not one of the kit's: it is one of the plugin harness's own cards,
 * `amigaCard`, `barCard` and `edgeCard` from `tools/gltest/main.cpp`, ported
 * pixel for pixel and uploaded as a texture. Those are the pictures the plugin
 * was measured on, and all three are drawn on Workbench blue, #0055AA, which is
 * the Key Colour's default — so the key has something to find. The choice of
 * card is the kit's `demo.variants` dropdown, because "which picture is on the
 * other layer" is not a parameter the plugin declares.
 *
 * **Both inputs are the composition's size, and neither is padded.** In Arena
 * both arrived as 1280x720 of 1280x768 (measured, `AGENTS.md`), so MaxUV.y was
 * not 1; a browser texture is not padded, so here both MaxUVs are exactly 1 and
 * the per-input MaxUV handling the plugin exists to get right is not exercised.
 * The half-texel insets are each input's own.
 *
 * **Key Source is on the panel, although Arena hides it.** Arena 7.27.1 exposes
 * 25 of the 26 parameters and the missing one is Key Source, the first; in
 * Resolume the key is therefore always Colour 0. The plugin declares it, so it
 * is here, and its hint and the disclosure say that Luma and Alpha cannot be
 * reached in Resolume.
 *
 * **Opacity is a slider here and the layer's fader there.** Arena binds a
 * mixer parameter named `Opacity` to the layer's opacity fader and ignores
 * writes to the mixer's own. A web page has no layer, so the plugin's
 * declaration is shown as it is.
 *
 * **The clock is the page's.** The plugin works out whether the host speaks
 * seconds or milliseconds by voting against a wall clock and then runs from a
 * frame-relative epoch. The page's clock is already seconds from zero, so the
 * vote and the epoch are skipped and `elapsed` is the page time. Restart is a
 * new epoch, which is what re-triggering the clip is in the plugin.
 *
 * **Presets are this page's, not the plugin's.** Genlock ships none. The kit's
 * Presets dropdown writes ordinary values into the panel as shortcuts to the
 * states worth seeing, and the disclosure says so.
 *
 * **The About block is absent.** A text line and link buttons exist so a host
 * has somewhere to put them; this page has links of its own.
 */

import { mountDemo } from './vendor/demo.js';
import { Program } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here.
//---------------------------------------------------------------------------

const VERTEX_SHADER = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
`;

const GENLOCK_SHADER = `#version 410 core

//inputTextures[0] -- Dest, the layer BELOW: the incoming video.
uniform sampler2D TextureDest;
//inputTextures[1] -- Src, THIS layer: the computer's picture, fill and key.
uniform sampler2D TextureSrc;

//Each input has its own. They are not the same number and there is no
//circumstance in which using one for the other is safe.
uniform vec2 MaxUVDest;
uniform vec2 MaxUVSrc;
uniform vec2 HalfTexelDest;
uniform vec2 HalfTexelSrc;

//Key.
uniform float KeySource;//0 colour 0, 1 luma, 2 alpha
uniform vec3 KeyColour;
uniform float Tolerance;
uniform float Softness;
uniform float Invert;

//Timing. All of these are picture-space fractions, already reduced on the
//CPU in double -- see Timing.h. The shader never sees a clock.
uniform float KeyDelay; //signed, fraction of picture width
uniform float RollPhase;//0..1 picture heights; exactly 0 while locked
uniform float TearThrow;//fraction of picture width; exactly 0 while locked
uniform float TearBand; //fraction of picture height either side of the seam

//Fader.
uniform float FaderMode;//0 video, 1 overlay, 2 dissolve
uniform float Dissolve;

//Look.
uniform float Fringe;
uniform vec3 EdgeTint;
uniform float Opacity;

in vec2 uv;
out vec4 fragColor;

//The video. Clamped half a texel inside the used area: GL_LINEAR at the
//boundary takes half its weight from the texture's undrawn padding, and on
//a host that hands over a padded texture that padding is whatever was in
//that memory before.
vec4 fetchDest( vec2 p )
{
	vec2 q = clamp( p, HalfTexelDest, vec2( 1.0 ) - HalfTexelDest );
	return texture( TextureDest, q * MaxUVDest );
}

vec4 fetchSrc( vec2 p )
{
	vec2 q = clamp( p, HalfTexelSrc, vec2( 1.0 ) - HalfTexelSrc );
	return texture( TextureSrc, q * MaxUVSrc );
}

//Distance from the seam the roll wraps at, measured the short way round.
float seamDistance( float v )
{
	float seam = fract( -RollPhase );
	return abs( fract( v - seam + 0.5 ) - 0.5 );
}

//Where the computer's picture is this frame. The video does not move; the
//overlay is the thing that has lost lock.
//
//While locked this is the identity: RollPhase and TearThrow are exactly
//zero, fract( p.y ) returns p.y for anything inside the picture, and
//p.x + 0.0 * lean is p.x. Nothing downstream has to special-case it.
vec2 overlayCoord( vec2 p )
{
	float lean = TearBand > 0.0 ? max( 0.0, 1.0 - seamDistance( p.y ) / TearBand ) : 0.0;
	return vec2( p.x + TearThrow * lean, fract( p.y + RollPhase ) );
}

//The key: how much of this pixel is the palette's colour 0, i.e. how much
//of it should be VIDEO. 1 is video, 0 is the computer's own picture.
float keyOf( vec4 s )
{
	//Un-premultiply before measuring a colour: a soft alpha edge is not a
	//darker colour, and keying it as one would put the fringe in a different
	//place on an antialiased overlay than on a hard-edged one.
	vec3 straight = s.a > 0.0031 ? s.rgb / s.a : vec3( 0.0 );

	float away;
	if( KeySource < 0.5 )
		away = length( straight - KeyColour ) * 0.5773502691896258;//1/sqrt(3): the cube's diagonal
	else if( KeySource < 1.5 )
		away = dot( straight, vec3( 0.2126, 0.7152, 0.0722 ) );//Rec. 709 luma
	else
		away = s.a;

	//smoothstep is undefined when its two edges are equal, and Softness
	//reaches zero, so the upper edge is held a hair above the lower one. At
	//that setting the key is a step and the edge lands wherever the sampled
	//colour crosses the threshold, which is inside one texel.
	float k = 1.0 - smoothstep( Tolerance, Tolerance + max( Softness, 1.0e-5 ), away );

	return mix( k, 1.0 - k, Invert );
}

void main()
{
	vec4 videoPix = fetchDest( uv );

	vec2 here    = overlayCoord( uv );
	vec4 fillPix = fetchSrc( here );

	//THE WHOLE PLUGIN IS THIS LINE.
	//
	//The key is cut from the computer's picture at a DIFFERENT place from
	//the fill it belongs to, because it arrives on the computer's own pixel
	//clock and that clock is not locked to the incoming video. KeyDelay is
	//where the operator put it; the crawl is already added into it on the
	//CPU, so the displacement walks as the two clocks drift apart.
	vec4 keyPix = fetchSrc( overlayCoord( uv - vec2( KeyDelay, 0.0 ) ) );

	float k       = keyOf( keyPix );
	float aligned = keyOf( fillPix );

	vec4 keyed = mix( fillPix, videoPix, k );

	//Where the delayed key and the fill disagree is the fringe. It is
	//already visible without this -- the disagreement IS the artefact, and
	//it shows as a band of the palette's colour 0 laid over the video, or
	//as video cut into the fill, depending on the sign. What this adds is
	//the chroma crosstalk that comes with it on real hardware.
	float disagree = abs( k - aligned );
	keyed.rgb      = mix( keyed.rgb, EdgeTint * keyed.a, Fringe * disagree );

	vec4 result = videoPix;
	if( FaderMode > 1.5 )
		result = mix( videoPix, keyed, Dissolve );
	else if( FaderMode > 0.5 )
		result = keyed;

	fragColor = mix( videoPix, result, Opacity );
}
`;

//---------------------------------------------------------------------------
// Controls.h / Controls.cpp, ported. Every host parameter is 0..1; these are
// what they mean. Math.fround where the C++ is float arithmetic, so the value
// the uniform receives is the value the plugin would hand it.
//---------------------------------------------------------------------------
const f = Math.fround;
const clamp01 = (v) => Math.min(1, Math.max(0, v));

const AM_LORES = 0;
const AM_COUNT = 3;
const K_AMIGA_LORES_WIDTH = 320.0;
const K_AMIGA_LORES_PIXEL_CLOCK_HZ = 7093790.0;
const K_CRAWL_WRAP_MIN_AMIGA_PX = 1.0;
const K_CRAWL_WRAP_MAX_AMIGA_PX = 16.0;
const K_LOCK_THRESHOLD = f(0.5);
const K_TEAR_BAND = f(0.06);
const K_TEAR_AMIGA_PX = f(24.0);
const K_CLOCK_ERROR_MIN_PPM = 0.01;
const K_CLOCK_ERROR_MAX_PPM = 50.0;
const K_MODE_SCALE = [1.0, 2.0, 4.0];

const modeScale = (mode) => K_MODE_SCALE[Math.min(AM_COUNT - 1, Math.max(0, mode))];
const amigaWidthForMode = (mode) => K_AMIGA_LORES_WIDTH * modeScale(mode);
const amigaPixelClockHzForMode = (mode) => K_AMIGA_LORES_PIXEL_CLOCK_HZ * modeScale(mode);

const keyDelayFromParam = (v) => f(f(clamp01(v) - 0.5) * 16.0);
const toleranceFromParam = (v) => f(clamp01(v));
const softnessFromParam = (v) => f(clamp01(v) * 0.5);
const clockErrorPpmFromParam = (v) =>
  f(K_CLOCK_ERROR_MIN_PPM * Math.pow(K_CLOCK_ERROR_MAX_PPM / K_CLOCK_ERROR_MIN_PPM, f(clamp01(v))));
const crawlRateFromParam = (v) => f(clamp01(v) * 4.0);
const crawlWrapAmigaPxFromParam = (v) =>
  f(K_CRAWL_WRAP_MIN_AMIGA_PX + f(clamp01(v)) * (K_CRAWL_WRAP_MAX_AMIGA_PX - K_CRAWL_WRAP_MIN_AMIGA_PX));
const crawlWrapParamFor = (px) =>
  clamp01(f((px - K_CRAWL_WRAP_MIN_AMIGA_PX) / (K_CRAWL_WRAP_MAX_AMIGA_PX - K_CRAWL_WRAP_MIN_AMIGA_PX)));
const rollRateFromParam = (v) => f(clamp01(v) * 4.0);
const crawlRateAmigaPxPerSecond = (clockErrorParam, crawlRateParam, mode) =>
  amigaPixelClockHzForMode(mode) * clockErrorPpmFromParam(clockErrorParam) * 1e-6 * crawlRateFromParam(crawlRateParam);

// Timing.cpp. Reduced in double, from a frame-relative time.
function positiveMod(value, period) {
  if (!(period > 0)) return 0;
  const r = value % period; // fmod: keeps the numerator's sign
  return r < 0 ? r + period : r;
}
const crawlPhase = (elapsed, rate, wrap) => positiveMod(rate * elapsed, wrap);
const rollPhase = (elapsed, rollsPerSecond) => positiveMod(rollsPerSecond * elapsed, 1.0);

const optionIndex = (value, count) => Math.min(count - 1, Math.max(0, Math.round(value)));

//---------------------------------------------------------------------------
// The second input: the harness's own cards, from tools/gltest/main.cpp.
//
// Row 0 of each is the BOTTOM of the picture, because gltest hands them to
// glTexImage2D as they are and so does this. Colours, fractions and rounding
// are the harness's — std::lround is Math.round for the positive values here.
//---------------------------------------------------------------------------
const COLOUR_0 = [0x00, 0x55, 0xaa]; // Workbench blue, the Key Colour default
const FILL = [0xff, 0xff, 0xff];
const EDGE_RAMP_AMIGA_PX = 2.0;

function flatField(width, height, c) {
  const image = new Uint8Array(width * height * 4);
  for (let i = 0; i < width * height; i += 1) {
    image[i * 4] = c[0]; image[i * 4 + 1] = c[1]; image[i * 4 + 2] = c[2]; image[i * 4 + 3] = 255;
  }
  return image;
}

function put(image, width, x, y, c) {
  const at = (y * width + x) * 4;
  image[at] = c[0]; image[at + 1] = c[1]; image[at + 2] = c[2]; image[at + 3] = 255;
}

const lerpByte = (a, b, t) => Math.round(a + t * (b - a));

/** `amigaCard`: colour 0, a window with a hard-edged title bar and blocks, and one soft gradient. */
function amigaCard(width, height) {
  const image = flatField(width, height, COLOUR_0);
  const px = (fr, span) => Math.round(fr * span);

  for (let y = px(0.18, height); y < px(0.78, height); y += 1)
    for (let x = px(0.12, width); x < px(0.62, width); x += 1) put(image, width, x, y, [0xe8, 0xe8, 0xe8]);
  for (let y = px(0.70, height); y < px(0.78, height); y += 1)
    for (let x = px(0.12, width); x < px(0.62, width); x += 1) put(image, width, x, y, [0x00, 0x00, 0x20]);

  const blocks = [[0xff, 0x88, 0x00], [0xff, 0xff, 0xff], [0x00, 0x00, 0x20], [0xaa, 0x00, 0x00]];
  for (let block = 0; block < 4; block += 1)
    for (let y = px(0.26, height); y < px(0.46, height); y += 1)
      for (let x = px(0.16 + 0.11 * block, width); x < px(0.24 + 0.11 * block, width); x += 1)
        put(image, width, x, y, blocks[block]);

  const x0 = px(0.68, width);
  const x1 = px(0.94, width);
  for (let y = px(0.18, height); y < px(0.78, height); y += 1)
    for (let x = x0; x < x1; x += 1) {
      const t = (x - x0) / (x1 - x0);
      put(image, width, x, y, COLOUR_0.map((c) => lerpByte(c, 255, t)));
    }
  return image;
}

/** `barCard( w, h, 0.5, 0.1 )`: a full-width bar of fill on colour 0, hard-edged — the roll's card. */
function barCard(width, height) {
  const image = flatField(width, height, COLOUR_0);
  const lo = Math.round((0.5 - 0.1 * 0.5) * height);
  const hi = Math.round((0.5 + 0.1 * 0.5) * height);
  for (let y = Math.max(0, lo); y < Math.min(height, hi); y += 1)
    for (let x = 0; x < width; x += 1) put(image, width, x, y, FILL);
  return image;
}

/** `edgeCard( w, h, 0.5 )`: colour 0 left, fill right, ramped over two Amiga pixels — the delay's card. */
function edgeCard(width, height) {
  const image = new Uint8Array(width * height * 4);
  const edgePx = 0.5 * width;
  const ramp = EDGE_RAMP_AMIGA_PX * (width / K_AMIGA_LORES_WIDTH);
  for (let x = 0; x < width; x += 1) {
    const t = Math.min(1, Math.max(0, ((x + 0.5) - (edgePx - ramp * 0.5)) / ramp));
    const c = [0, 1, 2].map((i) => lerpByte(COLOUR_0[i], FILL[i], t));
    for (let y = 0; y < height; y += 1) put(image, width, x, y, c);
  }
  return image;
}

const CARDS = { amiga: amigaCard, bar: barCard, edge: edgeCard };

/** The Src texture, rebuilt only when the card or the raster changes. */
class CardTexture {
  constructor(gl) {
    this.gl = gl;
    this.texture = gl.createTexture();
    this.key = '';
  }

  ensure(card, width, height) {
    const key = `${card}:${width}x${height}`;
    if (key === this.key) return this.texture;
    const gl = this.gl;
    const pixels = (CARDS[card] ?? amigaCard)(width, height);
    gl.bindTexture(gl.TEXTURE_2D, this.texture);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.bindTexture(gl.TEXTURE_2D, null);
    this.key = key;
    return this.texture;
  }
}

//---------------------------------------------------------------------------
// The frame: Genlock::ProcessOpenGL's uniform arithmetic, ported.
//---------------------------------------------------------------------------
let lastTimings = null;

function createRenderer(gl, quad) {
  const program = new Program(gl, VERTEX_SHADER, GENLOCK_SHADER, 'genlock');
  const card = new CardTexture(gl);

  return {
    render({ input, params, width, height, time, variant }) {
      const p = (id) => params.get(id);

      // Dest is the kit's clip at its own size; Src is the card at the
      // composition's. They differ whenever a visitor's own file is loaded.
      const srcW = width;
      const srcH = height;
      const srcTexture = card.ensure(variant ?? 'amiga', srcW, srcH);

      // The page's clock is already seconds from a zero it restarts at, which
      // is what Clock::Tick produces once the unit is settled.
      const elapsed = time;

      const mode = optionIndex(p('amigaMode'), AM_COUNT);
      const modeWidth = amigaWidthForMode(mode);
      const crawlRate = crawlRateAmigaPxPerSecond(p('clockError'), p('crawlRate'), mode);
      const crawlWrap = crawlWrapAmigaPxFromParam(p('crawlWrap'));
      const crawl = crawlPhase(elapsed, crawlRate, crawlWrap);

      // The sum is taken BEFORE the division by the mode's width, as the plugin does.
      const keyDelayPx = keyDelayFromParam(p('keyDelay'));
      const delayAmiga = keyDelayPx + crawl;
      const keyDelayPicture = delayAmiga / modeWidth;

      const quality = f(Math.min(1, Math.max(0, p('syncQuality'))));
      const locked = quality >= K_LOCK_THRESHOLD;
      const roll = locked ? 0.0 : rollPhase(elapsed, rollRateFromParam(p('rollRate')));
      const lockLoss = locked ? 0 : f(f(K_LOCK_THRESHOLD - quality) / K_LOCK_THRESHOLD);

      lastTimings = { crawl, crawlWrap, keyDelayPx, delayAmiga, roll, mode, locked, crawlRate };

      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);
      gl.disable(gl.BLEND);

      program.use();
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, input.texture);
      gl.activeTexture(gl.TEXTURE1);
      gl.bindTexture(gl.TEXTURE_2D, srcTexture);
      program.setSampler('TextureDest', 0);
      program.setSampler('TextureSrc', 1);

      // A browser texture is never padded, so both MaxUVs are exactly 1. In
      // Arena they were not (1280x720 of 1280x768).
      program.set('MaxUVDest', 1, 1);
      program.set('MaxUVSrc', 1, 1);
      program.set('HalfTexelDest', f(0.5 / input.width), f(0.5 / input.height));
      program.set('HalfTexelSrc', f(0.5 / srcW), f(0.5 / srcH));

      program.set('KeySource', optionIndex(p('keySource'), 3));
      program.set('KeyColour', p('keyR'), p('keyG'), p('keyB'));
      program.set('Tolerance', toleranceFromParam(p('tolerance')));
      program.set('Softness', softnessFromParam(p('softness')));
      program.set('Invert', p('invert') > 0.5 ? 1 : 0);

      // The tear is a TIME error, so it is thrown in lores pixels in every mode.
      program.set('KeyDelay', f(keyDelayPicture));
      program.set('RollPhase', f(roll));
      program.set('TearThrow', f(f(lockLoss * K_TEAR_AMIGA_PX) / K_AMIGA_LORES_WIDTH));
      program.set('TearBand', K_TEAR_BAND);

      program.set('FaderMode', optionIndex(p('fader'), 3));
      program.set('Dissolve', clamp01(p('dissolve')));

      program.set('Fringe', clamp01(p('fringe')));
      program.set('EdgeTint', p('tintR'), p('tintG'), p('tintB'));
      program.set('Opacity', clamp01(p('opacity')));

      quad.draw();

      gl.activeTexture(gl.TEXTURE1);
      gl.bindTexture(gl.TEXTURE_2D, null);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, null);
    },
  };
}

//---------------------------------------------------------------------------
// The parameters, in Genlock::Genlock's order, groups, names and defaults.
//---------------------------------------------------------------------------
const pct = (v) => `${Math.round(clamp01(v) * 100)}%`;
const signedPx = (x) => `${x > 0 ? '+' : x < 0 ? '−' : ''}${Math.abs(x).toFixed(2)} px`;

const PARAMS = [
  { id: 'keySource', name: 'Key Source', type: 'option', elements: ['Colour 0', 'Luma', 'Alpha'], default: 0, group: 'Key',
    hint: 'What the key is cut from. In Resolume this control is not reachable: Arena 7.27.1 exposes 25 of the plugin’s 26 parameters and hides this one, the first, so there the key is always Colour 0. It is here because the plugin declares it.' },
  { id: 'keyR', name: 'Key Colour', type: 'colour', default: 0.0, group: 'Key',
    hint: 'Key Colour Red, Green and Blue in the plugin, shown as one swatch as a host does. Workbench blue, #0055AA: the colour 0 a generation of Amigas booted to.' },
  { id: 'keyG', name: 'Key Colour Green', type: 'colour', default: 0.333, group: 'Key' },
  { id: 'keyB', name: 'Key Colour Blue', type: 'colour', default: 0.667, group: 'Key' },
  { id: 'tolerance', name: 'Tolerance', type: 'standard', default: 0.12, group: 'Key',
    display: (v) => toleranceFromParam(v).toFixed(3),
    hint: 'How far a colour may be from the key colour and still be keyed, as a fraction of the unit RGB cube’s diagonal.' },
  { id: 'softness', name: 'Softness', type: 'standard', default: 0.06, group: 'Key',
    display: (v) => softnessFromParam(v).toFixed(3),
    hint: '0 to 0.5 in the same units: the width of the key’s soft edge.' },
  { id: 'invert', name: 'Invert', type: 'boolean', default: 0, group: 'Key' },

  { id: 'amigaMode', name: 'Amiga Mode', type: 'option', elements: ['Lores', 'Hires', 'Superhires'], default: 0, group: 'Timing',
    hint: 'Sets the pixel. Key Delay and Crawl Wrap count the computer’s own pixel clocks, so they are half the distance in hires and a quarter in superhires. The crawl’s speed across the picture and the tear’s throw are time errors and do not change.' },
  { id: 'keyDelay', name: 'Key Delay', type: 'standard', default: 0.4375, group: 'Timing',
    display: (v) => signedPx(keyDelayFromParam(v)),
    hint: '−8 to +8 Amiga pixels of the current mode: where the key lands relative to its fill. Negative lays colour 0 over the video along one side of every edge; positive cuts video into the graphic. At exactly zero the key is fetched where the fill is, and there is no fringe.' },
  { id: 'clockError', name: 'Clock Error', type: 'standard', default: 0.30, group: 'Timing',
    display: (v) => `${clockErrorPpmFromParam(v).toPrecision(3)} ppm`,
    hint: '0.01 to 50 ppm, geometric: the frequency difference between the Amiga’s pixel clock and the video’s. It is what makes the fringe crawl, at the pixel clock times the error.' },
  { id: 'crawlRate', name: 'Crawl Rate', type: 'standard', default: 0.25, group: 'Timing',
    display: (v) => `×${crawlRateFromParam(v).toFixed(2)}`,
    hint: '0 to 4, unity at a quarter: a multiplier on the crawl. Zero freezes it without claiming the clocks are locked.' },
  { id: 'crawlWrap', name: 'Crawl Wrap', type: 'standard', default: 0.0, group: 'Timing',
    display: (v) => `${crawlWrapAmigaPxFromParam(v).toFixed(2)} px`,
    hint: '1 to 16 Amiga pixels, linear: how far the fringe walks before the line sync pulls it back. One pixel is a clean re-lock every line; sixteen is the length of the PAL colour burst, past which there is nothing left to lock to.' },
  { id: 'syncQuality', name: 'Sync Quality', type: 'standard', default: 1.0, group: 'Timing',
    display: (v) => (f(clamp01(v)) >= K_LOCK_THRESHOLD ? `${pct(v)} locked` : `${pct(v)} no lock`),
    hint: 'Below a half the overlay loses vertical lock and rolls, tearing at the seam — a branch, not a fade, because that is what losing lock is. The further below, the harder the tear.' },
  { id: 'rollRate', name: 'Roll Rate', type: 'standard', default: 0.125, group: 'Timing',
    display: (v) => `${rollRateFromParam(v).toFixed(2)} /s`,
    hint: '0 to 4 rolls a second, once lock is lost. Does nothing while Sync Quality is at or above a half.' },

  { id: 'fader', name: 'Fader', type: 'option', elements: ['Video', 'Overlay', 'Dissolve'], default: 1, group: 'Fader',
    hint: 'The hardware’s three-position switch. Video is the layer below untouched; Overlay is the keyed picture; Dissolve mixes the two with the pot below.' },
  { id: 'dissolve', name: 'Dissolve', type: 'standard', default: 0.5, group: 'Fader', display: pct,
    hint: 'The dissolve pot. Only does anything with the Fader on Dissolve.' },

  { id: 'fringe', name: 'Fringe', type: 'standard', default: 0.35, group: 'Look', display: pct,
    hint: 'Chroma crosstalk where the key and the fill disagree. The fringe is visible without it — the disagreement IS the artefact — and this tints it.' },
  { id: 'tintR', name: 'Edge Tint', type: 'colour', default: 0.25, group: 'Look',
    hint: 'Edge Tint Red, Green and Blue in the plugin, shown as one swatch.' },
  { id: 'tintG', name: 'Edge Tint Green', type: 'colour', default: 0.95, group: 'Look' },
  { id: 'tintB', name: 'Edge Tint Blue', type: 'colour', default: 1.0, group: 'Look' },
  { id: 'opacity', name: 'Opacity', type: 'standard', default: 1.0, group: 'Look', display: pct,
    hint: 'The master blend against the layer below. In Resolume this is the LAYER’s opacity fader, not a slider in the mixer’s panel: Arena binds a mixer parameter named Opacity to it and ignores writes to the mixer’s own.' },
];

const mounted = mountDemo({
  name: 'Genlock',
  pluginId: 'GL01',
  tagline:
    'An Amiga genlock, flaws and all: this layer’s colour 0 keyed over the layer below — but the key is cut on the computer’s own pixel clock, which is not locked to the video. So it lands a pixel away from its fill, every edge carries a coloured fringe, and as the two clocks drift the fringe crawls. Drop Sync Quality below a half and the overlay loses vertical lock and rolls.',
  repo: 'https://github.com/stoatworks-labs/genlock',
  page: 'https://stoatworks-labs.com/software/genlock/',
  video: 'https://www.youtube.com/watch?v=g_ieK_B1WiU',

  // A mixer reads TWO clips, and only one of them is the kit's.
  blurb:
    'It is Genlock’s own GLSL, ported from the repository to WebGL2 and mixing two pictures generated in this page — a clip for the layer below, and one of the plugin harness’s own test cards for this layer — with the same parameters and the same maths, no install.',

  params: PARAMS,

  // Dest, the layer below. A genlock's video wants something moving with a
  // full range of colour for the key to reveal.
  sources: ['scene', 'bars', 'grid', 'ramp', 'detail', 'spot'],

  // Src, this layer: which of gltest's cards is the computer's picture.
  variants: {
    label: 'This layer (Src)',
    default: 'amiga',
    options: [
      { id: 'amiga', name: 'Workbench window', hint: 'gltest’s amiga card: a window with a hard title bar and four blocks on colour 0, and one soft gradient from colour 0 to white for Softness to bite on.' },
      { id: 'bar', name: 'Bar on colour 0', hint: 'gltest’s bar card: one hard-edged white bar across colour 0 — the card the roll is measured on. Try it with Sync Quality below a half.' },
      { id: 'edge', name: 'One edge', hint: 'gltest’s edge card: colour 0 on the left, white on the right, ramped over two Amiga pixels — the card the key delay is measured on.' },
    ],
  },

  presets: {
    'Colour-0 fringe (defaults)': {},
    'Video cut into the graphic': { keyDelay: 0.5625 },
    'Three-pixel fringe, tinted': { keyDelay: 0.3125, fringe: 0.8 },
    'Crawling to the burst': { crawlWrap: crawlWrapParamFor(8), clockError: 0.45, keyDelay: 0.375 },
    'Hires, same delay': { amigaMode: 1 },
    'Lost vertical lock': { syncQuality: 0.3, rollRate: 0.125 },
    'Losing it badly': { syncQuality: 0.05, rollRate: 0.3, clockError: 0.6, crawlWrap: 1 },
    'Dissolve': { fader: 2, dissolve: 0.5 },
  },

  differences: [
    'A mixer needs two inputs and this page makes both. The layer below (Dest, the video) is the kit’s generated clip, or your own file. This layer (Src, the computer’s picture) is one of the three test cards the plugin’s own harness, gltest, measures it on — ported from tools/gltest/main.cpp pixel for pixel and drawn on Workbench blue so the default key finds it. In Resolume both would be whatever is on the two layers.',
    'Both inputs arrive unpadded, so both MaxUVs are exactly 1 and the per-input MaxUV handling is not exercised. In Arena 7.27.1 both arrived as 1280×720 of 1280×768. The half-texel insets are each input’s own, as in the plugin.',
    'Key Source is on this panel, and it is not in Resolume’s. Arena exposes 25 of the plugin’s 26 parameters and the hidden one is Key Source, the first — so in Resolume the key is always Colour 0 and Luma and Alpha cannot be reached. Why Arena hides a mixer’s first parameter is not known.',
    'Opacity is a slider here. In Resolume it is the layer’s opacity fader: Arena binds a mixer parameter named Opacity to it, and writes to the mixer’s own Opacity never reach the plugin.',
    'The CPU half is a port to JavaScript that nothing checks but a reader: the control conversions from Controls.cpp, the crawl and roll phases from Timing.cpp, and the uniform arithmetic of ProcessOpenGL. The shader is the plugin’s, and demo/tools/check_shaders.py fails the repository’s verify script if a character of it drifts from source/Shaders.cpp.',
    'The clock is the page’s. The plugin works out whether its host speaks seconds or milliseconds by voting against a wall clock, then runs from its own epoch; the page’s clock is already seconds from zero, so neither is needed. Restart is a new epoch, as re-triggering the clip is in the plugin.',
    'The banner’s closing sentence is the shared kit’s, identical on every demo page, and calls the plugin an effect. Genlock is an FFGL mixer: in Resolume it is chosen as a layer’s Blend Mode, not added as an effect.',
    'The presets are this page’s shortcuts, not the plugin’s: Genlock ships no presets. Each one only sets ordinary parameter values. The About block, which exists so a host has somewhere to put links, is absent.',
    'Nothing here is measured. The whole-pixel key delay translating the key exactly, the fractional delay recovered from partial coverage, the crawl against its closed form, the roll at two rasters and the crawl’s speed bit-identical across the three modes are tools/gltest in the repository, and that harness — not this page — is the reason to believe the timing model. The plugin has been loaded in Arena on Windows on software rendering and no frame of its output has ever been captured there.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The two transport controls, relabelled to say which input they drive. The
// kit calls its one clip "Clip"; a mixer has two, and an unlabelled one is a
// claim about which layer it is that the page would be leaving to a guess.
//---------------------------------------------------------------------------
for (const field of document.querySelectorAll('.transport__field')) {
  const label = field.querySelector('.transport__label');
  if (label?.textContent === 'Clip') {
    label.textContent = 'Layer below (Dest)';
    field.title = 'inputTextures[0]: the incoming video. "Use my own…" replaces this one.';
  }
}
const own = document.querySelector('.transport__file .btn');
if (own) own.textContent = 'Own video below…';

//---------------------------------------------------------------------------
// One line of the plugin's own timing state under the picture, the same
// numbers its Diag log carries: where the key is this frame and why.
//---------------------------------------------------------------------------
const stage = document.querySelector('.stage');
if (stage && mounted) {
  const line = document.createElement('p');
  line.className = 'stage__status';
  line.setAttribute('aria-live', 'off');
  stage.append(line);
  const modes = ['lores', 'hires', 'superhires'];
  const tick = () => {
    const t = lastTimings;
    if (t) {
      line.textContent =
        `Key ${signedPx(t.keyDelayPx)} + crawl ${t.crawl.toFixed(2)} of ${t.crawlWrap.toFixed(2)} px `
        + `= ${signedPx(t.delayAmiga)} ${modes[t.mode]}, crawling ${t.crawlRate.toFixed(2)} px/s · `
        + (t.locked ? 'vertical lock held' : `LOCK LOST, roll at ${(t.roll * 100).toFixed(0)}% of the picture`);
    }
    setTimeout(tick, 150);
  };
  tick();
}
