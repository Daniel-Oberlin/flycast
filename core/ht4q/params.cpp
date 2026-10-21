/*
	HydroThunder4Quest: runtime-tunable stereo parameters (env defaults, memserver 'S' updates).
*/
#include "ht4q.h"
#include <cstdlib>

namespace ht4q
{

static float envFloat(const char *name, float def)
{
	const char *v = getenv(name);
	return v != nullptr && v[0] != '\0' ? (float)atof(v) : def;
}

StereoParams& stereoParams()
{
	static StereoParams params {
#ifdef HT4Q_XR
		// 2.4 m screen at 2 m: 64 mm total disparity at infinity = 2.67% of width = 0.0267 NDC per eye
		envFloat("HT4Q_STEREO_SEP", 0.0267f),
#else
		envFloat("HT4Q_STEREO_SEP", 0.015f),
#endif
		envFloat("HT4Q_STEREO_CONV", 20.f),
		envFloat("HT4Q_STEREO_HUDZ", 0.75f),
#ifdef HT4Q_XR
		envFloat("HT4Q_STEREO_HUD", 35.f),	// about 10.7 m away in the headset
#else
		envFloat("HT4Q_STEREO_HUD", 15.f),
#endif
		// guess: feet (eye 7.5 units above the boat, chase camera ~18 behind)
		envFloat("HT4Q_UNITS_PER_METER", 3.28f),
		// the game's HUD sits at the edges of a 90 deg view: too far out to read in a headset
		envFloat("HT4Q_HUD_SHRINK_VR", 0.35f),
		// VR sky re-projection: the game's sky moves this many pixels per radian of camera turn
		// (0 = off)
		envFloat("HT4Q_SKY_CORRECT", 270.f),
		// 4:3 image covering about +-50 deg vertically: the Quest 3's per-eye view
		envFloat("HT4Q_VR_FOV", 116.f),
	};
	return params;
}

}	// namespace ht4q
