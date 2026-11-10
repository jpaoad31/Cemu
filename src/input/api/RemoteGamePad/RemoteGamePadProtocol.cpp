#include "input/api/RemoteGamePad/RemoteGamePadProtocol.h"

#include <algorithm>
#include <cstring>

namespace RemoteGamePad
{
	namespace
	{
		// little-endian helpers (independent of host byte order)
		uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
		uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
		uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
		float rdf(const uint8_t* p)
		{
			const uint32_t v = rd32(p);
			float f;
			std::memcpy(&f, &v, 4);
			return f;
		}

		void wr8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
		void wr16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); }
		void wr32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF); }
		void wr64(std::vector<uint8_t>& b, uint64_t v) { wr32(b, (uint32_t)v); wr32(b, (uint32_t)(v >> 32)); }
		void wrf(std::vector<uint8_t>& b, float f)
		{
			uint32_t v;
			std::memcpy(&v, &f, 4);
			wr32(b, v);
		}
		void wrstr(std::vector<uint8_t>& b, const std::string& s)
		{
			const size_t n = std::min<size_t>(s.size(), 255);
			wr8(b, (uint8_t)n);
			b.insert(b.end(), s.begin(), s.begin() + n);
		}
		// reads a u8-length-prefixed string; false if it overruns
		bool rdstr(const uint8_t* data, size_t size, size_t& off, std::string& out)
		{
			if (off + 1 > size)
				return false;
			const size_t n = data[off++];
			if (off + n > size)
				return false;
			out.assign((const char*)data + off, n);
			off += n;
			return true;
		}

		std::vector<uint8_t> begin(MsgType type, uint32_t seq)
		{
			std::vector<uint8_t> b;
			b.reserve(128);
			b.insert(b.end(), kMagic, kMagic + 4);
			wr8(b, kVersion);
			wr8(b, (uint8_t)type);
			wr16(b, 0); // length, patched by finish()
			wr32(b, seq);
			return b;
		}
		void finish(std::vector<uint8_t>& b)
		{
			b[6] = b.size() & 0xFF;
			b[7] = (b.size() >> 8) & 0xFF;
		}

		bool clean(float f) { return f == f && f > -1e6f && f < 1e6f; } // rejects NaN/inf
		float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
	}

	std::optional<Header> ParseHeader(const uint8_t* data, size_t size)
	{
		if (size < kHeaderSize || size > kMaxPacketSize)
			return std::nullopt;
		if (std::memcmp(data, kMagic, 4) != 0)
			return std::nullopt;
		Header h;
		h.version = data[4];
		h.type = (MsgType)data[5];
		h.length = rd16(data + 6);
		h.seq = rd32(data + 8);
		// a newer minor revision may append fields, but never changes the major version byte
		if (h.version != kVersion)
			return std::nullopt;
		if (h.length < kHeaderSize || h.length > size)
			return std::nullopt;
		return h;
	}

	std::optional<Hello> ParseHello(const uint8_t* data, size_t size)
	{
		const auto h = ParseHeader(data, size);
		if (!h || h->type != MsgType::Hello)
			return std::nullopt;
		size = h->length;
		size_t off = kHeaderSize;
		if (off + 8 > size)
			return std::nullopt;
		Hello hello;
		hello.client_id = rd32(data + off);
		hello.capabilities = rd16(data + off + 4);
		hello.minor_version = rd16(data + off + 6);
		off += 8; // u32 client id, u16 caps, u16 reserved
		if (!rdstr(data, size, off, hello.device_name))
			return std::nullopt;
		// model and app version are optional (older/smaller senders)
		if (!rdstr(data, size, off, hello.device_model))
			return hello;
		rdstr(data, size, off, hello.app_version);
		return hello;
	}

	std::optional<Input> ParseInput(const uint8_t* data, size_t size)
	{
		const auto h = ParseHeader(data, size);
		if (!h || h->type != MsgType::Input || h->length < kInputPacketSize)
			return std::nullopt;
		const uint8_t* p = data + kHeaderSize;
		Input in;
		in.timestamp_us = rd64(p + 0);
		in.buttons = rd32(p + 8);
		in.lx = rdf(p + 12);
		in.ly = rdf(p + 16);
		in.rx = rdf(p + 20);
		in.ry = rdf(p + 24);
		in.zl = rdf(p + 28);
		in.zr = rdf(p + 32);
		in.flags = p[36];
		in.touch_x = rdf(p + 40);
		in.touch_y = rdf(p + 44);
		for (int i = 0; i < 3; ++i)
			in.gyro[i] = rdf(p + 48 + 4 * i);
		for (int i = 0; i < 3; ++i)
			in.accel[i] = rdf(p + 60 + 4 * i);
		for (int i = 0; i < 4; ++i)
			in.orientation[i] = rdf(p + 72 + 4 * i);
		if (h->length >= kInputPacketSizeV11)
		{
			in.has_counts = true;
			for (int i = 0; i < kButtonCount; ++i)
				in.press_count[i] = p[88 + i];
			in.touch_count = p[88 + kButtonCount];
		}

		// sanitise: a bad float must not reach the emulator
		float* all[] = {&in.lx, &in.ly, &in.rx, &in.ry, &in.zl, &in.zr, &in.touch_x, &in.touch_y};
		for (float* f : all)
			if (!clean(*f))
				*f = 0;
		in.lx = clamp(in.lx, -1, 1);
		in.ly = clamp(in.ly, -1, 1);
		in.rx = clamp(in.rx, -1, 1);
		in.ry = clamp(in.ry, -1, 1);
		in.zl = clamp(in.zl, 0, 1);
		in.zr = clamp(in.zr, 0, 1);
		in.touch_x = clamp(in.touch_x, 0, 1);
		in.touch_y = clamp(in.touch_y, 0, 1);
		for (int i = 0; i < 3; ++i)
		{
			if (!clean(in.gyro[i]) || !clean(in.accel[i]))
			{
				in.flags &= ~kFlagMotionValid;
				in.gyro = {};
				in.accel = {};
				break;
			}
		}
		return in;
	}

	std::vector<uint8_t> BuildHelloAck(uint32_t seq, const HelloAck& ack)
	{
		auto b = begin(MsgType::HelloAck, seq);
		wr32(b, ack.client_id);
		wr16(b, ack.capabilities);
		wr16(b, ack.video_port);
		wrstr(b, ack.host_name);
		finish(b);
		return b;
	}

	std::vector<uint8_t> BuildInputAck(uint32_t seq, const InputAck& ack)
	{
		auto b = begin(MsgType::InputAck, seq);
		wr32(b, ack.acked_seq);
		wr64(b, ack.phone_timestamp_us);
		wr64(b, ack.host_receive_us);
		wr16(b, ack.host_packet_rate);
		wr16(b, ack.host_max_gap_ms);
		finish(b);
		return b;
	}

	std::optional<InputAck> ParseInputAck(const uint8_t* data, size_t size)
	{
		const auto h = ParseHeader(data, size);
		if (!h || h->type != MsgType::InputAck || h->length < kInputAckPacketSize)
			return std::nullopt;
		const uint8_t* p = data + kHeaderSize;
		InputAck a;
		a.acked_seq = rd32(p);
		a.phone_timestamp_us = rd64(p + 4);
		a.host_receive_us = rd64(p + 12);
		a.host_packet_rate = rd16(p + 20);
		a.host_max_gap_ms = rd16(p + 22);
		return a;
	}

	std::vector<uint8_t> BuildHello(uint32_t seq, const Hello& hello)
	{
		auto b = begin(MsgType::Hello, seq);
		wr32(b, hello.client_id);
		wr16(b, hello.capabilities);
		wr16(b, hello.minor_version);
		wrstr(b, hello.device_name);
		wrstr(b, hello.device_model);
		wrstr(b, hello.app_version);
		finish(b);
		return b;
	}

	std::vector<uint8_t> BuildInput(uint32_t seq, const Input& in)
	{
		auto b = begin(MsgType::Input, seq);
		wr64(b, in.timestamp_us);
		wr32(b, in.buttons);
		wrf(b, in.lx);
		wrf(b, in.ly);
		wrf(b, in.rx);
		wrf(b, in.ry);
		wrf(b, in.zl);
		wrf(b, in.zr);
		wr8(b, in.flags);
		wr8(b, 0);
		wr16(b, 0);
		wrf(b, in.touch_x);
		wrf(b, in.touch_y);
		for (float f : in.gyro)
			wrf(b, f);
		for (float f : in.accel)
			wrf(b, f);
		for (float f : in.orientation)
			wrf(b, f);
		if (in.has_counts)
		{
			for (uint8_t c : in.press_count)
				wr8(b, c);
			wr8(b, in.touch_count);
			wr8(b, 0);
		}
		finish(b);
		return b;
	}
}
