#include "common.hpp"
#include <boost/asio.hpp>
#include <iostream>
#include <random>
#include <fstream>
#include <unordered_map>

using boost::asio::ip::tcp;

// Correlated preprocessing bundles shared between P0 and P1
struct PreprocessBundle {
    std::vector<field_t> X0, X1, Y0, Y1;
    field_t alpha{0};
    bool served_p0{false};
    bool served_p1{false};
};

#include <deque>
#include <memory>

// Pending bundles per dimension; accessed on the same io_context thread (no locks needed)
static std::unordered_map<size_t, std::deque<std::shared_ptr<PreprocessBundle>>> pending_by_dim;

static std::shared_ptr<PreprocessBundle> generate_bundle(size_t dimension) {
    auto b = std::make_shared<PreprocessBundle>();
    b->X0.resize(dimension);
    b->X1.resize(dimension);
    b->Y0.resize(dimension);
    b->Y1.resize(dimension);
    b->alpha = Field::small_random();
    for (size_t i = 0; i < dimension; ++i) {
        b->X0[i] = Field::small_random();
        b->X1[i] = Field::small_random();
        b->Y0[i] = Field::small_random();
        b->Y1[i] = Field::small_random();
    }
    return b;
}

// Handle preprocessing requests from P0/P1 using field elements
boost::asio::awaitable<void> handle_client(tcp::socket socket, bool is_p0) {
    try {
        while (true) {
            // Receive dimension request as field element
            field_t requested_dimension;
            co_await boost::asio::async_read(socket, 
                boost::asio::buffer(&requested_dimension, sizeof(requested_dimension)), 
                boost::asio::use_awaitable);
            
            size_t dim = static_cast<size_t>(requested_dimension);
            auto& q = pending_by_dim[dim];
            std::shared_ptr<PreprocessBundle> bundle;
            
            if (is_p0) {
                // If there is a bundle waiting for P0 (created by P1 earlier), use it; else create a new one
                if (!q.empty() && !q.front()->served_p0 && q.front()->served_p1) {
                    bundle = q.front();
                    bundle->served_p0 = true;
                    // Will pop after sending if both served
                } else {
                    bundle = generate_bundle(dim);
                    bundle->served_p0 = true;
                    bundle->served_p1 = false;
                    q.push_back(bundle);
                }
                // Compute and send P0's view: X0, Y0, corr = <X0,Y1> + alpha
                field_t corr = 0;
                for (size_t i = 0; i < dim; ++i) {
                    corr = Field::add(corr, Field::mul(bundle->X0[i], bundle->Y1[i]));
                }
                corr = Field::add(corr, bundle->alpha);
                co_await boost::asio::async_write(socket, boost::asio::buffer(&corr, sizeof(corr)), boost::asio::use_awaitable);
                for (size_t i = 0; i < dim; ++i) {
                    co_await boost::asio::async_write(socket, boost::asio::buffer(&bundle->X0[i], sizeof(field_t)), boost::asio::use_awaitable);
                    co_await boost::asio::async_write(socket, boost::asio::buffer(&bundle->Y0[i], sizeof(field_t)), boost::asio::use_awaitable);
                }
                if (bundle->served_p0 && bundle->served_p1) {
                    q.pop_front();
                }
            } else {
                // P1
                if (!q.empty() && q.front()->served_p0 && !q.front()->served_p1) {
                    bundle = q.front();
                    bundle->served_p1 = true;
                } else {
                    bundle = generate_bundle(dim);
                    bundle->served_p0 = false;
                    bundle->served_p1 = true;
                    q.push_back(bundle);
                }
                // Compute and send P1's view: X1, Y1, corr = <X1,Y0> - alpha
                field_t corr = 0;
                for (size_t i = 0; i < dim; ++i) {
                    corr = Field::add(corr, Field::mul(bundle->X1[i], bundle->Y0[i]));
                }
                corr = Field::sub(corr, bundle->alpha);
                co_await boost::asio::async_write(socket, boost::asio::buffer(&corr, sizeof(corr)), boost::asio::use_awaitable);
                for (size_t i = 0; i < dim; ++i) {
                    co_await boost::asio::async_write(socket, boost::asio::buffer(&bundle->X1[i], sizeof(field_t)), boost::asio::use_awaitable);
                    co_await boost::asio::async_write(socket, boost::asio::buffer(&bundle->Y1[i], sizeof(field_t)), boost::asio::use_awaitable);
                }
                if (bundle->served_p0 && bundle->served_p1) {
                    q.pop_front();
                }
            }
        }
    } catch (std::exception& e) {
        // Client disconnected
    }
}

// Run multiple coroutines in parallel
template <typename... Fs>
void run_in_parallel(boost::asio::io_context& io, Fs&&... funcs) {
    (boost::asio::co_spawn(io, funcs, boost::asio::detached), ...);
}

int main() {
    try {
        boost::asio::io_context io_context;

    tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), 9002));

    // Accept clients and identify roles via handshake
    tcp::socket sA(io_context);
    acceptor.accept(sA);
    field_t roleA = 9;
    boost::asio::read(sA, boost::asio::buffer(&roleA, sizeof(roleA)));

    tcp::socket sB(io_context);
    acceptor.accept(sB);
    field_t roleB = 9;
    boost::asio::read(sB, boost::asio::buffer(&roleB, sizeof(roleB)));

    tcp::socket socket_p0 = (roleA == 0 ? std::move(sA) : std::move(sB));
    tcp::socket socket_p1 = (roleA == 0 ? std::move(sB) : std::move(sA));

        // Launch client handlers in parallel - they will keep listening for requests
        run_in_parallel(io_context,
            [&]() -> boost::asio::awaitable<void> {
                co_await handle_client(std::move(socket_p0), true);  // P0
            },
            [&]() -> boost::asio::awaitable<void> {
                co_await handle_client(std::move(socket_p1), false); // P1
            }
        );

        io_context.run();

    } catch (std::exception& e) {
        std::cerr << "Exception in P2: " << e.what() << "\n";
    }
}
