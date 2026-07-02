/*
 * fritz-rctd
 *
 * Copyright (C) 2026 Crazor
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ---
 *
 * Standalone command-line program that drives the SIP orchestration for a
 * reversed FRITZ!Box Click-to-Dial flow:
 *
 *   1. Dial Call A -> the user's own extension/phone.
 *   2. Wait for CONFIRMED. Call A stays SIP-wise active (no formal hold).
 *   3. Dial Call B -> the external target number.
 *   4. Do NOT wait for Call B to be CONFIRMED - only until a SIP dialog
 *      exists (e.g. 180/183 with a to-tag, typically within a few
 *      100ms). Trigger an attended transfer (REFER with Replaces)
 *      immediately afterwards, while Call B is still ringing. The
 *      FRITZ!Box then takes over Call B itself and connects the user's
 *      extension directly to the still-ringing/later-answered Call B -
 *      the user then hears the FRITZ!Box's own native ringback/busy
 *      tone, not audio relayed through this client. This also avoids
 *      the FRITZ!Box's own "call is being held" announcement, since
 *      Call A is never put on formal SIP hold.
 *   5. Tear down both call objects once the transfer is confirmed.
 *
 * Configuration via FRITZ_RCTD_* environment variables and/or CLI
 * flags (a flag takes precedence over the matching environment variable)
 * - see --help.
 *
 * All diagnostic/SIP logs go to stderr. stdout contains exactly one JSON
 * line with the result (exception: --help/-h).
 */

#include <pjsua2.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

using namespace pj;

namespace {

// Set by a signal handler (SIGINT/SIGTERM) - must therefore consist ONLY
// of async-signal-safe operations (std::atomic<bool>::store is one). All
// wait loops poll this flag so that active calls can still be cleaned up
// (hangup) on interruption instead of leaving dangling SIP dialogs.
std::atomic<bool> g_interrupted{false};

extern "C" void handleTermSignal(int /*signum*/)
{
    g_interrupted.store(true);
}

constexpr auto kPollInterval = std::chrono::milliseconds(100);

// Exit codes: distinguishable by callers without parsing stdout (even
// though the JSON on stdout is the primary result).
enum ExitCode {
    EXIT_OK = 0,
    EXIT_GENERIC_ERROR = 1,
    EXIT_OWN_BUSY = 2,
    EXIT_OWN_NO_ANSWER = 3,
    EXIT_OWN_REJECTED = 4,
    EXIT_TARGET_BUSY = 5,
    EXIT_TARGET_NO_ANSWER = 6,
    EXIT_TARGET_REJECTED = 7,
    EXIT_TRANSFER_FAILED = 8,
    EXIT_REGISTRATION_FAILED = 9,
    EXIT_TRANSPORT_ERROR = 10,
    EXIT_BAD_ARGS = 11,
    EXIT_INTERRUPTED = 12,
    EXIT_OWN_HANGUP = 13,
};

std::string jsonEscape(const std::string &s)
{
    std::ostringstream out;
    for (char c : s) {
        switch (c) {
            case '"':  out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out << buf;
                } else {
                    out << c;
                }
        }
    }
    return out.str();
}

void emitResult(const std::string &status, const std::string &code,
                 const std::string &message)
{
    std::cout << "{\"status\":\"" << jsonEscape(status) << "\""
               << ",\"code\":\"" << jsonEscape(code) << "\""
               << ",\"message\":\"" << jsonEscape(message) << "\""
               << "}" << std::endl;
}

/* Redirects PJSIP log output to stderr, so stdout only ever contains the
 * final JSON result. */
class StderrLogWriter : public LogWriter {
public:
    virtual void write(const LogEntry &entry) override
    {
        std::cerr << entry.msg << std::endl;
    }
};

struct Options {
    std::string domain;
    int sipPort = 5060;
    std::string username;
    std::string password;
    std::string ownTarget;
    std::string targetNumber;
    std::string displayName;
    int localPort = 0;
    int ownTimeoutSec = 30;
    int targetTimeoutSec = 30;
    int transferTimeoutSec = 10;
    int regTimeoutSec = 10;
    int logLevel = 3;
};

/* Shared wait state for one call leg: filled from the pjsua2 worker thread
 * (callbacks), awaited from the main thread via condition_variable. */
struct CallWaitState {
    std::mutex mtx;
    std::condition_variable cv;
    bool confirmed = false;
    // Set as soon as a SIP dialog exists for this call (state
    // EARLY/CONNECTING/CONFIRMED - i.e. already at 180/183 with a
    // to-tag). That's already enough to reference it via xferReplaces();
    // it does not need to be CONFIRMED (answered) yet.
    bool established = false;
    bool disconnected = false;
    pjsip_status_code lastStatusCode = PJSIP_SC_NULL;
    std::string lastReason;
};

struct TransferWaitState {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    pjsip_status_code statusCode = PJSIP_SC_NULL;
    std::string reason;
    // Set on EVERY onCallTransferStatus notification, including
    // non-final ones (e.g. "100 Accepted"). Some FRITZ!Box firmware
    // versions don't send a final NOTIFY after a successful attended
    // transfer, and instead just end both call legs with a BYE - that
    // case is then recognized via Call A's disconnect (see below), but
    // only trusted if the transfer was actually accepted first.
    bool accepted = false;
};

struct RegWaitState {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool active = false;
    int code = 0;
};

class AttendantCall : public Call {
public:
    AttendantCall(Account &acc, CallWaitState &waitState)
        : Call(acc), waitState_(waitState)
    {}

    virtual void onCallState(OnCallStateParam &prm) override
    {
        PJ_UNUSED_ARG(prm);
        CallInfo ci;
        try {
            ci = getInfo();
        } catch (const Error &) {
            return;
        }

        std::cerr << "[call] state=" << ci.stateText
                   << " lastStatus=" << ci.lastStatusCode
                   << " reason=" << ci.lastReason << std::endl;

        std::lock_guard<std::mutex> lock(waitState_.mtx);
        if (ci.state == PJSIP_INV_STATE_CONFIRMED) {
            waitState_.confirmed = true;
            waitState_.established = true;
            waitState_.cv.notify_all();
        } else if (ci.state == PJSIP_INV_STATE_EARLY ||
                   ci.state == PJSIP_INV_STATE_CONNECTING) {
            waitState_.established = true;
            waitState_.cv.notify_all();
        } else if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
            waitState_.disconnected = true;
            waitState_.lastStatusCode = ci.lastStatusCode;
            waitState_.lastReason = ci.lastReason;
            waitState_.cv.notify_all();
        }
    }

    virtual void onCallTransferStatus(OnCallTransferStatusParam &prm) override
    {
        std::cerr << "[transfer] status=" << prm.statusCode
                   << " reason=" << prm.reason
                   << " final=" << prm.finalNotify << std::endl;

        if (!transferWaitState_) {
            return;
        }
        std::lock_guard<std::mutex> lock(transferWaitState_->mtx);
        transferWaitState_->accepted = true;
        if (prm.finalNotify) {
            transferWaitState_->done = true;
            transferWaitState_->statusCode = prm.statusCode;
            transferWaitState_->reason = prm.reason;
            transferWaitState_->cv.notify_all();
            prm.cont = false;
        }
    }

    void watchTransfer(TransferWaitState &t) { transferWaitState_ = &t; }

private:
    CallWaitState &waitState_;
    TransferWaitState *transferWaitState_ = nullptr;
};

class AttendantAccount : public Account {
public:
    explicit AttendantAccount(RegWaitState &regState) : regState_(regState) {}

    virtual void onRegState(OnRegStateParam &prm) override
    {
        AccountInfo ai = getInfo();
        std::cerr << "[reg] active=" << ai.regIsActive
                   << " code=" << prm.code << std::endl;

        std::lock_guard<std::mutex> lock(regState_.mtx);
        regState_.done = true;
        regState_.active = ai.regIsActive;
        regState_.code = prm.code;
        regState_.cv.notify_all();
    }

private:
    RegWaitState &regState_;
};

/* Waits until confirmed or disconnected is set, the timeout expires, or a
 * SIGINT/SIGTERM arrives (outInterrupted). Returns true if CONFIRMED was
 * reached. Polls in short intervals instead of blocking once, so that
 * g_interrupted is noticed promptly. */
bool waitForCallOutcome(CallWaitState &state, int timeoutSec,
                         pjsip_status_code &outStatusCode,
                         std::string &outReason, bool &outTimedOut,
                         bool &outInterrupted)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    std::unique_lock<std::mutex> lock(state.mtx);
    while (!state.confirmed && !state.disconnected) {
        if (g_interrupted.load()) {
            outInterrupted = true;
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            outTimedOut = true;
            return false;
        }
        state.cv.wait_for(lock, kPollInterval);
    }
    outTimedOut = false;
    if (state.confirmed) {
        return true;
    }
    outStatusCode = state.lastStatusCode;
    outReason = state.lastReason;
    return false;
}

/* Waits only until a SIP dialog exists for `state` (established) - that
 * is enough for xferReplaces(); CONFIRMED (answered) is not awaited.
 * Additionally aborts immediately with outOtherDisconnected=true if
 * `other` ends while waiting. Used while dialing Call B to watch Call A
 * (the user's own phone): if the user hangs up there before Call B has
 * even built a dialog, Call B should not keep ringing unheard, instead of
 * only failing with a confusing error at the next step (attended
 * transfer). */
bool waitForDialogOrOtherDisconnect(CallWaitState &state, CallWaitState &other,
                                     int timeoutSec,
                                     pjsip_status_code &outStatusCode,
                                     std::string &outReason, bool &outTimedOut,
                                     bool &outInterrupted,
                                     bool &outOtherDisconnected)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    while (true) {
        {
            std::lock_guard<std::mutex> lock(state.mtx);
            if (state.established) {
                return true;
            }
            if (state.disconnected) {
                outStatusCode = state.lastStatusCode;
                outReason = state.lastReason;
                return false;
            }
        }
        {
            std::lock_guard<std::mutex> lock(other.mtx);
            if (other.disconnected) {
                outOtherDisconnected = true;
                return false;
            }
        }
        if (g_interrupted.load()) {
            outInterrupted = true;
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            outTimedOut = true;
            return false;
        }
        pj_thread_sleep(100);
    }
}

/* Classifies a SIP status code for a failed call into a rough category
 * (busy / no-answer / rejected), so every failure exits cleanly with an
 * understandable message. */
struct FailureClassification {
    ExitCode exitCode;
    std::string code;
};

FailureClassification classifyFailure(pjsip_status_code status, bool timedOut,
                                       bool isOwnLeg)
{
    if (timedOut) {
        return {isOwnLeg ? EXIT_OWN_NO_ANSWER : EXIT_TARGET_NO_ANSWER,
                isOwnLeg ? "OWN_NO_ANSWER" : "TARGET_NO_ANSWER"};
    }
    switch (status) {
        case PJSIP_SC_BUSY_HERE:
        case PJSIP_SC_BUSY_EVERYWHERE:
            return {isOwnLeg ? EXIT_OWN_BUSY : EXIT_TARGET_BUSY,
                    isOwnLeg ? "OWN_BUSY" : "TARGET_BUSY"};
        case PJSIP_SC_TEMPORARILY_UNAVAILABLE:
        case PJSIP_SC_REQUEST_TIMEOUT:
        case PJSIP_SC_NOT_FOUND:
            return {isOwnLeg ? EXIT_OWN_NO_ANSWER : EXIT_TARGET_NO_ANSWER,
                    isOwnLeg ? "OWN_NO_ANSWER" : "TARGET_NO_ANSWER"};
        default:
            return {isOwnLeg ? EXIT_OWN_REJECTED : EXIT_TARGET_REJECTED,
                    isOwnLeg ? "OWN_REJECTED" : "TARGET_REJECTED"};
    }
}

void hangupQuietly(Call *call)
{
    if (!call) return;
    try {
        CallOpParam prm;
        call->hangup(prm);
    } catch (const Error &) {
        // Call may already have ended - not a problem during cleanup.
    }
}

std::string envOrDefault(const char *name, const std::string &fallback)
{
    const char *v = std::getenv(name);
    return (v && *v != '\0') ? std::string(v) : fallback;
}

/* Reads an integer environment variable if set and non-empty - leaves
 * `value` unchanged if the variable is absent. Returns false if it is
 * set but not a valid integer. */
bool applyEnvInt(const char *name, int &value, std::string &errorMsg)
{
    const char *v = std::getenv(name);
    if (!v || *v == '\0') {
        return true;
    }
    char *end = nullptr;
    long parsed = std::strtol(v, &end, 10);
    if (end == v || *end != '\0') {
        errorMsg = std::string("Environment variable ") + name +
                   " must be an integer, was: '" + v + "'";
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

/* Fills `opts` with values from FRITZ_RCTD_* environment variables.
 * Called before parseArgs(), so that CLI flags (if given) keep
 * precedence afterwards. --own/--target differ per call and deliberately
 * remain pure CLI arguments without an environment variable. */
bool applyEnvDefaults(Options &opts, std::string &errorMsg)
{
    opts.domain = envOrDefault("FRITZ_RCTD_SIP_DOMAIN", opts.domain);
    opts.username = envOrDefault("FRITZ_RCTD_SIP_USERNAME", opts.username);
    opts.password = envOrDefault("FRITZ_RCTD_SIP_PASSWORD", opts.password);
    opts.displayName = envOrDefault("FRITZ_RCTD_DISPLAY", opts.displayName);
    return applyEnvInt("FRITZ_RCTD_SIP_PORT", opts.sipPort, errorMsg) &&
           applyEnvInt("FRITZ_RCTD_OWN_TIMEOUT_SEC", opts.ownTimeoutSec, errorMsg) &&
           applyEnvInt("FRITZ_RCTD_TARGET_TIMEOUT_SEC", opts.targetTimeoutSec, errorMsg) &&
           applyEnvInt("FRITZ_RCTD_TRANSFER_TIMEOUT_SEC", opts.transferTimeoutSec, errorMsg) &&
           applyEnvInt("FRITZ_RCTD_REG_TIMEOUT_SEC", opts.regTimeoutSec, errorMsg) &&
           applyEnvInt("FRITZ_RCTD_LOG_LEVEL", opts.logLevel, errorMsg);
}

const char *kHelpText =
    "fritz-rctd --own <own extension> --target <external target number> [options]\n"
    "\n"
    "Dials the user's own extension first, then the external target\n"
    "number, and connects both via an attended transfer (REFER+Replaces)\n"
    "as soon as Call B has built a SIP dialog (not only after it is\n"
    "answered).\n"
    "\n"
    "Result: exactly one JSON line on stdout\n"
    "({\"status\":...,\"code\":...,\"message\":...}), diagnostic/SIP logs go\n"
    "to stderr.\n"
    "\n"
    "Required (via flag or environment variable, flag takes precedence):\n"
    "  --domain DOMAIN                FRITZ!Box IP/hostname\n"
    "                                  (FRITZ_RCTD_SIP_DOMAIN)\n"
    "  --username USER                SIP username\n"
    "                                  (FRITZ_RCTD_SIP_USERNAME)\n"
    "\n"
    "Required, environment variable only (never a CLI flag, so it can't\n"
    "leak into another user's process listing):\n"
    "  FRITZ_RCTD_SIP_PASSWORD     SIP password\n"
    "\n"
    "Flag only, differs per call:\n"
    "  --own EXTENSION                 Own extension/number, e.g. **621\n"
    "  --target TARGETNUMBER           External target number\n"
    "\n"
    "Optional (flag overrides environment variable overrides default):\n"
    "  --sip-port PORT                 Default 5060 (FRITZ_RCTD_SIP_PORT)\n"
    "  --own-timeout SECONDS           Default 30 (FRITZ_RCTD_OWN_TIMEOUT_SEC)\n"
    "  --target-timeout SECONDS        Default 30\n"
    "                                  (FRITZ_RCTD_TARGET_TIMEOUT_SEC)\n"
    "  --transfer-timeout SECONDS      Default 10\n"
    "                                  (FRITZ_RCTD_TRANSFER_TIMEOUT_SEC)\n"
    "  --reg-timeout SECONDS           Default 10 (FRITZ_RCTD_REG_TIMEOUT_SEC)\n"
    "  --log-level 0-6                 Default 3 (FRITZ_RCTD_LOG_LEVEL)\n"
    "  --local-port PORT               Local SIP UDP port, default 0 (=random)\n"
    "  --display NAME                  Caller ID display name shown when Call\n"
    "                                  A (the own extension) rings, e.g. a\n"
    "                                  customer/ticket reference from the\n"
    "                                  calling program (FRITZ_RCTD_DISPLAY).\n"
    "                                  Only affects the own extension - the\n"
    "                                  FRITZ!Box presents its own identity to\n"
    "                                  the external target after transfer.\n"
    "\n"
    "  --help, -h                      Show this help\n";

bool parseArgs(int argc, char **argv, Options &opts, std::string &errorMsg)
{
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto needValue = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                errorMsg = std::string("Missing value for ") + name;
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--domain") {
            const char *v = needValue("--domain");
            if (!v) return false;
            opts.domain = v;
        } else if (arg == "--sip-port") {
            const char *v = needValue("--sip-port");
            if (!v) return false;
            opts.sipPort = std::atoi(v);
        } else if (arg == "--username") {
            const char *v = needValue("--username");
            if (!v) return false;
            opts.username = v;
        } else if (arg == "--own") {
            const char *v = needValue("--own");
            if (!v) return false;
            opts.ownTarget = v;
        } else if (arg == "--target") {
            const char *v = needValue("--target");
            if (!v) return false;
            opts.targetNumber = v;
        } else if (arg == "--display") {
            const char *v = needValue("--display");
            if (!v) return false;
            opts.displayName = v;
        } else if (arg == "--local-port") {
            const char *v = needValue("--local-port");
            if (!v) return false;
            opts.localPort = std::atoi(v);
        } else if (arg == "--own-timeout") {
            const char *v = needValue("--own-timeout");
            if (!v) return false;
            opts.ownTimeoutSec = std::atoi(v);
        } else if (arg == "--target-timeout") {
            const char *v = needValue("--target-timeout");
            if (!v) return false;
            opts.targetTimeoutSec = std::atoi(v);
        } else if (arg == "--transfer-timeout") {
            const char *v = needValue("--transfer-timeout");
            if (!v) return false;
            opts.transferTimeoutSec = std::atoi(v);
        } else if (arg == "--reg-timeout") {
            const char *v = needValue("--reg-timeout");
            if (!v) return false;
            opts.regTimeoutSec = std::atoi(v);
        } else if (arg == "--log-level") {
            const char *v = needValue("--log-level");
            if (!v) return false;
            opts.logLevel = std::atoi(v);
        } else {
            errorMsg = "Unknown argument: " + arg;
            return false;
        }
    }
    return true;
}

/* Called after applyEnvDefaults() and parseArgs(), so it checks the
 * final merged values - not just what was given via CLI flag. */
bool validateRequired(const Options &opts, std::string &errorMsg)
{
    if (opts.domain.empty() || opts.username.empty() || opts.password.empty()) {
        errorMsg = "--domain/FRITZ_RCTD_SIP_DOMAIN and "
                   "--username/FRITZ_RCTD_SIP_USERNAME are required "
                   "(as a flag or environment variable); "
                   "FRITZ_RCTD_SIP_PASSWORD is required (environment "
                   "variable only)";
        return false;
    }
    if (opts.ownTarget.empty() || opts.targetNumber.empty()) {
        errorMsg = "--own and --target are required";
        return false;
    }
    return true;
}

std::string sipUri(const std::string &user, const std::string &domain,
                    int port)
{
    std::ostringstream out;
    out << "sip:" << user << "@" << domain;
    if (port != 5060) {
        out << ":" << port;
    }
    return out.str();
}

/* Wraps `uri` in a SIP name-addr with `displayName` as the quoted display
 * name (`"Display Name" <sip:...>`), for use as an account's idUri so it
 * shows up as the caller ID when that account places a call - here, when
 * Call A rings the own extension. Rejects CR/LF in the display name
 * outright, since those could otherwise inject additional SIP headers
 * into the From line; `"` and `\` are backslash-escaped per the SIP
 * quoted-string grammar (RFC 3261). */
bool sipNameAddr(const std::string &displayName, const std::string &uri,
                  std::string &outNameAddr, std::string &errorMsg)
{
    if (displayName.find('\r') != std::string::npos ||
        displayName.find('\n') != std::string::npos) {
        errorMsg = "--display/FRITZ_RCTD_DISPLAY must not contain line breaks";
        return false;
    }
    std::string escaped;
    escaped.reserve(displayName.size());
    for (char c : displayName) {
        if (c == '"' || c == '\\') {
            escaped += '\\';
        }
        escaped += c;
    }
    outNameAddr = "\"" + escaped + "\" <" + uri + ">";
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << kHelpText;
            return EXIT_OK;
        }
    }

    std::signal(SIGINT, handleTermSignal);
    std::signal(SIGTERM, handleTermSignal);

    Options opts;
    std::string argError;
    if (!applyEnvDefaults(opts, argError)) {
        emitResult("error", "BAD_ARGS", argError);
        return EXIT_BAD_ARGS;
    }
    if (!parseArgs(argc, argv, opts, argError)) {
        emitResult("error", "BAD_ARGS", argError);
        return EXIT_BAD_ARGS;
    }
    if (!validateRequired(opts, argError)) {
        emitResult("error", "BAD_ARGS", argError);
        return EXIT_BAD_ARGS;
    }

    // Own-account idUri: plain "sip:user@domain" normally, or a SIP
    // name-addr with a caller-ID display name if --display/
    // FRITZ_RCTD_DISPLAY was given. This is the identity Call A presents
    // when ringing the own extension (see kHelpText).
    std::string ownIdUri = sipUri(opts.username, opts.domain, opts.sipPort);
    if (!opts.displayName.empty()) {
        std::string quoted;
        if (!sipNameAddr(opts.displayName, ownIdUri, quoted, argError)) {
            emitResult("error", "BAD_ARGS", argError);
            return EXIT_BAD_ARGS;
        }
        ownIdUri = quoted;
    }

    Endpoint ep;
    AttendantCall *callA = nullptr;
    AttendantCall *callB = nullptr;
    AttendantAccount *account = nullptr;
    int result = EXIT_GENERIC_ERROR;

    try {
        ep.libCreate();

        EpConfig epCfg;
        epCfg.logConfig.msgLogging = 1;
        epCfg.logConfig.level = opts.logLevel;
        epCfg.logConfig.consoleLevel = opts.logLevel;
        epCfg.logConfig.writer = new StderrLogWriter();
        ep.libInit(epCfg);

        TransportConfig tcfg;
        tcfg.port = opts.localPort;
        ep.transportCreate(PJSIP_TRANSPORT_UDP, tcfg);

        ep.libStart();

        // This process is not a media endpoint for human ears - both
        // call legs run over real phones, not this machine. Without real
        // sound hardware (see --disable-sound in
        // scripts/build_pjproject.sh) there is no "default audio
        // device", so the null device must be set explicitly - otherwise
        // Call::makeCall() already fails with PJMEDIA_EAUD_NODEFDEV.
        ep.audDevManager().setNullDev();
        std::cerr << "[init] pjsua2 started" << std::endl;

        RegWaitState regState;
        AccountConfig accCfg;
        accCfg.idUri = ownIdUri;
        accCfg.regConfig.registrarUri =
            "sip:" + opts.domain +
            (opts.sipPort != 5060 ? ":" + std::to_string(opts.sipPort) : "");
        AuthCredInfo cred("digest", "*", opts.username, 0, opts.password);
        accCfg.sipConfig.authCreds.push_back(PJSUA2_MOVE(cred));

        account = new AttendantAccount(regState);
        account->create(accCfg);

        {
            auto deadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(opts.regTimeoutSec);
            std::unique_lock<std::mutex> lock(regState.mtx);
            while (!regState.done) {
                if (g_interrupted.load()) {
                    emitResult("error", "INTERRUPTED",
                               "Interrupted (SIGINT/SIGTERM) during registration");
                    delete account;
                    ep.libDestroy();
                    return EXIT_INTERRUPTED;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    break;
                }
                regState.cv.wait_for(lock, kPollInterval);
            }
            if (!regState.done || !regState.active) {
                emitResult("error", "REGISTRATION_FAILED",
                           "Registration with " + opts.domain +
                               " failed or timed out");
                delete account;
                ep.libDestroy();
                return EXIT_REGISTRATION_FAILED;
            }
        }
        std::cerr << "[init] Registration successful" << std::endl;

        // --- Call A: own phone ---
        CallWaitState callAWait;
        callA = new AttendantCall(*account, callAWait);
        {
            CallOpParam prm(true);
            callA->makeCall(sipUri(opts.ownTarget, opts.domain, opts.sipPort),
                             prm);
        }
        std::cerr << "[callA] Call started -> " << opts.ownTarget
                   << std::endl;

        pjsip_status_code statusCode = PJSIP_SC_NULL;
        std::string reason;
        bool timedOut = false;
        bool interrupted = false;
        bool confirmed = waitForCallOutcome(callAWait, opts.ownTimeoutSec,
                                             statusCode, reason, timedOut, interrupted);
        if (interrupted) {
            hangupQuietly(callA);
            emitResult("error", "INTERRUPTED",
                       "Interrupted (SIGINT/SIGTERM) during Call A");
            result = EXIT_INTERRUPTED;
            goto cleanup;
        }
        if (!confirmed) {
            auto cls = classifyFailure(statusCode, timedOut, true);
            hangupQuietly(callA);
            emitResult("error", cls.code,
                       "Own phone (" + opts.ownTarget +
                           ") did not answer: " +
                           (timedOut ? "timeout" : reason));
            result = cls.exitCode;
            goto cleanup;
        }
        std::cerr << "[callA] CONFIRMED" << std::endl;

        // --- Call B: external target number ---
        {
            CallWaitState callBWait;
            callB = new AttendantCall(*account, callBWait);
            {
                CallOpParam prm(true);
                callB->makeCall(
                    sipUri(opts.targetNumber, opts.domain, opts.sipPort),
                    prm);
            }
            std::cerr << "[callB] Call started -> " << opts.targetNumber
                       << std::endl;

            // Do not wait for CONFIRMED - as soon as Call B has a SIP
            // dialog (typically within a few 100ms), hand it over to the
            // FRITZ!Box via attended transfer immediately. The FRITZ!Box
            // then takes over Call B itself, and the user hears its
            // native ringback/busy tone instead of audio relayed through
            // this client.
            bool ownHangup = false;
            bool established = waitForDialogOrOtherDisconnect(
                callBWait, callAWait, opts.targetTimeoutSec, statusCode, reason,
                timedOut, interrupted, ownHangup);
            if (interrupted) {
                hangupQuietly(callB);
                hangupQuietly(callA);
                emitResult("error", "INTERRUPTED",
                           "Interrupted (SIGINT/SIGTERM) during Call B");
                result = EXIT_INTERRUPTED;
                goto cleanup;
            }
            if (ownHangup) {
                // Call A has already ended (user hung up) - no need for
                // hangupQuietly(callA), just stop Call B so it doesn't
                // keep ringing unheard for the called party.
                hangupQuietly(callB);
                emitResult("error", "OWN_HANGUP",
                           "Own phone (" + opts.ownTarget +
                               ") was hung up before the target number (" +
                               opts.targetNumber + ") had built a dialog");
                result = EXIT_OWN_HANGUP;
                goto cleanup;
            }
            if (!established) {
                auto cls = classifyFailure(statusCode, timedOut, false);
                hangupQuietly(callB);
                hangupQuietly(callA);
                emitResult("error", cls.code,
                           "Target number (" + opts.targetNumber +
                               ") unreachable: " +
                               (timedOut ? "timeout" : reason));
                result = cls.exitCode;
                goto cleanup;
            }
            std::cerr << "[callB] Dialog established, transferring immediately"
                       << std::endl;

            // --- Attended transfer: Call A (own phone) is instructed
            // via REFER+Replaces to take over Call B (target) directly.
            // ---
            TransferWaitState transferWait;
            callA->watchTransfer(transferWait);
            try {
                CallOpParam xferPrm;
                callA->xferReplaces(*callB, xferPrm);
            } catch (const Error &err) {
                hangupQuietly(callB);
                hangupQuietly(callA);
                emitResult("error", "TRANSFER_FAILED",
                           "xferReplaces failed: " + err.info());
                result = EXIT_TRANSFER_FAILED;
                goto cleanup;
            }
            std::cerr << "[transfer] xferReplaces sent" << std::endl;

            // Some FRITZ!Box firmware versions don't send a final NOTIFY
            // for the REFER subscription after a successful attended
            // transfer, and instead end Call A directly with a BYE (200
            // "Normal call clearing"). So wait for TWO signals, whichever
            // arrives first: the final transfer NOTIFY, or Call A's
            // disconnect - but only treat the latter as success if the
            // transfer was actually accepted by the recipient beforehand
            // (otherwise it could be a spontaneous hangup).
            bool transferOk = false;
            bool determined = false;
            bool transferInterrupted = false;
            auto deadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(opts.transferTimeoutSec);
            while (!determined && std::chrono::steady_clock::now() < deadline) {
                if (g_interrupted.load()) {
                    transferInterrupted = true;
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(transferWait.mtx);
                    if (transferWait.done) {
                        determined = true;
                        transferOk = transferWait.statusCode >= 200 &&
                                     transferWait.statusCode < 300;
                        reason = transferWait.reason;
                        statusCode = transferWait.statusCode;
                    }
                }
                if (!determined) {
                    std::lock_guard<std::mutex> lock(callAWait.mtx);
                    if (callAWait.disconnected && transferWait.accepted) {
                        determined = true;
                        transferOk = callAWait.lastStatusCode == PJSIP_SC_OK;
                        reason = "Call A ended after accepted transfer: " +
                                 callAWait.lastReason;
                        statusCode = callAWait.lastStatusCode;
                    }
                }
                if (!determined) {
                    pj_thread_sleep(100);
                }
            }
            if (!determined && !transferInterrupted) {
                reason = "Timeout waiting for transfer confirmation";
            }

            // Tear down both legs in every case: on success, the
            // FRITZ!Box now connects A and B directly; on failure, no
            // dangling dialogs should remain.
            hangupQuietly(callA);
            hangupQuietly(callB);

            if (transferInterrupted) {
                emitResult("error", "INTERRUPTED",
                           "Interrupted (SIGINT/SIGTERM) during the transfer");
                result = EXIT_INTERRUPTED;
                goto cleanup;
            }

            if (!transferOk) {
                emitResult("error", "TRANSFER_FAILED",
                           "Attended transfer failed: " + reason);
                result = EXIT_TRANSFER_FAILED;
                goto cleanup;
            }
        }

        std::cerr << "[transfer] successful, leaving the media path"
                   << std::endl;
        emitResult("ok", "TRANSFERRED",
                   "Call successfully connected from " + opts.ownTarget + " to " +
                       opts.targetNumber);
        result = EXIT_OK;

    cleanup:
        delete callA;
        delete callB;
        delete account;  // also ends the registration
        ep.libDestroy();
        return result;

    } catch (const Error &err) {
        emitResult("error", "INTERNAL_ERROR", err.info());
        try {
            delete callA;
            delete callB;
            delete account;
            ep.libDestroy();
        } catch (...) {
        }
        return EXIT_GENERIC_ERROR;
    }
}
