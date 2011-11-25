#include <agar/core.h>
#include <agar/gui.h>
#include <agar/gui/cursors.h>
#include <agar/gui/sdl.h>

#include <SDL/SDL.h>
#include <SDL/SDL_ffmpeg.h>

AG_Surface *
AG_SDL_ShadowSurface(SDL_Surface *ss)
{
	AG_PixelFormat *pf;
	AG_Surface *ds;

	if ((pf = AG_SDL_GetPixelFormat(ss)) == NULL) {
		return (NULL);
	}
	if (pf->palette != NULL) {
		AG_SetError("Indexed formats not supported");
		AG_PixelFormatFree(pf);
		return (NULL);
	}

	if ((ds = AG_SurfaceNew(AG_SURFACE_PACKED, 0, 0, pf, 0))
	    == NULL) {
		goto out;
	}
	if (ss->flags & SDL_SRCCOLORKEY) { ds->flags |= AG_SRCCOLORKEY; }
	if (ss->flags & SDL_SRCALPHA) { ds->flags |= AG_SRCALPHA; }

	ds->pixels = ss->pixels;
	ds->w = ss->w;
	ds->h = ss->h;
	ds->pitch = ds->w * pf->BytesPerPixel;
	ds->clipRect = AG_RECT(0,0,ds->w,ds->h);

out:
	AG_PixelFormatFree(pf);
	return (ds);
}
