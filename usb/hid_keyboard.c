/* SPDX-License-Identifier: Apache-2.0
 * USB HID 1.11 Appendix B boot keyboard, US layout. No report-descriptor parser.
 */
#include "keyboard.h"
#include <string.h>
static bool contains(const uint8_t *keys,uint8_t key)
{for(unsigned i=0;i<6;i++)if(keys[i]==key)return true;return false;}
static uint32_t translate(uint8_t u,uint8_t mods,const hid_keyboard_t *s)
{
    bool shift=(mods&0x22u)!=0;
    if(u>=4 && u<=29)return (shift!=s->caps?'A':'a')+u-4;
    if(u>=30 && u<=39)return (uint8_t)(shift?"!@#$%^&*()":"1234567890")[u-30];
    switch(u){
    case 40:case 88:return KEY_ENTER;
    case 41:return KEY_ESC;case 42:return KEY_BACKSPACE;
    case 43:return shift?KEY_BACKTAB:KEY_TAB;case 44:return ' ';
    case 45:return shift?'_':'-';case 46:return shift?'+':'=';
    case 47:return shift?'{':'[';case 48:return shift?'}':']';
    case 49:case 50:return shift?'|':'\\';case 51:return shift?':':';';
    case 52:return shift?'"':'\'';case 53:return shift?'~':'`';
    case 54:return shift?'<':',';case 55:return shift?'>':'.';case 56:return shift?'?':'/';
    case 74:return KEY_HOME;case 76:return KEY_DELETE;case 77:return KEY_END;
    case 79:return KEY_RIGHT;case 80:return KEY_LEFT;case 81:return KEY_DOWN;case 82:return KEY_UP;
    case 84:return '/';case 85:return '*';case 86:return '-';case 87:return '+';case 103:return '=';
    default:break;
    }
    if(s->num && u>=89 && u<=97)return '1'+u-89;
    if(s->num && u==98)return '0';
    if(s->num && u==99)return '.';
    if(!s->num)switch(u){case 89:return KEY_END;case 90:return KEY_DOWN;case 92:return KEY_LEFT;
      case 94:return KEY_RIGHT;case 95:return KEY_HOME;case 96:return KEY_UP;case 99:return KEY_DELETE;default:break;}
    return 0;
}
void hid_keyboard_init(hid_keyboard_t *s){memset(s,0,sizeof(*s));}
uint8_t hid_keyboard_leds(const hid_keyboard_t *s){return (s->caps?2u:0u)|(s->num?1u:0u);}
void hid_keyboard_release(hid_keyboard_t *s,hid_emit_t emit)
{
    for(unsigned i=0;i<6;i++)if(s->keys[i] && s->mapped[i]){
        key_event_t e={s->mapped[i],s->keys[i],0,0,0};emit(&e);
    }
    memset(s->keys,0,6);memset(s->mapped,0,sizeof(s->mapped));s->modifiers=0;
}
bool hid_keyboard_report(hid_keyboard_t *s,const uint8_t *r,size_t length,hid_emit_t emit)
{
    if(length<8)return false;
    for(unsigned i=2;i<8;i++)if(r[i]>=1 && r[i]<=3)return false; /* preserve held keys during rollover */
    uint8_t keys[6]={0};unsigned used=0;
    for(unsigned i=2;i<8;i++)if(r[i] && !contains(keys,r[i]))keys[used++]=r[i];
    for(unsigned i=0;i<6;i++)if(s->keys[i] && !contains(keys,s->keys[i]) && s->mapped[i]){
        key_event_t e={s->mapped[i],s->keys[i],r[0],0,0};emit(&e);
    }
    if(contains(keys,57) && !contains(s->keys,57))s->caps=!s->caps;
    if(contains(keys,83) && !contains(s->keys,83))s->num=!s->num;
    uint32_t mapped[6]={0};
    for(unsigned i=0;i<6;i++)if(keys[i]){
        if(contains(s->keys,keys[i])) {
            for(unsigned j=0;j<6;j++)if(s->keys[j]==keys[i])mapped[i]=s->mapped[j];
        } else {
            mapped[i]=translate(keys[i],r[0],s);
            if(mapped[i]){key_event_t e={mapped[i],keys[i],r[0],1,0};emit(&e);}
        }
    }
    memcpy(s->keys,keys,6);memcpy(s->mapped,mapped,sizeof(mapped));s->modifiers=r[0];return true;
}
