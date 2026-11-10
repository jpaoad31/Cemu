#pragma once

// Remote GamePad wire protocol (version 1). Pure C++, no Cemu dependencies, so it can be
// unit-tested on its own (see RemoteGamePadProtocolTest.cpp).
// The authoritative description is PROTOCOL.md in the iOS app repository
// (jpaoad31/wiiu-gamepad-remote); keep the two in sync.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace RemoteGamePad
{
	constexpr uint8_t kMagic[4] = {'W', 'U', 'G', 'P'};
	constexpr uint8_t kVersion = 1;
	// minor revision, announced in HELLO; 1.1 appends press counters to INPUT
	constexpr uint16_t kMinorVersion = 1;
	constexpr uint16_t kDefaultPort = 26770;
	constexpr size_t kHeaderSize = 12;
	constexpr size_t kMaxPacketSize = 1400;

	enum class MsgType : uint8_t
	{
		Hello = 0x01,    // phone -> host
		HelloAck = 0x02, // host -> phone
		Input = 0x03,    // phone -> host, ~120 Hz
		Bye = 0x04,      // either direction
		InputAck = 0x05, // host -> phone, ~16 Hz: latency echo
		// 0x10-0x1F: video (phase 2, separate stream), 0x20-0x2F: rumble, 0x30-0x3F: audio (phase 3)
	};

	// Wii U GamePad buttons, already mapped by position on the phone
	enum Button : uint32_t
	{
		kBtnA = 1u << 0,
		kBtnB = 1u << 1,
		kBtnX = 1u << 2,
		kBtnY = 1u << 3,
		kBtnL = 1u << 4,
		kBtnR = 1u << 5,
		kBtnZL = 1u << 6,
		kBtnZR = 1u << 7,
		kBtnPlus = 1u << 8,
		kBtnMinus = 1u << 9,
		kBtnHome = 1u << 10,
		kBtnStickL = 1u << 11,
		kBtnStickR = 1u << 12,
		kBtnUp = 1u << 13,
		kBtnDown = 1u << 14,
		kBtnLeft = 1u << 15,
		kBtnRight = 1u << 16,
		kBtnTV = 1u << 17,
	};
	constexpr int kButtonCount = 18;

	// Protocol button -> Cemu's emulated GamePad. The controller reports protocol bit N as raw
	// button N (kButtonN); VPADController's default mapping binds VPADController::ButtonId
	// `vpad_button_id` to it, and the game sees `vpad_flag` (VPAD_BUTTON_*). TV has no VPAD button.
	struct GamePadButtonMap
	{
		uint32_t bit;
		int vpad_button_id; // VPADController::ButtonId
		uint32_t vpad_flag;   // VPADStatus hold bit (Home: 0x0002, delivered via the HOME menu path)
		const char* name;
	};
	inline constexpr GamePadButtonMap kGamePadButtons[] = {
		{kBtnA, 1, 0x8000, "A"},
		{kBtnB, 2, 0x4000, "B"},
		{kBtnX, 3, 0x2000, "X"},
		{kBtnY, 4, 0x1000, "Y"},
		{kBtnL, 5, 0x0020, "L"},
		{kBtnR, 6, 0x0010, "R"},
		{kBtnZL, 7, 0x0080, "ZL"},
		{kBtnZR, 8, 0x0040, "ZR"},
		{kBtnPlus, 9, 0x0008, "+"},
		{kBtnMinus, 10, 0x0004, "-"},
		{kBtnUp, 11, 0x0200, "Up"},
		{kBtnDown, 12, 0x0100, "Down"},
		{kBtnLeft, 13, 0x0800, "Left"},
		{kBtnRight, 14, 0x0400, "Right"},
		{kBtnStickL, 15, 0x00040000, "StickL"},
		{kBtnStickR, 16, 0x00020000, "StickR"},
		{kBtnHome, 27, 0x0002, "Home"},
	};

	// raw button index (kButtonN) of a protocol bit
	constexpr int BitIndex(uint32_t bit)
	{
		int i = 0;
		while (bit > 1)
		{
			bit >>= 1;
			++i;
		}
		return i;
	}

	// "A B Up" for a buttons bitfield (status lines)
	inline std::string ButtonNames(uint32_t buttons)
	{
		static const char* names[kButtonCount] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "+", "-", "Home",
			"StickL", "StickR", "Up", "Down", "Left", "Right", "TV"};
		std::string s;
		for (int i = 0; i < kButtonCount; ++i)
		{
			if (buttons & (1u << i))
			{
				if (!s.empty())
					s += ' ';
				s += names[i];
			}
		}
		return s.empty() ? std::string("none") : s;
	}

	enum InputFlags : uint8_t
	{
		kFlagTouchDown = 1 << 0,
		kFlagMotionValid = 1 << 1,
		kFlagOrientationValid = 1 << 2,
	};

	enum Capability : uint16_t
	{
		kCapMotion = 1 << 0,
		kCapTouch = 1 << 1,
		kCapVideo = 1 << 2,  // phase 2
		kCapRumble = 1 << 3, // phase 3
		kCapAudio = 1 << 4,  // phase 3
	};

	struct Header
	{
		uint8_t version;
		MsgType type;
		uint16_t length; // whole packet, header included
		uint32_t seq;
	};

	struct Hello
	{
		uint32_t client_id = 0;
		uint16_t capabilities = 0;
		uint16_t minor_version = 0; // 0 = 1.0, 1 = 1.1
		std::string device_name;
		std::string device_model;
		std::string app_version;
	};

	struct HelloAck
	{
		uint32_t client_id = 0;
		uint16_t capabilities = 0;
		uint16_t video_port = 0; // 0: no video stream (phase 1)
		std::string host_name;
	};

	// echo of the newest input packet, for round-trip time; also the host's view of the stream
	struct InputAck
	{
		uint32_t acked_seq = 0;
		uint64_t phone_timestamp_us = 0; // copied from that input packet
		uint64_t host_receive_us = 0;    // host monotonic clock, when it arrived
		uint16_t host_packet_rate = 0;   // packets/s seen by the host
		uint16_t host_max_gap_ms = 0;    // longest interval between packets, last ~3 s
	};
	constexpr size_t kInputAckPacketSize = kHeaderSize + 24;

	constexpr size_t kInputPayloadSize = 88;
	constexpr size_t kInputPacketSize = kHeaderSize + kInputPayloadSize; // 100 (1.0)
	// 1.1: + u8 press_count[18], u8 touch_count, u8 reserved
	constexpr size_t kInputPacketSizeV11 = kInputPacketSize + 20;      // 120

	struct Input
	{
		uint64_t timestamp_us = 0; // sender's monotonic clock
		uint32_t buttons = 0;
		float lx = 0, ly = 0, rx = 0, ry = 0; // -1..1, +y = up
		float zl = 0, zr = 0;                 // 0..1
		uint8_t flags = 0;
		float touch_x = 0, touch_y = 0;       // 0..1, origin top-left of the GamePad screen
		std::array<float, 3> gyro{};          // rad/s, GamePad frame
		std::array<float, 3> accel{};         // g, GamePad frame, CoreMotion sign (face up = z -1)
		std::array<float, 4> orientation{1, 0, 0, 0}; // w,x,y,z, informational
		// 1.1: wrapping counters of presses per button bit and of touch-downs, so a press shorter
		// than a packet interval (or in a lost packet) is still seen
		bool has_counts = false;
		std::array<uint8_t, kButtonCount> press_count{};
		uint8_t touch_count = 0;

		bool touch_down() const { return flags & kFlagTouchDown; }
		bool motion_valid() const { return flags & kFlagMotionValid; }
	};

	// Press latch: a press (button or touch-down) that started since the last game read is reported
	// to that read as down even if it has been released already, so taps shorter than a game frame
	// (VPADRead at ~60 Hz samples the newest state) aren't lost. Releases otherwise keep their real
	// timing. Presses are found from rising edges between packets and, with protocol 1.1, from the
	// phone's press counters (presses that started and ended between two packets, or were in a lost
	// packet). Unread presses expire after kExpiryUs (no game running).
	class PressLatch
	{
	public:
		static constexpr uint64_t kExpiryUs = 250000;

		struct View
		{
			uint32_t buttons = 0;
			bool touch = false;
			float touch_x = 0, touch_y = 0;
			uint32_t touch_seq = 0; // identifies the touch-down, for consume()
		};

		// prev: the previous packet of the same stream; continuous = false after a reconnect/timeout
		void OnInput(const Input& prev, bool continuous, const Input& in, uint64_t now_us)
		{
			uint32_t pressed = continuous ? (in.buttons & ~prev.buttons) : in.buttons;
			bool touch_pressed = in.touch_down() && (!continuous || !prev.touch_down());
			if (in.has_counts)
			{
				if (m_have_counts && continuous)
				{
					for (int i = 0; i < kButtonCount; ++i)
						if (in.press_count[i] != m_counts[i])
							pressed |= 1u << i;
					if (in.touch_count != m_touch_count)
						touch_pressed = true;
				}
				m_counts = in.press_count;
				m_touch_count = in.touch_count;
				m_have_counts = true;
			}
			else
				m_have_counts = false;
			if (pressed)
			{
				m_view.buttons |= pressed;
				m_button_time = now_us;
			}
			if (touch_pressed)
			{
				// the packet's position: where the finger is, or where a quick tap was lifted
				m_view.touch = true;
				m_view.touch_x = in.touch_x;
				m_view.touch_y = in.touch_y;
				m_view.touch_seq++;
				m_touch_time = now_us;
			}
		}

		View Get(uint64_t now_us) const
		{
			View v = m_view;
			if (now_us - m_button_time > kExpiryUs)
				v.buttons = 0;
			if (now_us - m_touch_time > kExpiryUs)
				v.touch = false;
			return v;
		}

		// a game read reported these: drop them (the touch only if no newer touch-down came since)
		void Consume(uint32_t buttons, bool touch, uint32_t touch_seq)
		{
			m_view.buttons &= ~buttons;
			if (touch && m_view.touch_seq == touch_seq)
				m_view.touch = false;
		}

		void Clear()
		{
			m_view.buttons = 0;
			m_view.touch = false;
			m_have_counts = false;
		}

	private:
		View m_view{};
		uint64_t m_button_time = 0, m_touch_time = 0;
		bool m_have_counts = false;
		std::array<uint8_t, kButtonCount> m_counts{};
		uint8_t m_touch_count = 0;
	};

	// parse the 12-byte header; nullopt if magic/version/length are wrong
	std::optional<Header> ParseHeader(const uint8_t* data, size_t size);
	std::optional<Hello> ParseHello(const uint8_t* data, size_t size);
	std::optional<Input> ParseInput(const uint8_t* data, size_t size);

	std::vector<uint8_t> BuildHelloAck(uint32_t seq, const HelloAck& ack);
	std::vector<uint8_t> BuildInputAck(uint32_t seq, const InputAck& ack);
	std::optional<InputAck> ParseInputAck(const uint8_t* data, size_t size);
	// used by the unit test (and handy for tools)
	std::vector<uint8_t> BuildHello(uint32_t seq, const Hello& hello);
	std::vector<uint8_t> BuildInput(uint32_t seq, const Input& input);

	// true when seq is newer than last (wrap-around safe)
	inline bool SeqNewer(uint32_t seq, uint32_t last) { return (int32_t)(seq - last) > 0; }

	// Converts protocol motion (GamePad frame: +X right, +Y top edge, +Z out of the screen,
	// right-handed; gyro rad/s right-hand rule; accel in g with CoreMotion's sign, so lying face up
	// reads z = -1) into the arguments Cemu's WiiUMotionHandler::processMotionSample expects.
	// This is exactly what Cemu's SDL path produces for an SDL gamepad whose face is the GamePad's
	// face (SDL: +X right, +Y out of the face, +Z towards the player; accel = specific force).
	struct HandlerMotion
	{
		float gx, gy, gz;
		float ax, ay, az;
	};
	inline HandlerMotion ToMotionHandlerFrame(const std::array<float, 3>& gyro, const std::array<float, 3>& accel)
	{
		// SDL frame:  X = gp.x, Y = gp.z, Z = -gp.y; SDL accel = -accel (CoreMotion sign -> specific force)
		// SDL path:   gyro -> (x, -y, -z); acc -> -sdl/g, then handler gets (acc.x, -acc.y, -acc.z)
		HandlerMotion m;
		m.gx = gyro[0];
		m.gy = -gyro[2];
		m.gz = gyro[1];
		m.ax = accel[0];
		m.ay = -accel[2];
		m.az = accel[1];
		return m;
	}
}
