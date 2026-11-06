"""Execute the delivered ARM ELF with a deliberately small synthetic MMIO model.

This is a CPU/context-switch regression test, not BCM2835/firmware simulation.
Unicorn executes the actual ARM instructions (including SRS/RFE). This harness
models exception entry and GPIO/UART/system-timer/IRQ registers. No code patches,
kernel function replacements, pre-recorded UART output or Linux are used.
"""
import argparse
from collections import deque
from pathlib import Path
import struct
import json

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_BLOCK
from unicorn import UC_HOOK_INTR, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import (
    UC_CPU_ARM_1176, UC_ARM_REG_CPSR, UC_ARM_REG_SPSR, UC_ARM_REG_PC,
    UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1,
    UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5,
    UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9,
    UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12,
)

REGISTERS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
             UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
             UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
             UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_CPSR]
TIMER_CS, TIMER_CLO, TIMER_C1 = 0x20003000, 0x20003004, 0x20003010
PENDING1, ENABLE1, DISABLE1 = 0x2000B204, 0x2000B210, 0x2000B21C
UART_DR, UART_FR = 0x20201000, 0x20201018
MASK = 0xFFFFFFFF


class Target:
    def __init__(self, elf_path, ticks, initial_us=0, delay_once=False):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
        self.uc.mem_map(0, 0x01000000)
        self.uc.mem_map(0x20000000, 0x00300000)
        # Poison BSS before boot: startup.s must clear it, rather than relying
        # on a zero-initialized emulator memory allocation.
        self.uc.mem_write(0x8000, b'\xa5' * (0x01000000 - 0x8000))
        self.symbols = {}
        with elf_path.open('rb') as source:
            elf = ELFFile(source)
            for section in elf.iter_sections():
                if section['sh_type'] == 'SHT_SYMTAB':
                    for symbol in section.iter_symbols():
                        self.symbols[symbol.name] = symbol['st_value']
            for segment in elf.iter_segments():
                if segment['p_type'] == 'PT_LOAD':
                    self.uc.mem_write(segment['p_paddr'], segment.data())
        self.uc.reg_write(UC_ARM_REG_CPSR, 0x1D3)
        self.uc.reg_write(UC_ARM_REG_PC, 0x8000)
        self.us = initial_us
        self.initial_us = initial_us
        self.tick_goal = ticks
        self.delay_once = delay_once
        self.delay_injected = False
        self.compare = None
        self.cs = 0
        self.enable1 = 0
        self.rx = deque()
        self.input_injected = False
        self.output = []
        self.pending = None
        self.failure = None
        self.blocks = 0
        self.max_blocks = 3000000
        self.min_low_completed = 5
        self.expected_uart_commands = 3
        self.irq_entries = 0
        self.svc_entries = 0
        self.verified_irq_returns = 0
        self.verified_svc_returns = 0
        self.pending_checks = {}
        self.mode_seen = set()
        self.uc.hook_add(UC_HOOK_BLOCK, self.on_block)
        self.uc.hook_add(UC_HOOK_INTR, self.on_intr)
        self.uc.hook_add(UC_HOOK_MEM_READ, self.on_read,
                         begin=0x20000000, end=0x202FFFFF)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.on_write,
                         begin=0x20000000, end=0x202FFFFF)

    def value(self, name, size=4):
        return int.from_bytes(self.uc.mem_read(self.symbols[name], size), 'little')

    def store32(self, address, value):
        self.uc.mem_write(address, struct.pack('<I', value & MASK))

    def timer_update(self):
        if self.compare is not None and self.us >= self.compare:
            self.cs |= 2
            self.compare = None

    def on_read(self, uc, access, address, size, value, user):
        self.timer_update()
        if address == TIMER_CLO:
            self.store32(address, self.us)
        elif address == TIMER_CS:
            self.store32(address, self.cs)
        elif address == PENDING1:
            self.store32(address, self.cs & self.enable1)
        elif address == UART_FR:
            self.store32(address, 0 if self.rx else 16)
        elif address == UART_DR:
            self.store32(address, self.rx.popleft() if self.rx else 0)

    def on_write(self, uc, access, address, size, value, user):
        if address == TIMER_CS:
            self.cs &= ~value
        elif address == TIMER_C1:
            delta = (value - (self.us & MASK)) & MASK
            self.compare = self.us + delta
        elif address == ENABLE1:
            self.enable1 |= value
        elif address == DISABLE1:
            self.enable1 &= ~value
        elif address == UART_DR:
            ch = chr(value & 255)
            self.output.append(ch)
            # Do not continue forever in the target's fatal spin loop.
            if ch == '\n' and ('PANIC:' in ''.join(self.output[-160:])):
                self.failure = 'Target panic: ' + ''.join(self.output[-200:])
                uc.emu_stop()

    def on_intr(self, uc, number, user):
        if number != 2:
            self.failure = f'Unexpected CPU exception {number}, PC={uc.reg_read(UC_ARM_REG_PC):08x}'
        else:
            self.pending = 'svc'
        uc.emu_stop()

    def on_block(self, uc, address, size, user):
        if self.pending_checks and (uc.reg_read(UC_ARM_REG_CPSR) & 31) == 31:
            task = self.value('OSTCBCur')
            entry = self.pending_checks.get(task)
            if entry and entry['pc'] == address:
                actual = [uc.reg_read(reg) for reg in REGISTERS]
                expected = entry['expected']
                assert actual == expected, (
                    f"{entry['kind']} register corruption at {address:08x}: "
                    + repr([(i, hex(a), hex(b)) for i, (a, b) in
                            enumerate(zip(expected, actual)) if a != b]))
                del self.pending_checks[task]
                if entry['kind'] == 'irq':
                    self.verified_irq_returns += 1
                else:
                    self.verified_svc_returns += 1
        self.blocks += 1
        self.us += 10  # Synthetic elapsed time, deliberately NOT cycle accurate.
        self.timer_update()
        cpsr = uc.reg_read(UC_ARM_REG_CPSR)
        self.mode_seen.add(cpsr & 31)
        if self.value('OSRunning', 1) == 1:
            ticks = self.value('OSTime')
            if self.delay_once and not self.delay_injected and ticks >= 30:
                self.us += 35000  # Simulate one late ISR; exercise tick catch-up.
                self.delay_injected = True
                self.timer_update()
            if ticks >= 200 and not self.input_injected:
                self.rx.extend(b'sX\r')
                self.input_injected = True
            if ticks >= self.tick_goal and (cpsr & 31) == 31 and self.value('OSIntNesting', 1) == 0:
                uc.emu_stop()
                return
        if self.blocks > self.max_blocks:
            self.failure = f'Execution stalled: PC={address:08x}'
            uc.emu_stop()
        elif self.cs & self.enable1 & 2 and not cpsr & 0x80:
            self.pending = 'irq'
            uc.emu_stop()

    def inject_exception(self, kind):
        uc = self.uc
        cpsr = uc.reg_read(UC_ARM_REG_CPSR)
        pc = uc.reg_read(UC_ARM_REG_PC)
        assert cpsr & 31 == 31, f'{kind} interrupted non-SYS mode: {cpsr:08x}'
        task = self.value('OSTCBCur')
        assert task not in self.pending_checks, 'Task interrupted before previous context returned'
        expected = [uc.reg_read(reg) for reg in REGISTERS]
        entry = {'task': task, 'pc': pc, 'expected': expected, 'kind': kind}

        self.pending_checks[task] = entry
        mode, vector = (0x12, 0x18) if kind == 'irq' else (0x13, 0x08)
        uc.reg_write(UC_ARM_REG_CPSR, (cpsr & ~0x3F) | mode | 0x80)
        uc.reg_write(UC_ARM_REG_SPSR, cpsr)
        uc.reg_write(UC_ARM_REG_LR, pc + (4 if kind == 'irq' else 0))
        uc.reg_write(UC_ARM_REG_PC, vector)
        if kind == 'irq':
            self.irq_entries += 1
        else:
            self.svc_entries += 1

    def run(self):
        while self.value('OSTime') < self.tick_goal or self.value('OSRunning', 1) != 1:
            # OSTime is poisoned before BSS clear, so OSRunning must also be 1,
            # not merely any nonzero value, before treating boot as complete.
            self.pending = None
            self.uc.emu_start(self.uc.reg_read(UC_ARM_REG_PC), 0x01000000)
            if self.failure:
                raise AssertionError(self.failure)
            if self.pending:
                self.inject_exception(self.pending)
            elif self.value('OSRunning', 1) == 1 and self.value('OSTime') >= self.tick_goal:
                break
            else:
                raise AssertionError('Target stopped unexpectedly')
        assert self.value('OSRunning', 1) == 1
        assert self.value('app_preempt_pass') == 1
        assert self.value('app_high_runs') >= 4
        assert self.value('app_low_completed') >= self.min_low_completed
        assert self.value('app_uart_commands') == self.expected_uart_commands
        assert self.verified_irq_returns >= 100
        assert self.verified_svc_returns >= 100
        assert self.value('port_irq_switches') > 0
        assert self.value('port_svc_switches') > 0
        assert self.value('OSIntNesting', 1) == 0
        assert {0x12, 0x13, 0x1F}.issubset(self.mode_seen)
        text = ''.join(self.output).replace('\r\n', '\n')
        assert '[PASS] HIGH woke by IRQ while LOW was busy (no yield).' in text
        assert 'X' in text
        assert 'PANIC:' not in text and 'FATAL' not in text
        for name in ('high_stack', 'uart_stack', 'low_stack', 'OSTaskIdleStk'):
            # Unused lowest task-stack words must remain zero.
            assert self.value(name) == 0, f'Stack overflow: {name}'
        for mode in ('svc', 'irq', 'abt', 'und', 'fiq'):
            assert self.value('__' + mode + '_stack_bottom') == 0xA5A5A5A5, 'Exception stack overflow: ' + mode
        if self.delay_once:
            assert self.value('bsp_late_ticks') >= 2
        report = {name: self.value(name) for name in (
            'OSTime', 'app_high_runs', 'app_low_completed', 'app_uart_commands',
            'port_irq_switches', 'port_svc_switches', 'bsp_late_ticks')}
        report.update(verified_irq_returns=self.verified_irq_returns,
                      verified_svc_returns=self.verified_svc_returns,
                      synthetic_elapsed_us=self.us - self.initial_us,
                      initial_us=self.initial_us, delay_once=self.delay_once)
        return text, report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--elf', type=Path, default=Path(__file__).resolve().parents[1] / 'build/kernel.elf')
    parser.add_argument('--ticks', type=int, default=600)
    parser.add_argument('--initial-us', type=lambda value: int(value, 0), default=0)
    parser.add_argument('--late-irq', action='store_true')
    parser.add_argument('--log', type=Path)
    args = parser.parse_args()
    target = Target(args.elf, args.ticks, args.initial_us, args.late_irq)
    try:
        text, report = target.run()
    except Exception:
        print(''.join(target.output))
        print('PC=', hex(target.uc.reg_read(UC_ARM_REG_PC)), 'CPSR=', hex(target.uc.reg_read(UC_ARM_REG_CPSR)))
        raise
    print(text)
    print('PASS: actual ARM ELF executed with synthetic MMIO; context registers checked.')
    print(json.dumps(report, indent=2))
    if args.log:
        args.log.write_text(text + '\n' + json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
