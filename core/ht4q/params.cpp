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
		envFloat("HT4Q_STEREO_HUD", 15.f),
	};
	return params;
}

}	// namespace ht4q
