// Loads and decodes the selected gallery picture on a worker thread.
#pragma once
#include <3ds.h>

// Full size of the picture on screen. The view may show it reduced.
extern int g_gallerySourceWidth;
extern int g_gallerySourceHeight;

// Main loop: starts the load the gallery asks for and shows the picture once it is decoded. True while
// loading.
bool gallery_loader_poll(void);
// What the last gallery_loader_poll said.
bool gallery_loader_busy(void);
