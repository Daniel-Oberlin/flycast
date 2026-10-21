/*
	HydroThunder4Quest: OpenXR presentation for Meta Quest (quest flavor, HT4Q_XR).

	The Android window surface is destroyed once the app turns immersive, so Flycast renders
	into a 4:3 pbuffer instead (its "screen"). On each swap we copy the pbuffer into an
	OpenXR swapchain and submit it as a world-locked quad layer ("screen
	mode"), so everything Flycast draws - game, ImGui menus, OSD - shows up in the headset.
	The XR frame loop runs on Flycast's render thread: one xrWaitFrame/xrEndFrame per swap.

	In game, Flycast's stereo renderer (core/rend/gles/ht4q_stereo.cpp, Xr mode) keeps a
	separate image per eye; we then show the screen as two quads, one per eye ("3D TV").
	Menus (Flycast GUI open) stay mono.
*/
#ifdef HT4Q_XR
#include "types.h"
#include "wsi/egl.h"
#include "jni_util.h"
#include "ht4q_xr.h"
#include "stdclass.h"
#include "cfg/option.h"
#include "ui/vgamepad.h"
#include "ui/gui.h"
#include "rend/gles/ht4q_stereo.h"
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

	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSpace space = XR_NULL_HANDLE;
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
	if (space != XR_NULL_HANDLE)
		xrDestroySpace(space);
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

bool XrGLGC::submitFrame()
{
	XrFrameState fs{ XR_TYPE_FRAME_STATE };
	XR_CHECK(xrWaitFrame(session, nullptr, &fs));
	XR_CHECK(xrBeginFrame(session, nullptr));

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
	EGLint w = 0, h = 0;
	eglQuerySurface(display, surface, EGL_WIDTH, &w);
	eglQuerySurface(display, surface, EGL_HEIGHT, &h);
	if (!fs.shouldRender)
		;
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
