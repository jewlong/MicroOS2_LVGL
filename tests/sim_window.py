"""Interactive Windows GUI Simulator for Raspberry Pi Zero W uC/OS-II + LVGL + ViewGP + USB Keyboard.

Executes the actual compiled ARM1176 build/kernel.elf using Unicorn Engine,
maps sdcard/project.json + sdcard/wifi.txt via ATAG_INITRD2 at 0x02000000,
renders the live HDMI framebuffer in a Tkinter window, and forwards PC keyboard
events as USB HID Boot Keyboard reports into the emulated DWC2 USB controller.
"""
import argparse
from collections import deque
from pathlib import Path
import struct
import sys
import tkinter as tk

from unicorn import UC_HOOK_BLOCK, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import (
    UC_ARM_REG_PC, UC_ARM_REG_CPSR, UC_ARM_REG_SPSR, UC_ARM_REG_LR
)
from hdmi_model import HDMITarget, FB_ADDRESS, MBOX_WRITE
from emulate import TIMER_CLO

USB_BASE = 0x20980000
INITRD_ADDR = 0x02000000
INITRD_MAX = 0x00100000

# Map Tkinter keysym to (modifier_mask, hid_usage_code)
KEYSYM_TO_HID = {
    'Return': (0, 40),
    'KP_Enter': (0, 88),
    'Escape': (0, 41),
    'BackSpace': (0, 42),
    'Tab': (0, 43),
    'space': (0, 44),
    'minus': (0, 45),
    'KP_Subtract': (0, 86),
    'equal': (0, 46),
    'plus': (2, 46),
    'KP_Add': (0, 87),
    'Right': (0, 79),
    'Left': (0, 80),
    'Down': (0, 81),
    'Up': (0, 82),
    'Home': (0, 74),
    'End': (0, 77),
    'Delete': (0, 76),
    'period': (0, 55),
    'KP_Decimal': (0, 99),
}
for i, ch in enumerate('abcdefghijklmnopqrstuvwxyz'):
    KEYSYM_TO_HID[ch] = (0, 4 + i)
    KEYSYM_TO_HID[ch.upper()] = (2, 4 + i)
for i, ch in enumerate('1234567890'):
    KEYSYM_TO_HID[ch] = (0, 30 + i)
    KEYSYM_TO_HID[f'KP_{ch}'] = (0, 30 + i)


class InteractiveSimTarget(HDMITarget):
    def __init__(self, elf_path, project_root, width=800, height=480):
        self.block_hook = None
        self.fast_jit = False
        super().__init__(elf_path, ticks=99999999, case='rgb')
        self.sim_w = width
        self.sim_h = height
        self.pitch = self.sim_w * 4
        self.frame_size = self.pitch * self.sim_h
        self.max_blocks = 999999999
        self.low_start = self.symbols.get('low_task', 0)
        self.idle_start = self.symbols.get('OS_TaskIdle', 0)

        # Map DWC2 USB controller MMIO
        self.uc.mem_map(USB_BASE, 0x10000)
        self.uc.hook_add(UC_HOOK_MEM_READ, self.on_usb_read, begin=USB_BASE, end=USB_BASE + 0xFFFF)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.on_usb_write, begin=USB_BASE, end=USB_BASE + 0xFFFF)
        self.usb_reg = {
            0x40: 0x4F54280A,
            0x48: (2 << 3) | (7 << 14) | (1 << 6),
            0x4C: 4080 << 16,
            0x10: 1 << 31,
            0x14: 1,
        }
        self.usb_connected = True
        self.usb_enabled = False
        self.usb_port_power = False
        self.usb_port_reset = False
        self.usb_address = 0
        self.usb_configured = False
        self.usb_boot = False
        self.usb_setup = None
        self.usb_offset = 0
        self.usb_ctrl_pid = 2
        self.usb_interrupt_pid = 0
        self.usb_device_desc = bytes([
            18, 1, 0x10, 1, 0, 0, 0, 8,
            0x6D, 0x04, 0x1C, 0xC3, 0, 1, 0, 0, 0, 1
        ])
        self.usb_config_desc = bytes([
            9, 2, 34, 0, 1, 1, 0, 0x80, 50,
            9, 4, 0, 0, 1, 3, 1, 1, 0,
            9, 0x21, 0x11, 1, 0, 1, 0x22, 63, 0,
            7, 5, 0x81, 3, 8, 0, 10
        ])
        self.hid_reports = deque()

        # Load sdcard/project.json + sdcard/wifi.txt into INITRD_ADDR (0x02000000)
        self.uc.mem_map(INITRD_ADDR, INITRD_MAX)
        blobs = []
        proj_file = project_root / 'sdcard' / 'project.json'
        wifi_file = project_root / 'sdcard' / 'wifi.txt'
        if proj_file.is_file():
            blobs.append(proj_file.read_bytes())
        if wifi_file.is_file():
            blobs.append(b'\n' + wifi_file.read_bytes() + b'\n')
        initrd_bytes = b''.join(blobs)
        if initrd_bytes:
            self.uc.mem_write(INITRD_ADDR, initrd_bytes)
            # Write ATAG_CORE + ATAG_INITRD2 + ATAG_NONE at 0x100
            atags = struct.pack(
                '<11I',
                5, 0x54410001, 0, 0, 0,
                4, 0x54420005, INITRD_ADDR, len(initrd_bytes),
                0, 0
            )
            self.uc.mem_write(0x100, atags)

    def push_key_press_release(self, mod, usage):
        self.hid_reports.append(bytes([mod, 0, usage, 0, 0, 0, 0, 0]))
        self.hid_reports.append(bytes([0, 0, 0, 0, 0, 0, 0, 0]))

    def usb_port_reg(self):
        speed = 2  # Low-speed boot keyboard
        return (
            int(self.usb_connected)
            | (4 if self.usb_enabled else 0)
            | (speed << 17)
            | (4096 if self.usb_port_power else 0)
            | (256 if self.usb_port_reset else 0)
        )

    def on_usb_read(self, uc, access, address, size, value, user):
        r = address - USB_BASE
        val = (
            self.usb_port_reg() if r == 0x440
            else (self.us // 1000) & 0xFFFF if r == 0x408
            else self.usb_reg.get(r, 0)
        )
        self.store32(address, val)

    def usb_finish(self, flags, actual=0):
        requested = self.usb_reg.get(0x510, 0) & 0x7FFFF
        self.usb_reg[0x508] = 2 | flags
        self.usb_reg[0x500] = self.usb_reg.get(0x500, 0) & ~(1 << 31)
        self.usb_reg[0x510] = (self.usb_reg.get(0x510, 0) & ~0x7FFFF) | max(0, requested - actual)

    def usb_dma_write(self, raw):
        addr = self.usb_reg[0x514] & 0x3FFFFFFF
        self.uc.mem_write(addr, raw)

    def usb_transfer(self, ch):
        ep = (ch >> 11) & 15
        direction = bool(ch & (1 << 15))
        size = self.usb_reg[0x510] & 0x7FFFF
        pid = self.usb_reg[0x510] >> 29
        dma = self.usb_reg[0x514] & 0x3FFFFFFF
        if ep == 0:
            if pid == 3:
                self.usb_setup = struct.unpack('<BBHHH', self.uc.mem_read(dma, 8))
                self.usb_offset = 0
                self.usb_ctrl_pid = 2
                bm, req, val, index, length = self.usb_setup
                if req == 6:
                    src = self.usb_device_desc if val == 0x100 else self.usb_config_desc
                    self.usb_response = src[:length]
                self.usb_finish(1, 8)
                return
            bm, req, val, index, length = self.usb_setup
            if length > 0 and self.usb_offset < length:
                if direction:
                    raw = self.usb_response[self.usb_offset:self.usb_offset + (ch & 2047)]
                    self.usb_dma_write(raw)
                    self.usb_offset += len(raw)
                    self.usb_ctrl_pid ^= 2
                    self.usb_finish(1, len(raw))
                    return
                self.usb_offset += size
                self.usb_ctrl_pid ^= 2
                self.usb_finish(1, size)
                return
            if req == 5:
                self.usb_address = val
            elif req == 9 and bm == 0:
                self.usb_configured = True
                self.usb_interrupt_pid = 0
            elif req == 11:
                self.usb_boot = True
            self.usb_finish(1, 0)
            return
        # Endpoint 1 Interrupt IN
        if self.hid_reports:
            raw = self.hid_reports.popleft()
            self.usb_dma_write(raw)
            self.usb_interrupt_pid ^= 2
            self.usb_finish(1, len(raw))
        else:
            self.usb_finish(16, 0)  # NAK

    def on_usb_write(self, uc, access, address, size, value, user):
        r = address - USB_BASE
        if r == 0x10:
            self.usb_reg[r] = 1 << 31
        elif r == 0x14:
            self.usb_reg[r] = 1
        elif r == 0x508:
            self.usb_reg[r] = self.usb_reg.get(r, 0) & ~value
        elif r == 0x440:
            self.usb_port_power = bool(value & 4096)
            if self.usb_port_reset and not (value & 256):
                self.usb_enabled = self.usb_connected
                self.usb_address = 0
                self.usb_configured = False
                self.usb_boot = False
            self.usb_port_reset = bool(value & 256)
        elif r == 0x500:
            self.usb_reg[r] = value
            if value & (1 << 30):
                self.usb_reg[r] &= ~(1 << 31)
                self.usb_reg[0x508] = 2
            elif value & (1 << 31):
                self.usb_transfer(value)
        else:
            self.usb_reg[r] = value

    def on_write(self, uc, access, address, size, value, user):
        if address == MBOX_WRITE:
            qaddr = value & ~15
            byte_len = int.from_bytes(uc.mem_read(qaddr, 4), 'little')
            if byte_len == 32:
                w = list(struct.unpack('<8I', uc.mem_read(qaddr, 32)))
                if w[2] == 0x28001:  # USB power on
                    w[1] = 0x80000000
                    w[4] = 0x80000008
                    w[6] = 1
                    uc.mem_write(qaddr, struct.pack('<8I', *w))
                    self.responses.append(value)
                    return
                return
            if byte_len == 52:
                q = list(struct.unpack('<13I', uc.mem_read(qaddr, 52)))
                q[1] = 0x80000000
                q[4], q[9] = 0x80000008, 0x80000008
                q[5], q[10] = self.sim_w, self.sim_w
                q[6], q[11] = self.sim_h, self.sim_h
                uc.mem_write(qaddr, struct.pack('<13I', *q))
                self.responses.append(value)
                return
            if byte_len == 156:
                words = list(struct.unpack('<39I', uc.mem_read(qaddr, 156)))
                for offset, capacity in [(2, 8), (7, 8), (12, 4), (16, 4), (20, 8), (25, 4), (29, 8), (34, 4)]:
                    words[offset + 2] = 0x80000000 | capacity
                words[1] = 0x80000000
                words[19] = 1  # RGB order
                words[32] = FB_ADDRESS | 0xC0000000
                words[33] = self.frame_size
                words[37] = self.pitch
                uc.mem_write(qaddr, struct.pack('<39I', *words))
                self.responses.append(value)
                return
        super().on_write(uc, access, address, size, value, user)

    def on_read(self, uc, access, address, size, value, user):
        if self.fast_jit and address == TIMER_CLO:
            self.us += 200
        super().on_read(uc, access, address, size, value, user)

    def on_block(self, uc, address, size, user):
        super().on_block(uc, address, size, user)
        # Once the 50ms boot preemption self-test has passed, switch to native C JIT mode
        # (removing the per-basic-block Python callback for a >500x speedup!).
        if (
            not self.fast_jit
            and self.value('app_preempt_pass') == 1
            and self.value('app_low_completed') >= 1
            and (uc.reg_read(UC_ARM_REG_CPSR) & 31) == 31
        ):
            self.fast_jit = True
            self.pending_checks.clear()
            if self.block_hook is not None:
                self.uc.hook_del(self.block_hook)
                self.block_hook = None
            uc.emu_stop()

    def fast_inject_exception(self, kind):
        uc = self.uc
        cpsr = uc.reg_read(UC_ARM_REG_CPSR)
        pc = uc.reg_read(UC_ARM_REG_PC)
        mode, vector = (0x12, 0x18) if kind == 'irq' else (0x13, 0x08)
        uc.reg_write(UC_ARM_REG_CPSR, (cpsr & ~0x3F) | mode | 0x80)
        uc.reg_write(UC_ARM_REG_SPSR, cpsr)
        uc.reg_write(UC_ARM_REG_LR, pc + (4 if kind == 'irq' else 0))
        uc.reg_write(UC_ARM_REG_PC, vector)
        if kind == 'irq':
            self.irq_entries += 1
        else:
            self.svc_entries += 1

    def step_quantum(self, max_slices=60):
        for _ in range(max_slices):
            self.pending = None
            if not self.fast_jit:
                self.uc.emu_start(self.uc.reg_read(UC_ARM_REG_PC), 0x01000000)
                if self.failure:
                    raise RuntimeError(self.failure)
                if self.pending and not self.fast_jit:
                    self.inject_exception(self.pending)
                continue

            # Fast Native C JIT execution (no per-block Python overhead)
            self.uc.emu_start(self.uc.reg_read(UC_ARM_REG_PC), 0x01000000, count=25000)
            if self.failure:
                raise RuntimeError(self.failure)
            if self.pending == 'svc':
                self.fast_inject_exception('svc')
            else:
                pc = self.uc.reg_read(UC_ARM_REG_PC)
                if (
                    self.compare is not None
                    and self.us < self.compare
                    and (
                        (self.low_start <= pc < self.low_start + 200)
                        or (self.idle_start <= pc < self.idle_start + 120)
                    )
                ):
                    self.us = self.compare
                else:
                    self.us += 400

            self.timer_update()
            if self.cs & self.enable1 & 2:
                cpsr = self.uc.reg_read(UC_ARM_REG_CPSR)
                if (cpsr & 31) == 31 and not (cpsr & 0x80):
                    self.fast_inject_exception('irq')

    def get_ppm_bytes(self):
        raw = bytes(self.uc.mem_read(FB_ADDRESS, self.frame_size))
        # Convert XRGB8888 (B, G, R, X in little-endian memory when order==1 is R, G, B, X)
        rgb = bytearray(self.sim_w * self.sim_h * 3)
        # Fast slice assignment per channel
        rgb[0::3] = raw[0::4]
        rgb[1::3] = raw[1::4]
        rgb[2::3] = raw[2::4]
        header = f'P6\n{self.sim_w} {self.sim_h}\n255\n'.encode('ascii')
        return header + bytes(rgb)


def read_config_resolution(project_root):
    w, h = 640, 480
    for cfg in (project_root / 'sdcard' / 'config.txt', project_root / 'config.txt'):
        if cfg.is_file():
            for line in cfg.read_text(encoding='utf-8', errors='ignore').splitlines():
                line = line.split('#', 1)[0].strip()
                if '=' in line:
                    k, v = [p.strip().lower() for p in line.split('=', 1)]
                    if k == 'framebuffer_width' and v.isdigit():
                        w = int(v)
                    elif k == 'framebuffer_height' and v.isdigit():
                        h = int(v)
            break
    return w, h


def main():
    parser = argparse.ArgumentParser(description='Windows uC/OS-II + LVGL ARM1176 Simulator')
    project_root = Path(__file__).resolve().parents[1]
    cfg_w, cfg_h = read_config_resolution(project_root)
    parser.add_argument('--elf', type=Path, default=project_root / 'build' / 'kernel.elf')
    parser.add_argument('--width', type=int, default=cfg_w)
    parser.add_argument('--height', type=int, default=cfg_h)
    args = parser.parse_args()

    if not args.elf.is_file():
        print(f'Error: {args.elf} not found. Run .\\tools\\build-and-stage.ps1 first.')
        sys.exit(1)

    sim = InteractiveSimTarget(args.elf, project_root, args.width, args.height)

    root = tk.Tk()
    root.title('Pi Zero W uC/OS-II + LVGL + ViewGP Simulator (ARM1176)')
    root.configure(bg='#0f172a')

    info_var = tk.StringVar(value='Booting ARM1176 uC/OS-II kernel...')
    info_bar = tk.Label(
        root, textvariable=info_var, fg='#38bdf8', bg='#0f172a',
        font=('Consolas', 10), anchor='w', padx=8, pady=4
    )
    info_bar.pack(fill='x')

    canvas = tk.Label(root, bg='black', bd=0)
    canvas.pack(padx=6, pady=(0, 6))

    help_lbl = tk.Label(
        root,
        text='操作說明: [Tab / 方向鍵] 切換焦點   [Enter / 空白鍵] 按下按鈕或開關   [0-9 / + / - / Backspace] 修改數值輸入框   [Esc] 關閉彈窗',
        fg='#94a3b8', bg='#0f172a', font=('Microsoft JhengHei', 10), pady=4
    )
    help_lbl.pack(fill='x')

    def refresh_display():
        frames = sim.value('app_display_frames') if sim.value('gui_ready') else 0
        if frames != last_frames[0] or photo_holder[0] is None:
            last_frames[0] = frames
            ppm = sim.get_ppm_bytes()
            photo = tk.PhotoImage(data=ppm, format='PPM')
            canvas.configure(image=photo)
            photo_holder[0] = photo

        os_ticks = sim.value('OSTime') if sim.value('OSRunning', 1) == 1 else 0
        usb_st = sim.value('usb_state')
        keys = sim.value('usb_key_presses')
        irq_sw = sim.value('port_irq_switches')
        svc_sw = sim.value('port_svc_switches')
        info_var.set(
            f'uC/OS-II OSTime={os_ticks}  |  IRQ_SW={irq_sw} SVC_SW={svc_sw}  |  '
            f'GUI_Frames={frames}  |  USB_State={usb_st} Keys={keys}'
        )

    def on_key(event):
        mapped = KEYSYM_TO_HID.get(event.keysym)
        if mapped:
            sim.push_key_press_release(mapped[0], mapped[1])
            if sim.fast_jit:
                sim.step_quantum(80)
                refresh_display()

    root.bind('<KeyPress>', on_key)
    photo_holder = [None]
    last_frames = [-1]

    def tick():
        sim.step_quantum(45)
        refresh_display()
        root.after(12, tick)

    root.after(10, tick)
    root.mainloop()


if __name__ == '__main__':
    main()
