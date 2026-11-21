/* Disassemble the SSP1601 instruction forms executed by the integrated SVP. */
#include "disassemble.h"

#include <stdio.h>

static const char *const registers[] = {
  "-", "X", "Y", "A", "ST", "STACK", "PC", "P",
  "PM0", "PM1", "PM2", "XST", "PM4", "GR13", "PMC", "AL"
};

static void operand_pointer(char *out, size_t capacity, unsigned index,
                            unsigned modifier, int indirections)
{
  static const char *const suffix[] = {"", "+!", "-", "+"};
  const char *open = indirections == 2 ? "((" : "(";
  const char *close = indirections == 2 ? "))" : ")";
  if ((index & 3) == 3)
    snprintf(out, capacity, "%sr%u|%u%s", open, index, modifier, close);
  else
    snprintf(out, capacity, "%sr%u%s%s", open, index, suffix[modifier], close);
}

static const char *condition(unsigned op)
{
  switch (op & 0xf0)
  {
    case 0: return "always";
    case 0x50: return op & 0x100 ? "z=1" : "z=0";
    case 0x70: return op & 0x100 ? "n=1" : "n=0";
    default: return NULL;
  }
}

unsigned kr_debug_disassemble_ssp(kr_debug_instruction *out, unsigned available)
{
  if (available < 2) return 0;

  const unsigned op = (out->bytes[0] << 8) | out->bytes[1];
  const unsigned group = op >> 9;
  const unsigned form = group & 0xf;
  const unsigned dst = (op >> 4) & 0xf;
  const unsigned src = op & 0xf;
  const unsigned pointer = ((op >> 6) & 4) | (op & 3);
  const unsigned modifier = (op >> 2) & 3;
  const unsigned next = (out->bytes[2] << 8) | out->bytes[3];
  unsigned length = 2;
  char arg[24], other[24];

  switch (group)
  {
    case 0:
      if (!op) snprintf(out->text, sizeof(out->text), "nop");
      else
      {
        snprintf(out->text, sizeof(out->text), "ld %s, %s", registers[dst], registers[src]);
        if (dst == 6 && src == 5) out->flags = KR_DEBUG_RETURN;
      }
      break;
    case 1:
    case 2:
    case 5:
      operand_pointer(arg, sizeof(arg), pointer, modifier, group == 5 ? 2 : 1);
      if (group == 2)
        snprintf(out->text, sizeof(out->text), "ld %s, %s", arg, registers[dst]);
      else
        snprintf(out->text, sizeof(out->text), "ld %s, %s", registers[dst], arg);
      break;
    case 3:
    case 7:
      snprintf(out->text, sizeof(out->text), group == 3 ? "ld A, RAM[$%03x]" :
                                                         "ld RAM[$%03x], A", op & 0x1ff);
      break;
    case 4:
    case 6:
      length = 4;
      if (group == 6) operand_pointer(arg, sizeof(arg), pointer, modifier, 1);
      snprintf(out->text, sizeof(out->text), "ldi %s, $%04x", group == 4 ? registers[dst] : arg, next);
      break;
    case 9:
      snprintf(out->text, sizeof(out->text), "ld %s, r%u", registers[dst], pointer);
      break;
    case 0xa:
      snprintf(out->text, sizeof(out->text), "ld r%u, %s", pointer, registers[dst]);
      break;
    case 0xc: case 0xd: case 0xe: case 0xf:
      snprintf(out->text, sizeof(out->text), "ldi r%u, $%02x", (op >> 8) & 7, op & 0xff);
      break;
    case 0x24:
    case 0x26:
    {
      const char *cond = condition(op);
      if (cond)
        snprintf(out->text, sizeof(out->text), "%s %s, $%04x", group == 0x24 ? "call" : "bra", cond, next);
      else
        snprintf(out->text, sizeof(out->text), "%s cond($%02x), $%04x",
            group == 0x24 ? "call" : "bra", (op >> 4) & 0x1f, next);
      length = 4;
      if (group == 0x24) out->flags = KR_DEBUG_CALL;
      break;
    }
    case 0x25:
      snprintf(out->text, sizeof(out->text), "ld %s, (A)", registers[dst]);
      break;
    case 0x48:
    {
      static const char *const operations[] = {NULL, NULL, "shr", "shl", NULL, NULL, "neg", "abs"};
      const char *operation = operations[op & 7];
      const char *cond = condition(op);
      if (operation && cond)
        snprintf(out->text, sizeof(out->text), "%s %s", operation, cond);
      else
        snprintf(out->text, sizeof(out->text), "mod $%04x", op);
      break;
    }
    case 0x1b: case 0x4b: case 0x5b:
      operand_pointer(arg, sizeof(arg), (op & 3), modifier, 1);
      operand_pointer(other, sizeof(other), 4 + ((op >> 4) & 3), (op >> 6) & 3, 1);
      snprintf(out->text, sizeof(out->text), "%s %s, %s%s",
          group == 0x1b ? "mpys" : group == 0x4b ? "mpya" : "mld",
          other, arg, op & 0x100 ? ", b" : "");
      break;
    default:
    {
      static const char *const alu[] = {NULL, "sub", NULL, "cmp", "add", "and", "or", "eor"};
      const char *operation = alu[group >> 4];
      if (!operation)
      {
        snprintf(out->text, sizeof(out->text), ".word $%04x", op);
        break;
      }
      switch (form)
      {
        case 0:
          snprintf(arg, sizeof(arg), "%s", registers[src]);
          break;
        case 1:
        case 5:
          operand_pointer(arg, sizeof(arg), pointer, modifier, form == 5 ? 2 : 1);
          break;
        case 3:
          snprintf(arg, sizeof(arg), "RAM[$%03x]", op & 0x1ff);
          break;
        case 4:
          length = 4;
          snprintf(arg, sizeof(arg), "$%04x", next);
          break;
        case 9:
          snprintf(arg, sizeof(arg), "r%u", pointer);
          break;
        case 0xc:
          snprintf(arg, sizeof(arg), "$%02x", op & 0xff);
          break;
        default:
          snprintf(out->text, sizeof(out->text), ".word $%04x", op);
          return 2;
      }
      snprintf(out->text, sizeof(out->text), "%s A, %s", operation, arg);
      break;
    }
  }

  return length <= available ? length : 0;
}
