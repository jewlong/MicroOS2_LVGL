"""Execute production ARM ELF with a synthetic DWC2 Full-Speed USB HID Multi-Touch screen.
Verifies HID Report Descriptor transfer, phantom-mouse skip, real LVGL pointer hit-testing
on rendered buttons, multi-touch slot fallback, and safe disconnect cancellation.
"""
import argparse, struct, json
from pathlib import Path
from hdmi_model import HDMITarget, MBOX_WRITE, FB_WIDTH, FB_HEIGHT
from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE

BASE = 0x20980000

# Standard 2-slot Multi-Touch Digitizer HID Report Descriptor (Report ID=1, X/Y 0..4095)
TOUCH_RDESC = bytes([
    0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01,             # Usage Page(Digitizers), Usage(Touch Screen), Coll(App)
    0x85, 0x01,                                     # Report ID (1)
    # Slot 0
    0x09, 0x22, 0xA1, 0x02,                         # Usage(Finger), Coll(Logical)
    0x09, 0x42, 0x15, 0x00, 0x25, 0x01,             # Tip Switch 1-bit
    0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
    0x95, 0x07, 0x81, 0x03,                         # 7-bit padding
    0x09, 0x51, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, # Contact ID 8-bit
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,             # X, Y
    0x15, 0x00, 0x26, 0xFF, 0x0F,                   # Logical 0..4095
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02,             # 16-bit x 2
    0xC0,
    # Slot 1
    0x05, 0x0D, 0x09, 0x22, 0xA1, 0x02,
    0x09, 0x42, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
    0x95, 0x07, 0x81, 0x03,
    0x09, 0x51, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x00, 0x26, 0xFF, 0x0F,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
    0xC0,
    0x05, 0x0D, 0x09, 0x54, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, # Contact Count
    0xC0,
])

def to_raw(px, py):
    rx = round(px * 4095 / (FB_WIDTH - 1))
    ry = round(py * 4095 / (FB_HEIGHT - 1))
    return rx, ry

def mt_pkt(s0_tip, s0_x, s0_y, s1_tip=0, s1_x=0, s1_y=0):
    cnt = (1 if s0_tip else 0) + (1 if s1_tip else 0)
    return struct.pack('<BBBHHBBHHB', 1, s0_tip, 0, s0_x, s0_y, s1_tip, 1, s1_x, s1_y, cnt)

# Fallback screen button centers on 640x480:
# panel at (24,210), buttons at y=154..198 -> screen y=364..408 (center 386)
# minus: x=24+18..24+164 (center 115), plus: x=24+176..24+322 (center 273), reset: x=24+334..24+574 (center 478)
PLUS_RAW = to_raw(273, 386)
MINUS_RAW = to_raw(115, 386)
RESET_RAW = to_raw(478, 386)

# (packet, min_ticks_gap_after)
STEPS = [
    (mt_pkt(1, *PLUS_RAW), 20),           # Tap [+] #1 down -> 50->51 on release
    (mt_pkt(0, *PLUS_RAW), 30),           # Tap [+] #1 up
    (mt_pkt(1, *PLUS_RAW), 20),           # Tap [+] #2 down -> 51->52 on release
    (mt_pkt(0, *PLUS_RAW), 30),           # Tap [+] #2 up
    (mt_pkt(0, 0, 0, 1, *MINUS_RAW), 20), # Tap [-] via Slot 1 down -> 52->51 on release
    (mt_pkt(0, 0, 0, 0, *MINUS_RAW), 30), # Tap [-] via Slot 1 up
    (mt_pkt(1, *RESET_RAW), 25),          # Hold [R] down, then unplug USB -> must NOT reset to 50!
]

class USBTouchTarget(HDMITarget):
    def __init__(self, args):
        super().__init__(args.elf, args.ticks, 'rgb')
        self.max_blocks = 30000000
        self.uc.mem_map(BASE, 0x10000)
        self.uc.hook_add(UC_HOOK_MEM_READ, self.on_read, begin=BASE, end=BASE + 0xffff)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.on_write, begin=BASE, end=BASE + 0xffff)
        self.reg = {0x40: 0x4f54280a, 0x48: (2 << 3) | (7 << 14) | (1 << 6), 0x4c: 4080 << 16, 0x10: 1 << 31, 0x14: 1}
        self.connected = True
        self.enabled = False
        self.port_power = False
        self.port_reset = False
        self.address = 0
        self.configured = False
        self.setup = None
        self.offset = 0
        self.ep0 = 64
        self.mps = 32
        self.device = bytes([18, 1, 0x00, 2, 0, 0, 0, self.ep0, 0xEF, 0x0E, 0x01, 0x00, 0, 1, 0, 0, 0, 1])
        rlen = len(TOUCH_RDESC)
        # Composite config: IF0 = HID Touch (3/0/0, EP2 IN MPS=32), IF1 = secondary Boot Mouse (3/1/2, EP3 IN)
        self.config = bytes([
            9, 2, 59, 0, 2, 1, 0, 0xA0, 50,
            9, 4, 0, 0, 1, 3, 0, 0, 0,
            9, 0x21, 0x11, 1, 0, 1, 0x22, rlen & 0xFF, (rlen >> 8) & 0xFF,
            7, 5, 0x82, 3, self.mps, 0, 4,
            9, 4, 1, 0, 1, 3, 1, 2, 0,
            9, 0x21, 0x11, 1, 0, 1, 0x22, 52, 0,
            7, 5, 0x83, 3, 8, 0, 10,
        ])
        self.step = 0
        self.event_goal = 0
        self.interrupt_pid = 0
        self.ctrl_pid = 2
        self.rdesc_fetched = False
        self.disconnected = False
        self.dmas = 0

    def port(self):
        speed = 1 # Full Speed
        return int(self.connected) | (4 if self.enabled else 0) | (speed << 17) | (4096 if self.port_power else 0) | (256 if self.port_reset else 0)

    def on_read(self, uc, access, address, size, value, user):
        if BASE <= address < BASE + 0x10000:
            r = address - BASE
            self.store32(address, self.port() if r == 0x440 else (self.us // 1000) & 0xffff if r == 0x408 else self.reg.get(r, 0))
            return
        super().on_read(uc, access, address, size, value, user)

    def finish(self, flags, actual=0):
        requested = self.reg[0x510] & 0x7ffff
        self.reg[0x508] = 2 | flags
        self.reg[0x500] &= ~(1 << 31)
        self.reg[0x510] = (self.reg[0x510] & ~0x7ffff) | (requested - actual)

    def dma_write(self, raw):
        address = self.reg[0x514]
        assert address & 0xc0000000 == 0x40000000
        address &= 0x3fffffff
        assert address % 64 == 0 and 0x8000 < address < 0x1000000 and len(raw) <= 64
        self.uc.mem_write(address, raw)
        self.dmas += 1

    def transfer(self, ch):
        ep = (ch >> 11) & 15
        direction = bool(ch & (1 << 15))
        kind = (ch >> 18) & 3
        addr = (ch >> 22) & 127
        size = self.reg[0x510] & 0x7ffff
        pid = self.reg[0x510] >> 29
        assert addr == self.address
        dma = self.reg[0x514] & 0x3fffffff
        if ep == 0:
            if pid == 3:
                self.setup = struct.unpack('<BBHHH', self.uc.mem_read(dma, 8))
                self.offset = 0
                self.ctrl_pid = 2
                bm, req, val, index, length = self.setup
                if req == 6 and bm == 0x80:
                    source = self.device if val == 0x100 else self.config
                    self.response = source[:length]
                elif req == 6 and bm == 0x81 and (val >> 8) == 0x22 and index == 0:
                    self.rdesc_fetched = True
                    self.response = TOUCH_RDESC[:length]
                else:
                    self.response = b''
                self.finish(1, 8)
                return
            bm, req, val, index, length = self.setup
            if length > 0 and self.offset < length:
                if direction:
                    raw = self.response[self.offset:self.offset + (ch & 2047)]
                    self.dma_write(raw)
                    self.offset += len(raw)
                    self.ctrl_pid ^= 2
                    self.finish(1, len(raw))
                    return
            if req == 5: self.address = val
            if req == 9 and bm == 0: self.configured = True; self.interrupt_pid = 0
            if req == 11: assert False, "Must NOT send SET_PROTOCOL(BOOT) to HID touch interface"
            self.finish(1)
            return

        # Interrupt IN on EP2 (touch)
        assert ep == 2 and kind == 3 and direction and self.configured
        assert pid == self.interrupt_pid and size == self.mps
        if not self.value('gui_ready') or self.value('app_display_frames') < 2:
            self.finish(16)
            return
        if self.step >= len(STEPS) or self.value('gui_touch_events') < self.event_goal:
            self.finish(16)
            return
        raw, _gap = STEPS[self.step]
        self.step += 1
        self.event_goal = self.value('gui_touch_events') + 1
        self.dma_write(raw)
        self.interrupt_pid ^= 2
        self.finish(1, len(raw))

    def on_write(self, uc, access, address, size, value, user):
        if address == MBOX_WRITE:
            a = value & ~15
            w = list(struct.unpack('<8I', uc.mem_read(a, 32)))
            if w[0] == 32 and w[2] == 0x28001:
                w[1] = 0x80000000; w[4] = 0x80000008; w[6] = 1
                uc.mem_write(a, struct.pack('<8I', *w))
                self.responses.append(value)
                return
        if BASE <= address < BASE + 0x10000:
            r = address - BASE
            if r == 0x10: self.reg[r] = 1 << 31
            elif r == 0x14: self.reg[r] = 1
            elif r == 0x508: self.reg[r] = self.reg.get(r, 0) & ~value
            elif r == 0x440:
                self.port_power = bool(value & 4096)
                if self.port_reset and not value & 256:
                    self.enabled = self.connected; self.address = 0; self.configured = False
                self.port_reset = bool(value & 256)
            elif r == 0x500:
                self.reg[r] = value
                if value & (1 << 30): self.reg[r] &= ~(1 << 31); self.reg[0x508] = 2
                elif value & (1 << 31): self.transfer(value)
            else:
                self.reg[r] = value
            return
        super().on_write(uc, access, address, size, value, user)

    def on_block(self, uc, address, size, user):
        super().on_block(uc, address, size, user)
        if self.blocks % 64 or self.value('OSRunning', 1) != 1: return
        if self.step == len(STEPS) and not self.disconnected and self.value('gui_touch_down') == 1 and self.value('gui_touch_events') >= self.event_goal:
            # Unplug touch screen while [R] button is held down!
            self.connected = False
            self.enabled = False
            self.disconnected = True

    def check(self):
        assert self.rdesc_fetched, "HID Report Descriptor was not fetched"
        assert self.step == len(STEPS), (self.step, len(STEPS))
        assert self.disconnected
        assert self.value('usb_mouse_connected') == 0, "Secondary mouse on touch device must be skipped"
        assert self.value('usb_touch_connected') == 0, "Touch must clear connected flag on unplug"
        assert self.value('usb_touch_reports') == len(STEPS), self.value('usb_touch_reports')
        assert self.value('gui_touch_resets') >= 1, self.value('gui_touch_resets')
        assert self.value('gui_touch_down') == 0, self.value('gui_touch_down')
        assert self.value('gui_button_events') == 3, self.value('gui_button_events')
        assert self.value('gui_value') == 51, f"Expected 50+1+1-1=51 (unplug on Reset must cancel), got {self.value('gui_value')}"
        assert self.value('usb_stack') == 0 and self.value('gui_stack') == 0

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--elf', type=Path, default=Path('build/kernel.elf'))
    p.add_argument('--ticks', type=int, default=7500)
    a = p.parse_args()
    t = USBTouchTarget(a)
    try:
        text, out = t.run()
        t.check()
    except Exception:
        print(''.join(t.output)[:2500])
        print('step', t.step, 'touch_conn', t.value('usb_touch_connected'),
              'touch_reports', t.value('usb_touch_reports'),
              'touch_ev', t.value('gui_touch_events'), 'touch_rst', t.value('gui_touch_resets'),
              'gui_value', t.value('gui_value'), 'btn_ev', t.value('gui_button_events'))
        raise
    out.update(dma_writes=t.dmas, touch_steps=t.step,
        **{n: t.value(n) for n in ['usb_state', 'usb_connects', 'usb_disconnects',
                                   'usb_touch_connected', 'usb_touch_reports',
                                   'usb_mouse_connected', 'gui_touch_events', 'gui_touch_resets',
                                   'gui_touch_down', 'gui_button_events', 'gui_value']})
    print(text + '\nPASS: production ARM USB HID Touch + LVGL pointer hit-test + safe unplug cancel.\n' + json.dumps(out, indent=2))

if __name__ == '__main__':
    main()
