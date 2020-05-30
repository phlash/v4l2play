// Poking about inside SDL2 to discover drivers etc.
#include <stdio.h>
#include <SDL.h>

int main(int argc, char **argv) {
	int n, i, dov=0 , doa=0;
	for (i=1; i<argc; i++) {
		if (argv[i][0]=='v')
			dov = 1;
		else if (argv[i][0]=='a')
			doa = 1;
		else
			puts("usage: sdlprobe [audio] [video]");
	}
	n = SDL_GetNumVideoDrivers();
	for (i=0; i<n; i++) {
		printf("SDL video(%d): %s\n", i, SDL_GetVideoDriver(i));
	}
	n = SDL_GetNumAudioDrivers();
	for (i=0; i<n; i++) {
		printf("SDL audio(%d): %s\n", i, SDL_GetAudioDriver(i));
	}
	if (dov) {
		SDL_InitSubSystem(SDL_INIT_VIDEO);
		printf("SDL chosen video: %s\n", SDL_GetCurrentVideoDriver());
	}
	if (doa) {
		SDL_InitSubSystem(SDL_INIT_AUDIO);
		printf("SDL chosen audio: %s\n", SDL_GetCurrentAudioDriver());
	}
	if (dov || doa) {
		printf("Press a key..");
		fflush(stdout);
		fgetc(stdin);
	}
	SDL_Quit();
	return 0;
}
