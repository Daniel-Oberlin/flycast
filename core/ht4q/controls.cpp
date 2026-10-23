/*
	HydroThunder4Quest: headset controllers -> Dreamcast pad on maple port A, and Dreamcast rumble
	-> controller haptics.

	The XR presenter hands us the raw Quest Touch state every frame (setXrControllers). The player's
	mapping (launcher settings, saved in emu.cfg [ht4q]) assigns up to two controller inputs to each
	game function; steering comes from a thumbstick or from tilting a controller left/right, speed
	from the mapped throttle/brake inputs and optionally from tilting a controller forward/back.
	The memserver's virtual controller, applied after this, still overrides it for scripted tests.

	View adjustment: F_ADJUST (right stick press) pauses a race with the game's own pause (Start),
	whose screen keeps drawing the scene, and hands the sticks to the HUD size and depth effect;
	pressing it again resumes. F_RESET_VIEW (left stick press) resets both, any time.
*/
#include "ht4q.h"
#include "controls.h"
#include "cfg/option.h"
#include "emulator.h"
#include "hw/maple/maple_cfg.h"
#include "input/gamepad.h"
#include "log/Log.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <sstream>

namespace ht4q
{

static config::Option<std::string, false> ButtonMap("ButtonMap", "", "ht4q");
static config::Option<int, false> SteeringSource("Steering", (int)Steering::RightStick, "ht4q");
static config::Option<bool, false> TiltSpeedOpt("TiltSpeed", false, "ht4q");
static config::Option<int, false> TiltRangeOpt("TiltRange", 30, "ht4q");		// degrees for full lock / full throttle
static config::Option<int, false> TiltNeutralOpt("TiltNeutral", 0, "ht4q");	// controller pitch held for no throttle, degrees
static config::Option<int, false> HapticsOpt("Haptics", 70, "ht4q");			// strength %, 0: off
static config::Option<int, false> HudSizeOpt("HudSize", 65, "ht4q");			// % of the game's HUD size
static config::Option<int, false> DepthEffectOpt("DepthEffect", 100, "ht4q");	// % of the real eye separation

static const struct {
	const char *key;
	const char *label;
	u32 dcButton;		// 0: analog (throttle, brake) or view adjustment
	Binding defaults;
} funcs[] = {
	{ "throttle", "Throttle", 0, { XI_RTRIGGER, -1 } },
	{ "brake", "Brake / reverse", 0, { XI_LTRIGGER, -1 } },
	// boost is held while steering with the right stick, so it goes on the left hand and the right grip
	{ "boost", "Boost (DC A)", DC_BTN_A, { XI_X, XI_RGRIP } },
	{ "camera", "Change view (DC Y)", DC_BTN_Y, { XI_A, -1 } },
	{ "b", "DC B", DC_BTN_B, { XI_B, -1 } },
	{ "x", "DC X", DC_BTN_X, { XI_Y, -1 } },
	{ "start", "Start / pause", DC_BTN_START, { XI_MENU, -1 } },
	{ "adjust", "Adjust view (pause)", 0, { XI_RSTICK, -1 } },
	{ "resetview", "Reset view", 0, { XI_LSTICK, -1 } },
	{ "hudaxis", "HUD size (adjusting)", 0, { XA_RY, -1 } },
	{ "depthaxis", "Depth effect (adjusting)", 0, { XA_LY, -1 } },
};
static_assert(std::size(funcs) == F_COUNT, "functions");

static const char *inputKeys[XA_END] = { "a", "b", "x", "y", "menu", "lstick", "rstick", "ltrigger", "rtrigger",
	"lgrip", "rgrip", "lx", "ly", "rx", "ry" };
static const char *inputLabels[XA_END] = { "A", "B", "X", "Y", "Menu", "Left stick press", "Right stick press",
	"Left trigger", "Right trigger", "Left grip", "Right grip",
	"Left stick left/right", "Left stick fwd/back", "Right stick left/right", "Right stick fwd/back" };

static std::mutex mapMutex;
static Binding bindings[F_COUNT];
static bool bindingsLoaded;

// "throttle=rtrigger,boost=x+rgrip,..." - functions not listed keep their defaults
static void loadBindings()
{
	for (int f = 0; f < F_COUNT; f++)
		bindings[f] = funcs[f].defaults;
	std::stringstream ss(ButtonMap.get());
	std::string item;
	while (std::getline(ss, item, ','))
	{
		size_t eq = item.find('=');
		if (eq == std::string::npos)
			continue;
		std::string key = item.substr(0, eq), val = item.substr(eq + 1);
		for (int f = 0; f < F_COUNT; f++)
		{
			if (key != funcs[f].key)
				continue;
			Binding b { -1, -1 };
			std::stringstream vs(val);
			std::string in;
			for (int slot = 0; slot < 2 && std::getline(vs, in, '+'); )
				for (int i = 0; i < XA_END; i++)
					if (in == inputKeys[i])
						b[slot++] = i;
			bindings[f] = b;
		}
	}
	bindingsLoaded = true;
}

static void storeBindings()
{
	std::string s;
	for (int f = 0; f < F_COUNT; f++)
	{
		s += std::string(s.empty() ? "" : ",") + funcs[f].key + "=";
		bool first = true;
		for (int in : bindings[f])
			if (in >= 0)
			{
				s += std::string(first ? "" : "+") + inputKeys[in];
				first = false;
			}
	}
	ButtonMap.set(s);
}

namespace controls
{
const char *funcLabel(int f) { return funcs[f].label; }
bool funcIsAxis(int f) { return f == F_HUD_AXIS || f == F_DEPTH_AXIS; }
const char *inputLabel(int in) { return in >= 0 && in < XA_END ? inputLabels[in] : "-"; }

Binding binding(int f)
{
	std::lock_guard<std::mutex> lock(mapMutex);
	if (!bindingsLoaded)
		loadBindings();
	return bindings[f];
}

void setBinding(int f, const Binding& b)
{
	std::lock_guard<std::mutex> lock(mapMutex);
	if (!bindingsLoaded)
		loadBindings();
	bindings[f] = b;
	storeBindings();
}

void resetDefaults()
{
	std::lock_guard<std::mutex> lock(mapMutex);
	ButtonMap.set("");
	loadBindings();
	SteeringSource.set((int)Steering::RightStick);
	TiltSpeedOpt.set(false);
	TiltRangeOpt.set(30);
	HapticsOpt.set(70);
}

Steering steering() { return (Steering)SteeringSource.get(); }
void setSteering(Steering s) { SteeringSource.set((int)s); }
bool tiltSpeed() { return TiltSpeedOpt; }
void setTiltSpeed(bool on) { TiltSpeedOpt.set(on); }
int tiltRange() { return TiltRangeOpt; }
void setTiltRange(int deg) { TiltRangeOpt.set(std::clamp(deg, 10, 60)); }
int hapticStrength() { return HapticsOpt; }
void setHapticStrength(int pct) { HapticsOpt.set(std::clamp(pct, 0, 100)); }
int hudSize() { return HudSizeOpt; }
void setHudSize(int pct) { HudSizeOpt.set(std::clamp(pct, 30, 120)); }
int depthEffect() { return DepthEffectOpt; }
void setDepthEffect(int pct) { DepthEffectOpt.set(std::clamp(pct, 0, 300)); }
void resetView()
{
	HudSizeOpt.set(65);
	DepthEffectOpt.set(100);
}
}	// namespace controls

float depthEffectFactor() { return DepthEffectOpt / 100.f; }

static std::mutex xrInputMutex;
static bool xrInputActive;
static XrControllers xrState;

void setXrControllers(bool active, const XrControllers& c)
{
	std::lock_guard<std::mutex> lock(xrInputMutex);
	xrInputActive = active;
	xrState = c;
}

bool xrControllers(XrControllers& out)
{
	std::lock_guard<std::mutex> lock(xrInputMutex);
	out = xrState;
	return xrInputActive;
}

static void rotate(const float q[4], const float v[3], float out[3])
{
	// v + 2w (u x v) + 2 u x (u x v), u = q.xyz
	const float t[3] { 2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0]) };
	out[0] = v[0] + q[3] * t[0] + q[1] * t[2] - q[2] * t[1];
	out[1] = v[1] + q[3] * t[1] + q[2] * t[0] - q[0] * t[2];
	out[2] = v[2] + q[3] * t[2] + q[0] * t[1] - q[1] * t[0];
}

// A controller's tilt in degrees: roll (+ = tilted right, right side down) and pitch (+ = nose up)
static void tilt(const XrHand& h, float& roll, float& pitch)
{
	static const float right[3] { 1, 0, 0 }, forward[3] { 0, 0, -1 };
	float r[3], f[3];
	rotate(h.q, right, r);
	rotate(h.q, forward, f);
	constexpr float deg = 180.f / (float)M_PI;
	roll = -std::asin(std::clamp(r[1], -1.f, 1.f)) * deg;
	pitch = std::asin(std::clamp(f[1], -1.f, 1.f)) * deg;
}

namespace controls
{
void calibrateTilt()
{
	XrControllers c;
	xrControllers(c);
	float sum = 0, n = 0;
	for (const XrHand& h : c.hand)
		if (h.tracked)
		{
			float roll, pitch;
			tilt(h, roll, pitch);
			sum += pitch;
			n++;
		}
	if (n > 0)
		TiltNeutralOpt.set((int)std::lround(sum / n));
}

// Steering and speed from tilt, -1..1, the hand tilted furthest wins (either hand works alone)
void tiltAxes(const XrControllers& c, float& steer, float& speed)
{
	steer = speed = 0.f;
	const float range = (float)TiltRangeOpt.get(), deadZone = 3.f;
	auto axis = [&](float a) {
		a = std::fabs(a) < deadZone ? 0.f : a - std::copysign(deadZone, a);
		return std::clamp(a / (range - deadZone), -1.f, 1.f);
	};
	for (const XrHand& h : c.hand)
		if (h.tracked)
		{
			float roll, pitch;
			tilt(h, roll, pitch);
			float s = axis(roll), v = axis(TiltNeutralOpt - pitch);	// nose down = faster
			if (std::fabs(s) > std::fabs(steer))
				steer = s;
			if (std::fabs(v) > std::fabs(speed))
				speed = v;
		}
}
}	// namespace controls

static float axisValue(const XrControllers& c, int axis)
{
	switch (axis)
	{
	case XA_LX: return c.hand[0].stick[0];
	case XA_LY: return c.hand[0].stick[1];
	case XA_RX: return c.hand[1].stick[0];
	case XA_RY: return c.hand[1].stick[1];
	default: return 0.f;
	}
}

// View adjustment state (emu thread)
static constexpr u32 ATTACHED_PHYS = 0x8c1fc988;	// non-null while racing
static constexpr u32 GAME_PAUSED = 0x8c259a08;		// 1 while the game's pause screen is up
static std::atomic<bool> adjusting;
static int startPulse;		// polls left holding Start to (un)pause the game
static int pauseCheck;		// polls until we check the game followed

static std::atomic<bool> adjustToggle;	// test hook: as if the adjust button was pressed

bool adjustingView() { return adjusting; }
void toggleAdjust() { adjustToggle = true; }

static bool racing()
{
	u32 phys = readRam32(ATTACHED_PHYS);
	return (phys & 0x1f000000) == 0x0c000000;
}

// Holds Start for a few polls: the game toggles its pause screen
static void pulseStart()
{
	startPulse = 6;
	pauseCheck = 40;
}

void applyXrInput(MapleInputState inputState[4])
{
	XrControllers c;
	if (!xrControllers(c))
		return;
	Binding b[F_COUNT];
	for (int f = 0; f < F_COUNT; f++)
		b[f] = controls::binding(f);
	auto value = [&](int f) {
		float v = 0.f;
		for (int in : b[f])
			if (in >= 0 && in < XI_COUNT)
				v = std::max(v, c.value[in]);
		return v;
	};
	auto axis = [&](int f) {
		float v = 0.f;
		for (int in : b[f])
			if (in >= XI_COUNT && std::fabs(axisValue(c, in)) > std::fabs(v))
				v = axisValue(c, in);
		return v;
	};
	MapleInputState& st = inputState[0];
	stereoParams().hudShrinkVr = 1.f - controls::hudSize() / 100.f;

	// view adjustment buttons: act on press
	static bool adjustHeld, resetHeld;
	const bool adjustDown = value(F_ADJUST) > 0.5f, resetDown = value(F_RESET_VIEW) > 0.5f;
	if (resetDown && !resetHeld)
		controls::resetView();
	if ((adjustDown && !adjustHeld) || adjustToggle.exchange(false))
	{
		adjusting = !adjusting;
		const bool paused = readRam32(GAME_PAUSED) == 1;
		if (racing() && paused != adjusting)
			pulseStart();
		if (!adjusting)
			SaveSettings();
	}
	adjustHeld = adjustDown;
	resetHeld = resetDown;
	if (startPulse > 0)
	{
		startPulse--;
		st.kcode &= ~DC_BTN_START;
		return;
	}
	if (pauseCheck > 0 && --pauseCheck == 0 && racing() && (readRam32(GAME_PAUSED) == 1) != adjusting)
		pulseStart();		// the game missed it (e.g. mid-transition): try again
	if (adjusting)
	{
		// the game gets nothing: the sticks change the view, about 50% per second at full tilt
		auto stick = [](float a) { return std::fabs(a) < 0.15f ? 0.f : a; };
		static float hudAcc, depthAcc;
		hudAcc += stick(axis(F_HUD_AXIS)) * 50.f / 60.f;
		depthAcc += stick(axis(F_DEPTH_AXIS)) * 50.f / 60.f;
		if (std::fabs(hudAcc) >= 1.f)
		{
			controls::setHudSize(controls::hudSize() + (int)hudAcc);
			hudAcc -= (int)hudAcc;
		}
		if (std::fabs(depthAcc) >= 1.f)
		{
			controls::setDepthEffect(controls::depthEffect() + (int)depthAcc);
			depthAcc -= (int)depthAcc;
		}
		return;
	}

	float throttle = value(F_THROTTLE), brake = value(F_BRAKE);
	for (int f = 0; f < F_COUNT; f++)
		if (funcs[f].dcButton != 0 && value(f) > 0.5f)
			st.kcode &= ~funcs[f].dcButton;

	float steerX = 0.f, stickY = 0.f;
	const Steering steering = controls::steering();
	if (steering == Steering::Tilt || controls::tiltSpeed())
	{
		float tiltSteer, tiltSpeed;
		controls::tiltAxes(c, tiltSteer, tiltSpeed);
		if (steering == Steering::Tilt)
			steerX = tiltSteer;
		if (controls::tiltSpeed())
		{
			throttle = std::max(throttle, tiltSpeed);
			brake = std::max(brake, -tiltSpeed);
		}
	}
	if (steering != Steering::Tilt)
	{
		const XrHand& h = c.hand[steering == Steering::LeftStick ? 0 : 1];
		// small dead zone; steering softer around center (slope 0.7), still full lock at the edge
		auto dz = [](float a) { return std::fabs(a) < 0.08f ? 0.f : std::clamp(a, -1.f, 1.f); };
		const float x = dz(h.stick[0]);
		steerX = 0.7f * x + 0.3f * x * x * x;
		stickY = dz(h.stick[1]);
	}
	st.halfAxes[PJTI_L] = std::max(st.halfAxes[PJTI_L], (u16)std::lround(std::clamp(brake, 0.f, 1.f) * 0xffff));
	st.halfAxes[PJTI_R] = std::max(st.halfAxes[PJTI_R], (u16)std::lround(std::clamp(throttle, 0.f, 1.f) * 0xffff));
	if (steerX != 0.f || stickY != 0.f)
	{
		st.fullAxes[PJAI_X1] = (s16)std::lround(steerX * 32767);
		st.fullAxes[PJAI_Y1] = (s16)std::lround(-stickY * 32767);	// Dreamcast stick y is down-positive
	}
}

static std::mutex hapticsMutex;
static Haptics lastHaptics;

void onVibration(u32 port, float power, float inclination, u32 durationMs)
{
	if (port != 0)
		return;
	std::lock_guard<std::mutex> lock(hapticsMutex);
	lastHaptics.seq++;
	lastHaptics.amplitude = std::clamp(power, 0.f, 1.f) * HapticsOpt / 100.f;
	lastHaptics.durationMs = durationMs;
	if (lastHaptics.seq <= 3)
		NOTICE_LOG(INPUT, "ht4q rumble: power %.2f inclination %.3f %u ms", power, inclination, durationMs);
}

Haptics haptics()
{
	std::lock_guard<std::mutex> lock(hapticsMutex);
	return lastHaptics;
}

}	// namespace ht4q
