#include "redclaw/net/net_module.h"
#include "redclaw/net/ice_candidate_diagnostics.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <future>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <rtc/rtc.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "peer_harness.h"
namespace redclaw::net {
namespace {
constexpr std::string_view kLoopbackMessage = "redclaw-m01-loopback";
bool is_failed_state(rtc::PeerConnection::State state) {
    return state == rtc::PeerConnection::State::Failed
        || state == rtc::PeerConnection::State::Disconnected
        || state == rtc::PeerConnection::State::Closed;
}
}  // namespace

PeerLoopbackPocResult run_peer_loopback_poc(std::uint32_t timeout_ms) {
    PeerLoopbackPocResult result;

    rtc::Configuration config;
    auto left = std::make_shared<rtc::PeerConnection>(config);
    auto right = std::make_shared<rtc::PeerConnection>(config);

    struct CandidateQueue {
        bool remote_description_ready = false;
        std::vector<rtc::Candidate> pending;
    };

    std::mutex state_mutex;
    std::condition_variable state_cv;
    CandidateQueue left_to_right;
    CandidateQueue right_to_left;
    std::shared_ptr<rtc::DataChannel> left_channel;
    std::shared_ptr<rtc::DataChannel> right_channel;
    bool right_answer_started = false;
    bool left_channel_open = false;
    bool right_channel_open = false;

    auto try_send_loopback = [&]() {
        std::shared_ptr<rtc::DataChannel> channel_to_send;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!left_channel_open || !right_channel_open || left_channel == nullptr) {
                return;
            }
            channel_to_send = left_channel;
        }

        if (!channel_to_send->send(std::string(kLoopbackMessage))) {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (result.error.empty()) {
                result.error = "failed to send loopback data-channel message";
            }
        }
        state_cv.notify_all();
    };

    auto set_error = [&](std::string error) {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (result.error.empty()) {
            result.error = std::move(error);
        }
        state_cv.notify_all();
    };

    auto flush_candidates = [&](const std::shared_ptr<rtc::PeerConnection>& target,
                               CandidateQueue& queue,
                               const std::string& tag) -> bool {
        std::vector<rtc::Candidate> pending;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            pending.swap(queue.pending);
        }

        for (const auto& candidate : pending) {
            try {
                target->addRemoteCandidate(candidate);
            } catch (const std::exception& ex) {
                std::lock_guard<std::mutex> lock(state_mutex);
                if (result.error.empty()) {
                    result.error = tag + ": " + ex.what();
                }
                return false;
            }
        }
        return true;
    };

    auto forward_candidate = [&](const rtc::Candidate& candidate,
                                const std::shared_ptr<rtc::PeerConnection>& target,
                                CandidateQueue& queue,
                                const std::string& tag) {
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!queue.remote_description_ready) {
                queue.pending.push_back(candidate);
                return;
            }
        }

        try {
            target->addRemoteCandidate(candidate);
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (result.error.empty()) {
                result.error = tag + ": " + ex.what();
            }
        }
        state_cv.notify_all();
    };

    left->onStateChange([&](rtc::PeerConnection::State state) {
        if (is_failed_state(state)) {
            set_error("left peer entered failed state");
        }
    });
    right->onStateChange([&](rtc::PeerConnection::State state) {
        if (is_failed_state(state)) {
            set_error("right peer entered failed state");
        }
    });

    bool left_offer_applied = false;
    bool right_answer_applied = false;

    left->onLocalDescription([&](rtc::Description description) {
        if (description.type() != rtc::Description::Type::Offer || left_offer_applied) {
            return;
        }
        left_offer_applied = true;

        try {
            right->setRemoteDescription(description);

            {
                std::lock_guard<std::mutex> lock(state_mutex);
                left_to_right.remote_description_ready = true;
            }

            if (!flush_candidates(right, left_to_right, "flush left->right candidates failed")) {
                state_cv.notify_all();
                return;
            }

            bool should_start_answer = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                if (!right_answer_started) {
                    right_answer_started = true;
                    should_start_answer = true;
                }
            }

            if (should_start_answer) {
                right->setLocalDescription();
            }
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (result.error.empty()) {
                result.error = std::string("left local description forwarding failed: ") + ex.what();
            }
        }
        state_cv.notify_all();
    });

    right->onLocalDescription([&](rtc::Description description) {
        if (description.type() != rtc::Description::Type::Answer || right_answer_applied) {
            return;
        }
        right_answer_applied = true;

        try {
            left->setRemoteDescription(description);

            {
                std::lock_guard<std::mutex> lock(state_mutex);
                right_to_left.remote_description_ready = true;
            }

            if (!flush_candidates(left, right_to_left, "flush right->left candidates failed")) {
                state_cv.notify_all();
                return;
            }
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (result.error.empty()) {
                result.error = std::string("right local description forwarding failed: ") + ex.what();
            }
        }
        state_cv.notify_all();
    });

    left->onLocalCandidate([&](rtc::Candidate candidate) {
        forward_candidate(candidate, right, left_to_right, "left candidate forwarding failed");
    });

    right->onLocalCandidate([&](rtc::Candidate candidate) {
        forward_candidate(candidate, left, right_to_left, "right candidate forwarding failed");
    });

    right->onDataChannel([&](std::shared_ptr<rtc::DataChannel> channel) {
        right_channel = std::move(channel);
        right_channel->onOpen([&]() {
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                right_channel_open = true;
            }
            try_send_loopback();
        });

        right_channel->onMessage([&](rtc::message_variant data) {
            std::lock_guard<std::mutex> lock(state_mutex);
            const auto* text = std::get_if<rtc::string>(&data);
            if (text == nullptr) {
                if (result.error.empty()) {
                    result.error = "received non-text data-channel payload";
                }
            } else {
                result.received_message = *text;
                result.ok = (result.received_message == kLoopbackMessage);
                if (!result.ok && result.error.empty()) {
                    result.error = "received unexpected loopback payload";
                }
            }
            state_cv.notify_all();
        });
    });

    left_channel = left->createDataChannel("m01-loopback");
    left_channel->onOpen([&]() {
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            left_channel_open = true;
        }
        try_send_loopback();
    });

    try {
        left->setLocalDescription();
    } catch (const std::exception& ex) {
        result.error = std::string("failed to start loopback negotiation: ") + ex.what();
        left->close();
        right->close();
        return result;
    }

    {
        std::unique_lock<std::mutex> lock(state_mutex);
        state_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]() {
            return result.ok || !result.error.empty();
        });
        if (!result.ok && result.error.empty()) {
            result.error = "loopback timed out before receiving data-channel message"
                " (left_state=" + std::to_string(static_cast<int>(left->state()))
                + ", right_state=" + std::to_string(static_cast<int>(right->state())) + ")";
        }
    }

    left->close();
    right->close();
    return result;
}

namespace {

PeerLoopbackPocResult run_candidate_exchange_attempt(std::uint32_t timeout_ms, bool block_all_candidates) {
    PeerLoopbackPocResult result;

    rtc::Configuration config;
    auto left = std::make_shared<rtc::PeerConnection>(config);
    auto right = std::make_shared<rtc::PeerConnection>(config);

    struct CandidateQueue {
        bool remote_description_ready = false;
        std::vector<rtc::Candidate> pending;
    };

    std::mutex state_mutex;
    std::condition_variable state_cv;
    CandidateQueue left_to_right;
    CandidateQueue right_to_left;
    std::shared_ptr<rtc::DataChannel> left_channel;
    std::shared_ptr<rtc::DataChannel> right_channel;
    bool right_answer_started = false;
    bool left_channel_open = false;
    bool right_channel_open = false;
    bool left_offer_applied = false;
    bool right_answer_applied = false;

    auto try_send_loopback = [&]() {
        std::shared_ptr<rtc::DataChannel> channel_to_send;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!left_channel_open || !right_channel_open || left_channel == nullptr) {
                return;
            }
            channel_to_send = left_channel;
        }

        if (!channel_to_send->send(std::string(kLoopbackMessage))) {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (result.error.empty()) {
                result.error = "failed to send loopback data-channel message";
            }
        }
        state_cv.notify_all();
    };

    auto set_error = [&](std::string error) {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (result.error.empty()) {
            result.error = std::move(error);
        }
        state_cv.notify_all();
    };

    auto flush_candidates = [&](const std::shared_ptr<rtc::PeerConnection>& target,
                               CandidateQueue& queue,
                               const std::string& tag) -> bool {
        std::vector<rtc::Candidate> pending;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            pending.swap(queue.pending);
        }

        for (const auto& candidate : pending) {
            if (block_all_candidates) {
                continue;
            }

            try {
                target->addRemoteCandidate(candidate);
            } catch (const std::exception& ex) {
                std::lock_guard<std::mutex> lock(state_mutex);
                if (result.error.empty()) {
                    result.error = tag + ": " + ex.what();
                }
                return false;
            }
        }
        return true;
    };

    auto forward_candidate = [&](const rtc::Candidate& candidate,
                                const std::shared_ptr<rtc::PeerConnection>& target,
                                CandidateQueue& queue,
                                const std::string& tag) {
        if (block_all_candidates) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!queue.remote_description_ready) {
                queue.pending.push_back(candidate);
                return;
            }
        }

        try {
            target->addRemoteCandidate(candidate);
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (result.error.empty()) {
                result.error = tag + ": " + ex.what();
            }
        }
        state_cv.notify_all();
    };

    left->onStateChange([&](rtc::PeerConnection::State state) {
        if (is_failed_state(state)) {
            set_error("left peer entered failed state");
        }
    });
    right->onStateChange([&](rtc::PeerConnection::State state) {
        if (is_failed_state(state)) {
            set_error("right peer entered failed state");
        }
    });

    left->onLocalDescription([&](rtc::Description description) {
        if (description.type() != rtc::Description::Type::Offer || left_offer_applied) {
            return;
        }
        left_offer_applied = true;

        try {
            right->setRemoteDescription(description);

            {
                std::lock_guard<std::mutex> lock(state_mutex);
                left_to_right.remote_description_ready = true;
            }

            if (!flush_candidates(right, left_to_right, "flush left->right candidates failed")) {
                state_cv.notify_all();
                return;
            }

            bool should_start_answer = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                if (!right_answer_started) {
                    right_answer_started = true;
                    should_start_answer = true;
                }
            }

            if (should_start_answer) {
                right->setLocalDescription();
            }
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (result.error.empty()) {
                result.error = std::string("left local description forwarding failed: ") + ex.what();
            }
        }
        state_cv.notify_all();
    });

    right->onLocalDescription([&](rtc::Description description) {
        if (description.type() != rtc::Description::Type::Answer || right_answer_applied) {
            return;
        }
        right_answer_applied = true;

        try {
            left->setRemoteDescription(description);

            {
                std::lock_guard<std::mutex> lock(state_mutex);
                right_to_left.remote_description_ready = true;
            }

            if (!flush_candidates(left, right_to_left, "flush right->left candidates failed")) {
                state_cv.notify_all();
                return;
            }
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (result.error.empty()) {
                result.error = std::string("right local description forwarding failed: ") + ex.what();
            }
        }
        state_cv.notify_all();
    });

    left->onLocalCandidate([&](rtc::Candidate candidate) {
        forward_candidate(candidate, right, left_to_right, "left candidate forwarding failed");
    });

    right->onLocalCandidate([&](rtc::Candidate candidate) {
        forward_candidate(candidate, left, right_to_left, "right candidate forwarding failed");
    });

    right->onDataChannel([&](std::shared_ptr<rtc::DataChannel> channel) {
        right_channel = std::move(channel);
        right_channel->onOpen([&]() {
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                right_channel_open = true;
            }
            try_send_loopback();
        });

        right_channel->onMessage([&](rtc::message_variant data) {
            std::lock_guard<std::mutex> lock(state_mutex);
            const auto* text = std::get_if<rtc::string>(&data);
            if (text == nullptr) {
                if (result.error.empty()) {
                    result.error = "received non-text data-channel payload";
                }
            } else {
                result.received_message = *text;
                result.ok = (result.received_message == kLoopbackMessage);
                if (!result.ok && result.error.empty()) {
                    result.error = "received unexpected loopback payload";
                }
            }
            state_cv.notify_all();
        });
    });

    left_channel = left->createDataChannel("m01-lan-harness");
    left_channel->onOpen([&]() {
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            left_channel_open = true;
        }
        try_send_loopback();
    });

    try {
        left->setLocalDescription();
    } catch (const std::exception& ex) {
        result.error = std::string("failed to start lan harness negotiation: ") + ex.what();
        left->close();
        right->close();
        return result;
    }

    {
        std::unique_lock<std::mutex> lock(state_mutex);
        state_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]() {
            return result.ok || !result.error.empty();
        });
        if (!result.ok && result.error.empty()) {
            result.error = block_all_candidates
                ? "lan harness timed out while direct candidates were blocked"
                : "lan harness timed out before receiving data-channel message";
        }
    }

    left->close();
    right->close();
    return result;
}

}  // namespace

LanPeerIntegrationHarnessResult run_lan_peer_integration_harness(const LanPeerIntegrationHarnessConfig& config) {
    LanPeerIntegrationHarnessResult out;

    const auto direct = run_candidate_exchange_attempt(config.timeout_ms, config.block_direct_candidates);
    if (direct.ok) {
        out.ok = true;
        out.used_relay_fallback = false;
        out.received_message = direct.received_message;
        return out;
    }

    if (!config.block_direct_candidates || !config.enable_relay_fallback) {
        out.ok = false;
        out.used_relay_fallback = false;
        out.error = direct.error.empty() ? "direct lan attempt failed" : direct.error;
        return out;
    }

    const auto relay = run_candidate_exchange_attempt(config.timeout_ms, false);
    if (relay.ok) {
        out.ok = true;
        out.used_relay_fallback = true;
        out.received_message = relay.received_message;
        return out;
    }

    out.ok = false;
    out.used_relay_fallback = true;
    out.error = relay.error.empty() ? "relay fallback attempt failed" : relay.error;
    return out;
}
}  // namespace redclaw::net
