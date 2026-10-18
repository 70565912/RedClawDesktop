# AMD AMF 1.5.3 archive integrity repair

The [upstream vcpkg port](https://github.com/microsoft/vcpkg/blob/7daab73d066d04dd9b4661c21b51631fa72d1945/ports/amd-amf/portfile.cmake)
recorded the original header archive's SHA512 on 2026-10-02. AMD subsequently
uploaded the same-named release asset on 2026-10-16, causing a fresh CI install
to fail with `unexpected hash`. This overlay keeps AMF 1.5.3 and strict SHA512
validation for both the archive and license; no encoder feature is disabled.

Provenance checked on 2026-10-18:

- [Official release metadata](https://api.github.com/repos/GPUOpen-LibrariesAndSDKs/AMF/releases/tags/v1.5.3):
  asset `648395569`, `AMF-headers-v1.5.3.tar.gz`, 88,584 bytes,
  created/updated `2026-10-16T19:36:31Z`.
- The downloaded archive's SHA256 equals the official asset digest:
  `0f521ad98041f8001147cded4a660127f18b1c27a801a5cd691bda1fa48381be`.
- Its SHA512 equals the value pinned in `portfile.cmake`.
- All 57 extracted files exactly match Git blob hashes under `amf/public/include`
  in the [v1.5.3 source commit](https://github.com/GPUOpen-LibrariesAndSDKs/AMF/tree/57ee0cfb0e1a08d9041f77435653794c1b012be0/amf/public/include).
  No source header is missing and no extra file is present. Verification used
  `git hash-object --no-filters`, so line-ending conversion cannot mask changes.
- The license is fetched from that full commit ID and retains upstream vcpkg's
  unchanged SHA512. Asset-specific download filenames avoid colliding with an
  older cached archive or another port's generic `LICENSE.txt`.

The old archive was not available for comparison; this establishes the current
asset's correspondence to tagged source, not why AMD replaced the archive.
Future changed bytes still fail checksum verification. Remove this overlay and
its registration once the upstream port supplies this verified asset (or a
reviewed newer version) and a clean dependency install passes.
