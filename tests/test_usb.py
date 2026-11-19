"""Execute production ARM ELF with a synthetic DWC2 DMA + boot keyboard.
The device responses are synthetic. No physical USB/PHY/electrical test is implied.
"""
import argparse,struct,json
from pathlib import Path
from hdmi_model import HDMITarget,MBOX_WRITE
from unicorn import UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
BASE=0x20980000
def report(mod=0,key=0):return bytes([mod,0,key,0,0,0,0,0])
# payload, number of expected decoded GUI events
STEPS=[(report(2,4),1),(report(),1),(report(0,57),0),(report(),0),
       (report(0,5),1),(report(),1),(report(2,6),1),(report(),1),
       (report(0,42),1),(report(),1),(report(0,57),0),(report(),0),
       (report(0,7),1),(report(0,7),0),(bytes([0,0,1,1,1,1,1,1]),0),(report(),1),
       (report(0,43),1),(report(),1),(report(0,40),1),(report(),1),
       (report(2,43),1),(report(),1),(report(0,40),1),(report(),1),
       (report(0,79),1),(report(),1),(report(0,40),1),(report(),1),
       (report(2,46),1),(report(),1),(report(0,45),1),(report(),1),
       (report(0,40),1)] # disconnect with Enter held: must cancel, not click

class USBTarget(HDMITarget):
    def __init__(self,a):
        super().__init__(a.elf,a.ticks,'rgb');self.mode=a.mode
        self.max_blocks=30000000
        self.uc.mem_map(BASE,0x10000)
        self.uc.hook_add(UC_HOOK_MEM_READ,self.on_read,begin=BASE,end=BASE+0xffff)
        self.uc.hook_add(UC_HOOK_MEM_WRITE,self.on_write,begin=BASE,end=BASE+0xffff)
        self.reg={0x40:0x4f54280a,0x48:(2<<3)|(7<<14)|(1<<6),0x4c:4080<<16,0x10:1<<31,0x14:1}
        self.connected=a.mode!='absent';self.enabled=False;self.port_power=False;self.port_reset=False
        self.address=0;self.configured=False;self.boot=False;self.setup=None;self.offset=0
        self.ep0=64 if a.mode=='full' else 8;self.mps=16 if a.mode=='full' else 8
        self.device=bytes([18,1,0x10,1,0,0,0,self.ep0,0x34,0x12,0x78,0x56,0,1,0,0,0,1])
        self.config=bytearray([9,2,43,0,2,1,0,0x80,50,9,4,0,0,0,0xff,0,0,0,
            9,4,3,0,1,3,1,1,0,9,0x21,0x11,1,0,1,0x22,63,0,7,5,0x81,3,self.mps,0,10])
        if a.mode=='malformed':self.config[18]=0
        self.step=0;self.report_goal=0;self.event_goal=0
        self.interrupt_pid=0;self.ctrl_pid=2;self.leds=[];self.setup_log=[];self.dmas=0
        self.nak_once=False;self.disconnected=False;self.disconnected_at=0
        self.reset_goal=0;self.replugged=False;self.second_keyboard=False
        self.old_bus_write=None
    def port(self):
        speed=1 if self.mode=='full' else 2
        return int(self.connected)|(4 if self.enabled else 0)|(speed<<17)|(4096 if self.port_power else 0)|(256 if self.port_reset else 0)
    def on_read(self,uc,access,address,size,value,user):
        if BASE<=address<BASE+0x10000:
            r=address-BASE
            self.store32(address,self.port() if r==0x440 else (self.us//1000)&0xffff if r==0x408 else self.reg.get(r,0))
            return
        super().on_read(uc,access,address,size,value,user)
    def finish(self,flags,actual=0):
        requested=self.reg[0x510]&0x7ffff
        self.reg[0x508]=2|flags;self.reg[0x500]&=~(1<<31)
        self.reg[0x510]=(self.reg[0x510]&~0x7ffff)|(requested-actual)
    def dma_write(self,raw):
        address=self.reg[0x514];assert address&0xc0000000==0x40000000
        address&=0x3fffffff
        assert address%64==0 and 0x8000<address<0x1000000 and len(raw)<=64
        self.uc.mem_write(address,raw);self.dmas+=1
    def transfer(self,ch):
        ep=(ch>>11)&15;direction=bool(ch&(1<<15));kind=(ch>>18)&3;addr=(ch>>22)&127
        size=self.reg[0x510]&0x7ffff;pid=self.reg[0x510]>>29
        assert (self.reg[0x510]>>19)&1023==1
        assert self.reg.get(0x504,0)==0 and ch&(1<<20)
        assert bool(ch&(1<<17))==(self.mode!='full')
        assert addr==self.address,(addr,self.address)
        assert self.reg[0x514]&0xc0000000==0x40000000
        dma=self.reg[0x514]&0x3fffffff
        assert dma%64==0 and size<=64
        if self.mode=='timeout':
            self.reg[0x508]=0 # leave channel active; host must halt before reuse
            return
        if ep==0:
            assert kind==0
            if pid==3:
                assert not direction and size==8
                if not self.nak_once:
                    self.nak_once=True;self.finish(16);return
                self.setup=struct.unpack('<BBHHH',self.uc.mem_read(dma,8));self.setup_log.append(self.setup)
                self.offset=0;self.ctrl_pid=2
                bm,req,val,index,length=self.setup
                if req==6:
                    assert bm==0x80;source=self.device if val==0x100 else bytes(self.config)
                    self.response=source[:length]
                elif req in (10,11) or (req==9 and bm==0x21):assert index==3
                self.finish(1,8);return
            assert self.setup is not None
            bm,req,val,index,length=self.setup
            data_stage=length>0 and self.offset<length
            if data_stage:
                assert direction==bool(bm&128) and pid==self.ctrl_pid
                if direction:
                    raw=self.response[self.offset:self.offset+(ch&2047)]
                    self.dma_write(raw);self.offset+=len(raw);self.ctrl_pid^=2;self.finish(1,len(raw));return
                assert req==9 and val==0x200 and length==1
                self.leds.append(bytes(self.uc.mem_read(dma,size))[0]);self.offset+=size
                self.ctrl_pid^=2;self.finish(1,size);return
            assert pid==2 and direction==(not bool(bm&128) if length else True)
            if req==5:self.address=val
            if req==9 and bm==0:self.configured=True;self.interrupt_pid=0
            if req==11:assert val==0;self.boot=True
            if req==10 and self.mode=='full':self.finish(8);return # optional SET_IDLE stalls
            self.finish(1);return
        assert ep==1 and kind==3 and direction and self.configured and self.boot
        assert pid==self.interrupt_pid and size==self.mps
        if not self.value('gui_ready') or self.value('app_display_frames')<1:
            self.finish(16);return
        if self.replugged:
            self.second_keyboard=True;self.finish(16);return
        if self.step>=len(STEPS) or self.value('usb_reports')<self.report_goal or self.value('gui_usb_events')<self.event_goal:
            self.finish(16);return
        raw,events=STEPS[self.step];self.step+=1
        self.report_goal=self.value('usb_reports')+1
        self.event_goal=self.value('gui_usb_events')+events
        self.dma_write(raw);self.interrupt_pid^=2;self.finish(1,len(raw))
    def on_write(self,uc,access,address,size,value,user):
        if address==MBOX_WRITE:
            a=value&~15;w=list(struct.unpack('<8I',uc.mem_read(a,32)))
            if w[0]==32 and w[2]==0x28001:
                assert w[3:7]==[8,8,3,3]
                w[1]=0x80000000;w[4]=0x80000008;w[6]=1
                uc.mem_write(a,struct.pack('<8I',*w));self.responses.append(value);return
        if BASE<=address<BASE+0x10000:
            r=address-BASE
            if r==0x10:self.reg[r]=1<<31
            elif r==0x14:self.reg[r]=1
            elif r==0x508:self.reg[r]=self.reg.get(r,0)&~value
            elif r==0x440:
                assert not value&((1<<1)|(1<<2)|(1<<3)|(1<<5)),'HPRT W1C/enable written'
                self.port_power=bool(value&4096)
                if self.port_reset and not value&256:
                    self.enabled=self.connected;self.address=0;self.configured=False;self.boot=False
                self.port_reset=bool(value&256)
            elif r==0x500:
                self.reg[r]=value
                if value&(1<<30):self.reg[r]&=~(1<<31);self.reg[0x508]=2
                elif value&(1<<31):self.transfer(value)
            else:self.reg[r]=value
            return
        super().on_write(uc,access,address,size,value,user)
    def on_block(self,uc,address,size,user):
        super().on_block(uc,address,size,user)
        if self.blocks%64 or self.value('OSRunning',1)!=1 or self.mode not in ('low','full'):return
        if self.step==len(STEPS) and self.value('gui_usb_down') and self.value('gui_usb_events')>=self.event_goal and not self.disconnected:
            assert self.value('gui_value')==51,self.value('gui_value')
            self.connected=False;self.enabled=False;self.disconnected=True
            self.disconnected_at=self.value('OSTime');self.reset_goal=self.value('gui_usb_resets')+1
        if self.disconnected and not self.replugged and self.value('OSTime')-self.disconnected_at>=30 and self.value('gui_usb_resets')>=self.reset_goal and self.value('gui_usb_down')==0:
            assert self.value('gui_usb_down')==0 and self.value('gui_value')==51,(self.value('gui_usb_down'),self.value('gui_value'))
            self.connected=True;self.replugged=True;self.step=len(STEPS)
    def check(self):
        assert self.value('gui_ready')==1 and self.value('app_display_frames')>1
        assert self.value('usb_stack')==0 and self.value('gui_stack')==0
        if self.mode in ('low','full'):
            assert self.step==len(STEPS) and self.second_keyboard,(self.step,len(STEPS),self.second_keyboard)
            assert self.value('usb_connects')==2 and self.value('usb_disconnects')==1
            assert self.value('gui_value')==51 and self.value('gui_usb_down')==0
            text=bytes(self.uc.mem_read(self.symbols['gui_usb_text'],64)).split(b'\0')[0]
            assert text==b'ABd+-',text
            assert 2 in self.leds and 0 in self.leds
            assert self.value('usb_rollovers')==1 and self.value('usb_queue_drops')==0
        elif self.mode=='absent':assert self.value('usb_state')==2 and self.value('usb_connects')==0
        else:assert self.value('usb_state')==254 and self.value('usb_connects')==0
def main():
    p=argparse.ArgumentParser();p.add_argument('--elf',type=Path,default=Path('build/kernel.elf'))
    p.add_argument('--mode',choices=['low','full','absent','malformed','timeout'],default='low')
    p.add_argument('--ticks',type=int,default=8000);p.add_argument('--log',type=Path)
    a=p.parse_args();t=USBTarget(a)
    try:text,out=t.run();t.check()
    except Exception:
        print(''.join(t.output)[-3500:]);print('step',t.step,'goal',t.event_goal,'events',t.value('gui_usb_events'),'state',t.value('usb_state'),'error',t.value('usb_error'))
        raise
    out.update(mode=a.mode,dma_writes=t.dmas,led_reports=t.leds,keyboard_steps=t.step,
       **{n:t.value(n) for n in ['usb_state','usb_reports','usb_connects','usb_disconnects','usb_rollovers','usb_queue_drops','gui_usb_events','gui_usb_resets','gui_value','gui_usb_down']})
    result=text+'\nPASS: production ARM USB/HID/LVGL with synthetic DWC2; NOT physical USB.\n'+json.dumps(out,indent=2)
    print(result)
    if a.log:a.log.write_text(result,encoding='utf8')
if __name__=='__main__':main()
