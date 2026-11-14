/* Sega PAC connections to Genesis Plus GX. See player/LICENSE for the
 * LaserActive behavior and video calibration documented by ares/Nemesis. */
#include "shared.h"
#include "integration.h"
#include "player/ld_player.h"
#include "video_cache.h"
#include <limits.h>
#include <stddef.h>

extern int8 audio_hard_disable;
int kr_megald_active;
static megald_media *disc_image;
static const megald_media_side *side;
static unsigned side_index;
static kr_ld_player player;
static kr_ld_cache *cache;
static const uint8_t *rgb;
static kr_ld_video_view view;
static int decoded_frame = -1;
static unsigned x_table[720];
static int table_width, table_h40, table_border;
static uint64_t lead_samples;
extern uint8_t *kega_recording_bitmap;

static int track_at(int lba)
{
  int track = 0;
  while (track + 1 < cdd.toc.last && lba >= cdd.toc.tracks[track].end) ++track;
  return track;
}

static void drive_event(void *unused, enum kr_ld_event event, int32_t lba, int32_t pause)
{
  (void)unused;
  switch (event) {
    case KR_LD_EVENT_SEEK:
      if (lba < -150) lba = -150;
      if (lba >= cdd.toc.end) lba = cdd.toc.end - 1;
      cdd.latency = 20 + (unsigned)(10LL * abs(lba - cdd.lba) / cdd.toc.end);
      cdd.pending = 0;
      cdd.lba = lba;
      cdd.index = track_at(lba);
      cdd.status = pause ? CD_PAUSE : CD_PLAY;
      break;
    case KR_LD_EVENT_PLAY: cdd.status = CD_PLAY; break;
    case KR_LD_EVENT_PAUSE: cdd.status = CD_PAUSE; break;
    case KR_LD_EVENT_STOP: cdd.status = CD_STOP; cdd.latency = 0; break;
    case KR_LD_EVENT_EJECT: cdd.status = CD_OPEN; cdd.latency = 0; break;
    case KR_LD_EVENT_INSERT: cdd.status = CD_TOC; break;
  }
}

static void sync_timeline(void)
{
  int status = cdd.latency || cdd.pending ? KR_LD_SEEKING : cdd.status;
  if (status > KR_LD_OPEN) status = cdd.status == NO_DISC ? KR_LD_NODISC : KR_LD_STOPPED;
  int sample = (int)((uint64_t)cdd.cycles * 588 / 2000000) % 588;
  kr_ld_player_set_timeline(&player, cdd.lba, sample * 4, status, cdd.index + 1);
}

unsigned kr_megald_sides(void)
{
  return kr_megald_active ? megald_media_get_info(disc_image)->side_count : 0;
}

unsigned kr_megald_side(void)
{
  return side_index;
}

/* Prepare the new side before dropping the current one. No emulated CPU,
 * CD RAM, backup RAM or frontend session is reset during a physical flip. */
static int mount_side(unsigned index, char *boot_header)
{
  uint8_t sector[2352];
  const megald_media_side *candidate = megald_media_get_side(disc_image, index);
  if (!candidate || !candidate->has_video || candidate->width < 256 ||
      candidate->height != 525 || !candidate->track_count ||
      !candidate->digital_sectors || candidate->digital_sectors > INT_MAX)
    return 0;
  if (boot_header && (!megald_media_read_sector(disc_image, index, 0, sector) ||
                      memcmp(sector + 16, "SEGADISCSYSTEM", 14))) return 0;
  if (cache && side->width == candidate->width && side->height == candidate->height) {
    kr_ld_cache_select_side(cache, index);
  } else {
    kr_ld_cache *new_cache = kr_ld_cache_open(disc_image, index, candidate->width, candidate->height);
    if (!new_cache) return 0;
    kr_ld_cache_close(cache);
    cache = new_cache;
  }
  side = candidate;
  side_index = index;
  rgb = NULL;
  decoded_frame = -1;
  table_width = 0;
  memset(&view, 0, sizeof(view));
  memset(&cdd.toc, 0, sizeof(cdd.toc));
  if (boot_header) memcpy(boot_header, sector + 16, 0x210);
  cdd.toc.end = (int)side->digital_sectors;
  cdd.toc.last = side->track_count;
  kr_ld_disc_info layout = {0};
  layout.loaded = layout.laserdisc = 1;
  layout.clv = !strcmp(side->format, "NTSC-CLV");
  layout.digital_audio = side->has_digital_audio;
  layout.side = side->side_no;
  layout.first_track = side->tracks[0].number;
  layout.last_track = side->tracks[side->track_count-1].number;
  layout.lead_in_frames = side->lead_in_frames;
  layout.active_frames = side->active_frames;
  layout.lead_out_frames = side->lead_out_frames;
  layout.leadout_lba = cdd.toc.end;
  for (int i = 0; i < side->track_count; ++i) {
    const megald_media_track *t = &side->tracks[i];
    cdd.toc.tracks[i].start = (int)t->index01;
    cdd.toc.tracks[i].end = (int)t->end_lba;
    cdd.toc.tracks[i].type = t->is_data ? 1 : 0;
    layout.track_lba[t->number] = (int)t->index01;
    layout.track_control[t->number] = t->is_data ? 4 : 0;
  }
  cdd.toc.tracks[cdd.toc.last].start = cdd.toc.end;
  cdd.loaded = HW_ADDON_MEGACD;
  cdd.sectorSize = 2352;
  lead_samples = (uint64_t)side->lead_in_frames * 44100 * 1001 / 30000;
  kr_ld_player_set_disc(&player, &layout);
  return 1;
}

int kr_megald_select_side(unsigned index)
{
  if (index >= kr_megald_sides()) return 0;
  if (index == side_index) return 1;
  if (!mount_side(index, NULL)) return 0;
  cdd_reset();
  scd.regs[0x36>>1].byte.h = cdd.toc.tracks[0].type != 0;
  return 1;
}

int kr_megald_mount(const char *path, char *header)
{
  char reason[256];
  disc_image = megald_media_open(path, reason, sizeof(reason));
  if (!disc_image) return -1;
  kr_ld_player_init(&player);
  kr_ld_player_set_hooks(&player, drive_event, NULL);
  if (strcmp(megald_media_get_info(disc_image)->system, "MegaLD") || !mount_side(0, header)) goto fail;
  kr_megald_active = 1;
  return 1;
fail:
  kr_megald_close();
  return -1;
}

int kr_megald_japanese(void)
{
  return kr_megald_active && megald_media_get_info(disc_image)->region[0] == 'J';
}

void kr_megald_close(void)
{
  kr_ld_cache_close(cache);
  cache = NULL;
  if (disc_image) megald_media_close(disc_image);
  disc_image = NULL;
  side = NULL;
  side_index = 0;
  rgb = NULL;
  decoded_frame = -1;
  table_width = 0;
  memset(&view, 0, sizeof(view));
  kr_megald_active = 0;
}

void kr_megald_reset(int hard)
{
  kr_ld_player_reset(&player, !hard);
  decoded_frame = -1;
  rgb = NULL;
}

static unsigned read_byte(unsigned addr)
{
  if (!(addr & 1)) return 0xff;
  sync_timeline();
  return kr_ld_player_read(&player, addr);
}
static unsigned read_word(unsigned addr) { return read_byte(addr | 1); }
static void write_byte(unsigned addr, unsigned data)
{
  if (addr & 1) {
    sync_timeline();
    kr_ld_player_write(&player, addr, (uint8_t)data);
  }
}
static void write_word(unsigned addr, unsigned data) { write_byte(addr | 1, data); }

void kr_megald_map_bus(void)
{
  s68k.memory_map[0xfd].base = NULL;
  s68k.memory_map[0xfd].read8 = read_byte;
  s68k.memory_map[0xfd].read16 = read_word;
  s68k.memory_map[0xfd].write8 = write_byte;
  s68k.memory_map[0xfd].write16 = write_word;
}

void kr_megald_sector(uint8_t *dst)
{
  uint8_t sector[2352];
  if (cdd.lba >= 0 && megald_media_read_sector(disc_image, side_index, cdd.lba, sector))
    memcpy(dst, sector + 16, 2048);
  else memset(dst, 0, 2048);
}

void kr_megald_seek_complete(void)
{
  sync_timeline();
  kr_ld_player_sector_advanced(&player, cdd.lba);
}

void kr_megald_advance(void)
{
  int next = cdd.lba + kr_ld_player_advance_delta(&player);
  if (next >= cdd.toc.end) {
    cdd.status = CD_END;
    return;
  }
  cdd.lba = next < -150 ? -150 : next;
  cdd.index = track_at(cdd.lba);
  scd.regs[0x36>>1].byte.h = cdd.toc.tracks[cdd.index].type != 0;
  kr_megald_seek_complete();
}

/* The sample address is derived from the same drive clock that chooses the
 * video frame. No continuously running second playback clock survives a seek. */
void kr_megald_audio(unsigned samples, int *previous, int left_gain, int right_gain)
{
  int64_t pos = (int64_t)cdd.lba * 588 + (int64_t)(cdd.cycles % 2000000) * 588 / 2000000 - samples;
  int playing = cdd.status == CD_PLAY && !cdd.latency && !cdd.pending;
  for (unsigned done = 0; done < samples;) {
    int64_t lba = pos >= 0 ? pos / 588 : (pos - 587) / 588;
    unsigned offset = (unsigned)(pos - lba * 588);
    unsigned n = 588 - offset;
    if (n > samples - done) n = samples - done;
    int16_t analog[588*2] = {0};
    unsigned char digital[2352] = {0};
    if (playing && !audio_hard_disable) {
      int64_t source = pos + 150 * 588 + (int64_t)lead_samples;
      if (source >= 0 && side->has_analog_audio)
        megald_media_read_pcm(disc_image, side_index, source, analog, n);
      if (lba >= 0 && lba < cdd.toc.end && !cdd.toc.tracks[track_at((int)lba)].type)
        megald_media_read_sector(disc_image, side_index, lba, digital);
    }
    for (unsigned j = 0; j < n; ++j) {
      const unsigned char *p = digital + (offset + j) * 4;
      int16_t l, r;
      kr_ld_player_mix_audio(&player, (int16_t)(p[0] | p[1]<<8), (int16_t)(p[2] | p[3]<<8),
        analog[j*2], analog[j*2+1], &l, &r);
      cdd.audio[0] = l;
      cdd.audio[1] = r;
      int a = l * left_gain / 100, b = r * right_gain / 100;
      if (a != previous[0] || b != previous[1])
        blip_add_delta_fast(snd.blips[2], done+j, a-previous[0], b-previous[1]);
      previous[0] = a;
      previous[1] = b;
    }
    pos += n;
    done += n;
  }
  blip_end_frame(snd.blips[2], samples);
}

static void refresh_view(int check_stop)
{
  view = kr_ld_player_video(&player);
  if (view.file_frame < 0 || (unsigned)view.file_frame >= side->frame_count) {
    rgb = NULL;
    decoded_frame = -1;
    return;
  }
  if (view.file_frame == decoded_frame) return;
  rgb = kr_ld_cache_get(cache, view.file_frame);
  decoded_frame = view.file_frame;
  if (check_stop && rgb) kr_ld_player_check_picture_stop(&player, rgb, side->width, side->height, side->width * 3);
  int next = decoded_frame + (player.currentPlaybackDirection ? -1 : 1);
  if (next >= 0 && (unsigned)next < side->frame_count) kr_ld_cache_prefetch(cache, next);
}

void kr_megald_begin_field(void)
{
  kr_ld_player_begin_field(&player);
  refresh_view(1);
}

int kr_megald_scanline(int y, int output_y, const uint8_t *indices, const uint16_t *palette, int width)
{
  if (!(player.inputRegs[1] & 0xc0) || width < 1 || width > 720 || output_y < 0 ||
      output_y >= bitmap.height) return 0;
  const int h40 = reg[12] & 1, border = bitmap.viewport.x;
  if (table_width != width || table_h40 != h40 || table_border != border) {
    const int dots = h40 ? 4 : 5;
    /* These are coordinates within the mixed signal: the VDP scanline
     * starts after the border, with another 13 VDP pixels before active video.
     * A later viewport crop shifts both sources together. */
    const int left = h40 ? 17*4 + 13*4 : 8*5 + 13*5;
    for (int x=0; x<width; ++x) {
      int sx = 118 + ((left + (x-border)*dots)*(int)(side->width-118))/1495;
      x_table[x] = sx < 0 ? 0 : sx >= (int)side->width ? side->width-1 : (unsigned)sx;
    }
    table_width=width; table_h40=h40; table_border=border;
  }
  int row = y + 30 + (view.even_field ? 263 : 0);
  const uint8_t *line = rgb && !view.blanked && row >= 0 && row < (int)side->height ?
    rgb + (size_t)row * side->width * 3 : NULL;
  uint16_t *out = (uint16_t *)(bitmap.data + output_y*bitmap.pitch);
  for (int x=0; x<width; ++x) {
    unsigned fade = player.inputRegs[(indices[x] & 15) ? 26 : 27] >> 2;
    if (fade == 63) {
      out[x] = palette[indices[x]];
      continue;
    }
    const uint8_t *s = line ? line + x_table[x] * 3 : NULL;
    if (fade == 0) {
      out[x] = s ? (uint16_t)((s[0] >> 3) << 11 | (s[1] >> 2) << 5 | s[2] >> 3) : 0;
      continue;
    }
    const uint16_t px = palette[indices[x]];
    uint32_t vdp = (((px>>11)*255/31)<<16) | ((((px>>5)&63)*255/63)<<8) | ((px&31)*255/31);
    uint32_t analog = s ? ((uint32_t)s[0]<<16 | s[1]<<8 | s[2]) : 0;
    uint32_t c = kr_ld_player_mix_pixel(&player, vdp, !(indices[x]&15), analog);
    out[x] = (uint16_t)(((c>>8)&0xf800) | ((c>>5)&0x7e0) | ((c>>3)&31));
  }
  if (kega_recording_bitmap) memcpy(kega_recording_bitmap+output_y*bitmap.pitch,out,width*2);
  return 1;
}

unsigned kr_megald_save(uint8_t *out)
{
  /* Only emulated state between immutable disc metadata and host callbacks. */
  size_t size = offsetof(kr_ld_player,event) - offsetof(kr_ld_player,video);
  memcpy(out,&player.video,size);
  /* drawIndex uses one bit. Older saves have zero in the upper bits, i.e.
   * side A. This retains the original state size and the following PCM state. */
  out[offsetof(kr_ld_video_state, drawIndex)] |= (uint8_t)(side_index << 1);
  return (unsigned)size;
}
unsigned kr_megald_restore(const uint8_t *in)
{
  size_t size = offsetof(kr_ld_player,event) - offsetof(kr_ld_player,video);
  unsigned saved_side = in[offsetof(kr_ld_video_state, drawIndex)] >> 1;
  /* The surrounding CDD state was already restored. Only switch its media. */
  if (saved_side != side_index && !mount_side(saved_side, NULL)) return 0;
  memcpy(&player.video,in,size);
  player.video.drawIndex &= 1;
  decoded_frame = -1;
  refresh_view(0);
  return (unsigned)size;
}
