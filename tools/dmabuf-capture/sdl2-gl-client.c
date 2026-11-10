// sdl2-gl-client: a deliberately tiny SDL2 client for the dmabuf-capture server. It uses SDL2's Wayland video
// driver and an accelerated (GLES) renderer, so the frame is a real client DMA-BUF, not a test pattern made by the
// capture server. Build for the handheld; run only under tools/dmabuf-capture/capture-server.
//
//   sdl2-gl-client [seconds]        default 15

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 15;
	SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
	if (SDL_Init(SDL_INIT_VIDEO) != 0)
	{
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	printf("sdl2 video_driver=%s\n", SDL_GetCurrentVideoDriver());
	SDL_Window *w = SDL_CreateWindow("dmabuf-capture", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1240, 1080,
	                                 SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_OPENGL);
	if (!w)
	{
		fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Renderer *r = SDL_CreateRenderer(w, -1, SDL_RENDERER_ACCELERATED);
	if (!r)
	{
		fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
		return 1;
	}
	SDL_RendererInfo info;
	SDL_GetRendererInfo(r, &info);
	int ww = 0, wh = 0;
	SDL_GetWindowSize(w, &ww, &wh);
	printf("sdl2 window=%dx%d renderer=%s flags=0x%x\n", ww, wh, info.name ? info.name : "?", info.flags);
	for (int i = 0; i < secs * 60; i++)
	{
		SDL_SetRenderDrawColor(r, (Uint8)((i * 3) & 0xff), (Uint8)((i * 5) & 0xff), (Uint8)((i * 7) & 0xff), 255);
		SDL_RenderClear(r);
		SDL_Rect box = { (i * 7) % (ww > 200 ? ww - 200 : 1), wh / 3, 200, 200 };
		SDL_SetRenderDrawColor(r, 255, 220, 80, 255);
		SDL_RenderFillRect(r, &box);
		SDL_RenderPresent(r);
		SDL_Delay(16);
	}
	printf("sdl2 done\n");
	SDL_DestroyRenderer(r);
	SDL_DestroyWindow(w);
	SDL_Quit();
	return 0;
}
