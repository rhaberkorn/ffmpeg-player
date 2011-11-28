#include <agar/core.h>
#include <agar/gui.h>

#include "ffmpeg_player.h"

void
playButtonPressed(AG_Event *event)
{
	ffmpegPlayerAction(AG_PTR(1), FFMPEGPLAYER_TOGGLE);
}

int
main(int argc, char *argv[])
{
	SDL_Surface *screen;
	int sdlFlags;

	AG_Window *win;
	ffmpegPlayer *player;

	/* check if we got an argument */
	if(argc < 2) {
		printf("usage: \"%s\" \"filename\"\n", argv[0]);
		return -1;
	}

	if (AG_InitCore("agarhello", 0) == -1)
		return 1;

	if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_TIMER) == -1) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}

	sdlFlags = SDL_RESIZABLE | SDL_HWSURFACE;
	if ((screen = SDL_SetVideoMode(640, 480, 32, sdlFlags)) == NULL) {
		fprintf(stderr, "SDL_SetVideoMode: %s\n", SDL_GetError());
		goto fail;
	}

	/* Initialize Agar-GUI to reuse display */
	if (AG_InitVideoSDL(screen, 0) == -1) {
		fprintf(stderr, "%s\n", AG_GetError());
		AG_Destroy();
		goto fail;
	}
	AG_BindGlobalKey(AG_KEY_ESCAPE, AG_KEYMOD_ANY, AG_QuitGUI);

	AG_RegisterClass(&ffmpegPlayerClass);

	win = AG_WindowNew(0);
	AG_LabelNew(win, 0, "Hello, world!");

	player = ffmpegPlayerNew(win, 320, 240, AG_FFMPEGPLAYER_EXPAND, screen);
	if (!player)
		goto fail;

	AG_ButtonNewFn(win, 0, "Play", playButtonPressed, "%p", player);
	
	if (ffmpegPlayerLoad(player, argv[1]))
		goto fail;

	AG_WindowShow(win);

	AG_EventLoop();

	AG_Destroy();
	SDL_Quit();
	return 0;

fail:

	AG_Destroy();
	SDL_Quit();
	return 1;
}
