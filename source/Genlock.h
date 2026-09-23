#pragma once

#include <FFGLSDK.h>

//AFTER the SDK: this header names FFUInt32 and does not pull the SDK in
//itself, so an include placed above it fails with "unknown type name" errors
//that point at the About block rather than at the include order.
#include "StoatworksAboutParams.h"
#include "Timing.h"

#include <string>

/**
    Genlock -- an Amiga genlock, as an FFGL **mixer** for Resolume.

    A genlock keys the computer's colour 0 over incoming video. Its key is
    derived from the computer's own pixel clock, and that clock is not locked
    to the video's. So the key edge arrives a pixel or so away from the fill
    it belongs to, and because the two clocks differ slightly, that error
    **crawls**.

    Everything this plugin does falls out of that one disagreement: the
    coloured fringe down one side of every overlay edge, the fringe walking
    as the clocks drift, and the whole overlay losing vertical lock and
    rolling when the incoming sync degrades. `resolume-luma-keyer` is the
    keyer that works; here the flaws are the point.

    **This is the fleet's first FF_MIXER.** Read the mixer section of
    AGENTS.md before changing `ProcessOpenGL`: what is written there was
    measured, and several things the SDK's headers imply are not true.
*/
class Genlock : public CFFGLPlugin
{
public:
	Genlock();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	/// None of these three change a pixel. They exist so that ONE session in
	/// front of Resolume leaves a log that answers the things about mixers
	/// this repo has never been able to measure -- see "Reading the log after
	/// an Arena run" in AGENTS.md. SetHostInfo in particular makes the log
	/// self-identifying: it names the host and its version, so a log is
	/// evidence about a known build rather than about "Resolume".
	void SetHostInfo( const char* hostname, const char* version ) override;
	void SetBeatInfo( float bpm, float barPhase ) override;
	void SetSampleRate( unsigned int sampleRate ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// The SDK's `Add` mixer overrides this and the SDK's own `Mixer` base
	/// class does not, which is the kind of disagreement that costs an
	/// afternoon. It is NOT required -- `CFFGLPlugin::GetShortName` returns
	/// 0 and `getPluginShortName` in FFGL.cpp passes a null straight back to
	/// the host. See AGENTS.md. It is here because a four-character label is
	/// what a host with no room for "SW Genlock" will show, and guessing on
	/// the host's behalf is worse than saying.
	const char* GetShortName() override
	{
		return "GnLk";
	}

	/// The reduced phases the last rendered frame actually used. The harness
	/// compares these against the closed form as well as against the pixels,
	/// so a measurement that disagrees says WHICH of the two is wrong.
	struct Timings
	{
		double elapsedSeconds   = 0.0;
		double crawlAmigaPx     = 0.0;///< reduced into [0, crawlWrapAmigaPx)
		double crawlWrapAmigaPx = 0.0;///< the wrap the Crawl Wrap control asked for
		double delayAmigaPx     = 0.0;///< Key Delay plus the crawl
		double rollPhase        = 0.0;///< 0..1 picture heights; 0 while locked
		int amigaMode           = 0;  ///< the mode the last frame was rendered in
		bool locked             = true;
	};
	Timings TimingsForTest() const
	{
		return lastTimings;
	}

	/// Clock test hook. The offline harness DECLARES its unit rather than
	/// leaving the calibration to infer one.
	void SetClockScaleForTest( double scale )
	{
		clock.SetScaleForTest( scale );
	}

	/// Deliberate mistakes, for the harness's negative controls.
	///
	/// A check that cannot fail is not a check. Each of these is the wrong
	/// answer to one of the "does it scale with the mode?" questions in
	/// Controls.h, written into the real ProcessOpenGL so that `gltest
	/// --modes` can show its own assertion failing against it. The host can
	/// never reach them: nothing but this setter changes `fault`, and it
	/// starts at FAULT_NONE.
	enum Fault : int
	{
		FAULT_NONE = 0,
		FAULT_CRAWL_IN_LORES,       ///< the crawl rate ignores the mode's faster clock
		FAULT_DELAY_IN_LORES,       ///< Key Delay counted in lores pixels in every mode
		FAULT_TEAR_SCALES_WITH_MODE,///< the tear's throw counted in the mode's pixels
		FAULT_WRAP_FIXED            ///< Crawl Wrap ignored: the old constant one pixel
	};
	void SetFaultForTest( Fault f )
	{
		fault = f;
	}

	/// Compile THIS fragment shader in InitGL instead of the shipped one.
	/// Only `gltest --mutation` calls it: once with the shipped text, to show
	/// the default path and the hook render the same bytes, and once with a
	/// single character changed, to show a check fails. Must be called
	/// before InitGL, and the pointer must outlive it.
	void SetFragmentShaderForTest( const char* source )
	{
		fragmentOverride = source;
	}

	/// The order the host shows them in: cut the key, run the clocks, work
	/// the fader, then the look.
	enum ParamID : FFUInt32
	{
		//Key
		PT_KEY_SOURCE,
		PT_KEY_R,
		PT_KEY_G,
		PT_KEY_B,
		PT_TOLERANCE,
		PT_SOFTNESS,
		PT_INVERT,

		//Timing. Amiga Mode comes FIRST in the group because it sets the unit
		//every other control in it is stated in: one Amiga pixel is 1/320 of
		//the picture in lores, 1/640 in hires and 1/1280 in superhires, so
		//Key Delay and Crawl Wrap mean a different distance in each.
		PT_AMIGA_MODE,
		PT_KEY_DELAY,
		PT_CLOCK_ERROR,
		PT_CRAWL_RATE,
		PT_CRAWL_WRAP,
		PT_SYNC_QUALITY,
		PT_ROLL_RATE,

		//Fader
		PT_FADER,
		PT_DISSOLVE,

		//Look
		PT_FRINGE,
		PT_TINT_R,
		PT_TINT_G,
		PT_TINT_B,
		PT_OPACITY,

		//About. FFGL has no window and cannot make one, so the name, the
		//version, the maker and the links are parameters the host draws with
		//everything else. Last in the enum so nothing before it ever moves.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// Key Source, in declaration order.
	enum KeySource : int
	{
		KS_COLOUR_0 = 0,
		KS_LUMA     = 1,
		KS_ALPHA    = 2,
		KS_COUNT    = 3
	};

	/// Fader position, in declaration order: the three-position switch a
	/// genlock's front panel actually has.
	enum Fader : int
	{
		FD_VIDEO    = 0,///< the incoming video alone -- Dest, untouched
		FD_OVERLAY  = 1,///< the keyed composite
		FD_DISSOLVE = 2,///< a mix of the two, by the Dissolve pot
		FD_COUNT    = 3
	};

private:
	ffglex::FFGLShader shader;
	ffglex::FFGLScreenQuad quad;

	genlock::timing::Clock clock;
	Timings lastTimings;
	Fault fault                  = FAULT_NONE;
	const char* fragmentOverride = nullptr;

	/// What the log has already said, so a host calling something on every
	/// frame writes one line rather than sixty a second. See "Reading the log
	/// after an Arena run" in AGENTS.md for what each line answers.
	struct LogState
	{
		unsigned long frames    = 0;
		unsigned long beatCalls = 0;
		unsigned long timeCalls = 0;
		unsigned guardsLogged   = 0;///< one bit per guard that has fired
		int opacityLines        = 0;
		float lastOpacity       = -1.0f;
		bool firstTimeLogged    = false;
	};
	LogState logState;
	void logClock( const char* when );

	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
