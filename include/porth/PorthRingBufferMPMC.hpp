#include <iostream>
#include <atomic>
#include <cstdint>
#include <vector>

template <typename T, size_t SIZE>
class MPMC {
    static_assert((SIZE & (SIZE - 1)) == 0, "SIZE must be to a power of two.");

private:
    struct Cell {
        std::atomic<size_t> sequence;
        T data;
    };

    Cell* const buffer;
    alignas(64) std::atomic<size_t> head{0};
    alignas(64) std::atomic<size_t> tail{0};

public:
    MPMC() : buffer(new Cell[SIZE]) {
        for (size_t i = 0; i < SIZE; ++i) {
            buffer[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    ~MPMC() { delete[] buffer; }

    bool push(const T& item) {
        size_t pos = head.load(std::memory_order_relaxed);

        while (true) {
            Cell& cell = buffer[pos & (SIZE - 1)];
            size_t seq = cell.sequence.load(std::memory_order_acquire);
            int diff = (int)seq - (int)pos;

            if (diff == 0) {
                 if (head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    cell.data = item;
                    cell.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                 }

            } else if (diff < 0) {
                return false;
            } else {
                pos = head.load(std::memory_order_relaxed);
            }
        }
    }

    bool pop(T& item) {
        size_t pos = tail.load(std::memory_order_relaxed);

        while (true) {
            Cell& cell = buffer[pos & (SIZE - 1)];
            size_t seq = cell.sequence.load(std::memory_order_acquire);
            int diff = (int)seq - (int)(pos + 1);

            if (diff == 0) {
                if (tail.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    item = cell.data;
                    cell.sequence.store(pos + SIZE, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = tail.load(std::memory_order_relaxed);
            }
        }
    }

    MPMC(const MPMC&) = delete;
    MPMC& operator=(const MPMC&) = delete;
    MPMC(MPMC&&) = delete;
    MPMC& operator=(MPMC&&) = delete;
};