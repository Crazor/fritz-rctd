[![CI (macOS)](https://img.shields.io/github/actions/workflow/status/Crazor/fritz-rctd/ci-macos.yml?branch=main&style=for-the-badge&label=CI%20%28macOS%29&logo=apple)](https://github.com/Crazor/fritz-rctd/actions/workflows/ci-macos.yml)
[![CI (Linux)](https://img.shields.io/github/actions/workflow/status/Crazor/fritz-rctd/ci-linux.yml?branch=main&style=for-the-badge&label=CI%20%28Linux%29&logo=linux)](https://github.com/Crazor/fritz-rctd/actions/workflows/ci-linux.yml)
[![License](https://img.shields.io/github/license/Crazor/fritz-rctd?style=for-the-badge&logo=gplv3)](LICENSE)
![Static Badge](https://img.shields.io/badge/FRITZBox-blue?style=for-the-badge&logo=avm)
![Static Badge](https://img.shields.io/badge/C%2B%2B-17-blue?style=for-the-badge&logo=cplusplus)
![Static Badge](https://img.shields.io/badge/made%20with-Claude%20Code-orange?style=for-the-badge&logo=claudecode)

# FRITZ!Box Reverse Click-to-Dial (fritz-rctd)

A standalone replacement for the FRITZ!Box's built-in Click-to-Dial
("Wählhilfe") feature, with the call order reversed.

The stock FRITZ!Box feature dials the external target number first, so the
external party hears a "please hold" announcement while you pick up your
own extension. `fritz-rctd` dials your own extension first instead, and
only dials the external number once you've answered. As soon
as the external call has built a SIP dialog (typically within a few
100ms, while it's still ringing - no need to wait for it to be answered),
the call is handed off to the FRITZ!Box via an attended transfer (SIP
REFER with Replaces). The FRITZ!Box then takes over the external call
itself and connects both sides directly once it's answered - this program
only stays in the media path for a fraction of a second.

Single self-contained CLI binary, configuration via environment variables
and/or CLI flags, result as a single JSON line on stdout. Any program that
can spawn a child process and read its stdout/exit code can use this directly.

## How it works

1. Dial Call A (the user's own extension) and wait for it to be answered.
2. Dial Call B (the external target number).
3. Wait only until Call B has a SIP dialog (e.g. a 180/183 response with a
   to-tag) - not until it's answered.
4. Immediately trigger an attended transfer (REFER with Replaces) from
   Call A to Call B. The FRITZ!Box takes over Call B and connects it
   directly to the user's extension once answered.
5. Tear down both local call legs once the transfer is confirmed.

Call A is never put on formal SIP hold, which avoids the FRITZ!Box's own
"call is being held" announcement. Because the handoff happens almost
immediately, the user hears the FRITZ!Box's own native ringback/busy tone
for Call B, not audio relayed through this program.

## Build

Supported platforms: macOS and Linux. (PJPROJECT builds on Windows too, but
via an entirely different, Visual-Studio-based build system that isn't
covered here.)

### 1. Build the PJPROJECT core libraries

```bash
./scripts/build_pjproject.sh          # builds into /tmp/pjproject (~2-5 minutes)
```

Requirements:
- macOS: Xcode Command Line Tools, Homebrew with `openssl@3`, CMake.
- Linux: a C/C++ toolchain (`build-essential` or equivalent), OpenSSL
  development headers (e.g. `libssl-dev` on Debian/Ubuntu), CMake, git.

If a PJPROJECT checkout already exists somewhere in the container (e.g. an
agent sandbox that pre-clones the repo into a workspace directory), the
script finds and builds that instead of fetching anything. Otherwise it
clones `pjsip/pjproject` from GitHub. In sandboxed environments that only
allow git/network access to repos you own (e.g. some cloud CI or agent
sandboxes) and have no pre-existing checkout, fork `pjsip/pjproject` to
your own account first, then point the script at your fork:

```bash
PJ_REPO_URL=https://github.com/<you>/pjproject ./scripts/build_pjproject.sh
```

### 2. Build the binary

```bash
cmake -S . -B build -DPJ_DIR=/tmp/pjproject
cmake --build build
```

Result: `build/fritz-rctd`, statically linked (no runtime dependency on
PJPROJECT's shared libraries, `.dylib` on macOS / `.so` on Linux).

## Configuration

SIP credentials and connection details are read from environment
variables, not hardcoded. The password in particular is accepted **only**
via the `FRITZ_RCTD_SIP_PASSWORD` environment variable and has no CLI
flag, so it can't leak into another user's process listing.

| Variable | Required | CLI flag | Meaning |
|---|---|---|---|
| `FRITZ_RCTD_SIP_DOMAIN` | yes | `--domain` | FRITZ!Box IP/hostname |
| `FRITZ_RCTD_SIP_USERNAME` | yes | `--username` | SIP username of the own extension |
| `FRITZ_RCTD_SIP_PASSWORD` | yes | *(none - env only)* | SIP password |
| `FRITZ_RCTD_SIP_PORT` | no | `--sip-port` | Default `5060` |
| `FRITZ_RCTD_OWN_TIMEOUT_SEC` | no | `--own-timeout` | Default `30` |
| `FRITZ_RCTD_TARGET_TIMEOUT_SEC` | no | `--target-timeout` | Default `30` |
| `FRITZ_RCTD_TRANSFER_TIMEOUT_SEC` | no | `--transfer-timeout` | Default `10` |
| `FRITZ_RCTD_REG_TIMEOUT_SEC` | no | `--reg-timeout` | Default `10` |
| `FRITZ_RCTD_LOG_LEVEL` | no | `--log-level` | Default `3` |

Where a variable has a matching CLI flag, the flag always takes
precedence over the environment variable. `--local-port` (local SIP UDP
port, default `0` = random) is the one option that is flag-only, with no
environment variable. See `--help` for the full list.

The binary does **not** load a `.env` file itself (only real environment
variables) - kept portable, so that any calling program/shell decides for
itself how to provide configuration. See `.env.example` for a template to
load manually.

## Usage

```bash
export FRITZ_RCTD_SIP_DOMAIN=192.168.178.1
export FRITZ_RCTD_SIP_USERNAME=620
export FRITZ_RCTD_SIP_PASSWORD=xxx
build/fritz-rctd --own '**621' --target 004912345678
```

All options: `build/fritz-rctd --help`.

**Own extension format (`--own`):** the FRITZ!Box web UI displays internal
numbers as `**621`. Only the format **with** both leading asterisks
(`**621`) is confirmed to work - the plain number without `**` (`621`) is
accepted by the FRITZ!Box (183 Session Progress) but the phone never
actually rings. In a shell, `**621` needs quotes, otherwise the shell
expands `*` as a glob pattern.

**`--target-timeout SECONDS`** overrides `FRITZ_RCTD_TARGET_TIMEOUT_SEC`
for a single run. It only bounds the wait until Call B builds a SIP
dialog at all (typically within a few 100ms) - after that, the transfer
happens immediately, without waiting for an answer. It's a safety net for
the rare case where the target number never responds at all (e.g. a
routing error):

```bash
build/fritz-rctd --own '**621' --target 004912345678 --target-timeout 15
```

**Result protocol:** stdout contains exactly one JSON line
(`{"status":"ok"|"error","code":"...","message":"..."}`), diagnostic/SIP
logs go to stderr. On success `status` is `ok` and `code` is
`TRANSFERRED`; on failure `status` is `error` and `code` is one of the
failure codes. The exit code is also machine-readable (0 = success, see
the `ExitCode` enum in `fritz-rctd.cpp` for every code).
Callers can check stdout (JSON) and/or the exit code - both carry the
same code.

## Notes

- Builds and runs on Linux (verified: builds cleanly, initializes
  pjsua2, and correctly reports `REGISTRATION_FAILED` against an
  unreachable domain), in addition to macOS. Call handling against a
  real FRITZ!Box has only been verified on macOS so far - please report
  an issue if the actual call flow behaves differently on Linux.
- Only tested against one FRITZ!Box firmware/device type (7590 AX on 8.20)
  for Call A (a SIP extension) and DECT phones. Behavior on analog phones
  or other firmware versions (number format, final-NOTIFY behavior,
  transfer-while-ringing support) is unverified.
- Some FRITZ!Box firmware versions don't send a final NOTIFY for the
  REFER subscription after a successful attended transfer, and instead
  end Call A directly with a BYE (200 "Normal call clearing"). This is
  handled as an alternate success signal (see `onCallTransferStatus` /
  the `accepted` flag in `fritz-rctd.cpp`).
- The transfer happens while Call B is still ringing (not yet answered).
  PJSIP itself only requires an established dialog for this, but whether
  a firmware reliably completes an attended transfer of a still-ringing
  call is only confirmed for one FRITZ!Box firmware (including correct
  caller ID display on the user's phone).
- The window in which `OWN_HANGUP` (hanging up the own phone before Call
  B has a dialog) can even apply is a few 100ms, since the transfer
  happens immediately. Hanging up during or after the transfer is
  entirely up to the FRITZ!Box's own call handling from that point on -
  same as a normal consultation-call transfer from a real phone.

## License

[GPL-3.0-or-later](LICENSE). This project links statically against
[PJSIP/PJPROJECT](https://github.com/pjsip/pjproject), which is licensed
GPL-2.0-or-later; the resulting binary is a combined work covered by the
GPL for that reason as well.
