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
#include "ht4q/stream.h"
#include "emulator.h"
#include "oslib/oslib.h"
#include <fstream>
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

// Virtual screen: 4:3, 2.4 m wide, 2 m in front of the head when a menu opens (or the view is recentered).
static constexpr float SCREEN_WIDTH = 2.4f;
static constexpr float SCREEN_DISTANCE = 2.0f;
static constexpr int SCREEN_PIXELS_W = 1440, SCREEN_PIXELS_H = 1080;
static constexpr int MIN_RENDER_LINES = 1440;

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
	bool createSwapchain(Swapchain& sc, int width, int height, bool depth = false);
	bool acquire(Swapchain& sc);
	bool submitDepth(const StereoEyes& eyes, XrCompositionLayerDepthInfoKHR depthInfo[2], XrCompositionLayerProjectionView views[2]);
	bool submitFrame();
	void trackHead(XrTime time);
	bool createActions();
	void pollControllers(XrTime time);
	void pointGui(const XrControllers& c);
	void applyHaptics();
	void placeScreen();
	bool viewerStereoFrame();

	XrActionSet actionSet = XR_NULL_HANDLE;
	XrAction actInput[XI_COUNT] {};		// inputPaths
	XrAction actStick[2] {};			// left, right thumbstick
	XrAction actAim[2] {};				// aim poses
	XrAction actHaptic[2] {};
	XrSpace aimSpace[2] { XR_NULL_HANDLE, XR_NULL_HANDLE };
	int pointerHand = 1;				// the hand that last pulled its trigger points at the menus

	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSpace space = XR_NULL_HANDLE;		// LOCAL: recentered, gravity aligned
	XrSpace viewSpace = XR_NULL_HANDLE;	// the head
	XrPosef eyeInHead[2] {};			// eye poses relative to the head
	XrPosef head { { 0.f, 0.f, 0.f, 1.f }, {} };	// last valid head pose in LOCAL
	bool headValid = false;
	XrPosef screen { { 0.f, 0.f, 0.f, 1.f }, { 0.f, 0.f, -SCREEN_DISTANCE } };	// the virtual screen in LOCAL
	bool screenPlaced = false;			// placed for the open menu (reset when it closes or on recenter)
	XrSessionState state = XR_SESSION_STATE_UNKNOWN;
	bool running = false;

	Swapchain mono;		// the pbuffer: menus, flat fallback
	Swapchain stereo;	// left eye | right eye, side by side
	Swapchain depth;	// their depth, same layout (immersive view)
	bool depthExt = false;	// XR_KHR_composition_layer_depth
	GLuint fbo = 0;
	// joined a game on another headset (core/ht4q/viewer.cpp): its latest frame, as a texture
	ViewerFrame viewerImg;
	GLuint viewerTex = 0, viewerFbo = 0;
	int viewerTexW = 0, viewerTexH = 0;
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

	u32 extCount = 0;
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
	std::vector<XrExtensionProperties> extProps(extCount, { XR_TYPE_EXTENSION_PROPERTIES });
	xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, extProps.data());
	for (const XrExtensionProperties& e : extProps)
		depthExt = depthExt || strcmp(e.extensionName, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME) == 0;
	NOTICE_LOG(RENDERER, "ht4q xr: depth layers %ssupported", depthExt ? "" : "not ");
	std::vector<const char *> extensions { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME };
	if (depthExt)
		extensions.push_back(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
	XrInstanceCreateInfoAndroidKHR androidInfo{ XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	androidInfo.applicationVM = vm;
	androidInfo.applicationActivity = g_activity;
	XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
	ici.next = &androidInfo;
	strcpy(ici.applicationInfo.applicationName, "HydroThunder4Quest");
	ici.applicationInfo.applicationVersion = 1;
	strcpy(ici.applicationInfo.engineName, "Flycast");
	ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	ici.enabledExtensionCount = extensions.size();
	ici.enabledExtensionNames = extensions.data();
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
	// frame dumps (ht4q_stereo.cpp) on request: touch <data dir>/../ht4q_frame.txt.req
	setenv("HT4Q_FRAMEDUMP", get_writable_data_path("../ht4q_frame.txt").c_str(), 0);
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
	for (XrSpace& s : aimSpace)
		if (s != XR_NULL_HANDLE)
			xrDestroySpace(s), s = XR_NULL_HANDLE;
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
		if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
			screenPlaced = false;	// recentered: bring the screen back in front
		else if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
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

bool XrGLGC::createSwapchain(Swapchain& sc, int width, int height, bool depth)
{
	if (sc.handle != XR_NULL_HANDLE && width == sc.width && height == sc.height)
		return true;
	if (sc.handle != XR_NULL_HANDLE)
		xrDestroySwapchain(sc.handle);
	sc = {};

	XrSwapchainCreateInfo sci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
	// depth: same format as ht4q_stereo's converted depth, so it can be blitted
	sci.usageFlags = (depth ? XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)
			| XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	sci.format = depth ? GL_DEPTH_COMPONENT24 : GL_SRGB8_ALPHA8;
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

// Copies the eyes' depth into the depth swapchain and chains it to the projection views.
// Without it the views are submitted as before (color only).
bool XrGLGC::submitDepth(const StereoEyes& eyes, XrCompositionLayerDepthInfoKHR depthInfo[2], XrCompositionLayerProjectionView views[2])
{
	static bool failed;
	if (!depthExt || failed || eyes.depthFbo[0] == 0 || eyes.depthFbo[1] == 0)
		return false;
	// <data dir>/ht4q_nodepth: off, for comparing (quest.sh depth on|off)
	static const std::string off = get_writable_data_path("../ht4q_nodepth");
	static u32 frames;
	static bool disabled;
	if (frames++ % 36 == 0)
		disabled = access(off.c_str(), F_OK) == 0;
	if (disabled)
		return false;
	if (!createSwapchain(depth, eyes.width * 2, eyes.height, true))
	{
		failed = true;	// format not offered
		return false;
	}
	u32 index;
	if (XR_FAILED(xrAcquireSwapchainImage(depth.handle, nullptr, &index)))
		return false;
	XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	wi.timeout = XR_INFINITE_DURATION;
	xrWaitSwapchainImage(depth.handle, &wi);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth.images[index].image, 0);
	const int ew = eyes.width, eh = eyes.height;
	for (int eye = 0; eye < 2; eye++)
	{
		glBindFramebuffer(GL_READ_FRAMEBUFFER, eyes.depthFbo[eye]);
		glBlitFramebuffer(0, 0, ew, eh, eye * ew, eh, eye * ew + ew, 0, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
		depthInfo[eye] = { XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR };
		depthInfo[eye].subImage.swapchain = depth.handle;
		depthInfo[eye].subImage.imageRect = { { eye * ew, 0 }, { ew, eh } };
		depthInfo[eye].minDepth = 0.f;
		depthInfo[eye].maxDepth = 1.f;
		depthInfo[eye].nearZ = eyes.depthNearZ;
		depthInfo[eye].farZ = eyes.depthFarZ;
		views[eye].next = &depthInfo[eye];
	}
	GLenum err = glGetError();
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
	XR_CHECK(xrReleaseSwapchainImage(depth.handle, nullptr));
	static bool logged;
	if (!logged)
	{
		NOTICE_LOG(RENDERER, "ht4q xr: submitting depth %dx%d, glError %x", ew * 2, eh, err);
		logged = true;
	}
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

// <data dir>/ht4q_save.req -> savestate slot 9, copied to <data dir>/ht4q_save.state (for adb pull,
// to reproduce a headset scene on the desktop). Saved like the GUI does: emulator stopped meanwhile.
static void maybeSaveState()
{
	static const std::string req = get_writable_data_path("../ht4q_save.req");
	if (access(req.c_str(), F_OK) != 0)
		return;
	unlink(req.c_str());
	std::thread([] {
		emu.stop();
		dc_savestate(9);
		emu.start();
		std::ifstream in(hostfs::getSavestatePath(9, false), std::ios::binary);
		std::ofstream out(get_writable_data_path("../ht4q_save.state"), std::ios::binary);
		out << in.rdbuf();
		NOTICE_LOG(RENDERER, "ht4q xr: state saved, copied %d", (int)out.good());
	}).detach();
}

// <data dir>/ht4q_click.req holding "x y" (screen pixels, 1440x1080): a click there, as the controller's
// pointer and trigger would (test hook, to drive the menus without wearing the headset)
static void maybeTestClick()
{
	static const std::string req = get_writable_data_path("../ht4q_click.req");
	static int frames;
	static int x, y;
	if (frames == 0)
	{
		FILE *f = fopen(req.c_str(), "r");
		if (f == nullptr)
			return;
		const bool ok = fscanf(f, "%d %d", &x, &y) == 2;
		fclose(f);
		unlink(req.c_str());
		if (!ok)
			return;
		frames = 12;
	}
	frames--;
	const float scale = settings.display.pointScale;
	gui_set_mouse_position((int)std::lround(x / scale), (int)std::lround(y / scale), false);
	gui_set_mouse_button(0, frames >= 4 && frames < 8, false);	// a few frames hovering, pressed, released
}

// Quest Touch controllers, raw; core/ht4q/controls.cpp maps them to the Dreamcast pad
static const char *inputPaths[XI_COUNT] = {
	"/user/hand/right/input/a/click", "/user/hand/right/input/b/click",
	"/user/hand/left/input/x/click", "/user/hand/left/input/y/click",
	"/user/hand/left/input/menu/click",
	"/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click",
	"/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value",
	"/user/hand/left/input/squeeze/value", "/user/hand/right/input/squeeze/value",
};
static const char *const handPaths[2] = { "/user/hand/left", "/user/hand/right" };

bool XrGLGC::createActions()
{
	XrActionSetCreateInfo asci{ XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy(asci.actionSetName, "gameplay");
	strcpy(asci.localizedActionSetName, "Gameplay");
	XR_CHECK(xrCreateActionSet(instance, &asci, &actionSet));

	auto make = [this](XrAction& act, const std::string& name, XrActionType type) {
		XrActionCreateInfo aci{ XR_TYPE_ACTION_CREATE_INFO };
		strcpy(aci.actionName, name.c_str());
		strcpy(aci.localizedActionName, name.c_str());
		aci.actionType = type;
		return xrCreateAction(actionSet, &aci, &act);
	};
	auto path = [this](const std::string& s) {
		XrPath p = XR_NULL_PATH;
		xrStringToPath(instance, s.c_str(), &p);
		return p;
	};
	std::vector<XrActionSuggestedBinding> bindings;
	for (int i = 0; i < XI_COUNT; i++)
	{
		// float for all: the runtime turns clicks into 0/1
		XR_CHECK(make(actInput[i], "input" + std::to_string(i), XR_ACTION_TYPE_FLOAT_INPUT));
		bindings.push_back({ actInput[i], path(inputPaths[i]) });
	}
	for (int h = 0; h < 2; h++)
	{
		const std::string hand = h == 0 ? "left" : "right";
		XR_CHECK(make(actStick[h], hand + "_stick", XR_ACTION_TYPE_VECTOR2F_INPUT));
		XR_CHECK(make(actAim[h], hand + "_aim", XR_ACTION_TYPE_POSE_INPUT));
		XR_CHECK(make(actHaptic[h], hand + "_haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT));
		bindings.push_back({ actStick[h], path(std::string(handPaths[h]) + "/input/thumbstick") });
		bindings.push_back({ actAim[h], path(std::string(handPaths[h]) + "/input/aim/pose") });
		bindings.push_back({ actHaptic[h], path(std::string(handPaths[h]) + "/output/haptic") });
	}
	XrInteractionProfileSuggestedBinding spb{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	spb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
	spb.suggestedBindings = bindings.data();
	spb.countSuggestedBindings = bindings.size();
	XR_CHECK(xrSuggestInteractionProfileBindings(instance, &spb));

	for (int h = 0; h < 2; h++)
	{
		XrActionSpaceCreateInfo asi{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
		asi.action = actAim[h];
		asi.poseInActionSpace.orientation.w = 1.f;
		XR_CHECK(xrCreateActionSpace(session, &asi, &aimSpace[h]));
	}
	XrSessionActionSetsAttachInfo sasai{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	sasai.countActionSets = 1;
	sasai.actionSets = &actionSet;
	XR_CHECK(xrAttachSessionActionSets(session, &sasai));
	return true;
}

void XrGLGC::pollControllers(XrTime time)
{
	XrControllers c{};
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
		for (int i = 0; i < XI_COUNT; i++)
		{
			gi.action = actInput[i];
			XrActionStateFloat f{ XR_TYPE_ACTION_STATE_FLOAT };
			if (XR_SUCCEEDED(xrGetActionStateFloat(session, &gi, &f)) && f.isActive)
				c.value[i] = std::clamp(f.currentState, 0.f, 1.f);
		}
		for (int h = 0; h < 2; h++)
		{
			gi.action = actStick[h];
			XrActionStateVector2f v{ XR_TYPE_ACTION_STATE_VECTOR2F };
			if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &gi, &v)) && v.isActive)
			{
				c.hand[h].stick[0] = v.currentState.x;
				c.hand[h].stick[1] = v.currentState.y;
			}
			XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
			const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
			if (XR_SUCCEEDED(xrLocateSpace(aimSpace[h], space, time, &loc)) && (loc.locationFlags & needed) == needed)
			{
				XrHand& hand = c.hand[h];
				hand.tracked = true;
				hand.q[0] = loc.pose.orientation.x;
				hand.q[1] = loc.pose.orientation.y;
				hand.q[2] = loc.pose.orientation.z;
				hand.q[3] = loc.pose.orientation.w;
				hand.p[0] = loc.pose.position.x;
				hand.p[1] = loc.pose.position.y;
				hand.p[2] = loc.pose.position.z;
			}
		}
	}
	setXrControllers(active, c);
	if (active)
	{
		pointGui(c);
		applyHaptics();
	}
}

// Dreamcast rumble -> both controllers
void XrGLGC::applyHaptics()
{
	static u32 seq;
	static float amplitude;
	static std::chrono::steady_clock::time_point end;
	const Haptics hp = haptics();
	if (hp.seq == seq)
		return;
	seq = hp.seq;
	// the game repeats its command every few frames: only pass on changes
	const auto now = std::chrono::steady_clock::now();
	if (hp.amplitude == amplitude && (amplitude == 0.f || now + std::chrono::milliseconds(100) < end))
		return;
	amplitude = hp.amplitude;
	end = now + std::chrono::milliseconds(hp.durationMs);
	for (int h = 0; h < 2; h++)
	{
		XrHapticActionInfo hai{ XR_TYPE_HAPTIC_ACTION_INFO };
		hai.action = actHaptic[h];
		if (hp.amplitude <= 0.f)
		{
			xrStopHapticFeedback(session, &hai);
			continue;
		}
		XrHapticVibration vib{ XR_TYPE_HAPTIC_VIBRATION };
		vib.amplitude = hp.amplitude;
		vib.duration = (XrDuration)hp.durationMs * 1000000;
		vib.frequency = XR_FREQUENCY_UNSPECIFIED;
		xrApplyHapticFeedback(session, &hai, (const XrHapticBaseHeader *)&vib);
	}
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

// Puts the screen SCREEN_DISTANCE in front of the head, upright, facing it (head yaw only)
void XrGLGC::placeScreen()
{
	const XrVector3f f = rotate(head.orientation, { 0.f, 0.f, -1.f });
	const float yaw = std::atan2(-f.x, -f.z);
	screen.orientation = { 0.f, std::sin(yaw / 2), 0.f, std::cos(yaw / 2) };
	const XrVector3f ahead = rotate(screen.orientation, { 0.f, 0.f, -SCREEN_DISTANCE });
	screen.position = { head.position.x + ahead.x, head.position.y, head.position.z + ahead.z };
	screenPlaced = true;
}

// While a menu is open (launcher, Flycast GUI), the controller's aim ray on the screen quad is the
// mouse and its trigger the left button.
void XrGLGC::pointGui(const XrControllers& c)
{
	static bool pointing, pressed;
	if (!gui_is_open())
	{
		if (pointing)
		{
			gui_set_mouse_button(0, false, false);
			gui_set_mouse_position(-1, -1, false);
			pointing = pressed = false;
		}
		return;
	}
	if (c.value[XI_LTRIGGER] > 0.6f && c.value[XI_RTRIGGER] < 0.3f)
		pointerHand = 0;
	else if (c.value[XI_RTRIGGER] > 0.6f && c.value[XI_LTRIGGER] < 0.3f)
		pointerHand = 1;
	const XrHand& h = c.hand[pointerHand];
	// the aim ray in the screen's frame (the screen is its z = 0 plane, facing +z)
	const XrQuaternionf inv { -screen.orientation.x, -screen.orientation.y, -screen.orientation.z, screen.orientation.w };
	const XrVector3f d = rotate(inv, rotate({ h.q[0], h.q[1], h.q[2], h.q[3] }, { 0.f, 0.f, -1.f }));
	const XrVector3f o = rotate(inv, { h.p[0] - screen.position.x, h.p[1] - screen.position.y, h.p[2] - screen.position.z });
	const float t = d.z < -1e-3f ? -o.z / d.z : -1.f;
	float px = -1.f, py = -1.f;
	if (h.tracked && t > 0.f)
	{
		const float x = o.x + t * d.x, y = o.y + t * d.y;
		px = (x / SCREEN_WIDTH + 0.5f) * settings.display.width;
		py = (0.5f - y / (SCREEN_WIDTH * 3.f / 4.f)) * settings.display.height;
	}
	const float scale = settings.display.pointScale;
	gui_set_mouse_position((int)std::lround(px / scale), (int)std::lround(py / scale), false);
	const float trigger = c.value[pointerHand == 0 ? XI_LTRIGGER : XI_RTRIGGER];
	const bool was = pressed;
	pressed = trigger > (pressed ? 0.4f : 0.6f);
	if (pressed != was)	// to follow menu clicks in the log
		NOTICE_LOG(RENDERER, "ht4q xr: pointer %s, hand %d at %.0f,%.0f", pressed ? "down" : "up", pointerHand, px, py);
	gui_set_mouse_button(0, pressed, false);
	// its thumbstick scrolls
	const float scroll = h.stick[1];
	if (std::fabs(scroll) > 0.2f)
		gui_set_mouse_wheel(-scroll * 4.f);
	pointing = true;
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
	if (valid && viewerState() == ViewerState::Connected)
		viewerSendXrHead(head, ipd);	// the host draws our view for this pose
	if (valid)
	{
		this->head = loc.pose;
		headValid = true;
	}
	static u32 calls;
	if (calls++ % 900 == 0)
		NOTICE_LOG(RENDERER, "ht4q xr: head valid %d (flags %x, state %d) ipd %.4f pos %.2f %.2f %.2f", valid,
				(u32)loc.locationFlags, (int)state, ipd, head.p[0], head.p[1], head.p[2]);
}

// Joined a game on another headset: uploads its newest frame; true when it's a stereo frame (the race,
// drawn for the head pose it carries) to show as a projection layer. Mono frames (the host's menus) are
// drawn by the launcher's viewing page and shown as the screen like any menu.
bool XrGLGC::viewerStereoFrame()
{
	if (viewerState() != ViewerState::Connected)
		return false;
	if (viewerFrame(viewerImg))
	{
		if (viewerTex == 0)
		{
			glGenTextures(1, &viewerTex);
			glGenFramebuffers(1, &viewerFbo);
		}
		glBindTexture(GL_TEXTURE_2D, viewerTex);
		if (viewerImg.width != viewerTexW || viewerImg.height != viewerTexH)
		{
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, viewerImg.width, viewerImg.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, viewerImg.rgba.data());
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			viewerTexW = viewerImg.width;
			viewerTexH = viewerImg.height;
			glBindFramebuffer(GL_READ_FRAMEBUFFER, viewerFbo);
			glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, viewerTex, 0);
		}
		else
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, viewerImg.width, viewerImg.height, GL_RGBA, GL_UNSIGNED_BYTE, viewerImg.rgba.data());
		glBindTexture(GL_TEXTURE_2D, 0);
	}
	return viewerImg.info.stereo && viewerTexW > 0;
}

bool XrGLGC::submitFrame()
{
	XrFrameState fs{ XR_TYPE_FRAME_STATE };
	XR_CHECK(xrWaitFrame(session, nullptr, &fs));
	XR_CHECK(xrBeginFrame(session, nullptr));
	trackHead(fs.predictedDisplayTime);
	pollControllers(fs.predictedDisplayTime);
	maybeSaveState();
	maybeTestClick();

	XrCompositionLayerProjection proj{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerProjectionView projViews[2] { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
	XrCompositionLayerDepthInfoKHR depthInfo[2] {};
	XrCompositionLayerQuad quads[2] { { XR_TYPE_COMPOSITION_LAYER_QUAD }, { XR_TYPE_COMPOSITION_LAYER_QUAD } };
	const XrCompositionLayerBaseHeader *layers[] = { (XrCompositionLayerBaseHeader *)&quads[0], (XrCompositionLayerBaseHeader *)&quads[1] };
	u32 layerCount = 0;
	if (!gui_is_open())
		screenPlaced = false;
	else if (!screenPlaced && headValid)
		placeScreen();
	for (XrCompositionLayerQuad& q : quads)
	{
		q.space = space;
		q.pose = screen;
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
	else if (viewerStereoFrame() && createSwapchain(stereo, viewerTexW, viewerTexH))
	{
		// the joined game's view of the race, rendered by the host for the head pose it carries
		const int ew = viewerTexW / 2, eh = viewerTexH;
		if (!acquire(stereo))
			return false;
		glBindFramebuffer(GL_READ_FRAMEBUFFER, viewerFbo);
		glBlitFramebuffer(0, 0, ew * 2, eh, 0, eh, ew * 2, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);	// rows are top-down
		const StreamFrameInfo& vi = viewerImg.info;
		const XrPosef vhead { { vi.headQ[0], vi.headQ[1], vi.headQ[2], vi.headQ[3] }, { vi.headP[0], vi.headP[1], vi.headP[2] } };
		const float ah = std::atan(320.f / vi.zscreen), av = std::atan(240.f / vi.zscreen);
		for (int eye = 0; eye < 2; eye++)
		{
			projViews[eye].pose = compose(vhead, eyeInHead[eye]);
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
	else if (vr && createSwapchain(stereo, eyes.width * 2, eyes.height))
	{
		// VR: the eye images were rendered for the head pose fp.head with the game's own
		// frustum; tell the compositor exactly that and let it reproject to the current pose
		const int ew = eyes.width, eh = eyes.height;
		if (!acquire(stereo))
			return false;
		const XrPosef head { { fp.head.q[0], fp.head.q[1], fp.head.q[2], fp.head.q[3] },
							 { fp.head.p[0], fp.head.p[1], fp.head.p[2] } };
		const float ah = std::atan(320.f / fp.zscreen), av = std::atan(240.f / (fp.zscreenY > 0.f ? fp.zscreenY : fp.zscreen));
		for (int eye = 0; eye < 2; eye++)
		{
			glBindFramebuffer(GL_READ_FRAMEBUFFER, eyes.fbo[eye]);
			glBlitFramebuffer(0, 0, ew, eh, eye * ew, eh, eye * ew + ew, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
			projViews[eye].pose = compose(head, eyeInHead[eye]);
			projViews[eye].fov = { -ah, ah, av, -av };
			projViews[eye].subImage.swapchain = stereo.handle;
			projViews[eye].subImage.imageRect = { { eye * ew, 0 }, { ew, eh } };
		}
		if (!adjustingView())	// else the shot is the adjustment panel, below
			maybeShot(fbo, ew * 2, eh);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		XR_CHECK(xrReleaseSwapchainImage(stereo.handle, nullptr));
		submitDepth(eyes, depthInfo, projViews);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		proj.space = space;
		proj.viewCount = 2;
		proj.views = projViews;
		layers[0] = (XrCompositionLayerBaseHeader *)&proj;
		layerCount = 1;
		if (adjustingView() && panelFramebuffer() != 0 && w > 0 && h > 0 && createSwapchain(mono, w, h) && acquire(mono))
		{
			// the values being adjusted: their panel (drawn with alpha, premultiplied), in front of the eyes
			const int px = int(w * ADJUST_PANEL[0]), pw = int(w * ADJUST_PANEL[2]), ph = int(h * ADJUST_PANEL[3]);
			const int py = h - int(h * ADJUST_PANEL[1]) - ph;	// GL rows are bottom-up
			glBindFramebuffer(GL_READ_FRAMEBUFFER, panelFramebuffer());
			glBlitFramebuffer(px, py, px + pw, py + ph, 0, 0, pw, ph, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			maybeShot(fbo, pw, ph);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			XR_CHECK(xrReleaseSwapchainImage(mono.handle, nullptr));
			XrCompositionLayerQuad& q = quads[0];
			q.space = viewSpace;
			q.pose = { { 0.f, 0.f, 0.f, 1.f }, { 0.f, -0.05f, -1.2f } };
			q.size = { 0.7f, 0.7f * ph / pw };
			q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			q.subImage.swapchain = mono.handle;
			q.subImage.imageRect = { { 0, 0 }, { pw, ph } };
			layers[1] = (XrCompositionLayerBaseHeader *)&q;
			layerCount = 2;
		}
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

// GL state that the runtime (or our copies) may change during a frame submission, restored after
// it: Flycast's glcache assumes nobody else touches the context, so a program or enable it believes
// current but isn't makes its next draws (the GUI's) come out black.
struct GlStateBackup
{
	GLint program, vao, arrayBuffer, activeTexture, texture2d, scissorBox[4];
	GLboolean blend, scissor, depth, stencil, cull;
	GlStateBackup()
	{
		glGetIntegerv(GL_CURRENT_PROGRAM, &program);
		glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
		glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
		glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture2d);
		glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
		blend = glIsEnabled(GL_BLEND);
		scissor = glIsEnabled(GL_SCISSOR_TEST);
		depth = glIsEnabled(GL_DEPTH_TEST);
		stencil = glIsEnabled(GL_STENCIL_TEST);
		cull = glIsEnabled(GL_CULL_FACE);
	}
	~GlStateBackup()
	{
		glUseProgram(program);
		glBindVertexArray(vao);
		glBindBuffer(GL_ARRAY_BUFFER, arrayBuffer);
		glActiveTexture(activeTexture);
		glBindTexture(GL_TEXTURE_2D, texture2d);
		glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
		const auto set = [](GLenum cap, GLboolean on) { on ? glEnable(cap) : glDisable(cap); };
		set(GL_BLEND, blend);
		set(GL_SCISSOR_TEST, scissor);
		set(GL_DEPTH_TEST, depth);
		set(GL_STENCIL_TEST, stencil);
		set(GL_CULL_FACE, cull);
	}
};

void XrGLGC::swap()
{
	static u32 swaps;
	if (swaps++ % 600 == 0)
		NOTICE_LOG(RENDERER, "ht4q xr: swap %u, session state %d, running %d, gui %d, display %dx%d", swaps, (int)state, running, gui_is_open(),
				settings.display.width, settings.display.height);
	if (session == XR_NULL_HANDLE)
	{
		EGLGraphicsContext::swap();
		return;
	}
	pollEvents();
	if (running)
	{
		GlStateBackup glState;
		submitFrame();	// xrWaitFrame paces us to the headset; the window surface isn't shown
	}
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
