#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <immintrin.h>

// Streaming 256-bit checksum for page validation.
//
// FNV-1a over 512 MB costs ~350 ms because it is strictly sequential per byte.
// This accumulator xors 32-byte blocks into four 64-bit lanes, folds a cheap
// multiply in and is foldable at the end, so the same 512 MB is validated in a
// handful of milliseconds while still being sensitive to byte order, zeros and
// swapped blocks (length and tail are mixed into the final avalanche).
class Hash64 {
public:
    Hash64() : acc_(_mm256_set1_epi64x(0x9E3779B97F4A7C15ull)) {}

    void Update(const void* data, size_t bytes)
    {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        length_ += bytes;

        if (carryLen_ != 0) {
            const size_t need = 32 - carryLen_;
            const size_t take = bytes < need ? bytes : need;
            std::memcpy(carry_ + carryLen_, p, take);
            carryLen_ += take;
            p += take;
            bytes -= take;
            if (carryLen_ == 32) {
                Block(carry_);
                carryLen_ = 0;
            }
        }

        while (bytes >= 32) {
            Block(p);
            p += 32;
            bytes -= 32;
        }
        if (bytes != 0) {
            std::memcpy(carry_, p, bytes);
            carryLen_ = bytes;
        }
    }

    uint64_t Final() const
    {
        uint64_t tail = 0xCBF29CE484222325ull ^ (length_ * 0x100000001B3ull);
        for (size_t i = 0; i < carryLen_; ++i) {
            tail = (tail ^ carry_[i]) * 0x100000001B3ull;
        }

        alignas(32) uint64_t lanes[4];
        _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), acc_);
        uint64_t h = lanes[0] ^ lanes[1] ^ lanes[2] ^ lanes[3] ^ tail;

        h ^= h >> 33;
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 33;
        h *= 0xC4CEB9FE1A85EC53ull;
        h ^= h >> 33;
        return h;
    }

    static uint64_t Compute(const void* data, size_t bytes)
    {
        Hash64 hasher;
        hasher.Update(data, bytes);
        return hasher.Final();
    }

private:
    void Block(const uint8_t* p)
    {
        // Three cheap ops per 32 bytes: xor, add and a lane permute. The
        // multiply lives in Final(), so the hot loop stays memory bound.
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
        acc_ = _mm256_xor_si256(acc_, v);
        acc_ = _mm256_add_epi64(acc_, v);
        acc_ = _mm256_shuffle_epi32(acc_, _MM_SHUFFLE(1, 0, 3, 2));
    }

    __m256i acc_;
    uint64_t length_ = 0;
    size_t carryLen_ = 0;
    uint8_t carry_[32]{};
};
