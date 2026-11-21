#include "coredebug/disassemble.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

static kr_debug_instruction decode(unsigned opcode,uint32_t address) {
    kr_debug_instruction out;
    memset(&out,0xa5,sizeof(out));
    out.address=address;out.bytes[0]=opcode>>8;out.bytes[1]=opcode;
    assert(kr_debug_disassemble_sh2(&out,2)==2 && out.length==2);
    assert(memchr(out.text,0,sizeof(out.text)));
    return out;
}
int main(void) {
    for(unsigned op=0;op<65536;op++) {
        decode(op,0);
        decode(op,0xfffffffeu);
    }
    assert(decode(0xb001,0).flags==(KR_DEBUG_CALL|KR_DEBUG_DELAYED));
    assert(decode(0x400b,0).flags==(KR_DEBUG_CALL|KR_DEBUG_DELAYED));
    assert(decode(0x0003,0).flags==(KR_DEBUG_CALL|KR_DEBUG_DELAYED));
    assert(decode(0xc301,0).flags==KR_DEBUG_CALL);
    assert(decode(0x000b,0).flags==(KR_DEBUG_RETURN|KR_DEBUG_DELAYED));
    assert(decode(0x002b,0).flags==(KR_DEBUG_RETURN|KR_DEBUG_DELAYED));
    assert(decode(0xa001,0).flags==KR_DEBUG_DELAYED);
    assert(decode(0x8901,0).flags==0);
    assert(decode(0x8d01,0).flags==KR_DEBUG_DELAYED);
    kr_debug_instruction shortInput={0};
    assert(!kr_debug_disassemble_sh2(&shortInput,1));
    return 0;
}
