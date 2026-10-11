#include "pe_image.h"
#include <stdio.h>
#include <string.h>
static void w16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void w32(uint8_t *p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static void w64(uint8_t *p,uint64_t v){w32(p,(uint32_t)v);w32(p+4,(uint32_t)(v>>32));}
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
    fixture(b,sizeof b,0);
    w32(b+0x98+104,0x1010);w32(b+0x98+108,40);
    w32(b+0x210,0x1050);w32(b+0x21c,0x1040);w32(b+0x220,0x1060);
    memcpy(b+0x240,"KERNEL32.dll",13);
    w32(b+0x250,0x1070);w16(b+0x270,7);memcpy(b+0x272,"ExitProcess",12);
    if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_OK)return 13;
    {
        struct wb_pe_import imp;struct wb_pe_import_symbol sym;char dll[32],symbol_name[32];
        if(wb_pe_import_at(b,sizeof b,&pe,0,&imp)!=WB_PE_OK||imp.name_rva!=0x1040)return 14;
        pe.pe32_plus=2u;
        if(wb_pe_import_symbol_at(b,sizeof b,&pe,&imp,0,&sym,symbol_name,sizeof symbol_name)!=WB_PE_EINVAL)return 15;
        pe.pe32_plus=0u;
        memset(dll,'X',sizeof dll);
        if(wb_pe_read_ascii_string(b,sizeof b,&pe,imp.name_rva,dll,sizeof dll)!=WB_PE_OK)return 15;
        if(strcmp(dll,"KERNEL32.dll")!=0)return 16;
        if(wb_pe_import_symbol_at(b,sizeof b,&pe,&imp,0,&sym,symbol_name,sizeof symbol_name)!=WB_PE_OK)return 17;
        if(sym.is_ordinal||sym.hint!=7||strcmp(symbol_name,"ExitProcess")!=0)return 18;
        w32(b+0x250,0x80000005u);
        if(wb_pe_import_symbol_at(b,sizeof b,&pe,&imp,0,&sym,symbol_name,sizeof symbol_name)!=WB_PE_OK||!sym.is_ordinal||sym.ordinal!=5)return 19;
        w32(b+0x250,0x1070);
        if(wb_pe_import_symbol_at(b,sizeof b,&pe,&imp,1,&sym,symbol_name,sizeof symbol_name)!=WB_PE_ENOTFOUND)return 20;
        if(wb_pe_import_at(b,sizeof b,&pe,1,&imp)!=WB_PE_ENOTFOUND)return 21;
        w32(b+0x21c,0);memset(&imp,0xff,sizeof imp);
        if(wb_pe_import_at(b,sizeof b,&pe,0,&imp)!=WB_PE_EFORMAT||imp.name_rva!=0)return 22;
        w32(b+0x21c,0x1040);
        b[0x2ff]='A';
        if(wb_pe_read_ascii_string(b,sizeof b,&pe,0x10ff,dll,sizeof dll)!=WB_PE_EBOUNDS||dll[0]!='\0')return 23;
    }
    /* Exercise 64-bit import thunks separately from the PE32 fixture. */
    fixture(b,sizeof b,1);
    w32(b+0x98+120,0x1010);w32(b+0x98+124,40);
    w32(b+0x210,0x1050);w32(b+0x21c,0x1040);w32(b+0x220,0x1060);
    memcpy(b+0x240,"KERNEL32.dll",13);
    w64(b+0x250,0x1070);w16(b+0x270,9);memcpy(b+0x272,"GetTickCount",13);
    if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_OK)return 24;
    {
        struct wb_pe_import imp;struct wb_pe_import_symbol sym;char name[32];
        if(wb_pe_import_at(b,sizeof b,&pe,0,&imp)!=WB_PE_OK)return 26;
        if(wb_pe_import_symbol_at(b,sizeof b,&pe,&imp,0,&sym,name,sizeof name)!=WB_PE_OK)return 27;
        if(sym.is_ordinal||sym.hint!=9||strcmp(name,"GetTickCount")!=0)return 28;
        w64(b+0x250,0x8000000000000005ULL);
        if(wb_pe_import_symbol_at(b,sizeof b,&pe,&imp,0,&sym,name,sizeof name)!=WB_PE_OK||
           !sym.is_ordinal||sym.ordinal!=5)return 29;
        w64(b+0x250,0x8000000100000005ULL);
        if(wb_pe_import_symbol_at(b,sizeof b,&pe,&imp,0,&sym,name,sizeof name)!=WB_PE_EFORMAT)return 30;
    }
    fixture(b,sizeof b,0);if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_OK)return 24;
    pe.section_count=WB_PE_MAX_SECTIONS+1u;off=77;
    if(wb_pe_rva_to_file(b,sizeof b,&pe,0x1000,1,&off)!=WB_PE_EINVAL||off!=77)return 25;
    puts("WinBridge PE parser host tests: PASS");return 0;
}
