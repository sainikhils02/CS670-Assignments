#pragma once

#include <array>
#include <cstdint>
#include <istream>
#include <ostream>
#include <random>
#include <stdexcept>
#include <vector>

#include <openssl/evp.h>

namespace dpf {

struct Seed256 {
    std::array<uint32_t, 8> k{};
};

inline Seed256 seed_xor(const Seed256& a, const Seed256& b) {
    Seed256 r;
    for (int i = 0; i < 8; ++i) r.k[i] = a.k[i] ^ b.k[i];
    return r;
}

inline void seed_xor_inplace(Seed256& a, const Seed256& b) {
    for (int i = 0; i < 8; ++i) a.k[i] ^= b.k[i];
}

inline void seed_to_key(const Seed256& seed, uint8_t key[16]) {
    for (int i = 0; i < 4; ++i) {
        uint32_t w = seed.k[i];
        key[4 * i + 0] = static_cast<uint8_t>(w & 0xFF);
        key[4 * i + 1] = static_cast<uint8_t>((w >> 8) & 0xFF);
        key[4 * i + 2] = static_cast<uint8_t>((w >> 16) & 0xFF);
        key[4 * i + 3] = static_cast<uint8_t>((w >> 24) & 0xFF);
    }
}

inline void make_nonce(const Seed256& seed, uint32_t domainConst, std::array<uint32_t, 3>& nonce) {
    nonce[0] = seed.k[4] ^ domainConst;
    nonce[1] = seed.k[5];
    nonce[2] = seed.k[6];
}

inline void aes_ctr_words(const uint8_t key[16], const std::array<uint32_t, 3>& nonce,
                          uint32_t counterStart, uint32_t numWords, uint32_t* out) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");

    uint8_t iv[16];
    uint32_t inw[4] = {nonce[0], nonce[1], nonce[2], counterStart};
    for (int i = 0; i < 4; ++i) {
        iv[4 * i + 0] = static_cast<uint8_t>(inw[i] & 0xFF);
        iv[4 * i + 1] = static_cast<uint8_t>((inw[i] >> 8) & 0xFF);
        iv[4 * i + 2] = static_cast<uint8_t>((inw[i] >> 16) & 0xFF);
        iv[4 * i + 3] = static_cast<uint8_t>((inw[i] >> 24) & 0xFF);
    }

    if (1 != EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr, key, iv)) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_EncryptInit_ex failed");
    }

    uint32_t produced = 0;
    uint8_t zeros[16] = {0};
    while (produced < numWords) {
        uint8_t outb[16];
        int outlen = 0;
        if (1 != EVP_EncryptUpdate(ctx, outb, &outlen, zeros, 16)) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("EVP_EncryptUpdate failed");
        }
        for (int i = 0; i < 4 && produced < numWords; ++i) {
            out[produced++] = static_cast<uint32_t>(outb[4 * i]) |
                               (static_cast<uint32_t>(outb[4 * i + 1]) << 8) |
                               (static_cast<uint32_t>(outb[4 * i + 2]) << 16) |
                               (static_cast<uint32_t>(outb[4 * i + 3]) << 24);
        }
    }
    EVP_CIPHER_CTX_free(ctx);
}

inline void clear_lsbs(Seed256& s) {
    s.k[0] &= ~0x3u;
}

inline void prg_expand(const Seed256& in, Seed256& sL, uint8_t& tL, Seed256& sR, uint8_t& tR) {
    Seed256 seed = in;
    clear_lsbs(seed);
    const uint32_t DOMAIN_SL = 0xC0015E5Au;
    const uint32_t DOMAIN_SR = 0xC0015255u;
    const uint32_t DOMAIN_T = 0xC0017455u;
    uint8_t key[16];
    seed_to_key(seed, key);
    std::array<uint32_t, 3> nonceSL, nonceSR, nonceT;
    make_nonce(seed, DOMAIN_SL, nonceSL);
    make_nonce(seed, DOMAIN_SR, nonceSR);
    make_nonce(seed, DOMAIN_T, nonceT);
    uint32_t wL[8], wR[8], wT[2];
    aes_ctr_words(key, nonceSL, 0u, 8u, wL);
    aes_ctr_words(key, nonceSR, 0u, 8u, wR);
    aes_ctr_words(key, nonceT, 0u, 2u, wT);
    for (int i = 0; i < 8; ++i) {
        sL.k[i] = wL[i];
        sR.k[i] = wR[i];
    }
    tL = static_cast<uint8_t>(wT[0] & 1u);
    tR = static_cast<uint8_t>(wT[1] & 1u);
    clear_lsbs(sL);
    clear_lsbs(sR);
}

inline uint64_t prg_leaf(const Seed256& seed) {
    const uint32_t DOMAIN_OUT = 0x0BADF00Du;
    uint8_t key[16];
    seed_to_key(seed, key);
    std::array<uint32_t, 3> nonce;
    make_nonce(seed, DOMAIN_OUT, nonce);
    uint32_t w[2];
    aes_ctr_words(key, nonce, 0u, 2u, w);
    return static_cast<uint64_t>(w[0]) | (static_cast<uint64_t>(w[1]) << 32);
}

struct DPFKey {
    Seed256 rootSeed;
    uint8_t rootT{0};
    std::vector<Seed256> cw_seed;
    std::vector<uint8_t> cw_tL;
    std::vector<uint8_t> cw_tR;
    uint64_t cw_out{0};
    size_t size{0};
    int depth{0};
};

struct DPFKeys {
    DPFKey k0;
    DPFKey k1;
};

inline bool is_power_of_two(uint64_t x) {
    return x && ((x & (x - 1)) == 0);
}

inline int ilog2_size(size_t n) {
    int d = 0;
    while ((size_t(1) << d) < n) ++d;
    return d;
}

inline uint8_t get_bit(size_t index, int depth, int level) {
    int shift = depth - 1 - level;
    return static_cast<uint8_t>((index >> shift) & 1ULL);
}

inline DPFKeys generate(size_t size, size_t location, uint64_t value, std::mt19937_64& rng) {
    if (!is_power_of_two(size)) throw std::invalid_argument("DPF domain must be power of two");
    if (location >= size) throw std::invalid_argument("location out of range");

    DPFKeys pair;
    const int depth = ilog2_size(size);

    auto init_seed = [&](Seed256& s) {
        for (int i = 0; i < 8; ++i) s.k[i] = static_cast<uint32_t>(rng() & 0xFFFFFFFFu);
    };

    Seed256 s0, s1;
    init_seed(s0);
    init_seed(s1);
    uint8_t t0 = static_cast<uint8_t>(s0.k[0] & 1u);
    uint8_t t1 = static_cast<uint8_t>(t0 ^ 1u);

    std::vector<Seed256> cw_seed(depth);
    std::vector<uint8_t> cw_tL(depth), cw_tR(depth);

    Seed256 cur0 = s0;
    Seed256 cur1 = s1;
    uint8_t tau0 = t0;
    uint8_t tau1 = t1;

    for (int level = 0; level < depth; ++level) {
        Seed256 s0L, s0R, s1L, s1R;
        uint8_t t0L, t0R, t1L, t1R;
        prg_expand(cur0, s0L, t0L, s0R, t0R);
        prg_expand(cur1, s1L, t1L, s1R, t1R);

        const uint8_t bit = get_bit(location, depth, level);
        const uint8_t keep = bit;
        const uint8_t lose = keep ^ 1u;

        const uint8_t cwtL = static_cast<uint8_t>(t0L ^ t1L ^ bit ^ 1U);
        const uint8_t cwtR = static_cast<uint8_t>(t0R ^ t1R ^ bit);
        cw_tL[level] = cwtL;
        cw_tR[level] = cwtR;

        const Seed256 corr = (lose == 0) ? seed_xor(s0L, s1L) : seed_xor(s0R, s1R);
        cw_seed[level] = corr;

        Seed256 child0 = keep == 0 ? s0L : s0R;
        uint8_t tchild0 = keep == 0 ? t0L : t0R;
        if (tau0 == 0) seed_xor_inplace(child0, corr);
        tau0 = static_cast<uint8_t>(tchild0 ^ (tau0 & (keep == 0 ? cwtL : cwtR)));
        cur0 = child0;

        Seed256 child1 = keep == 0 ? s1L : s1R;
        uint8_t tchild1 = keep == 0 ? t1L : t1R;
        if (tau1 == 0) seed_xor_inplace(child1, corr);
        tau1 = static_cast<uint8_t>(tchild1 ^ (tau1 & (keep == 0 ? cwtL : cwtR)));
        cur1 = child1;
    }

    const uint64_t cw_out = value ^ prg_leaf(cur0) ^ prg_leaf(cur1);

    pair.k0.rootSeed = s0;
    pair.k0.rootT = t0;
    pair.k0.cw_seed = cw_seed;
    pair.k0.cw_tL = cw_tL;
    pair.k0.cw_tR = cw_tR;
    pair.k0.cw_out = cw_out;
    pair.k0.size = size;
    pair.k0.depth = depth;

    pair.k1.rootSeed = s1;
    pair.k1.rootT = t1;
    pair.k1.cw_seed = cw_seed;
    pair.k1.cw_tL = cw_tL;
    pair.k1.cw_tR = cw_tR;
    pair.k1.cw_out = cw_out;
    pair.k1.size = size;
    pair.k1.depth = depth;

    return pair;
}

inline uint64_t eval(const DPFKey& key, size_t index) {
    if (index >= key.size) throw std::out_of_range("eval: index out of range");
    Seed256 s = key.rootSeed;
    uint8_t t = key.rootT;
    for (int level = 0; level < key.depth; ++level) {
        Seed256 sL, sR;
        uint8_t tL, tR;
        prg_expand(s, sL, tL, sR, tR);
        const uint8_t bit = get_bit(index, key.depth, level);
        Seed256 child = bit ? sR : sL;
        uint8_t tau = bit ? tR : tL;
        const uint8_t cwt = bit ? key.cw_tR[level] : key.cw_tL[level];
        uint8_t next_t = static_cast<uint8_t>(tau ^ (t & cwt));
        if (t == 0) seed_xor_inplace(child, key.cw_seed[level]);
        s = child;
        t = next_t;
    }
    uint64_t y = prg_leaf(s);
    if (t) y ^= key.cw_out;
    return y;
}

inline std::vector<uint64_t> eval_full(const DPFKey& key) {
    std::vector<uint64_t> values(key.size);
    for (size_t i = 0; i < key.size; ++i) {
        values[i] = eval(key, i);
    }
    return values;
}

inline bool serialize_key(std::ostream& out, const DPFKey& key) {
    uint64_t size64 = static_cast<uint64_t>(key.size);
    uint32_t depth32 = static_cast<uint32_t>(key.depth);
    out.write(reinterpret_cast<const char*>(&size64), sizeof(size64));
    out.write(reinterpret_cast<const char*>(&depth32), sizeof(depth32));
    out.write(reinterpret_cast<const char*>(&key.rootSeed), sizeof(key.rootSeed));
    out.write(reinterpret_cast<const char*>(&key.rootT), sizeof(key.rootT));
    out.write(reinterpret_cast<const char*>(&key.cw_out), sizeof(key.cw_out));
    for (int level = 0; level < key.depth; ++level) {
        out.write(reinterpret_cast<const char*>(key.cw_seed[level].k.data()), sizeof(uint32_t) * 8);
    }
    if (!key.cw_tL.empty()) {
        out.write(reinterpret_cast<const char*>(key.cw_tL.data()), key.cw_tL.size());
        out.write(reinterpret_cast<const char*>(key.cw_tR.data()), key.cw_tR.size());
    }
    return out.good();
}

inline bool deserialize_key(std::istream& in, DPFKey& key) {
    uint64_t size64 = 0;
    uint32_t depth32 = 0;
    if (!in.read(reinterpret_cast<char*>(&size64), sizeof(size64))) return false;
    if (!in.read(reinterpret_cast<char*>(&depth32), sizeof(depth32))) return false;
    key.size = static_cast<size_t>(size64);
    key.depth = static_cast<int>(depth32);
    if (!in.read(reinterpret_cast<char*>(&key.rootSeed), sizeof(key.rootSeed))) return false;
    if (!in.read(reinterpret_cast<char*>(&key.rootT), sizeof(key.rootT))) return false;
    if (!in.read(reinterpret_cast<char*>(&key.cw_out), sizeof(key.cw_out))) return false;
    key.cw_seed.assign(key.depth, Seed256{});
    key.cw_tL.assign(key.depth, 0);
    key.cw_tR.assign(key.depth, 0);
    for (int level = 0; level < key.depth; ++level) {
        if (!in.read(reinterpret_cast<char*>(key.cw_seed[level].k.data()), sizeof(uint32_t) * 8)) return false;
    }
    if (!key.cw_tL.empty()) {
        if (!in.read(reinterpret_cast<char*>(key.cw_tL.data()), key.cw_tL.size())) return false;
        if (!in.read(reinterpret_cast<char*>(key.cw_tR.data()), key.cw_tR.size())) return false;
    }
    return true;
}

inline bool serialize_key_text(std::ostream& out, const DPFKey& key) {
    out << key.size << ' ' << key.depth << '\n';
    for (int i = 0; i < 8; ++i) {
        out << key.rootSeed.k[i];
        out << (i + 1 < 8 ? ' ' : '\n');
    }
    out << static_cast<uint32_t>(key.rootT) << '\n';
    out << key.cw_out << '\n';
    for (int level = 0; level < key.depth; ++level) {
        for (int j = 0; j < 8; ++j) {
            out << key.cw_seed[level].k[j];
            out << (j + 1 < 8 ? ' ' : '\n');
        }
    }
    for (int level = 0; level < key.depth; ++level) {
        out << static_cast<int>(key.cw_tL[level]);
        if (level + 1 < key.depth) out << ' ';
    }
    out << '\n';
    for (int level = 0; level < key.depth; ++level) {
        out << static_cast<int>(key.cw_tR[level]);
        if (level + 1 < key.depth) out << ' ';
    }
    out << '\n';
    return out.good();
}

inline bool deserialize_key_text(std::istream& in, DPFKey& key) {
    size_t size = 0;
    int depth = 0;
    if (!(in >> size >> depth)) return false;
    key.size = size;
    key.depth = depth;
    for (int i = 0; i < 8; ++i) {
        if (!(in >> key.rootSeed.k[i])) return false;
    }
    uint32_t rootT32 = 0;
    if (!(in >> rootT32)) return false;
    key.rootT = static_cast<uint8_t>(rootT32 & 0xFFu);
    if (!(in >> key.cw_out)) return false;
    key.cw_seed.assign(key.depth, Seed256{});
    key.cw_tL.assign(key.depth, 0);
    key.cw_tR.assign(key.depth, 0);
    for (int level = 0; level < key.depth; ++level) {
        for (int j = 0; j < 8; ++j) {
            if (!(in >> key.cw_seed[level].k[j])) return false;
        }
    }
    for (int level = 0; level < key.depth; ++level) {
        int bit = 0;
        if (!(in >> bit)) return false;
        key.cw_tL[level] = static_cast<uint8_t>(bit & 1);
    }
    for (int level = 0; level < key.depth; ++level) {
        int bit = 0;
        if (!(in >> bit)) return false;
        key.cw_tR[level] = static_cast<uint8_t>(bit & 1);
    }
    return true;
}

} // namespace dpf
