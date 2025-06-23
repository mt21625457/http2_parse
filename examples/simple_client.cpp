#include "../http2_connection.h"
#include "../http2_frame_serializer.h"
#include <iostream>
#include <vector>

/**
 * @file simple_client.cpp
 * @brief An example demonstrating the basic usage of the http2 library for a client.
 * @brief 一个演示客户端如何基本使用http2库的示例。
 *
 * This example shows how to:
 * 1. Create an Http2Connection.
 * 2. Set up callbacks to receive parsed frames and bytes to be sent.
 * 3. Send a request (HEADERS frame).
 * 4. Process a response (SETTINGS, HEADERS, DATA, GOAWAY frames).
 *
 * Note: This example does not perform actual network I/O. It simulates the
 * interaction by feeding serialized bytes back into the connection's parser.
 *
 * 本示例展示了如何：
 * 1. 创建一个Http2Connection。
 * 2. 设置回调以接收解析后的帧和要发送的字节。
 * 3. 发送一个请求（HEADERS帧）。
 * 4. 处理一个响应（SETTINGS, HEADERS, DATA, GOAWAY帧）。
 *
 * 注意：本示例不执行实际的网络I/O。它通过将序列化后的字节反馈给
 * 连接的解析器来模拟交互。
 */

// A simple container to hold bytes that would be sent over the network.
// 一个简单的容器，用于存放本应通过网络发送的字节。
static std::vector<std::byte> network_buffer;

// Callback to handle bytes that the connection wants to send.
// In a real application, this would write to a socket.
// 用于处理连接想要发送的字节的回调。
// 在真实的应用中，这里会写入一个套接字。
void on_send_bytes(std::vector<std::byte> bytes_to_send) {
    std::cout << "[Network] Client wants to send " << bytes_to_send.size() << " bytes." << std::endl;
    network_buffer.insert(network_buffer.end(), bytes_to_send.begin(), bytes_to_send.end());
}

// Callback to handle fully parsed frames received from the (simulated) peer.
// 用于处理从（模拟的）对等方接收到的完整解析帧的回调。
void on_frame_received(const http2::AnyHttp2Frame& frame) {
    std::cout << "[Client] Received a frame of type: " << static_cast<int>(frame.type())
              << " on stream " << frame.stream_id() << std::endl;

    // Use std::visit to handle the specific frame type.
    // 使用 std::visit 来处理具体的帧类型。
    std::visit([](const auto& f) {
        using T = std::decay_t<decltype(f)>;
        if constexpr (std::is_same_v<T, http2::SettingsFrame>) {
            if (f.has_ack_flag()) {
                std::cout << "  -> It's a SETTINGS ACK frame." << std::endl;
            } else {
                std::cout << "  -> It's a SETTINGS frame with " << f.settings.size() << " settings." << std::endl;
            }
        } else if constexpr (std::is_same_v<T, http2::HeadersFrame>) {
            std::cout << "  -> It's a HEADERS frame with " << f.headers.size() << " headers." << std::endl;
            for (const auto& header : f.headers) {
                std::cout << "    " << header.name << ": " << header.value << std::endl;
            }
            if (f.has_end_stream_flag()) {
                std::cout << "    (End of stream)" << std::endl;
            }
        } else if constexpr (std::is_same_v<T, http2::DataFrame>) {
            std::cout << "  -> It's a DATA frame with " << f.data.size() << " bytes of data." << std::endl;
            // In a real client, you would process this data.
            // 在真实的客户端中，你会处理这些数据。
             if (f.has_end_stream_flag()) {
                std::cout << "    (End of stream)" << std::endl;
            }
        } else if constexpr (std::is_same_v<T, http2::GoAwayFrame>) {
            std::cout << "  -> It's a GOAWAY frame. Last stream ID: " << f.last_stream_id
                      << ", Error code: " << static_cast<uint32_t>(f.error_code) << std::endl;
        } else if constexpr (std::is_same_v<T, http2::WindowUpdateFrame>) {
            std::cout << "  -> It's a WINDOW_UPDATE frame. Increment: " << f.window_size_increment << std::endl;
        }
    }, frame.frame_variant);
}


int main() {
    std::cout << "--- HTTP/2 Client API Usage Example ---" << std::endl;

    // 1. Initialize a client connection.
    // 1. 初始化一个客户端连接。
    http2::Http2Connection client_connection(false); // false for client

    // 2. Set up the necessary callbacks.
    // 2. 设置必要的回调。
    client_connection.set_on_send_bytes(on_send_bytes);
    client_connection.set_frame_callback(on_frame_received);

    // The client must send the connection preface first. This is a magic string.
    // A real implementation would handle this, but here we just clear the buffer.
    // 客户端必须首先发送连接前言。这是一个固定的魔法字符串。
    // 真实的实现会处理它，但这里我们只是清空缓冲区。
    const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    network_buffer.assign(reinterpret_cast<const std::byte*>(preface.c_str()), reinterpret_cast<const std::byte*>(preface.c_str()) + preface.length());
    std::cout << "[Network] Sending client connection preface (" << network_buffer.size() << " bytes)." << std::endl;
    // In a real app, send `network_buffer` here and clear it.
    // 在真实应用中，在这里发送 `network_buffer` 并清空它。
    network_buffer.clear();


    // 3. Client sends its initial SETTINGS frame.
    // 3. 客户端发送其初始的SETTINGS帧。
    std::cout << "\n--- Client sending initial SETTINGS frame ---" << std::endl;
    client_connection.send_settings({}); // Sending empty settings for this example.
    // The bytes are now in `network_buffer`, ready to be sent.
    // 字节现在位于 `network_buffer` 中，准备发送。

    // --- Simulate Server Interaction ---
    // At this point, a real client would send the bytes in `network_buffer`
    // to the server and wait for a response. We will simulate this.
    // --- 模拟服务器交互 ---
    // 此时，真实的客户端会将 `network_buffer` 中的字节发送给服务器并等待响应。
    // 我们将模拟这个过程。
    std::cout << "\n--- Simulating server response ---" << std::endl;
    
    // Let's pretend the server sends its SETTINGS frame back.
    // 假设服务器发回其SETTINGS帧。
    http2::SettingsFrame server_settings;
    server_settings.header = {0, http2::FrameType::SETTINGS, 0, 0}; // Payload size will be calculated by serializer.
    // The server acknowledges our settings with an ACK frame.
    // 服务器用一个ACK帧来确认我们的设置。
    http2::SettingsFrame server_settings_ack;
    server_settings_ack.header = {0, http2::FrameType::SETTINGS, http2::SettingsFrame::ACK_FLAG, 0};

    auto server_settings_bytes = http2::FrameSerializer::serialize_settings_frame(server_settings);
    auto server_settings_ack_bytes = http2::FrameSerializer::serialize_settings_frame(server_settings_ack);

    std::cout << "[Server] Server sends its SETTINGS frame." << std::endl;
    client_connection.process_incoming_data({server_settings_bytes.data(), server_settings_bytes.size()});
    std::cout << "[Server] Server sends a SETTINGS ACK." << std::endl;
    client_connection.process_incoming_data({server_settings_ack_bytes.data(), server_settings_ack_bytes.size()});
    // Our connection should have sent an ACK back for the server's settings.
    // Let's clear the network buffer of our ACK.
    // 我们的连接应该已经为服务器的设置回送了一个ACK。我们清空网络缓冲区中的ACK。
    network_buffer.clear();


    // 4. Client sends a request on a new stream.
    // 4. 客户端在新流上发送一个请求。
    std::cout << "\n--- Client sending a GET request ---" << std::endl;
    http2::stream_id_t request_stream_id = client_connection.get_next_available_stream_id();
    std::vector<http2::HttpHeader> request_headers = {
        {":method", "GET"},
        {":scheme", "https"},
        {":authority", "example.com"},
        {":path", "/"}
    };
    client_connection.send_headers(request_stream_id, request_headers, true); // true for END_STREAM
    // Now `network_buffer` contains the HEADERS frame for the request.
    // 现在 `network_buffer` 中包含了请求的HEADERS帧。


    // --- Simulate Server Response to the Request ---
    // --- 模拟服务器对请求的响应 ---
    std::cout << "\n--- Simulating server response to GET request ---" << std::endl;
    
    // Server responds with HEADERS
    // 服务器用HEADERS帧响应
    std::vector<http2::HttpHeader> response_headers = {
        {":status", "200"},
        {"content-type", "text/plain"}
    };
    http2::HeadersFrame server_response_headers;
    server_response_headers.header = {0, http2::FrameType::HEADERS, http2::HeadersFrame::END_HEADERS_FLAG, request_stream_id};
    server_response_headers.headers = response_headers;
    // This is complex as it needs an encoder. We'll simplify and just construct the frame object
    // to feed it to the connection, rather than serializing and parsing. For a real test,
    // you would need a server-side HpackEncoder.
    // 
    // Instead of serializing, we will manually create an AnyHttp2Frame and call the internal handler
    // to simulate parsing. This is a hack for the example's simplicity.
    // 为了简化，我们将手动创建AnyHttp2Frame并调用内部处理程序来模拟解析，而不是序列化。
    http2::AnyHttp2Frame server_headers_anyframe(server_response_headers);
    client_connection.handle_parsed_frame(server_headers_anyframe);


    // Server sends some DATA
    // 服务器发送一些DATA
    http2::DataFrame server_data_frame;
    server_data_frame.header = {0, http2::FrameType::DATA, http2::DataFrame::END_STREAM_FLAG, request_stream_id};
    std::string body = "Hello, world!";
    server_data_frame.data.assign(reinterpret_cast<const std::byte*>(body.c_str()), reinterpret_cast<const std::byte*>(body.c_str()) + body.length());
    http2::AnyHttp2Frame server_data_anyframe(server_data_frame);
    client_connection.handle_parsed_frame(server_data_anyframe);

    std::cout << "\n--- Example Finished ---" << std::endl;

    return 0;
} 