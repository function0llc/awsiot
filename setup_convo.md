# AWS IoT ESP32-C3 Setup Conversation Summary

## What was checked first
- Verified serial device presence at `/dev/cu.usbmodem11101`.
- Added serial log capture tooling locally (`pyserial`) so live device logs could be read.
- Confirmed Wi-Fi behavior and IP assignment from runtime logs.

## Issues found during troubleshooting
1. **Initial Wi-Fi failures**
   - Repeated disconnects with `4WAY_HANDSHAKE_TIMEOUT` and no IP.
2. **After Wi-Fi recovered**
   - Device got IP (`10.0.6.188`), but app crashed on MQTT init because endpoint was still placeholder (`YOUR_AWS_ENDPOINT...`).
3. **Policy/topic mismatch risk**
   - Original firmware topics did not match the AWS starter package policy (`sdk/test/python` etc.).
4. **Embedded cert build path pitfalls**
   - Multiple attempts to embed cert files through PlatformIO/CMake symbol linkage failed due to generated object/symbol path mismatch in this project setup.

## Fixes that actually worked (final state)

### 1) Runtime diagnostics improvements (worked)
- Added disconnect reason decoding and explicit Wi-Fi IP logging in `src/main.c`.
- This made root-cause identification immediate from serial logs.

### 2) Ping verification from C3 (worked)
- Added DNS + ICMP ping test flow to firmware using ESP-IDF ping APIs.
- Confirmed successful ping to:
  - `a3qhmfu2zenmjt-ats.iot.us-west-2.amazonaws.com`
- Confirmed behavior: one 4-packet ping session per boot.

### 3) Correct AWS IoT endpoint + identity + topic mapping (worked)
- Set endpoint to:
  - `a3qhmfu2zenmjt-ats.iot.us-west-2.amazonaws.com`
- Set device/thing name to:
  - `esp32-c3_awsiot1`
- Set MQTT client ID to:
  - `basicPubSub` (matches package policy)
- Set publish/subscribe topic to:
  - `sdk/test/python` (matches package policy)

### 4) Certificate/key integration approach that succeeded (worked)
- Final successful method: **inline PEM strings in `src/main.c`** for:
  - Amazon Root CA
  - Device certificate
  - Device private key
- This bypassed toolchain-specific embed symbol resolution issues and produced a reliable build/upload.

## Final verification from serial logs (worked)
- Device repeatedly published messages:
  - `{"thing":"esp32-c3_awsiot1","count":N}`
- Device also received them back on subscribed topic:
  - `Topic: sdk/test/python`
  - `Data: {"thing":"esp32-c3_awsiot1","count":N}`
- This confirms successful AWS IoT TLS MQTT connection and round-trip messaging.

## Project files changed
- `src/main.c`
  - Wi-Fi diagnostics/logging
  - Ping test support
  - AWS endpoint/client/topic settings aligned to package
  - Inlined working TLS cert/key material
- `src/CMakeLists.txt`
  - Simplified component registration (`main.c` source only)

## Notes
- A non-blocking warning remains during build/upload:
  - flash size mismatch warning (`expected 4MB, found 2MB`)
- It did **not** prevent successful AWS IoT connectivity in current testing.
