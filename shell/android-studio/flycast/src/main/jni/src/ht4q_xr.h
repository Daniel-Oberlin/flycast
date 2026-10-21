/*
	HydroThunder4Quest: OpenXR presentation for Meta Quest.
*/
#pragma once

namespace ht4q
{
// Creates the OpenXR-presenting EGL graphics context (quest flavor builds only).
bool createXrContext(void *window, void *display);
}
