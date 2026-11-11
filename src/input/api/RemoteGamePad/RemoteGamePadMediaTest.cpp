// Standalone test of the Remote GamePad media path on macOS (not part of the CMake build), with a
// synthetic GamePad image instead of the renderer:
//
//   selftest  in one process: the media hub (RemoteGamePadMediaApple.cpp) encodes a moving test
//             pattern; a fake phone does the handshake, sends feedback, reassembles and decodes the
//             video with VideoToolbox and checks the pixels, frame rate, latency, loss recovery
//             (a dropped fragment -> keyframe request -> recovery) and bitrate adaptation; then the
//             camera (fake phone encodes -> hub decodes to NV12) and microphone paths.
//   serve [port] [seconds]
//             a minimal "Cemu": answers HELLO on port (default 26790) with the media port (port+1)
//             and streams the test pattern, for tools/send_test.py video in the app repository.
//
// Build and run from the repository root:
//   clang++ -std=c++20 -O1 -Wall -I src -I build/vcpkg_installed/arm64-osx/include -DFMT_HEADER_ONLY \
//     src/input/api/RemoteGamePad/RemoteGamePadMediaTest.cpp src/input/api/RemoteGamePad/RemoteGamePadMediaApple.cpp \
//     src/input/api/RemoteGamePad/RemoteGamePadMediaCore.cpp src/input/api/RemoteGamePad/RemoteGamePadProtocol.cpp \
//     src/input/api/RemoteGamePad/RemoteGamePadUdp.cpp \
//     -include src/input/api/RemoteGamePad/RemoteGamePadMediaTestShim.h \
//     -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework CoreFoundation -framework IOSurface \
//     -o /tmp/rgmedia && /tmp/rgmedia selftest

#include "input/api/RemoteGamePad/RemoteGamePadMedia.h"
#include "input/api/RemoteGamePad/RemoteGamePadMediaCore.h"

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace RemoteGamePad;
using namespace std::chrono_literals;

void SetThreadName(const char*) {}

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

namespace
{
	uint64_t now_us() { return Media::NowUs(); }

	// the test pattern: a colour per quadrant that changes with the frame number, plus a moving bar
	void pattern_color(uint32_t frame, int quadrant, uint8_t& r, uint8_t& g, uint8_t& b)
	{
		const uint32_t k = frame / 30 + quadrant; // changes every half second
		r = (k & 1) ? 220 : 30;
		g = (k & 2) ? 220 : 30;
		b = (k & 4) ? 220 : 30;
	}

	void fill_pattern(void* pb_, uint32_t frame)
	{
		auto pb = (CVPixelBufferRef)pb_;
		CVPixelBufferLockBaseAddress(pb, 0);
		auto* base = (uint8_t*)CVPixelBufferGetBaseAddress(pb);
		const size_t pitch = CVPixelBufferGetBytesPerRow(pb);
		const int w = (int)CVPixelBufferGetWidth(pb), h = (int)CVPixelBufferGetHeight(pb);
		const int bar = (int)(frame * 8 % w);
		for (int y = 0; y < h; ++y)
		{
			uint8_t* row = base + y * pitch;
			for (int x = 0; x < w; ++x)
			{
				uint8_t r, g, b;
				pattern_color(frame, (y >= h / 2) * 2 + (x >= w / 2), r, g, b);
				if (x >= bar && x < bar + 16)
					r = g = b = 255;
				if (y < 64) // a band of noise that changes every frame: frames span several datagrams
				{
					uint32_t hsh = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ frame * 83492791u;
					hsh ^= hsh >> 13;
					hsh *= 0x5bd1e995u;
					r = g = b = (uint8_t)(hsh >> 24);
				}
				row[4 * x + 0] = b;
				row[4 * x + 1] = g;
				row[4 * x + 2] = r;
				row[4 * x + 3] = 255;
			}
		}
		CVPixelBufferUnlockBaseAddress(pb, 0);
	}

	// the synthetic renderer: 60 fps while a phone wants video
	struct Source
	{
		std::atomic_bool run{true};
		std::atomic_uint32_t frames{0};
		std::thread thread;
		void Start()
		{
			thread = std::thread([this] {
				auto next = std::chrono::steady_clock::now();
				uint32_t n = 0;
				while (run)
				{
					next += std::chrono::microseconds(16667);
					std::this_thread::sleep_until(next);
					if (!Media::VideoWanted())
						continue;
					void* pb = Media::AcquireVideoBuffer();
					if (!pb)
						continue;
					fill_pattern(pb, n++);
					const uint64_t t = now_us(); // the renderer takes this when the GPU copy is queued
					Media::SubmitVideoBuffer(pb, t);
					frames++;
				}
			});
		}
		void Stop()
		{
			run = false;
			if (thread.joinable())
				thread.join();
		}
	};

	int udp_socket_v6(uint16_t port)
	{
		int fd = socket(AF_INET6, SOCK_DGRAM, 0);
		int off = 0;
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
		sockaddr_in6 a{};
		a.sin6_len = sizeof(a);
		a.sin6_family = AF_INET6;
		a.sin6_port = htons(port);
		a.sin6_addr = in6addr_any;
		if (bind(fd, (sockaddr*)&a, sizeof(a)) != 0)
		{
			perror("bind");
			exit(2);
		}
		return fd;
	}

	// the "Cemu" control port: HELLO -> HELLO_ACK with the media port, and Media::SetClient
	struct ControlServer
	{
		int fd = -1;
		std::atomic_bool run{true};
		std::thread thread;
		void Start(uint16_t port)
		{
			fd = udp_socket_v6(port);
			thread = std::thread([this] {
				uint8_t buf[2048];
				uint32_t seq = 0;
				while (run)
				{
					pollfd p{fd, POLLIN, 0};
					if (poll(&p, 1, 50) <= 0)
						continue;
					sockaddr_storage from{};
					socklen_t len = sizeof(from);
					const ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (sockaddr*)&from, &len);
					if (n <= 0)
						continue;
					const auto hello = ParseHello(buf, (size_t)n);
					if (!hello)
						continue;
					Media::SetClient(hello->client_id, hello->capabilities, hello->device_name);
					HelloAck ack;
					ack.client_id = hello->client_id;
					ack.capabilities = kCapMotion | kCapTouch | kCapVideo | kCapCamera | kCapMic;
					ack.video_port = Media::Port();
					ack.host_name = "RemoteGamePadMediaTest";
					const auto pkt = BuildHelloAck(seq++, ack);
					sendto(fd, pkt.data(), pkt.size(), 0, (sockaddr*)&from, len);
				}
			});
		}
		void Stop()
		{
			run = false;
			if (thread.joinable())
				thread.join();
			close(fd);
		}
	};

	// ---------------- the fake phone (selftest)

	struct Phone
	{
		int fd = -1;
		sockaddr_in6 control{}, media{};
		uint32_t client_id = 0x51A7E57;
		uint32_t seq = 0;
		FrameAssembler assembler;
		VTDecompressionSessionRef decoder = nullptr;
		CMVideoFormatDescriptionRef format = nullptr;
		std::atomic_uint32_t decoded{0};
		uint32_t decode_errors = 0;
		std::vector<double> latencies_ms;
		uint64_t pending_capture_us = 0;
		CVPixelBufferRef last_image = nullptr;
		uint32_t last_frame_id = 0;
		uint32_t keyframes = 0, keyframe_requests = 0;
		uint32_t drop_fragment_of_frame = 0; // simulate loss: drop fragment 1 of this frame id
		MediaControl last_control{};
		uint32_t controls = 0;
		std::vector<uint16_t> bitrates;

		Phone(uint16_t port)
		{
			fd = socket(AF_INET6, SOCK_DGRAM, 0);
			int off = 0;
			setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
			sockaddr_in6 a{};
			a.sin6_len = sizeof(a);
			a.sin6_family = AF_INET6;
			a.sin6_addr = in6addr_any;
			bind(fd, (sockaddr*)&a, sizeof(a));
			int rcv = 4 << 20;
			setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
			control.sin6_len = sizeof(control);
			control.sin6_family = AF_INET6;
			control.sin6_port = htons(port);
			control.sin6_addr = in6addr_loopback; // IPv6 on purpose: the dual-stack path
		}

		void send(const std::vector<uint8_t>& p, const sockaddr_in6& to) { sendto(fd, p.data(), p.size(), 0, (const sockaddr*)&to, sizeof(to)); }

		bool Handshake()
		{
			Hello h;
			h.client_id = client_id;
			h.capabilities = kCapMotion | kCapTouch | kCapVideo | kCapCamera | kCapMic;
			h.minor_version = kMinorVersion;
			h.device_name = "selftest phone";
			send(BuildHello(seq++, h), control);
			uint8_t buf[2048];
			pollfd p{fd, POLLIN, 0};
			if (poll(&p, 1, 1000) <= 0)
				return false;
			const ssize_t n = recv(fd, buf, sizeof(buf), 0);
			const auto h2 = ParseHeader(buf, (size_t)n);
			if (!h2 || h2->type != MsgType::HelloAck)
				return false;
			const uint16_t video_port = buf[18] | (buf[19] << 8);
			media = control;
			media.sin6_port = htons(video_port);
			return video_port != 0;
		}

		void Feedback(bool keyframe_request, bool video = true)
		{
			const auto st = assembler.TakeStats();
			VideoFeedback f;
			f.client_id = client_id;
			f.newest_frame = last_frame_id;
			f.frames_shown = (uint16_t)st.frames_complete;
			f.frames_dropped = (uint16_t)st.frames_dropped;
			f.fragments_received = (uint16_t)st.fragments;
			f.fragments_missing = (uint16_t)st.fragments_missing;
			f.interval_ms = 100;
			f.flags = (keyframe_request ? kFeedbackKeyframeRequest : 0) | (video ? 0 : kFeedbackNoVideo);
			f.display_fps_x10 = 600;
			send(BuildVideoFeedback(seq++, f), media);
			if (keyframe_request)
				keyframe_requests++;
		}

		static void Decoded(void* refcon, void*, OSStatus status, VTDecodeInfoFlags, CVImageBufferRef image, CMTime pts, CMTime)
		{
			auto* self = (Phone*)refcon;
			if (status != noErr || !image)
			{
				self->decode_errors++;
				return;
			}
			if (self->last_image)
				CVPixelBufferRelease(self->last_image);
			self->last_image = CVPixelBufferRetain(image);
			self->decoded++;
			self->latencies_ms.push_back((now_us() - (uint64_t)pts.value) / 1000.0);
		}

		void Decode(const FrameAssembler::Frame& fr)
		{
			std::vector<const uint8_t*> ps;
			std::vector<size_t> ps_sizes;
			std::vector<uint8_t> sample;
			size_t off = 0;
			const auto& d = fr.data;
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
			if (ps.size() >= 2 && !decoder)
			{
				CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr, ps.size(), ps.data(), ps_sizes.data(), 4, &format);
				const void* k[] = {kCVPixelBufferPixelFormatTypeKey};
				int32_t fmt = kCVPixelFormatType_32BGRA;
				CFNumberRef v0 = CFNumberCreate(nullptr, kCFNumberSInt32Type, &fmt);
				const void* v[] = {v0};
				CFDictionaryRef attrs = CFDictionaryCreate(nullptr, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
				VTDecompressionOutputCallbackRecord cb{&Phone::Decoded, this};
				VTDecompressionSessionCreate(nullptr, format, nullptr, attrs, &cb, &decoder);
				CFRelease(attrs);
				CFRelease(v0);
			}
			if (!decoder || sample.empty())
				return;
			CMBlockBufferRef bb = nullptr;
			CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, sample.size(), nullptr, nullptr, 0, sample.size(), 0, &bb);
			CMBlockBufferReplaceDataBytes(sample.data(), bb, 0, sample.size());
			CMSampleBufferRef sb = nullptr;
			const size_t sz = sample.size();
			CMSampleTimingInfo timing{kCMTimeInvalid, CMTimeMake((int64_t)fr.timestamp_us, 1000000), kCMTimeInvalid};
			CMSampleBufferCreateReady(nullptr, bb, format, 1, 1, &timing, 1, &sz, &sb);
			VTDecompressionSessionDecodeFrame(decoder, sb, 0, nullptr, nullptr);
			CFRelease(sb);
			CFRelease(bb);
		}

		// receive for `ms`, feedback every 100 ms
		void Run(int ms)
		{
			const uint64_t end = now_us() + (uint64_t)ms * 1000;
			uint64_t next_fb = 0;
			uint8_t buf[2048];
			while (now_us() < end)
			{
				if (now_us() >= next_fb)
				{
					Feedback(assembler.NeedKeyframe());
					next_fb = now_us() + 100000;
				}
				pollfd p{fd, POLLIN, 0};
				if (poll(&p, 1, 5) <= 0)
					continue;
				const ssize_t n = recv(fd, buf, sizeof(buf), 0);
				const auto h = ParseHeader(buf, (size_t)n);
				if (!h)
					continue;
				if (h->type == MsgType::MediaControl)
				{
					if (auto c = ParseMediaControl(buf, (size_t)n))
					{
						last_control = *c;
						controls++;
					}
					continue;
				}
				const auto f = ParseFragment(buf, (size_t)n);
				if (!f || h->type != MsgType::VideoFragment)
					continue;
				if (f->frame_id == drop_fragment_of_frame && f->index == 0)
					continue; // simulated loss
				if (f->index == 0)
					bitrates.push_back(f->bitrate_10kbps);
				const bool was_needed = assembler.NeedKeyframe();
				auto fr = assembler.Add(*f, now_us());
				if (was_needed && !assembler.NeedKeyframe())
					Feedback(false); // recovered
				else if (!was_needed && assembler.NeedKeyframe())
					Feedback(true); // ask right away
				if (fr)
				{
					if (fr->keyframe())
						keyframes++;
					last_frame_id = fr->id;
					Decode(*fr);
				}
			}
		}

		// check the newest decoded image against the pattern of its frame (quadrant colours)
		bool CheckImage()
		{
			if (!last_image)
				return false;
			CVPixelBufferLockBaseAddress(last_image, kCVPixelBufferLock_ReadOnly);
			auto* base = (const uint8_t*)CVPixelBufferGetBaseAddress(last_image);
			const size_t pitch = CVPixelBufferGetBytesPerRow(last_image);
			const int w = (int)CVPixelBufferGetWidth(last_image), h = (int)CVPixelBufferGetHeight(last_image);
			// each quadrant should be a flat primary-ish colour: sample the centre of each quadrant away from the bar
			bool ok = w == 854 && h == 480;
			for (int q = 0; q < 4 && ok; ++q)
			{
				const int x = (q & 1) ? w * 3 / 4 : w / 4, y = (q & 2) ? h * 3 / 4 : h / 4;
				const uint8_t* px = base + y * pitch + 4 * x;
				// unless the bar is there, each channel is either ~30 or ~220
				for (int c = 0; c < 3; ++c)
				{
					const int v = px[c];
					if (!(std::abs(v - 30) < 25 || std::abs(v - 220) < 25 || v > 240))
					{
						printf("  quadrant %d channel %d = %d\n", q, c, v);
						ok = false;
					}
				}
			}
			CVPixelBufferUnlockBaseAddress(last_image, kCVPixelBufferLock_ReadOnly);
			return ok;
		}
	};

	// fake phone camera: encode a synthetic NV12 picture and send it as CAMERA_FRAGMENTs
	struct PhoneCamera
	{
		VTCompressionSessionRef enc = nullptr;
		Phone* phone;
		uint32_t frame_id = 1, seq = 0;
		bool force_key = true;
		static void Encoded(void* refcon, void*, OSStatus st, VTEncodeInfoFlags, CMSampleBufferRef sb)
		{
			auto* self = (PhoneCamera*)refcon;
			if (st != noErr || !sb)
				return;
			std::vector<uint8_t> frame;
			bool key = true;
			if (CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false); att && CFArrayGetCount(att))
			{
				auto d = (CFDictionaryRef)CFArrayGetValueAtIndex(att, 0);
				CFBooleanRef ns = nullptr;
				if (CFDictionaryGetValueIfPresent(d, kCMSampleAttachmentKey_NotSync, (const void**)&ns) && ns == kCFBooleanTrue)
					key = false;
			}
			if (key)
			{
				auto fmt = CMSampleBufferGetFormatDescription(sb);
				size_t count = 0;
				CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, nullptr, nullptr, &count, nullptr);
				for (size_t i = 0; i < count; ++i)
				{
					const uint8_t* p;
					size_t n;
					CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &p, &n, nullptr, nullptr);
					const uint8_t len[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
					frame.insert(frame.end(), len, len + 4);
					frame.insert(frame.end(), p, p + n);
				}
			}
			auto bb = CMSampleBufferGetDataBuffer(sb);
			const size_t base = frame.size(), size = CMBlockBufferGetDataLength(bb);
			frame.resize(base + size);
			CMBlockBufferCopyDataBytes(bb, 0, size, frame.data() + base);
			MediaFragment t;
			t.frame_id = self->frame_id++;
			t.flags = key ? kFragKeyframe : 0;
			t.timestamp_us = now_us();
			t.width = 640;
			t.height = 480;
			for (const auto& p : Packetize(MsgType::CameraFragment, self->seq, t, frame.data(), frame.size()))
				self->phone->send(p, self->phone->media);
		}
		void Send(int n)
		{
			if (!enc)
			{
				VTCompressionSessionCreate(nullptr, 640, 480, kCMVideoCodecType_H264, nullptr, nullptr, nullptr, &PhoneCamera::Encoded, this, &enc);
				VTSessionSetProperty(enc, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
				VTSessionSetProperty(enc, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
			}
			for (int i = 0; i < n; ++i)
			{
				CVPixelBufferRef pb = nullptr;
				CVPixelBufferCreate(nullptr, 640, 480, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, nullptr, &pb);
				CVPixelBufferLockBaseAddress(pb, 0);
				auto* y = (uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
				auto* uv = (uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 1);
				const size_t yp = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvp = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
				for (int r = 0; r < 480; ++r)
					memset(y + r * yp, r < 240 ? 60 : 200, 640); // dark top half, bright bottom half
				for (int r = 0; r < 240; ++r)
					memset(uv + r * uvp, 128, 640);
				CVPixelBufferUnlockBaseAddress(pb, 0);
				CFDictionaryRef props = nullptr;
				if (force_key)
				{
					const void* k[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
					const void* v[] = {kCFBooleanTrue};
					props = CFDictionaryCreate(nullptr, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
					force_key = false;
				}
				VTCompressionSessionEncodeFrame(enc, pb, CMTimeMake(i, 30), kCMTimeInvalid, props, nullptr, nullptr);
				if (props)
					CFRelease(props);
				CVPixelBufferRelease(pb);
				VTCompressionSessionCompleteFrames(enc, kCMTimeInvalid);
				std::this_thread::sleep_for(33ms);
			}
		}
	};

	double median(std::vector<double> v)
	{
		if (v.empty())
			return 0;
		std::sort(v.begin(), v.end());
		return v[v.size() / 2];
	}

	int selftest()
	{
		const uint16_t port = 26790;
		std::string err;
		CHECK(Media::Start(port + 1, err));
		ControlServer control;
		control.Start(port);
		Source source;
		source.Start();

		Phone phone(port);
		CHECK(phone.Handshake());
		printf("handshake ok (IPv6 loopback to a dual-stack socket), media port %u\n", ntohs(phone.media.sin6_port));

		// 1. steady stream for 3 s
		phone.Run(3000);
		const uint32_t decoded = phone.decoded;
		printf("3 s: source %u frames, decoded %u, keyframes %u, decode errors %u, capture->decoded median %.1f ms\n",
			source.frames.load(), decoded, phone.keyframes, phone.decode_errors, median(phone.latencies_ms));
		CHECK(decoded > 150);           // ~180 at 60 fps (the first frames wait for the encoder)
		CHECK(phone.decode_errors == 0);
		CHECK(phone.keyframes >= 1);
		CHECK(median(phone.latencies_ms) < 25);
		CHECK(phone.CheckImage());
		printf("hub: %s\n", Media::StatusText().c_str());

		// 2. lose a fragment: keyframe request, recovery within a few frames
		const uint32_t keys_before = phone.keyframes;
		phone.drop_fragment_of_frame = phone.last_frame_id + 10;
		const uint32_t dec_before = phone.decoded;
		phone.Run(1000);
		printf("after a lost fragment: keyframe requests %u, keyframes %u -> %u, decoded %u more\n",
			phone.keyframe_requests, keys_before, phone.keyframes, phone.decoded - dec_before);
		CHECK(phone.keyframe_requests >= 1);
		CHECK(phone.keyframes > keys_before);
		CHECK(phone.decoded - dec_before > 45);
		CHECK(phone.CheckImage());

		// 3. bitrate goes up while clean (starts at 6 Mbit/s)
		CHECK(!phone.bitrates.empty() && phone.bitrates.back() > phone.bitrates.front());
		printf("bitrate %u -> %u kbit/s\n", phone.bitrates.front() * 10, phone.bitrates.back() * 10);

		// 4. camera: the game opens the camera -> MEDIA_CONTROL asks for it -> phone sends -> NV12 here
		Media::SetCameraWanted(true);
		phone.Run(400);
		CHECK(Media::PhoneCameraSelected());
		CHECK(phone.last_control.flags & kControlCamera);
		PhoneCamera cam{nullptr, &phone};
		cam.Send(10);
		std::this_thread::sleep_for(100ms);
		std::vector<uint8_t> nv12(768 * 480 * 3 / 2, 0);
		const bool got = Media::GetCameraFrameNV12(nv12.data(), 640, 480, 768);
		CHECK(got);
		if (got)
		{
			const int top = nv12[100 * 768 + 320], bottom = nv12[380 * 768 + 320], uv = nv12[768 * 480 + 100 * 768 + 320];
			printf("camera: NV12 Y top %d bottom %d, UV %d\n", top, bottom, uv);
			CHECK(std::abs(top - 60) < 12 && std::abs(bottom - 200) < 12 && std::abs(uv - 128) < 12);
		}
		Media::SetCameraWanted(false);

		// 5. microphone: wanted -> control flag -> samples through the jitter buffer
		Media::SetMicWanted(true);
		phone.Run(400);
		CHECK(phone.last_control.flags & kControlMic);
		CHECK(Media::PhoneMicSelected());
		uint32_t first = 0;
		for (int k = 0; k < 8; ++k) // 80 ms of a 1 kHz tone
		{
			int16_t s[320];
			for (int i = 0; i < 320; ++i)
				s[i] = (int16_t)(10000 * std::sin(2 * M_PI * 1000 * (first + i) / 32000.0));
			MicAudio m;
			m.first_sample = first;
			m.sample_count = 320;
			phone.send(BuildMicAudio(phone.seq++, m, s), phone.media);
			first += 320;
		}
		std::this_thread::sleep_for(100ms);
		int16_t out[96];
		CHECK(Media::ReadMic(out, 96));
		int peak = 0;
		for (int16_t v : out)
			peak = std::max(peak, std::abs((int)v));
		printf("mic: read 96 samples, peak %d\n", peak);
		CHECK(peak > 8000);
		printf("hub: %s\n", Media::StatusText().c_str());

		// 5b. a "blow" (loud white noise, like breath on a microphone) for 400 ms, through ReadMic as
		// mic.rpl's mic_updateOnAXFrame pulls it (96 samples per 3 ms AX frame) into the game's ring buffer
		// the way mic_feedSamples writes it (big-endian, 10240 samples like Wii Fit U's MICInit), and read
		// back the way the game's jet::Platform::GetDRCMicSamples does (MICGetStatus: available and read
		// index; big-endian halfwords; up to 533 per call; MICSetDataConsumed)
		{
			uint32_t rng = 12345;
			std::vector<int16_t> sent;
			const uint32_t ring_size = 10240;
			std::vector<uint16_t> ring(ring_size, 0); // game memory: big-endian samples
			uint32_t read_index = 0, write_index = 0;
			std::vector<int16_t> game;
			int frame = 0;
			// one AX frame (3 ms): mic_updateOnAXFrame, and the game's read about once per video frame
			auto ax_frame = [&] {
				int16_t blk[96];
				CHECK(Media::ReadMic(blk, 96));
				for (int16_t v : blk) // mic_feedSamples
				{
					ring[write_index] = (uint16_t)(((uint16_t)v >> 8) | ((uint16_t)v << 8));
					write_index = (write_index + 1) % ring_size;
				}
				if (++frame % 5 == 0)
				{
					const uint32_t avail = (ring_size + write_index - read_index) % ring_size; // MICGetStatus
					const uint32_t n = std::min<uint32_t>(avail, 533);
					for (uint32_t i = 0; i < n; ++i)
					{
						const uint16_t be = ring[(read_index + i) % ring_size];
						game.push_back((int16_t)((be >> 8) | (be << 8))); // lhax
					}
					read_index = (read_index + n) % ring_size; // MICSetDataConsumed
				}
			};
			const auto t0 = std::chrono::steady_clock::now();
			for (int k = 0; k < 40; ++k) // 400 ms, sent and read at real-time pace
			{
				int16_t blk[320];
				for (int i = 0; i < 320; ++i)
				{
					rng = rng * 1664525u + 1013904223u;
					blk[i] = (int16_t)((int32_t)(rng >> 16) - 32768) / 2; // about -9 dBFS RMS noise
				}
				sent.insert(sent.end(), blk, blk + 320);
				MicAudio m;
				m.first_sample = first;
				m.sample_count = 320;
				phone.send(BuildMicAudio(phone.seq++, m, blk), phone.media);
				first += 320;
				while (frame * 3 < (k + 1) * 10)
				{
					std::this_thread::sleep_until(t0 + std::chrono::milliseconds(frame * 3));
					ax_frame();
				}
			}
			for (int i = 0; i < 40; ++i) // 120 ms more: drain
			{
				std::this_thread::sleep_until(t0 + std::chrono::milliseconds(frame * 3));
				ax_frame();
			}
			auto rms_db = [](const std::vector<int16_t>& v) {
				double sum = 0;
				int nz = 0;
				for (int16_t x : v)
					if (x) { sum += (double)x * x; nz++; }
				return nz ? 20 * std::log10(std::sqrt(sum / nz) / 32768.0) : -96.0;
			};
			// the game sees the same samples, in order
			// (the tone from step 5 is still ahead of it in the buffer: find where the noise starts)
			size_t start = game.size();
			for (size_t i = 0; i + 4 <= game.size(); ++i)
				if (std::equal(sent.begin(), sent.begin() + 4, game.begin() + i))
				{
					start = i;
					break;
				}
			const size_t matched = (start < game.size() && start + 1000 <= game.size())
				? (size_t)std::distance(sent.begin(), std::mismatch(sent.begin(), sent.end(), game.begin() + start, game.end()).first) : 0;
			printf("blow: sent %.1f dBFS, game ring buffer %.1f dBFS, %zu samples to the game, first %zu identical to what was sent\n",
				rms_db(sent), rms_db(game), game.size(), matched);
			CHECK(std::abs(rms_db(sent) - rms_db(game)) < 0.5);
			CHECK(matched >= 8000);
			printf("hub: %s\n", Media::StatusText().c_str());
		}

		// 6. video off: the hub stops sending
		for (int i = 0; i < 3; ++i)
		{
			phone.Feedback(false, false);
			std::this_thread::sleep_for(100ms);
		}
		std::this_thread::sleep_for(100ms);
		CHECK(!Media::VideoWanted());

		source.Stop();
		control.Stop();
		printf(fails ? "%d FAILED\n" : "all passed\n", fails);
		return fails ? 1 : 0;
	}

	int serve(uint16_t port, int seconds)
	{
		std::string err;
		if (!Media::Start(port + 1, err))
		{
			printf("can't open media port %u: %s\n", port + 1, err.c_str());
			return 2;
		}
		ControlServer control;
		control.Start(port);
		Source source;
		source.Start();
		printf("serving on UDP %u (control) and %u (media) for %d s\n", port, port + 1, seconds);
		fflush(stdout);
		for (int i = 0; i < seconds; ++i)
		{
			std::this_thread::sleep_for(1s);
			printf("%s\n", Media::StatusText().c_str());
			fflush(stdout);
		}
		source.Stop();
		control.Stop();
		return 0;
	}
}

int main(int argc, char** argv)
{
	const std::string mode = argc > 1 ? argv[1] : "selftest";
	if (mode == "serve")
		return serve(argc > 2 ? (uint16_t)atoi(argv[2]) : 26790, argc > 3 ? atoi(argv[3]) : 30);
	return selftest();
}
