#pragma once
#include <utility>
#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <iostream>
#include <random>
#include <vector>
#include <fstream>
#include <limits>
#include <cstdint>
#include "shares.hpp"

using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
using boost::asio::use_awaitable;
using boost::asio::ip::tcp;
namespace this_coro = boost::asio::this_coro;

// Field element type - using uint64_t for 2^64 field
// (Field operations are defined in shares.hpp)

// Legacy functions for backward compatibility - now use field operations
inline field_t random_int32() {
    return Field::random();
}

inline uint32_t random_uint32() {
    return static_cast<uint32_t>(Field::random());
}

// Blind by XOR mask (works for field elements)
inline field_t blind_value(field_t v) {
    return v ^ 0xDEADBEEFCAFEBABEULL;
}

// MPC dot product computation between two secret-shared vectors using field operations
inline ShareField MPC_DOTPRODUCT(const ShareVectorField& a, const ShareVectorField& b) {
    if (a.size() != b.size()) {
        throw std::runtime_error("Vector dimensions don't match");
    }
    
    ShareField result(0);
    for (size_t i = 0; i < a.size(); ++i) {
        field_t product = Field::mul(a[i].value, b[i].value);
        result = result + ShareField(product);
    }
    return result;
}

// Load shares from file
inline bool load_shares_from_file(const std::string& filename, ShareMatrix& matrix) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        return false;
    }
    
    for (size_t i = 0; i < matrix.rows; ++i) {
        for (size_t j = 0; j < matrix.cols; ++j) {
            file >> matrix[i][j].value;
        }
    }
    return true;
}

// Save shares to file (Legacy version)
inline bool save_shares_to_file(const std::string& filename, const ShareMatrix& matrix) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        return false;
    }
    
    for (size_t i = 0; i < matrix.rows; ++i) {
        for (size_t j = 0; j < matrix.cols; ++j) {
            file << matrix[i][j].value;
            if (j < matrix.cols - 1) file << " ";
        }
        file << "\n";
    }
    return true;
}

// Save field shares to file (New field version)
inline bool save_field_shares_to_file(const std::string& filename, const ShareMatrixField& matrix) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        return false;
    }
    
    for (size_t i = 0; i < matrix.rows; ++i) {
        for (size_t j = 0; j < matrix.cols; ++j) {
            // Write field elements as unsigned integers
            file << matrix[i][j].value;
            if (j < matrix.cols - 1) file << " ";
        }
        file << "\n";
    }
    return true;
}

// Load field shares from file
inline bool load_field_shares_from_file(const std::string& filename, ShareMatrixField& matrix) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        return false;
    }
    
    for (size_t i = 0; i < matrix.rows; ++i) {
        for (size_t j = 0; j < matrix.cols; ++j) {
            field_t value;
            file >> value;
            matrix[i][j].value = value;
        }
    }
    return true;
}

// Alternative name for compatibility
inline bool load_matrix_shares(const std::string& filename, ShareMatrixField& matrix) {
    return load_field_shares_from_file(filename, matrix);
}

// Save field-based matrix shares to file  
inline bool save_matrix_shares(const std::string& filename, const ShareMatrixField& matrix) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        return false;
    }
    
    for (size_t i = 0; i < matrix.rows; ++i) {
        for (size_t j = 0; j < matrix.cols; ++j) {
            file << matrix[i][j].value;
            if (j < matrix.cols - 1) file << " ";
        }
        file << "\n";
    }
    
    return true;
}
