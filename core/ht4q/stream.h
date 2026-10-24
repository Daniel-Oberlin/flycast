/*
	HydroThunder4Quest: player 2's view streamed to a remote viewer (core/ht4q/stream.cpp).
*/
#pragma once
#include "types.h"
#include "ht4q.h"
#include <memory>
#include <string>
#include <vector>

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
// Player 2's rumble (Puru Puru on port B) for the viewer's controllers.
void streamVibration(float amplitude, u32 durationMs);

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

// Joining a game streamed by another host (core/ht4q/viewer.cpp).
struct StreamHost {
	std::string addr;
	int port;
	std::string name;
};
// Looks for hosts on the local network while on (UDP broadcast); hosts found in the last few seconds.
void viewerDiscover(bool on);
std::vector<StreamHost> viewerHosts();
void viewerJoin(const std::string& addr, int port);
void viewerLeave();
enum class ViewerState { Idle, Connecting, Connected, Failed };
ViewerState viewerState();
// The latest decoded frame: top-down RGBA (both eyes side by side when stereo), and its header data.
struct ViewerFrame {
	std::vector<u8> rgba;
	int width = 0, height = 0;
	u32 seq = 0;			// counts up with each new frame
	StreamFrameInfo info {};
	u64 hostTimeUs = 0;
};
// Copies the latest frame into out if newer than out.seq; false otherwise.
bool viewerFrame(ViewerFrame& out);
// Sends this player's pad and (in a headset) head pose; call once per displayed frame.
void viewerSendInput(const MapleInputState& pad);
void viewerSendXrHead(const XrHeadPose& head, float ipdMeters);

// Platform video decoder: decode() hands frames to viewerDecoded(), possibly from another thread.
class StreamDecoder
{
public:
	virtual ~StreamDecoder() = default;
	virtual void decode(const u8 *annexB, size_t size, int width, int height, u64 timeUs, u32 tag) = 0;
};
std::unique_ptr<StreamDecoder> createStreamDecoder();
// From the decoder: a decoded image, top-down RGBA rows (stride bytes apart).
void viewerDecoded(const u8 *rgba, int width, int height, size_t stride, u32 tag);
}
