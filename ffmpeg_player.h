#ifndef __FFMPEG_PLAYER_H
#define __FFMPEG_PLAYER_H

#include <stdint.h>

#include <agar/core.h>
#include <agar/gui.h>

#include <SDL/SDL.h>
#include <SDL/SDL_ffmpeg.h>

typedef struct ffmpeg_player {
	struct ag_widget _inherit;	/* Inherit from AG_Widget */

	int w, h;			/* user-requested geometry */
	int flags;

	SDL_Surface *screen;
	AG_Surface *surface;
	int surface_id;

	SDL_ffmpegFile *file;
	SDL_ffmpegVideoFrame *frame;
	AG_Thread video_drawThread;

#define FFMPEGPLAYER_BUFSIZE 10
	SDL_ffmpegAudioFrame *audioFrame[FFMPEGPLAYER_BUFSIZE];
	int curAudioFrame;
	AG_Thread audio_fillThread;
	AG_Cond audio_cond;

	int playing;
	/* syncing */
	uint64_t sync, offset;
} ffmpegPlayer;

extern AG_WidgetClass ffmpegPlayerClass;

#define AG_FFMPEGPLAYER_HFILL		(1 << 0)
#define AG_FFMPEGPLAYER_VFILL		(1 << 1)
#define AG_FFMPEGPLAYER_EXPAND		(AG_FFMPEGPLAYER_HFILL | AG_FFMPEGPLAYER_VFILL)
#define AG_FFMPEGPLAYER_KEEPRATIO	(1 << 2)

ffmpegPlayer *ffmpegPlayerNew(void *parent, int w, int h, int flags,
			      SDL_Surface *screen);
int ffmpegPlayerLoad(ffmpegPlayer *me, const char *path);

enum ffmpegPlayerAction {
	FFMPEGPLAYER_PLAY,
	FFMPEGPLAYER_PAUSE,
	FFMPEGPLAYER_TOGGLE,
	FFMPEGPLAYER_GETPAUSE
};
int ffmpegPlayerAction(ffmpegPlayer *me, enum ffmpegPlayerAction action);

#endif
