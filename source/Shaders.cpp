#include "Shaders.h"

namespace genlock
{

//---------------------------------------------------------------------------
// The vertex shader passes UV through UNSCALED, which is not what the SDK's
// own mixer example does.
//
// `Add` folds MaxUVDest and MaxUVSrc in here and hands the fragment shader
// two ready-made texture coordinates. That is fine for a plugin that only
// ever fetches at the fragment's own position. This one fetches the key at a
// displaced position, and a displacement expressed in one input's texture
// space is the wrong size in the other's -- so every coordinate here stays in
// picture space and MaxUV is applied once, at each fetch, inside the
// fragment shader.
//---------------------------------------------------------------------------
const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// The genlock.
//---------------------------------------------------------------------------
const char* const kGenlockShader = R"(#version 410 core

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
)";

} // namespace genlock
