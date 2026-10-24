/*
	HydroThunder4Quest: player 2's view streamed to a remote viewer (core/ht4q/stream.cpp).
*/
#pragma once
#include "types.h"
#include <memory>

struct MapleInputState;

namespace ht4q
{
// Listen for a viewer on this TCP port (all interfaces); once. HT4Q_STREAM=<port> does it at startup.
void streamStart(int port);
// A viewer is connected: the renderer then draws player 2's view for it each 2-player frame.
bool streamActive();
// What a stream frame was drawn for (sent in its header).
struct StreamFrameInfo {
	u32 poseId;
	bool stereo;		// left eye | right eye, side by side
	float headQ[4];		// the viewer's head pose it was drawn for (stereo), its LOCAL space
	float headP[3];
	float zscreen;		// projection distance for 640 px: tan(half fov x) = 320 / zscreen
};
// Render thread: player 2's image, top-down RGBA rows of width * 4 bytes.
void streamFrame(const u8 *rgba, int width, int height, const StreamFrameInfo& info);
// Emu thread (onMaplePoll): the viewer's controller on port B.
void streamPoll(MapleInputState inputState[4]);

// Platform video encoder: encode() hands its output to streamPacket(), possibly from another thread.
class StreamEncoder
{
public:
	virtual ~StreamEncoder() = default;
	// tag: passed back to streamPacket with the frame's output
	virtual void encode(const u8 *rgba, int width, int height, u64 timeUs, u32 tag, bool keyframe) = 0;
};
std::unique_ptr<StreamEncoder> createStreamEncoder(int width, int height, int fps, int bitrate);
void streamPacket(const u8 *annexB, size_t size, bool keyframe, u64 timeUs, u32 tag);
}
