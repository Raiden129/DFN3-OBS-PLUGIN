#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

class SampleRingBuffer {
public:
    SampleRingBuffer() = default;
    explicit SampleRingBuffer(size_t capacity_samples)
    {
        Reset(capacity_samples);
    }

    void Reset(size_t capacity_samples)
    {
        data_.assign(capacity_samples, 0.0f);
        head_ = 0;
        tail_ = 0;
        size_ = 0;
    }

    void Clear()
    {
        head_ = 0;
        tail_ = 0;
        size_ = 0;
    }

    [[nodiscard]] size_t Size() const
    {
        return size_;
    }

    [[nodiscard]] size_t Capacity() const
    {
        return data_.size();
    }

    [[nodiscard]] size_t AvailableWrite() const
    {
        return Capacity() - size_;
    }

    [[nodiscard]] bool PushBack(const float *src, size_t count)
    {
        if (Capacity() == 0) {
            return false;
        }
        if (!src || count == 0) {
            return true;
        }
        if (count > AvailableWrite()) {
            return false;
        }

        const size_t first = std::min(count, Capacity() - tail_);
        std::copy(src, src + first, data_.begin() + static_cast<std::ptrdiff_t>(tail_));

        const size_t remaining = count - first;
        if (remaining > 0) {
            std::copy(src + first, src + count, data_.begin());
        }

        tail_ = (tail_ + count) % Capacity();
        size_ += count;
        return true;
    }

    [[nodiscard]] bool PopFront(float *dst, size_t count)
    {
        if (Capacity() == 0) {
            return false;
        }
        if (count > size_) {
            return false;
        }

        if (count == 0) {
            return true;
        }

        const size_t first = std::min(count, Capacity() - head_);
        if (dst) {
            std::copy(data_.begin() + static_cast<std::ptrdiff_t>(head_),
                      data_.begin() + static_cast<std::ptrdiff_t>(head_ + first),
                      dst);
        }

        const size_t remaining = count - first;
        if (remaining > 0 && dst) {
            std::copy(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(remaining), dst + first);
        }

        head_ = (head_ + count) % Capacity();
        size_ -= count;
        return true;
    }

private:
    std::vector<float> data_;
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t size_ = 0;
};
