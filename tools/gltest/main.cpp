/**
    gltest -- render Genlock offline, and measure what the two clocks are doing.

    This is the fleet's first harness that drives **two inputs**, because
    Genlock is the fleet's first FFGL mixer. `inputTextures[0]` is Dest, the
    layer below -- the incoming video. `inputTextures[1]` is Src, this layer
    -- the computer's picture. They are `--input-a` and `--input-b` here, and
    they may be different sizes, with different hardware padding, rendered to
    an output that is a third size again.

        gltest --out /tmp/frame.png     a picture, through the real plugin
        gltest --list                   every parameter, for the sweep
        gltest --names                  no name over FFGL's 16 characters
        gltest --mixer                  two inputs, two MaxUVs, and the guards
        gltest --delay                  the key lags the fill by the stated amount
        gltest --crawl                  the delay walks at the rate the ppm predicts
        gltest --roll                   the overlay rolls at the stated rate and wraps
        gltest --fader                  at Video the output IS Dest
        gltest --modes                  Amiga Mode and Crawl Wrap, with negative controls
        gltest --defaults               the new controls' defaults ARE the old behaviour
        gltest --mutation               a one-character GLSL mutation fails a check
        gltest --bench                  ms/frame at 720p through 4K

    Every check renders through the REAL plugin class in a headless CGL
    context. There is no CPU mirror of the shader to drift: what is compared
    against is an independent statement of the arithmetic (a closed form, a
    whole-texel translation, or the input itself), not a transcription.

    ------------------------------------------------------- about the numbers

    Every tolerance in this file is derived from something physical -- one
    output texel, one source texel, one 8-bit code, one frame, the raster's
    own height -- and never from the number this machine happened to print
    first. AGENTS.md lists them one by one with the reasoning. Two rules run
    through all of them:

      * A **whole-texel** displacement translates a sampled image exactly.
        A **fractional** one lands as partial coverage that has to be
        recovered from the filter's own interpolation, and that is
        rasteriser-dependent. They get different tolerances and they are
        never checked by the same measurement.
      * An **exactly zero** cancellation is only bitwise while both sides
        round identically. Where this file claims bitwise it says which
        arithmetic makes it so.
*/

#include "Controls.h"
#include "Genlock.h"
#include "Shaders.h"
#include "Timing.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace genlock;

namespace
{
int failures = 0;

/// The fragment shader the next Rig compiles instead of the shipped one.
/// Null -- the shipped one -- except inside `--mutation`.
const char* g_fragmentForNextRig = nullptr;

void Check( bool ok, const std::string& what )
{
	std::printf( "  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str() );
	if( !ok )
		++failures;
}

/// A formatted line. EVERY conversion must be a floating-point one: the
/// arguments are doubles, so a %d here reads the wrong half of a register and
/// prints a number with no relation to the measurement -- which is worse than
/// no message, because the check beside it still says ok. Count something with
/// %.0f. The attribute makes the compiler enforce it.
__attribute__( ( format( printf, 1, 0 ) ) ) std::string fmt( const char* format, double a, double b = 0.0, double c = 0.0 )
{
	char buffer[ 256 ];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
	std::snprintf( buffer, sizeof( buffer ), format, a, b, c );
#pragma clang diagnostic pop
	return buffer;
}

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency. Takes rows BOTTOM-UP, as GL hands them back,
// and writes them top-down.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

using Image = std::vector< unsigned char >;

bool writePng( const std::string& path, int width, int height, const Image& bottomUp )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = bottomUp.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Pictures. All of them bottom-up, in GL's orientation, so a measurement's
// y is the shader's y.
//---------------------------------------------------------------------------
struct Rgb
{
	unsigned char r, g, b;
};

unsigned char toByte( float v )
{
	return static_cast< unsigned char >( std::lround( std::min( 1.0f, std::max( 0.0f, v ) ) * 255.0f ) );
}

/// Workbench blue, #0055AA -- the Amiga colour 0 a generation of genlocks
/// keyed on, and this plugin's default Key Colour.
constexpr Rgb kColour0 = { 0x00, 0x55, 0xAA };
constexpr Rgb kFill    = { 0xFF, 0xFF, 0xFF };
constexpr Rgb kVideo   = { 0x00, 0x99, 0x00 };

void put( Image& image, int width, int x, int y, Rgb c, unsigned char a = 255 )
{
	const size_t at = ( static_cast< size_t >( y ) * width + x ) * 4;
	image[ at + 0 ] = c.r;
	image[ at + 1 ] = c.g;
	image[ at + 2 ] = c.b;
	image[ at + 3 ] = a;
}

Rgb pixelAt( const Image& image, int width, int x, int y )
{
	const size_t at = ( static_cast< size_t >( y ) * width + x ) * 4;
	return { image[ at + 0 ], image[ at + 1 ], image[ at + 2 ] };
}

Image flatField( int width, int height, Rgb c )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
			put( image, width, x, y, c );
	return image;
}

/// The edge card: colour 0 on the left, fill on the right, with the
/// transition ramped over `kEdgeRampAmigaPx` Amiga pixels so the key is a
/// ramp rather than a step and a sub-texel crossing can be recovered from it.
///
/// The ramp is stated in AMIGA pixels, not output pixels, so it is the same
/// fraction of the picture at every raster -- which is what makes `--delay`
/// and `--crawl` mean the same thing at 640 wide and at 1280.
constexpr double kEdgeRampAmigaPx = 2.0;

Image edgeCard( int width, int height, double edgeFraction )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	const double edgePx = edgeFraction * width;
	const double ramp   = kEdgeRampAmigaPx * ( static_cast< double >( width ) / kAmigaLoresWidth );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double t = std::clamp( ( ( x + 0.5 ) - ( edgePx - ramp * 0.5 ) ) / ramp, 0.0, 1.0 );
			const Rgb c    = { static_cast< unsigned char >( std::lround( kColour0.r + t * ( kFill.r - kColour0.r ) ) ),
                            static_cast< unsigned char >( std::lround( kColour0.g + t * ( kFill.g - kColour0.g ) ) ),
                            static_cast< unsigned char >( std::lround( kColour0.b + t * ( kFill.b - kColour0.b ) ) ) };
			put( image, width, x, y, c );
		}
	return image;
}

/// A full-width bar of fill on colour 0, with hard top and bottom edges, for
/// the roll. Hard on purpose: a binary mask translated by a whole number of
/// rows is translated exactly, which is what `--roll` is entitled to assert.
/// The bar's true centre row, in pixel-centre coordinates. Taken from the
/// generator's own rounding rather than from `centreFraction * height`: at
/// 270 rows those differ by half a row, which is exactly the size of the
/// error the roll check is looking for.
double barCentreRow( int height, double centreFraction, double heightFraction )
{
	const double lo = std::lround( ( centreFraction - heightFraction * 0.5 ) * height );
	const double hi = std::lround( ( centreFraction + heightFraction * 0.5 ) * height );
	return 0.5 * ( lo + hi ) - 0.5;
}

Image barCard( int width, int height, double centreFraction, double heightFraction )
{
	Image image = flatField( width, height, kColour0 );
	const int lo = static_cast< int >( std::lround( ( centreFraction - heightFraction * 0.5 ) * height ) );
	const int hi = static_cast< int >( std::lround( ( centreFraction + heightFraction * 0.5 ) * height ) );
	for( int y = std::max( 0, lo ); y < std::min( height, hi ); ++y )
		for( int x = 0; x < width; ++x )
			put( image, width, x, y, kFill );
	return image;
}

/// Four quadrants of flat colour and one marker square, for `--mixer`. The
/// marker sits in the bottom-left quadrant so the other three can be
/// measured as flat fields, and its bounds are TEXEL ALIGNED and returned,
/// so the expected position carries no rounding of the generator's own.
struct QuadCard
{
	Image image;
	Rgb quadrant[ 4 ];///< 0 bottom-left (holds the marker), 1 BR, 2 TL, 3 TR
	Rgb marker;
	double markerCentreU = 0.0;
	double markerCentreV = 0.0;
};

QuadCard quadCard( int width, int height, bool srcSide )
{
	QuadCard card;
	if( srcSide )
	{
		//Deliberately NOT magenta or purple. The texture padding is magenta,
		//and a card colour near it would make "did any padding leak?" a
		//question about a rounding rather than about a leak. Every colour
		//here is at least 270 away from the sentinel in the sum of absolute
		//channel differences -- see kSentinelNear.
		card.quadrant[ 0 ] = { 0x00, 0xB4, 0xB4 };//teal
		card.quadrant[ 1 ] = { 0xB4, 0xB4, 0x00 };//olive
		card.quadrant[ 2 ] = { 0x3C, 0x78, 0xFF };//steel
		card.quadrant[ 3 ] = { 0x60, 0x60, 0x60 };//grey
		card.marker        = { 0xA0, 0xFF, 0x00 };//lime
	}
	else
	{
		card.quadrant[ 0 ] = { 0xC8, 0x00, 0x00 };//red
		card.quadrant[ 1 ] = { 0x00, 0xC8, 0x00 };//green
		card.quadrant[ 2 ] = { 0x00, 0x00, 0xC8 };//blue
		card.quadrant[ 3 ] = { 0xF0, 0xE0, 0xC0 };//cream -- not white, which sat exactly on the sentinel limit
		card.marker        = { 0xFF, 0x80, 0x00 };//orange
	}

	card.image = Image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const int q = ( y >= height / 2 ? 2 : 0 ) + ( x >= width / 2 ? 1 : 0 );
			put( card.image, width, x, y, card.quadrant[ q ] );
		}

	//An eighth of the picture across, centred on the middle of the
	//bottom-left quadrant.
	const int x0 = static_cast< int >( std::lround( 0.1875 * width ) );
	const int x1 = static_cast< int >( std::lround( 0.3125 * width ) );
	const int y0 = static_cast< int >( std::lround( 0.1875 * height ) );
	const int y1 = static_cast< int >( std::lround( 0.3125 * height ) );
	for( int y = y0; y < y1; ++y )
		for( int x = x0; x < x1; ++x )
			put( card.image, width, x, y, card.marker );

	card.markerCentreU = ( 0.5 * ( x0 + x1 ) ) / static_cast< double >( width );
	card.markerCentreV = ( 0.5 * ( y0 + y1 ) ) / static_cast< double >( height );
	return card;
}

/// HSV to RGB, for the video card's hue sweep.
void hsv( float h, float s, float v, float& r, float& g, float& b )
{
	const float c    = v * s;
	const float x    = c * ( 1.0f - std::fabs( std::fmod( h * 6.0f, 2.0f ) - 1.0f ) );
	const float m    = v - c;
	float rr = 0, gg = 0, bb = 0;
	const int sector = static_cast< int >( h * 6.0f ) % 6;
	switch( sector )
	{
	case 0: rr = c; gg = x; break;
	case 1: rr = x; gg = c; break;
	case 2: gg = c; bb = x; break;
	case 3: gg = x; bb = c; break;
	case 4: rr = x; bb = c; break;
	default: rr = c; bb = x; break;
	}
	r = rr + m;
	g = gg + m;
	b = bb + m;
}

/// The incoming video: bars across the top, a grey ramp, and a hue field, so
/// whatever the key lets through has something to be.
Image videoCard( int width, int height )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const float u = ( x + 0.5f ) / width;
			const float v = ( y + 0.5f ) / height;//0 at the bottom
			float r = 0, g = 0, b = 0;

			if( v > 0.72f )
			{
				static const float bars[ 7 ][ 3 ] = {
					{ 0.75f, 0.75f, 0.75f }, { 0.75f, 0.75f, 0.0f }, { 0.0f, 0.75f, 0.75f }, { 0.0f, 0.75f, 0.0f },
					{ 0.75f, 0.0f, 0.75f }, { 0.75f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.75f },
				};
				const int which = std::min( 6, static_cast< int >( u * 7.0f ) );
				r = bars[ which ][ 0 ];
				g = bars[ which ][ 1 ];
				b = bars[ which ][ 2 ];
			}
			else if( v > 0.58f )
			{
				r = g = b = u;
			}
			else
			{
				hsv( u, 0.8f, 0.3f + 0.65f * ( v / 0.58f ), r, g, b );
			}

			put( image, width, x, y, { toByte( r ), toByte( g ), toByte( b ) } );
		}
	return image;
}

/// The computer's picture: colour 0, a window with a hard-edged title bar
/// and blocks, and one soft gradient so Softness has something to bite on.
Image amigaCard( int width, int height )
{
	Image image = flatField( width, height, kColour0 );
	const auto px = [ & ]( double f, int span ) { return static_cast< int >( std::lround( f * span ) ); };

	//A window.
	for( int y = px( 0.18, height ); y < px( 0.78, height ); ++y )
		for( int x = px( 0.12, width ); x < px( 0.62, width ); ++x )
			put( image, width, x, y, { 0xE8, 0xE8, 0xE8 } );
	//Its title bar.
	for( int y = px( 0.70, height ); y < px( 0.78, height ); ++y )
		for( int x = px( 0.12, width ); x < px( 0.62, width ); ++x )
			put( image, width, x, y, { 0x00, 0x00, 0x20 } );
	//Blocks in it.
	for( int block = 0; block < 4; ++block )
	{
		const Rgb c[ 4 ] = { { 0xFF, 0x88, 0x00 }, { 0xFF, 0xFF, 0xFF }, { 0x00, 0x00, 0x20 }, { 0xAA, 0x00, 0x00 } };
		for( int y = px( 0.26, height ); y < px( 0.46, height ); ++y )
			for( int x = px( 0.16 + 0.11 * block, width ); x < px( 0.24 + 0.11 * block, width ); ++x )
				put( image, width, x, y, c[ block ] );
	}
	//A soft gradient from colour 0 out to fill, off to the right: the only
	//thing in this picture whose key edge is not a step.
	for( int y = px( 0.18, height ); y < px( 0.78, height ); ++y )
		for( int x = px( 0.68, width ); x < px( 0.94, width ); ++x )
		{
			const double t = ( x - px( 0.68, width ) ) / static_cast< double >( px( 0.94, width ) - px( 0.68, width ) );
			put( image, width, x, y,
			     { static_cast< unsigned char >( std::lround( kColour0.r + t * ( 255 - kColour0.r ) ) ),
			       static_cast< unsigned char >( std::lround( kColour0.g + t * ( 255 - kColour0.g ) ) ),
			       static_cast< unsigned char >( std::lround( kColour0.b + t * ( 255 - kColour0.b ) ) ) } );
		}
	return image;
}

Image generate( const std::string& name, int width, int height )
{
	if( name == "video" )
		return videoCard( width, height );
	if( name == "amiga" )
		return amigaCard( width, height );
	if( name == "quads-a" )
		return quadCard( width, height, false ).image;
	if( name == "quads-b" )
		return quadCard( width, height, true ).image;
	if( name == "edge" )
		return edgeCard( width, height, 0.5 );
	if( name == "bar" )
		return barCard( width, height, 0.5, 0.1 );
	if( name == "colour0" )
		return flatField( width, height, kColour0 );
	return flatField( width, height, kVideo );//"flat"
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without
	//a GPU, where it will at least prove the shaders compile.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const unsigned char* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
// The rig: the plugin, TWO input textures, and an output framebuffer, each
// at its own size.
//
// `hardware` is deliberately allowed to exceed `used`. That is the case
// MaxUV exists for, it is the case a mixer gets wrong silently, and the
// padding is filled with a sentinel colour that appears nowhere else so
// `--mixer` can say whether any of it leaked into the picture.
//---------------------------------------------------------------------------
constexpr Rgb kSentinel = { 0xFF, 0x00, 0xFF };

/// How close to the sentinel a pixel has to be, in the sum of absolute
/// channel differences, before it counts as texture padding.
///
/// Not a taste. The nearest legitimate card colour is 270 away by that
/// measure (see quadCard), so half of that means "more padding than
/// picture" -- a pixel that is majority sentinel. It cannot be reached by
/// any blend of two card colours, and a whole leaked pixel scores zero. The
/// first version of this test used a per-channel box, which sat 55 away
/// from a magenta quadrant of the card itself.
constexpr int kSentinelNear = 135;

int l1( Rgb a, Rgb b )
{
	return std::abs( a.r - b.r ) + std::abs( a.g - b.g ) + std::abs( a.b - b.b );
}

struct InputSpec
{
	int usedW = 0, usedH = 0;
	int hwW = 0, hwH = 0;

	static InputSpec Exact( int w, int h )
	{
		return { w, h, w, h };
	}
	static InputSpec Padded( int w, int h, int hw, int hh )
	{
		return { w, h, hw, hh };
	}
};

struct Rig
{
	Genlock plugin;
	int width = 0, height = 0;
	InputSpec destSpec, srcSpec;

	GLuint destTexture = 0, srcTexture = 0;
	GLuint outputTexture = 0, outputFBO = 0;
	FFGLTextureStruct destStruct = {}, srcStruct = {};
	FFGLTextureStruct* inputs[ 2 ] = { nullptr, nullptr };
	ProcessOpenGLStruct process    = {};
	bool ready                     = false;

	bool Init( int outW, int outH, InputSpec dest, InputSpec src )
	{
		width    = outW;
		height   = outH;
		destSpec = dest;
		srcSpec  = src;

		if( g_fragmentForNextRig != nullptr )
			plugin.SetFragmentShaderForTest( g_fragmentForNextRig );

		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( outW );
		viewport.height             = static_cast< FFUInt32 >( outH );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for the shader\n" );
			return false;
		}
		plugin.SetClockScaleForTest( 1.0 );

		destTexture = makeTexture( dest.hwW, dest.hwH, flatField( dest.hwW, dest.hwH, kSentinel ).data() );
		srcTexture  = makeTexture( src.hwW, src.hwH, flatField( src.hwW, src.hwH, kSentinel ).data() );

		outputTexture = makeTexture( outW, outH, nullptr );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );

		destStruct.Width          = static_cast< FFUInt32 >( dest.usedW );
		destStruct.Height         = static_cast< FFUInt32 >( dest.usedH );
		destStruct.HardwareWidth  = static_cast< FFUInt32 >( dest.hwW );
		destStruct.HardwareHeight = static_cast< FFUInt32 >( dest.hwH );
		destStruct.Handle         = destTexture;

		srcStruct.Width          = static_cast< FFUInt32 >( src.usedW );
		srcStruct.Height         = static_cast< FFUInt32 >( src.usedH );
		srcStruct.HardwareWidth  = static_cast< FFUInt32 >( src.hwW );
		srcStruct.HardwareHeight = static_cast< FFUInt32 >( src.hwH );
		srcStruct.Handle         = srcTexture;

		inputs[ 0 ]              = &destStruct;
		inputs[ 1 ]              = &srcStruct;
		process.numInputTextures = 2;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
		ready                    = true;
		return true;
	}

	bool Init( int outW, int outH )
	{
		return Init( outW, outH, InputSpec::Exact( outW, outH ), InputSpec::Exact( outW, outH ) );
	}

	void UploadDest( const Image& bottomUp )
	{
		glBindTexture( GL_TEXTURE_2D, destTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, destSpec.usedW, destSpec.usedH, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void UploadSrc( const Image& bottomUp )
	{
		glBindTexture( GL_TEXTURE_2D, srcTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, srcSpec.usedW, srcSpec.usedH, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	bool Set( const std::string& name, float value )
	{
		for( unsigned int i = 0; i < Genlock::PT_COUNT; ++i )
		{
			const char* declared = plugin.GetParamName( i );
			if( declared != nullptr && name == declared )
			{
				plugin.SetFloatParameter( i, value );
				return true;
			}
		}
		std::fprintf( stderr, "no parameter called '%s'\n", name.c_str() );
		return false;
	}

	/// Drive the plugin's clock. Synthetic, and it has to be: left to the
	/// wall clock the harness renders a hundred frames in a few
	/// milliseconds, so no time passes, the crawl never crawls, and no two
	/// runs produce the same picture.
	bool Render( int frame, double fps = 60.0 )
	{
		plugin.SetClockScaleForTest( 1.0 );//seconds, said out loud rather than inferred
		plugin.SetTime( static_cast< double >( frame ) / fps );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
	}

	bool RenderFrames( int frames, double fps = 60.0 )
	{
		for( int frame = 0; frame < frames; ++frame )
			if( !Render( frame, fps ) )
				return false;
		return true;
	}

	/// ProcessOpenGL with the input block deliberately broken, for the
	/// guards. `nullIndex` of -2 nulls the whole array. Restores the rig
	/// afterwards.
	FFResult RenderBroken( int numInputs, int nullIndex )
	{
		FFGLTextureStruct* saved[ 2 ] = { inputs[ 0 ], inputs[ 1 ] };
		if( nullIndex >= 0 && nullIndex < 2 )
			inputs[ nullIndex ] = nullptr;
		if( nullIndex == -2 )
			process.inputTextures = nullptr;
		process.numInputTextures = static_cast< FFUInt32 >( numInputs );

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		const FFResult result = plugin.ProcessOpenGL( &process );

		inputs[ 0 ]              = saved[ 0 ];
		inputs[ 1 ]              = saved[ 1 ];
		process.inputTextures    = inputs;
		process.numInputTextures = 2;
		return result;
	}

	Image Pixels()
	{
		Image pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return pixels;
	}

	~Rig()
	{
		if( !ready )
			return;
		plugin.DeInitGL();
		glDeleteFramebuffers( 1, &outputFBO );
		glDeleteTextures( 1, &outputTexture );
		glDeleteTextures( 1, &srcTexture );
		glDeleteTextures( 1, &destTexture );
	}
};

//---------------------------------------------------------------------------
// Measurement.
//---------------------------------------------------------------------------

/// The key, recovered from one row of the output.
///
/// The shader writes `mix( fill, video, k )`. Where the fill is a known flat
/// colour -- which is true everywhere to the left of the card's own edge --
/// that inverts exactly: k = ( out - fill ) / ( video - fill ), read off the
/// channel where the two differ most so the 8-bit quantisation costs the
/// least.
std::vector< double > keyAcrossRow( const Image& image, int width, int row, Rgb fill, Rgb video )
{
	const int span[ 3 ] = { video.r - fill.r, video.g - fill.g, video.b - fill.b };
	int channel         = 0;
	for( int c = 1; c < 3; ++c )
		if( std::abs( span[ c ] ) > std::abs( span[ channel ] ) )
			channel = c;

	const double base  = ( channel == 0 ? fill.r : channel == 1 ? fill.g : fill.b );
	const double scale = span[ channel ];

	std::vector< double > key( static_cast< size_t >( width ) );
	for( int x = 0; x < width; ++x )
	{
		const size_t at = ( static_cast< size_t >( row ) * width + x ) * 4 + static_cast< size_t >( channel );
		key[ static_cast< size_t >( x ) ] = ( static_cast< double >( image[ at ] ) - base ) / scale;
	}
	return key;
}

/// Where a falling edge sits, by INTEGRATING it rather than interpolating
/// across it.
///
/// The key is 1 to the left of the edge, 0 to the right, and its transition
/// is symmetric about the edge -- the card's colour ramp is linear and
/// `smoothstep` is odd about its own midpoint. So the sum of the key over a
/// window that contains the whole transition IS the edge's distance from the
/// window's left side, to sub-texel precision.
///
/// This is deliberately not a 50% crossing found by linear interpolation.
/// Interpolating a linear functional is exact under translation; interpolating
/// across a smooth S-curve is not, and its bias changes with where the sample
/// grid happens to fall relative to the edge -- which is exactly the
/// difference between a whole-pixel offset and a fractional one, and so
/// exactly the thing being measured. Integration has no such bias: every
/// column contributes once, whatever the curve does in between.
///
/// Returns a negative number if the transition is not wholly inside the
/// window, because then the sum means nothing.
double edgeByIntegral( const std::vector< double >& v, int x0, int x1 )
{
	if( v[ static_cast< size_t >( x0 ) ] < 0.98 || v[ static_cast< size_t >( x1 ) ] > 0.02 )
		return -1.0;
	double sum = 0.0;
	for( int x = x0; x <= x1; ++x )
		sum += std::clamp( v[ static_cast< size_t >( x ) ], 0.0, 1.0 );
	return x0 + sum;
}

/// The circular centroid of a horizontal band, in pixel-centre coordinates.
///
/// Circular because a rolling picture wraps, and the arithmetic mean of a
/// band that straddles the wrap is the one place in the middle where the
/// band is not.
///
/// WEIGHTED, not thresholded, and that is the whole point. A hard threshold
/// quantises the answer to whole rows, so a roll that advances 4.5 rows per
/// frame -- which is what 1 roll/s at 270 rows and 60 fps is -- comes back
/// snapped to 4 or 5 and the check fails at one raster while passing at
/// another. The first version of this function did exactly that. Weighting
/// each row by how much of the band it holds recovers the fraction, and the
/// weighted vector sum rotates EXACTLY when the picture rotates, whatever
/// shape the band's edges are.
double circularBandRow( const Image& image, int width, int height, int channel )
{
	double sx = 0.0, sy = 0.0, total = 0.0;
	for( int y = 0; y < height; ++y )
	{
		double row = 0.0;
		for( int x = 0; x < width; ++x )
			row += image[ ( static_cast< size_t >( y ) * width + x ) * 4 + static_cast< size_t >( channel ) ];
		row /= 255.0 * width;

		const double angle = 2.0 * M_PI * ( y + 0.5 ) / height;
		sx += row * std::cos( angle );
		sy += row * std::sin( angle );
		total += row;
	}
	if( total <= 0.0 )
		return -1.0;
	double angle = std::atan2( sy, sx );
	if( angle < 0.0 )
		angle += 2.0 * M_PI;
	return angle / ( 2.0 * M_PI ) * height - 0.5;
}

/// True when `later` is `rows` rows of `first` rotated upward, byte for byte.
bool isRowRotationOf( const Image& later, const Image& first, int width, int height, int rows )
{
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
	{
		const int from = ( ( y + rows ) % height + height ) % height;
		if( std::memcmp( later.data() + static_cast< size_t >( y ) * stride,
		                 first.data() + static_cast< size_t >( from ) * stride, stride )
		    != 0 )
			return false;
	}
	return true;
}

/// The shortest distance between two rows on a cylinder of `height` rows.
double rowDistance( double a, double b, int height )
{
	double d = std::fmod( a - b, static_cast< double >( height ) );
	if( d < 0.0 )
		d += height;
	return std::min( d, height - d );
}

/// The largest per-byte difference between two images of the same size.
int maxByteDifference( const Image& a, const Image& b )
{
	int worst = 0;
	for( size_t i = 0; i < a.size() && i < b.size(); ++i )
		worst = std::max( worst, std::abs( static_cast< int >( a[ i ] ) - static_cast< int >( b[ i ] ) ) );
	return worst;
}

/// Mean colour over a rectangle, in 0..255.
void meanOver( const Image& image, int width, int x0, int y0, int x1, int y1, double out[ 3 ] )
{
	double sum[ 3 ] = { 0, 0, 0 };
	long n          = 0;
	for( int y = y0; y < y1; ++y )
		for( int x = x0; x < x1; ++x )
		{
			const size_t at = ( static_cast< size_t >( y ) * width + x ) * 4;
			sum[ 0 ] += image[ at + 0 ];
			sum[ 1 ] += image[ at + 1 ];
			sum[ 2 ] += image[ at + 2 ];
			++n;
		}
	for( int c = 0; c < 3; ++c )
		out[ c ] = n > 0 ? sum[ c ] / n : -1.0;
}

//---------------------------------------------------------------------------
// --list, --names
//---------------------------------------------------------------------------
int runList()
{
	Genlock plugin;
	std::printf( "%-4s %-22s %-9s %10s   %-16s %s\n", "id", "name", "kind", "value", "range", "means" );
	for( unsigned int id = 0; id < Genlock::PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( id >= Genlock::PT_ABOUT_FIRST )
		{
			std::printf( "%-4u %-22s %-9s %10s   %-16s %s\n", id, name ? name : "", "about", "-", "-",
			             "the Stoatworks About block; not swept" );
			continue;
		}
		const char* kind = "standard";
		switch( plugin.GetParamType( id ) )
		{
		case FF_TYPE_BOOLEAN: kind = "boolean"; break;
		case FF_TYPE_EVENT: kind = "event"; break;
		case FF_TYPE_INTEGER: kind = "integer"; break;
		case FF_TYPE_OPTION: kind = "option"; break;
		case FF_TYPE_BUFFER: kind = "buffer"; break;
		case FF_TYPE_TEXT: kind = "text"; break;
		case FF_TYPE_RED:
		case FF_TYPE_GREEN:
		case FF_TYPE_BLUE: kind = "colour"; break;
		default: break;
		}
		RangeStruct range = plugin.GetParamRange( id );
		if( plugin.GetParamType( id ) == FF_TYPE_OPTION )
		{
			range.min = 0.0f;
			range.max = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( id ) ) ) - 1.0f;
		}
		char rangeText[ 32 ] = {};
		std::snprintf( rangeText, sizeof( rangeText ), "[%g .. %g]", range.min, range.max );
		std::printf( "%-4u %-22s %-9s %10.4f   %-16s\n", id, name ? name : "", kind, plugin.GetFloatParameter( id ), rangeText );
	}
	return 0;
}

int runNames()
{
	Genlock plugin;
	std::printf( "names longer than FFGL's 16 characters:\n\n" );
	int over = 0;
	for( unsigned int id = 0; id < Genlock::PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name != nullptr && std::strlen( name ) > 16 )
		{
			std::printf( "  %-3u  %-28s %zu\n", id, name, std::strlen( name ) );
			++over;
		}
		for( unsigned int e = 0; e < plugin.GetNumParamElements( id ); ++e )
		{
			const char* el = plugin.GetParamElementName( id, e );
			if( el != nullptr && std::strlen( el ) > 16 )
			{
				std::printf( "  %-3u  %-28s element %u: %s (%zu)\n", id, name, e, el, std::strlen( el ) );
				++over;
			}
		}
	}
	std::printf( "\n  %d over the limit\n", over );

	//And no two alike. `--set` and the sweep find a parameter by name and
	//take the FIRST match, so a duplicate makes the second one unreachable
	//from every tool in this repo -- and a host that keys saved values by
	//name would do the same to an operator's composition.
	int duplicates = 0;
	for( unsigned int a = 0; a < Genlock::PT_COUNT; ++a )
		for( unsigned int b = a + 1; b < Genlock::PT_COUNT; ++b )
		{
			const char* na = plugin.GetParamName( a );
			const char* nb = plugin.GetParamName( b );
			if( na != nullptr && nb != nullptr && std::strcmp( na, nb ) == 0 )
			{
				std::printf( "  %u and %u are both called \"%s\"\n", a, b, na );
				++duplicates;
			}
		}
	std::printf( "  %d duplicate names\n", duplicates );
	return over == 0 && duplicates == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --mixer
//
// Nothing else in the fleet has ever asserted any of this, because nothing
// else in the fleet has two inputs.
//---------------------------------------------------------------------------

/// Turn the key off entirely, whatever the picture is: an alpha key on an
/// opaque input, with nothing keyed. Leaves the output equal to the fill.
void keyOffEverywhere( Rig& rig )
{
	rig.Set( "Key Source", 2.0f );//Alpha
	rig.Set( "Tolerance", 0.0f );
	rig.Set( "Softness", 0.0f );
	rig.Set( "Invert", 0.0f );
	rig.Set( "Fringe", 0.0f );
	rig.Set( "Key Delay", 0.5f );//no displacement
	rig.Set( "Crawl Rate", 0.0f );
	rig.Set( "Fader", 1.0f );//Overlay
	rig.Set( "Opacity", 1.0f );
}

int runMixer()
{
	std::printf( "a mixer takes two inputs, of two sizes, with two MaxUVs\n\n" );

	//-----------------------------------------------------------------
	// The guards. Resolume calls a mixer with one input while the operator
	// is still patching, and may hand over a null for either.
	//-----------------------------------------------------------------
	{
		Rig rig;
		if( !rig.Init( 320, 200 ) )
			return 1;
		rig.UploadDest( videoCard( 320, 200 ) );
		rig.UploadSrc( amigaCard( 320, 200 ) );

		Check( rig.RenderBroken( 2, -2 ) == FF_FAIL, "a null input ARRAY returns FF_FAIL" );
		Check( rig.RenderBroken( 0, -1 ) == FF_FAIL, "zero input textures returns FF_FAIL" );
		Check( rig.RenderBroken( 1, -1 ) == FF_FAIL, "one input texture returns FF_FAIL" );
		Check( rig.RenderBroken( 2, 0 ) == FF_FAIL, "a null Dest returns FF_FAIL" );
		Check( rig.RenderBroken( 2, 1 ) == FF_FAIL, "a null Src returns FF_FAIL" );
		Check( rig.Render( 0 ), "and two real inputs still render afterwards" );
	}

	//-----------------------------------------------------------------
	// Two inputs, two sizes, two hardware paddings, and an output that is
	// a third size again. The two Width/HardwareWidth ratios differ in both
	// axes on purpose: a plugin that used one input's MaxUV for the other
	// would pass a test where they happened to match.
	//-----------------------------------------------------------------
	const int outW = 320, outH = 200;
	const InputSpec dest = InputSpec::Padded( 200, 120, 256, 256 );
	const InputSpec src  = InputSpec::Padded( 96, 70, 128, 128 );

	const QuadCard destCard = quadCard( dest.usedW, dest.usedH, false );
	const QuadCard srcCard  = quadCard( src.usedW, src.usedH, true );

	std::printf( "  Dest %dx%d used of %dx%d  (MaxUV %.5f, %.5f)\n", dest.usedW, dest.usedH, dest.hwW, dest.hwH,
	             static_cast< double >( dest.usedW ) / dest.hwW, static_cast< double >( dest.usedH ) / dest.hwH );
	std::printf( "  Src  %dx%d used of %dx%d  (MaxUV %.5f, %.5f)\n", src.usedW, src.usedH, src.hwW, src.hwH,
	             static_cast< double >( src.usedW ) / src.hwW, static_cast< double >( src.usedH ) / src.hwH );
	std::printf( "  out  %dx%d\n\n", outW, outH );

	Rig rig;
	if( !rig.Init( outW, outH, dest, src ) )
		return 1;
	rig.UploadDest( destCard.image );
	rig.UploadSrc( srcCard.image );

	struct Side
	{
		const char* name;
		float fader;
		const QuadCard* card;
		int usedW, usedH;
	};
	const Side sides[ 2 ] = {
		{ "Dest", 0.0f, &destCard, dest.usedW, dest.usedH },//Fader = Video
		{ "Src", 1.0f, &srcCard, src.usedW, src.usedH }     //Fader = Overlay, key off
	};

	for( const Side& side : sides )
	{
		keyOffEverywhere( rig );
		rig.Set( "Fader", side.fader );
		if( !rig.Render( 0 ) )
		{
			Check( false, std::string( side.name ) + ": ProcessOpenGL failed" );
			continue;
		}
		const Image out = rig.Pixels();

		//1. No sentinel. The padding around the used area of both inputs is
		//   magenta and appears in no card. One pixel of it in the output
		//   means a MaxUV or a half-texel inset is wrong.
		int sentinelPixels = 0;
		int nearestCard    = 1000;
		for( int q = 0; q < 4; ++q )
			nearestCard = std::min( nearestCard, l1( side.card->quadrant[ q ], kSentinel ) );
		nearestCard = std::min( nearestCard, l1( side.card->marker, kSentinel ) );
		for( int y = 0; y < outH; ++y )
			for( int x = 0; x < outW; ++x )
				if( l1( pixelAt( out, outW, x, y ), kSentinel ) < kSentinelNear )
					++sentinelPixels;
		Check( nearestCard >= 2 * kSentinelNear,
		       std::string( side.name ) + fmt( ": no card colour is within %.0f of the padding", nearestCard )
		           + fmt( " (needs %.0f)", 2.0 * kSentinelNear ) );
		Check( sentinelPixels == 0,
		       std::string( side.name ) + ": no texture padding reached the picture"
		           + fmt( " (%.0f sentinel pixels)", sentinelPixels ) );

		//2. The three marker-free quadrants are flat and the right colour.
		//   Inset by one SOURCE texel expressed in output pixels, plus one
		//   output pixel: that is the whole width a quadrant boundary can
		//   smear over under bilinear resampling, and it is stated in terms
		//   of the two rasters rather than measured.
		const int insetX = static_cast< int >( std::ceil( static_cast< double >( outW ) / side.usedW ) ) + 1;
		const int insetY = static_cast< int >( std::ceil( static_cast< double >( outH ) / side.usedH ) ) + 1;

		double worst = 0.0;
		for( int q = 1; q < 4; ++q )
		{
			const int qx = ( q & 1 ) ? outW / 2 : 0;
			const int qy = ( q & 2 ) ? outH / 2 : 0;
			double mean[ 3 ];
			meanOver( out, outW, qx + insetX, qy + insetY, qx + outW / 2 - insetX, qy + outH / 2 - insetY, mean );
			const Rgb want = side.card->quadrant[ q ];
			worst          = std::max( worst, std::fabs( mean[ 0 ] - want.r ) );
			worst          = std::max( worst, std::fabs( mean[ 1 ] - want.g ) );
			worst          = std::max( worst, std::fabs( mean[ 2 ] - want.b ) );
		}
		//One 8-bit code: the smallest difference the readback can express.
		//A wrong MaxUV moves a quadrant boundary by tens of pixels and
		//would show up here as tens of codes, not one.
		Check( worst <= 1.0, std::string( side.name ) + ": each quadrant is its own flat colour"
		                         + fmt( " (worst %.3f of 255, tolerance 1)", worst ) );

		//3. A known pixel lands where it belongs. The marker's centroid,
		//   against the normalised centre the generator actually used.
		double sx = 0.0, sy = 0.0;
		long n    = 0;
		for( int y = 0; y < outH / 2; ++y )
			for( int x = 0; x < outW / 2; ++x )
			{
				//Half way from the marker to the only other colour in this
				//quadrant: a pixel counts when it is more marker than
				//background, which for a symmetric box leaves the centroid
				//symmetric too.
				const int half = l1( side.card->marker, side.card->quadrant[ 0 ] ) / 2;
				const Rgb p    = pixelAt( out, outW, x, y );
				if( l1( p, side.card->marker ) < half )
				{
					sx += x + 0.5;
					sy += y + 0.5;
					++n;
				}
			}
		const double gotU  = n > 0 ? sx / n / outW : -1.0;
		const double gotV  = n > 0 ? sy / n / outH : -1.0;
		//One source texel, expressed in normalised picture units. That is
		//the quantum the marker's own edges are drawn on; anything finer
		//would be asserting something about the filter, and anything
		//coarser would miss a swapped MaxUV.
		const double tolU = 1.0 / side.usedW;
		const double tolV = 1.0 / side.usedH;
		Check( n > 0 && std::fabs( gotU - side.card->markerCentreU ) <= tolU
		           && std::fabs( gotV - side.card->markerCentreV ) <= tolV,
		       std::string( side.name ) + ": the marker is where it was put"
		           + fmt( " (%.4f, %.4f", gotU, gotV )
		           + fmt( " vs %.4f, %.4f", side.card->markerCentreU, side.card->markerCentreV )
		           + fmt( "; tolerance %.4f, %.4f)", tolU, tolV ) );
	}

	std::printf( "\n  Two inputs at different sizes, each with its own MaxUV, resolved\n"
	             "  independently to a third raster.\n" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --delay
//
// Two tolerances, and they are not interchangeable.
//---------------------------------------------------------------------------

/// The card, the colours and the settings every timing check shares.
void setUpTimingRig( Rig& rig, int width, int height, double edgeFraction )
{
	rig.UploadDest( flatField( width, height, kVideo ) );
	rig.UploadSrc( edgeCard( width, height, edgeFraction ) );

	rig.Set( "Key Source", 0.0f );//Colour 0
	rig.Set( "Key Colour Red", kColour0.r / 255.0f );
	rig.Set( "Key Colour Green", kColour0.g / 255.0f );
	rig.Set( "Key Colour Blue", kColour0.b / 255.0f );
	rig.Set( "Tolerance", 0.10f );
	//Softness has to be NON-ZERO here, and that is not a convenience.
	//
	//At zero the key is a step in colour distance, so however finely the
	//delay is displaced the key still snaps to whichever side of the
	//threshold the pixel centre lands on: every measurement comes back a
	//whole number of texels and a fractional delay is indistinguishable
	//from the nearest whole one. The first version of this check did that
	//and reported a 0.5-texel delay as 1.0 texels. A ramped key is what
	//turns the sub-texel part into partial coverage that can be recovered.
	rig.Set( "Softness", 0.9f );//0.45 of the distance range
	rig.Set( "Invert", 0.0f );
	rig.Set( "Fringe", 0.0f );
	rig.Set( "Crawl Rate", 0.0f );//no crawl unless a check asks for one
	rig.Set( "Sync Quality", 1.0f );
	rig.Set( "Fader", 1.0f );//Overlay
	rig.Set( "Opacity", 1.0f );
}

/// The 0..1 parameter that means `amiga` Amiga pixels of key delay.
float delayParam( double amigaPixels )
{
	return static_cast< float >( 0.5 + amigaPixels / 16.0 );
}

int runDelay()
{
	const int width = 640, height = 360;
	const double edgeFraction = 0.5;
	const double pxPerAmiga   = static_cast< double >( width ) / kAmigaLoresWidth;//2 at this raster, in lores -- the default mode
	const double edgePx       = edgeFraction * width;
	const double rampPx       = kEdgeRampAmigaPx * pxPerAmiga;
	const double rampStart    = edgePx - rampPx * 0.5;
	const double rampEnd      = edgePx + rampPx * 0.5;

	//Three delays, in Amiga pixels. The first two are a WHOLE number of
	//output texels apart at this raster; the third is half a texel from the
	//first. Every one of them is inside the control's own -8..+8 range.
	const double delays[ 3 ] = { -6.0, -8.0, -5.75 };

	std::printf( "the key lags the fill by the stated delay\n\n" );
	std::printf( "  raster %dx%d, so one Amiga pixel is %.3f output texels\n", width, height, pxPerAmiga );
	std::printf( "  the card's edge is at column %.1f, ramping over %.1f texels\n", edgePx, rampPx );
	std::printf( "  delays %.2f, %.2f, %.2f Amiga px = %.1f, %.1f, %.1f output texels\n\n",
	             delays[ 0 ], delays[ 1 ], delays[ 2 ],
	             delays[ 0 ] * pxPerAmiga, delays[ 1 ] * pxPerAmiga, delays[ 2 ] * pxPerAmiga );

	Rig rig;
	if( !rig.Init( width, height ) )
		return 1;
	setUpTimingRig( rig, width, height, edgeFraction );

	Image frame[ 3 ];
	for( int i = 0; i < 3; ++i )
	{
		rig.Set( "Key Delay", delayParam( delays[ i ] ) );
		if( !rig.Render( 0 ) )
		{
			Check( false, "ProcessOpenGL failed" );
			return 1;
		}
		frame[ i ] = rig.Pixels();
	}

	const int row = height / 2;

	//Every window below is derived from the raster and the card, not from a
	//column number anybody typed. At a different width they all move.
	const double shortest = std::fabs( delays[ 2 ] ) * pxPerAmiga;//the least the key is displaced
	const double longest  = std::fabs( delays[ 1 ] ) * pxPerAmiga;//the most

	//-----------------------------------------------------------------
	// 1. A WHOLE-pixel change translates the picture exactly.
	//
	// The first two delays are `shift` output texels apart, exactly. To the
	// left of the card's own ramp the fill is a constant, so the output
	// there is a function of the key alone -- and a whole-texel translation
	// of a sampled image is exact on any rasteriser, because every sample
	// still lands on a texel centre and the filter weight is 0/1. So the
	// assertion is not a tolerance at all. It is byte equality.
	//-----------------------------------------------------------------
	const double shiftExact = ( delays[ 0 ] - delays[ 1 ] ) * pxPerAmiga;
	//The byte-equality claim below is ONLY true when the two delays differ
	//by a whole number of output texels, which depends on the raster: two
	//Amiga pixels is four texels at 640 wide and 4.006 at 641. Assert it
	//rather than assume it, or a raster change turns an exact check into a
	//silently wrong one.
	Check( std::fabs( shiftExact - std::lround( shiftExact ) ) < 1e-9,
	       fmt( "the two whole-pixel delays are %.6f output texels apart, a whole number", shiftExact ) );
	const int shift    = static_cast< int >( std::lround( shiftExact ) );
	const int windowHi = static_cast< int >( std::floor( rampStart ) ) - shift - 1;
	const int windowLo = windowHi - 40;

	int differing = 0;
	for( int x = windowLo; x <= windowHi; ++x )
		for( int c = 0; c < 4; ++c )
		{
			const size_t a = ( static_cast< size_t >( row ) * width + x + shift ) * 4 + static_cast< size_t >( c );
			const size_t b = ( static_cast< size_t >( row ) * width + x ) * 4 + static_cast< size_t >( c );
			if( frame[ 0 ][ a ] != frame[ 1 ][ b ] )
				++differing;
		}
	Check( differing == 0,
	       fmt( "a whole-pixel delay translates the key EXACTLY: %.0f of ", differing )
	           + fmt( "%.0f bytes differ over columns ", ( windowHi - windowLo + 1 ) * 4.0 )
	           + fmt( "%.0f..%.0f", windowLo, windowHi ) );

	//-----------------------------------------------------------------
	// 2. A FRACTIONAL delay lands as partial coverage.
	//
	// Half a texel is not a translation. The sub-texel part exists only in
	// the filter's own interpolation across the card's ramp, and GL
	// promises no more than eight bits of subtexel precision for that. So
	// the key is read back off the picture and INTEGRATED -- see
	// edgeByIntegral -- and the edge positions compared.
	//
	// The tolerance is 0.25 output texels, derived rather than fitted:
	//   * the key is recovered from a channel with 170 codes of contrast,
	//     so each column costs at most 0.5/170 = 0.0029 of key value;
	//   * about four columns are partially covered, and the integral adds
	//     their errors: 4 x 0.0029 = 0.012 texels;
	//   * GL's minimum subtexel precision is 8 bits, another 0.0039;
	//   * two edge positions are differenced, so twice all of that:
	//     about 0.03 texels.
	// 0.25 leaves a factor of eight over that bound, and sits a factor of
	// two BELOW the 0.5-texel error a plugin would make if it rounded the
	// fractional part away -- which is the failure this check exists for.
	// Nothing about it depends on the number this machine printed.
	//-----------------------------------------------------------------
	const double keyTolerance = 0.25;
	//And the bound, computed from THIS raster and THIS card rather than
	//asserted once and left. The number of partially covered columns is the
	//ramp width in texels, which grows with the raster -- so a much wider
	//render would need a wider tolerance, and this says so instead of
	//failing mysteriously.
	const double contrast   = std::fabs( static_cast< double >( kVideo.b ) - kColour0.b );
	const double errorBound = 2.0 * ( rampPx * ( 0.5 / contrast ) + 1.0 / 256.0 );
	Check( errorBound * 3.0 <= keyTolerance,
	       fmt( "the derived error bound is %.4f texels, at least three times inside the ", errorBound )
	           + fmt( "%.2f tolerance", keyTolerance ) );
	//The integration window has to contain the whole transition, at every
	//delay: the key must still be 1 at the left edge (so the card is still
	//colour 0 `longest` texels further right) and already 0 at the right
	//(so the card is past its ramp `shortest` texels further right).
	const int measureHi = static_cast< int >( std::floor( rampStart ) ) - 1;
	const int measureLo = measureHi - 80;
	Check( measureHi + shortest > rampEnd && measureLo + longest < rampStart,
	       fmt( "the integration window %.0f..%.0f holds the whole transition at every delay",
	            measureLo, measureHi ) );

	double edge[ 3 ];
	for( int i = 0; i < 3; ++i )
	{
		const std::vector< double > key = keyAcrossRow( frame[ i ], width, row, kColour0, kVideo );
		edge[ i ]                       = edgeByIntegral( key, measureLo, measureHi );
	}

	const bool found = edge[ 0 ] > 0 && edge[ 1 ] > 0 && edge[ 2 ] > 0;
	Check( found, "the key edge is wholly inside the window in all three renders" );
	if( !found )
		return 1;

	const double wantWhole = ( delays[ 1 ] - delays[ 0 ] ) * pxPerAmiga;
	const double gotWhole  = edge[ 1 ] - edge[ 0 ];
	Check( std::fabs( gotWhole - wantWhole ) <= keyTolerance,
	       fmt( "whole-pixel delay measures %.4f texels, predicted %.4f", gotWhole, wantWhole )
	           + fmt( " (within %.2f)", keyTolerance ) );

	const double wantFraction = ( delays[ 2 ] - delays[ 0 ] ) * pxPerAmiga;
	const double gotFraction  = edge[ 2 ] - edge[ 0 ];
	Check( std::fabs( gotFraction - wantFraction ) <= keyTolerance,
	       fmt( "fractional delay measures %.4f texels, predicted %.4f", gotFraction, wantFraction )
	           + fmt( " (within %.2f)", keyTolerance ) );

	//And it really is fractional: half a texel away from a whole-texel
	//position, so a plugin that rounded the delay would fail the check
	//above rather than passing it by luck.
	Check( std::fabs( std::fmod( std::fabs( gotFraction ) + 1.0, 1.0 ) - 0.5 ) <= keyTolerance,
	       fmt( "and it lands half a texel off the lattice (%.4f)", gotFraction ) );

	//-----------------------------------------------------------------
	// 3. The FILL does not move. That is the whole idea: the key is
	// delayed and the fill is not.
	//
	// Two statements, because one alone would not be worth much:
	//
	//   a) past the card's ramp the key is zero at every delay, so the
	//      output is the fill and nothing else -- `mix( fill, video, 0.0 )`
	//      is `fill*1 + video*0`, exactly `fill` in IEEE-754 and still
	//      exact under FMA contraction. Byte equality.
	//   b) with the key forced off everywhere, all three delays render the
	//      SAME FRAME, ramp included. That is the statement that no delay
	//      term reaches the fill fetch at all, and it covers the edge
	//      itself rather than the flat area beside it.
	//-----------------------------------------------------------------
	const int fillLo = static_cast< int >( std::ceil( rampEnd ) ) + 1;
	const int fillHi = width - 4;
	int fillDiffer   = 0;
	for( int x = fillLo; x <= fillHi; ++x )
		for( int c = 0; c < 4; ++c )
		{
			const size_t at = ( static_cast< size_t >( row ) * width + x ) * 4 + static_cast< size_t >( c );
			if( frame[ 1 ][ at ] != frame[ 0 ][ at ] || frame[ 2 ][ at ] != frame[ 0 ][ at ] )
				++fillDiffer;
		}
	Check( fillDiffer == 0,
	       fmt( "past the edge every delay renders the same bytes: %.0f differ", fillDiffer )
	           + fmt( " over columns %.0f..%.0f", fillLo, fillHi ) );

	{
		keyOffEverywhere( rig );
		Image off[ 3 ];
		for( int i = 0; i < 3; ++i )
		{
			rig.Set( "Key Delay", delayParam( delays[ i ] ) );
			if( !rig.Render( 0 ) )
				return 1;
			off[ i ] = rig.Pixels();
		}
		const int worst = std::max( maxByteDifference( off[ 0 ], off[ 1 ] ), maxByteDifference( off[ 0 ], off[ 2 ] ) );
		Check( worst == 0,
		       fmt( "with the key off, all three delays are the same frame (worst byte %.0f)", worst ) );
	}

	//-----------------------------------------------------------------
	// 4. And an ABSOLUTE anchor, because everything above is a difference.
	//
	// Measuring the key's position as a difference between two renders is
	// what makes it independent of where the card's own ramp puts the 50%
	// point -- but it also means a CONSTANT bias on the key fetch, the same
	// at every delay, cancels and goes unseen. (It does: a deliberate
	// third-of-a-texel bias was added to the shader and every check above
	// still passed.)
	//
	// At a Key Delay of exactly zero the key is cut from exactly where the
	// fill is, so the two agree everywhere, the disagreement is exactly
	// zero, and `mix( rgb, tint, Fringe * 0.0 )` returns rgb whatever
	// Fringe is. So: at zero delay the fringe controls must do NOTHING, to
	// the byte. Any constant displacement of the key breaks that.
	//-----------------------------------------------------------------
	setUpTimingRig( rig, width, height, edgeFraction );
	rig.Set( "Key Delay", delayParam( 0.0 ) );
	rig.Set( "Fringe", 0.0f );
	if( !rig.Render( 0 ) )
		return 1;
	const Image noFringe = rig.Pixels();
	rig.Set( "Fringe", 1.0f );
	if( !rig.Render( 0 ) )
		return 1;
	const int fringeAtZero = maxByteDifference( noFringe, rig.Pixels() );
	Check( fringeAtZero == 0,
	       fmt( "at zero delay the key and the fill agree, so Fringe has nothing to "
	            "colour (worst byte %.0f)", fringeAtZero ) );

	//And the same control at a real delay must plainly do something, or the
	//check above would pass on a plugin whose fringe was simply dead.
	rig.Set( "Key Delay", delayParam( delays[ 0 ] ) );
	rig.Set( "Fringe", 0.0f );
	if( !rig.Render( 0 ) )
		return 1;
	const Image plain = rig.Pixels();
	rig.Set( "Fringe", 1.0f );
	if( !rig.Render( 0 ) )
		return 1;
	const int fringeAtDelay = maxByteDifference( plain, rig.Pixels() );
	Check( fringeAtDelay > 1,
	       fmt( "and at a real delay it colours the disagreement (%.0f codes)", fringeAtDelay ) );

	std::printf( "\n  A whole-pixel delay is a translation and is checked as one.\n"
	             "  A fractional delay is partial coverage and is checked as one.\n"
	             "  Zero delay is an absolute anchor, and is checked to the byte.\n" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --crawl
//---------------------------------------------------------------------------

/// The Clock Error slider position that means `ppm`, found by inverting the
/// plugin's own map. The checks state the physics first -- a rate in Amiga
/// pixels per second -- and find the slider afterwards, so they are about the
/// physics and not about the slider.
float clockParamFor( double ppm )
{
	double lo = 0.0, hi = 1.0;
	for( int i = 0; i < 60; ++i )
	{
		const double mid = 0.5 * ( lo + hi );
		if( ClockErrorPpmFromParam( static_cast< float >( mid ) ) < ppm )
			lo = mid;
		else
			hi = mid;
	}
	return static_cast< float >( 0.5 * ( lo + hi ) );
}

int runCrawl()
{
	//A wider raster than --delay, because the quantity under test is a
	//FRACTION of an Amiga pixel per frame: four output texels per Amiga
	//pixel puts the per-frame step well above the measurement tolerance.
	const int width = 1280, height = 360;
	const double pxPerAmiga = static_cast< double >( width ) / kAmigaLoresWidth;//4, in lores -- the default mode
	const double edgePx     = 0.5 * width;
	const double rampPx     = kEdgeRampAmigaPx * pxPerAmiga;
	const double rampStart  = edgePx - rampPx * 0.5;
	const double rampEnd    = edgePx + rampPx * 0.5;
	const double baseDelay  = -4.0;//Amiga pixels
	const double fps        = 60.0;
	const int frames        = 30;

	//The rate is stated FIRST, in Amiga pixels per second, and the slider
	//position is found by inverting the plugin's own map to reach it. The
	//prediction below is then built from the physical constants -- pixel
	//clock times fractional frequency error -- rather than from the map, so
	//this check is about the physics and not about the slider.
	const double wantRate = 8.0;//Amiga px/s: one wrap every 7.5 frames at 60fps
	const double wantPpm  = wantRate / ( kAmigaLoresPixelClockHz * 1e-6 );

	const float clockParam = clockParamFor( wantPpm );
	const double ppm       = ClockErrorPpmFromParam( clockParam );
	const double rate      = kAmigaLoresPixelClockHz * ppm * 1e-6 * 1.0;//Crawl Rate at unity

	//This check runs at the DEFAULT mode and the DEFAULT wrap -- lores, one
	//Amiga pixel -- which is the behaviour the plugin had before either
	//control existed. `--modes` is where the other settings are measured.
	const double kCrawlWrapAmigaPx = kCrawlWrapMinAmigaPx;

	std::printf( "the key delay walks at the rate the clock error predicts\n\n" );
	std::printf( "  Clock Error %.6f (%.6f ppm), Crawl Rate unity\n", clockParam, ppm );
	std::printf( "  predicted crawl %.6f Amiga px/s, wrapping every %.3f s (%.2f frames)\n",
	             rate, kCrawlWrapAmigaPx / rate, kCrawlWrapAmigaPx / rate * fps );
	std::printf( "  raster %dx%d, so one Amiga pixel is %.1f output texels\n\n", width, height, pxPerAmiga );

	Rig rig;
	if( !rig.Init( width, height ) )
		return 1;
	setUpTimingRig( rig, width, height, 0.5 );
	rig.Set( "Key Delay", delayParam( baseDelay ) );
	rig.Set( "Clock Error", clockParam );
	rig.Set( "Crawl Rate", 0.25f );//unity
	//Amiga Mode and Crawl Wrap are left at their defaults ON PURPOSE, and
	//the first rendered frame says what they came to.

	//Derived from the raster and the card, like --delay's. The key edge
	//travels between baseDelay and baseDelay + one Amiga pixel, so the
	//window has to hold the whole transition at both ends of that travel.
	const int measureHi = static_cast< int >( std::floor( rampStart ) ) - 1;
	const int measureLo = measureHi - 100;
	const int row       = height / 2;
	Check( measureHi + std::fabs( baseDelay + kCrawlWrapAmigaPx ) * pxPerAmiga > rampEnd
	           && measureLo + std::fabs( baseDelay ) * pxPerAmiga < rampStart,
	       fmt( "the integration window %.0f..%.0f holds the transition through a whole period",
	            measureLo, measureHi ) );

	//The tolerance is the same 0.25 output texels as --delay's fractional
	//case, and for the same reason: every frame of a crawl is at a
	//fractional offset, so every one of these is a partial-coverage
	//measurement. The bound is recomputed for THIS raster, whose ramp is
	//twice as wide in texels as --delay's.
	const double tolerance  = 0.25;
	const double contrast   = std::fabs( static_cast< double >( kVideo.b ) - kColour0.b );
	const double errorBound = 2.0 * ( rampPx * ( 0.5 / contrast ) + 1.0 / 256.0 );
	Check( errorBound * 3.0 <= tolerance,
	       fmt( "the derived error bound is %.4f texels, at least three times inside the ", errorBound )
	           + fmt( "%.2f tolerance", tolerance ) );

	//A wrap is a jump back of most of a period. Half a period is the
	//threshold, and a period is one Amiga pixel -- so this is stated in
	//texels of THIS raster, not in a constant that happens to work at 1280.
	const double wrapDrop = kCrawlWrapAmigaPx * pxPerAmiga * 0.5;

	double worstPhase = 0.0, worstPixels = 0.0;
	double firstEdge = 0.0, firstCrawl = 0.0;
	int wraps = 0, measured = 0;
	double previous = -1.0;
	for( int frame = 0; frame < frames; ++frame )
	{
		if( !rig.Render( frame, fps ) )
		{
			Check( false, "ProcessOpenGL failed" );
			return 1;
		}
		const Image out = rig.Pixels();

		const double t         = frame / fps;
		const double wantCrawl = timing::PositiveMod( rate * t, kCrawlWrapAmigaPx );
		//Relative to frame 0, so the measurement's own origin -- where the
		//card's ramp puts the 50% point, which is a property of the card and
		//not of the crawl -- cancels out.
		const double wantMoved = ( wantCrawl - firstCrawl ) * pxPerAmiga;

		//The plugin's own reduction, against the closed form. This
		//separates "the arithmetic is wrong" from "the arithmetic never
		//reached the picture".
		const Genlock::Timings timings = rig.plugin.TimingsForTest();
		worstPhase                     = std::max( worstPhase, std::fabs( timings.crawlAmigaPx - wantCrawl ) );

		const std::vector< double > key = keyAcrossRow( out, width, row, kColour0, kVideo );
		const double got                = edgeByIntegral( key, measureLo, measureHi );
		if( got < 0.0 )
		{
			Check( false, fmt( "frame %.0f: the key edge left the measurement window", frame ) );
			return 1;
		}
		if( frame == 0 )
		{
			firstEdge  = got;
			firstCrawl = wantCrawl;
		}
		else
			worstPixels = std::max( worstPixels, std::fabs( ( got - firstEdge ) - wantMoved ) );
		++measured;

		if( previous > 0.0 && got < previous - wrapDrop )
			++wraps;
		previous = got;
	}

	const Genlock::Timings last = rig.plugin.TimingsForTest();
	Check( last.amigaMode == AM_LORES && last.crawlWrapAmigaPx == kCrawlWrapAmigaPx,
	       fmt( "at the defaults the plugin crawls in lores with a %.6f Amiga px wrap", last.crawlWrapAmigaPx ) );
	Check( worstPhase < 1e-9,
	       fmt( "the reduced phase matches the closed form to %.3e Amiga px over ", worstPhase )
	           + fmt( "%.0f frames", measured ) );
	Check( worstPixels <= tolerance,
	       fmt( "the key edge is where the phase says, worst %.4f output texels", worstPixels )
	           + fmt( " (tolerance %.2f)", tolerance ) );

	//The wrap. The phase is reduced into one Amiga pixel because a genlock
	//re-locks on every line sync -- so the fringe walks a pixel and snaps
	//back rather than sliding off the screen.
	const int wantWraps = static_cast< int >( std::floor( rate * ( frames - 1 ) / fps / kCrawlWrapAmigaPx ) );
	Check( wraps == wantWraps,
	       fmt( "it wraps at one Amiga pixel: %.0f snaps in %.0f frames", wraps, static_cast< double >( frames ) )
	           + fmt( ", predicted %.0f", wantWraps ) );

	//And the crawl is not merely present but the right SIZE: over one full
	//period the edge travels one Amiga pixel, which at this raster is four
	//output texels -- sixteen times the tolerance.
	Check( kCrawlWrapAmigaPx * pxPerAmiga >= 8.0 * tolerance,
	       fmt( "one period of travel is %.1f output texels, ", kCrawlWrapAmigaPx * pxPerAmiga )
	           + fmt( "%.0fx the tolerance", kCrawlWrapAmigaPx * pxPerAmiga / tolerance ) );

	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --roll
//---------------------------------------------------------------------------
int rollAt( int width, int height )
{
	const double fps            = 60.0;
	const double rollsPerSecond = 1.0;//Roll Rate 0.25 of full travel
	const double barCentre = 0.5, barHeight = 0.1;
	const int frames       = 24;
	const double rowsPerFrame = rollsPerSecond * height / fps;

	std::printf( "  raster %dx%d: one roll is %d rows, %.2f rows per frame -- %s\n",
	             width, height, height, rowsPerFrame,
	             std::fabs( rowsPerFrame - std::lround( rowsPerFrame ) ) < 1e-9 ? "a WHOLE number"
	                                                                            : "a FRACTION of a row" );

	Rig rig;
	if( !rig.Init( width, height ) )
		return 1;
	//Dest is a FLAT field on purpose: the rotation check below says the
	//rolled frame is frame zero rotated, and that is only true of the
	//overlay. A patterned video would (correctly) stay put and break it.
	rig.UploadDest( flatField( width, height, kVideo ) );
	rig.UploadSrc( barCard( width, height, barCentre, barHeight ) );

	rig.Set( "Key Source", 0.0f );
	rig.Set( "Key Colour Red", kColour0.r / 255.0f );
	rig.Set( "Key Colour Green", kColour0.g / 255.0f );
	rig.Set( "Key Colour Blue", kColour0.b / 255.0f );
	rig.Set( "Tolerance", 0.10f );
	rig.Set( "Softness", 0.0f );
	rig.Set( "Key Delay", 0.5f );//no horizontal displacement
	rig.Set( "Crawl Rate", 0.0f );
	rig.Set( "Fringe", 0.0f );
	rig.Set( "Fader", 1.0f );
	rig.Set( "Opacity", 1.0f );
	rig.Set( "Roll Rate", static_cast< float >( rollsPerSecond / 4.0 ) );

	//-----------------------------------------------------------------
	// Locked. Losing lock is a branch, not a fade, so above the threshold
	// the roll phase is exactly zero and two frames a second apart are the
	// same bytes. Exact.
	//-----------------------------------------------------------------
	rig.Set( "Sync Quality", 1.0f );
	if( !rig.Render( 0, fps ) )
		return 1;
	const Image locked0 = rig.Pixels();
	if( !rig.Render( 60, fps ) )
		return 1;
	const Image locked60 = rig.Pixels();
	Check( rig.plugin.TimingsForTest().locked, "above the threshold the plugin reports lock" );
	Check( maxByteDifference( locked0, locked60 ) == 0,
	       fmt( "locked, a second apart, not one byte moves (worst %.0f)", maxByteDifference( locked0, locked60 ) ) );

	//-----------------------------------------------------------------
	// Unlocked.
	//
	// Two claims, and they are not the same claim -- which is why this runs
	// at two rasters. At 360 rows a roll of one per second advances a
	// WHOLE six rows per frame; at 270 rows it advances four and a half.
	// The whole-row case is a rotation and is asserted as byte equality.
	// The fractional case is partial coverage and gets a tolerance.
	//-----------------------------------------------------------------
	rig.Set( "Sync Quality", 0.2f );

	//0.05 rows, derived:
	//  * the bilinear reconstruction of the rolled overlay nulls the first
	//    alias almost completely, so the weighted centroid's dependence on
	//    where the row grid falls is below 0.001 rows;
	//  * an 8-bit readback costs 0.5/255 of weight on each of about four
	//    partially covered rows, which moves the centroid of a bar this
	//    deep by about 0.002 rows;
	//  * GL's minimum subtexel precision is 8 bits: 0.004 rows.
	// So the bound is under 0.01 rows. 0.05 is five times that, and twenty
	// times smaller than the one-row error a rounded offset would make.
	const double tolerance = 0.05;

	if( !rig.Render( 0, fps ) )
		return 1;
	const Image frame0 = rig.Pixels();
	const double row0  = circularBandRow( frame0, width, height, 0 );

	//An ABSOLUTE anchor. Everything below is measured as a difference from
	//this frame, which is what makes it independent of the bar's shape --
	//and which would also hide a constant vertical displacement of the
	//overlay. At zero phase the overlay is exactly where the card put it,
	//so the bar's centre is the generator's own, to the same tolerance.
	const double wantRow0 = barCentreRow( height, barCentre, barHeight );
	Check( std::fabs( rowDistance( row0, wantRow0, height ) ) <= tolerance,
	       fmt( "at zero phase the bar is where the card put it: %.4f rows, ", row0 )
	           + fmt( "the card says %.4f", wantRow0 ) );

	double worst = 0.0;
	int whole = 0, fraction = 0, rotations = 0;
	for( int frame = 1; frame < frames; ++frame )
	{
		if( !rig.Render( frame, fps ) )
			return 1;
		const Image out = rig.Pixels();

		//Closed form. The shader samples the overlay at fract( v + phase ),
		//so content at source v appears at output v - phase: the bar moves
		//DOWN the picture as the phase advances, and wraps at the raster's
		//own height.
		const double phase  = timing::PositiveMod( rollsPerSecond * frame / fps, 1.0 );
		const double moved  = phase * height;

		//The bar is fill (white) where the key is off and the video behind
		//it is green, so the red channel carries the band with the widest
		//margin available.
		const double got = circularBandRow( out, width, height, 0 );
		if( got < 0.0 )
		{
			Check( false, fmt( "frame %.0f: the bar is not in the picture", frame ) );
			return 1;
		}
		worst = std::max( worst, std::fabs( rowDistance( got, row0 - moved, height ) ) );

		//A whole number of rows is a rotation, and a rotation of a sampled
		//image is exact: every output row reads exactly one source row.
		if( std::fabs( moved - std::lround( moved ) ) < 1e-9 )
		{
			++whole;
			if( isRowRotationOf( out, frame0, width, height, static_cast< int >( std::lround( moved ) ) ) )
				++rotations;
		}
		else
			++fraction;
	}

	//`whole > 0` is part of the assertion, not a precondition: at a raster
	//where no frame lands on a whole row the exact claim would silently
	//stop being made, and this check would pass by having nothing to say.
	Check( whole > 0 && rotations == whole,
	       fmt( "whole-row advances are EXACT rotations of frame 0: %.0f of %.0f", rotations, whole ) );
	Check( worst <= tolerance,
	       fmt( "the bar is where the rate says: worst %.4f rows (tolerance %.2f) over ", worst, tolerance )
	           + fmt( "%.0f whole-row and %.0f fractional frames", whole, fraction ) );

	//The wrap, said plainly: after exactly one period the picture is back
	//where it started, byte for byte. The period is the RASTER's, not a
	//number -- height rows, at whatever height this is.
	const int period = static_cast< int >( std::lround( fps / rollsPerSecond ) );
	Check( timing::PositiveMod( rollsPerSecond * period / fps, 1.0 ) == 0.0,
	       fmt( "%.0f frames is exactly one period at this rate", static_cast< double >( period ) ) );
	if( !rig.Render( period, fps ) )
		return 1;
	const Image atPeriod = rig.Pixels();
	Check( maxByteDifference( frame0, atPeriod ) == 0,
	       fmt( "the roll wraps after %.0f frames, exactly (worst byte %.0f)", static_cast< double >( period ),
	            maxByteDifference( frame0, atPeriod ) ) );

	//And half a period is half the raster, which is a claim about the
	//raster and not about a number: 180 rows at 360, 135 at 270.
	if( !rig.Render( period / 2, fps ) )
		return 1;
	const double half = circularBandRow( rig.Pixels(), width, height, 0 );
	Check( std::fabs( rowDistance( half, row0, height ) - height / 2.0 ) <= tolerance,
	       fmt( "half a period is half the raster: %.3f rows of %.0f", rowDistance( half, row0, height ),
	            height / 2.0 ) );

	return 0;
}

int runRoll()
{
	std::printf( "below the sync threshold the overlay rolls, at the stated rate\n\n" );
	//Two rasters, because a check that depends on the raster must be stated
	//in terms of the raster. If any of this were calibrated to 360 rows it
	//would fail at 270.
	if( rollAt( 640, 360 ) != 0 )
		return 1;
	std::printf( "\n" );
	if( rollAt( 480, 270 ) != 0 )
		return 1;
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --fader
//---------------------------------------------------------------------------
int runFader()
{
	const int width = 640, height = 360;
	std::printf( "the fader, and what \"the output IS Dest\" is worth\n\n" );

	Rig rig;
	if( !rig.Init( width, height ) )
		return 1;

	const Image video = videoCard( width, height );
	const Image amiga = amigaCard( width, height );
	rig.UploadDest( video );
	rig.UploadSrc( amiga );

	keyOffEverywhere( rig );

	//-----------------------------------------------------------------
	// At Video the output is Dest, bitwise -- and here is exactly why.
	//
	// The path is one fetch and one write with `mix( video, video, Opacity )`
	// between them. At Opacity 1 that is `video * (1-1) + video * 1`:
	// (1-1) is exactly 0, x*0 is exactly 0, x*1 is exactly x, and 0 + x is
	// exactly x. An FMA contraction computes fma( video, 1, video*0 ) and
	// gets the same answer, because neither factor is a rounded quantity.
	// So nothing in the arithmetic can move a bit.
	//
	// The FETCH is exact for a different reason, and only under a stated
	// condition: the output raster equals Dest's raster and Dest has no
	// hardware padding, so MaxUV is exactly 1, every sample lands on a
	// texel centre, and the filter weight is 0/1 with no interpolation to
	// get wrong. Change either and this stops being bitwise -- it becomes
	// a resample, which is measured separately below.
	//-----------------------------------------------------------------
	rig.Set( "Fader", 0.0f );//Video
	rig.Set( "Opacity", 1.0f );
	if( !rig.Render( 0 ) )
		return 1;
	const Image atVideo = rig.Pixels();
	Check( maxByteDifference( atVideo, video ) == 0,
	       fmt( "Fader = Video is Dest, bitwise: worst byte %.0f", maxByteDifference( atVideo, video ) ) );

	//Opacity in between is NOT an exact cancellation: `video*(1-o) +
	//video*o` re-rounds twice. It still cannot move an 8-bit code by more
	//than one, and that -- one code -- is the tolerance, from the readback
	//and not from the measurement.
	int worstOpacity = 0;
	for( float o : { 0.0f, 0.25f, 0.5f, 0.75f } )
	{
		rig.Set( "Opacity", o );
		if( !rig.Render( 0 ) )
			return 1;
		worstOpacity = std::max( worstOpacity, maxByteDifference( rig.Pixels(), video ) );
	}
	Check( worstOpacity <= 1,
	       fmt( "and stays Dest at any Opacity, to one code (worst %.0f)", worstOpacity ) );
	rig.Set( "Opacity", 1.0f );

	//-----------------------------------------------------------------
	// At Overlay with the key off the output is Src, bitwise, by the same
	// argument: `mix( fill, video, 0.0 )` is `fill*1 + video*0`.
	//-----------------------------------------------------------------
	rig.Set( "Fader", 1.0f );//Overlay
	if( !rig.Render( 0 ) )
		return 1;
	const Image atOverlay = rig.Pixels();
	Check( maxByteDifference( atOverlay, amiga ) == 0,
	       fmt( "Fader = Overlay with no key is Src, bitwise: worst byte %.0f",
	            maxByteDifference( atOverlay, amiga ) ) );

	//-----------------------------------------------------------------
	// Dissolve reaches both ends exactly, and moves in between.
	//-----------------------------------------------------------------
	rig.Set( "Fader", 2.0f );//Dissolve
	rig.Set( "Dissolve", 0.0f );
	if( !rig.Render( 0 ) )
		return 1;
	const Image dissolve0 = rig.Pixels();
	rig.Set( "Dissolve", 1.0f );
	if( !rig.Render( 0 ) )
		return 1;
	const Image dissolve1 = rig.Pixels();
	rig.Set( "Dissolve", 0.5f );
	if( !rig.Render( 0 ) )
		return 1;
	const Image dissolveHalf = rig.Pixels();

	Check( maxByteDifference( dissolve0, atVideo ) == 0,
	       fmt( "Dissolve at 0 is the Video position, bitwise (worst %.0f)", maxByteDifference( dissolve0, atVideo ) ) );
	Check( maxByteDifference( dissolve1, atOverlay ) == 0,
	       fmt( "Dissolve at 1 is the Overlay position, bitwise (worst %.0f)",
	            maxByteDifference( dissolve1, atOverlay ) ) );
	//One code, not eight: one code is the smallest difference an 8-bit
	//readback can express, so "more than one" is the weakest honest way to
	//say the pot is in the signal path. A larger threshold would have been
	//fitted to this picture.
	Check( maxByteDifference( dissolveHalf, atVideo ) > 1 && maxByteDifference( dissolveHalf, atOverlay ) > 1,
	       fmt( "and halfway is neither (%.0f codes from Video, ", maxByteDifference( dissolveHalf, atVideo ) )
	           + fmt( "%.0f from Overlay)", maxByteDifference( dissolveHalf, atOverlay ) ) );

	//-----------------------------------------------------------------
	// The same claim at a raster Dest does not share. This is NOT bitwise,
	// it was never going to be, and the number is reported rather than
	// asserted -- a tolerance wide enough to cover a resampled test card's
	// edges would be wide enough to cover a broken fader.
	//-----------------------------------------------------------------
	{
		Rig other;
		if( !other.Init( width, height, InputSpec::Exact( 400, 240 ), InputSpec::Exact( width, height ) ) )
			return 1;
		other.UploadDest( videoCard( 400, 240 ) );
		other.UploadSrc( amiga );
		keyOffEverywhere( other );
		other.Set( "Fader", 0.0f );
		other.Set( "Opacity", 1.0f );
		if( !other.Render( 0 ) )
			return 1;
		//Compare against the SAME card resampled by nothing: there is no
		//such thing, so all that is reported is that the picture arrived.
		const Image out = other.Pixels();
		double mean[ 3 ];
		meanOver( out, width, width / 4, height / 4, 3 * width / 4, 3 * height / 4, mean );
		std::printf( "\n  At a Dest raster of 400x240 into a %dx%d output, Fader = Video is a\n"
		             "  RESAMPLE and not a copy -- there is no bitwise claim to make. The\n"
		             "  picture is there (centre mean %.1f, %.1f, %.1f) and that is all this\n"
		             "  says.\n",
		             width, height, mean[ 0 ], mean[ 1 ], mean[ 2 ] );
	}

	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --modes
//
// Amiga Mode changes the unit, and Controls.h says for each quantity whether
// it scales with the mode or not. These checks hold the plugin to that, at
// two rasters, and each one is run a second time against a plugin with the
// WRONG answer built in (Genlock::Fault) to show that it fails.
//
//   crawl speed     the same distance per second across the picture in every
//                   mode -- bit-identical, not merely close
//   Key Delay       a count of the mode's pixel clocks: half the distance in
//                   hires, a quarter in superhires
//   tear throw      a time error, stated in lores pixels: the same in every
//                   mode
//   Crawl Wrap      the crawl wraps where the control says, in pixels of the
//                   mode
//---------------------------------------------------------------------------

/// What one run of a check came to: whether every claim held, and if not
/// the first that did not -- which is what a negative control reports.
struct Outcome
{
	bool pass = true;
	std::string firstFailure;
	bool pictureFailed = false;///< a claim measured OUT OF THE PICTURE failed
	std::string firstPictureFailure;
};

/// Marks a claim as measured from the rendered pixels, rather than from
/// the plugin's own report of what it did or from arithmetic.
constexpr bool kFromPicture = true;

/// A claim that is reported as a Check when `report` is set and only
/// recorded when it is not. The negative controls run the SAME claims
/// silently against a broken plugin and then make one Check of their own:
/// that something failed.
struct Claims
{
	bool report = true;
	Outcome outcome;

	void operator()( bool ok, const std::string& what, bool fromPicture = false )
	{
		if( report )
			Check( ok, what );
		if( !ok && outcome.pass )
		{
			outcome.pass         = false;
			outcome.firstFailure = what;
		}
		if( !ok && fromPicture && !outcome.pictureFailed )
		{
			outcome.pictureFailed       = true;
			outcome.firstPictureFailure = what;
		}
	}
};

/// The negative control's own Check: the claims above FAILED against a
/// plugin with `fault` built in -- and specifically one measured out of the
/// PICTURE. A fault caught only by the plugin's report of its own phases
/// would show the plumbing is checked, not that the pixels are.
void expectFailure( const Outcome& outcome, const std::string& fault )
{
	Check( outcome.pictureFailed, "negative control, " + fault + ": the check FAILS in the picture"
	                                  + ( outcome.pictureFailed ? " (" + outcome.firstPictureFailure + ")"
	                                                            : std::string( " -- it did not, so it cannot tell" ) ) );
}

/// The key, recovered over a card whose fill is NOT flat.
///
/// keyAcrossRow assumes the fill is colour 0 everywhere it looks, which holds
/// only to the left of the card's own edge -- so --delay and --crawl keep
/// the key's delay negative. A crawl from a zero delay walks the key RIGHT,
/// into the ramp and the fill. The output is still `mix( fill, video, k )`
/// and the fill is still the card at this very column -- the fill fetch is
/// not displaced while locked, and at matched rasters it lands on texel
/// centres -- so the inversion holds column by column with the card's own
/// value in place of a constant. Read off BLUE, where the video is 0 and the
/// card is 170 at its darkest: the same 170 codes of contrast the flat-fill
/// version had, so the same error bound.
std::vector< double > keyOverCard( const Image& out, const Image& card, int width, int row )
{
	std::vector< double > key( static_cast< size_t >( width ) );
	for( int x = 0; x < width; ++x )
	{
		const size_t at    = ( static_cast< size_t >( row ) * width + x ) * 4 + 2;
		const double fill  = card[ at ];
		const double video = kVideo.b;
		key[ static_cast< size_t >( x ) ] = ( static_cast< double >( out[ at ] ) - fill ) / ( video - fill );
	}
	return key;
}

/// The FILL's edge, with the key off: 1 where the card is colour 0, 0 where
/// it is white, read off blue (170 to 255: 85 codes of contrast).
std::vector< double > fillProfile( const Image& out, int width, int row )
{
	std::vector< double > v( static_cast< size_t >( width ) );
	for( int x = 0; x < width; ++x )
	{
		const size_t at = ( static_cast< size_t >( row ) * width + x ) * 4 + 2;
		v[ static_cast< size_t >( x ) ] = ( 255.0 - out[ at ] ) / ( 255.0 - kColour0.b );
	}
	return v;
}

/// The error bound for an edge found by integration, derived as --delay
/// derives it: half a code of each partially covered column, over `contrast`
/// codes, across the ramp; plus GL's eight bits of subtexel precision; twice,
/// because two edges are differenced.
double integralErrorBound( double rampPx, double contrast )
{
	return 2.0 * ( rampPx * ( 0.5 / contrast ) + 1.0 / 256.0 );
}

/// The same 0.25 output texels as --delay's fractional case, for the same
/// reasons: a factor of three over the derived bound (asserted, per raster),
/// and half the 0.5-texel error of a plugin that rounded to whole texels.
constexpr double kEdgeTolerance = 0.25;

struct ModeSettings
{
	int mode            = AM_LORES;
	float keyDelay      = 0.5f;//zero
	float clockError    = 0.3f;
	float crawlRate     = 0.0f;
	float crawlWrap     = 0.0f;
	float syncQuality   = 1.0f;
	bool keyOff         = false;
	Genlock::Fault fault = Genlock::FAULT_NONE;
};

struct ModeRun
{
	bool ok = true;
	std::vector< Image > frames;
	std::vector< Genlock::Timings > timings;
};

/// Frames 0..count-1 of the edge card through ONE rig with these settings.
///
/// One rig per check, not per mode: every mode is rendered by the same
/// program object on the same textures, so where two modes hand the shader
/// the same uniform bits, "the same bytes" rests on nothing but GL's
/// repeatability rule -- the same program, state and inputs give the same
/// result -- which every conforming implementation owes. The clock's epoch
/// is the first frame this rig ever rendered, t = 0, so frame f is t = f/60
/// however many times the frames are rendered again.
ModeRun renderModes( Rig& rig, const ModeSettings& s, int count, double fps = 60.0 )
{
	ModeRun run;
	setUpTimingRig( rig, rig.width, rig.height, 0.5 );
	if( s.keyOff )
		keyOffEverywhere( rig );
	rig.plugin.SetFaultForTest( s.fault );
	rig.Set( "Amiga Mode", static_cast< float >( s.mode ) );
	rig.Set( "Key Delay", s.keyDelay );
	rig.Set( "Clock Error", s.clockError );
	rig.Set( "Crawl Rate", s.crawlRate );
	rig.Set( "Crawl Wrap", s.crawlWrap );
	rig.Set( "Sync Quality", s.syncQuality );
	for( int frame = 0; frame < count; ++frame )
	{
		if( !rig.Render( frame, fps ) )
		{
			run.ok = false;
			return run;
		}
		run.frames.push_back( rig.Pixels() );
		run.timings.push_back( rig.plugin.TimingsForTest() );
	}
	rig.plugin.SetFaultForTest( Genlock::FAULT_NONE );
	return run;
}

const char* const kModeNames[ AM_COUNT ] = { "lores", "hires", "superhires" };

//---------------------------------------------------------------------------
// The crawl's speed across the picture, in all three modes.
//---------------------------------------------------------------------------
Outcome crawlAcrossModes( int width, int height, Genlock::Fault fault, bool report )
{
	Claims claim { report, {} };
	const double fps        = 60.0;
	const int frames        = 24;
	const double pxPerLores = width / kAmigaLoresWidth;
	const double edgePx     = 0.5 * width;
	const double rampPx     = kEdgeRampAmigaPx * pxPerLores;

	//Stated in LORES pixels per second, the physics; the slider is found
	//afterwards. Hires and superhires get the same Clock Error, and so a
	//crawl of 16 and 32 of THEIR pixels per second.
	const double wantLores = 8.0;
	const float clockParam = clockParamFor( wantLores / ( kAmigaLoresPixelClockHz * 1e-6 ) );
	const double loresRate = kAmigaLoresPixelClockHz * ClockErrorPpmFromParam( clockParam ) * 1e-6;
	const float wrapParam  = 1.0f;//16 pixels of the mode, the widest

	//The claim is about SPEED, so no mode may wrap inside the run: at the
	//last frame superhires has walked four times as many of its own pixels
	//as lores has of its.
	const double lastT = ( frames - 1 ) / fps;
	claim( 4.0 * loresRate * lastT < CrawlWrapAmigaPxFromParam( wrapParam ),
	       fmt( "no mode wraps inside the run (superhires reaches %.3f of its 16-pixel wrap)", 4.0 * loresRate * lastT ) );

	const double maxTravel = loresRate * lastT * pxPerLores;
	const int lo           = static_cast< int >( std::floor( edgePx - rampPx * 0.5 ) ) - 3;
	const int hi           = static_cast< int >( std::ceil( edgePx + rampPx * 0.5 + maxTravel ) ) + 3;
	const double bound     = integralErrorBound( rampPx, std::fabs( static_cast< double >( kVideo.b ) - kColour0.b ) );
	claim( bound * 3.0 <= kEdgeTolerance,
	       fmt( "the derived error bound is %.4f texels, three times inside %.2f", bound, kEdgeTolerance ) );

	Rig rig;
	if( !rig.Init( width, height ) )
	{
		claim( false, "the rig initialises" );
		return claim.outcome;
	}
	const Image card = edgeCard( width, height, 0.5 );
	const int row    = height / 2;

	ModeRun runs[ AM_COUNT ];
	for( int m = 0; m < AM_COUNT; ++m )
	{
		ModeSettings s;
		s.mode       = m;
		s.clockError = clockParam;
		s.crawlRate  = 0.25f;//unity
		s.crawlWrap  = wrapParam;
		s.fault      = fault;
		runs[ m ]    = renderModes( rig, s, frames, fps );
		if( !runs[ m ].ok )
		{
			claim( false, std::string( kModeNames[ m ] ) + ": ProcessOpenGL failed" );
			return claim.outcome;
		}
	}

	//1. The plugin's own reduction, as a fraction of the picture, is the
	//   SAME BITS in every mode. Division by the mode's width undoes the
	//   mode's clock exactly because both are the lores value times the same
	//   power of two.
	int bitMismatches = 0;
	for( int m = 1; m < AM_COUNT; ++m )
		for( int f = 0; f < frames; ++f )
			if( runs[ m ].timings[ f ].crawlAmigaPx / AmigaWidthForMode( m )
			    != runs[ 0 ].timings[ f ].crawlAmigaPx / AmigaWidthForMode( AM_LORES ) )
				++bitMismatches;
	claim( bitMismatches == 0,
	       fmt( "the crawl as a fraction of the picture is bit-identical in all three modes (%.0f of %.0f frames differ)",
	            bitMismatches, 2.0 * frames ) );

	//2. So the PICTURES are the same bytes. Same uniforms, same program --
	//   see renderModes.
	int frameMismatches = 0;
	for( int m = 1; m < AM_COUNT; ++m )
		for( int f = 0; f < frames; ++f )
			if( maxByteDifference( runs[ m ].frames[ f ], runs[ 0 ].frames[ f ] ) != 0 )
				++frameMismatches;
	claim( frameMismatches == 0,
	       fmt( "hires and superhires render the same bytes as lores, frame for frame (%.0f of %.0f differ)",
	            frameMismatches, 2.0 * frames ),
	       kFromPicture );

	//3. Measured out of the picture, in each mode, against the physics in
	//   lores pixels -- not against the other modes, which would pass a
	//   plugin that was wrong the same way in all three.
	for( int m = 0; m < AM_COUNT; ++m )
	{
		double worst = 0.0, first = 0.0;
		bool inside  = true;
		for( int f = 0; f < frames; ++f )
		{
			const double edge = edgeByIntegral( keyOverCard( runs[ m ].frames[ f ], card, width, row ), lo, hi );
			if( edge < 0.0 )
			{
				inside = false;
				break;
			}
			if( f == 0 )
				first = edge;
			else
				worst = std::max( worst, std::fabs( ( edge - first ) - loresRate * ( f / fps ) * pxPerLores ) );
		}
		claim( inside && worst <= kEdgeTolerance,
		       std::string( kModeNames[ m ] )
		           + ( inside ? fmt( ": the key walks %.4f lores px/s across the picture, worst %.4f texels off", loresRate, worst )
		                          + fmt( " (tolerance %.2f)", kEdgeTolerance )
		                      : std::string( ": the key edge left the measurement window" ) ),
		       kFromPicture );
	}
	return claim.outcome;
}

//---------------------------------------------------------------------------
// Key Delay is a count of the mode's pixel clocks.
//---------------------------------------------------------------------------
Outcome keyDelayAcrossModes( int width, int height, Genlock::Fault fault, bool report )
{
	Claims claim { report, {} };
	const double pxPerLores = width / kAmigaLoresWidth;
	const double edgePx     = 0.5 * width;
	const double rampPx     = kEdgeRampAmigaPx * pxPerLores;
	const double delayPx    = -8.0;//of the mode: the end of the control's range

	const int lo       = static_cast< int >( std::floor( edgePx - rampPx * 0.5 + delayPx * pxPerLores ) ) - 3;
	const int hi       = static_cast< int >( std::ceil( edgePx + rampPx * 0.5 ) ) + 3;
	const double bound = integralErrorBound( rampPx, std::fabs( static_cast< double >( kVideo.b ) - kColour0.b ) );
	claim( bound * 3.0 <= kEdgeTolerance,
	       fmt( "the derived error bound is %.4f texels, three times inside %.2f", bound, kEdgeTolerance ) );

	Rig rig;
	if( !rig.Init( width, height ) )
	{
		claim( false, "the rig initialises" );
		return claim.outcome;
	}
	const Image card = edgeCard( width, height, 0.5 );
	const int row    = height / 2;

	auto render = [ & ]( int mode, float keyDelay, Genlock::Fault f ) {
		ModeSettings s;
		s.mode     = mode;
		s.keyDelay = keyDelay;
		s.fault    = f;
		ModeRun run = renderModes( rig, s, 1 );
		return run.ok ? run.frames[ 0 ] : Image();
	};

	Image atDelay[ AM_COUNT ];
	for( int m = 0; m < AM_COUNT; ++m )
	{
		const Image zero = render( m, delayParam( 0.0 ), fault );
		atDelay[ m ]     = render( m, delayParam( delayPx ), fault );
		if( zero.empty() || atDelay[ m ].empty() )
		{
			claim( false, "ProcessOpenGL failed" );
			return claim.outcome;
		}

		//The prediction is -8 of THIS mode's pixels, i.e. -8/320, -8/640 or
		//-8/1280 of the picture. At both rasters this runs at, each is a
		//whole number of output texels, and that is asserted rather than
		//assumed.
		const double want = delayPx * width / AmigaWidthForMode( m );
		claim( std::fabs( want - std::lround( want ) ) < 1e-9,
		       std::string( kModeNames[ m ] ) + fmt( ": -8 of its pixels is %.4f output texels, a whole number", want ) );

		const double e0 = edgeByIntegral( keyOverCard( zero, card, width, row ), lo, hi );
		const double e1 = edgeByIntegral( keyOverCard( atDelay[ m ], card, width, row ), lo, hi );
		const double got = e1 - e0;
		const bool found = e0 >= 0.0 && e1 >= 0.0;
		claim( found && std::fabs( got - want ) <= kEdgeTolerance,
		       std::string( kModeNames[ m ] )
		           + ( found ? fmt( ": Key Delay -8 moves the key %.4f texels, predicted %.4f", got, want )
		                           + fmt( " (%.3f lores px)", got / pxPerLores )
		                     : std::string( ": the key edge left the measurement window" ) ),
		       kFromPicture );
	}

	//And EXACTLY: -8 hires pixels is -4 lores pixels, and -8 superhires is
	//-2, to the bit -- (-8)/640 and (-4)/320 are the same real quotient and
	//IEEE division rounds it the same way -- so the pictures are the same
	//bytes. Checked on the CPU first, so a failure below means the plugin
	//and not the arithmetic.
	const float hiresUniform = static_cast< float >( -8.0 / AmigaWidthForMode( AM_HIRES ) );
	const float superUniform = static_cast< float >( -8.0 / AmigaWidthForMode( AM_SUPERHIRES ) );
	claim( hiresUniform == static_cast< float >( -4.0 / kAmigaLoresWidth )
	           && superUniform == static_cast< float >( -2.0 / kAmigaLoresWidth ),
	       "-8/640 is -4/320 and -8/1280 is -2/320, to the bit" );
	const Image lores4 = render( AM_LORES, delayParam( -4.0 ), Genlock::FAULT_NONE );
	const Image lores2 = render( AM_LORES, delayParam( -2.0 ), Genlock::FAULT_NONE );
	claim( !lores4.empty() && maxByteDifference( atDelay[ AM_HIRES ], lores4 ) == 0,
	       fmt( "hires at -8 is lores at -4, byte for byte (worst %.0f)", maxByteDifference( atDelay[ AM_HIRES ], lores4 ) ),
	       kFromPicture );
	claim( !lores2.empty() && maxByteDifference( atDelay[ AM_SUPERHIRES ], lores2 ) == 0,
	       fmt( "superhires at -8 is lores at -2, byte for byte (worst %.0f)",
	            maxByteDifference( atDelay[ AM_SUPERHIRES ], lores2 ) ),
	       kFromPicture );
	return claim.outcome;
}

//---------------------------------------------------------------------------
// The tear's throw is the same distance in every mode.
//---------------------------------------------------------------------------
Outcome tearAcrossModes( int width, int height, Genlock::Fault fault, bool report )
{
	Claims claim { report, {} };
	const double pxPerLores = width / kAmigaLoresWidth;
	const double edgePx     = 0.5 * width;
	const double rampPx     = kEdgeRampAmigaPx * pxPerLores;
	const float quality     = 0.2f;

	//The throw, computed as the plugin computes it, in float, so the
	//prediction carries the same rounding: a fraction of the picture.
	const float lockLoss  = ( kLockThreshold - quality ) / kLockThreshold;
	const double throwPic = static_cast< float >( lockLoss * kTearAmigaPx / kAmigaLoresWidth );

	//How hard a row leans, from the shader's own definition: 1 at the seam,
	//falling to 0 at kTearBand from it. At frame 0 the roll phase is exactly
	//0, so the seam is at the bottom (and, wrapping, the top) of the picture.
	const auto lean = [ & ]( int row ) {
		const double v    = ( row + 0.5 ) / height;
		const double seam = std::min( v, 1.0 - v );
		return std::max( 0.0, 1.0 - seam / kTearBand );
	};

	const double maxShift = throwPic * width;
	const int lo          = static_cast< int >( std::floor( edgePx - rampPx * 0.5 - maxShift ) ) - 3;
	const int hi          = static_cast< int >( std::ceil( edgePx + rampPx * 0.5 ) ) + 3;
	const double bound    = integralErrorBound( rampPx, 255.0 - kColour0.b );
	claim( bound * 3.0 <= kEdgeTolerance,
	       fmt( "the derived error bound is %.4f texels, three times inside %.2f", bound, kEdgeTolerance ) );

	Rig rig;
	if( !rig.Init( width, height ) )
	{
		claim( false, "the rig initialises" );
		return claim.outcome;
	}

	Image frame[ AM_COUNT ];
	for( int m = 0; m < AM_COUNT; ++m )
	{
		ModeSettings s;
		s.mode        = m;
		s.keyOff      = true;//the FILL's edge: what the tear throws is the overlay
		s.syncQuality = quality;
		s.fault       = fault;
		ModeRun run   = renderModes( rig, s, 1 );
		if( !run.ok )
		{
			claim( false, "ProcessOpenGL failed" );
			return claim.outcome;
		}
		frame[ m ] = run.frames[ 0 ];
		claim( !run.timings[ 0 ].locked && run.timings[ 0 ].rollPhase == 0.0,
		       std::string( kModeNames[ m ] ) + ": unlocked, at roll phase exactly 0" );

		//Rows at the seam against the middle row, which leans not at all.
		const int middle  = height / 2;
		const double base = edgeByIntegral( fillProfile( frame[ m ], width, middle ), lo, hi );
		for( int row : { 0, height - 1 } )
		{
			const double want = -throwPic * lean( row ) * width;
			const double edge = edgeByIntegral( fillProfile( frame[ m ], width, row ), lo, hi );
			const double got  = edge - base;
			const bool found = base >= 0.0 && edge >= 0.0;
			claim( found && std::fabs( got - want ) <= kEdgeTolerance,
			       std::string( kModeNames[ m ] )
			           + ( found ? fmt( ": row %.0f is thrown %.4f texels, predicted %.4f", row, got, want )
			                           + fmt( " (%.3f lores px)", got / pxPerLores )
			                     : fmt( ": row %.0f left the measurement window", row ) ),
			       kFromPicture );
		}
	}

	//Nothing mode-dependent reaches the shader here -- Key Delay is zero and
	//the crawl is stopped -- so if the throw is mode-independent the three
	//pictures are the same bytes.
	claim( maxByteDifference( frame[ AM_HIRES ], frame[ AM_LORES ] ) == 0
	           && maxByteDifference( frame[ AM_SUPERHIRES ], frame[ AM_LORES ] ) == 0,
	       fmt( "the torn frame is the same bytes in all three modes (worst %.0f)",
	            std::max( maxByteDifference( frame[ AM_HIRES ], frame[ AM_LORES ] ),
	                      maxByteDifference( frame[ AM_SUPERHIRES ], frame[ AM_LORES ] ) ) ),
	       kFromPicture );
	return claim.outcome;
}

//---------------------------------------------------------------------------
// The crawl wraps at the Crawl Wrap setting, in pixels of the mode.
//---------------------------------------------------------------------------
Outcome wrapAtSetting( int width, int height, int mode, Genlock::Fault fault, bool report )
{
	Claims claim { report, {} };
	const double fps       = 60.0;
	const int frames       = 60;
	const double pxPerMode = width / AmigaWidthForMode( mode );
	const double edgePx    = 0.5 * width;
	const double rampPx    = kEdgeRampAmigaPx * width / kAmigaLoresWidth;
	const double wantWrap  = 4.0;//pixels of the mode
	const std::string tag  = kModeNames[ mode ];

	//14 lores pixels a second of clock error, so 28 hires ones: a wrap every
	//17 frames in lores and every 8.6 in hires -- never on a frame, so no
	//count depends on which side of a boundary a rounding falls.
	const float clockParam = clockParamFor( 14.0 / ( kAmigaLoresPixelClockHz * 1e-6 ) );
	const float wrapParam  = CrawlWrapParamFor( wantWrap );
	const double rate      = AmigaPixelClockHzForMode( mode ) * ClockErrorPpmFromParam( clockParam ) * 1e-6;

	const int lo       = static_cast< int >( std::floor( edgePx - rampPx * 0.5 ) ) - 3;
	const int hi       = static_cast< int >( std::ceil( edgePx + rampPx * 0.5 + wantWrap * pxPerMode ) ) + 3;
	const double bound = integralErrorBound( rampPx, std::fabs( static_cast< double >( kVideo.b ) - kColour0.b ) );
	claim( bound * 3.0 <= kEdgeTolerance,
	       fmt( "the derived error bound is %.4f texels, three times inside %.2f", bound, kEdgeTolerance ) );
	//One period of travel has to be worth measuring at this raster: at
	//least eight tolerances. 4 hires pixels at 320 wide is exactly 2 texels,
	//which is exactly eight.
	claim( wantWrap * pxPerMode >= 8.0 * kEdgeTolerance,
	       tag + fmt( ": one period is %.2f output texels, at least 8x the tolerance", wantWrap * pxPerMode ) );

	Rig rig;
	if( !rig.Init( width, height ) )
	{
		claim( false, "the rig initialises" );
		return claim.outcome;
	}
	ModeSettings s;
	s.mode       = mode;
	s.clockError = clockParam;
	s.crawlRate  = 0.25f;
	s.crawlWrap  = wrapParam;
	s.fault      = fault;
	const ModeRun run = renderModes( rig, s, frames, fps );
	if( !run.ok )
	{
		claim( false, "ProcessOpenGL failed" );
		return claim.outcome;
	}

	//The control's value, as the plugin reports using it: 4 of this mode's
	//pixels, to a float's precision (the slider is a float).
	const double wrap = CrawlWrapAmigaPxFromParam( wrapParam );
	claim( std::fabs( wrap - wantWrap ) < 1e-6 && run.timings.back().crawlWrapAmigaPx == wrap,
	       tag + fmt( ": the plugin wraps at %.6f of its pixels, the control asks for %.6f", run.timings.back().crawlWrapAmigaPx,
	                  wrap ) );

	const Image card = edgeCard( width, height, 0.5 );
	const int row    = height / 2;
	double worstPhase = 0.0, worstPixels = 0.0, first = 0.0, previous = -1.0, furthest = 0.0;
	int wraps   = 0;
	bool inside = true;
	for( int f = 0; f < frames; ++f )
	{
		const double want = timing::PositiveMod( rate * ( f / fps ), wrap );
		worstPhase        = std::max( worstPhase, std::fabs( run.timings[ f ].crawlAmigaPx - want ) );

		const double edge = edgeByIntegral( keyOverCard( run.frames[ f ], card, width, row ), lo, hi );
		if( edge < 0.0 )
		{
			inside = false;
			break;
		}
		if( f == 0 )
			first = edge;
		worstPixels = std::max( worstPixels, std::fabs( ( edge - first ) - want * pxPerMode ) );
		furthest    = std::max( furthest, edge - first );
		//A wrap is a jump back of more than half a period, in texels of
		//THIS raster and this mode.
		if( previous >= 0.0 && edge < previous - wrap * pxPerMode * 0.5 )
			++wraps;
		previous = edge;
	}
	claim( inside, tag + ": the key edge stays inside the measurement window", kFromPicture );

	claim( worstPhase < 1e-9, tag + fmt( ": the reduced phase is the closed form mod %.1f, to %.3e px", wrap, worstPhase ) );
	claim( worstPixels <= kEdgeTolerance,
	       tag + fmt( ": the key edge is where that phase says, worst %.4f texels (tolerance %.2f)", worstPixels,
	                  kEdgeTolerance ),
	       kFromPicture );

	const int wantWraps = static_cast< int >( std::floor( rate * ( frames - 1 ) / fps / wrap ) );
	claim( wraps == wantWraps,
	       tag + fmt( ": %.0f wraps in %.0f frames, predicted %.0f", wraps, frames, wantWraps ), kFromPicture );

	//And the reach, which is what the control is FOR: the fringe gets within
	//one frame's travel of the wrap before it snaps back, and never past it.
	//The default one-pixel wrap would stop at a quarter of this.
	const double reach = wrap * pxPerMode, step = rate / fps * pxPerMode;
	claim( furthest >= reach - step - kEdgeTolerance && furthest <= reach + kEdgeTolerance,
	       tag + fmt( ": the key walks %.3f texels before it snaps, the wrap is %.3f", furthest, reach )
	           + fmt( " (one frame is %.3f)", step ),
	       kFromPicture );
	return claim.outcome;
}

int runModes()
{
	std::printf( "Amiga Mode: what scales with the mode, and what does not\n" );

	//The arithmetic first, with no GPU in it: the crawl's speed across the
	//picture is the same BITS in every mode, over the whole range of both
	//controls that set it.
	{
		int mismatches = 0, tried = 0;
		for( float clock : { 0.0f, 0.3f, 0.555f, 0.8f, 1.0f } )
			for( float crawl : { 0.1f, 0.25f, 0.7f, 1.0f } )
				for( int m = 1; m < AM_COUNT; ++m )
				{
					++tried;
					if( CrawlRateAmigaPxPerSecond( clock, crawl, m ) / AmigaWidthForMode( m )
					    != CrawlRateAmigaPxPerSecond( clock, crawl, AM_LORES ) / AmigaWidthForMode( AM_LORES ) )
						++mismatches;
				}
		std::printf( "\n  arithmetic\n" );
		Check( mismatches == 0,
		       fmt( "rate / width is bit-identical across modes for %.0f settings (%.0f differ)", tried, mismatches ) );
		Check( AmigaWidthForMode( AM_HIRES ) == 640.0 && AmigaWidthForMode( AM_SUPERHIRES ) == 1280.0
		           && AmigaPixelClockHzForMode( AM_HIRES ) == 2.0 * kAmigaLoresPixelClockHz
		           && AmigaPixelClockHzForMode( AM_SUPERHIRES ) == 4.0 * kAmigaLoresPixelClockHz,
		       "hires and superhires are 640 and 1280 across, at twice and four times the lores clock" );
		Check( AmigaWidthForMode( -1 ) == kAmigaLoresWidth && AmigaWidthForMode( 7 ) == kAmigaLoresWidth * 4.0,
		       "an out-of-range mode is clamped, not read past the table" );
	}

	//Two rasters: the one these were developed at, and the one CI renders
	//at. Every window, tolerance and prediction below is computed from the
	//raster, so each is a statement about 320x180 in its own right.
	const int rasters[ 2 ][ 2 ] = { { 640, 360 }, { 320, 180 } };
	for( const auto& r : rasters )
	{
		const int w = r[ 0 ], h = r[ 1 ];
		std::printf( "\n  %dx%d: one lores pixel is %.2f output texels\n", w, h, w / kAmigaLoresWidth );

		std::printf( "\n   the crawl's speed across the picture\n" );
		crawlAcrossModes( w, h, Genlock::FAULT_NONE, true );
		expectFailure( crawlAcrossModes( w, h, Genlock::FAULT_CRAWL_IN_LORES, false ),
		               "the crawl rate ignoring the mode's faster clock" );

		std::printf( "\n   Key Delay counts the mode's pixel clocks\n" );
		keyDelayAcrossModes( w, h, Genlock::FAULT_NONE, true );
		expectFailure( keyDelayAcrossModes( w, h, Genlock::FAULT_DELAY_IN_LORES, false ),
		               "Key Delay counted in lores pixels in every mode" );

		std::printf( "\n   the tear throws the same distance in every mode\n" );
		tearAcrossModes( w, h, Genlock::FAULT_NONE, true );
		expectFailure( tearAcrossModes( w, h, Genlock::FAULT_TEAR_SCALES_WITH_MODE, false ),
		               "the tear's throw counted in the mode's pixels" );

		std::printf( "\n   the crawl wraps at Crawl Wrap\n" );
		for( int m : { AM_LORES, AM_HIRES } )
		{
			wrapAtSetting( w, h, m, Genlock::FAULT_NONE, true );
			expectFailure( wrapAtSetting( w, h, m, Genlock::FAULT_WRAP_FIXED, false ),
			               std::string( kModeNames[ m ] ) + ", Crawl Wrap ignored (the old fixed one pixel)" );
		}
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --defaults
//
// Two controls were added in front of a plugin that already had a
// behaviour. At their defaults they must BE that behaviour, not resemble it.
//---------------------------------------------------------------------------

/// The crawl and the delay as the plugin computed them before Amiga Mode
/// and Crawl Wrap existed (cf17b14): the lores clock, 7093790 Hz, written
/// out as the literal it was; a wrap of exactly one pixel; the operator's
/// delay added before anything else. Deliberately NOT built from the new
/// functions -- this is the old formula, restated as it was.
void oldCrawl( float clockParam, float crawlParam, float delayParam_, double t, double& crawl, double& delay )
{
	const double ppm  = static_cast< double >( ClockErrorPpmFromParam( clockParam ) );
	const double rate = 7093790.0 * ppm * 1e-6 * static_cast< double >( CrawlRateFromParam( crawlParam ) );
	crawl             = timing::PositiveMod( rate * t, 1.0 );
	delay             = static_cast< double >( KeyDelayFromParam( delayParam_ ) ) + crawl;
}

/// Frames against the old formula. Returns how many frames did NOT match
/// it bit for bit.
int framesOffOldFormula( const std::vector< std::pair< std::string, float > >& settings, float clockParam )
{
	Rig rig;
	if( !rig.Init( 320, 180 ) )
		return -1;
	rig.UploadDest( videoCard( 320, 180 ) );
	rig.UploadSrc( amigaCard( 320, 180 ) );
	rig.Set( "Clock Error", clockParam );
	for( const auto& s : settings )
		rig.Set( s.first, s.second );

	const float crawlParam = rig.plugin.GetFloatParameter( Genlock::PT_CRAWL_RATE );
	const float delayP     = rig.plugin.GetFloatParameter( Genlock::PT_KEY_DELAY );
	int off                = 0;
	for( int frame = 0; frame < 60; ++frame )
	{
		if( !rig.Render( frame ) )
			return -1;
		double crawl = 0.0, delay = 0.0;
		oldCrawl( clockParam, crawlParam, delayP, frame / 60.0, crawl, delay );
		const Genlock::Timings t = rig.plugin.TimingsForTest();
		if( t.crawlAmigaPx != crawl || t.delayAmigaPx != delay )
			++off;
	}
	return off;
}

int runDefaults()
{
	std::printf( "the new controls' defaults ARE the old behaviour\n\n" );
	Genlock plugin;

	//The shape of the parameter list. Two parameters were inserted in front
	//of the About block, which must still close it.
	Check( Genlock::PT_COUNT == 25,
	       fmt( "%.0f parameters: 21 controls and the 4-entry About block", static_cast< double >( Genlock::PT_COUNT ) ) );
	Check( Genlock::PT_ABOUT_FIRST == 21 && std::strcmp( plugin.GetParamName( Genlock::PT_ABOUT_FIRST ), "About" ) == 0,
	       "the About block starts at 21 and is last" );
	bool aboutOnlyAtEnd = true;
	for( unsigned int id = 0; id < Genlock::PT_ABOUT_FIRST; ++id )
		if( plugin.GetParamType( id ) == FF_TYPE_TEXT || plugin.GetParamType( id ) == FF_TYPE_EVENT )
			aboutOnlyAtEnd = false;
	Check( aboutOnlyAtEnd, "no text or button parameter before it" );

	struct Expected
	{
		unsigned int id;
		const char* name;
		float value;
	};
	//Every control's default, the two new ones included. The old ones are
	//the values cf17b14 declared; nothing about them was meant to move.
	const Expected expected[] = {
		{ Genlock::PT_KEY_SOURCE, "Key Source", 0.0f },
		{ Genlock::PT_KEY_R, "Key Colour Red", 0.0f },
		{ Genlock::PT_KEY_G, "Key Colour Green", 0.333f },
		{ Genlock::PT_KEY_B, "Key Colour Blue", 0.667f },
		{ Genlock::PT_TOLERANCE, "Tolerance", 0.12f },
		{ Genlock::PT_SOFTNESS, "Softness", 0.06f },
		{ Genlock::PT_INVERT, "Invert", 0.0f },
		{ Genlock::PT_AMIGA_MODE, "Amiga Mode", 0.0f },
		{ Genlock::PT_KEY_DELAY, "Key Delay", 0.4375f },
		{ Genlock::PT_CLOCK_ERROR, "Clock Error", 0.30f },
		{ Genlock::PT_CRAWL_RATE, "Crawl Rate", 0.25f },
		{ Genlock::PT_CRAWL_WRAP, "Crawl Wrap", 0.0f },
		{ Genlock::PT_SYNC_QUALITY, "Sync Quality", 1.0f },
		{ Genlock::PT_ROLL_RATE, "Roll Rate", 0.125f },
		{ Genlock::PT_FADER, "Fader", 1.0f },
		{ Genlock::PT_DISSOLVE, "Dissolve", 0.5f },
		{ Genlock::PT_FRINGE, "Fringe", 0.35f },
		{ Genlock::PT_TINT_R, "Edge Tint Red", 0.25f },
		{ Genlock::PT_TINT_G, "Edge Tint Green", 0.95f },
		{ Genlock::PT_TINT_B, "Edge Tint Blue", 1.0f },
		{ Genlock::PT_OPACITY, "Opacity", 1.0f },
	};
	int wrong = 0;
	for( const Expected& e : expected )
	{
		const char* name = plugin.GetParamName( e.id );
		if( name == nullptr || std::strcmp( name, e.name ) != 0 || plugin.GetFloatParameter( e.id ) != e.value )
		{
			std::printf( "    %u: got \"%s\" = %g, want \"%s\" = %g\n", e.id, name ? name : "", plugin.GetFloatParameter( e.id ),
			             e.name, static_cast< double >( e.value ) );
			++wrong;
		}
	}
	Check( wrong == 0, fmt( "every control has its name, its place and its default (%.0f wrong)", wrong ) );

	Check( plugin.GetParamType( Genlock::PT_AMIGA_MODE ) == FF_TYPE_OPTION
	           && plugin.GetNumParamElements( Genlock::PT_AMIGA_MODE ) == AM_COUNT
	           && std::strcmp( plugin.GetParamElementName( Genlock::PT_AMIGA_MODE, 0 ), "Lores" ) == 0
	           && std::strcmp( plugin.GetParamElementName( Genlock::PT_AMIGA_MODE, 1 ), "Hires" ) == 0
	           && std::strcmp( plugin.GetParamElementName( Genlock::PT_AMIGA_MODE, 2 ), "Superhires" ) == 0,
	       "Amiga Mode is a dropdown: Lores, Hires, Superhires" );

	//The constants the defaults resolve to, against the old ones written
	//out as literals.
	Check( AmigaWidthForMode( AM_LORES ) == 320.0 && AmigaPixelClockHzForMode( AM_LORES ) == 7093790.0,
	       "lores is 320 across at 7093790 Hz, the old kAmigaWidth and kAmigaPixelClockHz" );
	Check( CrawlWrapAmigaPxFromParam( 0.0f ) == 1.0 && CrawlWrapAmigaPxFromParam( 1.0f ) == 16.0,
	       "Crawl Wrap's default is exactly one pixel, the old kCrawlWrapAmigaPx, and its top is sixteen" );
	Check( CrawlWrapParamFor( 1.0 ) == 0.0f && CrawlWrapParamFor( 16.0 ) == 1.0f && CrawlWrapParamFor( 8.5 ) == 0.5f,
	       "CrawlWrapParamFor inverts the map at both ends and the middle" );

	//The plugin at its defaults, frame by frame, against the old formula --
	//bitwise, because it is the same double arithmetic in the same order.
	//Clock Error is raised from its default so the crawl crosses the
	//one-pixel wrap inside the run: at the default 0.13 ppm it would not
	//wrap in a second, and a wrap that never happens is not being tested.
	const float fastClock = 0.6f;//1.66 ppm, 11.8 px/s: eleven wraps in 60 frames
	const int off         = framesOffOldFormula( {}, fastClock );
	Check( off == 0, fmt( "at the defaults the crawl and delay are the old formula's, bit for bit (%.0f of 60 frames differ)",
	                      off ) );

	//Negative controls: the same comparison, with each new control moved off
	//its default, must NOT match.
	const int offWrap = framesOffOldFormula( { { "Crawl Wrap", 1.0f } }, fastClock );
	Check( offWrap > 0, fmt( "negative control, Crawl Wrap at 16: the comparison FAILS (%.0f of 60 frames differ)", offWrap ) );
	const int offMode = framesOffOldFormula( { { "Amiga Mode", 1.0f } }, fastClock );
	Check( offMode > 0, fmt( "negative control, Amiga Mode at Hires: the comparison FAILS (%.0f of 60 frames differ)", offMode ) );

	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --mutation
//
// Proves the checks are measuring the GLSL that ships, not a copy of it:
// the plugin's own shader text, with one character changed, must fail one.
//---------------------------------------------------------------------------
int runMutation()
{
	std::printf( "one character of the shipped shader, changed, must fail a check\n\n" );

	//The key fetch: `uv - vec2( KeyDelay, 0.0 )`. Flipping the minus to a
	//plus delays the key the other way -- the fringe on the wrong side of
	//every edge -- and changes nothing else.
	const std::string shipped = kGenlockShader;
	const std::string site    = "uv - vec2( KeyDelay, 0.0 )";
	const size_t at           = shipped.find( site );
	Check( at != std::string::npos && shipped.find( site, at + 1 ) == std::string::npos,
	       "the mutation site occurs exactly once in the shipped shader" );
	if( at == std::string::npos )
		return 1;
	std::string mutant       = shipped;
	mutant[ at + 3 ]         = '+';
	int changed              = 0;
	for( size_t i = 0; i < shipped.size(); ++i )
		changed += shipped[ i ] != mutant[ i ];
	Check( changed == 1, fmt( "the mutant differs from the shipped shader in %.0f character", changed ) );

	//The default path and the test hook, given the SAME text, render the
	//same bytes -- so what the hook compiles is what the plugin compiles.
	const int width = 320, height = 180;
	Image viaDefault, viaHook;
	{
		Rig rig;
		if( !rig.Init( width, height ) )
			return 1;
		ModeSettings s;
		s.keyDelay = delayParam( -6.0 );
		const ModeRun run = renderModes( rig, s, 1 );
		viaDefault        = run.ok ? run.frames[ 0 ] : Image();
	}
	{
		g_fragmentForNextRig = shipped.c_str();
		Rig rig;
		const bool ready     = rig.Init( width, height );
		g_fragmentForNextRig = nullptr;
		if( !ready )
			return 1;
		ModeSettings s;
		s.keyDelay = delayParam( -6.0 );
		const ModeRun run = renderModes( rig, s, 1 );
		viaHook           = run.ok ? run.frames[ 0 ] : Image();
	}
	Check( !viaDefault.empty() && !viaHook.empty() && maxByteDifference( viaDefault, viaHook ) == 0,
	       "the shipped text through the hook renders the default path's bytes" );

	//And the mutant through the same hook fails the Key Delay check. The
	//check is re-run whole, with every one of its claims, and must fail.
	{
		//keyDelayAcrossModes builds its own rig, so the hook is reached
		//through the harness's rig-level default: g_fragmentForNextRig.
		g_fragmentForNextRig  = mutant.c_str();
		const Outcome outcome = keyDelayAcrossModes( width, height, Genlock::FAULT_NONE, false );
		g_fragmentForNextRig  = nullptr;
		expectFailure( outcome, "`uv - vec2( KeyDelay` mutated to `uv + vec2( KeyDelay` in the shipped GLSL" );
	}
	{
		g_fragmentForNextRig = shipped.c_str();
		const Outcome outcome = keyDelayAcrossModes( width, height, Genlock::FAULT_NONE, false );
		g_fragmentForNextRig  = nullptr;
		Check( outcome.pass, "and the unmutated text through the same hook passes it" + ( outcome.pass ? std::string()
		                                                                                                : " -- " + outcome.firstFailure ) );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( int width, int height, int frames, double fps )
{
	Rig rig;
	if( !rig.Init( width, height ) )
		return -1.0;
	rig.UploadDest( videoCard( width, height ) );
	rig.UploadSrc( amigaCard( width, height ) );
	rig.Set( "Sync Quality", 0.3f );//rolling: the most expensive path

	//A warm-up that is thrown away: the first frames pay for shader
	//specialisation, which is a real cost but not the per-frame cost.
	const int warmup = 20;
	for( int frame = 0; frame < warmup; ++frame )
		rig.Render( frame, fps );
	glFinish();

	//glFinish on both sides: GL calls queue, and without forcing completion
	//this times how fast the driver accepts commands, not how fast the GPU
	//runs them.
	const auto start = std::chrono::steady_clock::now();
	for( int frame = 0; frame < frames; ++frame )
		rig.Render( warmup + frame, fps );
	glFinish();
	const auto end = std::chrono::steady_clock::now();

	return std::chrono::duration< double >( end - start ).count() * 1000.0 / frames;
}

int runBench( int frames, double fps )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "2560x1440 ", 2560, 1440 },
		{ "3840x2160 ", 3840, 2160 },
	};

	std::printf( "%d frames each, after a 20-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "resolution     ms/frame   equivalent fps   %% of a 60fps frame\n" );

	for( const Size& size : sizes )
	{
		const double ms = benchAt( size.width, size.height, frames, fps );
		if( ms < 0.0 )
			return 1;
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%\n",
		             size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0, ms / 16.667 * 100.0 );
	}

	std::printf( "\nOne pass, three texture fetches: the video, the fill and the key.\n"
	             "Both inputs are at the output raster here, which is the common case.\n" );
	return 0;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"gltest -- render and measure the Genlock FFGL mixer\n"
		"\n"
		"  --out PATH        render both inputs through the plugin (default /tmp/genlock.png)\n"
		"  --input-a NAME    the DEST generator, i.e. the layer below (default video)\n"
		"  --input-b NAME    the SRC generator, i.e. this layer (default amiga)\n"
		"                    video | amiga | quads-a | quads-b | edge | bar | colour0 | flat\n"
		"  --card PATH       write input B alone, ungenlocked\n"
		"  --size WxH        picture size (default 1280x720); --width N / --height N also work\n"
		"  --frames N        frames to render before reading back (default 1)\n"
		"  --fps N           synthetic frame rate driving the two clocks (default 60)\n"
		"  --set \"Name=V\"    set a parameter by its display name. Repeatable.\n"
		"  --list            print every parameter, its kind, default and range, then exit\n"
		"  --names           no parameter or element name over 16 characters\n"
		"  --mixer           two inputs, two MaxUVs, and the missing-input guards\n"
		"  --delay           the key lags the fill by exactly the stated delay\n"
		"  --crawl           the delay walks at the rate the clock error predicts\n"
		"  --roll            the overlay rolls at the stated rate and wraps, at two rasters\n"
		"  --fader           at Video the output IS Dest\n"
		"  --modes           Amiga Mode and Crawl Wrap: what scales, what does not, at two rasters\n"
		"  --defaults        the new controls' defaults reproduce the old behaviour exactly\n"
		"  --mutation        one character of the shipped GLSL, changed, fails a check\n"
		"  --bench           time ProcessOpenGL at 720p through 4K\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath  = "/tmp/genlock.png";
	std::string cardPath;
	std::string inputA = "video";
	std::string inputB = "amiga";
	int width = 1280, height = 720;
	int frames  = 1;
	double fps  = 60.0;
	bool wantList = false, wantBench = false;
	std::string check;
	std::vector< std::string > settings;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;

		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--card" && hasNext )
			cardPath = argv[ ++i ];
		else if( argument == "--input-a" && hasNext )
			inputA = argv[ ++i ];
		else if( argument == "--input-b" && hasNext )
			inputB = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--names" || argument == "--mixer" || argument == "--delay"
		         || argument == "--crawl" || argument == "--roll" || argument == "--fader" || argument == "--modes"
		         || argument == "--defaults" || argument == "--mutation" )
			check = argument;
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	//The checks that need no GL context, answered before one is made.
	if( wantList )
		return runList();
	if( check == "--names" )
		return runNames();

	if( !cardPath.empty() )
	{
		if( !writePng( cardPath, width, height, generate( inputB, width, height ) ) )
		{
			std::fprintf( stderr, "could not write %s\n", cardPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s\n", cardPath.c_str() );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	int result = 0;
	if( check == "--mixer" )
		result = runMixer();
	else if( check == "--delay" )
		result = runDelay();
	else if( check == "--crawl" )
		result = runCrawl();
	else if( check == "--roll" )
		result = runRoll();
	else if( check == "--fader" )
		result = runFader();
	else if( check == "--modes" )
		result = runModes();
	else if( check == "--defaults" )
		result = runDefaults();
	else if( check == "--mutation" )
		result = runMutation();
	else if( wantBench )
		result = runBench( frames > 1 ? frames : 60, fps );
	else
	{
		Rig rig;
		if( !rig.Init( width, height ) )
			return 1;
		rig.UploadDest( generate( inputA, width, height ) );
		rig.UploadSrc( generate( inputB, width, height ) );

		for( const std::string& setting : settings )
		{
			const size_t equals = setting.find( '=' );
			if( equals == std::string::npos
			    || !rig.Set( setting.substr( 0, equals ), std::strtof( setting.substr( equals + 1 ).c_str(), nullptr ) ) )
			{
				std::fprintf( stderr, "--set %s: expected Name=Value with a known name (try --list)\n", setting.c_str() );
				return 2;
			}
		}

		if( !rig.RenderFrames( frames, fps ) )
		{
			std::fprintf( stderr, "ProcessOpenGL failed\n" );
			result = 1;
		}
		else if( !writePng( outPath, width, height, rig.Pixels() ) )
		{
			std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
			result = 1;
		}
		else
			std::printf( "wrote %s (%dx%d, %d frames, A=%s B=%s)\n", outPath.c_str(), width, height, frames,
			             inputA.c_str(), inputB.c_str() );
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result != 0 || failures != 0 ? 1 : 0;
}
