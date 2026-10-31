// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fixed-capacity FIFO of interleaved audio samples between the audio decoder
// thread and the audio sink callback. Not internally synchronised: the owner
// (FFmpegAudioDecoder) guards it with its lock, which also makes flushes on
// seek trivially safe. Engine-independent for host unit tests.

#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace WebCore {
namespace FFmpegMedia {

template<typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : m_buffer(std::max<size_t>(capacity, 1))
    {
    }

    size_t capacity() const { return m_buffer.size(); }
    size_t size() const { return m_size; }
    size_t freeSpace() const { return capacity() - m_size; }
    bool isEmpty() const { return !m_size; }

    // Writes up to `count` items, returns how many fit.
    size_t write(const T* data, size_t count)
    {
        count = std::min(count, freeSpace());
        size_t writePos = (m_readPos + m_size) % capacity();
        size_t first = std::min(count, capacity() - writePos);
        std::copy(data, data + first, m_buffer.begin() + writePos);
        std::copy(data + first, data + count, m_buffer.begin());
        m_size += count;
        return count;
    }

    // Appends `count` copies of `value` (silence), returns how many fit.
    size_t fill(const T& value, size_t count)
    {
        count = std::min(count, freeSpace());
        size_t writePos = (m_readPos + m_size) % capacity();
        for (size_t i = 0; i < count; ++i)
            m_buffer[(writePos + i) % capacity()] = value;
        m_size += count;
        return count;
    }

    // Reads up to `count` items, returns how many were read.
    size_t read(T* out, size_t count)
    {
        count = std::min(count, m_size);
        size_t first = std::min(count, capacity() - m_readPos);
        std::copy(m_buffer.begin() + m_readPos, m_buffer.begin() + m_readPos + first, out);
        std::copy(m_buffer.begin(), m_buffer.begin() + (count - first), out + first);
        m_readPos = (m_readPos + count) % capacity();
        m_size -= count;
        return count;
    }

    // Drops up to `count` items without copying them out.
    size_t skip(size_t count)
    {
        count = std::min(count, m_size);
        m_readPos = (m_readPos + count) % capacity();
        m_size -= count;
        return count;
    }

    void clear()
    {
        m_readPos = 0;
        m_size = 0;
    }

private:
    std::vector<T> m_buffer;
    size_t m_readPos { 0 };
    size_t m_size { 0 };
};

} // namespace FFmpegMedia
} // namespace WebCore
