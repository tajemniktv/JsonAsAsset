#pragma once

#ifndef __linux__

#include "nvimage/Image.h"
#include "nvimage/DirectDrawSurface.h"

#undef __FUNC__						// conflicted with our guard macros

NVTT_API bool DecodeDDS(const unsigned char* Data, int SourceDataSize, int SizeX, int SizeY, int SizeZ, nv::DDSHeader& Header, nv::Image& Image);

#endif // __linux__
