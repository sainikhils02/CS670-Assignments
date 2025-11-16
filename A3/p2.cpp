#include "common.hpp"

#include <deque>
#include <memory>
#include <unordered_map>

using boost::asio::ip::tcp;

struct PreprocessBundle {
    std::vector<field_t> X0, X1, Y0, Y1;
    field_t alpha{0};
    bool served_p0{false};
    bool served_p1{false};
};

static std::unordered_map<size_t, std::deque<std::shared_ptr<PreprocessBundle>>> pending_by_dim;

std::shared_ptr<PreprocessBundle> generate_bundle(size_t dimension) {
    auto bundle = std::make_shared<PreprocessBundle>();
    bundle->X0.resize(dimension);
    bundle->X1.resize(dimension);
    bundle->Y0.resize(dimension);
    bundle->Y1.resize(dimension);
    bundle->alpha = Field::small_random();
    for (size_t i = 0; i < dimension; ++i) {
        bundle->X0[i] = Field::small_random();
        bundle->X1[i] = Field::small_random();
        bundle->Y0[i] = Field::small_random();
        bundle->Y1[i] = Field::small_random();
    }
    return bundle;
}

awaitable<void> handle_client(tcp::socket socket, bool is_p0) {
    try {
        for (;;) {
            field_t dim_field = 0;
            co_await recv_field(socket, dim_field);
            size_t dim = static_cast<size_t>(dim_field);
            auto& queue = pending_by_dim[dim];
            std::shared_ptr<PreprocessBundle> bundle;

            if (is_p0) {
                if (!queue.empty() && queue.front()->served_p1 && !queue.front()->served_p0) {
                    bundle = queue.front();
                    bundle->served_p0 = true;
                } else {
                    bundle = generate_bundle(dim);
                    bundle->served_p0 = true;
                    queue.push_back(bundle);
                }
                field_t corr = 0;
                for (size_t i = 0; i < dim; ++i) {
                    corr = Field::add(corr, Field::mul(bundle->X0[i], bundle->Y1[i]));
                }
                corr = Field::add(corr, bundle->alpha);
                co_await send_field(socket, corr);
                co_await send_vector(socket, bundle->X0);
                co_await send_vector(socket, bundle->Y0);
            } else {
                if (!queue.empty() && queue.front()->served_p0 && !queue.front()->served_p1) {
                    bundle = queue.front();
                    bundle->served_p1 = true;
                } else {
                    bundle = generate_bundle(dim);
                    bundle->served_p1 = true;
                    queue.push_back(bundle);
                }
                field_t corr = 0;
                for (size_t i = 0; i < dim; ++i) {
                    corr = Field::add(corr, Field::mul(bundle->X1[i], bundle->Y0[i]));
                }
                corr = Field::sub(corr, bundle->alpha);
                co_await send_field(socket, corr);
                co_await send_vector(socket, bundle->X1);
                co_await send_vector(socket, bundle->Y1);
            }

            if (bundle->served_p0 && bundle->served_p1) {
                if (!queue.empty() && queue.front() == bundle) {
                    queue.pop_front();
                }
            }
        }
    } catch (...) {
    }
}

int main() {
    try {
        boost::asio::io_context io;
        tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 9002));

        tcp::socket first(io);
        acceptor.accept(first);
        field_t role_first = 0;
        boost::asio::read(first, boost::asio::buffer(&role_first, sizeof(role_first)));

        tcp::socket second(io);
        acceptor.accept(second);
        field_t role_second = 0;
        boost::asio::read(second, boost::asio::buffer(&role_second, sizeof(role_second)));

        tcp::socket socket_p0 = (role_first == 0) ? std::move(first) : std::move(second);
        tcp::socket socket_p1 = (role_first == 0) ? std::move(second) : std::move(first);

        co_spawn(io, handle_client(std::move(socket_p0), true), detached);
        co_spawn(io, handle_client(std::move(socket_p1), false), detached);

        io.run();
    } catch (const std::exception& ex) {
        std::cerr << "P2 exception: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
