#pragma once

/**
    Host parameters are 0..1; these are what they mean.

    Every ranged parameter this plugin declares is a plain FF_TYPE_STANDARD
    float in 0..1, including the ones that stand for Amiga pixels, parts per
    million or rolls per second. That is not a style preference: `SetParamInfo`
    clamps a standard default into 0..1 *before* returning, and `SetParamRange`
    can only be called afterwards -- so a parameter declared in pixels cannot
    declare a default in pixels, and 8 would silently become 1. The conversions
    live here, in one file the plugin and the harness both use, so there is
    only ever one answer to what a slider position means.

    The unit that matters is the **Amiga pixel**, not the output pixel. A
    genlock's fringe is a fixed number of the computer's own pixels wide, and
    an Amiga LORES line is 320 of them across the active picture whatever the
    monitor is. So one lores pixel is 1/320 of the picture width at every
    raster, which is also the only definition under which the artefact looks
    the same at 720p and at 4K.

    ------------------------------------------------------------ and the mode

    An Amiga pixel is only 1/320 of the line in lores. The display mode sets
    both the count across the line and the pixel clock, and the two move
    together -- which decides, for each quantity here, whether it scales with
    the mode or not:

    - a quantity that is a count of the computer's own PIXEL CLOCKS (the key's
      delay, the phase the line sync leaves behind) is half the DISTANCE in
      hires and a quarter in superhires, because the clock is twice and four
      times as fast;
    - a quantity that is a TIME ERROR in the incoming video (the crawl's
      speed, the tear's throw) is the same distance in every mode, because
      nothing about it is counted in the computer's pixels at all.

    Both kinds are below, and each says which it is.
*/
namespace genlock
{

/// The Amiga's display mode. Lores puts 320 pixels across the active line,
/// hires 640 and superhires 1280, and the pixel clock doubles with each.
///
/// Superhires is here because it is the third and last member of that set and
/// its clock is exactly four times lores'. Genlocking one was never common --
/// most genlocks were composite-rate devices with nothing like the bandwidth
/// to resolve a 28 MHz pixel -- and that is a statement about the hardware,
/// not about the arithmetic.
enum AmigaMode : int
{
	AM_LORES      = 0,
	AM_HIRES      = 1,
	AM_SUPERHIRES = 2,
	AM_COUNT      = 3
};

/// Lores pixels across the active line of a PAL/NTSC Amiga display.
inline constexpr double kAmigaLoresWidth = 320.0;

/// The Amiga's LORES pixel clock, PAL: twice the 3.546895 MHz colour carrier,
/// which is itself four fifths of the 4.43361875 MHz subcarrier. This is the
/// number that turns a clock error in ppm into a crawl rate in Amiga pixels
/// per second, and it is the only reason the crawl has a speed rather than a
/// taste.
inline constexpr double kAmigaLoresPixelClockHz = 7093790.0;

/// Pixels across the active line in this mode: 320, 640, 1280. Out-of-range
/// modes answer lores.
double AmigaWidthForMode( int mode );

/// The pixel clock in this mode, in Hz: the lores clock times an exact power
/// of two. Scaling both this and the width by the same power of two is what
/// makes the crawl's speed ACROSS THE PICTURE identical in every mode, to the
/// bit -- see CrawlRateAmigaPxPerSecond.
double AmigaPixelClockHzForMode( int mode );

/// How much accumulated phase error survives before the line sync pulls it
/// back, in Amiga pixels of the current mode. The genlock re-locks on every
/// horizontal sync, so a free-running clock does not slide the picture away
/// -- only the remainder shows, and it wraps.
///
/// ONE pixel is the remainder a genlock that re-locks cleanly on every line
/// leaves behind, and it is the bottom of the range and the default: the
/// phase is quantised by the pixel clock, so anything below one pixel of it
/// is not a thing the hardware can hold.
inline constexpr double kCrawlWrapMinAmigaPx = 1.0;

/// SIXTEEN is the top, and it is the colour burst.
///
/// The lores pixel clock is exactly 1.6 times the PAL subcarrier (7.09379 =
/// 1.6 x 4.43361875), and the burst is ten subcarrier cycles -- so the burst
/// is exactly 10 x 1.6 = 16 lores pixels long. A genlock locks its clock to
/// that burst through a gate it opens for the burst's duration. Once the
/// accumulated phase error exceeds the burst, the gate no longer overlaps the
/// burst at all and there is nothing left to lock to: past 16 pixels the
/// device is not a genlock with poor line sync, it is a device that has lost
/// lock, which is what Sync Quality is for.
inline constexpr double kCrawlWrapMaxAmigaPx = 16.0;

/// Below this Sync Quality the overlay loses vertical lock. Above it the
/// roll is exactly zero -- a branch, not a fade, because that is what losing
/// lock is.
inline constexpr float kLockThreshold = 0.5f;

/// The band around the roll seam that tears, as a fraction of picture height.
inline constexpr float kTearBand = 0.06f;

/// The widest a tear can throw a line, in LORES pixels, at total loss of
/// lock -- and it stays in lores pixels in every mode.
///
/// A tear is the incoming sync collapsing, so the line is thrown by a TIME
/// error, and a time error is the same distance across the picture whatever
/// the computer's pixel clock happens to be doing. Scaling this with the mode
/// would say the overlay's own resolution changes how far a failing sync
/// throws a line, which is backwards.
inline constexpr float kTearAmigaPx = 24.0f;

/// -8 to +8 Amiga pixels, linear, with 0.5 as zero. The signed horizontal
/// displacement of the KEY relative to the fill, in pixels of the CURRENT
/// MODE -- a count of the computer's own pixel clocks, so the same nominal
/// delay is half the fringe in hires and a quarter in superhires. Negative
/// lays the palette's colour 0 over the video along the left of every edge --
/// the coloured fringe; positive cuts video into the fill instead.
float KeyDelayFromParam( float value );

/// 0 to 1, linear. How far a pixel may be from the key colour and still be
/// keyed, as a fraction of the longest distance in the unit RGB cube.
float ToleranceFromParam( float value );

/// 0 to 0.5, linear, in the same distance units as Tolerance. The width of
/// the key's soft edge.
float SoftnessFromParam( float value );

/// 0.01 to 50 ppm, geometrically. The frequency difference between the
/// Amiga's pixel clock and the incoming video's.
///
/// Geometric because the interesting range is all at the bottom: at 0.01 ppm
/// the fringe takes fourteen seconds to walk one Amiga pixel, and above about
/// 1 ppm the phase slews faster than seven pixels a second and stops reading
/// as a crawl at all. 50 ppm is an ordinary crystal's tolerance and it looks
/// like a blur, which is the honest answer.
float ClockErrorPpmFromParam( float value );

/// 0 to 4, linear, with 0.25 as unity. A dimensionless multiplier on the
/// crawl rate. It does the same thing to the rate that Clock Error does, on
/// purpose: it lets an operator freeze or exaggerate the crawl without
/// restating the clock error, and 0 freezes it without claiming the clocks
/// are locked.
float CrawlRateFromParam( float value );

/// kCrawlWrapMinAmigaPx to kCrawlWrapMaxAmigaPx, LINEAR, so a slider position
/// reads as a number of Amiga pixels rather than as a curve. 0 is 1 pixel --
/// a genlock that re-locks cleanly on every line, which is what this plugin
/// did before the control existed -- and 1 is the colour burst.
///
/// Linear and not geometric because this is a DISTANCE the fringe walks, and
/// a distance control whose middle is not its middle is a control an operator
/// cannot read. The span is only 16x; Clock Error is geometric because its
/// span is 5000x.
float CrawlWrapAmigaPxFromParam( float value );

/// The slider position that means this many Amiga pixels of wrap. The inverse
/// of the map above, for a harness that wants to state the physics first and
/// find the slider afterwards.
float CrawlWrapParamFor( double amigaPixels );

/// 0 to 4 rolls per second, linear. How fast the overlay rolls once vertical
/// lock is gone. One roll is the whole picture height.
float RollRateFromParam( float value );

/// The crawl rate in Amiga pixels per second OF THE GIVEN MODE: that mode's
/// pixel clock times the fractional frequency error, times the operator's
/// multiplier. The closed form `gltest --crawl` measures against.
///
/// Because the clock and the width scale by the same exact power of two, the
/// crawl's speed across the PICTURE -- rate divided by the mode's width -- is
/// identical in all three modes, bit for bit. That is the right answer and
/// not a coincidence: the crawl is a time error accumulating, and counting it
/// in smaller pixels does not make it travel further.
double CrawlRateAmigaPxPerSecond( float clockErrorParam, float crawlRateParam, int mode );

} // namespace genlock
