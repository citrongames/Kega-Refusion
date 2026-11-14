/* Pioneer PD6103A HLE and analog mixing.
 * Adapted from ares 0b928930af4cc59ed99e942996f0d3c815379388
 * ares/md/mcd/{megald.cpp,cdd.cpp}. See accompanying ISC LICENSE.
 * Host CDD scheduling/media reads remain outside this state machine.
 */
#include "ld_player.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
static unsigned ld_bits(unsigned value, int lo, int hi) {
  return (value >> lo) & ((1u << (hi-lo+1))-1u);
}
static void ld_setbits(uint8_t *value, int lo, int hi, unsigned bits) {
  unsigned mask=((1u << (hi-lo+1))-1u) << lo;
  *value=(uint8_t)((*value & ~mask) | ((bits << lo) & mask));
}
static int ld_max(int a,int b) { return a>b?a:b; }
static uint8_t ld_bcd_encode(unsigned n) { return (uint8_t)(((n/10)<<4)|(n%10)); }
static unsigned ld_bcd_decode(uint8_t n) { return (n>>4)*10+(n&15); }
static int ld_track_count(const kr_ld_player *p) {
  return p->disc.first_track && p->disc.last_track>=p->disc.first_track ? p->disc.last_track-p->disc.first_track+1 : 0;
}
static int ld_isTrackAudio(const kr_ld_player *p,int track) {
  return track>=1 && track<=99 && !(p->disc.track_control[track]&4);
}
static void ld_msf(int32_t aba,uint8_t *m,uint8_t *s,uint8_t *f) {
  if(aba<0) aba=0;
  *f=(uint8_t)(aba%75); aba/=75; *s=(uint8_t)(aba%60); *m=(uint8_t)(aba/60);
}
static void ld_getCurrentTimecode(kr_ld_player *p,uint8_t *m,uint8_t *s,uint8_t *f) { ld_msf(p->lba+150,m,s,f); }
static void ld_getCurrentTrackRelativeTimecode(kr_ld_player *p,uint8_t *m,uint8_t *s,uint8_t *f) {
  int t=p->current_track; ld_msf(p->lba-((t>=1&&t<=99)?p->disc.track_lba[t]:0),m,s,f);
}
static void ld_getLeadOutTimecode(kr_ld_player *p,uint8_t *m,uint8_t *s,uint8_t *f) { ld_msf(p->disc.leadout_lba+150,m,s,f); }
static void ld_getTrackTocData(kr_ld_player *p,int t,uint8_t *flags,uint8_t *m,uint8_t *s,uint8_t *f) {
  *flags=0;*m=*s=*f=0;
  if(t>=p->disc.first_track && t<=p->disc.last_track && t>=1 && t<=99) {
    *flags=p->disc.track_control[t];ld_msf(p->disc.track_lba[t]+150,m,s,f);
  }
}
static void ld_emit(kr_ld_player *p,enum kr_ld_event e,int32_t a,int32_t b) {
  if(p->event) p->event(p->opaque,e,a,b);
}
static void ld_play(kr_ld_player *p) {
  if(p->cdd_status==KR_LD_SEEKING) p->cdd_seeking_pause=0; else p->cdd_status=KR_LD_PLAYING;
  ld_emit(p,KR_LD_EVENT_PLAY,0,0);
}
static void ld_pause(kr_ld_player *p) {
  if(p->cdd_status==KR_LD_SEEKING) p->cdd_seeking_pause=1; else p->cdd_status=KR_LD_PAUSED;
  ld_emit(p,KR_LD_EVENT_PAUSE,0,0);
}
static void ld_stop(kr_ld_player *p) {
  p->cdd_status=p->disc.loaded?KR_LD_STOPPED:KR_LD_NODISC;ld_emit(p,KR_LD_EVENT_STOP,0,0);
}
static void ld_eject(kr_ld_player *p) {
  p->cdd_status=KR_LD_NODISC;p->lba=0;p->sample_byte=0;p->current_track=0;
  ld_emit(p,KR_LD_EVENT_EJECT,0,0);
}
static void ld_insert(kr_ld_player *p) { ld_emit(p,KR_LD_EVENT_INSERT,0,0); }
static int32_t ld_abaFromTime(kr_ld_player *p,int h,int m,int s,int f) {
  (void)p;return (((h*60+m)*60+s)*75+f);
}
static void ld_seekToSector(kr_ld_player *p,int32_t lba,int paused) {
  int t;
  p->cdd_status=KR_LD_SEEKING;p->cdd_seeking_pause=!!paused;p->lba=lba;p->sample_byte=0;
  p->sectorRepeatCount=0;
  for(t=p->disc.first_track;t<=p->disc.last_track && t<=99;++t) {
    if(p->disc.track_lba[t]<=lba) p->current_track=(uint8_t)t;
  }
  ld_emit(p,KR_LD_EVENT_SEEK,lba,!!paused);
}
static void ld_seekToTime(kr_ld_player *p,int h,int m,int s,int f,int paused) {
  ld_seekToSector(p,ld_abaFromTime(p,h,m,s,f)-150,paused);
}
static void ld_seekToRelativeTime(kr_ld_player *p,int t,int m,int s,int f,int paused) {
  if(t<p->disc.first_track || t>p->disc.last_track || t<1 || t>99) return;
  ld_seekToSector(p,p->disc.track_lba[t]+(m*60+s)*75+f,paused);
}
static void ld_seekToTrack(kr_ld_player *p,int t,int paused) {
  if(t<1 || t>99) return;
  ld_seekToSector(p,p->disc.track_lba[t],paused);
}
static void ld_latch_frame(kr_ld_player *p) {
  int32_t frame=p->video.currentVideoFrameIndex;
  uint32_t count=p->disc.active_frames;
  if(p->video.currentVideoFrameLeadIn) {
    count=p->disc.lead_in_frames;
    /* The lead-in index is counted backwards from ABA 0. */
    frame=(int32_t)p->disc.lead_in_frames-1-frame;
  } else {
    frame+=(int32_t)p->disc.lead_in_frames;
    if(p->video.currentVideoFrameLeadOut) { frame+=(int32_t)p->disc.active_frames;count=p->disc.lead_out_frames; }
  }
  if(count && frame>=0) { p->video.latched_file_frame=frame;p->video.pendingFrame=1; }
}

static void ld_updateStopPointWithCurrentState(kr_ld_player *p);
static void ld_resetSeekTargetToDefault(kr_ld_player *p);
static int ld_liveSeekRegistersContainsLatchableTarget(kr_ld_player *p);
static int ld_latchSeekTargetFromCurrentState(kr_ld_player *p);
static void ld_performSeekWithLatchedState(kr_ld_player *p);
static uint8_t ld_getOutputRegisterValue(kr_ld_player *p, int regNum);
static void ld_processInputRegisterWrite(kr_ld_player *p, int regNum, uint8_t data, uint8_t previousData, int wasDeferredRegisterWrite);
static int32_t ld_zeroBasedFrameIndexFromABA(kr_ld_player *p, int32_t aba, int processLeadIn);
static int32_t ld_abaFromZeroBasedFrameIndex(kr_ld_player *p, int32_t frameIndex);
static void ld_VideoTimeToRedbookTime(kr_ld_player *p, uint8_t *hours, uint8_t *minutes, uint8_t *seconds, uint8_t *frames);
static void ld_handleStopPointReached(kr_ld_player *p, int32_t aba);
static void ld_updateCurrentVideoFrameNumber(kr_ld_player *p, int32_t aba);
uint8_t kr_ld_player_read(kr_ld_player *p, uint32_t address) {
  int isOutput = (address & 0x80);
  uint8_t regNum = (address & 0x3f) >> 1;
  uint8_t data = 0;
  if (!isOutput) {
    data = p->areInputRegsFrozen ? p->inputFrozenRegs[regNum] : p->inputRegs[regNum];
  } else if (p->outputRegsWrittenCooldownTimer[regNum] > 0) {
    /* Driven output lines briefly retain a host write before returning live data. */
    --p->outputRegsWrittenCooldownTimer[regNum];
    data = p->outputRegsWrittenData[regNum];
  } else if (p->areOutputRegsFrozen) {
    data = p->outputFrozenRegs[regNum];
  } else {
    data = ld_getOutputRegisterValue(p, regNum);
  }

  return data;
}
void kr_ld_player_write(kr_ld_player *p, uint32_t address, uint8_t data) {

  int isOutput = (address & 0x80);
  uint8_t regNum = (address & 0x3f) >> 1;
  if (!isOutput) {
    if (regNum == 0x00) {
      if (p->areInputRegsFrozen) {
        p->inputFrozenRegs[regNum] = data;
      }
      int previousOutputRegsFrozenState = p->areOutputRegsFrozen;
      p->areOutputRegsFrozen = ld_bits(data, 6, 6);
      if (p->areOutputRegsFrozen && !previousOutputRegsFrozenState) {
        for (int i = 0; i < 32; ++i) {
          p->outputFrozenRegs[i] = p->outputRegs[i] = ld_getOutputRegisterValue(p, i);
        }
      }
      int previousInputRegsFrozenState = p->areInputRegsFrozen;
      p->areInputRegsFrozen = !ld_bits(data, 7, 7);
      if (!p->areInputRegsFrozen && previousInputRegsFrozenState) {
        /* Atomic input commit: seek registers, mechanics, playback, then triggers. */
        uint8_t seekRegsPreviousState[6];
        seekRegsPreviousState[0] = p->inputRegs[0x06];
        seekRegsPreviousState[1] = p->inputRegs[0x07];
        seekRegsPreviousState[2] = p->inputRegs[0x08];
        seekRegsPreviousState[3] = p->inputRegs[0x09];
        seekRegsPreviousState[4] = p->inputRegs[0x0A];
        seekRegsPreviousState[5] = p->inputRegs[0x0B];
        p->inputRegs[0x06] = p->inputFrozenRegs[0x06];
        p->inputRegs[0x07] = p->inputFrozenRegs[0x07];
        p->inputRegs[0x08] = p->inputFrozenRegs[0x08];
        p->inputRegs[0x09] = p->inputFrozenRegs[0x09];
        p->inputRegs[0x0A] = p->inputFrozenRegs[0x0A];
        p->inputRegs[0x0B] = p->inputFrozenRegs[0x0B];
        if (ld_liveSeekRegistersContainsLatchableTarget(p)) {
          ld_latchSeekTargetFromCurrentState(p);
        }
        ld_processInputRegisterWrite(p, 0x02, p->inputFrozenRegs[0x02], p->inputRegs[0x02], 1);
        ld_processInputRegisterWrite(p, 0x03, p->inputFrozenRegs[0x03], p->inputRegs[0x03], 1);
        for (int i = 0; i < 32; ++i) {
          uint8_t previousData = ((i >= 0x06) && (i <= 0x0B) ? seekRegsPreviousState[i - 0x06] : p->inputRegs[i]);
          ld_processInputRegisterWrite(p, i, p->inputFrozenRegs[i], previousData, 1);
        }
      } else if (p->areInputRegsFrozen && !previousInputRegsFrozenState) {
        for (int i = 0; i < 32; ++i) {
          p->inputFrozenRegs[i] = p->inputRegs[i];
        }
      }
    }
    if (p->areInputRegsFrozen) {
      p->inputFrozenRegs[regNum] = data;
      if (regNum >= 0x19) {
        ld_processInputRegisterWrite(p, regNum, data, p->inputRegs[regNum], 0);
      }
    } else {
      ld_processInputRegisterWrite(p, regNum, data, p->inputRegs[regNum], 0);
    }
  } else if (p->areOutputRegsFrozen) {
    if (regNum == 0x00) {
      p->outputFrozenRegs[regNum] = (data & 0x7F) | (p->outputRegs[regNum] & 0x80);
      p->outputRegs[regNum] = (data & 0x3E) | (p->outputRegs[regNum] & 0xC1);
    } else {
      p->outputFrozenRegs[regNum] = data;
      if (regNum >= 0x1A) {
        p->outputRegs[regNum] = data;
      }
    }
  } else {
    p->outputRegsWrittenData[regNum] = data;
    p->outputRegsWrittenCooldownTimer[regNum] = 5;
    if (regNum == 0x00) {
      p->outputRegs[regNum] = (data & 0x3E) | (p->outputRegs[regNum] & 0xC1);
    } else if (regNum >= 0x1A) {
      p->outputRegs[regNum] = data;
    }
  }
}
static uint8_t ld_getOutputRegisterValue(kr_ld_player *p, int regNum)
{
  uint8_t previousData = p->outputRegs[regNum];
  uint8_t data = previousData;
  uint8_t flags;
  uint8_t minute;
  uint8_t second;
  uint8_t frame;
  switch (regNum) {
  case 0x00:
    ld_setbits(&data, 7, 7, 1);
    ld_setbits(&data, 6, 6, 0);
    ld_setbits(&data, 0, 0, ld_bits(p->inputRegs[0x00], 0, 0));
    break;
  case 0x01:
    data = 0;
    ld_setbits(&data, 7, 7, 1);
    break;
  case 0x02:
    data = 0;
    if (p->currentDriveState < 0x02) {
      data = 0x00;
    } else if (!(p->disc.loaded)) {
      ld_setbits(&data, 4, 4, 1);
    } else if ((p->disc.laserdisc)) {
      data = (p->currentDriveState >= 0x04) ? ((p->disc.clv) ? 0xC6 : 0xC5) : 0x80;
    } else {
      data = 0x20;
    }
    break;
  case 0x03:
    if (p->currentDriveState < 0x02) {
      data = 0;
    } else if (p->currentDriveState < 0x04) {
    } else {
      ld_setbits(&data, 3, 3, (p->disc.loaded));
      if ((p->disc.loaded) && (p->disc.laserdisc))
      {
        ld_setbits(&data, 6, 7, (p->disc.side > 1 ? 0b10 : 0b01));
      } else {
        ld_setbits(&data, 6, 7, 0);
      }
    }
    break;
  case 0x04:
    data = 0;
    ld_setbits(&data, 5, 5, !(p->disc.loaded) || !(p->disc.laserdisc) || (p->currentDriveState < 0x04));
    if (!(p->disc.loaded) || (p->disc.laserdisc) || (p->currentDriveState < 0x02)) {
      ld_setbits(&data, 7, 7, ld_bits(p->inputRegs[0x0D], 4, 4));
    }
    if ((p->disc.loaded) && (p->disc.laserdisc) && (p->currentDriveState >= 0x04)) {
      ld_setbits(&data, 0, 3, (ld_isTrackAudio(p, (p->current_track)) ? 0x0C : 0x0E));
    } else if ((p->disc.loaded) && !(p->disc.laserdisc) && (p->currentDriveState >= 0x04)) {
      ld_setbits(&data, 0, 3, 0x2);
    }
    break;
  case 0x05:
    data = 0x7F;
    break;
  case 0x06:
    /* BIOS code requires at least one busy observation for mechanical changes. */
    if ((ld_bits(previousData, 0, 3) != p->currentDriveState) || (p->driveStateChangeDelayCounter > 0)) {
      if ((ld_bits(previousData, 7, 7) != 1) || (p->driveStateChangeDelayCounter == 0)) {
        static const unsigned int ChangeDelayCount = 10;
        p->driveStateChangeDelayCounter = ChangeDelayCount;
      } else if (p->driveStateChangeDelayCounter == 1) {
        ld_setbits(&data, 0, 3, p->currentDriveState);
        p->driveStateChangeDelayCounter = 0;
      } else {
        --p->driveStateChangeDelayCounter;
      }
    }
    if ((ld_bits(data, 0, 3) == 5) && ((p->cdd_status == KR_LD_SEEKING) || ((p->cdd_status == KR_LD_PLAYING) && p->seekPerformedSinceLastFrameUpdate))) {
      ld_setbits(&data, 7, 7, 1);
      ld_setbits(&data, 5, 5, 1);
    } else {
      ld_setbits(&data, 7, 7, (p->driveStateChangeDelayCounter > 0));
      ld_setbits(&data, 5, 5, 0);
    }
    ld_setbits(&data, 4, 4, p->currentPauseState || p->targetPauseState);
    break;
  case 0x07:
    ld_setbits(&data, 4, 7, p->currentPlaybackMode);
    ld_setbits(&data, 3, 3, p->currentPlaybackDirection);
    ld_setbits(&data, 0, 2, p->currentPlaybackSpeed);
    if (p->reachedStopPoint) {
      data = 0x20;
    } else if ((p->currentPlaybackMode == 0x02) && (p->currentPlaybackSpeed == 0x00)) {
      ld_setbits(&data, 3, 3, 0);
    } else if ((p->currentPlaybackMode == 0x01) && (p->currentPlaybackSpeed == 0x01) && (p->video.frameSkipCounter == -1)) {
    }
    break;
  case 0x08:
    ld_setbits(&data, 7, 7, (p->disc.loaded) && (p->disc.laserdisc) && (p->currentDriveState >= 5));
    ld_setbits(&data, 6, 6, (p->disc.loaded) && ((!(p->disc.laserdisc) && (p->currentDriveState >= 2)) || ((p->disc.laserdisc) && (p->currentDriveState >= 4))));
    ld_setbits(&data, 0, 0, (p->disc.loaded) && (p->currentDriveState >= 4));
    break;
  case 0x09:
    ld_setbits(&data, 4, 4, p->operationErrorFlag1);
    ld_setbits(&data, 1, 1, p->operationErrorFlag2);
    ld_setbits(&data, 0, 0, p->operationErrorFlag3);
    break;
  case 0x0A:
    data = p->inputRegs[0x06];
    break;
  case 0x0B:
    data = p->inputRegs[0x07];
    break;
  case 0x0C:
    data = p->inputRegs[0x08];
    break;
  case 0x0D:
    data = p->inputRegs[0x09];
    break;
  case 0x0E:
    data = p->inputRegs[0x0A];
    break;
  case 0x0F:
    data = p->inputRegs[0x0B];
    break;
  case 0x10:
    data = p->inputRegs[0x05];
    break;
  case 0x11: {
    if (p->selectedTrackInfo > ld_track_count(p)) {
      data = 0;
    } else {
      int trackToQuery = (p->selectedTrackInfo == 0 ? (p->current_track) : (uint8_t)p->selectedTrackInfo);
      ld_getTrackTocData(p, trackToQuery, &flags, &minute, &second, &frame);
      ld_setbits(&data, 4, 7, flags);
      ld_setbits(&data, 0, 3, 0x01);
    }
    break;}
  case 0x12:
    if ((p->selectedTrackInfo == 0xA0) || (p->selectedTrackInfo == 0xB0)) {
      data = ld_bcd_encode(p->disc.first_track);
    } else if ((p->selectedTrackInfo == 0xA1) || (p->selectedTrackInfo == 0xB1)) {
      ld_getLeadOutTimecode(p, &minute, &second, &frame);
      data = ld_bcd_encode(minute);
    } else if (((p->selectedTrackInfo == 0) || (p->selectedTrackInfo == 0xFF)) && (ld_track_count(p) > 0)) {
      ld_getCurrentTimecode(p, &minute, &second, &frame);
      data = ld_bcd_encode(minute);
    } else if (p->selectedTrackInfo > ld_track_count(p)) {
      data = 0xFF;
    } else {
      ld_getTrackTocData(p, p->selectedTrackInfo, &flags, &minute, &second, &frame);
      data = ld_bcd_encode(minute);
    }
    break;
  case 0x13:
    if ((p->selectedTrackInfo == 0xA0) || (p->selectedTrackInfo == 0xB0)) {
      data = ld_bcd_encode(ld_track_count(p));
    } else if ((p->selectedTrackInfo == 0xA1) || (p->selectedTrackInfo == 0xB1)) {
      ld_getLeadOutTimecode(p, &minute, &second, &frame);
      data = ld_bcd_encode(second);
    } else if (((p->selectedTrackInfo == 0) || (p->selectedTrackInfo == 0xFF)) && (ld_track_count(p) > 0)) {
      ld_getCurrentTimecode(p, &minute, &second, &frame);
      data = ld_bcd_encode(second);
    } else if (p->selectedTrackInfo > ld_track_count(p)) {
      data = 0xFF;
    } else {
      ld_getTrackTocData(p, p->selectedTrackInfo, &flags, &minute, &second, &frame);
      data = ld_bcd_encode(second);
    }
    break;
  case 0x14:
    if ((p->selectedTrackInfo == 0xA0) || (p->selectedTrackInfo == 0xB0)) {
      data = 0x00;
    } else if ((p->selectedTrackInfo == 0xA1) || (p->selectedTrackInfo == 0xB1)) {
      ld_getLeadOutTimecode(p, &minute, &second, &frame);
      data = ld_bcd_encode(frame);
    } else if (((p->selectedTrackInfo == 0) || (p->selectedTrackInfo == 0xFF)) && (ld_track_count(p) > 0)) {
      ld_getCurrentTimecode(p, &minute, &second, &frame);
      data = ld_bcd_encode(frame);
    } else if (p->selectedTrackInfo > ld_track_count(p)) {
      data = 0xFF;
    } else {
      ld_getTrackTocData(p, p->selectedTrackInfo, &flags, &minute, &second, &frame);
      data = ld_bcd_encode(frame);
    }
    break;
  case 0x15:
    if ((p->disc.loaded) && (p->currentDriveState >= 5)) {
      data = ld_bcd_encode((p->current_track));
    } else if ((p->disc.loaded) && (p->currentDriveState >= 2)) {
      data = 0x01;
    } else {
      data = 0x00;
    }
    break;
  case 0x16:
    data = 0;
    if ((p->disc.loaded) && (p->currentDriveState >= 5)) {
      if ((p->disc.laserdisc)) {
        int frameNumber = ld_zeroBasedFrameIndexFromABA(p, (p->lba + 150), 0) + 1;
        if ((p->disc.clv)) {
          data = ld_bcd_encode((frameNumber / (60 * 60 * 30)) % 60);
        } else {
          data = ld_bcd_encode((frameNumber / (100 * 100)) % 100);
        }
      } else {
        data = 0x01;
      }
    }
    break;
  case 0x17:
    data = 0;
    if ((p->disc.loaded) && (p->currentDriveState >= 5)) {
      if ((p->disc.laserdisc)) {
        int frameNumber = ld_zeroBasedFrameIndexFromABA(p, (p->lba + 150), 0) + 1;
        if ((p->disc.clv)) {
          data = ld_bcd_encode((frameNumber / (60 * 30)) % 60);
        } else {
          data = ld_bcd_encode((frameNumber / 100) % 100);
        }
      } else {
        ld_getCurrentTrackRelativeTimecode(p, &minute, &second, &frame);
        data = ld_bcd_encode(minute);
      }
    }
    break;
  case 0x18:
    data = 0;
    if ((p->disc.loaded) && (p->currentDriveState >= 5)) {
      if ((p->disc.laserdisc)) {
        int frameNumber = ld_zeroBasedFrameIndexFromABA(p, (p->lba + 150), 0) + 1;
        if ((p->disc.clv)) {
          data = ld_bcd_encode((frameNumber / 30) % 60);
        } else {
          data = ld_bcd_encode(frameNumber % 100);
        }
      } else {
        ld_getCurrentTrackRelativeTimecode(p, &minute, &second, &frame);
        data = ld_bcd_encode(second);
      }
    }
    break;
  case 0x19:
    data = 0;
    if ((p->disc.loaded) && (p->currentDriveState >= 5)) {
      if ((p->disc.laserdisc)) {
        if ((p->disc.clv)) {
          int frameNumber = ld_zeroBasedFrameIndexFromABA(p, (p->lba + 150), 0) + 1;
          data = ld_bcd_encode(frameNumber % 30);
        } else {
          data = 0x00;
        }
      } else {
        ld_getCurrentTrackRelativeTimecode(p, &minute, &second, &frame);
        data = ld_bcd_encode(frame);
      }
    }
    break;
  case 0x1A:
   /* fall through */
  case 0x1B:
   /* fall through */
  case 0x1C:
   /* fall through */
  case 0x1D:
   /* fall through */
  case 0x1E:
   /* fall through */
  case 0x1F:
    break;
  default:
    break;
  }
  p->outputRegs[regNum] = data;
  return data;
}
static void ld_processInputRegisterWrite(kr_ld_player *p, int regNum, uint8_t data, uint8_t previousData, int wasDeferredRegisterWrite)
{
  switch (regNum) {
  case 0x00:
    if (ld_bits(previousData, 0, 0) != ld_bits(data, 0, 0)) {
      p->operationErrorFlag1 = 0;
      p->operationErrorFlag2 = 0;
      p->operationErrorFlag3 = 0;
      p->reachedStopPoint = 0;
      if (p->seekEnabled) {
        if (ld_liveSeekRegistersContainsLatchableTarget(p)) {
          if (ld_latchSeekTargetFromCurrentState(p)) {
            ld_performSeekWithLatchedState(p);
          }
        } else {
          ld_performSeekWithLatchedState(p);
        }
      }
      int livePlaybackMode = ld_bits(p->inputRegs[0x03], 4, 7);
      int livePlaybackSpeed = ld_bits(p->inputRegs[0x03], 0, 2);
      if ((livePlaybackMode == 0x01) && (p->currentPlaybackMode == 0x01) && (livePlaybackSpeed == 0x01) && (p->currentPlaybackSpeed == 0x01) && (p->video.frameSkipCounter == -1)) {
        p->video.frameSkipBaseFrame = 0;
        p->video.frameSkipCounter = 0;
      } else if ((livePlaybackMode == 0x02) && (p->currentPlaybackMode == 0x02) && (livePlaybackSpeed == 0x01) && (p->currentPlaybackSpeed == 0x00)) {
        p->currentPlaybackSpeed = 0x01;
      }
      if (p->currentDriveState == 0x05) {
        if (p->currentPauseState) {
          ld_pause(p);
        } else {
          ld_play(p);
        }
      }
    }
    break;
  case 0x01:
    break;
  case 0x02: {
    if (ld_bits(data, 0, 5) == ld_bits(previousData, 0, 5)) {
      break;
    }
    p->seekEnabled = ld_bits(data, 5, 5);
    p->operationErrorFlag1 = 0;
    p->operationErrorFlag2 = 0;
    p->operationErrorFlag3 = 0;
    int newDriveState = ld_bits(data, 0, 3);
    int pauseFlag = ld_bits(data, 4, 4);
    int previousPauseFlag = ld_bits(previousData, 4, 4);
    if (pauseFlag != previousPauseFlag) {
      if ((newDriveState == 0x0) || (newDriveState == 0x5) || (newDriveState == 0x7)) {
        p->targetPauseState = pauseFlag;
        p->currentPauseState = p->targetPauseState;
      }
    }
    p->targetDriveState = newDriveState;
    switch (p->targetDriveState) {
    case 0x01:
      ld_eject(p);
      p->currentDriveState = 0x01;
      ld_resetSeekTargetToDefault(p);
      break;
    case 0x02:
      ld_stop(p);
      p->currentDriveState = 0x02;
      ld_resetSeekTargetToDefault(p);
      break;
    case 0x03:
      ld_stop(p);
      p->currentDriveState = 0x03;
      ld_resetSeekTargetToDefault(p);
      break;
    case 0x04:
      if (!(p->disc.loaded)) {
        p->operationErrorFlag1 = 1;
        p->operationErrorFlag2 = 1;
        break;
      }
      if (p->currentDriveState <= 3) {
        ld_insert(p);
        ld_resetSeekTargetToDefault(p);
        ld_seekToTrack(p, 1, 1);
        p->seekPerformedSinceLastFrameUpdate = 1;
        p->currentPauseState = 1;
      }
      if (p->currentDriveState < 0x04) {
        p->currentDriveState = 0x04;
      } else {
        p->operationErrorFlag1 = 1;
      }
      break;
    case 0x05: {
      if (!(p->disc.loaded)) {
        p->operationErrorFlag1 = 1;
        p->operationErrorFlag2 = 1;
        break;
      }
      if (p->seekEnabled) {
        p->reachedStopPoint = 0;
      }
      int performedLoadOfDisc = 0;
      if (p->currentDriveState <= 3) {
        ld_insert(p);
        ld_resetSeekTargetToDefault(p);
        ld_seekToTrack(p, 1, 1);
        p->seekPerformedSinceLastFrameUpdate = 1;
        p->currentPauseState = 1;
        performedLoadOfDisc = 1;
      }
      p->currentPauseState = p->targetPauseState;
      p->currentDriveState = 0x05;
      if (p->seekEnabled && (performedLoadOfDisc || !wasDeferredRegisterWrite)) {
        if (ld_liveSeekRegistersContainsLatchableTarget(p)) {
          if (ld_latchSeekTargetFromCurrentState(p)) {
            ld_performSeekWithLatchedState(p);
          }
        } else {
          ld_performSeekWithLatchedState(p);
        }
      }
      if (p->currentPauseState || p->reachedStopPoint) {
        ld_pause(p);
      } else {
        ld_play(p);
      }
      break; }
    case 0x00:
    case 0x07:
      if (!(p->disc.loaded)) {
        if (p->targetDriveState == 0x07) {
          p->operationErrorFlag1 = 1;
          p->operationErrorFlag2 = 1;
        }
        break;
      }
      if (p->currentDriveState == 0x05) {
        if (p->targetPauseState || p->reachedStopPoint) {
          ld_pause(p);
        } else {
          ld_play(p);
        }
      }
      break;
    default:
      p->operationErrorFlag1 = 1;
      if (!(p->disc.loaded)) {
        p->operationErrorFlag2 = 1;
      }
      break;
    }
    break;}
  case 0x03: {
    if (data == previousData) {
      break;
    }
    p->operationErrorFlag1 = 0;
    p->operationErrorFlag2 = 0;
    p->operationErrorFlag3 = 0;
    int newPlaybackMode = ld_bits(data, 4, 7);
    if (newPlaybackMode >= 0x04) {
      p->operationErrorFlag1 = 1;
    } else {
      int newPlaybackSpeed = ld_bits(data, 0, 2);
      int newPlaybackDirection = ld_bits(data, 3, 3);
      switch (newPlaybackMode) {
      case 0:
        newPlaybackSpeed = 0;
        break;
      case 1:
        newPlaybackDirection = 0;
        break;
      case 2:
        newPlaybackDirection = (newPlaybackSpeed == 0) ? 0 : newPlaybackDirection;
        break;
      case 3:
        if (newPlaybackSpeed == 0x07) {
          newPlaybackSpeed = 0x06;
        }
        break;
      }
      p->currentPlaybackMode = newPlaybackMode;
      p->currentPlaybackSpeed = newPlaybackSpeed;
      p->currentPlaybackDirection = newPlaybackDirection;
      p->reachedStopPoint = 0;
      if (p->currentDriveState == 0x05) {
        if (p->currentPauseState) {
          ld_pause(p);
        } else {
          ld_play(p);
        }
      }
    }
    break;
  }
  case 0x04:
    break;
  case 0x05:
    p->selectedTrackInfo = (data < 0x99 ? ld_bcd_decode(data) : (uint8_t)data);
    break;
  case 0x06: {
    int seekModeUpdated = 0;
    int targetSeekMode = ld_bits(data, 0, 1);
    if (((targetSeekMode == 1) || (targetSeekMode == 2)) && (p->currentSeekMode != targetSeekMode)) {
      p->currentSeekMode = targetSeekMode;
      seekModeUpdated = 1;
    }
    int targetSeekModeTimeFormat = ld_bits(data, 2, 2);
    if (((targetSeekMode == 1) || (targetSeekMode == 2)) && (p->currentSeekModeTimeFormat != targetSeekModeTimeFormat)) {
      p->currentSeekModeTimeFormat = targetSeekModeTimeFormat;
      seekModeUpdated = 1;
    }
    if (targetSeekMode == 3) {
      if (ld_bits(previousData, 0, 2) != ld_bits(data, 0, 2)) {
        ld_updateStopPointWithCurrentState(p);
      }
    } else if (seekModeUpdated && p->seekEnabled && (p->currentDriveState == 0x5) && (((p->currentSeekMode == 1) && (p->currentSeekModeTimeFormat == 1)) || (p->currentSeekMode == 2))) {
      if (ld_latchSeekTargetFromCurrentState(p)) {
        ld_performSeekWithLatchedState(p);
      }
    }
    break;}
  case 0x07:
    /* fall through */
  case 0x08:
    /* fall through */
  case 0x09:
    /* fall through */
  case 0x0A:
    /* fall through */
  case 0x0B: {
    p->inputRegs[regNum] = data;
    int targetSeekMode = ld_bits(p->inputRegs[0x06], 0, 1);
    if ((targetSeekMode == 3) && (previousData != data)) {
      ld_updateStopPointWithCurrentState(p);
    }
    if (!wasDeferredRegisterWrite && p->seekEnabled && (p->currentDriveState == 0x5) && (((p->currentSeekMode == 1) && (p->currentSeekModeTimeFormat == 1)) || (p->currentSeekMode == 2))) {
      if (ld_latchSeekTargetFromCurrentState(p)) {
        ld_performSeekWithLatchedState(p);
      }
    }
    break;}
  case 0x0C:
    break;
  case 0x0D:
    break;
  case 0x0E:
    break;
  case 0x0F:
    break;
  case 0x10:
    break;
  case 0x11:
    break;
  case 0x12:
    break;
  case 0x13:
    break;
  case 0x14:
    break;
  case 0x15:
    break;
  case 0x16:
    break;
  case 0x17:
    break;
  case 0x18:
    break;
  case 0x19:
    break;
  case 0x1A:
    break;
  case 0x1B:
    break;
  case 0x1C:
    break;
  case 0x1D:
    break;
  case 0x1E:
    p->inputRegs[regNum] = data;
    if (ld_bits(p->inputRegs[0x1E], 1, 1) && ld_bits(p->inputRegs[0x1E], 3, 3)) {
      if (!ld_bits(p->inputRegs[0x1E], 2, 2)) {
        p->analogAudioFadeToMutedLeft = 1;
        p->analogAudioFadeToMutedRight = 1;
      } else if (!ld_bits(p->inputRegs[0x1E], 0, 0)) {
        p->analogAudioFadeToMutedLeft = 1;
      } else {
        p->analogAudioFadeToMutedRight = 1;
      }
    }
    /* fall through */
  case 0x1F:
    if (ld_bits(p->inputRegs[0x1E], 1, 1) && !ld_bits(p->inputRegs[0x1E], 3, 3)) {
      if (!ld_bits(p->inputRegs[0x1E], 2, 2)) {
        p->analogAudioAttenuationLeft = data;
        p->analogAudioAttenuationRight = data;
        p->analogAudioFadeToMutedLeft = 0;
        p->analogAudioFadeToMutedRight = 0;
      } else if (!ld_bits(p->inputRegs[0x1E], 0, 0)) {
        p->analogAudioAttenuationLeft = data;
        p->analogAudioFadeToMutedLeft = 0;
      } else {
        p->analogAudioAttenuationRight = data;
        p->analogAudioFadeToMutedRight = 0;
      }
    }
    break;
  default:

    break;
  }
  p->inputRegs[regNum] = data;
}
static void ld_updateStopPointWithCurrentState(kr_ld_player *p) {
  if (p->inputRegs[0x07] == 0xFF) {
    p->outputRegs[0x1A] = 0xFF;
    p->outputRegs[0x1B] = 0xFF;
    p->outputRegs[0x1C] = 0xFF;
    p->outputRegs[0x1D] = 0xFF;
    p->outputRegs[0x1E] = 0xFF;
    p->outputRegs[0x1F] = 0x01;
    p->stopPointEnabled = 0;

  } else {
    p->stopPointRegs[0] = (p->inputRegs[0x07] == 0x00) ? (uint8_t)0xFF : p->inputRegs[0x07];
    p->stopPointRegs[1] = (!(p->disc.laserdisc) ? 0x00 : (p->inputRegs[0x08] & 0x0F));
    p->stopPointRegs[2] = p->inputRegs[0x09];
    p->stopPointRegs[3] = p->inputRegs[0x0A];
    p->stopPointRegs[4] = ((p->disc.laserdisc) && !(p->disc.clv)) ? (uint8_t)0x00 : p->inputRegs[0x0B];
    p->outputRegs[0x1A] = p->stopPointRegs[0];
    p->outputRegs[0x1B] = p->stopPointRegs[4];
    p->outputRegs[0x1C] = p->stopPointRegs[3];
    p->outputRegs[0x1D] = p->stopPointRegs[2];
    p->outputRegs[0x1E] = p->stopPointRegs[1];
    p->outputRegs[0x1F] = 0x01;
    int32_t stopAba;
    if (!(p->disc.laserdisc)) {
      stopAba = ld_abaFromTime(p, 0, ld_bcd_decode(p->stopPointRegs[2]), ld_bcd_decode(p->stopPointRegs[3]), ld_bcd_decode(p->stopPointRegs[4]));
    } else if (!(p->disc.clv)) {
      int32_t frameNumber = ((int32_t)ld_bcd_decode(p->stopPointRegs[1]) * 10000) + ((int32_t)ld_bcd_decode(p->stopPointRegs[2]) * 100) + (int32_t)ld_bcd_decode(p->stopPointRegs[3]);
      stopAba = ld_abaFromZeroBasedFrameIndex(p, frameNumber - 1);
    } else {
      uint8_t videoTimeHours = ld_bcd_decode(p->stopPointRegs[1]);
      uint8_t videoTimeMinutes = ld_bcd_decode(p->stopPointRegs[2]);
      uint8_t videoTimeSeconds = ld_bcd_decode(p->stopPointRegs[3]);
      uint8_t videoTimeFrames = ld_bcd_decode(p->stopPointRegs[4]);
      ld_VideoTimeToRedbookTime(p, &videoTimeHours, &videoTimeMinutes, &videoTimeSeconds, &videoTimeFrames);
      stopAba = ld_abaFromTime(p, videoTimeHours, videoTimeMinutes, videoTimeSeconds, videoTimeFrames);
    }
    p->targetStopPoint = stopAba;
    p->stopPointEnabled = 1;
    p->reachedStopPointPreviously = 0;

    if (p->reachedStopPoint) {
      ld_handleStopPointReached(p, (p->lba + 150));
    }
  }
}
static void ld_resetSeekTargetToDefault(kr_ld_player *p) {
  p->currentSeekMode = 0x1;
  p->currentSeekModeTimeFormat = 0x1;
  p->seekPointRegs[0] = 0x00;
  p->seekPointRegs[1] = 0x00;
  p->seekPointRegs[2] = 0x00;
  p->seekPointRegs[3] = 0x00;
  p->seekPointRegs[4] = 0x00;
  p->activeSeekMode = (uint8_t)1;
}
static int ld_liveSeekRegistersContainsLatchableTarget(kr_ld_player *p) {
  int targetSeekMode = ld_bits(p->inputRegs[0x06], 0, 1);
  if ((targetSeekMode == 3) || (targetSeekMode == 0)) {
    return 0;
  }
  return 1;
}
static int ld_latchSeekTargetFromCurrentState(kr_ld_player *p) {
  if (!ld_liveSeekRegistersContainsLatchableTarget(p)) {
    return 0;
  }
  int targetSeekMode = ld_bits(p->inputRegs[0x06], 0, 1);
  int targetSeekModeTimeFormat = ld_bits(p->inputRegs[0x06], 2, 2);
  p->currentSeekMode = targetSeekMode;
  p->currentSeekModeTimeFormat = targetSeekModeTimeFormat;
  if (!p->reachedStopPoint) {
    p->operationErrorFlag1 = 0;
    p->operationErrorFlag2 = 0;
    p->operationErrorFlag3 = 0;
  }
  if (!(p->disc.loaded) || (p->currentDriveState <= 0x04) || (p->targetDriveState != 0x05)) {
    p->operationErrorFlag1 = 1;
    p->operationErrorFlag2 = 0;
    p->operationErrorFlag3 = 1;
    return 0;
  }
  if (p->currentSeekMode == 1) {
    int targetTrack = p->inputRegs[0x07];
    int targetTrackAsInt = ((p->disc.laserdisc) && (targetTrack == 0x00)) ? (uint8_t)(p->disc.first_track) : (uint8_t)ld_bcd_decode(targetTrack);
    if ((targetTrack > 0x99) || (targetTrackAsInt < (p->disc.first_track)) || (targetTrackAsInt > (p->disc.last_track))) {
      p->operationErrorFlag1 = 1;
      p->operationErrorFlag2 = 0;
      p->operationErrorFlag3 = 1;
      p->reachedStopPoint = 0;
      return 0;
    }
    int allowSeekToTime = !(p->disc.laserdisc) || (!p->currentSeekModeTimeFormat && (p->disc.digital_audio));
    p->seekPointRegs[0] = targetTrack;
    p->seekPointRegs[1] = 0x00;
    p->seekPointRegs[2] = (allowSeekToTime ? p->inputRegs[0x09] : (uint8_t)0x00);
    p->seekPointRegs[3] = (allowSeekToTime ? p->inputRegs[0x0A] : (uint8_t)0x00);
    p->seekPointRegs[4] = (allowSeekToTime ? p->inputRegs[0x0B] : (uint8_t)0x00);
    p->activeSeekMode = (uint8_t)1;
  }
  if (p->currentSeekMode == 2) {
    int seekToRedbookTimecode = !(p->disc.laserdisc) || (!p->currentSeekModeTimeFormat && (p->disc.laserdisc));
    int seekToVideoFrame = (p->disc.laserdisc) && p->currentSeekModeTimeFormat && !(p->disc.clv);
    int seekToVideoTime = (p->disc.laserdisc) && ((!(p->disc.clv) && !(p->disc.digital_audio)) || ((p->disc.clv) && (p->disc.digital_audio)));
    if (seekToRedbookTimecode) {
      p->seekPointRegs[0] = 0x00;
      p->seekPointRegs[1] = 0x00;
      p->seekPointRegs[2] = p->inputRegs[0x09];
      p->seekPointRegs[3] = p->inputRegs[0x0A];
      p->seekPointRegs[4] = p->inputRegs[0x0B];
      p->activeSeekMode = (uint8_t)0;
    } else if (seekToVideoFrame) {
      p->seekPointRegs[0] = 0x00;
      p->seekPointRegs[1] = p->inputRegs[0x08];
      p->seekPointRegs[2] = p->inputRegs[0x09];
      p->seekPointRegs[3] = p->inputRegs[0x0A];
      p->seekPointRegs[4] = 0x00;
      p->activeSeekMode = (uint8_t)2;
    } else if (seekToVideoTime) {
      p->seekPointRegs[0] = 0x00;
      p->seekPointRegs[1] = p->inputRegs[0x08] & 0x0F;
      p->seekPointRegs[2] = p->inputRegs[0x09];
      p->seekPointRegs[3] = p->inputRegs[0x0A];
      p->seekPointRegs[4] = p->inputRegs[0x0B];
      p->activeSeekMode = (uint8_t)3;
    }
  }
  return 1;
}
static void ld_performSeekWithLatchedState(kr_ld_player *p) {
  p->video.frameSkipBaseFrame = 0;
  p->video.frameSkipCounter = 0;
  switch (p->activeSeekMode) {
  case 0:

    ld_seekToTime(p, 0, ld_bcd_decode(p->seekPointRegs[2]), ld_bcd_decode(p->seekPointRegs[3]), ld_bcd_decode(p->seekPointRegs[4]), p->targetPauseState && !p->reachedStopPoint);
    p->seekPerformedSinceLastFrameUpdate = 1;
    break;
  case 1:

    ld_seekToRelativeTime(p, ld_bcd_decode(p->seekPointRegs[0]), ld_bcd_decode(p->seekPointRegs[2]), ld_bcd_decode(p->seekPointRegs[3]), ld_bcd_decode(p->seekPointRegs[4]), p->targetPauseState && !p->reachedStopPoint);
    p->seekPerformedSinceLastFrameUpdate = 1;
    break;
  case 2: {
    int32_t frameNumber = ((int32_t)ld_bcd_decode(p->seekPointRegs[1]) * 10000) + ((int32_t)ld_bcd_decode(p->seekPointRegs[2]) * 100) + (int32_t)ld_bcd_decode(p->seekPointRegs[3]);
    int aba = ld_abaFromZeroBasedFrameIndex(p, ld_max(1, frameNumber) - 1);

    ld_seekToSector(p, (aba - 150), p->targetPauseState && !p->reachedStopPoint);
    p->seekPerformedSinceLastFrameUpdate = 1;
    break;}
  case 3: {
    uint8_t videoTimeHours = ld_bcd_decode(p->seekPointRegs[1]);
    uint8_t videoTimeMinutes = ld_bcd_decode(p->seekPointRegs[2]);
    uint8_t videoTimeSeconds = ld_bcd_decode(p->seekPointRegs[3]);
    uint8_t videoTimeFrames = ld_bcd_decode(p->seekPointRegs[4]);

    ld_VideoTimeToRedbookTime(p, &videoTimeHours, &videoTimeMinutes, &videoTimeSeconds, &videoTimeFrames);
    ld_seekToTime(p, videoTimeHours, videoTimeMinutes, videoTimeSeconds, videoTimeFrames, p->targetPauseState && !p->reachedStopPoint);
    p->seekPerformedSinceLastFrameUpdate = 1;
    break; }
  }
}
static int32_t ld_zeroBasedFrameIndexFromABA(kr_ld_player *p, int32_t aba, int processLeadIn) {
  (void)p;
  if (!processLeadIn && (aba < 0)) {
    return 0;
  }
  int frameIndex = (int32_t)round(((double)aba / 75.0) * (30000.0 / 1001.0));
  frameIndex = (processLeadIn && (aba < 0)) ? (-frameIndex) - 1 : frameIndex;
  return frameIndex;
}
static int32_t ld_abaFromZeroBasedFrameIndex(kr_ld_player *p, int32_t frameIndex) {
  (void)p;
  int aba = (int32_t)round(((double)frameIndex / (30000.0 / 1001.0)) * 75.0);
  return aba;
}
static void ld_VideoTimeToRedbookTime(kr_ld_player *p, uint8_t *hours, uint8_t *minutes, uint8_t *seconds, uint8_t *frames) {
  (void)p;
  int videoTimeTotalFrames = ((((((int32_t)(*hours) * 60) + (int32_t)(*minutes)) * 60) + (int32_t)(*seconds)) * 30) + (int32_t)(*frames);
  int redbookTimeTotalFrames = (int32_t)round(((double)videoTimeTotalFrames / (30000.0 / 1001.0)) * 75.0);
  (*frames) = redbookTimeTotalFrames % 75;
  redbookTimeTotalFrames /= 75;
  (*seconds) = redbookTimeTotalFrames % 60;
  redbookTimeTotalFrames /= 60;
  (*minutes) = redbookTimeTotalFrames % 60;
  redbookTimeTotalFrames /= 60;
  (*hours) = redbookTimeTotalFrames;
}
static void ld_handleStopPointReached(kr_ld_player *p, int32_t aba) {
  (void)aba;
  if ((p->currentPlaybackMode == 2) && (p->currentPlaybackSpeed <= 1)) {
    return;
  }
  if (ld_bits(p->inputRegs[0x06], 7, 7)) {

    p->reachedStopPoint = 0;
    ld_play(p);
    ld_performSeekWithLatchedState(p);
  } else {

    p->outputRegs[0x1A] = 0xFF;
    p->outputRegs[0x1B] = 0xFF;
    p->outputRegs[0x1C] = 0xFF;
    p->outputRegs[0x1D] = 0xFF;
    p->outputRegs[0x1E] = 0xFF;
    p->outputRegs[0x1F] = 0x00;
    p->reachedStopPointPreviously = 1;
    p->operationErrorFlag1 = 0;
    p->operationErrorFlag2 = 1;
    p->operationErrorFlag3 = 1;
    p->reachedStopPoint = 1;
    ld_pause(p);
    p->stopPointEnabled = 0;
  }
}
static void ld_updateCurrentVideoFrameNumber(kr_ld_player *p, int32_t aba) {
  int justCompletedSeekFirstFrame = p->seekPerformedSinceLastFrameUpdate;
  p->seekPerformedSinceLastFrameUpdate = 0;
  int newVideoFrameIndex = ld_zeroBasedFrameIndexFromABA(p, aba, 1);
  int newVideoFrameLeadIn = (aba < 0);
  int newVideoFrameLeadOut = (newVideoFrameIndex >= (int32_t)p->disc.active_frames);
  if (newVideoFrameLeadOut) {
    newVideoFrameIndex = newVideoFrameIndex - p->disc.active_frames;
  }
  if (newVideoFrameLeadIn) {
    newVideoFrameIndex = (newVideoFrameIndex >= (int32_t)p->disc.lead_in_frames) ? (p->disc.lead_in_frames > 0 ? (int32_t)p->disc.lead_in_frames - 1 : 0) : newVideoFrameIndex;
  } else if (newVideoFrameLeadOut) {
    newVideoFrameIndex = (newVideoFrameIndex >= (int32_t)p->disc.lead_out_frames) ? (p->disc.lead_out_frames > 0 ? (int32_t)p->disc.lead_out_frames - 1 : 0) : newVideoFrameIndex;
  }
  int playerIsLoopingOnSingleFrame = (p->currentDriveState == 0x05) && !p->currentPauseState && (p->currentPlaybackMode == 0x2) && (p->currentPlaybackSpeed == 0x0);
  int newFrameIsSameAsLastFrame = (newVideoFrameIndex == p->video.currentVideoFrameIndex) && (newVideoFrameLeadIn == p->video.currentVideoFrameLeadIn) && (newVideoFrameLeadOut == p->video.currentVideoFrameLeadOut);
  if (!playerIsLoopingOnSingleFrame && newFrameIsSameAsLastFrame) {
    return;
  }
  if (!justCompletedSeekFirstFrame && (p->currentPlaybackMode == 0x01)) {
    int32_t newFrameSkipCounter = 0;
    switch (p->currentPlaybackSpeed) {
    case 0x00:
      newFrameSkipCounter = 1;
      break;
    case 0x01:
      newFrameSkipCounter = -1;
      p->currentPlaybackDirection = !p->currentPlaybackDirection;
      break;
    case 0x02:
      newFrameSkipCounter = 2;
      break;
    case 0x03:
      newFrameSkipCounter = 4;
      break;
    case 0x04:
      newFrameSkipCounter = 8;
      break;
    case 0x05:
      newFrameSkipCounter = 16;
      break;
    case 0x06:
      newFrameSkipCounter = 30;
      break;
    case 0x07:
      newFrameSkipCounter = 90;
      break;
    }
    if ((p->video.frameSkipBaseFrame == 0) || (p->video.frameSkipCounter != newFrameSkipCounter)) {
      p->video.frameSkipBaseFrame = newVideoFrameIndex;
      p->video.frameSkipCounter = newFrameSkipCounter;
    } else {
      int frameDeltaFromBase = abs((int)newVideoFrameIndex - (int)p->video.frameSkipBaseFrame);
      if ((frameDeltaFromBase % p->video.frameSkipCounter) != 0) {
        return;
      }
    }
  } else if ((p->currentPlaybackMode == 0x02) && (p->currentPlaybackSpeed == 0x01)) {
    p->currentPlaybackSpeed = 0;
  } else {
    p->video.frameSkipBaseFrame = 0;
    p->video.frameSkipCounter = 0;
  }
  if ((ld_bits(p->inputRegs[0x0C], 5, 5) && p->video.digitalMemoryFrameLatched) || ld_bits(p->inputRegs[0x0C], 2, 2)) {
    if (ld_bits(p->inputRegs[0x0C], 2, 2) && !ld_bits(p->inputRegs[0x0C], 5, 5)) {
      p->video.digitalMemoryFrameLatched = 0;
    }
    return;
  }
  int buildIndex = p->video.drawIndex ^ 0x01;
  int analogMixingMode = ld_bits(p->inputRegs[0x01], 6, 7);
  int newFieldSelectionEnabledState = (analogMixingMode >= 2) && (ld_bits(p->inputRegs[0x0C], 1, 1) || ld_bits(p->inputRegs[0x0C], 3, 3));
  int newFieldSelectionEvenField = (newFieldSelectionEnabledState ? ld_bits(p->inputRegs[0x0C], 0, 0) : 0);
  int fieldSelectionChanged = 0;
  if ((newFieldSelectionEnabledState != p->video.currentVideoFrameFieldSelectionEnabled[buildIndex]) || (newFieldSelectionEvenField != p->video.currentVideoFrameFieldSelectionEvenField[buildIndex])) {
    fieldSelectionChanged = 1;
    p->video.currentVideoFrameFieldSelectionEnabled[buildIndex] = newFieldSelectionEnabledState;
    p->video.currentVideoFrameFieldSelectionEvenField[buildIndex] = newFieldSelectionEvenField;
  }
  p->video.digitalMemoryFrameLatched = 1;
  if (!fieldSelectionChanged && newFrameIsSameAsLastFrame) {
    return;
  }
  p->video.currentVideoFrameIndex = newVideoFrameIndex;
  p->video.currentVideoFrameLeadIn = newVideoFrameLeadIn;
  p->video.currentVideoFrameLeadOut = newVideoFrameLeadOut;
  ld_latch_frame(p);

}


void kr_ld_player_init(kr_ld_player *p) { memset(p,0,sizeof(*p));kr_ld_player_reset(p,0); }
void kr_ld_player_set_hooks(kr_ld_player *p,kr_ld_event_fn event,void *opaque) { p->event=event;p->opaque=opaque; }
void kr_ld_player_reset(kr_ld_player *p,int gate_array_reset) {
  uint8_t av[20];kr_ld_disc_info disc=p->disc;kr_ld_event_fn event=p->event;void *opaque=p->opaque;
  kr_ld_video_state video=p->video;
  uint8_t attl=p->analogAudioAttenuationLeft, attr=p->analogAudioAttenuationRight;
  uint8_t fadel=p->analogAudioFadeToMutedLeft,fader=p->analogAudioFadeToMutedRight;
  memcpy(av,p->inputRegs+12,20);memset(p,0,sizeof(*p));p->disc=disc;p->event=event;p->opaque=opaque;
  p->currentDriveState=p->targetDriveState=2;p->cdd_status=disc.loaded?KR_LD_STOPPED:KR_LD_NODISC;
  if(gate_array_reset) {
    memcpy(p->inputRegs+12,av,20);p->video=video;
    p->analogAudioAttenuationLeft=attl;p->analogAudioAttenuationRight=attr;
    p->analogAudioFadeToMutedLeft=fadel;p->analogAudioFadeToMutedRight=fader;
  } else {
    p->inputRegs[26]=p->inputRegs[27]=p->inputRegs[28]=p->inputRegs[29]=255;
    p->video.currentVideoFrameIndex=-99999999;p->video.latched_file_frame=-1;
  }
}
void kr_ld_player_set_disc(kr_ld_player *p,const kr_ld_disc_info *disc) {
  p->disc=*disc;p->currentDriveState=p->targetDriveState=2;
  p->video.currentVideoFrameIndex=-99999999;p->video.latched_file_frame=-1;
  p->video.digitalMemoryFrameLatched=p->video.pendingFrame=0;
  p->stopPointEnabled=0;p->cdd_status=disc->loaded?KR_LD_STOPPED:KR_LD_NODISC;
}
void kr_ld_player_disc_ejected(kr_ld_player *p) {
  memset(&p->disc,0,sizeof(p->disc));p->currentDriveState=p->targetDriveState=1;
  p->cdd_status=KR_LD_NODISC;p->stopPointEnabled=0;
  p->video.latched_file_frame=-1;p->video.digitalMemoryFrameLatched=p->video.pendingFrame=0;
}
void kr_ld_player_set_timeline(kr_ld_player *p,int32_t lba,int sample_byte,int status,int track) {
  p->lba=lba;p->sample_byte=sample_byte;p->cdd_status=(uint8_t)status;p->current_track=(uint8_t)track;
}
void kr_ld_player_sector_advanced(kr_ld_player *p,int32_t lba) {
  p->lba=lba;p->sample_byte=0;ld_updateCurrentVideoFrameNumber(p,lba+150);
  if(p->stopPointEnabled && lba+150==p->targetStopPoint) ld_handleStopPointReached(p,lba+150);
}
int32_t kr_ld_player_advance_delta(kr_ld_player *p) {
  static const int rates[8]={1,2,3,8,14,20,1,1};
  static const int repeats[8]={0,1,2,4,8,16,30,90};
  int delta=1,speed=p->currentPlaybackSpeed&7;
  if(p->reachedStopPoint) return 0;
  if(p->currentPlaybackMode==2) {
    if(!speed) { delta=0;p->sectorRepeatCount=0; }
    else if(speed==1) { delta=1;p->sectorRepeatCount=0; }
    else { delta=(++p->sectorRepeatCount>=(unsigned)repeats[speed]);if(delta) p->sectorRepeatCount=0; }
    if(p->currentPlaybackDirection) delta=-delta;
  } else if(p->currentPlaybackMode==3) {
    delta=rates[speed];if(p->currentPlaybackDirection) delta=-delta;p->sectorRepeatCount=0;
  } else p->sectorRepeatCount=0;
  return delta;
}
void kr_ld_player_begin_field(kr_ld_player *p) {
  int build=p->video.drawIndex^1;
  p->video.currentVideoFrameInterlaced=!(p->inputRegs[12]&(32|8|2));
  if(p->video.pendingFrame) {
    p->video.currentVideoFrameFieldSelectionEnabled[p->video.drawIndex]=p->video.currentVideoFrameFieldSelectionEnabled[build];
    p->video.currentVideoFrameFieldSelectionEvenField[p->video.drawIndex]=p->video.currentVideoFrameFieldSelectionEvenField[build];
    p->video.drawIndex=(uint8_t)build;p->video.pendingFrame=0;p->video.currentVideoFrameOnEvenField=0;
  } else if(p->video.currentVideoFrameInterlaced) p->video.currentVideoFrameOnEvenField^=1;
  p->video.currentVideoFrameBlanked=!!(p->inputRegs[12]&4) || (p->currentPauseState && !(p->inputRegs[12]&32));
}
kr_ld_video_view kr_ld_player_video(const kr_ld_player *p) {
  kr_ld_video_view v;int draw=p->video.drawIndex;
  v.file_frame=p->video.latched_file_frame;v.analog_enabled=!!(p->inputRegs[1]&192);
  v.blanked=p->video.currentVideoFrameBlanked;v.field_selection=p->video.currentVideoFrameFieldSelectionEnabled[draw];
  v.even_field=v.field_selection?p->video.currentVideoFrameFieldSelectionEvenField[draw]:p->video.currentVideoFrameOnEvenField;
  v.interlaced=p->video.currentVideoFrameInterlaced;return v;
}
void kr_ld_player_picture_stop(kr_ld_player *p,uint32_t a,uint32_t b) {
  if(p->disc.clv || (p->inputRegs[12]&128) || (p->currentPlaybackMode==2 && p->currentPlaybackSpeed<=1)) return;
  if(a==0x82cfff || b==0x82cfff) {
    p->reachedStopPointPreviously=1;p->operationErrorFlag1=0;p->operationErrorFlag2=0;p->operationErrorFlag3=1;
    p->reachedStopPoint=1;p->stopPointEnabled=0;ld_pause(p);
  }
}
int32_t kr_ld_player_frame_from_aba(int32_t aba,int lead) { return ld_zeroBasedFrameIndexFromABA(NULL,aba,lead); }
int32_t kr_ld_player_aba_from_frame(int32_t f) { return ld_abaFromZeroBasedFrameIndex(NULL,f); }
static int16_t ld_clip(int value) { return (int16_t)(value>32767?32767:value< -32768?-32768:value); }
void kr_ld_player_mix_audio(const kr_ld_player *p,int16_t dl0,int16_t dr0,int16_t al0,int16_t ar0,int16_t *left,int16_t *right) {
  int dl=dl0,dr=dr0,al=al0,ar=ar0,mode=p->inputRegs[1]>>6,sel=p->inputRegs[13]>>6;
  int dflags=p->inputRegs[13],aflags=p->inputRegs[14];
  if(mode && (sel==2 || (mode==1 && (dflags&16)))) dl=dr=0;
  if(mode && sel!=3) { dl=dl*p->inputRegs[15]/255;dr=dr*p->inputRegs[15]/255; }
  if(mode && (dflags&3)==3) { dl/=2;dr/=2; }
  else if(dflags&1) dr=dl;else if(dflags&2) dl=dr;
  if(!mode || (mode==1 && !(dflags&16))) al=ar=0;
  if((aflags&128) || (aflags&3)==3) al=ar=0;
  else if(aflags&1) ar=al;else if(aflags&2) al=ar;
  al=al*(p->analogAudioFadeToMutedLeft?0:256-p->analogAudioAttenuationLeft)>>8;
  ar=ar*(p->analogAudioFadeToMutedRight?0:256-p->analogAudioAttenuationRight)>>8;
  if(p->currentPlaybackMode==2) dl=dr=al=ar=0;
  *left=ld_clip(dl+al);*right=ld_clip(dr+ar);
}
uint32_t kr_ld_player_mix_pixel(const kr_ld_player *p,uint32_t vdp,int backdrop,uint32_t analog) {
  unsigned f=p->inputRegs[backdrop?27:26]>>2,weight=(f<<10)|(f<<4)|(f>>2);
  unsigned c,shift;uint32_t out=0xff000000;
  if(!(p->inputRegs[1]&192)) return vdp;
  for(shift=0;shift<=16;shift+=8) {
    unsigned a=((analog>>shift)&255)*257,b=((vdp>>shift)&255)*257;
    c=(unsigned)(((uint64_t)b*weight)>>16)+(unsigned)(((uint64_t)a*(65536-weight))>>16);
    c=((c+128)-((c+128)>>8))>>8;out|=(c&255)<<shift;
  }
  return out;
}

uint32_t kr_ld_player_decode_vbi(const uint8_t *rgb,uint32_t width,uint32_t height,size_t pitch,unsigned line) {
  /* IEC 60857 24-bit biphase, same thresholds/search window as pinned ares. */
  double cell=(double)width*(2.0/(1000000.0/((30000.0/1001.0)*525.0)));
  unsigned start=(unsigned)(width*0.15),end=(unsigned)(width*0.25),quarter=(unsigned)(cell/4.0);
  unsigned x,i;uint32_t code=0;
  const uint8_t *row;
  if(!rgb || width<128 || line>=height || pitch<(size_t)width*3 || quarter==0) return 0;
  row=rgb+(size_t)line*pitch;
  for(;start<end;++start) {
    for(i=0;i<quarter && start+i<width;++i) {
      const uint8_t *v=row+(start+i)*3;
      if(v[0]<128 || v[1]<128 || v[2]<128) break;
    }
    if(i==quarter) break;
  }
  if(start==end || start<(unsigned)(cell/2.0)) return 0;
  start-=(unsigned)(cell/2.0);
  for(i=0;i<24;++i) {
    const uint8_t *a,*b;int av,bv;
    x=start+(unsigned)(cell*i+cell/4.0);
    if(x>=width) return 0;
    a=row+x*3;
    x=start+(unsigned)(cell*i+cell*3.0/4.0);
    if(x>=width) return 0;
    b=row+x*3;
    av=a[0]>=192 && a[1]>=192 && a[2]>=192;
    bv=b[0]>=192 && b[1]>=192 && b[2]>=192;
    if(av==bv) return 0;
    code=(code<<1)|(unsigned)bv;
  }
  return code;
}
void kr_ld_player_check_picture_stop(kr_ld_player *p,const uint8_t *rgb,uint32_t width,uint32_t height,size_t pitch) {
  uint32_t a,b;
  if(p->disc.clv || (p->inputRegs[12]&128) || (p->currentPlaybackMode==2 && p->currentPlaybackSpeed<=1)) return;
  a=kr_ld_player_decode_vbi(rgb,width,height,pitch,15);
  if(!a) a=kr_ld_player_decode_vbi(rgb,width,height,pitch,16);
  b=kr_ld_player_decode_vbi(rgb,width,height,pitch,278);
  if(!b) b=kr_ld_player_decode_vbi(rgb,width,height,pitch,279);
  kr_ld_player_picture_stop(p,a,b);
}
