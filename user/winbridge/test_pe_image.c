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
        w32(b+0x258,0x1070); /* forged thunk after the null terminator */
        if(wb_pe_import_symbol_at(b,sizeof b,&pe,&imp,2,&sym,symbol_name,sizeof symbol_name)!=WB_PE_ENOTFOUND)return 47;
        w32(b+0x258,0);
        if(wb_pe_import_symbol_at(b,sizeof b,&pe,&imp,0xffffffffu,&sym,symbol_name,sizeof symbol_name)!=WB_PE_EBOUNDS)return 48;
        /* A thunk prefix may not extend beyond the section's raw-backed bytes. */
        imp.original_first_thunk=0x10fcu;
        memset(&sym,0xff,sizeof sym);memset(symbol_name,'X',sizeof symbol_name);
        if(wb_pe_import_symbol_at(b,sizeof b,&pe,&imp,1,&sym,symbol_name,sizeof symbol_name)!=WB_PE_EBOUNDS ||
           sym.hint!=0u || sym.ordinal!=0u || symbol_name[0]!='\0')return 50;
        imp.original_first_thunk=0x1050u;
        /* Forged image_size must not truncate the prefix byte count before mapping. */
        pe.image_size=0xffffffffu;
        if(wb_pe_import_symbol_at(b,sizeof b,&pe,&imp,0x02000000u,&sym,symbol_name,sizeof symbol_name)!=WB_PE_EBOUNDS)return 49;
        pe.image_size=0x2000u;
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
    /* Reject inconsistent import-directory metadata and bounded strings. */
    fixture(b,sizeof b,0);
    w32(b+0x98+104,0x1010); /* RVA without a size must fail inspection. */
    if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_EFORMAT)return 31;
    fixture(b,sizeof b,0);
    w32(b+0x98+108,40); /* Size without an RVA must fail inspection. */
    if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_EFORMAT)return 32;
    fixture(b,sizeof b,0);
    w32(b+0x178+8,0x100);  /* virtual section extends beyond file-backed data */
    w32(b+0x178+16,0x10);
    w32(b+0x98+104,0x1020);
    w32(b+0x98+108,20);    /* import directory points into zero-fill tail */
    if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_EBOUNDS)return 40;
    fixture(b,sizeof b,0);
    if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_OK)return 33;
    {
        char tiny[4]={'X','X','X','X'};
        memcpy(b+0x200,"ABCD",4);
        if(wb_pe_read_ascii_string(b,sizeof b,&pe,0x1000,tiny,sizeof tiny)!=WB_PE_EBOUNDS ||
           tiny[0]!='\0')return 34;
        pe.import_rva=0u;pe.import_size=20u;
        {
            struct wb_pe_import imp;
            memset(&imp,0xff,sizeof imp);
            if(wb_pe_import_at(b,sizeof b,&pe,0,&imp)!=WB_PE_EFORMAT ||
               imp.original_first_thunk!=0u || imp.name_rva!=0u)return 35;
        }
    }
    fixture(b,sizeof b,0);if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_OK)return 24;
    /* Caller-supplied section metadata must be validated as a whole. */
    pe.sections[0].raw_offset=(uint32_t)sizeof b-1u;
    pe.sections[0].raw_size=4u;off=77;
    if(wb_pe_rva_to_file(b,sizeof b,&pe,0x1000,1,&off)!=WB_PE_EBOUNDS||off!=77)return 36;
    fixture(b,sizeof b,0);if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_OK)return 37;
    pe.sections[0].virtual_address=0x1fffu;
    pe.sections[0].virtual_size=2u;off=77;
    if(wb_pe_rva_to_file(b,sizeof b,&pe,0x1000,1,&off)!=WB_PE_EBOUNDS||off!=77)return 38;
    fixture(b,sizeof b,0);if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_OK)return 39;
    pe.section_count=WB_PE_MAX_SECTIONS+1u;off=77;
    if(wb_pe_rva_to_file(b,sizeof b,&pe,0x1000,1,&off)!=WB_PE_EINVAL||off!=77)return 25;
    /* Public import reader must reject forged directory ranges and clear output. */
    fixture(b,sizeof b,0);if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_OK)return 41;
    pe.import_rva=0x1ff0u;pe.import_size=40u;
    {
        struct wb_pe_import imp;
        memset(&imp,0xff,sizeof imp);
        if(wb_pe_import_at(b,sizeof b,&pe,0,&imp)!=WB_PE_EBOUNDS ||
           imp.original_first_thunk!=0u || imp.name_rva!=0u)return 42;
    }
    fixture(b,sizeof b,0);if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_OK)return 43;
    pe.import_rva=0x10f0u;pe.import_size=40u;
    {
        struct wb_pe_import imp;
        memset(&imp,0xff,sizeof imp);
        if(wb_pe_import_at(b,sizeof b,&pe,0,&imp)!=WB_PE_EBOUNDS ||
           imp.original_first_thunk!=0u || imp.name_rva!=0u)return 44;
    }
    /* A later nonzero descriptor must not be reachable past the null terminator. */
    fixture(b,sizeof b,0);if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_OK)return 45;
    pe.import_rva=0x1010u;pe.import_size=40u;
    w32(b+0x230,0x1040);w32(b+0x234,0x1060);
    {
        struct wb_pe_import imp;
        if(wb_pe_import_at(b,sizeof b,&pe,1,&imp)!=WB_PE_ENOTFOUND)return 46;
    }
    /* A nonzero descriptor table with no all-zero terminator is malformed,
     * even when the requested first descriptor itself looks valid. */
    fixture(b,sizeof b,0);if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_OK)return 51;
    pe.import_rva=0x1010u;pe.import_size=20u;
    w32(b+0x210,0x1050);w32(b+0x21c,0x1040);w32(b+0x220,0x1060);
    {
        struct wb_pe_import imp;
        memset(&imp,0xff,sizeof imp);
        if(wb_pe_import_at(b,sizeof b,&pe,0,&imp)!=WB_PE_EFORMAT ||
           imp.original_first_thunk!=0u || imp.name_rva!=0u)return 52;
    }
    fixture(b,sizeof b,0);w32(b+0x98+104,0x1010);w32(b+0x98+108,20u*4097u);
    if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_ENOTSUP)return 53;
    fixture(b,sizeof b,0);if(wb_pe_inspect(b,sizeof b,&pe)!=WB_PE_OK)return 54;
    pe.import_rva=0x1010u;pe.import_size=20u*4097u;
    { struct wb_pe_import imp;memset(&imp,0xff,sizeof imp);
      if(wb_pe_import_at(b,sizeof b,&pe,0,&imp)!=WB_PE_ENOTSUP || imp.name_rva!=0u)return 55; }
    puts("WinBridge PE parser host tests: PASS");return 0;
}
