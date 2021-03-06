﻿/*
	@file
	@author		Albert Semenov
	@date		11/2007
*/
/*
	This file is part of MyGUI.

	MyGUI is free software: you can redistribute it and/or modify
	it under the terms of the GNU Lesser General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	MyGUI is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Lesser General Public License for more details.

	You should have received a copy of the GNU Lesser General Public License
	along with MyGUI.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "MyGUI_Precompiled.h"
#include "MyGUI_ResourceTrueTypeFont.h"
#include "MyGUI_DataManager.h"
#include "MyGUI_RenderManager.h"
#include "MyGUI_Bitwise.h"

#ifdef MYGUI_USE_FREETYPE

	#include FT_GLYPH_H
	#include FT_TRUETYPE_TABLES_H
	#include FT_BITMAP_H
	#include FT_WINFONTS_H

	// The following macro enables a workaround for a bug in FreeType's bytecode interpreter that, when using certain fonts at
	// certain sizes, causes FreeType to start measuring and rendering some glyphs inconsistently after certain other glyphs have
	// been loaded. See FreeType bug #35374 for details: https://savannah.nongnu.org/bugs/?35374
	//
	// To reproduce the bug, first disable the workaround by defining MYGUI_USE_FREETYPE_BYTECODE_BUG_FIX to 0. Then load the
	// DejaVu Sans font at 10 pt using default values for all other properties. Observe that the glyphs for the "0", 6", "8", and
	// "9" characters are now badly corrupted when rendered.
	//
	// This bug still exists as of FreeType 2.4.8 and there are currently no plans to fix it. If the bug is ever fixed, this
	// workaround should be disabled, as it causes fonts to take longer to load.
	//
	// The bug can currently also be suppressed by disabling FreeType's bytecode interpreter altogether. To do so, remove the
	// TT_CONFIG_OPTION_BYTECODE_INTERPRETER macro in the "ftoption.h" FreeType header file. Once this is done, this workaround can
	// be safely disabled. Note that disabling FreeType's bytecode interpreter will cause rendered text to look somewhat different.
	// Whether it looks better or worse is a matter of taste and may also depend on the font.
	#ifndef MYGUI_USE_FREETYPE_BYTECODE_BUG_FIX
		#define MYGUI_USE_FREETYPE_BYTECODE_BUG_FIX 1
	#endif

#endif // MYGUI_USE_FREETYPE

namespace MyGUI
{
	const int ResourceTrueTypeFont::sDefaultTextureBufferWidth = 512;
	const int ResourceTrueTypeFont::sDefaultTextureBufferHeight = 512;

	//class FTFactory;
	//class FTFace;
	class FTFaceCache;


	class FTFactory
	{
	public:
		~FTFactory()
		{
			if (mLibrary)
			{
				FT_Done_FreeType(mLibrary);
			}
		}
		FT_Library GetFT_Library()
		{
			if (nullptr == mLibrary)
			{
				if (FT_Init_FreeType(&mLibrary) != 0)
					MYGUI_EXCEPT("ResourceTrueTypeFont: Could not init the FreeType library!");
			}
			return mLibrary;
		}

		static void CreateFTFactory(FTFactory** instance)
		{
			assert(nullptr != instance);
			assert(nullptr == *instance);
			++(sInst.mCount);
			(*instance) = &sInst;			
		}
		static void DistroyFTFactory(FTFactory** instance)
		{
			if (&sInst == *instance)
			{
				--sInst.mCount;
				if (0 >= sInst.mCount)
				{
					if (sInst.mLibrary)
					{
						FT_Done_FreeType(sInst.mLibrary);
						sInst.mLibrary = nullptr;
					}
				}
			}
			*instance = nullptr;
		}
	private:
		FTFactory()
			:mLibrary(nullptr)
			,mCount(0)
		{
		}
		FT_Library mLibrary;
		int mCount;		//引用计数器

		static FTFactory sInst;
		
	};
	FTFactory FTFactory::sInst = FTFactory();


	class FTFace
	{
	public:
		friend class FTFaceCache;
		inline FT_Face GetFT_Face()
		{
			return mFace;
		}
		void CountAdd()
		{
			++mCounter;
		}
	private:		
		FTFace()
			:mFace(nullptr)
			,mBuffer(nullptr)
			,mCounter(0)
		{

		}
		FTFace(FT_Face face, uint8* buffer, std::string& name)
		{
			mFace = face;
			mBuffer = buffer;
			mName = name;
			mCounter = 1;
		}

		~FTFace()
		{
			FT_Done_Face(mFace);
			delete[] mBuffer;
		}
		FT_Face mFace;
		uint8* mBuffer;
		std::string mName;
		int mCounter;
	};

	class FTFaceCache
	{
	public:
		~FTFaceCache()
		{
			FaceBufferMap::iterator begin = mMap.begin();
			FaceBufferMap::iterator end = mMap.end();
			while(begin != end)
			{
				delete[] *begin;
				++begin;
			}
		}
		static FTFace* Find(std::string& file_name)
		{
			FaceBufferMap::iterator begin = mMap.begin();
			FaceBufferMap::iterator end = mMap.end();
			while(begin != end)
			{
				if ((*begin)->mName == file_name)
				{
					return (*begin);
				}
				++begin;
			}
			return nullptr;
		}
		static FTFace* BuildFace(FT_Face ft_face, uint8* buffer, std::string& name)
		{
			FTFace* face = new FTFace(ft_face, buffer, name);
			mMap.push_back(face);
			return face;
		}
		static void ReleaseFace(FTFace* face)
		{
			if (face->mCounter > 1)
			{
				--face->mCounter;
			}
			else
			{
				FaceBufferMap::iterator result = find(mMap.begin(), mMap.end(), face);
				if (result < mMap.end())
				{
					delete *result;
				}
				mMap.erase(result);
			}

		}
	private:
		typedef std::vector<FTFace*> FaceBufferMap;
		static FaceBufferMap mMap;
	};
	FTFaceCache::FaceBufferMap FTFaceCache::mMap;
	// ------------------------------------------------------------------------------------------------------


	namespace
	{

		template<typename T>
		void setMax(T& _var, const T& _newValue)
		{
			if (_var < _newValue)
				_var = _newValue;
		}

		std::pair<const Char, const uint8> charMaskData[] =
		{
			std::make_pair(FontCodeType::Selected, (const uint8)'\x88'),
			std::make_pair(FontCodeType::SelectedBack, (const uint8)'\x60'),
			std::make_pair(FontCodeType::Cursor, (const uint8)'\xFF'),
			std::make_pair(FontCodeType::Tab, (const uint8)'\x00')
		};

		const std::map<const Char, const uint8> charMask(charMaskData, charMaskData + sizeof charMaskData / sizeof *charMaskData);

		const uint8 charMaskBlack = (const uint8)'\x00';
		const uint8 charMaskWhite = (const uint8)'\xFF';

		template<bool LAMode>
		struct PixelBase
		{
			// Returns PixelFormat::R8G8B8A8 when LAMode is false, or PixelFormat::L8A8 when LAMode is true.
			static PixelFormat::Enum getFormat();

			// Returns 4 when LAMode is false, or 2 when LAMode is true.
			static size_t getNumBytes();

		protected:
			// Sets a pixel in _dest as 4 or 2 bytes: L8L8L8A8 if LAMode is false, or L8A8 if LAMode is true.
			// Automatically advances _dest just past the pixel written.
			static void set(uint8*& _dest, uint8 _luminance, uint8 _alpha);
		};

		template<>
		struct PixelBase<false>
		{
			static size_t getNumBytes()
			{
				return 4;
			}

			static PixelFormat::Enum getFormat()
			{
				return PixelFormat::R8G8B8A8;
			}

		protected:
			static void set(uint8*& _dest, uint8 _luminance, uint8 _alpha)
			{
				*_dest++ = _luminance;
				*_dest++ = _luminance;
				*_dest++ = _luminance;
				*_dest++ = _alpha;
			}
		};

		template<>
		struct PixelBase<true>
		{
			static size_t getNumBytes()
			{
				return 2;
			}

			static PixelFormat::Enum getFormat()
			{
				return PixelFormat::L8A8;
			}

		protected:
			static void set(uint8*& _dest, uint8 _luminance, uint8 _alpha)
			{
				*_dest++ = _luminance;
				*_dest++ = _alpha;
			}
		};

		template<bool LAMode, bool FromSource = false, bool Antialias = false>
		struct Pixel : PixelBase<LAMode>
		{
			// Sets a pixel in _dest as 4 or 2 bytes: L8L8L8A8 if LAMode is false, or L8A8 if LAMode is true.
			// Sets luminance from _source if both FromSource and Antialias are true; otherwise, uses the specified value.
			// Sets alpha from _source if FromSource is true; otherwise, uses the specified value.
			// Automatically advances _source just past the pixel read if FromSource is true.
			// Automatically advances _dest just past the pixel written.
			static void set(uint8*& _dest, uint8 _luminance, uint8 _alpha, uint8*& _source);
		};

		template<bool LAMode, bool Antialias>
		struct Pixel<LAMode, false, Antialias> : PixelBase<LAMode>
		{
			// Sets the destination pixel using the specified luminance and alpha. Source is ignored, since FromSource is false.
			static void set(uint8*& _dest, uint8 _luminance, uint8 _alpha, uint8* = nullptr)
			{
				PixelBase<LAMode>::set(_dest, _luminance, _alpha);
			}
		};

		template<bool LAMode>
		struct Pixel<LAMode, true, false> : PixelBase<LAMode>
		{
			// Sets the destination pixel using the specified _luminance and using the alpha from the specified source.
			static void set(uint8*& _dest, uint8 _luminance, uint8, uint8*& _source)
			{
				PixelBase<LAMode>::set(_dest, _luminance, *_source++);
			}
		};

		template<bool LAMode>
		struct Pixel<LAMode, true, true> : PixelBase<LAMode>
		{
			// Sets the destination pixel using both the luminance and alpha from the specified source, since Antialias is true.
			static void set(uint8*& _dest, uint8, uint8, uint8*& _source)
			{
				PixelBase<LAMode>::set(_dest, *_source, *_source);
				++_source;
			}
		};

	}

	const int ResourceTrueTypeFont::mGlyphSpacing = 1;
	const float ResourceTrueTypeFont::mSelectedWidth = 1.0f;
	const float ResourceTrueTypeFont::mCursorWidth = 2.0f;

	ResourceTrueTypeFont::ResourceTrueTypeFont() :
		mSize(0),
		mResolution(96),
		mAntialias(false),
		mSpaceWidth(0.0f),
		mTabWidth(0.0f),
		mOffsetHeight(0),
		mSubstituteCodePoint(FontCodeType::NotDefined),
		mDefaultHeight(0),
		mSubstituteGlyphInfo(nullptr),
		mTexture(nullptr),

		mFTFactory(nullptr),
		mFTFace(nullptr),
		//mFTBuffer(nullptr),
		mTexturePacker(0, 0),
		mTextureWidth(0),
		mTextureHeight(0),
		mAutoTextureSize(false),
		mFontAscent(0),
		mFontDescent(0)

	{
	}

	ResourceTrueTypeFont::~ResourceTrueTypeFont()
	{
		if (mFTLock.IsLock)
		{
			unlock();
		}
		if (mFTLock.FTBitmap)
		{
			FT_Bitmap_Done(mFTFactory->GetFT_Library(), mFTLock.FTBitmap);
			delete[] mFTLock.FTBitmap;
		}

		if (mTexture != nullptr)
		{
			RenderManager::getInstance().destroyTexture(mTexture);
			mTexture = nullptr;
		}
		if (mFTFace)
		{
			FTFaceCache::ReleaseFace(mFTFace);
			//FT_Done_Face(mFTFace);
		}
		if (mFTFactory)
		{
			FTFactory::DistroyFTFactory(&mFTFactory);
		}		
		//if (mFTBuffer)
		//{
		//	delete [] mFTBuffer;
		//}
	}

	void ResourceTrueTypeFont::deserialization(xml::ElementPtr _node, Version _version)
	{
		Base::deserialization(_node, _version);

		xml::ElementEnumerator node = _node->getElementEnumerator();
		while (node.next())
		{
			if (node->getName() == "Property")
			{
				const std::string& key = node->findAttribute("key");
				const std::string& value = node->findAttribute("value");
				if (key == "Source") mSource = value;
				else if (key == "Size") mSize = utility::parseFloat(value);
				else if (key == "Resolution") mResolution = utility::parseUInt(value);
				else if (key == "Antialias") mAntialias = utility::parseBool(value);
				else if (key == "SpaceWidth")
				{
					mSpaceWidth = utility::parseFloat(value);
					MYGUI_LOG(Warning, _node->findAttribute("type") << ": Property '" << key << "' in font '" << _node->findAttribute("name") << "' is deprecated; remove it to use automatic calculation.");
				}
				else if (key == "TabWidth") mTabWidth = utility::parseFloat(value);
				else if (key == "OffsetHeight") mOffsetHeight = utility::parseInt(value);
				else if (key == "SubstituteCode") mSubstituteCodePoint = utility::parseInt(value);
				else if (key == "CursorWidth" || key == "Distance")
				{
					MYGUI_LOG(Warning, _node->findAttribute("type") << ": Property '" << key << "' in font '" << _node->findAttribute("name") << "' is deprecated; value ignored.");
				}
				else if (key == "TextureSize")	// 缓冲纹理的大小
				{
	
					if ("Auto" == value || "auto" == value)
					{
						mAutoTextureSize = true;
					}
					else
					{
						mAutoTextureSize = false;
						std::vector<std::string> parse_size = utility::split(value);
						if (!parse_size.empty())
						{
							mTextureWidth = utility::parseUInt(parse_size[0]);
							if (parse_size.size() > 1)
							{
								mTextureHeight = utility::parseUInt(parse_size[1]);
							}
							else
							{
								mTextureHeight = mTextureWidth;
							}
							mTextureWidth = Bitwise::firstPO2From(mTextureWidth);
							mTextureHeight = Bitwise::firstPO2From(mTextureHeight);
						}
					}

				}
			}
			else if (node->getName() == "Codes")
			{
				// Range of inclusions.
				xml::ElementEnumerator range = node->getElementEnumerator();
				while (range.next("Code"))
				{
					std::string range_value;
					if (range->findAttribute("range", range_value))
					{
						std::vector<std::string> parse_range = utility::split(range_value);
						if (!parse_range.empty())
						{
							Char first = utility::parseUInt(parse_range[0]);
							Char last = parse_range.size() > 1 ? utility::parseUInt(parse_range[1]) : first;
							addCodePointRange(first, last);
						}
					}
				}

				// If no code points have been included, include the Unicode Basic Multilingual Plane by default before processing
				//	any exclusions.
				if (mCharMap.empty())
					addCodePointRange(0, 0xFFFF);

				// Range of exclusions.
				range = node->getElementEnumerator();
				while (range.next("Code"))
				{
					std::string range_value;
					if (range->findAttribute("hide", range_value))
					{
						std::vector<std::string> parse_range = utility::split(range_value);
						if (!parse_range.empty())
						{
							Char first = utility::parseUInt(parse_range[0]);
							Char last = parse_range.size() > 1 ? utility::parseUInt(parse_range[1]) : first;
							removeCodePointRange(first, last);
						}
					}
				}
			}
		}

		initialise();
	}

#ifdef MYGUI_USE_FREETYPE

	//GlyphInfo* ResourceTrueTypeFont::getGlyphInfo(Char _id)
	//{
		//CharMap::const_iterator charIter = mCharMap.find(_id);

		//if (charIter != mCharMap.end())
		//{
		//	GlyphMap::iterator glyphIter = mGlyphMap.find(charIter->second);

		//	if (glyphIter != mGlyphMap.end())
		//		return &glyphIter->second;
		//}

		//return mSubstituteGlyphInfo;
	//}
	bool ResourceTrueTypeFont::getGlyphInfo(Char _id, GlyphInfo** _info)
	{
		assert(nullptr != _info);
		assert(nullptr == *_info);
		CharMap::const_iterator charIter = mCharMap.find(_id);

		if (charIter != mCharMap.end())
		{
			GlyphMap::iterator glyphIter = mGlyphMap.find(charIter->second);

			if (glyphIter != mGlyphMap.end())
			{
				 *_info = &glyphIter->second;
				 return true;
			}
		}

		*_info = mSubstituteGlyphInfo;
		return false;
	}

#else

	GlyphInfo* ResourceTrueTypeFont::getGlyphInfo(Char _id)
	{
		return nullptr;
	}

#endif // MYGUI_USE_FREETYPE

	bool ResourceTrueTypeFont::lock()
	{
		if (!mFTLock.IsLock && mTexture && mFTFace->GetFT_Face())
		{
			mFTLock.TexBuffer = static_cast<uint8*>(mTexture->lock(TextureUsage::Write));
			mFTLock.IsLock = true;

			if (mFTFace->GetFT_Face()->face_flags & FT_FACE_FLAG_SCALABLE)
			{
				// The font is scalable, so set the font size by first converting the requested size to FreeType's 26.6 fixed-point
				// format.
				FT_F26Dot6 ftSize = (FT_F26Dot6)(mSize * (1 << 6));

				if (FT_Set_Char_Size(mFTFace->GetFT_Face(), ftSize, 0, mResolution, mResolution) != 0)
					MYGUI_EXCEPT("ResourceTrueTypeFont: Could not set the font size for '" << getResourceName() << "'!");
			}
			else
			{
				// Select the first bitmap strike available in the selected face. This needs to be done explicitly even though Windows
				// FNT/FON files contain only one bitmap strike per face.
				if (FT_Select_Size(mFTFace->GetFT_Face(), 0) != 0)
					MYGUI_EXCEPT("ResourceTrueTypeFont: Could not set the font size for '" << getResourceName() << "'!");
			}
			return mFTLock.IsLock;
		}
		return false;
	}

	void ResourceTrueTypeFont::unlock()
	{
		if (mFTLock.IsLock)
		{
			mTexture->unlock();		
			mFTLock.IsLock = false;
		}

	}
	bool ResourceTrueTypeFont::appendChar(Char _codePoint, GlyphInfo** _info)
	{
		assert(nullptr == *_info);
		assert(mFTLock.IsLock);
		FT_UInt glyphIndex = FT_Get_Char_Index(mFTFace->GetFT_Face(), _codePoint);
		if (0 == glyphIndex)
		{
			if (_info)
			{
				*_info = mSubstituteGlyphInfo;
			}
			return false;
		}

		FT_GlyphSlot glyph = mFTFace->GetFT_Face()->glyph;
		float bearingX = glyph->metrics.horiBearingX / 64.0f;
		GlyphInfo glyphInfo(_codePoint, 
			glyph->metrics.width / 64.0f, 
			glyph->metrics.height / 64.0f,
			(glyph->advance.x / 64.0f) - bearingX,
			bearingX, 
			mFontAscent - (glyph->metrics.horiBearingY / 64.0f) - mOffsetHeight);

#if MYGUI_USE_FREETYPE_BYTECODE_BUG_FIX
		if (FT_Load_Glyph(mFTFace->GetFT_Face(), glyphIndex, FT_LOAD_DEFAULT) == 0)
		{
			GlyphInfo newInfo = createFaceGlyphInfo(0, mFontAscent, mFTFace->GetFT_Face()->glyph);

			if (glyphInfo.width != newInfo.width)
			{
				glyphInfo.width = newInfo.width;
			}

			if (glyphInfo.height != newInfo.height)
				glyphInfo.height = newInfo.height;			

			if (glyphInfo.advance != newInfo.advance)
				glyphInfo.advance = newInfo.advance;

			if (glyphInfo.bearingX != newInfo.bearingX)
				glyphInfo.bearingX = newInfo.bearingX;

			if (glyphInfo.bearingY != newInfo.bearingY)
				glyphInfo.bearingY = newInfo.bearingY;
		}
		else
		{
			MYGUI_LOG(Warning, "ResourceTrueTypeFont: Cannot load glyph " << glyphIndex << " for character " << _codePoint << " in font '" << getResourceName() << "'.");
		}
#endif // MYGUI_USE_FREETYPE_BYTECODE_BUG_FIX

		if (FT_Load_Glyph(mFTFace->GetFT_Face(), glyphIndex, FT_LOAD_RENDER) == 0)
		{
			if (mFTFace->GetFT_Face()->glyph->bitmap.buffer != nullptr)
			{
				uint8* glyphBuffer = nullptr;

				switch (mFTFace->GetFT_Face()->glyph->bitmap.pixel_mode)
				{
				case FT_PIXEL_MODE_GRAY:
					glyphBuffer = mFTFace->GetFT_Face()->glyph->bitmap.buffer;
					break;

				case FT_PIXEL_MODE_MONO:
					if (nullptr == mFTLock.FTBitmap)
					{
						mFTLock.FTBitmap = new FT_Bitmap();
						FT_Bitmap_New(mFTLock.FTBitmap);
					}
					// Convert the monochrome bitmap to 8-bit before rendering it.
					if (FT_Bitmap_Convert(mFTFactory->GetFT_Library(), &mFTFace->GetFT_Face()->glyph->bitmap, mFTLock.FTBitmap, 1) == 0)
					{
						// Go through the bitmap and convert all of the nonzero values to 0xFF (white).
						for (uint8* p = mFTLock.FTBitmap->buffer, * endP = p + mFTLock.FTBitmap->width * mFTLock.FTBitmap->rows; p != endP; ++p)
							*p ^= -*p ^ *p;

						glyphBuffer = mFTLock.FTBitmap->buffer;
					}
					break;
				}

				if (glyphBuffer != nullptr)
				{
					int texX = 0, texY = 0;	
					// Select and call an appropriate initialisation method. By making this decision up front, we avoid having to branch on
					// these variables many thousands of times inside tight nested loops later. From this point on, the various function
					// templates ensure that all of the necessary branching is done purely at compile time for all combinations.
					int init = (mLaMode ? 2 : 0) | (mAntialias ? 1 : 0);
					
					switch (init)
					{
					case 0:
						renderGlyph<false, true, false>(glyphInfo, charMaskWhite, charMaskWhite, charMaskWhite, (int)glyphInfo.height, mFTLock.TexBuffer, mTextureWidth, mTextureHeight, texX, texY, glyphBuffer);
						break;
					case 1:
						renderGlyph<false, true, true>(glyphInfo, charMaskWhite, charMaskWhite, charMaskWhite, (int)glyphInfo.height, mFTLock.TexBuffer, mTextureWidth, mTextureHeight, texX, texY, glyphBuffer);
						break;
					case 2:
						renderGlyph<true, true, false>(glyphInfo, charMaskWhite, charMaskWhite, charMaskWhite, (int)glyphInfo.height, mFTLock.TexBuffer, mTextureWidth, mTextureHeight, texX, texY, glyphBuffer);
						break;
					case 3:
						renderGlyph<true, true, true>(glyphInfo, charMaskWhite, charMaskWhite, charMaskWhite, (int)glyphInfo.height, mFTLock.TexBuffer, mTextureWidth, mTextureHeight, texX, texY, glyphBuffer);
						break;
					}
				}
			}
		}
		else
		{
			if (_info)
			{
				*_info = mSubstituteGlyphInfo;
			}
			MYGUI_LOG(Warning, "ResourceTrueTypeFont: Cannot render glyph " << glyphIndex << " for character " << _codePoint << " in font '" << getResourceName() << "'.");
			return false;
		}
		mCharMap.insert(CharMap::value_type(_codePoint, glyphIndex));
		
		if (_info)
		{
			*_info = &(mGlyphMap.insert(GlyphMap::value_type(glyphIndex, glyphInfo)).first->second);
		}
		else
		{
			mGlyphMap.insert(GlyphMap::value_type(glyphIndex, glyphInfo));
		}
		return true;
	}


	ITexture* ResourceTrueTypeFont::getTextureFont()
	{
		return mTexture;
	}

	int ResourceTrueTypeFont::getDefaultHeight()
	{
		return mDefaultHeight;
	}

	std::vector<std::pair<Char, Char> > ResourceTrueTypeFont::getCodePointRanges() const
	{
		std::vector<std::pair<Char, Char> > result;

		if (!mCharMap.empty())
		{
			CharMap::const_iterator iter = mCharMap.begin(), endIter = mCharMap.end();

			// Start the first range with the first code point in the map.
			Char rangeBegin = iter->first, rangeEnd = rangeBegin;

			// Loop over the rest of the map and find the contiguous ranges.
			for (++iter; iter != endIter; ++iter)
			{
				if (iter->first == rangeEnd + 1)
				{
					// Extend the current range.
					++rangeEnd;
				}
				else
				{
					// Found the start of a new range. First, save the current range.
					result.push_back(std::make_pair(rangeBegin, rangeEnd));

					// Then start the new range.
					rangeBegin = rangeEnd = iter->first;
				}
			}

			// Save the final range.
			result.push_back(std::make_pair(rangeBegin, rangeEnd));
		}

		return result;
	}

	Char ResourceTrueTypeFont::getSubstituteCodePoint() const
	{
		return mSubstituteCodePoint;
	}

	void ResourceTrueTypeFont::addCodePoint(Char _codePoint)
	{
		mCharMap.insert(CharMap::value_type(_codePoint, 0));
	}

	void ResourceTrueTypeFont::removeCodePoint(Char _codePoint)
	{
		mCharMap.erase(_codePoint);
	}

	void ResourceTrueTypeFont::addCodePointRange(Char _first, Char _second)
	{
		CharMap::iterator positionHint = mCharMap.lower_bound(_first);

		if (positionHint != mCharMap.begin())
			--positionHint;

		for (Char i = _first; i <= _second; ++i)
			positionHint = mCharMap.insert(positionHint, CharMap::value_type(i, 0));
	}

	void ResourceTrueTypeFont::removeCodePointRange(Char _first, Char _second)
	{
		mCharMap.erase(mCharMap.lower_bound(_first), mCharMap.upper_bound(_second));
	}

	void ResourceTrueTypeFont::clearCodePoints()
	{
		mCharMap.clear();
	}

	void ResourceTrueTypeFont::initialise()
	{
#ifndef MYGUI_USE_FREETYPE

		MYGUI_LOG(Error, "ResourceTrueTypeFont: TrueType font '" << getResourceName() << "' disabled. Define MYGUI_USE_FREETYE if you need TrueType fonts.");

#else // MYGUI_USE_FREETYPE

		// If L8A8 (2 bytes per pixel) is supported, use it; otherwise, use R8G8B8A8 (4 bytes per pixel) as L8L8L8A8.
		//bool laMode = MyGUI::RenderManager::getInstance().isFormatSupported(Pixel<true>::getFormat(), TextureUsage::Static | TextureUsage::Write);

		mLaMode = MyGUI::RenderManager::getInstance().isFormatSupported(Pixel<true>::getFormat(), TextureUsage::Static | TextureUsage::Write);

		// Select and call an appropriate initialisation method. By making this decision up front, we avoid having to branch on
		// these variables many thousands of times inside tight nested loops later. From this point on, the various function
		// templates ensure that all of the necessary branching is done purely at compile time for all combinations.
		int init = (mLaMode ? 2 : 0) | (mAntialias ? 1 : 0);

		switch (init)
		{
		case 0:
			ResourceTrueTypeFont::initialiseFreeType<false, false>();
			break;
		case 1:
			ResourceTrueTypeFont::initialiseFreeType<false, true>();
			break;
		case 2:
			ResourceTrueTypeFont::initialiseFreeType<true, false>();
			break;
		case 3:
			ResourceTrueTypeFont::initialiseFreeType<true, true>();
			break;
		}

#endif // MYGUI_USE_FREETYPE
	}

#ifdef MYGUI_USE_FREETYPE

	template<bool LAMode, bool Antialias>
	void ResourceTrueTypeFont::initialiseFreeType()
	{
		//-------------------------------------------------------------------//
		// Initialise FreeType and load the font.
		//-------------------------------------------------------------------//

		// -------------- [2012/5/8 zhongdaohuan] --------------------------------

		FTFactory::CreateFTFactory(&mFTFactory);
		//mFTBuffer = nullptr;
		mFTFace = loadFace(mFTFactory->GetFT_Library());
		if (mFTFace == nullptr)
		{
			MYGUI_LOG(Error, "ResourceTrueTypeFont: Could not load the font '" << getResourceName() << "'!");
			return;
		}
		if (mFTFace->GetFT_Face()->face_flags & FT_FACE_FLAG_SCALABLE)
		{
			// The font is scalable, so set the font size by first converting the requested size to FreeType's 26.6 fixed-point
			// format.
			FT_F26Dot6 ftSize = (FT_F26Dot6)(mSize * (1 << 6));

			if (FT_Set_Char_Size(mFTFace->GetFT_Face(), ftSize, 0, mResolution, mResolution) != 0)
				MYGUI_EXCEPT("ResourceTrueTypeFont: Could not set the font size for '" << getResourceName() << "'!");


		}
		else
		{
			// Select the first bitmap strike available in the selected face. This needs to be done explicitly even though Windows
			// FNT/FON files contain only one bitmap strike per face.
			if (FT_Select_Size(mFTFace->GetFT_Face(), 0) != 0)
				MYGUI_EXCEPT("ResourceTrueTypeFont: Could not set the font size for '" << getResourceName() << "'!");
		}






		// -----------------------------------------------------------------------


		//if (mFTFace == nullptr)
		//{
		//	MYGUI_LOG(Error, "ResourceTrueTypeFont: Could not load the font '" << getResourceName() << "'!");
		//	return;
		//}

		//-------------------------------------------------------------------//
		// Calculate the font metrics.
		//-------------------------------------------------------------------//

		// The font's overall ascent and descent are defined in three different places in a TrueType font, and with different
		// values in each place. The most reliable source for these metrics is usually the "usWinAscent" and "usWinDescent" pair of
		// values in the OS/2 header; however, some fonts contain inaccurate data there. To be safe, we use the highest of the set
		// of values contained in the face metrics and the two sets of values contained in the OS/2 header.
		mFontAscent = mFTFace->GetFT_Face()->size->metrics.ascender >> 6;
		mFontDescent = -mFTFace->GetFT_Face()->size->metrics.descender >> 6;

		TT_OS2* os2 = (TT_OS2*)FT_Get_Sfnt_Table(mFTFace->GetFT_Face(), ft_sfnt_os2);

		if (os2 != nullptr)
		{
			setMax(mFontAscent, os2->usWinAscent * mFTFace->GetFT_Face()->size->metrics.y_ppem / mFTFace->GetFT_Face()->units_per_EM);
			setMax(mFontDescent, os2->usWinDescent * mFTFace->GetFT_Face()->size->metrics.y_ppem / mFTFace->GetFT_Face()->units_per_EM);

			setMax(mFontAscent, os2->sTypoAscender * mFTFace->GetFT_Face()->size->metrics.y_ppem / mFTFace->GetFT_Face()->units_per_EM);
			setMax(mFontDescent, -os2->sTypoDescender * mFTFace->GetFT_Face()->size->metrics.y_ppem / mFTFace->GetFT_Face()->units_per_EM);
		}

		// The nominal font height is calculated as the sum of its ascent and descent as specified by the font designer. Previously
		// it was defined by MyGUI in terms of the maximum ascent and descent of the glyphs currently in use, but this caused the
		// font's line spacing to change whenever glyphs were added to or removed from the font definition. Doing it this way
		// instead prevents a lot of layout problems, and it is also more typographically correct and more aesthetically pleasing.
		mDefaultHeight = mFontAscent + mFontDescent;

		//-------------------------------------------------------------------//
		// Create the glyphs and calculate their metrics.
		//-------------------------------------------------------------------//

		GlyphHeightMap glyphHeightMap;
		int texWidth = 0;

		// Before creating the glyphs, add the "Space" code point to force that glyph to be created. For historical reasons, MyGUI
		// users are accustomed to omitting this code point in their font definitions.
		addCodePoint(FontCodeType::Space);

		// Create the standard glyphs.
		for (CharMap::iterator iter = mCharMap.begin(); iter != mCharMap.end(); )
		{
			const Char& codePoint = iter->first;
			FT_UInt glyphIndex = FT_Get_Char_Index(mFTFace->GetFT_Face(), codePoint);

			texWidth += createFaceGlyph(glyphIndex, codePoint, mFontAscent, mFTFace->GetFT_Face(), glyphHeightMap);

			// If the newly created glyph is the "Not Defined" glyph, it means that the code point is not supported by the font.
			// Remove it from the character map so that we can provide our own substitute instead of letting FreeType do it.
			if (iter->second != 0)
				++iter;
			else
				mCharMap.erase(iter++);
		}

#if MYGUI_USE_FREETYPE_BYTECODE_BUG_FIX

		for (GlyphMap::iterator iter = mGlyphMap.begin(); iter != mGlyphMap.end(); ++iter)
		{
			if (FT_Load_Glyph(mFTFace->GetFT_Face(), iter->first, FT_LOAD_DEFAULT) == 0)
			{
				GlyphInfo& info = iter->second;
				GlyphInfo newInfo = createFaceGlyphInfo(0, mFontAscent, mFTFace->GetFT_Face()->glyph);

				if (info.width != newInfo.width)
				{
					texWidth += (int)ceil(newInfo.width) - (int)ceil(info.width);
					info.width = newInfo.width;
				}

				if (info.height != newInfo.height)
				{
					GlyphHeightMap::mapped_type oldHeightMap = glyphHeightMap[(FT_Pos)info.height];
					GlyphHeightMap::mapped_type::iterator heightMapItem = oldHeightMap.find(iter->first);
					glyphHeightMap[(FT_Pos)newInfo.height].insert(*heightMapItem);
					oldHeightMap.erase(heightMapItem);
					info.height = newInfo.height;
				}

				if (info.advance != newInfo.advance)
					info.advance = newInfo.advance;

				if (info.bearingX != newInfo.bearingX)
					info.bearingX = newInfo.bearingX;

				if (info.bearingY != newInfo.bearingY)
					info.bearingY = newInfo.bearingY;
			}
			else
			{
				MYGUI_LOG(Warning, "ResourceTrueTypeFont: Cannot load glyph " << iter->first << " for character " << iter->second.codePoint << " in font '" << getResourceName() << "'.");
			}
		}

#endif // MYGUI_USE_FREETYPE_BYTECODE_BUG_FIX

		// Do some special handling for the "Space" and "Tab" glyphs.
		//GlyphInfo* spaceGlyphInfo = getGlyphInfo(FontCodeType::Space);
		
		GlyphInfo* spaceGlyphInfo = nullptr;
		getGlyphInfo(FontCodeType::Space, &spaceGlyphInfo);


		if (spaceGlyphInfo != nullptr && spaceGlyphInfo->codePoint == FontCodeType::Space)
		{
			// Adjust the width of the "Space" glyph if it has been customized.
			if (mSpaceWidth != 0.0f)
			{
				texWidth += (int)ceil(mSpaceWidth) - (int)ceil(spaceGlyphInfo->width);
				spaceGlyphInfo->width = mSpaceWidth;
				spaceGlyphInfo->advance = mSpaceWidth;
			}

			// If the width of the "Tab" glyph hasn't been customized, make it eight spaces wide.
			if (mTabWidth == 0.0f)
				mTabWidth = 8.0f * spaceGlyphInfo->advance;
		}

		// Create the special glyphs. They must be created after the standard glyphs so that they take precedence in case of a
		// collision. To make sure that the indices of the special glyphs don't collide with any glyph indices in the font, we must
		// use glyph indices higher than the highest glyph index in the font.
		FT_UInt nextGlyphIndex = (FT_UInt)mFTFace->GetFT_Face()->num_glyphs;

		float height = (float)mDefaultHeight;

		texWidth += createGlyph(nextGlyphIndex++, GlyphInfo(FontCodeType::Tab, 0.0f, 0.0f, mTabWidth, 0.0f, 0.0f), glyphHeightMap);
		texWidth += createGlyph(nextGlyphIndex++, GlyphInfo(FontCodeType::Selected, mSelectedWidth, height, 0.0f, 0.0f, 0.0f), glyphHeightMap);
		texWidth += createGlyph(nextGlyphIndex++, GlyphInfo(FontCodeType::SelectedBack, mSelectedWidth, height, 0.0f, 0.0f, 0.0f), glyphHeightMap);
		texWidth += createGlyph(nextGlyphIndex++, GlyphInfo(FontCodeType::Cursor, mCursorWidth, height, 0.0f, 0.0f, 0.0f), glyphHeightMap);

		// If a substitute code point has been specified, check to make sure that it exists in the character map. If it doesn't,
		// revert to the default "Not Defined" code point. This is not a real code point but rather an invalid Unicode value that
		// is guaranteed to cause the "Not Defined" special glyph to be created.
		if (mSubstituteCodePoint != FontCodeType::NotDefined && mCharMap.find(mSubstituteCodePoint) == mCharMap.end())
		{
			mSubstituteCodePoint = FontCodeType::NotDefined;
		}

		// Create the "Not Defined" code point (and its corresponding glyph) if it's in use as the substitute code point.
		if (mSubstituteCodePoint == FontCodeType::NotDefined)
		{
			texWidth += createFaceGlyph(0, FontCodeType::NotDefined, mFontAscent, mFTFace->GetFT_Face(), glyphHeightMap);
		}

		// Cache a pointer to the substitute glyph info for fast lookup.
		mSubstituteGlyphInfo = &mGlyphMap.find(mCharMap.find(mSubstituteCodePoint)->second)->second;

		// Calculate the average height of all of the glyphs that are in use. This value will be used for estimating how large the
		// texture needs to be.
		double averageGlyphHeight = 0.0;

		for (GlyphHeightMap::const_iterator j = glyphHeightMap.begin(); j != glyphHeightMap.end(); ++j)
			averageGlyphHeight += j->first * j->second.size();

		averageGlyphHeight /= mGlyphMap.size();

		//-------------------------------------------------------------------//
		// Calculate the final texture size.
		//-------------------------------------------------------------------//

		// Round the current texture width and height up to the nearest powers of two.
		texWidth = Bitwise::firstPO2From(texWidth);
		int texHeight = Bitwise::firstPO2From((int)ceil(averageGlyphHeight) + mGlyphSpacing);

		// At this point, the texture is only one glyph high and is probably very wide. For efficiency reasons, we need to make the
		// texture as square as possible. If the texture cannot be made perfectly square, make it taller than it is wide, because
		// the height may decrease in the final layout due to height packing.
		while (texWidth > texHeight)
		{
			texWidth /= 2;
			texHeight *= 2;
		}

		// Calculate the final layout of all of the glyphs in the texture, packing them tightly by first arranging them by height.
		// We assume that the texture width is fixed but that the texture height can be adjusted up or down depending on how much
		// space is actually needed.
		// In most cases, the final texture will end up square or almost square. In some rare cases, however, we can end up with a
		// texture that's more than twice as high as it is wide; when this happens, we double the width and try again.
		do
		{
			if (texHeight > texWidth * 2)
				texWidth *= 2;

			int texX = 0, texY = 0;

			for (GlyphHeightMap::const_iterator j = glyphHeightMap.begin(); j != glyphHeightMap.end(); ++j)
			{
				for (GlyphHeightMap::mapped_type::const_iterator i = j->second.begin(); i != j->second.end(); ++i)
				{
					GlyphInfo& info = *i->second;

					int glyphWidth = (int)ceil(info.width);
					int glyphHeight = (int)ceil(info.height);

					autoWrapGlyphPos(glyphWidth, texWidth, glyphHeight, texX, texY);

					if (glyphWidth > 0)
						texX += mGlyphSpacing + glyphWidth;
				}
			}

			texHeight = Bitwise::firstPO2From(texY + glyphHeightMap.rbegin()->first);
		}
		while (texHeight > texWidth * 2);

		//-------------------------- [2012/5/8 zhongdaohuan] -----------------------------------------//
		// 
		
		if (mAutoTextureSize)	
		{
			mTextureWidth = texWidth;
			mTextureHeight = texHeight;		
		}
		else
		{
			if (0 == mTextureWidth)	
			{
				mTextureWidth = sDefaultTextureBufferWidth;
			}
			if (0 == mTextureHeight)	
			{
				mTextureHeight = sDefaultTextureBufferHeight;
			}

			if (mTextureWidth < texWidth)
			{
				mTextureWidth = texWidth;
			}
			if (mTextureHeight < texHeight)
			{
				mTextureHeight = texHeight;
			}
		}
		//------------------------------------------------------------------------------------------//
		//-------------------------------------------------------------------//
		// Create the texture and render the glyphs onto it.
		//-------------------------------------------------------------------//

		mTexture = RenderManager::getInstance().createTexture(MyGUI::utility::toString((size_t)this, "_TrueTypeFont"));

		mTexture->createManual(mTextureWidth, mTextureHeight, TextureUsage::Static | TextureUsage::Write, Pixel<LAMode>::getFormat());
		mTexturePacker.resetSize(mTextureWidth, mTextureHeight);

		uint8* texBuffer = static_cast<uint8*>(mTexture->lock(TextureUsage::Write));

		if (texBuffer != nullptr)
		{
			// Make the texture background transparent white.
			for (uint8* dest = texBuffer, * endDest = dest + mTextureWidth * mTextureHeight * Pixel<LAMode>::getNumBytes(); dest != endDest; )
				Pixel<LAMode, false, false>::set(dest, charMaskWhite, charMaskBlack);

			renderGlyphs<LAMode, Antialias>(glyphHeightMap, mFTFactory->GetFT_Library(), mFTFace->GetFT_Face(), texBuffer, mTextureWidth, mTextureHeight);

			mTexture->unlock();

			MYGUI_LOG(Info, "ResourceTrueTypeFont: Font '" << getResourceName() << "' using texture size " << mTextureWidth << " x " << mTextureHeight << ".");
			MYGUI_LOG(Info, "ResourceTrueTypeFont: Font '" << getResourceName() << "' using real height " << mDefaultHeight << " pixels.");
		}
		else
		{
			MYGUI_LOG(Error, "ResourceTrueTypeFont: Error locking texture; pointer is nullptr.");
		}

		//FT_Done_Face(face);
		//FT_Done_FreeType(ftLibrary);
		//delete [] fontBuffer;
	}

	//FT_Face ResourceTrueTypeFont::loadFace(const FT_Library& _ftLibrary, uint8*& _fontBuffer)
	FTFace* ResourceTrueTypeFont::loadFace(const FT_Library& _ftLibrary)
	{
		//-------- [2012/5/28 zhongdaohuan] ---------------------
		FTFace* face = FTFaceCache::Find(mSource);
		if (face)
		{
			face->CountAdd();
			return face;
		}	
		// ------------------------------------------------------

		FT_Face result = nullptr;
		uint8* _fontBuffer = nullptr;
		// Load the font file.
		IDataStream* datastream = DataManager::getInstance().getData(mSource);

		if (datastream == nullptr)
			//return result;
			return face;//[2012/5/28 zhongdaohuan]

		size_t fontBufferSize = datastream->size();
		_fontBuffer = new uint8[fontBufferSize];
		datastream->read(_fontBuffer, fontBufferSize);
		delete datastream;

		// Determine how many faces the font contains.
		if (FT_New_Memory_Face(_ftLibrary, _fontBuffer, (FT_Long)fontBufferSize, -1, &result) != 0)
			MYGUI_EXCEPT("ResourceTrueTypeFont: Could not load the font '" << getResourceName() << "'!");

		FT_Long numFaces = result->num_faces;
		FT_Long faceIndex = 0;

		// Load the first face.
		if (FT_New_Memory_Face(_ftLibrary, _fontBuffer, (FT_Long)fontBufferSize, faceIndex, &result) != 0)
			MYGUI_EXCEPT("ResourceTrueTypeFont: Could not load the font '" << getResourceName() << "'!");

		if (result->face_flags & FT_FACE_FLAG_SCALABLE)
		{
			// The font is scalable, so set the font size by first converting the requested size to FreeType's 26.6 fixed-point
			// format.
			
			//[2012/5/28 zhongdaohuan]现在不在加载时指定字符大小了 ------------------------
			//FT_F26Dot6 ftSize = (FT_F26Dot6)(mSize * (1 << 6));

			//if (FT_Set_Char_Size(result, ftSize, 0, mResolution, mResolution) != 0)
			//	MYGUI_EXCEPT("ResourceTrueTypeFont: Could not set the font size for '" << getResourceName() << "'!");


			//[2012/5/28 zhongdaohuan] 注掉------------------------
			//// If no code points have been specified, use the Unicode Basic Multilingual Plane by default.
			//if (mCharMap.empty())
			//	addCodePointRange(0, 0xFFFF);
		}
		else
		{
			// The font isn't scalable, so try to load it as a Windows FNT/FON file.
			FT_WinFNT_HeaderRec fnt;

			// Enumerate all of the faces in the font and select the smallest one that's at least as large as the requested size
			// (after adjusting for resolution). If none of the faces are large enough, use the largest one.
			std::map<float, FT_Long> faceSizes;

			do
			{
				if (FT_Get_WinFNT_Header(result, &fnt) != 0)
					MYGUI_EXCEPT("ResourceTrueTypeFont: Could not load the font '" << getResourceName() << "'!");

				faceSizes.insert(std::make_pair((float)fnt.nominal_point_size * fnt.vertical_resolution / mResolution, faceIndex));

				FT_Done_Face(result);

				if (++faceIndex < numFaces)
					if (FT_New_Memory_Face(_ftLibrary, _fontBuffer, (FT_Long)fontBufferSize, faceIndex, &result) != 0)
						MYGUI_EXCEPT("ResourceTrueTypeFont: Could not load the font '" << getResourceName() << "'!");
			}
			while (faceIndex < numFaces);

			std::map<float, FT_Long>::const_iterator iter = faceSizes.lower_bound(mSize);

			faceIndex = (iter != faceSizes.end()) ? iter->second : faceSizes.rbegin()->second;

			if (FT_New_Memory_Face(_ftLibrary, _fontBuffer, (FT_Long)fontBufferSize, faceIndex, &result) != 0)
				MYGUI_EXCEPT("ResourceTrueTypeFont: Could not load the font '" << getResourceName() << "'!");

			// Select the first bitmap strike available in the selected face. This needs to be done explicitly even though Windows
			// FNT/FON files contain only one bitmap strike per face.

			//[2012/5/28 zhongdaohuan]现在不在加载时指定字符大小了
			//if (FT_Select_Size(result, 0) != 0)		
			//	MYGUI_EXCEPT("ResourceTrueTypeFont: Could not set the font size for '" << getResourceName() << "'!");

			// Windows FNT/FON files do not support Unicode, so restrict the code-point range to either ISO-8859-1 or ASCII,
			// depending on the font's encoding.

			//[2012/5/28 zhongdaohuan] 注掉 ------------------------
			//if (mCharMap.empty())
			//{
			//	// No code points have been specified, so add the printable ASCII range by default.
			//	addCodePointRange(0x20, 0x7E);

			//	// Additionally, if the font's character set is CP-1252, add the range of non-ASCII 8-bit code points that are
			//	// common between CP-1252 and ISO-8859-1; i.e., everything but 0x80 through 0x9F.
			//	if (fnt.charset == FT_WinFNT_ID_CP1252)
			//		addCodePointRange(0xA0, 0xFF);
			//}
			//else
			//{
			//	// Some code points have been specified, so remove anything in the non-printable ASCII range as well as anything
			//	// over 8 bits.
			//	removeCodePointRange(0, 0x1F);
			//	removeCodePointRange(0x100, std::numeric_limits<Char>::max());

			//	// Additionally, remove non-ASCII 8-bit code points (plus ASCII DEL, 0x7F). If the font's character set is CP-1252,
			//	// remove only the code points that differ between CP-1252 and ISO-8859-1; otherwise, remove all of them.
			//	if (fnt.charset == FT_WinFNT_ID_CP1252)
			//		removeCodePointRange(0x7F, 0x9F);
			//	else
			//		removeCodePointRange(0x7F, 0xFF);
			//}
		}
		//return result;//[2012/5/28 zhongdaohuan] 注掉 ------------------------

		return FTFaceCache::BuildFace(result, _fontBuffer, mSource);	// [2012/5/28 zhongdaohuan]

		
	}

	void ResourceTrueTypeFont::autoWrapGlyphPos(int _glyphWidth, int _texWidth, int _lineHeight, int& _texX, int& _texY)
	{
		if (_glyphWidth > 0 && _texX + mGlyphSpacing + _glyphWidth > _texWidth)
		{
			_texX = 0;
			_texY += mGlyphSpacing + _lineHeight;
		}
	}

	GlyphInfo ResourceTrueTypeFont::createFaceGlyphInfo(Char _codePoint, int _fontAscent, FT_GlyphSlot _glyph)
	{
		float bearingX = _glyph->metrics.horiBearingX / 64.0f;

		// The following calculations aren't currently needed but are kept here for future use.
		// float ascent = _glyph->metrics.horiBearingY / 64.0f;
		// float descent = (_glyph->metrics.height / 64.0f) - ascent;

		return GlyphInfo(
			_codePoint,
			_glyph->metrics.width / 64.0f,
			_glyph->metrics.height / 64.0f,
			(_glyph->advance.x / 64.0f) - bearingX,
			bearingX,
			_fontAscent - (_glyph->metrics.horiBearingY / 64.0f) - mOffsetHeight);
	}

	int ResourceTrueTypeFont::createGlyph(FT_UInt _glyphIndex, const GlyphInfo& _glyphInfo, GlyphHeightMap& _glyphHeightMap)
	{
		mCharMap[_glyphInfo.codePoint] = _glyphIndex;
		GlyphInfo& info = mGlyphMap.insert(GlyphMap::value_type(_glyphIndex, _glyphInfo)).first->second;
		_glyphHeightMap[(FT_Pos)_glyphInfo.height].insert(std::make_pair(_glyphIndex, &info));

		int width = (int)ceil(_glyphInfo.width);

		return (width > 0) ? mGlyphSpacing + width : 0;
	}

	int ResourceTrueTypeFont::createFaceGlyph(FT_UInt _glyphIndex, Char _codePoint, int _fontAscent, const FT_Face& _face, GlyphHeightMap& _glyphHeightMap)
	{
		if (mGlyphMap.find(_glyphIndex) == mGlyphMap.end())
		{
			if (FT_Load_Glyph(_face, _glyphIndex, FT_LOAD_DEFAULT) == 0)
				return createGlyph(_glyphIndex, createFaceGlyphInfo(_codePoint, _fontAscent, _face->glyph), _glyphHeightMap);
			else
				MYGUI_LOG(Warning, "ResourceTrueTypeFont: Cannot load glyph " << _glyphIndex << " for character " << _codePoint << " in font '" << getResourceName() << "'.");
		}
		else
		{
			mCharMap[_codePoint] = _glyphIndex;
		}

		return 0;
	}

	template<bool LAMode, bool Antialias>
	void ResourceTrueTypeFont::renderGlyphs(const GlyphHeightMap& _glyphHeightMap, const FT_Library& _ftLibrary, const FT_Face& _face, uint8* _texBuffer, int _texWidth, int _texHeight)
	{
		FT_Bitmap ftBitmap;
		FT_Bitmap_New(&ftBitmap);

		int texX = 0, texY = 0;

		for (GlyphHeightMap::const_iterator j = _glyphHeightMap.begin(); j != _glyphHeightMap.end(); ++j)
		{
			for (GlyphHeightMap::mapped_type::const_iterator i = j->second.begin(); i != j->second.end(); ++i)
			{
				GlyphInfo& info = *i->second;

				switch (info.codePoint)
				{
				case FontCodeType::Selected:
				case FontCodeType::SelectedBack:
				{
					renderGlyph<LAMode, false, false>(info, charMaskWhite, charMaskBlack, charMask.find(info.codePoint)->second, j->first, _texBuffer, _texWidth, _texHeight, texX, texY);

					// Manually adjust the glyph's width to zero. This prevents artifacts from appearing at the seams when
					// rendering multi-character selections.
					//GlyphInfo* glyphInfo = getGlyphInfo(info.codePoint);
					
					GlyphInfo* glyphInfo = nullptr;
					getGlyphInfo(info.codePoint, &glyphInfo); //[2012/5/8 zhongdaohuan]

					glyphInfo->width = 0.0f;
					glyphInfo->uvRect.right = glyphInfo->uvRect.left;
				}
				break;

				case FontCodeType::Cursor:
				case FontCodeType::Tab:
					renderGlyph<LAMode, false, false>(info, charMaskWhite, charMaskBlack, charMask.find(info.codePoint)->second, j->first, _texBuffer, _texWidth, _texHeight, texX, texY);
					break;

				default:
					if (FT_Load_Glyph(_face, i->first, FT_LOAD_RENDER) == 0)
					{
						if (_face->glyph->bitmap.buffer != nullptr)
						{
							uint8* glyphBuffer = nullptr;

							switch (_face->glyph->bitmap.pixel_mode)
							{
							case FT_PIXEL_MODE_GRAY:
								glyphBuffer = _face->glyph->bitmap.buffer;
								break;

							case FT_PIXEL_MODE_MONO:
								// Convert the monochrome bitmap to 8-bit before rendering it.
								if (FT_Bitmap_Convert(_ftLibrary, &_face->glyph->bitmap, &ftBitmap, 1) == 0)
								{
									// Go through the bitmap and convert all of the nonzero values to 0xFF (white).
									for (uint8* p = ftBitmap.buffer, * endP = p + ftBitmap.width * ftBitmap.rows; p != endP; ++p)
										*p ^= -*p ^ *p;

									glyphBuffer = ftBitmap.buffer;
								}
								break;
							}

							if (glyphBuffer != nullptr)
								renderGlyph<LAMode, true, Antialias>(info, charMaskWhite, charMaskWhite, charMaskWhite, j->first, _texBuffer, _texWidth, _texHeight, texX, texY, glyphBuffer);
						}
					}
					else
					{
						MYGUI_LOG(Warning, "ResourceTrueTypeFont: Cannot render glyph " << i->first << " for character " << info.codePoint << " in font '" << getResourceName() << "'.");
					}
					break;
				}
			}
		}

		FT_Bitmap_Done(_ftLibrary, &ftBitmap);
	}

	template<bool LAMode, bool UseBuffer, bool Antialias>
	void ResourceTrueTypeFont::renderGlyph(GlyphInfo& _info, uint8 _luminance0, uint8 _luminance1, uint8 _alpha, int _lineHeight, uint8* _texBuffer, int _texWidth, int _texHeight, int& _texX, int& _texY, uint8* _glyphBuffer)
	{
		//在新的逻辑中，int& _texX, int& _texY,已沦为一个局部变量。只在本函数内部才有意义。
		////[2012/5/8 zhongdaohuan]用mTexturePacker来管理后，_texX和_texY其实都不用传进来了。
		int width = (int)ceil(_info.width);
		int height = (int)ceil(_info.height);

		//autoWrapGlyphPos(width, _texWidth, _lineHeight, _texX, _texY);

		int alloc_width = width + mGlyphSpacing;
		int alloc_height = _lineHeight + mGlyphSpacing;

		if(!mTexturePacker.allocate(alloc_width, alloc_height, _texX, _texY))
		{
			assert(0);
		}

		uint8* dest = _texBuffer + (_texY * _texWidth + _texX) * Pixel<LAMode>::getNumBytes();

		// Calculate how much to advance the destination pointer after each row to get to the start of the next row.
		ptrdiff_t destNextRow = (_texWidth - width) * Pixel<LAMode>::getNumBytes();

		for (int j = height; j > 0; --j)
		{
			int i;
			for (i = width; i > 1; i -= 2)
			{
				Pixel<LAMode, UseBuffer, Antialias>::set(dest, _luminance0, _alpha, _glyphBuffer);
				Pixel<LAMode, UseBuffer, Antialias>::set(dest, _luminance1, _alpha, _glyphBuffer);
			}

			if (i > 0)
				Pixel<LAMode, UseBuffer, Antialias>::set(dest, _luminance0, _alpha, _glyphBuffer);

			dest += destNextRow;
		}

		// Calculate and store the glyph's UV coordinates within the texture.
		_info.uvRect.left = (float)_texX / _texWidth; // u1
		_info.uvRect.top = (float)_texY / _texHeight; // v1
		_info.uvRect.right = (float)(_texX + _info.width) / _texWidth; // u2
		_info.uvRect.bottom = (float)(_texY + _info.height) / _texHeight; // v2

		//if (width > 0)						
		//	_texX += mGlyphSpacing + width;	//[2012/5/8 zhongdaohuan]用mTexturePacker来管理后，_texX和_texY其实都不用传进来了。
	}

#endif // MYGUI_USE_FREETYPE

} // namespace MyGUI
