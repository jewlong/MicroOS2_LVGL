/* SPDX-License-Identifier: Apache-2.0 */
#include "wifi_port.h"
#include "wifi_config.h"
#include "whd_wifi_api.h"
#include "cyhal_sdio.h"
#include "cyhal_gpio.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "bsp.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
volatile uint32_t net_stage,net_error,net_rx_frames,net_tx_frames;
volatile uint32_t net_reconnects,net_udp_echoes,net_tcp_bytes;
static whd_driver_t driver;
static whd_interface_t interface;
static cyhal_sdio_t host;
_Alignas(8) static OS_STK whd_stack[8192];
static struct netif wlan;
static volatile bool netif_ready;
static OS_EVENT *core_done;
static char status_text[80]="Wi-Fi: starting";
static whd_sync_scan_result_t scan_results[16];
static const char ssid_text[]=WIFI_SSID,password[]=WIFI_PASSWORD;
_Static_assert(sizeof(ssid_text)-1<=32,"SSID too long");
_Static_assert(sizeof(password)-1<=63,"WPA2 passphrase too long");
extern volatile uint32_t app_preempt_pass;
static void status(const char *s)
{
    OS_CPU_SR sr=OS_CPU_SR_Save();
    size_t n=strlen(s);if(n>=sizeof(status_text))n=sizeof(status_text)-1;
    memcpy(status_text,s,n);status_text[n]=0;OS_CPU_SR_Restore(sr);
}
void network_snapshot(char *out,size_t size)
{
    if(!size)return;
    OS_CPU_SR sr=OS_CPU_SR_Save();
    size_t n=strlen(status_text);if(n>=size)n=size-1;
    memcpy(out,status_text,n);out[n]=0;OS_CPU_SR_Restore(sr);
}
void net_platform_init(void) {net_heap_init();}
static err_t transmit(struct netif *n,struct pbuf *p)
{
    (void)n;whd_buffer_t b;
    if(!interface || !netif_is_link_up(&wlan))return ERR_IF;
    if(board_buffers.whd_host_buffer_get(&b,WHD_NETWORK_TX,p->tot_len,0)!=WHD_SUCCESS)return ERR_MEM;
    pbuf_copy_partial(p,board_buffers.whd_buffer_get_current_piece_data_pointer(b),p->tot_len,0);
    /* WHD SDPCM consumes the buffer on both queue-full and success paths. */
    whd_result_t r=whd_network_send_ethernet_data(interface,b);
    if(r){net_error=r;return ERR_IF;}++net_tx_frames;return ERR_OK;
}
static void receive(whd_interface_t ifp,whd_buffer_t b)
{
    (void)ifp;uint16_t n=board_buffers.whd_buffer_get_current_piece_size(b);
    if(netif_ready && n>=14 && n<=1518) {
        struct pbuf *p=pbuf_alloc(PBUF_RAW,n,PBUF_POOL);
        if(p) {
            if(pbuf_take(p,board_buffers.whd_buffer_get_current_piece_data_pointer(b),n)!=ERR_OK ||
               tcpip_input(p,&wlan)!=ERR_OK)pbuf_free(p);
            else ++net_rx_frames;
        }
    }
    board_buffers.whd_buffer_release(b,WHD_NETWORK_RX);
}
static whd_netif_funcs_t net_functions={receive};
static err_t interface_init(struct netif *n)
{
    n->name[0]='w';n->name[1]='l';n->hostname="pi-zero-ucos2";
    n->hwaddr_len=6;memcpy(n->hwaddr,net_board_mac,6);n->mtu=1500;
    n->flags=NETIF_FLAG_BROADCAST|NETIF_FLAG_ETHARP|NETIF_FLAG_ETHERNET;
    n->output=etharp_output;n->linkoutput=transmit;return ERR_OK;
}
static void address_changed(struct netif *n)
{
    if(!ip4_addr_isany_val(*netif_ip4_addr(n))) {
        char line[80];snprintf(line,sizeof(line),"Wi-Fi: %s",ip4addr_ntoa(netif_ip4_addr(n)));
        status(line);net_stage=6;
        net_log("[IP] %s / UDP echo 5000 / TCP echo 5001\n",ip4addr_ntoa(netif_ip4_addr(n)));
    }
}
static void udp_receive(void *arg,struct udp_pcb *pcb,struct pbuf *p,const ip_addr_t *ip,u16_t port)
{
    (void)arg;
    if(p){if(udp_sendto(pcb,p,ip,port)==ERR_OK)++net_udp_echoes;pbuf_free(p);}
}
static err_t tcp_receive(void *arg,struct tcp_pcb *pcb,struct pbuf *p,err_t error)
{
    (void)arg;
    if(!p){if(tcp_close(pcb)!=ERR_OK){tcp_abort(pcb);return ERR_ABRT;}return ERR_OK;}
    if(error!=ERR_OK){pbuf_free(p);return error;}
    if(p->tot_len>tcp_sndbuf(pcb))return ERR_MEM;
    void *data=malloc(p->tot_len);if(!data)return ERR_MEM;
    pbuf_copy_partial(p,data,p->tot_len,0);
    err_t e=tcp_write(pcb,data,p->tot_len,TCP_WRITE_FLAG_COPY);free(data);
    if(e!=ERR_OK)return e;
    net_tcp_bytes+=p->tot_len;tcp_recved(pcb,p->tot_len);pbuf_free(p);tcp_output(pcb);return ERR_OK;
}
static err_t tcp_accept_client(void *arg,struct tcp_pcb *pcb,err_t error)
{(void)arg;if(error!=ERR_OK)return error;tcp_recv(pcb,tcp_receive);return ERR_OK;}
static void core_init_done(void *arg){(void)arg;OSSemPost(core_done);}
static void add_interface(void *arg)
{
    (void)arg;
    if(!netif_add(&wlan,NULL,NULL,NULL,NULL,interface_init,tcpip_input))bsp_panic("netif add");
    netif_set_default(&wlan);netif_set_status_callback(&wlan,address_changed);netif_set_up(&wlan);
    struct udp_pcb *u=udp_new();
    if(!u || udp_bind(u,IP_ADDR_ANY,5000)!=ERR_OK)bsp_panic("UDP bind");
    udp_recv(u,udp_receive,NULL);
    struct tcp_pcb *t=tcp_new();
    if(!t || tcp_bind(t,IP_ADDR_ANY,5001)!=ERR_OK)bsp_panic("TCP bind");
    t=tcp_listen_with_backlog(t,2);if(!t)bsp_panic("TCP listen");
    tcp_accept(t,tcp_accept_client);
    netif_ready=true;OSSemPost(core_done);
}
static void link_up(void *arg)
{
    (void)arg;netif_set_link_up(&wlan);
    if(dhcp_start(&wlan)!=ERR_OK){net_error=0xe001;status("Wi-Fi: DHCP allocation failed");}
    OSSemPost(core_done);
}
static void link_down(void *arg)
{
    (void)arg;dhcp_stop(&wlan);netif_set_link_down(&wlan);
    netif_set_addr(&wlan,IP4_ADDR_ANY4,IP4_ADDR_ANY4,IP4_ADDR_ANY4);OSSemPost(core_done);
}
static void core_call(tcpip_callback_fn fn)
{
    if(tcpip_callback(fn,NULL)!=ERR_OK)bsp_panic("tcpip callback queue");
    if(!net_sem_wait(core_done,UINT32_MAX))bsp_panic("tcpip completion");
}
static void stop(uint32_t error,const char *message)
{
    net_error=error;net_stage=0xff;status(message);
    net_log("[Wi-Fi] stopped: %s (0x%08lx); GUI and scheduler continue.\n",message,(unsigned long)error);
    for(;;)OSTimeDly(100);
}
#ifdef NET_TEST
#include "../tests/net_probe.inc"
#endif
void network_task(void *arg)
{
    (void)arg;while(!app_preempt_pass)OSTimeDly(1);
    core_done=OSSemCreate(0);if(!core_done)bsp_panic("network completion sem");
    tcpip_init(core_init_done,NULL);
    net_sem_wait(core_done,UINT32_MAX);
    net_stage=1;net_log("[NET] lwIP 2.2.1 core running in task 11.\n");
    #ifdef NET_TEST
    network_probe();
    #endif
    if(!wifi_sdio_init())stop(0xe100,"Wi-Fi: SDIO initialization failed");
    if(!board_resources_init())stop(0xe101,"Wi-Fi: NVRAM invalid");
    whd_init_config_t config={whd_stack,sizeof(whd_stack),8,
        (whd_country_code_t)MK_CNTRY(WIFI_COUNTRY_A,WIFI_COUNTRY_B,0)};
    whd_result_t r=whd_init(&driver,&config,&board_resources,&board_buffers,&net_functions);
    if(r)stop(r,"Wi-Fi: WHD initialization failed");
    whd_sdio_config_t bus={0};bus.oob_config.host_oob_pin=CYHAL_NC_PIN_VALUE;
    r=whd_bus_sdio_attach(driver,&bus,&host);if(r)stop(r,"Wi-Fi: SDIO attach failed");
    net_stage=2;status("Wi-Fi: loading radio firmware");
    r=whd_wifi_on(driver,&interface);if(r)stop(r,"Wi-Fi: radio firmware failed");
    whd_mac_t mac;r=whd_wifi_get_mac_address(interface,&mac);
    if(r)stop(r,"Wi-Fi: MAC query failed");
    memcpy(net_board_mac,mac.octet,6);
    net_log("[Wi-Fi] radio ready, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
        mac.octet[0],mac.octet[1],mac.octet[2],mac.octet[3],mac.octet[4],mac.octet[5]);
    core_call(add_interface);
    net_stage=3;status("Wi-Fi: scanning 2.4 GHz");
    uint32_t count=16;
    r=whd_wifi_scan_synch(interface,scan_results,&count);
    net_log("[SCAN] result=0x%08lx count=%lu\n",(unsigned long)r,(unsigned long)count);
    if(!r) for(uint32_t i=0;i<count && i<16;i++) {
        char name[33];unsigned n=scan_results[i].SSID.length;if(n>32)n=32;
        for(unsigned j=0;j<n;j++) {uint8_t c=scan_results[i].SSID.value[j];name[j]=(c>=32 && c<127)?(char)c:'?';}
        name[n]=0;
        net_log("[AP] ch=%u RSSI=%d SSID=%s\n",scan_results[i].channel,scan_results[i].signal_strength,name);
    }
    if(sizeof(ssid_text)==1) {
        status("Wi-Fi: scan done; configure SSID locally");
        net_log("[Wi-Fi] scan-only image. Run configure-wifi.ps1, rebuild and copy kernel.img.\n");
        for(;;)OSTimeDly(100);
    }
    if(sizeof(password)<9)stop(0xe102,"Wi-Fi: WPA2 password must be 8-63 bytes");
    whd_ssid_t ssid={0};ssid.length=sizeof(ssid_text)-1;memcpy(ssid.value,ssid_text,ssid.length);
    uint32_t delay=5;
    for(;;) {
        net_stage=4;status("Wi-Fi: joining WPA2 network");
        r=whd_wifi_join(interface,&ssid,WHD_SECURITY_WPA2_AES_PSK,(const uint8_t *)password,sizeof(password)-1);
        if(r) {
            net_error=r;net_log("[JOIN] failed 0x%08lx; retry in %lus\n",(unsigned long)r,(unsigned long)delay);
            status("Wi-Fi: join failed; retry pending");whd_wifi_leave(interface);
            OSTimeDly(delay*100);if(delay<30)delay+=5;++net_reconnects;continue;
        }
        delay=5;net_stage=5;status("Wi-Fi: associated; waiting for DHCP");core_call(link_up);
        net_log("[JOIN] associated; DHCP started.\n");
        do {OSTimeDly(100);}while(whd_wifi_is_ready_to_transceive(interface)==WHD_SUCCESS);
        core_call(link_down);whd_wifi_leave(interface);++net_reconnects;
        status("Wi-Fi: disconnected; reconnecting");net_log("[LINK] lost; reconnecting.\n");OSTimeDly(500);
    }
}
