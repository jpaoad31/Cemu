// Standalone tests of the virtual Fit Meter: frames/CRC/XOR, the GamePad IR link, the IRCU slave + SAMU
// (registration, sync, persistence, initialize) driven by a model of Wii Fit U's IRC/IRCU master, and the
// log-stream encoder. Not in CMake (Cemu has no test target). Build and run:
//
//   P=build/vcpkg_installed/arm64-osx; D=src/Cafe/OS/libs/nsysccr
//   clang++ -std=c++20 -Wall -I $P/include $D/FitMeterTest.cpp $D/FitMeterProtocol.cpp $D/DrcIrLink.cpp \
//     $D/VirtualFitMeter.cpp $D/FitMeterActivity.cpp -o build/fitmeter_test && build/fitmeter_test
//
// Dump mode (for tools/fitmeter/decoder_harness.py, which runs the game's own decoder on the streams):
//   build/fitmeter_test --dump activity.json 2026-11-22T14:30 outdir/
// writes outdir/b2.bin .. b6.bin: what the meter sends for banks 02..06 (XOR undone), through the whole link.

#include "DrcIrLink.h"
#include "FitMeterActivity.h"
#include "FitMeterProtocol.h"
#include "VirtualFitMeter.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace FitMeter;
namespace fs = std::filesystem;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; const auto _a = (a); const auto _b = (b); if (!(_a == _b)) { std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, (long long)_a, (long long)_b); g_failures++; } } while (0)

// ---------------------------------------------------------------------------------------------------------
// Wii Fit U's side, as read from its asm (IRC: IRCProc/IRCSend/__CCRCDCIRC*; ircu: IRCUConnect mode 0,
// IRCUMasterSend, IRCUMasterReceive, ____IRCUReceive with the data callback's acks; SAMU::ThreadMain's XOR)

struct Console
{
	DrcIrLink& link;
	uint64_t& now;
	bool verbose = false;
	static constexpr size_t PACKET = 509; // .data 0x1014D764
	uint8_t req[520]{}, resp[520]{};

	int32_t Ctl(uint32_t reqSize, uint32_t respSize) { return link.Control(2, req, reqSize, resp, respSize, now); }

	bool SendPayload(const std::vector<uint8_t>& payload, uint16_t replyPayload)
	{
		// __CCRCDCIRCSend: A5, sid 0, flags/len, rxSize, payload, a CRC slot; request zero-padded to 520
		auto frame = BuildIrcFrame(0, ReplyPayloadToRxField(replyPayload), payload, false);
		frame.push_back(0); // the GamePad fills in the CRC
		std::memset(req, 0, sizeof(req));
		req[0] = 1;
		req[1] = (uint8_t)(frame.size() >> 8);
		req[2] = (uint8_t)frame.size();
		std::memcpy(req + 3, frame.data(), frame.size());
		return Ctl(520, 1) == 0 && resp[0] == 0;
	}

	// IRCProc + receive: one payload within timeoutMs (time advances 1 ms per poll)
	bool ReceivePayload(std::vector<uint8_t>& p, uint32_t timeoutMs, uint8_t* result = nullptr)
	{
		for (uint32_t t = 0; t <= timeoutMs; t++, now++)
		{
			const uint32_t st = link.GetStatus(now);
			if (st & DrcIrLink::STATUS_EVENT)
			{
				req[0] = 2;
				Ctl(1, 520);
				link.ClearEvent();
				if (result)
					*result = resp[0];
				if (resp[0] != 0)
					return false;
				const uint16_t size = (uint16_t)((resp[1] << 8) | resp[2]);
				IrcFrame f;
				if (!ParseIrcFrame(resp + 3, size, f))
					return false;
				// the CRC the GamePad adds
				CHECK_EQ(resp[3 + size - 1], Crc8(resp + 3, size - 1));
				p = f.payload;
				return true;
			}
		}
		return false;
	}

	// IRCUConnect(0, 1000, mode 0): CONNECT type 0 (wait), rxSize 1; link up; then the meter's F3
	bool Connect(uint8_t* failResult = nullptr)
	{
		std::memset(req, 0, sizeof(req));
		req[0] = 0;
		req[1] = 1000 >> 8;
		req[2] = 1000 & 0xFF;
		req[3] = 0;          // 115200
		req[4] = 0;
		req[5] = 1 + 6;      // rxSize 1 + the header
		req[6] = 0xDC;       // IRCInit(0, 220)
		req[7] = 0;          // type 0
		if (Ctl(8, 1) != 0 || resp[0] != 0)
			return false;
		for (int t = 0; t < 2000; t++, now++)
		{
			const uint32_t st = link.GetStatus(now);
			if (st & DrcIrLink::STATUS_LINK)
			{
				std::vector<uint8_t> p;
				return ReceivePayload(p, 3000) && p.size() == 1 && p[0] == IRCU_ACK_LAST;
			}
			if (st & DrcIrLink::STATUS_EVENT)
			{
				req[0] = 2;
				Ctl(1, 520);
				if (failResult)
					*failResult = resp[0];
				return false;
			}
		}
		return false;
	}

	// SAMU::ThreadMain: retries IRCUConnect while it fails with a timeout
	bool ConnectRetry(int tries = 300)
	{
		for (int i = 0; i < tries; i++)
		{
			uint8_t r = 0;
			if (Connect(&r))
				return true;
			if (r != DrcIrLink::RESULT_TIMEOUT)
				return false;
			Disconnect();
		}
		return false;
	}

	void Disconnect()
	{
		req[0] = 3;
		Ctl(1, 1);
	}

	// IRCUMasterSend(0, cmd, arg, data, len, 1000)
	bool MasterSend(uint8_t cmd, uint16_t arg, std::vector<uint8_t> data)
	{
		XorSamu(data);
		std::vector<uint8_t> p;
		if (!SendPayload({IRCU_COMMAND, (uint8_t)(cmd | 0x80), (uint8_t)(arg >> 8), (uint8_t)arg}, 1))
			return false;
		if (!ReceivePayload(p, 1000) || p != std::vector<uint8_t>{IRCU_ACK})
			return false;
		for (size_t off = 0; off < data.size();)
		{
			const size_t n = std::min(PACKET, data.size() - off);
			const bool last = off + n == data.size();
			std::vector<uint8_t> chunk{last ? IRCU_DATA_LAST : IRCU_DATA};
			chunk.insert(chunk.end(), data.begin() + (ptrdiff_t)off, data.begin() + (ptrdiff_t)(off + n));
			for (;;)
			{
				if (!SendPayload(chunk, 1) || !ReceivePayload(p, 1000) || p.size() != 1)
					return false;
				if (p[0] == IRCU_RESEND)
					continue;
				if (p[0] != (last ? IRCU_ACK_LAST : IRCU_ACK))
					return false;
				break;
			}
			off += n;
		}
		return true;
	}

	// IRCUMasterReceive(0, cmd, arg, buf, size, 1000, 500)
	bool MasterReceive(uint8_t cmd, uint16_t arg, size_t size, std::vector<uint8_t>& out)
	{
		out.clear();
		std::vector<uint8_t> p;
		if (!SendPayload({IRCU_COMMAND, (uint8_t)(cmd & 0x7F), (uint8_t)(arg >> 8), (uint8_t)arg}, (uint16_t)(std::min(size, PACKET) + 1)))
			return false;
		while (out.size() < size)
		{
			if (!ReceivePayload(p, 1000) || p.empty() || (p[0] != IRCU_DATA && p[0] != IRCU_DATA_LAST))
				return false;
			if (out.size() + p.size() - 1 > size)
				return false; // error 20 in the game
			out.insert(out.end(), p.begin() + 1, p.end());
			const size_t remaining = size - out.size();
			const bool last = p[0] == IRCU_DATA_LAST;
			if (!SendPayload({last ? IRCU_ACK_LAST : IRCU_ACK}, (uint16_t)(std::min(remaining, PACKET) + 1)))
				return false;
		}
		if (!ReceivePayload(p, 1000) || p != std::vector<uint8_t>{IRCU_ACK_LAST})
			return false;
		XorSamu(out);
		return true;
	}
};

// ---------------------------------------------------------------------------------------------------------

static int64_t LocalEpoch(int y, int mo, int d, int h, int mi)
{
	std::tm tm{};
	tm.tm_year = y - 1900;
	tm.tm_mon = mo - 1;
	tm.tm_mday = d;
	tm.tm_hour = h;
	tm.tm_min = mi;
	tm.tm_isdst = -1;
	return (int64_t)std::mktime(&tm);
}

static void TestFrames()
{
	// CRC-8 poly 0x07 init 0: the standard check value of "123456789" is 0xF4
	const char* s = "123456789";
	CHECK_EQ(Crc8((const uint8_t*)s, 9), 0xF4);
	// a small frame
	auto f = BuildIrcFrame(0x12, 7, {0xF4, 0x80, 0x00, 0x00}, true);
	CHECK_EQ(f.size(), (size_t)(3 + 2 + 4 + 1));
	CHECK_EQ(f[0], 0xA5);
	CHECK_EQ(f[2], 6);
	CHECK_EQ(f.back(), Crc8(f.data(), f.size() - 1));
	IrcFrame pf;
	CHECK(ParseIrcFrame(f.data(), f.size(), pf));
	CHECK_EQ(pf.sid, 0x12);
	CHECK_EQ(pf.rxSize, 7);
	CHECK(pf.payload == std::vector<uint8_t>({0xF4, 0x80, 0x00, 0x00}));
	// a large one (len > 0x3F: 0x4000 | len)
	std::vector<uint8_t> big(300, 0x5A);
	big[0] = IRCU_DATA;
	f = BuildIrcFrame(1, 0, big, true);
	CHECK_EQ(f[2], (uint8_t)(0x40 | ((302 >> 8) & 0x3F)));
	CHECK_EQ(f[3], (uint8_t)(302 & 0xFF));
	CHECK(ParseIrcFrame(f.data(), f.size(), pf));
	CHECK(pf.payload == big);
	// a control frame is flagged
	const uint8_t ctl[] = {0xA5, 0x00, 0x81, 0x02, 0x00};
	CHECK(ParseIrcFrame(ctl, sizeof(ctl), pf));
	CHECK(pf.control);
	// bad magic
	const uint8_t bad[] = {0xA4, 0x00, 0x02, 0x00, 0x00};
	CHECK(!ParseIrcFrame(bad, sizeof(bad), pf));
	// rxSize field <-> reply size (__CCRCDCIRCSend: +6, +7 above 61, 0 -> 5)
	for (uint16_t n : {1, 4, 61, 62, 100, 510})
		CHECK_EQ(RxFieldToReplyPayload(ReplyPayloadToRxField(n)), n);
	CHECK_EQ(ReplyPayloadToRxField(0), 5);
	// XOR
	std::vector<uint8_t> x = {0x00, 0xAA, 0xFF};
	XorSamu(x);
	CHECK(x == std::vector<uint8_t>({0xAA, 0x00, 0x55}));
	XorSamu(x);
	CHECK(x == std::vector<uint8_t>({0x00, 0xAA, 0xFF}));
	CHECK(DescribeIrcu({0xF4, 0x06, 0x00, 0x78}).find("read steps") != std::string::npos);
}

static void TestEncoder()
{
	Activity a;
	std::string err;
	const char* json = R"({ "days": [
		{ "date": "2026-11-22", "steps": 1234, "kcal": 56 },
		{ "date": "2026-11-20", "steps": 70000 },
		{ "date": "2026-11-23", "steps": 9 } ] })";
	CHECK(ParseActivityJson(json, a, err));
	MeterTime now{2026, 11, 22, 14, 30};
	auto d = EncodeDayStream(a, now, 0, false, 120);
	CHECK_EQ(d.size(), (size_t)120);
	CHECK_EQ((d[0] << 8) | d[1], 1234);  // today
	CHECK_EQ((d[2] << 8) | d[3], 0);     // 11-21: no entry inside the range = 0
	CHECK_EQ((d[4] << 8) | d[5], 0xFFF9); // 11-20: capped (FFFC..FFFF are markers)
	CHECK_EQ((d[6] << 8) | d[7], 0xFFFF); // before the oldest day: no data
	auto k = EncodeDayStream(a, now, 0, true, 120);
	CHECK_EQ((k[0] << 8) | k[1], 56);
	CHECK_EQ((k[4] << 8) | k[5], EstimateKcal(70000));
	// night owl: at 01:00 the meter's "today" is still the day before
	MeterTime early{2026, 11, 23, 1, 0};
	auto n = EncodeDayStream(a, early, 3, false, 120);
	CHECK_EQ((n[0] << 8) | n[1], 1234);
	auto n0 = EncodeDayStream(a, early, 0, false, 120);
	CHECK_EQ((n0[0] << 8) | n0[1], 9);
	// no data at all
	Activity empty;
	auto e = EncodeDayStream(empty, now, 0, false, 120);
	CHECK(e == std::vector<uint8_t>(120, 0xFF));
	CHECK(EncodeMinuteStream(empty, now, 2, 100) == std::vector<uint8_t>(100, 0xFF));

	// minutes: METs 3.0 at 14:28-14:29, rest earlier from 14:00 (segments), now 14:30
	Activity m;
	CHECK(ParseActivityJson(R"({ "days": [ { "date": "2026-11-22", "segments": [
		{ "start": "14:00", "minutes": 1, "mets": 2.0 },
		{ "start": "14:28", "minutes": 2, "mets": 3.0, "tag": 5, "climb_m": 4 } ] } ] })", m, err));
	auto s2 = EncodeMinuteStream(m, now, 2, 10080);
	// newest first: 14:30 (no value -> 0), 14:29 30, 14:28 30, 14:27..14:01 (27 x 0), 14:00 20
	const std::vector<uint8_t> want2 = {1, 0, 30, 30, 27, 0, 20};
	CHECK(std::vector<uint8_t>(s2.begin(), s2.begin() + 7) == want2);
	CHECK_EQ(s2[7], 0xFF);
	auto s4 = EncodeMinuteStream(m, now, 4, 10080);
	const std::vector<uint8_t> want4 = {1, 0, 5, 5, 28, 0}; // 14:00 has METs, default tag 0
	CHECK(std::vector<uint8_t>(s4.begin(), s4.begin() + 6) == want4);
	auto s3 = EncodeMinuteStream(m, now, 3, 10080);
	const std::vector<uint8_t> want3 = {1, 0, 2, 2, 28, 0}; // +4 m over two minutes; from 14:00 like the METs
	CHECK(std::vector<uint8_t>(s3.begin(), s3.begin() + 6) == want3);
	CHECK_EQ(s3[6], 0xFF);
	// altitude from absolute values: up 300 m in one minute (spills), then down 5
	Activity h;
	std::string alt = R"({ "days": [ { "date": "2026-11-22", "altitude": [)";
	for (int i = 0; i < 870; i++)
		alt += i < 860 ? "100," : (i < 865 ? "400," : "395,");
	alt += "395] } ] }";
	CHECK(ParseActivityJson(alt, h, err));
	auto a3 = EncodeMinuteStream(h, now, 3, 10080);
	// time order from 00:00: 860 x 0, then +127 +127 +46 0 0, then -5, 0..0 (to 14:30)
	// newest first: 14:30 0, 14:29..14:25 (5 x -5? no: one -5 at 14:25 then 0) ...
	std::vector<int> deltas; // decode back
	for (size_t i = 0; i < a3.size() && a3[i] != 0xFF;)
	{
		if (i + 1 < a3.size() && a3[i + 1] == 0x00)
		{
			for (int r = 0; r < a3[i]; r++)
				deltas.push_back(0);
			i += 2;
		}
		else
		{
			deltas.push_back(a3[i] < 0x80 ? a3[i] : -(a3[i] - 0x80 + 1));
			i++;
		}
	}
	CHECK_EQ(deltas.size(), (size_t)871);
	int sum = 0;
	for (int v : deltas)
		sum += v;
	CHECK_EQ(sum, 295);
	CHECK_EQ(deltas[870 - 860], 127);
	CHECK_EQ(deltas[870 - 861], 127);
	CHECK_EQ(deltas[870 - 862], 46);
	CHECK_EQ(deltas[870 - 865], -5);
	// a limit that cuts through an "n 00" pair drops the whole pair
	auto cut = EncodeMinuteStream(m, now, 2, 5); // tokens: [1 0][30][30][27 0] -> 4 bytes fit
	CHECK(cut == std::vector<uint8_t>({1, 0, 30, 30, 0xFF}));
	// dates
	int32_t dn;
	CHECK(ParseDate("2026-02-28", dn));
	CHECK(FormatDate(dn + 1) == "2026-03-01");
	CHECK(!ParseDate("2026-02-30", dn));
}

static void TestLinkAndMeter(const fs::path& dir)
{
	fs::remove_all(dir);
	fs::create_directories(dir);
	int64_t wall = LocalEpoch(2026, 11, 22, 14, 30) + 17;
	uint64_t now = 1000;
	auto meter = std::make_unique<VirtualFitMeter>(dir.string());
	meter->SetWallClock([&] { return wall; });
	std::vector<std::string> log;
	DrcIrLink link;
	link.SetLogger([&](const std::string& s) { log.push_back(s); });
	meter->SetLogger([&](const std::string& s) { log.push_back(s); });
	link.SetPeer(meter.get());
	Console c{link, now};

	// disabled meter: the connect times out with result 1 (IRCU's timeout)
	meter->State().enabled = false;
	uint8_t fail = 0xEE;
	CHECK(!c.Connect(&fail));
	CHECK_EQ(fail, DrcIrLink::RESULT_TIMEOUT);
	CHECK(link.GetState() == DrcIrLink::State::Idle);
	meter->State().enabled = true;

	// --- registration (SAMUSeq mode 3: ident must be unregistered, then the initial data, ident last)
	CHECK(c.ConnectRetry());
	CHECK(link.GetState() == DrcIrLink::State::Connected);
	CHECK(now >= 1000 + 1500); // the auto press waited ~1.5 s from the first attempt
	std::vector<uint8_t> r;
	CHECK(c.MasterReceive(SAMU_READ_IDENT, 0, 9, r));
	CHECK_EQ(r.size(), (size_t)9);
	CHECK_EQ(r[8], 0xFF); // not registered
	// time: the console's 2026-11-22 15:45 (meter clock was 14:30)
	CHECK(c.MasterSend(SAMU_WRITE_TIME, 0, {0, 26, 11, 22, 15, 45}));
	std::vector<uint8_t> loc(1200);
	for (size_t i = 0; i < loc.size(); i++)
		loc[i] = (uint8_t)(i * 7);
	CHECK(c.MasterSend(SAMU_WRITE_LOCALIZE, 0x10, loc)); // 3 chunks: 509 509 182
	std::vector<uint8_t> personal = {40, 0x01, 0x2C, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0x00, 0xC8};
	CHECK(c.MasterSend(SAMU_WRITE_PERSONAL, 0, personal));
	for (uint16_t i = 0; i < 8; i++)
		CHECK(c.MasterSend(SAMU_WRITE_MII_ICON, i, std::vector<uint8_t>(200, (uint8_t)i)));
	std::vector<uint8_t> mii(102, 0);
	const char16_t* name = u"Alice";
	for (int i = 0; name[i]; i++)
	{
		mii[0x1A + i * 2] = (uint8_t)(name[i] >> 8); // Wii U: big-endian
		mii[0x1A + i * 2 + 1] = (uint8_t)name[i];
	}
	CHECK(c.MasterSend(SAMU_WRITE_MII_INFO, 0, mii));
	CHECK(c.MasterSend(SAMU_WRITE_IDENT, 0, {0x12, 0x34, 0x56, 0x78, 0xCA, 0xFE, 0xBA, 0xBE, 0xFF}));
	c.Disconnect();
	CHECK(link.GetState() == DrcIrLink::State::Idle);
	CHECK(meter->State().registered);
	CHECK(meter->State().miiName == "Alice");
	CHECK_EQ(meter->State().clockOffsetSeconds, 75 * 60);
	CHECK(meter->State().blobs.at("82_10") == loc);
	CHECK(meter->State().DayStartHour() == 3);

	// --- persistence: a new meter object from the same folder
	meter = std::make_unique<VirtualFitMeter>(dir.string());
	meter->SetWallClock([&] { return wall; });
	link.SetPeer(meter.get());
	CHECK(meter->State().registered);
	CHECK_EQ(meter->State().samuId, 0xCAFEBABEu);
	CHECK_EQ(meter->State().saveDataId, 0x12345678u);
	CHECK(meter->State().miiInfo == mii);
	CHECK(meter->State().personal == personal);
	CHECK(meter->State().blobs.size() == 9);
	CHECK(meter->State().blobs.at("83_07") == std::vector<uint8_t>(200, 7));
	CHECK(meter->State().clockSet);

	// --- sync (SAMUSeq mode 4): ident, time, then banks 02..06
	{
		std::ofstream f(dir / "activity.json");
		f << R"({ "days": [ { "date": "2026-11-22", "steps": 4321, "kcal": 99,
			"segments": [ { "start": "15:00", "minutes": 10, "mets": 4.0, "tag": 3, "climb_m": 20 } ] },
			{ "date": "2026-11-21", "steps": 10000 } ] })";
	}
	wall += 3600; // an hour later: meter 16:45, console 16:45
	CHECK(c.ConnectRetry());
	CHECK(c.MasterReceive(SAMU_READ_IDENT, 0, 9, r));
	CHECK(r == std::vector<uint8_t>({0x12, 0x34, 0x56, 0x78, 0xCA, 0xFE, 0xBA, 0xBE, 0x00}));
	CHECK(c.MasterReceive(SAMU_READ_TIME, 0, 6, r));
	CHECK(r == std::vector<uint8_t>({0, 26, 11, 22, 16, 45}));
	std::vector<uint8_t> b2, b3, b4, b5, b6;
	CHECK(c.MasterReceive(SAMU_READ_METS, 10080, 10080, b2));
	CHECK(c.MasterReceive(SAMU_READ_ALTITUDE, 10080, 10080, b3));
	CHECK(c.MasterReceive(SAMU_READ_TAG, 10080, 10080, b4));
	CHECK(c.MasterReceive(SAMU_READ_KCAL, 120, 120, b5));
	CHECK(c.MasterReceive(SAMU_READ_STEPS, 120, 120, b6));
	c.Disconnect();
	CHECK_EQ(b6.size(), (size_t)120);
	CHECK_EQ((b6[0] << 8) | b6[1], 4321);
	CHECK_EQ((b6[2] << 8) | b6[3], 10000);
	CHECK_EQ((b5[0] << 8) | b5[1], 99);
	// METs: 16:45 back to 15:10 = 96 minutes of 0, then 10 x 40 (15:09..15:00)
	CHECK(std::vector<uint8_t>(b2.begin(), b2.begin() + 12) == std::vector<uint8_t>({96, 0, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40}));
	CHECK_EQ(b2[12], 0xFF);
	CHECK(std::vector<uint8_t>(b3.begin(), b3.begin() + 12) == std::vector<uint8_t>({96, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2}));
	CHECK_EQ(b4[2], 3);
	CHECK(!meter->State().lastSync.empty());
	CHECK_EQ(meter->State().syncCount, 1u);

	// a resend request (F5) repeats the last packet
	CHECK(c.ConnectRetry());
	CHECK(c.SendPayload({IRCU_COMMAND, SAMU_READ_IDENT, 0, 0}, 10));
	std::vector<uint8_t> p1, p2;
	CHECK(c.ReceivePayload(p1, 100));
	CHECK(c.SendPayload({IRCU_RESEND}, 10));
	CHECK(c.ReceivePayload(p2, 100));
	CHECK(p1 == p2 && !p1.empty() && p1[0] == IRCU_DATA_LAST);
	c.Disconnect();

	// the manual button links without the auto press
	meter->State().autoPress = false;
	meter->PressButton(now);
	CHECK(c.ConnectRetry());
	c.Disconnect();
	meter->State().autoPress = true;

	// --- the game's "initialize" (SAMUSeq mode 1/2: Send(0xF0) with one byte 00)
	CHECK(c.ConnectRetry());
	CHECK(c.MasterSend(SAMU_INITIALIZE, 0, {0x00}));
	c.Disconnect();
	CHECK(!meter->State().registered);
	meter = std::make_unique<VirtualFitMeter>(dir.string());
	CHECK(!meter->State().registered);
	CHECK(meter->State().clockSet); // the clock stays
	link.SetPeer(nullptr);

	bool sawFrame = false;
	for (const auto& l : log)
		if (l.find("console -> meter: F4 command 06 arg 120: SAMU read steps") != std::string::npos)
			sawFrame = true;
	CHECK(sawFrame);
}

static int Dump(const char* activityPath, const char* when, const char* outDir)
{
	int y, mo, d, h, mi;
	if (std::sscanf(when, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5)
	{
		std::printf("bad time %s (YYYY-MM-DDTHH:MM)\n", when);
		return 2;
	}
	const fs::path tmp = fs::temp_directory_path() / "fitmeter_dump";
	fs::remove_all(tmp);
	fs::create_directories(tmp);
	fs::create_directories(outDir);
	int64_t wall = LocalEpoch(y, mo, d, h, mi);
	uint64_t now = 0;
	VirtualFitMeter meter(tmp.string());
	meter.SetWallClock([&] { return wall; });
	meter.SetDataSource(std::make_shared<JsonFileMeterSource>(activityPath));
	meter.State().registered = true;
	meter.State().clockSet = true;
	meter.State().autoPressDelayMs = 0;
	DrcIrLink link;
	link.SetPeer(&meter);
	Console c{link, now};
	if (!c.ConnectRetry())
	{
		std::printf("connect failed\n");
		return 1;
	}
	const struct { uint8_t bank; size_t size; } banks[] = {{2, 10080}, {3, 10080}, {4, 10080}, {5, 120}, {6, 120}};
	for (const auto& b : banks)
	{
		std::vector<uint8_t> data;
		if (!c.MasterReceive(b.bank, (uint16_t)b.size, b.size, data))
		{
			std::printf("bank %d failed\n", b.bank);
			return 1;
		}
		std::ofstream f(fs::path(outDir) / ("b" + std::to_string(b.bank) + ".bin"), std::ios::binary);
		f.write((const char*)data.data(), (std::streamsize)data.size());
	}
	c.Disconnect();
	std::printf("wrote %s/b2.bin .. b6.bin\n", outDir);
	return 0;
}

int main(int argc, char** argv)
{
	if (argc == 5 && std::strcmp(argv[1], "--dump") == 0)
		return Dump(argv[2], argv[3], argv[4]);
	setenv("TZ", "UTC", 1);
	tzset();
	TestFrames();
	TestEncoder();
	TestLinkAndMeter(fs::temp_directory_path() / "fitmeter_test");
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
