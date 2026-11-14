/* Pioneer PD6103A HLE. Adapted from ares commit
 * 0b928930af4cc59ed99e942996f0d3c815379388, ares/md/mcd/megald.cpp.
 * See LICENSE. The host owns all media buffers and the CDD clock. */
#ifndef KR_LD_PLAYER_H
#define KR_LD_PLAYER_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

enum kr_ld_cdd_status { KR_LD_STOPPED=0, KR_LD_PLAYING=1, KR_LD_SEEKING=2,
  KR_LD_SCANNING=3, KR_LD_PAUSED=4, KR_LD_OPEN=5, KR_LD_NODISC=6 };
enum kr_ld_event { KR_LD_EVENT_PLAY, KR_LD_EVENT_PAUSE, KR_LD_EVENT_STOP,
  KR_LD_EVENT_EJECT, KR_LD_EVENT_INSERT, KR_LD_EVENT_SEEK };
/* SEEK: arg0=Redbook LBA (ABA-150), arg1=start paused. All other args=0.
 * Called synchronously; no re-entry into player reads/writes from callback. */
typedef void (*kr_ld_event_fn)(void *opaque, enum kr_ld_event event, int32_t arg0, int32_t arg1);
typedef struct kr_ld_disc_info {
  uint8_t loaded, laserdisc, clv, digital_audio, side, first_track, last_track;
  uint32_t lead_in_frames, active_frames, lead_out_frames;
  int32_t leadout_lba, track_lba[100];
  uint8_t track_control[100]; /* Redbook Q control nibble: bit 2=data. */
} kr_ld_disc_info;
typedef struct kr_ld_video_state {
  int32_t frameSkipBaseFrame, frameSkipCounter, currentVideoFrameIndex;
  uint8_t currentVideoFrameLeadIn, currentVideoFrameLeadOut;
  uint8_t currentVideoFrameFieldSelectionEnabled[2], currentVideoFrameFieldSelectionEvenField[2];
  uint8_t currentVideoFrameOnEvenField, currentVideoFrameBlanked, currentVideoFrameInterlaced;
  uint8_t digitalMemoryFrameLatched, drawIndex, pendingFrame;
  int32_t latched_file_frame;
} kr_ld_video_state;
typedef struct kr_ld_player {
  kr_ld_disc_info disc;
  kr_ld_video_state video;
  uint8_t inputRegs[32], inputFrozenRegs[32], outputRegs[32], outputFrozenRegs[32];
  uint8_t outputRegsWrittenData[32], outputRegsWrittenCooldownTimer[32];
  uint8_t areInputRegsFrozen, areOutputRegsFrozen;
  uint8_t operationErrorFlag1, operationErrorFlag2, operationErrorFlag3;
  uint8_t seekEnabled, currentSeekMode, currentSeekModeTimeFormat, currentSeekModeRepeat;
  uint8_t analogAudioAttenuationLeft, analogAudioAttenuationRight;
  uint8_t analogAudioFadeToMutedLeft, analogAudioFadeToMutedRight;
  uint8_t activeSeekMode, seekPointRegs[5], stopPointRegs[5];
  uint8_t reachedStopPoint, reachedStopPointPreviously;
  uint8_t currentPlaybackMode, currentPlaybackSpeed, currentPlaybackDirection;
  uint8_t targetDriveState, currentDriveState, targetPauseState, currentPauseState;
  uint8_t seekPerformedSinceLastFrameUpdate, driveStateChangeDelayCounter, selectedTrackInfo;
  int32_t lba, sample_byte, targetStopPoint;
  uint8_t cdd_status, cdd_seeking_pause, current_track, stopPointEnabled;
  uint32_t sectorRepeatCount;
  kr_ld_event_fn event;
  void *opaque;
} kr_ld_player;
typedef struct kr_ld_video_view {
  int32_t file_frame; /* -1=no latched frame; absolute QON index. */
  uint8_t analog_enabled, blanked, field_selection, even_field, interlaced;
} kr_ld_video_view;

void kr_ld_player_init(kr_ld_player *p);
void kr_ld_player_reset(kr_ld_player *p, int gate_array_reset);
void kr_ld_player_set_hooks(kr_ld_player *p, kr_ld_event_fn event, void *opaque);
void kr_ld_player_set_disc(kr_ld_player *p, const kr_ld_disc_info *disc);
void kr_ld_player_disc_ejected(kr_ld_player *p);
/* Host synchronizes after CDD operations and before register accesses. sample_byte
 * is 0..2351, Redbook LBA is ABA-150. Seek completion must invoke sector_advanced
 * even when completing paused, before exposing seek-complete to the BIOS. */
void kr_ld_player_set_timeline(kr_ld_player *p, int32_t lba, int sample_byte, int status, int track);
void kr_ld_player_sector_advanced(kr_ld_player *p, int32_t lba);
int32_t kr_ld_player_advance_delta(kr_ld_player *p);
uint8_t kr_ld_player_read(kr_ld_player *p, uint32_t address);
void kr_ld_player_write(kr_ld_player *p, uint32_t address, uint8_t data);
/* Invoke exactly once per output NTSC field/frame, before querying the view. */
void kr_ld_player_begin_field(kr_ld_player *p);
kr_ld_video_view kr_ld_player_video(const kr_ld_player *p);
/* Apply a decoded 24-bit VBI picture stop code to current playback. */
void kr_ld_player_picture_stop(kr_ld_player *p, uint32_t field0_code, uint32_t field1_code);
uint32_t kr_ld_player_decode_vbi(const uint8_t *rgb, uint32_t width, uint32_t height, size_t pitch, unsigned line);
void kr_ld_player_check_picture_stop(kr_ld_player *p, const uint8_t *rgb, uint32_t width, uint32_t height, size_t pitch);
int32_t kr_ld_player_frame_from_aba(int32_t aba, int process_lead_in);
int32_t kr_ld_player_aba_from_frame(int32_t frame);
void kr_ld_player_mix_audio(const kr_ld_player *p, int16_t dl, int16_t dr,
  int16_t al, int16_t ar, int16_t *left, int16_t *right);
uint32_t kr_ld_player_mix_pixel(const kr_ld_player *p, uint32_t vdp_rgb,
  int backdrop, uint32_t analog_rgb);
#ifdef __cplusplus
}
#endif
#endif
