/*
	HydroThunder4Quest: controller mapping settings (core/ht4q/controls.cpp), for the launcher.
*/
#pragma once
#include "ht4q.h"
#include <array>

namespace ht4q
{
// Functions the controllers can be mapped to: game buttons and triggers, then the view adjustment
// (pause, reset, and the two stick axes that change the HUD size and the depth effect while paused)
enum Func { F_THROTTLE, F_BRAKE, F_BOOST, F_CAMERA, F_B, F_X, F_START, F_ADJUST, F_RESET_VIEW,
	F_HUD_AXIS, F_DEPTH_AXIS, F_COUNT };
// stick axes, numbered after the XrInputs
enum StickAxis { XA_LX = XI_COUNT, XA_LY, XA_RX, XA_RY, XA_END };
using Binding = std::array<int, 2>;	// up to two XrInput (an axis function: StickAxis), -1 = none
enum class Steering { RightStick, LeftStick, Tilt };

namespace controls
{
const char *funcLabel(int f);
bool funcIsAxis(int f);
const char *inputLabel(int in);
Binding binding(int f);
void setBinding(int f, const Binding& b);
void resetDefaults();

Steering steering();
void setSteering(Steering s);
bool tiltSpeed();		// tilt forward/back also drives throttle/brake
void setTiltSpeed(bool on);
int tiltRange();		// degrees of tilt for full lock / full throttle
void setTiltRange(int deg);
void calibrateTilt();	// the controllers' current pitch becomes "no throttle"
void tiltAxes(const XrControllers& c, float& steer, float& speed);
int hapticStrength();	// %, 0 = off
void setHapticStrength(int pct);

// View adjustment (in game: F_ADJUST pauses, the axes change these, F_RESET_VIEW resets them)
int hudSize();			// % of the game's HUD size
void setHudSize(int pct);
int depthEffect();		// % of the real eye separation
void setDepthEffect(int pct);
void resetView();
}
}
