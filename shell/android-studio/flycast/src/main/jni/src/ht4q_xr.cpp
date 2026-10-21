/*
	HydroThunder4Quest: OpenXR presentation for Meta Quest (quest flavor, HT4Q_XR).

	Flycast keeps rendering into its normal EGL window surface. On each swap we copy that
	surface into an OpenXR swapchain and submit it as a world-locked quad layer ("screen
	mode"), so everything Flycast draws - game, ImGui menus, OSD - shows up in the headset.
	The XR frame loop runs on Flycast's render thread: one xrWaitFrame/xrEndFrame per swap.

	Milestone 1 of the Quest port. Stereo/immersive mode comes later.
*/
#ifdef HT4Q_XR
#include "types.h"
#include "wsi/egl.h"
#include "jni_util.h"
#include "ht4q_xr.h"
#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

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

class XrGLGC : public EGLGraphicsContext
{
public:
	XrGLGC(void *window, void *display) : EGLGraphicsContext(window, display) {
		if (!initXr())
			ERROR_LOG(RENDERER, "ht4q xr: OpenXR unavailable, falling back to the flat window");
	}
	~XrGLGC() { termXr(); }

	void swap() override;

private:
	bool initXr();
	void termXr();
	void pollEvents();
	bool createSwapchain(int width, int height);
	bool submitFrame();

	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSpace space = XR_NULL_HANDLE;
	XrSessionState state = XR_SESSION_STATE_UNKNOWN;
	bool running = false;

	XrSwapchain swapchain = XR_NULL_HANDLE;
	int scWidth = 0, scHeight = 0;
	std::vector<XrSwapchainImageOpenGLESKHR> images;
	GLuint fbo = 0;
};

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

	// the binding needs the EGLConfig of our context
	EGLint configId = 0, numConfigs = 0;
	eglQueryContext(display, context, EGL_CONFIG_ID, &configId);
	EGLint attribs[] = { EGL_CONFIG_ID, configId, EGL_NONE };
	EGLConfig config = nullptr;
	eglChooseConfig(display, attribs, &config, 1, &numConfigs);

	XrGraphicsBindingOpenGLESAndroidKHR binding{ XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
	binding.display = display;
	binding.config = config;
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
	NOTICE_LOG(RENDERER, "ht4q xr: session created");
	return true;
}

void XrGLGC::termXr()
{
	if (swapchain != XR_NULL_HANDLE)
		xrDestroySwapchain(swapchain);
	if (space != XR_NULL_HANDLE)
		xrDestroySpace(space);
	if (session != XR_NULL_HANDLE)
		xrDestroySession(session);
	if (instance != XR_NULL_HANDLE)
		xrDestroyInstance(instance);
	if (fbo != 0)
		glDeleteFramebuffers(1, &fbo);
	swapchain = XR_NULL_HANDLE;
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
			INFO_LOG(RENDERER, "ht4q xr: session state %d", (int)state);
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

bool XrGLGC::createSwapchain(int width, int height)
{
	if (swapchain != XR_NULL_HANDLE && width == scWidth && height == scHeight)
		return true;
	if (swapchain != XR_NULL_HANDLE)
		xrDestroySwapchain(swapchain);
	swapchain = XR_NULL_HANDLE;

	XrSwapchainCreateInfo sci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
	sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	sci.format = GL_SRGB8_ALPHA8;
	sci.sampleCount = 1;
	sci.width = width;
	sci.height = height;
	sci.faceCount = 1;
	sci.arraySize = 1;
	sci.mipCount = 1;
	XR_CHECK(xrCreateSwapchain(session, &sci, &swapchain));

	u32 count = 0;
	XR_CHECK(xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr));
	images.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR });
	XR_CHECK(xrEnumerateSwapchainImages(swapchain, count, &count, (XrSwapchainImageBaseHeader *)images.data()));
	scWidth = width;
	scHeight = height;
	NOTICE_LOG(RENDERER, "ht4q xr: swapchain %dx%d, %u images", width, height, count);
	return true;
}

bool XrGLGC::submitFrame()
{
	XrFrameState fs{ XR_TYPE_FRAME_STATE };
	XR_CHECK(xrWaitFrame(session, nullptr, &fs));
	XR_CHECK(xrBeginFrame(session, nullptr));

	XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
	const XrCompositionLayerBaseHeader *layers[] = { (XrCompositionLayerBaseHeader *)&quad };
	u32 layerCount = 0;

	EGLint w = 0, h = 0;
	eglQuerySurface(display, surface, EGL_WIDTH, &w);
	eglQuerySurface(display, surface, EGL_HEIGHT, &h);
	if (fs.shouldRender && w > 0 && h > 0 && createSwapchain(w, h))
	{
		u32 index;
		XR_CHECK(xrAcquireSwapchainImage(swapchain, nullptr, &index));
		XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		wi.timeout = XR_INFINITE_DURATION;
		XR_CHECK(xrWaitSwapchainImage(swapchain, &wi));

		// window surface -> swapchain image
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[index].image, 0);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glDisable(GL_SCISSOR_TEST);
		// copy raw gamma-space bytes into the sRGB image (no linear->sRGB encode), so the
		// compositor's sRGB decode gives back exactly what Flycast drew
		glDisable(0x8DB9);	// GL_FRAMEBUFFER_SRGB_EXT (EXT_sRGB_write_control)
		glGetError();
		glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		XR_CHECK(xrReleaseSwapchainImage(swapchain, nullptr));

		quad.space = space;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.subImage.swapchain = swapchain;
		quad.subImage.imageRect = { { 0, 0 }, { w, h } };
		quad.pose.orientation.w = 1.f;
		quad.pose.position = { 0.f, 0.f, -SCREEN_DISTANCE };
		quad.size = { SCREEN_WIDTH, SCREEN_WIDTH * h / w };
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
	new XrGLGC(window, display);
	return true;
}

}	// namespace ht4q
#endif	// HT4Q_XR
