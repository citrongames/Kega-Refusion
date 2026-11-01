/* Read-only SH2/32X BIOS probe. This is not full MD/CD game emulation. */
#include "core32x/machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
static uint8_t *rom;
static size_t rom_size;
static uint8_t *read_file(const char *name,size_t expected,size_t *length)
{
    FILE *f=fopen(name,"rb");long n;uint8_t *p;
    if(!f)return NULL;
    if(fseek(f,0,SEEK_END) || (n=ftell(f))<0 || n>8*1024*1024 || (expected && n!=(long)expected)) {fclose(f);return NULL;}
    rewind(f);p=malloc((size_t)n);
    if(!p || fread(p,1,(size_t)n,f)!=(size_t)n) {free(p);fclose(f);return NULL;}
    fclose(f);*length=(size_t)n;return p;
}
static uint16_t cart_read(void *p,uint32_t a)
{(void)p;return a+1<rom_size?(uint16_t)((rom[a]<<8)|rom[a+1]):0xffff;}
int main(int argc,char **argv)
{
    uint8_t *mb,*sb;size_t ml,sl;Kega32xMachine *m;Kega32xMachineHost h={0};
    KegaSh2State s[2];Kega32xBus *b;uint64_t t;
    if(argc!=4){fprintf(stderr,"usage: device_probe master.bin slave.bin cartridge.32x\n");return 2;}
    mb=read_file(argv[1],2048,&ml);sb=read_file(argv[2],1024,&sl);rom=read_file(argv[3],0,&rom_size);
    if(!mb || !sb || !rom){free(mb);free(sb);free(rom);return 2;}
    h.rom_read16=cart_read;m=kega32x_machine_create(&h,mb,ml,sb,sl,0,0);
    free(mb);free(sb);if(!m){free(rom);return 2;}
    b=kega32x_machine_bus(m);kega32x_bus_write16(b,2,0,3,0);
    for(t=3420;t<64000000;t+=3420) {
        kega32x_machine_run_until(m,t);
        if(b->s.regs[16]==0x4d5f && b->s.regs[17]==0x4f4b && b->s.regs[18]==0x535f && b->s.regs[19]==0x4f4b)break;
    }
    kega_sh2_get_state(kega32x_machine_cpu(m,0),&s[0]);kega_sh2_get_state(kega32x_machine_cpu(m,1),&s[1]);
    printf("mclk=%llu PC=%08x/%08x COMM=%04x %04x %04x %04x SR=%04x/%04x\n",
        (unsigned long long)t,s[0].pc,s[1].pc,b->s.regs[16],b->s.regs[17],b->s.regs[18],b->s.regs[19],s[0].sr,s[1].sr);
    printf("R8/R9=%08x/%08x, cycles=%llu/%llu\n",s[0].r[8],s[0].r[9],(unsigned long long)s[0].cycles,(unsigned long long)s[1].cycles);
    kega32x_machine_destroy(m);free(rom);
    return t<64000000?0:1;
}
