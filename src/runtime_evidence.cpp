#include "neuralpass/runtime_evidence.hpp"

#include <iomanip>
#include <sstream>

namespace neuralpass {
namespace {

std::string quoted(std::string_view value) {
    std::ostringstream output;
    output << '"';
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (ch < 0x20) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned int>(ch) << std::dec;
            } else {
                output << static_cast<char>(ch);
            }
            break;
        }
    }
    output << '"';
    return output.str();
}

const char *boolean(bool value) { return value ? "true" : "false"; }

} // namespace

std::string runtime_display_evidence_json(const RuntimeDisplayEvidence &value) {
    std::ostringstream output;
    output << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"generated_utc\": " << quoted(value.generated_utc) << ",\n"
           << "  \"process_architecture\": " << quoted(value.process_architecture) << ",\n"
           << "  \"graphics_api\": " << quoted(value.graphics_api) << ",\n"
           << "  \"graphics_api_value\": " << value.graphics_api_value << ",\n"
           << "  \"width\": " << value.width << ",\n"
           << "  \"height\": " << value.height << ",\n"
           << "  \"backbuffer_format\": " << quoted(value.backbuffer_format) << ",\n"
           << "  \"backbuffer_format_value\": " << value.backbuffer_format_value << ",\n"
           << "  \"swapchain_color_space\": " << quoted(value.swapchain_color_space) << ",\n"
           << "  \"swapchain_color_space_value\": " << value.swapchain_color_space_value << ",\n"
           << "  \"display_capture_supported\": "
           << boolean(value.display_capture_supported) << ",\n"
           << "  \"display_encoding\": " << quoted(value.display_encoding) << ",\n"
           << "  \"hdr_path\": " << boolean(value.hdr_path) << ",\n"
           << "  \"classification\": " << quoted(value.classification) << "\n"
           << "}\n";
    return output.str();
}

} // namespace neuralpass
