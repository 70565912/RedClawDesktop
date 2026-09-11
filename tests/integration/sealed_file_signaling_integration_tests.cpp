#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "redclaw/protocol/protocol_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

redclaw::protocol::OfferBlobV1 make_blob(
    std::string description_sdp,
    std::string nonce,
    std::uint64_t created_at_ms) {
    redclaw::protocol::OfferBlobV1 blob;
    blob.session_id = "sealed-file-session";
    blob.host_peer_id = "host-a";
    blob.controller_peer_id = "controller-b";
    blob.nonce = std::move(nonce);
    blob.created_at_ms = created_at_ms;
    blob.description_sdp = std::move(description_sdp);
    blob.ice_ufrag = "ufrag-a";
    blob.ice_pwd = "pwd-a";
    blob.host_fingerprint = "sha256:aa=bb";
    blob.candidates = {
        "candidate:1 1 udp 2122260223 192.168.0.2 50000 typ host",
        "candidate:2 1 tcp 1019216383 10.0.0.5 9 typ host tcptype active",
    };
    return blob;
}

bool write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

bool read_text_file(const std::filesystem::path& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return in.good() || in.eof();
}

bool test_sealed_file_exchange_roundtrip() {
    const std::string passphrase = "sealed-passphrase-012345";
    const auto host_offer = make_blob(
        "v=0\na=setup:actpass\na=mid:0\n",
        "nonce-host-offer-01",
        1710000001000ULL);

    const auto controller_answer = make_blob(
        "v=0\na=setup:active\na=mid:0\n",
        "nonce-controller-answer-02",
        1710000002000ULL);

    const auto temp_dir = std::filesystem::temp_directory_path() / "redclaw_sealed_file_signaling_tests";
    std::error_code ec;
    std::filesystem::create_directories(temp_dir, ec);
    if (ec) {
        return expect_true(false, "failed to create temporary test directory");
    }

    const auto host_offer_path = temp_dir / "host-offer.sealed.txt";
    const auto controller_answer_path = temp_dir / "controller-answer.sealed.txt";

    const auto encrypted_offer = redclaw::protocol::encrypt_offer_blob_v1(host_offer, passphrase);
    if (!expect_true(
            encrypted_offer.ok,
            std::string("host sealed offer encryption should succeed: ") + encrypted_offer.error)) {
        return false;
    }

    const auto encrypted_answer = redclaw::protocol::encrypt_offer_blob_v1(controller_answer, passphrase);
    if (!expect_true(
            encrypted_answer.ok,
            std::string("controller sealed answer encryption should succeed: ") + encrypted_answer.error)) {
        return false;
    }

    if (!expect_true(write_text_file(host_offer_path, encrypted_offer.value), "host offer sealed file write should succeed")) {
        return false;
    }
    if (!expect_true(write_text_file(controller_answer_path, encrypted_answer.value), "controller answer sealed file write should succeed")) {
        return false;
    }

    std::string host_offer_text;
    std::string controller_answer_text;
    if (!expect_true(read_text_file(host_offer_path, &host_offer_text), "host offer sealed file read should succeed")) {
        return false;
    }
    if (!expect_true(read_text_file(controller_answer_path, &controller_answer_text), "controller answer sealed file read should succeed")) {
        return false;
    }

    const auto decrypted_offer = redclaw::protocol::decrypt_offer_blob_v1(host_offer_text, passphrase);
    const auto decrypted_answer = redclaw::protocol::decrypt_offer_blob_v1(controller_answer_text, passphrase);

    bool ok = true;
    ok = expect_true(decrypted_offer.ok, "decrypted host offer should succeed") && ok;
    ok = expect_true(decrypted_answer.ok, "decrypted controller answer should succeed") && ok;
    ok = expect_true(decrypted_offer.value.description_sdp == host_offer.description_sdp, "host description_sdp should roundtrip") && ok;
    ok = expect_true(decrypted_answer.value.description_sdp == controller_answer.description_sdp, "controller description_sdp should roundtrip") && ok;
    ok = expect_true(decrypted_offer.value.candidates.size() == host_offer.candidates.size(), "host candidates should roundtrip") && ok;
    ok = expect_true(decrypted_answer.value.candidates.size() == controller_answer.candidates.size(), "controller candidates should roundtrip") && ok;

    std::filesystem::remove(host_offer_path, ec);
    std::filesystem::remove(controller_answer_path, ec);
    std::filesystem::remove(temp_dir, ec);

    return ok;
}

bool test_sealed_file_wrong_passphrase_rejected() {
    const auto blob = make_blob(
        "v=0\na=setup:actpass\na=mid:0\n",
        "nonce-wrong-passphrase-03",
        1710000003000ULL);

    const auto encrypted = redclaw::protocol::encrypt_offer_blob_v1(blob, "sealed-passphrase-012345");
    if (!expect_true(
            encrypted.ok,
            std::string("encryption should succeed before wrong passphrase check: ") + encrypted.error)) {
        return false;
    }

    const auto wrong = redclaw::protocol::decrypt_offer_blob_v1(encrypted.value, "incorrect-passphrase-xxxxx");
    return expect_true(!wrong.ok, "decrypt with wrong passphrase should fail");
}

bool test_sealed_file_trickle_description_without_candidates_roundtrips() {
    auto blob = make_blob(
        "v=0\na=setup:active\na=mid:0\n",
        "nonce-trickle-answer-05",
        1710000005000ULL);
    blob.trickle_ice = true;
    blob.candidates.clear();

    const std::string passphrase = "sealed-trickle-passphrase-012345";
    const auto encrypted = redclaw::protocol::encrypt_offer_blob_v1(blob, passphrase);
    if (!expect_true(
            encrypted.ok,
            std::string("candidate-free trickle description should encrypt: ") + encrypted.error)) {
        return false;
    }
    const auto decrypted = redclaw::protocol::decrypt_offer_blob_v1(encrypted.value, passphrase);
    bool ok = expect_true(decrypted.ok, "candidate-free trickle description should decrypt")
        && expect_true(decrypted.value.trickle_ice, "trickle marker should survive sealed exchange")
        && expect_true(decrypted.value.candidates.empty(), "trickle description should remain candidate-free");

    blob.candidates.push_back(
        "candidate:9 1 udp 2122260223 192.0.2.1 50009 typ host");
    const auto updated = redclaw::protocol::encrypt_offer_blob_v1(blob, passphrase);
    if (!expect_true(updated.ok, "later trickled candidate update should encrypt")) {
        return false;
    }
    const auto updated_decrypted = redclaw::protocol::decrypt_offer_blob_v1(
        updated.value, passphrase);
    ok = expect_true(updated_decrypted.ok, "later trickled candidate update should decrypt") && ok;
    ok = expect_true(
             updated_decrypted.value.description_sdp == decrypted.value.description_sdp,
             "trickled candidate update must retain the same description") && ok;
    ok = expect_true(
             updated_decrypted.value.candidates.size() == 1,
             "later trickled candidate update should add exactly one candidate") && ok;
    return ok;
}

bool test_sealed_file_tampered_blob_rejected() {
    const auto blob = make_blob(
        "v=0\na=setup:actpass\na=mid:0\n",
        "nonce-tampered-blob-04",
        1710000004000ULL);

    const std::string passphrase = "sealed-passphrase-012345";
    const auto encrypted = redclaw::protocol::encrypt_offer_blob_v1(blob, passphrase);
    if (!expect_true(
            encrypted.ok,
            std::string("encryption should succeed before tamper check: ") + encrypted.error)) {
        return false;
    }

    std::string tampered = encrypted.value;
    for (char& ch : tampered) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = (ch == 'A') ? 'B' : 'A';
            break;
        }
        if (ch >= 'a' && ch <= 'z') {
            ch = (ch == 'a') ? 'b' : 'a';
            break;
        }
        if (ch >= '0' && ch <= '9') {
            ch = (ch == '0') ? '1' : '0';
            break;
        }
    }

    const auto tampered_result = redclaw::protocol::decrypt_offer_blob_v1(tampered, passphrase);
    return expect_true(!tampered_result.ok, "decrypt should fail for tampered sealed blob")
        && expect_true(!tampered_result.error.empty(), "tampered decrypt error should be populated");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_sealed_file_exchange_roundtrip() && ok;
    ok = test_sealed_file_trickle_description_without_candidates_roundtrips() && ok;
    ok = test_sealed_file_wrong_passphrase_rejected() && ok;
    ok = test_sealed_file_tampered_blob_rejected() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_sealed_file_signaling_integration_tests" << '\n';
    return 0;
}
