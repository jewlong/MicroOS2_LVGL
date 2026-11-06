/* SPDX-License-Identifier: Apache-2.0 */
#include "wifi_port.h"
#include "wifi_config.h"
#include <string.h>
#include <stdio.h>
extern const uint8_t wifi_fw_start[],wifi_fw_end[],wifi_clm_start[],wifi_clm_end[];
extern const uint8_t wifi_nv_start[],wifi_nv_end[];
static uint8_t nvram[4096];
static uint32_t nvram_size;
bool board_resources_init(void)
{
    const uint8_t *p=wifi_nv_start; size_t used=0; unsigned macs=0;
    while(p<wifi_nv_end) {
        char line[256];size_t n=0;
        while(p<wifi_nv_end && *p!='\n') {
            if(*p!='\r') { if(n+1>=sizeof(line))return false;line[n++]=(char)*p; } ++p;
        }
        if(p<wifi_nv_end)++p;
        line[n]=0;
        if(!n || line[0]=='#')continue;
        char *comment=strchr(line,'#');if(comment)*comment=0;
        n=strlen(line);while(n && (line[n-1]==' ' || line[n-1]=='\t'))line[--n]=0;
        if(!n)continue;
        if(strncmp(line,"macaddr=",8)==0 || strncmp(line,"il0macaddr=",11)==0) {
            const char *key=line[0]=='i'?"il0macaddr":"macaddr";++macs;
            snprintf(line,sizeof(line),"%s=%02x:%02x:%02x:%02x:%02x:%02x",key,
                net_board_mac[0],net_board_mac[1],net_board_mac[2],
                net_board_mac[3],net_board_mac[4],net_board_mac[5]);
        } else if(strncmp(line,"ccode=",6)==0) {
            snprintf(line,sizeof(line),"ccode=%c%c",WIFI_COUNTRY_A,WIFI_COUNTRY_B);
        } else if(strncmp(line,"regrev=",7)==0) {
            memcpy(line,"regrev=0",9);
        }
        n=strlen(line)+1;if(used+n+4>sizeof(nvram))return false;
        memcpy(nvram+used,line,n);used+=n;
    }
    if(!macs)return false;
    nvram[used++]=0;
    while(used%4)nvram[used++]=0;
    nvram_size=used;return true;
}
static uint32_t resource(whd_resource_type_t type,const uint8_t **p,uint32_t *n)
{
    switch(type) {
    case WHD_RESOURCE_WLAN_FIRMWARE:*p=wifi_fw_start;*n=wifi_fw_end-wifi_fw_start;break;
    case WHD_RESOURCE_WLAN_CLM:*p=wifi_clm_start;*n=wifi_clm_end-wifi_clm_start;break;
    case WHD_RESOURCE_WLAN_NVRAM:*p=nvram;*n=nvram_size;break;
    default:return WHD_BADARG;
    }
    return *n?WHD_SUCCESS:WHD_BADARG;
}
static uint32_t res_size(whd_driver_t d,whd_resource_type_t t,uint32_t *n)
{ const uint8_t *p;(void)d;return resource(t,&p,n); }
static uint32_t res_block(whd_driver_t d,whd_resource_type_t t,uint32_t block,const uint8_t **p,uint32_t *n)
{
    (void)d;uint32_t total,r=resource(t,p,&total);if(r)return r;
    uint32_t count=(total+1023)/1024;if(block>=count)return WHD_BADARG;
    *p+=block*1024;*n=total-block*1024;if(*n>1024)*n=1024;return WHD_SUCCESS;
}
static uint32_t res_count(whd_driver_t d,whd_resource_type_t t,uint32_t *n)
{ uint32_t r=res_size(d,t,n);if(!r)*n=(*n+1023)/1024;return r; }
static uint32_t res_block_size(whd_driver_t d,whd_resource_type_t t,uint32_t *n)
{ uint32_t r=res_size(d,t,n);if(!r)*n=1024;return r; }
static uint32_t res_read(whd_driver_t d,whd_resource_type_t t,uint32_t off,uint32_t size,uint32_t *n,void *buf)
{
    (void)d;const uint8_t *p;uint32_t total,r=resource(t,&p,&total);
    if(r || off>total || size>total-off){*n=0;return WHD_BADARG;}
    memcpy(buf,p+off,size);*n=size;return WHD_SUCCESS;
}
whd_resource_source_t board_resources={res_size,res_block,res_count,res_block_size,res_read};
