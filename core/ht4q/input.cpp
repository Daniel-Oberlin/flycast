/*
	HydroThunder4Quest: headset controller input for maple port A (Quest Touch controllers,
	read by the OpenXR presenter). The memserver's virtual controller, applied after this,
	still overrides it for scripted tests.
*/
#include "ht4q.h"
#include "hw/maple/maple_cfg.h"
#include <algorithm>
#include <mutex>

namespace ht4q
{

static std::mutex xrInputMutex;
static bool xrInputActive;
static XrPadState xrPad;

void setXrInput(bool active, const XrPadState& pad)
{
	std::lock_guard<std::mutex> lock(xrInputMutex);
	xrInputActive = active;
	xrPad = pad;
}

void applyXrInput(MapleInputState inputState[4])
{
	std::lock_guard<std::mutex> lock(xrInputMutex);
	if (!xrInputActive)
		return;
	MapleInputState& st = inputState[0];
	st.kcode &= ~xrPad.pressed;
	st.halfAxes[PJTI_L] = std::max(st.halfAxes[PJTI_L], xrPad.lt);
	st.halfAxes[PJTI_R] = std::max(st.halfAxes[PJTI_R], xrPad.rt);
	if (xrPad.x != 0 || xrPad.y != 0)
	{
		st.fullAxes[PJAI_X1] = xrPad.x;
		st.fullAxes[PJAI_Y1] = xrPad.y;
	}
}

}	// namespace ht4q
