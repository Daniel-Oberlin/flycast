/*
	HydroThunder4Quest: stereo rendering for the OpenGL renderer.
*/
#pragma once

namespace ht4q
{
// Latest per-eye images in Xr mode (Quest): color framebuffers of the output size,
// 0 = left, 1 = right. frame counts up each time both eyes have been rendered.
struct StereoEyes {
	unsigned fbo[2];
	int width, height;
	unsigned depthFbo[2];		// immersive frames: their depth (GL_DEPTH_COMPONENT24), else 0
	float depthNearZ, depthFarZ;	// meters at depth 0 and 1 (XrCompositionLayerDepthInfoKHR)
	unsigned poseId;			// ht4q::framePose id the frame was drawn with
	unsigned long long frame;
};
const StereoEyes& stereoEyes();

// Called by OpenGLRenderer::renderFrame in place of DrawStrips() for on-screen frames.
// Draws both eyes when stereo is enabled, otherwise just calls DrawStrips().
void drawStripsStereo(int width, int height);
}
