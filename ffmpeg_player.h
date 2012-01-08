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

	SDL_ffmpegFile *file;

	int disp_w, disp_h;		/* size of frame on screen */
#define FFMPEGPLAYER_BUFSIZE 10
	SDL_ffmpegVideoFrame *videoFrame[FFMPEGPLAYER_BUFSIZE];
	int curVideoFrame;
	AG_Thread video_drawThread;
	AG_Thread video_fillThread;
	AG_Cond video_cond;

	SDL_ffmpegAudioFrame *audioFrame[FFMPEGPLAYER_BUFSIZE];
	int curAudioFrame;
	AG_Thread audio_fillThread;
	AG_Cond audio_cond;

	int playing;
	/* syncing */
	uint64_t sync, offset;
} ffmpegPlayer;

__BEGIN_DECLS

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

__END_DECLS

#endif