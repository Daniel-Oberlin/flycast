/*
	HydroThunder4Quest: streams player 2's view of a 2-player race to a viewer on the network (another
	headset, or tools/ht4q/stream_view.py), which sends player 2's controller and head pose back.

	Enabled by HT4Q_STREAM=<TCP port> (all interfaces, one viewer at a time). Protocol, little-endian:
	  host -> viewer, per video frame: header (64 bytes)
	      char magic[4] "HT4V", u32 size, u32 frame, u32 flags (1 = keyframe, 2 = stereo: left | right eye),
	      u64 host time (us), u32 pose id, u16 width, u16 height,
	      f32 head q[4] (x y z w), f32 head p[3]: the viewer's head pose the frame was drawn for ('X'),
	      f32 zscreen: projection distance for 640 px wide (tan(half fov x) = 320 / zscreen, 4:3 per eye)
	    then `size` bytes of H.264 (Annex-B, parameter sets before each keyframe)
	  viewer -> host, messages of a type byte and a fixed payload:
	      'I' u32 pressed (DC_BTN_* bits held), u16 lt, u16 rt, s16 x, s16 y, u32 reserved   player 2's pad
	      'H' u32 active, f32 x, y, z, yaw, pitch, roll   player 2's head pose (ht4q::HeadPose), for tests
	      'X' f32 q[4] (x y z w), f32 p[3], f32 ipd (m)   player 2's headset in its LOCAL space: the host
	          then draws player 2 as a VR view (both eyes, for that pose)
	Discovery (core/ht4q/viewer.cpp): the host answers UDP datagrams "HT4Q?" on the same port number with
	"HT4Q!" u16 TCP port, then its name.
	Outside 2-player races (menus) the viewer gets the screen itself, so player 2 can join from the menus.
	The encoder is per platform (StreamEncoder): VideoToolbox on Apple, MediaCodec on Android.
*/
#include "stream.h"
#include "ht4q.h"
#include "input/gamepad.h"
#include "hw/maple/maple_cfg.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ht4q
{

static constexpr int FPS = 30;
static constexpr int BITRATE = 12000000;
static constexpr size_t MAX_QUEUED = 4;		// frames waiting to be sent; more = drop them, ask a keyframe
static constexpr size_t HEADER = 64;

struct Packet {
	std::vector<u8> data;	// header + payload
};

static std::mutex queueMutex;
static std::condition_variable queueCv;
static std::deque<Packet> queue;
static std::atomic<int> viewerFd { -1 };
static std::atomic<bool> needKeyframe { true };
static u32 frameSeq;

static std::mutex encoderMutex;		// render thread (encode) vs. connection changes
static std::unique_ptr<StreamEncoder> encoder;
static int encoderW, encoderH;
static std::mutex infoMutex;
static StreamFrameInfo infos[16];		// by encoder tag: header data of the frames being encoded
static u32 infoSeq;

static std::mutex inputMutex;
static struct {
	bool active;
	u32 pressed;
	u16 lt, rt;
	s16 x, y;
} remoteInput;

static u64 nowUs()
{
	using namespace std::chrono;
	return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

bool streamActive()
{
	return viewerFd >= 0;
}

void streamPacket(const u8 *annexB, size_t size, bool keyframe, u64 timeUs, u32 tag)
{
	if (viewerFd < 0)
		return;
	StreamFrameInfo info;
	{
		std::lock_guard<std::mutex> lock(infoMutex);
		info = infos[tag % std::size(infos)];
	}
	Packet p;
	p.data.resize(HEADER + size);
	u8 *h = p.data.data();
	memcpy(h, "HT4V", 4);
	const u32 sz = size, frame = frameSeq++, flags = (keyframe ? 1 : 0) | (info.stereo ? 2 : 0);
	const u16 w = encoderW, ht = encoderH;
	memcpy(h + 4, &sz, 4);
	memcpy(h + 8, &frame, 4);
	memcpy(h + 12, &flags, 4);
	memcpy(h + 16, &timeUs, 8);
	memcpy(h + 24, &info.poseId, 4);
	memcpy(h + 28, &w, 2);
	memcpy(h + 30, &ht, 2);
	memcpy(h + 32, info.headQ, 16);
	memcpy(h + 48, info.headP, 12);
	memcpy(h + 60, &info.zscreen, 4);
	memcpy(h + HEADER, annexB, size);
	std::lock_guard<std::mutex> lock(queueMutex);
	if (queue.size() >= MAX_QUEUED && !keyframe)
	{
		// the network can't keep up: drop what's queued and restart from a keyframe
		queue.clear();
		needKeyframe = true;
		return;
	}
	queue.push_back(std::move(p));
	queueCv.notify_one();
}

void streamFrame(const u8 *rgba, int width, int height, const StreamFrameInfo& info)
{
	if (viewerFd < 0)
		return;
	std::lock_guard<std::mutex> lock(encoderMutex);
	if (encoder == nullptr || encoderW != width || encoderH != height)
	{
		encoder.reset();
		encoderW = width;
		encoderH = height;
		encoder = createStreamEncoder(width, height, FPS, BITRATE);
		if (encoder == nullptr)
			return;
		needKeyframe = true;
	}
	const bool key = needKeyframe.exchange(false);
	u32 tag;
	{
		std::lock_guard<std::mutex> lock(infoMutex);
		tag = infoSeq++;
		infos[tag % std::size(infos)] = info;
	}
	encoder->encode(rgba, width, height, nowUs(), tag, key);
}

void streamPoll(MapleInputState inputState[4])
{
	std::lock_guard<std::mutex> lock(inputMutex);
	if (!remoteInput.active)
		return;
	MapleInputState& st = inputState[1];
	st.kcode = ~remoteInput.pressed;
	st.halfAxes[PJTI_L] = remoteInput.lt;
	st.halfAxes[PJTI_R] = remoteInput.rt;
	st.fullAxes[PJAI_X1] = remoteInput.x;
	st.fullAxes[PJAI_Y1] = remoteInput.y;
}

static bool recvAll(int fd, void *buf, size_t len)
{
	u8 *p = (u8 *)buf;
	while (len > 0)
	{
		ssize_t n = recv(fd, p, len, 0);
		if (n <= 0)
			return false;
		p += n;
		len -= n;
	}
	return true;
}

static bool sendAll(int fd, const u8 *p, size_t len)
{
	while (len > 0)
	{
		ssize_t n = send(fd, p, len, 0);
		if (n <= 0)
			return false;
		p += n;
		len -= n;
	}
	return true;
}

// viewer -> host messages, until the connection closes
static void readerThread(int fd)
{
	for (;;)
	{
		u8 type;
		if (!recvAll(fd, &type, 1))
			break;
		if (type == 'I')
		{
			u8 m[16];
			if (!recvAll(fd, m, sizeof(m)))
				break;
			std::lock_guard<std::mutex> lock(inputMutex);
			remoteInput.active = true;
			memcpy(&remoteInput.pressed, m, 4);
			memcpy(&remoteInput.lt, m + 4, 2);
			memcpy(&remoteInput.rt, m + 6, 2);
			memcpy(&remoteInput.x, m + 8, 2);
			memcpy(&remoteInput.y, m + 10, 2);
		}
		else if (type == 'H')
		{
			u32 active;
			HeadPose pose;
			if (!recvAll(fd, &active, 4) || !recvAll(fd, &pose, sizeof(pose)))
				break;
			setHeadPoseOverride(active != 0, pose, 1);
		}
		else if (type == 'X')
		{
			float m[8];
			if (!recvAll(fd, m, sizeof(m)))
				break;
			XrHeadPose head;
			memcpy(head.q, m, 16);
			memcpy(head.p, m + 4, 12);
			setRemoteXrHead(true, head, m[7]);
		}
		else
			break;
	}
	shutdown(fd, SHUT_RDWR);
}

static void serveViewer(int fd)
{
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		queue.clear();
	}
	needKeyframe = true;
	viewerFd = fd;
	std::thread reader(readerThread, fd);
	for (;;)
	{
		Packet p;
		{
			std::unique_lock<std::mutex> lock(queueMutex);
			queueCv.wait_for(lock, std::chrono::milliseconds(500), [] { return !queue.empty(); });
			if (queue.empty())
				continue;
			p = std::move(queue.front());
			queue.pop_front();
		}
		if (!sendAll(fd, p.data.data(), p.data.size()))
			break;
	}
	viewerFd = -1;
	shutdown(fd, SHUT_RDWR);
	reader.join();
	close(fd);
	{
		std::lock_guard<std::mutex> lock(inputMutex);
		remoteInput.active = false;
	}
	setHeadPoseOverride(false, {}, 1);
	setRemoteXrHead(false, {}, 0.f);
	{
		std::lock_guard<std::mutex> lock(encoderMutex);
		encoder.reset();
	}
	NOTICE_LOG(COMMON, "ht4q stream: viewer left");
}

// Answers viewers looking for a game on the network.
static void discoveryThread(int port)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(fd, (sockaddr *)&sa, sizeof(sa)) != 0)
	{
		ERROR_LOG(COMMON, "ht4q stream: no discovery on UDP port %d", port);
		close(fd);
		return;
	}
	char name[64] {};
	gethostname(name, sizeof(name) - 1);
	for (;;)
	{
		char buf[16];
		sockaddr_in peer{};
		socklen_t plen = sizeof(peer);
		ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (sockaddr *)&peer, &plen);
		if (n != 5 || memcmp(buf, "HT4Q?", 5) != 0)
			continue;
		u8 reply[6 + sizeof(name)];
		memcpy(reply, "HT4Q!", 5);
		const u16 p = port;
		memcpy(reply + 5, &p, 2);
		const size_t len = strlen(name);
		memcpy(reply + 7, name, len);
		sendto(fd, reply, 7 + len, 0, (sockaddr *)&peer, plen);
	}
}

static void serverThread(int port)
{
	std::thread(discoveryThread, port).detach();
	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(lfd, (sockaddr *)&sa, sizeof(sa)) != 0 || listen(lfd, 1) != 0)
	{
		ERROR_LOG(COMMON, "ht4q stream: cannot listen on port %d", port);
		close(lfd);
		return;
	}
	NOTICE_LOG(COMMON, "ht4q stream: waiting for a viewer on port %d", port);
	for (;;)
	{
		sockaddr_in peer{};
		socklen_t plen = sizeof(peer);
		int fd = accept(lfd, (sockaddr *)&peer, &plen);
		if (fd < 0)
			continue;
		char addr[32];
		inet_ntop(AF_INET, &peer.sin_addr, addr, sizeof(addr));
		NOTICE_LOG(COMMON, "ht4q stream: viewer %s", addr);
		serveViewer(fd);	// one at a time
	}
}

void streamStart(int port)
{
	static std::once_flag started;
	std::call_once(started, [port] { std::thread(serverThread, port).detach(); });
}

namespace {
struct StreamStarter	// not "Starter": memserver.cpp has one, and inline constructors merge
{
	StreamStarter()
	{
		const char *env = getenv("HT4Q_STREAM");
		if (env != nullptr && atoi(env) > 0)
			streamStart(atoi(env));
	}
} starter;
}

#if !defined(__APPLE__) && !defined(__ANDROID__)
std::unique_ptr<StreamEncoder> createStreamEncoder(int, int, int, int)
{
	WARN_LOG(COMMON, "ht4q stream: no video encoder on this platform yet");
	return nullptr;
}
#endif
#if !defined(__APPLE__) && !defined(__ANDROID__)
std::unique_ptr<StreamDecoder> createStreamDecoder()
{
	WARN_LOG(COMMON, "ht4q viewer: no video decoder on this platform yet");
	return nullptr;
}
#endif

}	// namespace ht4q

#else
namespace ht4q {
bool streamActive() { return false; }
void streamStart(int) {}
void streamFrame(const u8 *, int, int, const StreamFrameInfo&) {}
void streamPacket(const u8 *, size_t, bool, u64, u32) {}
void streamPoll(MapleInputState [4]) {}
}
#endif
