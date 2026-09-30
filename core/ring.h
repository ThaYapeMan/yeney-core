// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#pragma once
#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <vector>
namespace yeney {
// Single-owner bounded FIFO. No sentinel byte; the advertised capacity is usable.
template <class T> class Ring {
    std::vector<T> data_;
    size_t head_ = 0, used_ = 0;

public:
    explicit Ring(size_t capacity) : data_(capacity) {
        if (!capacity)
            throw std::invalid_argument("zero ring capacity");
    }
    size_t size() const { return used_; }
    size_t capacity() const { return data_.size(); }
    size_t free() const { return capacity() - used_; }
    void clear() { head_ = used_ = 0; }
    size_t push(const T *p, size_t n) {
        n = std::min(n, free());
        for (size_t i = 0; i < n; ++i)
            data_[(head_ + used_ + i) % capacity()] = p[i];
        used_ += n;
        return n;
    }
    T at(size_t i) const {
        if (i >= used_)
            throw std::out_of_range("ring index");
        return data_[(head_ + i) % capacity()];
    }
    void discard(size_t n) {
        if (n > used_)
            throw std::out_of_range("ring discard");
        head_ = (head_ + n) % capacity();
        used_ -= n;
    }
    size_t pop(T *p, size_t n) {
        n = std::min(n, used_);
        for (size_t i = 0; i < n; ++i)
            p[i] = at(i);
        discard(n);
        return n;
    }
};
} // namespace yeney
