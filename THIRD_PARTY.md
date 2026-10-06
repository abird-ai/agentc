# Third-party notices

`agentc` is MIT licensed (see LICENSE). Everything in this repository that was
not written for it is listed below. The design record (`.agents/design/`) draws
inspiration from [pi](https://github.com/earendil-works/pi) (MIT, © 2025 Mario
Zechner); no pi code, data or text is copied, so it carries no notice below.

## mbedTLS 3.6.2 — https://github.com/Mbed-TLS/mbedtls

Copyright (c) The Mbed TLS Contributors. Apache License 2.0.

Vendored as an unmodified upstream subset under `third_party/mbedtls/`
(`include/`, `library/`, `LICENSE`). Built from source in freestanding mode
(`-ffreestanding -nostdlib`) as the TLS 1.2/1.3 + X.509 client backend; no
runtime dependency, and no `dlopen` in the TLS path (the macOS/Windows dynamic
extension loader uses the OS loader and links no third-party code).

## Mozilla CA bundle — `third_party/cacert.pem`

From https://curl.se/ca/cacert.pem, MPL 2.0. Trust anchors for X.509
verification. Replaced at build time by a system bundle when present.
