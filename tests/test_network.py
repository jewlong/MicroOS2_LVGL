"""Actual ARM tests with explicit synthetic SDHCI/Ethernet boundaries.
No radio firmware execution, RF, association or WPA2 interoperability is modeled.
"""
import argparse, struct, json
from pathlib import Path
from collections import deque
from hdmi_model import HDMITarget, MBOX_WRITE
from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
EMMC=0x20300000
MAC=bytes.fromhex('025212345678')
PEER=bytes.fromhex('021122334455')
SERVER=bytes([10,0,0,1]); IP=bytes([10,0,0,20]); CLIENT=bytes([10,0,0,2])
UDP_PAYLOAD=b'ucos2-lwip-udp-echo'
TCP_PAYLOAD=b'ucos2-lwip-tcp-echo-cross-task'
def checksum(data):
    if len(data)%2:data+=b'\0'
    n=sum(struct.unpack('!%dH'%(len(data)//2),data))
    while n>>16:n=(n&65535)+(n>>16)
    return (~n)&65535
def ipv4(proto,src,dst,body):
    h=struct.pack('!BBHHHBBH4s4s',0x45,0,20+len(body),1,0,64,proto,0,src,dst)
    return h[:10]+struct.pack('!H',checksum(h))+h[12:]+body
def ethernet(body,kind=0x800,dst=MAC):
    return dst+PEER+struct.pack('!H',kind)+body
def udp(src,dst,sport,dport,body):
    return ethernet(ipv4(17,src,dst,struct.pack('!HHHH',sport,dport,len(body)+8,0)+body))
def tcp(seq,ack,flags,body=b''):
    h=struct.pack('!HHIIBBHHH',40001,5001,seq,ack,0x50,flags,12000,0,0)
    c=checksum(CLIENT+IP+bytes([0,6])+struct.pack('!H',len(h)+len(body))+h+body)
    h=h[:16]+struct.pack('!H',c)+h[18:]
    return ethernet(ipv4(6,CLIENT,IP,h+body))
def options(data):
    out={};i=240
    while i<len(data):
        tag=data[i];i+=1
        if tag==255:break
        if tag==0:continue
        size=data[i];i+=1;out[tag]=data[i:i+size];i+=size
    return out

class NetworkTarget(HDMITarget):
    def __init__(self,args):
        super().__init__(args.elf,args.ticks,'rgb')
        self.kind=args.mode;self.max_blocks=20000000
        self.uc.mem_map(EMMC,4096)
        self.uc.hook_add(UC_HOOK_MEM_READ,self.on_read,begin=EMMC,end=EMMC+4095)
        self.uc.hook_add(UC_HOOK_MEM_WRITE,self.on_write,begin=EMMC,end=EMMC+4095)
        self.regs={};self.intr=0;self.pio=0;self.wire=0;self.blocksize=0;self.written=bytearray()
        self.sdio_transfers=[];self.commands=[]
        self.frames=deque();self.txseq=0;self.dhcp=[];self.udp_echo=False;self.tcp_echo=False
        self.sent_apps=False;self.tcp_started=False;self.tcp_connected=False;self.tcp_data=b''
        self.rx_count=0
    def write_symbol(self,name,value):
        self.store32(self.symbols[name],value)
    def on_read(self,uc,access,address,size,value,user):
        if EMMC<=address<EMMC+4096:
            r=address-EMMC
            if r==0x30:self.store32(address,self.intr)
            elif r==0x20:
                chunk=bytes((self.pio+j)&255 for j in range(4))
                self.store32(address,int.from_bytes(chunk,'little'));self.pio+=4;self.advance()
            else:self.store32(address,self.regs.get(r,0))
            return
        super().on_read(uc,access,address,size,value,user)
    def advance(self):
        if self.pio>=self.wire:
            self.intr|=2
            self.sdio_transfers.append(bytes(self.written))
        elif self.pio%self.blocksize==0:
            self.intr|=32 if self.reading else 16
    def on_write(self,uc,access,address,size,value,user):
        if address==MBOX_WRITE and self.kind=='sdio':
            a=value&~15
            w=list(struct.unpack('<8I',uc.mem_read(a,32)))
            if w[0]==32:
                assert w[7]==0 and w[3]==8
                w[1]=0x80000000;w[4]=0x80000008
                if w[2]==0x30002:w[5:7]=[1,250000000]
                elif w[2]==0x10004:w[5:7]=[0x12345678,0]
                else:raise AssertionError(w)
                uc.mem_write(a,struct.pack('<8I',*w));self.responses.append(value);return
        if EMMC<=address<EMMC+4096:
            r=address-EMMC
            if r==0x30:self.intr&=~value
            elif r==0x2c:self.regs[r]=(value&~(7<<24))|2
            elif r==0x0c:
                self.regs[r]=value;cmd=value>>24;arg=self.regs.get(8,0)
                self.commands.append(cmd);response=0
                if cmd==5:response=0xa0300000
                elif cmd==3:response=0x12340000
                elif cmd==7:self.intr|=2
                elif cmd==52:response=0x8000 if ((arg>>9)&0x1ffff)==2 else 0x12
                elif cmd==53:
                    self.blocksize=self.regs[4]&4095;blocks=self.regs[4]>>16
                    self.wire=self.blocksize*blocks;self.pio=0;self.written=bytearray()
                    self.reading=bool(value&16);self.intr|=32 if self.reading else 16
                    assert bool(arg&(1<<31)) != self.reading
                self.regs[0x10]=response;self.intr|=1
            elif r==0x20:
                self.written.extend(value.to_bytes(4,'little'));self.pio+=4;self.advance()
            else:self.regs[r]=value
            return
        super().on_write(uc,access,address,size,value,user)
    def packet(self,frame):
        kind=int.from_bytes(frame[12:14],'big');body=frame[14:]
        if kind==0x806:
            op=int.from_bytes(body[6:8],'big');target=body[24:28]
            if op==1 and target in (SERVER,CLIENT):
                reply=struct.pack('!HHBBH',1,0x800,6,4,2)+PEER+target+body[8:14]+body[14:18]
                self.frames.append(ethernet(reply,0x806))
            return
        if kind!=0x800:return
        size=int.from_bytes(body[2:4],'big');header=(body[0]&15)*4
        assert checksum(body[:header])==0
        src,dst=body[12:16],body[16:20];payload=body[header:size]
        if body[9]==17:
            sport,dport,length,cs=struct.unpack('!HHHH',payload[:8]);data=payload[8:length]
            if sport==68 and dport==67:
                opt=options(data);msg=opt[53][0];self.dhcp.append(msg)
                assert msg in (1,3),msg
                boot=bytearray(240);boot[:4]=bytes([2,1,6,0]);boot[4:8]=data[4:8]
                boot[16:20]=IP;boot[20:24]=SERVER;boot[28:34]=MAC
                boot[236:240]=bytes([99,130,83,99])
                opts=bytes([53,1,2 if msg==1 else 5,54,4])+SERVER
                opts+=bytes([51,4])+struct.pack('!I',3600)+bytes([1,4,255,255,255,0,3,4])+SERVER+bytes([6,4])+SERVER+bytes([255])
                self.frames.append(udp(SERVER,bytes([255]*4),67,68,bytes(boot)+opts))
            elif sport==5000:
                assert data==UDP_PAYLOAD,(sport,dport,data);self.udp_echo=True
        elif body[9]==6:
            sport,dport,seq,ack=struct.unpack('!HHII',payload[:12])
            assert sport==5001 and dport==40001
            assert checksum(src+dst+bytes([0,6])+struct.pack('!H',len(payload))+payload)==0
            flags=payload[13];data=payload[(payload[12]>>4)*4:]
            if flags&2:
                assert flags&16 and ack==1001
                self.server_next=seq+1
                self.frames.append(tcp(1001,self.server_next,16))
                self.frames.append(tcp(1001,self.server_next,24,TCP_PAYLOAD));self.tcp_connected=True
            elif data:
                assert seq==self.server_next
                self.server_next+=len(data);self.tcp_data+=data
                self.frames.append(tcp(1001+len(TCP_PAYLOAD),self.server_next,16))
                if self.tcp_data==TCP_PAYLOAD:self.tcp_echo=True
    def on_block(self,uc,address,size,user):
        super().on_block(uc,address,size,user)
        if self.value('OSRunning',1)!=1:return
        if self.value('test_phase')==1 and self.kind=='sdio':self.write_symbol('test_mode',1)
        if self.kind!='ethernet':return
        seq=self.value('test_tx_seq')
        if seq!=self.txseq:
            assert seq==self.txseq+1
            self.txseq=seq
            self.packet(bytes(uc.mem_read(self.symbols['test_tx_data'],self.value('test_tx_len'))))
        if self.value('net_stage')==6 and not self.sent_apps:
            bad=bytearray(udp(CLIENT,IP,40000,5000,b'bad-checksum'));bad[24]^=1
            self.frames.append(bytes(bad))
            self.frames.append(udp(CLIENT,IP,40000,5000,UDP_PAYLOAD))
            self.sent_apps=True
        if self.udp_echo and not self.tcp_started:
            self.frames.append(tcp(1000,0,2));self.tcp_started=True
        if self.frames and self.value('test_phase')==2 and not self.value('test_rx_pending'):
            data=self.frames.popleft();uc.mem_write(self.symbols['test_rx_data'],data)
            self.write_symbol('test_rx_len',len(data));self.write_symbol('test_rx_pending',1);self.rx_count+=1
    def check(self):
        assert self.value('test_assertions')>=30
        assert self.value('gui_ready')==1 and self.value('app_display_frames')>0
        for name in ('network_stack','tcpip_stack','gui_stack'):
            assert self.value(name)==0,(name,'stack guard overwritten')
        if self.kind=='sdio':
            assert self.value('test_phase')==3
            assert self.commands[:5]==[0,5,5,3,7],self.commands
            assert self.value('net_sdio_errors')==1
            assert self.sdio_transfers[-1][:70]==bytes(range(70))
            assert self.sdio_transfers[-1][70:128]==bytes(58)
        else:
            assert self.dhcp[:2]==[1,3],self.dhcp
            assert self.udp_echo and self.tcp_echo,(self.udp_echo,self.tcp_data)
            assert self.value('net_udp_echoes')==1
            assert self.value('net_tcp_bytes')==len(TCP_PAYLOAD)
            assert self.value('net_rx_frames')==self.rx_count
def main():
    p=argparse.ArgumentParser()
    p.add_argument('--mode',choices=['sdio','ethernet'],required=True)
    p.add_argument('--elf',type=Path,default=Path('build/network-test.elf'))
    p.add_argument('--ticks',type=int,default=2500);p.add_argument('--log',type=Path)
    a=p.parse_args();t=NetworkTarget(a)
    try:text,report=t.run();t.check()
    except Exception:
        print(''.join(t.output)[-5000:]);print('phase',t.value('test_phase'),'assertions',t.value('test_assertions'),'DHCP',t.dhcp)
        raise
    report.update(mode=a.mode,assertions=t.value('test_assertions'),dhcp=t.dhcp,
        udp_echo=t.udp_echo,tcp_echo=t.tcp_echo,sdio_commands=t.commands,
        rx=t.value('net_rx_frames'),tx=t.value('net_tx_frames'),gui_frames=t.value('app_display_frames'))
    result=text+'\nPASS: actual ARM tests with synthetic '+a.mode+' boundary; NOT physical Wi-Fi.\n'+json.dumps(report,indent=2)
    print(result)
    if a.log:a.log.write_text(result,encoding='utf8')
if __name__=='__main__':main()
