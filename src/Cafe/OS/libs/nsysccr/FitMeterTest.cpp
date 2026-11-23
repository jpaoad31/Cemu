// Standalone tests of the virtual Fit Meter: frames/CRC/XOR, the GamePad IR link, the IRCU slave + SAMU
// (registration, sync, persistence, initialize) driven by a model of Wii Fit U's IRC/IRCU master, and the
// log-stream encoder. Not in CMake (Cemu has no test target). Build and run:
//
//   P=build/vcpkg_installed/arm64-osx; D=src/Cafe/OS/libs/nsysccr
//   clang++ -std=c++20 -Wall -I $P/include $D/FitMeterTest.cpp $D/FitMeterProtocol.cpp $D/DrcIrLink.cpp \
//     $D/VirtualFitMeter.cpp $D/FitMeterActivity.cpp $D/FitMeterDemo.cpp -o build/fitmeter_test && build/fitmeter_test
//
// Dump mode (for tools/fitmeter/decoder_harness.py, which runs the game's own decoder on the streams):
//   build/fitmeter_test --dump activity.json|demo 2026-11-22T14:30 outdir/ [--night-owl] [--since 2026-11-20T08:00] [--minute-bytes N]
// writes outdir/b2.bin .. b6.bin: what the meter sends for banks 02..06 (XOR undone), through the whole link,
// and outdir/activity_used.json: the source's data the streams were made from.

#include "DrcIrLink.h"
#include "FitMeterActivity.h"
#include "FitMeterDemo.h"
#include "FitMeterProtocol.h"
#include "VirtualFitMeter.h"

#include <algorithm>
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
			"segments": [ { "start": "16:00", "minutes": 10, "mets": 4.0, "tag": 3, "climb_m": 20 } ] },
			{ "date": "2026-11-21", "steps": 10000 } ] })";
	}
	CHECK(meter->State().source == "demo"); // the default
	// the meter's memory starts at its registration (the ident write at meter time 15:45)
	CHECK_EQ(meter->State().historyStartMinute, (int64_t)DaysFromCivil(2026, 11, 22) * 1440 + 15 * 60 + 45);
	meter->State().source = "file";
	meter->UseSourceFromState();
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
	CHECK_EQ((b6[2] << 8) | b6[3], 0xFFFF); // 11-21: before the meter was registered (11-22 15:45)
	CHECK_EQ((b5[0] << 8) | b5[1], 99);
	// METs: 16:45 back to 16:10 = 36 minutes of 0, then 10 x 40 (16:09..16:00); nothing before 15:45 (registration)
	CHECK(std::vector<uint8_t>(b2.begin(), b2.begin() + 12) == std::vector<uint8_t>({36, 0, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40}));
	CHECK_EQ(b2[12], 0xFF);
	CHECK(std::vector<uint8_t>(b3.begin(), b3.begin() + 12) == std::vector<uint8_t>({36, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2}));
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


static int32_t DayWithTemplate(int t)
{
	int32_t d = DaysFromCivil(2026, 11, 1);
	while (DemoTemplateIndex(d) != t)
		d++;
	return d;
}

static MeterTime TimeOf(int32_t day, int h, int m)
{
	MeterTime t;
	CivilFromDays(day, t.year, t.month, t.day);
	t.hour = h;
	t.minute = m;
	return t;
}

static void TestDemo()
{
	// consecutive dates are consecutive template days, across the 28-day wrap; template 0 is a Monday
	const int32_t start = DaysFromCivil(2026, 10, 1);
	for (int32_t d = start; d < start + 70; d++)
		CHECK_EQ(DemoTemplateIndex(d + 1), (DemoTemplateIndex(d) + 1) % DEMO_CYCLE_DAYS);
	CHECK_EQ(DemoTemplateIndex(DaysFromCivil(2024, 1, 1)), 0);
	CHECK_EQ(DemoTemplateIndex(DaysFromCivil(2026, 11, 23)) % 7, 0); // 2026-11-23 is a Monday

	DemoMeterSource src;
	std::string err;
	auto ctxOf = [](const MeterTime& now, int dayStart) {
		SourceContext c;
		c.now = now;
		c.dayStartHour = dayStart;
		return c;
	};
	auto steps = [&](const MeterTime& now, int dayStart, int32_t day) {
		Activity a;
		src.GetActivity(ctxOf(now, dayStart), a, err);
		auto it = a.days.find(day);
		return it == a.days.end() ? -2 : it->second.steps;
	};
	// the shape: weekdays, a hike, a lazy day, a run day
	int32_t mon = DayWithTemplate(0);
	for (int t = 0; t < DEMO_CYCLE_DAYS; t++)
	{
		const int32_t d = mon + t;
		const int st = steps(TimeOf(d + 1, 12, 0), 0, d);
		if (t % 7 < 5)
			CHECK(st >= 5000 && st <= 14000);
		std::printf("  demo template %2d (%s): %5d steps\n", t, FormatDate(d).c_str(), st);
	}
	CHECK(steps(TimeOf(mon + 13, 12, 0), 0, mon + 12) > 15000); // week 2 Saturday: the hike
	CHECK(steps(TimeOf(mon + 21, 12, 0), 0, mon + 20) < 3000);  // week 3 Sunday: lazy
	// deterministic and lined up: yesterday at a sync tomorrow = today complete at 23:59
	const int32_t d = DayWithTemplate(27); // the wrap: template 27 -> 0
	CHECK_EQ(steps(TimeOf(d, 23, 59), 0, d), steps(TimeOf(d + 1, 10, 0), 0, d));
	CHECK_EQ(steps(TimeOf(d + 1, 10, 0), 0, d), steps(TimeOf(d + 1, 10, 0), 0, d));
	Activity a1, a2;
	src.GetActivity(ctxOf(TimeOf(d + 1, 10, 0), 0), a1, err);
	src.GetActivity(ctxOf(TimeOf(d + 5, 9, 30), 0), a2, err);
	CHECK(a1.days.at(d).mets.size() == 1440 && a2.days.at(d).mets.size() == 1440);
	bool same = true;
	for (int m = 0; m < 1440; m++)
		same = same && a1.days.at(d).mets[m] == a2.days.at(d).mets[m] && a1.days.at(d).tag[m] == a2.days.at(d).tag[m];
	CHECK(same);
	// partial today: grows during the day; minutes after now have no value
	const int32_t wd = DayWithTemplate(2);
	const int s0730 = steps(TimeOf(wd, 7, 30), 0, wd), s0800 = steps(TimeOf(wd, 8, 0), 0, wd), s2359 = steps(TimeOf(wd, 23, 59), 0, wd);
	CHECK(s0730 < s0800 && s0800 < s2359);
	Activity p;
	src.GetActivity(ctxOf(TimeOf(wd, 8, 0), 0), p, err);
	CHECK(!std::isnan(p.days.at(wd).mets[480]) && std::isnan(p.days.at(wd).mets[481]));
	CHECK(p.days.find(wd + 1) == p.days.end());
	// night owl: a Saturday with a walk home at 00:25 (template 5): its 00:00-02:59 steps count for Friday
	const int32_t sat = DayWithTemplate(5), fri = sat - 1;
	int early = 0, earlyFri = 0;
	for (int m = 0; m < 180; m++)
	{
		early += DemoTemplateDay(5).at(m).steps;
		earlyFri += DemoTemplateDay(4).at(m).steps;
	}
	CHECK(early > 3000);
	const MeterTime later = TimeOf(sat + 1, 12, 0);
	CHECK_EQ(steps(later, 3, fri), steps(later, 0, fri) + early - earlyFri);
	CHECK_EQ(steps(later, 3, sat), steps(later, 0, sat) - early + DemoTemplateDay(6).at(30).steps * 0); // Sunday 00:00-03:00 empty in week 1
	// at 01:30 on Saturday with night owl, the per-day stream's "today" is Friday, including the walk so far
	Activity n;
	const MeterTime t0130 = TimeOf(sat, 1, 30);
	src.GetActivity(ctxOf(t0130, 3), n, err);
	auto day = EncodeDayStream(n, t0130, 3, false, 120);
	int friSoFar = 0;
	for (int m = 180; m < 1440; m++)
		friSoFar += DemoTemplateDay(4).at(m).steps;
	for (int m = 0; m <= 90; m++)
		friSoFar += DemoTemplateDay(5).at(m).steps;
	CHECK_EQ((day[0] << 8) | day[1], friSoFar);
	CHECK_EQ(n.days.at(sat).steps, -1); // the calendar day only carries minutes
	// kcal follows the METs
	Activity k;
	src.GetActivity(ctxOf(TimeOf(mon + 2, 12, 0), 0), k, err);
	CHECK(k.days.at(mon + 1).kcal > 150 && k.days.at(mon + 1).kcal < 900);
	// the steps-today override (any source) lives in the meter
	const fs::path dir = fs::temp_directory_path() / "fitmeter_demo_test";
	fs::remove_all(dir);
	int64_t wall = 0;
	{
		std::tm tm{};
		tm.tm_year = 2026 - 1900; tm.tm_mon = 10; tm.tm_mday = 23; tm.tm_hour = 9; tm.tm_min = 0; tm.tm_isdst = -1;
		wall = (int64_t)std::mktime(&tm);
	}
	VirtualFitMeter vm(dir.string());
	vm.SetWallClock([&] { return wall; });
	CHECK(vm.GetDataSource()->Describe() == "Built-in demo (4-week cycle)");
	Activity o;
	vm.GetActivity(o, err);
	CHECK(o.days.at(vm.LogicalToday()).steps > 0);
	vm.State().stepsOverrideDay = vm.LogicalToday();
	vm.State().stepsOverride = 12345;
	vm.GetActivity(o, err);
	CHECK_EQ(o.days.at(vm.LogicalToday()).steps, 12345);
	wall += 86400; // tomorrow the override no longer applies
	vm.GetActivity(o, err);
	CHECK(o.days.at(vm.LogicalToday() - 1).steps != 12345);
}


// a meter only has data from after its registration, and keeps 7 days of minutes
static void TestHistory()
{
	const fs::path dir = fs::temp_directory_path() / "fitmeter_history_test";
	fs::remove_all(dir);
	int64_t wall = LocalEpoch(2026, 11, 23, 0, 48);
	VirtualFitMeter vm(dir.string());
	vm.SetWallClock([&] { return wall; });
	std::string err;
	Activity a;
	// unregistered: the demo has its whole range (the UI shows it); registered at 00:48: only since then
	vm.GetActivity(a, err);
	CHECK(a.days.size() > 40);
	vm.HandleWrite(SAMU_WRITE_IDENT, 0, {1, 2, 3, 4, 5, 6, 7, 8, 0xFF});
	CHECK(vm.State().registered);
	const int32_t today = DaysFromCivil(2026, 11, 23);
	auto daySum = [](const std::vector<uint8_t>& b) {
		int n = 0, sum = 0;
		for (size_t i = 0; i + 1 < b.size(); i += 2)
		{
			const int v = (b[i] << 8) | b[i + 1];
			if (v == 0xFFFF)
				break;
			n++;
			sum += v;
		}
		return std::make_pair(n, sum);
	};
	auto steps = *vm.HandleRead(SAMU_READ_STEPS, 120);
	auto [n0, s0] = daySum(steps);
	CHECK_EQ(n0, 1);  // only today
	CHECK_EQ(s0, 0);  // nothing walked since 00:48 yet
	CHECK(vm.HandleRead(SAMU_READ_METS, 10080)->at(0) == 1); // "1 00": the current minute only
	// a minute later: still only the minutes since registration
	wall += 60;
	auto m2 = *vm.HandleRead(SAMU_READ_METS, 10080);
	CHECK(m2[0] == 2 && m2[1] == 0 && m2[2] == 0xFF);
	// the next day at noon: two days, today's partial, yesterday's from 00:48
	wall = LocalEpoch(2026, 11, 24, 12, 0);
	steps = *vm.HandleRead(SAMU_READ_STEPS, 120);
	auto [n1, s1] = daySum(steps);
	CHECK_EQ(n1, 2);
	int fromReg = 0;
	for (int m = 48; m < 1440; m++)
		fromReg += DemoTemplateDay(DemoTemplateIndex(today)).at(m).steps;
	CHECK_EQ((steps[2] << 8) | steps[3], fromReg);
	// two weeks later: daily totals since registration (15 days), minutes for 7 days at most
	wall = LocalEpoch(2026, 12, 7, 12, 0);
	auto [n2, s2] = daySum(*vm.HandleRead(SAMU_READ_STEPS, 120));
	CHECK_EQ(n2, 15);
	auto mets = *vm.HandleRead(SAMU_READ_METS, 10080);
	int covered = 0;
	for (size_t i = 0; i < mets.size() && mets[i] != 0xFF;)
	{
		if (i + 1 < mets.size() && mets[i + 1] == 0) { covered += mets[i]; i += 2; }
		else { covered++; i++; }
	}
	CHECK_EQ(covered, 7 * 1440);
	// "clear meter history": nothing before now, the registration stays
	vm.ClearHistory();
	auto [n3, s3] = daySum(*vm.HandleRead(SAMU_READ_STEPS, 120));
	CHECK_EQ(n3, 1);
	CHECK_EQ(s3, 0);
	CHECK(vm.State().registered);
	// it persists
	VirtualFitMeter again(dir.string());
	CHECK_EQ(again.State().historyStartMinute, vm.State().historyStartMinute);
	// the test option: the data starts N days before the history start; the registration stays
	vm.State().pretendDaysAgo = 7;
	vm.State().clockSet = true;
	vm.State().reportClockUnsetOnce = true; // as FitMeterHost::SetPretendDaysAgo does
	CHECK_EQ(vm.HandleRead(SAMU_READ_TIME, 0)->at(0), 0xFF);
	vm.HandleWrite(SAMU_WRITE_TIME, 0, {0, 26, 12, 7, 12, 0});
	CHECK_EQ(vm.HandleRead(SAMU_READ_TIME, 0)->at(0), 0x00);
	CHECK_EQ(vm.DataStartMinute(), vm.State().historyStartMinute - 7 * 1440);
	auto [n4, s4] = daySum(*vm.HandleRead(SAMU_READ_STEPS, 120));
	CHECK_EQ(n4, 8); // 7 days + today
	CHECK(s4 > 30000);
	CHECK(vm.State().registered);
	VirtualFitMeter reread(dir.string());
	CHECK_EQ(reread.State().pretendDaysAgo, 7);
	vm.State().pretendDaysAgo = 0;
	vm.SaveState();
	// a meter.json from before history_start: the registration time is the start
	{
		std::ofstream f(dir / "meter.json");
		f << R"({ "registered": true, "registered_at": "2026-11-23 00:48:03", "clock_offset_s": 0, "save_data_id": 1, "samu_id": 2 })";
	}
	VirtualFitMeter old(dir.string());
	CHECK_EQ(old.State().historyStartMinute, (int64_t)today * 1440 + 48);
}

static int Dump(const char* activityPath, const char* when, const char* outDir, bool nightOwl, const char* since, size_t minuteBytes)
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
	if (std::strcmp(activityPath, "demo") != 0)
		meter.SetDataSource(std::make_shared<JsonFileMeterSource>(activityPath));
	if (nightOwl)
		meter.State().personal = {40, 1, 44, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 200};
	meter.State().registered = true;
	meter.State().clockSet = true;
	if (since)
	{
		int sy, smo, sd, sh, smi;
		if (std::sscanf(since, "%d-%d-%dT%d:%d", &sy, &smo, &sd, &sh, &smi) == 5)
			meter.State().historyStartMinute = (int64_t)DaysFromCivil(sy, smo, sd) * 1440 + sh * 60 + smi;
	}
	meter.State().autoPressDelayMs = 0;
	{
		// what the source gave, as activity.json (the verifier's reference)
		Activity act;
		std::string err;
		meter.GetActivity(act, err);
		std::ofstream f(fs::path(outDir) / "activity_used.json");
		f << ActivityToJson(act);
	}
	DrcIrLink link;
	link.SetPeer(&meter);
	Console c{link, now};
	if (!c.ConnectRetry())
	{
		std::printf("connect failed\n");
		return 1;
	}
	// the game asks banks 02-04 for min(10080, minutes since the last transfer + 10) bytes
	const struct { uint8_t bank; size_t size; } banks[] = {{2, minuteBytes}, {3, minuteBytes}, {4, minuteBytes}, {5, 120}, {6, 120}};
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
	if (argc >= 5 && std::strcmp(argv[1], "--dump") == 0)
	{
		bool nightOwl = false;
		const char* since = nullptr;
		size_t minuteBytes = 10080;
		for (int i = 5; i < argc; i++)
		{
			if (std::strcmp(argv[i], "--minute-bytes") == 0 && i + 1 < argc)
			{
				minuteBytes = (size_t)std::clamp(std::atoi(argv[++i]), 1, 10080);
				continue;
			}
			if (std::strcmp(argv[i], "--night-owl") == 0)
				nightOwl = true;
			else if (std::strcmp(argv[i], "--since") == 0 && i + 1 < argc)
				since = argv[++i];
		}
		return Dump(argv[2], argv[3], argv[4], nightOwl, since, minuteBytes);
	}
	setenv("TZ", "UTC", 1);
	tzset();
	TestFrames();
	TestEncoder();
	TestDemo();
	TestHistory();
	TestLinkAndMeter(fs::temp_directory_path() / "fitmeter_test");
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
