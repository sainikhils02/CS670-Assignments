#include "common.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <random>
#include <limits>

struct Query {
    size_t user_id;     // i (public)
    std::vector<field_t> e_j0;  // Additive share for P0 using field elements
    std::vector<field_t> e_j1;  // Additive share for P1 using field elements
};

int main(int argc, char* argv[]) {
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0] << " <num_users> <num_items> <num_features> <num_queries>" << std::endl;
        return 1;
    }
    
    size_t m = std::stoul(argv[1]);  // number of users
    size_t n = std::stoul(argv[2]);  // number of items
    size_t k = std::stoul(argv[3]);  // number of features
    size_t num_queries = std::stoul(argv[4]);
    
    std::random_device rd;
    std::mt19937_64 gen(rd());  // Use 64-bit generator for field elements
    std::uniform_int_distribution<size_t> user_dist(0, m - 1);
    std::uniform_int_distribution<size_t> item_dist(0, n - 1);
    std::uniform_int_distribution<field_t> share_dist;  // Full range for field elements
    std::uniform_int_distribution<field_t> small_share_dist(1, 5);  // Small values for query shares
    
    // Generate secret-shared matrices U and V using small field values
    ShareMatrixField U0(m, k), U1(m, k), V0(n, k), V1(n, k);
    
    // Initialize with small random values instead of full randomization
    for (size_t i = 0; i < m; ++i) {
        for (size_t j = 0; j < k; ++j) {
            U0[i][j].value = Field::small_random();
            U1[i][j].value = Field::small_random();
        }
    }
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < k; ++j) {
            V0[i][j].value = Field::small_random();
            V1[i][j].value = Field::small_random();
        }
    }
    
    // Save field shares to files
    save_field_shares_to_file("data/U0_shares.txt", U0);
    save_field_shares_to_file("data/U1_shares.txt", U1);
    save_field_shares_to_file("data/V0_shares.txt", V0);
    save_field_shares_to_file("data/V1_shares.txt", V1);
    
    // Save configuration for other programs
    std::ofstream config_file("data/config.txt");
    config_file << m << " " << n << " " << k << " " << num_queries << std::endl;
    config_file.close();
    
    // Generate queries
    std::vector<Query> queries;
    std::ofstream queries_p0("data/queries_p0.txt");
    std::ofstream queries_p1("data/queries_p1.txt");
    
    for (size_t q = 0; q < num_queries; ++q) {
        Query query;
        query.user_id = user_dist(gen);
        size_t item_id = item_dist(gen);
        
        // Create standard basis vector e_j (item_id-th position is 1, rest 0)
        std::vector<field_t> e_j(n, 0);
        e_j[item_id] = 1;  // Set selected item to 1 in field
        
        // Generate additive shares using field operations: e_j0 + e_j1 = e_j
        query.e_j0.resize(n);
        query.e_j1.resize(n);
        
        for (size_t i = 0; i < n; ++i) {
            query.e_j0[i] = small_share_dist(gen);  // Small random field element for P0
            query.e_j1[i] = Field::sub(e_j[i], query.e_j0[i]);  // P1's share: e_j[i] - e_j0[i]
        }
        
        queries.push_back(query);
        
        // Write to P0 file
        queries_p0 << query.user_id;
        for (const auto& share : query.e_j0) {
            queries_p0 << " " << share;
        }
        queries_p0 << "\n";
        
        // Write to P1 file
        queries_p1 << query.user_id;
        for (const auto& share : query.e_j1) {
            queries_p1 << " " << share;
        }
        queries_p1 << "\n";
    }
    
    queries_p0.close();
    queries_p1.close();
    
    return 0;
}