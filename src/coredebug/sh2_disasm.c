/* SH-2 decoder adapted from PicoDrive 07abb00550528f7bf53087d932dd3d747cf2d0df.
 * Non-commercial donor terms: third_party/picodrive32x/COPYING.
 * Bounded output and control-flow metadata for Kega Refusion. */
#include <stdio.h>
#include "disassemble.h"

typedef int INT32;
typedef unsigned int   UINT32;
typedef unsigned short UINT16;
typedef unsigned char  UINT8;

#define DASMFLAG_STEP_OUT (KR_DEBUG_RETURN | KR_DEBUG_DELAYED)
#define DASMFLAG_STEP_OVER (KR_DEBUG_CALL | KR_DEBUG_DELAYED)
#define DASMFLAG_STEP_OVER_EXTRA(x) 0

#define SIGNX8(x) ((INT32)((x)&0x7f) - (INT32)((x)&0x80))
#define SIGNX12(x) ((INT32)((x)&0x7ff) - (INT32)((x)&0x800))

#define Rn ((opcode >> 8) & 15)
#define Rm ((opcode >> 4) & 15)

static const char *const regname[16] = {
	"R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
	"R8", "R9", "R10","R11","R12","R13","R14","SP"
};

static UINT32 op0000(char *buffer, UINT32 pc, UINT16 opcode)
{
	UINT32  flags = 0;
	switch(opcode & 0x3f)
	{
	case 0x02:
		snprintf(buffer,160,"STC     SR,%s", regname[Rn]);
		break;
	case 0x03:
		snprintf(buffer,160,"BSRF    %s", regname[Rn]);
        flags = DASMFLAG_STEP_OVER;
		break;
	case 0x08:
		snprintf(buffer,160,"CLRT");
		break;
	case 0x09:
		snprintf(buffer,160,"NOP");
		break;
	case 0x0A:
		snprintf(buffer,160,"STS     MACH,%s", regname[Rn]);
		break;
	case 0x0B:
		snprintf(buffer,160,"RTS");
		flags = DASMFLAG_STEP_OUT;
		break;
	case 0x12:
		snprintf(buffer,160,"STC     GBR,%s", regname[Rn]);
		break;
	case 0x18:
		snprintf(buffer,160,"SETT");
		break;
	case 0x19:
		snprintf(buffer,160,"DIV0U");
		break;
	case 0x1A:
		snprintf(buffer,160,"STS     MACL,%s", regname[Rn]);
		break;
	case 0x1B:
		snprintf(buffer,160,"SLEEP");
		break;
	case 0x22:
		snprintf(buffer,160,"STC     VBR,%s", regname[Rn]);
		break;
	case 0x23:
		snprintf(buffer,160,"BRAF    %s", regname[Rn]);
		break;
	case 0x28:
		snprintf(buffer,160,"CLRMAC");
		break;
	case 0x29:
		snprintf(buffer,160,"MOVT    %s", regname[Rn]);
		break;
	case 0x2A:
		snprintf(buffer,160,"STS     PR,%s", regname[Rn]);
		break;
	case 0x2B:
		snprintf(buffer,160,"RTE");
		flags = DASMFLAG_STEP_OUT;
		break;
	default:
		switch(opcode & 15)
		{
		case  0:
			snprintf(buffer,160, "??????  $%04X", opcode);
			break;
		case  1:
			snprintf(buffer,160, "??????  $%04X", opcode);
			break;
		case  2:
			snprintf(buffer,160, "??????  $%04X", opcode);
			break;
		case  3:
			snprintf(buffer,160, "??????  $%04X", opcode);
			break;
		case  4:
			snprintf(buffer,160, "MOV.B   %s,@(R0,%s)", regname[Rm], regname[Rn]);
			break;
		case  5:
			snprintf(buffer,160, "MOV.W   %s,@(R0,%s)", regname[Rm], regname[Rn]);
			break;
		case  6:
			snprintf(buffer,160, "MOV.L   %s,@(R0,%s)", regname[Rm], regname[Rn]);
			break;
		case  7:
			snprintf(buffer,160, "MUL.L   %s,%s", regname[Rm], regname[Rn]);
			break;
		case  8:
			snprintf(buffer,160, "??????  $%04X", opcode);
			break;
		case  9:
			snprintf(buffer,160, "??????  $%04X", opcode);
			break;
		case 10:
			snprintf(buffer,160, "??????  $%04X", opcode);
			break;
		case 11:
			snprintf(buffer,160, "??????  $%04X", opcode);
			break;
		case 12:
			snprintf(buffer,160, "MOV.B   @(R0,%s),%s", regname[Rm], regname[Rn]);
			break;
		case 13:
			snprintf(buffer,160, "MOV.W   @(R0,%s),%s", regname[Rm], regname[Rn]);
			break;
		case 14:
			snprintf(buffer,160, "MOV.L   @(R0,%s),%s", regname[Rm], regname[Rn]);
			break;
		case 15:
			snprintf(buffer,160, "MAC.L   @%s+,@%s+", regname[Rn], regname[Rm]);
			break;
		}
	}
	return flags;
}

static UINT32 op0001(char *buffer, UINT32 pc, UINT16 opcode)
{
	snprintf(buffer,160, "MOV.L   %s,@($%02X,%s)", regname[Rm], (opcode & 15) * 4, regname[Rn]);
	return 0;
}

static UINT32 op0010(char *buffer, UINT32 pc, UINT16 opcode)
{
	switch (opcode & 15)
	{
	case  0:
		snprintf(buffer,160, "MOV.B   %s,@%s", regname[Rm], regname[Rn]);
		break;
	case  1:
		snprintf(buffer,160, "MOV.W   %s,@%s", regname[Rm], regname[Rn]);
		break;
	case  2:
		snprintf(buffer,160, "MOV.L   %s,@%s", regname[Rm], regname[Rn]);
		break;
	case  3:
		snprintf(buffer,160, "??????  $%04X", opcode);
		break;
	case  4:
		snprintf(buffer,160, "MOV.B   %s,@-%s", regname[Rm], regname[Rn]);
		break;
	case  5:
		snprintf(buffer,160, "MOV.W   %s,@-%s", regname[Rm], regname[Rn]);
		break;
	case  6:
		snprintf(buffer,160, "MOV.L   %s,@-%s", regname[Rm], regname[Rn]);
		break;
	case  7:
		snprintf(buffer,160, "DIV0S   %s,%s", regname[Rm], regname[Rn]);
		break;
	case  8:
		snprintf(buffer,160, "TST     %s,%s", regname[Rm], regname[Rn]);
		break;
	case  9:
		snprintf(buffer,160, "AND     %s,%s", regname[Rm], regname[Rn]);
		break;
	case 10:
		snprintf(buffer,160, "XOR     %s,%s", regname[Rm], regname[Rn]);
		break;
	case 11:
		snprintf(buffer,160, "OR      %s,%s", regname[Rm], regname[Rn]);
		break;
	case 12:
		snprintf(buffer,160, "CMP/STR %s,%s", regname[Rm], regname[Rn]);
		break;
	case 13:
		snprintf(buffer,160, "XTRCT   %s,%s", regname[Rm], regname[Rn]);
		break;
	case 14:
		snprintf(buffer,160, "MULU.W  %s,%s", regname[Rm], regname[Rn]);
		break;
	case 15:
		snprintf(buffer,160, "MULS.W  %s,%s", regname[Rm], regname[Rn]);
		break;
	}
	return 0;
}

static UINT32 op0011(char *buffer, UINT32 pc, UINT16 opcode)
{
	switch (opcode & 15)
	{
	case  0:
		snprintf(buffer,160, "CMP/EQ  %s,%s", regname[Rm], regname[Rn]);
		break;
	case  1:
		snprintf(buffer,160, "??????  %s,%s", regname[Rm], regname[Rn]);
		break;
	case  2:
		snprintf(buffer,160, "CMP/HS  %s,%s", regname[Rm], regname[Rn]);
		break;
	case  3:
		snprintf(buffer,160, "CMP/GE  %s,%s", regname[Rm], regname[Rn]);
		break;
	case  4:
		snprintf(buffer,160, "DIV1    %s,%s", regname[Rm], regname[Rn]);
		break;
	case  5:
		snprintf(buffer,160, "DMULU.L %s,%s", regname[Rm], regname[Rn]);
		break;
	case  6:
		snprintf(buffer,160, "CMP/HI  %s,%s", regname[Rm], regname[Rn]);
		break;
	case  7:
		snprintf(buffer,160, "CMP/GT  %s,%s", regname[Rm], regname[Rn]);
		break;
	case  8:
		snprintf(buffer,160, "SUB     %s,%s", regname[Rm], regname[Rn]);
		break;
	case  9:
		snprintf(buffer,160, "??????  %s,%s", regname[Rm], regname[Rn]);
		break;
	case 10:
		snprintf(buffer,160, "SUBC    %s,%s", regname[Rm], regname[Rn]);
		break;
	case 11:
		snprintf(buffer,160, "SUBV    %s,%s", regname[Rm], regname[Rn]);
		break;
	case 12:
		snprintf(buffer,160, "ADD     %s,%s", regname[Rm], regname[Rn]);
		break;
	case 13:
		snprintf(buffer,160, "DMULS.L %s,%s", regname[Rm], regname[Rn]);
		break;
	case 14:
		snprintf(buffer,160, "ADDC    %s,%s", regname[Rm], regname[Rn]);
		break;
	case 15:
		snprintf(buffer,160, "ADDV    %s,%s", regname[Rm], regname[Rn]);
		break;
	}
	return 0;
}

static UINT32 op0100(char *buffer, UINT32 pc, UINT16 opcode)
{
	UINT32 flags = 0;
	switch(opcode & 0x3F)
	{
	case 0x00:
		snprintf(buffer,160, "SHLL    %s", regname[Rn]);
		break;
	case 0x01:
		snprintf(buffer,160, "SHLR    %s", regname[Rn]);
		break;
	case 0x02:
		snprintf(buffer,160, "STS.L   MACH,@-%s", regname[Rn]);
		break;
	case 0x03:
		snprintf(buffer,160, "STC.L   SR,@-%s", regname[Rn]);
		break;
	case 0x04:
		snprintf(buffer,160, "ROTL    %s", regname[Rn]);
		break;
	case 0x05:
		snprintf(buffer,160, "ROTR    %s", regname[Rn]);
		break;
	case 0x06:
		snprintf(buffer,160, "LDS.L   @%s+,MACH", regname[Rn]);
		break;
	case 0x07:
		snprintf(buffer,160, "LDC.L   @%s+,SR", regname[Rn]);
		break;
	case 0x08:
		snprintf(buffer,160, "SHLL2   %s", regname[Rn]);
		break;
	case 0x09:
		snprintf(buffer,160, "SHLR2   %s", regname[Rn]);
		break;
	case 0x0a:
		snprintf(buffer,160, "LDS     %s,MACH", regname[Rn]);
		break;
	case 0x0b:
		snprintf(buffer,160, "JSR     %s", regname[Rn]);
		flags = DASMFLAG_STEP_OVER | DASMFLAG_STEP_OVER_EXTRA(1);
		break;
	case 0x0e:
		snprintf(buffer,160, "LDC     %s,SR", regname[Rn]);
		break;
	case 0x10:
		snprintf(buffer,160, "DT      %s", regname[Rn]);
		break;
	case 0x11:
		snprintf(buffer,160, "CMP/PZ  %s", regname[Rn]);
		break;
	case 0x12:
		snprintf(buffer,160, "STS.L   MACL,@-%s", regname[Rn]);
		break;
	case 0x13:
		snprintf(buffer,160, "STC.L   GBR,@-%s", regname[Rn]);
		break;
	case 0x15:
		snprintf(buffer,160, "CMP/PL  %s", regname[Rn]);
		break;
	case 0x16:
		snprintf(buffer,160, "LDS.L   @%s+,MACL", regname[Rn]);
		break;
	case 0x17:
		snprintf(buffer,160, "LDC.L   @%s+,GBR", regname[Rn]);
		break;
	case 0x18:
		snprintf(buffer,160, "SHLL8   %s", regname[Rn]);
		break;
	case 0x19:
		snprintf(buffer,160, "SHLR8   %s", regname[Rn]);
		break;
	case 0x1a:
		snprintf(buffer,160, "LDS     %s,MACL", regname[Rn]);
		break;
	case 0x1b:
		snprintf(buffer,160, "TAS     %s", regname[Rn]);
		break;
	case 0x1e:
		snprintf(buffer,160, "LDC     %s,GBR", regname[Rn]);
		break;
	case 0x20:
		snprintf(buffer,160, "SHAL    %s", regname[Rn]);
		break;
	case 0x21:
		snprintf(buffer,160, "SHAR    %s", regname[Rn]);
		break;
	case 0x22:
		snprintf(buffer,160, "STS.L   PR,@-%s", regname[Rn]);
		break;
	case 0x23:
		snprintf(buffer,160, "STC.L   VBR,@-%s", regname[Rn]);
		break;
	case 0x24:
		snprintf(buffer,160, "ROTCL   %s", regname[Rn]);
		break;
	case 0x25:
		snprintf(buffer,160, "ROTCR   %s", regname[Rn]);
		break;
	case 0x26:
		snprintf(buffer,160, "LDS.L   @%s+,PR", regname[Rn]);
		break;
	case 0x27:
		snprintf(buffer,160, "LDC.L   @%s+,VBR", regname[Rn]);
		break;
	case 0x28:
		snprintf(buffer,160, "SHLL16  %s", regname[Rn]);
		break;
	case 0x29:
		snprintf(buffer,160, "SHLR16  %s", regname[Rn]);
		break;
	case 0x2a:
		snprintf(buffer,160, "LDS     %s,PR", regname[Rn]);
		break;
	case 0x2b:
		snprintf(buffer,160, "JMP     %s", regname[Rn]);
		break;
	case 0x2e:
		snprintf(buffer,160, "LDC     %s,VBR", regname[Rn]);
		break;
	default:
		if ((opcode & 15) == 15)
			snprintf(buffer,160, "MAC.W   @%s+,@%s+", regname[Rm], regname[Rn]);
		else
			snprintf(buffer,160, "??????  $%04X", opcode);
	}
	return flags;
}

static UINT32 op0101(char *buffer, UINT32 pc, UINT16 opcode)
{
	snprintf(buffer,160, "MOV.L   @($%02X,%s),%s", (opcode & 15) * 4, regname[Rm], regname[Rn]);
	return 0;
}

static UINT32 op0110(char *buffer, UINT32 pc, UINT16 opcode)

{
	switch(opcode & 0xF)
	{
	case 0x00:
		snprintf(buffer,160, "MOV.B   @%s,%s", regname[Rm], regname[Rn]);
		break;
	case 0x01:
		snprintf(buffer,160, "MOV.W   @%s,%s", regname[Rm], regname[Rn]);
		break;
	case 0x02:
		snprintf(buffer,160, "MOV.L   @%s,%s", regname[Rm], regname[Rn]);
		break;
	case 0x03:
		snprintf(buffer,160, "MOV     %s,%s", regname[Rm], regname[Rn]);
		break;
	case 0x04:
		snprintf(buffer,160, "MOV.B   @%s+,%s", regname[Rm], regname[Rn]);
		break;
	case 0x05:
		snprintf(buffer,160, "MOV.W   @%s+,%s", regname[Rm], regname[Rn]);
		break;
	case 0x06:
		snprintf(buffer,160, "MOV.L   @%s+,%s", regname[Rm], regname[Rn]);
		break;
	case 0x07:
		snprintf(buffer,160, "NOT     %s,%s", regname[Rm], regname[Rn]);
		break;
	case 0x08:
		snprintf(buffer,160, "SWAP.B  %s,%s", regname[Rm], regname[Rn]);
		break;
	case 0x09:
		snprintf(buffer,160, "SWAP.W  %s,%s", regname[Rm], regname[Rn]);
		break;
	case 0x0a:
		snprintf(buffer,160, "NEGC    %s,%s", regname[Rm], regname[Rn]);
		break;
	case 0x0b:
		snprintf(buffer,160, "NEG     %s,%s", regname[Rm], regname[Rn]);
		break;
	case 0x0c:
		snprintf(buffer,160, "EXTU.B  %s,%s", regname[Rm], regname[Rn]);
		break;
	case 0x0d:
		snprintf(buffer,160, "EXTU.W  %s,%s", regname[Rm], regname[Rn]);
		break;
	case 0x0e:
		snprintf(buffer,160, "EXTS.B  %s,%s", regname[Rm], regname[Rn]);
		break;
	case 0x0f:
		snprintf(buffer,160, "EXTS.W  %s,%s", regname[Rm], regname[Rn]);
		break;
	}
	return 0;
}

static UINT32 op0111(char *buffer, UINT32 pc, UINT16 opcode)
{
	snprintf(buffer,160, "ADD     #$%02X,%s", opcode & 0xff, regname[Rn]);
	return 0;
}

static UINT32 op1000(char *buffer, UINT32 pc, UINT16 opcode)
{
	switch((opcode >> 8) & 15)
	{
	case  0:
		snprintf(buffer,160, "MOV.B   R0,@($%02X,%s)", (opcode & 15), regname[Rm]);
		break;
	case  1:
		snprintf(buffer,160, "MOV.W   R0,@($%02X,%s)", (opcode & 15) * 2, regname[Rm]);
		break;
	case  4:
		snprintf(buffer,160, "MOV.B   @($%02X,%s),R0", (opcode & 15), regname[Rm]);
		break;
	case  5:
		snprintf(buffer,160, "MOV.W   @($%02X,%s),R0", (opcode & 15) * 2, regname[Rm]);
		break;
	case  8:
		snprintf(buffer,160, "CMP/EQ  #$%02X,R0", (opcode & 0xff));
		break;
	case  9:
		snprintf(buffer,160, "BT      $%08x", pc + SIGNX8(opcode & 0xff) * 2 + 2);
		break;
	case 11:
		snprintf(buffer,160, "BF      $%08x", pc + SIGNX8(opcode & 0xff) * 2 + 2);
		break;
	case 13:
		snprintf(buffer,160, "BTS     $%08x", pc + SIGNX8(opcode & 0xff) * 2 + 2);
		break;
	case 15:
		snprintf(buffer,160, "BFS     $%08x", pc + SIGNX8(opcode & 0xff) * 2 + 2);
		break;
	default :
		snprintf(buffer,160, "invalid $%04X", opcode);
	}
	return 0;
}

static UINT32 op1001(char *buffer, UINT32 pc, UINT16 opcode)
{
	snprintf(buffer,160, "MOV.W   @($%02X,PC),%s  ; @$%08x", (opcode & 0xff) * 2, regname[Rn],
		pc + (opcode & 0xff) * 2 + 2);
	return 0;
}

static UINT32 op1010(char *buffer, UINT32 pc, UINT16 opcode)
{
	snprintf(buffer,160, "BRA     $%08x", SIGNX12(opcode & 0xfff) * 2 + pc + 2);
	return 0;
}

static UINT32 op1011(char *buffer, UINT32 pc, UINT16 opcode)
{
	snprintf(buffer,160, "BSR     $%08x", SIGNX12(opcode & 0xfff) * 2 + pc + 2);
	return DASMFLAG_STEP_OVER | DASMFLAG_STEP_OVER_EXTRA(1);
}

static UINT32 op1100(char *buffer, UINT32 pc, UINT16 opcode)
{
	UINT32 flags = 0;
	switch((opcode >> 8) & 15)
	{
	case  0:
		snprintf(buffer,160, "MOV.B   R0,@($%02X,GBR)", opcode & 0xff);
		break;
	case  1:
		snprintf(buffer,160, "MOV.W   R0,@($%04X,GBR)", (opcode & 0xff) * 2);
		break;
	case  2:
		snprintf(buffer,160, "MOV.L   R0,@($%04X,GBR)", (opcode & 0xff) * 4);
		break;
	case  3:
		snprintf(buffer,160, "TRAPA   #$%02X", opcode & 0xff);
		flags = KR_DEBUG_CALL;
		break;
	case  4:
		snprintf(buffer,160, "MOV.B   @($%02X,GBR),R0", opcode & 0xff);
		break;
	case  5:
		snprintf(buffer,160, "MOV.W   @($%04X,GBR),R0", (opcode & 0xff) * 2);
		break;
	case  6:
		snprintf(buffer,160, "MOV.L   @($%04X,GBR),R0", (opcode & 0xff) * 4);
		break;
	case  7:
		snprintf(buffer,160, "MOVA    @($%04X,PC),R0  ; @$%08x", (opcode & 0xff) * 4,
			((pc + 2) & ~3) + (opcode & 0xff) * 4);
		break;
	case  8:
		snprintf(buffer,160, "TST     #$%02X,R0", opcode & 0xff);
		break;
	case  9:
		snprintf(buffer,160, "AND     #$%02X,R0", opcode & 0xff);
		break;
	case 10:
		snprintf(buffer,160, "XOR     #$%02X,R0", opcode & 0xff);
		break;
	case 11:
		snprintf(buffer,160, "OR      #$%02X,R0", opcode & 0xff);
		break;
	case 12:
		snprintf(buffer,160, "TST.B   #$%02X,@(R0,GBR)", opcode & 0xff);
		break;
	case 13:
		snprintf(buffer,160, "AND.B   #$%02X,@(R0,GBR)", opcode & 0xff);
		break;
	case 14:
		snprintf(buffer,160, "XOR.B   #$%02X,@(R0,GBR)", opcode & 0xff);
		break;
	case 15:
		snprintf(buffer,160, "OR.B    #$%02X,@(R0,GBR)", opcode & 0xff);
		break;
	}
	return flags;
}

static UINT32 op1101(char *buffer, UINT32 pc, UINT16 opcode)
{
	snprintf(buffer,160, "MOV.L   @($%02X,PC),%s  ; @$%08x", (opcode & 0xff) * 4, regname[Rn],
		(pc + (opcode & 0xff) * 4 + 2) & ~3);
	return 0;
}

static UINT32 op1110(char *buffer, UINT32 pc, UINT16 opcode)
{
	snprintf(buffer,160, "MOV     #$%02X,%s", (opcode & 0xff), regname[Rn]);
	return 0;
}

static UINT32 op1111(char *buffer, UINT32 pc, UINT16 opcode)
{
	snprintf(buffer,160, "unknown $%04X", opcode);
	return 0;
}

static unsigned DasmSH2(char *buffer, unsigned pc, UINT16 opcode)
{
	UINT32 flags;

	pc += 2;

	switch((opcode >> 12) & 15)
	{
	case  0: flags = op0000(buffer,pc,opcode);	  break;
	case  1: flags = op0001(buffer,pc,opcode);	  break;
	case  2: flags = op0010(buffer,pc,opcode);	  break;
	case  3: flags = op0011(buffer,pc,opcode);	  break;
	case  4: flags = op0100(buffer,pc,opcode);	  break;
	case  5: flags = op0101(buffer,pc,opcode);	  break;
	case  6: flags = op0110(buffer,pc,opcode);	  break;
	case  7: flags = op0111(buffer,pc,opcode);	  break;
	case  8: flags = op1000(buffer,pc,opcode);	  break;
	case  9: flags = op1001(buffer,pc,opcode);	  break;
	case 10: flags = op1010(buffer,pc,opcode);	  break;
	case 11: flags = op1011(buffer,pc,opcode);	  break;
	case 12: flags = op1100(buffer,pc,opcode);	  break;
	case 13: flags = op1101(buffer,pc,opcode);	  break;
	case 14: flags = op1110(buffer,pc,opcode);	  break;
	default: flags = op1111(buffer,pc,opcode);	  break;
	}
	return flags;
}


unsigned kr_debug_disassemble_sh2(kr_debug_instruction *out,unsigned available) {
    out->length=2;
    if(available<2)return 0;
    unsigned opcode=((unsigned)out->bytes[0]<<8)|out->bytes[1];
    out->flags=DasmSH2(out->text,out->address,opcode);
    if((opcode&0xf000)==0xa000 || (opcode&0xf0ff)==0x0023 ||
       (opcode&0xf0ff)==0x402b || (opcode&0xff00)==0x8d00 || (opcode&0xff00)==0x8f00)
        out->flags|=KR_DEBUG_DELAYED;
    return 2;
}
