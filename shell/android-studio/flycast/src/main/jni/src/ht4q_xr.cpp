/*
	HydroThunder4Quest: OpenXR presentation for Meta Quest (quest flavor, HT4Q_XR).

	The Android window surface is destroyed once the app turns immersive, so Flycast renders
	into a 4:3 pbuffer instead (its "screen"). On each swap we copy the pbuffer into an
	OpenXR swapchain and submit it as a world-locked quad layer ("screen
	mode"), so everything Flycast draws - game, ImGui menus, OSD - shows up in the headset.
	The XR frame loop runs on Flycast's render thread: one xrWaitFrame/xrEndFrame per swap.

	In game, Flycast's stereo renderer (core/rend/gles/ht4q_stereo.cpp, Xr mode) keeps a
	separate image per eye; we then show the screen as two quads, one per eye ("3D TV").
	Menus (Flycast GUI open) stay mono. While racing, the head drives the game camera (camhook)
	and the eye images go out as a projection layer with the head pose and game frustum
	they were drawn with, so the compositor's reprojection keeps the world steady.
*/
#ifdef HT4Q_XR
#include "types.h"
#include "wsi/egl.h"
#include "jni_util.h"
#include "ht4q_xr.h"
#include "stdclass.h"
#include "cfg/option.h"
#include "input/gamepad.h"
#include <algorithm>
#include "ui/vgamepad.h"
#include "ui/gui.h"
#include "rend/gles/ht4q_stereo.h"
#include "ht4q/ht4q.h"
#include <cmath>
#include <stb_image_write.h>
#include <unistd.h>
#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <chrono>
#include <thread>
#include <vector>

extern jobject g_activity;

namespace ht4q
{

#define XR_CHECK(call)                                                       \
	do {                                                                     \
		XrResult _r = (call);                                                \
		if (XR_FAILED(_r)) {                                                 \
			ERROR_LOG(RENDERER, "ht4q xr: %s failed: %d", #call, (int)_r);   \
			return false;                                                    \
		}                                                                    \
	} while (0)

// Virtual screen: 4:3, 2.4 m wide, 2 m in front of the starting head position.
static constexpr float SCREEN_WIDTH = 2.4f;
static constexpr float SCREEN_DISTANCE = 2.0f;
static constexpr int SCREEN_PIXELS_W = 1440, SCREEN_PIXELS_H = 1080;
static constexpr int MIN_RENDER_LINES = 960;

static EGLConfig contextConfig(EGLDisplay display, EGLContext context)
{
	EGLint configId = 0, numConfigs = 0;
	eglQueryContext(display, context, EGL_CONFIG_ID, &configId);
	EGLint attribs[] = { EGL_CONFIG_ID, configId, EGL_NONE };
	EGLConfig config = nullptr;
	eglChooseConfig(display, attribs, &config, 1, &numConfigs);
	return config;
}

class XrGLGC : public EGLGraphicsContext
{
public:
	XrGLGC(void *window, void *display) : EGLGraphicsContext(window, display) {
		if (!usePbuffer())
			ERROR_LOG(RENDERER, "ht4q xr: no pbuffer, rendering will stop when the window goes away");
		if (!initXr())
			ERROR_LOG(RENDERER, "ht4q xr: OpenXR unavailable, falling back to the flat window");
		vgamepad::hide();	// no touchscreen on Quest
		// each eye gets the full output image; native 480 lines is too soft in a headset
		if (config::RenderResolution < MIN_RENDER_LINES)
			config::RenderResolution = MIN_RENDER_LINES;
	}
	~XrGLGC() { termXr(); }

	void swap() override;

private:
	bool usePbuffer();
	bool initXr();
	void termXr();
	void pollEvents();
	struct Swapchain {
		XrSwapchain handle = XR_NULL_HANDLE;
		int width = 0, height = 0;
		std::vector<XrSwapchainImageOpenGLESKHR> images;
	};
	bool createSwapchain(Swapchain& sc, int width, int height);
	bool acquire(Swapchain& sc);
	bool submitFrame();
	void trackHead(XrTime time);
	bool createActions();
	void pollControllers();

	XrActionSet actionSet = XR_NULL_HANDLE;
	XrAction actThrottle = XR_NULL_HANDLE, actBrake = XR_NULL_HANDLE, actSteer = XR_NULL_HANDLE;
	XrAction actButton[5] {};	// A, B, X, Y, Start

	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSpace space = XR_NULL_HANDLE;		// LOCAL: recentered, gravity aligned
	XrSpace viewSpace = XR_NULL_HANDLE;	// the head
	XrPosef eyeInHead[2] {};			// eye poses relative to the head
	XrSessionState state = XR_SESSION_STATE_UNKNOWN;
	bool running = false;

	Swapchain mono;		// the pbuffer: menus, flat fallback
	Swapchain stereo;	// left eye | right eye, side by side
	GLuint fbo = 0;
};

bool XrGLGC::usePbuffer()
{
	EGLConfig config = contextConfig(display, context);
	EGLint types = 0;
	eglGetConfigAttrib(display, config, EGL_SURFACE_TYPE, &types);
	if (!(types & EGL_PBUFFER_BIT))
		return false;
	EGLint attribs[] = { EGL_WIDTH, SCREEN_PIXELS_W, EGL_HEIGHT, SCREEN_PIXELS_H, EGL_NONE };
	EGLSurface pbuffer = eglCreatePbufferSurface(display, config, attribs);
	if (pbuffer == EGL_NO_SURFACE || !eglMakeCurrent(display, pbuffer, pbuffer, context))
		return false;
	eglDestroySurface(display, surface);
	surface = pbuffer;
	settings.display.width = SCREEN_PIXELS_W;
	settings.display.height = SCREEN_PIXELS_H;
	NOTICE_LOG(RENDERER, "ht4q xr: rendering into a %dx%d pbuffer", SCREEN_PIXELS_W, SCREEN_PIXELS_H);
	return true;
}

bool XrGLGC::initXr()
{
	PFN_xrInitializeLoaderKHR initLoader = nullptr;
	XR_CHECK(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&initLoader));
	JavaVM *vm = nullptr;
	jni::env()->GetJavaVM(&vm);
	XrLoaderInitInfoAndroidKHR loaderInfo{ XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
	loaderInfo.applicationVM = vm;
	loaderInfo.applicationContext = g_activity;
	XR_CHECK(initLoader((const XrLoaderInitInfoBaseHeaderKHR *)&loaderInfo));

	const char *extensions[] = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME };
	XrInstanceCreateInfoAndroidKHR androidInfo{ XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	androidInfo.applicationVM = vm;
	androidInfo.applicationActivity = g_activity;
	XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
	ici.next = &androidInfo;
	strcpy(ici.applicationInfo.applicationName, "HydroThunder4Quest");
	ici.applicationInfo.applicationVersion = 1;
	strcpy(ici.applicationInfo.engineName, "Flycast");
	ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	ici.enabledExtensionCount = std::size(extensions);
	ici.enabledExtensionNames = extensions;
	XR_CHECK(xrCreateInstance(&ici, &instance));

	XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XR_CHECK(xrGetSystem(instance, &sgi, &systemId));

	PFN_xrGetOpenGLESGraphicsRequirementsKHR getReqs = nullptr;
	XR_CHECK(xrGetInstanceProcAddr(instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getReqs));
	XrGraphicsRequirementsOpenGLESKHR reqs{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
	XR_CHECK(getReqs(instance, systemId, &reqs));

	XrGraphicsBindingOpenGLESAndroidKHR binding{ XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
	binding.display = display;
	binding.config = contextConfig(display, context);
	binding.context = context;
	XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
	sci.next = &binding;
	sci.systemId = systemId;
	XR_CHECK(xrCreateSession(instance, &sci, &session));

	XrReferenceSpaceCreateInfo rsci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rsci.poseInReferenceSpace.orientation.w = 1.f;
	XR_CHECK(xrCreateReferenceSpace(session, &rsci, &space));
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	XR_CHECK(xrCreateReferenceSpace(session, &rsci, &viewSpace));

	if (!createActions())
		WARN_LOG(RENDERER, "ht4q xr: controller actions unavailable");
	glGenFramebuffers(1, &fbo);
	NOTICE_LOG(RENDERER, "ht4q xr: session created; shots via %s", get_writable_data_path("../ht4q_shot.req").c_str());
	return true;
}

void XrGLGC::termXr()
{
	for (Swapchain *sc : { &mono, &stereo })
		if (sc->handle != XR_NULL_HANDLE)
			xrDestroySwapchain(sc->handle);
	mono = {};
	stereo = {};
	if (viewSpace != XR_NULL_HANDLE)
		xrDestroySpace(viewSpace);
	if (space != XR_NULL_HANDLE)
		xrDestroySpace(space);
	viewSpace = XR_NULL_HANDLE;
	if (actionSet != XR_NULL_HANDLE)
		xrDestroyActionSet(actionSet);
	actionSet = XR_NULL_HANDLE;
	if (session != XR_NULL_HANDLE)
		xrDestroySession(session);
	if (instance != XR_NULL_HANDLE)
		xrDestroyInstance(instance);
	if (fbo != 0)
		glDeleteFramebuffers(1, &fbo);
	space = XR_NULL_HANDLE;
	session = XR_NULL_HANDLE;
	instance = XR_NULL_HANDLE;
	fbo = 0;
}

void XrGLGC::pollEvents()
{
	XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
	while (xrPollEvent(instance, &ev) == XR_SUCCESS)
	{
		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
		{
			state = ((XrEventDataSessionStateChanged *)&ev)->state;
			NOTICE_LOG(RENDERER, "ht4q xr: session state %d", (int)state);
			if (state == XR_SESSION_STATE_READY && !running)
			{
				XrSessionBeginInfo sbi{ XR_TYPE_SESSION_BEGIN_INFO };
				sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				running = XR_SUCCEEDED(xrBeginSession(session, &sbi));
			}
			else if (state == XR_SESSION_STATE_STOPPING && running)
			{
				xrEndSession(session);
				running = false;
			}
		}
		ev = { XR_TYPE_EVENT_DATA_BUFFER };
	}
}

bool XrGLGC::createSwapchain(Swapchain& sc, int width, int height)
{
	if (sc.handle != XR_NULL_HANDLE && width == sc.width && height == sc.height)
		return true;
	if (sc.handle != XR_NULL_HANDLE)
		xrDestroySwapchain(sc.handle);
	sc = {};

	XrSwapchainCreateInfo sci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
	sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	sci.format = GL_SRGB8_ALPHA8;
	sci.sampleCount = 1;
	sci.width = width;
	sci.height = height;
	sci.faceCount = 1;
	sci.arraySize = 1;
	sci.mipCount = 1;
	XR_CHECK(xrCreateSwapchain(session, &sci, &sc.handle));

	u32 count = 0;
	XR_CHECK(xrEnumerateSwapchainImages(sc.handle, 0, &count, nullptr));
	sc.images.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR });
	XR_CHECK(xrEnumerateSwapchainImages(sc.handle, count, &count, (XrSwapchainImageBaseHeader *)sc.images.data()));
	sc.width = width;
	sc.height = height;
	NOTICE_LOG(RENDERER, "ht4q xr: swapchain %dx%d, %u images", width, height, count);
	return true;
}

// Acquires the next image and binds it as the draw framebuffer, ready for blits.
bool XrGLGC::acquire(Swapchain& sc)
{
	u32 index;
	XR_CHECK(xrAcquireSwapchainImage(sc.handle, nullptr, &index));
	XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	wi.timeout = XR_INFINITE_DURATION;
	XR_CHECK(xrWaitSwapchainImage(sc.handle, &wi));
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc.images[index].image, 0);
	glDisable(GL_SCISSOR_TEST);
	// copy raw gamma-space bytes into the sRGB image (no linear->sRGB encode), so the
	// compositor's sRGB decode gives back exactly what Flycast drew
	glDisable(0x8DB9);	// GL_FRAMEBUFFER_SRGB_EXT (EXT_sRGB_write_control)
	glGetError();
	return true;
}

// <data dir>/ht4q_shot.req -> <data dir>/ht4q_shot.png of the screen we present (for adb pull)
static void maybeShot(GLuint framebuffer, int w, int h)
{
	static const std::string req = get_writable_data_path("../ht4q_shot.req");
	if (access(req.c_str(), F_OK) != 0)
		return;
	unlink(req.c_str());
	std::vector<u8> px(w * h * 4);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
	FILE *f = fopen(get_writable_data_path("../ht4q_shot.png").c_str(), "wb");
	if (f == nullptr)
		return;
	stbi_write_png_to_func([](void *ctx, void *data, int size) { fwrite(data, 1, size, (FILE *)ctx); },
			f, w, h, 4, px.data() + (h - 1) * w * 4, -w * 4);	// GL rows are bottom-up
	fclose(f);
	NOTICE_LOG(RENDERER, "ht4q xr: shot saved, glError %x", glGetError());
}

// Quest Touch controllers -> Dreamcast pad (see core/ht4q/input.cpp)
static const struct {
	const char *name;
	const char *path;
	u32 dcButton;
} buttonMap[5] = {
	{ "a", "/user/hand/right/input/a/click", DC_BTN_A },
	{ "b", "/user/hand/right/input/b/click", DC_BTN_B },
	{ "x", "/user/hand/left/input/x/click", DC_BTN_X },
	{ "y", "/user/hand/left/input/y/click", DC_BTN_Y },
	{ "start", "/user/hand/left/input/menu/click", DC_BTN_START },
};

bool XrGLGC::createActions()
{
	XrActionSetCreateInfo asci{ XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy(asci.actionSetName, "gameplay");
	strcpy(asci.localizedActionSetName, "Gameplay");
	XR_CHECK(xrCreateActionSet(instance, &asci, &actionSet));

	auto make = [this](XrAction& act, const char *name, XrActionType type) {
		XrActionCreateInfo aci{ XR_TYPE_ACTION_CREATE_INFO };
		strcpy(aci.actionName, name);
		strcpy(aci.localizedActionName, name);
		aci.actionType = type;
		return xrCreateAction(actionSet, &aci, &act);
	};
	XR_CHECK(make(actThrottle, "throttle", XR_ACTION_TYPE_FLOAT_INPUT));
	XR_CHECK(make(actBrake, "brake", XR_ACTION_TYPE_FLOAT_INPUT));
	XR_CHECK(make(actSteer, "steer", XR_ACTION_TYPE_VECTOR2F_INPUT));
	for (int i = 0; i < 5; i++)
		XR_CHECK(make(actButton[i], buttonMap[i].name, XR_ACTION_TYPE_BOOLEAN_INPUT));

	auto path = [this](const char *s) {
		XrPath p = XR_NULL_PATH;
		xrStringToPath(instance, s, &p);
		return p;
	};
	std::vector<XrActionSuggestedBinding> bindings {
		{ actThrottle, path("/user/hand/right/input/trigger/value") },
		{ actBrake, path("/user/hand/left/input/trigger/value") },
		{ actSteer, path("/user/hand/left/input/thumbstick") },
	};
	for (int i = 0; i < 5; i++)
		bindings.push_back({ actButton[i], path(buttonMap[i].path) });
	XrInteractionProfileSuggestedBinding spb{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	spb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
	spb.suggestedBindings = bindings.data();
	spb.countSuggestedBindings = bindings.size();
	XR_CHECK(xrSuggestInteractionProfileBindings(instance, &spb));

	XrSessionActionSetsAttachInfo sasai{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	sasai.countActionSets = 1;
	sasai.actionSets = &actionSet;
	XR_CHECK(xrAttachSessionActionSets(session, &sasai));
	return true;
}

void XrGLGC::pollControllers()
{
	XrPadState pad{};
	bool active = actionSet != XR_NULL_HANDLE && state == XR_SESSION_STATE_FOCUSED;
	if (active)
	{
		XrActiveActionSet aas{ actionSet, XR_NULL_PATH };
		XrActionsSyncInfo si{ XR_TYPE_ACTIONS_SYNC_INFO };
		si.countActiveActionSets = 1;
		si.activeActionSets = &aas;
		active = XR_SUCCEEDED(xrSyncActions(session, &si));
	}
	if (active)
	{
		XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
		XrActionStateFloat f{ XR_TYPE_ACTION_STATE_FLOAT };
		gi.action = actThrottle;
		if (XR_SUCCEEDED(xrGetActionStateFloat(session, &gi, &f)) && f.isActive)
			pad.rt = (u16)std::lround(std::clamp(f.currentState, 0.f, 1.f) * 0xffff);
		gi.action = actBrake;
		f = { XR_TYPE_ACTION_STATE_FLOAT };
		if (XR_SUCCEEDED(xrGetActionStateFloat(session, &gi, &f)) && f.isActive)
			pad.lt = (u16)std::lround(std::clamp(f.currentState, 0.f, 1.f) * 0xffff);
		gi.action = actSteer;
		XrActionStateVector2f v{ XR_TYPE_ACTION_STATE_VECTOR2F };
		if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &gi, &v)) && v.isActive)
		{
			// small dead zone; Dreamcast stick y is down-positive
			auto axis = [](float a) { return std::fabs(a) < 0.08f ? 0.f : std::clamp(a, -1.f, 1.f); };
			pad.x = (s16)std::lround(axis(v.currentState.x) * 32767);
			pad.y = (s16)std::lround(-axis(v.currentState.y) * 32767);
		}
		for (int i = 0; i < 5; i++)
		{
			gi.action = actButton[i];
			XrActionStateBoolean b{ XR_TYPE_ACTION_STATE_BOOLEAN };
			if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &b)) && b.isActive && b.currentState)
				pad.pressed |= buttonMap[i].dcButton;
		}
	}
	setXrInput(active, pad);
}

static XrQuaternionf mul(const XrQuaternionf& a, const XrQuaternionf& b)
{
	return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
			 a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
			 a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
			 a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}

static XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v)
{
	XrQuaternionf p = mul(mul(q, { v.x, v.y, v.z, 0.f }), { -q.x, -q.y, -q.z, q.w });
	return { p.x, p.y, p.z };
}

static XrPosef compose(const XrPosef& a, const XrPosef& b)	// b expressed in a
{
	XrVector3f t = rotate(a.orientation, b.position);
	return { mul(a.orientation, b.orientation), { a.position.x + t.x, a.position.y + t.y, a.position.z + t.z } };
}

// Head pose at the frame's predicted display time -> camera hook, eye offsets for the layers.
void XrGLGC::trackHead(XrTime time)
{
	XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
	const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
	bool valid = XR_SUCCEEDED(xrLocateSpace(viewSpace, space, time, &loc))
			&& (loc.locationFlags & needed) == needed && state == XR_SESSION_STATE_FOCUSED;

	XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
	vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	vli.displayTime = time;
	vli.space = viewSpace;
	XrViewState vs{ XR_TYPE_VIEW_STATE };
	XrView views[2] { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
	u32 count = 0;
	float ipd = 0.064f;
	if (XR_SUCCEEDED(xrLocateViews(session, &vli, &vs, 2, &count, views)) && count == 2)
	{
		eyeInHead[0] = views[0].pose;
		eyeInHead[1] = views[1].pose;
		const XrVector3f& a = views[0].pose.position, & b = views[1].pose.position;
		ipd = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
	}
	XrHeadPose head;
	head.q[0] = loc.pose.orientation.x;
	head.q[1] = loc.pose.orientation.y;
	head.q[2] = loc.pose.orientation.z;
	head.q[3] = loc.pose.orientation.w;
	head.p[0] = loc.pose.position.x;
	head.p[1] = loc.pose.position.y;
	head.p[2] = loc.pose.position.z;
	setXrHead(valid, head, ipd);
	static u32 calls;
	if (calls++ % 900 == 0)
		NOTICE_LOG(RENDERER, "ht4q xr: head valid %d (flags %x, state %d) ipd %.4f pos %.2f %.2f %.2f", valid,
				(u32)loc.locationFlags, (int)state, ipd, head.p[0], head.p[1], head.p[2]);
}

bool XrGLGC::submitFrame()
{
	XrFrameState fs{ XR_TYPE_FRAME_STATE };
	XR_CHECK(xrWaitFrame(session, nullptr, &fs));
	XR_CHECK(xrBeginFrame(session, nullptr));
	trackHead(fs.predictedDisplayTime);
	pollControllers();

	XrCompositionLayerProjection proj{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerProjectionView projViews[2] { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
	XrCompositionLayerQuad quads[2] { { XR_TYPE_COMPOSITION_LAYER_QUAD }, { XR_TYPE_COMPOSITION_LAYER_QUAD } };
	const XrCompositionLayerBaseHeader *layers[] = { (XrCompositionLayerBaseHeader *)&quads[0], (XrCompositionLayerBaseHeader *)&quads[1] };
	u32 layerCount = 0;
	for (XrCompositionLayerQuad& q : quads)
	{
		q.space = space;
		q.pose.orientation.w = 1.f;
		q.pose.position = { 0.f, 0.f, -SCREEN_DISTANCE };
		q.size = { SCREEN_WIDTH, SCREEN_WIDTH * 3.f / 4.f };
	}

	const StereoEyes& eyes = stereoEyes();
	FramePose fp;
	const bool vr = eyes.frame != 0 && !gui_is_open() && framePose(eyes.poseId, fp) && fp.immersive;
	EGLint w = 0, h = 0;
	eglQuerySurface(display, surface, EGL_WIDTH, &w);
	eglQuerySurface(display, surface, EGL_HEIGHT, &h);
	if (!fs.shouldRender)
		;
	else if (vr && createSwapchain(stereo, eyes.width * 2, eyes.height))
	{
		// VR: the eye images were rendered for the head pose fp.head with the game's own
		// frustum; tell the compositor exactly that and let it reproject to the current pose
		const int ew = eyes.width, eh = eyes.height;
		if (!acquire(stereo))
			return false;
		const XrPosef head { { fp.head.q[0], fp.head.q[1], fp.head.q[2], fp.head.q[3] },
							 { fp.head.p[0], fp.head.p[1], fp.head.p[2] } };
		const float ah = std::atan(320.f / fp.zscreen), av = std::atan(240.f / fp.zscreen);
		for (int eye = 0; eye < 2; eye++)
		{
			glBindFramebuffer(GL_READ_FRAMEBUFFER, eyes.fbo[eye]);
			glBlitFramebuffer(0, 0, ew, eh, eye * ew, eh, eye * ew + ew, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
			projViews[eye].pose = compose(head, eyeInHead[eye]);
			projViews[eye].fov = { -ah, ah, av, -av };
			projViews[eye].subImage.swapchain = stereo.handle;
			projViews[eye].subImage.imageRect = { { eye * ew, 0 }, { ew, eh } };
		}
		maybeShot(fbo, ew * 2, eh);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		XR_CHECK(xrReleaseSwapchainImage(stereo.handle, nullptr));
		proj.space = space;
		proj.viewCount = 2;
		proj.views = projViews;
		layers[0] = (XrCompositionLayerBaseHeader *)&proj;
		layerCount = 1;
	}
	else if (eyes.frame != 0 && !gui_is_open() && createSwapchain(stereo, eyes.width * 2, eyes.height))
	{
		// stereo: each eye sees its own half. Flycast's output framebuffers are stored top-down,
		// so flip while copying.
		const int ew = eyes.width, eh = eyes.height;
		if (!acquire(stereo))
			return false;
		for (int eye = 0; eye < 2; eye++)
		{
			glBindFramebuffer(GL_READ_FRAMEBUFFER, eyes.fbo[eye]);
			glBlitFramebuffer(0, 0, ew, eh, eye * ew, eh, eye * ew + ew, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
			quads[eye].eyeVisibility = eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
			quads[eye].subImage.swapchain = stereo.handle;
			quads[eye].subImage.imageRect = { { eye * ew, 0 }, { ew, eh } };
		}
		maybeShot(fbo, ew * 2, eh);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		XR_CHECK(xrReleaseSwapchainImage(stereo.handle, nullptr));
		layerCount = 2;
	}
	else if (w > 0 && h > 0 && createSwapchain(mono, w, h))
	{
		// mono: the pbuffer as Flycast presented it (menus, OSD)
		if (!acquire(mono))
			return false;
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		maybeShot(0, w, h);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		XR_CHECK(xrReleaseSwapchainImage(mono.handle, nullptr));
		quads[0].eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quads[0].subImage.swapchain = mono.handle;
		quads[0].subImage.imageRect = { { 0, 0 }, { w, h } };
		layerCount = 1;
	}

	XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
	fei.displayTime = fs.predictedDisplayTime;
	fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	fei.layerCount = layerCount;
	fei.layers = layers;
	XR_CHECK(xrEndFrame(session, &fei));
	return true;
}

void XrGLGC::swap()
{
	static u32 swaps;
	if (swaps++ % 600 == 0)
		NOTICE_LOG(RENDERER, "ht4q xr: swap %u, session state %d, running %d, display %dx%d", swaps, (int)state, running,
				settings.display.width, settings.display.height);
	if (session == XR_NULL_HANDLE)
	{
		EGLGraphicsContext::swap();
		return;
	}
	pollEvents();
	if (running)
		submitFrame();	// xrWaitFrame paces us to the headset; the window surface isn't shown
	else
		EGLGraphicsContext::swap();
}

bool createXrContext(void *window, void *display)
{
	// EGL loading sometimes fails while the Android UI thread initializes its own EGL at
	// app start, so retry a few times
	for (int attempt = 0;; attempt++)
	{
		try {
			new XrGLGC(window, display);
			return true;
		} catch (const FlycastException& e) {
			if (attempt == 4)
				throw;
			WARN_LOG(RENDERER, "ht4q xr: %s, retrying", e.what());
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}
}

}	// namespace ht4q
#endif	// HT4Q_XR
