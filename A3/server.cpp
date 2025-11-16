#include "common.hpp"
#include "dpf.hpp"

#include <filesystem>
#include <iostream>

using namespace std::string_literals;

struct QueryEntry {
    uint32_t user_id{0};
    dpf::DPFKey key;
};

std::vector<QueryEntry> load_queries(const std::string& filename, size_t expected_domain) {
    std::vector<QueryEntry> queries;
    std::ifstream in(filename);
    if (!in.is_open()) {
        std::cerr << "Failed to open query file: " << filename << '\n';
        return queries;
    }
    uint32_t count = 0;
    size_t domain = 0;
    if (!(in >> count >> domain)) {
        std::cerr << "Malformed query file header" << std::endl;
        return queries;
    }
    if (domain != expected_domain) {
        std::cerr << "Warning: query domain " << domain << " != expected " << expected_domain << std::endl;
    }
    queries.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        QueryEntry entry;
        if (!(in >> entry.user_id)) {
            std::cerr << "Malformed query file" << std::endl;
            queries.clear();
            return queries;
        }
        if (!dpf::deserialize_key_text(in, entry.key)) {
            std::cerr << "Malformed DPF key in query file" << std::endl;
            queries.clear();
            return queries;
        }
        queries.push_back(entry);
    }
    return queries;
}

awaitable<tcp::socket> connect_to_p2(boost::asio::io_context& io, tcp::resolver& resolver, bool is_p0) {
    auto endpoints = resolver.resolve("p2", "9002");
    tcp::socket socket(io);
    co_await boost::asio::async_connect(socket, endpoints, use_awaitable);
    field_t role = is_p0 ? 0 : 1;
    co_await boost::asio::async_write(socket, boost::asio::buffer(&role, sizeof(role)), use_awaitable);
    co_return socket;
}

awaitable<tcp::socket> connect_peers(boost::asio::io_context& io, bool is_p0) {
    if (is_p0) {
        tcp::resolver resolver(io);
        auto endpoints = resolver.resolve("p1", "9001");
        tcp::socket socket(io);
        co_await boost::asio::async_connect(socket, endpoints, use_awaitable);
        co_return socket;
    } else {
        tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 9001));
        tcp::socket socket = co_await acceptor.async_accept(use_awaitable);
        co_return socket;
    }
}

awaitable<void> run(boost::asio::io_context& io) {
    ProgramConfig config;
    if (!config.load("data/config.txt")) {
        co_return;
    }

#ifdef ROLE_p0
    constexpr bool is_p0 = true;
    const std::string role_name = "P0";
#else
    constexpr bool is_p0 = false;
    const std::string role_name = "P1";
#endif

    ShareVectorField U(config.num_users), V(config.num_items);
#ifdef ROLE_p0
    if (!load_vector_shares("data/U0_shares.txt", U) || !load_vector_shares("data/V0_shares.txt", V)) {
        std::cerr << "Failed to load share files for P0\n";
        co_return;
    }
#else
    if (!load_vector_shares("data/U1_shares.txt", U) || !load_vector_shares("data/V1_shares.txt", V)) {
        std::cerr << "Failed to load share files for P1\n";
        co_return;
    }
#endif

    auto queries = load_queries(is_p0 ? "data/queries_p0.txt" : "data/queries_p1.txt", config.num_items);
    if (queries.empty()) {
        std::cerr << role_name << ": no queries to process" << std::endl;
        co_return;
    }

    tcp::resolver resolver(io);
    tcp::socket p2_socket = co_await connect_to_p2(io, resolver, is_p0);
    tcp::socket peer_socket = co_await connect_peers(io, is_p0);

    std::cout << role_name << ": starting query processing for " << queries.size() << " queries\n";

    for (size_t q = 0; q < queries.size(); ++q) {
        const auto& qr = queries[q];
        size_t user_idx = qr.user_id % config.num_users;
        ShareField ui_share = U[user_idx];

        auto indicator_xor = dpf::eval_full(qr.key);
        if (indicator_xor.size() != config.num_items) {
            std::cerr << role_name << ": domain mismatch for indicator vector" << std::endl;
            break;
        }
        auto indicator_add = co_await convert_xor_to_additive(peer_socket, indicator_xor, is_p0);

        ShareVectorField indicator(config.num_items);
        for (size_t i = 0; i < config.num_items; ++i) {
            indicator[i].value = indicator_add[i];
        }

        ShareField vj_share = co_await secure_dot_product(peer_socket, p2_socket, V, indicator, is_p0);
        ShareField dot_share = co_await secure_multiplication(peer_socket, p2_socket, ui_share, vj_share, is_p0);

        ShareField delta_share;
        if (is_p0) {
            delta_share.value = Field::sub(1, dot_share.value);
        } else {
            delta_share.value = Field::sub(0, dot_share.value);
        }

        ShareField M_share = co_await secure_multiplication(peer_socket, p2_socket, ui_share, delta_share, is_p0);

        // Securely scale the secret indicator vector by the scalar update share.
        for (size_t i = 0; i < config.num_items; ++i) {
            ShareField delta_entry = indicator[i];
            ShareField update_share = co_await secure_multiplication(
                peer_socket, p2_socket, delta_entry, M_share, is_p0);
            V[i].value = Field::add(V[i].value, update_share.value);
        }

        if ((q + 1) % 1 == 0) {
            std::cout << role_name << ": processed query " << (q + 1) << '/' << queries.size() << std::endl;
        }
    }

#ifdef ROLE_p0
    save_vector_shares("data/V0_shares_updated.txt", V);
#else
    save_vector_shares("data/V1_shares_updated.txt", V);
#endif

    std::cout << role_name << ": completed all queries" << std::endl;
}

int main() {
    std::cout.setf(std::ios::unitbuf);
    boost::asio::io_context io(1);
    co_spawn(io, run(io), detached);
    io.run();
    return 0;
}
