/* Test-only synthetic Ethernet boundary; never linked into kernel.img.
 * Real lwIP, µC/OS-II, HAL and buffer/resource adapters are retained.
 */
#include "lwip/sys.h"
#include "cyabs_rtos.h"
volatile uint32_t test_mode,test_phase,test_tx_seq,test_tx_len,test_rx_len,test_rx_pending;
volatile uint32_t test_assertions;
uint8_t test_tx_data[1600],test_rx_data[1600];
static void check(bool ok)
{if(!ok)bsp_panic("network probe assertion");++test_assertions;}
static err_t test_transmit(struct netif *n,struct pbuf *p)
{
    (void)n;check(p->tot_len<=sizeof(test_tx_data));
    test_tx_len=pbuf_copy_partial(p,test_tx_data,p->tot_len,0);
    ++test_tx_seq;++net_tx_frames;return ERR_OK;
}
static void probe_core(void *arg)
{
    (void)arg;wlan.linkoutput=test_transmit;netif_set_link_up(&wlan);
    check(dhcp_start(&wlan)==ERR_OK);OSSemPost(core_done);
}
static void primitive_tests(void)
{
    sys_mbox_t m;void *p=(void *)123;sys_sem_t s;sys_mutex_t lock;
    check(sys_mbox_new(&m,2)==ERR_OK);
    check(sys_mbox_trypost(&m,NULL)==ERR_OK);
    check(sys_mbox_trypost(&m,(void *)77)==ERR_OK);
    check(sys_mbox_trypost(&m,(void *)88)==ERR_MEM);
    check(sys_arch_mbox_tryfetch(&m,&p)==0 && p==NULL);
    check(sys_arch_mbox_fetch(&m,&p,10)!=SYS_ARCH_TIMEOUT && p==(void *)77);
    check(sys_arch_mbox_tryfetch(&m,&p)==SYS_MBOX_EMPTY);
    uint32_t before=sys_now();
    check(sys_arch_mbox_fetch(&m,&p,25)==SYS_ARCH_TIMEOUT);
    check((uint32_t)(sys_now()-before)>=30);sys_mbox_free(&m);
    check(sys_sem_new(&s,0)==ERR_OK);
    before=sys_now();check(sys_arch_sem_wait(&s,25)==SYS_ARCH_TIMEOUT);
    check((uint32_t)(sys_now()-before)>=30);
    sys_sem_signal(&s);check(sys_arch_sem_wait(&s,1)!=SYS_ARCH_TIMEOUT);sys_sem_free(&s);
    check(sys_mutex_new(&lock)==ERR_OK);sys_mutex_lock(&lock);sys_mutex_unlock(&lock);sys_mutex_free(&lock);
    cy_semaphore_t cs=0;
    check(cy_rtos_init_semaphore(&cs,1,0)==CY_RSLT_SUCCESS);
    check(cy_rtos_set_semaphore(&cs,false)==CY_RSLT_SUCCESS);
    check(cy_rtos_set_semaphore(&cs,false)==CY_RSLT_SUCCESS);
    check(cy_rtos_get_semaphore(&cs,0,false)==CY_RSLT_SUCCESS);
    check(cy_rtos_get_semaphore(&cs,0,false)==CY_RTOS_TIMEOUT);
    check(cy_rtos_deinit_semaphore(&cs)==CY_RSLT_SUCCESS);
    whd_buffer_t b;check(board_buffers.whd_host_buffer_get(&b,WHD_NETWORK_TX,1514,0)==WHD_SUCCESS);
    uint8_t *original=board_buffers.whd_buffer_get_current_piece_data_pointer(b);
    check(board_buffers.whd_buffer_add_remove_at_front(&b,-32)==WHD_SUCCESS);
    check(board_buffers.whd_buffer_get_current_piece_size(b)==1546);
    check(board_buffers.whd_buffer_add_remove_at_front(&b,32)==WHD_SUCCESS);
    check(board_buffers.whd_buffer_get_current_piece_data_pointer(b)==original);
    check(board_buffers.whd_buffer_add_remove_at_front(&b,1515)!=WHD_SUCCESS);
    check(board_buffers.whd_buffer_set_size(b,65535)!=WHD_SUCCESS);
    board_buffers.whd_buffer_release(b,WHD_NETWORK_TX);
    check(board_resources_init());
    uint32_t size;const uint8_t *res;
    check(board_resources.whd_resource_size(NULL,WHD_RESOURCE_WLAN_NVRAM,&size)==WHD_SUCCESS && size%4==0);
    check(board_resources.whd_get_resource_block(NULL,WHD_RESOURCE_WLAN_NVRAM,0,&res,&size)==WHD_SUCCESS);
    check(board_resources.whd_get_resource_block(NULL,WHD_RESOURCE_WLAN_NVRAM,999,&res,&size)!=WHD_SUCCESS);
}
static void probe_sdio(void)
{
    check(wifi_sdio_init());
    uint32_t response;cyhal_sdio_t dev={0};
    check(cyhal_sdio_send_cmd(&dev,CYHAL_READ,52,0,&response)==CY_RSLT_SUCCESS && response==0x12);
    check(cyhal_sdio_send_cmd(&dev,CYHAL_READ,52,2u<<9,&response)!=CY_RSLT_SUCCESS);
    _Alignas(4) uint8_t data[132];memset(data,0xcc,sizeof(data));
    check(cyhal_sdio_bulk_transfer(&dev,CYHAL_READ,(1u<<28)|7u,(uint32_t *)(void *)(data+4),7,&response)==CY_RSLT_SUCCESS);
    for(unsigned i=0;i<7;i++)check(data[4+i]==(uint8_t)i);
    check(data[3]==0xcc && data[11]==0xcc);
    memset(data,0xcc,sizeof(data));
    check(cyhal_sdio_bulk_transfer(&dev,CYHAL_READ,(1u<<28)|(1u<<27)|2u,(uint32_t *)(void *)(data+4),70,&response)==CY_RSLT_SUCCESS);
    for(unsigned i=0;i<70;i++)check(data[4+i]==(uint8_t)i);
    check(data[3]==0xcc && data[74]==0xcc);
    for(unsigned i=0;i<70;i++)data[4+i]=(uint8_t)i;
    check(cyhal_sdio_bulk_transfer(&dev,CYHAL_WRITE,(1u<<31)|(1u<<28)|(1u<<27)|2u,(uint32_t *)(void *)(data+4),70,&response)==CY_RSLT_SUCCESS);
    check(cyhal_sdio_bulk_transfer(&dev,CYHAL_WRITE,7,(uint32_t *)(void *)data,7,&response)!=CY_RSLT_SUCCESS);
    check(cyhal_sdio_bulk_transfer(&dev,CYHAL_READ,7,(uint32_t *)(void *)data,6,&response)!=CY_RSLT_SUCCESS);
    check(net_sdio_errors==1);
    test_phase=3;net_log("[TEST] SDIO command/error/PIO/padding guards passed.\n");
    for(;;)OSTimeDly(100);
}
static void network_probe(void)
{
    const uint8_t mac[6]={2,0x52,0x12,0x34,0x56,0x78};
    memcpy(net_board_mac,mac,6);
    primitive_tests();test_phase=1;OSTimeDly(5);
    if(test_mode==1)probe_sdio();
    core_call(add_interface);core_call(probe_core);test_phase=2;
    net_log("[TEST] RTOS/buffer/resources passed; synthetic Ethernet ready.\n");
    for(;;) {
        if(test_rx_pending) {
            uint32_t n=test_rx_len;check(n>=14 && n<=1518);
            whd_buffer_t b;
            check(board_buffers.whd_host_buffer_get(&b,WHD_NETWORK_RX,n,0)==WHD_SUCCESS);
            memcpy(board_buffers.whd_buffer_get_current_piece_data_pointer(b),test_rx_data,n);
            receive(NULL,b);test_rx_pending=0;
        }
        OSTimeDly(1);
    }
}
