/*
	HydroThunder4Quest: tiny main-RAM read/write server for reverse engineering.

	Enabled only when the environment variable HT4Q_MEMSERVER is set (value = TCP port,
	default 3264), and always in Quest (HT4Q_XR) builds. Unlike the GDB server it never halts the emulator or disables the
	dynarec. Listens on 127.0.0.1 only.

	Protocol (little-endian):
		'R' u32 addr u32 len          -> len bytes
		'W' u32 addr u32 len <data>   -> 1 byte: 0 = ok, 1 = out of range
		'I' u32 0 u32 16 <u32 active, u32 pressed, u16 lt, u16 rt, s16 x, s16 y>  -> 1 byte 0
		    Virtual controller: while active, `pressed` (DC_BTN_* bits) are held down and the
		    triggers/stick replace the real values. active = 1 | port << 8 (0 = A, 1 = B...); one per port.
		    Release: port << 8 (that port), 0 (all ports).
		'P' u32 addr u32 len <data>   -> 1 byte: 0 = queued, 1 = out of range
		    Like 'W' but applied on the emulator thread at the next maple poll, so writes to
		    dynarec-protected code pages invalidate blocks the same way game self-modification does.
		'H' u32 0 u32 28 <u32 active, f32 x, y, z, yaw, pitch, roll>  -> 1 byte 0
		    Head pose override for the camera hook (camhook.cpp): active = 1 | player << 8 (0 = player 1,
		    1 = player 2 in 2-player races); low byte 0 returns to the mouse (player 2: no head pose).
		'S' u32 0 u32 32 <f32 sep, conv, hudMinZ, hudDepth, unitsPerMeter, hudShrinkVr, skyCorrect, vrFovDeg>  -> 1 byte 0   stereo parameters
	Addresses are SH-4 main RAM (0x0c000000-0x0cffffff, any P0-P3 mirror).
	A bad read range returns len zero bytes.
*/
#include "types.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_if.h"
#include "emulator.h"
#include "ht4q.h"
#include "hw/maple/maple_cfg.h"
#include <mutex>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace ht4q
{

static std::mutex inputMutex;
struct InputOverride {
	u32 active;
	u32 pressed;
	u16 lt, rt;
	s16 x, y;
};
static InputOverride inputOverride[4];	// per port

struct Patch {
	u32 offset;
	std::vector<u8> data;
};
static std::vector<Patch> pendingPatches;

void memserverPoll(MapleInputState inputState[4])
{
	std::lock_guard<std::mutex> lock(inputMutex);
	for (const Patch& p : pendingPatches)
		memcpy(&mem_b[p.offset], p.data.data(), p.data.size());
	pendingPatches.clear();
	for (int port = 0; port < 4; port++)
	{
		const InputOverride& o = inputOverride[port];
		if (!o.active)
			continue;
		MapleInputState& st = inputState[port];
		st.kcode &= ~o.pressed;
		st.halfAxes[PJTI_L] = o.lt;
		st.halfAxes[PJTI_R] = o.rt;
		st.fullAxes[PJAI_X1] = o.x;
		st.fullAxes[PJAI_Y1] = o.y;
	}
}

static bool ramRange(u32 addr, u32 len, u32& offset)
{
	if ((addr & 0x1f000000) != 0x0c000000)
		return false;
	offset = addr & RAM_MASK;
	return len <= RAM_SIZE && offset + len <= RAM_SIZE;
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

static bool sendAll(int fd, const void *buf, size_t len)
{
	const u8 *p = (const u8 *)buf;
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

static void serveClient(int fd)
{
	std::vector<u8> buf;
	for (;;)
	{
		u8 cmd;
		u32 hdr[2];
		if (!recvAll(fd, &cmd, 1) || !recvAll(fd, hdr, sizeof(hdr)))
			break;
		u32 addr = hdr[0], len = hdr[1], offset;
		bool ok = emu.running() && ramRange(addr, len, offset);
		if (cmd == 'R')
		{
			buf.assign(len, 0);
			if (ok)
				memcpy(buf.data(), &mem_b[offset], len);
			if (!sendAll(fd, buf.data(), len))
				break;
		}
		else if (cmd == 'W')
		{
			buf.resize(len);
			if (!recvAll(fd, buf.data(), len))
				break;
			if (ok)
				memcpy(&mem_b[offset], buf.data(), len);
			u8 status = ok ? 0 : 1;
			if (!sendAll(fd, &status, 1))
				break;
		}
		else if (cmd == 'P')
		{
			buf.resize(len);
			if (!recvAll(fd, buf.data(), len))
				break;
			if (ok)
			{
				std::lock_guard<std::mutex> lock(inputMutex);
				pendingPatches.push_back({ offset, buf });
			}
			u8 status = ok ? 0 : 1;
			if (!sendAll(fd, &status, 1))
				break;
		}
		else if (cmd == 'I' && len == sizeof(InputOverride))
		{
			InputOverride o;
			if (!recvAll(fd, &o, len))
				break;
			{
				std::lock_guard<std::mutex> lock(inputMutex);
				const u32 port = (o.active >> 8) & 3;
				if (o.active & 0xff)
					inputOverride[port] = o;
				else if (o.active == 0)
					for (InputOverride& io : inputOverride)	// old clients: release all
						io.active = 0;
				else
					inputOverride[port].active = 0;
			}
			u8 status = 0;
			if (!sendAll(fd, &status, 1))
				break;
		}
		else if (cmd == 'S' && len == sizeof(StereoParams))
		{
			StereoParams params;
			if (!recvAll(fd, &params, sizeof(params)))
				break;
			stereoParams() = params;
			u8 status = 0;
			if (!sendAll(fd, &status, 1))
				break;
		}
		else if (cmd == 'C')	// current SH4 pc (where the game spends its time, sampled by tools)
		{
			u32 pc = emu.running() ? Sh4cntx.pc : 0;
			if (!sendAll(fd, &pc, 4))
				break;
		}
		else if (cmd == 'H' && len == 28)
		{
			u32 active;
			HeadPose pose;
			if (!recvAll(fd, &active, 4) || !recvAll(fd, &pose, sizeof(pose)))
				break;
			setHeadPoseOverride((active & 0xff) != 0, pose, (active >> 8) & 1);
			u8 status = 0;
			if (!sendAll(fd, &status, 1))
				break;
		}
		else
			break;
	}
	close(fd);
}

static void serverThread(int port)
{
	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(lfd, (sockaddr *)&sa, sizeof(sa)) != 0 || listen(lfd, 4) != 0)
	{
		ERROR_LOG(COMMON, "ht4q memserver: cannot listen on port %d", port);
		close(lfd);
		return;
	}
	NOTICE_LOG(COMMON, "ht4q memserver listening on 127.0.0.1:%d", port);
	for (;;)
	{
		int fd = accept(lfd, nullptr, nullptr);
		if (fd < 0)
			continue;
		std::thread(serveClient, fd).detach();
	}
}

static struct Starter
{
	Starter()
	{
		const char *env = getenv("HT4Q_MEMSERVER");
#ifdef HT4Q_XR
		if (env == nullptr)
			env = "3264";	// Quest build: reach it with `adb forward tcp:3264 tcp:3264`
#endif
		if (env == nullptr)
			return;
		int port = atoi(env);
		std::thread(serverThread, port > 0 ? port : 3264).detach();
	}
} starter;

}	// namespace ht4q
#else
namespace ht4q {
void memserverPoll(MapleInputState inputState[4]) {}
}
#endif
