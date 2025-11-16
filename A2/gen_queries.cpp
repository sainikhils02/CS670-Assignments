#include <bits/stdc++.h>
#include <openssl/evp.h>
#include <openssl/aes.h>

struct Seed256 {
    std::array<uint32_t, 8> k{}; // 256-bit key
};

// seed256_xor: XOR two 256-bit seeds and return the result.
static inline Seed256 seed256_xor(const Seed256 &a, const Seed256 &b) {
    Seed256 r;
    for (int i = 0; i < 8; ++i) r.k[i] = a.k[i] ^ b.k[i];
    return r;
}

// seed256_xor_inplace: XOR 'a' with 'b' in-place.
static inline void seed256_xor_inplace(Seed256 &a, const Seed256 &b) {
    for (int i = 0; i < 8; ++i) a.k[i] ^= b.k[i];
}

// seed256_u64_preview: 64-bit preview for printing/debugging of a 256-bit seed.
static inline uint64_t seed256_u64_preview(const Seed256 &s) {
    uint64_t lo = static_cast<uint64_t>(s.k[0]) | (static_cast<uint64_t>(s.k[1]) << 32);
    return lo;
}

// Helpers to derive AES key and nonce from Seed256 and generate words
static inline void seed_to_aes_key(const Seed256 &seed, uint8_t key[16]){
    for(int i=0;i<4;++i){
        uint32_t w = seed.k[i];
        key[4*i+0] = (uint8_t)(w & 0xFF);
        key[4*i+1] = (uint8_t)((w>>8)&0xFF);
        key[4*i+2] = (uint8_t)((w>>16)&0xFF);
        key[4*i+3] = (uint8_t)((w>>24)&0xFF);
    }
}

static inline void make_nonce(const Seed256 &seed, uint32_t domainConst, std::array<uint32_t,3> &nonce){
    nonce[0] = seed.k[4] ^ domainConst;
    nonce[1] = seed.k[5];
    nonce[2] = seed.k[6];
}

static void aes_ctr_words(const uint8_t key[16], const std::array<uint32_t,3> &nonce, uint32_t counterStart, uint32_t numWords, uint32_t *out){
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if(!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    uint8_t iv[16];
    // Compose IV bytes from nonce words and counter (little-endian words)
    uint32_t inw[4] = { nonce[0], nonce[1], nonce[2], counterStart };
    for(int i=0;i<4;++i){
        iv[4*i+0] = (uint8_t)(inw[i] & 0xFF);
        iv[4*i+1] = (uint8_t)((inw[i]>>8)&0xFF);
        iv[4*i+2] = (uint8_t)((inw[i]>>16)&0xFF);
        iv[4*i+3] = (uint8_t)((inw[i]>>24)&0xFF);
    }
    if(1!=EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr, key, iv)){
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_EncryptInit_ex failed");
    }
    uint32_t produced = 0;
    uint8_t inZeros[16] = {0};
    while(produced < numWords){
        uint8_t outb[16]; int outlen=0;
        if(1!=EVP_EncryptUpdate(ctx, outb, &outlen, inZeros, 16)){
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("EVP_EncryptUpdate failed");
        }
        // Parse 4 words from keystream block
        for(int i=0;i<4 && produced < numWords; ++i){
            out[produced++] = (uint32_t)outb[4*i] | ((uint32_t)outb[4*i+1]<<8) | ((uint32_t)outb[4*i+2]<<16) | ((uint32_t)outb[4*i+3]<<24);
        }
    }
    EVP_CIPHER_CTX_free(ctx);
}

// Domain-separated expansions using AES-CTR derived words
static inline void prg_expand_full(const Seed256 &seed, Seed256 &sL, uint8_t &tL, Seed256 &sR, uint8_t &tR) {
    const uint32_t DOMAIN_SL = 0xC0015E5A;
    const uint32_t DOMAIN_SR = 0xC0015255;
    const uint32_t DOMAIN_T  = 0xC0017455;
    uint8_t key[16]; seed_to_aes_key(seed, key);
    std::array<uint32_t,3> nonceSL, nonceSR, nonceT;
    make_nonce(seed, DOMAIN_SL, nonceSL);
    make_nonce(seed, DOMAIN_SR, nonceSR);
    make_nonce(seed, DOMAIN_T,  nonceT);
    uint32_t wL[8], wR[8], wT[2];
    aes_ctr_words(key, nonceSL, 0u, 8u, wL);
    aes_ctr_words(key, nonceSR, 0u, 8u, wR);
    aes_ctr_words(key, nonceT,  0u, 2u, wT);
    for(int i=0;i<8;++i){ sL.k[i]=wL[i]; sR.k[i]=wR[i]; }
    tL = (uint8_t)(wT[0] & 1u);
    tR = (uint8_t)(wT[1] & 1u);
}

static inline uint64_t prg_leaf_output(const Seed256 &seed) {
    const uint32_t DOMAIN_OUT = 0x0BADF00D;
    uint8_t key[16]; seed_to_aes_key(seed, key);
    std::array<uint32_t,3> nonce;
    make_nonce(seed, DOMAIN_OUT, nonce);
    uint32_t w[2];
    aes_ctr_words(key, nonce, 0u, 2u, w);
    return (uint64_t)w[0] | ((uint64_t)w[1] << 32);
}


struct DPFKey {
    Seed256 rootSeed;
    uint8_t rootT {0};
    std::vector<Seed256> cw_seed; // per level seed correction 
    std::vector<uint8_t> cw_tL;   // per level bit correction for taking Left
    std::vector<uint8_t> cw_tR;   // per level bit correction for taking Right
    uint64_t cw_out {0};          // final output correction
    size_t size {0};
    int depth {0};
};

struct DPFKeys {
    DPFKey k0;
    DPFKey k1;
    size_t size;
    uint64_t targetValue;
    size_t location;
};

// isPowerOfTwo: Return true iff x is a power of two and non-zero.
static inline bool isPowerOfTwo(uint64_t x) {
    return x && ((x & (x - 1)) == 0);
}

// ilog2_size: Compute depth d with 2^d >= n; precondition ensures n is power of two so 2^d == n.
static inline int ilog2_size(size_t n) {
    int d = 0;
    while ((size_t(1) << d) < n) ++d;
    return d;
}

// Expand a seed into left/right child seeds and control bits deterministically

// Helper to get MSB-first bit at a given level: level in [0..depth-1]
// get_bit_msb: MSB-first bit of 'index' at 'level' (0..depth-1), with 0 as the top level.
static inline uint8_t get_bit_msb(size_t index, int depth, int level) {
    int shift = depth - 1 - level;
    return static_cast<uint8_t>((index >> shift) & 1ULL);
}

// ---  helpers: clear low 2 bits (mimic block clear_lsb(..., 0b11)) ---
static inline void _clear_lsbs(Seed256 &s) {
    s.k[0] &= ~0x3u; // clear 2 LSBs of the aggregate by masking first word
}

//  expand that clears LSBs before PRG and in returned children, t bits separate
static inline void prg_expand_(const Seed256 &seedIn, Seed256 &sL, uint8_t &tL, Seed256 &sR, uint8_t &tR) {
    Seed256 seed = seedIn;
    _clear_lsbs(seed);
    prg_expand_full(seed, sL, tL, sR, tR);
    _clear_lsbs(sL);
    _clear_lsbs(sR);
}


// -------- : generation --------
static DPFKeys generateDPF_(size_t size, size_t location, uint64_t value, std::mt19937_64 &rng) {
    if (location >= size) throw std::invalid_argument("location out of range");
    if (!isPowerOfTwo(size)) throw std::invalid_argument("DPF_size must be a power of two");

    DPFKeys keys{};
    keys.size = size;
    keys.location = location;
    keys.targetValue = value;

    const int depth = ilog2_size(size);

    auto make_seed = [&](Seed256 &dst){
        for (int i = 0; i < 8; ++i) dst.k[i] = static_cast<uint32_t>(rng() & 0xFFFFFFFFu);
    };

    // Initialize party roots and t bits; ensure t0 ^ t1 = 1 
    Seed256 s0, s1; make_seed(s0); make_seed(s1);
    uint8_t t0 = static_cast<uint8_t>(s0.k[0] & 1u);
    uint8_t t1 = static_cast<uint8_t>(t0 ^ 1u);
    const Seed256 s0_root = s0, s1_root = s1;
    const uint8_t t0_root = t0, t1_root = t1;

    std::vector<Seed256> cw_seed(depth);
    std::vector<uint8_t> cw_tL(depth), cw_tR(depth);

    const int nbits = depth; // MSB-first traversal bits
    for (int level = 0; level < depth; ++level) {
        // Expand both party seeds
        Seed256 s0L, s0R, s1L, s1R; uint8_t t0L, t0R, t1L, t1R;
    prg_expand_(s0, s0L, t0L, s0R, t0R);
    prg_expand_(s1, s1L, t1L, s1R, t1R);

        const uint8_t bit = get_bit_msb(location, nbits, level);
        const uint8_t keep = (bit == 0) ? 0 : 1; // 0:Left, 1:Right
        const uint8_t lose = 1 ^ keep;

        // Bit corrections per direction 
        // cwt[L] = t0L ^ t1L ^ bit ^ 1;  cwt[R] = t0R ^ t1R ^ bit
        const uint8_t cwtL = static_cast<uint8_t>(t0L ^ t1L ^ bit ^ 1U);
        const uint8_t cwtR = static_cast<uint8_t>(t0R ^ t1R ^ bit);
        cw_tL[level] = cwtL; cw_tR[level] = cwtR;

        // Seed correction for the lose branch
        const Seed256 nextcw = (lose == 0) ? seed256_xor(s0L, s1L) : seed256_xor(s0R, s1R);
        cw_seed[level] = nextcw;

        // Advance party 0 along keep direction
    Seed256 child0 = (keep == 0) ? s0L : s0R; uint8_t tau0 = (keep == 0) ? t0L : t0R;
    if (t0 == 0) seed256_xor_inplace(child0, nextcw); // apply seed correction when prev t == 0
    _clear_lsbs(child0);
        t0 = static_cast<uint8_t>(tau0 ^ (t0 & (keep == 0 ? cwtL : cwtR)));
        s0 = child0;

        // Advance party 1 along keep direction
        Seed256 child1 = (keep == 0) ? s1L : s1R; uint8_t tau1 = (keep == 0) ? t1L : t1R;
        if (t1 == 0) seed256_xor_inplace(child1, nextcw);
        _clear_lsbs(child1);
        t1 = static_cast<uint8_t>(tau1 ^ (t1 & (keep == 0 ? cwtL : cwtR)));
        s1 = child1;
    }

    // Finalizer-style correction: F = value ^ stretch(s0) ^ stretch(s1)
    const uint64_t cw_out = value ^ prg_leaf_output(s0) ^ prg_leaf_output(s1);

    // Fill keys
    keys.k0.rootSeed = s0_root; keys.k0.rootT = t0_root;
    keys.k0.cw_seed = cw_seed; keys.k0.cw_tL = cw_tL; keys.k0.cw_tR = cw_tR; keys.k0.cw_out = cw_out; keys.k0.size = size; keys.k0.depth = depth;
    keys.k1.rootSeed = s1_root; keys.k1.rootT = t1_root;
    keys.k1.cw_seed = cw_seed; keys.k1.cw_tL = cw_tL; keys.k1.cw_tR = cw_tR; keys.k1.cw_out = cw_out; keys.k1.size = size; keys.k1.depth = depth;

    return keys;
}

// -------- : evaluation --------
static inline uint64_t evalDPF_(const DPFKey &key, size_t index) {
    if (index >= key.size) throw std::out_of_range("evalDPF_: index out of range");
    Seed256 s = key.rootSeed; uint8_t t = key.rootT;
    for (int level = 0; level < key.depth; ++level) {
        Seed256 sL, sR; uint8_t tL, tR; prg_expand_(s, sL, tL, sR, tR);
        const uint8_t b = get_bit_msb(index, key.depth, level);
        Seed256 child = (b == 0) ? sL : sR; uint8_t tau = (b == 0) ? tL : tR;
        const uint8_t cwt = (b == 0) ? key.cw_tL[level] : key.cw_tR[level];
        // Update t with masked cwt, and apply seed correction when prev t == 0
        uint8_t newt = static_cast<uint8_t>(tau ^ (t & cwt));
        if (t == 0) seed256_xor_inplace(child, key.cw_seed[level]);
        _clear_lsbs(child);
        s = child; t = newt;
    }
    uint64_t y = prg_leaf_output(s);
    if (t) y ^= key.cw_out;
    return y;
}

static bool EvalFull_(const DPFKeys &keys, bool verbose = false) {
    bool ok = true;
    for (size_t i = 0; i < keys.size; ++i) {
        uint64_t v0 = evalDPF_(keys.k0, i);
        uint64_t v1 = evalDPF_(keys.k1, i);
        uint64_t combined = v0 ^ v1;
        uint64_t expected = (i == keys.location) ? keys.targetValue : 0ULL;
        if (combined != expected) {
            ok = false;
            if (verbose) {
                std::cerr << "Mismatch() at index " << i
                          << ": got=0x" << std::hex << std::setw(16) << std::setfill('0') << combined
                          << ", expected=0x" << std::setw(16) << expected << std::setfill(' ') << std::dec << "\n";
            }
        }
    }
    return ok;
}

static void printKeyPreview_(const DPFKey &k, size_t preview = 4) {
    std::cout << "  rootSeed=0x" << std::hex << std::setw(16) << std::setfill('0') << seed256_u64_preview(k.rootSeed)
              << std::dec << ", rootT=" << static_cast<int>(k.rootT) << "\n";
    size_t n = std::min(preview, k.cw_seed.size());
    std::cout << "  cw_seed[0.." << (n ? n - 1 : 0) << "]:";
    for (size_t i = 0; i < n; ++i) {
        std::cout << " 0x" << std::hex << std::setw(16) << std::setfill('0') << seed256_u64_preview(k.cw_seed[i]) << std::dec;
    }
    if (k.cw_seed.size() > n) std::cout << " ...";
    std::cout << "\n";
    std::cout << "  cw_tL[0.." << (n ? n - 1 : 0) << "]:";
    for (size_t i = 0; i < n; ++i) std::cout << ' ' << static_cast<int>(k.cw_tL[i]);
    if (k.cw_tL.size() > n) std::cout << " ...";
    std::cout << "\n";
    std::cout << "  cw_tR[0.." << (n ? n - 1 : 0) << "]:";
    for (size_t i = 0; i < n; ++i) std::cout << ' ' << static_cast<int>(k.cw_tR[i]);
    if (k.cw_tR.size() > n) std::cout << " ...";
    std::cout << "\n";
    std::cout << "  cw_out=0x" << std::hex << std::setw(16) << std::setfill('0') << k.cw_out << std::dec << "\n";
}

// main: CLI entry point for generating and verifying DPF keys.
int main(int argc, char* argv[]) {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    if (argc < 3 || argc > 4) {
        std::cerr << "Usage: ./gen_queries <DPF_size> <num_DPFs> [--print-evals]\n";
        return 1;
    }

    // Parse arguments
    uint64_t size64 = 0;
    uint64_t num64 = 0;
    try {
        size64 = std::stoull(argv[1]);
        num64 = std::stoull(argv[2]);
    } catch (const std::exception &) {
        std::cerr << "Invalid arguments. Both should be positive integers.\n";
        return 1;
    }

    if (size64 == 0 || num64 == 0) {
        std::cerr << "Both <DPF_size> and <num_DPFs> must be > 0.\n";
        return 1;
    }

    if (size64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        std::cerr << "<DPF_size> too large for this platform.\n";
        return 1;
    }

    const size_t DPF_size = static_cast<size_t>(size64);
    const size_t num_DPFs = static_cast<size_t>(num64);
    const bool print_evals = (argc == 4 && std::string(argv[3]) == "--print-evals");

    std::random_device rd;
    std::seed_seq seed{rd(), rd(), rd(), rd(), rd(), rd()};
    std::mt19937_64 rng(seed);

    std::uniform_int_distribution<size_t> locDist(0, DPF_size - 1);
    std::uniform_int_distribution<uint64_t> valDist(0, std::numeric_limits<uint64_t>::max());

    // Generate and test each DPF 
    for (size_t d = 0; d < num_DPFs; ++d) {
        size_t location = locDist(rng);
        uint64_t target = valDist(rng);
        DPFKeys keys = generateDPF_(DPF_size, location, target, rng);

        bool ok = EvalFull_(keys, false);
        std::cout << "DPF #" << d << ": size=" << keys.size
                  << ", location=" << keys.location
                  << ", target=0x" << std::hex << std::setw(16) << std::setfill('0') << keys.targetValue
                  << std::dec << ", result=" << (ok ? "Test Passed" : "Test Failed")
                  << "\n";

        std::cout << "  Key0 preview: (For debug purposes)" << "\n";
        printKeyPreview_(keys.k0);
        std::cout << "  Key1 preview: (For debug purposes)" << "\n";
        printKeyPreview_(keys.k1);

        if (print_evals) {
            std::cout << "  Per-index evaluations (v0, v1, v0^v1):\n";
            for (size_t i = 0; i < keys.size; ++i) {
                uint64_t v0 = evalDPF_(keys.k0, i);
                uint64_t v1 = evalDPF_(keys.k1, i);
                uint64_t vx = v0 ^ v1;
                std::cout << "    i=" << i
                          << ": v0=0x" << std::hex << std::setw(16) << std::setfill('0') << v0
                          << " v1=0x" << std::setw(16) << v1
                          << " xor=0x" << std::setw(16) << vx
                          << std::setfill(' ') << std::dec << '\n';
            }
        }
    }

    return 0;
}
