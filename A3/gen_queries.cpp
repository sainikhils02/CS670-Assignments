#include "common.hpp"
#include "dpf.hpp"

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

struct QueryRecord {
    uint32_t user_id{0};
    dpf::DPFKey key;
};

bool write_queries(const std::string& filename, const std::vector<QueryRecord>& queries, size_t domain_size) {
    std::ofstream out(filename);
    if (!out.is_open()) {
        std::cerr << "Failed to open " << filename << " for writing" << std::endl;
        return false;
    }
    uint32_t count = static_cast<uint32_t>(queries.size());
    out << count << ' ' << domain_size << '\n';
    for (const auto& q : queries) {
        out << q.user_id << '\n';
        if (!dpf::serialize_key_text(out, q.key)) {
            std::cerr << "Failed to serialize DPF key to " << filename << std::endl;
            return false;
        }
        out << '\n';
    }
    return true;
}

bool is_power_of_two(size_t x) {
    return x && ((x & (x - 1)) == 0);
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <num_users> <num_items> <num_queries>" << std::endl;
        return 1;
    }

    const size_t num_users = std::stoul(argv[1]);
    const size_t num_items = std::stoul(argv[2]);
    const size_t num_queries = std::stoul(argv[3]);

    if (!is_power_of_two(num_items)) {
        std::cerr << "num_items must be a power of two" << std::endl;
        return 1;
    }

    fs::create_directories("data");

    ShareVectorField U0(num_users), U1(num_users), V0(num_items), V1(num_items);
    for (size_t i = 0; i < num_users; ++i) {
        field_t secret = Field::random();
        field_t share0 = Field::random();
        field_t share1 = Field::sub(secret, share0);
        U0[i].value = share0;
        U1[i].value = share1;
    }
    for (size_t i = 0; i < num_items; ++i) {
        field_t secret = Field::random();
        field_t share0 = Field::random();
        field_t share1 = Field::sub(secret, share0);
        V0[i].value = share0;
        V1[i].value = share1;
    }

    if (!save_vector_shares("data/U0_shares.txt", U0) ||
        !save_vector_shares("data/U1_shares.txt", U1) ||
        !save_vector_shares("data/V0_shares.txt", V0) ||
        !save_vector_shares("data/V1_shares.txt", V1)) {
        std::cerr << "Failed to save share files" << std::endl;
        return 1;
    }

    {
        std::ofstream cfg("data/config.txt");
        if (!cfg.is_open()) {
            std::cerr << "Failed to write config file" << std::endl;
            return 1;
        }
        cfg << num_users << ' ' << num_items << ' ' << num_queries << '\n';
    }

    std::random_device rd;
    std::seed_seq seed{rd(), rd(), rd(), rd(), rd(), rd()};
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<size_t> user_dist(0, num_users - 1);
    std::uniform_int_distribution<size_t> item_dist(0, num_items - 1);

    std::vector<QueryRecord> queries_p0, queries_p1;
    queries_p0.reserve(num_queries);
    queries_p1.reserve(num_queries);

    for (size_t q = 0; q < num_queries; ++q) {
        size_t user_id = user_dist(rng);
        size_t item_index = item_dist(rng);
        auto keys = dpf::generate(num_items, item_index, 1u, rng);

        QueryRecord rec0;
        rec0.user_id = static_cast<uint32_t>(user_id);
        rec0.key = keys.k0;
        queries_p0.push_back(rec0);

        QueryRecord rec1;
        rec1.user_id = static_cast<uint32_t>(user_id);
        rec1.key = keys.k1;
        queries_p1.push_back(rec1);
    }

    if (!write_queries("data/queries_p0.txt", queries_p0, num_items) ||
        !write_queries("data/queries_p1.txt", queries_p1, num_items)) {
        return 1;
    }

    std::cout << "Generated " << num_queries << " queries for " << num_users
              << " users and " << num_items << " items" << std::endl;
    return 0;
}
