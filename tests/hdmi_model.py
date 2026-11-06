"""Run actual ARM ELF with a synthetic mailbox/framebuffer model.

This checks the driver and task integration; it is NOT a VideoCore, EDID,
HDMI signal or physical monitor simulation. No target code is patched.
"""
import argparse
import json
import struct
from collections import deque
from pathlib import Path
from emulate import Target

MBOX_READ = 0x2000B880
MBOX_STATUS0 = 0x2000B898
MBOX_WRITE = 0x2000B8A0
MBOX_STATUS1 = 0x2000B8B8
FB_ADDRESS = 0x1C100000
FB_WIDTH, FB_HEIGHT = 640, 480


class HDMITarget(Target):
    def __init__(self, elf, ticks, case, initial_us=0, late=False, redraw=False):
        super().__init__(elf, ticks, initial_us, late)
        self.case = case
        self.request_redraw = redraw
        self.redraw_sent = False
        self.expected_uart_commands = 4 if redraw else 3
        self.order = 0 if case == 'bgr' else 1
        self.pitch = FB_WIDTH*4 + (96 if case == 'bgr' else 64)
        self.frame_size = self.pitch * FB_HEIGHT
        self.responses = deque()
        self.property_requests = 0
        self.max_blocks = 6000000
        self.min_low_completed = 1
        self.uc.mem_map(0x1C000000, 0x00400000)
        self.uc.mem_write(0x1C000000, b'\xa5' * 0x00400000)

    def on_read(self, uc, access, address, size, value, user):
        super().on_read(uc, access, address, size, value, user)
        if address == MBOX_STATUS1:
            self.store32(address, 0x80000000 if self.case == 'full' else 0)
        elif address == MBOX_STATUS0:
            self.store32(address, 0 if self.responses else 0x40000000)
        elif address == MBOX_READ:
            self.store32(address, self.responses.popleft() if self.responses else 0)

    def on_write(self, uc, access, address, size, value, user):
        super().on_write(uc, access, address, size, value, user)
        if address == MBOX_WRITE:
            # Network clock/serial requests are a separate 32-byte message.
            # This GUI regression deliberately models their timeout.
            if int.from_bytes(uc.mem_read(value & ~15,4),'little') == 32:
                return
            self.property_requests += 1
            assert value & 15 == 8, 'Expected property channel 8'
            address = value & ~15
            assert address < 0x01000000, 'Property request must use ARM physical address'
            words = list(struct.unpack('<39I', uc.mem_read(address, 39*4)))
            assert words[:2] == [156, 0] and words[38] == 0
            requests = [
                (2, 0x48003, 8, [640,480]),
                (7, 0x48004, 8, [640,480]),
                (12,0x48005, 4, [32]),
                (16,0x48006, 4, [1]),
                (20,0x48009, 8, [0,0]),
                (25,0x48007, 4, [2]),
                (29,0x40001, 8, [4096,0]),
                (34,0x40008, 4, [0]),
            ]
            for offset, tag, capacity, values in requests:
                assert words[offset:offset+3] == [tag, capacity, 0], 'Bad request tag header'
                assert words[offset+3:offset+3+len(values)] == values, 'Bad tag request values'
                words[offset+2] = 0x80000000 | capacity
            if self.case == 'timeout':
                return  # No GPU writes and no reply; target must time out safely.
            words[1] = 0x80000000
            words[19] = self.order
            words[32] = FB_ADDRESS | (0x40000000 if self.case == 'bgr' else 0xC0000000)
            words[33] = self.frame_size
            words[37] = self.pitch
            if self.case == 'bad_geometry':
                words[5] = 800
            elif self.case == 'bad_pitch':
                words[37] = 2556
            elif self.case == 'bad_depth':
                words[15] = 16
            elif self.case == 'bad_tag':
                words[36] = 4  # Missing response-success flag.
            elif self.case == 'overlap':
                words[32] = 0xC0008000  # Must reject overlap with kernel RAM.
            elif self.case == 'short_size':
                words[33] = self.frame_size - 4
            uc.mem_write(address, struct.pack('<39I', *words))
            # Include unrelated/stale tokens: the driver must match channel AND address.
            self.responses.extend([0x1239, value ^ 0x100, value])

    def on_block(self, uc, address, size, user):
        super().on_block(uc, address, size, user)
        if self.request_redraw and self.input_injected and not self.redraw_sent:
            self.rx.extend(b'r')
            self.redraw_sent = True

    def snapshot(self, path):
        """PPM uses only the Python standard library; bytes come from target RAM."""
        assert self.case in ('rgb', 'bgr')
        raw = bytes(self.uc.mem_read(FB_ADDRESS, self.frame_size))
        output = bytearray()
        for y in range(FB_HEIGHT):
            for x in range(FB_WIDTH):
                i = y*self.pitch + x*4
                output.extend(raw[i:i+3] if self.order == 1 else (raw[i+2],raw[i+1],raw[i]))
        path.write_bytes(b'P6\n640 480\n255\n' + output)

