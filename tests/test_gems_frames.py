"""Frame fixtures captured from a real NAS GEMS engine ECU (P38 4.6).

Source: howeypeter/landrover-gems_t4, memory/real-gems-protocol.md, which
records these as bench captures (2026-08/09). They pin the framing rules the
firmware's kline_checksum() must follow: a modulo-256 sum of every byte before
the checksum.

Run from the repo root:
    python -m unittest tests.test_gems_frames -v
"""
import unittest


def checksum(frame: bytes) -> int:
    """Modulo-256 sum of all bytes. Mirrors kline_checksum() in main/kline.c."""
    return sum(frame) & 0xFF


def frame(hex_text: str) -> bytes:
    return bytes.fromhex(hex_text)


# Tester -> ECU: header 68 6A F1 (target 6A, source F1), Mode 01, PID, checksum.
REQ_PID00 = frame("68 6A F1 01 00 C4")
REQ_PID05 = frame("68 6A F1 01 05 C9")

# ECU -> tester: header 48 6B E8 (target 6B, source E8), Mode 41 (0x01 + 0x40).
RESP_PID00 = frame("48 6B E8 41 00 BF 9F F9 91 C4")   # supported-PID bitmask
RESP_PID05 = frame("48 6B E8 41 05 00 E1")            # coolant temperature


def coolant_celsius(raw: int) -> int:
    """PID 05 scaling: A - 40, in degrees C."""
    return raw - 40


class ChecksumTests(unittest.TestCase):
    def test_request_frames_checksum(self):
        for req in (REQ_PID00, REQ_PID05):
            with self.subTest(req=req.hex(" ")):
                self.assertEqual(checksum(req[:-1]), req[-1])

    def test_response_frames_checksum(self):
        for resp in (RESP_PID00, RESP_PID05):
            with self.subTest(resp=resp.hex(" ")):
                self.assertEqual(checksum(resp[:-1]), resp[-1])

    def test_checksum_is_modulo_256(self):
        # 0xFF + 0x02 = 0x101 -> 0x01
        self.assertEqual(checksum(bytes([0xFF, 0x02])), 0x01)


class FrameLayoutTests(unittest.TestCase):
    def test_request_header_and_mode(self):
        self.assertEqual(REQ_PID00[:3], bytes([0x68, 0x6A, 0xF1]))
        self.assertEqual(REQ_PID00[3], 0x01)  # Mode 01, current data

    def test_response_header_and_service(self):
        self.assertEqual(RESP_PID00[:3], bytes([0x48, 0x6B, 0xE8]))
        self.assertEqual(RESP_PID00[3], 0x41)  # 0x01 + 0x40 positive response

    def test_response_echoes_requested_pid(self):
        self.assertEqual(RESP_PID00[4], REQ_PID00[4])
        self.assertEqual(RESP_PID05[4], REQ_PID05[4])


class DecodeTests(unittest.TestCase):
    def test_coolant_zero_raw_is_minus_40(self):
        # RESP_PID05 data byte is 0x00 -> -40 C, as recorded on the bench.
        self.assertEqual(coolant_celsius(RESP_PID05[5]), -40)

    def test_pid00_bitmask_length(self):
        # PID 00 returns four data bytes: 0xBF 0x9F 0xF9 0x91.
        self.assertEqual(len(RESP_PID00[5:-1]), 4)


if __name__ == "__main__":
    unittest.main()
