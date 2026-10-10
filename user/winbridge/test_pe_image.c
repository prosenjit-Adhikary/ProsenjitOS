#include "pe_image.h"
#include <stdio.h>
#include <string.h>
static void w16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void w32(uint8_t *p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static void fixture(uint8_t *b,size_t n,int plus){
    memset(b,0,n); b[0]='M';b[1]='Z';w32(b+0x3c,0x80);b[0x80]='P';b[0x81]='E';
    w16(b+0x84,plus?WB_PE_MACHINE_AMD64:WB_PE_MACHINE_I386);w16(b+0x86,1);
    w16(b+0x94,plus?0xf0:0xe0);size_t o=0x98;w16(b+o,plus?0x20b:0x10b);
    w32(b+o+16,0x1000);w32(b+o+56,0x2000);w32(b+o+60,0x200);w16(b+o+68,3);
    if(plus){w32(b+o+108,16);}else{w32(b+o+92,16);}
    size_t s=o+(plus?0xf0:0xe0);memcpy(b+s,".text",5);w32(b+s+8,0x100);w32(b+s+12,0x1000);w32(b+s+16,0x100);w32(b+s+20,0x200);w32(b+s+36,0x60000000);
    b[0x200]=0xc3;
}
int main(void){
    uint8_t b[1024];struct wb_pe_image pe;size_t off=0;
    fixture(b,sizeof b,0);if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_OK||pe.pe32_plus)return 1;
    if(wb_pe_rva_to_file(b,sizeof b,&pe,0x1000,1,&off)!=WB_PE_OK||off!=0x200)return 2;
    if(wb_pe_rva_to_file(b,sizeof b,&pe,0x10ff,2,&off)!=WB_PE_EBOUNDS)return 3;
    fixture(b,sizeof b,1);if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_OK||!pe.pe32_plus)return 4;
    w16(b+0x84,WB_PE_MACHINE_I386);if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_ENOTSUP)return 5;
    fixture(b,sizeof b,0);w32(b+0x3c,0xfffffff0u);if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_EBOUNDS)return 6;
    fixture(b,sizeof b,0);w32(b+0x98+56,0x1000);if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_EFORMAT)return 7;
    fixture(b,sizeof b,0);w32(b+0x98+16,0x1800);if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_EFORMAT)return 8;
    fixture(b,sizeof b,0);w32(b+0x98+92,0xffffffffu);if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_EFORMAT)return 9;
    fixture(b,sizeof b,0);w32(b+0x178+36,0xa0000000u);if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_ECONFLICT)return 10;
    fixture(b,sizeof b,0);w32(b+0x178+8,0x100);w32(b+0x178+16,0x10);w32(b+0x98+16,0x1020);if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_EFORMAT)return 11;
    fixture(b,sizeof b,0);b[0]='X';memset(&pe,0xff,sizeof pe);if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_EFORMAT||pe.machine!=0)return 12;
    fixture(b,sizeof b,0);if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_OK)return 13;
    pe.section_count=WB_PE_MAX_SECTIONS+1u;off=77;
    if(wb_pe_rva_to_file(b,sizeof b,&pe,0x1000,1,&off)!=WB_PE_EINVAL||off!=77)return 14;
    puts("WinBridge PE parser host tests: PASS");return 0;
}
