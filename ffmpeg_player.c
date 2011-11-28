#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <arpa/inet.h>

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

#define FLOOR_THREE(X) ((X) & ~03)

AG_Surface *AG_SDL_ShadowSurface(SDL_Surface *ss); /* in ag_sdl_aux.c */

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
	int surf_w = FLOOR_THREE(AGWIDGET(me)->w > 0 ? AGWIDGET(me)->w : me->w);
	int surf_h = FLOOR_THREE(AGWIDGET(me)->h > 0 ? AGWIDGET(me)->h : me->h);

	if (me->surface_id != -1)
		AG_WidgetUnmapSurface(AGWIDGET(me), me->surface_id);

	if (me->frame->surface != NULL)
		SDL_FreeSurface(me->frame->surface);
	if (me->frame->overlay != NULL)
		SDL_FreeYUVOverlay(me->frame->overlay);

	if (me->flags & AG_FFMPEGPLAYER_KEEPRATIO) {
		/* FIXME */
		int film_w, film_h;
		float aspect;

		SDL_ffmpegGetVideoSize(me->file, &film_w, &film_h);
		aspect = (float)film_h / film_w;
		surf_h = surf_w * aspect;
	}

#if 0
	me->frame->overlay = SDL_CreateYUVOverlay(surf_w, surf_h,
						  SDL_YUY2_OVERLAY, me->screen);
	if (me->frame->overlay == NULL)
		/* FIXME */
		return -1;
	printf("overflay: %d\n", me->frame->overlay->hw_overlay);
#endif

	me->frame->surface = SDL_CreateRGBSurface(SDL_HWSURFACE,
						  surf_w, surf_h, 24,
						  htonl(0xFF000000),
						  htonl(0x00FF0000),
						  htonl(0x0000FF00), 0);
	if (me->frame->surface == NULL)
		/* FIXME */
		return -1;

#ifdef USE_SDL_SHADOWSURFACE
	me->surface = AG_SDL_ShadowSurface(me->frame->surface);
	if (me->surface == NULL)
		/* FIXME */
		return -1;

	me->surface_id = AG_WidgetMapSurfaceNODUP(AGWIDGET(me), me->surface);
	if (me->surface_id == -1)
		/* FIXME */
		return -1;
#else
	me->surface = NULL;
#endif

	return 0;
}

static void
audioCallback(void *data, Uint8 *stream, int length)
{
	ffmpegPlayer *me = data;
	SDL_ffmpegAudioFrame *frame;

	/* lock mutex, so audioFrame[0] will not be changed from another thread */
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

		me->curAudioFrame = (me->curAudioFrame + 1) % FFMPEGPLAYER_BUFSIZE;

		/* wake up buffer-fill thread */
		AG_CondSignal(&me->audio_cond);
	} else {
		/* no data available, just set output to zero */
		memset(stream, 0, length);
	}

	/* were done with the audio frame, release lock */
	AG_ObjectUnlock(me);
}

static void *
fillAudioBufferThread(void *data)
{
	ffmpegPlayer *me = data;

	AG_ObjectLock(me);

	while (!AG_CondWait(&me->audio_cond, &AGOBJECT(me)->lock)) {
		if (me->file == NULL)
			/* gracious shutdown */
			break;

		/* fill empty spaces in audio buffer */
		for (int i = (me->curAudioFrame + 1) % FFMPEGPLAYER_BUFSIZE;
		     i != me->curAudioFrame;
		     i = (i + 1) % FFMPEGPLAYER_BUFSIZE) {
			/* protect against spurious wakeups */
			if (me->audioFrame[i] == NULL)
				break;

			/* check if frame is empty */
			if (!me->audioFrame[i]->size)
				/* fill frame with new data */
				SDL_ffmpegGetAudioFrame(me->file, me->audioFrame[i]);
		}
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

int
ffmpegPlayerLoad(ffmpegPlayer *me, const char *path)
{
	AG_ObjectLock(me);

	if (me->file)
		SDL_ffmpegFree(me->file);

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

	if (resizePlayer(me)) {
		/* FIXME */
		AG_ObjectUnlock(me);
		return -1;
	}

	/* for the time being, redraw even when paused */
	AG_RedrawOnTick(AGWIDGET(me), 10);

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
	me->surface_id = -1;

	memset(me->audioFrame, 0, sizeof(me->audioFrame));
	me->curAudioFrame = 0;
	AG_CondInit(&me->audio_cond);
	if (AG_ThreadCreate(&me->audio_fillThread, fillAudioBufferThread, me))
		/* FIXME */
		return;

	me->file = NULL;
	me->frame = SDL_ffmpegCreateVideoFrame();
	if (me->frame == NULL)
		/* FIXME */
		return;

	me->playing = 0;
	me->sync = 0;
	me->offset = 0;
}

static void
Destroy(void *obj)
{
	ffmpegPlayer *me = obj;

	if (me->surface_id != -1)
		AG_WidgetUnmapSurface(AGWIDGET(me), me->surface_id);

	if (me->frame)
		/* also frees surface and/or overlay */
		SDL_ffmpegFreeVideoFrame(me->frame);

	if (me->file) {
		SDL_ffmpegFree(me->file);
		me->file = NULL;
	}

	/* shut down audio fill thread: it watches for me->file == NULL */
	AG_CondSignal(&me->audio_cond);
	AG_ThreadJoin(me->audio_fillThread, NULL);
	AG_CondDestroy(&me->audio_cond);

	for (int i = 0; i < FFMPEGPLAYER_BUFSIZE; i++) {
		if (me->audioFrame[i] != NULL)
			SDL_ffmpegFreeAudioFrame(me->audioFrame[i]);
	}
}

static void
Draw(void *p)
{
	ffmpegPlayer *me = p;

	if (me->file == NULL || me->frame == NULL)
		return;

	if (!me->frame->ready)
		SDL_ffmpegGetVideoFrame(me->file, me->frame);

	if (me->frame->ready &&
	    me->frame->pts <= getSync(me)) {
		if (me->frame->overlay != NULL) {
			/* FIXME */
		} else if (me->frame->surface != NULL) {
#ifdef USE_SDL_SHADOWSURFACE
			AG_WidgetUpdateSurface(AGWIDGET(me), me->surface_id);
#else
			if (me->surface != NULL)
				AG_SurfaceFree(me->surface);
			me->surface = AG_SurfaceFromSDL(me->frame->surface);
#endif
		}

		me->frame->ready = 0;
	}

	if (me->frame->overlay != NULL) {
		SDL_Rect rect = {
			.x = AGWIDGET(me)->rView.x1,
			.y = AGWIDGET(me)->rView.y1,
			.w = AGWIDGET(me)->w,
			.h = AGWIDGET(me)->h
		};
		SDL_DisplayYUVOverlay(me->frame->overlay, &rect);
	} else if (me->frame->surface != NULL) {
		int y = (AGWIDGET(me)->h - me->frame->surface->h) / 2;

#ifdef USE_SDL_SHADOWSURFACE
		AG_WidgetBlitSurface(AGWIDGET(me), me->surface_id, 0, y);
#else
		if (me->surface != NULL)
			AG_WidgetBlit(AGWIDGET(me), me->surface, 0, y);
#endif
	}
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
