#include "http2_frame.h"

// Most of the frame logic is in the header due to templates and structs.
// This file would contain implementations for methods if they were more complex
// and not suitable for inline/header definition.

// For example, if AnyHttp2Frame had more complex methods:
/*
namespace http2 {

// Example: If AnyHttp2Frame had a non-template method.
// void AnyHttp2Frame::someComplexMethod() {
//     // ... implementation ...
// }

} // namespace http2
*/

// Currently, http2_frame.h is mostly definitions.
// We can add helper functions here for serializing/deserializing if not part of the parser directly.
// For now, this file can remain minimal.

// Helper function to convert FrameType enum to string for debugging
namespace http2 {
    const char* frame_type_to_string(FrameType type) {
        switch (type) {
            case FrameType::DATA: return "DATA";
            case FrameType::HEADERS: return "HEADERS";
            case FrameType::PRIORITY: return "PRIORITY";
            case FrameType::RST_STREAM: return "RST_STREAM";
            case FrameType::SETTINGS: return "SETTINGS";
            case FrameType::PUSH_PROMISE: return "PUSH_PROMISE";
            case FrameType::PING: return "PING";
            case FrameType::GOAWAY: return "GOAWAY";
            case FrameType::WINDOW_UPDATE: return "WINDOW_UPDATE";
            case FrameType::CONTINUATION: return "CONTINUATION";
            default: return "UNKNOWN";
        }
    }
} // namespace http2
