#include "../ld_player.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int seeks,plays,pauses;static int32_t last_seek;static int last_paused;
static void event(void *user,enum kr_ld_event e,int32_t a,int32_t b) {
  (void)user;if(e==KR_LD_EVENT_SEEK) { ++seeks;last_seek=a;last_paused=b; }
  else if(e==KR_LD_EVENT_PLAY) ++plays;else if(e==KR_LD_EVENT_PAUSE) ++pauses;
}
static void wr(kr_ld_player *p,int reg,int data) { kr_ld_player_write(p,0xfd0001u+reg*2,(uint8_t)data); }
static int rd(kr_ld_player *p,int reg) { return kr_ld_player_read(p,0xfd0081u+reg*2); }
int main(void) {
  kr_ld_player p;kr_ld_disc_info d;kr_ld_video_view v;int i;int16_t l,r;
  memset(&d,0,sizeof(d));d.loaded=d.laserdisc=d.digital_audio=1;d.side=1;
  d.lead_in_frames=10;d.active_frames=54000;d.lead_out_frames=10;
  d.first_track=1;d.last_track=2;d.track_lba[1]=0;d.track_lba[2]=1000;
  d.track_control[1]=4;d.leadout_lba=135000;
  kr_ld_player_init(&p);kr_ld_player_set_disc(&p,&d);kr_ld_player_set_hooks(&p,event,0);
  assert(rd(&p,0)==128);assert(rd(&p,2)==128);assert(rd(&p,5)==127);
  /* Batch-latched seek starts from the new seek target when spinning up. */
  wr(&p,0,0);wr(&p,6,6);wr(&p,7,0);wr(&p,8,0);wr(&p,9,1);wr(&p,10,0);wr(&p,11,0);wr(&p,2,0x25);
  assert(seeks==0);wr(&p,0,128);assert(seeks>=1);
  assert(last_seek==kr_ld_player_aba_from_frame(99)-150);assert(last_paused==0);assert(plays);
  for(i=0;i<16;++i) (void)rd(&p,6);
  assert((rd(&p,6)&0xaf)==0xa5); /* Seek remains busy after mechanical delay. */
  kr_ld_player_set_timeline(&p,last_seek,0,KR_LD_PLAYING,1);
  kr_ld_player_sector_advanced(&p,last_seek);kr_ld_player_begin_field(&p);v=kr_ld_player_video(&p);
  assert(v.file_frame==109);assert(!(rd(&p,6)&0xa0));assert(rd(&p,23)==1 && rd(&p,24)==0);
  /* Input/output freeze preserves snapshots and commits input only on unfreeze. */
  wr(&p,0,192);i=rd(&p,24);kr_ld_player_set_timeline(&p,500,0,KR_LD_PLAYING,1);assert(rd(&p,24)==i);
  wr(&p,0,128);assert(rd(&p,24)!=i);
  kr_ld_player_write(&p,0xfd00bf,0x55);for(i=0;i<6;++i) assert(rd(&p,31)==0x55);
  kr_ld_player_write(&p,0xfd008b,0x12);for(i=0;i<5;++i) assert(rd(&p,5)==0x12);assert(rd(&p,5)==127);
  /* Hold locks the display while the reported transport time keeps advancing. */
  wr(&p,12,32);kr_ld_player_sector_advanced(&p,600);kr_ld_player_begin_field(&p);assert(kr_ld_player_video(&p).file_frame==v.file_frame);
  wr(&p,12,0);kr_ld_player_sector_advanced(&p,601);kr_ld_player_begin_field(&p);assert(kr_ld_player_video(&p).file_frame!=v.file_frame);
  /* Single-step is consumed when a new video frame is latched. */
  wr(&p,3,0x21);assert(kr_ld_player_advance_delta(&p)==1);kr_ld_player_sector_advanced(&p,605);
  assert(p.currentPlaybackSpeed==0);assert(kr_ld_player_advance_delta(&p)==0);assert(rd(&p,7)==0x20);
  wr(&p,3,0x32);assert(kr_ld_player_advance_delta(&p)==3);wr(&p,3,0x3a);assert(kr_ld_player_advance_delta(&p)==-3);
  wr(&p,3,0);wr(&p,1,64);wr(&p,13,16);kr_ld_player_mix_audio(&p,1000,-1000,10000,-10000,&l,&r);assert(l==10000 && r==-10000);
  wr(&p,30,2);wr(&p,31,128);kr_ld_player_mix_audio(&p,1000,-1000,10000,-10000,&l,&r);assert(l==5000 && r==-5000);
  wr(&p,13,0);wr(&p,15,255);kr_ld_player_mix_audio(&p,1000,-1000,10000,-10000,&l,&r);assert(l==1000 && r==-1000);
  wr(&p,26,0);assert((kr_ld_player_mix_pixel(&p,0xff0000,0,0x123456)&0xffffff)==0x123456);
  wr(&p,26,255);assert((kr_ld_player_mix_pixel(&p,0xff0000,0,0x123456)&0xffffff)==0xff0000);
  kr_ld_player_reset(&p,1);assert(p.inputRegs[1]==0 && p.inputRegs[15]==255 && p.inputRegs[26]==255 && p.analogAudioAttenuationLeft==128);
  wr(&p,0,128);wr(&p,3,0);kr_ld_player_picture_stop(&p,0x82cfff,0);assert(p.reachedStopPoint && p.cdd_status==KR_LD_PAUSED && pauses);
  assert(kr_ld_player_frame_from_aba(0,0)==0);assert(kr_ld_player_aba_from_frame(30000)==75075);
  puts("PD6103A register/seek/timeline/hold/step/mix tests passed");return 0;
}
