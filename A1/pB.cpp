#include "common.hpp"
#include <fstream>
#include <sstream>

using boost::asio::awaitable;
using boost::asio::use_awaitable;
using boost::asio::async_read;
using boost::asio::async_write;
using boost::asio::ip::tcp;

// Role validation
#if !defined(ROLE_p0) && !defined(ROLE_p1)
#error "ROLE must be defined as ROLE_p0 or ROLE_p1"
#endif

// ----------------------- Helper coroutines -----------------------

// Field element communication
awaitable<void> send_coroutine(tcp::socket& sock, field_t value) {
    co_await async_write(sock, boost::asio::buffer(&value, sizeof(value)), use_awaitable);
}

awaitable<void> recv_coroutine(tcp::socket& sock, field_t& out) {
    co_await async_read(sock, boost::asio::buffer(&out, sizeof(out)), use_awaitable);
}

awaitable<void> send_vector(tcp::socket& sock, const std::vector<field_t>& vec) {
    for (const auto& val : vec) {
        co_await send_coroutine(sock, val);
    }
}

awaitable<void> recv_vector(tcp::socket& sock, std::vector<field_t>& vec) {
    for (auto& val : vec) {
        co_await recv_coroutine(sock, val);
    }
}

// Legacy int32_t communication for backward compatibility
awaitable<void> send_coroutine_int32(tcp::socket& sock, int32_t value) {
    field_t field_val = Field::from_signed(value);
    co_await send_coroutine(sock, field_val);
}

awaitable<void> recv_coroutine_int32(tcp::socket& sock, int32_t& out) {
    field_t field_val;
    co_await recv_coroutine(sock, field_val);
    out = static_cast<int32_t>(Field::to_signed(field_val));
}

// Global parameters (read from config file or command line)
struct ProgramConfig {
    size_t m, n, k, num_queries;
    
    bool load_from_config() {
        std::ifstream config_file("data/config.txt");
        if (!config_file.is_open()) {
            std::cerr << "Error: Could not read data/config.txt" << std::endl;
            return false;
        }
        
        config_file >> m >> n >> k >> num_queries;
        config_file.close();
        return true;
    }
    
    void load_from_args(int argc, char* argv[]) {
        if (argc >= 4) {
            m = std::stoul(argv[1]);
            n = std::stoul(argv[2]);
            k = std::stoul(argv[3]);
            num_queries = (argc >= 5) ? std::stoul(argv[4]) : 5;
        } else if (!load_from_config()) {
            // Fallback defaults
            m = 10; n = 8; k = 4; num_queries = 5;
            std::cerr << "Warning: Using default dimensions" << std::endl;
        }
    }
} config;

struct QueryData {
    size_t user_id;
    std::vector<field_t> e_j_share;  // Additive share of item selection vector using field elements
};

struct PreprocessingData {
    // (2+1)-party dot product preprocessing using field elements
    field_t correction_term;  // ⟨X₀ᵀ, Y₁⟩ + α for P0, ⟨X₁ᵀ, Y₀⟩ - α for P1
    std::vector<field_t> X;   // Random masking matrix (X0 for P0, X1 for P1)
    std::vector<field_t> Y;   // Random masking vector (Y0 for P0, Y1 for P1)
};

// Request preprocessing for vectors of specific dimension
awaitable<PreprocessingData> request_preprocessing(tcp::socket& sock, size_t dimension) {
    // Send dimension request to P2
    field_t dim = static_cast<field_t>(dimension);
    co_await send_coroutine(sock, dim);
    
    // Receive preprocessing for this dimension
    PreprocessingData prep;
    prep.X.resize(dimension);
    prep.Y.resize(dimension);
    
    // Receive correction term
    co_await recv_coroutine(sock, prep.correction_term);
    
    // Receive X and Y vectors
    for (size_t i = 0; i < dimension; ++i) {
        co_await recv_coroutine(sock, prep.X[i]);
        co_await recv_coroutine(sock, prep.Y[i]);
    }
    
    co_return prep;
}

// ----------------------- Setup connections -----------------------

// Setup connection to P2 (P0/P1 act as clients, P2 acts as server)
awaitable<tcp::socket> setup_server_connection(boost::asio::io_context& io_context, tcp::resolver& resolver) {
    tcp::socket sock(io_context);
    auto endpoints_p2 = resolver.resolve("p2", "9002");
    co_await boost::asio::async_connect(sock, endpoints_p2, use_awaitable);
    // Handshake: send our role (0 for P0, 1 for P1)
    field_t role_code =
#ifdef ROLE_p0
        0
#else
        1
#endif
        ;
    co_await async_write(sock, boost::asio::buffer(&role_code, sizeof(role_code)), use_awaitable);
    co_return sock;
}

// Setup peer connection between P0 and P1
awaitable<tcp::socket> setup_peer_connection(boost::asio::io_context& io_context, tcp::resolver& resolver) {
    tcp::socket sock(io_context);

#ifdef ROLE_p0
    auto endpoints_p1 = resolver.resolve("p1", "9001");
    co_await boost::asio::async_connect(sock, endpoints_p1, use_awaitable);
#else
    tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), 9001));
    sock = co_await acceptor.async_accept(use_awaitable);
#endif
    co_return sock;
}

// ----------------------- File I/O Functions -----------------------

bool load_query_data(const std::string& filename, size_t n, QueryData& query) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Cannot open query file: " << filename << std::endl;
        return false;
    }
    
    std::string line;
    if (!std::getline(file, line)) {
        return false;
    }
    
    std::istringstream iss(line);
    iss >> query.user_id;
    
    query.e_j_share.resize(n);
    for (size_t i = 0; i < n; ++i) {
        iss >> query.e_j_share[i];
    }
    
    return true;
}

// Load all queries from file
std::vector<QueryData> load_all_queries(const std::string& filename, size_t n) {
    std::vector<QueryData> queries;
    std::ifstream file(filename);
    
    if (!file.is_open()) {
        std::cerr << "Cannot open query file: " << filename << std::endl;
        return queries;
    }
    
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        
        QueryData query;
        std::istringstream iss(line);
        iss >> query.user_id;
        
        query.e_j_share.resize(n);
        for (size_t i = 0; i < n; ++i) {
            iss >> query.e_j_share[i];
        }
        queries.push_back(query);
    }
    
    return queries;
}

// ----------------------- MPC Protocol Functions -----------------------

// Secure dot product protocol implementation
// Protocol: Compute shares of ⟨D, t⟩ where D is matrix (treated as vector here) and t is vector
awaitable<ShareField> secure_dot_product(
    tcp::socket& peer_sock,
    tcp::socket& p2_socket,  // Added P2 socket for preprocessing requests
    const ShareVectorField& D_share,  // Our share of matrix D (treated as vector for dot product)
    const ShareVectorField& t_share) { // Our share of vector t
    
    if (D_share.size() != t_share.size()) {
        throw std::runtime_error("Vector dimensions don't match");
    }
    
    size_t k = D_share.size();
    
    // Request preprocessing for this specific dimension
    PreprocessingData prep = co_await request_preprocessing(p2_socket, k);
    
    // Step 1: Compute masked shares to send using field operations
    std::vector<field_t> masked_t(k), masked_D(k);
    for (size_t i = 0; i < k; ++i) {
        masked_t[i] = Field::add(t_share[i].value, prep.Y[i]);  // t_i + Y_i
        masked_D[i] = Field::add(D_share[i].value, prep.X[i]); // D_i + X_i
    }
    
    // Step 2: Exchange masked values with peer
    std::vector<field_t> peer_masked_t(k), peer_masked_D(k);
    
    // Send our masked values
    co_await send_vector(peer_sock, masked_t);
    co_await send_vector(peer_sock, masked_D);
    
    // Receive peer's masked values
    co_await recv_vector(peer_sock, peer_masked_t);
    co_await recv_vector(peer_sock, peer_masked_D);
    
    // Step 3: Compute local share according to protocol using field arithmetic
    field_t local_result = 0;
    
#ifdef ROLE_p0
    // Alice (P0) computes: ⟨D₀, t₀ + t̃₁⟩ - ⟨Y₀, D̃₁⟩ + ⟨X₀, Y₁⟩ + α
    // Where correction_term already contains ⟨X₀, Y₁⟩ + α
    
    // Compute ⟨D₀, t₀ + t̃₁⟩ using field operations
    for (size_t i = 0; i < k; ++i) {
        field_t t_sum = Field::add(t_share[i].value, peer_masked_t[i]);
        local_result = Field::add(local_result, Field::mul(D_share[i].value, t_sum));
    }
    
    // Subtract ⟨Y₀, D̃₁⟩
    for (size_t i = 0; i < k; ++i) {
        field_t product = Field::mul(prep.Y[i], peer_masked_D[i]);
        local_result = Field::sub(local_result, product);
    }
    
    // Add correction term: ⟨X₀, Y₁⟩ + α
    local_result = Field::add(local_result, prep.correction_term);
    
#else
    // Bob (P1) computes: ⟨D₁, t₁ + t̃₀⟩ - ⟨Y₁, D̃₀⟩ + ⟨X₁, Y₀⟩ - α
    // Where correction_term already contains ⟨X₁, Y₀⟩ - α
    
    // Compute ⟨D₁, t₁ + t̃₀⟩ using field operations
    for (size_t i = 0; i < k; ++i) {
        field_t t_sum = Field::add(t_share[i].value, peer_masked_t[i]);
        local_result = Field::add(local_result, Field::mul(D_share[i].value, t_sum));
    }
    
    // Subtract ⟨Y₁, D̃₀⟩
    for (size_t i = 0; i < k; ++i) {
        field_t product = Field::mul(prep.Y[i], peer_masked_D[i]);
        local_result = Field::sub(local_result, product);
    }
    
    // Add correction term: ⟨X₁, Y₀⟩ - α
    local_result = Field::add(local_result, prep.correction_term);
#endif
    
    co_return ShareField(local_result);
}

// Secure scalar-vector multiplication using existing secure_dot_product
// Treat scalar as a vector of same dimension and compute element-wise products
awaitable<ShareVectorField> secure_scalar_vector_mult(
    tcp::socket& peer_sock,
    tcp::socket& p2_socket,           // Added P2 socket for preprocessing requests
    const ShareField& scalar_share,      // delta_i share using field element
    const ShareVectorField& vector_share)  // v_j share using field elements
{
    // Convert scalar to vector by repeating it k times
    // This allows us to compute element-wise multiplication using secure_dot_product
    size_t k = vector_share.size();
    ShareVectorField result(k);
    
    // For each dimension, compute scalar * vector[i] using secure_dot_product
    // We treat both scalar and vector[i] as 1-dimensional vectors
    for (size_t i = 0; i < k; ++i) {
        // Create 1-element vectors for dot product
        ShareVectorField scalar_vec(1);
        ShareVectorField element_vec(1);
        
        scalar_vec[0] = scalar_share;           // [delta_share]
        element_vec[0] = vector_share[i];       // [vector_share[i]]
        
        // Compute secure dot product: [delta] · [v_j[i]] = [delta * v_j[i]]
        ShareField product_result = co_await secure_dot_product(peer_sock, p2_socket, scalar_vec, element_vec);
        result[i] = product_result;
    }
    
    co_return result;
}

// Compute v_j shares using secure multiplication protocol
// v_j = V^T * e_j where V = V0 + V1 and e_j = e_j0 + e_j1 (both additive shares)
awaitable<ShareVectorField> secure_compute_vj_share(
    tcp::socket& peer_sock,
    tcp::socket& p2_socket,          // Added P2 socket for preprocessing requests
    const ShareMatrixField& V_share,
    const std::vector<field_t>& e_j_share) {
    
    size_t k = V_share.cols;  // number of features
    size_t n = V_share.rows;  // number of items
    
    if (e_j_share.size() != n) {
        throw std::runtime_error("Item selection vector size doesn't match matrix rows");
    }
    
    // Convert e_j_share to ShareVectorField format for secure dot product
    ShareVectorField e_j_additive(n);
    for (size_t i = 0; i < n; ++i) {
        e_j_additive[i].value = e_j_share[i];
    }
    
    ShareVectorField vj_result(k);
    
    // For each feature dimension, compute v_j[f] = sum_i(V[i][f] * e_j[i])
    for (size_t f = 0; f < k; ++f) {
        // Extract the f-th column of V as a vector
        ShareVectorField V_column(n);
        for (size_t i = 0; i < n; ++i) {
            V_column[i] = V_share[i][f];
        }
        
        // Compute secure dot product between V_column and e_j_additive
        ShareField column_result = co_await secure_dot_product(peer_sock, p2_socket, V_column, e_j_additive);
        vj_result[f] = column_result;
    }
    
    co_return vj_result;
}

// ----------------------- Main protocol -----------------------
awaitable<void> run(boost::asio::io_context& io_context, int argc, char* argv[]) {
    // Load configuration from config file or command line
    config.load_from_args(argc, argv);
    
    tcp::resolver resolver(io_context);
    
    // Step 1: Connect to P2 for dynamic preprocessing requests
    tcp::socket server_sock = co_await setup_server_connection(io_context, resolver);
    
    // Step 2: Load our shares of U and V matrices using field elements
    ShareMatrixField U_share(config.m, config.k), V_share(config.n, config.k);
    
#ifdef ROLE_p0
    if (!load_matrix_shares("data/U0_shares.txt", U_share) || 
        !load_matrix_shares("data/V0_shares.txt", V_share)) {
        std::cerr << "P0: Failed to load matrix shares" << std::endl;
        co_return;
    }
#else
    if (!load_matrix_shares("data/U1_shares.txt", U_share) || 
        !load_matrix_shares("data/V1_shares.txt", V_share)) {
        std::cerr << "P1: Failed to load matrix shares" << std::endl;
        co_return;
    }
#endif
    
    // Step 3: Load all queries
    std::vector<QueryData> queries;
#ifdef ROLE_p0
    queries = load_all_queries("data/queries_p0.txt", config.n);
    if (queries.empty()) {
        std::cerr << "P0: Failed to load any query data" << std::endl;
        co_return;
    }
#else
    queries = load_all_queries("data/queries_p1.txt", config.n);
    if (queries.empty()) {
        std::cerr << "P1: Failed to load any query data" << std::endl;
        co_return;
    }
#endif
    
    // Step 4: Connect to peer (P0 <-> P1)
    tcp::socket peer_sock = co_await setup_peer_connection(io_context, resolver);

    // Print initial shares
    std::cout << "\n=== Initial Shares ===" << std::endl;
    std::cout << (
#ifdef ROLE_p0
        "U0_shares (first 10 users, all features):"
#else
        "U1_shares (first 10 users, all features):"
#endif
    ) << std::endl;
    for (size_t i = 0; i < std::min(size_t(10), config.m); ++i) {
        std::cout << "User " << i << ": ";
        for (size_t j = 0; j < config.k; ++j) {
            std::cout << U_share[i][j].value << " ";
        }
        std::cout << std::endl;
    }

    std::cout << (
#ifdef ROLE_p0
        "V0_shares (first 10 items, all features):"
#else
        "V1_shares (first 10 items, all features):"
#endif
    ) << std::endl;
    for (size_t i = 0; i < std::min(size_t(10), config.n); ++i) {
        std::cout << "Item " << i << ": ";
        for (size_t j = 0; j < config.k; ++j) {
            std::cout << V_share[i][j].value << " ";
        }
        std::cout << std::endl;
    }
    std::cout << "======================" << std::endl;
    
    // Step 5: Process each query consecutively
    for (size_t query_idx = 0; query_idx < queries.size(); ++query_idx) {
        const QueryData& query = queries[query_idx];
        // Align user_id with peer to ensure both update the same row
        size_t user_id;
#ifdef ROLE_p0
        {
            field_t my_uid = static_cast<field_t>(query.user_id);
            co_await send_coroutine(peer_sock, my_uid);
            field_t peer_uid;
            co_await recv_coroutine(peer_sock, peer_uid);
            user_id = static_cast<size_t>(query.user_id); // P0 drives
            if (user_id != static_cast<size_t>(peer_uid)) {
                std::cerr << "Warning: user_id mismatch (P0=" << user_id
                          << ", P1=" << static_cast<size_t>(peer_uid) << ") — using P0's user_id\n";
            }
        }
#else
        {
            field_t peer_uid;
            co_await recv_coroutine(peer_sock, peer_uid);
            field_t my_uid = static_cast<field_t>(query.user_id);
            co_await send_coroutine(peer_sock, my_uid);
            user_id = static_cast<size_t>(peer_uid); // Follow P0
            if (static_cast<size_t>(my_uid) != user_id) {
                std::cerr << "Warning: user_id mismatch (P1=" << static_cast<size_t>(my_uid)
                          << ", P0=" << user_id << ") — using P0's user_id\n";
            }
        }
#endif
        
        // Step 5a: Compute v_j shares securely using MPC
        ShareVectorField vj_share = co_await secure_compute_vj_share(peer_sock, server_sock, V_share, query.e_j_share);
        
        // Step 5b: Compute secure dot product <u_i, v_j>
    ShareVectorField ui_share = U_share[user_id];  // Get user i's profile
        ShareField dot_product_share = co_await secure_dot_product(peer_sock, server_sock, ui_share, vj_share);
        
        // Step 5c: Compute delta = 1 - <u_i, v_j> (locally)
        ShareField delta_share;
#ifdef ROLE_p0
        delta_share.value = Field::sub(1, dot_product_share.value);  // P0 subtracts from 1
#else
        delta_share.value = Field::sub(0, dot_product_share.value);  // P1 just negates
#endif
        
        // Step 5d: Compute v_j * delta securely, then update u_i <- u_i + v_j * delta
        ShareVectorField vj_delta_share = co_await secure_scalar_vector_mult(peer_sock, server_sock, delta_share, vj_share);
        
        ShareVectorField updated_ui = ui_share;
        for (size_t f = 0; f < config.k; ++f) {
            updated_ui[f].value = Field::add(ui_share[f].value, vj_delta_share[f].value);
        }
        
        // Update the U matrix with the new user profile
        U_share[user_id] = updated_ui;
    }

    // Print final shares
    std::cout << "\n=== Final Shares ===" << std::endl;
    std::cout << (
#ifdef ROLE_p0
        "U0_shares (first 10 users, all features):"
#else
        "U1_shares (first 10 users, all features):"
#endif
    ) << std::endl;
    for (size_t i = 0; i < std::min(size_t(10), config.m); ++i) {
        std::cout << "User " << i << ": ";
        for (size_t j = 0; j < config.k; ++j) {
            std::cout << U_share[i][j].value << " ";
        }
        std::cout << std::endl;
    }

    std::cout << (
#ifdef ROLE_p0
        "V0_shares (first 10 items, all features):"
#else
        "V1_shares (first 10 items, all features):"
#endif
    ) << std::endl;
    for (size_t i = 0; i < std::min(size_t(10), config.n); ++i) {
        std::cout << "Item " << i << ": ";
        for (size_t j = 0; j < config.k; ++j) {
            std::cout << V_share[i][j].value << " ";
        }
        std::cout << std::endl;
    }
    std::cout << "=====================" << std::endl;
    
    // Step 6: Write updated shares to new files (preserve originals)
    // Note: Only save U matrices since V matrices are never updated
#ifdef ROLE_p0
    if (!save_matrix_shares("data/U0_shares_updated.txt", U_share)) {
        std::cerr << "P0: Failed to save updated U0 shares" << std::endl;
    }
    std::cout << "P0: Updated U shares saved to U0_shares_updated.txt" << std::endl;
#else
    if (!save_matrix_shares("data/U1_shares_updated.txt", U_share)) {
        std::cerr << "P1: Failed to save updated U1 shares" << std::endl;
    }
    std::cout << "P1: Updated U shares saved to U1_shares_updated.txt" << std::endl;
#endif
    
    co_return;
}

int main(int argc, char* argv[]) {
    std::cout.setf(std::ios::unitbuf); // auto-flush cout for Docker logs
    boost::asio::io_context io_context(1);
    co_spawn(io_context, run(io_context, argc, argv), boost::asio::detached);
    io_context.run();
    return 0;
}