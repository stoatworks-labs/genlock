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
} // namespace

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

float RollRateFromParam( float value )
{
	return clamp01( value ) * 4.0f;
}

double CrawlRateAmigaPxPerSecond( float clockErrorParam, float crawlRateParam )
{
	const double ppm = static_cast< double >( ClockErrorPpmFromParam( clockErrorParam ) );
	return kAmigaPixelClockHz * ppm * 1e-6 * static_cast< double >( CrawlRateFromParam( crawlRateParam ) );
}

} // namespace genlock
