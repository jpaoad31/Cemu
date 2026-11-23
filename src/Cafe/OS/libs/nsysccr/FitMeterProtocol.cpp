#include "FitMeterProtocol.h"

#include <cstdio>

namespace FitMeter
{
	uint8_t Crc8(const uint8_t* data, size_t size)
	{
		uint8_t crc = 0;
		for (size_t i = 0; i < size; i++)
		{
			crc ^= data[i];
			for (int b = 0; b < 8; b++)
				crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
		}
		return crc;
	}

	void XorSamu(std::vector<uint8_t>& data)
	{
		for (auto& b : data)
			b ^= SAMU_XOR;
	}

	std::vector<uint8_t> BuildIrcFrame(uint8_t sid, uint16_t rxSize, const std::vector<uint8_t>& payload, bool crc)
	{
		std::vector<uint8_t> f;
		const size_t len = payload.size() + 2; // rxSize + payload
		f.push_back(0xA5);
		f.push_back(sid);
		if (len > 0x3F)
		{
			const uint16_t v = 0x4000 | (uint16_t)(len & 0x3FFF);
			f.push_back((uint8_t)(v >> 8));
			f.push_back((uint8_t)v);
		}
		else
			f.push_back((uint8_t)len);
		f.push_back((uint8_t)(rxSize >> 8));
		f.push_back((uint8_t)rxSize);
		f.insert(f.end(), payload.begin(), payload.end());
		if (crc)
			f.push_back(Crc8(f.data(), f.size()));
		return f;
	}

	bool ParseIrcFrame(const uint8_t* data, size_t size, IrcFrame& out, std::string* error)
	{
		auto fail = [&](const char* msg) {
			if (error)
				*error = msg;
			return false;
		};
		if (size < 3)
			return fail("short frame");
		if (data[0] != 0xA5)
			return fail("no A5 magic");
		out.sid = data[1];
		const uint8_t flags = data[2];
		out.control = (flags & 0x80) != 0;
		size_t len, hdr;
		if (flags & 0x40)
		{
			if (size < 4)
				return fail("short frame");
			len = (((size_t)flags << 8) | data[3]) & 0x3FFF;
			hdr = 4;
		}
		else
		{
			len = flags & 0x3F;
			hdr = 3;
		}
		if (out.control)
		{
			if (hdr + len > size)
				return fail("length past the buffer");
			out.rxSize = 0;
			out.payload.assign(data + hdr, data + hdr + len);
			return true;
		}
		if (len < 2)
			return fail("length < 2");
		if (hdr + len > size)
			return fail("length past the buffer");
		out.rxSize = (uint16_t)((data[hdr] << 8) | data[hdr + 1]);
		out.payload.assign(data + hdr + 2, data + hdr + len);
		return true;
	}

	uint16_t RxFieldToReplyPayload(uint16_t rxField)
	{
		if (rxField <= 5)
			return 0;
		return rxField <= 67 ? (uint16_t)(rxField - 6) : (uint16_t)(rxField - 7);
	}

	uint16_t ReplyPayloadToRxField(uint16_t replyPayload)
	{
		if (replyPayload == 0)
			return 5;
		return replyPayload <= 61 ? (uint16_t)(replyPayload + 6) : (uint16_t)(replyPayload + 7);
	}

	std::string HexBytes(const uint8_t* data, size_t size, size_t max)
	{
		std::string s;
		char buf[4];
		for (size_t i = 0; i < size && i < max; i++)
		{
			std::snprintf(buf, sizeof(buf), i ? " %02X" : "%02X", data[i]);
			s += buf;
		}
		if (size > max)
			s += " ...";
		return s;
	}

	const char* SamuCommandName(uint8_t cmd)
	{
		switch (cmd)
		{
		case SAMU_READ_IDENT: return "read ident";
		case SAMU_READ_TIME: return "read time";
		case SAMU_READ_METS: return "read METs";
		case SAMU_READ_ALTITUDE: return "read altitude";
		case SAMU_READ_TAG: return "read activity tag";
		case SAMU_READ_KCAL: return "read kcal";
		case SAMU_READ_STEPS: return "read steps";
		case SAMU_WRITE_IDENT: return "write ident (register)";
		case SAMU_WRITE_TIME: return "write time";
		case SAMU_WRITE_LOCALIZE: return "write localize/common/sound data";
		case SAMU_WRITE_MII_ICON: return "write Mii icon";
		case SAMU_WRITE_MII_INFO: return "write Mii info";
		case SAMU_WRITE_PERSONAL: return "write personal data";
		case SAMU_INITIALIZE: return "initialize (unregister)";
		case SAMU_NOT_YOURS: return "not your meter / not registered";
		default:
			if (cmd >= 0x86 && cmd <= 0x8B)
				return "debug write";
			return "unknown";
		}
	}

	std::string DescribeIrcu(const std::vector<uint8_t>& p)
	{
		char buf[160];
		if (p.empty())
			return "(empty)";
		switch (p[0])
		{
		case IRCU_COMMAND:
			if (p.size() >= 4)
			{
				const uint8_t cmd = p[1];
				const unsigned arg = (unsigned)((p[2] << 8) | p[3]);
				std::snprintf(buf, sizeof(buf), "F4 command %02X arg %u: SAMU %s", cmd, arg, SamuCommandName(cmd));
				return buf;
			}
			return "F4 command (short)";
		case IRCU_DATA:
		case IRCU_DATA_LAST:
			std::snprintf(buf, sizeof(buf), "%02X data%s, %zu bytes: ", p[0], p[0] == IRCU_DATA_LAST ? " (last)" : "", p.size() - 1);
			return buf + HexBytes(p.data() + 1, p.size() - 1, 24);
		case IRCU_ACK: return "F2 ack";
		case IRCU_ACK_LAST: return "F3 ack (last / ready)";
		case IRCU_RESEND: return "F5 resend";
		default:
			return "?? " + HexBytes(p.data(), p.size(), 16);
		}
	}
}
