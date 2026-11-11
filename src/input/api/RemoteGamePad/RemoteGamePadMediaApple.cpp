// Remote GamePad media on macOS: the media UDP port (dual-stack), the GamePad video encoder
// (VideoToolbox H.264, low-latency rate control), the phone camera decoder and the phone
// microphone buffer. See RemoteGamePadMedia.h and the app's PROTOCOL.md ("Media").

#include "input/api/RemoteGamePad/RemoteGamePadMedia.h"
#include "input/api/RemoteGamePad/RemoteGamePadMediaCore.h"
#include "input/api/RemoteGamePad/RemoteGamePadUdp.h"
void SetThreadName(const char* name); // util/helpers/helpers.h

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CVPixelBuffer.h>
#include <CoreVideo/CVPixelBufferPool.h>
#include <VideoToolbox/VideoToolbox.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cmath>
#include <deque>

using namespace RemoteGamePad;
using namespace std::chrono_literals;

namespace
{
	constexpr int kVideoWidth = 854;
	constexpr int kVideoHeight = 480;
	constexpr int kVideoFps = 60;
	constexpr int kPoolBuffers = 4;           // frames in flight between the renderer and the encoder
	constexpr uint64_t kSubscriberTimeoutUs = 2000000; // no feedback for 2 s: stop the video
	constexpr uint64_t kEncoderIdleUs = 10000000;      // tear the encoder down after 10 s unused
	constexpr uint64_t kKeyframeMinIntervalUs = 200000; // honour keyframe requests at most every 200 ms
	constexpr uint64_t kControlIntervalUs = 250000;     // MEDIA_CONTROL at 4 Hz
	constexpr uint64_t kCameraFreshUs = 1000000;        // a phone camera frame older than this is stale

	uint64_t now_us()
	{
		return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	void set_bool(CFMutableDictionaryRef d, CFStringRef key, bool v) { CFDictionarySetValue(d, key, v ? kCFBooleanTrue : kCFBooleanFalse); }
	void set_int(CFMutableDictionaryRef d, CFStringRef key, int32_t v)
	{
		CFNumberRef n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &v);
		CFDictionarySetValue(d, key, n);
		CFRelease(n);
	}
	OSStatus set_prop_int(VTSessionRef s, CFStringRef key, int32_t v)
	{
		CFNumberRef n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &v);
		const OSStatus st = VTSessionSetProperty(s, key, n);
		CFRelease(n);
		return st;
	}
	OSStatus set_prop_double(VTSessionRef s, CFStringRef key, double v)
	{
		CFNumberRef n = CFNumberCreate(nullptr, kCFNumberDoubleType, &v);
		const OSStatus st = VTSessionSetProperty(s, key, n);
		CFRelease(n);
		return st;
	}

	// running averages over a window, reset by Take()
	struct Avg
	{
		double sum = 0;
		uint32_t n = 0;
		void Add(double v) { sum += v; n++; }
		double Mean() const { return n ? sum / n : 0; }
	};

	class MediaHub
	{
	public:
		~MediaHub() { Stop(); }

		bool Start(uint16_t port, std::string& error)
		{
			std::scoped_lock lock(m_start_mutex);
			if (m_udp.IsOpen())
				return true;
			// IPv4 + IPv6-only sockets; replies leave from the address the phone sent to (RemoteGamePadUdp.h)
			if (!m_udp.Open(port, error, true))
				return false;
			const bool v6 = m_udp.HasV6();

			m_port = port;
			m_dual_stack = v6;
			m_running = true;
			m_thread = std::thread(&MediaHub::Loop, this);
			cemuLog_log(LogType::Force, "Remote GamePad: media on UDP port {} ({})", port, v6 ? "IPv4 + IPv6" : "IPv4 only");
			return true;
		}

		void Stop()
		{
			m_running = false;
			if (m_thread.joinable())
				m_thread.join();
			DestroyEncoder();
			DestroyCameraDecoder();
			m_udp.Close();
		}

		uint16_t Port() const { return m_udp.IsOpen() ? m_port : 0; }

		void SetClient(uint32_t id, uint16_t caps, const std::string& name)
		{
			std::scoped_lock lock(m_mutex);
			if (id != m_client_id)
			{
				m_have_subscriber = false;
				m_video_subscribed = false;
				m_phone = {};
			}
			m_client_id = id;
			m_client_caps = caps;
			m_client_name = name;
			m_client_valid = true;
		}

		void ClearClient()
		{
			std::scoped_lock lock(m_mutex);
			m_client_valid = false;
			m_have_subscriber = false;
			m_video_subscribed = false;
		}

		// ---------------- video

		bool VideoWanted() const { return m_video_wanted.load(std::memory_order_relaxed); }

		void* AcquireVideoBuffer()
		{
			std::scoped_lock lock(m_encoder_mutex);
			if (!m_session || !m_pool || !VideoWanted())
				return nullptr;
			static CFDictionaryRef aux = [] {
				CFMutableDictionaryRef d = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
				set_int(d, kCVPixelBufferPoolAllocationThresholdKey, kPoolBuffers);
				return (CFDictionaryRef)d;
			}();
			CVPixelBufferRef pb = nullptr;
			const CVReturn r = CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(nullptr, m_pool, aux, &pb);
			if (r != kCVReturnSuccess || !pb)
			{
				m_stat_capture_busy++;
				return nullptr;
			}
			return pb;
		}

		void SubmitVideoBuffer(void* p, uint64_t capture_us)
		{
			CVPixelBufferRef pb = (CVPixelBufferRef)p;
			{
				// how evenly the game delivers GamePad frames
				std::scoped_lock lock(m_stats_mutex);
				if (m_last_capture_us && capture_us > m_last_capture_us)
				{
					const double ms = (capture_us - m_last_capture_us) / 1000.0;
					m_stat_interval.Add(ms);
					m_stat_interval_sq += ms * ms;
					m_stat_interval_max = std::max(m_stat_interval_max, ms);
					const int bucket = ms < 12 ? 0 : ms < 20 ? 1 : ms < 28 ? 2 : ms < 40 ? 3 : 4;
					m_stat_interval_hist[bucket]++;
				}
				m_last_capture_us = capture_us;
				m_stat_captures++;
			}
			{
				std::scoped_lock lock(m_encoder_mutex);
				if (m_session && VideoWanted())
				{
					CFDictionaryRef props = nullptr;
					if (m_force_keyframe.exchange(false))
					{
						const void* k[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
						const void* v[] = {kCFBooleanTrue};
						props = CFDictionaryCreate(nullptr, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
						m_stat_keyframes_forced++;
					}
					const OSStatus st = VTCompressionSessionEncodeFrame(m_session, pb, CMTimeMake((int64_t)capture_us, 1000000), kCMTimeInvalid, props, nullptr, nullptr);
					if (props)
						CFRelease(props);
					if (st != noErr)
						m_stat_encode_errors++;
				}
			}
			CVPixelBufferRelease(pb);
		}

		static void* GetIOSurface(void* pb) { return pb ? (void*)CVPixelBufferGetIOSurface((CVPixelBufferRef)pb) : nullptr; }

		// ---------------- camera

		bool PhoneCameraSelected() const
		{
			std::scoped_lock lock(m_mutex);
			return m_camera_source == Media::CameraSource::Auto && PhoneAlive() && (m_client_caps & kCapCamera);
		}

		void SetCameraWanted(bool w)
		{
			if (m_camera_wanted.exchange(w) != w)
				m_control_dirty = true;
		}

		bool GetCameraFrameNV12(uint8_t* dst, uint32_t width, uint32_t height, uint32_t pitch)
		{
			CVPixelBufferRef pb = nullptr;
			{
				std::scoped_lock lock(m_camera_mutex);
				if (!m_camera_latest || now_us() - m_camera_latest_us > kCameraFreshUs)
					return false;
				pb = CVPixelBufferRetain(m_camera_latest);
			}
			bool ok = false;
			if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess)
			{
				if (CVPixelBufferGetPlaneCount(pb) == 2)
				{
					const uint32_t sw = (uint32_t)CVPixelBufferGetWidth(pb), sh = (uint32_t)CVPixelBufferGetHeight(pb);
					const uint8_t* sy = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
					const uint8_t* suv = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 1);
					const size_t sy_pitch = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), suv_pitch = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
					// centre crop to the target aspect, then nearest-neighbour scale (a plain copy at 640x480)
					uint32_t cw = sw, ch = sh;
					if ((uint64_t)sw * height > (uint64_t)sh * width)
						cw = (uint32_t)((uint64_t)sh * width / height) & ~1u;
					else
						ch = (uint32_t)((uint64_t)sw * height / width) & ~1u;
					const uint32_t x0 = ((sw - cw) / 2) & ~1u, y0 = ((sh - ch) / 2) & ~1u;
					uint8_t* dy = dst;
					uint8_t* duv = dst + (size_t)pitch * height;
					for (uint32_t y = 0; y < height; ++y)
					{
						const uint8_t* row = sy + (size_t)(y0 + (uint64_t)y * ch / height) * sy_pitch + x0;
						if (cw == width)
							memcpy(dy + (size_t)y * pitch, row, width);
						else
							for (uint32_t x = 0; x < width; ++x)
								dy[(size_t)y * pitch + x] = row[(uint64_t)x * cw / width];
					}
					for (uint32_t y = 0; y < height / 2; ++y)
					{
						const uint8_t* row = suv + (size_t)(y0 / 2 + (uint64_t)y * ch / height) * suv_pitch + x0;
						if (cw == width)
							memcpy(duv + (size_t)y * pitch, row, width);
						else
							for (uint32_t x = 0; x < width / 2; ++x)
							{
								const size_t sx = ((uint64_t)x * cw / width) * 2;
								duv[(size_t)y * pitch + 2 * x] = row[sx];
								duv[(size_t)y * pitch + 2 * x + 1] = row[sx + 1];
							}
					}
					ok = true;
				}
				CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
			}
			CVPixelBufferRelease(pb);
			return ok;
		}

		// ---------------- microphone

		bool PhoneMicSelected() const
		{
			std::scoped_lock lock(m_mutex);
			return m_mic_source == Media::MicSource::Auto && PhoneAlive() && (m_client_caps & kCapMic);
		}

		void SetMicWanted(bool w)
		{
			if (m_mic_wanted.exchange(w) != w)
			{
				m_control_dirty = true;
				{
					std::scoped_lock lock(m_mic_mutex);
					if (!w)
						m_mic.Clear();
					m_mic_level_text.clear();
					m_mic_level_start = 0;
				}
				// (outside the mic lock: PhoneMicSelected takes m_mutex, which StatusText holds before it)
				cemuLog_log(LogType::Force, "Remote GamePad: a game {} the GamePad mic{}", w ? "opened" : "closed",
					w ? (PhoneMicSelected() ? " (the phone's microphone)" : " (Cemu's audio input device: the phone isn't the mic source)") : "");
			}
		}

		bool ReadMic(int16_t* dst, size_t count)
		{
			if (!PhoneMicSelected())
				return false;
			std::scoped_lock lock(m_mic_mutex);
			m_mic.Pop(dst, count);
			m_mic_read_samples += count;
			return true;
		}

		// ---------------- settings, status

		void SetCameraSource(Media::CameraSource s) { std::scoped_lock lock(m_mutex); m_camera_source = s; m_control_dirty = true; }
		Media::CameraSource GetCameraSource() const { std::scoped_lock lock(m_mutex); return m_camera_source; }
		void SetMicSource(Media::MicSource s) { std::scoped_lock lock(m_mutex); m_mic_source = s; m_control_dirty = true; }
		Media::MicSource GetMicSource() const { std::scoped_lock lock(m_mutex); return m_mic_source; }

		std::string StatusText()
		{
			std::scoped_lock lock(m_mutex);
			if (!m_udp.IsOpen())
				return "Media: not available";
			std::string s;
			if (!m_client_valid || !m_have_subscriber || now_us() - m_last_feedback_us > kSubscriberTimeoutUs)
				s = fmt::format("Media port {} ({}): waiting for the phone", m_port, m_dual_stack ? "IPv4+IPv6" : "IPv4");
			else if (!m_video_subscribed)
				s = fmt::format("Media port {}: video off on the phone", m_port);
			else
			{
				const auto& p = m_phone;
				s = fmt::format("Video: {:.0f} fps sent, {:.2f} Mbit/s (target {:.1f}); phone shows {:.0f} fps, latency ~{:.0f} ms "
					"(Mac {:.1f}, arrival {:.1f}, decode {:.1f}), dropped {}/s{}",
					m_rate.fps, m_rate.mbps, m_bitrate.kbps() / 1000.0, p.display_fps_x10 / 10.0, p.latency_100us / 10.0,
					m_rate.pipeline_ms, p.assembly_100us / 10.0, p.decode_100us / 10.0,
					p.interval_ms ? (uint32_t)(p.frames_dropped * 1000 / p.interval_ms) : 0,
					m_session ? "" : " (encoder starting)");
				const auto& r = m_rate;
				s += fmt::format("\nGamePad frames from the game: {:.1f}/s, interval {:.1f} ms ±{:.1f} (max {:.0f}); "
					"intervals <12/~17/~25/~33/>40 ms: {}/{}/{}/{}/{}{}; encoder {}",
					r.capture_fps, r.interval_ms, r.interval_sd, r.interval_max, r.hist[0], r.hist[1], r.hist[2], r.hist[3], r.hist[4],
					r.busy ? fmt::format(", skipped (encoder busy) {}", r.busy) : std::string(),
					m_low_latency ? "low-latency" : "real-time");
			}
			const bool cam_phone = m_camera_source == Media::CameraSource::Auto && PhoneAlive() && (m_client_caps & kCapCamera);
			const bool mic_phone = m_mic_source == Media::MicSource::Auto && PhoneAlive() && (m_client_caps & kCapMic);
			std::string cam = cam_phone ? (m_camera_wanted ? fmt::format("phone, {:.0f} fps", m_camera_fps) : std::string("phone (idle)")) : std::string("Mac");
			std::string mic;
			if (mic_phone)
			{
				std::scoped_lock ml(m_mic_mutex);
				mic = m_mic_wanted ? fmt::format("phone, buffer {} ms, underruns {}{}", m_mic.buffered() / 32, m_mic.underruns(), m_mic_level_text) : std::string("phone (idle)");
			}
			else
				mic = "Cemu's input device";
			s += fmt::format("\nCamera: {} | Mic: {}", cam, mic);
			return s;
		}

	private:
		// m_mutex held
		bool PhoneAlive() const { return m_client_valid && m_have_subscriber && now_us() - m_last_feedback_us <= kSubscriberTimeoutUs; }

		void Loop()
		{
			SetThreadName("RemoteGamePadMedia");
			std::vector<uint8_t> buf(2048);
			uint64_t last_control = 0, last_rate = now_us();
			while (m_running)
			{
				// wait up to 20 ms, then drain everything that's there
				Net::UdpAddr from;
				for (size_t n = m_udp.Receive(buf.data(), buf.size(), from, 20); n > 0; n = m_udp.Receive(buf.data(), buf.size(), from, 0))
					Handle(buf.data(), n, from);
				const uint64_t now = now_us();
				// subscription state
				bool want_video = false;
				{
					std::scoped_lock lock(m_mutex);
					want_video = m_client_valid && m_have_subscriber && m_video_subscribed && now - m_last_feedback_us <= kSubscriberTimeoutUs;
					if (m_video_subscribed && !want_video && m_have_subscriber)
					{
						cemuLog_log(LogType::Force, "Remote GamePad: video stopped (no feedback from the phone)");
						m_video_subscribed = false;
					}
				}
				if (want_video)
				{
					m_last_video_use_us = now;
					if (!m_session)
						CreateEncoder();
				}
				else if (m_session && now - m_last_video_use_us > kEncoderIdleUs)
					DestroyEncoder();
				m_video_wanted = want_video && m_session;

				if (now - last_control >= kControlIntervalUs || m_control_dirty.exchange(false))
				{
					last_control = now;
					SendControl(false);
				}
				if (now - last_rate >= 1000000)
				{
					UpdateRates(now - last_rate);
					last_rate = now;
				}
			}
		}

		// out of the socket of the destination's family (the one the phone's packets came in on)
		bool SendTo(const std::vector<uint8_t>& p, const Net::UdpAddr& to) const { return m_udp.Send(to, p.data(), p.size()); }

		void Handle(const uint8_t* data, size_t size, const Net::UdpAddr& from)
		{
			const auto h = ParseHeader(data, size);
			if (!h)
				return;
			switch (h->type)
			{
			case MsgType::VideoFeedback:
				if (const auto f = ParseVideoFeedback(data, size))
					OnFeedback(*f, from);
				break;
			case MsgType::CameraFragment:
				if (const auto f = ParseFragment(data, size); f && FromSubscriber(from))
					OnCameraFragment(*f);
				break;
			case MsgType::MicAudio:
				if (const auto m = ParseMicAudio(data, size); m && FromSubscriber(from))
					OnMic(*m);
				break;
			default:
				break;
			}
		}

		bool FromSubscriber(const Net::UdpAddr& from)
		{
			std::scoped_lock lock(m_mutex);
			return m_have_subscriber && from.SameRemote(m_sub);
		}

		void OnFeedback(const VideoFeedback& f, const Net::UdpAddr& from)
		{
			const uint64_t now = now_us();
			bool bitrate_changed = false;
			{
				std::scoped_lock lock(m_mutex);
				if (!m_client_valid || f.client_id != m_client_id)
					return; // a stale or unknown phone
				const bool video = !(f.flags & kFeedbackNoVideo);
				if (!m_have_subscriber || !from.SameRemote(m_sub) || from.LocalAddress() != m_sub.LocalAddress())
				{
					cemuLog_log(LogType::Force, "Remote GamePad: media from \"{}\" at {} (to {}, video {})", m_client_name, from.Address(), from.LocalAddress(), video ? "on" : "off");
					m_have_subscriber = true;
					m_control_dirty = true;
				}
				m_sub = from; // the newest local address too
				if (video && !m_video_subscribed)
				{
					m_bitrate.Reset();
					m_force_keyframe = true;
				}
				m_video_subscribed = video;
				m_last_feedback_us = now;
				m_bitrate.SetMax(f.max_bitrate_kbps);
				if (video && f.interval_ms)
				{
					m_phone = f;
					bitrate_changed = m_bitrate.OnFeedback(f, now);
				}
				if (video && (f.flags & kFeedbackKeyframeRequest) && now - m_last_key_request_us > kKeyframeMinIntervalUs)
				{
					m_last_key_request_us = now;
					m_force_keyframe = true;
				}
			}
			if (bitrate_changed)
				ApplyBitrate();
		}

		// ---------------- encoder

		void CreateEncoder()
		{
			std::scoped_lock lock(m_encoder_mutex);
			if (m_session)
				return;
			CFMutableDictionaryRef spec = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			set_bool(spec, kVTVideoEncoderSpecification_EnableLowLatencyRateControl, true);
			CFMutableDictionaryRef src = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			set_int(src, kCVPixelBufferPixelFormatTypeKey, kCVPixelFormatType_32BGRA);
			set_int(src, kCVPixelBufferWidthKey, kVideoWidth);
			set_int(src, kCVPixelBufferHeightKey, kVideoHeight);
			set_bool(src, kCVPixelBufferMetalCompatibilityKey, true);
			CFDictionaryRef empty = CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			CFDictionarySetValue(src, kCVPixelBufferIOSurfacePropertiesKey, empty);
			CFRelease(empty);

			VTCompressionSessionRef session = nullptr;
			OSStatus st = VTCompressionSessionCreate(nullptr, kVideoWidth, kVideoHeight, kCMVideoCodecType_H264, spec, src, nullptr, &MediaHub::EncodedCallback, this, &session);
			m_low_latency = st == noErr;
			if (st != noErr)
			{
				// no low-latency encoder: a plain real-time one with a short GOP instead
				st = VTCompressionSessionCreate(nullptr, kVideoWidth, kVideoHeight, kCMVideoCodecType_H264, nullptr, src, nullptr, &MediaHub::EncodedCallback, this, &session);
			}
			CFRelease(spec);
			if (st != noErr || !session)
			{
				CFRelease(src);
				if (!m_logged_encoder_error)
					cemuLog_log(LogType::Force, "Remote GamePad: can't create the H.264 encoder ({})", (int)st);
				m_logged_encoder_error = true;
				return;
			}
			VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
			VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
			set_prop_int(session, kVTCompressionPropertyKey_ExpectedFrameRate, kVideoFps);
			set_prop_int(session, kVTCompressionPropertyKey_AverageBitRate, (int32_t)m_bitrate.kbps() * 1000);
			VTSessionSetProperty(session, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue);
			set_prop_int(session, kVTCompressionPropertyKey_MaxFrameDelayCount, 0);
			if (!m_low_latency)
			{
				// keyframes on request like the low-latency encoder; a long safety interval only, since big
				// periodic keyframes make a regular hitch
				set_prop_int(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, kVideoFps * 10);
				set_prop_double(session, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, 10.0);
				VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_Main_AutoLevel);
			}
			// several slices per frame (each about a datagram); not every encoder supports it
			m_slices_supported = set_prop_int(session, kVTCompressionPropertyKey_MaxH264SliceBytes, (int32_t)kMaxFragmentData - 100) == noErr;
			VTCompressionSessionPrepareToEncodeFrames(session);

			CFMutableDictionaryRef pool_attr = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			set_int(pool_attr, kCVPixelBufferPoolMinimumBufferCountKey, kPoolBuffers);
			CVPixelBufferPoolRef pool = nullptr;
			CVPixelBufferPoolCreate(nullptr, pool_attr, src, &pool);
			CFRelease(pool_attr);
			CFRelease(src);
			if (!pool)
			{
				VTCompressionSessionInvalidate(session);
				CFRelease(session);
				cemuLog_log(LogType::Force, "Remote GamePad: can't create the video buffer pool");
				return;
			}
			m_session = session;
			m_pool = pool;
			m_force_keyframe = true;
			cemuLog_log(LogType::Force, "Remote GamePad: H.264 encoder {}x{} @ {} fps, {} rate control, slices {}, {:.1f} Mbit/s",
				kVideoWidth, kVideoHeight, kVideoFps, m_low_latency ? "low-latency" : "real-time (keyframes on request, safety every 10 s)",
				m_slices_supported ? "~1 per datagram" : "encoder default", m_bitrate.kbps() / 1000.0);
		}

		void DestroyEncoder()
		{
			VTCompressionSessionRef session = nullptr;
			CVPixelBufferPoolRef pool = nullptr;
			{
				std::scoped_lock lock(m_encoder_mutex);
				m_video_wanted = false;
				session = m_session;
				pool = m_pool;
				m_session = nullptr;
				m_pool = nullptr;
			}
			if (session)
			{
				VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
				VTCompressionSessionInvalidate(session);
				CFRelease(session);
				cemuLog_log(LogType::Force, "Remote GamePad: video encoder stopped");
			}
			if (pool)
				CVPixelBufferPoolRelease(pool);
		}

		void ApplyBitrate()
		{
			std::scoped_lock lock(m_encoder_mutex);
			if (!m_session)
				return;
			const uint32_t kbps = m_bitrate.kbps();
			set_prop_int(m_session, kVTCompressionPropertyKey_AverageBitRate, (int32_t)kbps * 1000);
			// hard cap: 1.5x the average over any second
			int64_t bytes = (int64_t)kbps * 1000 / 8 * 3 / 2;
			double secs = 1.0;
			CFNumberRef b = CFNumberCreate(nullptr, kCFNumberSInt64Type, &bytes);
			CFNumberRef d = CFNumberCreate(nullptr, kCFNumberDoubleType, &secs);
			const void* vals[] = {b, d};
			CFArrayRef limits = CFArrayCreate(nullptr, vals, 2, &kCFTypeArrayCallBacks);
			VTSessionSetProperty(m_session, kVTCompressionPropertyKey_DataRateLimits, limits);
			CFRelease(limits);
			CFRelease(b);
			CFRelease(d);
		}

		static void EncodedCallback(void* refcon, void*, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sb)
		{
			auto* self = (MediaHub*)refcon;
			if (status != noErr || !sb || (flags & kVTEncodeInfo_FrameDropped))
			{
				self->m_stat_encode_errors++;
				return;
			}
			self->OnEncoded(sb);
		}

		void OnEncoded(CMSampleBufferRef sb)
		{
			bool keyframe = true;
			if (CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false); att && CFArrayGetCount(att) > 0)
			{
				auto d = (CFDictionaryRef)CFArrayGetValueAtIndex(att, 0);
				CFBooleanRef not_sync = nullptr;
				if (CFDictionaryGetValueIfPresent(d, kCMSampleAttachmentKey_NotSync, (const void**)&not_sync) && not_sync == kCFBooleanTrue)
					keyframe = false;
			}
			std::vector<uint8_t>& frame = m_frame_buf; // only the encoder's callback thread uses it
			frame.clear();
			int nal_len_size = 4;
			if (keyframe)
			{
				CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sb);
				size_t count = 0;
				if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, nullptr, nullptr, &count, &nal_len_size) == noErr)
				{
					for (size_t i = 0; i < count; ++i)
					{
						const uint8_t* ps = nullptr;
						size_t ps_size = 0;
						if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &ps, &ps_size, nullptr, nullptr) != noErr)
							continue;
						const uint32_t n = (uint32_t)ps_size;
						const uint8_t len[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
						frame.insert(frame.end(), len, len + 4);
						frame.insert(frame.end(), ps, ps + ps_size);
					}
				}
			}
			CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
			if (!bb)
				return;
			const size_t size = CMBlockBufferGetDataLength(bb);
			const size_t base = frame.size();
			frame.resize(base + size);
			if (CMBlockBufferCopyDataBytes(bb, 0, size, frame.data() + base) != kCMBlockBufferNoErr)
				return;
			if (nal_len_size != 4)
			{
				// re-prefix with 4-byte lengths (VideoToolbox uses 4 in practice)
				std::vector<uint8_t> out(frame.begin(), frame.begin() + base);
				size_t off = base;
				while (off + nal_len_size <= frame.size())
				{
					size_t n = 0;
					for (int i = 0; i < nal_len_size; ++i)
						n = (n << 8) | frame[off + i];
					off += nal_len_size;
					if (off + n > frame.size())
						break;
					const uint8_t len[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
					out.insert(out.end(), len, len + 4);
					out.insert(out.end(), frame.begin() + off, frame.begin() + off + n);
					off += n;
				}
				frame.swap(out);
			}

			const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
			const uint64_t capture_us = (uint64_t)CMTimeConvertScale(pts, 1000000, kCMTimeRoundingMethod_Default).value;
			Net::UdpAddr to;
			{
				std::scoped_lock lock(m_mutex);
				if (!m_have_subscriber || !m_video_subscribed)
					return;
				to = m_sub;
			}
			MediaFragment t;
			t.frame_id = m_frame_id++;
			t.codec = kCodecH264;
			t.flags = (keyframe ? kFragKeyframe : 0) | (m_low_latency ? kFragLowLatencyEncoder : 0);
			t.timestamp_us = capture_us;
			t.width = kVideoWidth;
			t.height = kVideoHeight;
			const uint64_t send_us = now_us();
			t.pipeline_100us = (uint16_t)std::min<uint64_t>((send_us - capture_us) / 100, 0xFFFF);
			t.bitrate_10kbps = (uint16_t)std::min<uint32_t>(m_bitrate.kbps() / 10, 0xFFFF);
			const auto packets = Packetize(MsgType::VideoFragment, m_video_seq, t, frame.data(), frame.size());
			for (const auto& p : packets)
			{
				if (!SendTo(p, to))
					m_stat_send_errors++;
			}
			std::scoped_lock lock(m_stats_mutex);
			m_stat_frames++;
			m_stat_bytes += frame.size();
			if (keyframe)
				m_stat_keyframes++;
			m_stat_pipeline.Add((send_us - capture_us) / 1000.0);
		}

		void UpdateRates(uint64_t elapsed_us)
		{
			std::scoped_lock lock(m_stats_mutex);
			const double secs = elapsed_us / 1e6;
			Rates r;
			r.fps = m_stat_frames / secs;
			r.mbps = m_stat_bytes * 8 / secs / 1e6;
			r.pipeline_ms = m_stat_pipeline.Mean();
			r.capture_fps = m_stat_captures / secs;
			r.interval_ms = m_stat_interval.Mean();
			r.interval_sd = m_stat_interval.n ? std::sqrt(std::max(0.0, m_stat_interval_sq / m_stat_interval.n - r.interval_ms * r.interval_ms)) : 0;
			r.interval_max = m_stat_interval_max;
			r.hist = m_stat_interval_hist;
			r.busy = m_stat_capture_busy.exchange(0);
			m_stat_captures = 0;
			m_stat_interval = {};
			m_stat_interval_sq = 0;
			m_stat_interval_max = 0;
			m_stat_interval_hist = {};
			m_stat_frames = 0;
			m_stat_bytes = 0;
			m_stat_pipeline = {};
			std::scoped_lock l2(m_mutex);
			m_rate = r;
			m_camera_fps = m_camera_frames / secs;
			m_camera_frames = 0;
		}

		// ---------------- control, camera, mic

		void SendControl(bool camera_keyframe)
		{
			Net::UdpAddr to;
			MediaControl c;
			{
				std::scoped_lock lock(m_mutex);
				if (!PhoneAlive())
					return;
				to = m_sub;
				if (m_camera_wanted && m_camera_source == Media::CameraSource::Auto && (m_client_caps & kCapCamera))
					c.flags |= kControlCamera;
				if (m_mic_wanted && m_mic_source == Media::MicSource::Auto && (m_client_caps & kCapMic))
					c.flags |= kControlMic;
			}
			if ((c.flags & kControlCamera) && (camera_keyframe || m_camera_assembler.NeedKeyframe()))
			{
				const uint64_t now = now_us();
				if (now - m_last_camera_key_request_us > kKeyframeMinIntervalUs)
				{
					m_last_camera_key_request_us = now;
					c.flags |= kControlCameraKeyframe;
				}
			}
			const auto p = BuildMediaControl(m_control_seq++, c);
			SendTo(p, to);
		}

		void OnCameraFragment(const MediaFragment& f)
		{
			if (!m_camera_wanted)
				return;
			auto frame = m_camera_assembler.Add(f, now_us());
			if (m_camera_assembler.NeedKeyframe() && now_us() - m_last_camera_key_request_us > kKeyframeMinIntervalUs)
				SendControl(true);
			if (!frame || frame->codec != kCodecH264)
				return;
			DecodeCamera(*frame);
		}

		static void DecodedCallback(void* refcon, void*, OSStatus status, VTDecodeInfoFlags, CVImageBufferRef image, CMTime, CMTime)
		{
			auto* self = (MediaHub*)refcon;
			if (status != noErr || !image)
			{
				self->m_camera_decode_failed = true;
				return;
			}
			std::scoped_lock lock(self->m_camera_mutex);
			if (self->m_camera_latest)
				CVPixelBufferRelease(self->m_camera_latest);
			self->m_camera_latest = CVPixelBufferRetain(image);
			self->m_camera_latest_us = now_us();
		}

		void DecodeCamera(const FrameAssembler::Frame& frame)
		{
			// split off SPS/PPS; the rest is the sample
			std::vector<const uint8_t*> ps;
			std::vector<size_t> ps_sizes;
			std::vector<uint8_t> sample;
			sample.reserve(frame.data.size());
			size_t off = 0;
			const auto& d = frame.data;
			while (off + 4 <= d.size())
			{
				const size_t n = ((size_t)d[off] << 24) | ((size_t)d[off + 1] << 16) | ((size_t)d[off + 2] << 8) | d[off + 3];
				if (n == 0 || off + 4 + n > d.size())
					break;
				const uint8_t type = d[off + 4] & 0x1F;
				if (type == 7 || type == 8)
				{
					ps.push_back(&d[off + 4]);
					ps_sizes.push_back(n);
				}
				else
					sample.insert(sample.end(), d.begin() + off, d.begin() + off + 4 + n);
				off += 4 + n;
			}
			if (ps.size() >= 2)
			{
				CMVideoFormatDescriptionRef fmt = nullptr;
				if (CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr, ps.size(), ps.data(), ps_sizes.data(), 4, &fmt) == noErr)
				{
					if (!m_camera_session || !m_camera_format || !CMFormatDescriptionEqual(fmt, m_camera_format))
					{
						DestroyCameraDecoder();
						m_camera_format = fmt;
						fmt = nullptr;
						CFMutableDictionaryRef dst = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
						set_int(dst, kCVPixelBufferPixelFormatTypeKey, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
						VTDecompressionOutputCallbackRecord cb{&MediaHub::DecodedCallback, this};
						VTDecompressionSessionRef s = nullptr;
						const OSStatus st = VTDecompressionSessionCreate(nullptr, m_camera_format, nullptr, dst, &cb, &s);
						CFRelease(dst);
						if (st == noErr)
						{
							VTSessionSetProperty(s, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
							m_camera_session = s;
							const auto dims = CMVideoFormatDescriptionGetDimensions(m_camera_format);
							cemuLog_log(LogType::Force, "Remote GamePad: phone camera stream {}x{}", dims.width, dims.height);
						}
						else
							cemuLog_log(LogType::Force, "Remote GamePad: can't create the camera decoder ({})", (int)st);
					}
					if (fmt)
						CFRelease(fmt);
				}
			}
			if (!m_camera_session || sample.empty())
				return;
			CMBlockBufferRef bb = nullptr;
			if (CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, sample.size(), nullptr, nullptr, 0, sample.size(), 0, &bb) != kCMBlockBufferNoErr)
				return;
			CMBlockBufferReplaceDataBytes(sample.data(), bb, 0, sample.size());
			CMSampleBufferRef sb = nullptr;
			const size_t sample_size = sample.size();
			if (CMSampleBufferCreateReady(nullptr, bb, m_camera_format, 1, 0, nullptr, 1, &sample_size, &sb) == noErr)
			{
				m_camera_decode_failed = false;
				VTDecompressionSessionDecodeFrame(m_camera_session, sb, 0, nullptr, nullptr);
				CFRelease(sb);
				if (m_camera_decode_failed)
					SendControl(true);
				else
				{
					std::scoped_lock lock(m_mutex);
					m_camera_frames++;
				}
			}
			CFRelease(bb);
		}

		void DestroyCameraDecoder()
		{
			if (m_camera_session)
			{
				VTDecompressionSessionInvalidate(m_camera_session);
				CFRelease(m_camera_session);
				m_camera_session = nullptr;
			}
			if (m_camera_format)
			{
				CFRelease(m_camera_format);
				m_camera_format = nullptr;
			}
		}

		void OnMic(const MicAudio& m)
		{
			if (!m_mic_wanted || m.sample_rate != 32000 || m.sample_count == 0)
				return;
			int16_t samples[kMaxMicSamples];
			for (size_t i = 0; i < m.sample_count; ++i)
				samples[i] = (int16_t)(m.samples[2 * i] | (m.samples[2 * i + 1] << 8));
			std::scoped_lock lock(m_mic_mutex);
			m_mic.Push(samples, m.sample_count);
			// level meter over each second: what arrives from the phone
			const uint64_t now = now_us();
			for (size_t i = 0; i < m.sample_count; ++i)
			{
				const double x = samples[i];
				m_mic_sumsq += x * x;
				m_mic_peak = std::max(m_mic_peak, std::abs((int)samples[i]));
			}
			m_mic_count += m.sample_count;
			if (m_mic_level_start == 0)
				m_mic_level_start = now;
			if (now - m_mic_level_start >= 1000000)
			{
				auto db = [](double a) { return a > 0 ? 20 * std::log10(a / 32768.0) : -96.0; };
				m_mic_level_text = fmt::format(", level {:.0f} dBFS, peak {:.0f} dBFS, to the game {}/s",
					db(std::sqrt(m_mic_sumsq / std::max<uint64_t>(m_mic_count, 1))), db(m_mic_peak),
					m_mic_read_samples * 1000000 / (now - m_mic_level_start));
				m_mic_sumsq = 0;
				m_mic_peak = 0;
				m_mic_count = 0;
				m_mic_read_samples = 0;
				m_mic_level_start = now;
			}
		}

		double m_mic_sumsq = 0;
		int m_mic_peak = 0;
		uint64_t m_mic_count = 0, m_mic_level_start = 0, m_mic_read_samples = 0;
		std::string m_mic_level_text;

		struct Rates
		{
			double fps = 0, mbps = 0, pipeline_ms = 0;
			double capture_fps = 0, interval_ms = 0, interval_sd = 0, interval_max = 0;
			std::array<uint32_t, 5> hist{}; // capture intervals <12, 12-20, 20-28, 28-40, >=40 ms
			uint32_t busy = 0;              // frames skipped: encoder behind (no free buffer)
		};

		std::mutex m_start_mutex;
		std::atomic_bool m_running = false;
		std::thread m_thread;
		Net::UdpDual m_udp;
		uint16_t m_port = 0;
		bool m_dual_stack = false;

		mutable std::recursive_mutex m_mutex; // client, subscriber, settings, phone stats
		bool m_client_valid = false;
		uint32_t m_client_id = 0;
		uint16_t m_client_caps = 0;
		std::string m_client_name;
		bool m_have_subscriber = false;
		Net::UdpAddr m_sub{};
		bool m_video_subscribed = false;
		uint64_t m_last_feedback_us = 0;
		uint64_t m_last_key_request_us = 0;
		VideoFeedback m_phone{};
		BitrateController m_bitrate;
		Rates m_rate;
		double m_camera_fps = 0;
		uint32_t m_camera_frames = 0;
		Media::CameraSource m_camera_source = Media::CameraSource::Auto;
		Media::MicSource m_mic_source = Media::MicSource::Auto;
		std::atomic_bool m_control_dirty = false;
		std::atomic_bool m_camera_wanted = false;
		std::atomic_bool m_mic_wanted = false;

		// encoder
		std::mutex m_encoder_mutex;
		VTCompressionSessionRef m_session = nullptr;
		CVPixelBufferPoolRef m_pool = nullptr;
		bool m_low_latency = false, m_slices_supported = false, m_logged_encoder_error = false;
		std::atomic_bool m_video_wanted = false;
		std::atomic_bool m_force_keyframe = false;
		uint64_t m_last_video_use_us = 0;
		uint32_t m_frame_id = 1;
		uint32_t m_video_seq = 0;
		uint32_t m_control_seq = 0;
		std::vector<uint8_t> m_frame_buf;
		std::mutex m_stats_mutex;
		uint32_t m_stat_frames = 0;
		uint64_t m_stat_bytes = 0;
		uint32_t m_stat_keyframes = 0;
		Avg m_stat_pipeline;
		uint64_t m_last_capture_us = 0;
		uint32_t m_stat_captures = 0;
		Avg m_stat_interval;
		double m_stat_interval_sq = 0, m_stat_interval_max = 0;
		std::array<uint32_t, 5> m_stat_interval_hist{};
		std::atomic_uint32_t m_stat_capture_busy = 0, m_stat_encode_errors = 0, m_stat_send_errors = 0, m_stat_keyframes_forced = 0;

		// camera (receive thread, except m_camera_latest)
		FrameAssembler m_camera_assembler;
		VTDecompressionSessionRef m_camera_session = nullptr;
		CMVideoFormatDescriptionRef m_camera_format = nullptr;
		bool m_camera_decode_failed = false;
		uint64_t m_last_camera_key_request_us = 0;
		std::mutex m_camera_mutex;
		CVPixelBufferRef m_camera_latest = nullptr;
		uint64_t m_camera_latest_us = 0;

		// microphone
		mutable std::mutex m_mic_mutex;
		MicJitterBuffer m_mic{32000};
	};

	MediaHub& Hub()
	{
		static MediaHub* hub = new MediaHub(); // never destroyed: renderer callbacks may outlive static destruction
		return *hub;
	}
}

namespace RemoteGamePad::Media
{
	bool IsSupported() { return true; }
	bool Start(uint16_t port, std::string& error) { return Hub().Start(port, error); }
	uint16_t Port() { return Hub().Port(); }
	void SetClient(uint32_t id, uint16_t caps, const std::string& name) { Hub().SetClient(id, caps, name); }
	void ClearClient() { Hub().ClearClient(); }
	std::string StatusText() { return Hub().StatusText(); }
	void SetCameraSource(CameraSource s) { Hub().SetCameraSource(s); }
	CameraSource GetCameraSource() { return Hub().GetCameraSource(); }
	void SetMicSource(MicSource s) { Hub().SetMicSource(s); }
	MicSource GetMicSource() { return Hub().GetMicSource(); }

	bool VideoWanted() { return Hub().VideoWanted(); }
	void GetVideoSize(int& w, int& h) { w = kVideoWidth; h = kVideoHeight; }
	void* AcquireVideoBuffer() { return Hub().AcquireVideoBuffer(); }
	void SubmitVideoBuffer(void* pb, uint64_t capture_us) { Hub().SubmitVideoBuffer(pb, capture_us); }
	void ReleaseVideoBuffer(void* pb) { if (pb) CVPixelBufferRelease((CVPixelBufferRef)pb); }
	void* GetIOSurface(void* pb) { return MediaHub::GetIOSurface(pb); }
	uint64_t NowUs() { return now_us(); }

	bool PhoneCameraSelected() { return Hub().PhoneCameraSelected(); }
	void SetCameraWanted(bool w) { Hub().SetCameraWanted(w); }
	bool GetCameraFrameNV12(uint8_t* dst, uint32_t w, uint32_t h, uint32_t pitch) { return Hub().GetCameraFrameNV12(dst, w, h, pitch); }

	bool PhoneMicSelected() { return Hub().PhoneMicSelected(); }
	void SetMicWanted(bool w) { Hub().SetMicWanted(w); }
	bool ReadMic(int16_t* dst, size_t count) { return Hub().ReadMic(dst, count); }
}
