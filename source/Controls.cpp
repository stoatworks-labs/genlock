#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace genlock
{
namespace
{
float clamp01( float value )
{
	return std::min( 1.0f, std::max( 0.0f, value ) );
}

constexpr double kClockErrorMinPpm = 0.01;
constexpr double kClockErrorMaxPpm = 50.0;

/// Lores, hires, superhires. EXACT powers of two, which is what lets the
/// crawl's picture-space speed be identical in all three modes rather than
/// merely close: scaling a double by 2 or 4 is exact, and IEEE division of
/// the same real quotient rounds to the same bits.
constexpr double kModeScale[ AM_COUNT ] = { 1.0, 2.0, 4.0 };

double modeScale( int mode )
{
	return kModeScale[ std::clamp( mode, 0, static_cast< int >( AM_COUNT ) - 1 ) ];
}
} // namespace

double AmigaWidthForMode( int mode )
{
	return kAmigaLoresWidth * modeScale( mode );
}

double AmigaPixelClockHzForMode( int mode )
{
	return kAmigaLoresPixelClockHz * modeScale( mode );
}

float KeyDelayFromParam( float value )
{
	return ( clamp01( value ) - 0.5f ) * 16.0f;
}

float ToleranceFromParam( float value )
{
	return clamp01( value );
}

float SoftnessFromParam( float value )
{
	return clamp01( value ) * 0.5f;
}

float ClockErrorPpmFromParam( float value )
{
	const double t = static_cast< double >( clamp01( value ) );
	return static_cast< float >( kClockErrorMinPpm * std::pow( kClockErrorMaxPpm / kClockErrorMinPpm, t ) );
}

float CrawlRateFromParam( float value )
{
	return clamp01( value ) * 4.0f;
}

float CrawlWrapAmigaPxFromParam( float value )
{
	return static_cast< float >( kCrawlWrapMinAmigaPx
	                             + static_cast< double >( clamp01( value ) )
	                                   * ( kCrawlWrapMaxAmigaPx - kCrawlWrapMinAmigaPx ) );
}

float CrawlWrapParamFor( double amigaPixels )
{
	const double t = ( amigaPixels - kCrawlWrapMinAmigaPx ) / ( kCrawlWrapMaxAmigaPx - kCrawlWrapMinAmigaPx );
	return clamp01( static_cast< float >( t ) );
}

float RollRateFromParam( float value )
{
	return clamp01( value ) * 4.0f;
}

double CrawlRateAmigaPxPerSecond( float clockErrorParam, float crawlRateParam, int mode )
{
	const double ppm = static_cast< double >( ClockErrorPpmFromParam( clockErrorParam ) );
	return AmigaPixelClockHzForMode( mode ) * ppm * 1e-6 * static_cast< double >( CrawlRateFromParam( crawlRateParam ) );
}

} // namespace genlock
