/*
Copyright (C) 2013 StrmnNrmn

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "stdafx.h"
#include "Graphics/NativeTexture.h"
#include "Graphics/ColourValue.h"
#include "SysPSP/Graphics/PixelFormatPSP.h" // FIXME

#include "Math/MathUtil.h"

#include <png.h>

using namespace PixelFormats::Psp;

static u32		PALETTE4_BYTES_REQUIRED( 16 * sizeof( u32 ) );
static u32		PALETTE8_BYTES_REQUIRED( 256 * sizeof( u32 ) );


static u32 GetTextureBlockWidth( u32 dimension, ETextureFormat texture_format )
{
	DAEDALUS_ASSERT( GetNextPowerOf2( dimension ) == dimension, "This is not a power of 2" );

	// Ensure that the pitch is at least 16 bytes
	while( CalcBytesRequired( dimension, texture_format ) < 16 )
	{
		dimension *= 2;
	}

	return dimension;
}

static inline u32 CorrectDimension( u32 dimension )
{
	static const u32 MIN_TEXTURE_DIMENSION = 1;
	return Max( GetNextPowerOf2( dimension ), MIN_TEXTURE_DIMENSION );
}

static void * VAlloc(size_t len)
{
	return malloc(len);
}

static void VFree(void * p)
{
	free(p);
}

CRefPtr<CNativeTexture>	CNativeTexture::Create( u32 width, u32 height, ETextureFormat texture_format )
{
	return new CNativeTexture( width, height, texture_format );
}

CNativeTexture::CNativeTexture( u32 w, u32 h, ETextureFormat texture_format )
:	mTextureFormat( texture_format )
,	mWidth( w )
,	mHeight( h )
,	mCorrectedWidth( CorrectDimension( w ) )
,	mCorrectedHeight( CorrectDimension( h ) )
,	mTextureBlockWidth( GetTextureBlockWidth( mCorrectedWidth, texture_format ) )
,	mpPalette( NULL )
,	mIsDataVidMem( false )
,	mIsPaletteVidMem( false )
,	mIsSwizzled( true )
#ifdef DAEDALUS_ENABLE_ASSERTS
,	mPaletteSet( false )
#endif
{
	mScale.x = 1.0f / mCorrectedWidth;
	mScale.y = 1.0f / mCorrectedHeight;

	u32		bytes_required( GetBytesRequired() );


	mpData = VAlloc( bytes_required );
	if( !mpData)
	{
		DAEDALUS_ERROR( "Out of memory for texels ( %d bytes)", bytes_required );
	}
	switch( texture_format )
	{
	case TexFmt_CI4_8888:
		mpPalette = VAlloc( PALETTE4_BYTES_REQUIRED );
		if( !mpPalette )
		{
			DAEDALUS_ERROR( "Out of memory for 4-bit palette, %d bytes", PALETTE4_BYTES_REQUIRED );
		}
		break;

	case TexFmt_CI8_8888:
		mpPalette = VAlloc( PALETTE8_BYTES_REQUIRED );
		if( !mpPalette )
		{
			DAEDALUS_ERROR( "Out of memory for 8-bit palette, %d bytes", PALETTE8_BYTES_REQUIRED );
		}
		break;

	default:
		DAEDALUS_ASSERT( !IsTextureFormatPalettised( texture_format ), "Unhandled palette texture" );
		break;
	}
}

CNativeTexture::~CNativeTexture()
{
	VFree ( mpData );
	VFree ( mpPalette );
}

bool	CNativeTexture::HasData() const
{
	return mpData != NULL && (!IsTextureFormatPalettised( mTextureFormat ) || mpPalette != NULL );
}

void	CNativeTexture::InstallTexture() const
{
	// FIXME: bind
}


namespace
{
	template< typename T >
	void ReadPngData( u32 width, u32 height, u32 stride, u8 ** p_row_table, int color_type, T * p_dest )
	{
		u8 r=0, g=0, b=0, a=0;

		for ( u32 y = 0; y < height; ++y )
		{
			const u8 * pRow = p_row_table[ y ];

			T * p_dest_row( p_dest );

			for ( u32 x = 0; x < width; ++x )
			{
				switch ( color_type )
				{
				case PNG_COLOR_TYPE_GRAY:
					r = g = b = *pRow++;
					if ( r == 0 && g == 0 && b == 0 )	a = 0x00;
					else								a = 0xff;
					break;
				case PNG_COLOR_TYPE_GRAY_ALPHA:
					r = g = b = *pRow++;
					if ( r == 0 && g == 0 && b == 0 )	a = 0x00;
					else								a = 0xff;
					pRow++;
					break;
				case PNG_COLOR_TYPE_RGB:
					b = *pRow++;
					g = *pRow++;
					r = *pRow++;
					if ( r == 0 && g == 0 && b == 0 )	a = 0x00;
					else								a = 0xff;
					break;
				case PNG_COLOR_TYPE_RGB_ALPHA:
					b = *pRow++;
					g = *pRow++;
					r = *pRow++;
					a = *pRow++;
					break;
				}

				p_dest_row[ x ] = T( r, g, b, a );
			}

			p_dest = reinterpret_cast< T * >( reinterpret_cast< u8 * >( p_dest ) + stride );
		}
	}

	//*****************************************************************************
	//	Thanks 71M/Shazz
	//	p_texture is either an existing texture (in case it must be of the
	//	correct dimensions and format) else a new texture is created and returned.
	//*****************************************************************************
	CRefPtr<CNativeTexture>	LoadPng( const char * p_filename, ETextureFormat texture_format )
	{
		const size_t	SIGNATURE_SIZE = 8;
		u8	signature[ SIGNATURE_SIZE ];

		FILE * fh( fopen( p_filename,"rb" ) );
		if(fh == NULL)
		{
			return NULL;
		}

		if (fread( signature, sizeof(u8), SIGNATURE_SIZE, fh ) != SIGNATURE_SIZE)
		{
			fclose(fh);
			return NULL;
		}

		if ( !png_check_sig( signature, SIGNATURE_SIZE ) )
		{
			return NULL;
		}

		png_struct * p_png_struct( png_create_read_struct( PNG_LIBPNG_VER_STRING, NULL, NULL, NULL ) );
		if ( p_png_struct == NULL)
		{
			return NULL;
		}

		png_info * p_png_info( png_create_info_struct( p_png_struct ) );
		if ( p_png_info == NULL )
		{
			png_destroy_read_struct( &p_png_struct, NULL, NULL );
			return NULL;
		}

		if ( setjmp( png_jmpbuf(p_png_struct) ) != 0 )
		{
			png_destroy_read_struct( &p_png_struct, NULL, NULL );
			return NULL;
		}

		png_init_io( p_png_struct, fh );
		png_set_sig_bytes( p_png_struct, SIGNATURE_SIZE );
		png_read_png( p_png_struct, p_png_info, PNG_TRANSFORM_STRIP_16 | PNG_TRANSFORM_PACKING | PNG_TRANSFORM_EXPAND | PNG_TRANSFORM_BGR, NULL );

		png_uint_32 width  = png_get_image_width( p_png_struct, p_png_info );
		png_uint_32 height = png_get_image_height( p_png_struct, p_png_info );

		CRefPtr<CNativeTexture>	texture = CNativeTexture::Create( width, height, texture_format );

		DAEDALUS_ASSERT( texture->GetWidth() >= width, "Width is unexpectedly small" );
		DAEDALUS_ASSERT( texture->GetHeight() >= height, "Height is unexpectedly small" );
		DAEDALUS_ASSERT( texture_format == texture->GetFormat(), "Texture format doesn't match" );

		u8 *	p_dest( new u8[ texture->GetBytesRequired() ] );
		if( !p_dest )
		{
			texture = NULL;
		}
		else
		{
			u32		stride( texture->GetStride() );

			u8 ** row_pointers = png_get_rows( p_png_struct, p_png_info );
			int color_type = png_get_color_type( p_png_struct, p_png_info );

			switch( texture_format )
			{
			case TexFmt_5650:
				ReadPngData< Pf5650 >( width, height, stride, row_pointers, color_type, reinterpret_cast< Pf5650 * >( p_dest ) );
				break;
			case TexFmt_5551:
				ReadPngData< Pf5551 >( width, height, stride, row_pointers, color_type, reinterpret_cast< Pf5551 * >( p_dest ) );
				break;
			case TexFmt_4444:
				ReadPngData< Pf4444 >( width, height, stride, row_pointers, color_type, reinterpret_cast< Pf4444 * >( p_dest ) );
				break;
			case TexFmt_8888:
				ReadPngData< Pf8888 >( width, height, stride, row_pointers, color_type, reinterpret_cast< Pf8888 * >( p_dest ) );
				break;

			case TexFmt_CI4_8888:
			case TexFmt_CI8_8888:
				DAEDALUS_ERROR( "Can't use palettised format for png." );
				break;

			default:
				DAEDALUS_ERROR( "Unhandled texture format" );
				break;
			}

			texture->SetData( p_dest, NULL );
		}

		//
		// Cleanup
		//
		delete [] p_dest;
		png_destroy_read_struct( &p_png_struct, &p_png_info, NULL );
		fclose(fh);

		return texture;
	}
}

CRefPtr<CNativeTexture>	CNativeTexture::CreateFromPng( const char * p_filename, ETextureFormat texture_format )
{
	return LoadPng( p_filename, texture_format );
}

void	CNativeTexture::SetData( void * data, void * palette )
{
	u32		bytes_per_row( GetStride() );

	if( HasData() )
	{
		mIsSwizzled = false;
		memcpy( mpData, data, bytes_per_row * mCorrectedHeight );

		if( mpPalette != NULL )
		{
			DAEDALUS_ASSERT( palette != NULL, "No palette provided" );

			#ifdef DAEDALUS_ENABLE_ASSERTS
				mPaletteSet = true;
			#endif

			switch( mTextureFormat )
			{
			case TexFmt_CI4_8888:
				memcpy( mpPalette, palette, PALETTE4_BYTES_REQUIRED );
				break;
			case TexFmt_CI8_8888:
				memcpy( mpPalette, palette, PALETTE8_BYTES_REQUIRED );
				break;

			default:
				DAEDALUS_ERROR( "Unhandled palette format" );
				break;
			}
		}
		else
		{
			DAEDALUS_ASSERT( palette == NULL, "Palette provided when not needed" );
		}
	}
}

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
u32	CNativeTexture::GetVideoMemoryUsage() const
{
	if( mIsDataVidMem )
	{
		return GetBytesRequired();
	}

	return 0;
}

u32	CNativeTexture::GetSystemMemoryUsage() const
{
	if( !mIsDataVidMem )
	{
		return GetBytesRequired();
	}

	return 0;
}
#endif

u32	CNativeTexture::GetStride() const
{
	return CalcBytesRequired( mTextureBlockWidth, mTextureFormat );
}

u32		CNativeTexture::GetBytesRequired() const
{
	return GetStride() * mCorrectedHeight;
}