# Research v2: fclash UDP ASSOCIATE session routing behavior
# Comprehensive test matrix with statistical repetition
#
# Usage:
#   python udp_session_research_v2.py              # run all tests
#   python udp_session_research_v2.py --test 1     # run specific test
#   python udp_session_research_v2.py --rounds 5   # custom round count
#   python udp_session_research_v2.py --delays 0,100,500,2000  # custom delay ms

import socket
import struct
import sys
import time
import uuid
import argparse
import json
from dataclasses import dataclass, field
from typing import Optional

SOCKS_HOST = "127.0.0.1"
SOCKS_PORT = 7890
BIZ_TARGET_1 = ("10.15.118.159", 9999)   # echo server 1
#BIZ_TARGET_2 = ("10.15.118.33", 19999)   # echo server 2 , previous to test send by port 53
BIZ_TARGET_2 = ("10.15.118.33", 19999)   # echo server 2
DNS_TARGET   = ("8.8.8.8", 53)
RECV_TIMEOUT = 2  # seconds (shorter for faster FAIL detection)

# ============================================================
# Helpers
# ============================================================

def build_dns_probe():
    """Minimal DNS query for probe.invalid"""
    return bytes.fromhex(
        "CAFE01000001000000000000"
        "0570726F626507696E76616C696400"
        "00010001"
    )

def make_frame(dst_ip, dst_port, payload):
    return (b"\x00\x00\x00\x01"
            + socket.inet_aton(dst_ip)
            + struct.pack("!H", dst_port)
            + payload)

class Session:
    """Manages a SOCKS5 UDP ASSOCIATE session"""
    def __init__(self, bind_local=True):
        self.tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.tcp.settimeout(5)
        self.tcp.connect((SOCKS_HOST, SOCKS_PORT))
        self.tcp.sendall(b"\x05\x01\x00")
        assert self.tcp.recv(2) == b"\x05\x00"

        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.settimeout(RECV_TIMEOUT)
        if bind_local:
            self.udp.bind(("127.0.0.1", 0))

        # ASSOCIATE with wildcard
        self.tcp.sendall(b"\x05\x03\x00\x01\x00\x00\x00\x00\x00\x00")
        hdr = self.tcp.recv(4)
        assert hdr[1] == 0, f"ASSOCIATE failed: REP={hdr[1]:#x}"
        if hdr[3] == 1:
            data = self.tcp.recv(6)
            self.bnd_ip = socket.inet_ntoa(data[:4])
            self.bnd_port = struct.unpack("!H", data[4:6])[0]
        elif hdr[3] == 4:
            data = self.tcp.recv(18)
            self.bnd_ip = SOCKS_HOST
            self.bnd_port = struct.unpack("!H", data[16:18])[0]
        else:
            raise RuntimeError(f"Unsupported ATYP={hdr[3]}")
        if self.bnd_ip in ("0.0.0.0", "::"):
            self.bnd_ip = SOCKS_HOST

        try:
            self.local_port = self.udp.getsockname()[1]
        except OSError:
            self.local_port = 0  # unbound, OS assigns on first send

    def send(self, dst_ip, dst_port, payload):
        frame = make_frame(dst_ip, dst_port, payload)
        self.udp.sendto(frame, (self.bnd_ip, self.bnd_port))

    def recv(self):
        """Returns (payload_after_header, raw_data) or (None, None) on timeout"""
        try:
            data, addr = self.udp.recvfrom(65535)
            # Strip SOCKS5 UDP header (10 bytes for IPv4 ATYP=1)
            if len(data) > 10 and data[3] == 0x01:
                return data[10:], data
            return data, data
        except socket.timeout:
            return None, None

    def send_recv(self, dst_ip, dst_port, payload):
        """Send and try to receive. Returns (success, payload_or_None)"""
        self.send(dst_ip, dst_port, payload)
        resp, raw = self.recv()
        return resp is not None, resp

    def drain(self):
        """Read and discard any pending data"""
        old_timeout = self.udp.gettimeout()
        self.udp.settimeout(0.1)
        try:
            while True:
                self.udp.recvfrom(65535)
        except socket.timeout:
            pass
        self.udp.settimeout(old_timeout)

    def close(self):
        self.udp.close()
        self.tcp.close()


@dataclass
class TestResult:
    name: str
    params: dict
    rounds: int = 0
    passes: int = 0
    fails: int = 0
    payload_errors: int = 0  # received but wrong content
    details: list = field(default_factory=list)

    @property
    def rate(self):
        return f"{self.passes}/{self.rounds}" if self.rounds > 0 else "N/A"

    @property
    def status(self):
        if self.rounds == 0: return "SKIP"
        if self.passes == self.rounds: return "PASS"
        if self.fails == self.rounds: return "FAIL"
        if self.fails == 0 and self.payload_errors > 0: return "PAYLOAD_ERR"
        return "FLAKY"


def validate_echo(sent_payload, received_payload):
    """Check echo server response: should be b'echo:' + original"""
    if received_payload is None:
        return False, "timeout"
    expected = b"echo:" + sent_payload
    if received_payload == expected:
        return True, "ok"
    return False, f"mismatch: sent={sent_payload!r} got={received_payload!r}"


# ============================================================
# Test Definitions
# ============================================================

def test_port_vs_payload(rounds, delays):
    """
    Test 1: Port 53 vs DNS payload matrix
    A: port 53 + DNS payload → 8.8.8.8:53  (standard DNS)
    B: port 53 + non-DNS payload → 8.8.8.8:53  (garbage to DNS resolver)
    C: non-53 + DNS payload → echo server  (DNS bytes to echo)
    D: non-53 + non-DNS payload → echo server  (normal, control)

    Each: send warmup variant, then business to BIZ_TARGET_1
    """
    results = []
    dns_payload = build_dns_probe()
    garbage_payload = b"NOT-DNS-" + uuid.uuid4().hex[:8].encode()

    variants = [
        ("A: port53+DNS",    DNS_TARGET[0],      DNS_TARGET[1],      dns_payload),
        ("B: port53+garbage", DNS_TARGET[0],      DNS_TARGET[1],      garbage_payload),
        ("C: echo+DNS",      BIZ_TARGET_2[0],    BIZ_TARGET_2[1],    dns_payload),
        ("D: echo+garbage",  BIZ_TARGET_2[0],    BIZ_TARGET_2[1],    garbage_payload),
    ]

    for delay_ms in delays:
        for vname, wip, wport, wpayload in variants:
            r = TestResult(
                name=f"1-{vname}",
                params={"delay_ms": delay_ms, "warmup_dst": f"{wip}:{wport}",
                        "warmup_is_dns": wpayload == dns_payload})

            for i in range(rounds):
                s = Session()
                # Warmup
                s.send(wip, wport, wpayload)
                # Try to receive warmup response (don't care about result)
                s.recv()

                if delay_ms > 0:
                    time.sleep(delay_ms / 1000.0)

                # Business
                biz_msg = f"t1-{vname[:1]}-{i}-{uuid.uuid4().hex[:4]}".encode()
                ok, resp = s.send_recv(BIZ_TARGET_1[0], BIZ_TARGET_1[1], biz_msg)
                r.rounds += 1
                if ok:
                    valid, detail = validate_echo(biz_msg, resp)
                    if valid:
                        r.passes += 1
                    else:
                        r.payload_errors += 1
                        r.details.append(f"round {i}: {detail}")
                else:
                    r.fails += 1

                s.close()

            results.append(r)
            print(f"  {r.name} delay={delay_ms}ms: {r.status} ({r.rate})"
                  + (f" payload_err={r.payload_errors}" if r.payload_errors else ""))

    return results


def test_recv_timing(rounds, delays):
    """
    Test 2: Does recv matter?
    A: send DNS, NO recv, immediately send business
    B: send DNS, recv (wait for response), then send business
    C: send DNS, drain buffer, then send business
    """
    results = []

    for delay_ms in delays:
        for mode in ["no_recv", "recv_wait", "drain"]:
            r = TestResult(
                name=f"2-{mode}",
                params={"delay_ms": delay_ms, "mode": mode})

            for i in range(rounds):
                s = Session()

                # Send DNS warmup
                s.send(DNS_TARGET[0], DNS_TARGET[1], build_dns_probe())

                if mode == "no_recv":
                    # Don't read anything, go straight to business
                    if delay_ms > 0:
                        time.sleep(delay_ms / 1000.0)
                elif mode == "recv_wait":
                    # Explicitly receive DNS response
                    s.recv()
                    if delay_ms > 0:
                        time.sleep(delay_ms / 1000.0)
                elif mode == "drain":
                    # Wait a bit for response to arrive, then drain
                    time.sleep(0.3)
                    s.drain()
                    if delay_ms > 0:
                        time.sleep(delay_ms / 1000.0)

                # Business
                biz_msg = f"t2-{mode[:1]}-{i}-{uuid.uuid4().hex[:4]}".encode()
                ok, resp = s.send_recv(BIZ_TARGET_1[0], BIZ_TARGET_1[1], biz_msg)
                r.rounds += 1
                if ok:
                    valid, _ = validate_echo(biz_msg, resp)
                    r.passes += 1 if valid else 0
                    if not valid: r.payload_errors += 1
                else:
                    r.fails += 1

                s.close()

            results.append(r)
            print(f"  {r.name} delay={delay_ms}ms: {r.status} ({r.rate})")

    return results


def test_order(rounds, delays):
    """
    Test 3: Business first → DNS → Business
    Confirms that successful first packet prevents lockout
    """
    results = []

    for delay_ms in delays:
        r = TestResult(name="3-biz-dns-biz", params={"delay_ms": delay_ms})

        for i in range(rounds):
            s = Session()

            # First business
            msg1 = f"t3-a-{i}-{uuid.uuid4().hex[:4]}".encode()
            ok1, resp1 = s.send_recv(BIZ_TARGET_1[0], BIZ_TARGET_1[1], msg1)

            if delay_ms > 0:
                time.sleep(delay_ms / 1000.0)

            # DNS in the middle
            s.send(DNS_TARGET[0], DNS_TARGET[1], build_dns_probe())
            s.recv()

            if delay_ms > 0:
                time.sleep(delay_ms / 1000.0)

            # Second business
            msg2 = f"t3-b-{i}-{uuid.uuid4().hex[:4]}".encode()
            ok2, resp2 = s.send_recv(BIZ_TARGET_1[0], BIZ_TARGET_1[1], msg2)

            r.rounds += 1
            if ok1 and ok2:
                v1, _ = validate_echo(msg1, resp1)
                v2, _ = validate_echo(msg2, resp2)
                if v1 and v2:
                    r.passes += 1
                else:
                    r.payload_errors += 1
            else:
                r.fails += 1
                r.details.append(f"round {i}: biz1={ok1} biz2={ok2}")

            s.close()

        results.append(r)
        print(f"  {r.name} delay={delay_ms}ms: {r.status} ({r.rate})")

    return results


def test_multi_target(rounds, delays):
    """
    Test 4: Multiple different targets on same session
    Send to target1, then target2, then target1 again
    """
    results = []

    for delay_ms in delays:
        r = TestResult(name="4-multi-target", params={"delay_ms": delay_ms})

        for i in range(rounds):
            s = Session()

            msg1 = f"t4-a-{i}-{uuid.uuid4().hex[:4]}".encode()
            ok1, resp1 = s.send_recv(BIZ_TARGET_1[0], BIZ_TARGET_1[1], msg1)

            if delay_ms > 0:
                time.sleep(delay_ms / 1000.0)

            msg2 = f"t4-b-{i}-{uuid.uuid4().hex[:4]}".encode()
            ok2, resp2 = s.send_recv(BIZ_TARGET_2[0], BIZ_TARGET_2[1], msg2)

            if delay_ms > 0:
                time.sleep(delay_ms / 1000.0)

            msg3 = f"t4-c-{i}-{uuid.uuid4().hex[:4]}".encode()
            ok3, resp3 = s.send_recv(BIZ_TARGET_1[0], BIZ_TARGET_1[1], msg3)

            r.rounds += 1
            all_ok = ok1 and ok2 and ok3
            if all_ok:
                v1, _ = validate_echo(msg1, resp1)
                v2, _ = validate_echo(msg2, resp2)
                v3, _ = validate_echo(msg3, resp3)
                if v1 and v2 and v3:
                    r.passes += 1
                else:
                    r.payload_errors += 1
            else:
                r.fails += 1
                r.details.append(f"round {i}: t1={ok1} t2={ok2} t3={ok3}")

            s.close()

        results.append(r)
        print(f"  {r.name} delay={delay_ms}ms: {r.status} ({r.rate})")

    return results


def test_bind_vs_nobind(rounds, delays):
    """
    Test 5: Bound (127.0.0.1:random) vs unbound local address
    Subtests: with and without DNS warmup
    """
    results = []

    for delay_ms in delays:
        for bind_local in [True, False]:
            for warmup in [False, True]:
                label = f"5-{'bind' if bind_local else 'nobind'}-{'dns' if warmup else 'nodns'}"
                r = TestResult(name=label, params={
                    "delay_ms": delay_ms, "bind": bind_local, "warmup": warmup})

                for i in range(rounds):
                    s = Session(bind_local=bind_local)

                    if warmup:
                        s.send(DNS_TARGET[0], DNS_TARGET[1], build_dns_probe())
                        s.recv()
                        if delay_ms > 0:
                            time.sleep(delay_ms / 1000.0)

                    msg = f"t5-{i}-{uuid.uuid4().hex[:4]}".encode()
                    ok, resp = s.send_recv(BIZ_TARGET_1[0], BIZ_TARGET_1[1], msg)
                    r.rounds += 1
                    if ok:
                        valid, _ = validate_echo(msg, resp)
                        r.passes += 1 if valid else 0
                        if not valid: r.payload_errors += 1
                    else:
                        r.fails += 1

                    s.close()

                results.append(r)
                print(f"  {r.name} delay={delay_ms}ms: {r.status} ({r.rate})")

    return results


# ============================================================
# Main
# ============================================================

def main():
    parser = argparse.ArgumentParser(description="fclash UDP session research v2")
    parser.add_argument("--test", type=int, default=0, help="Run specific test (1-5), 0=all")
    parser.add_argument("--rounds", type=int, default=5, help="Rounds per config")
    parser.add_argument("--delays", type=str, default="0,500",
                        help="Comma-separated delay values in ms")
    args = parser.parse_args()

    delays = [int(d) for d in args.delays.split(",")]
    rounds = args.rounds

    print(f"=== fclash UDP Session Research v2 ===")
    print(f"  SOCKS5:    {SOCKS_HOST}:{SOCKS_PORT}")
    print(f"  Target 1:  {BIZ_TARGET_1[0]}:{BIZ_TARGET_1[1]}")
    print(f"  Target 2:  {BIZ_TARGET_2[0]}:{BIZ_TARGET_2[1]}")
    print(f"  Rounds:    {rounds}")
    print(f"  Delays:    {delays} ms")
    print(f"  Timeout:   {RECV_TIMEOUT}s")
    print()

    all_results = []

    tests = {
        1: ("Port vs Payload matrix", test_port_vs_payload),
        2: ("Recv timing (send-only vs recv)", test_recv_timing),
        3: ("Business-first ordering", test_order),
        4: ("Multi-target same session", test_multi_target),
        5: ("Bind vs no-bind + warmup", test_bind_vs_nobind),
    }

    for num in sorted(tests.keys()):
        if args.test != 0 and args.test != num:
            continue
        name, fn = tests[num]
        print(f"{'='*60}")
        print(f"Test {num}: {name}")
        print()
        results = fn(rounds, delays)
        all_results.extend(results)
        print()

    # Summary
    print("=" * 60)
    print("SUMMARY")
    print(f"{'Test':<35} {'Delay':>6} {'Result':>6} {'Rate':>7} {'PayErr':>6}")
    print("-" * 65)
    for r in all_results:
        delay = r.params.get("delay_ms", "?")
        pe = r.payload_errors if r.payload_errors else ""
        print(f"{r.name:<35} {delay:>5}ms {r.status:>6} {r.rate:>7} {pe:>6}")

    # Details for failures
    has_details = [r for r in all_results if r.details]
    if has_details:
        print()
        print("DETAILS:")
        for r in has_details:
            print(f"  {r.name}:")
            for d in r.details[:5]:
                print(f"    {d}")


if __name__ == "__main__":
    main()
