#pragma once

#include <cstddef>
#include <cstdint>

namespace neuralpass::worker {

inline constexpr std::uint32_t k_magic = 0x57504c4e; // "NLPW" little-endian
inline constexpr std::uint16_t k_version = 1;
inline constexpr std::size_t k_shared_bytes = 8u * 1024u * 1024u;
inline constexpr std::uint32_t k_max_dimension = 640;

enum class MessageType : std::uint16_t {
    hello = 1,
    hello_ack = 2,
    infer = 3,
    infer_done = 4,
    shutdown = 5,
    error = 6,
};

#pragma pack(push, 1)
struct Message {
    std::uint32_t magic = k_magic;
    std::uint16_t version = k_version;
    MessageType type = MessageType::error;
    std::uint32_t size = sizeof(Message);
    std::uint64_t request = 0;
    std::uint64_t scene_generation = 0;
    std::uint64_t style_generation = 0;
    std::uint64_t binding_generation = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t payload_bytes = 0;
    std::uint32_t status = 0;
};
#pragma pack(pop)

static_assert(sizeof(Message) == 60);

[[nodiscard]] inline bool valid(const Message &message) noexcept {
    return message.magic == k_magic && message.version == k_version &&
           message.size == sizeof(Message);
}

[[nodiscard]] inline bool valid_image(const Message &message) noexcept {
    if (message.width == 0 || message.height == 0 ||
        message.width > k_max_dimension || message.height > k_max_dimension)
        return false;
    const auto pixels = static_cast<std::uint64_t>(message.width) * message.height;
    const auto bytes = pixels * 4u * sizeof(float);
    return bytes <= k_shared_bytes && message.payload_bytes == bytes;
}

[[nodiscard]] inline bool matches_inference(const Message &request,
                                            const Message &response) noexcept {
    return valid(request) && valid(response) &&
           request.type == MessageType::infer &&
           response.type == MessageType::infer_done && response.status == 0 &&
           response.request == request.request &&
           response.scene_generation == request.scene_generation &&
           response.style_generation == request.style_generation &&
           response.binding_generation == request.binding_generation &&
           response.width == request.width && response.height == request.height &&
           response.payload_bytes == request.payload_bytes;
}

} // namespace neuralpass::worker
