#include "http2_connection.h"
#include <iostream>
#include <vector>
#include <fstream>
#include <string>
#include <numeric>
#include <algorithm>

/**
 * @file serialize_request.cpp
 * @brief Example for serializing a complex HTTP/2 stream using the Http2Connection API.
 * @brief 使用Http2Connection API序列化复杂HTTP/2流的示例。
 *
 * This program uses the high-level API of the Http2Connection class to generate a complex stream,
 * relying on the class to handle frame splitting (for large headers and data), stream management,
 * and flow control. The output file can be parsed by `parse_request.cpp`.
 *
 * 本程序使用Http2Connection类的高级API来生成一个复杂的流，依赖该类来处理帧的分割（针对大头部和大数据）、
 * 流管理和流量控制。输出文件可由`parse_request.cpp`解析。
 */
int main() {
    std::cout << "--- API-driven Complex HTTP/2 Stream Serialization ---" << std::endl;

    std::vector<std::byte> serialized_output;
    auto on_send_cb = [&](std::vector<std::byte> bytes_to_send) {
        std::cout << "[Callback] Capturing " << bytes_to_send.size() << " bytes to send." << std::endl;
        serialized_output.insert(serialized_output.end(), bytes_to_send.begin(), bytes_to_send.end());
    };

    // 1. Setup a client connection with the callback.
    http2::Http2Connection client_connection(false); // false for client
    client_connection.set_on_send_bytes(on_send_cb);

    // 2. Prepend connection preface.
    const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    serialized_output.assign(reinterpret_cast<const std::byte*>(preface.c_str()),
                             reinterpret_cast<const std::byte*>(preface.c_str()) + preface.length());
    std::cout << "1. Prepended " << preface.length() << " bytes of connection preface." << std::endl;

    // 3. Send initial SETTINGS frame.
    std::cout << "2. Sending initial SETTINGS frame..." << std::endl;
    client_connection.send_settings({});

    // --- Stream 1: Large Headers (HEADERS + CONTINUATION) & Large Data ---
    std::cout << "\n--- Stream 1: Large Headers and Data ---" << std::endl;
    http2::stream_id_t stream1_id = client_connection.get_next_available_stream_id();

    // 4. Create and send a large header block to force continuation.
    std::cout << "3. Sending large HEADERS frame for stream " << stream1_id << "..." << std::endl;
    std::vector<http2::HttpHeader> large_headers = {
        {":method", "POST"}, {":scheme", "https"}, {":authority", "api.example.com"}, {":path", "/large_upload"}
    };
    for (int i = 0; i < 500; ++i) {
        large_headers.push_back({"x-custom-header-" + std::to_string(i), std::string(200, 'x')});
    }
    // send_headers API should handle splitting into HEADERS + CONTINUATION
    client_connection.send_headers(stream1_id, large_headers, false); // end_stream = false

    // 5. Intersperse a PING frame
    std::cout << "\n4. Sending a PING frame..." << std::endl;
    std::array<std::byte, 8> ping_payload;
    ping_payload.fill(std::byte{0xAB});
    client_connection.send_ping(ping_payload, false); // false = not an ack

    // 6. Send a large DATA payload for Stream 1.
    std::cout << "5. Sending large DATA payload for Stream 1..." << std::endl;
    std::vector<uint8_t> large_data_uint(30000);
    std::iota(large_data_uint.begin(), large_data_uint.end(), 0);
    std::vector<std::byte> large_data(large_data_uint.size());
    std::transform(large_data_uint.begin(), large_data_uint.end(), large_data.begin(), [](uint8_t v) { return std::byte{v}; });

    // send_data API should handle splitting into multiple DATA frames
    client_connection.send_data(stream1_id, large_data, true); // end_stream = true

    // --- Stream 3: A second, smaller request ---
    std::cout << "\n--- Stream 3: A second concurrent request ---" << std::endl;
    http2::stream_id_t stream3_id = client_connection.get_next_available_stream_id();

    // 7. HEADERS for stream 3
    std::cout << "6. Sending HEADERS for stream " << stream3_id << "..." << std::endl;
    client_connection.send_headers(stream3_id, {{":method", "GET"}, {":path", "/status"}}, false); // end_stream = false

    // 8. Send a WINDOW_UPDATE frame
    std::cout << "7. Sending a WINDOW_UPDATE frame..." << std::endl;
    client_connection.send_window_update_action(0, 100000); // For connection

    // 9. DATA for stream 3
    std::cout << "8. Sending DATA for stream " << stream3_id << "..." << std::endl;
    std::string s3_body = "ping";
    std::vector<std::byte> s3_body_bytes(s3_body.size());
    std::transform(s3_body.begin(), s3_body.end(), s3_body_bytes.begin(), [](char c){ return std::byte(c); });
    client_connection.send_data(stream3_id, s3_body_bytes, true); // end_stream = true

    // 10. Write to file
    const std::string filename = "http2_request_complex.bin";
    std::cout << "\n9. Writing a total of " << serialized_output.size() << " bytes to " << filename << std::endl;
    std::ofstream output_file(filename, std::ios::binary);
    if (!output_file) {
        std::cerr << "Error: Could not open file " << filename << " for writing." << std::endl;
        return 1;
    }
    output_file.write(reinterpret_cast<const char*>(serialized_output.data()), serialized_output.size());
    output_file.close();

    std::cout << "\nAPI-driven complex serialization complete." << std::endl;

    return 0;
} 