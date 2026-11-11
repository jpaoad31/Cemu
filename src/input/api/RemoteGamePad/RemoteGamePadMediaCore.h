#pragma once

// Platform-independent parts of the Remote GamePad media stream (protocol 1.2): cutting an
// encoded frame into fragments, putting fragments back together ("newest complete frame wins"),
// the video bitrate controller and the microphone jitter buffer. No Cemu dependencies, so the
// standalone test (RemoteGamePadProtocolTest.cpp) covers them.

#include "input/api/RemoteGamePad/RemoteGamePadProtocol.h"

#include <map>

namespace RemoteGamePad
{
	// Offsets of the slice NAL units (H.264 types 1 and 5) in an AVCC access unit
	// (4-byte big-endian length before each NAL unit). Stops at the first malformed length.
	std::vector<size_t> AvccSliceOffsets(const uint8_t* frame, size_t size);

	// Cuts one encoded frame into fragment packets. `tmpl` gives the per-frame fields (frame id,
	// flags, timestamp, size, bitrate...); index/count/offset/first_slice/data are filled here.
	// seq is advanced by one per packet.
	std::vector<std::vector<uint8_t>> Packetize(MsgType type, uint32_t& seq, const MediaFragment& tmpl,
		const uint8_t* frame, size_t size, size_t max_data = kMaxFragmentData);

	// Puts fragments back together. Only frames newer than the newest completed one are kept;
	// a frame completing drops every older incomplete one. A gap (a frame that never completed)
	// makes the following frames undecodable until the next keyframe: they are dropped and
	// NeedKeyframe() is true, so the receiver can ask for one.
	class FrameAssembler
	{
	public:
		struct Frame
		{
			uint32_t id = 0;
			uint8_t codec = 0, flags = 0;
			uint16_t width = 0, height = 0;
			uint64_t timestamp_us = 0;
			uint16_t pipeline_100us = 0;
			uint16_t bitrate_10kbps = 0;
			uint64_t first_arrival_us = 0; // receiver clock: first fragment
			uint64_t complete_us = 0;      // receiver clock: last fragment
			std::vector<uint8_t> data;
			bool keyframe() const { return flags & kFragKeyframe; }
		};

		struct Stats
		{
			uint32_t fragments = 0;
			uint32_t fragments_missing = 0;
			uint32_t frames_complete = 0; // returned by Add
			uint32_t frames_dropped = 0;  // incomplete, late, or after a gap before a keyframe
		};

		static constexpr size_t kMaxPending = 4;

		// returns the frame this fragment completed, if it is decodable
		std::optional<Frame> Add(const MediaFragment& f, uint64_t now_us);
		bool NeedKeyframe() const { return m_need_keyframe; }
		Stats TakeStats()
		{
			Stats s = m_stats;
			m_stats = {};
			return s;
		}
		void Reset()
		{
			m_pending.clear();
			m_have_last = false;
			m_need_keyframe = true;
		}

	private:
		struct Pending
		{
			Frame frame;
			std::vector<uint8_t> have;
			uint16_t count = 0;
			uint16_t received = 0;
		};
		void drop(std::map<uint32_t, Pending>::iterator it);

		std::map<uint32_t, Pending> m_pending; // by frame id (wrap is handled by evicting old ids)
		bool m_have_last = false;
		uint32_t m_last_complete = 0;
		bool m_need_keyframe = true; // until the first keyframe
		Stats m_stats;
	};

	// Video bitrate from the phone's feedback: multiplicative decrease on loss or slow frame
	// delivery, slow increase after a clean second.
	class BitrateController
	{
	public:
		static constexpr uint32_t kMinKbps = 1000;
		static constexpr uint32_t kDefaultMaxKbps = 16000;
		static constexpr uint32_t kStartKbps = 6000;

		uint32_t kbps() const { return m_kbps; }
		uint32_t max_kbps() const { return m_max; }
		void SetMax(uint32_t max_kbps)
		{
			m_max = max_kbps ? std::max(max_kbps, kMinKbps) : kDefaultMaxKbps;
			m_kbps = std::min(m_kbps, m_max);
		}
		// one feedback report; returns true if the bitrate changed
		bool OnFeedback(const VideoFeedback& f, uint64_t now_us);
		void Reset() { m_kbps = std::min(kStartKbps, m_max); m_last_decrease_us = 0; m_clean_since_us = 0; }

	private:
		uint32_t m_kbps = kStartKbps;
		uint32_t m_max = kDefaultMaxKbps;
		uint64_t m_last_decrease_us = 0;
		uint64_t m_clean_since_us = 0;
	};

	// Microphone samples from the network, read in small blocks by the emulator (mic.rpl reads
	// 96 samples per 3 ms audio frame). Holds about kTarget of audio: waits for that much before
	// playing (and again after running dry), and drops the oldest samples above kMax.
	class MicJitterBuffer
	{
	public:
		explicit MicJitterBuffer(uint32_t sample_rate = 32000)
			: m_target(sample_rate * 40 / 1000), m_max(sample_rate * 150 / 1000), m_buffer(sample_rate) {}

		void Push(const int16_t* samples, size_t count);
		// fills dst with count samples; returns how many came from the network (the rest are silence)
		size_t Pop(int16_t* dst, size_t count);
		size_t buffered() const { return m_size; }
		uint32_t underruns() const { return m_underruns; }
		uint32_t overflow_drops() const { return m_dropped; }
		void Clear() { m_size = 0; m_read = 0; m_playing = false; }

	private:
		size_t m_target, m_max;
		std::vector<int16_t> m_buffer; // ring
		size_t m_read = 0, m_size = 0;
		bool m_playing = false;
		uint32_t m_underruns = 0, m_dropped = 0;
	};
}
