/*
	HydroThunder4Quest: stereo rendering for the OpenGL renderer.
*/
#pragma once

namespace ht4q
{
// Called by OpenGLRenderer::renderFrame in place of DrawStrips() for on-screen frames.
// Draws both eyes when stereo is enabled, otherwise just calls DrawStrips().
void drawStripsStereo(int width, int height);
}
