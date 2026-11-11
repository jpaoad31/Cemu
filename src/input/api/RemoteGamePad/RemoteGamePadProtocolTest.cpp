// Standalone check of the Remote GamePad protocol parser (not part of the CMake build: Cemu has no test target).
// Build and run from the repository root:
//   clang++ -std=c++20 -Wall -I src src/input/api/RemoteGamePad/RemoteGamePadProtocolTest.cpp src/input/api/RemoteGamePad/RemoteGamePadProtocol.cpp \
//     src/input/api/RemoteGamePad/RemoteGamePadMediaCore.cpp -o /tmp/rgtest && /tmp/rgtest
#include "input/api/RemoteGamePad/RemoteGamePadProtocol.h"
#include "input/api/RemoteGamePad/RemoteGamePadMediaCore.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
using namespace RemoteGamePad;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

int main()
{
	// --- input round trip, byte layout
	Input in;
	in.timestamp_us = 0x0102030405060708ull;
	in.buttons = kBtnA | kBtnZR | kBtnHome | kBtnRight;
	in.lx = 0.5f; in.ly = -0.25f; in.rx = -1.0f; in.ry = 1.0f;
	in.zl = 0.75f; in.zr = 1.0f;
	in.flags = kFlagTouchDown | kFlagMotionValid;
	in.touch_x = 0.125f; in.touch_y = 0.875f;
	in.gyro = {0.1f, -0.2f, 0.3f};
	in.accel = {0.0f, -1.0f, 0.0f};
	in.orientation = {1, 0, 0, 0};
	auto pkt = BuildInput(0xDEADBEEF, in);
	CHECK(pkt.size() == kInputPacketSize);
	CHECK(pkt.size() == 100);
	CHECK(memcmp(pkt.data(), "WUGP", 4) == 0);
	CHECK(pkt[4] == 1 && pkt[5] == 0x03);
	CHECK(pkt[6] == 100 && pkt[7] == 0);
	CHECK(pkt[8] == 0xEF && pkt[11] == 0xDE); // little-endian seq
	CHECK(pkt[12] == 0x08 && pkt[19] == 0x01); // little-endian timestamp
	CHECK(pkt[12 + 36] == (kFlagTouchDown | kFlagMotionValid)); // flags at payload offset 36
	// 0.5f = 0x3F000000 at payload offset 12
	CHECK(pkt[24] == 0 && pkt[25] == 0 && pkt[26] == 0 && pkt[27] == 0x3F);

	auto h = ParseHeader(pkt.data(), pkt.size());
	CHECK(h && h->type == MsgType::Input && h->seq == 0xDEADBEEF && h->length == 100);
	auto out = ParseInput(pkt.data(), pkt.size());
	CHECK(out.has_value());
	if (out)
	{
		CHECK(out->timestamp_us == in.timestamp_us);
		CHECK(out->buttons == in.buttons);
		CHECK(out->lx == 0.5f && out->ly == -0.25f && out->rx == -1.0f && out->ry == 1.0f);
		CHECK(out->zl == 0.75f && out->zr == 1.0f);
		CHECK(out->touch_down() && out->motion_valid());
		CHECK(out->touch_x == 0.125f && out->touch_y == 0.875f);
		CHECK(out->gyro[2] == 0.3f && out->accel[1] == -1.0f);
	}

	// --- 1.1 press counters
	{
		Input c = in;
		c.has_counts = true;
		c.press_count[0] = 3;
		c.press_count[17] = 250;
		c.touch_count = 7;
		auto cp = BuildInput(2, c);
		CHECK(cp.size() == kInputPacketSizeV11 && cp.size() == 120 && cp[6] == 120);
		CHECK(cp[100] == 3 && cp[117] == 250 && cp[118] == 7);
		auto co = ParseInput(cp.data(), cp.size());
		CHECK(co && co->has_counts && co->press_count[0] == 3 && co->press_count[17] == 250 && co->touch_count == 7);
		CHECK(out && !out->has_counts); // a 1.0 packet has none
	}

	// --- rejects
	CHECK(!ParseInput(pkt.data(), 99));                    // truncated
	{ auto bad = pkt; bad[0] = 'X'; CHECK(!ParseHeader(bad.data(), bad.size())); }
	{ auto bad = pkt; bad[4] = 2; CHECK(!ParseHeader(bad.data(), bad.size())); }   // other major version
	{ auto bad = pkt; bad[6] = 120; CHECK(!ParseHeader(bad.data(), bad.size())); } // length > datagram
	{ auto bad = pkt; bad[5] = 0x01; CHECK(!ParseInput(bad.data(), bad.size())); } // wrong type
	// longer packet (future fields appended) is accepted
	{
		auto longer = pkt;
		longer.resize(116, 0xAA);
		longer[6] = 116;
		CHECK(ParseInput(longer.data(), longer.size()).has_value());
	}
	// NaN and out-of-range values are sanitised
	{
		Input w = in;
		w.lx = std::numeric_limits<float>::quiet_NaN();
		w.ry = 7.0f;
		w.touch_x = -3.0f;
		w.gyro[0] = std::numeric_limits<float>::infinity();
		auto p = BuildInput(1, w);
		auto o = ParseInput(p.data(), p.size());
		CHECK(o && o->lx == 0 && o->ry == 1.0f && o->touch_x == 0.0f && !o->motion_valid() && o->gyro[0] == 0);
	}

	// --- hello / ack
	Hello hello;
	hello.client_id = 0x11223344;
	hello.capabilities = kCapMotion | kCapTouch;
	hello.device_name = "John's iPhone";
	hello.device_model = "iPhone17,1";
	hello.app_version = "1.0 (1)";
	hello.minor_version = 1;
	auto hp = BuildHello(7, hello);
	auto ho = ParseHello(hp.data(), hp.size());
	CHECK(ho && ho->client_id == 0x11223344 && ho->capabilities == 3 && ho->device_name == "John's iPhone" &&
		ho->device_model == "iPhone17,1" && ho->app_version == "1.0 (1)" && ho->minor_version == 1);
	{ auto bad = hp; bad[20] = 200; CHECK(!ParseHello(bad.data(), bad.size())); } // name length overruns
	HelloAck ack;
	ack.client_id = 0x11223344;
	ack.capabilities = kCapMotion | kCapTouch;
	ack.host_name = "Cemu on mac";
	auto ap = BuildHelloAck(3, ack);
	CHECK(ap.size() == 12 + 8 + 1 + ack.host_name.size());
	CHECK(ap[5] == 0x02 && ap[12] == 0x44 && ap[20] == ack.host_name.size());

	{
		InputAck ia{0x01020304, 123456789012ull, 987654321ull, 120, 45};
		auto ip = BuildInputAck(9, ia);
		CHECK(ip.size() == kInputAckPacketSize && ip.size() == 36 && ip[5] == 0x05);
		auto io = ParseInputAck(ip.data(), ip.size());
		CHECK(io && io->acked_seq == 0x01020304 && io->phone_timestamp_us == 123456789012ull &&
			io->host_receive_us == 987654321ull && io->host_packet_rate == 120 && io->host_max_gap_ms == 45);
	}

	// --- sequence wrap
	CHECK(SeqNewer(1, 0) && !SeqNewer(0, 1) && SeqNewer(0, 0xFFFFFFFF) && !SeqNewer(5, 5));

	// --- motion frame: must equal Cemu's SDL path for an SDL pad whose face is the GamePad face.
	// SDL path (SDLControllerProvider.cpp): acc_t = -sdl/9.81; handler(acc_t.x, -acc_t.y, -acc_t.z);
	// gyro_t = (sdl.x, -sdl.y, -sdl.z). SDL frame: X right, Y out of the face, Z towards the player.
	auto sdl_path = [](const float sdlGyro[3], const float sdlAcc[3]) {
		HandlerMotion m;
		float acc[3] = {-sdlAcc[0] / 9.81f, -sdlAcc[1] / 9.81f, -sdlAcc[2] / 9.81f};
		m.ax = acc[0]; m.ay = -acc[1]; m.az = -acc[2];
		m.gx = sdlGyro[0]; m.gy = -sdlGyro[1]; m.gz = -sdlGyro[2];
		return m;
	};
	// lying face up: protocol accel (CoreMotion sign) z = -1; SDL specific force +Y = +9.81
	{
		const float sdlG[3] = {0, 0, 0}, sdlA[3] = {0, 9.81f, 0};
		auto a = ToMotionHandlerFrame({0, 0, 0}, {0, 0, -1});
		auto b = sdl_path(sdlG, sdlA);
		CHECK(near(a.ax, b.ax) && near(a.ay, b.ay) && near(a.az, b.az));
		CHECK(near(a.ay, 1.0));
	}
	// held upright facing the player (top edge up): CoreMotion y = -1; SDL: towards player axis Z points down -> +Z... specific force up = -Z
	{
		const float sdlG[3] = {0, 0, 0}, sdlA[3] = {0, 0, -9.81f};
		auto a = ToMotionHandlerFrame({0, 0, 0}, {0, -1, 0});
		auto b = sdl_path(sdlG, sdlA);
		CHECK(near(a.ax, b.ax) && near(a.ay, b.ay) && near(a.az, b.az));
	}
	// rotations about each GamePad axis: gp x -> SDL x, gp y (top) -> SDL -Z, gp z (out of screen) -> SDL Y
	{
		const float g1[3] = {1, 0, 0}, g2[3] = {0, 0, -1}, g3[3] = {0, 1, 0}, z[3] = {0, 0, 0};
		auto a1 = ToMotionHandlerFrame({1, 0, 0}, {0, 0, 0}); auto b1 = sdl_path(g1, z);
		auto a2 = ToMotionHandlerFrame({0, 1, 0}, {0, 0, 0}); auto b2 = sdl_path(g2, z);
		auto a3 = ToMotionHandlerFrame({0, 0, 1}, {0, 0, 0}); auto b3 = sdl_path(g3, z);
		CHECK(near(a1.gx, b1.gx) && near(a1.gy, b1.gy) && near(a1.gz, b1.gz));
		CHECK(near(a2.gx, b2.gx) && near(a2.gy, b2.gy) && near(a2.gz, b2.gz));
		CHECK(near(a3.gx, b3.gx) && near(a3.gy, b3.gy) && near(a3.gz, b3.gz));
	}

	// --- press latch: 120 Hz packets, a game reading at 60 Hz (like VPADRead)
	{
		struct Sim
		{
			PressLatch latch;
			Input prev{};
			bool continuous = false;
			uint64_t t = 1000000;
			int reads_with_a = 0, reads_with_touch = 0;
			float last_touch_x = -1;
			uint64_t next_read = 1000000 + 4000; // reads offset from packets
			Input current{};
			void packet(const Input& in)
			{
				latch.OnInput(prev, continuous, in, t);
				prev = in;
				current = in;
				continuous = true;
			}
			void read()
			{
				const auto v = latch.Get(t);
				const uint32_t b = current.buttons | v.buttons;
				const bool touch = current.touch_down() || v.touch;
				if (b & kBtnA)
					reads_with_a++;
				if (touch)
				{
					reads_with_touch++;
					last_touch_x = current.touch_down() ? current.touch_x : v.touch_x;
				}
				latch.Consume(v.buttons, v.touch, v.touch_seq);
			}
			// advance time to `until`, doing the game's reads on the way
			void run(uint64_t until)
			{
				while (next_read <= until)
				{
					t = next_read;
					read();
					next_read += 16667;
				}
				t = until;
			}
		};
		auto idle = [](Sim& s, int packets, Input base) {
			for (int i = 0; i < packets; ++i)
			{
				s.run(s.t + 8333);
				s.packet(base);
			}
		};

		// 1.0 phone: A down in one packet only, up 5 ms later (between two reads)
		{
			Sim s;
			Input base;
			idle(s, 10, base);
			s.reads_with_a = 0;
			Input down = base;
			down.buttons = kBtnA;
			s.run(s.t + 8333);
			s.packet(down);
			s.run(s.t + 5000);
			s.packet(base);
			idle(s, 20, base);
			CHECK(s.reads_with_a == 1);
		}
		// 1.1 phone: press never visible as down, only the counter moves
		{
			Sim s;
			Input base;
			base.has_counts = true;
			idle(s, 10, base);
			s.reads_with_a = 0;
			base.press_count[0]++;
			idle(s, 20, base);
			CHECK(s.reads_with_a == 1);
		}
		// quick touch, counter only (1.1), at 0.3,0.6
		{
			Sim s;
			Input base;
			base.has_counts = true;
			idle(s, 10, base);
			Input lifted = base;
			lifted.touch_count++;
			lifted.touch_x = 0.3f;
			lifted.touch_y = 0.6f;
			idle(s, 20, lifted);
			CHECK(s.reads_with_touch == 1 && s.last_touch_x == 0.3f);
		}
		// quick touch, one down packet (1.0)
		{
			Sim s;
			Input base;
			idle(s, 10, base);
			Input down = base;
			down.flags = kFlagTouchDown;
			down.touch_x = 0.7f;
			s.run(s.t + 8333);
			s.packet(down);
			s.run(s.t + 5000);
			Input up = down;
			up.flags = 0;
			s.packet(up);
			idle(s, 20, up);
			CHECK(s.reads_with_touch == 1 && s.last_touch_x == 0.7f);
		}
		// a 100 ms hold: seen by ~6 reads, and released on time (no extra read after release)
		{
			Sim s;
			Input base;
			idle(s, 10, base);
			s.reads_with_a = 0;
			Input down = base;
			down.buttons = kBtnA;
			idle(s, 12, down); // 100 ms
			s.packet(base);    // release
			const int during = s.reads_with_a;
			idle(s, 12, base);
			CHECK(during >= 5 && during <= 7);
			CHECK(s.reads_with_a == during);
		}
		// nobody reads: the latch expires
		{
			PressLatch l;
			Input a, b;
			b.buttons = kBtnA;
			l.OnInput(a, true, b, 1000);
			CHECK(l.Get(1000 + 100000).buttons == kBtnA);
			CHECK(l.Get(1000 + PressLatch::kExpiryUs + 1).buttons == 0);
		}
		// a touch-down after the read sampled the latch survives that read's consume
		{
			PressLatch l;
			Input up, down;
			down.flags = kFlagTouchDown;
			l.OnInput(up, true, down, 1000);
			const auto v = l.Get(1000);
			l.OnInput(down, true, up, 2000);
			l.OnInput(up, true, down, 3000); // second tap before the consume
			l.Consume(v.buttons, v.touch, v.touch_seq);
			CHECK(l.Get(4000).touch);
		}
	}

	// --- each protocol button alone -> raw button index -> VPAD button id -> VPAD hold bit
	{
		// reference VPAD_BUTTON_* values (wut vpad/input.h)
		struct Ref { uint32_t bit; uint32_t vpad; };
		const Ref refs[] = {
			{kBtnA, 0x8000}, {kBtnB, 0x4000}, {kBtnX, 0x2000}, {kBtnY, 0x1000},
			{kBtnL, 0x0020}, {kBtnR, 0x0010}, {kBtnZL, 0x0080}, {kBtnZR, 0x0040},
			{kBtnPlus, 0x0008}, {kBtnMinus, 0x0004}, {kBtnHome, 0x0002},
			{kBtnUp, 0x0200}, {kBtnDown, 0x0100}, {kBtnLeft, 0x0800}, {kBtnRight, 0x0400},
			{kBtnStickR, 0x00020000}, {kBtnStickL, 0x00040000},
		};
		for (const auto& r : refs)
		{
			// a packet with only this button, through the parser
			Input one;
			one.buttons = r.bit;
			auto pk = BuildInput(1, one);
			auto parsed = ParseInput(pk.data(), pk.size());
			CHECK(parsed && parsed->buttons == r.bit);
			// raw button index the controller reports (bit N -> kButtonN)
			const int raw = BitIndex(parsed->buttons);
			CHECK((1u << raw) == r.bit);
			// default mapping entry for that raw button
			int found = 0;
			uint32_t flag = 0;
			for (const auto& m : kGamePadButtons)
				if (BitIndex(m.bit) == raw)
				{
					found++;
					flag = m.vpad_flag;
				}
			if (found != 1 || flag != r.vpad)
				printf("FAIL button %s: %d mapping(s), VPAD flag %#x, want %#x\n", ButtonNames(r.bit).c_str(), found, flag, r.vpad);
			CHECK(found == 1 && flag == r.vpad);
		}
		// every GamePad button id appears once
		for (const auto& a : kGamePadButtons)
		{
			int n = 0;
			for (const auto& b : kGamePadButtons)
				n += a.vpad_button_id == b.vpad_button_id;
			CHECK(n == 1);
		}
		CHECK(ButtonNames(kBtnA | kBtnUp) == "A Up" && ButtonNames(0) == "none");
	}


	// --- 1.2 media messages
	{
		// a fake AVCC keyframe: SPS, PPS, three IDR slices of different sizes
		std::vector<uint8_t> frame;
		auto nal = [&](uint8_t header, size_t n) {
			frame.push_back((uint8_t)(n >> 24)); frame.push_back((uint8_t)(n >> 16));
			frame.push_back((uint8_t)(n >> 8)); frame.push_back((uint8_t)n);
			frame.push_back(header);
			for (size_t i = 1; i < n; ++i)
				frame.push_back((uint8_t)(i * 7 + header));
		};
		nal(0x67, 12); nal(0x68, 4); nal(0x65, 1500); nal(0x65, 900); nal(0x65, 2000);
		const auto slices = AvccSliceOffsets(frame.data(), frame.size());
		CHECK(slices.size() == 3 && slices[0] == 16 + 8);

		MediaFragment t;
		t.frame_id = 41;
		t.flags = kFragKeyframe;
		t.timestamp_us = 123456789;
		t.width = 854; t.height = 480;
		t.pipeline_100us = 42;
		t.bitrate_10kbps = 600;
		uint32_t seq = 7;
		const auto packets = Packetize(MsgType::VideoFragment, seq, t, frame.data(), frame.size());
		CHECK(packets.size() == (frame.size() + kMaxFragmentData - 1) / kMaxFragmentData);
		CHECK(seq == 7 + packets.size());
		size_t total = 0;
		for (size_t i = 0; i < packets.size(); ++i)
		{
			CHECK(packets[i].size() <= kFragmentHeaderSize + kMaxFragmentData);
			const auto f = ParseFragment(packets[i].data(), packets[i].size());
			CHECK(f && f->frame_id == 41 && f->index == i && f->count == packets.size() && f->frame_size == frame.size());
			CHECK(f && f->width == 854 && f->height == 480 && f->timestamp_us == 123456789 && f->bitrate_10kbps == 600);
			CHECK(f && memcmp(f->data, frame.data() + f->offset, f->data_size) == 0);
			total += f ? f->data_size : 0;
		}
		CHECK(total == frame.size());
		// first fragment holds SPS/PPS and the start of the first slice
		CHECK(ParseFragment(packets[0].data(), packets[0].size())->first_slice == 0);

		// assembler: in order -> complete keyframe
		{
			FrameAssembler a;
			std::optional<FrameAssembler::Frame> out;
			for (const auto& p : packets)
			{
				CHECK(!out);
				out = a.Add(*ParseFragment(p.data(), p.size()), 1000);
			}
			CHECK(out && out->data == frame && out->id == 41 && out->keyframe());
			CHECK(!a.NeedKeyframe());
			auto st = a.TakeStats();
			CHECK(st.frames_complete == 1 && st.frames_dropped == 0 && st.fragments == packets.size());
		}
		// out of order and duplicated fragments still complete once
		{
			FrameAssembler a;
			int completions = 0;
			for (int i = (int)packets.size() - 1; i >= 0; --i)
				for (int k = 0; k < 2; ++k)
					completions += a.Add(*ParseFragment(packets[i].data(), packets[i].size()), 1000).has_value();
			CHECK(completions == 1);
		}
		// a lost fragment: the frame never completes; the next P frame is dropped until a keyframe
		{
			FrameAssembler a;
			for (const auto& p : packets)
				a.Add(*ParseFragment(p.data(), p.size()), 0); // keyframe 41 complete
			auto make = [&](uint32_t id, bool key) {
				MediaFragment m = t;
				m.frame_id = id;
				m.flags = key ? kFragKeyframe : 0;
				uint32_t sq = 0;
				return Packetize(MsgType::VideoFragment, sq, m, frame.data(), frame.size());
			};
			auto p42 = make(42, false);
			for (size_t i = 1; i < p42.size(); ++i) // fragment 0 lost
				CHECK(!a.Add(*ParseFragment(p42[i].data(), p42[i].size()), 0));
			auto p43 = make(43, false);
			std::optional<FrameAssembler::Frame> r;
			for (const auto& p : p43)
				r = a.Add(*ParseFragment(p.data(), p.size()), 0);
			CHECK(!r && a.NeedKeyframe()); // 42 is missing: 43 can't be decoded
			auto st = a.TakeStats();
			CHECK(st.frames_dropped == 2 && st.fragments_missing == 1);
			// late fragment of 42 is ignored now
			CHECK(!a.Add(*ParseFragment(p42[0].data(), p42[0].size()), 0));
			auto p44 = make(44, true);
			for (const auto& p : p44)
				r = a.Add(*ParseFragment(p.data(), p.size()), 0);
			CHECK(r && r->id == 44 && !a.NeedKeyframe());
		}
		// bad fragments are rejected by the parser
		{
			auto bad = packets[0];
			bad[12 + 6] = 0; bad[12 + 7] = 0; // count 0
			CHECK(!ParseFragment(bad.data(), bad.size()));
			auto bad2 = packets[0];
			bad2[12 + 20] = 1; bad2[12 + 21] = 0; bad2[12 + 22] = 0; bad2[12 + 23] = 0; // frame size 1 < data
			CHECK(!ParseFragment(bad2.data(), bad2.size()));
		}

		// feedback, control, mic round trips
		VideoFeedback fb;
		fb.client_id = 0xCAFEF00D; fb.newest_frame = 99; fb.frames_shown = 6; fb.frames_dropped = 1;
		fb.fragments_received = 300; fb.fragments_missing = 2; fb.interval_ms = 100; fb.decode_100us = 23;
		fb.assembly_100us = 11; fb.latency_100us = 250; fb.flags = kFeedbackKeyframeRequest; fb.display_fps_x10 = 599;
		fb.max_bitrate_kbps = 12000;
		auto fp = BuildVideoFeedback(3, fb);
		CHECK(fp.size() == kVideoFeedbackSize);
		auto fb2 = ParseVideoFeedback(fp.data(), fp.size());
		CHECK(fb2 && fb2->client_id == 0xCAFEF00D && fb2->newest_frame == 99 && fb2->frames_dropped == 1 && fb2->fragments_missing == 2
			&& fb2->decode_100us == 23 && fb2->latency_100us == 250 && fb2->flags == kFeedbackKeyframeRequest && fb2->display_fps_x10 == 599
			&& fb2->max_bitrate_kbps == 12000);
		MediaControl mc;
		mc.flags = kControlCamera | kControlMic;
		auto mp = BuildMediaControl(4, mc);
		CHECK(mp.size() == kMediaControlSize);
		auto mc2 = ParseMediaControl(mp.data(), mp.size());
		CHECK(mc2 && mc2->flags == (kControlCamera | kControlMic) && mc2->camera_width == 640 && mc2->camera_height == 480 && mc2->mic_sample_rate == 32000);
		int16_t pcm[320];
		for (int i = 0; i < 320; ++i)
			pcm[i] = (int16_t)(i * 100 - 16000);
		MicAudio ma;
		ma.timestamp_us = 5; ma.first_sample = 3200; ma.sample_count = 320;
		auto ap = BuildMicAudio(5, ma, pcm);
		CHECK(ap.size() == kMicHeaderSize + 640);
		auto ma2 = ParseMicAudio(ap.data(), ap.size());
		CHECK(ma2 && ma2->sample_count == 320 && ma2->first_sample == 3200 && ma2->sample_rate == 32000);
		CHECK(ma2 && (int16_t)(ma2->samples[2] | (ma2->samples[3] << 8)) == pcm[1]);
	}

	// --- bitrate controller
	{
		BitrateController b;
		VideoFeedback clean;
		clean.fragments_received = 500; clean.interval_ms = 100; clean.assembly_100us = 20;
		uint64_t t = 0;
		for (int i = 0; i < 30; ++i) // 3 s clean: goes up
			b.OnFeedback(clean, t += 100000);
		CHECK(b.kbps() > BitrateController::kStartKbps);
		const uint32_t high = b.kbps();
		VideoFeedback lossy = clean;
		lossy.fragments_missing = 60; // > 5 %
		b.OnFeedback(lossy, t += 100000);
		CHECK(b.kbps() < high * 0.75);
		const uint32_t cut = b.kbps();
		b.OnFeedback(lossy, t += 100000); // within 300 ms of the cut: no second cut yet
		CHECK(b.kbps() == cut);
		for (int i = 0; i < 100; ++i)
			b.OnFeedback(lossy, t += 100000);
		CHECK(b.kbps() == BitrateController::kMinKbps);
		b.SetMax(3000);
		for (int i = 0; i < 300; ++i)
			b.OnFeedback(clean, t += 100000);
		CHECK(b.kbps() == 3000);
	}

	// --- microphone jitter buffer
	{
		MicJitterBuffer j(32000);
		int16_t in[320], out[96];
		for (int i = 0; i < 320; ++i)
			in[i] = (int16_t)(i + 1);
		j.Push(in, 320); // 10 ms: below the 40 ms target, still silent
		CHECK(j.Pop(out, 96) == 0 && out[0] == 0);
		for (int k = 0; k < 4; ++k)
			j.Push(in, 320);
		CHECK(j.Pop(out, 96) == 96 && out[0] == 1 && out[95] == 96);
		// overfill: trimmed back to the target
		for (int k = 0; k < 40; ++k)
			j.Push(in, 320);
		CHECK(j.buffered() <= 32000 * 150 / 1000);
		CHECK(j.overflow_drops() > 0);
		// drain: underrun counted, then silence until refilled
		int16_t big[8000];
		j.Pop(big, 8000);
		CHECK(j.underruns() == 1 && j.buffered() == 0);
	}

	printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails ? 1 : 0;
}
