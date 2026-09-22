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
    an Amiga lores line is 320 of them across the active picture whatever the
    monitor is. So one Amiga pixel is 1/320 of the picture width at every
    raster, which is also the only definition under which the artefact looks
    the same at 720p and at 4K.
*/
namespace genlock
{

/// Lores pixels across the active line of a PAL/NTSC Amiga display. Every
/// horizontal quantity in this plugin is measured in these.
inline constexpr double kAmigaWidth = 320.0;

/// The Amiga's lores pixel clock, PAL: twice the 3.546895 MHz colour carrier.
/// This is the number that turns a clock error in ppm into a crawl rate in
/// Amiga pixels per second, and it is the only reason the crawl has a speed
/// rather than a taste.
inline constexpr double kAmigaPixelClockHz = 7093790.0;

/// How much accumulated phase error survives before the line sync pulls it
/// back, in Amiga pixels. The genlock re-locks on every horizontal sync, so
/// a free-running clock does not slide the picture away -- only the
/// sub-pixel remainder shows, and it wraps. One pixel is that remainder.
inline constexpr double kCrawlWrapAmigaPx = 1.0;

/// Below this Sync Quality the overlay loses vertical lock. Above it the
/// roll is exactly zero -- a branch, not a fade, because that is what losing
/// lock is.
inline constexpr float kLockThreshold = 0.5f;

/// The band around the roll seam that tears, as a fraction of picture height.
inline constexpr float kTearBand = 0.06f;

/// The widest a tear can throw a line, in Amiga pixels, at total loss of lock.
inline constexpr float kTearAmigaPx = 24.0f;

/// -8 to +8 Amiga pixels, linear, with 0.5 as zero. The signed horizontal
/// displacement of the KEY relative to the fill. Negative lays the palette's
/// colour 0 over the video along the left of every edge -- the coloured
/// fringe; positive cuts video into the fill instead.
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

/// 0 to 4 rolls per second, linear. How fast the overlay rolls once vertical
/// lock is gone. One roll is the whole picture height.
float RollRateFromParam( float value );

/// The crawl rate in Amiga pixels per second: the pixel clock times the
/// fractional frequency error, times the operator's multiplier. The closed
/// form `gltest --crawl` measures against.
double CrawlRateAmigaPxPerSecond( float clockErrorParam, float crawlRateParam );

} // namespace genlock
