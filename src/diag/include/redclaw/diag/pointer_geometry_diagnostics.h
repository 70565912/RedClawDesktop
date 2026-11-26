#pragma once

#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace redclaw::diag {

inline constexpr std::uint64_t kPointerInputSampleIntervalMs = 3000;

struct PointerGeometryRecord {
    std::string event;
    std::string backend;
    std::string display_id;
    int output_index = -1;
    std::int32_t origin_x = 0;
    std::int32_t origin_y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rotation = 0;
    std::uint64_t revision = 0;
    std::uint32_t region_x = 0;
    std::uint32_t region_y = 0;
    std::uint32_t region_width = 0;
    std::uint32_t region_height = 0;
    std::uint64_t region_revision = 0;
    int process_dpi_awareness = -1;
    std::string thread_dpi_context = "unsupported";
    std::int32_t getdesc_left = 0;
    std::int32_t getdesc_top = 0;
    std::int32_t getdesc_right = 0;
    std::int32_t getdesc_bottom = 0;
    bool getdesc_valid = false;
    std::int32_t virtual_x = 0;
    std::int32_t virtual_y = 0;
    std::int32_t virtual_width = 0;
    std::int32_t virtual_height = 0;
    bool virtual_valid = false;
    std::int32_t pma_virtual_x = 0;
    std::int32_t pma_virtual_y = 0;
    std::int32_t pma_virtual_width = 0;
    std::int32_t pma_virtual_height = 0;
    bool pma_virtual_valid = false;
};

struct PointerInputSampleRecord {
    std::string reason;
    std::string stage;
    std::uint64_t sample_id = 0;
    std::uint16_t normalized_x = 0;
    std::uint16_t normalized_y = 0;
    std::int32_t global_x = 0;
    std::int32_t global_y = 0;
    std::int32_t sendinput_dx = 0;
    std::int32_t sendinput_dy = 0;
    bool sendinput_present = false;
    std::int32_t virtual_x = 0;
    std::int32_t virtual_y = 0;
    std::int32_t virtual_width = 0;
    std::int32_t virtual_height = 0;
    bool virtual_valid = false;
    bool absolute = true;
    bool virtual_desk = false;
    std::int32_t origin_x = 0;
    std::int32_t origin_y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rotation = 0;
    std::uint64_t revision = 0;
    std::uint32_t region_x = 0;
    std::uint32_t region_y = 0;
    std::uint32_t region_width = 0;
    std::uint32_t region_height = 0;
    std::uint64_t region_revision = 0;
    std::string display_id;
};

class PointerInputSampleGate {
public:
    enum class Reason { kNone, kFirstAfterChange, kInterval, kMappingInputsChanged };

    void note_mapping_inputs_changed() { first_after_change_ = true; }

    [[nodiscard]] Reason evaluate(std::uint64_t now_ms, std::uint64_t signature) {
        Reason reason = Reason::kNone;
        if (first_after_change_) {
            reason = Reason::kFirstAfterChange;
        } else if (signature != last_signature_) {
            reason = Reason::kMappingInputsChanged;
        } else if (last_sample_ms_ == 0
                   || now_ms >= last_sample_ms_ + kPointerInputSampleIntervalMs) {
            reason = Reason::kInterval;
        }
        if (reason != Reason::kNone) {
            first_after_change_ = false;
            last_sample_ms_ = now_ms;
            last_signature_ = signature;
        }
        return reason;
    }

    static const char* reason_name(Reason reason) {
        switch (reason) {
        case Reason::kFirstAfterChange: return "first_after_change";
        case Reason::kInterval: return "interval";
        case Reason::kMappingInputsChanged: return "mapping_inputs_changed";
        case Reason::kNone: return "none";
        }
        return "none";
    }

private:
    bool first_after_change_ = true;
    std::uint64_t last_sample_ms_ = 0;
    std::uint64_t last_signature_ = 0;
};

inline std::uint64_t pointer_mapping_signature(
    std::int32_t origin_x,
    std::int32_t origin_y,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t rotation,
    std::uint64_t revision,
    std::uint32_t region_x,
    std::uint32_t region_y,
    std::uint32_t region_width,
    std::uint32_t region_height,
    std::uint64_t region_revision,
    std::int32_t virtual_x,
    std::int32_t virtual_y,
    std::int32_t virtual_width,
    std::int32_t virtual_height) {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(origin_x)));
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(origin_y)));
    mix(width);
    mix(height);
    mix(rotation);
    mix(revision);
    mix(region_x);
    mix(region_y);
    mix(region_width);
    mix(region_height);
    mix(region_revision);
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(virtual_x)));
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(virtual_y)));
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(virtual_width)));
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(virtual_height)));
    return hash;
}

inline void append_kv(std::ostringstream& out, std::string_view key, std::string_view value) {
    out << ' ' << key << '=';
    for (const char ch : value) {
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            out << '_';
        } else {
            out << ch;
        }
    }
}

inline void append_kv(std::ostringstream& out, std::string_view key, bool value) {
    out << ' ' << key << '=' << (value ? "true" : "false");
}

inline void append_kv_i64(std::ostringstream& out, std::string_view key, std::int64_t value) {
    out << ' ' << key << '=' << value;
}

inline std::string format_pointer_geometry_line(const PointerGeometryRecord& record) {
    std::ostringstream out;
    out << "Runtime pointer geometry";
    append_kv(out, "event", record.event);
    append_kv(out, "backend", record.backend);
    append_kv(out, "display_id", record.display_id);
    append_kv_i64(out, "output_index", record.output_index);
    append_kv_i64(out, "geometry_origin_x", record.origin_x);
    append_kv_i64(out, "geometry_origin_y", record.origin_y);
    append_kv_i64(out, "geometry_width", record.width);
    append_kv_i64(out, "geometry_height", record.height);
    append_kv_i64(out, "geometry_rotation", record.rotation);
    append_kv_i64(out, "geometry_revision", static_cast<std::int64_t>(record.revision));
    append_kv_i64(out, "region_x", record.region_x);
    append_kv_i64(out, "region_y", record.region_y);
    append_kv_i64(out, "region_width", record.region_width);
    append_kv_i64(out, "region_height", record.region_height);
    append_kv_i64(out, "region_revision", static_cast<std::int64_t>(record.region_revision));
    append_kv_i64(out, "process_dpi_awareness", record.process_dpi_awareness);
    append_kv(out, "thread_dpi_context", record.thread_dpi_context);
    append_kv(out, "getdesc_valid", record.getdesc_valid);
    append_kv_i64(out, "getdesc_left", record.getdesc_left);
    append_kv_i64(out, "getdesc_top", record.getdesc_top);
    append_kv_i64(out, "getdesc_right", record.getdesc_right);
    append_kv_i64(out, "getdesc_bottom", record.getdesc_bottom);
    append_kv(out, "virtual_valid", record.virtual_valid);
    append_kv_i64(out, "virtual_x", record.virtual_x);
    append_kv_i64(out, "virtual_y", record.virtual_y);
    append_kv_i64(out, "virtual_width", record.virtual_width);
    append_kv_i64(out, "virtual_height", record.virtual_height);
    append_kv(out, "pma_virtual_valid", record.pma_virtual_valid);
    append_kv_i64(out, "pma_virtual_x", record.pma_virtual_x);
    append_kv_i64(out, "pma_virtual_y", record.pma_virtual_y);
    append_kv_i64(out, "pma_virtual_width", record.pma_virtual_width);
    append_kv_i64(out, "pma_virtual_height", record.pma_virtual_height);
    return out.str();
}

inline std::string format_pointer_input_sample_line(const PointerInputSampleRecord& record) {
    std::ostringstream out;
    out << "Runtime pointer input sample";
    append_kv(out, "reason", record.reason);
    append_kv(out, "stage", record.stage);
    append_kv_i64(out, "sample_id", static_cast<std::int64_t>(record.sample_id));
    append_kv_i64(out, "normalized_x", record.normalized_x);
    append_kv_i64(out, "normalized_y", record.normalized_y);
    append_kv_i64(out, "global_x", record.global_x);
    append_kv_i64(out, "global_y", record.global_y);
    append_kv(out, "sendinput_present", record.sendinput_present);
    append_kv_i64(out, "sendinput_dx", record.sendinput_dx);
    append_kv_i64(out, "sendinput_dy", record.sendinput_dy);
    append_kv(out, "virtual_valid", record.virtual_valid);
    append_kv_i64(out, "virtual_x", record.virtual_x);
    append_kv_i64(out, "virtual_y", record.virtual_y);
    append_kv_i64(out, "virtual_width", record.virtual_width);
    append_kv_i64(out, "virtual_height", record.virtual_height);
    append_kv(out, "absolute", record.absolute);
    append_kv(out, "virtual_desk", record.virtual_desk);
    append_kv_i64(out, "geometry_origin_x", record.origin_x);
    append_kv_i64(out, "geometry_origin_y", record.origin_y);
    append_kv_i64(out, "geometry_width", record.width);
    append_kv_i64(out, "geometry_height", record.height);
    append_kv_i64(out, "geometry_rotation", record.rotation);
    append_kv_i64(out, "geometry_revision", static_cast<std::int64_t>(record.revision));
    append_kv_i64(out, "region_x", record.region_x);
    append_kv_i64(out, "region_y", record.region_y);
    append_kv_i64(out, "region_width", record.region_width);
    append_kv_i64(out, "region_height", record.region_height);
    append_kv_i64(out, "region_revision", static_cast<std::int64_t>(record.region_revision));
    append_kv(out, "display_id", record.display_id);
    return out.str();
}

inline void emit_pointer_diagnostic_line(const std::string& line) {
    std::cout << line << '\n';
    std::cout.flush();
}

class PointerPathSampleCoordinator {
public:
    void request(PointerInputSampleRecord record) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_) {
            pending_->stage = "mapped";
            emit_pointer_diagnostic_line(format_pointer_input_sample_line(*pending_));
        }
        pending_ = std::move(record);
    }

    [[nodiscard]] std::optional<PointerInputSampleRecord> take_matching(
        std::int32_t global_x, std::int32_t global_y) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_ || pending_->global_x != global_x || pending_->global_y != global_y) {
            return std::nullopt;
        }
        auto record = std::move(*pending_);
        pending_.reset();
        return record;
    }

private:
    std::mutex mutex_;
    std::optional<PointerInputSampleRecord> pending_;
};

inline PointerPathSampleCoordinator& pointer_path_samples() {
    static PointerPathSampleCoordinator coordinator;
    return coordinator;
}

#if defined(_WIN32)
inline const char* describe_dpi_context(DPI_AWARENESS_CONTEXT context) {
    using AreEqualFn = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT);
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr || context == nullptr) {
        return context == nullptr ? "none" : "other";
    }
    const auto are_equal = reinterpret_cast<AreEqualFn>(
        GetProcAddress(user32, "AreDpiAwarenessContextsEqual"));
    if (are_equal == nullptr) {
        return "unsupported";
    }
    if (are_equal(context, DPI_AWARENESS_CONTEXT_UNAWARE)) return "unaware";
    if (are_equal(context, DPI_AWARENESS_CONTEXT_SYSTEM_AWARE)) return "system";
    if (are_equal(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) return "per_monitor";
    if (are_equal(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return "per_monitor_v2";
#ifdef DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED
    if (are_equal(context, DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED)) return "unaware_gdiscaled";
#endif
    return "other";
}

inline void fill_thread_dpi_snapshot(PointerGeometryRecord* record) {
    if (record == nullptr) {
        return;
    }
    using GetProcessAwarenessFn = HRESULT (WINAPI*)(HANDLE, int*);
    using GetThreadContextFn = DPI_AWARENESS_CONTEXT (WINAPI*)();
    using SetThreadContextFn = DPI_AWARENESS_CONTEXT (WINAPI*)(DPI_AWARENESS_CONTEXT);

    const HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore != nullptr) {
        const auto get_process = reinterpret_cast<GetProcessAwarenessFn>(
            GetProcAddress(shcore, "GetProcessDpiAwareness"));
        int awareness = -1;
        if (get_process != nullptr && SUCCEEDED(get_process(nullptr, &awareness))) {
            record->process_dpi_awareness = awareness;
        }
        FreeLibrary(shcore);
    }

    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) {
        return;
    }
    const auto get_thread = reinterpret_cast<GetThreadContextFn>(
        GetProcAddress(user32, "GetThreadDpiAwarenessContext"));
    const auto set_thread = reinterpret_cast<SetThreadContextFn>(
        GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
    DPI_AWARENESS_CONTEXT current = nullptr;
    if (get_thread != nullptr) {
        current = get_thread();
        record->thread_dpi_context = describe_dpi_context(current);
    }

    record->virtual_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    record->virtual_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    record->virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    record->virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    record->virtual_valid = record->virtual_width > 0 && record->virtual_height > 0;

    if (set_thread == nullptr) {
        return;
    }
    if (record->thread_dpi_context == "per_monitor_v2") {
        record->pma_virtual_x = record->virtual_x;
        record->pma_virtual_y = record->virtual_y;
        record->pma_virtual_width = record->virtual_width;
        record->pma_virtual_height = record->virtual_height;
        record->pma_virtual_valid = record->virtual_valid;
        return;
    }

    const DPI_AWARENESS_CONTEXT previous = set_thread(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    record->pma_virtual_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    record->pma_virtual_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    record->pma_virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    record->pma_virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    record->pma_virtual_valid = record->pma_virtual_width > 0 && record->pma_virtual_height > 0;
    if (previous != nullptr) {
        set_thread(previous);
    } else if (current != nullptr) {
        set_thread(current);
    }
}
#endif

}  // namespace redclaw::diag
