/*
	HydroThunder4Quest: joining a 2-player race streamed by another host (core/ht4q/stream.cpp, protocol
	there): finds hosts on the local network, receives and decodes player 2's view, and sends this
	player's controller and head pose. The emulator doesn't run here; the launcher shows the frames
	(desktop) or the XR presenter does (headset). The host's sound plays through Flycast's audio backend
	(a short jitter buffer, at most ~100 ms behind), its rumble for player 2 on our controllers.
*/
#include "stream.h"
#include "hw/maple/maple_cfg.h"
#include "audio/audiostream.h"
#include <algorithm>
#include <deque>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace ht4q
{

static constexpr int DEFAULT_PORT = 3266;

using Clock = std::chrono::steady_clock;

// --- discovery

static std::mutex hostsMutex;
static std::vector<std::pair<StreamHost, Clock::time_point>> hosts;
static std::atomic<bool> discovering { false };
static std::thread discoverThread;

static void discoverLoop()
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
	timeval tv { 0, 200000 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	sockaddr_in bc{};
	bc.sin_family = AF_INET;
	bc.sin_port = htons(DEFAULT_PORT);
	bc.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	Clock::time_point lastAsk;
	while (discovering)
	{
		if (Clock::now() - lastAsk > std::chrono::seconds(1))
		{
			sendto(fd, "HT4Q?", 5, 0, (sockaddr *)&bc, sizeof(bc));
			// and this machine, for testing a host and a viewer side by side
			sockaddr_in lo = bc;
			lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			sendto(fd, "HT4Q?", 5, 0, (sockaddr *)&lo, sizeof(lo));
			lastAsk = Clock::now();
		}
		u8 buf[128];
		sockaddr_in peer{};
		socklen_t plen = sizeof(peer);
		ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (sockaddr *)&peer, &plen);
		if (n < 7 || memcmp(buf, "HT4Q!", 5) != 0)
			continue;
		StreamHost h;
		char addr[32];
		inet_ntop(AF_INET, &peer.sin_addr, addr, sizeof(addr));
		h.addr = addr;
		u16 port;
		memcpy(&port, buf + 5, 2);
		h.port = port;
		h.name.assign((const char *)buf + 7, n - 7);
		std::lock_guard<std::mutex> lock(hostsMutex);
		auto it = std::find_if(hosts.begin(), hosts.end(), [&](const auto& e) { return e.first.addr == h.addr && e.first.port == h.port; });
		if (it != hosts.end())
			*it = { h, Clock::now() };
		else
			hosts.emplace_back(h, Clock::now());
	}
	close(fd);
}

void viewerDiscover(bool on)
{
	if (on == discovering)
		return;
	if (on)
	{
		discovering = true;
		discoverThread = std::thread(discoverLoop);
	}
	else
	{
		discovering = false;
		discoverThread.join();
	}
}

std::vector<StreamHost> viewerHosts()
{
	std::lock_guard<std::mutex> lock(hostsMutex);
	std::vector<StreamHost> out;
	for (const auto& [h, seen] : hosts)
		if (Clock::now() - seen < std::chrono::seconds(4))
			out.push_back(h);
	return out;
}

// --- connection

static std::atomic<ViewerState> state { ViewerState::Idle };
static std::atomic<int> sockFd { -1 };
static std::thread recvThread;
static std::mutex sendMutex;

static std::mutex frameMutex;
static ViewerFrame latest;
static StreamFrameInfo pending[16];		// header data by decoder tag
static u64 pendingTime[16];

ViewerState viewerState() { return state; }

// --- sound: received samples, played by a thread feeding the audio backend

static std::mutex audioMutex;
static std::deque<s16> audioQueue;		// right, left pairs
static std::atomic<bool> audioRunning { false };
static constexpr size_t AUDIO_MAX = 4410 * 2, AUDIO_KEEP = 2205 * 2;	// 100 ms, 50 ms

static void audioLoop()
{
	InitAudio();
	std::vector<s16> chunk;
	while (audioRunning)
	{
		{
			std::lock_guard<std::mutex> lock(audioMutex);
			if (audioQueue.size() > AUDIO_MAX)		// fell behind (host faster, or a stall): catch up
				audioQueue.erase(audioQueue.begin(), audioQueue.end() - AUDIO_KEEP);
			const size_t n = std::min(audioQueue.size(), (size_t)882);
			chunk.assign(audioQueue.begin(), audioQueue.begin() + n);
			audioQueue.erase(audioQueue.begin(), audioQueue.begin() + n);
		}
		if (chunk.empty())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			continue;
		}
		for (size_t i = 0; i + 1 < chunk.size(); i += 2)
			WriteSample(chunk[i], chunk[i + 1]);	// blocks while the backend's buffer is full
	}
	TermAudio();
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

static void sendMsg(const void *data, size_t len)
{
	const int fd = sockFd;
	if (fd < 0)
		return;
	std::lock_guard<std::mutex> lock(sendMutex);
	const u8 *p = (const u8 *)data;
	while (len > 0)
	{
		ssize_t n = send(fd, p, len, 0);
		if (n <= 0)
			return;
		p += n;
		len -= n;
	}
}

static void receive(std::string addr, int port)
{
	addrinfo hints{}, *res = nullptr;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	int fd = -1;
	if (getaddrinfo(addr.c_str(), std::to_string(port).c_str(), &hints, &res) == 0 && res != nullptr)
	{
		fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) != 0)
		{
			close(fd);
			fd = -1;
		}
		freeaddrinfo(res);
	}
	if (fd < 0)
	{
		WARN_LOG(COMMON, "ht4q viewer: cannot connect to %s:%d", addr.c_str(), port);
		state = ViewerState::Failed;
		return;
	}
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	sockFd = fd;
	state = ViewerState::Connected;
	NOTICE_LOG(COMMON, "ht4q viewer: joined %s:%d", addr.c_str(), port);
	std::unique_ptr<StreamDecoder> decoder = createStreamDecoder();
	std::vector<u8> payload;
	u32 tag = 0;
	audioRunning = true;
	std::thread audio(audioLoop);
	for (;;)
	{
		u8 h[64];
		if (!recvAll(fd, h, sizeof(h)))
			break;
		if (memcmp(h, "HT4A", 4) == 0 || memcmp(h, "HT4R", 4) == 0)
		{
			u32 size;
			memcpy(&size, h + 4, 4);
			if (size > 65536)
				break;
			payload.resize(size);
			if (!recvAll(fd, payload.data(), size))
				break;
			if (h[3] == 'A')
			{
				const s16 *smp = (const s16 *)payload.data();
				std::lock_guard<std::mutex> lock(audioMutex);
				audioQueue.insert(audioQueue.end(), smp, smp + size / 2);
			}
			else if (size >= 8)
			{
				float amplitude;
				u32 ms;
				memcpy(&amplitude, payload.data(), 4);
				memcpy(&ms, payload.data() + 4, 4);
				setRemoteHaptics(amplitude, ms);
			}
			continue;
		}
		if (memcmp(h, "HT4V", 4) != 0)
			break;
		u32 size, flags;
		u16 width, height;
		memcpy(&size, h + 4, 4);
		memcpy(&flags, h + 12, 4);
		memcpy(&width, h + 28, 2);
		memcpy(&height, h + 30, 2);
		StreamFrameInfo info{};
		u64 timeUs;
		memcpy(&timeUs, h + 16, 8);
		memcpy(&info.poseId, h + 24, 4);
		info.stereo = flags & 2;
		memcpy(info.headQ, h + 32, 16);
		memcpy(info.headP, h + 48, 12);
		memcpy(&info.zscreen, h + 60, 4);
		if (size > 16 * 1024 * 1024)
			break;
		payload.resize(size);
		if (!recvAll(fd, payload.data(), size))
			break;
		if (decoder == nullptr)
			continue;
		{
			std::lock_guard<std::mutex> lock(frameMutex);
			pending[tag % std::size(pending)] = info;
			pendingTime[tag % std::size(pending)] = timeUs;
		}
		decoder->decode(payload.data(), size, width, height, timeUs, tag++);
	}
	decoder.reset();
	audioRunning = false;
	audio.join();
	{
		std::lock_guard<std::mutex> lock(audioMutex);
		audioQueue.clear();
	}
	{
		std::lock_guard<std::mutex> lock(sendMutex);
		sockFd = -1;
	}
	close(fd);
	if (state == ViewerState::Connected)
		state = ViewerState::Failed;	// the host went away
	NOTICE_LOG(COMMON, "ht4q viewer: disconnected");
}

void viewerJoin(const std::string& addr, int port)
{
	viewerLeave();
	state = ViewerState::Connecting;
	recvThread = std::thread(receive, addr, port > 0 ? port : DEFAULT_PORT);
}

void viewerLeave()
{
	if (const int fd = sockFd; fd >= 0)
		shutdown(fd, SHUT_RDWR);
	if (recvThread.joinable())
		recvThread.join();
	state = ViewerState::Idle;
}

void viewerDecoded(const u8 *rgba, int width, int height, size_t stride, u32 tag)
{
	std::lock_guard<std::mutex> lock(frameMutex);
	latest.rgba.resize((size_t)width * height * 4);
	for (int y = 0; y < height; y++)
		memcpy(&latest.rgba[(size_t)y * width * 4], rgba + y * stride, (size_t)width * 4);
	latest.width = width;
	latest.height = height;
	latest.info = pending[tag % std::size(pending)];
	latest.hostTimeUs = pendingTime[tag % std::size(pending)];
	latest.seq++;
}

bool viewerFrame(ViewerFrame& out)
{
	std::lock_guard<std::mutex> lock(frameMutex);
	if (latest.seq == out.seq || latest.width == 0)
		return false;
	out = latest;
	return true;
}

void viewerSendInput(const MapleInputState& pad)
{
	u8 m[17] { 'I' };
	const u32 pressed = ~pad.kcode;
	const u16 lt = pad.halfAxes[PJTI_L], rt = pad.halfAxes[PJTI_R];
	const s16 x = pad.fullAxes[PJAI_X1], y = pad.fullAxes[PJAI_Y1];
	memcpy(m + 1, &pressed, 4);
	memcpy(m + 5, &lt, 2);
	memcpy(m + 7, &rt, 2);
	memcpy(m + 9, &x, 2);
	memcpy(m + 11, &y, 2);
	sendMsg(m, sizeof(m));
}

void viewerSendXrHead(const XrHeadPose& head, float ipdMeters)
{
	u8 m[33] { 'X' };
	memcpy(m + 1, head.q, 16);
	memcpy(m + 17, head.p, 12);
	memcpy(m + 29, &ipdMeters, 4);
	sendMsg(m, sizeof(m));
}

}	// namespace ht4q

#else
namespace ht4q {
void viewerDiscover(bool) {}
std::vector<StreamHost> viewerHosts() { return {}; }
void viewerJoin(const std::string&, int) {}
void viewerLeave() {}
ViewerState viewerState() { return ViewerState::Idle; }
bool viewerFrame(ViewerFrame&) { return false; }
void viewerSendInput(const MapleInputState&) {}
void viewerSendXrHead(const XrHeadPose&, float) {}
void viewerDecoded(const u8 *, int, int, size_t, u32) {}
}
#endif
