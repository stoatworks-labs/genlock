#pragma once

/**
    The two clocks, and the arithmetic that keeps their difference small.

    A genlock's whole subject is a phase error that accumulates. The naive
    way to render one is `phase = rate * hostTime`, and that is wrong twice
    over in a host:

    - **Resolume hands over milliseconds, not seconds.** The FFGL header never
      says which, and hosts disagree. `Clock` settles the unit by watching the
      host's clock against a real one for a few frames -- the ratio names the
      unit -- exactly as tinsel does.
    - **A float cannot hold the host's clock.** Resolume counts milliseconds
      from the start of the session, and past about 4.99e8 ms -- a little under
      six days -- a 32-bit float can no longer represent consecutive
      milliseconds at all. Anything computed from an absolute host time in
      float stops moving. So time here is kept **frame-relative**: the first
      settled reading becomes the epoch, everything downstream is `now -
      epoch`, and the reduction to a phase happens in **double** before any
      float ever sees it.

    What the shader is handed is never a time. It is a phase already reduced
    into its own period -- under one Amiga pixel for the crawl, under one
    picture height for the roll -- so the float it arrives in has its full
    precision available for the part that matters.
*/
namespace genlock::timing
{

/// The host's clock, in seconds, with its unit worked out by observation.
class Clock
{
public:
	/// Called from the plugin's SetTime with whatever the host said.
	void Observe( double hostTime );

	/// Advance to this frame. Must be called once per ProcessOpenGL, after
	/// Observe. Returns seconds since the epoch.
	double Tick();

	/// Seconds since the epoch, as of the last Tick.
	double Elapsed() const
	{
		return elapsed;
	}

	/// The multiplier taking the host's unit to seconds: 1.0 for seconds,
	/// 0.001 for milliseconds, 0 while undecided. Declared rather than
	/// inferred by the offline harness, which knows it sends seconds.
	void SetScaleForTest( double scale )
	{
		scale_ = scale;
	}

	double Scale() const
	{
		return scale_;
	}

private:
	double raw_        = -1.0;///< the host's last reading, in the host's unit
	double lastRaw_    = -1.0;
	double lastWall_   = -1.0;
	double wallStart_  = -1.0;
	double scale_      = 0.0;
	double epoch_      = 0.0;
	bool epochSet_     = false;
	int secondsVotes_  = 0;
	int millisVotes_   = 0;
	double elapsed     = 0.0;
};

/// The crawl phase: how far the key has walked from where the operator put
/// it, in Amiga pixels, reduced into [0, wrapPx).
///
/// Reduced in double, and from an elapsed time that is already frame-relative.
/// `rate` can reach 350 Amiga px/s at the top of the Clock Error range, so
/// over an eight-hour session the product is about 1e7 -- comfortably inside
/// double's exact integers and nowhere near a float's.
double CrawlPhase( double elapsedSeconds, double ratePxPerSecond, double wrapPx );

/// The roll phase: the overlay's vertical position, in picture heights,
/// reduced into [0, 1). Zero when vertical lock is held.
double RollPhase( double elapsedSeconds, double rollsPerSecond );

/// A positive remainder. `fmod` keeps the sign of the numerator, so a
/// negative rate -- or a host that scrubs backwards -- would otherwise put
/// the phase outside its own period.
double PositiveMod( double value, double period );

} // namespace genlock::timing
