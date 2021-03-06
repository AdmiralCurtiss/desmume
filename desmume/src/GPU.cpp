/*
	Copyright (C) 2006 yopyop
	Copyright (C) 2006-2007 Theo Berkau
	Copyright (C) 2007 shash
	Copyright (C) 2008-2015 DeSmuME team

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with the this software.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "GPU.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <iostream>

#ifdef ENABLE_SSE2
#include <emmintrin.h>
#endif

#ifdef ENABLE_SSSE3
#include <tmmintrin.h>
#endif

#include "common.h"
#include "MMU.h"
#include "FIFO.h"
#include "debug.h"
#include "render3D.h"
#include "registers.h"
#include "gfx3d.h"
#include "debug.h"
#include "GPU_osd.h"
#include "NDSSystem.h"
#include "readwrite.h"
#include "matrix.h"
#include "emufile.h"

#ifdef FASTBUILD
	#undef FORCEINLINE
	#define FORCEINLINE
	//compilation speed hack (cuts time exactly in half by cutting out permutations)
	#define DISABLE_MOSAIC
#endif

u32 Render3DFramesPerSecond;

//instantiate static instance
u16 GPUEngineBase::_fadeInColors[17][0x8000];
u16 GPUEngineBase::_fadeOutColors[17][0x8000];
u8 GPUEngineBase::_blendTable555[17][17][32][32];
GPUEngineBase::MosaicLookup GPUEngineBase::_mosaicLookup;

GPUSubsystem *GPU = NULL;

static size_t _gpuLargestDstLineCount = 1;
static size_t _gpuVRAMBlockOffset = GPU_VRAM_BLOCK_LINES * GPU_FRAMEBUFFER_NATIVE_WIDTH;

static u16 *_gpuDstToSrcIndex = NULL; // Key: Destination pixel index / Value: Source pixel index

static CACHE_ALIGN size_t _gpuDstPitchCount[GPU_FRAMEBUFFER_NATIVE_WIDTH];	// Key: Source pixel index in x-dimension / Value: Number of x-dimension destination pixels for the source pixel
static CACHE_ALIGN size_t _gpuDstPitchIndex[GPU_FRAMEBUFFER_NATIVE_WIDTH];	// Key: Source pixel index in x-dimension / Value: First destination pixel that maps to the source pixel
static CACHE_ALIGN size_t _gpuDstLineCount[GPU_FRAMEBUFFER_NATIVE_HEIGHT];	// Key: Source line index / Value: Number of destination lines for the source line
static CACHE_ALIGN size_t _gpuDstLineIndex[GPU_FRAMEBUFFER_NATIVE_HEIGHT];	// Key: Source line index / Value: First destination line that maps to the source line
static CACHE_ALIGN size_t _gpuCaptureLineIndex[GPU_VRAM_BLOCK_LINES + 1];	// Key: Source line index / Value: First destination line that maps to the source line

const CACHE_ALIGN SpriteSize GPUEngineBase::_sprSizeTab[4][4] = {
     {{8, 8}, {16, 8}, {8, 16}, {8, 8}},
     {{16, 16}, {32, 8}, {8, 32}, {8, 8}},
     {{32, 32}, {32, 16}, {16, 32}, {8, 8}},
     {{64, 64}, {64, 32}, {32, 64}, {8, 8}},
};

const CACHE_ALIGN BGType GPUEngineBase::_mode2type[8][4] = {
      {BGType_Text, BGType_Text, BGType_Text, BGType_Text},
      {BGType_Text, BGType_Text, BGType_Text, BGType_Affine},
      {BGType_Text, BGType_Text, BGType_Affine, BGType_Affine},
      {BGType_Text, BGType_Text, BGType_Text, BGType_AffineExt},
      {BGType_Text, BGType_Text, BGType_Affine, BGType_AffineExt},
      {BGType_Text, BGType_Text, BGType_AffineExt, BGType_AffineExt},
      {BGType_Invalid, BGType_Invalid, BGType_Large8bpp, BGType_Invalid},
      {BGType_Invalid, BGType_Invalid, BGType_Invalid, BGType_Invalid}
};

//dont ever think of changing these to bits because you could avoid the multiplies in the main tile blitter.
//it doesnt really help any
const CACHE_ALIGN BGLayerSize GPUEngineBase::_BGLayerSizeLUT[8][4] = {
	{{0, 0}, {0, 0}, {0, 0}, {0, 0}}, //Invalid
    {{256,256}, {512,256}, {256,512}, {512,512}}, //text
    {{128,128}, {256,256}, {512,512}, {1024,1024}}, //affine
    {{512,1024}, {1024,512}, {0,0}, {0,0}}, //large 8bpp
	{{0, 0}, {0, 0}, {0, 0}, {0, 0}}, //affine ext (to be elaborated with another value)
	{{128,128}, {256,256}, {512,512}, {1024,1024}}, //affine ext 256x16
	{{128,128}, {256,256}, {512,256}, {512,512}}, //affine ext 256x1
	{{128,128}, {256,256}, {512,256}, {512,512}}, //affine ext direct
};

const CACHE_ALIGN u8 GPUEngineBase::_winEmpty[GPU_FRAMEBUFFER_NATIVE_WIDTH] = {
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

/*****************************************************************************/
//			BACKGROUND RENDERING -ROTOSCALE-
/*****************************************************************************/

template<GPULayerID LAYERID, bool ISDEBUGRENDER, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
FORCEINLINE void rot_tiled_8bit_entry(GPUEngineBase *gpu, u16 *__restrict dstColorLine, const u16 lineIndex, const s32 auxX, const s32 auxY, const int lg, const u32 map, const u32 tile, const u16 *__restrict pal, const size_t i)
{
	const u16 tileindex = *(u8*)MMU_gpu_map(map + ((auxX>>3) + (auxY>>3) * (lg>>3)));
	const u16 x = auxX & 7;
	const u16 y = auxY & 7;
	const u8 palette_entry = *(u8*)MMU_gpu_map(tile + ((tileindex<<6)+(y<<3)+x));
	const u16 color = LE_TO_LOCAL_16( pal[palette_entry] );
	
	gpu->__setFinalColorBck<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED>(dstColorLine, lineIndex, color, i, (palette_entry != 0));
}

template<GPULayerID LAYERID, bool ISDEBUGRENDER, bool MOSAIC, bool extPal, bool ISCUSTOMRENDERINGNEEDED>
FORCEINLINE void rot_tiled_16bit_entry(GPUEngineBase *gpu, u16 *__restrict dstColorLine, const u16 lineIndex, const s32 auxX, const s32 auxY, const int lg, const u32 map, const u32 tile, const u16 *__restrict pal, const size_t i)
{
	TILEENTRY tileentry;
	tileentry.val = LE_TO_LOCAL_16( *(u16 *)MMU_gpu_map(map + (((auxX>>3) + (auxY>>3) * (lg>>3))<<1)) );
	
	const u16 x = ((tileentry.bits.HFlip) ? 7 - (auxX) : (auxX)) & 7;
	const u16 y = ((tileentry.bits.VFlip) ? 7 - (auxY) : (auxY)) & 7;
	const u8 palette_entry = *(u8*)MMU_gpu_map(tile + ((tileentry.bits.TileNum<<6)+(y<<3)+x));
	const u16 color = LE_TO_LOCAL_16( pal[(palette_entry + (extPal ? (tileentry.bits.Palette<<8) : 0))] );
	
	gpu->__setFinalColorBck<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED>(dstColorLine, lineIndex, color, i, (palette_entry != 0));
}

template<GPULayerID LAYERID, bool ISDEBUGRENDER, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
FORCEINLINE void rot_256_map(GPUEngineBase *gpu, u16 *__restrict dstColorLine, const u16 lineIndex, const s32 auxX, const s32 auxY, const int lg, const u32 map, const u32 tile, const u16 *__restrict pal, const size_t i)
{
	const u8 palette_entry = *(u8*)MMU_gpu_map((map) + ((auxX + auxY * lg)));
	const u16 color = LE_TO_LOCAL_16( pal[palette_entry] );
	
	gpu->__setFinalColorBck<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED>(dstColorLine, lineIndex, color, i, (palette_entry != 0));
}

template<GPULayerID LAYERID, bool ISDEBUGRENDER, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED, bool USECUSTOMVRAM>
FORCEINLINE void rot_BMP_map(GPUEngineBase *gpu, u16 *__restrict dstColorLine, const u16 lineIndex, const s32 auxX, const s32 auxY, const int lg, const u32 map, const u32 tile, const u16 *__restrict pal, const size_t i)
{
	const u16 color = LE_TO_LOCAL_16( *(u16 *)MMU_gpu_map((map) + ((auxX + auxY * lg) << 1)) );
	
	gpu->___setFinalColorBck<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED, USECUSTOMVRAM>(dstColorLine, lineIndex, color, i, ((color & 0x8000) != 0));
}

void gpu_savestate(EMUFILE* os)
{
	const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
	const GPUEngineA *mainEngine = GPU->GetEngineMain();
	const GPUEngineB *subEngine = GPU->GetEngineSub();
	
	//version
	write32le(1,os);
	
	os->fwrite((u8 *)dispInfo.masterCustomBuffer, GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT * sizeof(u16) * 2);
	
	write32le(mainEngine->savedBG2X.value, os);
	write32le(mainEngine->savedBG2Y.value, os);
	write32le(mainEngine->savedBG3X.value, os);
	write32le(mainEngine->savedBG3Y.value, os);
	write32le(subEngine->savedBG2X.value, os);
	write32le(subEngine->savedBG2Y.value, os);
	write32le(subEngine->savedBG3X.value, os);
	write32le(subEngine->savedBG3Y.value, os);
}

bool gpu_loadstate(EMUFILE* is, int size)
{
	const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
	GPUEngineA *mainEngine = GPU->GetEngineMain();
	GPUEngineB *subEngine = GPU->GetEngineSub();
	
	//read version
	u32 version;
	
	//sigh.. shouldve used a new version number
	if (size == GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT * sizeof(u16) * 2)
	{
		version = 0;
	}
	else if (size == 0x30024)
	{
		read32le(&version,is);
		version = 1;
	}
	else
	{
		if(read32le(&version,is) != 1) return false;
	}
	
	if (version > 1) return false;
	
	is->fread((u8 *)dispInfo.masterCustomBuffer, GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT * sizeof(u16) * 2);
	
	if (version == 1)
	{
		read32le((u32 *)&mainEngine->savedBG2X, is);
		read32le((u32 *)&mainEngine->savedBG2Y, is);
		read32le((u32 *)&mainEngine->savedBG3X, is);
		read32le((u32 *)&mainEngine->savedBG3Y, is);
		read32le((u32 *)&subEngine->savedBG2X, is);
		read32le((u32 *)&subEngine->savedBG2Y, is);
		read32le((u32 *)&subEngine->savedBG3X, is);
		read32le((u32 *)&subEngine->savedBG3Y, is);
		//removed per nitsuja feedback. anyway, this same thing will happen almost immediately in gpu line=0
		//mainEngine->refreshAffineStartRegs(-1,-1);
		//subEngine->refreshAffineStartRegs(-1,-1);
	}
	
	mainEngine->ParseAllRegisters<GPUEngineID_Main>();
	subEngine->ParseAllRegisters<GPUEngineID_Sub>();
	
	return !is->fail();
}

/*****************************************************************************/
//			INITIALIZATION
/*****************************************************************************/
void GPUEngineBase::_InitLUTs()
{
	static bool didInit = false;
	
	if (didInit)
	{
		return;
	}
	
	/*
	NOTE: gbatek (in the reference above) seems to expect 6bit values 
	per component, but as desmume works with 5bit per component, 
	we use 31 as top, instead of 63. Testing it on a few games, 
	using 63 seems to give severe color wraping, and 31 works
	nicely, so for now we'll just that, until proven wrong.

	i have seen pics of pokemon ranger getting white with 31, with 63 it is nice.
	it could be pb of alpha or blending or...

	MightyMax> created a test NDS to check how the brightness values work,
	and 31 seems to be correct. FactorEx is a override for max brighten/darken
	See: http://mightymax.org/gfx_test_brightness.nds
	The Pokemon Problem could be a problem with 8/32 bit writes not recognized yet,
	i'll add that so you can check back.

	*/

	for(int i = 0; i <= 16; i++)
	{
		for(int j = 0x8000; j < 0x10000; j++)
		{
			COLOR cur;

			cur.val = j;
			cur.bits.red = (cur.bits.red + ((31 - cur.bits.red) * i / 16));
			cur.bits.green = (cur.bits.green + ((31 - cur.bits.green) * i / 16));
			cur.bits.blue = (cur.bits.blue + ((31 - cur.bits.blue) * i / 16));
			cur.bits.alpha = 0;
			GPUEngineBase::_fadeInColors[i][j & 0x7FFF] = cur.val;

			cur.val = j;
			cur.bits.red = (cur.bits.red - (cur.bits.red * i / 16));
			cur.bits.green = (cur.bits.green - (cur.bits.green * i / 16));
			cur.bits.blue = (cur.bits.blue - (cur.bits.blue * i / 16));
			cur.bits.alpha = 0;
			GPUEngineBase::_fadeOutColors[i][j & 0x7FFF] = cur.val;
		}
	}


	for(int c0=0;c0<=31;c0++) 
		for(int c1=0;c1<=31;c1++) 
			for(int eva=0;eva<=16;eva++)
				for(int evb=0;evb<=16;evb++)
				{
					int blend = ((c0 * eva) + (c1 * evb) ) / 16;
					int final = std::min<int>(31,blend);
					GPUEngineBase::_blendTable555[eva][evb][c0][c1] = final;
				}
	
	didInit = true;
}

GPUEngineBase::GPUEngineBase()
{
	_IORegisterMap = NULL;
	_paletteOBJ = NULL;
	
	this->_BGLayer[GPULayerID_BG0].layerID = GPULayerID_BG0;
	this->_BGLayer[GPULayerID_BG1].layerID = GPULayerID_BG1;
	this->_BGLayer[GPULayerID_BG2].layerID = GPULayerID_BG2;
	this->_BGLayer[GPULayerID_BG3].layerID = GPULayerID_BG3;
	
	this->_BGLayer[GPULayerID_BG0].extPaletteSlot = GPULayerID_BG0;
	this->_BGLayer[GPULayerID_BG1].extPaletteSlot = GPULayerID_BG1;
	this->_BGLayer[GPULayerID_BG2].extPaletteSlot = GPULayerID_BG2;
	this->_BGLayer[GPULayerID_BG3].extPaletteSlot = GPULayerID_BG3;
	
	this->_BGLayer[GPULayerID_BG0].extPalette = NULL;
	this->_BGLayer[GPULayerID_BG1].extPalette = NULL;
	this->_BGLayer[GPULayerID_BG2].extPalette = NULL;
	this->_BGLayer[GPULayerID_BG3].extPalette = NULL;
	
	_InitLUTs();
	_workingDstColorBuffer = NULL;
	_dstLayerID = NULL;
}

GPUEngineBase::~GPUEngineBase()
{
	free_aligned(this->_workingDstColorBuffer);
	this->_workingDstColorBuffer = NULL;
	free_aligned(this->_dstLayerID);
	this->_dstLayerID = NULL;
}

void GPUEngineBase::_Reset_Base()
{
	const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
	
	memset(this->_sprColor, 0, sizeof(this->_sprColor));
	memset(this->_sprAlpha, 0, sizeof(this->_sprAlpha));
	memset(this->_sprType, OBJMode_Normal, sizeof(this->_sprType));
	memset(this->_sprPrio, 0x7F, sizeof(this->_sprPrio));
	memset(this->_sprNum, 0, sizeof(this->_sprNum));
	
	memset(this->_h_win[0], 0, sizeof(this->_h_win[0]));
	memset(this->_h_win[1], 0, sizeof(this->_h_win[1]));
	memset(&this->_mosaicColors, 0, sizeof(MosaicColor));
	memset(this->_itemsForPriority, 0, sizeof(this->_itemsForPriority));
	
	if (this->_workingDstColorBuffer != NULL)
	{
		memset(this->_workingDstColorBuffer, 0, dispInfo.customWidth * _gpuLargestDstLineCount * sizeof(u16));
	}
	if (this->_dstLayerID != NULL)
	{
		memset(this->_dstLayerID, 0, dispInfo.customWidth * _gpuLargestDstLineCount * 4 * sizeof(u8));
	}
	
	this->_enableLayer[GPULayerID_BG0] = false;
	this->_enableLayer[GPULayerID_BG1] = false;
	this->_enableLayer[GPULayerID_BG2] = false;
	this->_enableLayer[GPULayerID_BG3] = false;
	this->_enableLayer[GPULayerID_OBJ] = false;
	this->_isAnyBGLayerEnabled = false;
	
	this->_BGLayer[GPULayerID_BG0].BGnCNT = this->_IORegisterMap->BG0CNT;
	this->_BGLayer[GPULayerID_BG1].BGnCNT = this->_IORegisterMap->BG1CNT;
	this->_BGLayer[GPULayerID_BG2].BGnCNT = this->_IORegisterMap->BG2CNT;
	this->_BGLayer[GPULayerID_BG3].BGnCNT = this->_IORegisterMap->BG3CNT;
	
	this->_BGLayer[GPULayerID_BG0].size = GPUEngineBase::_BGLayerSizeLUT[BGType_Affine][1];
	this->_BGLayer[GPULayerID_BG1].size = GPUEngineBase::_BGLayerSizeLUT[BGType_Affine][1];
	this->_BGLayer[GPULayerID_BG2].size = GPUEngineBase::_BGLayerSizeLUT[BGType_Affine][1];
	this->_BGLayer[GPULayerID_BG3].size = GPUEngineBase::_BGLayerSizeLUT[BGType_Affine][1];
	
	this->_BGLayer[GPULayerID_BG0].type = BGType_Invalid;
	this->_BGLayer[GPULayerID_BG1].type = BGType_Invalid;
	this->_BGLayer[GPULayerID_BG2].type = BGType_Invalid;
	this->_BGLayer[GPULayerID_BG3].type = BGType_Invalid;
	
	this->_BGLayer[GPULayerID_BG0].priority = 0;
	this->_BGLayer[GPULayerID_BG1].priority = 0;
	this->_BGLayer[GPULayerID_BG2].priority = 0;
	this->_BGLayer[GPULayerID_BG3].priority = 0;
	
	this->_BGLayer[GPULayerID_BG0].isVisible = false;
	this->_BGLayer[GPULayerID_BG1].isVisible = false;
	this->_BGLayer[GPULayerID_BG2].isVisible = false;
	this->_BGLayer[GPULayerID_BG3].isVisible = false;
	
	this->_BGLayer[GPULayerID_BG0].isMosaic = false;
	this->_BGLayer[GPULayerID_BG1].isMosaic = false;
	this->_BGLayer[GPULayerID_BG2].isMosaic = false;
	this->_BGLayer[GPULayerID_BG3].isMosaic = false;
	
	this->_BGLayer[GPULayerID_BG0].isDisplayWrapped = false;
	this->_BGLayer[GPULayerID_BG1].isDisplayWrapped = false;
	this->_BGLayer[GPULayerID_BG2].isDisplayWrapped = false;
	this->_BGLayer[GPULayerID_BG3].isDisplayWrapped = false;
	
	this->_BGLayer[GPULayerID_BG0].extPaletteSlot = GPULayerID_BG0;
	this->_BGLayer[GPULayerID_BG1].extPaletteSlot = GPULayerID_BG1;
	this->_BGLayer[GPULayerID_BG0].extPalette = (u16 **)&MMU.ExtPal[this->_engineID][GPULayerID_BG0];
	this->_BGLayer[GPULayerID_BG1].extPalette = (u16 **)&MMU.ExtPal[this->_engineID][GPULayerID_BG1];
	this->_BGLayer[GPULayerID_BG2].extPalette = (u16 **)&MMU.ExtPal[this->_engineID][GPULayerID_BG2];
	this->_BGLayer[GPULayerID_BG3].extPalette = (u16 **)&MMU.ExtPal[this->_engineID][GPULayerID_BG3];
	
	this->_mosaicWidthBG = &GPUEngineBase::_mosaicLookup.table[0][0];
	this->_mosaicHeightBG = &GPUEngineBase::_mosaicLookup.table[0][0];
	this->_mosaicWidthOBJ = &GPUEngineBase::_mosaicLookup.table[0][0];
	this->_mosaicHeightOBJ = &GPUEngineBase::_mosaicLookup.table[0][0];
	this->_isBGMosaicSet = false;
	this->_isOBJMosaicSet = false;
	
	this->_curr_win[0] = GPUEngineBase::_winEmpty;
	this->_curr_win[1] = GPUEngineBase::_winEmpty;
	this->_needUpdateWINH[0] = true;
	this->_needUpdateWINH[1] = true;
	
	this->isCustomRenderingNeeded = false;
	this->is3DEnabled = false;
	this->vramBGLayer = VRAM_NO_3D_USAGE;
	this->vramBlockBGIndex = VRAM_NO_3D_USAGE;
	this->vramBlockOBJIndex = VRAM_NO_3D_USAGE;
	
	this->_sprBoundary = 0;
	this->_sprBMPBoundary = 0;
	
	this->_WIN0_ENABLED = false;
	this->_WIN1_ENABLED = false;
	this->_WINOBJ_ENABLED = false;
	this->_isAnyWindowEnabled = false;
	
	this->_BLDALPHA_EVA = 0;
	this->_BLDALPHA_EVB = 0;
	this->_blendTable = (TBlendTable *)&GPUEngineBase::_blendTable555[this->_BLDALPHA_EVA][this->_BLDALPHA_EVB][0][0];
	this->_currentFadeInColors = &GPUEngineBase::_fadeInColors[0][0];
	this->_currentFadeOutColors = &GPUEngineBase::_fadeOutColors[0][0];
	
	this->_blend2[GPULayerID_BG0] = false;
	this->_blend2[GPULayerID_BG1] = false;
	this->_blend2[GPULayerID_BG2] = false;
	this->_blend2[GPULayerID_BG3] = false;
	this->_blend2[GPULayerID_OBJ] = false;
	this->_blend2[GPULayerID_Backdrop] = false;
	
	this->_isMasterBrightFullIntensity = false;
	
	this->_spriteRenderMode = SpriteRenderMode_Sprite1D;
	
	this->savedBG2X.value = 0;
	this->savedBG2Y.value = 0;
	this->savedBG3X.value = 0;
	this->savedBG3Y.value = 0;
	
	this->renderedWidth = GPU_FRAMEBUFFER_NATIVE_WIDTH;
	this->renderedHeight = GPU_FRAMEBUFFER_NATIVE_HEIGHT;
	this->renderedBuffer = this->nativeBuffer;
}

void GPUEngineBase::Reset()
{
	this->_Reset_Base();
}

void GPUEngineBase::_ResortBGLayers()
{
	const IOREG_DISPCNT &DISPCNT = this->_IORegisterMap->DISPCNT;
	int i, prio;
	itemsForPriority_t *item;
	
	// we don't need to check for windows here...
	// if we tick boxes, invisible layers become invisible & vice versa
#define OP ^ !
	// if we untick boxes, layers become invisible
	//#define OP &&
	this->_enableLayer[GPULayerID_BG0] = CommonSettings.dispLayers[this->_engineID][GPULayerID_BG0] OP(this->_BGLayer[GPULayerID_BG0].isVisible);
	this->_enableLayer[GPULayerID_BG1] = CommonSettings.dispLayers[this->_engineID][GPULayerID_BG1] OP(this->_BGLayer[GPULayerID_BG1].isVisible);
	this->_enableLayer[GPULayerID_BG2] = CommonSettings.dispLayers[this->_engineID][GPULayerID_BG2] OP(this->_BGLayer[GPULayerID_BG2].isVisible);
	this->_enableLayer[GPULayerID_BG3] = CommonSettings.dispLayers[this->_engineID][GPULayerID_BG3] OP(this->_BGLayer[GPULayerID_BG3].isVisible);
	this->_enableLayer[GPULayerID_OBJ] = CommonSettings.dispLayers[this->_engineID][GPULayerID_OBJ] OP(DISPCNT.OBJ_Enable);
	
	this->_isAnyBGLayerEnabled = this->_enableLayer[GPULayerID_BG0] || this->_enableLayer[GPULayerID_BG1] || this->_enableLayer[GPULayerID_BG2] || this->_enableLayer[GPULayerID_BG3];
	
	// KISS ! lower priority first, if same then lower num
	for (i = 0; i < NB_PRIORITIES; i++)
	{
		item = &(this->_itemsForPriority[i]);
		item->nbBGs = 0;
		item->nbPixelsX = 0;
	}
	
	for (i = NB_BG; i > 0; )
	{
		i--;
		if (!this->_enableLayer[i]) continue;
		prio = this->_BGLayer[i].priority;
		item = &(this->_itemsForPriority[prio]);
		item->BGs[item->nbBGs]=i;
		item->nbBGs++;
	}
	
#if 0
	//debug
	for (i = 0; i < NB_PRIORITIES; i++)
	{
		item = &(this->_itemsForPriority[i]);
		printf("%d : ", i);
		for (j=0; j<NB_PRIORITIES; j++)
		{
			if (j < item->nbBGs)
				printf("BG%d ", item->BGs[j]);
			else
				printf("... ", item->BGs[j]);
		}
	}
	printf("\n");
#endif
}

FORCEINLINE u16 GPUEngineBase::_ColorEffectBlend(const u16 colA, const u16 colB, const TBlendTable *blendTable)
{
	const u8 r = (*blendTable)[ colA        & 0x1F][ colB        & 0x1F];
	const u8 g = (*blendTable)[(colA >>  5) & 0x1F][(colB >>  5) & 0x1F];
	const u8 b = (*blendTable)[(colA >> 10) & 0x1F][(colB >> 10) & 0x1F];

	return r | (g << 5) | (b << 10);
}

void GPUEngineBase::ParseReg_MASTER_BRIGHT()
{
	if (!nds.isInVblank())
	{
		PROGINFO("Changing master brightness outside of vblank, line=%d\n", nds.VCount);
	}
	
	const IOREG_MASTER_BRIGHT &MASTER_BRIGHT = this->_IORegisterMap->MASTER_BRIGHT;
	this->_isMasterBrightFullIntensity = ( (MASTER_BRIGHT.Intensity >= 16) && ((MASTER_BRIGHT.Mode == GPUMasterBrightMode_Up) || (MASTER_BRIGHT.Mode == GPUMasterBrightMode_Down)) );
	//printf("MASTER BRIGHTNESS %d to %d at %d\n",this->core,MASTER_BRIGHT.Intensity,nds.VCount);
}

//Sets up LCD control variables for Display Engines A and B for quick reading
template<GPUEngineID ENGINEID>
void GPUEngineBase::ParseReg_DISPCNT()
{
	const IOREG_DISPCNT &DISPCNT = this->_IORegisterMap->DISPCNT;
	
	this->_WIN0_ENABLED	= (DISPCNT.Win0_Enable != 0);
	this->_WIN1_ENABLED	= (DISPCNT.Win1_Enable != 0);
	this->_WINOBJ_ENABLED = (DISPCNT.WinOBJ_Enable != 0);
	this->_isAnyWindowEnabled = (this->_WIN0_ENABLED || this->_WIN1_ENABLED || this->_WINOBJ_ENABLED);
	
	if (ENGINEID == GPUEngineID_Main)
	{
		((GPUEngineA *)this)->UpdateSelectedVRAMBlock();
	}
	
	if (DISPCNT.OBJ_Tile_mapping)
	{
		//1-d sprite mapping boundaries:
		//32k, 64k, 128k, 256k
		this->_sprBoundary = 5 + DISPCNT.OBJ_Tile_1D_Bound;
		
		//do not be deceived: even though a sprBoundary==8 (256KB region) is impossible to fully address
		//in GPU_SUB, it is still fully legal to address it with that granularity.
		//so don't do this: //if((gpu->core == GPU_SUB) && (cnt->OBJ_Tile_1D_Bound == 3)) gpu->sprBoundary = 7;

		this->_spriteRenderMode = SpriteRenderMode_Sprite1D;
	}
	else
	{
		//2d sprite mapping
		//boundary : 32k
		this->_sprBoundary = 5;
		this->_spriteRenderMode = SpriteRenderMode_Sprite2D;
	}
     
	if (DISPCNT.OBJ_BMP_1D_Bound && (ENGINEID == GPUEngineID_Main))
		this->_sprBMPBoundary = 8;
	else
		this->_sprBMPBoundary = 7;
	
	this->ParseReg_BGnCNT<ENGINEID, GPULayerID_BG3>();
	this->ParseReg_BGnCNT<ENGINEID, GPULayerID_BG2>();
	this->ParseReg_BGnCNT<ENGINEID, GPULayerID_BG1>();
	this->ParseReg_BGnCNT<ENGINEID, GPULayerID_BG0>();
}

template <GPUEngineID ENGINEID, GPULayerID LAYERID>
void GPUEngineBase::ParseReg_BGnCNT()
{
	const IOREG_DISPCNT &DISPCNT = this->_IORegisterMap->DISPCNT;
	const IOREG_BGnCNT &BGnCNT = this->_IORegisterMap->BGnCNT[LAYERID];
	this->_BGLayer[LAYERID].BGnCNT = BGnCNT;
	
	switch (LAYERID)
	{
		case GPULayerID_BG0: this->_BGLayer[LAYERID].isVisible = (DISPCNT.BG0_Enable != 0); break;
		case GPULayerID_BG1: this->_BGLayer[LAYERID].isVisible = (DISPCNT.BG1_Enable != 0); break;
		case GPULayerID_BG2: this->_BGLayer[LAYERID].isVisible = (DISPCNT.BG2_Enable != 0); break;
		case GPULayerID_BG3: this->_BGLayer[LAYERID].isVisible = (DISPCNT.BG3_Enable != 0); break;
			
		default:
			break;
	}
	
	if (ENGINEID == GPUEngineID_Main)
	{
		this->_BGLayer[LAYERID].largeBMPAddress  = MMU_ABG;
		this->_BGLayer[LAYERID].BMPAddress       = MMU_ABG + (BGnCNT.ScreenBase_Block * ADDRESS_STEP_16KB);
		this->_BGLayer[LAYERID].tileMapAddress   = MMU_ABG + (DISPCNT.ScreenBase_Block * ADDRESS_STEP_64KB) + (BGnCNT.ScreenBase_Block * ADDRESS_STEP_2KB);
		this->_BGLayer[LAYERID].tileEntryAddress = MMU_ABG + (DISPCNT.CharacBase_Block * ADDRESS_STEP_64KB) + (BGnCNT.CharacBase_Block * ADDRESS_STEP_16KB);
	}
	else
	{
		this->_BGLayer[LAYERID].largeBMPAddress  = MMU_BBG;
		this->_BGLayer[LAYERID].BMPAddress       = MMU_BBG + (BGnCNT.ScreenBase_Block * ADDRESS_STEP_16KB);
		this->_BGLayer[LAYERID].tileMapAddress   = MMU_BBG + (BGnCNT.ScreenBase_Block * ADDRESS_STEP_2KB);
		this->_BGLayer[LAYERID].tileEntryAddress = MMU_BBG + (BGnCNT.CharacBase_Block * ADDRESS_STEP_16KB);
	}
	
	//clarify affine ext modes
	BGType mode = GPUEngineBase::_mode2type[DISPCNT.BG_Mode][LAYERID];
	if (mode == BGType_AffineExt)
	{
		//see: http://nocash.emubase.de/gbatek.htm#dsvideobgmodescontrol
		const u8 affineModeSelection = (BGnCNT.PaletteMode << 1) | (BGnCNT.CharacBase_Block & 1);
		switch (affineModeSelection)
		{
			case 0:
			case 1:
				mode = BGType_AffineExt_256x16;
				break;
			case 2:
				mode = BGType_AffineExt_256x1;
				break;
			case 3:
				mode = BGType_AffineExt_Direct;
				break;
		}
	}
	
	// Extended palette slots can be changed for BG0 and BG1, but BG2 and BG3 remain constant.
	// Display wrapping can be changed for BG2 and BG3, but BG0 and BG1 cannot wrap.
	if (LAYERID == GPULayerID_BG0 || LAYERID == GPULayerID_BG1)
	{
		this->_BGLayer[LAYERID].extPaletteSlot = (BGnCNT.PaletteSet_Wrap * 2) + LAYERID;
	}
	else
	{
		this->_BGLayer[LAYERID].isDisplayWrapped = (BGnCNT.PaletteSet_Wrap != 0);
	}
	
	this->_BGLayer[LAYERID].type = mode;
	this->_BGLayer[LAYERID].size = GPUEngineBase::_BGLayerSizeLUT[mode][BGnCNT.ScreenSize];
	this->_BGLayer[LAYERID].isMosaic = (BGnCNT.Mosaic != 0);
	this->_BGLayer[LAYERID].priority = BGnCNT.Priority;
	this->_BGLayer[LAYERID].extPalette = (u16 **)&MMU.ExtPal[this->_engineID][this->_BGLayer[LAYERID].extPaletteSlot];
	
	this->_ResortBGLayers();
}

template <GPULayerID LAYERID>
void GPUEngineBase::ParseReg_BGnHOFS()
{
	const IOREG_BGnHOFS &BGnHOFS = this->_IORegisterMap->BGnOFS[LAYERID].BGnHOFS;
	this->_BGLayer[LAYERID].BGnHOFS = BGnHOFS;
	
#ifdef LOCAL_LE
	this->_BGLayer[LAYERID].xOffset = BGnHOFS.Offset;
#else
	this->_BGLayer[LAYERID].xOffset = LOCAL_TO_LE_16(BGnHOFS.value) & 0x01FF;
#endif
}

template <GPULayerID LAYERID>
void GPUEngineBase::ParseReg_BGnVOFS()
{
	const IOREG_BGnVOFS &BGnVOFS = this->_IORegisterMap->BGnOFS[LAYERID].BGnVOFS;
	this->_BGLayer[LAYERID].BGnVOFS = BGnVOFS;
	
#ifdef LOCAL_LE
	this->_BGLayer[LAYERID].yOffset = BGnVOFS.Offset;
#else
	this->_BGLayer[LAYERID].yOffset = LOCAL_TO_LE_16(BGnVOFS.value) & 0x01FF;
#endif
}

template<GPULayerID LAYERID>
void GPUEngineBase::ParseReg_BGnX()
{
	if (LAYERID == GPULayerID_BG2)
	{
		this->savedBG2X = this->_IORegisterMap->BG2X;
	}
	else if (LAYERID == GPULayerID_BG3)
	{
		this->savedBG3X = this->_IORegisterMap->BG3X;
	}
}

template<GPULayerID LAYERID>
void GPUEngineBase::ParseReg_BGnY()
{
	if (LAYERID == GPULayerID_BG2)
	{
		this->savedBG2Y = this->_IORegisterMap->BG2Y;
	}
	else if (LAYERID == GPULayerID_BG3)
	{
		this->savedBG3Y = this->_IORegisterMap->BG3Y;
	}
}

template <bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_RenderLine_Clear(const u16 clearColor, const u16 l, u16 *dstColorLine, const size_t dstLineWidth, const size_t dstLineCount)
{
	const size_t pixCount = dstLineWidth * dstLineCount;
	
	// Clear the current line with the clear color
	const IOREG_BLDCNT &BLDCNT = this->_IORegisterMap->BLDCNT;
	
	u16 dstClearColor = clearColor;
	
	if (BLDCNT.Backdrop_Target1 != 0)
	{
		if (BLDCNT.ColorEffect == ColorEffect_IncreaseBrightness)
		{
			dstClearColor = this->_currentFadeInColors[clearColor];
		}
		else if (BLDCNT.ColorEffect == ColorEffect_DecreaseBrightness)
		{
			dstClearColor = this->_currentFadeOutColors[clearColor];
		}
	}
	
	if (ISCUSTOMRENDERINGNEEDED)
	{
		memset_u16(dstColorLine, dstClearColor, pixCount);
	}
	else
	{
		memset_u16_fast<GPU_FRAMEBUFFER_NATIVE_WIDTH>(dstColorLine, dstClearColor);
	}
	
	memset(this->_dstLayerID, GPULayerID_Backdrop, pixCount);
	
	// init background color & priorities
	memset(this->_sprAlpha, 0, GPU_FRAMEBUFFER_NATIVE_WIDTH);
	memset(this->_sprType, OBJMode_Normal, GPU_FRAMEBUFFER_NATIVE_WIDTH);
	memset(this->_sprPrio, 0x7F, GPU_FRAMEBUFFER_NATIVE_WIDTH);
	memset(this->_sprWin, 0, GPU_FRAMEBUFFER_NATIVE_WIDTH);
	
	// init pixels priorities
	assert(NB_PRIORITIES == 4);
	this->_itemsForPriority[0].nbPixelsX = 0;
	this->_itemsForPriority[1].nbPixelsX = 0;
	this->_itemsForPriority[2].nbPixelsX = 0;
	this->_itemsForPriority[3].nbPixelsX = 0;
}

template<bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::RenderLine(const u16 l)
{
	
}

void GPUEngineBase::FramebufferPostprocess()
{
	this->RefreshAffineStartRegs();
}

const GPU_IOREG& GPUEngineBase::GetIORegisterMap() const
{
	return *this->_IORegisterMap;
}

bool GPUEngineBase::GetIsMasterBrightFullIntensity() const
{
	return this->_isMasterBrightFullIntensity;
}

/*****************************************************************************/
//			ENABLING / DISABLING LAYERS
/*****************************************************************************/

bool GPUEngineBase::GetEnableState()
{
	return CommonSettings.showGpu.screens[this->_engineID];
}

void GPUEngineBase::SetEnableState(bool theState)
{
	CommonSettings.showGpu.screens[this->_engineID] = theState;
}

bool GPUEngineBase::GetLayerEnableState(const size_t layerIndex)
{
	return CommonSettings.dispLayers[this->_engineID][layerIndex];
}

void GPUEngineBase::SetLayerEnableState(const size_t layerIndex, bool theState)
{
	CommonSettings.dispLayers[this->_engineID][layerIndex] = theState;
	this->_ResortBGLayers();
}

/*****************************************************************************/
//		ROUTINES FOR INSIDE / OUTSIDE WINDOW CHECKS
/*****************************************************************************/

// check whether (x,y) is within the rectangle (including wraparounds)
template<int WIN_NUM>
u8 GPUEngineBase::_WithinRect(const size_t x) const
{
	return this->_curr_win[WIN_NUM][x];
}

template <GPULayerID LAYERID>
FORCEINLINE void GPUEngineBase::_RenderPixel_CheckWindows(const size_t srcX, bool &didPassWindowTest, bool &enableColorEffect) const
{
	// If no windows enabled, then we don't need to perform any window tests.
	// In this case, the pixel always passes and the color effect is always processed.
	if (!this->_isAnyWindowEnabled)
	{
		didPassWindowTest = true;
		enableColorEffect = true;
		return;
	}
	
	// Window 0 has the highest priority, so always check this first.
	if (this->_WIN0_ENABLED)
	{
		if (this->_WithinRect<0>(srcX))
		{
			//INFO("bg%i passed win0 : (%i %i) was within (%i %i)(%i %i)\n", bgnum, x, gpu->_currentScanline, gpu->WIN0H0, gpu->WIN0V0, gpu->WIN0H1, gpu->WIN0V1);
			switch (LAYERID)
			{
				case GPULayerID_BG0: didPassWindowTest = (this->_IORegisterMap->WIN0IN.BG0_Enable != 0); break;
				case GPULayerID_BG1: didPassWindowTest = (this->_IORegisterMap->WIN0IN.BG1_Enable != 0); break;
				case GPULayerID_BG2: didPassWindowTest = (this->_IORegisterMap->WIN0IN.BG2_Enable != 0); break;
				case GPULayerID_BG3: didPassWindowTest = (this->_IORegisterMap->WIN0IN.BG3_Enable != 0); break;
				case GPULayerID_OBJ: didPassWindowTest = (this->_IORegisterMap->WIN0IN.OBJ_Enable != 0); break;
					
				default:
					break;
			}
			
			enableColorEffect = (this->_IORegisterMap->WIN0IN.Effect_Enable != 0);
			return;
		}
	}

	// Window 1 has medium priority, and is checked after Window 0.
	if (this->_WIN1_ENABLED)
	{
		if (this->_WithinRect<1>(srcX))
		{
			//INFO("bg%i passed win1 : (%i %i) was within (%i %i)(%i %i)\n", bgnum, x, gpu->_currentScanline, gpu->WIN1H0, gpu->WIN1V0, gpu->WIN1H1, gpu->WIN1V1);
			switch (LAYERID)
			{
				case GPULayerID_BG0: didPassWindowTest = (this->_IORegisterMap->WIN1IN.BG0_Enable != 0); break;
				case GPULayerID_BG1: didPassWindowTest = (this->_IORegisterMap->WIN1IN.BG1_Enable != 0); break;
				case GPULayerID_BG2: didPassWindowTest = (this->_IORegisterMap->WIN1IN.BG2_Enable != 0); break;
				case GPULayerID_BG3: didPassWindowTest = (this->_IORegisterMap->WIN1IN.BG3_Enable != 0); break;
				case GPULayerID_OBJ: didPassWindowTest = (this->_IORegisterMap->WIN1IN.OBJ_Enable != 0); break;
					
				default:
					break;
			}
			
			enableColorEffect = (this->_IORegisterMap->WIN1IN.Effect_Enable != 0);
			return;
		}
	}
	
	// Window OBJ has low priority, and is checked after both Window 0 and Window 1.
	if (this->_WINOBJ_ENABLED)
	{
		// it is in winOBJ, do we display ?
		// low priority
		if (this->_sprWin[srcX])
		{
			switch (LAYERID)
			{
				case GPULayerID_BG0: didPassWindowTest = (this->_IORegisterMap->WINOBJ.BG0_Enable != 0); break;
				case GPULayerID_BG1: didPassWindowTest = (this->_IORegisterMap->WINOBJ.BG1_Enable != 0); break;
				case GPULayerID_BG2: didPassWindowTest = (this->_IORegisterMap->WINOBJ.BG2_Enable != 0); break;
				case GPULayerID_BG3: didPassWindowTest = (this->_IORegisterMap->WINOBJ.BG3_Enable != 0); break;
				case GPULayerID_OBJ: didPassWindowTest = (this->_IORegisterMap->WINOBJ.OBJ_Enable != 0); break;
					
				default:
					break;
			}
			
			enableColorEffect = (this->_IORegisterMap->WINOBJ.Effect_Enable != 0);
			return;
		}
	}
	
	// If the pixel isn't inside any windows, then the pixel is outside, and therefore uses the WINOUT flags.
	// This has the lowest priority, and is always checked last.
	switch (LAYERID)
	{
		case GPULayerID_BG0: didPassWindowTest = (this->_IORegisterMap->WINOUT.BG0_Enable != 0); break;
		case GPULayerID_BG1: didPassWindowTest = (this->_IORegisterMap->WINOUT.BG1_Enable != 0); break;
		case GPULayerID_BG2: didPassWindowTest = (this->_IORegisterMap->WINOUT.BG2_Enable != 0); break;
		case GPULayerID_BG3: didPassWindowTest = (this->_IORegisterMap->WINOUT.BG3_Enable != 0); break;
		case GPULayerID_OBJ: didPassWindowTest = (this->_IORegisterMap->WINOUT.OBJ_Enable != 0); break;
			
		default:
			break;
	}
	
	enableColorEffect = (this->_IORegisterMap->WINOUT.Effect_Enable != 0);
}

/*****************************************************************************/
//			PIXEL RENDERING
/*****************************************************************************/
template <GPULayerID LAYERID, bool ISDEBUGRENDER>
FORCEINLINE void GPUEngineBase::_RenderPixel(const size_t srcX, const size_t dstX, const u16 src, const u8 srcAlpha, u16 *__restrict dstColorLine, u8 *__restrict dstLayerIDLine)
{
	if (ISDEBUGRENDER)
	{
		// If we're rendering pixels to a debugging context, then assume that the pixel
		// always passes the window test and that the color effect is always disabled.
		dstColorLine[dstX] = src | 0x8000;
		dstLayerIDLine[dstX] = LAYERID;
		return;
	}
	
	bool didPassWindowTest = true;
	bool enableColorEffect = true;
	
	this->_RenderPixel_CheckWindows<LAYERID>(srcX, didPassWindowTest, enableColorEffect);
	
	if (!didPassWindowTest)
	{
		return;
	}
	
	ColorEffect selectedEffect = ColorEffect_Disable;
	TBlendTable *selectedBlendTable = this->_blendTable;
	
	if (enableColorEffect)
	{
		const IOREG_BLDCNT &BLDCNT = this->_IORegisterMap->BLDCNT;
		const GPULayerID dstLayerID = (GPULayerID)dstLayerIDLine[dstX];
		bool srcEffectEnable = false;
		bool dstEffectEnable = (dstLayerID != LAYERID) && this->_blend2[dstLayerID];
		
		switch (LAYERID)
		{
			case GPULayerID_BG0:
				srcEffectEnable = (BLDCNT.BG0_Target1 != 0);
				break;
				
			case GPULayerID_BG1:
				srcEffectEnable = (BLDCNT.BG1_Target1 != 0);
				break;
				
			case GPULayerID_BG2:
				srcEffectEnable = (BLDCNT.BG2_Target1 != 0);
				break;
				
			case GPULayerID_BG3:
				srcEffectEnable = (BLDCNT.BG3_Target1 != 0);
				break;
				
			case GPULayerID_OBJ:
				srcEffectEnable = (BLDCNT.OBJ_Target1 != 0);
				break;
				
			default:
				break;
		}
		
		// Select the color effect based on the BLDCNT target flags.
		bool forceBlendEffect = false;
		
		if (LAYERID == GPULayerID_OBJ)
		{
			//translucent-capable OBJ are forcing the function to blend when the second target is satisfied
			const OBJMode objMode = (OBJMode)this->_sprType[srcX];
			const bool isObjTranslucentType = (objMode == OBJMode_Transparent) || (objMode == OBJMode_Bitmap);
			if (isObjTranslucentType && dstEffectEnable)
			{
				//obj without fine-grained alpha are using EVA/EVB for blending. this is signified by receiving 0xFF in the alpha
				//it's tested by the spriteblend demo and the glory of heracles title screen
				if (srcAlpha != 0xFF)
				{
					const u8 BLDALPHA_EVA = srcAlpha;
					const u8 BLDALPHA_EVB = 16 - srcAlpha;
					selectedBlendTable = &GPUEngineBase::_blendTable555[BLDALPHA_EVA][BLDALPHA_EVB];
				}
				
				forceBlendEffect = true;
			}
		}
		
		if (forceBlendEffect)
		{
			selectedEffect = ColorEffect_Blend;
		}
		else if (BLDCNT.ColorEffect == ColorEffect_Blend)
		{
			// For the Blend effect, both first and second target flags must be checked.
			if (srcEffectEnable && dstEffectEnable) selectedEffect = (ColorEffect)BLDCNT.ColorEffect;
		}
		else if ( (BLDCNT.ColorEffect == ColorEffect_IncreaseBrightness) || (BLDCNT.ColorEffect == ColorEffect_DecreaseBrightness) )
		{
			// For the Increase/Decrease Brightness effects, only the first target flag needs to be checked.
			// Test case: Bomberman Land Touch! dialog boxes will render too dark without this check.
			if (srcEffectEnable) selectedEffect = (ColorEffect)BLDCNT.ColorEffect;
		}
	}
	
	// Render the pixel using the selected color effect.
	u16 finalDstColor;
	
	switch (selectedEffect)
	{
		case ColorEffect_Disable:
			finalDstColor = src;
			break;
			
		case ColorEffect_IncreaseBrightness:
			finalDstColor = this->_currentFadeInColors[src & 0x7FFF];
			break;
			
		case ColorEffect_DecreaseBrightness:
			finalDstColor = this->_currentFadeOutColors[src & 0x7FFF];
			break;
			
		case ColorEffect_Blend:
			finalDstColor = this->_ColorEffectBlend(src, dstColorLine[dstX], selectedBlendTable);
			break;
	}
	
	dstColorLine[dstX] = finalDstColor | 0x8000;
	dstLayerIDLine[dstX] = LAYERID;
}

// TODO: Unify this method with GPUEngineBase::_RenderPixel().
// We can't unify this yet because the output framebuffer is in RGBA5551, but the 3D source pixels are in RGBA6665.
// However, GPUEngineBase::_RenderPixel() takes source pixels in RGB555. In order to unify the methods, all pixels
// must be processed in RGBA6665.
FORCEINLINE void GPUEngineBase::_RenderPixel3D(const size_t srcX, const size_t dstX, const FragmentColor src, u16 *__restrict dstColorLine, u8 *__restrict dstLayerIDLine)
{
	bool didPassWindowTest = true;
	bool enableColorEffect = true;
	
	this->_RenderPixel_CheckWindows<GPULayerID_BG0>(srcX, didPassWindowTest, enableColorEffect);
	
	if (!didPassWindowTest)
	{
		return;
	}
	
	// TODO: - should we do an alpha==0 -> bail out entirely check here?
	
	ColorEffect selectedEffect = ColorEffect_Disable;
	
	if (enableColorEffect)
	{
		const IOREG_BLDCNT &BLDCNT = this->_IORegisterMap->BLDCNT;
		const GPULayerID dstLayerID = (GPULayerID)dstLayerIDLine[dstX];
		bool srcEffectEnable = (this->_IORegisterMap->BLDCNT.BG0_Target1 != 0);
		bool dstEffectEnable = (dstLayerID != GPULayerID_BG0) && this->_blend2[dstLayerID];
		
		// Select the color effect based on the BLDCNT target flags.
		bool forceBlendEffect = false;
		
		// 3D rendering has a special override: If the destination pixel is set to blend, then always blend.
		// Test case: When starting a stage in Super Princess Peach, the screen will be solid black unless
		// blending is forced here.
		forceBlendEffect = dstEffectEnable;
		
		if (forceBlendEffect)
		{
			selectedEffect = ColorEffect_Blend;
		}
		else if (BLDCNT.ColorEffect == ColorEffect_Blend)
		{
			// For the Blend effect, both first and second target flags must be checked.
			if (srcEffectEnable && dstEffectEnable) selectedEffect = (ColorEffect)BLDCNT.ColorEffect;
		}
		else if ( (BLDCNT.ColorEffect == ColorEffect_IncreaseBrightness) || (BLDCNT.ColorEffect == ColorEffect_DecreaseBrightness) )
		{
			// For the Increase/Decrease Brightness effects, only the first target flag needs to be checked.
			// Test case: Bomberman Land Touch! dialog boxes will appear too dark without this check.
			if (srcEffectEnable) selectedEffect = (ColorEffect)BLDCNT.ColorEffect;
		}
	}
	
	// Render the pixel using the selected color effect.
	const u16 srcRGB555 = R6G6B6TORGB15(src.r, src.g, src.b);
	u16 finalDstColor;
	
	switch (selectedEffect)
	{
		case ColorEffect_Disable:
			finalDstColor = srcRGB555;
			break;
			
		case ColorEffect_IncreaseBrightness:
			finalDstColor = this->_currentFadeInColors[srcRGB555];
			break;
			
		case ColorEffect_DecreaseBrightness:
			finalDstColor = this->_currentFadeOutColors[srcRGB555];
			break;
			
		case ColorEffect_Blend:
		{
			if (src.a < 31)
			{
				const u8 alpha = src.a + 1;
				COLOR c2;
				COLOR cfinal;
				
				c2.val = dstColorLine[dstX];
				
				cfinal.bits.red = ((src.r * alpha) + ((c2.bits.red<<1) * (32 - alpha)))>>6;
				cfinal.bits.green = ((src.g * alpha) + ((c2.bits.green<<1) * (32 - alpha)))>>6;
				cfinal.bits.blue = ((src.b * alpha) + ((c2.bits.blue<<1) * (32 - alpha)))>>6;
				
				finalDstColor = cfinal.val;
			}
			else
			{
				finalDstColor = srcRGB555;
			}
			break;
		}
	}
	
	dstColorLine[dstX] = finalDstColor | 0x8000;
	dstLayerIDLine[dstX] = GPULayerID_BG0;
}

template<GPULayerID LAYERID, bool ISDEBUGRENDER, bool ISCUSTOMRENDERINGNEEDED, bool USECUSTOMVRAM>
FORCEINLINE void GPUEngineBase::____setFinalColorBck(u16 *__restrict dstColorLine, const u16 lineIndex, const u16 color, const size_t srcX)
{
	if (ISCUSTOMRENDERINGNEEDED)
	{
		u8 *__restrict dstLayerIDLine = this->_dstLayerID;
		
		const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
		
		for (size_t line = 0; line < _gpuDstLineCount[lineIndex]; line++)
		{
			const u16 *__restrict srcLine = (USECUSTOMVRAM) ? GPU->GetCustomVRAMBuffer() + (this->vramBlockBGIndex * _gpuVRAMBlockOffset) + ((_gpuDstLineIndex[lineIndex] + line) * dispInfo.customWidth) : NULL;
			
			for (size_t p = 0; p < _gpuDstPitchCount[srcX]; p++)
			{
				const size_t dstX = _gpuDstPitchIndex[srcX] + p;
				
				this->_RenderPixel<LAYERID, ISDEBUGRENDER>(srcX,
														   dstX,
														   (USECUSTOMVRAM) ? srcLine[dstX] : color,
														   0,
														   dstColorLine,
														   dstLayerIDLine);
			}
			
			dstColorLine += dispInfo.customWidth;
			dstLayerIDLine += dispInfo.customWidth;
		}
	}
	else
	{
		this->_RenderPixel<LAYERID, ISDEBUGRENDER>(srcX,
												   srcX,
												   color,
												   0,
												   dstColorLine,
												   this->_dstLayerID);
	}
}

//this was forced inline because most of the time it just falls through to setFinalColorBck() and the function call
//overhead was ridiculous and terrible
template<GPULayerID LAYERID, bool ISDEBUGRENDER, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED, bool USECUSTOMVRAM>
FORCEINLINE void GPUEngineBase::___setFinalColorBck(u16 *dstColorLine, const u16 lineIndex, u16 color, const size_t srcX, const bool opaque)
{
	//due to this early out, we will get incorrect behavior in cases where 
	//we enable mosaic in the middle of a frame. this is deemed unlikely.
	if (!MOSAIC)
	{
		if (opaque)
		{
			this->____setFinalColorBck<LAYERID, ISDEBUGRENDER, ISCUSTOMRENDERINGNEEDED, USECUSTOMVRAM>(dstColorLine, lineIndex, color, srcX);
		}
	}
	else
	{
		if (!opaque) color = 0xFFFF;
		else color &= 0x7FFF;
		
		if (this->_mosaicWidthBG[srcX].begin && this->_mosaicHeightBG[lineIndex].begin)
		{
			// Do nothing.
		}
		else
		{
			const size_t x_int = this->_mosaicWidthBG[srcX].trunc;
			color = this->_mosaicColors.bg[LAYERID][x_int];
		}
		
		this->_mosaicColors.bg[LAYERID][srcX] = color;
		
		if (color != 0xFFFF)
		{
			this->____setFinalColorBck<LAYERID, ISDEBUGRENDER, ISCUSTOMRENDERINGNEEDED, USECUSTOMVRAM>(dstColorLine, lineIndex, color, srcX);
		}
	}
}

template<GPULayerID LAYERID, bool ISDEBUGRENDER, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
FORCEINLINE void GPUEngineBase::__setFinalColorBck(u16 *dstColorLine, const u16 lineIndex, const u16 color, const size_t srcX, const bool opaque)
{
	return ___setFinalColorBck<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED, false>(dstColorLine, lineIndex, color, srcX, opaque);
}

//this is fantastically inaccurate.
//we do the early return even though it reduces the resulting accuracy
//because we need the speed, and because it is inaccurate anyway
void GPUEngineBase::_MosaicSpriteLinePixel(const size_t x, u16 l, u16 *__restrict dst, u8 *__restrict dst_alpha, u8 *__restrict typeTab, u8 *__restrict prioTab)
{
	const bool enableMosaic = (this->_oamList[this->_sprNum[x]].Mosaic != 0);
	if (!enableMosaic)
		return;
	
	const bool opaque = prioTab[x] <= 4;

	GPUEngineBase::MosaicColor::Obj objColor;
	objColor.color = LE_TO_LOCAL_16(dst[x]);
	objColor.alpha = dst_alpha[x];
	objColor.opaque = opaque;

	const size_t y = l;
	
	if (this->_mosaicWidthOBJ[x].begin && this->_mosaicHeightOBJ[y].begin)
	{
		// Do nothing.
	}
	else
	{
		const size_t x_int = this->_mosaicWidthOBJ[x].trunc;
		objColor = this->_mosaicColors.obj[x_int];
	}
	
	this->_mosaicColors.obj[x] = objColor;
	
	dst[x] = LE_TO_LOCAL_16(objColor.color);
	dst_alpha[x] = objColor.alpha;
	if (!objColor.opaque) prioTab[x] = 0x7F;
}

void GPUEngineBase::_MosaicSpriteLine(u16 l, u16 *__restrict dst, u8 *__restrict dst_alpha, u8 *__restrict typeTab, u8 *__restrict prioTab)
{
	if (!this->_isOBJMosaicSet)
	{
		return;
	}
	
	for (size_t i = 0; i < GPU_FRAMEBUFFER_NATIVE_WIDTH; i++)
	{
		this->_MosaicSpriteLinePixel(i, l, dst, dst_alpha, typeTab, prioTab);
	}
}

template<rot_fun fun, bool WRAP>
void GPUEngineBase::_rot_scale_op(u16 *__restrict dstColorLine, const u16 lineIndex, const IOREG_BGnParameter &param, const u16 LG, const s32 wh, const s32 ht, const u32 map, const u32 tile, const u16 *__restrict pal)
{
	IOREG_BGnX x = param.BGnX;
	IOREG_BGnY y = param.BGnY;
	const s32 dx = (s32)param.BGnPA.value;
	const s32 dy = (s32)param.BGnPC.value;
	
	// as an optimization, specially handle the fairly common case of
	// "unrotated + unscaled + no boundary checking required"
	if (dx == GPU_FRAMEBUFFER_NATIVE_WIDTH && dy == 0)
	{
		s32 auxX = (WRAP) ? x.Integer & (wh-1) : x.Integer;
		const s32 auxY = (WRAP) ? y.Integer & (ht-1) : y.Integer;
		
		if (WRAP || (auxX + LG < wh && auxX >= 0 && auxY < ht && auxY >= 0))
		{
			for (size_t i = 0; i < LG; i++)
			{
				fun(this, dstColorLine, lineIndex, auxX, auxY, wh, map, tile, pal, i);
				auxX++;
				
				if (WRAP)
					auxX = auxX & (wh-1);
			}
			
			return;
		}
	}
	
	for (size_t i = 0; i < LG; i++, x.value += dx, y.value += dy)
	{
		const s32 auxX = (WRAP) ? x.Integer & (wh-1) : x.Integer;
		const s32 auxY = (WRAP) ? y.Integer & (ht-1) : y.Integer;
		
		if (WRAP || ((auxX >= 0) && (auxX < wh) && (auxY >= 0) && (auxY < ht)))
			fun(this, dstColorLine, lineIndex, auxX, auxY, wh, map, tile, pal, i);
	}
}

template<GPULayerID LAYERID, rot_fun fun>
void GPUEngineBase::_apply_rot_fun(u16 *__restrict dstColorLine, const u16 lineIndex, const IOREG_BGnParameter &param, const u16 LG, const u32 map, const u32 tile, const u16 *__restrict pal)
{
	s32 wh = this->_BGLayer[LAYERID].size.width;
	s32 ht = this->_BGLayer[LAYERID].size.height;
	
	if (this->_BGLayer[LAYERID].isDisplayWrapped)
		this->_rot_scale_op<fun, true>(dstColorLine, lineIndex, param, LG, wh, ht, map, tile, pal);
	else
		this->_rot_scale_op<fun, false>(dstColorLine, lineIndex, param, LG, wh, ht, map, tile, pal);
}

/*****************************************************************************/
//			BACKGROUND RENDERING -TEXT-
/*****************************************************************************/
// render a text background to the combined pixelbuffer
template<GPULayerID LAYERID, bool ISDEBUGRENDER, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_RenderLine_TextBG(u16 *__restrict dstColorLine, const u16 lineIndex, u16 XBG, u16 YBG, u16 LG)
{
	const IOREG_DISPCNT &DISPCNT = this->_IORegisterMap->DISPCNT;
	const u16 lg     = this->_BGLayer[LAYERID].size.width;
	const u16 ht     = this->_BGLayer[LAYERID].size.height;
	const u16 wmask  = (lg-1);
	const u16 hmask  = (ht-1);
	u16 tmp = ((YBG & hmask) >> 3);
	u32 map;
	u32 tile;
	u16 xoff;
	u16 yoff;
	u32 xfin;

	s8 line_dir = 1;
	u32 mapinfo;
	TILEENTRY tileentry;

	u32 tmp_map = this->_BGLayer[LAYERID].tileMapAddress + (tmp&31) * 64;
	if (tmp > 31)
		tmp_map+= ADDRESS_STEP_512B << this->_BGLayer[LAYERID].BGnCNT.ScreenSize;

	map = tmp_map;
	tile = this->_BGLayer[LAYERID].tileEntryAddress;

	xoff = XBG;

	if (this->_BGLayer[LAYERID].BGnCNT.PaletteMode == PaletteMode_16x16) // color: 16 palette entries
	{
		const u16 *__restrict pal = this->_paletteBG;
		
		yoff = ((YBG&7)<<2);
		xfin = 8 - (xoff&7);
		for (size_t x = 0; x < LG; xfin = std::min<u16>(x+8, LG))
		{
			u16 tilePalette = 0;
			tmp = ((xoff&wmask)>>3);
			mapinfo = map + (tmp&0x1F) * 2;
			if(tmp>31) mapinfo += 32*32*2;
			tileentry.val = LOCAL_TO_LE_16( *(u16 *)MMU_gpu_map(mapinfo) );

			tilePalette = (tileentry.bits.Palette*16);

			u8 *__restrict line = (u8 *)MMU_gpu_map(tile + (tileentry.bits.TileNum * 0x20) + ((tileentry.bits.VFlip) ? (7*4)-yoff : yoff));
			u8 offset = 0;
			
			if (tileentry.bits.HFlip)
			{
				line += ( 3 - ((xoff & 7) >> 1) );
				for (; x < xfin; line--)
				{
					if (!(xoff & 1))
					{
						offset = *line >> 4;
						const u16 color = LE_TO_LOCAL_16( pal[offset + tilePalette] );
						this->__setFinalColorBck<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED>(dstColorLine, lineIndex, color, x, (offset != 0));
						x++; xoff++;
					}
					
					if (x < xfin)
					{
						offset = *line & 0xF;
						const u16 color = LE_TO_LOCAL_16( pal[offset + tilePalette] );
						this->__setFinalColorBck<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED>(dstColorLine, lineIndex, color, x, (offset != 0));
						x++; xoff++;
					}
				}
			}
			else
			{
				line += ((xoff & 7) >> 1);
				for (; x < xfin; line++)
				{
					if (!(xoff & 1))
					{
						offset = *line & 0xF;
						const u16 color = LE_TO_LOCAL_16( pal[offset + tilePalette] );
						this->__setFinalColorBck<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED>(dstColorLine, lineIndex, color, x, (offset != 0));
						x++; xoff++;
					}
					
					if (x < xfin)
					{
						offset = *line >> 4;
						const u16 color = LE_TO_LOCAL_16( pal[offset + tilePalette] );
						this->__setFinalColorBck<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED>(dstColorLine, lineIndex, color, x, (offset != 0));
						x++; xoff++;
					}
				}
			}
		}
	}
	else //256-color BG
	{
		const u16 *__restrict pal = (DISPCNT.ExBGxPalette_Enable) ? *(this->_BGLayer[LAYERID].extPalette) : this->_paletteBG;
		
		yoff = ((YBG&7)<<3);
		xfin = 8 - (xoff&7);
		const u32 extPalMask = -DISPCNT.ExBGxPalette_Enable;
		
		for (size_t x = 0; x < LG; xfin = std::min<u16>(x+8, LG))
		{
			tmp = (xoff & (lg-1))>>3;
			mapinfo = map + (tmp & 31) * 2;
			if(tmp > 31) mapinfo += 32*32*2;
			tileentry.val = LOCAL_TO_LE_16( *(u16 *)MMU_gpu_map(mapinfo) );
			
			const u16 *__restrict tilePal = (u16 *)((u8 *)pal + ((tileentry.bits.Palette<<9) & extPalMask));
			u8 *__restrict line = (u8 *)MMU_gpu_map(tile + (tileentry.bits.TileNum * 0x40) + ((tileentry.bits.VFlip) ? (7*8)-yoff : yoff));
			
			if (tileentry.bits.HFlip)
			{
				line += (7 - (xoff&7));
				line_dir = -1;
			}
			else
			{
				line += (xoff&7);
				line_dir = 1;
			}
			
			while (x < xfin)
			{
				const u16 color = LE_TO_LOCAL_16(tilePal[*line]);
				this->__setFinalColorBck<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED>(dstColorLine, lineIndex, color, x, (*line != 0));
				
				x++;
				xoff++;
				line += line_dir;
			}
		}
	}
}

template<GPULayerID LAYERID, bool ISDEBUGRENDER, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_RotBG2(u16 *__restrict dstColorLine, const u16 lineIndex, const IOREG_BGnParameter &param, const u16 LG)
{
//	printf("rot mode\n");
	this->_apply_rot_fun< LAYERID, rot_tiled_8bit_entry<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED> >(dstColorLine, lineIndex, param, LG, this->_BGLayer[LAYERID].tileMapAddress, this->_BGLayer[LAYERID].tileEntryAddress, this->_paletteBG);
}

template<GPULayerID LAYERID, bool ISDEBUGRENDER, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_ExtRotBG2(u16 *__restrict dstColorLine, const u16 lineIndex, const IOREG_BGnParameter &param, const u16 LG)
{
	const IOREG_DISPCNT &DISPCNT = this->_IORegisterMap->DISPCNT;
	
	u16 *__restrict pal = this->_paletteBG;

	switch (this->_BGLayer[LAYERID].type)
	{
		case BGType_AffineExt_256x16: // 16  bit bgmap entries
		{
			if (DISPCNT.ExBGxPalette_Enable)
			{
				pal = *(this->_BGLayer[LAYERID].extPalette);
				this->_apply_rot_fun< LAYERID, rot_tiled_16bit_entry<LAYERID, ISDEBUGRENDER, MOSAIC, true, ISCUSTOMRENDERINGNEEDED> >(dstColorLine, lineIndex, param, LG, this->_BGLayer[LAYERID].tileMapAddress, this->_BGLayer[LAYERID].tileEntryAddress, pal);
			}
			else
			{
				this->_apply_rot_fun< LAYERID, rot_tiled_16bit_entry<LAYERID, ISDEBUGRENDER, MOSAIC, false, ISCUSTOMRENDERINGNEEDED> >(dstColorLine, lineIndex, param, LG, this->_BGLayer[LAYERID].tileMapAddress, this->_BGLayer[LAYERID].tileEntryAddress, pal);
			}
			break;
		}
			
		case BGType_AffineExt_256x1: // 256 colors
			this->_apply_rot_fun< LAYERID, rot_256_map<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED> >(dstColorLine, lineIndex, param, LG, this->_BGLayer[LAYERID].BMPAddress, 0, pal);
			break;
			
		case BGType_AffineExt_Direct: // direct colors / BMP
		{
			if (ISCUSTOMRENDERINGNEEDED && (LAYERID == this->vramBGLayer))
			{
				this->_apply_rot_fun< LAYERID, rot_BMP_map<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED, true> >(dstColorLine, lineIndex, param, LG, this->_BGLayer[LAYERID].BMPAddress, 0, pal);
			}
			else
			{
				this->_apply_rot_fun< LAYERID, rot_BMP_map<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED, false> >(dstColorLine, lineIndex, param, LG, this->_BGLayer[LAYERID].BMPAddress, 0, pal);
			}
			break;
		}
			
		case BGType_Large8bpp: // large screen 256 colors
			this->_apply_rot_fun< LAYERID, rot_256_map<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED> >(dstColorLine, lineIndex, param, LG, this->_BGLayer[LAYERID].largeBMPAddress, 0, pal);
			break;
			
		default:
			break;
	}
}

/*****************************************************************************/
//			BACKGROUND RENDERING -HELPER FUNCTIONS-
/*****************************************************************************/

template<GPULayerID LAYERID, bool ISDEBUGRENDER, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_LineText(u16 *__restrict dstColorLine, const u16 lineIndex)
{
	if (ISDEBUGRENDER)
	{
		this->_RenderLine_TextBG<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED>(dstColorLine, lineIndex, 0, lineIndex, this->_BGLayer[LAYERID].size.width);
	}
	else
	{
		this->_RenderLine_TextBG<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED>(dstColorLine, lineIndex, this->_BGLayer[LAYERID].xOffset, lineIndex + this->_BGLayer[LAYERID].yOffset, 256);
	}
}

template<GPULayerID LAYERID, bool ISDEBUGRENDER, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_LineRot(u16 *__restrict dstColorLine, const u16 lineIndex)
{
	if (ISDEBUGRENDER)
	{
		static const IOREG_BGnParameter debugParams = {256, 0, 0, -77, 0, (s16)(lineIndex*GPU_FRAMEBUFFER_NATIVE_WIDTH)};
		this->_RotBG2<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED>(dstColorLine, lineIndex, debugParams, this->_BGLayer[LAYERID].size.width);
	}
	else
	{
		IOREG_BGnParameter *__restrict bgParams = (LAYERID == GPULayerID_BG2) ? (IOREG_BGnParameter *)&this->_IORegisterMap->BG2Param : (IOREG_BGnParameter *)&this->_IORegisterMap->BG3Param;
		
		this->_RotBG2<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED>(dstColorLine, lineIndex, *bgParams, 256);
		bgParams->BGnX.value += bgParams->BGnPB.value;
		bgParams->BGnY.value += bgParams->BGnPD.value;
	}
}

template<GPULayerID LAYERID, bool ISDEBUGRENDER, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_LineExtRot(u16 *__restrict dstColorLine, const u16 lineIndex)
{
	if (ISDEBUGRENDER)
	{
		static const IOREG_BGnParameter debugParams = {256, 0, 0, -77, 0, (s16)(lineIndex*GPU_FRAMEBUFFER_NATIVE_WIDTH)};
		this->_ExtRotBG2<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED>(dstColorLine, lineIndex, debugParams, this->_BGLayer[LAYERID].size.width);
	}
	else
	{
		IOREG_BGnParameter *__restrict bgParams = (LAYERID == GPULayerID_BG2) ? (IOREG_BGnParameter *)&this->_IORegisterMap->BG2Param : (IOREG_BGnParameter *)&this->_IORegisterMap->BG3Param;
		
		this->_ExtRotBG2<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED>(dstColorLine, lineIndex, *bgParams, 256);
		bgParams->BGnX.value += bgParams->BGnPB.value;
		bgParams->BGnY.value += bgParams->BGnPD.value;
	}
}

/*****************************************************************************/
//			SPRITE RENDERING -HELPER FUNCTIONS-
/*****************************************************************************/

/* if i understand it correct, and it fixes some sprite problems in chameleon shot */
/* we have a 15 bit color, and should use the pal entry bits as alpha ?*/
/* http://nocash.emubase.de/gbatek.htm#dsvideoobjs */
template<bool ISDEBUGRENDER>
void GPUEngineBase::_RenderSpriteBMP(const u8 spriteNum, const u16 l, u16 *__restrict dst, const u32 srcadr, u8 *__restrict dst_alpha, u8 *__restrict typeTab, u8 *__restrict prioTab, const u8 prio, const size_t lg, size_t sprX, size_t x, const s32 xdir, const u8 alpha)
{
	const u16 *__restrict bmpBuffer = (u16 *)MMU_gpu_map(srcadr);
	size_t i = 0;
	
#ifdef ENABLE_SSE2
	if (xdir == 1)
	{
		if (ISDEBUGRENDER)
		{
			const size_t ssePixCount = lg - (lg % 8);
			for (; i < ssePixCount; i += 8, x += 8, sprX += 8)
			{
				__m128i color_vec128 = _mm_loadu_si128((__m128i *)(bmpBuffer + x));
				const __m128i colorAlpha_vec128 = _mm_and_si128(color_vec128, _mm_set1_epi16(0x8000));
				const __m128i colorAlphaCompare = _mm_cmpeq_epi16(colorAlpha_vec128, _mm_set1_epi16(0x8000));
				color_vec128 = _mm_or_si128( _mm_and_si128(colorAlphaCompare, color_vec128), _mm_andnot_si128(colorAlphaCompare, _mm_loadu_si128((__m128i *)(dst + sprX))) );
				
				_mm_storeu_si128((__m128i *)(dst + sprX), color_vec128);
			}
		}
		else
		{
			const __m128i prio_vec128 = _mm_set1_epi8(prio);
			
			const size_t ssePixCount = lg - (lg % 16);
			for (; i < ssePixCount; i += 16, x += 16, sprX += 16)
			{
				__m128i prioTab_vec128 = _mm_loadu_si128((__m128i *)(prioTab + sprX));
				const __m128i prioCompare = _mm_cmplt_epi8(prio_vec128, prioTab_vec128);
				
				__m128i colorLo_vec128 = _mm_loadu_si128((__m128i *)(bmpBuffer + x));
				__m128i colorHi_vec128 = _mm_loadu_si128((__m128i *)(bmpBuffer + x + 8));
				
				const __m128i colorAlphaLo_vec128 = _mm_and_si128(colorLo_vec128, _mm_set1_epi16(0x8000));
				const __m128i colorAlphaHi_vec128 = _mm_and_si128(colorHi_vec128, _mm_set1_epi16(0x8000));
				
				const __m128i colorAlphaLoCompare = _mm_cmpeq_epi16(colorAlphaLo_vec128, _mm_set1_epi16(0x8000));
				const __m128i colorAlphaHiCompare = _mm_cmpeq_epi16(colorAlphaHi_vec128, _mm_set1_epi16(0x8000));
				const __m128i colorAlphaPackedCompare = _mm_cmpeq_epi8( _mm_packs_epi16(colorAlphaLoCompare, colorAlphaHiCompare), _mm_set1_epi8(0xFF) );
				
				const __m128i combinedPackedCompare = _mm_and_si128(prioCompare, colorAlphaPackedCompare);
				const __m128i combinedLoCompare = _mm_cmpeq_epi16( _mm_unpacklo_epi8(combinedPackedCompare, _mm_setzero_si128()), _mm_set1_epi16(0x00FF) );
				const __m128i combinedHiCompare = _mm_cmpeq_epi16( _mm_unpackhi_epi8(combinedPackedCompare, _mm_setzero_si128()), _mm_set1_epi16(0x00FF) );
				
				colorLo_vec128 = _mm_or_si128( _mm_and_si128(combinedLoCompare, colorLo_vec128), _mm_andnot_si128(combinedLoCompare, _mm_loadu_si128((__m128i *)(dst + sprX))) );
				colorHi_vec128 = _mm_or_si128( _mm_and_si128(combinedHiCompare, colorHi_vec128), _mm_andnot_si128(combinedHiCompare, _mm_loadu_si128((__m128i *)(dst + sprX + 8))) );
				const __m128i dstAlpha_vec128 = _mm_or_si128( _mm_and_si128(combinedPackedCompare, _mm_set1_epi8(alpha + 1)), _mm_andnot_si128(combinedPackedCompare, _mm_loadu_si128((__m128i *)(dst_alpha + sprX))) );
				const __m128i dstTypeTab_vec128 = _mm_or_si128( _mm_and_si128(combinedPackedCompare, _mm_set1_epi8(3)), _mm_andnot_si128(combinedPackedCompare, _mm_loadu_si128((__m128i *)(typeTab + sprX))) );
				prioTab_vec128 = _mm_or_si128( _mm_and_si128(combinedPackedCompare, prio_vec128), _mm_andnot_si128(combinedPackedCompare, prioTab_vec128) );
				const __m128i sprNum_vec128 = _mm_or_si128( _mm_and_si128(combinedPackedCompare, _mm_set1_epi8(spriteNum)), _mm_andnot_si128(combinedPackedCompare, _mm_loadu_si128((__m128i *)(this->_sprNum + sprX))) );
				
				_mm_storeu_si128((__m128i *)(dst + sprX), colorLo_vec128);
				_mm_storeu_si128((__m128i *)(dst + sprX + 8), colorHi_vec128);
				_mm_storeu_si128((__m128i *)(dst_alpha + sprX), dstAlpha_vec128);
				_mm_storeu_si128((__m128i *)(typeTab + sprX), dstTypeTab_vec128);
				_mm_storeu_si128((__m128i *)(prioTab + sprX), prioTab_vec128);
				_mm_storeu_si128((__m128i *)(this->_sprNum + sprX), sprNum_vec128);
			}
		}
	}
#endif
	
	for (; i < lg; i++, sprX++, x += xdir)
	{
		const u16 color = LE_TO_LOCAL_16(bmpBuffer[x]);
		
		//a cleared alpha bit suppresses the pixel from processing entirely; it doesnt exist
		if (ISDEBUGRENDER)
		{
			if (color & 0x8000)
			{
				dst[sprX] = color;
			}
		}
		else
		{
			if ((color & 0x8000) && (prio < prioTab[sprX]))
			{
				dst[sprX] = color;
				dst_alpha[sprX] = alpha+1;
				typeTab[sprX] = OBJMode_Bitmap;
				prioTab[sprX] = prio;
				this->_sprNum[sprX] = spriteNum;
			}
		}
	}
}

template<bool ISDEBUGRENDER>
void GPUEngineBase::_RenderSprite256(const u8 spriteNum, const u16 l, u16 *__restrict dst, const u32 srcadr, const u16 *__restrict pal, u8 *__restrict dst_alpha, u8 *__restrict typeTab, u8 *__restrict prioTab, const u8 prio, const size_t lg, size_t sprX, size_t x, const s32 xdir, const u8 alpha)
{
	for (size_t i = 0; i < lg; i++, ++sprX, x += xdir)
	{
		const u32 adr = srcadr + (x & 0x7) + ((x & 0xFFF8) << 3);
		const u8 *__restrict src = (u8 *)MMU_gpu_map(adr);
		const u8 palette_entry = *src;

		//a zero value suppresses the pixel from processing entirely; it doesnt exist
		if (ISDEBUGRENDER)
		{
			if (palette_entry > 0)
			{
				dst[sprX] = LE_TO_LOCAL_16(pal[palette_entry]);
			}
		}
		else
		{
			if ((palette_entry > 0) && (prio < prioTab[sprX]))
			{
				dst[sprX] = LE_TO_LOCAL_16(pal[palette_entry]);
				dst_alpha[sprX] = 0xFF;
				typeTab[sprX] = (alpha ? OBJMode_Transparent : OBJMode_Normal);
				prioTab[sprX] = prio;
				this->_sprNum[sprX] = spriteNum;
			}
		}
	}
}

template<bool ISDEBUGRENDER>
void GPUEngineBase::_RenderSprite16(const u8 spriteNum, const u16 l, u16 *__restrict dst, const u32 srcadr, const u16 *__restrict pal, u8 *__restrict dst_alpha, u8 *__restrict typeTab, u8 *__restrict prioTab, const u8 prio, const size_t lg, size_t sprX, size_t x, const s32 xdir, const u8 alpha)
{
	for (size_t i = 0; i < lg; i++, ++sprX, x += xdir)
	{
		const u16 x1 = x >> 1;
		const u32 adr = srcadr + (x1 & 0x3) + ((x1 & 0xFFFC) << 3);
		const u8 *__restrict src = (u8 *)MMU_gpu_map(adr);
		const u8 palette = *src;
		const u8 palette_entry = (x & 1) ? palette >> 4 : palette & 0xF;
		
		//a zero value suppresses the pixel from processing entirely; it doesnt exist
		if (ISDEBUGRENDER)
		{
			if (palette_entry > 0)
			{
				dst[sprX] = LE_TO_LOCAL_16(pal[palette_entry]);
			}
		}
		else
		{
			if ((palette_entry > 0) && (prio < prioTab[sprX]))
			{
				dst[sprX] = LE_TO_LOCAL_16(pal[palette_entry]);
				dst_alpha[sprX] = 0xFF;
				typeTab[sprX] = (alpha ? OBJMode_Transparent : OBJMode_Normal);
				prioTab[sprX] = prio;
				this->_sprNum[sprX] = spriteNum;
			}
		}
	}
}

void GPUEngineBase::_RenderSpriteWin(const u8 *src, const bool col256, const size_t lg, size_t sprX, size_t x, const s32 xdir)
{
	if (col256)
	{
		for (size_t i = 0; i < lg; i++, sprX++, x += xdir)
		{
			//_gpuSprWin[sprX] = (src[x])?1:0;
			if (src[(x & 7) + ((x & 0xFFF8) << 3)])
			{
				this->_sprWin[sprX] = 1;
			}
		}
	}
	else
	{
		for (size_t i = 0; i < lg; i++, sprX++, x += xdir)
		{
			const s32 x1 = x >> 1;
			const u8 palette = src[(x1 & 0x3) + ((x1 & 0xFFFC) << 3)];
			const u8 palette_entry = (x & 1) ? palette >> 4 : palette & 0xF;
			
			if (palette_entry)
			{
				this->_sprWin[sprX] = 1;
			}
		}
	}
}

// return val means if the sprite is to be drawn or not
bool GPUEngineBase::_ComputeSpriteVars(const OAMAttributes &spriteInfo, const u16 l, SpriteSize &sprSize, s32 &sprX, s32 &sprY, s32 &x, s32 &y, s32 &lg, s32 &xdir)
{
	x = 0;
	// get sprite location and size
	sprX = spriteInfo.X;
	sprY = spriteInfo.Y;
	sprSize = GPUEngineBase::_sprSizeTab[spriteInfo.Size][spriteInfo.Shape];
	lg = sprSize.width;
	
// FIXME: for rot/scale, a list of entries into the sprite should be maintained,
// that tells us where the first pixel of a screenline starts in the sprite,
// and how a step to the right in a screenline translates within the sprite

	//this wasn't really tested by anything. very unlikely to get triggered
	y = (l - sprY) & 0xFF;                        /* get the y line within sprite coords */
	if (y >= sprSize.height)
		return false;

	if ((sprX == GPU_FRAMEBUFFER_NATIVE_WIDTH) || (sprX+sprSize.width <= 0))	/* sprite pixels outside of line */
		return false;				/* not to be drawn */

	// sprite portion out of the screen (LEFT)
	if (sprX < 0)
	{
		lg += sprX;	
		x = -(sprX);
		sprX = 0;
	}
	// sprite portion out of the screen (RIGHT)
	if (sprX+sprSize.width >= GPU_FRAMEBUFFER_NATIVE_WIDTH)
		lg = GPU_FRAMEBUFFER_NATIVE_WIDTH - sprX;

	// switch TOP<-->BOTTOM
	if (spriteInfo.VFlip)
		y = sprSize.height - y - 1;
	
	// switch LEFT<-->RIGHT
	if (spriteInfo.HFlip)
	{
		x = sprSize.width - x - 1;
		xdir = -1;
	}
	else
	{
		xdir = 1;
	}
	
	return true;
}

/*****************************************************************************/
//			SPRITE RENDERING
/*****************************************************************************/


//TODO - refactor this so there isnt as much duped code between rotozoomed and non-rotozoomed versions

u32 GPUEngineBase::_SpriteAddressBMP(const OAMAttributes &spriteInfo, const SpriteSize sprSize, const s32 y)
{
	const IOREG_DISPCNT &DISPCNT = this->_IORegisterMap->DISPCNT;
	
	if (DISPCNT.OBJ_BMP_mapping)
	{
		//tested by buffy sacrifice damage blood splatters in corner
		return this->_sprMem + (spriteInfo.TileIndex << this->_sprBMPBoundary) + (y * sprSize.width * 2);
	}
	else
	{
		//2d mapping:
		//verified in rotozoomed mode by knights in the nightmare intro

		if (DISPCNT.OBJ_BMP_2D_dim)
			//256*256, verified by heroes of mana FMV intro
			return this->_sprMem + (((spriteInfo.TileIndex&0x3E0) * 64 + (spriteInfo.TileIndex&0x1F) * 8 + (y << 8)) << 1);
		else 
			//128*512, verified by harry potter and the order of the phoenix conversation portraits
			return this->_sprMem + (((spriteInfo.TileIndex&0x3F0) * 64 + (spriteInfo.TileIndex&0x0F) * 8 + (y << 7)) << 1);
	}
}

template <bool ISDEBUGRENDER>
void GPUEngineBase::_SpriteRender(const u16 lineIndex, u16 *__restrict dst, u8 *__restrict dst_alpha, u8 *__restrict typeTab, u8 *__restrict prioTab)
{
	if (this->_spriteRenderMode == SpriteRenderMode_Sprite1D)
		this->_SpriteRenderPerform<SpriteRenderMode_Sprite1D, ISDEBUGRENDER>(lineIndex, dst, dst_alpha, typeTab, prioTab);
	else
		this->_SpriteRenderPerform<SpriteRenderMode_Sprite2D, ISDEBUGRENDER>(lineIndex, dst, dst_alpha, typeTab, prioTab);
}

void GPUEngineBase::SpriteRenderDebug(const u16 lineIndex, u16 *dst)
{
	this->_SpriteRender<true>(lineIndex, dst, NULL, NULL, NULL);
}

template <SpriteRenderMode MODE, bool ISDEBUGRENDER>
void GPUEngineBase::_SpriteRenderPerform(const u16 lineIndex, u16 *__restrict dst, u8 *__restrict dst_alpha, u8 *__restrict typeTab, u8 *__restrict prioTab)
{
	const IOREG_DISPCNT &DISPCNT = this->_IORegisterMap->DISPCNT;
	size_t cost = 0;
	
	for (size_t i = 0; i < 128; i++)
	{
		OAMAttributes spriteInfo = this->_oamList[i];

		//for each sprite:
		if (cost >= 2130)
		{
			//out of sprite rendering time
			//printf("sprite overflow!\n");
			//return;		
		}

		//do we incur a cost if a sprite is disabled?? we guess so.
		cost += 2;
		
		// Check if sprite is disabled before everything
		if (spriteInfo.RotScale == 0 && spriteInfo.Disable != 0)
			continue;
		
		// Must explicitly convert endianness with attributes 1 and 2.
		spriteInfo.attr[1] = LOCAL_TO_LE_16(spriteInfo.attr[1]);
		spriteInfo.attr[2] = LOCAL_TO_LE_16(spriteInfo.attr[2]);
		
		const OBJMode objMode = (OBJMode)spriteInfo.Mode;

		SpriteSize sprSize;
		s32 sprX;
		s32 sprY;
		s32 x;
		s32 y;
		s32 lg;
		s32 xdir;
		u8 prio = spriteInfo.Priority;
		u16 *__restrict pal;
		u8 *__restrict src;
		u32 srcadr;
		
		if (spriteInfo.RotScale != 0)
		{
			s32		fieldX, fieldY, auxX, auxY, realX, realY, offset;
			u8		blockparameter;
			s16		dx, dmx, dy, dmy;
			u16		colour;

			// Get sprite positions and size
			sprX = spriteInfo.X;
			sprY = spriteInfo.Y;
			sprSize = GPUEngineBase::_sprSizeTab[spriteInfo.Size][spriteInfo.Shape];

			// Copy sprite size, to check change it if needed
			fieldX = sprSize.width;
			fieldY = sprSize.height;
			lg = sprSize.width;

			// If we are using double size mode, double our control vars
			if (spriteInfo.DoubleSize != 0)
			{
				fieldX <<= 1;
				fieldY <<= 1;
				lg <<= 1;
			}

			//check if the sprite is visible y-wise. unfortunately our logic for x and y is different due to our scanline based rendering
			//tested thoroughly by many large sprites in Super Robot Wars K which wrap around the screen
			y = (lineIndex - sprY) & 0xFF;
			if (y >= fieldY)
				continue;

			//check if sprite is visible x-wise.
			if ((sprX == GPU_FRAMEBUFFER_NATIVE_WIDTH) || (sprX + fieldX <= 0))
				continue;

			cost += (sprSize.width * 2) + 10;

			// Get which four parameter block is assigned to this sprite
			blockparameter = (spriteInfo.RotScaleIndex + (spriteInfo.HFlip << 3) + (spriteInfo.VFlip << 4)) * 4;

			// Get rotation/scale parameters
			dx  = LE_TO_LOCAL_16((s16)this->_oamList[blockparameter+0].attr3);
			dmx = LE_TO_LOCAL_16((s16)this->_oamList[blockparameter+1].attr3);
			dy  = LE_TO_LOCAL_16((s16)this->_oamList[blockparameter+2].attr3);
			dmy = LE_TO_LOCAL_16((s16)this->_oamList[blockparameter+3].attr3);
			
			// Calculate fixed point 8.8 start offsets
			realX = (sprSize.width  << 7) - (fieldX >> 1)*dx - (fieldY >> 1)*dmx + y*dmx;
			realY = (sprSize.height << 7) - (fieldX >> 1)*dy - (fieldY >> 1)*dmy + y*dmy;

			if (sprX < 0)
			{
				// If sprite is not in the window
				if (sprX + fieldX <= 0)
					continue;

				// Otherwise, is partially visible
				lg += sprX;
				realX -= sprX*dx;
				realY -= sprX*dy;
				sprX = 0;
			}
			else
			{
				if (sprX + fieldX > GPU_FRAMEBUFFER_NATIVE_WIDTH)
					lg = GPU_FRAMEBUFFER_NATIVE_WIDTH - sprX;
			}

			// If we are using 1 palette of 256 colours
			if (spriteInfo.PaletteMode == PaletteMode_1x256)
			{
				src = (u8 *)MMU_gpu_map(this->_sprMem + (spriteInfo.TileIndex << this->_sprBoundary));

				// If extended palettes are set, use them
				pal = (DISPCNT.ExOBJPalette_Enable) ? (u16 *)(MMU.ObjExtPal[this->_engineID][0]+(spriteInfo.PaletteIndex*ADDRESS_STEP_512B)) : this->_paletteOBJ;

				for (size_t j = 0; j < lg; ++j, ++sprX)
				{
					// Get the integer part of the fixed point 8.8, and check if it lies inside the sprite data
					auxX = (realX >> 8);
					auxY = (realY >> 8);

					if (auxX >= 0 && auxY >= 0 && auxX < sprSize.width && auxY < sprSize.height)
					{
						if (MODE == SpriteRenderMode_Sprite2D)
							offset = (auxX&0x7) + ((auxX&0xFFF8)<<3) + ((auxY>>3)<<10) + ((auxY&0x7)*8);
						else
							offset = (auxX&0x7) + ((auxX&0xFFF8)<<3) + ((auxY>>3)*sprSize.width*8) + ((auxY&0x7)*8);

						colour = src[offset];
						
						if (ISDEBUGRENDER)
						{
							if (colour)
							{
								dst[sprX] = LE_TO_LOCAL_16(pal[colour]);
							}
						}
						else
						{
							if (colour && (prio < prioTab[sprX]))
							{
								dst[sprX] = LE_TO_LOCAL_16(pal[colour]);
								dst_alpha[sprX] = 0xFF;
								typeTab[sprX] = objMode;
								prioTab[sprX] = prio;
							}
						}
					}

					// Add the rotation/scale coefficients, here the rotation/scaling is performed
					realX += dx;
					realY += dy;
				}
			}
			// Rotozoomed direct color
			else if (objMode == OBJMode_Bitmap)
			{
				//transparent (i think, dont bother to render?) if alpha is 0
				if (spriteInfo.PaletteIndex == 0)
					continue;

				srcadr = this->_SpriteAddressBMP(spriteInfo, sprSize, 0);

				for (size_t j = 0; j < lg; ++j, ++sprX)
				{
					// Get the integer part of the fixed point 8.8, and check if it lies inside the sprite data
					auxX = realX >> 8;
					auxY = realY >> 8;

					//this is all very slow, and so much dup code with other rotozoomed modes.
					//dont bother fixing speed until this whole thing gets reworked

					if (auxX >= 0 && auxY >= 0 && auxX < sprSize.width && auxY < sprSize.height)
					{
						if (DISPCNT.OBJ_BMP_2D_dim)
							//tested by knights in the nightmare
							offset = (this->_SpriteAddressBMP(spriteInfo, sprSize, auxY)-srcadr)/2+auxX;
						else //tested by lego indiana jones (somehow?)
							//tested by buffy sacrifice damage blood splatters in corner
							offset = auxX + (auxY*sprSize.width);


						u16* mem = (u16*)MMU_gpu_map(srcadr + (offset<<1));
						colour = LE_TO_LOCAL_16(*mem);
						
						if (ISDEBUGRENDER)
						{
							if (colour & 0x8000)
							{
								dst[sprX] = colour;
							}
						}
						else
						{
							if ((colour & 0x8000) && (prio < prioTab[sprX]))
							{
								dst[sprX] = colour;
								dst_alpha[sprX] = spriteInfo.PaletteIndex;
								typeTab[sprX] = objMode;
								prioTab[sprX] = prio;
							}
						}
					}

					// Add the rotation/scale coefficients, here the rotation/scaling is performed
					realX += dx;
					realY += dy;
				}
			}
			// Rotozoomed 16/16 palette
			else
			{
				if (MODE == SpriteRenderMode_Sprite2D)
				{
					src = (u8 *)MMU_gpu_map(this->_sprMem + (spriteInfo.TileIndex << 5));
				}
				else
				{
					src = (u8 *)MMU_gpu_map(this->_sprMem + (spriteInfo.TileIndex << this->_sprBoundary));
				}
				
				pal = this->_paletteOBJ + (spriteInfo.PaletteIndex << 4);

				for (size_t j = 0; j < lg; ++j, ++sprX)
				{
					// Get the integer part of the fixed point 8.8, and check if it lies inside the sprite data
					auxX = realX >> 8;
					auxY = realY >> 8;

					if (auxX >= 0 && auxY >= 0 && auxX < sprSize.width && auxY < sprSize.height)
					{
						if (MODE == SpriteRenderMode_Sprite2D)
							offset = ((auxX>>1)&0x3) + (((auxX>>1)&0xFFFC)<<3) + ((auxY>>3)<<10) + ((auxY&0x7)*4);
						else
							offset = ((auxX>>1)&0x3) + (((auxX>>1)&0xFFFC)<<3) + ((auxY>>3)*sprSize.width)*4 + ((auxY&0x7)*4);
						
						colour = src[offset];

						// Get 4bits value from the readed 8bits
						if (auxX&1)	colour >>= 4;
						else		colour &= 0xF;
						
						if (ISDEBUGRENDER)
						{
							if (colour)
							{
								dst[sprX] = LE_TO_LOCAL_16(pal[colour]);
							}
						}
						else
						{
							if (colour && (prio < prioTab[sprX]))
							{
								if (objMode == OBJMode_Window)
								{
									this->_sprWin[sprX] = 1;
								}
								else
								{
									dst[sprX] = LE_TO_LOCAL_16(pal[colour]);
									dst_alpha[sprX] = 0xFF;
									typeTab[sprX] = objMode;
									prioTab[sprX] = prio;
								}
							}
						}
					}

					// Add the rotation/scale coeficients, here the rotation/scaling  is performed
					realX += dx;
					realY += dy;
				}
			}
		}
		else //NOT rotozoomed
		{
			if (!this->_ComputeSpriteVars(spriteInfo, lineIndex, sprSize, sprX, sprY, x, y, lg, xdir))
				continue;

			cost += sprSize.width;

			if (objMode == OBJMode_Window)
			{
				if (MODE == SpriteRenderMode_Sprite2D)
				{
					if (spriteInfo.PaletteMode == PaletteMode_1x256)
						src = (u8 *)MMU_gpu_map(this->_sprMem + ((spriteInfo.TileIndex)<<5) + ((y>>3)<<10) + ((y&0x7)*8));
					else
						src = (u8 *)MMU_gpu_map(this->_sprMem + ((spriteInfo.TileIndex)<<5) + ((y>>3)<<10) + ((y&0x7)*4));
				}
				else
				{
					if (spriteInfo.PaletteMode == PaletteMode_1x256)
						src = (u8 *)MMU_gpu_map(this->_sprMem + (spriteInfo.TileIndex<<this->_sprBoundary) + ((y>>3)*sprSize.width*8) + ((y&0x7)*8));
					else
						src = (u8 *)MMU_gpu_map(this->_sprMem + (spriteInfo.TileIndex<<this->_sprBoundary) + ((y>>3)*sprSize.width*4) + ((y&0x7)*4));
				}

				this->_RenderSpriteWin(src, (spriteInfo.PaletteMode == PaletteMode_1x256), lg, sprX, x, xdir);
			}
			else if (objMode == OBJMode_Bitmap) //sprite is in BMP format
			{
				srcadr = this->_SpriteAddressBMP(spriteInfo, sprSize, y);

				//transparent (i think, dont bother to render?) if alpha is 0
				if (spriteInfo.PaletteIndex == 0)
					continue;
				
				this->_RenderSpriteBMP<ISDEBUGRENDER>(i, lineIndex, dst, srcadr, dst_alpha, typeTab, prioTab, prio, lg, sprX, x, xdir, spriteInfo.PaletteIndex);
			}
			else if (spriteInfo.PaletteMode == PaletteMode_1x256) //256 colors
			{
				if (MODE == SpriteRenderMode_Sprite2D)
					srcadr = this->_sprMem + ((spriteInfo.TileIndex)<<5) + ((y>>3)<<10) + ((y&0x7)*8);
				else
					srcadr = this->_sprMem + (spriteInfo.TileIndex<<this->_sprBoundary) + ((y>>3)*sprSize.width*8) + ((y&0x7)*8);
				
				pal = (DISPCNT.ExOBJPalette_Enable) ? (u16 *)(MMU.ObjExtPal[this->_engineID][0]+(spriteInfo.PaletteIndex*ADDRESS_STEP_512B)) : this->_paletteOBJ;
				this->_RenderSprite256<ISDEBUGRENDER>(i, lineIndex, dst, srcadr, pal, dst_alpha, typeTab, prioTab, prio, lg, sprX, x, xdir, (objMode == OBJMode_Transparent));
			}
			else // 16 colors
			{
				if (MODE == SpriteRenderMode_Sprite2D)
				{
					srcadr = this->_sprMem + ((spriteInfo.TileIndex)<<5) + ((y>>3)<<10) + ((y&0x7)*4);
				}
				else
				{
					srcadr = this->_sprMem + (spriteInfo.TileIndex<<this->_sprBoundary) + ((y>>3)*sprSize.width*4) + ((y&0x7)*4);
				}
				
				pal = this->_paletteOBJ + (spriteInfo.PaletteIndex << 4);
				this->_RenderSprite16<ISDEBUGRENDER>(i, lineIndex, dst, srcadr, pal, dst_alpha, typeTab, prioTab, prio, lg, sprX, x, xdir, (objMode == OBJMode_Transparent));
			}
		}
	}
}

template<bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_RenderLine_Layer(const u16 l, u16 *dstColorLine, const size_t dstLineWidth, const size_t dstLineCount)
{
	
}

template <bool ISFULLINTENSITYHINT>
void GPUEngineBase::ApplyMasterBrightness()
{
	const IOREG_MASTER_BRIGHT &MASTER_BRIGHT = this->_IORegisterMap->MASTER_BRIGHT;
	const u32 intensity = MASTER_BRIGHT.Intensity;
	
	if (!ISFULLINTENSITYHINT && (intensity == 0)) return;
	
	u16 *dst = this->renderedBuffer;
	const size_t pixCount = this->renderedWidth * this->renderedHeight;
	
	switch (MASTER_BRIGHT.Mode)
	{
		case GPUMasterBrightMode_Disable:
			break;
			
		case GPUMasterBrightMode_Up:
		{
			if (!ISFULLINTENSITYHINT && !this->_isMasterBrightFullIntensity)
			{
				size_t i = 0;
				
#ifdef ENABLE_SSE2
				const size_t ssePixCount = pixCount - (pixCount % 8);
				for (; i < ssePixCount; i += 8)
				{
					__m128i dstColor_vec128 = _mm_load_si128((__m128i *)(dst + i));
					dstColor_vec128 = _mm_and_si128(dstColor_vec128, _mm_set1_epi16(0x7FFF));
					
					dst[i+7] = GPUEngineBase::_fadeInColors[intensity][ _mm_extract_epi16(dstColor_vec128, 7) ];
					dst[i+6] = GPUEngineBase::_fadeInColors[intensity][ _mm_extract_epi16(dstColor_vec128, 6) ];
					dst[i+5] = GPUEngineBase::_fadeInColors[intensity][ _mm_extract_epi16(dstColor_vec128, 5) ];
					dst[i+4] = GPUEngineBase::_fadeInColors[intensity][ _mm_extract_epi16(dstColor_vec128, 4) ];
					dst[i+3] = GPUEngineBase::_fadeInColors[intensity][ _mm_extract_epi16(dstColor_vec128, 3) ];
					dst[i+2] = GPUEngineBase::_fadeInColors[intensity][ _mm_extract_epi16(dstColor_vec128, 2) ];
					dst[i+1] = GPUEngineBase::_fadeInColors[intensity][ _mm_extract_epi16(dstColor_vec128, 1) ];
					dst[i+0] = GPUEngineBase::_fadeInColors[intensity][ _mm_extract_epi16(dstColor_vec128, 0) ];
				}
#endif
				for (; i < pixCount; i++)
				{
					dst[i] = GPUEngineBase::_fadeInColors[intensity][ dst[i] & 0x7FFF ];
				}
			}
			else
			{
				// all white (optimization)
				memset_u16(dst, 0x7FFF, pixCount);
			}
			break;
		}
			
		case GPUMasterBrightMode_Down:
		{
			if (!ISFULLINTENSITYHINT && !this->_isMasterBrightFullIntensity)
			{
				size_t i = 0;
				
#ifdef ENABLE_SSE2
				const size_t ssePixCount = pixCount - (pixCount % 8);
				for (; i < ssePixCount; i += 8)
				{
					__m128i dstColor_vec128 = _mm_load_si128((__m128i *)(dst + i));
					dstColor_vec128 = _mm_and_si128(dstColor_vec128, _mm_set1_epi16(0x7FFF));
					
					dst[i+7] = GPUEngineBase::_fadeOutColors[intensity][ _mm_extract_epi16(dstColor_vec128, 7) ];
					dst[i+6] = GPUEngineBase::_fadeOutColors[intensity][ _mm_extract_epi16(dstColor_vec128, 6) ];
					dst[i+5] = GPUEngineBase::_fadeOutColors[intensity][ _mm_extract_epi16(dstColor_vec128, 5) ];
					dst[i+4] = GPUEngineBase::_fadeOutColors[intensity][ _mm_extract_epi16(dstColor_vec128, 4) ];
					dst[i+3] = GPUEngineBase::_fadeOutColors[intensity][ _mm_extract_epi16(dstColor_vec128, 3) ];
					dst[i+2] = GPUEngineBase::_fadeOutColors[intensity][ _mm_extract_epi16(dstColor_vec128, 2) ];
					dst[i+1] = GPUEngineBase::_fadeOutColors[intensity][ _mm_extract_epi16(dstColor_vec128, 1) ];
					dst[i+0] = GPUEngineBase::_fadeOutColors[intensity][ _mm_extract_epi16(dstColor_vec128, 0) ];
				}
#endif
				for (; i < pixCount; i++)
				{
					dst[i] = GPUEngineBase::_fadeOutColors[intensity][ dst[i] & 0x7FFF ];
				}
			}
			else
			{
				// all black (optimization)
				memset(dst, 0, pixCount * sizeof(u16));
			}
			break;
		}
			
		case GPUMasterBrightMode_Reserved:
			break;
	}
}

template<size_t WIN_NUM>
void GPUEngineBase::_SetupWindows(const u16 lineIndex)
{
	const u16 windowTop    = (WIN_NUM == 0) ? this->_IORegisterMap->WIN0V.Top : this->_IORegisterMap->WIN1V.Top;
	const u16 windowBottom = (WIN_NUM == 0) ? this->_IORegisterMap->WIN0V.Bottom : this->_IORegisterMap->WIN1V.Bottom;
	
	if (WIN_NUM == 0 && !this->_WIN0_ENABLED) goto allout;
	if (WIN_NUM == 1 && !this->_WIN1_ENABLED) goto allout;

	if (windowTop > windowBottom)
	{
		if((lineIndex < windowTop) && (lineIndex > windowBottom)) goto allout;
	}
	else
	{
		if((lineIndex < windowTop) || (lineIndex >= windowBottom)) goto allout;
	}

	//the x windows will apply for this scanline
	this->_curr_win[WIN_NUM] = this->_h_win[WIN_NUM];
	return;
	
allout:
	this->_curr_win[WIN_NUM] = GPUEngineBase::_winEmpty;
}

template<size_t WIN_NUM>
void GPUEngineBase::_UpdateWINH()
{
	//dont even waste any time in here if the window isnt enabled
	if (WIN_NUM == 0 && !this->_WIN0_ENABLED) return;
	if (WIN_NUM == 1 && !this->_WIN1_ENABLED) return;

	this->_needUpdateWINH[WIN_NUM] = false;
	const size_t windowLeft  = (WIN_NUM == 0) ? this->_IORegisterMap->WIN0H.Left  : this->_IORegisterMap->WIN1H.Left;
	const size_t windowRight = (WIN_NUM == 0) ? this->_IORegisterMap->WIN0H.Right : this->_IORegisterMap->WIN1H.Right;

	//the original logic: if you doubt the window code, please check it against the newer implementation below
	//if(windowLeft > windowRight)
	//{
	//	if((x < windowLeft) && (x > windowRight)) return false;
	//}
	//else
	//{
	//	if((x < windowLeft) || (x >= windowRight)) return false;
	//}

	if (windowLeft > windowRight)
	{
		memset(this->_h_win[WIN_NUM], 1, GPU_FRAMEBUFFER_NATIVE_WIDTH * sizeof(u8));
		memset(this->_h_win[WIN_NUM] + windowRight + 1, 0, (windowLeft - (windowRight + 1)) * sizeof(u8));
	}
	else
	{
		memset(this->_h_win[WIN_NUM], 0, GPU_FRAMEBUFFER_NATIVE_WIDTH * sizeof(u8));
		memset(this->_h_win[WIN_NUM] + windowLeft, 1, (windowRight - windowLeft) * sizeof(u8));
	}
}

void GPUEngineBase::UpdateVRAM3DUsageProperties_BGLayer(const size_t bankIndex, VRAM3DUsageProperties &outProperty)
{
	const IOREG_DISPCNT &DISPCNT = this->_IORegisterMap->DISPCNT;
	const bool isBG2UsingVRAM = (DISPCNT.BG2_Enable == 1) && (this->_BGLayer[GPULayerID_BG2].type == BGType_AffineExt_Direct) && (this->_BGLayer[GPULayerID_BG2].size.width == 256) && (this->_BGLayer[GPULayerID_BG2].size.height == 256);
	const bool isBG3UsingVRAM = (DISPCNT.BG3_Enable == 1) && (this->_BGLayer[GPULayerID_BG3].type == BGType_AffineExt_Direct) && (this->_BGLayer[GPULayerID_BG3].size.width == 256) && (this->_BGLayer[GPULayerID_BG3].size.height == 256);
	u8 selectedBGLayer = VRAM_NO_3D_USAGE;
	
	if (!isBG2UsingVRAM && !isBG3UsingVRAM)
	{
		return;
	}
	else if (!isBG2UsingVRAM && isBG3UsingVRAM)
	{
		selectedBGLayer = (this->_BGLayer[GPULayerID_BG3].priority <= this->_BGLayer[GPULayerID_BG0].priority) ? GPULayerID_BG3 : VRAM_NO_3D_USAGE;
	}
	else if (isBG2UsingVRAM && !isBG3UsingVRAM)
	{
		selectedBGLayer = (this->_BGLayer[GPULayerID_BG2].priority <= this->_BGLayer[GPULayerID_BG0].priority) ? GPULayerID_BG2 : VRAM_NO_3D_USAGE;
	}
	else if (isBG2UsingVRAM && isBG3UsingVRAM)
	{
		selectedBGLayer = (this->_BGLayer[GPULayerID_BG3].priority <= this->_BGLayer[GPULayerID_BG2].priority) ? ((this->_BGLayer[GPULayerID_BG3].priority <= this->_BGLayer[GPULayerID_BG0].priority) ? GPULayerID_BG3 : VRAM_NO_3D_USAGE) : ((this->_BGLayer[GPULayerID_BG2].priority <= this->_BGLayer[GPULayerID_BG0].priority) ? GPULayerID_BG2 : VRAM_NO_3D_USAGE);
	}
	
	if (selectedBGLayer != VRAM_NO_3D_USAGE)
	{
		const IOREG_BGnParameter *bgParams = (selectedBGLayer == GPULayerID_BG2) ? (IOREG_BGnParameter *)&this->_IORegisterMap->BG2Param : (IOREG_BGnParameter *)&this->_IORegisterMap->BG3Param;
		const IOREG_BGnX &savedBGnX = (selectedBGLayer == GPULayerID_BG2) ? this->savedBG2X : this->savedBG3X;
		const IOREG_BGnY &savedBGnY = (selectedBGLayer == GPULayerID_BG2) ? this->savedBG2Y : this->savedBG3Y;
		
		if ( (bgParams->BGnPA.value != 0x100) ||
		     (bgParams->BGnPB.value !=     0) ||
		     (bgParams->BGnPC.value !=     0) ||
		     (bgParams->BGnPD.value != 0x100) ||
		     (savedBGnX.value       !=     0) ||
		     (savedBGnY.value       !=     0) )
		{
			selectedBGLayer = VRAM_NO_3D_USAGE;
		}
	}
	
	this->vramBGLayer = selectedBGLayer;
	this->vramBlockBGIndex = bankIndex;
	this->isCustomRenderingNeeded = (selectedBGLayer != VRAM_NO_3D_USAGE);
}

void GPUEngineBase::UpdateVRAM3DUsageProperties_OBJLayer(const size_t bankIndex, VRAM3DUsageProperties &outProperty)
{
	const IOREG_DISPCNT &DISPCNT = this->_IORegisterMap->DISPCNT;
	if ((DISPCNT.OBJ_Enable != 1) || (DISPCNT.OBJ_BMP_mapping != 0) || (DISPCNT.OBJ_BMP_2D_dim != 1))
	{
		return;
	}
	
	const GPUEngineA *mainEngine = GPU->GetEngineMain();
	const IOREG_DISPCAPCNT &DISPCAPCNT = mainEngine->GetIORegisterMap().DISPCAPCNT;
	
	for (size_t spriteIndex = 0; spriteIndex < 128; spriteIndex++)
	{
		const OAMAttributes &spriteInfo = this->_oamList[spriteIndex];
		
		if ( ((spriteInfo.RotScale != 0) || (spriteInfo.Disable == 0)) && (spriteInfo.Mode == OBJMode_Bitmap) && (spriteInfo.PaletteIndex != 0) )
		{
			const u32 vramAddress = ((spriteInfo.TileIndex & 0x1F) << 5) + ((spriteInfo.TileIndex & ~0x1F) << 7);
			const SpriteSize sprSize = GPUEngineBase::_sprSizeTab[spriteInfo.Size][spriteInfo.Shape];
			
			if( (vramAddress == (DISPCAPCNT.VRAMWriteOffset * ADDRESS_STEP_32KB)) && (sprSize.width == 64) && (sprSize.height == 64) )
			{
				this->vramBlockOBJIndex = bankIndex;
				this->isCustomRenderingNeeded = true;
				return;
			}
		}
	}
}

template <GPULayerID LAYERID, bool ISDEBUGRENDER, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_RenderLine_LayerBG(u16 *dstColorLine, const u16 lineIndex)
{
	switch (GPUEngineBase::_mode2type[this->_IORegisterMap->DISPCNT.BG_Mode][LAYERID])
	{
		case BGType_Text: this->_LineText<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED>(dstColorLine, lineIndex); break;
		case BGType_Affine: this->_LineRot<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED>(dstColorLine, lineIndex); break;
		case BGType_AffineExt: this->_LineExtRot<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED>(dstColorLine, lineIndex); break;
		case BGType_Large8bpp: this->_LineExtRot<LAYERID, ISDEBUGRENDER, MOSAIC, ISCUSTOMRENDERINGNEEDED>(dstColorLine, lineIndex); break;
		case BGType_Invalid: 
			PROGINFO("Attempting to render an invalid BG type\n");
			break;
		default:
			break;
	}
}

template <GPULayerID LAYERID>
void GPUEngineBase::RenderLayerBG(u16 *dstColorBuffer)
{
	u16 *dstColorLine = dstColorBuffer;
	const size_t layerWidth = this->_BGLayer[LAYERID].size.width;
	const size_t layerHeight = this->_BGLayer[LAYERID].size.height;
	
	for (size_t lineIndex = 0; lineIndex < layerHeight; lineIndex++)
	{
		this->_RenderLine_LayerBG<LAYERID, true, false, false>(dstColorLine, lineIndex);
		dstColorLine += layerWidth;
	}
}

template<bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_HandleDisplayModeOff(u16 *dstColorLine, const size_t l, const size_t dstLineWidth, const size_t dstLineCount)
{
	// In this display mode, the display is cleared to white.
	if (ISCUSTOMRENDERINGNEEDED)
	{
		memset_u16(dstColorLine, 0x7FFF, dstLineWidth * dstLineCount);
	}
	else
	{
		memset_u16_fast<GPU_FRAMEBUFFER_NATIVE_WIDTH>(dstColorLine, 0x7FFF);
	}
}

template<bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_HandleDisplayModeNormal(u16 *dstColorLine, const size_t l, const size_t dstLineWidth, const size_t dstLineCount)
{
	//do nothing: it has already been generated into the right place
}

void GPUEngineBase::ParseReg_MOSAIC()
{
	this->_mosaicWidthBG = &GPUEngineBase::_mosaicLookup.table[this->_IORegisterMap->MOSAIC.BG_MosaicH][0];
	this->_mosaicHeightBG = &GPUEngineBase::_mosaicLookup.table[this->_IORegisterMap->MOSAIC.BG_MosaicV][0];
	this->_mosaicWidthOBJ = &GPUEngineBase::_mosaicLookup.table[this->_IORegisterMap->MOSAIC.OBJ_MosaicH][0];
	this->_mosaicHeightOBJ = &GPUEngineBase::_mosaicLookup.table[this->_IORegisterMap->MOSAIC.OBJ_MosaicV][0];
	
	this->_isBGMosaicSet = (this->_IORegisterMap->MOSAIC.BG_MosaicH != 0) || (this->_IORegisterMap->MOSAIC.BG_MosaicV != 0);
	this->_isOBJMosaicSet = (this->_IORegisterMap->MOSAIC.OBJ_MosaicH != 0) || (this->_IORegisterMap->MOSAIC.OBJ_MosaicV != 0);
}

void GPUEngineBase::ParseReg_BLDCNT()
{
	const IOREG_BLDCNT &BLDCNT = this->_IORegisterMap->BLDCNT;
	
	this->_blend2[GPULayerID_BG0] = (BLDCNT.BG0_Target2 != 0);
	this->_blend2[GPULayerID_BG1] = (BLDCNT.BG1_Target2 != 0);
	this->_blend2[GPULayerID_BG2] = (BLDCNT.BG2_Target2 != 0);
	this->_blend2[GPULayerID_BG3] = (BLDCNT.BG3_Target2 != 0);
	this->_blend2[GPULayerID_OBJ] = (BLDCNT.OBJ_Target2 != 0);
	this->_blend2[GPULayerID_Backdrop] = (BLDCNT.Backdrop_Target2 != 0);
}

void GPUEngineBase::ParseReg_BLDALPHA()
{
	const IOREG_BLDALPHA &BLDALPHA = this->_IORegisterMap->BLDALPHA;
	
	this->_BLDALPHA_EVA = (BLDALPHA.EVA >= 16) ? 16 : BLDALPHA.EVA;
	this->_BLDALPHA_EVB = (BLDALPHA.EVB >= 16) ? 16 : BLDALPHA.EVB;
	this->_blendTable = (TBlendTable *)&GPUEngineBase::_blendTable555[this->_BLDALPHA_EVA][this->_BLDALPHA_EVB][0][0];
}

void GPUEngineBase::ParseReg_BLDY()
{
	const IOREG_BLDY &BLDY = this->_IORegisterMap->BLDY;
	const u8 BLDY_EVY = (BLDY.EVY >= 16) ? 16 : BLDY.EVY;
	
	this->_currentFadeInColors = &GPUEngineBase::_fadeInColors[BLDY_EVY][0];
	this->_currentFadeOutColors = &GPUEngineBase::_fadeOutColors[BLDY_EVY][0];
}

template<size_t WINNUM>
void GPUEngineBase::ParseReg_WINnH()
{
	this->_needUpdateWINH[WINNUM] = true;
}

const BGLayerInfo& GPUEngineBase::GetBGLayerInfoByID(const GPULayerID layerID)
{
	return this->_BGLayer[layerID];
}

NDSDisplayID GPUEngineBase::GetDisplayByID() const
{
	return this->_targetDisplayID;
}

void GPUEngineBase::SetDisplayByID(const NDSDisplayID theDisplayID)
{
	const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
	this->_targetDisplayID = theDisplayID;
	this->nativeBuffer = dispInfo.nativeBuffer[theDisplayID];
	this->customBuffer = dispInfo.customBuffer[theDisplayID];
}

GPUEngineID GPUEngineBase::GetEngineID() const
{
	return this->_engineID;
}

void GPUEngineBase::SetCustomFramebufferSize(size_t w, size_t h)
{
	u16 *oldWorkingScanline = this->_workingDstColorBuffer;
	u8 *oldBGPixels = this->_dstLayerID;
	u16 *newWorkingScanline = (u16 *)malloc_alignedCacheLine(w * _gpuLargestDstLineCount * sizeof(u16));
	u8 *newBGPixels = (u8 *)malloc_alignedCacheLine(w * _gpuLargestDstLineCount * 4 * sizeof(u8)); // yes indeed, this is oversized. map debug tools try to write to it
	
	this->_workingDstColorBuffer = newWorkingScanline;
	this->_dstLayerID = newBGPixels;
	this->customBuffer = GPU->GetDisplayInfo().customBuffer[this->_targetDisplayID];
	
	free_aligned(oldWorkingScanline);
	free_aligned(oldBGPixels);
}

void GPUEngineBase::ResolveToCustomFramebuffer()
{
	const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
	
	if (dispInfo.didPerformCustomRender[this->_targetDisplayID])
	{
		return;
	}
	
	u16 *src = this->nativeBuffer;
	u16 *dst = this->customBuffer;
	
	if (dispInfo.isCustomSizeRequested)
	{
		u16 *dstColorLine = this->customBuffer;
		
		for (size_t y = 0; y < GPU_FRAMEBUFFER_NATIVE_HEIGHT; y++)
		{
			for (size_t x = 0; x < GPU_FRAMEBUFFER_NATIVE_WIDTH; x++)
			{
				for (size_t p = 0; p < _gpuDstPitchCount[x]; p++)
				{
					dst[_gpuDstPitchIndex[x] + p] = src[x];
				}
			}
			
			dstColorLine = dst + dispInfo.customWidth;
			
			for (size_t line = 1; line < _gpuDstLineCount[y]; line++)
			{
				memcpy(dstColorLine, dst, dispInfo.customWidth * sizeof(u16));
				dstColorLine += dispInfo.customWidth;
			}
			
			src += GPU_FRAMEBUFFER_NATIVE_WIDTH;
			dst = dstColorLine;
		}
	}
	else
	{
		memcpy(this->customBuffer, this->nativeBuffer, GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT * sizeof(u16));
	}
	
	GPU->SetDisplayDidCustomRender(this->_targetDisplayID, true);
}

void GPUEngineBase::RefreshAffineStartRegs()
{
	//this is speculative. the idea is as follows:
	//whenever the user updates the affine start position regs, it goes into the active regs immediately
	//(this is handled on the set event from MMU)
	//maybe it shouldnt take effect until the next hblank or something..
	//this is a based on a combination of:
	//heroes of mana intro FMV
	//SPP level 3-8 rotoscale room
	//NSMB raster fx backdrops
	//bubble bobble revolution classic mode
	//NOTE:
	//I am REALLY unsatisfied with this logic now. But it seems to be working.
	
	this->_IORegisterMap->BG2X = this->savedBG2X;
	this->_IORegisterMap->BG2Y = this->savedBG2Y;
	this->_IORegisterMap->BG3X = this->savedBG3X;
	this->_IORegisterMap->BG3Y = this->savedBG3Y;
}

// normally should have same addresses
void GPUEngineBase::REG_DISPx_pack_test()
{
	const GPU_IOREG *r = this->_IORegisterMap;
	
	printf("%08lx %02x\n", (uintptr_t)r, (u32)((uintptr_t)(&r->DISPCNT) - (uintptr_t)r) );
	printf("\t%02x\n", (u32)((uintptr_t)(&r->DISPSTAT) - (uintptr_t)r) );
	printf("\t%02x\n", (u32)((uintptr_t)(&r->VCOUNT) - (uintptr_t)r) );
	printf("\t%02x\n", (u32)((uintptr_t)(&r->BGnCNT[GPULayerID_BG0]) - (uintptr_t)r) );
	printf("\t%02x\n", (u32)((uintptr_t)(&r->BGnOFS[GPULayerID_BG0]) - (uintptr_t)r) );
	printf("\t%02x\n", (u32)((uintptr_t)(&r->BG2Param) - (uintptr_t)r) );
	printf("\t%02x\n", (u32)((uintptr_t)(&r->BG3Param) - (uintptr_t)r) );
	printf("\t%02x\n", (u32)((uintptr_t)(&r->DISP3DCNT) - (uintptr_t)r) );
	printf("\t%02x\n", (u32)((uintptr_t)(&r->DISPCAPCNT) - (uintptr_t)r) );
	printf("\t%02x\n", (u32)((uintptr_t)(&r->DISP_MMEM_FIFO) - (uintptr_t)r) );
}

template<GPUEngineID ENGINEID>
void GPUEngineBase::ParseAllRegisters()
{
	this->ParseReg_DISPCNT<ENGINEID>();
	// No need to call ParseReg_BGnCNT<GPUEngineID, GPULayerID>(), since it is
	// already called by ParseReg_DISPCNT().
	
	this->ParseReg_BGnHOFS<GPULayerID_BG0>();
	this->ParseReg_BGnHOFS<GPULayerID_BG1>();
	this->ParseReg_BGnHOFS<GPULayerID_BG2>();
	this->ParseReg_BGnHOFS<GPULayerID_BG3>();
	this->ParseReg_BGnVOFS<GPULayerID_BG0>();
	this->ParseReg_BGnVOFS<GPULayerID_BG1>();
	this->ParseReg_BGnVOFS<GPULayerID_BG2>();
	this->ParseReg_BGnVOFS<GPULayerID_BG3>();
	
	this->ParseReg_BGnX<GPULayerID_BG2>();
	this->ParseReg_BGnY<GPULayerID_BG2>();
	this->ParseReg_BGnX<GPULayerID_BG3>();
	this->ParseReg_BGnY<GPULayerID_BG3>();
	
	this->ParseReg_WINnH<0>();
	this->ParseReg_WINnH<1>();
	
	this->ParseReg_MOSAIC();
	this->ParseReg_BLDCNT();
	this->ParseReg_BLDALPHA();
	this->ParseReg_BLDY();
	this->ParseReg_MASTER_BRIGHT();
}

GPUEngineA::GPUEngineA()
{
	_engineID = GPUEngineID_Main;
	_targetDisplayID = NDSDisplayID_Main;
	_IORegisterMap = (GPU_IOREG *)MMU.ARM9_REG;
	_paletteBG = (u16 *)MMU.ARM9_VMEM;
	_paletteOBJ = (u16 *)(MMU.ARM9_VMEM + ADDRESS_STEP_512B);
	_oamList = (OAMAttributes *)(MMU.ARM9_OAM);
	_sprMem = MMU_AOBJ;
	
	_VRAMNativeBlockPtr[0] = (u16 *)MMU.ARM9_LCD;
	_VRAMNativeBlockPtr[1] = _VRAMNativeBlockPtr[0] + (1 * GPU_VRAM_BLOCK_LINES * GPU_FRAMEBUFFER_NATIVE_WIDTH);
	_VRAMNativeBlockPtr[2] = _VRAMNativeBlockPtr[0] + (2 * GPU_VRAM_BLOCK_LINES * GPU_FRAMEBUFFER_NATIVE_WIDTH);
	_VRAMNativeBlockPtr[3] = _VRAMNativeBlockPtr[0] + (3 * GPU_VRAM_BLOCK_LINES * GPU_FRAMEBUFFER_NATIVE_WIDTH);
	_VRAMaddrNative = _VRAMNativeBlockPtr[0];
	
	_3DFramebufferRGBA6665 = (FragmentColor *)malloc_alignedCacheLine(GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT * sizeof(FragmentColor));
	_3DFramebufferRGBA5551 = (u16 *)malloc_alignedCacheLine(GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT * sizeof(u16));
	gfx3d_Update3DFramebuffers(_3DFramebufferRGBA6665, _3DFramebufferRGBA5551);
}

GPUEngineA::~GPUEngineA()
{
	free_aligned(this->_3DFramebufferRGBA6665);
	free_aligned(this->_3DFramebufferRGBA5551);
	gfx3d_Update3DFramebuffers(NULL, NULL);
}

GPUEngineA* GPUEngineA::Allocate()
{
	return new(malloc_aligned64(sizeof(GPUEngineA))) GPUEngineA();
}

void GPUEngineA::FinalizeAndDeallocate()
{
	this->~GPUEngineA();
	free_aligned(this);
}

void GPUEngineA::Reset()
{
	const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
	this->_Reset_Base();
	
	memset(&this->_dispCapCnt, 0, sizeof(DISPCAPCNT_parsed));
	
	
	this->_BGLayer[GPULayerID_BG0].BMPAddress = MMU_ABG;
	this->_BGLayer[GPULayerID_BG1].BMPAddress = MMU_ABG;
	this->_BGLayer[GPULayerID_BG2].BMPAddress = MMU_ABG;
	this->_BGLayer[GPULayerID_BG3].BMPAddress = MMU_ABG;
	
	this->_BGLayer[GPULayerID_BG0].largeBMPAddress = MMU_ABG;
	this->_BGLayer[GPULayerID_BG1].largeBMPAddress = MMU_ABG;
	this->_BGLayer[GPULayerID_BG2].largeBMPAddress = MMU_ABG;
	this->_BGLayer[GPULayerID_BG3].largeBMPAddress = MMU_ABG;
	
	this->_BGLayer[GPULayerID_BG0].tileMapAddress = MMU_ABG;
	this->_BGLayer[GPULayerID_BG1].tileMapAddress = MMU_ABG;
	this->_BGLayer[GPULayerID_BG2].tileMapAddress = MMU_ABG;
	this->_BGLayer[GPULayerID_BG3].tileMapAddress = MMU_ABG;
	
	this->_BGLayer[GPULayerID_BG0].tileEntryAddress = MMU_ABG;
	this->_BGLayer[GPULayerID_BG1].tileEntryAddress = MMU_ABG;
	this->_BGLayer[GPULayerID_BG2].tileEntryAddress = MMU_ABG;
	this->_BGLayer[GPULayerID_BG3].tileEntryAddress = MMU_ABG;
	
	this->SetDisplayByID(NDSDisplayID_Main);
	
	memset(this->_3DFramebufferRGBA6665, 0, dispInfo.customWidth * dispInfo.customHeight * sizeof(FragmentColor));
	memset(this->_3DFramebufferRGBA5551, 0, dispInfo.customWidth * dispInfo.customHeight * sizeof(u16));
}

void GPUEngineA::ParseReg_DISPCAPCNT()
{
	const IOREG_DISPCNT &DISPCNT = this->_IORegisterMap->DISPCNT;
	const IOREG_DISPCAPCNT &DISPCAPCNT = this->_IORegisterMap->DISPCAPCNT;
	
	this->_dispCapCnt.EVA = (DISPCAPCNT.EVA >= 16) ? 16 : DISPCAPCNT.EVA;
	this->_dispCapCnt.EVB = (DISPCAPCNT.EVB >= 16) ? 16 : DISPCAPCNT.EVB;
	this->_dispCapCnt.readOffset = (DISPCNT.DisplayMode == GPUDisplayMode_VRAM) ? 0 : DISPCAPCNT.VRAMReadOffset;
	
	switch (DISPCAPCNT.CaptureSize)
	{
		case 0:
			this->_dispCapCnt.capx = DISPCAPCNT_parsed::_128;
			this->_dispCapCnt.capy = 128;
			break;
			
		case 1:
			this->_dispCapCnt.capx = DISPCAPCNT_parsed::_256;
			this->_dispCapCnt.capy = 64;
			break;
			
		case 2:
			this->_dispCapCnt.capx = DISPCAPCNT_parsed::_256;
			this->_dispCapCnt.capy = 128;
			break;
			
		case 3:
			this->_dispCapCnt.capx = DISPCAPCNT_parsed::_256;
			this->_dispCapCnt.capy = 192;
			break;
			
		default:
			break;
	}
	
	/*INFO("Capture 0x%X:\n EVA=%i, EVB=%i, wBlock=%i, wOffset=%i, capX=%i, capY=%i\n rBlock=%i, rOffset=%i, srcCap=%i, dst=0x%X, src=0x%X\n srcA=%i, srcB=%i\n\n",
	 val, this->_dispCapCnt.EVA, this->_dispCapCnt.EVB, this->_dispCapCnt.writeBlock, this->_dispCapCnt.writeOffset,
	 this->_dispCapCnt.capx, this->_dispCapCnt.capy, this->_dispCapCnt.readBlock, this->_dispCapCnt.readOffset,
	 this->_dispCapCnt.capSrc, this->_dispCapCnt.dst - MMU.ARM9_LCD, this->_dispCapCnt.src - MMU.ARM9_LCD,
	 this->_dispCapCnt.srcA, this->_dispCapCnt.srcB);*/
}

FragmentColor* GPUEngineA::Get3DFramebufferRGBA6665() const
{
	return this->_3DFramebufferRGBA6665;
}

u16* GPUEngineA::Get3DFramebufferRGBA5551() const
{
	return this->_3DFramebufferRGBA5551;
}

void GPUEngineA::UpdateSelectedVRAMBlock()
{
	const IOREG_DISPCNT &DISPCNT = this->_IORegisterMap->DISPCNT;
	this->_VRAMaddrNative = this->_VRAMNativeBlockPtr[DISPCNT.VRAM_Block];
	this->_VRAMaddrCustom = this->_VRAMCustomBlockPtr[DISPCNT.VRAM_Block];
}

void GPUEngineA::SetCustomFramebufferSize(size_t w, size_t h)
{
	this->GPUEngineBase::SetCustomFramebufferSize(w, h);
	
	FragmentColor *oldColorRGBA6665Buffer = this->_3DFramebufferRGBA6665;
	u16 *oldColorRGBA5551Buffer = this->_3DFramebufferRGBA5551;
	FragmentColor *newColorRGBA6665Buffer = (FragmentColor *)malloc_alignedCacheLine(w * h * sizeof(FragmentColor));
	u16 *newColorRGBA5551 = (u16 *)malloc_alignedCacheLine(w * h * sizeof(u16));
	
	this->_3DFramebufferRGBA6665 = newColorRGBA6665Buffer;
	this->_3DFramebufferRGBA5551 = newColorRGBA5551;
	gfx3d_Update3DFramebuffers(this->_3DFramebufferRGBA6665, this->_3DFramebufferRGBA5551);
	
	const IOREG_DISPCNT &DISPCNT = this->_IORegisterMap->DISPCNT;
	this->_VRAMCustomBlockPtr[0] = GPU->GetCustomVRAMBuffer();
	this->_VRAMCustomBlockPtr[1] = this->_VRAMCustomBlockPtr[0] + (1 * _gpuCaptureLineIndex[GPU_VRAM_BLOCK_LINES] * w);
	this->_VRAMCustomBlockPtr[2] = this->_VRAMCustomBlockPtr[0] + (2 * _gpuCaptureLineIndex[GPU_VRAM_BLOCK_LINES] * w);
	this->_VRAMCustomBlockPtr[3] = this->_VRAMCustomBlockPtr[0] + (3 * _gpuCaptureLineIndex[GPU_VRAM_BLOCK_LINES] * w);
	this->_VRAMaddrCustom = this->_VRAMCustomBlockPtr[DISPCNT.VRAM_Block];
	
	free_aligned(oldColorRGBA6665Buffer);
	free_aligned(oldColorRGBA5551Buffer);
}

template<bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineA::RenderLine(const u16 l)
{
	const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
	const size_t dstLineWidth = (ISCUSTOMRENDERINGNEEDED) ? dispInfo.customWidth : GPU_FRAMEBUFFER_NATIVE_WIDTH;
	const size_t dstLineCount = (ISCUSTOMRENDERINGNEEDED) ? _gpuDstLineCount[l] : 1;
	const size_t dstLineIndex = (ISCUSTOMRENDERINGNEEDED) ? _gpuDstLineIndex[l] : l;
	u16 *dstColorLine = (ISCUSTOMRENDERINGNEEDED) ? this->customBuffer + (dstLineIndex * dstLineWidth) : this->nativeBuffer + (dstLineIndex * dstLineWidth);
	
	const IOREG_DISPCAPCNT &DISPCAPCNT = this->_IORegisterMap->DISPCAPCNT;
	
	//cache some parameters which are assumed to be stable throughout the rendering of the entire line
	if (this->_needUpdateWINH[0]) this->_UpdateWINH<0>();
	if (this->_needUpdateWINH[1]) this->_UpdateWINH<1>();
	
	this->_SetupWindows<0>(l);
	this->_SetupWindows<1>(l);
	
	const GPUDisplayMode displayMode = (GPUDisplayMode)this->_IORegisterMap->DISPCNT.DisplayMode;
	
	// Optimization: For normal display mode, render straight to the output buffer when that is what we are going to end
	// up displaying anyway. Otherwise, we need to use the working buffer.
	u16 *renderedDstColorLine = (displayMode == GPUDisplayMode_Normal) ? dstColorLine : this->_workingDstColorBuffer;
	
	// generate the 2d engine output
	this->_RenderLine_Layer<ISCUSTOMRENDERINGNEEDED>(l, renderedDstColorLine, dstLineWidth, dstLineCount);
	
	switch (displayMode)
	{
		case GPUDisplayMode_Off: // Display Off(Display white)
			this->_HandleDisplayModeOff<ISCUSTOMRENDERINGNEEDED>(dstColorLine, l, dstLineWidth, dstLineCount);
			break;
			
		case GPUDisplayMode_Normal: // Display BG and OBJ layers
			this->_HandleDisplayModeNormal<ISCUSTOMRENDERINGNEEDED>(dstColorLine, l, dstLineWidth, dstLineCount);
			break;
			
		case GPUDisplayMode_VRAM: // Display vram framebuffer
			this->_HandleDisplayModeVRAM<ISCUSTOMRENDERINGNEEDED>(dstColorLine, l, dstLineWidth, dstLineCount);
			break;
			
		case GPUDisplayMode_MainMemory: // Display memory FIFO
			this->_HandleDisplayModeMainMemory<ISCUSTOMRENDERINGNEEDED>(dstColorLine, l, dstLineWidth, dstLineCount);
			break;
	}
	
	//capture after displaying so that we can safely display vram before overwriting it here
	
	//BUG!!! if someone is capturing and displaying both from the fifo, then it will have been
	//consumed above by the display before we get here
	//(is that even legal? i think so)
	if ((DISPCAPCNT.CaptureEnable != 0) && (vramConfiguration.banks[DISPCAPCNT.VRAMWriteBlock].purpose == VramConfiguration::LCDC) && (l < this->_dispCapCnt.capy))
	{
		if (this->_dispCapCnt.capx == DISPCAPCNT_parsed::_128)
		{
			this->_RenderLine_DisplayCapture<ISCUSTOMRENDERINGNEEDED, GPU_FRAMEBUFFER_NATIVE_WIDTH/2>(renderedDstColorLine, l);
		}
		else
		{
			this->_RenderLine_DisplayCapture<ISCUSTOMRENDERINGNEEDED, GPU_FRAMEBUFFER_NATIVE_WIDTH>(renderedDstColorLine, l);
		}
	}
}

template <bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineA::_RenderLine_Layer(const u16 l, u16 *dstColorLine, const size_t dstLineWidth, const size_t dstLineCount)
{
	const size_t dstLineIndex = _gpuDstLineIndex[l];
	itemsForPriority_t *__restrict item;
	
	const u16 backdropColor = LE_TO_LOCAL_16(this->_paletteBG[0]) & 0x7FFF;
	this->_RenderLine_Clear<ISCUSTOMRENDERINGNEEDED>(backdropColor, l, dstColorLine, dstLineWidth, dstLineCount);
	
	// for all the pixels in the line
	if (this->_enableLayer[GPULayerID_OBJ])
	{
		//n.b. - this is clearing the sprite line buffer to the background color,
		memset_u16_fast<GPU_FRAMEBUFFER_NATIVE_WIDTH>(this->_sprColor, backdropColor);
		
		//zero 06-may-09: I properly supported window color effects for backdrop, but I am not sure
		//how it interacts with this. I wish we knew why we needed this
		
		this->_SpriteRender<false>(l, this->_sprColor, this->_sprAlpha, this->_sprType, this->_sprPrio);
		this->_MosaicSpriteLine(l, this->_sprColor, this->_sprAlpha, this->_sprType, this->_sprPrio);
		
		for (size_t i = 0; i < GPU_FRAMEBUFFER_NATIVE_WIDTH; i++)
		{
			// assign them to the good priority item
			const size_t prio = this->_sprPrio[i];
			if (prio >= 4) continue;
			
			item = &(this->_itemsForPriority[prio]);
			item->PixelsX[item->nbPixelsX] = i;
			item->nbPixelsX++;
		}
	}
	
	// paint lower priorities first
	// then higher priorities on top
	for (size_t prio = NB_PRIORITIES; prio > 0; )
	{
		prio--;
		item = &(this->_itemsForPriority[prio]);
		// render BGs
		if (this->_isAnyBGLayerEnabled)
		{
			for (size_t i = 0; i < item->nbBGs; i++)
			{
				const GPULayerID layerID = (GPULayerID)item->BGs[i];
				if (this->_enableLayer[layerID])
				{
					if (layerID == GPULayerID_BG0 && this->is3DEnabled)
					{
						const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
						const float customWidthScale = (float)dispInfo.customWidth / (float)GPU_FRAMEBUFFER_NATIVE_WIDTH;
						const FragmentColor *__restrict srcLine = this->_3DFramebufferRGBA6665 + (dstLineIndex * dispInfo.customWidth);
						u16 *__restrict dstColorLinePtr = dstColorLine;
						u8 *__restrict layerIDLine = this->_dstLayerID;
						
						// Horizontally offset the 3D layer by this amount.
						// Test case: Blowing up large objects in Nanostray 2 will cause the main screen to shake horizontally.
						const u16 hofs = (u16)( ((float)this->_BGLayer[GPULayerID_BG0].xOffset * customWidthScale) + 0.5f );
						
						for (size_t line = 0; line < dstLineCount; line++)
						{
							for (size_t dstX = 0; dstX < dstLineWidth; dstX++)
							{
								size_t srcX = dstX + hofs;
								if (srcX >= dstLineWidth * 2)
								{
									srcX -= dstLineWidth * 2;
								}
								
								if (srcX >= dstLineWidth || srcLine[srcX].a == 0)
									continue;
								
								this->_RenderPixel3D(_gpuDstToSrcIndex[dstX],
													 dstX,
													 srcLine[srcX],
													 dstColorLinePtr,
													 layerIDLine);
							}
							
							srcLine += dstLineWidth;
							dstColorLinePtr += dstLineWidth;
							layerIDLine += dstLineWidth;
						}
						
						continue;
					}
					
#ifndef DISABLE_MOSAIC
					if (this->_BGLayer[layerID].isMosaic && this->_isBGMosaicSet)
					{
						switch (layerID)
						{
							case GPULayerID_BG0: this->_RenderLine_LayerBG<GPULayerID_BG0, false, true, ISCUSTOMRENDERINGNEEDED>(dstColorLine, l); break;
							case GPULayerID_BG1: this->_RenderLine_LayerBG<GPULayerID_BG1, false, true, ISCUSTOMRENDERINGNEEDED>(dstColorLine, l); break;
							case GPULayerID_BG2: this->_RenderLine_LayerBG<GPULayerID_BG2, false, true, ISCUSTOMRENDERINGNEEDED>(dstColorLine, l); break;
							case GPULayerID_BG3: this->_RenderLine_LayerBG<GPULayerID_BG3, false, true, ISCUSTOMRENDERINGNEEDED>(dstColorLine, l); break;
								
							default:
								break;
						}
					}
					else
#endif
					{
						switch (layerID)
						{
							case GPULayerID_BG0: this->_RenderLine_LayerBG<GPULayerID_BG0, false, false, ISCUSTOMRENDERINGNEEDED>(dstColorLine, l); break;
							case GPULayerID_BG1: this->_RenderLine_LayerBG<GPULayerID_BG1, false, false, ISCUSTOMRENDERINGNEEDED>(dstColorLine, l); break;
							case GPULayerID_BG2: this->_RenderLine_LayerBG<GPULayerID_BG2, false, false, ISCUSTOMRENDERINGNEEDED>(dstColorLine, l); break;
							case GPULayerID_BG3: this->_RenderLine_LayerBG<GPULayerID_BG3, false, false, ISCUSTOMRENDERINGNEEDED>(dstColorLine, l); break;
								
							default:
								break;
						}
					}
				} //layer enabled
			}
		}
		
		// render sprite Pixels
		if (this->_enableLayer[GPULayerID_OBJ])
		{
			u16 *__restrict dstColorLinePtr = dstColorLine;
			u8 *__restrict layerIDLine = this->_dstLayerID;
			
			if (ISCUSTOMRENDERINGNEEDED)
			{
				const bool useCustomVRAM = (this->vramBlockOBJIndex != VRAM_NO_3D_USAGE);
				const u16 *__restrict srcLine = (useCustomVRAM) ? GPU->GetCustomVRAMBuffer() + (this->vramBlockOBJIndex * _gpuVRAMBlockOffset) + (dstLineIndex * dstLineWidth) : NULL;
				
				for (size_t line = 0; line < dstLineCount; line++)
				{
					for (size_t i = 0; i < item->nbPixelsX; i++)
					{
						const size_t srcX = item->PixelsX[i];
						
						for (size_t p = 0; p < _gpuDstPitchCount[srcX]; p++)
						{
							const size_t dstX = _gpuDstPitchIndex[srcX] + p;
							
							this->_RenderPixel<GPULayerID_OBJ, false>(srcX,
																	  dstX,
																	  (useCustomVRAM) ? srcLine[dstX] : this->_sprColor[srcX],
																	  this->_sprAlpha[srcX],
																	  dstColorLinePtr,
																	  layerIDLine);
						}
					}
					
					srcLine += dstLineWidth;
					dstColorLinePtr += dstLineWidth;
					layerIDLine += dstLineWidth;
				}
			}
			else
			{
				for (size_t i = 0; i < item->nbPixelsX; i++)
				{
					const size_t srcX = item->PixelsX[i];
					
					this->_RenderPixel<GPULayerID_OBJ, false>(srcX,
															  srcX,
															  this->_sprColor[srcX],
															  this->_sprAlpha[srcX],
															  dstColorLinePtr,
															  layerIDLine);
				}
			}
		}
	}
}

template<bool ISCUSTOMRENDERINGNEEDED, size_t CAPTURELENGTH>
void GPUEngineA::_RenderLine_DisplayCapture(u16 *dstColorLine, const u16 l)
{
	assert( (CAPTURELENGTH == GPU_FRAMEBUFFER_NATIVE_WIDTH/2) || (CAPTURELENGTH == GPU_FRAMEBUFFER_NATIVE_WIDTH) );
	
	const IOREG_DISPCNT &DISPCNT = this->_IORegisterMap->DISPCNT;
	const IOREG_DISPCAPCNT &DISPCAPCNT = this->_IORegisterMap->DISPCAPCNT;
	
	const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
	VRAM3DUsageProperties &vramUsageProperty = GPU->GetVRAM3DUsageProperties();
	const u8 vramWriteBlock = DISPCAPCNT.VRAMWriteBlock;
	const u8 vramReadBlock = DISPCNT.VRAM_Block;
	
	//128-wide captures should write linearly into memory, with no gaps
	//this is tested by hotel dusk
	u32 cap_dst_adr = ( (DISPCAPCNT.VRAMWriteOffset * 64 * GPU_FRAMEBUFFER_NATIVE_WIDTH) + (l * CAPTURELENGTH) ) * sizeof(u16);
	
	//Read/Write block wrap to 00000h when exceeding 1FFFFh (128k)
	//this has not been tested yet (I thought I needed it for hotel dusk, but it was fixed by the above)
	cap_dst_adr &= 0x1FFFF;
	cap_dst_adr += vramWriteBlock * GPU_VRAM_BLOCK_LINES * GPU_FRAMEBUFFER_NATIVE_WIDTH * sizeof(u16);
	
	const u16 *cap_src = (u16 *)MMU.blank_memory;
	u16 *cap_dst = (u16 *)(MMU.ARM9_LCD + cap_dst_adr);
	
	if (vramConfiguration.banks[vramReadBlock].purpose == VramConfiguration::LCDC)
	{
		u32 cap_src_adr = ( (this->_dispCapCnt.readOffset * 64 * GPU_FRAMEBUFFER_NATIVE_WIDTH) + (l * GPU_FRAMEBUFFER_NATIVE_WIDTH) ) * sizeof(u16);
		cap_src_adr &= 0x1FFFF;
		cap_src_adr += vramReadBlock * GPU_VRAM_BLOCK_LINES * GPU_FRAMEBUFFER_NATIVE_WIDTH * sizeof(u16);
		cap_src = (u16 *)(MMU.ARM9_LCD + cap_src_adr);
	}
	
	static CACHE_ALIGN u16 fifoLine[GPU_FRAMEBUFFER_NATIVE_WIDTH];
	const u16 *srcA = (DISPCAPCNT.SrcA == 0) ? dstColorLine : this->_3DFramebufferRGBA5551 + (_gpuDstLineIndex[l] * dispInfo.customWidth);
	const u16 *srcB = (DISPCAPCNT.SrcB == 0) ? cap_src : fifoLine;
	
	switch (DISPCAPCNT.CaptureSrc)
	{
		case 0: // Capture source is SourceA
		{
			//INFO("Capture source is SourceA\n");
			switch (DISPCAPCNT.SrcA)
			{
				case 0: // Capture screen (BG + OBJ + 3D)
				{
					//INFO("Capture screen (BG + OBJ + 3D)\n");
					this->_RenderLine_DispCapture_Copy<0, CAPTURELENGTH, !ISCUSTOMRENDERINGNEEDED, true>(srcA, cap_dst, CAPTURELENGTH, 1);
					break;
				}
					
				case 1: // Capture 3D
				{
					//INFO("Capture 3D\n");
					this->_RenderLine_DispCapture_Copy<1, CAPTURELENGTH, !ISCUSTOMRENDERINGNEEDED, true>(srcA, cap_dst, CAPTURELENGTH, 1);
					break;
				}
			}
			
			vramUsageProperty.isBlockUsed[vramWriteBlock] = ISCUSTOMRENDERINGNEEDED;
			break;
		}
			
		case 1: // Capture source is SourceB
		{
			//INFO("Capture source is SourceB\n");
			switch (DISPCAPCNT.SrcB)
			{
				case 0: // Capture VRAM
				{
					if (ISCUSTOMRENDERINGNEEDED && vramUsageProperty.isBlockUsed[vramReadBlock])
					{
						this->_RenderLine_DispCapture_Copy<0, CAPTURELENGTH, false, true>(srcB, cap_dst, CAPTURELENGTH, 1);
					}
					else
					{
						this->_RenderLine_DispCapture_Copy<0, CAPTURELENGTH, true, true>(srcB, cap_dst, CAPTURELENGTH, 1);
					}
					
					vramUsageProperty.isBlockUsed[vramWriteBlock] = vramUsageProperty.isBlockUsed[vramReadBlock];
					break;
				}
					
				case 1: // Capture dispfifo (not yet tested)
				{
					this->_RenderLine_DispCapture_FIFOToBuffer(fifoLine);
					this->_RenderLine_DispCapture_Copy<1, CAPTURELENGTH, true, true>(srcB, cap_dst, CAPTURELENGTH, 1);
					vramUsageProperty.isBlockUsed[vramWriteBlock] = false;
					break;
				}
			}
			
			break;
		}
			
		default: // Capture source is SourceA+B blended
		{
			//INFO("Capture source is SourceA+B blended\n");
			
			if (DISPCAPCNT.SrcB == 1)
			{
				// fifo - tested by splinter cell chaos theory thermal view
				this->_RenderLine_DispCapture_FIFOToBuffer(fifoLine);
				this->_RenderLine_DispCapture_Blend<CAPTURELENGTH, !ISCUSTOMRENDERINGNEEDED, true, true>(srcA, srcB, cap_dst, CAPTURELENGTH, 1);
			}
			else
			{
				if (vramUsageProperty.isBlockUsed[vramReadBlock])
				{
					this->_RenderLine_DispCapture_Blend<CAPTURELENGTH, !ISCUSTOMRENDERINGNEEDED, false, true>(srcA, srcB, cap_dst, CAPTURELENGTH, 1);
				}
				else
				{
					this->_RenderLine_DispCapture_Blend<CAPTURELENGTH, !ISCUSTOMRENDERINGNEEDED, true, true>(srcA, srcB, cap_dst, CAPTURELENGTH, 1);
				}
			}
			
			vramUsageProperty.isBlockUsed[vramWriteBlock] = ISCUSTOMRENDERINGNEEDED || vramUsageProperty.isBlockUsed[vramReadBlock];
			break;
		}
	}
	
	if (ISCUSTOMRENDERINGNEEDED)
	{
		const size_t captureLengthExt = (CAPTURELENGTH) ? dispInfo.customWidth : dispInfo.customWidth / 2;
		const size_t captureLineCount = _gpuDstLineCount[l];
		const size_t vramBlockOffsetExt = _gpuVRAMBlockOffset;
		const u32 ofsmulExt = (CAPTURELENGTH) ? dispInfo.customWidth : dispInfo.customWidth / 2;
		
		size_t cap_dst_adr_ext = (DISPCAPCNT.VRAMWriteOffset * _gpuCaptureLineIndex[64] * dispInfo.customWidth) + (_gpuCaptureLineIndex[l] * ofsmulExt);
		
		while (cap_dst_adr_ext >= vramBlockOffsetExt)
		{
			cap_dst_adr_ext -= vramBlockOffsetExt;
		}
		
		cap_dst_adr_ext += vramWriteBlock * vramBlockOffsetExt;
		
		const u16 *cap_src_ext = GPU->GetCustomVRAMBlankBuffer();
		u16 *cap_dst_ext = GPU->GetCustomVRAMBuffer() + cap_dst_adr_ext;
		
		if (vramConfiguration.banks[vramReadBlock].purpose == VramConfiguration::LCDC)
		{
			size_t cap_src_adr_ext = (this->_dispCapCnt.readOffset * _gpuCaptureLineIndex[64] * dispInfo.customWidth) + (_gpuCaptureLineIndex[l] * dispInfo.customWidth);
			
			while (cap_src_adr_ext >= vramBlockOffsetExt)
			{
				cap_src_adr_ext -= vramBlockOffsetExt;
			}
			
			cap_src_adr_ext += vramReadBlock * vramBlockOffsetExt;
			cap_src_ext = GPU->GetCustomVRAMBuffer() + cap_src_adr_ext;
		}
		
		srcB = (DISPCAPCNT.SrcB == 0) ? cap_src_ext : fifoLine;
		
		switch (DISPCAPCNT.CaptureSrc)
		{
			case 0: // Capture source is SourceA
			{
				switch (DISPCAPCNT.SrcA)
				{
					case 0: // Capture screen (BG + OBJ + 3D)
						this->_RenderLine_DispCapture_Copy<0, CAPTURELENGTH, false, false>(srcA, cap_dst_ext, captureLengthExt, captureLineCount);
						break;
						
					case 1: // Capture 3D
						this->_RenderLine_DispCapture_Copy<1, CAPTURELENGTH, false, false>(srcA, cap_dst_ext, captureLengthExt, captureLineCount);
						break;
				}
				break;
			}
				
			case 1: // Capture source is SourceB
			{
				switch (DISPCAPCNT.SrcB)
				{
					case 0: // Capture VRAM
					{
						if (vramUsageProperty.isBlockUsed[vramReadBlock])
						{
							this->_RenderLine_DispCapture_Copy<0, CAPTURELENGTH, false, false>(srcB, cap_dst_ext, captureLengthExt, captureLineCount);
						}
						else
						{
							this->_RenderLine_DispCapture_Copy<0, CAPTURELENGTH, true, false>(srcB, cap_dst_ext, captureLengthExt, captureLineCount);
						}
						break;
					}
						
					case 1: // Capture dispfifo (not yet tested)
						this->_RenderLine_DispCapture_Copy<1, CAPTURELENGTH, true, false>(srcB, cap_dst_ext, captureLengthExt, captureLineCount);
						break;
				}
				
				break;
			}
				
			default: // Capture source is SourceA+B blended
			{
				if (vramUsageProperty.isBlockUsed[vramReadBlock])
				{
					this->_RenderLine_DispCapture_Blend<CAPTURELENGTH, false, false, false>(srcA, srcB, cap_dst_ext, captureLengthExt, captureLineCount);
				}
				else
				{
					this->_RenderLine_DispCapture_Blend<CAPTURELENGTH, false, true, false>(srcA, srcB, cap_dst_ext, captureLengthExt, captureLineCount);
				}
				
				break;
			}
		}
	}
}

void GPUEngineA::_RenderLine_DispCapture_FIFOToBuffer(u16 *fifoLineBuffer)
{
#ifdef ENABLE_SSE2
	for (size_t i = 0; i < GPU_FRAMEBUFFER_NATIVE_WIDTH * sizeof(u16) / sizeof(__m128i); i++)
	{
		__m128i fifoColor = _mm_set_epi32(DISP_FIFOrecv(), DISP_FIFOrecv(), DISP_FIFOrecv(), DISP_FIFOrecv());
		_mm_store_si128((__m128i *)fifoLineBuffer + i, _mm_shuffle_epi32(fifoColor, 0x1B)); // We need to shuffle the four FIFO values back into the correct order, since they were originally loaded in reverse order.
	}
#else
	for (size_t i = 0; i < GPU_FRAMEBUFFER_NATIVE_WIDTH * sizeof(u16) / sizeof(u32); i++)
	{
		((u32 *)fifoLineBuffer)[i] = LE_TO_LOCAL_32( DISP_FIFOrecv() );
	}
#endif
}

template<int SOURCESWITCH, size_t CAPTURELENGTH, bool CAPTUREFROMNATIVESRC, bool CAPTURETONATIVEDST>
void GPUEngineA::_RenderLine_DispCapture_Copy(const u16 *__restrict src, u16 *__restrict dst, const size_t captureLengthExt, const size_t captureLineCount)
{
	const u16 alphaBit = (SOURCESWITCH == 0) ? 0x8000 : 0x0000;
	
#ifdef ENABLE_SSE2
	const __m128i alpha_vec128 = _mm_set1_epi16(alphaBit);
#endif
	
	if (CAPTURETONATIVEDST)
	{
		if (CAPTUREFROMNATIVESRC)
		{
#ifdef ENABLE_SSE2
			MACRODO_N(CAPTURELENGTH / (sizeof(__m128i) / sizeof(u16)), _mm_store_si128((__m128i *)dst + X, _mm_or_si128( _mm_load_si128( (__m128i *)src + X), alpha_vec128 ) ));
#else
			for (size_t i = 0; i < CAPTURELENGTH; i++)
			{
				dst[i] = LE_TO_LOCAL_16(src[i] | alphaBit);
			}
#endif
		}
		else
		{
			for (size_t i = 0; i < CAPTURELENGTH; i++)
			{
				dst[i] = LE_TO_LOCAL_16(src[_gpuDstPitchIndex[i]] | alphaBit);
			}
		}
	}
	else
	{
		const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
		
		if (CAPTUREFROMNATIVESRC)
		{
			for (size_t i = 0; i < CAPTURELENGTH; i++)
			{
				for (size_t p = 0; p < _gpuDstPitchCount[i]; p++)
				{
					dst[_gpuDstPitchIndex[i] + p] = LE_TO_LOCAL_16(src[i] | alphaBit);
				}
			}
			
			for (size_t line = 1; line < captureLineCount; line++)
			{
				memcpy(dst + (line * dispInfo.customWidth), dst, captureLengthExt * sizeof(u16));
			}
		}
		else
		{
			if (CAPTURELENGTH == GPU_FRAMEBUFFER_NATIVE_WIDTH)
			{
				const size_t pixCountExt = captureLengthExt * captureLineCount;
				size_t i = 0;
				
#ifdef ENABLE_SSE2
				const size_t ssePixCount = pixCountExt - (pixCountExt % 8);
				for (; i < ssePixCount; i += 8)
				{
					_mm_store_si128((__m128i *)(dst + i), _mm_or_si128( _mm_load_si128( (__m128i *)(src + i)), alpha_vec128 ) );
				}
#endif
				for (; i < pixCountExt; i++)
				{
					dst[i] = LE_TO_LOCAL_16(src[i] | alphaBit);
				}
			}
			else
			{
				for (size_t line = 0; line < captureLineCount; line++)
				{
					size_t i = 0;
#ifdef ENABLE_SSE2
					const size_t ssePixCount = captureLengthExt - (captureLengthExt % 8);
					for (; i < ssePixCount; i += 8)
					{
						_mm_store_si128((__m128i *)(dst + i), _mm_or_si128( _mm_load_si128( (__m128i *)(src + i)), alpha_vec128 ) );
					}
#endif
					for (; i < captureLengthExt; i++)
					{
						dst[i] = LE_TO_LOCAL_16(src[i] | alphaBit);
					}
					
					src += dispInfo.customWidth;
					dst += dispInfo.customWidth;
				}
			}
		}
	}
}

u16 GPUEngineA::_RenderLine_DispCapture_BlendFunc(const u16 srcA, const u16 srcB, const u8 blendEVA, const u8 blendEVB)
{
	u16 a = 0;
	u16 r = 0;
	u16 g = 0;
	u16 b = 0;
	u16 a_alpha = srcA & 0x8000;
	u16 b_alpha = srcB & 0x8000;
	
	if (a_alpha)
	{
		a = 0x8000;
		r =  ((srcA        & 0x1F) * blendEVA);
		g = (((srcA >>  5) & 0x1F) * blendEVA);
		b = (((srcA >> 10) & 0x1F) * blendEVA);
	}
	
	if (b_alpha)
	{
		a = 0x8000;
		r +=  ((srcB        & 0x1F) * blendEVB);
		g += (((srcB >>  5) & 0x1F) * blendEVB);
		b += (((srcB >> 10) & 0x1F) * blendEVB);
	}
	
	r >>= 4;
	g >>= 4;
	b >>= 4;
	
	//freedom wings sky will overflow while doing some fsaa/motionblur effect without this
	r = std::min((u16)31,r);
	g = std::min((u16)31,g);
	b = std::min((u16)31,b);
	
	return LOCAL_TO_LE_16(a | (b << 10) | (g << 5) | r);
}

#ifdef ENABLE_SSE2
__m128i GPUEngineA::_RenderLine_DispCapture_BlendFunc_SSE2(__m128i &srcA, __m128i &srcB, const __m128i &blendEVA, const __m128i &blendEVB)
{
	const __m128i colorBitMask = _mm_set1_epi16(0x001F);
	const __m128i srcA_alpha = _mm_and_si128(srcA, _mm_set1_epi16(0x8000));
	const __m128i srcB_alpha = _mm_and_si128(srcB, _mm_set1_epi16(0x8000));
	
	srcA = _mm_andnot_si128( _mm_cmpeq_epi16(srcA_alpha, _mm_setzero_si128()), srcA );
	srcB = _mm_andnot_si128( _mm_cmpeq_epi16(srcB_alpha, _mm_setzero_si128()), srcB );
	
	__m128i srcB_r = _mm_and_si128(srcB, colorBitMask);
	srcB_r = _mm_mullo_epi16(srcB_r, blendEVB);
	
	__m128i srcB_g = _mm_srli_epi16(srcB, 5);
	srcB_g = _mm_and_si128(srcB_g, colorBitMask);
	srcB_g = _mm_mullo_epi16(srcB_g, blendEVB);
	
	__m128i srcB_b = _mm_srli_epi16(srcB, 10);
	srcB_b = _mm_and_si128(srcB_b, colorBitMask);
	srcB_b = _mm_mullo_epi16(srcB_b, blendEVB);
	
	__m128i r = _mm_and_si128(srcA, colorBitMask);
	r = _mm_mullo_epi16(r, blendEVA);
	r = _mm_add_epi16(r, srcB_r);
	r = _mm_srli_epi16(r, 4);
	r = _mm_min_epi16(r, colorBitMask);
	
	__m128i g = _mm_srli_epi16(srcA, 5);
	g = _mm_and_si128(g, colorBitMask);
	g = _mm_mullo_epi16(g, blendEVA);
	g = _mm_add_epi16(g, srcB_g);
	g = _mm_srli_epi16(g, 4);
	g = _mm_min_epi16(g, colorBitMask);
	g = _mm_slli_epi16(g, 5);
	
	__m128i b = _mm_srli_epi16(srcA, 10);
	b = _mm_and_si128(b, colorBitMask);
	b = _mm_mullo_epi16(b, blendEVA);
	b = _mm_add_epi16(b, srcB_b);
	b = _mm_srli_epi16(b, 4);
	b = _mm_min_epi16(b, colorBitMask);
	b = _mm_slli_epi16(b, 10);
	
	const __m128i a = _mm_or_si128(srcA_alpha, srcB_alpha);
	
	return _mm_or_si128(_mm_or_si128(_mm_or_si128(r, g), b), a);
}
#endif

template<bool CAPTUREFROMNATIVESRCA, bool CAPTUREFROMNATIVESRCB>
void GPUEngineA::_RenderLine_DispCapture_BlendToCustomDstBuffer(const u16 *__restrict srcA, const u16 *__restrict srcB, u16 *__restrict dst, const u8 blendEVA, const u8 blendEVB, const size_t length, size_t l)
{
#ifdef ENABLE_SSE2
	const __m128i blendEVA_vec128 = _mm_set1_epi16(blendEVA);
	const __m128i blendEVB_vec128 = _mm_set1_epi16(blendEVB);
#endif
	
	const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
	size_t offset = _gpuDstToSrcIndex[_gpuDstLineIndex[l] * dispInfo.customWidth] - (l * GPU_FRAMEBUFFER_NATIVE_WIDTH);
	size_t i = 0;
	
#ifdef ENABLE_SSE2
	
	const size_t ssePixCount = length - (length % 8);
	for (; i < ssePixCount; i += 8)
	{
		__m128i srcA_vec128 = (!CAPTUREFROMNATIVESRCA) ? _mm_load_si128((__m128i *)(srcA + i)) : _mm_set_epi16(srcA[offset + i + 7],
																											   srcA[offset + i + 6],
																											   srcA[offset + i + 5],
																											   srcA[offset + i + 4],
																											   srcA[offset + i + 3],
																											   srcA[offset + i + 2],
																											   srcA[offset + i + 1],
																											   srcA[offset + i + 0]);
		
		__m128i srcB_vec128 = (!CAPTUREFROMNATIVESRCB) ? _mm_load_si128((__m128i *)(srcB + i)) : _mm_set_epi16(srcB[offset + i + 7],
																											   srcB[offset + i + 6],
																											   srcB[offset + i + 5],
																											   srcB[offset + i + 4],
																											   srcB[offset + i + 3],
																											   srcB[offset + i + 2],
																											   srcB[offset + i + 1],
																											   srcB[offset + i + 0]);
		
		_mm_store_si128( (__m128i *)(dst + i), this->_RenderLine_DispCapture_BlendFunc_SSE2(srcA_vec128, srcB_vec128, blendEVA_vec128, blendEVB_vec128) );
	}
#endif
	for (; i < length; i++)
	{
		const u16 colorA = (!CAPTUREFROMNATIVESRCA) ? srcA[i] : srcA[offset + i];
		const u16 colorB = (!CAPTUREFROMNATIVESRCB) ? srcB[i] : srcB[offset + i];
		
		dst[i] = this->_RenderLine_DispCapture_BlendFunc(colorA, colorB, blendEVA, blendEVB);
	}
}

template<size_t CAPTURELENGTH, bool CAPTUREFROMNATIVESRCA, bool CAPTUREFROMNATIVESRCB, bool CAPTURETONATIVEDST>
void GPUEngineA::_RenderLine_DispCapture_Blend(const u16 *__restrict srcA, const u16 *__restrict srcB, u16 *__restrict dst, const size_t captureLengthExt, const size_t l)
{
	const u8 blendEVA = GPU->GetEngineMain()->_dispCapCnt.EVA;
	const u8 blendEVB = GPU->GetEngineMain()->_dispCapCnt.EVB;
	
	if (CAPTURETONATIVEDST)
	{
#ifdef ENABLE_SSE2
		const __m128i blendEVA_vec128 = _mm_set1_epi16(blendEVA);
		const __m128i blendEVB_vec128 = _mm_set1_epi16(blendEVB);
		
		for (size_t i = 0; i < CAPTURELENGTH; i += 8)
		{
			__m128i srcA_vec128 = (CAPTUREFROMNATIVESRCA) ? _mm_load_si128((__m128i *)(srcA + i)) : _mm_set_epi16(srcA[_gpuDstPitchIndex[i+7]],
			                                                                                                      srcA[_gpuDstPitchIndex[i+6]],
			                                                                                                      srcA[_gpuDstPitchIndex[i+5]],
			                                                                                                      srcA[_gpuDstPitchIndex[i+4]],
			                                                                                                      srcA[_gpuDstPitchIndex[i+3]],
			                                                                                                      srcA[_gpuDstPitchIndex[i+2]],
			                                                                                                      srcA[_gpuDstPitchIndex[i+1]],
			                                                                                                      srcA[_gpuDstPitchIndex[i+0]]);
			
			__m128i srcB_vec128 = (CAPTUREFROMNATIVESRCB) ? _mm_load_si128((__m128i *)(srcB + i)) : _mm_set_epi16(srcB[_gpuDstPitchIndex[i+7]],
			                                                                                                      srcB[_gpuDstPitchIndex[i+6]],
			                                                                                                      srcB[_gpuDstPitchIndex[i+5]],
			                                                                                                      srcB[_gpuDstPitchIndex[i+4]],
			                                                                                                      srcB[_gpuDstPitchIndex[i+3]],
			                                                                                                      srcB[_gpuDstPitchIndex[i+2]],
			                                                                                                      srcB[_gpuDstPitchIndex[i+1]],
			                                                                                                      srcB[_gpuDstPitchIndex[i+0]]);
			
			_mm_store_si128( (__m128i *)(dst + i), this->_RenderLine_DispCapture_BlendFunc_SSE2(srcA_vec128, srcB_vec128, blendEVA_vec128, blendEVB_vec128) );
		}
#else
		for (size_t i = 0; i < CAPTURELENGTH; i++)
		{
			const u16 colorA = (CAPTUREFROMNATIVESRCA) ? srcA[i] : srcA[_gpuDstPitchIndex[i]];
			const u16 colorB = (CAPTUREFROMNATIVESRCB) ? srcB[i] : srcB[_gpuDstPitchIndex[i]];
			
			dst[i] = this->_RenderLine_DispCapture_BlendFunc(colorA, colorB, blendEVA, blendEVB);
		}
#endif
	}
	else
	{
		const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
		const size_t captureLineCount = _gpuDstLineCount[l];
		
		if (CAPTURELENGTH == GPU_FRAMEBUFFER_NATIVE_WIDTH)
		{
			this->_RenderLine_DispCapture_BlendToCustomDstBuffer<CAPTUREFROMNATIVESRCA, CAPTUREFROMNATIVESRCB>(srcA, srcB, dst, blendEVA, blendEVB, captureLengthExt * captureLineCount, l);
		}
		else
		{
			for (size_t line = 0; line < captureLineCount; line++)
			{
				this->_RenderLine_DispCapture_BlendToCustomDstBuffer<CAPTUREFROMNATIVESRCA, CAPTUREFROMNATIVESRCB>(srcA, srcB, dst, blendEVA, blendEVB, captureLengthExt, l);
				srcA += dispInfo.customWidth;
				srcB += dispInfo.customWidth;
				dst += dispInfo.customWidth;
			}
		}
	}
}

template<bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineA::_HandleDisplayModeVRAM(u16 *__restrict dstColorLine, const size_t l, const size_t dstLineWidth, const size_t dstLineCount)
{
	if (!ISCUSTOMRENDERINGNEEDED)
	{
		const u16 *__restrict src = this->_VRAMaddrNative + (l * GPU_FRAMEBUFFER_NATIVE_WIDTH);
#ifdef LOCAL_LE
		memcpy(dstColorLine, src, GPU_FRAMEBUFFER_NATIVE_WIDTH * sizeof(u16));
#else
		for (size_t x = 0; x < GPU_FRAMEBUFFER_NATIVE_WIDTH; x++)
		{
			dstColorLine[x] = LE_TO_LOCAL_16(src[x]);
		}
#endif
	}
	else
	{
		const IOREG_DISPCNT &DISPCNT = this->_IORegisterMap->DISPCNT;
		const VRAM3DUsageProperties &vramUsageProperty = GPU->GetVRAM3DUsageProperties();
		
		if (vramUsageProperty.isBlockUsed[DISPCNT.VRAM_Block] && (vramUsageProperty.blockIndexDisplayVRAM == DISPCNT.VRAM_Block))
		{
			const u16 *__restrict src = this->_VRAMaddrCustom + (_gpuDstLineIndex[l] * dstLineWidth);
#ifdef LOCAL_LE
			memcpy(dstColorLine, src, dstLineWidth * dstLineCount * sizeof(u16));
#else
			for (size_t x = 0; x < dstLineWidth * dstLineCount; x++)
			{
				dstColorLine[x] = LE_TO_LOCAL_16(src[x]);
			}
#endif
		}
		else
		{
			const u16 *__restrict src = this->_VRAMaddrNative + (l * GPU_FRAMEBUFFER_NATIVE_WIDTH);
			
			for (size_t x = 0; x < GPU_FRAMEBUFFER_NATIVE_WIDTH; x++)
			{
				const u16 color = LE_TO_LOCAL_16(src[x]);
				
				for (size_t p = 0; p < _gpuDstPitchCount[x]; p++)
				{
					dstColorLine[_gpuDstPitchIndex[x] + p] = color;
				}
			}
			
			for (size_t line = 1; line < dstLineCount; line++)
			{
				memcpy(dstColorLine + (line * dstLineWidth), dstColorLine, dstLineWidth * sizeof(u16));
			}
		}
	}
}

template<bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineA::_HandleDisplayModeMainMemory(u16 *dstColorLine, const size_t l, const size_t dstLineWidth, const size_t dstLineCount)
{
	//this has not been tested since the dma timing for dispfifo was changed around the time of
	//newemuloop. it may not work.
	
#ifdef ENABLE_SSE2
	const __m128i fifoMask = _mm_set1_epi32(0x7FFF7FFF);
	for (size_t i = 0; i < GPU_FRAMEBUFFER_NATIVE_WIDTH * sizeof(u16) / sizeof(__m128i); i++)
	{
		__m128i fifoColor = _mm_set_epi32(DISP_FIFOrecv(), DISP_FIFOrecv(), DISP_FIFOrecv(), DISP_FIFOrecv());
		fifoColor = _mm_shuffle_epi32(fifoColor, 0x1B); // We need to shuffle the four FIFO values back into the correct order, since they were originally loaded in reverse order.
		_mm_store_si128((__m128i *)dstColorLine + i, _mm_and_si128(fifoColor, fifoMask));
	}
#else
	for (size_t i = 0; i < GPU_FRAMEBUFFER_NATIVE_WIDTH * sizeof(u16) / sizeof(u32); i++)
	{
		((u32 *)dstColorLine)[i] = DISP_FIFOrecv() & 0x7FFF7FFF;
	}
#endif
	if (ISCUSTOMRENDERINGNEEDED)
	{
		for (size_t i = GPU_FRAMEBUFFER_NATIVE_WIDTH - 1; i < GPU_FRAMEBUFFER_NATIVE_WIDTH; i--)
		{
			for (size_t p = _gpuDstPitchCount[i] - 1; p < _gpuDstPitchCount[i]; p--)
			{
				dstColorLine[_gpuDstPitchIndex[i] + p] = dstColorLine[i];
			}
		}
		
		for (size_t line = 1; line < dstLineCount; line++)
		{
			memcpy(dstColorLine + (line * dstLineWidth), dstColorLine, dstLineWidth * sizeof(u16));
		}
	}
}

template<GPULayerID LAYERID, bool ISDEBUGRENDER, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineA::_LineLarge8bpp(u16 *__restrict dstColorLine, const u16 lineIndex)
{
	u16 XBG = this->_IORegisterMap->BGnOFS[LAYERID].BGnHOFS.Offset;
	u16 YBG = lineIndex + this->_IORegisterMap->BGnOFS[LAYERID].BGnVOFS.Offset;
	u16 lg = this->_BGLayer[LAYERID].size.width;
	u16 ht = this->_BGLayer[LAYERID].size.height;
	u16 wmask = (lg-1);
	u16 hmask = (ht-1);
	YBG &= hmask;
	
	//TODO - handle wrapping / out of bounds correctly from rot_scale_op?
	
	u32 tmp_map = this->_BGLayer[LAYERID].largeBMPAddress + lg * YBG;
	u8 *__restrict map = (u8 *)MMU_gpu_map(tmp_map);
	
	for (size_t x = 0; x < lg; ++x, ++XBG)
	{
		XBG &= wmask;
		const u16 color = LE_TO_LOCAL_16( this->_paletteBG[map[XBG]] );
		this->__setFinalColorBck<MOSAIC, ISDEBUGRENDER, false, ISCUSTOMRENDERINGNEEDED>(dstColorLine, lineIndex, color, x, (color != 0));
	}
}

void GPUEngineA::FramebufferPostprocess()
{
	this->GPUEngineBase::FramebufferPostprocess();
	
	this->_IORegisterMap->DISPCAPCNT.CaptureEnable = 0;
	DISP_FIFOreset();
}

GPUEngineB::GPUEngineB()
{
	_engineID = GPUEngineID_Sub;
	_targetDisplayID = NDSDisplayID_Touch;
	_IORegisterMap = (GPU_IOREG *)(&MMU.ARM9_REG[REG_DISPB]);
	_paletteBG = (u16 *)(MMU.ARM9_VMEM + ADDRESS_STEP_1KB);
	_paletteOBJ = (u16 *)(MMU.ARM9_VMEM + ADDRESS_STEP_1KB + ADDRESS_STEP_512B);
	_oamList = (OAMAttributes *)(MMU.ARM9_OAM + ADDRESS_STEP_1KB);
	_sprMem = MMU_BOBJ;
}

GPUEngineB::~GPUEngineB()
{
}

GPUEngineB* GPUEngineB::Allocate()
{
	return new(malloc_aligned64(sizeof(GPUEngineB))) GPUEngineB();
}

void GPUEngineB::FinalizeAndDeallocate()
{
	this->~GPUEngineB();
	free_aligned(this);
}

void GPUEngineB::Reset()
{
	this->_Reset_Base();
	
	this->_BGLayer[GPULayerID_BG0].BMPAddress = MMU_BBG;
	this->_BGLayer[GPULayerID_BG1].BMPAddress = MMU_BBG;
	this->_BGLayer[GPULayerID_BG2].BMPAddress = MMU_BBG;
	this->_BGLayer[GPULayerID_BG3].BMPAddress = MMU_BBG;
	
	this->_BGLayer[GPULayerID_BG0].largeBMPAddress = MMU_BBG;
	this->_BGLayer[GPULayerID_BG1].largeBMPAddress = MMU_BBG;
	this->_BGLayer[GPULayerID_BG2].largeBMPAddress = MMU_BBG;
	this->_BGLayer[GPULayerID_BG3].largeBMPAddress = MMU_BBG;
	
	this->_BGLayer[GPULayerID_BG0].tileMapAddress = MMU_BBG;
	this->_BGLayer[GPULayerID_BG1].tileMapAddress = MMU_BBG;
	this->_BGLayer[GPULayerID_BG2].tileMapAddress = MMU_BBG;
	this->_BGLayer[GPULayerID_BG3].tileMapAddress = MMU_BBG;
	
	this->_BGLayer[GPULayerID_BG0].tileEntryAddress = MMU_BBG;
	this->_BGLayer[GPULayerID_BG1].tileEntryAddress = MMU_BBG;
	this->_BGLayer[GPULayerID_BG2].tileEntryAddress = MMU_BBG;
	this->_BGLayer[GPULayerID_BG3].tileEntryAddress = MMU_BBG;
	
	this->SetDisplayByID(NDSDisplayID_Touch);
}

template<bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineB::RenderLine(const u16 l)
{
	const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
	const size_t dstLineWidth = (ISCUSTOMRENDERINGNEEDED) ? dispInfo.customWidth : GPU_FRAMEBUFFER_NATIVE_WIDTH;
	const size_t dstLineCount = (ISCUSTOMRENDERINGNEEDED) ? _gpuDstLineCount[l] : 1;
	const size_t dstLineIndex = (ISCUSTOMRENDERINGNEEDED) ? _gpuDstLineIndex[l] : l;
	u16 *dstColorLine = (ISCUSTOMRENDERINGNEEDED) ? this->customBuffer + (dstLineIndex * dstLineWidth) : this->nativeBuffer + (dstLineIndex * dstLineWidth);
	
	//cache some parameters which are assumed to be stable throughout the rendering of the entire line
	if (this->_needUpdateWINH[0]) this->_UpdateWINH<0>();
	if (this->_needUpdateWINH[1]) this->_UpdateWINH<1>();
	
	this->_SetupWindows<0>(l);
	this->_SetupWindows<1>(l);
	
	const GPUDisplayMode displayMode = (GPUDisplayMode)(this->_IORegisterMap->DISPCNT.DisplayMode & 0x01);
	switch (displayMode)
	{
		case GPUDisplayMode_Off: // Display Off(Display white)
			this->_HandleDisplayModeOff<ISCUSTOMRENDERINGNEEDED>(dstColorLine, l, dstLineWidth, dstLineCount);
			break;
			
		case GPUDisplayMode_Normal: // Display BG and OBJ layers
			this->_RenderLine_Layer<ISCUSTOMRENDERINGNEEDED>(l, dstColorLine, dstLineWidth, dstLineCount);
			this->_HandleDisplayModeNormal<ISCUSTOMRENDERINGNEEDED>(dstColorLine, l, dstLineWidth, dstLineCount);
			break;
			
		default:
			break;
	}
}

template <bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineB::_RenderLine_Layer(const u16 l, u16 *dstColorLine, const size_t dstLineWidth, const size_t dstLineCount)
{
	const size_t dstLineIndex = _gpuDstLineIndex[l];
	itemsForPriority_t *__restrict item;
	
	const u16 backdropColor = LE_TO_LOCAL_16(this->_paletteBG[0]) & 0x7FFF;
	this->_RenderLine_Clear<ISCUSTOMRENDERINGNEEDED>(backdropColor, l, dstColorLine, dstLineWidth, dstLineCount);
	
	// for all the pixels in the line
	if (this->_enableLayer[GPULayerID_OBJ])
	{
		//n.b. - this is clearing the sprite line buffer to the background color,
		memset_u16_fast<GPU_FRAMEBUFFER_NATIVE_WIDTH>(this->_sprColor, backdropColor);
		
		//zero 06-may-09: I properly supported window color effects for backdrop, but I am not sure
		//how it interacts with this. I wish we knew why we needed this
		
		this->_SpriteRender<false>(l, this->_sprColor, this->_sprAlpha, this->_sprType, this->_sprPrio);
		this->_MosaicSpriteLine(l, this->_sprColor, this->_sprAlpha, this->_sprType, this->_sprPrio);
		
		for (size_t i = 0; i < GPU_FRAMEBUFFER_NATIVE_WIDTH; i++)
		{
			// assign them to the good priority item
			const size_t prio = this->_sprPrio[i];
			if (prio >= 4) continue;
			
			item = &(this->_itemsForPriority[prio]);
			item->PixelsX[item->nbPixelsX] = i;
			item->nbPixelsX++;
		}
	}
	
	// paint lower priorities first
	// then higher priorities on top
	for (size_t prio = NB_PRIORITIES; prio > 0; )
	{
		prio--;
		item = &(this->_itemsForPriority[prio]);
		// render BGs
		if (this->_isAnyBGLayerEnabled)
		{
			for (size_t i = 0; i < item->nbBGs; i++)
			{
				const GPULayerID layerID = (GPULayerID)item->BGs[i];
				if (this->_enableLayer[layerID])
				{
#ifndef DISABLE_MOSAIC
					if (this->_BGLayer[layerID].isMosaic && this->_isBGMosaicSet)
					{
						switch (layerID)
						{
							case GPULayerID_BG0: this->_RenderLine_LayerBG<GPULayerID_BG0, false, true, ISCUSTOMRENDERINGNEEDED>(dstColorLine, l); break;
							case GPULayerID_BG1: this->_RenderLine_LayerBG<GPULayerID_BG1, false, true, ISCUSTOMRENDERINGNEEDED>(dstColorLine, l); break;
							case GPULayerID_BG2: this->_RenderLine_LayerBG<GPULayerID_BG2, false, true, ISCUSTOMRENDERINGNEEDED>(dstColorLine, l); break;
							case GPULayerID_BG3: this->_RenderLine_LayerBG<GPULayerID_BG3, false, true, ISCUSTOMRENDERINGNEEDED>(dstColorLine, l); break;
								
							default:
								break;
						}
					}
					else
#endif
					{
						switch (layerID)
						{
							case GPULayerID_BG0: this->_RenderLine_LayerBG<GPULayerID_BG0, false, false, ISCUSTOMRENDERINGNEEDED>(dstColorLine, l); break;
							case GPULayerID_BG1: this->_RenderLine_LayerBG<GPULayerID_BG1, false, false, ISCUSTOMRENDERINGNEEDED>(dstColorLine, l); break;
							case GPULayerID_BG2: this->_RenderLine_LayerBG<GPULayerID_BG2, false, false, ISCUSTOMRENDERINGNEEDED>(dstColorLine, l); break;
							case GPULayerID_BG3: this->_RenderLine_LayerBG<GPULayerID_BG3, false, false, ISCUSTOMRENDERINGNEEDED>(dstColorLine, l); break;
								
							default:
								break;
						}
					}
				} //layer enabled
			}
		}
		
		// render sprite Pixels
		if (this->_enableLayer[GPULayerID_OBJ])
		{
			u16 *__restrict dstColorLinePtr = dstColorLine;
			u8 *__restrict layerIDLine = this->_dstLayerID;
			
			if (ISCUSTOMRENDERINGNEEDED)
			{
				const bool useCustomVRAM = (this->vramBlockOBJIndex != VRAM_NO_3D_USAGE);
				const u16 *__restrict srcLine = (useCustomVRAM) ? GPU->GetCustomVRAMBuffer() + (this->vramBlockOBJIndex * _gpuVRAMBlockOffset) + (dstLineIndex * dstLineWidth) : NULL;
				
				for (size_t line = 0; line < dstLineCount; line++)
				{
					for (size_t i = 0; i < item->nbPixelsX; i++)
					{
						const size_t srcX = item->PixelsX[i];
						
						for (size_t p = 0; p < _gpuDstPitchCount[srcX]; p++)
						{
							const size_t dstX = _gpuDstPitchIndex[srcX] + p;
							
							this->_RenderPixel<GPULayerID_OBJ, false>(srcX,
																	  dstX,
																	  (useCustomVRAM) ? srcLine[dstX] : this->_sprColor[srcX],
																	  this->_sprAlpha[srcX],
																	  dstColorLinePtr,
																	  layerIDLine);
						}
					}
					
					srcLine += dstLineWidth;
					dstColorLinePtr += dstLineWidth;
					layerIDLine += dstLineWidth;
				}
			}
			else
			{
				for (size_t i = 0; i < item->nbPixelsX; i++)
				{
					const size_t srcX = item->PixelsX[i];
					
					this->_RenderPixel<GPULayerID_OBJ, false>(srcX,
															  srcX,
															  this->_sprColor[srcX],
															  this->_sprAlpha[srcX],
															  dstColorLinePtr,
															  layerIDLine);
				}
			}
		}
	}
}

GPUSubsystem::GPUSubsystem()
{
	_defaultEventHandler = new GPUEventHandlerDefault;
	_event = _defaultEventHandler;
	
	gfx3d_init();
	
	_engineMain = GPUEngineA::Allocate();
	_engineSub = GPUEngineB::Allocate();
	
	_displayMain = new NDSDisplay(NDSDisplayID_Main);
	_displayMain->SetEngine(_engineMain);
	_displayTouch = new NDSDisplay(NDSDisplayID_Touch);
	_displayTouch->SetEngine(_engineSub);
	
	_willAutoResolveToCustomBuffer = true;
	
	OSDCLASS *previousOSD = osd;
	osd = new OSDCLASS(-1);
	delete previousOSD;
	
	_customVRAM = NULL;
	_customVRAMBlank = NULL;
	_customFramebuffer = (u16 *)malloc_alignedCacheLine(GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT * sizeof(u16) * 2);
	
	_displayInfo.isCustomSizeRequested = false;
	_displayInfo.customWidth = GPU_FRAMEBUFFER_NATIVE_WIDTH;
	_displayInfo.customHeight = GPU_FRAMEBUFFER_NATIVE_HEIGHT;
	_displayInfo.masterCustomBuffer = _customFramebuffer;
	_displayInfo.masterNativeBuffer = _nativeFramebuffer;
	_displayInfo.nativeBuffer[NDSDisplayID_Main] = _nativeFramebuffer;
	_displayInfo.nativeBuffer[NDSDisplayID_Touch] = _nativeFramebuffer + (GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT);
	_displayInfo.customBuffer[NDSDisplayID_Main] = _customFramebuffer;
	_displayInfo.customBuffer[NDSDisplayID_Touch] = _customFramebuffer + (GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT);
	
	_displayInfo.didPerformCustomRender[NDSDisplayID_Main] = false;
	_displayInfo.didPerformCustomRender[NDSDisplayID_Touch] = false;
	_displayInfo.renderedWidth[NDSDisplayID_Main] = GPU_FRAMEBUFFER_NATIVE_WIDTH;
	_displayInfo.renderedWidth[NDSDisplayID_Touch] = GPU_FRAMEBUFFER_NATIVE_WIDTH;
	_displayInfo.renderedHeight[NDSDisplayID_Main] = GPU_FRAMEBUFFER_NATIVE_HEIGHT;
	_displayInfo.renderedHeight[NDSDisplayID_Touch] = GPU_FRAMEBUFFER_NATIVE_HEIGHT;
	_displayInfo.renderedBuffer[NDSDisplayID_Main] = _displayInfo.nativeBuffer[NDSDisplayID_Main];
	_displayInfo.renderedBuffer[NDSDisplayID_Touch] = _displayInfo.nativeBuffer[NDSDisplayID_Touch];
	
	ClearWithColor(0x8000);
}

GPUSubsystem::~GPUSubsystem()
{
	delete osd;
	osd = NULL;
	
	free_aligned(this->_customFramebuffer);
	free_aligned(this->_customVRAM);
	
	free_aligned(_gpuDstToSrcIndex);
	_gpuDstToSrcIndex = NULL;
	
	delete _displayMain;
	delete _displayTouch;
	_engineMain->FinalizeAndDeallocate();
	_engineSub->FinalizeAndDeallocate();
	
	gfx3d_deinit();
	
	delete _defaultEventHandler;
}

GPUSubsystem* GPUSubsystem::Allocate()
{
	return new(malloc_aligned64(sizeof(GPUSubsystem))) GPUSubsystem();
}

void GPUSubsystem::FinalizeAndDeallocate()
{
	this->~GPUSubsystem();
	free_aligned(this);
}

void GPUSubsystem::SetEventHandler(GPUEventHandler *eventHandler)
{
	this->_event = eventHandler;
}

GPUEventHandler* GPUSubsystem::GetEventHandler()
{
	return this->_event;
}

void GPUSubsystem::Reset()
{
	if (this->_customVRAM == NULL || this->_customVRAM == NULL || this->_customFramebuffer == NULL)
	{
		this->SetCustomFramebufferSize(this->_displayInfo.customWidth, this->_displayInfo.customHeight);
	}
	
	this->ClearWithColor(0xFFFF);
	
	gfx3d_reset();
	this->_engineMain->Reset();
	this->_engineSub->Reset();
	
	this->_VRAM3DUsage.blockIndexDisplayVRAM = VRAM_NO_3D_USAGE;
	this->_VRAM3DUsage.isBlockUsed[0] = false;
	this->_VRAM3DUsage.isBlockUsed[1] = false;
	this->_VRAM3DUsage.isBlockUsed[2] = false;
	this->_VRAM3DUsage.isBlockUsed[3] = false;
	
	DISP_FIFOreset();
	osd->clear();
}

void GPUSubsystem::UpdateVRAM3DUsageProperties()
{
	const IOREG_DISPCNT &mainDISPCNT = this->_engineMain->GetIORegisterMap().DISPCNT;
	
	this->_VRAM3DUsage.blockIndexDisplayVRAM = VRAM_NO_3D_USAGE;
	
	this->_engineMain->is3DEnabled = (mainDISPCNT.BG0_Enable == 1) && (mainDISPCNT.BG0_3D == 1);
	this->_engineMain->isCustomRenderingNeeded = false;
	this->_engineMain->vramBlockBGIndex = VRAM_NO_3D_USAGE;
	this->_engineMain->vramBlockOBJIndex = VRAM_NO_3D_USAGE;
	this->_engineMain->vramBGLayer = VRAM_NO_3D_USAGE;
	this->_engineMain->renderedWidth = GPU_FRAMEBUFFER_NATIVE_WIDTH;
	this->_engineMain->renderedHeight = GPU_FRAMEBUFFER_NATIVE_HEIGHT;
	this->_engineMain->renderedBuffer = this->_engineMain->nativeBuffer;
	
	this->_engineSub->is3DEnabled = false;
	this->_engineSub->isCustomRenderingNeeded = false;
	this->_engineSub->vramBlockBGIndex = VRAM_NO_3D_USAGE;
	this->_engineSub->vramBlockOBJIndex = VRAM_NO_3D_USAGE;
	this->_engineSub->vramBGLayer = VRAM_NO_3D_USAGE;
	this->_engineSub->renderedWidth = GPU_FRAMEBUFFER_NATIVE_WIDTH;
	this->_engineSub->renderedHeight = GPU_FRAMEBUFFER_NATIVE_HEIGHT;
	this->_engineSub->renderedBuffer = this->_engineSub->nativeBuffer;
	
	this->_displayInfo.didPerformCustomRender[NDSDisplayID_Main] = false;
	this->_displayInfo.didPerformCustomRender[NDSDisplayID_Touch] = false;
	
	this->_displayInfo.nativeBuffer[NDSDisplayID_Main] = this->_displayMain->GetEngine()->nativeBuffer;
	this->_displayInfo.renderedBuffer[NDSDisplayID_Main] = this->_displayMain->GetEngine()->renderedBuffer;
	this->_displayInfo.renderedWidth[NDSDisplayID_Main] = this->_displayMain->GetEngine()->renderedWidth;
	this->_displayInfo.renderedHeight[NDSDisplayID_Main] = this->_displayMain->GetEngine()->renderedHeight;
	
	this->_displayInfo.nativeBuffer[NDSDisplayID_Touch] = this->_displayTouch->GetEngine()->nativeBuffer;
	this->_displayInfo.renderedBuffer[NDSDisplayID_Touch] = this->_displayTouch->GetEngine()->renderedBuffer;
	this->_displayInfo.renderedWidth[NDSDisplayID_Touch] = this->_displayTouch->GetEngine()->renderedWidth;
	this->_displayInfo.renderedHeight[NDSDisplayID_Touch] = this->_displayTouch->GetEngine()->renderedHeight;
	
	if (!this->_displayInfo.isCustomSizeRequested)
	{
		return;
	}
	
	this->_engineMain->isCustomRenderingNeeded = this->_engineMain->is3DEnabled;
	
	// Iterate through VRAM banks A-D and determine if they will be used for this frame.
	for (size_t i = 0; i < 4; i++)
	{
		if (!this->_VRAM3DUsage.isBlockUsed[i])
		{
			continue;
		}
		
		switch (vramConfiguration.banks[i].purpose)
		{
			case VramConfiguration::ABG:
				this->_engineMain->UpdateVRAM3DUsageProperties_BGLayer(i, this->_VRAM3DUsage);
				break;
				
			case VramConfiguration::BBG:
				this->_engineSub->UpdateVRAM3DUsageProperties_BGLayer(i, this->_VRAM3DUsage);
				break;
				
			case VramConfiguration::AOBJ:
				this->_engineMain->UpdateVRAM3DUsageProperties_OBJLayer(i, this->_VRAM3DUsage);
				break;
				
			case VramConfiguration::BOBJ:
				this->_engineSub->UpdateVRAM3DUsageProperties_OBJLayer(i, this->_VRAM3DUsage);
				break;
				
			case VramConfiguration::LCDC:
			{
				if ((mainDISPCNT.DisplayMode == GPUDisplayMode_VRAM) && (mainDISPCNT.VRAM_Block == i))
				{
					this->_VRAM3DUsage.blockIndexDisplayVRAM = i;
				}
				break;
			}
				
			default:
				this->_VRAM3DUsage.isBlockUsed[i] = false;
				break;
		}
	}
	
	if (this->_engineMain->isCustomRenderingNeeded)
	{
		this->_engineMain->renderedWidth = this->_displayInfo.customWidth;
		this->_engineMain->renderedHeight = this->_displayInfo.customHeight;
		this->_engineMain->renderedBuffer = this->_engineMain->customBuffer;
	}
	
	if (this->_engineSub->isCustomRenderingNeeded)
	{
		this->_engineSub->renderedWidth = this->_displayInfo.customWidth;
		this->_engineSub->renderedHeight = this->_displayInfo.customHeight;
		this->_engineSub->renderedBuffer = this->_engineSub->customBuffer;
	}
	
	this->_displayInfo.didPerformCustomRender[NDSDisplayID_Main] = this->_displayMain->GetEngine()->isCustomRenderingNeeded;
	this->_displayInfo.renderedBuffer[NDSDisplayID_Main] = this->_displayMain->GetEngine()->renderedBuffer;
	this->_displayInfo.renderedWidth[NDSDisplayID_Main] = this->_displayMain->GetEngine()->renderedWidth;
	this->_displayInfo.renderedHeight[NDSDisplayID_Main] = this->_displayMain->GetEngine()->renderedHeight;
	
	this->_displayInfo.didPerformCustomRender[NDSDisplayID_Touch] = this->_displayTouch->GetEngine()->isCustomRenderingNeeded;
	this->_displayInfo.renderedBuffer[NDSDisplayID_Touch] = this->_displayTouch->GetEngine()->renderedBuffer;
	this->_displayInfo.renderedWidth[NDSDisplayID_Touch] = this->_displayTouch->GetEngine()->renderedWidth;
	this->_displayInfo.renderedHeight[NDSDisplayID_Touch] = this->_displayTouch->GetEngine()->renderedHeight;
}

const NDSDisplayInfo& GPUSubsystem::GetDisplayInfo()
{
	return this->_displayInfo;
}

void GPUSubsystem::SetDisplayDidCustomRender(NDSDisplayID displayID, bool theState)
{
	this->_displayInfo.didPerformCustomRender[displayID] = theState;
}

GPUEngineA* GPUSubsystem::GetEngineMain()
{
	return this->_engineMain;
}

GPUEngineB* GPUSubsystem::GetEngineSub()
{
	return this->_engineSub;
}

NDSDisplay* GPUSubsystem::GetDisplayMain()
{
	return this->_displayMain;
}

NDSDisplay* GPUSubsystem::GetDisplayTouch()
{
	return this->_displayTouch;
}

size_t GPUSubsystem::GetCustomFramebufferWidth() const
{
	return this->_displayInfo.customWidth;
}

size_t GPUSubsystem::GetCustomFramebufferHeight() const
{
	return this->_displayInfo.customHeight;
}

void GPUSubsystem::SetCustomFramebufferSize(size_t w, size_t h)
{
	if (w < GPU_FRAMEBUFFER_NATIVE_WIDTH || h < GPU_FRAMEBUFFER_NATIVE_HEIGHT)
	{
		return;
	}
	
	CurrentRenderer->RenderFinish();
	
	const float customWidthScale = (float)w / (float)GPU_FRAMEBUFFER_NATIVE_WIDTH;
	const float customHeightScale = (float)h / (float)GPU_FRAMEBUFFER_NATIVE_HEIGHT;
	const float newGpuLargestDstLineCount = (size_t)ceilf(customHeightScale);
	
	u16 *oldCustomFramebuffer = this->_customFramebuffer;
	u16 *oldGpuDstToSrcIndexPtr = _gpuDstToSrcIndex;
	u16 *oldCustomVRAM = this->_customVRAM;
	
	for (size_t srcX = 0, currentPitchCount = 0; srcX < GPU_FRAMEBUFFER_NATIVE_WIDTH; srcX++)
	{
		const size_t pitch = (size_t)ceilf((srcX+1) * customWidthScale) - currentPitchCount;
		_gpuDstPitchCount[srcX] = pitch;
		_gpuDstPitchIndex[srcX] = currentPitchCount;
		currentPitchCount += pitch;
	}
	
	for (size_t srcY = 0, currentLineCount = 0; srcY < GPU_FRAMEBUFFER_NATIVE_HEIGHT; srcY++)
	{
		const size_t lineCount = (size_t)ceilf((srcY+1) * customHeightScale) - currentLineCount;
		_gpuDstLineCount[srcY] = lineCount;
		_gpuDstLineIndex[srcY] = currentLineCount;
		currentLineCount += lineCount;
	}
	
	for (size_t srcY = 0, currentLineCount = 0; srcY < GPU_VRAM_BLOCK_LINES + 1; srcY++)
	{
		const size_t lineCount = (size_t)ceilf((srcY+1) * customHeightScale) - currentLineCount;
		_gpuCaptureLineIndex[srcY] = currentLineCount;
		currentLineCount += lineCount;
	}
	
	u16 *newGpuDstToSrcIndex = (u16 *)malloc_alignedCacheLine(w * h * sizeof(u16));
	for (size_t y = 0; y < GPU_FRAMEBUFFER_NATIVE_HEIGHT; y++)
	{
		for (size_t x = 0; x < GPU_FRAMEBUFFER_NATIVE_WIDTH; x++)
		{
			for (size_t l = 0; l < _gpuDstLineCount[y]; l++)
			{
				for (size_t p = 0; p < _gpuDstPitchCount[x]; p++)
				{
					newGpuDstToSrcIndex[((_gpuDstLineIndex[y] + l) * w) + (_gpuDstPitchIndex[x] + p)] = (y * GPU_FRAMEBUFFER_NATIVE_WIDTH) + x;
				}
			}
		}
	}
	
	u16 *newCustomFramebuffer = (u16 *)malloc_alignedCacheLine(w * h * sizeof(u16) * 2);
	memset_u16(newCustomFramebuffer, 0x8000, w * h * 2);
	
	const size_t newCustomVRAMBlockSize = _gpuCaptureLineIndex[GPU_VRAM_BLOCK_LINES] * w;
	const size_t newCustomVRAMBlankSize = newGpuLargestDstLineCount * w;
	u16 *newCustomVRAM = (u16 *)malloc_alignedCacheLine(((newCustomVRAMBlockSize * 4) + newCustomVRAMBlankSize) * sizeof(u16));
	memset(newCustomVRAM, 0, ((newCustomVRAMBlockSize * 4) + newCustomVRAMBlankSize) * sizeof(u16));
	
	_gpuLargestDstLineCount = newGpuLargestDstLineCount;
	_gpuVRAMBlockOffset = _gpuCaptureLineIndex[GPU_VRAM_BLOCK_LINES] * w;
	_gpuDstToSrcIndex = newGpuDstToSrcIndex;
	this->_customVRAM = newCustomVRAM;
	this->_customVRAMBlank = newCustomVRAM + (newCustomVRAMBlockSize * 4);
	
	this->_customFramebuffer = newCustomFramebuffer;
	
	this->_displayInfo.isCustomSizeRequested = ( (w != GPU_FRAMEBUFFER_NATIVE_WIDTH) || (h != GPU_FRAMEBUFFER_NATIVE_HEIGHT) );
	this->_displayInfo.masterCustomBuffer = this->_customFramebuffer;
	this->_displayInfo.customWidth = w;
	this->_displayInfo.customHeight = h;
	this->_displayInfo.customBuffer[NDSDisplayID_Main] = (this->_displayMain->GetEngine()->GetDisplayByID() == NDSDisplayID_Main) ? this->_customFramebuffer : this->_customFramebuffer + (w * h);
	this->_displayInfo.customBuffer[NDSDisplayID_Touch] = (this->_displayTouch->GetEngine()->GetDisplayByID() == NDSDisplayID_Main) ? this->_customFramebuffer : this->_customFramebuffer + (w * h);
	
	this->_engineMain->SetCustomFramebufferSize(w, h);
	this->_engineSub->SetCustomFramebufferSize(w, h);
	CurrentRenderer->SetFramebufferSize(w, h);
	
	if (this->_displayInfo.didPerformCustomRender[NDSDisplayID_Main])
	{
		this->_displayInfo.renderedBuffer[NDSDisplayID_Main] = this->_displayInfo.customBuffer[NDSDisplayID_Main];
		this->_displayInfo.renderedWidth[NDSDisplayID_Main] = this->_displayInfo.customWidth;
		this->_displayInfo.renderedHeight[NDSDisplayID_Main] = this->_displayInfo.customHeight;
	}
	
	if (this->_displayInfo.didPerformCustomRender[NDSDisplayID_Touch])
	{
		this->_displayInfo.renderedBuffer[NDSDisplayID_Touch] = this->_displayInfo.customBuffer[NDSDisplayID_Touch];
		this->_displayInfo.renderedWidth[NDSDisplayID_Touch] = this->_displayInfo.customWidth;
		this->_displayInfo.renderedHeight[NDSDisplayID_Touch] = this->_displayInfo.customHeight;
	}
	
	free_aligned(oldCustomFramebuffer);
	free_aligned(oldGpuDstToSrcIndexPtr);
	free_aligned(oldCustomVRAM);
}

u16* GPUSubsystem::GetCustomVRAMBuffer()
{
	return this->_customVRAM;
}

u16* GPUSubsystem::GetCustomVRAMBlankBuffer()
{
	return this->_customVRAMBlank;
}

VRAM3DUsageProperties& GPUSubsystem::GetVRAM3DUsageProperties()
{
	return this->_VRAM3DUsage;
}

bool GPUSubsystem::GetWillAutoResolveToCustomBuffer() const
{
	return this->_willAutoResolveToCustomBuffer;
}

void GPUSubsystem::SetWillAutoResolveToCustomBuffer(const bool willAutoResolve)
{
	this->_willAutoResolveToCustomBuffer = willAutoResolve;
}

void GPUSubsystem::RenderLine(const u16 l, bool isFrameSkipRequested)
{
	if (l == 0)
	{
		CurrentRenderer->RenderFinish();
		this->_event->DidFrameBegin();
		this->UpdateVRAM3DUsageProperties();
		
		// Clear displays to black if they are turned off by the user.
		if (!isFrameSkipRequested)
		{
			if (!CommonSettings.showGpu.main)
			{
				memset(this->_engineMain->renderedBuffer, 0, this->_engineMain->renderedWidth * this->_engineMain->renderedHeight * sizeof(u16));
			}
			else if (this->_engineMain->GetIsMasterBrightFullIntensity() && (this->_engineMain->GetIORegisterMap().DISPCAPCNT.CaptureEnable == 0))
			{
				this->_engineMain->ApplyMasterBrightness<true>();
			}
			
			if (!CommonSettings.showGpu.sub)
			{
				memset(this->_engineSub->renderedBuffer, 0, this->_engineSub->renderedWidth * this->_engineSub->renderedHeight * sizeof(u16));
			}
			else if (this->_engineSub->GetIsMasterBrightFullIntensity())
			{
				this->_engineSub->ApplyMasterBrightness<true>();
			}
		}
	}
	
	if (!isFrameSkipRequested)
	{
		if (CommonSettings.showGpu.main && !(this->_engineMain->GetIsMasterBrightFullIntensity() && (this->_engineMain->GetIORegisterMap().DISPCAPCNT.CaptureEnable == 0)))
		{
			if (this->_engineMain->isCustomRenderingNeeded)
			{
				this->_engineMain->RenderLine<true>(l);
			}
			else
			{
				this->_engineMain->RenderLine<false>(l);
			}
		}
		
		if (CommonSettings.showGpu.sub && !this->_engineSub->GetIsMasterBrightFullIntensity())
		{
			if (this->_engineSub->isCustomRenderingNeeded)
			{
				this->_engineSub->RenderLine<true>(l);
			}
			else
			{
				this->_engineSub->RenderLine<false>(l);
			}
		}
	}
	
	if (l == 191)
	{
		if (!isFrameSkipRequested)
		{
			if (CommonSettings.showGpu.main && !(this->_engineMain->GetIsMasterBrightFullIntensity() && (this->_engineMain->GetIORegisterMap().DISPCAPCNT.CaptureEnable == 0)))
			{
				this->_engineMain->ApplyMasterBrightness<false>();
			}
			
			if (CommonSettings.showGpu.sub && !this->_engineSub->GetIsMasterBrightFullIntensity())
			{
				this->_engineSub->ApplyMasterBrightness<false>();
			}
			
			if (this->_willAutoResolveToCustomBuffer)
			{
				this->_engineMain->ResolveToCustomFramebuffer();
				this->_engineSub->ResolveToCustomFramebuffer();
			}
		}
		
		this->_engineMain->FramebufferPostprocess();
		this->_engineSub->FramebufferPostprocess();
		
		gfx3d._videoFrameCount++;
		if (gfx3d._videoFrameCount == 60)
		{
			Render3DFramesPerSecond = gfx3d.render3DFrameCount;
			gfx3d.render3DFrameCount = 0;
			gfx3d._videoFrameCount = 0;
		}
		
		this->_event->DidFrameEnd(isFrameSkipRequested);
	}
}

void GPUSubsystem::ClearWithColor(const u16 colorBGRA5551)
{
	memset_u16(this->_nativeFramebuffer, colorBGRA5551, GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT * 2);
	memset_u16(this->_customFramebuffer, colorBGRA5551, this->_displayInfo.customWidth * this->_displayInfo.customHeight * 2);
}

NDSDisplay::NDSDisplay()
{
	_ID = NDSDisplayID_Main;
	_gpu = NULL;
}

NDSDisplay::NDSDisplay(const NDSDisplayID displayID)
{
	_ID = displayID;
	_gpu = NULL;
}

NDSDisplay::NDSDisplay(const NDSDisplayID displayID, GPUEngineBase *theEngine)
{
	_ID = displayID;
	_gpu = theEngine;
}

GPUEngineBase* NDSDisplay::GetEngine()
{
	return this->_gpu;
}

void NDSDisplay::SetEngine(GPUEngineBase *theEngine)
{
	this->_gpu = theEngine;
}

GPUEngineID NDSDisplay::GetEngineID()
{
	return this->_gpu->GetEngineID();
}

void NDSDisplay::SetEngineByID(const GPUEngineID theID)
{
	this->_gpu = (theID == GPUEngineID_Main) ? (GPUEngineBase *)GPU->GetEngineMain() : (GPUEngineBase *)GPU->GetEngineSub();
	this->_gpu->SetDisplayByID(this->_ID);
}

template void GPUEngineBase::ParseReg_DISPCNT<GPUEngineID_Main>();
template void GPUEngineBase::ParseReg_DISPCNT<GPUEngineID_Sub>();

template void GPUEngineBase::ParseReg_BGnCNT<GPUEngineID_Main, GPULayerID_BG0>();
template void GPUEngineBase::ParseReg_BGnCNT<GPUEngineID_Main, GPULayerID_BG1>();
template void GPUEngineBase::ParseReg_BGnCNT<GPUEngineID_Main, GPULayerID_BG2>();
template void GPUEngineBase::ParseReg_BGnCNT<GPUEngineID_Main, GPULayerID_BG3>();

template void GPUEngineBase::ParseReg_BGnCNT<GPUEngineID_Sub, GPULayerID_BG0>();
template void GPUEngineBase::ParseReg_BGnCNT<GPUEngineID_Sub, GPULayerID_BG1>();
template void GPUEngineBase::ParseReg_BGnCNT<GPUEngineID_Sub, GPULayerID_BG2>();
template void GPUEngineBase::ParseReg_BGnCNT<GPUEngineID_Sub, GPULayerID_BG3>();

template void GPUEngineBase::ParseReg_BGnHOFS<GPULayerID_BG0>();
template void GPUEngineBase::ParseReg_BGnHOFS<GPULayerID_BG1>();
template void GPUEngineBase::ParseReg_BGnHOFS<GPULayerID_BG2>();
template void GPUEngineBase::ParseReg_BGnHOFS<GPULayerID_BG3>();

template void GPUEngineBase::ParseReg_BGnVOFS<GPULayerID_BG0>();
template void GPUEngineBase::ParseReg_BGnVOFS<GPULayerID_BG1>();
template void GPUEngineBase::ParseReg_BGnVOFS<GPULayerID_BG2>();
template void GPUEngineBase::ParseReg_BGnVOFS<GPULayerID_BG3>();

template void GPUEngineBase::ParseReg_WINnH<0>();
template void GPUEngineBase::ParseReg_WINnH<1>();

template void GPUEngineBase::ParseReg_BGnX<GPULayerID_BG2>();
template void GPUEngineBase::ParseReg_BGnY<GPULayerID_BG2>();
template void GPUEngineBase::ParseReg_BGnX<GPULayerID_BG3>();
template void GPUEngineBase::ParseReg_BGnY<GPULayerID_BG3>();

template void GPUEngineBase::RenderLayerBG<GPULayerID_BG0>(u16 *dstColorBuffer);
template void GPUEngineBase::RenderLayerBG<GPULayerID_BG1>(u16 *dstColorBuffer);
template void GPUEngineBase::RenderLayerBG<GPULayerID_BG2>(u16 *dstColorBuffer);
template void GPUEngineBase::RenderLayerBG<GPULayerID_BG3>(u16 *dstColorBuffer);
