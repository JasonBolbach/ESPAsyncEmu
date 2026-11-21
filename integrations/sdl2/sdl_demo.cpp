// SDL2 integration: AsyncEmu::poll() once per rendered frame.
//
// A TCP echo server + UDP echo listen on port 7007. The window shows live
// activity without needing a font library:
//   * the top row of squares = connected TCP clients (one square each)
//   * the bar below = bytes received recently (decays over time)
//   * the bottom flash = a UDP packet just arrived
// Drive it with:  nc 127.0.0.1 7007    /    echo hi | nc -u -w1 127.0.0.1 7007
// Close the window or press Esc to quit.
#include <SDL.h>

#include <algorithm>
#include <cstdio>

#include "ESPAsyncTCP.h"
#include "ESPAsyncUDP.h"

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("ESPAsyncEmu + SDL2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 240, 0);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!ren) {
        std::fprintf(stderr, "window/renderer failed: %s\n", SDL_GetError());
        return 1;
    }

    // UI state, mutated straight from network callbacks (same thread).
    int clients = 0;
    float activity = 0;   // 0..1, decays each frame
    float udpFlash = 0;   // 0..1, decays each frame

    AsyncServer server(7007);
    server.onClient([&](void*, AsyncClient* c) {
        ++clients;
        c->onData([&](void*, AsyncClient* cl, void* d, size_t n) {
            activity = std::min(1.0f, activity + n / 256.0f);
            cl->write(static_cast<const char*>(d), n);
        });
        c->onDisconnect([&](void*, AsyncClient* cl) { --clients; delete cl; });
    });
    server.begin();

    AsyncUDP udp;
    udp.listen(7007);
    udp.onPacket([&](AsyncUDPPacket& p) {
        udpFlash = 1.0f;
        p.write(p.data(), p.length());
    });

    bool running = true;
    while (running) {
        for (SDL_Event e; SDL_PollEvent(&e);) {
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) running = false;
        }

        AsyncEmu::poll();  // <-- the whole integration: once per frame

        activity *= 0.95f;
        udpFlash *= 0.90f;

        SDL_SetRenderDrawColor(ren, 24, 26, 32, 255);
        SDL_RenderClear(ren);
        for (int i = 0; i < clients && i < 20; ++i) {
            SDL_Rect r{16 + i * 30, 16, 24, 24};
            SDL_SetRenderDrawColor(ren, 80, 200, 120, 255);
            SDL_RenderFillRect(ren, &r);
        }
        SDL_Rect bar{16, 90, static_cast<int>(608 * activity), 40};
        SDL_SetRenderDrawColor(ren, 70, 140, 240, 255);
        SDL_RenderFillRect(ren, &bar);
        SDL_Rect flash{16, 170, 608, 40};
        SDL_SetRenderDrawColor(ren, static_cast<Uint8>(40 + 215 * udpFlash), static_cast<Uint8>(40 + 140 * udpFlash), 40, 255);
        SDL_RenderFillRect(ren, &flash);
        SDL_RenderPresent(ren);  // vsync paces the loop (~60 fps)
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
