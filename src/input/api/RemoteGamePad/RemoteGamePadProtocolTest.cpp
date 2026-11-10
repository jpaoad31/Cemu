// Standalone check of the Remote GamePad protocol parser (not part of the CMake build: Cemu has no test target).
// Build and run from the repository root:
//   clang++ -std=c++20 -Wall -I src src/input/api/RemoteGamePad/RemoteGamePadProtocolTest.cpp src/input/api/RemoteGamePad/RemoteGamePadProtocol.cpp -o /tmp/rgtest && /tmp/rgtest
#include "input/api/RemoteGamePad/RemoteGamePadProtocol.h"
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

	printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails ? 1 : 0;
}
