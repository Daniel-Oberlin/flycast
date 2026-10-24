/*
	HydroThunder4Quest: H.264 stream encoder on Android (NDK MediaCodec, the headset's hardware
	encoder), see stream.cpp. First version: the RGBA image is converted to NV12 on the CPU and queued
	as a byte buffer; a GL input surface would save the readback and the conversion.
	Low latency, real-time priority, no B-frames; the codec's parameter sets go before each keyframe.
*/
#include "stream.h"
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace ht4q
{

static constexpr int COLOR_FormatYUV420SemiPlanar = 21;	// NV12
static constexpr int BUFFER_FLAG_KEY_FRAME = 1;
static constexpr int BUFFER_FLAG_CODEC_CONFIG = 2;

class MediaCodecEncoder : public StreamEncoder
{
public:
	bool init(int w, int h, int fps, int bitrate)
	{
		width = w;
		height = h;
		codec = AMediaCodec_createEncoderByType("video/avc");
		if (codec == nullptr)
		{
			ERROR_LOG(COMMON, "ht4q stream: no H.264 encoder");
			return false;
		}
		AMediaFormat *fmt = AMediaFormat_new();
		AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
		AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, w);
		AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, h);
		AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, COLOR_FormatYUV420SemiPlanar);
		AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
		AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, fps);
		AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 10);
		AMediaFormat_setInt32(fmt, "bitrate-mode", 2);		// CBR: steady for the network
		AMediaFormat_setInt32(fmt, "priority", 0);			// real time
		AMediaFormat_setInt32(fmt, "low-latency", 1);
		AMediaFormat_setInt32(fmt, "prepend-sps-pps-to-idr-frames", 1);
		media_status_t st = AMediaCodec_configure(codec, fmt, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
		AMediaFormat_delete(fmt);
		if (st != AMEDIA_OK || AMediaCodec_start(codec) != AMEDIA_OK)
		{
			ERROR_LOG(COMMON, "ht4q stream: H.264 encoder configuration failed (%d)", (int)st);
			AMediaCodec_delete(codec);
			codec = nullptr;
			return false;
		}
		stride = w;
		sliceHeight = h;
		if (AMediaFormat *in = AMediaCodec_getInputFormat(codec))
		{
			int32_t v;
			if (AMediaFormat_getInt32(in, "stride", &v) && v >= w)
				stride = v;
			if (AMediaFormat_getInt32(in, "slice-height", &v) && v >= h)
				sliceHeight = v;
			AMediaFormat_delete(in);
		}
		running = true;
		drainThread = std::thread(&MediaCodecEncoder::drain, this);
		NOTICE_LOG(COMMON, "ht4q stream: H.264 %dx%d (stride %d), %d Mb/s", w, h, stride, bitrate / 1000000);
		return true;
	}

	~MediaCodecEncoder() override
	{
		running = false;
		if (drainThread.joinable())
			drainThread.join();
		if (codec != nullptr)
		{
			AMediaCodec_stop(codec);
			AMediaCodec_delete(codec);
		}
	}

	void encode(const u8 *rgba, int w, int h, u64 timeUs, u32 tag, bool keyframe) override
	{
		if (w != width || h != height)
			return;
		if (keyframe)
		{
			AMediaFormat *p = AMediaFormat_new();
			AMediaFormat_setInt32(p, "request-sync", 0);
			AMediaCodec_setParameters(codec, p);
			AMediaFormat_delete(p);
		}
		ssize_t idx = AMediaCodec_dequeueInputBuffer(codec, 2000);
		if (idx < 0)
			return;		// encoder busy: skip this frame
		size_t cap = 0;
		u8 *buf = AMediaCodec_getInputBuffer(codec, idx, &cap);
		const size_t need = (size_t)stride * sliceHeight * 3 / 2;
		if (buf == nullptr || cap < need)
		{
			AMediaCodec_queueInputBuffer(codec, idx, 0, 0, timeUs, 0);
			return;
		}
		toNv12(rgba, buf, buf + (size_t)stride * sliceHeight);
		{
			std::lock_guard<std::mutex> lock(tagMutex);
			tags[tagIndex++ % std::size(tags)] = { timeUs, tag };
		}
		AMediaCodec_queueInputBuffer(codec, idx, 0, need, timeUs, 0);
	}

private:
	// BT.601 limited range, chroma from the top-left pixel of each 2x2 block
	void toNv12(const u8 *rgba, u8 *yPlane, u8 *uvPlane) const
	{
		for (int y = 0; y < height; y++)
		{
			const u8 *s = rgba + (size_t)y * width * 4;
			u8 *d = yPlane + (size_t)y * stride;
			for (int x = 0; x < width; x++, s += 4)
				d[x] = (u8)((66 * s[0] + 129 * s[1] + 25 * s[2] + 128) >> 8) + 16;
		}
		for (int y = 0; y < height; y += 2)
		{
			const u8 *s = rgba + (size_t)y * width * 4;
			u8 *d = uvPlane + (size_t)(y / 2) * stride;
			for (int x = 0; x < width; x += 2, s += 8)
			{
				d[x] = (u8)(((-38 * s[0] - 74 * s[1] + 112 * s[2] + 128) >> 8) + 128);
				d[x + 1] = (u8)(((112 * s[0] - 94 * s[1] - 18 * s[2] + 128) >> 8) + 128);
			}
		}
	}

	u32 tagFor(u64 timeUs)
	{
		std::lock_guard<std::mutex> lock(tagMutex);
		for (const Tag& t : tags)
			if (t.timeUs == timeUs)
				return t.tag;
		return 0;
	}

	void drain()
	{
		std::vector<u8> config, out;
		while (running)
		{
			AMediaCodecBufferInfo info;
			ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);
			if (idx < 0)
				continue;	// try again, format or buffers changed
			size_t cap = 0;
			const u8 *data = AMediaCodec_getOutputBuffer(codec, idx, &cap);
			if (data != nullptr && info.size > 0)
			{
				const u8 *p = data + info.offset;
				if (info.flags & BUFFER_FLAG_CODEC_CONFIG)
					config.assign(p, p + info.size);
				else
				{
					const bool key = info.flags & BUFFER_FLAG_KEY_FRAME;
					out.clear();
					if (key && !config.empty())
						out = config;
					out.insert(out.end(), p, p + info.size);
					streamPacket(out.data(), out.size(), key, info.presentationTimeUs, tagFor(info.presentationTimeUs));
				}
			}
			AMediaCodec_releaseOutputBuffer(codec, idx, false);
		}
	}

	struct Tag { u64 timeUs; u32 tag; };
	AMediaCodec *codec = nullptr;
	int width = 0, height = 0, stride = 0, sliceHeight = 0;
	std::atomic<bool> running { false };
	std::thread drainThread;
	std::mutex tagMutex;
	Tag tags[16] {};
	u32 tagIndex = 0;
};

std::unique_ptr<StreamEncoder> createStreamEncoder(int width, int height, int fps, int bitrate)
{
	auto enc = std::make_unique<MediaCodecEncoder>();
	if (!enc->init(width, height, fps, bitrate))
		return nullptr;
	return enc;
}

}	// namespace ht4q
