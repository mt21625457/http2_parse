# http2_parse: C++ HTTP/2 帧解析与连接管理库

`http2_parse` 是一个使用现代 C++ (C++20/23) 编写的、专注于 HTTP/2 协议底层帧处理的轻量级库。它提供了一个健壮的解析器、连接状态管理器以及帧序列化工具，旨在为构建高性能的 HTTP/2 客户端或服务器应用程序提供坚实的基础。

## 核心特性

- **纯粹的协议层实现**: 不涉及任何 I/O 操作（如 socket 读写），使其可以轻松集成到任何网络模型中（同步、异步、事件驱动等）。
- **面向帧的解析**: 提供一个回调驱动的 `Http2Parser`，可以逐帧解析二进制流，并上报结构化的帧数据。
- **连接与流状态管理**: `Http2Connection` 类负责管理连接级别的状态，包括设置（SETTINGS）、流的创建与销毁、流量控制窗口等。
- **HPACK 头压缩**: 集成了 HPACK 解码器（`HpackDecoder`），能够处理 `HEADERS` 和 `CONTINUATION` 帧的拼接与头信息的解析。
- **帧序列化**: 提供 `Http2FrameSerializer`，可将结构化的帧对象序列化为二进制字节流，用于发送。
- **现代 C++ 设计**: 大量使用 `std::span`, `std::variant`, `std::optional` 等现代 C++ 特性，确保类型安全和高性能。

## 项目结构

```
http2_parse/
├── src/                  # 核心库源代码
│   ├── http2_parser.h/.cpp       # HTTP/2 帧解析器
│   ├── http2_connection.h/.cpp   # HTTP/2 连接状态管理器
│   ├── http2_frame.h/.cpp        # 所有 HTTP/2 帧类型的定义
│   ├── http2_frame_serializer.h/.cpp # 帧序列化器
│   └── hpack_*.h/.cpp            # HPACK 头压缩相关实现
├── examples/             # 示例程序
│   ├── serialize_request.cpp   # 演示如何使用 API 构造并序列化一个复杂的请求流
│   └── parse_request.cpp       # 演示如何读取二进制流并使用解析器进行解析
├── tests/                # 单元测试
├── lib/                  # 第三方依赖（当前为空，未来可放置如 nghttp2 等）
└── CMakeLists.txt        # CMake 构建脚本
```

## 如何编译

本项目使用 CMake 进行构建。我们提供了一个便捷的 macOS 构建脚本。

```bash
# 1. 确保构建脚本有执行权限
chmod +x build_osx.sh

# 2. 执行构建脚本
./build_osx.sh
```
编译成功后，所有的可执行文件（包括示例程序和测试）都将位于 `cmake-build-release` 目录下。

## 如何使用示例程序

我们提供了一对可以互相验证的示例程序，用于展示库的核心功能。

1.  **`serialize_request`**:
    这个程序使用 `Http2Connection` 和 `Http2FrameSerializer` 的高级 API 来模拟客户端行为，生成一个包含多种帧类型（SETTINGS, HEADERS, DATA, PING, WINDOW_UPDATE）的复杂 HTTP/2 请求流，并将其保存到二进制文件 `http2_request_complex.bin` 中。

2.  **`parse_request`**:
    这个程序读取 `http2_request_complex.bin` 文件，并使用 `Http2Parser` 和 `Http2Connection` 来解析其中的数据。它通过注册回调函数来打印出每个成功解析的帧的信息。

**运行验证流程:**

构建脚本 `./build_osx.sh` 会自动编译并按顺序执行这两个示例程序，您可以直接观察其输出，以验证序列化和解析逻辑的正确性。

```bash
# 脚本会自动执行以下命令
./cmake-build-release/serialize_request
./cmake-build-release/parse_request
```

## 设计理念

### 解耦 I/O
本库的核心设计原则之一是将协议处理逻辑与网络 I/O 完全分离。库的使用者负责从网络读取数据，并将字节流（`std::span<const std::byte>`）喂给 `Http2Parser`。同样地，当需要发送数据时，使用者调用 `Http2FrameSerializer` 将帧对象转换为字节流，然后自行通过网络发送出去。

这种设计带来了极大的灵活性，使得 `http2_parse` 可以被用于任何网络编程框架。

### 状态管理
- **`Http2Parser`**: 只负责无状态的帧边界识别和基础解析。它不知道任何关于连接或流的状态。
- **`Http2Connection`**: 是状态管理的核心。它持有 `Http2Parser` 的实例，并响应解析器上报的帧。它负责：
    - 响应 `SETTINGS` 帧并更新本地和远程的配置。
    - 管理所有流（Stream）的生命周期和状态（idle, open, closed 等）。
    - 处理流量控制窗口 (`WINDOW_UPDATE`)。
    - 拼接跨越多个 `CONTINUATION` 帧的 `HEADERS` 块，并调用 HPACK 解码器。
    - 根据接收到的帧和当前状态，决定要采取的行动（例如，收到 PING 就准备返回 PONG，收到无效帧就准备发送 `GOAWAY` 或 `RST_STREAM`）。
