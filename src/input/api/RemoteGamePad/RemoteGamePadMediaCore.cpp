#include "input/api/RemoteGamePad/RemoteGamePadMediaCore.h"

#include <algorithm>
#include <cstring>

namespace RemoteGamePad
{
	std::vector<size_t> AvccSliceOffsets(const uint8_t* frame, size_t size)
	{
		std::vector<size_t> offsets;
		size_t off = 0;
		while (off + 5 <= size)
		{
			const size_t len = ((size_t)frame[off] << 24) | ((size_t)frame[off + 1] << 16) | ((size_t)frame[off + 2] << 8) | frame[off + 3];
			if (len == 0 || off + 4 + len > size)
				break;
			const uint8_t type = frame[off + 4] & 0x1F;
			if (type == 1 || type == 5)
				offsets.push_back(off);
			off += 4 + len;
		}
		return offsets;
	}

	std::vector<std::vector<uint8_t>> Packetize(MsgType type, uint32_t& seq, const MediaFragment& tmpl,
		const uint8_t* frame, size_t size, size_t max_data)
	{
		std::vector<std::vector<uint8_t>> packets;
		if (size == 0 || size > kMaxFrameSize || max_data == 0)
			return packets;
		max_data = std::min(max_data, kMaxPacketSize - kFragmentHeaderSize);
		const size_t count = (size + max_data - 1) / max_data;
		if (count > 0xFFFF)
			return packets;
		const auto slices = tmpl.codec == kCodecH264 ? AvccSliceOffsets(frame, size) : std::vector<size_t>{};
		size_t next_slice = 0;
		packets.reserve(count);
		for (size_t i = 0; i < count; ++i)
		{
			MediaFragment f = tmpl;
			f.index = (uint16_t)i;
			f.count = (uint16_t)count;
			f.frame_size = (uint32_t)size;
			f.offset = (uint32_t)(i * max_data);
			f.data = frame + f.offset;
			f.data_size = std::min(max_data, size - f.offset);
			// first slice starting inside this fragment
			while (next_slice < slices.size() && slices[next_slice] < f.offset)
				++next_slice;
			f.first_slice = (next_slice < slices.size() && slices[next_slice] < f.offset + f.data_size) ? (uint16_t)std::min<size_t>(next_slice, 0xFFFE) : 0xFFFF;
			packets.push_back(BuildFragment(type, seq++, f));
		}
		return packets;
	}

	void FrameAssembler::drop(std::map<uint32_t, Pending>::iterator it)
	{
		m_stats.frames_dropped++;
		m_stats.fragments_missing += it->second.count - it->second.received;
		m_pending.erase(it);
	}

	std::optional<FrameAssembler::Frame> FrameAssembler::Add(const MediaFragment& f, uint64_t now_us)
	{
		m_stats.fragments++;
		// late: a newer frame has already completed
		if (m_have_last && !SeqNewer(f.frame_id, m_last_complete))
			return std::nullopt;
		auto it = m_pending.find(f.frame_id);
		if (it == m_pending.end())
		{
			// make room: the oldest incomplete frame goes
			while (m_pending.size() >= kMaxPending)
			{
				auto oldest = m_pending.begin();
				for (auto i = m_pending.begin(); i != m_pending.end(); ++i)
					if (SeqNewer(oldest->first, i->first))
						oldest = i;
				// a fragment older than everything pending: drop it instead
				if (SeqNewer(oldest->first, f.frame_id))
					return std::nullopt;
				drop(oldest);
			}
			Pending p;
			p.frame.id = f.frame_id;
			p.frame.codec = f.codec;
			p.frame.flags = f.flags;
			p.frame.width = f.width;
			p.frame.height = f.height;
			p.frame.timestamp_us = f.timestamp_us;
			p.frame.pipeline_100us = f.pipeline_100us;
			p.frame.bitrate_10kbps = f.bitrate_10kbps;
			p.frame.first_arrival_us = now_us;
			p.frame.data.resize(f.frame_size);
			p.have.assign(f.count, 0);
			p.count = f.count;
			it = m_pending.emplace(f.frame_id, std::move(p)).first;
		}
		Pending& p = it->second;
		// inconsistent with the first fragment of this frame: ignore
		if (f.count != p.count || f.frame_size != p.frame.data.size() || f.index >= p.count || (uint64_t)f.offset + f.data_size > p.frame.data.size())
			return std::nullopt;
		if (p.have[f.index])
			return std::nullopt; // duplicate
		p.have[f.index] = 1;
		p.received++;
		if (f.data_size)
			std::memcpy(p.frame.data.data() + f.offset, f.data, f.data_size);
		if (f.pipeline_100us)
			p.frame.pipeline_100us = f.pipeline_100us;
		if (p.received < p.count)
			return std::nullopt;

		// complete: everything older is now late
		Frame frame = std::move(p.frame);
		frame.complete_us = now_us;
		m_pending.erase(it);
		uint32_t dropped_between = 0; // incomplete frames between the last complete one and this
		for (auto i = m_pending.begin(); i != m_pending.end();)
		{
			if (SeqNewer(frame.id, i->first))
			{
				if (!m_have_last || SeqNewer(i->first, m_last_complete))
					dropped_between++;
				m_stats.frames_dropped++;
				m_stats.fragments_missing += i->second.count - i->second.received;
				i = m_pending.erase(i);
			}
			else
				++i;
		}
		if (m_have_last)
		{
			// frames that never showed up at all (every fragment lost)
			const uint32_t gap = frame.id - m_last_complete - 1;
			if (gap > 0)
			{
				m_stats.frames_dropped += std::min<uint32_t>(gap - std::min(gap, dropped_between), 1000);
				m_need_keyframe = true;
			}
		}
		m_have_last = true;
		m_last_complete = frame.id;
		if (frame.keyframe())
			m_need_keyframe = false;
		if (m_need_keyframe)
		{
			m_stats.frames_dropped++; // references a frame we don't have
			return std::nullopt;
		}
		m_stats.frames_complete++;
		return frame;
	}

	bool BitrateController::OnFeedback(const VideoFeedback& f, uint64_t now_us)
	{
		const uint32_t before = m_kbps;
		const uint32_t total = (uint32_t)f.fragments_received + f.fragments_missing;
		const float loss = total ? (float)f.fragments_missing / (float)total : 0.0f;
		const bool slow = f.assembly_100us > 80; // frames take > 8 ms to arrive: queueing on the link
		const bool decrease_allowed = now_us - m_last_decrease_us > 300000; // let a cut take effect first
		if ((loss > 0.05f || f.frames_dropped > 3) && decrease_allowed)
		{
			m_kbps = (uint32_t)(m_kbps * 0.7f);
			m_last_decrease_us = now_us;
			m_clean_since_us = 0;
		}
		else if ((loss > 0.01f || f.frames_dropped > 0 || slow) && decrease_allowed)
		{
			m_kbps = (uint32_t)(m_kbps * 0.9f);
			m_last_decrease_us = now_us;
			m_clean_since_us = 0;
		}
		else if (loss == 0 && f.frames_dropped == 0 && !slow)
		{
			if (m_clean_since_us == 0)
				m_clean_since_us = now_us;
			else if (now_us - m_clean_since_us >= 1000000)
			{
				m_kbps += std::max<uint32_t>(m_kbps / 20, 100);
				m_clean_since_us = now_us;
			}
		}
		else
			m_clean_since_us = 0;
		m_kbps = std::clamp(m_kbps, kMinKbps, m_max);
		return m_kbps != before;
	}

	void MicJitterBuffer::Push(const int16_t* samples, size_t count)
	{
		const size_t cap = m_buffer.size();
		if (count > cap)
		{
			samples += count - cap;
			count = cap;
		}
		for (size_t i = 0; i < count; ++i)
		{
			if (m_size == cap)
			{
				m_read = (m_read + 1) % cap;
				m_size--;
			}
			m_buffer[(m_read + m_size) % cap] = samples[i];
			m_size++;
		}
		// too much latency: skip the oldest down to the target
		if (m_size > m_max)
		{
			const size_t skip = m_size - m_target;
			m_read = (m_read + skip) % cap;
			m_size -= skip;
			m_dropped += (uint32_t)skip;
		}
	}

	size_t MicJitterBuffer::Pop(int16_t* dst, size_t count)
	{
		if (!m_playing && m_size >= m_target)
			m_playing = true;
		size_t n = 0;
		if (m_playing)
		{
			n = std::min(count, m_size);
			const size_t cap = m_buffer.size();
			for (size_t i = 0; i < n; ++i)
				dst[i] = m_buffer[(m_read + i) % cap];
			m_read = (m_read + n) % cap;
			m_size -= n;
			if (n < count)
			{
				m_playing = false; // ran dry: build up the target again
				m_underruns++;
			}
		}
		std::fill(dst + n, dst + count, (int16_t)0);
		return n;
	}
}
