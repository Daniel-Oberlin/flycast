/*
	HydroThunder4Quest: H.264 stream encoder on Apple platforms (VideoToolbox), see stream.cpp.
	Real-time, low-latency rate control, no frame reordering; output converted to Annex-B with the
	parameter sets before each keyframe.
*/
#include "stream.h"
#include <VideoToolbox/VideoToolbox.h>
#include <cstring>
#include <vector>

namespace ht4q
{

class VtEncoder : public StreamEncoder
{
public:
	bool init(int width, int height, int fps, int bitrate)
	{
		const void *specKeys[] { kVTVideoEncoderSpecification_EnableLowLatencyRateControl };
		const void *specValues[] { kCFBooleanTrue };
		CFDictionaryRef spec = CFDictionaryCreate(nullptr, specKeys, specValues, 1,
				&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		int32_t fmt = kCVPixelFormatType_32BGRA;
		CFNumberRef fmtNum = CFNumberCreate(nullptr, kCFNumberSInt32Type, &fmt);
		CFNumberRef wNum = CFNumberCreate(nullptr, kCFNumberIntType, &width);
		CFNumberRef hNum = CFNumberCreate(nullptr, kCFNumberIntType, &height);
		const void *attrKeys[] { kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey, kCVPixelBufferHeightKey };
		const void *attrValues[] { fmtNum, wNum, hNum };
		CFDictionaryRef attrs = CFDictionaryCreate(nullptr, attrKeys, attrValues, 3,
				&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		OSStatus st = VTCompressionSessionCreate(nullptr, width, height, kCMVideoCodecType_H264, spec, attrs,
				nullptr, &VtEncoder::output, this, &session);
		CFRelease(spec);
		CFRelease(attrs);
		CFRelease(fmtNum);
		CFRelease(wNum);
		CFRelease(hNum);
		if (st != noErr)
		{
			ERROR_LOG(COMMON, "ht4q stream: VTCompressionSessionCreate failed (%d)", (int)st);
			session = nullptr;
			return false;
		}
		auto setInt = [this](CFStringRef key, int v) {
			CFNumberRef n = CFNumberCreate(nullptr, kCFNumberIntType, &v);
			VTSessionSetProperty(session, key, n);
			CFRelease(n);
		};
		VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
		VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
		VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel);
		setInt(kVTCompressionPropertyKey_AverageBitRate, bitrate);
		setInt(kVTCompressionPropertyKey_ExpectedFrameRate, fps);
		setInt(kVTCompressionPropertyKey_MaxKeyFrameInterval, fps * 10);
		VTCompressionSessionPrepareToEncodeFrames(session);
		NOTICE_LOG(COMMON, "ht4q stream: H.264 %dx%d, %d Mb/s", width, height, bitrate / 1000000);
		return true;
	}

	~VtEncoder() override
	{
		if (session != nullptr)
		{
			VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
			VTCompressionSessionInvalidate(session);
			CFRelease(session);
		}
	}

	void encode(const u8 *rgba, int width, int height, u64 timeUs, u32 poseId, bool keyframe) override
	{
		CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(session);
		CVPixelBufferRef pb = nullptr;
		if (pool == nullptr || CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &pb) != kCVReturnSuccess)
			return;
		CVPixelBufferLockBaseAddress(pb, 0);
		u8 *dst = (u8 *)CVPixelBufferGetBaseAddress(pb);
		const size_t stride = CVPixelBufferGetBytesPerRow(pb);
		for (int y = 0; y < height; y++)
		{
			const u8 *src = rgba + (size_t)y * width * 4;
			u8 *row = dst + y * stride;
			for (int x = 0; x < width * 4; x += 4)
			{
				row[x] = src[x + 2];
				row[x + 1] = src[x + 1];
				row[x + 2] = src[x];
				row[x + 3] = 0xff;
			}
		}
		CVPixelBufferUnlockBaseAddress(pb, 0);
		CFDictionaryRef props = nullptr;
		if (keyframe)
		{
			const void *k[] { kVTEncodeFrameOptionKey_ForceKeyFrame };
			const void *v[] { kCFBooleanTrue };
			props = CFDictionaryCreate(nullptr, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		}
		// the pose id and time ride along as the frame's refcon
		Tag *tag = new Tag { timeUs, poseId };
		OSStatus st = VTCompressionSessionEncodeFrame(session, pb, CMTimeMake((int64_t)timeUs, 1000000), kCMTimeInvalid,
				props, tag, nullptr);
		if (st != noErr)
			delete tag;
		if (props != nullptr)
			CFRelease(props);
		CVPixelBufferRelease(pb);
	}

private:
	struct Tag { u64 timeUs; u32 poseId; };

	static void output(void *self, void *refcon, OSStatus status, VTEncodeInfoFlags, CMSampleBufferRef sample)
	{
		Tag *tag = (Tag *)refcon;
		if (status == noErr && sample != nullptr)
			((VtEncoder *)self)->send(sample, *tag);
		delete tag;
	}

	void send(CMSampleBufferRef sample, const Tag& tag)
	{
		static const u8 startCode[4] { 0, 0, 0, 1 };
		bool keyframe = true;
		CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sample, false);
		if (att != nullptr && CFArrayGetCount(att) > 0)
		{
			CFDictionaryRef d = (CFDictionaryRef)CFArrayGetValueAtIndex(att, 0);
			keyframe = !CFDictionaryContainsKey(d, kCMSampleAttachmentKey_NotSync);
		}
		out.clear();
		if (keyframe)
		{
			CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(sample);
			size_t count = 0;
			CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, 0, nullptr, nullptr, &count, nullptr);
			for (size_t i = 0; i < count; i++)
			{
				const uint8_t *ps;
				size_t psSize;
				if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, i, &ps, &psSize, nullptr, nullptr) == noErr)
				{
					out.insert(out.end(), startCode, startCode + 4);
					out.insert(out.end(), ps, ps + psSize);
				}
			}
		}
		// AVCC (4-byte big-endian lengths) -> Annex-B
		CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
		size_t total = 0;
		char *data = nullptr;
		if (CMBlockBufferGetDataPointer(block, 0, nullptr, &total, &data) != kCMBlockBufferNoErr)
			return;
		for (size_t off = 0; off + 4 <= total;)
		{
			const u8 *p = (const u8 *)data + off;
			const size_t n = (size_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
			if (off + 4 + n > total)
				break;
			out.insert(out.end(), startCode, startCode + 4);
			out.insert(out.end(), p + 4, p + 4 + n);
			off += 4 + n;
		}
		streamPacket(out.data(), out.size(), keyframe, tag.timeUs, tag.poseId);
	}

	VTCompressionSessionRef session = nullptr;
	std::vector<u8> out;	// encoder callbacks are serialized
};

std::unique_ptr<StreamEncoder> createStreamEncoder(int width, int height, int fps, int bitrate)
{
	auto enc = std::make_unique<VtEncoder>();
	if (!enc->init(width, height, fps, bitrate))
		return nullptr;
	return enc;
}

}	// namespace ht4q
