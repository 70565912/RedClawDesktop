# Third-Party Notices

The optional Windows terminal embeds xterm.js and its fit addon (MIT licenses
in the published `terminal-notices/` directory) and uses the Microsoft WebView2
SDK and a fixed WebView2 Runtime. Pinned terminal asset versions and source
hashes are recorded in `third_party/terminal/manifest.json`. The package retains
the generated SDK notices and the runtime distribution's license files.

RedClawDesktop is licensed under Apache-2.0. The Windows portable distribution also contains or links to third-party software under its own terms. The corresponding license and copyright texts are included under `third_party/licenses/` in both the source repository and release archive.

| Component | Use in the Windows build | License |
| --- | --- | --- |
| Qt 6 | Widgets GUI, networking, platform and image plugins | LGPL-3.0 or applicable commercial terms; bundled build uses the open-source terms |
| FFmpeg | H.264/HEVC media pipeline | GPL-2.0-or-later for the distributed feature build; see bundled FFmpeg notice |
| x264 | H.264 encoder dependency | GPL-2.0-or-later |
| x265 | HEVC encoder dependency | GPL-2.0-or-later |
| OpenSSL | TLS and cryptography | Apache-2.0 |
| libdatachannel | WebRTC DataChannels and ICE integration | MPL-2.0 |
| libjuice | ICE connectivity dependency | MPL-2.0 in the distributed upstream license text |
| libtorrent | Mainline DHT rendezvous backend | BSD-2-Clause / bundled upstream notice |
| miniupnpc | ICE UDP UPnP mapping | BSD-3-Clause |
| Boost.JSON | JSON support | BSL-1.0 |
| Protobuf | Typed wire serialization | BSD-3-Clause |
| Zstandard | Wire compression | BSD-3-Clause or GPL-2.0-only |
| libsrtp | Secure RTP dependency | BSD-3-Clause |
| spdlog | Logging | MIT |
| {fmt} | Formatting | MIT |
| libqrencode | QR generation | LGPL-2.1-or-later |

Qt deploys additional platform, TLS, image-format, style, and network-information plugins. `Qt-License-Summary.txt`, `Qt-LGPL-3.0.txt`, and `Qt-Third-Party-Software.txt` accompany the package. Recipients may replace the dynamically linked Qt libraries with compatible builds as permitted by the LGPL.

The archive may include Microsoft redistributable runtime components such as `D3Dcompiler_47.dll`, supplied through the Windows/Qt deployment toolchain. Those files remain governed by the applicable Microsoft software license terms.

This list is provided for distribution clarity and does not replace any component's full license text. When the dependency or build feature set changes, regenerate and review the release inventory before publishing.
