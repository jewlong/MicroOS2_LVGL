"""Execute the actual ARM LVGL/RTOS ELF with synthetic mailbox/UART/timer.
No replacement of LVGL, rendering, task scheduler or CPU port functions.
Synthetic time is NOT hardware performance. HDMI signaling is not modeled.
"""
import argparse
import json
from pathlib import Path
from hdmi_model import HDMITarget, FB_ADDRESS, FB_WIDTH, FB_HEIGHT

class GUITarget(HDMITarget):
    def __init__(self, args):
        super().__init__(args.elf, args.ticks, args.case, args.initial_us, args.late_irq)
        self.max_blocks = 20000000
        self.phase = 0
        self.phase_frame = 0
        self.interactive = args.case in ('rgb', 'bgr')
        self.expected_uart_commands = 10 if self.interactive else 3
        self.value_steps = []

    def on_block(self, uc, address, size, user):
        super().on_block(uc, address, size, user)
        if not self.interactive or self.blocks % 256:
            return
        if self.value('gui_ready') != 1 or not self.input_injected:
            return
        frames = self.value('app_display_frames')
        count = self.value('gui_commands')
        if self.phase == 0 and frames >= 2:
            self.rx.extend(b'++-')
            self.phase = 1
        elif self.phase == 1 and count == 3:
            assert self.value('gui_value') == 51
            self.value_steps.append(51)
            self.phase_frame = frames
            self.phase = 2
        elif self.phase == 2 and frames > self.phase_frame + 1:
            self.rx.extend(b'r')
            self.phase = 3
        elif self.phase == 3 and count == 4:
            assert self.value('gui_value') == 50
            self.value_steps.append(50)
            self.phase_frame = frames
            self.phase = 4
        elif self.phase == 4 and frames > self.phase_frame + 1:
            self.rx.extend(b'+++')
            self.phase = 5
        elif self.phase == 5 and count == 7:
            assert self.value('gui_value') == 53
            self.value_steps.append(53)
            self.phase_frame = frames
            self.phase = 6

    def rgb(self, x, y):
        b = bytes(self.uc.mem_read(FB_ADDRESS + y*self.pitch + x*4, 4))
        return tuple(b[:3]) if self.order else (b[2],b[1],b[0])

    def check_frame(self):
        assert self.value('net_stage') == 255
        assert self.value('net_error') == 0xe100
        assert self.value('tcpip_created',1) == 1
        assert self.property_requests == (0 if self.case == 'full' else 1)
        assert self.value('ready', 1) == int(self.interactive)
        if not self.interactive:
            assert self.value('gui_ready') == 0
            assert self.value('app_display_frames') == 0
            assert bytes(self.uc.mem_read(FB_ADDRESS, self.frame_size)) == b'\xa5' * self.frame_size
            assert '[GUI] disabled:' in ''.join(self.output)
            return
        assert self.phase == 6, (self.phase, self.value('app_display_frames'))
        assert self.value('app_display_frames') > self.phase_frame + 1
        assert self.value_steps == [51,50,53]
        assert self.value('gui_commands') == 7
        assert self.value('gui_button_events') == 7
        assert self.value('gui_value') == 53
        assert self.value('gui_dropped') == 0
        assert self.value('gui_owner_checks') > 20
        assert self.value('gui_ready') == 1
        assert self.value('gui_heap_free') > 128*1024
        assert self.value('gui_stack_free') > 8*1024
        assert self.value('gui_stack') == 0
        # Real output checks: background, card, two differently colored buttons,
        # and bar boundary matching the queued value (53%, not initial 50%).
        samples = [
            (0,0,(0x10,0x1a,0x2b)),
            (40,180,(0x1c,0x2b,0x40)),
            (50,375,(0x50,0xdf,0xb6)),
            (210,375,(0xff,0xb8,0x6b)),
            (330,305,(0xff,0xb8,0x6b)),
            (345,305,(0x10,0x1a,0x2b)),
        ]
        for x,y,expected in samples:
            assert self.rgb(x,y) == expected, (x,y,self.rgb(x,y),expected)
        # Glyph rasterization must have produced light text in the title area.
        assert any(self.rgb(x,y)[0] > 220 for y in range(43,76) for x in range(24,350))
        for y in range(FB_HEIGHT):
            padding = bytes(self.uc.mem_read(FB_ADDRESS+y*self.pitch+FB_WIDTH*4,self.pitch-FB_WIDTH*4))
            assert padding == b'\xa5'*len(padding), f'Row padding overwritten: {y}'
        for addr in (FB_ADDRESS-4096, FB_ADDRESS+self.frame_size):
            assert bytes(self.uc.mem_read(addr,4096)) == b'\xa5'*4096

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--elf',type=Path,default=Path(__file__).resolve().parents[1]/'build/kernel.elf')
    parser.add_argument('--ticks',type=int,default=4000)
    parser.add_argument('--case',choices=['rgb','bgr','bad_pitch','bad_depth','bad_tag',
        'overlap','short_size','timeout','full','bad_geometry'],default='rgb')
    parser.add_argument('--initial-us',type=lambda x:int(x,0),default=0)
    parser.add_argument('--late-irq',action='store_true')
    parser.add_argument('--snapshot',type=Path)
    parser.add_argument('--log',type=Path)
    args=parser.parse_args()
    target=GUITarget(args)
    try:
        text,report=target.run()
        if args.snapshot: target.snapshot(args.snapshot)
        target.check_frame()
    except Exception:
        print(''.join(target.output)[-5000:])
        print('phase',target.phase,'frames',target.value('app_display_frames'),
              'commands',target.value('gui_commands'),'blocks',target.blocks)
        raise
    report.update(case=args.case, expected_gui=target.interactive, pixel_order=target.order,
        pitch=target.pitch, value_steps=target.value_steps, **{name:target.value(name)
        for name in ['app_display_frames','gui_commands','gui_dropped','gui_value',
                     'gui_button_events','gui_heap_free','gui_stack_free','gui_owner_checks']})
    if args.snapshot:
        target.snapshot(args.snapshot)
    result=text+'\nPASS: actual ARM LVGL GUI, queue, rendering and RTOS; NOT physical HDMI.\n'+json.dumps(report,indent=2)+'\n'
    print(result)
    if args.log:
        args.log.write_text(result,encoding='utf-8')

if __name__=='__main__':
    main()
