#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <pspdisplay.h>
#include <psputils.h>
#include <pspgu.h>
#include "mem64.h"
#include "graphics.h"
#include "ppa_video_output.h"
#include "ppa_memory.h"
#include "framebuffer.h"
#include "texture_subdivision.h"
#include "m33sdk.h"
#include "ppa_cache.h"

#define IS_ALPHA(color) (((color)&0xff000000)==0xff000000?0:1)
#ifndef PSP_FRAMEBUFFER_SIZE
#define PSP_FRAMEBUFFER_SIZE 557056
#endif

#ifndef PSP_TVOUT_BUFFER_SIZE
#define PSP_TVOUT_BUFFER_SIZE 1572864
#endif
#ifndef PSP_TVOUT_PSPLCD_BUFFER_SIZE
#define PSP_TVOUT_PSPLCD_BUFFER_SIZE 835584
#endif
#ifndef PSP_TVOUT_480P_BUFFER_SIZE
#define PSP_TVOUT_480P_BUFFER_SIZE 1474560
#endif
#ifndef PSP_TVOUT_480P_LINE_SIZE
#define PSP_TVOUT_480P_LINE_SIZE 3072
#endif

#define MAX(X, Y) ((X) > (Y) ? (X) : (Y))

struct vertex_struct
	{
	short texture_x;
	short texture_y;

	float vertex_x;
	float vertex_y;
	float vertex_z;
	};

typedef void (*BlitImageToScreen)(int, int, int, int, Image*, int, int);
typedef void (*BlitAlphaImageToScreen)(int, int, int, int, Image*, int, int);
typedef void (*CopyVramToExtraMemory)();
typedef void (*FlipScreen)();

void blitImageToScreenWithoutTVOut(int sx, int sy, int width, int height, Image* source, int dx, int dy);
void blitImageToScreenPSP(int sx, int sy, int width, int height, Image* source, int dx, int dy);
void blitImageToScreenTVOut(int sx, int sy, int width, int height, Image* source, int dx, int dy);

void blitAlphaImageToScreenWithoutTVOut(int sx, int sy, int width, int height, Image* source, int dx, int dy);
void blitAlphaImageToScreenPSP(int sx, int sy, int width, int height, Image* source, int dx, int dy);
void blitAlphaImageToScreenTVOut(int sx, int sy, int width, int height, Image* source, int dx, int dy);

void copyVramToExtraMemoryPSP();
void copyVramToExtraMemoryTVOutInterlace();
void copyVramToExtraMemoryTVOutProgressive();

void flipScreenWithoutTVOutSupported();
void flipScreenWithTVOutSupported();

unsigned int __attribute__((aligned(16))) list[262144];
static int pspType = 0;
static int videoMode = 0;
static int tvAspectRatio = 0; //0=16:9; 1=4:3
static int tvOverScanLeft;
static int tvOverScanTop;
static int tvOverScanRight;
static int tvOverScanBottom;
static int tvWidth;
static int tvHeight;
static int tvLeft;
static int tvTop;
static int displayBufferNumber;
static int initialized = 0;
static void* frameBuffer[2];
static BlitImageToScreen pBlitImageToScreen = blitImageToScreenWithoutTVOut;
static BlitAlphaImageToScreen pBlitAlphaImageToScreen = blitAlphaImageToScreenWithoutTVOut;
static CopyVramToExtraMemory pCopyVramToExtraMemory = copyVramToExtraMemoryPSP;
static FlipScreen pFlipScreen = flipScreenWithoutTVOutSupported;

static int getNextPower2(int width){
	int b = width;
	int n;
	for (n = 0; b != 0; n++) b >>= 1;
	b = 1 << n;
	if (b == 2 * width) b >>= 1;
	return b;
}

static void drawLine(int x0, int y0, int x1, int y1, int color, Color* destination, int width){
	int dy = y1 - y0;
	int dx = x1 - x0;
	int stepx, stepy;
	
	if (dy < 0) { dy = -dy;  stepy = -width; } else { stepy = width; }
	if (dx < 0) { dx = -dx;  stepx = -1; } else { stepx = 1; }
	dy <<= 1;
	dx <<= 1;
	
	y0 *= width;
	y1 *= width;
	destination[x0+y0] = color;
	if (dx > dy) {
		int fraction = dy - (dx >> 1);
		while (x0 != x1) {
			if (fraction >= 0) {
				y0 += stepy;
				fraction -= dx;
			}
			x0 += stepx;
			fraction += dy;
			destination[x0+y0] = color;
		}
	} else {
		int fraction = dx - (dy >> 1);
		while (y0 != y1) {
			if (fraction >= 0) {
				x0 += stepx;
				fraction -= dy;
			}
			y0 += stepy;
			fraction += dx;
			destination[x0+y0] = color;
		}
	}
}

void guStart(){
	sceGuStart(GU_DIRECT, list);
}

static void guSetupPSPLCD() {
	guStart();
	sceGuDrawBuffer(GU_PSM_8888, 0, PSP_TVOUT_TEXTURE_WIDTH);
	sceGuDispBuffer(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT, 0, PSP_TVOUT_TEXTURE_WIDTH);
	sceGuOffset(2048 - (PSP_SCREEN_WIDTH >> 1), 2048 - (PSP_SCREEN_HEIGHT >> 1));
	sceGuViewport(2048, 2048, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	sceGuScissor(0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDisable(GU_CULL_FACE);
	sceGuDisable(GU_DEPTH_TEST);	// Disable Z-compare
	sceGuDepthMask(GU_TRUE);		// Disable Z-writes
	sceGuDisable(GU_COLOR_TEST);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDisable(GU_LIGHTING);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuClearColor(0);
	sceGuClear(GU_COLOR_BUFFER_BIT);
	sceGuFinish();
	sceGuSync(0, 0);
	
	memset(frameBuffer[0], 0, PSP_TVOUT_BUFFER_SIZE);
	memset(frameBuffer[1], 0, PSP_TVOUT_BUFFER_SIZE);
	
	sceDisplayWaitVblankStart();
	sceDisplaySetFrameBuf(frameBuffer[displayBufferNumber], PSP_TVOUT_TEXTURE_WIDTH, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_IMMEDIATE);
}

static void guSetupTVOut() {
	guStart();
	sceGuDrawBuffer(GU_PSM_8888, 0, PSP_TVOUT_TEXTURE_WIDTH);
	sceGuDispBuffer(PSP_TVOUT_WIDTH, PSP_TVOUT_HEIGHT, 0, PSP_TVOUT_TEXTURE_WIDTH);
	sceGuOffset(2048 - (PSP_TVOUT_WIDTH >> 1), 2048 - (PSP_TVOUT_HEIGHT >> 1));
	sceGuViewport(2048, 2048, PSP_TVOUT_WIDTH, PSP_TVOUT_HEIGHT);
	sceGuScissor(0, 0, PSP_TVOUT_WIDTH, PSP_TVOUT_HEIGHT);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDisable(GU_CULL_FACE);
	sceGuDisable(GU_DEPTH_TEST);	// Disable Z-compare
	sceGuDepthMask(GU_TRUE);		// Disable Z-writes
	sceGuDisable(GU_COLOR_TEST);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDisable(GU_LIGHTING);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuClearColor(0);
	sceGuClear(GU_COLOR_BUFFER_BIT);
	sceGuFinish();
	sceGuSync(0, 0);
	
	memset(frameBuffer[0], 0, PSP_TVOUT_BUFFER_SIZE);
	memset(frameBuffer[1], 0, PSP_TVOUT_BUFFER_SIZE);
	
	sceDisplayWaitVblankStart();
	sceDisplaySetFrameBuf(frameBuffer[displayBufferNumber], PSP_TVOUT_TEXTURE_WIDTH, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_IMMEDIATE);
}

static void guDrawSprite(struct texture_subdivision_struct *t){
	struct vertex_struct *v = sceGuGetMemory(2 * sizeof(struct vertex_struct));

	v[0].texture_x = t->output_texture_x_start;
	v[0].texture_y = t->output_texture_y_start;
	v[0].vertex_x  = (int) t->output_vertex_x_start;
	v[0].vertex_y  = t->output_vertex_y_start;
	v[0].vertex_z  = 0.0;

	v[1].texture_x = t->output_texture_x_end;
	v[1].texture_y = t->output_texture_y_end;
	v[1].vertex_x  = (int) t->output_vertex_x_end;
	v[1].vertex_y  = t->output_vertex_y_end;
	v[1].vertex_z  = 0.0;

	sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
}

void setGraphicsTVAspectRatio(int ar) {
	tvAspectRatio = ar;
}

void setGraphicsTVOverScan(int left, int top, int right, int bottom) {
	tvOverScanLeft = ppa_video_output_margin(left, 320);
	tvOverScanTop = ppa_video_output_margin(top, 224);
	tvOverScanRight = ppa_video_output_margin(right, 320);
	tvOverScanBottom = ppa_video_output_margin(bottom, 224);
}

void setGraphicsTVOutScreen() {
	tvWidth = 720 - tvOverScanLeft - tvOverScanRight;
	tvHeight = 480 - tvOverScanTop - tvOverScanBottom;
	tvLeft = tvOverScanLeft;
	tvTop = tvOverScanTop;
	if( tvAspectRatio == 1) {
		tvTop += (tvHeight/8);
		tvHeight = tvHeight * 3 / 4;
	}
}

void setGraphicsVideoMode(int mode) {
	if ( initialized && (m33IsTVOutSupported(pspType) && ppa_memory_fixed_region_reserved()) && (videoMode != mode) ) {
		if ( mode == 0 ) {
			guSetupPSPLCD();
			pBlitImageToScreen = blitImageToScreenPSP;
			pBlitAlphaImageToScreen = blitAlphaImageToScreenPSP;
			pCopyVramToExtraMemory = copyVramToExtraMemoryPSP;
		}
		else if ( mode == 1 ) {
			guSetupTVOut();
			pBlitImageToScreen = blitImageToScreenTVOut;
			pBlitAlphaImageToScreen = blitAlphaImageToScreenTVOut;
			pCopyVramToExtraMemory = copyVramToExtraMemoryTVOutInterlace;
		}
		else if ( mode == 2 ) {
			guSetupTVOut();
			pBlitImageToScreen = blitImageToScreenTVOut;
			pBlitAlphaImageToScreen = blitAlphaImageToScreenTVOut;
			pCopyVramToExtraMemory = copyVramToExtraMemoryTVOutInterlace;
		}
		else if ( mode == 3 ) {
			guSetupTVOut();
			pBlitImageToScreen = blitImageToScreenTVOut;
			pBlitAlphaImageToScreen = blitAlphaImageToScreenTVOut;
			pCopyVramToExtraMemory = copyVramToExtraMemoryTVOutProgressive;
		}
		videoMode = mode;
	}
}

void initGraphicsWithTVoutSupported() {
	displayBufferNumber = 0;
	frameBuffer[0] = ppa_memory_extended_rgb_surface();
	frameBuffer[1] = (void *)((unsigned char *)frameBuffer[0] +
	                         PSP_TVOUT_BUFFER_SIZE);

	guSetupPSPLCD();
	
	pBlitImageToScreen = blitImageToScreenPSP;
	pBlitAlphaImageToScreen = blitAlphaImageToScreenPSP;
	pCopyVramToExtraMemory = copyVramToExtraMemoryPSP;
	videoMode = 0;
	initialized = 1;
}
	
void initGraphicsWithoutTVoutSupported() {
	displayBufferNumber = 0;
	frameBuffer[0] = (void*)(0x44000000 + PSP_FRAMEBUFFER_SIZE);
	frameBuffer[1] = (void*)(0x44000000 + 2*PSP_FRAMEBUFFER_SIZE);
		
	guStart();
	sceGuDrawBuffer(GU_PSM_8888, 0, PSP_SCREEN_TEXTURE_WIDTH);
	sceGuDispBuffer(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT, 0, PSP_SCREEN_TEXTURE_WIDTH);
	sceGuOffset(2048 - (PSP_SCREEN_WIDTH >> 1), 2048 - (PSP_SCREEN_HEIGHT >> 1));
	sceGuViewport(2048, 2048, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	sceGuScissor(0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDisable(GU_CULL_FACE);
	sceGuDisable(GU_DEPTH_TEST);	// Disable Z-compare
	sceGuDepthMask(GU_TRUE);		// Disable Z-writes
	sceGuDisable(GU_COLOR_TEST);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDisable(GU_LIGHTING);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuClearColor(0);
	sceGuClear(GU_COLOR_BUFFER_BIT);
	sceGuFinish();
	sceGuSync(0, 0);
	
	sceDisplayWaitVblankStart();
	sceDisplaySetFrameBuf(frameBuffer[displayBufferNumber], PSP_SCREEN_TEXTURE_WIDTH, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_IMMEDIATE);
			
	initialized = 1;
}

void initGraphics(int type, int mode){
	pspType = type;
	if ( (m33IsTVOutSupported(pspType) && ppa_memory_fixed_region_reserved()) ) {
		pFlipScreen = flipScreenWithTVOutSupported;
		initGraphicsWithTVoutSupported();
		setGraphicsVideoMode(mode);
	}
	else{
		pFlipScreen = flipScreenWithoutTVOutSupported;
		initGraphicsWithoutTVoutSupported();
	}
}

void disableGraphics(){
	initialized = 0;
	videoMode = 0;
	pspType = 0;
}

void clearScreen() {
	sceGuClearColor(0);
	sceGuClear(GU_COLOR_BUFFER_BIT);
}

void flipScreen(){
	pFlipScreen();
}

void flipScreenWithoutTVOutSupported() {
	if (!initialized) return;
	sceGuCopyImage(GU_PSM_8888, 0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT, PSP_SCREEN_TEXTURE_WIDTH, globalCachedVramBase, 0, 0, PSP_SCREEN_TEXTURE_WIDTH, frameBuffer[displayBufferNumber]);
	sceGuTexSync();
	sceGuFinish();
	sceGuSync(0,0);
	sceDisplayWaitVblankStart();
	sceDisplaySetFrameBuf(frameBuffer[displayBufferNumber], PSP_SCREEN_TEXTURE_WIDTH, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_IMMEDIATE);
	displayBufferNumber ^= 1;
}

void flipScreenWithTVOutSupported() {
	if (!initialized) return;
	pCopyVramToExtraMemory();
	sceDisplayWaitVblankStart();
	sceDisplaySetFrameBuf(frameBuffer[displayBufferNumber], PSP_TVOUT_TEXTURE_WIDTH, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_IMMEDIATE);
	displayBufferNumber ^= 1;
}

/*
 * Playback's 64 MiB LCD fast path presents three 512-pitch eDRAM surfaces
 * directly.  Browser/UI rendering, however, expects the GE draw target to be
 * eDRAM base 0 with a 768-pixel pitch and scans out one of the two stable
 * extended-RAM framebuffers.
 *
 * Never restore that state by clearing first: immediately after playback the
 * LCD may still be scanning the last eDRAM video surface.  A GE clear of that
 * surface races the display controller and is visible as a diagonal/triangular
 * black wedge.  Instead copy the complete currently scanned frame to the next
 * UI framebuffer, restore only GE state, queue the main-RAM framebuffer for
 * the next vertical boundary, and wait for that boundary before returning.
 */
int restoreGraphicsAfterDirectScanout() {
	void *source = 0;
	void *destination;
	int sourcePitch = 0;
	int sourceFormat = 0;
	int getResult;
	int syncResult;
	int setResult;
	int vblankResult;
	int copyCurrent = 0;

	if (!initialized || videoMode != 0 ||
	    !m33IsTVOutSupported(pspType) ||
	    !ppa_memory_fixed_region_reserved() ||
	    frameBuffer[0] == 0 || frameBuffer[1] == 0)
		return -1;

	destination = frameBuffer[displayBufferNumber];
	getResult = sceDisplayGetFrameBuf(&source, &sourcePitch, &sourceFormat,
	                                  PSP_DISPLAY_SETBUF_IMMEDIATE);
	if (getResult >= 0 && source != 0 &&
	    sourceFormat == PSP_DISPLAY_PIXEL_FORMAT_8888 &&
	    sourcePitch >= PSP_SCREEN_WIDTH && sourcePitch <= 2048 &&
	    ppa_cached_cptr(source) != ppa_cached_cptr(destination))
		copyCurrent = 1;

	/* If the current scanout address cannot be queried, the destination still
	 * contains a complete pre-playback/UI image (or Sony's last complete RGB
	 * frame).  It is therefore safe to switch to it; only the cosmetic final-
	 * frame preservation is skipped. */
	if (copyCurrent)
		ppa_cache_inv_range(destination, PSP_TVOUT_PSPLCD_BUFFER_SIZE);

	guStart();
	if (copyCurrent) {
		sceGuCopyImage(GU_PSM_8888,
		               0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT,
		               sourcePitch, source,
		               0, 0, PSP_TVOUT_TEXTURE_WIDTH, destination);
		sceGuTexSync();
	}

	/* Restore the exact LCD UI geometry/state established by guSetupPSPLCD(),
	 * except for its clear.  The browser's next normal paint owns that clear,
	 * after scanout has already moved to main RAM. */
	sceGuDrawBuffer(GU_PSM_8888, 0, PSP_TVOUT_TEXTURE_WIDTH);
	sceGuDispBuffer(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT, 0, PSP_TVOUT_TEXTURE_WIDTH);
	sceGuOffset(2048 - (PSP_SCREEN_WIDTH >> 1), 2048 - (PSP_SCREEN_HEIGHT >> 1));
	sceGuViewport(2048, 2048, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	sceGuScissor(0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDisable(GU_CULL_FACE);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDepthMask(GU_TRUE);
	sceGuDisable(GU_COLOR_TEST);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDisable(GU_LIGHTING);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuFinish();
	syncResult = sceGuSync(0, 0);
	if (syncResult < 0)
		return syncResult;

	setResult = sceDisplaySetFrameBuf(destination,
	                                  PSP_TVOUT_TEXTURE_WIDTH,
	                                  PSP_DISPLAY_PIXEL_FORMAT_8888,
	                                  PSP_DISPLAY_SETBUF_NEXTFRAME);
	if (setResult < 0)
		return setResult;
	vblankResult = sceDisplayWaitVblankStart();
	if (vblankResult < 0)
		return vblankResult;

	displayBufferNumber ^= 1;
	return 0;
}

static unsigned int imageDataBytes(const Image *image)
{
	if (image == NULL || image->data == NULL ||
	    image->textureWidth <= 0 || image->imageHeight <= 0)
		return 0;

	/* createImage allocates only imageHeight rows.  textureHeight is the GU
	 * descriptor's power-of-two extent, not the CPU allocation size. */
	return (unsigned int)image->textureWidth *
	       (unsigned int)image->imageHeight * sizeof(Color);
}

static void imagePrepareForGe(Image *image)
{
	if (image == NULL || image->data == NULL || !image->dirty)
		return;

	ppa_cache_cpu_wrote_ge_will_read(image->data, imageDataBytes(image));
	image->dirty = 0;
}

//---------------BlitImageToScreen-----------------------//
void blitImageToScreen(int sx, int sy, int width, int height, Image* source, int dx, int dy){
	pBlitImageToScreen(sx, sy, width, height, source, dx, dy);
}

void blitImageToScreenWithoutTVOut(int sx, int sy, int width, int height, Image* source, int dx, int dy){
	if (!initialized) return;
	
	imagePrepareForGe(source);
	sceGuDisable(GU_BLEND);
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, source->textureWidth, source->textureHeight, source->textureWidth, (void*) source->data);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	
	struct texture_subdivision_struct texture_subdivision;
	texture_subdivision_constructor_at(&texture_subdivision, sx, sy, width, height, 16, width, height, dx, dy);
	do
		{
		texture_subdivision_get(&texture_subdivision);
		guDrawSprite(&texture_subdivision);
		}
	while (texture_subdivision.output_last == 0);
	
}

void blitImageToScreenPSP(int sx, int sy, int width, int height, Image* source, int dx, int dy){
	blitImageToScreenWithoutTVOut(sx, sy, width, height, source, dx, dy);
}
	
void blitImageToScreenTVOut(int sx, int sy, int width, int height, Image* source, int dx, int dy){
	if (!initialized) return;
	
	imagePrepareForGe(source);
	sceGuDisable(GU_BLEND);
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, source->textureWidth, source->textureHeight, source->textureWidth, (void*) source->data);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	
	struct texture_subdivision_struct texture_subdivision;
	texture_subdivision_constructor_at(&texture_subdivision, sx, sy, width, height, 16, width*tvWidth/PSP_SCREEN_WIDTH, height*tvHeight/PSP_SCREEN_HEIGHT, dx*tvWidth/PSP_SCREEN_WIDTH+tvLeft, dy*tvHeight/PSP_SCREEN_HEIGHT+tvTop);
	do
		{
		texture_subdivision_get(&texture_subdivision);
		guDrawSprite(&texture_subdivision);
		}
	while (texture_subdivision.output_last == 0);
	
}

//---------------BlitAlphaImageToScreen-----------------------//
void blitAlphaImageToScreen(int sx, int sy, int width, int height, Image* source, int dx, int dy){
	pBlitAlphaImageToScreen(sx, sy, width, height, source, dx, dy);
}

void blitAlphaImageToScreenWithoutTVOut(int sx, int sy, int width, int height, Image* source, int dx, int dy){
	if (!initialized) return;

	imagePrepareForGe(source);

	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, source->textureWidth, source->textureHeight, source->textureWidth, (void*) source->data);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	
	struct texture_subdivision_struct texture_subdivision;
	texture_subdivision_constructor_at(&texture_subdivision, sx, sy, width, height, 16, width, height, dx, dy);
	do
		{
		texture_subdivision_get(&texture_subdivision);
		guDrawSprite(&texture_subdivision);
		}
	while (texture_subdivision.output_last == 0);
	
}

void blitAlphaImageToScreenPSP(int sx, int sy, int width, int height, Image* source, int dx, int dy){
	blitAlphaImageToScreenWithoutTVOut(sx, sy, width, height, source, dx, dy);
}

void blitAlphaImageToScreenTVOut(int sx, int sy, int width, int height, Image* source, int dx, int dy){
	if (!initialized) return;

	imagePrepareForGe(source);

	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, source->textureWidth, source->textureHeight, source->textureWidth, (void*) source->data);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	
	struct texture_subdivision_struct texture_subdivision;
	texture_subdivision_constructor_at(&texture_subdivision, sx, sy, width, height, 16, width*tvWidth/PSP_SCREEN_WIDTH, height*tvHeight/PSP_SCREEN_HEIGHT, dx*tvWidth/PSP_SCREEN_WIDTH+tvLeft, dy*tvHeight/PSP_SCREEN_HEIGHT+tvTop);
	do
		{
		texture_subdivision_get(&texture_subdivision);
		guDrawSprite(&texture_subdivision);
		}
	while (texture_subdivision.output_last == 0);
	
}

void blitImageToScreenWithOpacity(int sx, int sy, int width, int height,
                                Image* source, int dx, int dy,
                                unsigned int opacity) {
	struct texture_subdivision_struct subdivision;
	int tv_out;
	int vertex_width = width, vertex_height = height;
	int vertex_x = dx, vertex_y = dy;
	if (!initialized || source == NULL || source->data == NULL ||
	    width <= 0 || height <= 0 || sx < 0 || sy < 0 ||
	    sx > source->imageWidth || sy > source->imageHeight ||
	    width > source->imageWidth - sx ||
	    height > source->imageHeight - sy || opacity == 0U)
		return;
	if (opacity > 255U) opacity = 255U;
	tv_out = videoMode != 0 && m33IsTVOutSupported(pspType) &&
	         ppa_memory_fixed_region_reserved();
	if (tv_out) {
		vertex_width = width * tvWidth / PSP_SCREEN_WIDTH;
		vertex_height = height * tvHeight / PSP_SCREEN_HEIGHT;
		vertex_x = dx * tvWidth / PSP_SCREEN_WIDTH + tvLeft;
		vertex_y = dy * tvHeight / PSP_SCREEN_HEIGHT + tvTop;
	}

	/* A constant white fragment scales alpha only. The texture remains owned
	 * by the caller until flipScreen() has joined GE work; no per-frame image
	 * mutation, allocation or extra CPU framebuffer copy is needed. */
	imagePrepareForGe(source);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	sceGuColor((opacity << 24) | 0x00ffffffU);
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, source->textureWidth, source->textureHeight,
	              source->textureWidth, source->data);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	sceGuTexFilter(tv_out ? GU_LINEAR : GU_NEAREST,
	               tv_out ? GU_LINEAR : GU_NEAREST);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	texture_subdivision_constructor_at(&subdivision, sx, sy, width, height,
	                                  16, vertex_width, vertex_height,
	                                  vertex_x, vertex_y);
	do {
		texture_subdivision_get(&subdivision);
		guDrawSprite(&subdivision);
	} while (subdivision.output_last == 0);
	/* Restore the normal UI texture/color policy for subsequent commands. */
	sceGuColor(0xffffffffU);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
}

//---------------CopyVramToExtraMemory-----------------------//
void copyVramToExtraMemoryPSP(){
	sceGuCopyImage(GU_PSM_8888, 0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT, PSP_TVOUT_TEXTURE_WIDTH, globalCachedVramBase, 0, 0, PSP_TVOUT_TEXTURE_WIDTH, frameBuffer[displayBufferNumber]);
	sceGuTexSync();
	sceGuFinish();
	sceGuSync(0,0);
}
void copyVramToExtraMemoryTVOutInterlace(){
	int i;
	void* s0 = globalCachedVramBase;
	void* d0 = frameBuffer[displayBufferNumber];
	void* d1 = d0 + 262*PSP_TVOUT_480P_LINE_SIZE;
	for(i=0; i<240; i++)
		{
		sceGuCopyImage(GU_PSM_8888, 0, 0, PSP_TVOUT_WIDTH, 1, PSP_TVOUT_TEXTURE_WIDTH, s0, 0, 0, PSP_TVOUT_TEXTURE_WIDTH, d1);
		sceGuTexSync();
		s0+=PSP_TVOUT_480P_LINE_SIZE;
		d1+=PSP_TVOUT_480P_LINE_SIZE;
		sceGuCopyImage(GU_PSM_8888, 0, 0, PSP_TVOUT_WIDTH, 1, PSP_TVOUT_TEXTURE_WIDTH, s0, 0, 0, PSP_TVOUT_TEXTURE_WIDTH, d0);
		sceGuTexSync();
		s0+=PSP_TVOUT_480P_LINE_SIZE;
		d0+=PSP_TVOUT_480P_LINE_SIZE;
		}
	sceGuFinish();
	sceGuSync(0,0);
}
void copyVramToExtraMemoryTVOutProgressive(){
	sceGuCopyImage(GU_PSM_8888, 0, 0, PSP_TVOUT_WIDTH, PSP_TVOUT_HEIGHT, PSP_TVOUT_TEXTURE_WIDTH, globalCachedVramBase, 0, 0, PSP_TVOUT_TEXTURE_WIDTH, frameBuffer[displayBufferNumber]);
	sceGuTexSync();
	sceGuFinish();
	sceGuSync(0,0);
}

//---------------Image functions-----------------------//
Image* createImage(int width, int height){
	/* Skin PNG dimensions are user-controlled. Bound them before power-of-two
	 * rounding and byte multiplication; all bundled UI assets fit this policy. */
	if (width <= 0 || height <= 0 || width > PPA_IMAGE_MAX_DIMENSION ||
	    height > PPA_IMAGE_MAX_DIMENSION) return NULL;
	Image* image = (Image*) malloc_64(sizeof(Image));
	if (!image) return NULL;
	image->imageWidth = width;
	image->imageHeight = height;
	image->textureWidth = getNextPower2(width);
	image->textureHeight = getNextPower2(height);
	image->data = (Color*) memalign(64, image->textureWidth * image->imageHeight * sizeof(Color));
	if (!image->data) {
		free(image);
		return NULL;
	}
	memset(image->data, 0, image->textureWidth * image->imageHeight * sizeof(Color));
	image->dirty = 1;
	return image;
}

void freeImage(Image* image){
	if (image != NULL) {
		if (image->data != NULL)
			free(image->data);
		free(image);
	}
}

void clearImage(Image* image, Color color){
	int i;
	int size = image->textureWidth * image->imageHeight;
	Color* data = image->data;
	image->dirty = 1;
	for (i = 0; i < size; i++, data++) *data = color;
}

void putPixelToImage(Image* image, Color color, int x, int y){
	image->dirty = 1;
	image->data[x + y * image->textureWidth] = color;
}

Color getPixelFromImage(Image* image, int x, int y){
	return image->data[x + y * image->textureWidth];
}

void drawLineInImage(Image* image, Color color, int x0, int y0, int x1, int y1){
	image->dirty = 1;
	drawLine(x0, y0, x1, y1, color, image->data, image->textureWidth);
}

void fillImageRect(Image* image, Color color, int x0, int y0, int width, int height){
	int skipX = image->textureWidth - width;
	int x, y;
	Color* data = image->data + x0 + y0 * image->textureWidth;
	image->dirty = 1;
	for (y = 0; y < height; y++, data += skipX) {
		for (x = 0; x < width; x++, data++) *data = color;
	}
}

void fillImageEllipse(Image* image, Color color, int x0, int y0, int width, int height, int r) {
	int skipX = image->textureWidth - width;
	int x, y;
	Color* data = image->data + x0 + y0 * image->textureWidth;
	image->dirty = 1;
	for (y = 0; y < height; y++, data += skipX) {
		for (x = 0; x < width; x++, data++) {
			if ( (x < r) && (y < r)  && ((r-x)*(r-x)+(r-y)*(r-y) > r*r) )
				continue;
			else if ( (x < r) && (y > height-r-1)  && ((r-x)*(r-x)+(y-height+r+1)*(y-height+r+1) > r*r) )
				continue;
			else if ( (x > width-r-1) && (y < r)  && ((x-width+r+1)*(x-width+r+1)+(r-y)*(r-y) > r*r) )
				continue;
			else if ( (x > width-r-1) && (y > height-r-1)  && ((x-width+r+1)*(x-width+r+1)+(y-height+r+1)*(y-height+r+1) > r*r) )
				continue;
			else
				*data = color;
		}
	}
} 

void putImageToImage(Image* src, Image* dest, int dx, int dy, int dw, int dh) {
	int width = src->imageWidth > dw ? dw : src->imageWidth;
	int height = src->imageHeight > dh ? dh : src->imageHeight;
	int x, y;
	dest->dirty = 1;
	for(y = 0; y < height; y++ )
		for( x = 0; x < width; x++ ) {
			dest->data[(dy+y)*dest->textureWidth + (dx+x)] = src->data[y*src->textureWidth+x];
		}
}
