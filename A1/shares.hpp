#pragma once
#include <vector>
#include <random>
#include <iostream>
#include <limits>
#include <type_traits>
#include <cstdint>

// Forward declare field_t
using field_t = uint64_t;

// Field operations namespace
namespace Field {
    static constexpr field_t MODULUS = 1ULL << 32; // 2^32
    
    inline field_t add(field_t a, field_t b) { return (a + b) % MODULUS; }
    inline field_t sub(field_t a, field_t b) { return (a - b) % MODULUS; }
    inline field_t mul(field_t a, field_t b) { return (a * b) % MODULUS; }
    
    inline field_t random() {
        static std::random_device rd;
        static std::mt19937_64 gen(rd());
        static std::uniform_int_distribution<field_t> dis(0, MODULUS - 1);
        return dis(gen);
    }
    
    // Generate small random values to prevent exponential growth
    inline field_t small_random() {
        static std::random_device rd;
        static std::mt19937_64 gen(rd());
        static std::uniform_int_distribution<field_t> dis(1, 5);  // Very small values 1-5
        return dis(gen);
    }
    
    // Convert signed integer to field element
    inline field_t from_signed(int64_t x) {
        return static_cast<field_t>(x);
    }
    
    // Convert field element to signed integer (for display purposes)
    inline int64_t to_signed(field_t x) {
        return static_cast<int64_t>(x);
    }
}

// Share structure for secret sharing
template<typename T>
struct Share {
    T value;
    
    Share() : value(0) {}
    Share(T v) : value(v) {}
    
    // Randomize the share using appropriate field operations
    void randomize() {
        if constexpr (std::is_same_v<T, field_t>) {
            value = Field::random();
        } else {
            static std::random_device rd;
            static std::mt19937 gen(rd());
            if constexpr (std::is_signed_v<T>) {
                static std::uniform_int_distribution<T> dis(std::numeric_limits<T>::min(), std::numeric_limits<T>::max());
                value = dis(gen);
            } else {
                static std::uniform_int_distribution<T> dis;
                value = dis(gen);
            }
        }
    }
    
    Share operator+(const Share& other) const {
        if constexpr (std::is_same_v<T, field_t>) {
            return Share(Field::add(value, other.value));
        } else {
            return Share(value + other.value);
        }
    }
    
    Share operator-(const Share& other) const {
        if constexpr (std::is_same_v<T, field_t>) {
            return Share(Field::sub(value, other.value));
        } else {
            return Share(value - other.value);
        }
    }
    
    Share operator*(const Share& other) const {
        if constexpr (std::is_same_v<T, field_t>) {
            return Share(Field::mul(value, other.value));
        } else {
            return Share(value * other.value);
        }
    }
};

// Type definitions for both legacy and field-based shares
using Share32 = Share<int32_t>;           // Legacy support
using ShareField = Share<field_t>;        // New field-based shares

// Vector types
using ShareVector = std::vector<Share32>;         // Legacy support  
using ShareVectorField = std::vector<ShareField>; // New field-based vectors

// Matrix of shares (row-major order) - Legacy version
struct ShareMatrix {
    std::vector<ShareVector> data;
    size_t rows, cols;
    
    ShareMatrix(size_t r, size_t c) : rows(r), cols(c) {
        data.resize(r, ShareVector(c));
    }
    
    ShareVector& operator[](size_t i) { return data[i]; }
    const ShareVector& operator[](size_t i) const { return data[i]; }
    
    void randomize() {
        for (auto& row : data) {
            for (auto& share : row) {
                share.randomize();
            }
        }
    }
};

// Matrix of field shares (row-major order) - New field version  
struct ShareMatrixField {
    std::vector<ShareVectorField> data;
    size_t rows, cols;
    
    ShareMatrixField(size_t r, size_t c) : rows(r), cols(c) {
        data.resize(r, ShareVectorField(c));
    }
    
    ShareVectorField& operator[](size_t i) { return data[i]; }
    const ShareVectorField& operator[](size_t i) const { return data[i]; }
    
    void randomize() {
        for (auto& row : data) {
            for (auto& share : row) {
                share.randomize();
            }
        }
    }
};

// MPC preprocessing materials from trusted dealer (Legacy version)
// Following the (2+1)-party dot product protocol
struct MPCPreprocessing {
    // Random matrices for masking
    ShareMatrix X0, X1, Y0, Y1;  // For dot product protocol
    ShareVector correction_p0;   // ⟨X₀ᵀ, Y₁⟩ + α for P0
    ShareVector correction_p1;   // ⟨X₁ᵀ, Y₀⟩ - α for P1
    
    MPCPreprocessing(size_t dim) 
        : X0(1, dim), X1(1, dim), Y0(1, dim), Y1(1, dim), 
          correction_p0(1), correction_p1(1) {
        // Initialize random values
        X0.randomize();
        X1.randomize(); 
        Y0.randomize();
        Y1.randomize();
        correction_p0[0].randomize();
        correction_p1[0].randomize();
    }
};