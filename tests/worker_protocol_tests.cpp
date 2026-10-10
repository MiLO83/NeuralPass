#include "neuralpass/worker_protocol.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        using namespace neuralpass::worker;
        Message request;
        request.type = MessageType::infer;
        request.request = 7;
        request.scene_generation = 11;
        request.style_generation = 13;
        request.binding_generation = 17;
        request.width = 320;
        request.height = 320;
        request.payload_bytes = 320 * 320 * 4 * sizeof(float);
        require(valid_image(request), "valid bounded tile rejected");
        Message response = request;
        response.type = MessageType::infer_done;
        require(matches_inference(request, response), "matching response rejected");

        auto reject_mutation = [&](auto mutate, const char *message) {
            auto stale = response;
            mutate(stale);
            require(!matches_inference(request, stale), message);
        };
        reject_mutation([](Message &m) { ++m.request; }, "stale request accepted");
        reject_mutation([](Message &m) { ++m.scene_generation; }, "stale scene accepted");
        reject_mutation([](Message &m) { ++m.style_generation; }, "stale style accepted");
        reject_mutation([](Message &m) { ++m.binding_generation; }, "stale binding accepted");
        reject_mutation([](Message &m) { ++m.width; }, "wrong dimensions accepted");
        reject_mutation([](Message &m) { m.status = 1; }, "failed response accepted");

        request.width = k_max_dimension + 1;
        request.payload_bytes = request.width * request.height * 4 * sizeof(float);
        require(!valid_image(request), "oversized tile accepted");
        std::cout << "Worker protocol tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Worker protocol tests failed: " << error.what() << '\n';
        return 1;
    }
}
