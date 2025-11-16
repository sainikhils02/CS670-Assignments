#pragma once

#include <boost/asio.hpp>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
using boost::asio::ip::tcp;
using boost::asio::use_awaitable;

using field_t = uint64_t;

namespace Field {
    constexpr field_t MODULUS = 1ULL << 32;

    inline field_t add(field_t a, field_t b) { return (a + b) % MODULUS; }
    inline field_t sub(field_t a, field_t b) { return (a + MODULUS - b % MODULUS) % MODULUS; }
    inline field_t mul(field_t a, field_t b) { return (a * b) % MODULUS; }

    inline field_t random() {
        static std::random_device rd;
        static std::mt19937_64 gen(rd());
        static std::uniform_int_distribution<field_t> dis(0, MODULUS - 1);
        return dis(gen);
    }

    inline field_t small_random() {
        static std::random_device rd;
        static std::mt19937_64 gen(rd());
        static std::uniform_int_distribution<int> dis(1, 5);
        return static_cast<field_t>(dis(gen));
    }

    inline field_t from_signed(int64_t x) {
        return static_cast<field_t>(static_cast<uint64_t>(x) & (MODULUS - 1));
    }

    inline int64_t to_signed(field_t x) {
        return static_cast<int64_t>(static_cast<int32_t>(x & 0xFFFFFFFFULL));
    }
}

struct ShareField {
    field_t value{0};

    ShareField() = default;
    explicit ShareField(field_t v) : value(v) {}

    ShareField operator+(const ShareField& other) const { return ShareField(Field::add(value, other.value)); }
    ShareField operator-(const ShareField& other) const { return ShareField(Field::sub(value, other.value)); }
};

using ShareVectorField = std::vector<ShareField>;

struct ProgramConfig {
    size_t num_users{0};
    size_t num_items{0};
    size_t num_queries{0};

    bool load(const std::string& filename) {
        std::ifstream in(filename);
        if (!in.is_open()) {
            std::cerr << "Failed to open config file: " << filename << '\n';
            return false;
        }
        in >> num_users >> num_items >> num_queries;
        return in.good();
    }
};

inline bool load_vector_shares(const std::string& filename, ShareVectorField& vec) {
    std::ifstream in(filename);
    if (!in.is_open()) {
        std::cerr << "Cannot open share file: " << filename << '\n';
        return false;
    }
    for (auto& entry : vec) {
        if (!(in >> entry.value)) {
            std::cerr << "Unexpected EOF in " << filename << '\n';
            return false;
        }
    }
    return true;
}

inline bool save_vector_shares(const std::string& filename, const ShareVectorField& vec) {
    std::ofstream out(filename);
    if (!out.is_open()) {
        std::cerr << "Cannot open share file for writing: " << filename << '\n';
        return false;
    }
    for (size_t i = 0; i < vec.size(); ++i) {
        out << vec[i].value;
        if (i + 1 < vec.size()) out << '\n';
    }
    return true;
}

inline int64_t sum_vector(const std::vector<int64_t>& vec) {
    int64_t acc = 0;
    for (auto v : vec) acc += v;
    return acc;
}

// ---------------- Communication helpers ----------------

inline awaitable<void> send_field(tcp::socket& sock, field_t value) {
    co_await boost::asio::async_write(sock, boost::asio::buffer(&value, sizeof(value)), use_awaitable);
}

inline awaitable<void> recv_field(tcp::socket& sock, field_t& out) {
    co_await boost::asio::async_read(sock, boost::asio::buffer(&out, sizeof(out)), use_awaitable);
}

inline awaitable<void> send_int64(tcp::socket& sock, int64_t value) {
    co_await boost::asio::async_write(sock, boost::asio::buffer(&value, sizeof(value)), use_awaitable);
}

inline awaitable<void> recv_int64(tcp::socket& sock, int64_t& out) {
    co_await boost::asio::async_read(sock, boost::asio::buffer(&out, sizeof(out)), use_awaitable);
}

inline awaitable<void> send_vector(tcp::socket& sock, const std::vector<field_t>& vec) {
    for (auto val : vec) {
        co_await send_field(sock, val);
    }
}

inline awaitable<void> recv_vector(tcp::socket& sock, std::vector<field_t>& vec) {
    for (auto& val : vec) {
        co_await recv_field(sock, val);
    }
}

struct PreprocessingData {
    field_t correction_term{0};
    std::vector<field_t> X;
    std::vector<field_t> Y;
};

inline awaitable<PreprocessingData> request_preprocessing(tcp::socket& sock, size_t dimension) {
    field_t dim = static_cast<field_t>(dimension);
    co_await send_field(sock, dim);

    PreprocessingData prep;
    prep.X.resize(dimension);
    prep.Y.resize(dimension);

    co_await recv_field(sock, prep.correction_term);
    co_await recv_vector(sock, prep.X);
    co_await recv_vector(sock, prep.Y);
    co_return prep;
}

inline awaitable<ShareField> secure_dot_product(
    tcp::socket& peer_sock,
    tcp::socket& p2_sock,
    const ShareVectorField& a_share,
    const ShareVectorField& b_share,
    bool is_p0) {
    if (a_share.size() != b_share.size()) {
        throw std::runtime_error("secure_dot_product: dimension mismatch");
    }
    size_t k = a_share.size();
    PreprocessingData prep = co_await request_preprocessing(p2_sock, k);

    std::vector<field_t> masked_a(k), masked_b(k);
    for (size_t i = 0; i < k; ++i) {
        masked_a[i] = Field::add(a_share[i].value, prep.X[i]);
        masked_b[i] = Field::add(b_share[i].value, prep.Y[i]);
    }

    std::vector<field_t> peer_masked_a(k), peer_masked_b(k);

    co_await send_vector(peer_sock, masked_a);
    co_await send_vector(peer_sock, masked_b);

    co_await recv_vector(peer_sock, peer_masked_a);
    co_await recv_vector(peer_sock, peer_masked_b);

    field_t local_result = 0;

    for (size_t i = 0; i < k; ++i) {
        field_t sum_b = Field::add(b_share[i].value, peer_masked_b[i]);
        local_result = Field::add(local_result, Field::mul(a_share[i].value, sum_b));
    }

    for (size_t i = 0; i < k; ++i) {
        field_t product = Field::mul(prep.Y[i], peer_masked_a[i]);
        local_result = Field::sub(local_result, product);
    }

    if (is_p0) {
        local_result = Field::add(local_result, prep.correction_term);
    } else {
        local_result = Field::add(local_result, prep.correction_term);
    }

    co_return ShareField(local_result);
}

// Secure scalar multiplication using the same preprocessing-based protocol as the dot product.
inline awaitable<ShareField> secure_multiplication(
    tcp::socket& peer_sock,
    tcp::socket& p2_sock,
    const ShareField& left,
    const ShareField& right,
    bool is_p0) {
    ShareVectorField lhs_vec(1), rhs_vec(1);
    lhs_vec[0] = left;
    rhs_vec[0] = right;
    co_return co_await secure_dot_product(peer_sock, p2_sock, lhs_vec, rhs_vec, is_p0);
}

inline awaitable<ShareField> secure_scalar_product(
    tcp::socket& peer_sock,
    tcp::socket& p2_sock,
    const ShareField& left,
    const ShareField& right,
    bool is_p0) {
    co_return co_await secure_multiplication(peer_sock, p2_sock, left, right, is_p0);
}

inline awaitable<std::vector<field_t>> convert_xor_to_additive(
    tcp::socket& peer_sock,
    const std::vector<uint64_t>& xor_values,
    bool is_p0) {
    std::vector<int64_t> temp(xor_values.size());
    for (size_t i = 0; i < xor_values.size(); ++i) {
        int64_t val = static_cast<int64_t>(xor_values[i]);
        if (!is_p0) val = -val;
        temp[i] = val;
    }

    int64_t sum_local = sum_vector(temp);
    int64_t sum_peer = 0;
    if (is_p0) {
        co_await send_int64(peer_sock, sum_local);
        co_await recv_int64(peer_sock, sum_peer);
    } else {
        co_await recv_int64(peer_sock, sum_peer);
        co_await send_int64(peer_sock, sum_local);
    }

    int64_t total = sum_local + sum_peer;
    if (total < 0) {
        for (auto& v : temp) v = -v;
    }

    std::vector<field_t> additive(temp.size());
    for (size_t i = 0; i < temp.size(); ++i) {
        additive[i] = Field::from_signed(temp[i]);
    }
    co_return additive;
}
