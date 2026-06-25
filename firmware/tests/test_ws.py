"""Tests for the websocket framing + handshake helpers (run under CPython).

    cd firmware && python3 tests/test_ws.py
"""

import asyncio
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import ws  # noqa: E402


class FakeReader:
    """asyncio-style reader over a fixed byte buffer."""

    def __init__(self, data):
        self.data = data
        self.pos = 0

    async def readexactly(self, n):
        end = self.pos + n
        if end > len(self.data):
            raise EOFError("short read")
        out = self.data[self.pos:end]
        self.pos = end
        return out


def test_accept_key_rfc_example():
    # RFC 6455 §1.3 worked example.
    assert ws.accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="


def test_handshake_request():
    req = ws.build_handshake(ws.WS_HOST, ws.WS_PATH, "KEY123",
                             ws.query_string("g1", "u1", "tok")).decode()
    assert req.startswith("GET /v1?gameId=g1&userId=u1&stt=tok HTTP/1.1")
    assert "Upgrade: websocket" in req
    assert "Sec-WebSocket-Key: KEY123" in req
    assert "Sec-WebSocket-Version: 13" in req


def test_frame_roundtrip_small():
    frame = ws.build_client_frame(ws.OP_TEXT, b"hello", mask=bytes([1, 2, 3, 4]))
    # masked bit set, length 5
    assert frame[0] == 0x80 | ws.OP_TEXT
    assert frame[1] == 0x80 | 5
    opcode, payload = asyncio.run(ws.read_frame(FakeReader(frame)))
    assert opcode == ws.OP_TEXT and payload == b"hello"


def test_frame_roundtrip_extended_len():
    payload = b"x" * 200  # forces the 126 extended-length path
    frame = ws.build_client_frame(ws.OP_BIN, payload, mask=bytes([9, 8, 7, 6]))
    assert frame[1] == 0x80 | 126
    opcode, got = asyncio.run(ws.read_frame(FakeReader(frame)))
    assert opcode == ws.OP_BIN and got == payload


def test_unmasked_server_frame():
    # server frames are unmasked: FIN+text, len 3, no mask bit
    frame = bytes([0x80 | ws.OP_TEXT, 3]) + b"abc"
    opcode, payload = asyncio.run(ws.read_frame(FakeReader(frame)))
    assert opcode == ws.OP_TEXT and payload == b"abc"


def test_ping_and_close_opcodes():
    ping = bytes([0x80 | ws.OP_PING, 0])
    assert asyncio.run(ws.read_frame(FakeReader(ping)))[0] == ws.OP_PING
    close = bytes([0x80 | ws.OP_CLOSE, 0])
    assert asyncio.run(ws.read_frame(FakeReader(close)))[0] == ws.OP_CLOSE


def main():
    test_accept_key_rfc_example()
    test_handshake_request()
    test_frame_roundtrip_small()
    test_frame_roundtrip_extended_len()
    test_unmasked_server_frame()
    test_ping_and_close_opcodes()
    print("test_ws: OK")


if __name__ == "__main__":
    main()
