#include <gtest/gtest.h>

#include "redclaw/workspace/terminal_session.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {
using redclaw::workspace::TerminalSession;
using namespace std::chrono_literals;

// A real terminal answers cursor-position queries. ConPTY's PowerShell may ask
// one during PSReadLine initialization; answer without a GUI or user input.
void drain(TerminalSession& terminal, std::string& output) {
    while (auto chunk = terminal.take_output()) {
        const auto previous = output.size();
        output += *chunk;
        if (output.find("\x1b[6n", previous > 3 ? previous - 3 : 0) != std::string::npos) {
            (void)terminal.write("\x1b[1;1R");
        }
    }
}

bool until(TerminalSession& terminal, std::string& output, std::string_view marker,
           std::chrono::seconds timeout = 15s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        drain(terminal, output);
        if (output.find(marker) != std::string::npos) return true;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

TEST(TerminalSessionIntegration, PersistentPowerShellUnicodeResizeAndInterrupt) {
#ifndef _WIN32
    GTEST_SKIP() << "ConPTY requires Windows.";
#else
    TerminalSession terminal;
    std::string error, output;
    ASSERT_TRUE(terminal.start(std::filesystem::current_path(), {100, 30}, &error)) << error;
    ASSERT_TRUE(until(terminal, output, "PS "));
    ASSERT_TRUE(terminal.write("$x=21; Write-Output ('TERM_'+'RESULT_'+($x*2))\r"));
    ASSERT_TRUE(until(terminal, output, "TERM_RESULT_42"));
    ASSERT_TRUE(terminal.resize({132, 43}));
    ASSERT_TRUE(terminal.write("$x+=1; Write-Output ('PERSIST_'+'VALUE_'+$x); Write-Output ([char]0x4e2d+[string][char]0x6587)\r"));
    ASSERT_TRUE(until(terminal, output, "PERSIST_VALUE_22"));
    ASSERT_TRUE(until(terminal, output, "\xe4\xb8\xad\xe6\x96\x87"));
    ASSERT_TRUE(terminal.write("Write-Output ('SLEEP_'+'BEGIN'); Start-Sleep -Seconds 60\r"));
    ASSERT_TRUE(until(terminal, output, "SLEEP_BEGIN"));
    ASSERT_TRUE(terminal.write(std::string(1, '\x03')));
    std::this_thread::sleep_for(100ms);
    ASSERT_TRUE(terminal.write("Write-Output ('AFTER_'+'INTERRUPT')\r"));
    ASSERT_TRUE(until(terminal, output, "AFTER_INTERRUPT"));
    EXPECT_TRUE(terminal.running());
    terminal.stop();
    EXPECT_FALSE(terminal.running());
    EXPECT_FALSE(terminal.write("must not execute"));
    EXPECT_FALSE(terminal.resize({80, 25}));
#endif
}

TEST(TerminalSessionIntegration, BoundedOutputBackpressureAndShutdownOfProcessTree) {
#ifndef _WIN32
    GTEST_SKIP() << "ConPTY requires Windows.";
#else
    TerminalSession terminal;
    std::string error, output;
    ASSERT_TRUE(terminal.start(std::filesystem::current_path(), {120, 40}, &error)) << error;
    ASSERT_TRUE(until(terminal, output, "PS "));
    // The child is in the terminal job even though it creates no visible window.
    ASSERT_TRUE(terminal.write("$child=Start-Process ($PSHOME+'\\powershell.exe') -ArgumentList '-NoLogo -NoProfile -Command Start-Sleep -Seconds 120' -WindowStyle Hidden -PassThru; Write-Output ('CHILD_'+'PID='+$child.Id+';')\r"));
    ASSERT_TRUE(until(terminal, output, "CHILD_PID="));
    const auto start = output.find("CHILD_PID=") + std::string("CHILD_PID=").size();
    const auto end = output.find(';', start);
    ASSERT_NE(end, std::string::npos);
    const auto child_id = static_cast<DWORD>(std::stoul(output.substr(start, end - start)));
    HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, child_id);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(WaitForSingleObject(child, 0), WAIT_TIMEOUT);
    ASSERT_TRUE(terminal.write("$line='x'*1000; for($i=0;$i -lt 20000;$i++){[Console]::WriteLine($line)}\r"));
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (terminal.buffered_output_bytes() < TerminalSession::kMaxBufferedOutputBytes - TerminalSession::kOutputChunkBytes
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_GE(terminal.buffered_output_bytes(), TerminalSession::kMaxBufferedOutputBytes - TerminalSession::kOutputChunkBytes);
    EXPECT_LE(terminal.buffered_output_bytes(), TerminalSession::kMaxBufferedOutputBytes);
    EXPECT_FALSE(terminal.write(std::string(TerminalSession::kMaxBufferedInputBytes + 1, 'x')));
    const auto stopped_at = std::chrono::steady_clock::now();
    terminal.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - stopped_at, 5s);
    EXPECT_EQ(WaitForSingleObject(child, 1000), WAIT_OBJECT_0);
    CloseHandle(child);
    EXPECT_EQ(terminal.buffered_output_bytes(), 0U);
    EXPECT_FALSE(terminal.running());
#endif
}

TEST(TerminalSessionIntegration, InvalidStartAndNewSessionNeverReplayCommands) {
#ifndef _WIN32
    GTEST_SKIP() << "ConPTY requires Windows.";
#else
    TerminalSession terminal;
    std::string error, output;
    EXPECT_FALSE(terminal.start("relative", {80, 25}, &error));
    EXPECT_EQ(error, "terminal_invalid_start:0");
    EXPECT_FALSE(terminal.start(std::filesystem::current_path(), {0, 25}, &error));
    ASSERT_TRUE(terminal.start(std::filesystem::current_path(), {80, 25}, &error)) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(terminal.start(std::filesystem::current_path(), {80, 25}, &error));
    ASSERT_TRUE(until(terminal, output, "PS "));
    ASSERT_TRUE(terminal.write("$onlyOldSession=731; Write-Output ('OLD_'+'SET')\r"));
    ASSERT_TRUE(until(terminal, output, "OLD_SET"));
    terminal.stop();
    terminal.stop();
    output.clear();
    ASSERT_TRUE(terminal.start(std::filesystem::current_path(), {80, 25}, &error)) << error;
    ASSERT_TRUE(until(terminal, output, "PS "));
    ASSERT_TRUE(terminal.write("Write-Output ('NEW_'+'EMPTY='+($null -eq $onlyOldSession))\r"));
    ASSERT_TRUE(until(terminal, output, "NEW_EMPTY=True"));
    EXPECT_EQ(output.find("OLD_SET"), std::string::npos);
#endif
}

TEST(TerminalSessionIntegration, WindowsPowerShellReconstructsModulesAndRetainsOtherEnvironment) {
#ifndef _WIN32
    GTEST_SKIP() << "ConPTY requires Windows.";
#else
    struct RestoreEnvironment {
        std::wstring name, value;
        bool present;
        explicit RestoreEnvironment(const wchar_t* key) : name(key) {
            const auto size = GetEnvironmentVariableW(key, nullptr, 0);
            present = size != 0;
            if (present) { std::vector<wchar_t> buffer(size); GetEnvironmentVariableW(key, buffer.data(), size); value = buffer.data(); }
        }
        ~RestoreEnvironment() { SetEnvironmentVariableW(name.c_str(), present ? value.c_str() : nullptr); }
    } modules(L"PSModulePath"), sentinel(L"REDCLAW_TEST_TERMINAL_ENV");
    const auto poison = (std::filesystem::temp_directory_path() / L"redclaw-unusable-module-path").wstring();
    ASSERT_TRUE(SetEnvironmentVariableW(L"PSModulePath", poison.c_str()));
    ASSERT_TRUE(SetEnvironmentVariableW(L"REDCLAW_TEST_TERMINAL_ENV", L"retained"));
    TerminalSession terminal;
    std::string error, output;
    ASSERT_TRUE(terminal.start(std::filesystem::current_path(), {140, 30}, &error)) << error;
    std::vector<wchar_t> unchanged(poison.size() + 1);
    ASSERT_EQ(GetEnvironmentVariableW(L"PSModulePath", unchanged.data(), static_cast<DWORD>(unchanged.size())), poison.size());
    EXPECT_EQ(std::wstring(unchanged.data()), poison);
    ASSERT_TRUE(until(terminal, output, "PS "));
    ASSERT_TRUE(terminal.write("try { $null=Get-Acl . -ErrorAction Stop; Write-Output ('ACL_'+'READY_'+$env:REDCLAW_TEST_TERMINAL_ENV) } catch { Write-Output ('ACL_'+'FAILED') }\r"));
    EXPECT_TRUE(until(terminal, output, "ACL_READY_retained"));
    EXPECT_EQ(output.find("ACL_FAILED"), std::string::npos);
#endif
}
}
