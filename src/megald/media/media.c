/* Read-only MegaLD MMI backend. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _WIN32
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#endif
#include "media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <ctype.h>
#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#else
#include <sys/types.h>
#endif
#define QOI2_ALLOW_HEAP 0
#define QOI2_IMPLEMENTATION
#define QON_IMPLEMENTATION
#include "qon.h"
#include "qoi2.h"

#define MAX_ZIP_FILES 512
#define MAX_ZIP_NAME 1024
#define MAX_CENTRAL_SIZE (8u * 1024u * 1024u)
#define MAX_JSON_SIZE (1024u * 1024u)
#define MAX_CUE_SIZE (256u * 1024u)
#define MAX_JSON_TOKENS 16384
#define MAX_VIDEO_PIXELS (2048u * 2048u)
#define MAX_VIDEO_FRAMES 2000000u
#define MAX_REFERENCE_CHAIN 600u

typedef struct media_entry {
  char name[MAX_ZIP_NAME];
  uint64_t offset, size;
} media_entry;
typedef struct media_streams {
  media_entry video, audio, digital;
  qon_desc video_desc;
} media_streams;
struct megald_media {
  FILE *metadata_file, *video_file, *audio_file, *digital_file;
  uint64_t file_size;
  media_entry *entries;
  unsigned entry_count;
  megald_media_info info;
  megald_media_side sides[MEGALD_MEDIA_MAX_SIDES];
  media_streams streams[MEGALD_MEDIA_MAX_SIDES];
  unsigned char *compressed, *pixels, *reference;
  size_t compressed_capacity, pixel_capacity;
  int cached_side;
  uint32_t cached_frame;
};

static uint16_t rd16(const unsigned char *b) { return (uint16_t)(b[0] | (unsigned)b[1] << 8); }
static uint32_t rd32(const unsigned char *b) { return (uint32_t)rd16(b) | (uint32_t)rd16(b+2) << 16; }
static uint64_t rd64(const unsigned char *b) { return (uint64_t)rd32(b) | (uint64_t)rd32(b+4) << 32; }
static int range_ok(uint64_t off, uint64_t len, uint64_t size) { return off <= size && len <= size-off; }
static FILE *open_utf8(const char *path) {
#ifdef _WIN32
  int len;
  wchar_t *wide;
  FILE *f;
  if (!path || !(len=MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0))) return NULL;
  wide=(wchar_t *)malloc((size_t)len*sizeof(*wide));
  if (!wide) return NULL;
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, len)) { free(wide); return NULL; }
  f=_wfopen(wide,L"rb"); free(wide); return f;
#else
  return path ? fopen(path,"rb") : NULL;
#endif
}
static int seek_file(FILE *f, uint64_t off) {
  if (!f || off > INT64_MAX) return 0;
#ifdef _WIN32
  return _fseeki64(f,(__int64)off,SEEK_SET)==0;
#else
  return fseeko(f,(off_t)off,SEEK_SET)==0;
#endif
}
static int file_length(FILE *f, uint64_t *len) {
#ifdef _WIN32
  __int64 p;
  if (_fseeki64(f,0,SEEK_END) || (p=_ftelli64(f))<0) return 0;
#else
  off_t p;
  if (fseeko(f,0,SEEK_END) || (p=ftello(f))<0) return 0;
#endif
  *len=(uint64_t)p; return 1;
}
static int read_at(FILE *f, uint64_t off, void *out, size_t n) {
  return seek_file(f,off) && fread(out,1,n,f)==n;
}
static int entry_read(FILE *f, const media_entry *e, uint64_t off, void *out, size_t n) {
  return range_ok(off,(uint64_t)n,e->size) && read_at(f,e->offset+off,out,n);
}
static void error_set(char *out, size_t cap, const char *message) {
  if (out && cap) { size_t n=strlen(message); if(n>=cap)n=cap-1; memcpy(out,message,n);out[n]=0; }
}

/* ZIP64 offsets are parsed without mapping, extraction, or scanning media payloads. */
static int zip_open(megald_media *m) {
  unsigned char *tail=NULL, h[56], local[30], extra[65535];
  uint64_t tailoff, eocd=0, cd_off, cd_size, count, cd_end, pos;
  size_t tail_len, at;
  unsigned i;
  int found=0, ok=0;
  if (m->file_size < 22) return 0;
  tail_len=(size_t)(m->file_size < 65557 ? m->file_size : 65557);
  tailoff=m->file_size-tail_len;
  tail=(unsigned char *)malloc(tail_len);
  if (!tail || !read_at(m->metadata_file,tailoff,tail,tail_len)) goto done;
  for (at=tail_len-22;;--at) {
    if (rd32(tail+at)==0x06054b50 && at+22u+rd16(tail+at+20)==tail_len) { found=1;break; }
    if (!at) break;
  }
  if (!found) goto done;
  eocd=tailoff+at;
  if (rd16(tail+at+4) || rd16(tail+at+6) || rd16(tail+at+8)!=rd16(tail+at+10)) goto done;
  count=rd16(tail+at+10); cd_size=rd32(tail+at+12); cd_off=rd32(tail+at+16);
  if (count==0xffff || cd_size==0xffffffffu || cd_off==0xffffffffu) {
    uint64_t zip64;
    if (eocd<20 || !read_at(m->metadata_file,eocd-20,h,20) || rd32(h)!=0x07064b50 || rd32(h+4) || rd32(h+16)!=1) goto done;
    zip64=rd64(h+8);
    if (!range_ok(zip64,56,eocd-20) || !read_at(m->metadata_file,zip64,h,56) ||
        rd32(h)!=0x06064b50 || rd64(h+4)<44 || rd64(h+4)>UINT64_MAX-12 || !range_ok(zip64,rd64(h+4)+12,eocd-20) ||
        rd32(h+16) || rd32(h+20) || rd64(h+24)!=rd64(h+32)) goto done;
    count=rd64(h+32); cd_size=rd64(h+40); cd_off=rd64(h+48);
    eocd=zip64;
  }
  if (!count || count>MAX_ZIP_FILES || cd_size>MAX_CENTRAL_SIZE || !range_ok(cd_off,cd_size,eocd)) goto done;
  m->entries=(media_entry *)calloc((size_t)count,sizeof(*m->entries));
  if (!m->entries) goto done;
  m->entry_count=(unsigned)count; cd_end=cd_off+cd_size;pos=cd_off;
  for(i=0;i<(unsigned)count;++i) {
    uint16_t name_len, extra_len, comment_len, flags;
    uint32_t disk;
    uint64_t compressed, uncompressed, localoff, dataoff;
    size_t ex, n;
    media_entry *e=m->entries+i;
    if (!range_ok(pos,46,cd_end) || !read_at(m->metadata_file,pos,h,46) || rd32(h)!=0x02014b50) goto done;
    flags=rd16(h+8);
    /* MMI requires stored entries. Never silently treat compressed bytes as sectors. */
    if ((flags & (1|0x40|0x2000)) || rd16(h+10)!=0) goto done;
    compressed=rd32(h+20);uncompressed=rd32(h+24);
    name_len=rd16(h+28);extra_len=rd16(h+30);comment_len=rd16(h+32);disk=rd16(h+34);localoff=rd32(h+42);
    if (!name_len || name_len>=MAX_ZIP_NAME || !range_ok(pos+46,(uint64_t)name_len+extra_len+comment_len,cd_end) ||
        !read_at(m->metadata_file,pos+46,e->name,name_len)) goto done;
    if (memchr(e->name,0,name_len)) goto done;
    e->name[name_len]=0;
    for(n=0;n<i;++n) if(!strcmp(e->name,m->entries[n].name))goto done;
    if (extra_len && !read_at(m->metadata_file,pos+46+name_len,extra,extra_len)) goto done;
    found=0;
    for(ex=0;ex+4<=extra_len;) {
      size_t sz=rd16(extra+ex+2), p=ex+4, end=p+sz;
      uint16_t tag=rd16(extra+ex);
      if (end>extra_len) goto done;
      if(tag==1) {
        if(found)goto done;
        found=1;
#define ZIP64_FIELD(v,sentinel) if((v)==(sentinel)){ if(end-p<8)goto done;(v)=rd64(extra+p);p+=8; }
        ZIP64_FIELD(uncompressed,0xffffffffu)
        ZIP64_FIELD(compressed,0xffffffffu)
        ZIP64_FIELD(localoff,0xffffffffu)
#undef ZIP64_FIELD
        if(disk==0xffff){if(end-p<4)goto done;disk=rd32(extra+p);}
      }
      ex=end;
    }
    if(ex!=extra_len || disk || compressed!=uncompressed ||
       (!found && (compressed==0xffffffffu || uncompressed==0xffffffffu || localoff==0xffffffffu)) ||
       !range_ok(localoff,30,cd_off) || !read_at(m->metadata_file,localoff,local,30) ||
       rd32(local)!=0x04034b50 || rd16(local+8)!=0 || rd16(local+6)!=flags || rd16(local+26)!=name_len) goto done;
    dataoff=localoff+30u+rd16(local+26)+rd16(local+28);
    if (!range_ok(localoff+30,name_len,cd_off) || !range_ok(dataoff,uncompressed,cd_off)) goto done;
    /* Central/local names must agree; prevents ambiguous stream identities. */
    if (!read_at(m->metadata_file,localoff+30,extra,name_len) || memcmp(extra,e->name,name_len)) goto done;
    e->offset=dataoff;e->size=uncompressed;
    pos+=46u+name_len+extra_len+comment_len;
  }
  ok=(pos==cd_end);
done:
  free(tail);return ok;
}
static const media_entry *find_entry(const megald_media *m,const char *name) {
  unsigned i;for(i=0;i<m->entry_count;++i)if(!strcmp(m->entries[i].name,name))return m->entries+i;return NULL;
}

/* Small strict JSON tokenizer. Tokens and nesting have fixed upper bounds. */
typedef struct json_token { int start,end,next; char type; } json_token;
typedef struct json_reader { const char *s;int len,pos,count;json_token *t; } json_reader;
static void json_ws(json_reader *j) { while(j->pos<j->len && strchr(" \t\r\n",j->s[j->pos]))++j->pos; }
static int json_value(json_reader *j,int depth) {
  int token,k,start;
  char c;
  if(depth>32 || j->count>=MAX_JSON_TOKENS)return -1;
  json_ws(j);if(j->pos>=j->len)return -1;
  token=j->count++;start=j->pos;c=j->s[j->pos++];
  j->t[token].type=c;j->t[token].start=start;
  if(c=='{' || c=='[') {
    char end=(c=='{'?'}':']');
    json_ws(j);
    if(j->pos<j->len && j->s[j->pos]==end)++j->pos;
    else for(;;) {
      if(c=='{') {
        json_ws(j);if(j->pos>=j->len || j->s[j->pos]!='"')return -1;
        if(json_value(j,depth+1)<0)return -1;
        json_ws(j);if(j->pos>=j->len || j->s[j->pos++]!=':')return -1;
      }
      if(json_value(j,depth+1)<0)return -1;
      json_ws(j);if(j->pos>=j->len)return -1;
      if(j->s[j->pos]==end){++j->pos;break;}
      if(j->s[j->pos++]!=',')return -1;
    }
  } else if(c=='"') {
    for(;;) {
      unsigned char b;
      if(j->pos>=j->len)return -1;
      b=(unsigned char)j->s[j->pos++];
      if(b=='"')break;
      if(b<32)return -1;
      if(b=='\\') {
        if(j->pos>=j->len)return -1;
        b=(unsigned char)j->s[j->pos++];
        if(b=='u') {for(k=0;k<4;++k)if(j->pos>=j->len || !isxdigit((unsigned char)j->s[j->pos++]))return -1;}
        else if(!strchr("\"\\/bfnrt",b))return -1;
      }
    }
  } else if(c=='t' || c=='f' || c=='n') {
    const char *word=c=='t'?"true":c=='f'?"false":"null";int n=(int)strlen(word);
    if(start+n>j->len || memcmp(j->s+start,word,(size_t)n))return -1;
    j->pos=start+n;
  } else {
    j->t[token].type='0';j->pos=start;
    if(j->s[j->pos]=='-')++j->pos;
    if(j->pos>=j->len || !isdigit((unsigned char)j->s[j->pos]))return -1;
    if(j->s[j->pos]=='0')++j->pos;
    else while(j->pos<j->len && isdigit((unsigned char)j->s[j->pos]))++j->pos;
    if(j->pos<j->len && j->s[j->pos]=='.') {
      ++j->pos;k=j->pos;while(j->pos<j->len && isdigit((unsigned char)j->s[j->pos]))++j->pos;if(k==j->pos)return -1;
    }
    if(j->pos<j->len && (j->s[j->pos]=='e' || j->s[j->pos]=='E')) {
      ++j->pos;if(j->pos<j->len && (j->s[j->pos]=='+' || j->s[j->pos]=='-'))++j->pos;
      k=j->pos;while(j->pos<j->len && isdigit((unsigned char)j->s[j->pos]))++j->pos;if(k==j->pos)return -1;
    }
  }
  j->t[token].end=j->pos;j->t[token].next=j->count;return token;
}
static unsigned hex4(const char *s) {
  unsigned n=0;int i;for(i=0;i<4;++i){unsigned char c=(unsigned char)s[i];n=n*16+(c<='9'?(unsigned)(c-'0'):(unsigned)(tolower(c)-'a'+10));}return n;
}
static int json_string(const json_reader *j,int t,char *out,size_t cap) {
  int p,end;size_t n=0;unsigned cp;
  if(t<0 || j->t[t].type!='"' || !cap)return 0;
  p=j->t[t].start+1;end=j->t[t].end-1;
#define EMIT(b) do{if(n+1>=cap)return 0;out[n++]=(char)(b);}while(0)
  while(p<end) {
    unsigned char c=(unsigned char)j->s[p++];
    if(c!='\\'){EMIT(c);continue;}
    c=(unsigned char)j->s[p++];
    if(c=='u') {
      cp=hex4(j->s+p);p+=4;
      if(cp>=0xd800 && cp<=0xdbff) {
        unsigned low;
        if(end-p<6 || j->s[p]!='\\' || j->s[p+1]!='u')return 0;
        low=hex4(j->s+p+2);if(low<0xdc00 || low>0xdfff)return 0;
        cp=0x10000+((cp-0xd800)<<10)+low-0xdc00;p+=6;
      } else if(cp>=0xdc00 && cp<=0xdfff)return 0;
      if(!cp)return 0;
      if(cp<0x80){EMIT(cp);}
      else if(cp<0x800){EMIT(0xc0|(cp>>6));EMIT(0x80|(cp&63));}
      else if(cp<0x10000){EMIT(0xe0|(cp>>12));EMIT(0x80|((cp>>6)&63));EMIT(0x80|(cp&63));}
      else{EMIT(0xf0|(cp>>18));EMIT(0x80|((cp>>12)&63));EMIT(0x80|((cp>>6)&63));EMIT(0x80|(cp&63));}
    }else{
      if(c=='b')c='\b';else if(c=='f')c='\f';else if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';EMIT(c);
    }
  }
  out[n]=0;return 1;
#undef EMIT
}
static int json_get(const json_reader *j,int obj,const char *key) {
  int p,result=-1;char name[128];
  if(obj<0 || j->t[obj].type!='{')return -1;
  for(p=obj+1;p<j->t[obj].next;) {
    int v=j->t[p].next;
    if(v>=j->t[obj].next)return -1;
    if(json_string(j,p,name,sizeof(name)) && !strcmp(name,key)){if(result>=0)return -1;result=v;}
    p=j->t[v].next;
  }return result;
}
static int json_text(const json_reader *j,int obj,const char *key,char *out,size_t cap) {
  return json_string(j,json_get(j,obj,key),out,cap);
}
static int json_uint(const json_reader *j,int obj,const char *key,uint32_t *v) {
  int t=json_get(j,obj,key),p;uint64_t n=0;
  if(t<0 || j->t[t].type!='0')return 0;
  for(p=j->t[t].start;p<j->t[t].end;++p) {
    unsigned char c=(unsigned char)j->s[p];if(c<'0'||c>'9')return 0;
    n=n*10+c-'0';if(n>UINT32_MAX)return 0;
  }*v=(uint32_t)n;return 1;
}
static int json_int(const json_reader *j,int obj,const char *key,int *v) {
  uint32_t n;if(!json_uint(j,obj,key,&n) || n>INT_MAX)return 0;*v=(int)n;return 1;
}
static int time_lba(const char *s,int64_t *out) {
  unsigned long long mm,ss,ff;char c;
  if(sscanf(s,"%llu:%llu:%llu %c",&mm,&ss,&ff,&c)!=3 || mm>100000 || ss>59 || ff>74)return 0;
  *out=(int64_t)((mm*60+ss)*75+ff);return 1;
}
static int parse_cue(megald_media *m,const media_entry *cue,unsigned side) {
  char *data=NULL,*line,*next,*p,image[MAX_ZIP_NAME],name[MAX_ZIP_NAME];
  const media_entry *img=NULL;
  megald_media_side *si=m->sides+side;
  megald_media_track *track=NULL;
  int file_count=0,ok=0,i;
  if(!cue->size || cue->size>MAX_CUE_SIZE)return 0;
  data=(char *)malloc((size_t)cue->size+1);if(!data)return 0;
  if(!entry_read(m->metadata_file,cue,0,data,(size_t)cue->size) || memchr(data,0,(size_t)cue->size))goto done;
  data[cue->size]=0;
  for(line=data;line && *line;line=next) {
    char command[32],arg[32];int used=0;
    next=strchr(line,'\n');if(next)*next++=0;
    p=line;while(isspace((unsigned char)*p))++p;
    if(!*p)continue;
    if(sscanf(p,"%31s%n",command,&used)!=1)goto done;
    p+=used;
    while(isspace((unsigned char)*p))++p;
    if(!strcmp(command,"FILE")) {
      size_t len=0;const char *slash;
      if(++file_count!=1)goto done;
      if(*p=='"') {char *end=strchr(++p,'"');if(!end)goto done;len=(size_t)(end-p);if(len>=sizeof(image))goto done;memcpy(image,p,len);image[len]=0;p=end+1;}
      else {while(p[len] && !isspace((unsigned char)p[len]))++len;if(!len || len>=sizeof(image))goto done;memcpy(image,p,len);image[len]=0;p+=len;}
      if(sscanf(p,"%31s",arg)!=1 || strcmp(arg,"BINARY"))goto done;
      if(image[0]=='/' || image[0]=='\\' || strchr(image,':') || strchr(image,'\\') || strstr(image,"../"))goto done;
      slash=strrchr(cue->name,'/');
      len=slash?(size_t)(slash-cue->name)+1:0;
      if(len+strlen(image)>=sizeof(name))goto done;
      memcpy(name,cue->name,len);strcpy(name+len,image);img=find_entry(m,name);
      if(!img || !img->size || img->size%2352)goto done;
    } else if(!strcmp(command,"TRACK")) {
      int no;char extra;
      if(!img || sscanf(p,"%d %31s %c",&no,arg,&extra)!=2 || no!=si->track_count+1 || no>99)goto done;
      if(strcmp(arg,"AUDIO") && strcmp(arg,"MODE1/2352"))goto done;
      track=si->tracks+si->track_count++;track->number=no;track->is_data=strcmp(arg,"AUDIO")!=0;track->index00=track->index01=-1;
    } else if(!strcmp(command,"INDEX")) {
      int index;char time[64],extra;int64_t lba;
      if(!track || sscanf(p,"%d %63s %c",&index,time,&extra)!=2 || (index!=0 && index!=1) || !time_lba(time,&lba))goto done;
      if(index==0){if(track->index00>=0)goto done;track->index00=lba;}
      else{if(track->index01>=0)goto done;track->index01=lba;}
    } else if(strcmp(command,"REM") && strcmp(command,"TITLE") && strcmp(command,"PERFORMER") &&
              strcmp(command,"CATALOG") && strcmp(command,"ISRC") && strcmp(command,"FLAGS"))goto done;
  }
  if(!img || !si->track_count)goto done;
  si->digital_sectors=img->size/2352;
  if(si->digital_sectors>INT64_MAX)goto done;
  for(i=0;i<si->track_count;++i) {
    track=si->tracks+i;
    track->end_lba=i+1<si->track_count?(si->tracks[i+1].index00>=0?si->tracks[i+1].index00:si->tracks[i+1].index01):(int64_t)si->digital_sectors;
    if(track->index01<0 || track->index00>track->index01 || track->index01>=track->end_lba ||
       track->end_lba>(int64_t)si->digital_sectors ||
       (i && track->index01<=si->tracks[i-1].index01))goto done;
  }
  m->streams[side].digital=*img;si->has_digital_audio=1;ok=1;
done:free(data);return ok;
}
static int parse_side(megald_media *m,const json_reader *j,int object,unsigned side) {
  megald_media_side *si=m->sides+side;media_streams *st=m->streams+side;
  int array,t;char type[32];
  if(!json_text(j,object,"name",si->name,sizeof(si->name)) ||
     !json_text(j,object,"type",type,sizeof(type)) || strcmp(type,"LD") ||
     !json_text(j,object,"format",si->format,sizeof(si->format)) ||
     (strcmp(si->format,"NTSC-CAV") && strcmp(si->format,"NTSC-CLV")) ||
     !json_int(j,object,"sequenceNo",&si->sequence_no) ||
     !json_int(j,object,"volumeNo",&si->volume_no) || !json_int(j,object,"sideNo",&si->side_no))return 0;
  array=json_get(j,object,"streams");if(array<0 || j->t[array].type!='[')return 0;
  for(t=array+1;t<j->t[array].next;t=j->t[t].next) {
    char path[MAX_ZIP_NAME],format[32];const media_entry *e;
    if(!json_text(j,t,"type",type,sizeof(type)) || !json_text(j,t,"file",path,sizeof(path)) || !(e=find_entry(m,path)))return 0;
    if(!strcmp(type,"RawVideo")) {
      unsigned char header[24];uint64_t pixels;
      if(si->has_video || !json_text(j,t,"format",format,sizeof(format)) || (strcmp(format,"NTSC-J")&&strcmp(format,"NTSC")) ||
         !json_uint(j,t,"framesInLeadInRegion",&si->lead_in_frames) ||
         !json_uint(j,t,"framesInActiveRegion",&si->active_frames) ||
         !json_uint(j,t,"framesInLeadOutRegion",&si->lead_out_frames) ||
         !entry_read(m->metadata_file,e,0,header,sizeof(header)) || !qon_decode_header(header,sizeof(header),&st->video_desc))return 0;
      pixels=(uint64_t)st->video_desc.width*st->video_desc.height;
      if(pixels>MAX_VIDEO_PIXELS || st->video_desc.frame_count>MAX_VIDEO_FRAMES ||
         (st->video_desc.flags & ~(QON_FLAGS_USES_INTERFRAME_COMPRESSION|QON_FLAGS_LOOP_ANIMATION)) ||
         !range_ok(24,(uint64_t)st->video_desc.frame_count*8,e->size) ||
         !si->active_frames || si->lead_in_frames>st->video_desc.frame_count ||
         si->active_frames>st->video_desc.frame_count-si->lead_in_frames+1)return 0;
      st->video=*e;si->width=st->video_desc.width;si->height=st->video_desc.height;
      si->frame_count=st->video_desc.frame_count;si->frame_duration_us=st->video_desc.frame_duration_in_microseconds;si->has_video=1;
    }else if(!strcmp(type,"RawAudio")) {
      int channels;
      if(si->has_analog_audio || !json_text(j,t,"format",format,sizeof(format)) || strcmp(format,"s16le") ||
         !json_int(j,t,"channels",&channels) || channels!=2 || e->size%4)return 0;
      st->audio=*e;si->analog_sample_frames=e->size/4;si->has_analog_audio=1;
    }else if(!strcmp(type,"Redbook")) {
      if(si->has_digital_audio || !parse_cue(m,e,side))return 0;
    }else return 0;
  }
  return si->has_video && si->has_digital_audio;
}
static int parse_info(megald_media *m) {
  const media_entry *e=find_entry(m,"MediaInfo.json");char *data=NULL;json_token *tokens=NULL;json_reader j;
  int array,t,ok=0,i,k;
  if(!e || !e->size || e->size>MAX_JSON_SIZE)return 0;
  data=(char *)malloc((size_t)e->size+1);tokens=(json_token *)calloc(MAX_JSON_TOKENS,sizeof(*tokens));
  if(!data || !tokens || !entry_read(m->metadata_file,e,0,data,(size_t)e->size) || memchr(data,0,(size_t)e->size))goto done;
  data[e->size]=0;memset(&j,0,sizeof(j));j.s=data;j.len=(int)e->size;j.t=tokens;
  if(json_value(&j,0)!=0)goto done;
  json_ws(&j);if(j.pos!=j.len || j.t[0].type!='{')goto done;
  if(!json_text(&j,0,"name",m->info.name,sizeof(m->info.name)) ||
     !json_text(&j,0,"system",m->info.system,sizeof(m->info.system)) ||
     !json_text(&j,0,"regionCode",m->info.region,sizeof(m->info.region)) ||
     !json_text(&j,0,"catalogId",m->info.catalog_id,sizeof(m->info.catalog_id)))goto done;
  if(strcmp(m->info.system,"MegaLD") && strcmp(m->info.system,"LDROM2"))goto done;
  array=json_get(&j,0,"media");if(array<0 || j.t[array].type!='[')goto done;
  for(t=array+1;t<j.t[array].next;t=j.t[t].next) {
    if(m->info.side_count>=MEGALD_MEDIA_MAX_SIDES || !parse_side(m,&j,t,(unsigned)m->info.side_count))goto done;
    ++m->info.side_count;
  }
  if(!m->info.side_count)goto done;
  for(i=0;i<m->info.side_count;++i)for(k=i+1;k<m->info.side_count;++k) {
    if(m->sides[i].sequence_no==m->sides[k].sequence_no)goto done;
    if(m->sides[k].sequence_no<m->sides[i].sequence_no) {
      megald_media_side temp=m->sides[i];media_streams streams=m->streams[i];
      m->sides[i]=m->sides[k];m->streams[i]=m->streams[k];m->sides[k]=temp;m->streams[k]=streams;
    }
  }
  ok=1;
done:free(tokens);free(data);return ok;
}
megald_media *megald_media_open(const char *path,char *error,size_t cap) {
  megald_media *m=(megald_media *)calloc(1,sizeof(*m));const char *message="Out of memory";
  if(!m){error_set(error,cap,message);return NULL;}
  m->cached_side=-1;
  m->metadata_file=open_utf8(path);
  message="Cannot open MMI image";
  if(!m->metadata_file || !file_length(m->metadata_file,&m->file_size))goto fail;
  message="Invalid or unsupported stored ZIP64 MMI archive";
  if(!zip_open(m))goto fail;
  message="Invalid or unsupported MMI media metadata, CUE, or QON header";
  if(!parse_info(m))goto fail;
  m->video_file=open_utf8(path);m->audio_file=open_utf8(path);m->digital_file=open_utf8(path);
  message="Cannot open independent MMI streaming readers";
  if(!m->video_file || !m->audio_file || !m->digital_file)goto fail;
  fclose(m->metadata_file);m->metadata_file=NULL;
  free(m->entries);m->entries=NULL;m->entry_count=0;
  error_set(error,cap,"");return m;
fail:error_set(error,cap,message);megald_media_close(m);return NULL;
}
void megald_media_close(megald_media *m) {
  if(!m)return;
  if(m->metadata_file)fclose(m->metadata_file);
  if(m->video_file)fclose(m->video_file);
  if(m->audio_file)fclose(m->audio_file);
  if(m->digital_file)fclose(m->digital_file);
  free(m->entries);free(m->compressed);free(m->pixels);free(m->reference);free(m);
}
const megald_media_info *megald_media_get_info(const megald_media *m) { return m?&m->info:NULL; }
const megald_media_side *megald_media_get_side(const megald_media *m,unsigned side) {
  return m && side<(unsigned)m->info.side_count?m->sides+side:NULL;
}
int megald_media_read_sector(megald_media *m,unsigned side,uint64_t lba,unsigned char out[2352]) {
  const megald_media_side *s=megald_media_get_side(m,side);
  return s && s->has_digital_audio && out && lba<s->digital_sectors &&
    entry_read(m->digital_file,&m->streams[side].digital,lba*2352,out,2352);
}
static int frame_index(megald_media *m,unsigned side,uint32_t frame,uint64_t *off,unsigned *flags) {
  unsigned char index[8];media_streams *st=m->streams+side;uint64_t value,base=24+(uint64_t)st->video_desc.frame_count*8;
  if(frame>=st->video_desc.frame_count || !entry_read(m->video_file,&st->video,24+(uint64_t)frame*8,index,8))return 0;
  value=rd64(index);*flags=(unsigned)(value>>48);*off=value&UINT64_C(0xffffffffffff);
  if((*flags & ~(unsigned)QON_FRAME_FLAGS_INTERFRAME_COMPRESSION) || !range_ok(base,*off,st->video.size))return 0;
  *off+=base;return range_ok(*off,4,st->video.size);
}
static int decode_frame(megald_media *m,unsigned side,uint32_t frame,uint64_t off,unsigned flags) {
  unsigned char size_data[4];uint32_t size;size_t max_size; qoi2_desc desc;
  media_streams *st=m->streams+side;unsigned char *tmp;
  desc.width=st->video_desc.width;desc.height=st->video_desc.height;desc.channels=st->video_desc.channels;desc.colorspace=st->video_desc.colorspace;
  max_size=qoi2_max_encoded_data_size_without_header(&desc);
  if(!entry_read(m->video_file,&st->video,off,size_data,4))return 0;
  size=rd32(size_data);
  if(!size || size>max_size || !range_ok(off+4,size,st->video.size))return 0;
  if(size>m->compressed_capacity) {
    unsigned char *buffer=(unsigned char *)realloc(m->compressed,max_size);
    if(!buffer)return 0;
    m->compressed=buffer;m->compressed_capacity=max_size;
  }
  if(!entry_read(m->video_file,&st->video,off+4,m->compressed,size))return 0;
  if((flags & QON_FRAME_FLAGS_INTERFRAME_COMPRESSION) && (m->cached_side!=(int)side || !frame || m->cached_frame!=frame-1))return 0;
  if(!qoi2_decode_data(m->compressed,size,&desc,(flags & QON_FRAME_FLAGS_INTERFRAME_COMPRESSION)?m->reference:NULL,m->pixels,3))return 0;
  tmp=m->reference;m->reference=m->pixels;m->pixels=tmp;m->cached_side=(int)side;m->cached_frame=frame;return 1;
}
int megald_media_read_video(megald_media *m,unsigned side,uint32_t frame,unsigned char *rgb,size_t out_size,size_t stride) {
  const megald_media_side *s=megald_media_get_side(m,side);
  uint64_t off;unsigned flags;size_t row,pixels;uint32_t first,current;
  if(!s || !s->has_video || !rgb || frame>=s->frame_count || stride<(size_t)s->width*3 ||
     (s->height>1 && stride>(SIZE_MAX-(size_t)s->width*3)/(s->height-1)) ||
     out_size<(s->height-1)*stride+(size_t)s->width*3)return 0;
  pixels=(size_t)s->width*s->height*3;
  if(pixels>m->pixel_capacity) {
    unsigned char *a=(unsigned char *)malloc(pixels),*b=(unsigned char *)malloc(pixels);
    if(!a||!b){free(a);free(b);return 0;}
    free(m->pixels);free(m->reference);m->pixels=a;m->reference=b;m->pixel_capacity=pixels;m->cached_side=-1;
  }
  if(m->cached_side!=(int)side || m->cached_frame!=frame) {
    first=frame;
    for(;;) {
      if(!frame_index(m,side,first,&off,&flags))return 0;
      if(!(flags & QON_FRAME_FLAGS_INTERFRAME_COMPRESSION))break;
      if(m->cached_side==(int)side && first>0 && m->cached_frame==first-1)break;
      if(!first || frame-first>=MAX_REFERENCE_CHAIN)return 0;
      --first;
    }
    for(current=first;;++current) {
      if(!frame_index(m,side,current,&off,&flags) || !decode_frame(m,side,current,off,flags)){m->cached_side=-1;return 0;}
      if(current==frame)break;
    }
  }
  for(row=0;row<s->height;++row)memcpy(rgb+row*stride,m->reference+row*(size_t)s->width*3,(size_t)s->width*3);
  return 1;
}
size_t megald_media_read_pcm(megald_media *m,unsigned side,uint64_t sample,int16_t *stereo,size_t count) {
  const megald_media_side *s=megald_media_get_side(m,side);unsigned char *bytes=(unsigned char *)stereo;size_t i;
  if(!s || !s->has_analog_audio || !stereo || sample>=s->analog_sample_frames)return 0;
  if(count>s->analog_sample_frames-sample)count=(size_t)(s->analog_sample_frames-sample);
  if(count>SIZE_MAX/4)return 0;
  if(!entry_read(m->audio_file,&m->streams[side].audio,sample*4,stereo,count*4))return 0;
  /* Conversion also supports big-endian hosts without aliasing or alignment assumptions. */
  for(i=0;i<count*2;++i) { uint16_t v=rd16(bytes+2*i);int16_t signed_value;memcpy(&signed_value,&v,2);stereo[i]=signed_value; }
  return count;
}
