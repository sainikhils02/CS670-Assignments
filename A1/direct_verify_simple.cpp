#include <iostream>
#include <fstream>
#include <vector>
#include <sstream>
#include <string>
#include <cstdint>
#include <iomanip>
#include <algorithm>

typedef uint64_t field_t;

// Field operations for 2^64 field
namespace Field {
    static constexpr field_t MODULUS = 1ULL << 32; // 2^32
    
    inline field_t add(field_t a, field_t b) { return (a + b) % MODULUS; }
    inline field_t sub(field_t a, field_t b) { return (a - b) % MODULUS; }
    inline field_t mul(field_t a, field_t b) { return (a * b) % MODULUS; }
    
    // Convert signed integer to field element
    inline field_t from_signed(int64_t x) {
        return static_cast<field_t>(x);
    }
    
    // Convert field element to signed integer (for display purposes)
    inline int64_t to_signed(field_t x) {
        return static_cast<int64_t>(x);
    }
}

// Load matrix from file
bool load_matrix(const std::string& filename, std::vector<std::vector<field_t> >& matrix, int rows, int cols) {
    std::ifstream file(filename.c_str());
    if (!file) {
        std::cerr << "Cannot open file: " << filename << std::endl;
        return false;
    }
    
    matrix.clear();
    matrix.resize(rows);
    for (int i = 0; i < rows; ++i) {
        matrix[i].resize(cols);
    }
    
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            if (!(file >> matrix[i][j])) {
                std::cerr << "Failed to read matrix element at [" << i << "][" << j << "] from " << filename << std::endl;
                return false;
            }
        }
    }
    
    return true;
}

// Compute dot product
field_t dot_product(const std::vector<field_t>& a, const std::vector<field_t>& b) {
    field_t result = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        result = Field::add(result, Field::mul(a[i], b[i]));
    }
    return result;
}

int main() {
    std::cout << "=== Direct Computation Verification ===" << std::endl;
    
    // Configuration - adjust these based on your setup
    int m = 12;  // users
    int n = 10;  // items  
    int k = 2;   // features
    
    std::cout << "Users: " << m << ", Items: " << n << ", Features: " << k << std::endl;
    
    // Load shares
    std::vector<std::vector<field_t> > U0, U1, V0, V1;
    
    if (!load_matrix("data/U0_shares.txt", U0, m, k) ||
        !load_matrix("data/U1_shares.txt", U1, m, k) ||
        !load_matrix("data/V0_shares.txt", V0, n, k) ||
        !load_matrix("data/V1_shares.txt", V1, n, k)) {
        std::cerr << "Failed to load share matrices" << std::endl;
        return 1;
    }
    
    // Reconstruct original matrices
    std::vector<std::vector<field_t> > U(m), V(n);
    for (int i = 0; i < m; ++i) U[i].resize(k);
    for (int i = 0; i < n; ++i) V[i].resize(k);
    
    for (int i = 0; i < m; ++i) {
        for (int j = 0; j < k; ++j) {
            U[i][j] = Field::add(U0[i][j], U1[i][j]);
        }
    }
    
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < k; ++j) {
            V[i][j] = Field::add(V0[i][j], V1[i][j]);
        }
    }
    
    // Print original matrices
    std::cout << "\\n=== Original Reconstructed Matrices ===" << std::endl;
    std::cout << "U matrix (first 10 users):" << std::endl;
    for (int i = 0; i < std::min(10, m); ++i) {
        std::cout << "User " << i << ": ";
        for (int j = 0; j < k; ++j) {
            std::cout << std::setw(12) << U[i][j];
        }
        std::cout << std::endl;
    }
    
    std::cout << "\\nV matrix (first 10 items):" << std::endl;
    for (int i = 0; i < std::min(10, n); ++i) {
        std::cout << "Item " << i << ": ";
        for (int j = 0; j < k; ++j) {
            std::cout << std::setw(12) << V[i][j];
        }
        std::cout << std::endl;
    }
    
    // Load and process queries
    std::cout << "\\n=== Query Processing ===" << std::endl;
    
    std::ifstream f0("data/queries_p0.txt");
    std::ifstream f1("data/queries_p1.txt");
    if (!f0 || !f1) {
        std::cerr << "Cannot open query files" << std::endl;
        return 1;
    }
    
    std::vector<int> updated_users;
    std::string line0, line1;
    int query_num = 0;
    
    // Process queries one by one
    while (std::getline(f0, line0) && std::getline(f1, line1)) {
        int user_id_0, user_id_1;
        
        // Parse P0's query line: user_id e_j[0] e_j[1] ... e_j[n-1]
        std::istringstream iss0(line0);
        iss0 >> user_id_0;
        std::vector<field_t> e_j_p0(n);
        for (int i = 0; i < n; ++i) {
            iss0 >> e_j_p0[i];
        }
        
        // Parse P1's query line: user_id e_j[0] e_j[1] ... e_j[n-1]
        std::istringstream iss1(line1);
        iss1 >> user_id_1;
        std::vector<field_t> e_j_p1(n);
        for (int i = 0; i < n; ++i) {
            iss1 >> e_j_p1[i];
        }
        
        // Both files should have the same user_id for the same query
        if (user_id_0 != user_id_1) {
            std::cerr << "Warning: User ID mismatch in query " << query_num 
                      << ": P0=" << user_id_0 << ", P1=" << user_id_1 << std::endl;
        }
        
        int user_id = user_id_0;  // Use P0's user_id
        
        // Reconstruct item selection vector e_j
        std::vector<field_t> e_j(n);
        for (int i = 0; i < n; ++i) {
            e_j[i] = Field::add(e_j_p0[i], e_j_p1[i]);
        }
        
        // Find which item is selected (e_j[item] should equal 1)
        int selected_item = -1;
        for (int i = 0; i < n; ++i) {
            if (e_j[i] == 1) {
                selected_item = i;
                break;
            }
        }
        
        // Compute v_j = V^T * e_j  (select the item)
        std::vector<field_t> v_j(k, 0);
        for (int f = 0; f < k; ++f) {
            for (int i = 0; i < n; ++i) {
                v_j[f] = Field::add(v_j[f], Field::mul(V[i][f], e_j[i]));
            }
        }
        
        // Validate user_id
        if (user_id >= m) {
            std::cerr << "Invalid user_id: " << user_id << " >= " << m << std::endl;
            continue;
        }
        
        // Compute dot product <u_i, v_j>
        field_t dot_prod = dot_product(U[user_id], v_j);
        
        // Compute delta = 1 - <u_i, v_j>
        field_t delta = Field::sub(1, dot_prod);
        
        std::cout << "Query " << query_num << ": User " << user_id << " <- Item " << selected_item 
                  << " (dot=" << dot_prod << ", 1-dot=" << delta << ")" << std::endl;
        
        // Update u_i <- u_i + v_j * delta
        for (int f = 0; f < k; ++f) {
            field_t update = Field::mul(v_j[f], delta);
            U[user_id][f] = Field::add(U[user_id][f], update);
        }
        
        updated_users.push_back(user_id);
        query_num++;
    }
    
    std::cout << "\\nUsers updated: ";
    for (std::size_t i = 0; i < updated_users.size(); ++i) {
        std::cout << updated_users[i] << " ";
    }
    std::cout << std::endl;
    
    // Print direct computation results
    std::cout << "\\n=== Direct Computation Results ===" << std::endl;
    std::cout << "Updated U matrix (first 10 users):" << std::endl;
    for (int i = 0; i < std::min(10, m); ++i) {
        std::cout << "User " << i << ": ";
        for (int j = 0; j < k; ++j) {
            std::cout << std::setw(12) << U[i][j];
        }
        std::cout << std::endl;
    }
    
    // Load MPC results and compare
    std::vector<std::vector<field_t> > U0_updated, U1_updated;
    
    if (load_matrix("data/U0_shares_updated.txt", U0_updated, m, k) &&
        load_matrix("data/U1_shares_updated.txt", U1_updated, m, k)) {
        
        // Reconstruct MPC result
        std::vector<std::vector<field_t> > U_mpc(m);
        for (int i = 0; i < m; ++i) U_mpc[i].resize(k);
        
        for (int i = 0; i < m; ++i) {
            for (int j = 0; j < k; ++j) {
                U_mpc[i][j] = Field::add(U0_updated[i][j], U1_updated[i][j]);
            }
        }
        
        std::cout << "\\n=== MPC Results Verification ===" << std::endl;
        std::cout << "MPC U matrix (first 10 users):" << std::endl;
        for (int i = 0; i < std::min(10, m); ++i) {
            std::cout << "User " << i << ": ";
            for (int j = 0; j < k; ++j) {
                std::cout << std::setw(12) << U_mpc[i][j];
            }
            std::cout << std::endl;
        }
        
        // Compare results
        std::cout << "\\n=== Comparison ===" << std::endl;
        int mismatches = 0;
        int total_elements = 0;
        
        for (int i = 0; i < std::min(10, m); ++i) {
            for (int j = 0; j < k; ++j) {
                total_elements++;
                if (U[i][j] != U_mpc[i][j]) {
                    if (mismatches < 10) {
                        std::cout << "MISMATCH User " << i << " Feature " << j 
                                  << ": Direct=" << U[i][j] << " MPC=" << U_mpc[i][j] << std::endl;
                    }
                    mismatches++;
                }
            }
        }
        
        if (mismatches > 10) {
            std::cout << "... and " << (mismatches - 10) << " more mismatches" << std::endl;
        }
        
        std::cout << "\\n=== Final Verification Result ===" << std::endl;
        if (mismatches == 0) {
            std::cout << "✅ SUCCESS: All " << total_elements << " elements match perfectly!" << std::endl;
        } else {
            std::cout << "❌ FAILURE: " << mismatches << " mismatches found out of " << total_elements << " elements." << std::endl;
            std::cout << "There may be an error in the MPC implementation or field arithmetic." << std::endl;
        }
        
    } else {
        std::cout << "Could not load MPC updated shares for comparison" << std::endl;
    }
    
    return 0;
}