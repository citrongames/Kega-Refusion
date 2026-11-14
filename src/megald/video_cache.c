#include "video_cache.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct kr_ld_cache {
  pthread_t thread;
  pthread_mutex_t lock;
  pthread_cond_t wake;
  megald_media *media;
  unsigned side;
  unsigned char *pixels[2];
  size_t bytes, stride;
  int front, front_frame, ready, requested, decoding, stopping;
};

static void *decode_worker(void *arg)
{
  kr_ld_cache *v = arg;
  pthread_mutex_lock(&v->lock);
  while (!v->stopping) {
    while (!v->stopping && (v->requested < 0 || v->requested == v->ready))
      pthread_cond_wait(&v->wake, &v->lock);
    if (v->stopping) break;
    int frame = v->requested;
    unsigned char *dst = v->pixels[1 - v->front];
    v->ready = -1;
    v->decoding = 1;
    pthread_mutex_unlock(&v->lock);
    int ok = megald_media_read_video(v->media, v->side, frame, dst, v->bytes, v->stride);
    pthread_mutex_lock(&v->lock);
    /* A read error is a deterministic blank frame, never a stalled waiter. */
    if (!ok) memset(dst, 0, v->bytes);
    v->decoding = 0;
    v->ready = frame;
    pthread_cond_broadcast(&v->wake);
  }
  pthread_mutex_unlock(&v->lock);
  return NULL;
}

kr_ld_cache *kr_ld_cache_open(megald_media *m, unsigned side, uint32_t width, uint32_t height)
{
  if (!width || width > 2048 || !height || height > 1125) return NULL;
  kr_ld_cache *v = calloc(1, sizeof(*v));
  if (!v) return NULL;
  v->media = m;
  v->side = side;
  v->stride = width * 3;
  v->bytes = v->stride * height;
  v->requested = v->ready = v->front_frame = -1;
  for (int i = 0; i < 2; i++) v->pixels[i] = malloc(v->bytes);
  if (!v->pixels[0] || !v->pixels[1] || pthread_mutex_init(&v->lock, NULL)) goto fail;
  if (pthread_cond_init(&v->wake, NULL)) goto fail_mutex;
  if (pthread_create(&v->thread, NULL, decode_worker, v)) goto fail_condition;
  return v;
fail_condition:
  pthread_cond_destroy(&v->wake);
fail_mutex:
  pthread_mutex_destroy(&v->lock);
fail:
  free(v->pixels[0]);
  free(v->pixels[1]);
  free(v);
  return NULL;
}

/* Discard pending prefetch and join any in-flight read before changing its
 * source. Both frame buffers and the decoder thread are reused. */
void kr_ld_cache_select_side(kr_ld_cache *v, unsigned side)
{
  pthread_mutex_lock(&v->lock);
  v->requested = -1;
  while (v->decoding) pthread_cond_wait(&v->wake, &v->lock);
  v->side = side;
  v->ready = v->front_frame = -1;
  pthread_mutex_unlock(&v->lock);
}

void kr_ld_cache_close(kr_ld_cache *v)
{
  if (!v) return;
  pthread_mutex_lock(&v->lock);
  v->stopping = 1;
  pthread_cond_broadcast(&v->wake);
  pthread_mutex_unlock(&v->lock);
  pthread_join(v->thread, NULL);
  pthread_cond_destroy(&v->wake);
  pthread_mutex_destroy(&v->lock);
  free(v->pixels[0]);
  free(v->pixels[1]);
  free(v);
}

const unsigned char *kr_ld_cache_get(kr_ld_cache *v, int frame)
{
  if (frame < 0) return NULL;
  if (frame == v->front_frame) return v->pixels[v->front];
  pthread_mutex_lock(&v->lock);
  v->requested = frame;
  pthread_cond_broadcast(&v->wake);
  while (v->ready != frame) pthread_cond_wait(&v->wake, &v->lock);
  v->front ^= 1;
  v->front_frame = frame;
  v->ready = v->requested = -1;
  pthread_mutex_unlock(&v->lock);
  return v->pixels[v->front];
}

void kr_ld_cache_prefetch(kr_ld_cache *v, int frame)
{
  if (frame < 0 || frame == v->front_frame) return;
  pthread_mutex_lock(&v->lock);
  v->requested = frame;
  pthread_cond_broadcast(&v->wake);
  pthread_mutex_unlock(&v->lock);
}
