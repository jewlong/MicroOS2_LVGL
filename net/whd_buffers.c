/* SPDX-License-Identifier: Apache-2.0 */
#include "wifi_port.h"
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t capacity,offset,length,pad; uint8_t bytes[]; } buffer_t;
static whd_result_t get(whd_buffer_t *out,whd_buffer_dir_t dir,uint16_t size,uint32_t ms)
{
    (void)dir;uint32_t start=net_now_ms();*out=NULL;
    do {
        buffer_t *b=calloc(1,sizeof(*b)+(uint32_t)size+192u);
        if(b){b->capacity=(uint32_t)size+192u;b->offset=128;b->length=size;*out=b;return WHD_SUCCESS;}
        if(!ms || (uint32_t)(net_now_ms()-start)>=ms)break;
        OSTimeDly(1);
    }while(1);
    return WHD_BUFFER_UNAVAILABLE_TEMPORARY;
}
static void release(whd_buffer_t p,whd_buffer_dir_t dir){(void)dir;free(p);}
static uint8_t *pointer(whd_buffer_t p){buffer_t *b=p;return b->bytes+b->offset;}
static uint16_t size(whd_buffer_t p){buffer_t *b=p;return (uint16_t)b->length;}
static whd_result_t resize(whd_buffer_t p,uint16_t n)
{
    buffer_t *b=p;if(n>b->capacity-b->offset)return WHD_BUFFER_SIZE_SET_ERROR;
    b->length=n;return WHD_SUCCESS;
}
static whd_result_t move(whd_buffer_t *p,int32_t n)
{
    buffer_t *b=*p;
    if(n>=0) {if((uint32_t)n>b->length)return WHD_BUFFER_POINTER_MOVE_ERROR;}
    else if(n==INT32_MIN || (uint32_t)(-n)>b->offset || b->length+(uint32_t)(-n)>65535)
        return WHD_BUFFER_POINTER_MOVE_ERROR;
    b->offset+=n;b->length-=n;return WHD_SUCCESS;
}
whd_buffer_funcs_t board_buffers={get,release,pointer,size,resize,move};
