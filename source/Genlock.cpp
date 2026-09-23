#include "Genlock.h"

#include "Controls.h"
#include "Diag.h"
#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace ffglex;
using namespace genlock;

//---------------------------------------------------------------------------
// The eighth argument is the plugin TYPE, and it is the only thing in the
// whole repo that makes this a mixer rather than an effect.
//
// There are two routes to FF_MIXER in SDK b1afaf9 and they do not meet:
// derive from `ffglqs::Mixer`, whose `createPlugin` passes FF_MIXER, or
// declare it here. The SDK's own `Add` example -- the only mixer that exists
// anywhere in the SDK -- declares it here, and so does this. See AGENTS.md
// for why `ffglqs::Mixer` is not usable for a plugin that wants its own
// vertex shader.
//---------------------------------------------------------------------------
static CFFGLPluginInfo PluginInfo(
	PluginFactory< Genlock >,// Create method
	"GL01",                  // Plugin unique ID of maximum length 4.
	"SW Genlock",            // Plugin name
	2,                       // API major version number
	1,                       // API minor version number
	0,                       // Plugin major version number
	1,                       // Plugin minor version number
	FF_MIXER,                // Plugin type
	"An Amiga genlock, flaws and all. Keys this layer's colour 0 over the layer below -- but cuts the key on the computer's own pixel clock, which is not locked to the video. The key lands a pixel away from the fill, so every overlay edge carries a coloured fringe, and as the two clocks drift the fringe crawls. Drop Sync Quality and the overlay loses vertical lock and rolls.\n\nThis is a MIXER: it needs a layer below it.",
	"Genlock FFGL mixer" );

static_assert( Genlock::PT_COUNT - Genlock::PT_ABOUT_FIRST == stoatworks::about::kParamCount,
               "the About block's size changed with the generated header" );

//---------------------------------------------------------------------------
// One log line at LOAD time, before any instance exists.
//
// A host that scans the plugin folder dlopens every bundle in it and reads
// its info, whether or not it ever offers the plugin to the operator. So a
// log holding this line and nothing after it says "Resolume found the file
// and did not instantiate it" -- which is a different answer from no log at
// all ("it never looked in that folder"), and the only way to tell the two
// apart from inside the plugin. The path is the one the loader actually
// resolved -- which folder the host really scanned.
//
// Runs inside dlopen / DllMain. Diag::init does no fork, no exec and no
// shell -- see createDirectories -- and its state is a function-local static,
// so the order this object is constructed in relative to Diag.cpp's does
// not matter.
//---------------------------------------------------------------------------
namespace
{
struct LoadTimeLog
{
	LoadTimeLog()
	{
		genlock::diag::init();
		genlock::diag::info( "loaded from " + genlock::diag::modulePath() );
	}
};
const LoadTimeLog g_loadTimeLog;
} // namespace

namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kKeySourceNames[] = { "Colour 0", "Luma", "Alpha" };
const char* const kAmigaModeNames[] = { "Lores", "Hires", "Superhires" };
const char* const kFaderNames[]     = { "Video", "Overlay", "Dissolve" };

static_assert( sizeof( kAmigaModeNames ) / sizeof( kAmigaModeNames[ 0 ] ) == AM_COUNT,
               "one name per Amiga mode" );

std::string number( double value )
{
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.6g", value );
	return buffer;
}

int optionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}
} // namespace

Genlock::Genlock()
{
	//A mixer takes exactly two. The host reads these back through
	//getNumParameters' neighbours and decides what to offer the operator;
	//`Add` sets the same pair.
	SetMinInputs( 2 );
	SetMaxInputs( 2 );
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter,
	// so these assignments are what the host is told the defaults are.
	//---------------------------------------------------------------------
	params[ PT_KEY_SOURCE ] = static_cast< float >( KS_COLOUR_0 );
	//Workbench blue, #0055AA: the colour 0 a generation of Amigas booted to
	//and the one most genlock footage was keyed on.
	params[ PT_KEY_R ]    = 0.0f;
	params[ PT_KEY_G ]    = 0.333f;
	params[ PT_KEY_B ]    = 0.667f;
	params[ PT_TOLERANCE ] = 0.12f;
	params[ PT_SOFTNESS ]  = 0.06f;
	params[ PT_INVERT ]    = 0.0f;

	//Lores and a one-pixel wrap: exactly what this plugin did before either
	//control existed, so a saved composition from then renders the same
	//bytes. `gltest --defaults` holds both to it.
	params[ PT_AMIGA_MODE ]   = static_cast< float >( AM_LORES );
	params[ PT_KEY_DELAY ]    = 0.4375f;//-1 Amiga pixel: the colour-0 fringe
	params[ PT_CLOCK_ERROR ]  = 0.30f;  //about 0.13 ppm -- one pixel per second
	params[ PT_CRAWL_RATE ]   = 0.25f;  //unity
	params[ PT_CRAWL_WRAP ]   = 0.0f;   //one Amiga pixel: a clean re-lock every line
	params[ PT_SYNC_QUALITY ] = 1.0f;   //locked
	params[ PT_ROLL_RATE ]    = 0.125f; //half a roll per second, once lock is lost

	params[ PT_FADER ]    = static_cast< float >( FD_OVERLAY );
	params[ PT_DISSOLVE ] = 0.5f;

	params[ PT_FRINGE ] = 0.35f;
	params[ PT_TINT_R ] = 0.25f;
	params[ PT_TINT_G ] = 0.95f;
	params[ PT_TINT_B ] = 1.0f;
	params[ PT_OPACITY ] = 1.0f;

	//---------------------------------------------------------------------
	// Declaration. Every ranged parameter is a plain 0..1 float, with the
	// conversions in Controls.cpp -- see the note there.
	//---------------------------------------------------------------------
	auto option = [ this ]( unsigned int id, const char* name, int count, const char* const* names ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), names[ i ], static_cast< float >( i ) );
	};
	auto colour = [ this ]( unsigned int first, const char* label ) {
		//Consecutive red/green/blue parameters are what a host needs to show
		//a swatch instead of three sliders.
		const std::string base = label;
		SetParamInfo( first + 0, ( base + " Red" ).c_str(), FF_TYPE_RED, params[ first + 0 ] );
		SetParamInfo( first + 1, ( base + " Green" ).c_str(), FF_TYPE_GREEN, params[ first + 1 ] );
		SetParamInfo( first + 2, ( base + " Blue" ).c_str(), FF_TYPE_BLUE, params[ first + 2 ] );
	};

	option( PT_KEY_SOURCE, "Key Source", KS_COUNT, kKeySourceNames );
	colour( PT_KEY_R, "Key Colour" );
	SetParamInfof( PT_TOLERANCE, "Tolerance", FF_TYPE_STANDARD );
	SetParamInfof( PT_SOFTNESS, "Softness", FF_TYPE_STANDARD );
	SetParamInfo( PT_INVERT, "Invert", FF_TYPE_BOOLEAN, false );

	option( PT_AMIGA_MODE, "Amiga Mode", AM_COUNT, kAmigaModeNames );
	SetParamInfof( PT_KEY_DELAY, "Key Delay", FF_TYPE_STANDARD );
	SetParamInfof( PT_CLOCK_ERROR, "Clock Error", FF_TYPE_STANDARD );
	SetParamInfof( PT_CRAWL_RATE, "Crawl Rate", FF_TYPE_STANDARD );
	SetParamInfof( PT_CRAWL_WRAP, "Crawl Wrap", FF_TYPE_STANDARD );
	SetParamInfof( PT_SYNC_QUALITY, "Sync Quality", FF_TYPE_STANDARD );
	SetParamInfof( PT_ROLL_RATE, "Roll Rate", FF_TYPE_STANDARD );

	option( PT_FADER, "Fader", FD_COUNT, kFaderNames );
	SetParamInfof( PT_DISSOLVE, "Dissolve", FF_TYPE_STANDARD );

	SetParamInfof( PT_FRINGE, "Fringe", FF_TYPE_STANDARD );
	colour( PT_TINT_R, "Edge Tint" );
	//Named Opacity, not Mix. The SDK's Add example says in as many words
	//that "Resolume will look for a param named Opacity for mix value", and
	//ffglqs::Mixer declares its own under the name mixVal. Neither claim can
	//be tested here, and of the two names Opacity is the one with evidence
	//behind it. See AGENTS.md -- the spec called this control Mix.
	SetParamInfof( PT_OPACITY, "Opacity", FF_TYPE_STANDARD );

	// Groups, the way Resolume shows them. SetParamGroup collapses runs of
	// consecutive same-group ids, so each group is one contiguous run.
	for( FFUInt32 i = PT_KEY_SOURCE; i <= PT_INVERT; ++i )
		SetParamGroup( i, "Key" );
	for( FFUInt32 i = PT_AMIGA_MODE; i <= PT_ROLL_RATE; ++i )
		SetParamGroup( i, "Timing" );
	for( FFUInt32 i = PT_FADER; i <= PT_DISSOLVE; ++i )
		SetParamGroup( i, "Fader" );
	for( FFUInt32 i = PT_FRINGE; i <= PT_OPACITY; ++i )
		SetParamGroup( i, "Look" );

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Genlock mixer" );

	diag::init();
	diag::info( "instance created" );
}

//---------------------------------------------------------------------------
FFResult Genlock::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	if( !shader.Compile( kVertexShader, fragmentOverride != nullptr ? fragmentOverride : kGenlockShader ) )
	{
		//Returning FF_FAIL here is invisible to the operator: the mixer
		//simply does nothing in Resolume. These two lines are the only
		//record of why.
		diag::error( "the genlock shader failed to compile - the mixer will do nothing" );
		FFGLLog::LogToHost( "Genlock: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised, viewport " + number( vp ? vp->width : 0 ) + "x" + number( vp ? vp->height : 0 ) );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Genlock::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	//The SDK's Add example guards on the input count and on each pointer,
	//with a comment saying a host calls a mixer with one input while the
	//operator is still patching. That is its claim, not something measured
	//here -- but it costs four comparisons to believe it, and a mixer that
	//dereferenced a null would take Resolume down with it. Every check
	//before anything is read.
	//
	//Each guard logs the FIRST time it fires, and only then: whether a host
	//really does this, and which way, is one of the open questions about
	//mixers, and a host that does it on every frame while patching must
	//not fill the disk answering it.
	const auto guard = [ this ]( unsigned bit, const std::string& what ) {
		if( ( logState.guardsLogged & ( 1u << bit ) ) == 0 )
		{
			logState.guardsLogged |= 1u << bit;
			diag::warn( "guard: " + what + " -- returned FF_FAIL (logged once)" );
		}
		return FF_FAIL;
	};
	if( pGL == nullptr || pGL->inputTextures == nullptr )
		return guard( 0, "no input array" );
	if( pGL->numInputTextures < 2 )
		return guard( 1, "called with " + number( pGL->numInputTextures ) + " input(s)" );
	if( pGL->inputTextures[ 0 ] == nullptr || pGL->inputTextures[ 1 ] == nullptr )
		return guard( 2, std::string( "a null " ) + ( pGL->inputTextures[ 0 ] == nullptr ? "Dest" : "Src" ) );

	const FFGLTextureStruct& dest = *pGL->inputTextures[ 0 ];//the layer below
	const FFGLTextureStruct& src  = *pGL->inputTextures[ 1 ];//this layer
	//HardwareWidth and HardwareHeight are the DENOMINATORS in
	//GetMaxGLTexCoords, so a zero there is an infinite MaxUV rather than a
	//small picture.
	if( dest.Width == 0 || dest.Height == 0 || dest.HardwareWidth == 0 || dest.HardwareHeight == 0 )
		return guard( 3, "a zero-sized Dest" );
	if( src.Width == 0 || src.Height == 0 || src.HardwareWidth == 0 || src.HardwareHeight == 0 )
		return guard( 3, "a zero-sized Src" );

	//The first frame's two inputs, as the host handed them: whether a mixer
	//really is given two different sizes, and with what padding, is the
	//case the two MaxUVs exist for.
	if( logState.frames == 0 )
	{
		const auto describe = []( const FFGLTextureStruct& t ) {
			return number( t.Width ) + "x" + number( t.Height ) + " of " + number( t.HardwareWidth ) + "x"
			       + number( t.HardwareHeight );
		};
		diag::info( "first frame: Dest " + describe( dest ) + ", Src " + describe( src ) );
	}

	//-----------------------------------------------------------------
	// The two clocks. Everything the shader is handed is already reduced
	// into its own period, in double, from a frame-relative time.
	//-----------------------------------------------------------------
	const double elapsed = clock.Tick();

	//The mode sets the unit. Every "Amiga pixel" from here to the uniforms
	//is a pixel OF THIS MODE -- 1/320, 1/640 or 1/1280 of the line -- except
	//the tear's throw, which Controls.h says why is always lores.
	const int mode         = optionIndex( params[ PT_AMIGA_MODE ], AM_COUNT );
	const double modeWidth = AmigaWidthForMode( mode );

	//The crawl rate is in pixels of this mode per second, because it is this
	//mode's pixel clock that drifts. Divided by this mode's width below, the
	//mode cancels exactly -- see CrawlRateAmigaPxPerSecond.
	const double crawlRate = CrawlRateAmigaPxPerSecond( params[ PT_CLOCK_ERROR ], params[ PT_CRAWL_RATE ],
	                                                    fault == FAULT_CRAWL_IN_LORES ? AM_LORES : mode );
	const double crawlWrap = fault == FAULT_WRAP_FIXED
	                             ? kCrawlWrapMinAmigaPx
	                             : static_cast< double >( CrawlWrapAmigaPxFromParam( params[ PT_CRAWL_WRAP ] ) );
	const double crawl     = timing::CrawlPhase( elapsed, crawlRate, crawlWrap );

	//Key Delay is a count of this mode's pixel clocks, so it goes in as it
	//is and the division by this mode's width makes it half the distance in
	//hires. The sum is taken BEFORE the division, exactly as it was before
	//the mode existed, so lores renders the same bits it always did.
	const double keyDelayPx = static_cast< double >( KeyDelayFromParam( params[ PT_KEY_DELAY ] ) );
	const double delayAmiga = keyDelayPx + crawl;
	const double keyDelayPicture = fault == FAULT_DELAY_IN_LORES ? keyDelayPx / kAmigaLoresWidth + crawl / modeWidth
	                                                             : delayAmiga / modeWidth;

	const float quality = std::clamp( params[ PT_SYNC_QUALITY ], 0.0f, 1.0f );
	const bool locked   = quality >= kLockThreshold;
	const double roll   = locked ? 0.0 : timing::RollPhase( elapsed, RollRateFromParam( params[ PT_ROLL_RATE ] ) );
	//How far past the threshold the sync has fallen, 0 at the threshold and
	//1 at no sync at all. Only the TEAR scales with it; the roll rate is the
	//rate the operator asked for, so that it can be checked against one.
	const float lockLoss = locked ? 0.0f : ( kLockThreshold - quality ) / kLockThreshold;

	lastTimings.elapsedSeconds   = elapsed;
	lastTimings.crawlAmigaPx     = crawl;
	lastTimings.crawlWrapAmigaPx = crawlWrap;
	lastTimings.delayAmigaPx     = delayAmiga;
	lastTimings.rollPhase        = roll;
	lastTimings.amigaMode        = mode;
	lastTimings.locked           = locked;

	//Whether the host drives a MIXER's clock at all: once on the first
	//frame, and again five seconds in at 60 fps, by which time the unit vote
	//has had every chance to settle.
	++logState.frames;
	if( logState.frames == 1 )
		logClock( "frame 1" );
	else if( logState.frames == 300 )
		logClock( "frame 300" );

	//-----------------------------------------------------------------
	// Bind both inputs.
	//
	// The declaration ORDER of these four objects matters and is not
	// obvious: every ffglex::Scoped* clears its binding on exit rather than
	// restoring it, so they must unwind as activate(1), bind(1) then
	// activate(0), bind(0) -- which is what this order gives. Interleaved
	// like this, each texture is unbound while its own unit is still the
	// active one. Both activations first would unbind unit 0 twice and
	// leave unit 1 bound.
	//-----------------------------------------------------------------
	ScopedShaderBinding shaderBinding( shader.GetGLID() );
	ScopedSamplerActivation activateDest( 0 );
	Scoped2DTextureBinding bindDest( dest.Handle );
	ScopedSamplerActivation activateSrc( 1 );
	Scoped2DTextureBinding bindSrc( src.Handle );

	shader.Set( "TextureDest", 0 );
	shader.Set( "TextureSrc", 1 );

	//One MaxUV per input. They are different numbers whenever the two
	//layers are different sizes, which for a mixer is the normal case.
	const FFGLTexCoords maxDest = GetMaxGLTexCoords( dest );
	const FFGLTexCoords maxSrc  = GetMaxGLTexCoords( src );
	shader.Set( "MaxUVDest", maxDest.s, maxDest.t );
	shader.Set( "MaxUVSrc", maxSrc.s, maxSrc.t );
	shader.Set( "HalfTexelDest", 0.5f / static_cast< float >( dest.Width ), 0.5f / static_cast< float >( dest.Height ) );
	shader.Set( "HalfTexelSrc", 0.5f / static_cast< float >( src.Width ), 0.5f / static_cast< float >( src.Height ) );

	shader.Set( "KeySource", static_cast< float >( optionIndex( params[ PT_KEY_SOURCE ], KS_COUNT ) ) );
	shader.Set( "KeyColour", params[ PT_KEY_R ], params[ PT_KEY_G ], params[ PT_KEY_B ] );
	shader.Set( "Tolerance", ToleranceFromParam( params[ PT_TOLERANCE ] ) );
	shader.Set( "Softness", SoftnessFromParam( params[ PT_SOFTNESS ] ) );
	shader.Set( "Invert", params[ PT_INVERT ] > 0.5f ? 1.0f : 0.0f );

	//Amiga pixels to a fraction of the picture width. One Amiga pixel is
	//1/320, 1/640 or 1/1280 of the active line whatever the raster is, which
	//is what makes the fringe the same size at 720p and at 4K.
	//
	//The tear is thrown by a failing SYNC -- a time error -- so its throw is
	//a distance on the picture and is stated in LORES pixels in every mode.
	//See kTearAmigaPx.
	const double tearWidth = fault == FAULT_TEAR_SCALES_WITH_MODE ? modeWidth : kAmigaLoresWidth;
	shader.Set( "KeyDelay", static_cast< float >( keyDelayPicture ) );
	shader.Set( "RollPhase", static_cast< float >( roll ) );
	shader.Set( "TearThrow", static_cast< float >( lockLoss * kTearAmigaPx / tearWidth ) );
	shader.Set( "TearBand", kTearBand );

	shader.Set( "FaderMode", static_cast< float >( optionIndex( params[ PT_FADER ], FD_COUNT ) ) );
	shader.Set( "Dissolve", std::clamp( params[ PT_DISSOLVE ], 0.0f, 1.0f ) );

	shader.Set( "Fringe", std::clamp( params[ PT_FRINGE ], 0.0f, 1.0f ) );
	shader.Set( "EdgeTint", params[ PT_TINT_R ], params[ PT_TINT_G ], params[ PT_TINT_B ] );
	shader.Set( "Opacity", std::clamp( params[ PT_OPACITY ], 0.0f, 1.0f ) );

	//Whether Resolume binds a parameter called Opacity to the transition.
	//If it does, this moves with the layer's crossfader without the operator
	//touching the slider. The first sixteen changes of more than a hundredth,
	//and then silence.
	const float opacity = params[ PT_OPACITY ];
	if( logState.opacityLines < 16 && std::fabs( opacity - logState.lastOpacity ) > 0.01f )
	{
		diag::info( "Opacity " + number( opacity ) + " at frame " + number( static_cast< double >( logState.frames ) )
		            + ( ++logState.opacityLines == 16 ? " (the last Opacity line)" : "" ) );
		logState.lastOpacity = opacity;
	}

	quad.Draw();

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Genlock::DeInitGL()
{
	if( logState.frames > 0 )
	{
		logClock( "DeInitGL" );
		diag::info( "DeInitGL after " + number( static_cast< double >( logState.frames ) ) + " frames, "
		            + number( static_cast< double >( logState.beatCalls ) ) + " SetBeatInfo calls" );
	}
	shader.FreeGLResources();
	quad.Release();
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Genlock::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Genlock::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Genlock::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Genlock::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult Genlock::SetTime( double time )
{
	clock.Observe( time );
	++logState.timeCalls;
	if( !logState.firstTimeLogged )
	{
		logState.firstTimeLogged = true;
		diag::info( "first SetTime " + number( time ) + " (before frame " + number( static_cast< double >( logState.frames + 1 ) ) + ")" );
	}
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
// Diagnostics only. None of these changes a pixel -- see Genlock.h.
//---------------------------------------------------------------------------
void Genlock::logClock( const char* when )
{
	int seconds = 0, millis = 0;
	clock.Votes( seconds, millis );
	const double scale = clock.Scale();
	diag::info( std::string( "clock at " ) + when + ": SetTime " + ( clock.Observed() ? "called" : "NEVER called" )
	            + " (" + number( static_cast< double >( logState.timeCalls ) ) + " calls, last " + number( clock.Raw() )
	            + "), unit " + ( scale == 1.0 ? "seconds" : scale == 0.001 ? "milliseconds" : "undecided" )
	            + ", votes s=" + number( seconds ) + " ms=" + number( millis )
	            + ", elapsed " + number( clock.Elapsed() ) + " s" );
}

void Genlock::SetHostInfo( const char* hostname, const char* version )
{
	CFFGLPlugin::SetHostInfo( hostname, version );
	diag::info( std::string( "host " ) + ( hostname ? hostname : "(null)" ) + " version "
	            + ( version ? version : "(null)" ) );
}

void Genlock::SetBeatInfo( float bpm, float barPhase )
{
	CFFGLPlugin::SetBeatInfo( bpm, barPhase );
	//Hosts send this every frame. The first one says the host sends it to a
	//mixer at all; the count arrives at DeInitGL.
	if( logState.beatCalls++ == 0 )
		diag::info( "first SetBeatInfo bpm " + number( bpm ) + " bar phase " + number( barPhase ) );
}

void Genlock::SetSampleRate( unsigned int sampleRate )
{
	CFFGLPlugin::SetSampleRate( sampleRate );
	diag::info( "SetSampleRate " + number( sampleRate ) );
}
