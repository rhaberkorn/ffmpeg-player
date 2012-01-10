#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <errno.h>

#include <agar/core.h>
#include <agar/gui.h>
#include <agar/gui/cursors.h>
#include <agar/gui/sdl.h>

#include <SDL/SDL.h>
#include <SDL/SDL_ffmpeg.h>

#include "ffmpeg_player.h"

#ifdef SDEBUG
#define DEBUG(FMT, ...)	do {				\
	fprintf(stderr, "%s(%d): " FMT "\n",		\
		__FUNCTION__, __LINE__, ##__VA_ARGS__);	\
} while (0)
#else
#define DEBUG(FMT, ...) do {} while(0)
#endif

#ifdef USE_OVERLAY
/* SDL does scaling and it's optimized for 2x */
#define SCALE_FLOOR(X) ((X) & ~01)
#else
/* SDL_ffmpeg does scaling */
#define SCALE_FLOOR(X) ((X) & ~03)
#endif

#define RR_INC(CUR, SIZE) \
	((CUR) = ((CUR) + 1) % (SIZE))

	/* in ag_sdl_aux.c */
AG_Surface *AG_SDL_ShadowSurface(SDL_Surface *ss);
void AG_SDL_ShadowSurfaceFree(AG_Surface *s);

/* returns the current position the file should be at */
static uint64_t
getSync(ffmpegPlayer *me)
{
	if (me->file == NULL)
		return 0;

	/*
	 * prefer audio syncing
	 */
	if (SDL_ffmpegValidAudio(me->file))
		return me->sync;

	/*
	 * fallback syncing
	 */
	/* FIXME: probably broken... */

	if (!me->playing)
		return 0;
	
	if (SDL_ffmpegValidVideo(me->file))
		return (SDL_GetTicks() % SDL_ffmpegDuration(me->file)) + me->offset;

	return 0;
}

ffmpegPlayer *
ffmpegPlayerNew(void *parent, int w, int h, int flags, SDL_Surface *screen)
{
	ffmpegPlayer *me;

	assert(screen != NULL);

	/* Create a new instance of the class */
	me = malloc(sizeof(ffmpegPlayer));
	AG_ObjectInit(me, &ffmpegPlayerClass);

	/* Save the user-requested geometry */
	me->w = w;
	me->h = h;
	me->flags = flags;
	me->screen = screen;

	if (flags & AG_FFMPEGPLAYER_HFILL)
		AGWIDGET(me)->flags |= AG_WIDGET_HFILL;
	if (flags & AG_FFMPEGPLAYER_VFILL)
		AGWIDGET(me)->flags |= AG_WIDGET_VFILL;

	/* Attach the object to the parent (no-op if parent is NULL) */
	AG_ObjectAttach(parent, me);

	return me;
}

static int
resizePlayer(ffmpegPlayer *me)
{
	/* resizePlayer may be called before any size allocation */
	int widget_w = AGWIDGET(me)->w > 0 ? AGWIDGET(me)->w : me->w;
	int widget_h = AGWIDGET(me)->h > 0 ? AGWIDGET(me)->h : me->h;

	if (me->flags & AG_FFMPEGPLAYER_KEEPRATIO) {
		int film_w, film_h;
		float aspect;

		SDL_ffmpegGetVideoSize(me->file, &film_w, &film_h);
		aspect = (float)film_h / film_w;

		if (widget_w * aspect > widget_h) {
			me->disp_w = SCALE_FLOOR((int)(widget_h / aspect));
			me->disp_h = SCALE_FLOOR(widget_h);
		} else {
			me->disp_w = SCALE_FLOOR(widget_w);
			me->disp_h = SCALE_FLOOR((int)(widget_w * aspect));
		}
	} else {
		me->disp_w = SCALE_FLOOR(widget_w);
		me->disp_h = SCALE_FLOOR(widget_h);
	}

#ifndef USE_OVERLAY
	for (int i = 0; i < FFMPEGPLAYER_BUFSIZE; i++) {
		SDL_ffmpegVideoFrame *frame = me->videoFrame[i];

		if (frame->surface != NULL)
			SDL_FreeSurface(frame->surface);

		frame->surface = SDL_CreateRGBSurface(SDL_HWSURFACE,
						      me->disp_w, me->disp_h, 24,
						      htonl(0xFF000000),
						      htonl(0x00FF0000),
						      htonl(0x0000FF00), 0x0);
		if (frame->surface == NULL)
			/* FIXME */
			return -1;

		SDL_ffmpegGetVideoFrame(me->file, frame);
	}
	me->curVideoFrame = 0;
#endif

	return 0;
}

static void *
drawVideoThread(void *data)
{
	ffmpegPlayer *me = data;

	AG_ObjectLock(me);

	for (;;) {
		SDL_ffmpegVideoFrame *frame = me->videoFrame[me->curVideoFrame];

		if (!frame->ready) {
			DEBUG("Video buffer underrun!");
			SDL_ffmpegGetVideoFrame(me->file, frame);
		}

		uint64_t sync = getSync(me);
		uint64_t pts = frame->pts;

		if (pts >= sync) {
			AG_ObjectUnlock(me);
			/* condition variable waiting may be faster, test with 500mhz */
			AG_Delay(pts - sync);
			AG_ObjectLock(me);
			if (me->file == NULL) {
				/* FIXME */
				break;
			}

			if (pts > getSync(me))
				continue;

#ifdef USE_OVERLAY
			if (frame->overlay != NULL) {
				if (AG_WidgetVisible(me)) {
					int frame_x = (AGWIDGET(me)->w - me->disp_w) / 2;
					int frame_y = (AGWIDGET(me)->h - me->disp_h) / 2;

					SDL_Rect rect = {
						.x = AGWIDGET(me)->rView.x1 + frame_x,
						.y = AGWIDGET(me)->rView.y1 + frame_y,
						.w = me->disp_w,
						.h = me->disp_h
					};
					SDL_DisplayYUVOverlay(frame->overlay, &rect);
				}
			}
#else
			if (frame->surface != NULL) {
#ifdef USE_SDL_SHADOWSURFACE
				if (me->surface != NULL)
					AG_SDL_ShadowSurfaceFree(me->surface);
				me->surface = AG_SDL_ShadowSurface(frame->surface);
#else
				if (me->surface != NULL)
					AG_SurfaceFree(me->surface);
				me->surface = AG_SurfaceFromSDL(frame->surface);
#endif
				if (me->surface == NULL) {
					/* FIXME */
					break;
				}

				AG_Redraw(AGWIDGET(me));
			}
#endif
		} else {
			/* frame is skipped */
			DEBUG("skip frame: %lums late", sync - frame->pts);
		}

		frame->ready = 0;
		RR_INC(me->curVideoFrame, FFMPEGPLAYER_BUFSIZE);

		/* wake up buffer-fill thread */
		AG_CondSignal(&me->video_cond);
	}

	AG_ObjectUnlock(me);
	return NULL;
}

static void *
fillVideoBufferThread(void *data)
{
	ffmpegPlayer *me = data;

	AG_ObjectLock(me);

	/* NOTE: AG_CondWait shouldn't return error codes */
	while (!AG_CondWait(&me->video_cond, &AGOBJECT(me)->lock)) {
		if (me->file == NULL)
			/* gracious shutdown */
			break;

		/* fill empty spaces in audio buffer */
		int i = me->curVideoFrame;
		do {
			/* protect against spurious wakeups */
			if (me->videoFrame[i] == NULL)
				break;

			/* check if frame is empty */
			if (!me->videoFrame[i]->ready)
				/* fill frame with new data */
				SDL_ffmpegGetVideoFrame(me->file, me->videoFrame[i]);
		} while (RR_INC(i, FFMPEGPLAYER_BUFSIZE) != me->curVideoFrame);
	}

	AG_ObjectUnlock(me);
	return NULL;
}

static void
audioCallback(void *data, Uint8 *stream, int length)
{
	/* TODO: try to use a dedicated audio mutex */
	ffmpegPlayer *me = data;
	SDL_ffmpegAudioFrame *frame;

	AG_ObjectLock(me);

	frame = me->audioFrame[me->curAudioFrame];

	if (!frame->size)
		DEBUG("Audio buffer underrun!");

	if (frame->size == length) {
		/* update sync */
		me->sync = frame->pts;

		/* copy the data to the output */
		memcpy(stream, frame->buffer, frame->size);

		/* mark data as used */
		frame->size = 0;

		RR_INC(me->curAudioFrame, FFMPEGPLAYER_BUFSIZE);
	} else {
		/* no data available, just set output to zero */
		memset(stream, 0, length);
	}

	/* wake up buffer-fill thread */
	AG_CondSignal(&me->audio_cond);

	AG_ObjectUnlock(me);
}

static void *
fillAudioBufferThread(void *data)
{
	ffmpegPlayer *me = data;

	AG_ObjectLock(me);

	/* NOTE: AG_CondWait shouldn't return error codes */
	while (!AG_CondWait(&me->audio_cond, &AGOBJECT(me)->lock)) {
		if (me->file == NULL)
			/* gracious shutdown */
			break;

		/* fill empty spaces in audio buffer */
		int i = me->curAudioFrame;
		do {
			/* protect against spurious wakeups */
			if (me->audioFrame[i] == NULL)
				break;

			/* check if frame is empty */
			if (!me->audioFrame[i]->size)
				/* fill frame with new data */
				SDL_ffmpegGetAudioFrame(me->file, me->audioFrame[i]);
		} while (RR_INC(i, FFMPEGPLAYER_BUFSIZE) != me->curAudioFrame);
	}
	/* FIXME: error handling */

	AG_ObjectUnlock(me);
	return NULL;
}

static int
initPlayerAudio(ffmpegPlayer *me)
{
	int frameSize;
	SDL_AudioSpec specs;

	SDL_CloseAudio();

	/* check if a valid audio stream was selected */
	if (!SDL_ffmpegValidAudio(me->file))
		return 0;

	specs = SDL_ffmpegGetAudioSpec(me->file, 512, audioCallback);
	specs.userdata = me;

	/* Open the Audio device */
	if (SDL_OpenAudio(&specs, NULL) < 0) {
		fprintf(stderr, "Couldn't open audio: %s\n", SDL_GetError());
		return -1;
	}

	/* calculate frame size ( 2 bytes per sample ) */
	frameSize = specs.channels * specs.samples * 2;

	/* prepare audio buffer */
	for (int i = 0; i < FFMPEGPLAYER_BUFSIZE; i++) {
		if (me->audioFrame[i] != NULL)
			SDL_ffmpegFreeAudioFrame(me->audioFrame[i]);

		/* create frame */
		me->audioFrame[i] = SDL_ffmpegCreateAudioFrame(me->file, frameSize);
		if (me->audioFrame[i] == NULL) {
			/* no frame could be created, this is fatal */
			return -1;
		}

		/* fill frame with data */
		SDL_ffmpegGetAudioFrame(me->file, me->audioFrame[i]);
	}
	me->curAudioFrame = 0;

	return 0;
}

static int
initPlayerVideo(ffmpegPlayer *me)
{
#ifdef USE_OVERLAY
	int film_w, film_h;

	SDL_ffmpegGetVideoSize(me->file, &film_w, &film_h);

	for (int i = 0; i < FFMPEGPLAYER_BUFSIZE; i++) {
		SDL_ffmpegVideoFrame *frame = me->videoFrame[i];

		if (frame->overlay != NULL)
			SDL_FreeYUVOverlay(frame->overlay);

		frame->overlay = SDL_CreateYUVOverlay(film_w, film_h,
						      SDL_YUY2_OVERLAY, me->screen);
		if (frame->overlay == NULL)
			/* FIXME */
			return -1;
		DEBUG("frame: %p, overlay: %s", frame,
		      frame->overlay->hw_overlay ? "hardware" : "software");

		SDL_ffmpegGetVideoFrame(me->file, frame);
	}
#endif

	return 0;
}

int
ffmpegPlayerLoad(ffmpegPlayer *me, const char *path)
{
	AG_ObjectLock(me);

	if (me->file) {
		SDL_ffmpegFree(me->file);

		/* shut down video draw thread */
		me->file = NULL;
		AG_ObjectUnlock(me);
		AG_ThreadJoin(me->video_drawThread, NULL);
		AG_ObjectLock(me);
	}

	me->file = SDL_ffmpegOpen(path);
	if (me->file == NULL) {
		/* FIXME */
		AG_ObjectUnlock(me);
		return -1;
	}

	/* FIXME: select best audio and video stream */
	SDL_ffmpegSelectVideoStream(me->file, 0);

	SDL_ffmpegSelectAudioStream(me->file, 0);

	if (initPlayerAudio(me)) {
		/* FIXME */
		AG_ObjectUnlock(me);
		return -1;
	}

	if (initPlayerVideo(me)) {
		/* FIXME */
		AG_ObjectUnlock(me);
		return -1;
	}

	if (resizePlayer(me)) {
		/* FIXME */
		AG_ObjectUnlock(me);
		return -1;
	}

	if (AG_ThreadCreate(&me->video_drawThread, drawVideoThread, me)) {
		/* FIXME */
		AG_ObjectUnlock(me);
		return -1;
	}

	AG_ObjectUnlock(me);
	return 0;
}

int
ffmpegPlayerAction(ffmpegPlayer *me, enum ffmpegPlayerAction action)
{
	int ret = 0;

	AG_ObjectLock(me);

	switch (action) {
	case FFMPEGPLAYER_PLAY:
		if (me->playing)
			break;

		me->playing = 1;
		if (SDL_ffmpegValidAudio(me->file))
			SDL_PauseAudio(0);
		break;

	case FFMPEGPLAYER_PAUSE:
		if (!me->playing)
			break;

		me->playing = 0;
		if (SDL_ffmpegValidAudio(me->file))
			SDL_PauseAudio(1);
		break;

	case FFMPEGPLAYER_TOGGLE:
		me->playing = !me->playing;
		if (SDL_ffmpegValidAudio(me->file))
			SDL_PauseAudio(!me->playing);
		break;

	case FFMPEGPLAYER_GETPAUSE:
		ret = me->playing;
		break;
	}

	AG_ObjectUnlock(me);
	return ret;
}

static void
SizeRequest(void *obj, AG_SizeReq *r)
{
	ffmpegPlayer *me = obj;

	r->w = me->w;
	r->h = me->h;
}

static int
SizeAllocate(void *obj, const AG_SizeAlloc *a)
{
	ffmpegPlayer *me = obj;

	return resizePlayer(me);
}

static void
Init(void *obj)
{
	ffmpegPlayer *me = obj;

	me->w = 0;
	me->h = 0;
	me->flags = 0;

	me->screen = NULL;
	me->surface = NULL;

	memset(me->audioFrame, 0, sizeof(me->audioFrame));
	me->curAudioFrame = 0;
	AG_CondInit(&me->audio_cond);
	if (AG_ThreadCreate(&me->audio_fillThread, fillAudioBufferThread, me))
		/* FIXME */
		return;

	for (int i = 0; i < FFMPEGPLAYER_BUFSIZE; i++)
		if ((me->videoFrame[i] = SDL_ffmpegCreateVideoFrame()) == NULL)
			/* FIXME */
			return;
	me->curVideoFrame = 0;
	AG_CondInit(&me->video_cond);
	if (AG_ThreadCreate(&me->video_fillThread, fillVideoBufferThread, me))
		/* FIXME */
		return;

	me->file = NULL;

	me->playing = 0;
	me->sync = 0;
	me->offset = 0;
}

static void
Destroy(void *obj)
{
	ffmpegPlayer *me = obj;

	AG_ObjectLock(me);

	if (me->file) {
		SDL_ffmpegFree(me->file);
		me->file = NULL;

		AG_ObjectUnlock(me);
		/* shut down video draw thread */
		AG_ThreadJoin(me->video_drawThread, NULL);
		AG_ObjectLock(me);
	}

	/* shut down audio/video fill threads: they watch for me->file == NULL */
	AG_CondSignal(&me->video_cond);
	AG_CondSignal(&me->audio_cond);
	AG_ObjectUnlock(me);

	AG_ThreadJoin(me->video_fillThread, NULL);
	AG_CondDestroy(&me->video_cond);

	for (int i = 0; i < FFMPEGPLAYER_BUFSIZE; i++) {
		if (me->videoFrame[i] != NULL)
			/* also frees surface and/or overlay */
			SDL_ffmpegFreeVideoFrame(me->videoFrame[i]);
	}

	AG_ThreadJoin(me->audio_fillThread, NULL);
	AG_CondDestroy(&me->audio_cond);

	for (int i = 0; i < FFMPEGPLAYER_BUFSIZE; i++) {
		if (me->audioFrame[i] != NULL)
			SDL_ffmpegFreeAudioFrame(me->audioFrame[i]);
	}

#ifndef USE_OVERLAY
	if (me->surface != NULL)
#ifdef USE_SDL_SHADOWSURFACE
		AG_SDL_ShadowSurfaceFree(me->surface);
#else
		AG_SurfaceFree(me->surface);
#endif
#endif
}

static void
Draw(void *p)
{
	ffmpegPlayer *me = p;
	SDL_ffmpegVideoFrame *frame = me->videoFrame[me->curVideoFrame];
	int frame_x, frame_y;

	if (me->file == NULL || frame == NULL)
		return;

	if (frame->overlay == NULL && frame->surface == NULL)
		return;

	frame_x = (AGWIDGET(me)->w - me->disp_w) / 2;
	frame_y = (AGWIDGET(me)->h - me->disp_h) / 2;

	if (frame_x || frame_y) {
		AG_Rect rect = {
			.x = 0,
			.y = 0,
			.w = AGWIDGET(me)->w,
			.h = AGWIDGET(me)->h
		};
		AG_DrawRectFilled(AGWIDGET(me), rect, AG_ColorRGB(0, 0, 0));
	}

#ifndef USE_OVERLAY
	if (me->surface != NULL)
		AG_WidgetBlit(AGWIDGET(me), me->surface, frame_x, frame_y);
#endif
}

AG_WidgetClass ffmpegPlayerClass = {
	{
		"AG_Widget:ffmpegPlayer",	/* Name of class */
		sizeof(ffmpegPlayer),	/* Size of structure */
		{ 0,0 },		/* Version for load/save */
		Init,			/* Initialize dataset */
		NULL,			/* Reinit before load */
		Destroy,		/* Release resources */
		NULL,			/* Load widget (for GUI builder) */
		NULL,			/* Save widget (for GUI builder) */
		NULL			/* Edit (for GUI builder) */
	},
	Draw,				/* Render widget */
	SizeRequest,			/* Default size requisition */
	SizeAllocate			/* Size allocation callback */
};
