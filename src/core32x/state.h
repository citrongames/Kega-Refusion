/* Kega Refusion: pointer-free 32X snapshots and a versioned LE wire codec.
 * Hardware structures retain their respective PicoDrive/MAME notices.
 * No host/BIOS/ROM/filter/translated-code storage is included. */
#ifndef KEGA32X_STATE_H
#define KEGA32X_STATE_H
#include "machine.h"
#include "sh2/soc.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define KEGA32X_MACHINE_STATE_VERSION 1u
/* Keep metadata separate so decoding can validate on a small stack object
 * before writing any caller snapshot or copying its 512 KiB RAM payload. */
typedef struct Kega32xControlState {
    uint32_t version;
    Kega32xBusState bus;
    Kega32xPwmState pwm;
    KegaSh2State cpu[2];
    KegaSh2SocState soc[2];
    uint64_t epoch_sh2[2], now;
} Kega32xControlState;
typedef struct Kega32xMachineState {
    Kega32xControlState control;
    Kega32xMemory memory;
} Kega32xMachineState;
/* Pure validation; no allocation, device access, mutation or callbacks. */
int kega32x_control_state_validate(const Kega32xControlState *);
int kega32x_state_validate(const Kega32xMachineState *);
/* Exact fixed wire size for this schema. Fields are encoded individually;
 * numeric arrays are LE too. No C padding, native pointers or unsigned long. */
size_t kega32x_state_encoded_size(void);
/* Buffers must not overlap. Invalid input/capacity is rejected before writing.
 * Decode requires the exact encoded size; trailing bytes/version/reserved bits
 * are rejected. RAM bytes have no extra checksum: the enclosing save/rewind
 * container provides its integrity protection. No heap allocation occurs. */
int kega32x_state_encode(const Kega32xMachineState *, void *output, size_t capacity);
int kega32x_state_decode(Kega32xMachineState *, const void *input, size_t size);
#ifdef __cplusplus
}
#endif
#endif
