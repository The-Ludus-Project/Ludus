"""Resumable-ASSERT decision tests with a controlled helper.

The driver plays the external helper over the control socketpair, scripting the
handshake and the DecisionReply so the no-debugger dialog path is exercised
deterministically. It verifies:

  * ContinueOnce resumes the ASSERT (condition ran once, control returned).
  * Terminate aborts (SIGABRT), no resume.
  * Repeated ASSERTs each get an independent incident id and resume.
  * A reply with the wrong incident id, a duplicate/stale id, an unknown kind,
    a malformed frame, or a closed helper never authorizes continuation.
  * A REQUIRE never resumes even when the helper would reply ContinueOnce.

argv: <assert-decision-child-binary>

No debugger is attached (native), so the runtime takes the control-endpoint
branch. Reports are drained off the report socket so the child never blocks.
"""

import os
import resource
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

from control_transport import pair, receive

HEADER = struct.Struct("<IHHII")
MAGIC = 0x4C554443
VERSION = 1
HEADER_SIZE = HEADER.size
MAX_PAYLOAD = 2048
KIND_HELLO = 1
KIND_HELLO_ACK = 2
KIND_DECISION_REQUEST = 3
KIND_DECISION_REPLY = 4
CONTINUE_ONCE = 1
TERMINATE = 0

CLEAN_ENV = {k: v for k, v in os.environ.items()
             if k not in ("CI", "CONTINUOUS_INTEGRATION", "GITHUB_ACTIONS", "GITLAB_CI",
                          "BUILDKITE", "JENKINS_URL", "TEAMCITY_VERSION", "LUDUS_CI")}

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def encode(kind, incident=0, payload=b""):
    return HEADER.pack(MAGIC, VERSION, kind, incident, len(payload)) + payload


def decode(frame):
    if len(frame) < HEADER_SIZE:
        return None
    magic, version, kind, incident, length = HEADER.unpack(frame[:HEADER_SIZE])
    if magic != MAGIC or version != VERSION or HEADER_SIZE + length != len(frame):
        return None
    return kind, incident, frame[HEADER_SIZE:]


def drain_reports(report_recv, stop, packets):
    """Drain queued datagrams after child exit before stopping on a quiet socket."""
    while True:
        try:
            packets.append(report_recv.recv(4096))
        except socket.timeout:
            if stop.is_set():
                return
        except OSError:
            return


def test_report_drain_after_exit():
    report_recv, report_send = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
    with report_recv, report_send:
        report_recv.settimeout(0.01)
        report_send.send(b"minimal report")
        report_send.send(b"detailed fatal message")
        stop = threading.Event()
        stop.set()
        packets = []
        drain_reports(report_recv, stop, packets)
        assert packets == [b"minimal report", b"detailed fatal message"], packets
    print("  Child exit preserves queued minimal and detailed reports OK")


def run(binary, mode, responder, timeout=8, extra_env=None):
    """Launch the child with report+control sockets; `responder(control)` plays
    the helper on a worker thread. The `timeout` is a watchdog: an accidental
    input wait (a runtime that blocks for a reply that never authorizes continue)
    surfaces as a TimeoutExpired failure rather than a hang. Captured report
    datagrams are appended to `result.report` so tests can assert visible text."""
    report_recv, report_send = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
    control_helper, control_engine = pair()
    report_recv.settimeout(0.1)
    stop = threading.Event()
    packets = []

    def helper():
        try:
            responder(control_helper)
        except OSError:
            pass
        finally:
            try:
                control_helper.shutdown(socket.SHUT_WR)
            except OSError:
                pass

    drainer = threading.Thread(target=drain_reports, args=(report_recv, stop, packets))
    worker = threading.Thread(target=helper)
    drainer.start()
    worker.start()
    env = {**CLEAN_ENV,
           "LUDUS_DIAGNOSTIC_REPORT_FD": str(report_send.fileno()),
           "LUDUS_DIAGNOSTIC_CONTROL_FD": str(control_engine.fileno())}
    if extra_env:
        env.update(extra_env)
    try:
        result = subprocess.run([binary, mode], capture_output=True, text=True, timeout=timeout,
                                env=env, pass_fds=(report_send.fileno(), control_engine.fileno()))
    finally:
        stop.set()
        worker.join(timeout=2)
        drainer.join(timeout=2)
        for s in (report_recv, report_send, control_helper, control_engine):
            s.close()
    result.report = b"".join(packets).decode("utf-8", "replace")
    return result


def expect_hello(control):
    """Read the Hello and reply HelloAck. Returns True on success."""
    frame = receive(control)
    decoded = decode(frame)
    if decoded is None or decoded[0] != KIND_HELLO:
        return False
    control.sendall(encode(KIND_HELLO_ACK))
    return True


def read_decision_request(control):
    """Read one DecisionRequest; return its incident id (or None)."""
    frame = receive(control)
    decoded = decode(frame)
    if decoded is None or decoded[0] != KIND_DECISION_REQUEST:
        return None
    return decoded[1]


def test_continue(binary):
    def responder(control):
        assert expect_hello(control)
        incident = read_decision_request(control)
        control.sendall(encode(KIND_DECISION_REPLY, incident, bytes([CONTINUE_ONCE])))
    result = run(binary, "single", responder)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    assert "ASSERT-RETURNED" in result.stdout, result.stdout
    assert result.stdout.count("CONDITION\n") == 1
    # The report is visible with its expression and message text.
    assert "expression=FailingCondition()" in result.report, result.report
    assert "decision-probe" in result.report, result.report
    print("  ContinueOnce resumes the ASSERT (report text + expression visible) OK")


def test_invalid_descriptor_environment(binary):
    for value in (None, "", "garbage", "0suffix", "-1", "+1", " 1",
                  "2147483648", "18446744073709551616", "9" * 1025):
        env = {key: item for key, item in CLEAN_ENV.items()
               if key not in ("LUDUS_DIAGNOSTIC_REPORT_FD", "LUDUS_DIAGNOSTIC_CONTROL_FD")}
        if value is not None:
            env.update(LUDUS_DIAGNOSTIC_REPORT_FD=value, LUDUS_DIAGNOSTIC_CONTROL_FD=value)
        result = subprocess.run([binary, "check"], capture_output=True, text=True,
                                stdin=subprocess.DEVNULL, timeout=8, env=env)
        assert result.returncode == 0, (value, result.returncode, result.stdout, result.stderr)
        assert "CHECK-FALSE" in result.stdout, (value, result.stdout)
        assert "CONTROL-NOT-READY" not in result.stdout, (value, result.stdout)
    print("  Missing, malformed and out-of-range descriptor values never select a transport OK")


def test_terminate(binary):
    def responder(control):
        assert expect_hello(control)
        incident = read_decision_request(control)
        control.sendall(encode(KIND_DECISION_REPLY, incident, bytes([TERMINATE])))
    result = run(binary, "single", responder)
    assert result.returncode == -signal.SIGABRT, (result.returncode, result.stderr)
    assert "ASSERT-RETURNED" not in result.stdout
    print("  Terminate aborts, no resume OK")


def test_repeated(binary):
    incidents = []

    def responder(control):
        assert expect_hello(control)
        for _ in range(2):
            incident = read_decision_request(control)
            incidents.append(incident)
            control.sendall(encode(KIND_DECISION_REPLY, incident, bytes([CONTINUE_ONCE])))
    result = run(binary, "repeated", responder)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    assert "FIRST-RETURNED" in result.stdout and "SECOND-RETURNED" in result.stdout
    assert len(incidents) == 2 and incidents[0] != incidents[1], incidents
    print("  Repeated ASSERTs get independent incident ids and resume OK")


def test_wrong_incident(binary):
    def responder(control):
        assert expect_hello(control)
        incident = read_decision_request(control)
        # Reply for a different incident id: must NOT authorize continuation.
        control.sendall(encode(KIND_DECISION_REPLY, incident + 999, bytes([CONTINUE_ONCE])))
    result = run(binary, "single", responder)
    assert result.returncode == -signal.SIGABRT, (result.returncode, result.stderr)
    print("  Wrong incident id => Terminate OK")


def test_wrong_kind(binary):
    def responder(control):
        assert expect_hello(control)
        incident = read_decision_request(control)
        # A reply with an unexpected kind must not continue.
        control.sendall(encode(KIND_HELLO_ACK, incident, b""))
    result = run(binary, "single", responder)
    assert result.returncode == -signal.SIGABRT, (result.returncode, result.stderr)
    print("  Wrong reply kind => Terminate OK")


def test_malformed(binary):
    def responder(control):
        assert expect_hello(control)
        read_decision_request(control)
        control.sendall(b"\x00\x01\x02")  # Too short / bad magic.
    result = run(binary, "single", responder)
    assert result.returncode == -signal.SIGABRT, (result.returncode, result.stderr)
    print("  Malformed reply => Terminate OK")


def test_helper_exit(binary):
    def responder(control):
        assert expect_hello(control)
        read_decision_request(control)
        control.close()  # Helper disconnects without replying.
    result = run(binary, "single", responder)
    assert result.returncode == -signal.SIGABRT, (result.returncode, result.stderr)
    print("  Helper disconnect => Terminate OK")


def test_require_never_resumes(binary):
    def responder(control):
        assert expect_hello(control)
        # Even offer ContinueOnce if a request arrives; REQUIRE must not send one
        # and must terminate regardless.
        try:
            incident = read_decision_request(control)
            if incident is not None:
                control.sendall(encode(KIND_DECISION_REPLY, incident, bytes([CONTINUE_ONCE])))
        except OSError:
            pass
    result = run(binary, "require", responder)
    assert result.returncode == -signal.SIGABRT, (result.returncode, result.stderr)
    assert "REQUIRE-RETURNED" not in result.stdout
    print("  REQUIRE never resumes OK")


def test_fatal_never_resumes(binary):
    def responder(control):
        assert expect_hello(control)
        try:
            incident = read_decision_request(control)
            if incident is not None:
                control.sendall(encode(KIND_DECISION_REPLY, incident, bytes([CONTINUE_ONCE])))
        except OSError:
            pass
    result = run(binary, "fatal", responder)
    assert result.returncode == -signal.SIGABRT, (result.returncode, result.stderr)
    assert "fatal-must-terminate" in result.report, result.report
    print("  FATAL never resumes OK")


def test_formatted_continue(binary):
    def responder(control):
        assert expect_hello(control)
        incident = read_decision_request(control)
        control.sendall(encode(KIND_DECISION_REPLY, incident, bytes([CONTINUE_ONCE])))
    result = run(binary, "formatted", responder)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    assert "ASSERT-RETURNED" in result.stdout, result.stdout
    assert result.stdout.count("CONDITION\n") == 1
    assert "formatted index=7" in result.report, result.report
    print("  Formatted ASSERT_F resumes (typed arg evaluated once, visible) OK")


def test_app_lock_continue(binary):
    def responder(control):
        assert expect_hello(control)
        incident = read_decision_request(control)
        control.sendall(encode(KIND_DECISION_REPLY, incident, bytes([CONTINUE_ONCE])))
    result = run(binary, "app-lock", responder)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    assert "ASSERT-RETURNED" in result.stdout, result.stdout
    print("  Resume while application lock held: no deadlock/self-recursion OK")


def test_check_returns_false(binary):
    # CHECK must never send a DecisionRequest; it returns false with a visible
    # report. If the child ever asked for a decision, that is a defect.
    asked = {"decision": False}

    def responder(control):
        assert expect_hello(control)
        try:
            if read_decision_request(control) is not None:
                asked["decision"] = True
        except OSError:
            pass
    result = run(binary, "check", responder)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    assert "CHECK-FALSE" in result.stdout, result.stdout
    assert "check-recovery-probe" in result.report, result.report
    assert not asked["decision"], "CHECK must not send a DecisionRequest"
    print("  CHECK returns false with visible report, no dialog OK")


def test_ci_overrides_interactive_request(binary):
    # Even with a helper that would reply ContinueOnce, a CI environment must
    # veto: the runtime terminates and never sends a DecisionRequest. An
    # interactive request cannot override CI.
    asked = {"decision": False}

    def responder(control):
        assert expect_hello(control)
        try:
            if read_decision_request(control) is not None:
                asked["decision"] = True
                # Offer continue anyway; the runtime must have already terminated.
        except OSError:
            pass
    result = run(binary, "single", responder, extra_env={"CI": "true"})
    assert result.returncode == -signal.SIGABRT, (result.returncode, result.stderr)
    assert not asked["decision"], "CI veto must prevent any DecisionRequest"
    print("  CI vetoes an interactive request (no DecisionRequest, terminate) OK")


def test_closed_peer_on_next_request(binary):
    def responder(control):
        assert expect_hello(control)
        incident = read_decision_request(control)
        control.sendall(encode(KIND_DECISION_REPLY, incident, bytes([CONTINUE_ONCE])))
        control.close()
    result = run(binary, "repeated", responder)
    assert result.returncode == -signal.SIGABRT, (result.returncode, result.stderr)
    assert "FIRST-RETURNED" in result.stdout and "SECOND-RETURNED" not in result.stdout
    print("  Broken control peer on next request => Terminate, never SIGPIPE OK")


def test_stream_fragmentation(binary):
    if sys.platform != "darwin":
        return
    def responder(control):
        hello = receive(control)
        assert decode(hello)[0] == KIND_HELLO
        for byte in encode(KIND_HELLO_ACK):
            control.sendall(bytes([byte]))
            time.sleep(0.002)
        incident = read_decision_request(control)
        reply = encode(KIND_DECISION_REPLY, incident, bytes([CONTINUE_ONCE]))
        for byte in reply:
            control.sendall(bytes([byte]))
            time.sleep(0.002)
    result = run(binary, "single", responder)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    assert "ASSERT-RETURNED" in result.stdout
    print("  Fragmented Darwin handshake and decision resume OK")


def test_stream_coalesced_stale_reply(binary):
    if sys.platform != "darwin":
        return
    def responder(control):
        assert expect_hello(control)
        incident = read_decision_request(control)
        reply = encode(KIND_DECISION_REPLY, incident, bytes([CONTINUE_ONCE]))
        control.sendall(reply + reply)
        read_decision_request(control)
    result = run(binary, "repeated", responder)
    assert result.returncode == -signal.SIGABRT, (result.returncode, result.stderr)
    assert "FIRST-RETURNED" in result.stdout and "SECOND-RETURNED" not in result.stdout
    print("  Coalesced stale Darwin reply never resumes another incident OK")


def test_stream_oversized_reply(binary):
    if sys.platform != "darwin":
        return
    def responder(control):
        assert expect_hello(control)
        incident = read_decision_request(control)
        control.sendall(HEADER.pack(MAGIC, VERSION, KIND_DECISION_REPLY, incident, MAX_PAYLOAD + 1))
    result = run(binary, "single", responder)
    assert result.returncode == -signal.SIGABRT, (result.returncode, result.stderr)
    print("  Oversized Darwin reply rejected before reading payload OK")


def main():
    binary = sys.argv[1]
    test_invalid_descriptor_environment(binary)
    test_report_drain_after_exit()
    test_closed_peer_on_next_request(binary)
    test_stream_fragmentation(binary)
    test_stream_coalesced_stale_reply(binary)
    test_stream_oversized_reply(binary)
    test_continue(binary)
    test_terminate(binary)
    test_repeated(binary)
    test_wrong_incident(binary)
    test_wrong_kind(binary)
    test_malformed(binary)
    test_helper_exit(binary)
    test_require_never_resumes(binary)
    test_fatal_never_resumes(binary)
    test_formatted_continue(binary)
    test_app_lock_continue(binary)
    test_check_returns_false(binary)
    test_ci_overrides_interactive_request(binary)
    print("Passed resumable-ASSERT decision: continue(+visible text), terminate, repeated, "
          "wrong-id, wrong-kind, malformed, helper-exit, REQUIRE/FATAL-terminal, formatted, "
          "app-lock, CHECK-false, CI-veto")


if __name__ == "__main__":
    main()
