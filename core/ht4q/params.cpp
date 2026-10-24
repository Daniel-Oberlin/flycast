/*
	HydroThunder4Quest: runtime-tunable stereo parameters (env defaults, then the current game's
	(game.h), memserver 'S' updates).
*/
#include "game.h"
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
		envFloat("HT4Q_STEREO_CONV", 20.f),		// the game's own values: applyGameStereoDefaults
		envFloat("HT4Q_STEREO_HUDZ", 0.75f),
#ifdef HT4Q_XR
		envFloat("HT4Q_STEREO_HUD", 35.f),	// about 10.7 m away in the headset
#else
		envFloat("HT4Q_STEREO_HUD", 15.f),
#endif
		envFloat("HT4Q_UNITS_PER_METER", 3.28f),
		// the game's HUD sits at the edges of a 90 deg view: too far out to read in a headset
		envFloat("HT4Q_HUD_SHRINK_VR", 0.35f),
		// VR sky re-projection: the game's sky moves this many pixels per radian of camera turn
		// (0 = off)
		envFloat("HT4Q_SKY_CORRECT", 0.f),
		// 4:3 image covering about +-50 deg vertically: the Quest 3's per-eye view
		envFloat("HT4Q_VR_FOV", 116.f),
	};
	return params;
}

void applyGameStereoDefaults(const Game& game)
{
	StereoParams& p = stereoParams();
	p.conv = envFloat("HT4Q_STEREO_CONV", game.conv);
	p.hudMinZ = envFloat("HT4Q_STEREO_HUDZ", game.hudMinZ);
	p.unitsPerMeter = envFloat("HT4Q_UNITS_PER_METER", game.unitsPerMeter);
	p.skyCorrect = envFloat("HT4Q_SKY_CORRECT", game.skyCorrect);
}

}	// namespace ht4q
