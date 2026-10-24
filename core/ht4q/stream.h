/*
	HydroThunder4Quest: player 2's view streamed to a remote viewer (core/ht4q/stream.cpp).
*/
#pragma once
#include "types.h"
#include <memory>

struct MapleInputState;

namespace ht4q
{
// A viewer is connected: the renderer then draws player 2's view for it each 2-player frame.
bool streamActive();
// Render thread: player 2's image, top-down RGBA rows of width * 4 bytes, and the pose id it was drawn with.
void streamFrame(const u8 *rgba, int width, int height, u32 poseId);
// Emu thread (onMaplePoll): the viewer's controller on port B.
void streamPoll(MapleInputState inputState[4]);

// Platform video encoder: encode() hands its output to streamPacket(), possibly from another thread.
class StreamEncoder
{
public:
	virtual ~StreamEncoder() = default;
	virtual void encode(const u8 *rgba, int width, int height, u64 timeUs, u32 poseId, bool keyframe) = 0;
};
std::unique_ptr<StreamEncoder> createStreamEncoder(int width, int height, int fps, int bitrate);
void streamPacket(const u8 *annexB, size_t size, bool keyframe, u64 timeUs, u32 poseId);
}
