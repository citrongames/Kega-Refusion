# Native debugger trace

The core passes fixed records to an owning-thread observer. Binding and unbinding are independent of execution breakpoints. Tracing alone never yields a frame. The consumer copies each record during the callback and adds frame, console instance and sequence information. The host retains history and handles file output. Load, unload and deinitialization clear the native binding.

## Instruction records

Trace instructions are reported after the actual first opcode fetch. An accepted interrupt or register edit that changes the next fetch changes the trace accordingly. Z80 tracing records the HALT instruction once and skips subsequent refresh cycles. The two SH-2s use the instruction interpreter when observing their execution or accepted interrupts. The usual backend resumes when the relevant observer is removed.

The record address is the guest PC. The SH-2 delay-slot record uses the slot's own PC. SSP1601 PCs are word addresses. The first one or two bytes and the numeric opcode come from the executed fetch. Additional bytes come from the native code map at capture time and carry the `KR_TRACE_PEEK_TAIL` flag. Where some bytes cannot be read, the paired `valid_bytes` and `KR_TRACE_BYTES_TRUNCATED` fields delimit the available prefix. The byte-only decoder reads the provided bytes. It can decode prefix records after unload.

The common time is a signed frame-local master-clock position. Main 68000 and Z80 local ticks are already in their scheduler master-clock domain. Sub-68000 ticks are converted using the actual CD scheduler's cycles per line. SH-2 local ticks are the CPU clock count since its last reset, and common time comes from the 32X machine's clock conversion and frame base. SVP time within its scanline slice uses the scheduled cycle allowance and carries the quantized flag. Independent CPU slices can produce records with an earlier common time than the previous delivered record. Sequence remains the authority for observation order.

## Hardware payloads

| Event | Address | Value | Auxiliary |
|---|---|---|---|
| VDP register write | VDP register index | Committed byte | Previous byte |
| VDP DMA start | Initial VDP destination cursor | Transfer units in the native DMA counter | DMA mode in low byte, VDP access code in bits 8–11 |
| VDP DMA end | Final VDP destination cursor | Transfer units from the last scheduler batch | Same mode and access code as start |
| Accepted 68000 interrupt | Interrupted PC | Autovector number | Interrupt level |
| Accepted Z80 interrupt | Interrupted PC | Destination PC | Interrupt mode, or $100 for NMI |
| Accepted SH-2 interrupt | Interrupted PC | Vector number | Interrupt level |
| Sega CD drive command | Command commit address | CDD command nibble | Signed logical sector after processing |
| MegaLD player command | Player register or frozen input commit address | Written byte | Drive, playback and seek state in consecutive bytes |
| MD cable transition | Port B data register | New crossed TH level | Previous level |
| Game Gear cable transition | EXT parallel register 1 | New TH level | Previous level |
| Game Gear UART receive | Receive register 4 | Delivered byte | Stop-bit framing error |
| Firecore bank change | Bank or GPIO register | ROM window byte offset within the selected chip | Chip index, zero-based |

VDP register records cover writes accepted through guest port commands. Reset initialization, state reconstruction and debugger register edits are excluded. DMA completion describes the transfer count being exhausted and uses the scheduled completion cycle. A fill trigger is reported when data starts the transfer. Cable trace observes actual pin transitions or received data on the active link, independently of scheduler yields. Firecore events report the committed bank/chip mapping. Other mappers and 32X device-register writes are outside this hardware event set.

## Physical coverage and pixel provenance groundwork

Coverage requires the physical memory source at the actual access. Direct fetches can identify a bounded offset in a known allocation. A handler-backed read is unknown unless it provides its own identity. Main 68000 cached prefetch needs the original mapping latched alongside the prefetched word. Firecore's wrapping scratch page needs conversion through the active offset, chip and wrap mask. The scratch pointer alone cannot identify the original cartridge position. The 32X boot and banked ROM windows need their original mapper and the SH-2 data-array/SDRAM mirrors need their physical offsets. The SSP address space starts with IRAM before the cartridge-backed instruction region.

For precise pixels, a requested coordinate must carry source metadata from the real fetch and winner selection. VDP mode 5 needs mode-specific table and pattern address context at the real tiles and sprite fetches, and a winner decision from the same priority tables used to mix colors. Mode 4 and TMS legacy rendering have separate table decoding and sprite limits. Midline palette and backdrop segments, interlace, shadow/highlight, clipping and debug layer overrides affect the final winner or color. End-of-frame reconstructive rendering cannot recover that information from VRAM alone.

Firecore bitmap provenance can capture the real four-byte source group and the selected pixel within its 2×2 encoding. For 32X, use the bank/control latched by `video_sync`, the line table and physical framebuffer word or RLE run read by `kega32x_decode_line`, then the MD/32X choice in `mix_md_rgb565`. Carry this metadata through the existing pending-row slots. For MegaLD, `kr_megald_scanline_rgb` has the actual disc side, source frame and field, sample coordinates and fade factors. Blended output needs both contributors. Pixel provenance refers to an unfiltered emulator output coordinate when the NTSC filter or frontend resampling combines adjacent pixels.
