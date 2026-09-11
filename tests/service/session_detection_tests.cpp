#include "redclaw/service/session_detection.h"

#include <cassert>
#include <iostream>
#include <vector>

using namespace redclaw::service;

static void test_start_stop_lifecycle() {
	SessionDetectionListener listener;

	const auto start_result = listener.start();
	assert(start_result == SessionDetectionError::kNone);
	assert(listener.is_running());

	const auto stop_result = listener.stop();
	assert(stop_result == SessionDetectionError::kNone);
	assert(!listener.is_running());

	std::cout << "✓ Start/stop lifecycle\n";
}

static void test_double_start_rejected() {
	SessionDetectionListener listener;
	assert(listener.start() == SessionDetectionError::kNone);

	const auto second = listener.start();
	assert(second == SessionDetectionError::kAlreadyStarted);
	assert(!listener.last_error_detail().empty());

	std::cout << "✓ Double start rejected\n";
}

static void test_stop_without_start_rejected() {
	SessionDetectionListener listener;
	const auto stop_result = listener.stop();
	assert(stop_result == SessionDetectionError::kNotStarted);
	assert(!listener.last_error_detail().empty());

	std::cout << "✓ Stop without start rejected\n";
}

static void test_session_event_mapping() {
	SessionDetectionListener listener;
	std::vector<SessionChangeEvent> events;
	listener.set_session_change_handler([&](const SessionChangeEvent& e) {
		events.push_back(e);
	});

	listener.handle_session_change(0x5, 100);   // logon
	listener.handle_session_change(0x6, 100);   // logoff
	listener.handle_session_change(0x7, 100);   // lock
	listener.handle_session_change(0x8, 100);   // unlock
	listener.handle_session_change(0x3, 100);   // remote connect
	listener.handle_session_change(0x4, 100);   // remote disconnect
	listener.handle_session_change(0xFFFF, 100);

	assert(events.size() == 7);
	assert(events[0].type == SessionEventType::kLogon);
	assert(events[1].type == SessionEventType::kLogoff);
	assert(events[2].type == SessionEventType::kLock);
	assert(events[3].type == SessionEventType::kUnlock);
	assert(events[4].type == SessionEventType::kRemoteConnect);
	assert(events[5].type == SessionEventType::kRemoteDisconnect);
	assert(events[6].type == SessionEventType::kUnknown);
	assert(events[0].session_id == 100);
	assert(events[0].source == "WTSRegisterSessionNotification");

	std::cout << "✓ Session event mapping\n";
}

static void test_no_handler_is_safe() {
	SessionDetectionListener listener;
	listener.handle_session_change(0x5, 77);
	listener.handle_session_change(0x6, 77);

	std::cout << "✓ No handler is safe\n";
}

int main() {
	std::cout << "Running session detection tests...\n";
	test_start_stop_lifecycle();
	test_double_start_rejected();
	test_stop_without_start_rejected();
	test_session_event_mapping();
	test_no_handler_is_safe();
	std::cout << "All session detection tests passed!\n";
	return 0;
}

