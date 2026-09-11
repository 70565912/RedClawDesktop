#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "redclaw/security/security_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

std::string make_temp_file_path(const std::string& suffix) {
    const auto base = std::filesystem::temp_directory_path();
    const auto tick = std::to_string(static_cast<unsigned long long>(std::filesystem::file_time_type::clock::now().time_since_epoch().count()));
    return (base / ("redclaw_fp_store_" + tick + suffix)).string();
}

bool test_save_load_roundtrip_normalizes_and_dedups() {
    const std::string path = make_temp_file_path("_roundtrip.txt");
    redclaw::security::FileBackedPeerFingerprintStore store(path);

    std::string error;
    const bool save_ok = store.save({"SHA256:DEADBEEF", " sha256:deadbeef ", "sha256:cafebabe"}, &error);
    if (!expect_true(save_ok, "save should succeed")) {
        std::filesystem::remove(path);
        return false;
    }

    std::vector<std::string> loaded;
    const bool load_ok = store.load(&loaded, &error);
    std::filesystem::remove(path);

    return expect_true(load_ok, "load should succeed")
        && expect_true(loaded.size() == 2, "loaded fingerprints should be deduplicated")
        && expect_true(loaded[0] == "sha256:cafebabe", "loaded first fingerprint mismatch")
        && expect_true(loaded[1] == "sha256:deadbeef", "loaded second fingerprint mismatch");
}

bool test_persistence_backed_update_flow() {
    const std::string path = make_temp_file_path("_update.txt");
    redclaw::security::FileBackedPeerFingerprintStore store(path);

    std::string error;
    if (!expect_true(store.save({"sha256:deadbeef"}, &error), "initial save should succeed")) {
        std::filesystem::remove(path);
        return false;
    }

    std::vector<std::string> loaded;
    if (!expect_true(store.load(&loaded, &error), "initial load should succeed")) {
        std::filesystem::remove(path);
        return false;
    }

    redclaw::security::InMemoryPeerFingerprintVerifier verifier(loaded);
    const auto first_old = verifier.verify("sha256:deadbeef");
    const auto first_new = verifier.verify("sha256:cafebabe");

    if (!expect_true(store.save({"sha256:cafebabe"}, &error), "updated save should succeed")) {
        std::filesystem::remove(path);
        return false;
    }

    loaded.clear();
    if (!expect_true(store.load(&loaded, &error), "updated load should succeed")) {
        std::filesystem::remove(path);
        return false;
    }

    verifier.set_trusted_fingerprints(loaded);
    const auto second_old = verifier.verify("sha256:deadbeef");
    const auto second_new = verifier.verify("sha256:cafebabe");

    std::filesystem::remove(path);
    return expect_true(first_old.accepted, "old fingerprint should be trusted before update")
        && expect_true(!first_new.accepted, "new fingerprint should be untrusted before update")
        && expect_true(!second_old.accepted, "old fingerprint should be untrusted after update")
        && expect_true(second_new.accepted, "new fingerprint should be trusted after update");
}

bool test_load_rejects_invalid_line() {
    const std::string path = make_temp_file_path("_invalid.txt");
    {
        std::ofstream out(path, std::ios::trunc);
        out << "sha256:deadbeef\n";
        out << "invalid-fingerprint\n";
    }

    redclaw::security::FileBackedPeerFingerprintStore store(path);
    std::vector<std::string> loaded;
    std::string error;
    const bool ok = store.load(&loaded, &error);

    std::filesystem::remove(path);
    return expect_true(!ok, "load should fail on invalid fingerprint")
        && expect_true(error.find("invalid fingerprint") != std::string::npos, "error should mention invalid fingerprint");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_save_load_roundtrip_normalizes_and_dedups() && ok;
    ok = test_persistence_backed_update_flow() && ok;
    ok = test_load_rejects_invalid_line() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_security_peer_fingerprint_store_tests" << '\n';
    return 0;
}
