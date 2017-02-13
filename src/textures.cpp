/**
 * This file is part of Tales of Berseria "Fix".
 *
 * Tales of Berseria "Fix" is free software : you can redistribute it
 * and/or modify it under the terms of the GNU General Public License
 * as published by The Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * Tales of Berseria "Fix" is distributed in the hope that it will be
 * useful,
 *
 * But WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Tales of Berseria "Fix".
 *
 *   If not, see <http://www.gnu.org/licenses/>.
 *
**/

#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX

#include <d3d9.h>

#include "render.h"
#include "textures.h"
#include "config.h"
#include "framerate.h"
#include "hook.h"
#include "log.h"
#include <process.h>

#include <cstdint>
#include <algorithm>

#include "command.h"

#include <lzma/7z.h>
#include <lzma/7zAlloc.h>
#include <lzma/7zBuf.h>
#include <lzma/7zCrc.h>
#include <lzma/7zFile.h>
#include <lzma/7zVersion.h>

#include <atlbase.h>
#include <memory>
#include <ctime>

#define TBFIX_TEXTURE_DIR L"TBFix_Res"
#define TBFIX_TEXTURE_EXT L".dds"


typedef HRESULT (STDMETHODCALLTYPE *StretchRect_pfn)
  (      IDirect3DDevice9    *This,
         IDirect3DSurface9   *pSourceSurface,
   const RECT                *pSourceRect,
         IDirect3DSurface9   *pDestSurface,
   const RECT                *pDestRect,
         D3DTEXTUREFILTERTYPE Filter
  );

typedef HRESULT (STDMETHODCALLTYPE *SetRenderState_pfn)
(
  IDirect3DDevice9*  This,
  D3DRENDERSTATETYPE State,
  DWORD              Value
);

typedef HRESULT (WINAPI *D3DXLoadSurfaceFromSurface_pfn)
(
  _In_       LPDIRECT3DSURFACE9  pDestSurface,
  _In_ const PALETTEENTRY       *pDestPalette,
  _In_ const RECT               *pDestRect,
  _In_       LPDIRECT3DSURFACE9  pSrcSurface,
  _In_ const PALETTEENTRY       *pSrcPalette,
  _In_ const RECT               *pSrcRect,
  _In_       DWORD               Filter,
  _In_       D3DCOLOR            ColorKey
);


static D3DXSaveTextureToFile_pfn               D3DXSaveTextureToFile                        = nullptr;
static D3DXCreateTextureFromFileInMemoryEx_pfn D3DXCreateTextureFromFileInMemoryEx_Original = nullptr;

static BeginScene_pfn                          D3D9BeginScene                               = nullptr;
static EndScene_pfn                            D3D9EndScene                                 = nullptr;
       SetRenderState_pfn                      D3D9SetRenderState                           = nullptr;

static StretchRect_pfn                         D3D9StretchRect                              = nullptr;
static CreateTexture_pfn                       D3D9CreateTexture                            = nullptr;
static CreateRenderTarget_pfn                  D3D9CreateRenderTarget                       = nullptr;
static CreateDepthStencilSurface_pfn           D3D9CreateDepthStencilSurface                = nullptr;

static SetTexture_pfn                          D3D9SetTexture                               = nullptr;
static SetRenderTarget_pfn                     D3D9SetRenderTarget                          = nullptr;
static SetDepthStencilSurface_pfn              D3D9SetDepthStencilSurface                   = nullptr;

extern uint32_t
TBF_MakeShadowBitShift (uint32_t dim);

tbf::RenderFix::TextureManager
  tbf::RenderFix::tex_mgr;

iSK_Logger* tex_log = nullptr;

#include <set>
#include <queue>


// Cleanup
std::queue         <std::wstring>        screenshots_to_delete;


class TBF_AutoCritSection
{
public:
  TBF_AutoCritSection (CRITICAL_SECTION* crit_sec) : cs_ (crit_sec) {
    EnterCriticalSection (cs_);
  };

  ~TBF_AutoCritSection (void) {
    LeaveCriticalSection (cs_);
  }

private:
  CRITICAL_SECTION* cs_;
};


template <typename _T>
class TBF_HashSet
{
public:
  TBF_HashSet (void) {
    InitializeCriticalSection (&cs_);
  }

  ~TBF_HashSet (void) {
    DeleteCriticalSection (&cs_);
  }

  void insert (_T item)
  {
    TBF_AutoCritSection auto_crit (&cs_);

    container_.insert (item);
  }

  void erase (_T item)
  {
    TBF_AutoCritSection auto_crit (&cs_);

    container_.erase (item);
  }

  bool contains (_T item)
  {
    TBF_AutoCritSection auto_crit (&cs_);

    return container_.count (item) != 0;
  }

  bool empty (void)
  {
    TBF_AutoCritSection auto_crit (&cs_);

    return container_.empty ();
  }

protected:
private:
  std::unordered_set <_T> container_;
  CRITICAL_SECTION        cs_;
};


TBF_HashSet <IDirect3DSurface9 *> outstanding_screenshots; // Not excellent screenshots, but screenhots
                                                           //   that aren't finished yet and we can't reset
                                                           //     the D3D9 device because of.

tbf::RenderFix::pad_buttons_t   tbf::RenderFix::pad_buttons;

// D3DXSaveSurfaceToFile issues a StretchRect, but we don't want to log that...
bool dumping          = false;
bool __remap_textures = true;
bool __need_purge     = false;
bool __log_used       = false;
bool __show_cache     = false;

bool pending_loads            (void);
void TBFix_LoadQueuedTextures (void);

#include <map>
#include <set>
#include <queue>
#include <vector>
#include <unordered_set>
#include <unordered_map>

// All of the enumerated textures in TBFix_Textures/inject/...
std::unordered_map <uint32_t, tbf_tex_record_s> injectable_textures;
std::vector        <std::wstring>               archives;
std::unordered_set <uint32_t>                   dumped_textures;

std::vector <std::wstring>
TBF_GetTextureArchives (void)
{
  return archives;
}

std::vector < std::pair < uint32_t, tbf_tex_record_s > >
TBF_GetInjectableTextures (void)
{
  std::vector < std::pair < uint32_t, tbf_tex_record_s > > textures;

  for ( auto it : injectable_textures ) 
  {
    textures.push_back (std::make_pair (it.first, it.second));
  }

  return textures;
}

tbf_tex_record_s*
TBF_GetInjectableTexture (uint32_t checksum)
{
  if (injectable_textures.count (checksum))
    return &injectable_textures [checksum];

  return nullptr;
}

// The set of textures used during the last frame
std::vector        <uint32_t>                   textures_last_frame;
std::unordered_set <uint32_t>                   textures_used;
std::unordered_set <uint32_t>                   non_power_of_two_textures;

// Textures that we will not allow injection for
//   (primarily to speed things up, but also for EULA-related reasons).
std::unordered_set <uint32_t>                   inject_blacklist;

std::wstring
SK_D3D9_UsageToStr (DWORD dwUsage)
{
  std::wstring usage;

  if (dwUsage & D3DUSAGE_RENDERTARGET)
    usage += L"RenderTarget ";

  if (dwUsage & D3DUSAGE_DEPTHSTENCIL)
    usage += L"Depth/Stencil ";

  if (dwUsage & D3DUSAGE_DYNAMIC)
    usage += L"Dynamic";

  if (usage.empty ())
    usage = L"Don't Care";

  return usage;
}

INT
__stdcall
SK_D3D9_BytesPerPixel (D3DFORMAT Format)
{
  switch (Format)
  {
    case D3DFMT_UNKNOWN:
      return 0;

    case D3DFMT_R8G8B8:       return 3;
    case D3DFMT_A8R8G8B8:     return 4;
    case D3DFMT_X8R8G8B8:     return 4;
    case D3DFMT_R5G6B5:       return 2;
    case D3DFMT_X1R5G5B5:     return 2;
    case D3DFMT_A1R5G5B5:     return 2;
    case D3DFMT_A4R4G4B4:     return 2;
    case D3DFMT_R3G3B2:       return 8;
    case D3DFMT_A8:           return 1;
    case D3DFMT_A8R3G3B2:     return 2;
    case D3DFMT_X4R4G4B4:     return 2;
    case D3DFMT_A2B10G10R10:  return 4;
    case D3DFMT_A8B8G8R8:     return 4;
    case D3DFMT_X8B8G8R8:     return 4;
    case D3DFMT_G16R16:       return 4;
    case D3DFMT_A2R10G10B10:  return 4;
    case D3DFMT_A16B16G16R16: return 8;
    case D3DFMT_A8P8:         return 2;
    case D3DFMT_P8:           return 1;
    case D3DFMT_L8:           return 1;
    case D3DFMT_A8L8:         return 2;
    case D3DFMT_A4L4:         return 1;
    case D3DFMT_V8U8:         return 2;
    case D3DFMT_L6V5U5:       return 2;
    case D3DFMT_X8L8V8U8:     return 4;
    case D3DFMT_Q8W8V8U8:     return 4;
    case D3DFMT_V16U16:       return 4;
    case D3DFMT_A2W10V10U10:  return 4;

#if 0
    case D3DFMT_UYVY                 :
      return std::wstring (L"FourCC 'UYVY'");
    case D3DFMT_R8G8_B8G8            :
      return std::wstring (L"FourCC 'RGBG'");
    case D3DFMT_YUY2                 :
      return std::wstring (L"FourCC 'YUY2'");
    case D3DFMT_G8R8_G8B8            :
      return std::wstring (L"FourCC 'GRGB'");
#endif
    case D3DFMT_DXT1:          return -1;
    case D3DFMT_DXT2:          return -2;
    case D3DFMT_DXT3:          return -2;
    case D3DFMT_DXT4:          return -1;
    case D3DFMT_DXT5:          return -2;
                               
    case D3DFMT_D16_LOCKABLE:  return  2;
    case D3DFMT_D32:           return  4;
    case D3DFMT_D15S1:         return  2;
    case D3DFMT_D24S8:         return  4;
    case D3DFMT_D24X8:         return  4;
    case D3DFMT_D24X4S4:       return  4;
    case D3DFMT_D16:           return  2;
    case D3DFMT_D32F_LOCKABLE: return  4;
    case D3DFMT_D24FS8:        return  4;

/* D3D9Ex only -- */
#if !defined(D3D_DISABLE_9EX)

    /* Z-Stencil formats valid for CPU access */
    case D3DFMT_D32_LOCKABLE:  return 4;
    case D3DFMT_S8_LOCKABLE:   return 1;
#endif // !D3D_DISABLE_9EX



    case D3DFMT_L16:           return 2;

#if 0
    case D3DFMT_VERTEXDATA           :
      return std::wstring (L"VERTEXDATA") +
                (include_ordinal ? L" (100)" : L"");
#endif
    case D3DFMT_INDEX16:       return 2;
    case D3DFMT_INDEX32:       return 4;

    case D3DFMT_Q16W16V16U16:  return 8;

#if 0
    case D3DFMT_MULTI2_ARGB8         :
      return std::wstring (L"FourCC 'MET1'");
#endif

    // Floating point surface formats

    // s10e5 formats (16-bits per channel)
    case D3DFMT_R16F:          return 2;
    case D3DFMT_G16R16F:       return 4;
    case D3DFMT_A16B16G16R16F: return 8;

    // IEEE s23e8 formats (32-bits per channel)
    case D3DFMT_R32F:          return 4;
    case D3DFMT_G32R32F:       return 8;
    case D3DFMT_A32B32G32R32F: return 16;

#if 0
    case D3DFMT_CxV8U8               :
      return std::wstring (L"CxV8U8") +
                (include_ordinal ? L" (117)" : L"");
#endif

/* D3D9Ex only -- */
#if !defined(D3D_DISABLE_9EX)

    // Monochrome 1 bit per pixel format
    case D3DFMT_A1:            return -8;

#if 0
    // 2.8 biased fixed point
    case D3DFMT_A2B10G10R10_XR_BIAS  :
      return std::wstring (L"A2B10G10R10_XR_BIAS") +
                (include_ordinal ? L" (119)" : L"");
#endif


#if 0
    // Binary format indicating that the data has no inherent type
    case D3DFMT_BINARYBUFFER         :
      return std::wstring (L"BINARYBUFFER") +
                (include_ordinal ? L" (199)" : L"");
#endif

#endif // !D3D_DISABLE_9EX
/* -- D3D9Ex only */
  }

  return 0;
}

std::wstring
SK_D3D9_FormatToStr (D3DFORMAT Format, bool include_ordinal = true)
{
  switch (Format)
  {
    case D3DFMT_UNKNOWN:
      return std::wstring (L"Unknown") + (include_ordinal ? L" (0)" :
                                                            L"");

    case D3DFMT_R8G8B8:
      return std::wstring (L"R8G8B8")   +
                (include_ordinal ? L" (20)" : L"");
    case D3DFMT_A8R8G8B8:
      return std::wstring (L"A8R8G8B8") +
                (include_ordinal ? L" (21)" : L"");
    case D3DFMT_X8R8G8B8:
      return std::wstring (L"X8R8G8B8") +
                (include_ordinal ? L" (22)" : L"");
    case D3DFMT_R5G6B5               :
      return std::wstring (L"R5G6B5")   +
                (include_ordinal ? L" (23)" : L"");
    case D3DFMT_X1R5G5B5             :
      return std::wstring (L"X1R5G5B5") +
                (include_ordinal ? L" (24)" : L"");
    case D3DFMT_A1R5G5B5             :
      return std::wstring (L"A1R5G5B5") +
                (include_ordinal ? L" (25)" : L"");
    case D3DFMT_A4R4G4B4             :
      return std::wstring (L"A4R4G4B4") +
                (include_ordinal ? L" (26)" : L"");
    case D3DFMT_R3G3B2               :
      return std::wstring (L"R3G3B2")   +
                (include_ordinal ? L" (27)" : L"");
    case D3DFMT_A8                   :
      return std::wstring (L"A8")       +
                (include_ordinal ? L" (28)" : L"");
    case D3DFMT_A8R3G3B2             :
      return std::wstring (L"A8R3G3B2") +
                (include_ordinal ? L" (29)" : L"");
    case D3DFMT_X4R4G4B4             :
      return std::wstring (L"X4R4G4B4") +
                (include_ordinal ? L" (30)" : L"");
    case D3DFMT_A2B10G10R10          :
      return std::wstring (L"A2B10G10R10") +
                (include_ordinal ? L" (31)" : L"");
    case D3DFMT_A8B8G8R8             :
      return std::wstring (L"A8B8G8R8") +
                (include_ordinal ? L" (32)" : L"");
    case D3DFMT_X8B8G8R8             :
      return std::wstring (L"X8B8G8R8") +
                (include_ordinal ? L" (33)" : L"");
    case D3DFMT_G16R16               :
      return std::wstring (L"G16R16") +
                (include_ordinal ? L" (34)" : L"");
    case D3DFMT_A2R10G10B10          :
      return std::wstring (L"A2R10G10B10") +
                (include_ordinal ? L" (35)" : L"");
    case D3DFMT_A16B16G16R16         :
      return std::wstring (L"A16B16G16R16") +
                (include_ordinal ? L" (36)" : L"");

    case D3DFMT_A8P8                 :
      return std::wstring (L"A8P8") +
                (include_ordinal ? L" (40)" : L"");
    case D3DFMT_P8                   :
      return std::wstring (L"P8") +
                (include_ordinal ? L" (41)" : L"");

    case D3DFMT_L8                   :
      return std::wstring (L"L8") +
                (include_ordinal ? L" (50)" : L"");
    case D3DFMT_A8L8                 :
      return std::wstring (L"A8L8") +
                (include_ordinal ? L" (51)" : L"");
    case D3DFMT_A4L4                 :
      return std::wstring (L"A4L4") +
                (include_ordinal ? L" (52)" : L"");

    case D3DFMT_V8U8                 :
      return std::wstring (L"V8U8") +
                (include_ordinal ? L" (60)" : L"");
    case D3DFMT_L6V5U5               :
      return std::wstring (L"L6V5U5") +
                (include_ordinal ? L" (61)" : L"");
    case D3DFMT_X8L8V8U8             :
      return std::wstring (L"X8L8V8U8") +
                (include_ordinal ? L" (62)" : L"");
    case D3DFMT_Q8W8V8U8             :
      return std::wstring (L"Q8W8V8U8") +
                (include_ordinal ? L" (63)" : L"");
    case D3DFMT_V16U16               :
      return std::wstring (L"V16U16") +
                (include_ordinal ? L" (64)" : L"");
    case D3DFMT_A2W10V10U10          :
      return std::wstring (L"A2W10V10U10") +
                (include_ordinal ? L" (67)" : L"");

    case D3DFMT_UYVY                 :
      return std::wstring (L"FourCC 'UYVY'");
    case D3DFMT_R8G8_B8G8            :
      return std::wstring (L"FourCC 'RGBG'");
    case D3DFMT_YUY2                 :
      return std::wstring (L"FourCC 'YUY2'");
    case D3DFMT_G8R8_G8B8            :
      return std::wstring (L"FourCC 'GRGB'");
    case D3DFMT_DXT1                 :
      return std::wstring (L"DXT1");
    case D3DFMT_DXT2                 :
      return std::wstring (L"DXT2");
    case D3DFMT_DXT3                 :
      return std::wstring (L"DXT3");
    case D3DFMT_DXT4                 :
      return std::wstring (L"DXT4");
    case D3DFMT_DXT5                 :
      return std::wstring (L"DXT5");

    case D3DFMT_D16_LOCKABLE         :
      return std::wstring (L"D16_LOCKABLE") +
                (include_ordinal ? L" (70)" : L"");
    case D3DFMT_D32                  :
      return std::wstring (L"D32") +
                (include_ordinal ? L" (71)" : L"");
    case D3DFMT_D15S1                :
      return std::wstring (L"D15S1") +
                (include_ordinal ? L" (73)" : L"");
    case D3DFMT_D24S8                :
      return std::wstring (L"D24S8") +
                (include_ordinal ? L" (75)" : L"");
    case D3DFMT_D24X8                :
      return std::wstring (L"D24X8") +
                (include_ordinal ? L" (77)" : L"");
    case D3DFMT_D24X4S4              :
      return std::wstring (L"D24X4S4") +
                (include_ordinal ? L" (79)" : L"");
    case D3DFMT_D16                  :
      return std::wstring (L"D16") +
                (include_ordinal ? L" (80)" : L"");

    case D3DFMT_D32F_LOCKABLE        :
      return std::wstring (L"D32F_LOCKABLE") +
                (include_ordinal ? L" (82)" : L"");
    case D3DFMT_D24FS8               :
      return std::wstring (L"D24FS8") +
                (include_ordinal ? L" (83)" : L"");

/* D3D9Ex only -- */
#if !defined(D3D_DISABLE_9EX)

    /* Z-Stencil formats valid for CPU access */
    case D3DFMT_D32_LOCKABLE         :
      return std::wstring (L"D32_LOCKABLE") +
                (include_ordinal ? L" (84)" : L"");
    case D3DFMT_S8_LOCKABLE          :
      return std::wstring (L"S8_LOCKABLE") +
                (include_ordinal ? L" (85)" : L"");

#endif // !D3D_DISABLE_9EX



    case D3DFMT_L16                  :
      return std::wstring (L"L16") +
                (include_ordinal ? L" (81)" : L"");

    case D3DFMT_VERTEXDATA           :
      return std::wstring (L"VERTEXDATA") +
                (include_ordinal ? L" (100)" : L"");
    case D3DFMT_INDEX16              :
      return std::wstring (L"INDEX16") +
                (include_ordinal ? L" (101)" : L"");
    case D3DFMT_INDEX32              :
      return std::wstring (L"INDEX32") +
                (include_ordinal ? L" (102)" : L"");

    case D3DFMT_Q16W16V16U16         :
      return std::wstring (L"Q16W16V16U16") +
                (include_ordinal ? L" (110)" : L"");

    case D3DFMT_MULTI2_ARGB8         :
      return std::wstring (L"FourCC 'MET1'");

    // Floating point surface formats

    // s10e5 formats (16-bits per channel)
    case D3DFMT_R16F                 :
      return std::wstring (L"R16F") +
                (include_ordinal ? L" (111)" : L"");
    case D3DFMT_G16R16F              :
      return std::wstring (L"G16R16F") +
                (include_ordinal ? L" (112)" : L"");
    case D3DFMT_A16B16G16R16F        :
      return std::wstring (L"A16B16G16R16F") +
               (include_ordinal ? L" (113)" : L"");

    // IEEE s23e8 formats (32-bits per channel)
    case D3DFMT_R32F                 :
      return std::wstring (L"R32F") + 
                (include_ordinal ? L" (114)" : L"");
    case D3DFMT_G32R32F              :
      return std::wstring (L"G32R32F") +
                (include_ordinal ? L" (115)" : L"");
    case D3DFMT_A32B32G32R32F        :
      return std::wstring (L"A32B32G32R32F") +
                (include_ordinal ? L" (116)" : L"");

    case D3DFMT_CxV8U8               :
      return std::wstring (L"CxV8U8") +
                (include_ordinal ? L" (117)" : L"");

/* D3D9Ex only -- */
#if !defined(D3D_DISABLE_9EX)

    // Monochrome 1 bit per pixel format
    case D3DFMT_A1                   :
      return std::wstring (L"A1") +
                (include_ordinal ? L" (118)" : L"");

    // 2.8 biased fixed point
    case D3DFMT_A2B10G10R10_XR_BIAS  :
      return std::wstring (L"A2B10G10R10_XR_BIAS") +
                (include_ordinal ? L" (119)" : L"");


    // Binary format indicating that the data has no inherent type
    case D3DFMT_BINARYBUFFER         :
      return std::wstring (L"BINARYBUFFER") +
                (include_ordinal ? L" (199)" : L"");

#endif // !D3D_DISABLE_9EX
/* -- D3D9Ex only */
  }

  return std::wstring (L"UNKNOWN?!");
}

const wchar_t*
SK_D3D9_PoolToStr (D3DPOOL pool)
{
  switch (pool)
  {
    case D3DPOOL_DEFAULT:
      return L"    Default   (0)";
    case D3DPOOL_MANAGED:
      return L"    Managed   (1)";
    case D3DPOOL_SYSTEMMEM:
      return L"System Memory (2)";
    case D3DPOOL_SCRATCH:
      return L"   Scratch    (3)";
    default:
      return L"   UNKNOWN?!     ";
  }
}

COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9SetRenderState_Detour (IDirect3DDevice9*  This,
                           D3DRENDERSTATETYPE State,
                           DWORD              Value)
{
  // Ignore anything that's not the primary render device.
  if (This != tbf::RenderFix::pDevice) {
    dll_log->Log (L"[Render Fix] >> WARNING: SetRenderState came from unknown IDirect3DDevice9! << ");

    return D3D9SetRenderState (This, State, Value);
  }

#if 0
  if (tbf::RenderFix::tracer.log) {
    dll_log->Log ( L"[FrameTrace] SetRenderState  - State: %24s, Value: %lu",
                     SK_D3D9_RenderStateToStr (State),
                       Value );
  }
#endif

  return D3D9SetRenderState (This, State, Value);
}

COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9CreateRenderTarget_Detour (IDirect3DDevice9     *This,
                               UINT                  Width,
                               UINT                  Height,
                               D3DFORMAT             Format,
                               D3DMULTISAMPLE_TYPE   MultiSample,
                               DWORD                 MultisampleQuality,
                               BOOL                  Lockable,
                               IDirect3DSurface9   **ppSurface,
                               HANDLE               *pSharedHandle)
{
  tex_log->Log (L"[Unexpected][!] IDirect3DDevice9::CreateRenderTarget (%lu, %lu, "
                       L"%lu, %lu, %lu, %lu, %ph, %ph)",
                  Width, Height, Format, MultiSample, MultisampleQuality,
                  Lockable, ppSurface, pSharedHandle);

  return D3D9CreateRenderTarget (This, Width, Height, Format,
                                          MultiSample, MultisampleQuality,
                                          Lockable, ppSurface, pSharedHandle);
}

COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9CreateDepthStencilSurface_Detour (IDirect3DDevice9     *This,
                                      UINT                  Width,
                                      UINT                  Height,
                                      D3DFORMAT             Format,
                                      D3DMULTISAMPLE_TYPE   MultiSample,
                                      DWORD                 MultisampleQuality,
                                      BOOL                  Discard,
                                      IDirect3DSurface9   **ppSurface,
                                      HANDLE               *pSharedHandle)
{
  tex_log->Log (L"[Unexpected][!] IDirect3DDevice9::CreateDepthStencilSurface (%lu, %lu, "
                       L"%lu, %lu, %lu, %lu, %ph, %ph)",
                  Width, Height, Format, MultiSample, MultisampleQuality,
                  Discard, ppSurface, pSharedHandle);

  return D3D9CreateDepthStencilSurface (This, Width, Height, Format,
                                                 MultiSample, MultisampleQuality,
                                                 Discard, ppSurface, pSharedHandle);
}

int
tbf::RenderFix::TextureManager::numInjectedTextures (void)
{
  return injected_count;
}

int64_t
tbf::RenderFix::TextureManager::cacheSizeInjected (void)
{
  return injected_size;
}

int64_t
tbf::RenderFix::TextureManager::cacheSizeBasic (void)
{
  return basic_size;
}

int64_t
tbf::RenderFix::TextureManager::cacheSizeTotal (void)
{
  return cacheSizeBasic () + cacheSizeInjected ();
}

bool
tbf::RenderFix::TextureManager::isRenderTarget (IDirect3DBaseTexture9* pTex)
{
  return known.render_targets.count (pTex) != 0;
}

void
tbf::RenderFix::TextureManager::trackRenderTarget (IDirect3DBaseTexture9* pTex)
{
  known.render_targets.insert (pTex);
}

void
tbf::RenderFix::TextureManager::applyTexture (IDirect3DBaseTexture9* pTex)
{
  if (known.render_targets.count (pTex) != 0)
    used.render_targets.insert (pTex);
}

bool
tbf::RenderFix::TextureManager::isUsedRenderTarget (IDirect3DBaseTexture9* pTex)
{
  return used.render_targets.count (pTex) != 0;
}

void
tbf::RenderFix::TextureManager::resetUsedTextures (void)
{
  used.render_targets.clear ();
}

std::vector <IDirect3DBaseTexture9 *>
tbf::RenderFix::TextureManager::getUsedRenderTargets (void)
{
  return std::vector <IDirect3DBaseTexture9 *> (used.render_targets.begin (), used.render_targets.end ());
}

COM_DECLSPEC_NOTHROW
__declspec (noinline)
HRESULT
STDMETHODCALLTYPE
D3D9StretchRect_Detour (      IDirect3DDevice9    *This,
                              IDirect3DSurface9   *pSourceSurface,
                        const RECT                *pSourceRect,
                              IDirect3DSurface9   *pDestSurface,
                        const RECT                *pDestRect,
                              D3DTEXTUREFILTERTYPE Filter )
{
  dumping = false;

  return D3D9StretchRect (This, pSourceSurface, pSourceRect,
                                         pDestSurface,   pDestRect,
                                         Filter);
}


std::set <UINT> tbf::RenderFix::active_samplers;
extern IDirect3DTexture9* pFontTex;

COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9SetDepthStencilSurface_Detour (
                  _In_ IDirect3DDevice9  *This,
                  _In_ IDirect3DSurface9 *pNewZStencil
)
{
  // Ignore anything that's not the primary render device.
  if (This != tbf::RenderFix::pDevice) {
    return D3D9SetDepthStencilSurface (This, pNewZStencil);
  }

  return D3D9SetDepthStencilSurface (This, pNewZStencil);
}


uint32_t debug_tex_id = 0UL;
uint32_t current_tex  = 0ui32;

COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9SetTexture_Detour (
                  _In_ IDirect3DDevice9      *This,
                  _In_ DWORD                  Sampler,
                  _In_ IDirect3DBaseTexture9 *pTexture
)
{
  // Ignore anything that's not the primary render device.
  if (This != tbf::RenderFix::pDevice) {
    return D3D9SetTexture (This, Sampler, pTexture);
  }

  //if (tbf::RenderFix::tracer.log) {
    //dll_log->Log ( L"[FrameTrace] SetTexture      - Sampler: %lu, pTexture: %ph",
                     //Sampler, pTexture );
  //}

  extern uint32_t vs_checksum;
  extern uint32_t ps_checksum;

  tbf::RenderFix::tex_mgr.applyTexture (pTexture);
  tbf::RenderFix::tracked_rt.active  = (pTexture == tbf::RenderFix::tracked_rt.tracking_tex);

  if (tbf::RenderFix::tracked_rt.active)
  {
    tbf::RenderFix::tracked_rt.vertex_shaders.insert (vs_checksum);
    tbf::RenderFix::tracked_rt.pixel_shaders.insert  (ps_checksum);
  }


  if (tbf::RenderFix::tex_mgr.wantsScreenshot ())
  {
    if ( Sampler == 1 && vs_checksum == 0x1a97b826 &&
                         ps_checksum == 0x46618c0a &&
         tbf::RenderFix::tex_mgr.isRenderTarget (pTexture) )
    {
      D3DSURFACE_DESC             surf_desc;
      CComPtr <IDirect3DSurface9> pSurf  = nullptr;

      if (SUCCEEDED (This->GetRenderTarget (0, &pSurf)))
      {
        pSurf->GetDesc (&surf_desc);

        if ( surf_desc.Width  == tbf::RenderFix::width  &&
             surf_desc.Height == tbf::RenderFix::height )
        {
          tbf::RenderFix::tex_mgr.takeScreenshot (pSurf);
        }
      }
    }
  }


  void* dontcare;
  if ( pTexture != nullptr &&
       pTexture->QueryInterface (IID_SKTextureD3D9, &dontcare) == S_OK )
  {
    ISKTextureD3D9* pSKTex =
      (ISKTextureD3D9 *)pTexture;

    current_tex = pSKTex->tex_crc32;

    textures_used.insert (pSKTex->tex_crc32);

    QueryPerformanceCounter (&pSKTex->last_used);

    //
    // This is how blocking is implemented -- only do it when a texture that needs
    //                                          this feature is being applied.
    //
    while ( __remap_textures && pSKTex->must_block &&
                                pSKTex->pTexOverride == nullptr )
    {
      TBFix_LoadQueuedTextures ();
    }

    if (__remap_textures && pSKTex->pTexOverride != nullptr)
      pTexture = pSKTex->pTexOverride;
    else
      pTexture = pSKTex->pTex;

    if (pSKTex->tex_crc32 == (uint32_t)debug_tex_id && config.textures.highlight_debug_tex)
      pTexture = nullptr;

    if (pTexture != nullptr)
    {
      //
      // Fix UI Blurring and Artifacts on Certain Textures
      //
      if (config.textures.clamp_npot_coords)
      {
        if (non_power_of_two_textures.count (pSKTex->tex_crc32))
        {
          This->SetSamplerState ( Sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
          This->SetSamplerState ( Sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
          This->SetSamplerState ( Sampler, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP );
        }
      }
    }

#if 0
  if (pTexture != nullptr) tsf::RenderFix::active_samplers.insert (Sampler);
  else                     tsf::RenderFix::active_samplers.erase  (Sampler);
#endif
  }

  if (pTexture == nullptr)
    return S_OK;

  else
    return D3D9SetTexture(This, Sampler, pTexture);
}

D3DXSaveSurfaceToFile_pfn D3DXSaveSurfaceToFileW = nullptr;
IDirect3DSurface9*        pOld                   = nullptr;

COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9CreateTexture_Detour (IDirect3DDevice9   *This,
                          UINT                Width,
                          UINT                Height,
                          UINT                Levels,
                          DWORD               Usage,
                          D3DFORMAT           Format,
                          D3DPOOL             Pool,
                          IDirect3DTexture9 **ppTexture,
                          HANDLE             *pSharedHandle)
{
  // Ignore anything that's not the primary render device.
  if (This != tbf::RenderFix::pDevice) {
    return D3D9CreateTexture ( This, Width, Height,
                                          Levels, Usage, Format,
                                            Pool, ppTexture, pSharedHandle );
  }

#if 0
  if (Usage == D3DUSAGE_RENDERTARGET)
  dll_log->Log (L" [!] IDirect3DDevice9::CreateTexture (%lu, %lu, %lu, %lu, "
                                                   L"%lu, %lu, %08Xh, %08Xh)",
                  Width, Height, Levels, Usage, Format, Pool, ppTexture,
                  pSharedHandle);

  tex_log->Log ( L"[Load Trace] >> Creating Texture: "
                L"(%d x %d), Format: %s, Usage: [%s], Pool: %s",
                  Width, Height,
                    SK_D3D9_FormatToStr (Format),
                    SK_D3D9_UsageToStr  (Usage).c_str (),
                    SK_D3D9_PoolToStr   (Pool) );
#endif

  bool game_created = false;

  //
  // Model Shadows
  //
  if (Width == Height && (Width == 64 || Width == 128 || Width == 256) &&
                          (Usage == D3DUSAGE_RENDERTARGET)) {
    //tex_log->Log (L"[Shadow Mgr] (Model Resolution: (%lu x %lu)", Width, Height);
    // Assert (Levels == 1)
    //
    //   If Levels is not 1, then we've kind of screwed up because now we don't
    //     have a complete mipchain anymore.

    uint32_t shift = TBF_MakeShadowBitShift (Width);

    Width  <<= shift;
    Height <<= shift;

    game_created = true;
  }

  else if (Width == Height && (Format == D3DFMT_R32F && Height == 512 || Height == 1024 || Height == 2048) &&
                          (Usage == D3DUSAGE_RENDERTARGET || Usage == D3DUSAGE_DEPTHSTENCIL)) {
      //tex_log->Log (L"[Shadow Mgr] (Env. Resolution: (%lu x %lu) -- CREATE", Width, Height);
      uint32_t shift = config.render.env_shadow_rescale;

      Width  <<= shift;
      Height <<= shift;

      game_created = true;
    }

  //
  // Post-Processing (2048x1024) - FIXME damnit!
  //
  else if ( ( ( Width  == 2048 &&
                Height == 1024 ) ||
              ( Width  == 1024 &&
                Height == 512 )  ||
              ( Width  == 512  &&
                Height == 256 ) ) && Usage == D3DUSAGE_RENDERTARGET ) {
    if (config.render.postproc_ratio > 0.0f) {
      Width  = (UINT)(tbf::RenderFix::width  * config.render.postproc_ratio);
      Height = (UINT)(tbf::RenderFix::height * config.render.postproc_ratio);

      tex_log->Log (L"[ PostProc ] (Post-Resolution: (%lu x %lu)", Width, Height);
    }
  }

  else if ((Usage == D3DUSAGE_RENDERTARGET || Usage == D3DUSAGE_DEPTHSTENCIL) && config.render.fix_map_res)
  {
    if (Width == tbf::RenderFix::width / 11 && Height == tbf::RenderFix::height / 11) {
      Width *= 2; Height *= 2;
    }
    else if (Width == tbf::RenderFix::width / 10 && Height == tbf::RenderFix::height / 10) {
      Width *= 2; Height *= 2;
    }
    else if (Width == tbf::RenderFix::width / 9  && Height == tbf::RenderFix::height / 9) {
      Width *= 2; Height *= 2;
    }
    else if (Width == tbf::RenderFix::width / 8  && Height == tbf::RenderFix::height / 8) {
      Width *= 2; Height *= 2;
    }
    else if (Width == tbf::RenderFix::width / 7  && Height == tbf::RenderFix::height / 7) {
      Width *= 2; Height *= 2;
    }
    else if (Width == tbf::RenderFix::width / 6  && Height == tbf::RenderFix::height / 6) {
      Width *= 2; Height *= 2;
    }
    else if (Width == tbf::RenderFix::width / 5  && Height == tbf::RenderFix::height / 5) {
      Width *= 2; Height *= 2;
    }
    else if (Width == tbf::RenderFix::width / 4  && Height == tbf::RenderFix::height / 4) {
      Width *= 2; Height *= 2;
    }
    else if (Width == tbf::RenderFix::width / 3  && Height == tbf::RenderFix::height / 3) {
      Width *= 2; Height *= 2;
    }
    else if (Width == tbf::RenderFix::width / 2  && Height == tbf::RenderFix::height / 2)
    {
      Width *= 2; Height *= 2;
    }

    int levels = Levels;

    // ASSERT: Levels == 1
  }


  int levels = Levels;

  HRESULT result = 
    D3D9CreateTexture (This, Width, Height, levels, Usage,
                                Format, Pool, ppTexture, pSharedHandle);

  if ( ( Usage & D3DUSAGE_RENDERTARGET ) || 
       ( Usage & D3DUSAGE_DEPTHSTENCIL ) )
    tbf::RenderFix::tex_mgr.trackRenderTarget (*ppTexture);

  return result;
}

COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9BeginScene_Detour (IDirect3DDevice9* This)
{
  // Ignore anything that's not the primary render device.
  if (This != tbf::RenderFix::pDevice) {
    dll_log->Log (L"[Render Fix] >> WARNING: D3D9 BeginScene came from unknown IDirect3DDevice9! << ");

    return D3D9BeginScene (This);
  }

  tbf::RenderFix::draw_state.draws = 0;

  HRESULT result = D3D9BeginScene (This);

  return result;
}

static uint32_t crc32_tab[] = { 
   0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f, 
   0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988, 
   0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2, 
   0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7, 
   0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9, 
   0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172, 
   0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c, 
   0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59, 
   0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423, 
   0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924, 
   0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106, 
   0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433, 
   0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d, 
   0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e, 
   0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950, 
   0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65, 
   0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7, 
   0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0, 
   0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa, 
   0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f, 
   0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81, 
   0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a, 
   0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84, 
   0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1, 
   0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb, 
   0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc, 
   0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e, 
   0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b, 
   0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55, 
   0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236, 
   0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28, 
   0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d, 
   0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f, 
   0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38, 
   0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242, 
   0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777, 
   0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69, 
   0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2, 
   0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc, 
   0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9, 
   0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693, 
   0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94, 
   0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d 
 };

extern 
uint32_t
crc32 (uint32_t crc, const void *buf, size_t size);

typedef HRESULT (WINAPI *D3DXGetImageInfoFromFileInMemory_pfn)
(
  _In_ LPCVOID        pSrcData,
  _In_ UINT           SrcDataSize,
  _In_ D3DXIMAGE_INFO *pSrcInfo
);

D3DXGetImageInfoFromFileInMemory_pfn
  D3DXGetImageInfoFromFileInMemory = nullptr;

typedef HRESULT (WINAPI *D3DXGetImageInfoFromFile_pfn)
(
  _In_ LPCWSTR         pSrcFile,
  _In_ D3DXIMAGE_INFO *pSrcInfo
);

D3DXGetImageInfoFromFile_pfn
  D3DXGetImageInfoFromFile = nullptr;

typedef HRESULT (WINAPI *D3DXCreateTextureFromFileEx_pfn)
(
  _In_    LPDIRECT3DDEVICE9  pDevice,
  _In_    LPCWSTR            pSrcFile,
  _In_    UINT               Width,
  _In_    UINT               Height,
  _In_    UINT               MipLevels,
  _In_    DWORD              Usage,
  _In_    D3DFORMAT          Format,
  _In_    D3DPOOL            Pool,
  _In_    DWORD              Filter,
  _In_    DWORD              MipFilter,
  _In_    D3DCOLOR           ColorKey,
  _Inout_ D3DXIMAGE_INFO     *pSrcInfo,
  _Out_   PALETTEENTRY       *pPalette,
  _Out_   LPDIRECT3DTEXTURE9 *ppTexture
);

D3DXCreateTextureFromFileEx_pfn
  D3DXCreateTextureFromFileEx = nullptr;

typedef HRESULT (WINAPI *D3DXCreateTextureFromFile_pfn)
(
  _In_  LPDIRECT3DDEVICE9   pDevice,
  _In_  LPCWSTR             pSrcFile,
  _Out_ LPDIRECT3DTEXTURE9 *ppTexture
);

static D3DXCreateTextureFromFile_pfn
  D3DXCreateTextureFromFile = nullptr;

#define FONT_CRC32 0xef2d9b55

#define D3DX_FILTER_NONE             0x00000001
#define D3DX_FILTER_POINT            0x00000002
#define D3DX_FILTER_LINEAR           0x00000003
#define D3DX_FILTER_TRIANGLE         0x00000004
#define D3DX_FILTER_BOX              0x00000005
#define D3DX_FILTER_MIRROR_U         0x00010000
#define D3DX_FILTER_MIRROR_V         0x00020000
#define D3DX_FILTER_MIRROR_W         0x00040000
#define D3DX_FILTER_MIRROR           0x00070000
#define D3DX_FILTER_DITHER           0x00080000
#define D3DX_FILTER_DITHER_DIFFUSION 0x00100000
#define D3DX_FILTER_SRGB_IN          0x00200000
#define D3DX_FILTER_SRGB_OUT         0x00400000
#define D3DX_FILTER_SRGB             0x00600000


#define D3DX_SKIP_DDS_MIP_LEVELS_MASK 0x1f
#define D3DX_SKIP_DDS_MIP_LEVELS_SHIFT 26
#define D3DX_SKIP_DDS_MIP_LEVELS(l, f) ((((l) & D3DX_SKIP_DDS_MIP_LEVELS_MASK) \
<< D3DX_SKIP_DDS_MIP_LEVELS_SHIFT) | ((f) == D3DX_DEFAULT ? D3DX_FILTER_BOX : (f)))


#define __PTR_SIZE   sizeof LPCVOID
#define __PAGE_PRIVS PAGE_EXECUTE_READWRITE

#define D3D9_VIRTUAL_OVERRIDE(_Base,_Index,_Name,_Override,_Type) {           \
  void** vftable = *(void***)*_Base;                                          \
                                                                              \
  if (vftable [_Index] != _Override) {                                        \
    DWORD dwProtect;                                                          \
                                                                              \
    VirtualProtect (&vftable [_Index], __PTR_SIZE, __PAGE_PRIVS, &dwProtect); \
                                                                              \
    /*dll_log->Log (L" Old VFTable entry for %s: %08Xh  (Memory Policy: %s)",*/\
                 /*L##_Name, vftable [_Index],                              */\
                 /*SK_DescribeVirtualProtectFlags (dwProtect));             */\
                                                                              \
    if ( == NULL)                                                             \
       = (##_Type)vftable [_Index];                                           \
                                                                              \
    /*dll_log->Log (L"  + %s: %08Xh", L#, );*/               \
                                                                              \
    vftable [_Index] = _Override;                                             \
                                                                              \
    VirtualProtect (&vftable [_Index], __PTR_SIZE, dwProtect, &dwProtect);    \
                                                                              \
    /*dll_log->Log (L" New VFTable entry for %s: %08Xh  (Memory Policy: %s)\n",*/\
                  /*L##_Name, vftable [_Index],                               */\
                  /*SK_DescribeVirtualProtectFlags (dwProtect));              */\
  }                                                                           \
}

struct tbf_tex_load_s {
  enum {
    Stream,    // This load will be streamed
    Immediate, // This load must finish immediately   (pSrc is unused)
    Resample   // Change image properties             (pData is supplied)
  } type;

  LPDIRECT3DDEVICE9   pDevice;

  // Resample only
  LPVOID              pSrcData;
  UINT                SrcDataSize;

  uint32_t            checksum;
  uint32_t            size;

  // Stream / Immediate
  wchar_t             wszFilename [MAX_PATH];

  LPDIRECT3DTEXTURE9  pDest = nullptr;
  LPDIRECT3DTEXTURE9  pSrc  = nullptr;

  LARGE_INTEGER       start = { 0LL };
  LARGE_INTEGER       end   = { 0LL };
  LARGE_INTEGER       freq  = { 0LL };
};

class TexLoadRef {
public:
   TexLoadRef (tbf_tex_load_s* ref) { ref_ = ref;}
  ~TexLoadRef (void) { }

  operator tbf_tex_load_s* (void) {
    return ref_;
  }

protected:
  tbf_tex_load_s* ref_;
};

class SK_TextureThreadPool;

class SK_TextureWorkerThread {
friend class SK_TextureThreadPool;
public:
  SK_TextureWorkerThread (SK_TextureThreadPool* pool)
  {
    pool_ = pool;
    job_  = nullptr;

    control_.start =
      CreateEvent (nullptr, FALSE, FALSE, nullptr);
    control_.trim =
      CreateEvent (nullptr, FALSE, FALSE, nullptr);
    control_.shutdown =
      CreateEvent (nullptr, FALSE, FALSE, nullptr);

    thread_ =
      (HANDLE)_beginthreadex ( nullptr,
                                 0,
                                   ThreadProc,
                                     this,
                                       0,
                                         &thread_id_ );
  }

  ~SK_TextureWorkerThread (void)
  {
    shutdown ();

    WaitForSingleObject (thread_, INFINITE);

    CloseHandle (control_.shutdown);
    CloseHandle (control_.trim);
    CloseHandle (control_.start);

    CloseHandle (thread_);
  }

  void startJob  (tbf_tex_load_s* job) {
    job_ = job;
    SetEvent (control_.start);
  }

  void trim (void) {
    SetEvent (control_.trim);
  }

  void finishJob (void);

  bool isBusy   (void) {
    return (job_ != nullptr);
  }

  void shutdown (void) {
    SetEvent (control_.shutdown);
  }

protected:
  static CRITICAL_SECTION cs_worker_init;
  static ULONG            num_threads_init;

  static unsigned int __stdcall ThreadProc (LPVOID user);

  SK_TextureThreadPool* pool_;

  unsigned int          thread_id_;
  HANDLE                thread_;

  tbf_tex_load_s*       job_;

  struct {
    union {
      struct {
        HANDLE start;
        HANDLE trim;
        HANDLE shutdown;
      };
      HANDLE   ops [3];
    };
  } control_;
};

class SK_TextureThreadPool {
friend class SK_TextureWorkerThread;
public:
  SK_TextureThreadPool (void) {
    events_.jobs_added =
      CreateEvent (nullptr, FALSE, FALSE, nullptr);

    events_.results_waiting =
      CreateEvent (nullptr, FALSE, FALSE, nullptr);

    events_.shutdown =
      CreateEvent (nullptr, FALSE, FALSE, nullptr);

    InitializeCriticalSectionAndSpinCount (&cs_jobs,      10UL);
    InitializeCriticalSectionAndSpinCount (&cs_results, 1000UL);

    const int MAX_THREADS = config.textures.worker_threads;

    static bool init_worker_sync = false;
    if (! init_worker_sync) {
      // We will add a sync. barrier that waits for all of the threads in this pool, plus all of the threads
      //   in the other pool to initialize. This design is flawed, but safe.
      InitializeCriticalSectionAndSpinCount (&SK_TextureWorkerThread::cs_worker_init, 10000UL);
      init_worker_sync = true;
    }

    for (int i = 0; i < MAX_THREADS; i++) {
      SK_TextureWorkerThread* pWorker =
        new SK_TextureWorkerThread (this);

      workers_.push_back (pWorker);
    }

    // This will be deferred until it is first needed...
    spool_thread_ = nullptr;
  }

  ~SK_TextureThreadPool (void) {
    if (spool_thread_ != nullptr) {
      shutdown ();

      WaitForSingleObject (spool_thread_, INFINITE);
      CloseHandle         (spool_thread_);
    }

    DeleteCriticalSection (&cs_results);
    DeleteCriticalSection (&cs_jobs);

    CloseHandle (events_.results_waiting);
    CloseHandle (events_.jobs_added);
    CloseHandle (events_.shutdown);
  }

  void postJob (tbf_tex_load_s* job)
  {
    EnterCriticalSection (&cs_jobs);
    {
      // Defer the creation of this until the first job is posted
      if (! spool_thread_) {
        spool_thread_ =
          (HANDLE)_beginthreadex ( nullptr,
                                     0,
                                       Spooler,
                                         this,
                                           0x00,
                                             nullptr );
      }

      // Don't let the game free this while we are working on it...
      job->pDest->AddRef ();

      jobs_.push (job);
      SetEvent   (events_.jobs_added);
    }
    LeaveCriticalSection (&cs_jobs);
  }

  std::vector <tbf_tex_load_s *> getFinished (void)
  {
    std::vector <tbf_tex_load_s *> results;

    DWORD dwResults =
      WaitForSingleObject (events_.results_waiting, 0);

    // Nothing waiting
    if (dwResults != WAIT_OBJECT_0)
      return results;

    EnterCriticalSection (&cs_results);
    {
      while (! results_.empty ()) {
        results.push_back (results_.front ());
                           results_.pop   ();
      }
    }
    LeaveCriticalSection (&cs_results);

    return results;
  }

  bool working (void) {
    return (! results_.empty ());
  }

  size_t queueLength (void) {
    size_t num = 0;

    EnterCriticalSection (&cs_jobs);
    {
      num = jobs_.size ();
    }
    LeaveCriticalSection (&cs_jobs);

    return num;
  }

  void shutdown (void) {
    SetEvent (events_.shutdown);
  }


protected:
  static unsigned int __stdcall Spooler (LPVOID user);

  tbf_tex_load_s* getNextJob   (void) {
    tbf_tex_load_s* job       = nullptr;
    DWORD           dwResults = 0;

    //while (dwResults != WAIT_OBJECT_0) {
      //dwResults = WaitForSingleObject (events_.jobs_added, INFINITE);
    //}

    if (jobs_.empty ())
      return nullptr;

    EnterCriticalSection (&cs_jobs);
    {
      job = jobs_.front ();
            jobs_.pop   ();
    }
    LeaveCriticalSection (&cs_jobs);

    return job;
  }

  void            postFinished (tbf_tex_load_s* finished)
  {
    EnterCriticalSection (&cs_results);
    {
      // Remove the temporary reference we added earlier
      finished->pDest->Release ();

      results_.push (finished);
      SetEvent      (events_.results_waiting);
    }
    LeaveCriticalSection (&cs_results);
  }

private:
  std::queue <TexLoadRef> jobs_;
  std::queue <TexLoadRef> results_;

  std::vector <SK_TextureWorkerThread *> workers_;

  struct {
    HANDLE jobs_added;
    HANDLE results_waiting;
    HANDLE shutdown;
  } events_;

  CRITICAL_SECTION cs_jobs;
  CRITICAL_SECTION cs_results;

  HANDLE spool_thread_;
} *resample_pool = nullptr;

//
// Split stream jobs into small and large in order to prevent
//   starvation from wreaking havoc on load times.
//
//   This is a simple, but remarkably effective approach and
//     further optimization work probably will not be done.
//
struct SK_StreamSplitter
{
  bool working (void) {
    if (lrg_tex && lrg_tex->working ())
      return true;

    if (sm_tex && sm_tex->working ())
      return true;

    return false;
  }

  size_t queueLength (void)
  {
    size_t len = 0;

    if (lrg_tex) len += lrg_tex->queueLength ();
    if (sm_tex)  len += sm_tex->queueLength  ();

    return len;
  }

  std::vector <tbf_tex_load_s *> getFinished (void)
  {
    std::vector <tbf_tex_load_s *> results;

    std::vector <tbf_tex_load_s *> lrg_loads;
    std::vector <tbf_tex_load_s *> sm_loads;

    if (lrg_tex) lrg_loads = lrg_tex->getFinished ();
    if (sm_tex)  sm_loads  = sm_tex->getFinished  ();

    results.insert (results.begin (), lrg_loads.begin (), lrg_loads.end ());
    results.insert (results.begin (), sm_loads.begin  (), sm_loads.end  ());

    return results;
  }

  void postJob (tbf_tex_load_s* job)
  {
    // A "Large" load is one >= 128 KiB
    if (job->SrcDataSize > (128 * 1024))
      lrg_tex->postJob (job);
    else
      sm_tex->postJob (job);
  }

  SK_TextureThreadPool* lrg_tex = nullptr;
  SK_TextureThreadPool* sm_tex  = nullptr;
} stream_pool;

std::queue <TexLoadRef> textures_to_stream;

std::unordered_map   <uint32_t, tbf_tex_load_s *>
                              textures_in_flight;

std::queue <TexLoadRef> finished_loads;

CRITICAL_SECTION              cs_tex_stream;
CRITICAL_SECTION              cs_tex_resample;

#define D3DX_DEFAULT            ((UINT) -1)
#define D3DX_DEFAULT_NONPOW2    ((UINT) -2)
#define D3DX_DEFAULT_FLOAT      FLT_MAX
#define D3DX_FROM_FILE          ((UINT) -3)
#define D3DFMT_FROM_FILE        ((D3DFORMAT) -3)

#ifdef NO_TLS
std::set <DWORD> texinject_tids;
CRITICAL_SECTION cs_tex_inject;
#else
#include "tls.h"
#endif

volatile  LONG streaming       = 0L;
volatile ULONG streaming_bytes = 0L;

volatile  LONG resampling      = 0L;

bool
pending_loads (void)
{
  bool ret = false;

  return
    ( stream_pool.working () ||
        ( resample_pool != nullptr && resample_pool->working () ) );

//  EnterCriticalSection (&cs_tex_inject);
//  ret = (! finished_loads.empty ());
//  LeaveCriticalSection (&cs_tex_inject);

  return ret;
}

void
start_load (void)
{
#ifndef NO_TLS
  TBF_TLS* pTLS = TBF_GetTLS ();

  if (pTLS != nullptr)
    pTLS->d3d9.texinject_thread = true;
#else
  EnterCriticalSection (&cs_tex_inject);

  inject_tids.insert (GetCurrentThreadId ());

  LeaveCriticalSection (&cs_tex_inject);
#endif
}

void
end_load (void)
{
#ifndef NO_TLS
  TBF_TLS* pTLS = TBF_GetTLS ();

  if (pTLS != nullptr)
    pTLS->d3d9.texinject_thread = false;
#else
  EnterCriticalSection (&cs_tex_inject);

  inject_tids.erase (GetCurrentThreadId ());

  LeaveCriticalSection (&cs_tex_inject);
#endif
}


bool
pending_streams (void)
{
  bool ret = false;

  if (InterlockedExchangeAdd (&streaming, 0) || stream_pool.queueLength () || (resample_pool && resample_pool->queueLength ()))
    ret = true;

  return ret;
}

bool
is_streaming (uint32_t checksum)
{
  bool ret = false;

  EnterCriticalSection (&cs_tex_stream);

  if (textures_in_flight.count (checksum))
    ret = true;

  LeaveCriticalSection (&cs_tex_stream);

  return ret;
}

void
finished_streaming (uint32_t checksum)
{
  EnterCriticalSection (&cs_tex_stream);

  if (textures_in_flight.count (checksum))
    textures_in_flight.erase (checksum);

  LeaveCriticalSection (&cs_tex_stream);
}


HANDLE decomp_semaphore;

// Keep a pool of memory around so that we are not allocating and freeing
//  memory constantly...
namespace streaming_memory {
  std::unordered_map <DWORD, void*>    data;
  std::unordered_map <DWORD, size_t>   data_len;
  std::unordered_map <DWORD, uint32_t> data_age;

  bool alloc (size_t len, DWORD dwThreadId = GetCurrentThreadId ())
  {
    if (data_len [dwThreadId] < len) {
      if (data [dwThreadId] != nullptr)
        free (data [dwThreadId]);

      if (len < 8192 * 1024)
        data_len [dwThreadId] = 8192 * 1024;
      else
        data_len [dwThreadId] = len;

      data     [dwThreadId] = malloc      (data_len [dwThreadId]);
      data_age [dwThreadId] = timeGetTime ();

      if (data [dwThreadId] != nullptr) {
        return true;
      } else {
        data_len [dwThreadId] = 0;
        return false;
      }
    } else {
      return true;
    }
  }

  void trim (size_t max_size, uint32_t min_age, DWORD dwThreadId = GetCurrentThreadId ()) {
    if (data_age [dwThreadId] < min_age) {
      if (data_len [dwThreadId] > max_size) {
        free (data [dwThreadId]);
        data  [dwThreadId] = nullptr;

        if (max_size > 0)
          data [dwThreadId] = malloc (max_size);

        if (data  [dwThreadId] != nullptr) {
          data_len [dwThreadId] = max_size;
          data_age [dwThreadId] = timeGetTime ();
        } else {
          data_len [dwThreadId] = 0;
          data_age [dwThreadId] = 0;
        }
      }
    }
  }
}

HRESULT
InjectTexture (tbf_tex_load_s* load)
{
  D3DXIMAGE_INFO img_info = {    };
  bool           streamed =  false;
  size_t         size     =      0;
  HRESULT        hr       = E_FAIL;

  auto inject =
    injectable_textures.find (load->checksum);

  if (inject == injectable_textures.end ())
  {
    tex_log->Log ( L"[Inject Tex]  >> Load Request for Checksum: %X "
                   L"has no Injection Record !!",
                     load->checksum );

    return E_NOT_VALID_STATE;
  }

  load->pDest->AddRef ();

  const tbf_tex_record_s* inj_tex =
    &(*inject).second;

  streamed =
    (inj_tex->method == Streaming);

  //
  // Load:  From Regular Filesystem
  //
  if ( inj_tex->archive == std::numeric_limits <unsigned int>::max () )
  {
    HANDLE hTexFile =
      CreateFile ( load->wszFilename,
                     GENERIC_READ,
                       FILE_SHARE_READ,
                         nullptr,
                           OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL |
                             FILE_FLAG_SEQUENTIAL_SCAN,
                               nullptr );

    DWORD read = 0UL;

    if (hTexFile != INVALID_HANDLE_VALUE)
    {
      size = GetFileSize (hTexFile, nullptr);

      if (streaming_memory::alloc (size))
      {
        load->pSrcData = streaming_memory::data [GetCurrentThreadId ()];

        ReadFile (hTexFile, load->pSrcData, (DWORD)size, &read, nullptr);

        load->SrcDataSize = read;

        if (streamed && size > (32 * 1024))
        {
          SetThreadPriority ( GetCurrentThread (),
                                THREAD_PRIORITY_BELOW_NORMAL |
                                THREAD_MODE_BACKGROUND_BEGIN );
        }

        D3DXGetImageInfoFromFileInMemory (
          load->pSrcData,
            load->SrcDataSize,
              &img_info );

        hr = D3DXCreateTextureFromFileInMemoryEx_Original (
          load->pDevice,
            load->pSrcData, load->SrcDataSize,
              D3DX_DEFAULT, D3DX_DEFAULT, img_info.MipLevels,
                0, D3DFMT_FROM_FILE,
                  D3DPOOL_DEFAULT,
                    D3DX_DEFAULT, D3DX_DEFAULT,
                      0,
                        &img_info, nullptr,
                          &load->pSrc );

        load->pSrcData = nullptr;
      }

      else {
        // OUT OF MEMORY ?!
      }

      CloseHandle (hTexFile);
    }
  }

  //
  // Load:  From (Compressed) Archive (.7z or .zip)
  //
  else
  {
    wchar_t       arc_name [MAX_PATH] = { };
    CFileInStream arc_stream;
    std::unique_ptr <CLookToRead>
                  look_stream (new CLookToRead ());
    ISzAlloc      thread_alloc;
    ISzAlloc      thread_tmp_alloc;

    FileInStream_CreateVTable (&arc_stream);
    LookToRead_CreateVTable   (look_stream.get (), False);

    look_stream->realStream = &arc_stream.s;
    LookToRead_Init          (look_stream.get ());

    thread_alloc.Alloc     = SzAlloc;
    thread_alloc.Free      = SzFree;

    thread_tmp_alloc.Alloc = SzAllocTemp;
    thread_tmp_alloc.Free  = SzFreeTemp;

    CSzArEx      arc;
                 size   = inj_tex->size;
    int          fileno = inj_tex->fileno;

    if (inj_tex->archive <= archives.size ())
      wcscpy (arc_name, archives [inj_tex->archive].c_str ());
    else
      wcscpy (arc_name, L"INVALID");

    if (streamed && size > (32 * 1024))
    {
      SetThreadPriority ( GetCurrentThread (),
                            THREAD_PRIORITY_LOWEST |
                            THREAD_MODE_BACKGROUND_BEGIN );
    }

    if (InFile_OpenW (&arc_stream.file, arc_name))
    {
      tex_log->Log ( L"[Inject Tex]  ** Cannot open archive file: %s",
                       arc_name );
      return E_FAIL;
    }

    SzArEx_Init (&arc);

    if (SzArEx_Open (&arc, &look_stream->s, &thread_alloc, &thread_tmp_alloc) != SZ_OK)
    {
      tex_log->Log ( L"[Inject Tex]  ** Cannot open archive file: %s",
                       arc_name );
      return E_FAIL;
    }

    if (streaming_memory::alloc (size))
    {
      load->pSrcData = streaming_memory::data [GetCurrentThreadId ()];
      bool wait      = true;

      while (wait)
      {
        DWORD dwResult = WAIT_OBJECT_0;

        if (streamed && size > (32 * 1024))
        {
          dwResult =
            WaitForSingleObject ( decomp_semaphore, INFINITE );
        }

        switch (dwResult) 
        {
        case WAIT_OBJECT_0:
        {
          uint32_t block_idx     = 0xFFFFFFFF;
          Byte*    out           = (Byte *)streaming_memory::data     [GetCurrentThreadId ()];
          size_t   out_len       =         streaming_memory::data_len [GetCurrentThreadId ()];
          size_t   offset        = 0;
          size_t   decomp_size   = 0;

          SzArEx_Extract ( &arc,          &look_stream->s, fileno,
                           &block_idx,    &out,           &out_len,
                           &offset,       &decomp_size,
                           &thread_alloc, &thread_tmp_alloc );

          if (streamed && size > (32 * 1024))
            ReleaseSemaphore (decomp_semaphore, 1, nullptr);

          wait = false;

          load->pSrcData    = (Byte *)streaming_memory::data [GetCurrentThreadId ()] + offset;
          load->SrcDataSize = (UINT)decomp_size;

          D3DXGetImageInfoFromFileInMemory (
            load->pSrcData,
              load->SrcDataSize,
                &img_info );

          hr = D3DXCreateTextureFromFileInMemoryEx_Original (
            load->pDevice,
              load->pSrcData, load->SrcDataSize,
                img_info.Width, img_info.Height, img_info.MipLevels,
                  0, img_info.Format,
                    D3DPOOL_DEFAULT,
                      D3DX_DEFAULT, D3DX_DEFAULT,
                        0,
                          &img_info, nullptr,
                            &load->pSrc );
        } break;

        default:
          tex_log->Log ( L"[  Tex. Mgr  ] Unexpected Wait Status: %X (crc32=%x)",
                           dwResult,
                             load->checksum );
          wait = false;
          break; 
        }
      }

      load->pSrcData = nullptr;
    }

    File_Close  (&arc_stream.file);
    SzArEx_Free (&arc, &thread_alloc);
  }

  if (streamed && size > (32 * 1024))
  {
    SetThreadPriority ( GetCurrentThread (),
                          THREAD_MODE_BACKGROUND_END );
  }

  return hr;
}

CRITICAL_SECTION osd_cs           = { };
DWORD           last_queue_update =   0;

void
TBFix_UpdateQueueOSD (void)
{
  if (config.textures.show_loading_text)
  {
    DWORD dwTime = timeGetTime ();

    if (TryEnterCriticalSection (&osd_cs))
    {
      extern std::string mod_text;

      LONG resample_count = InterlockedExchangeAdd (&resampling, 0); size_t queue_len = resample_pool->queueLength ();
      LONG stream_count   = InterlockedExchangeAdd (&streaming,  0); size_t to_stream = textures_to_stream.size    ();

      bool is_resampling = (resample_pool->working () || resample_count || queue_len);
      bool is_streaming  = (stream_pool.working    () || stream_count   || to_stream);

      static std::string resampling_text; static DWORD dwLastResample = 0;
      static std::string streaming_text;  static DWORD dwLastStream   = 0;
      
      if (is_resampling)
      {
        size_t count = queue_len + resample_count;

            char szFormatted [64];
        sprintf (szFormatted, "  Resampling: %zi texture", count);

        resampling_text  = szFormatted;
        resampling_text += (count != 1) ? 's' : ' ';

        if (queue_len)
        {
          sprintf (szFormatted, " (%zu queued)", queue_len);
          resampling_text += szFormatted;
        }

        resampling_text += "\n";

        if (count)
          dwLastResample = dwTime;
      }
      
      if (is_streaming)
      {
        size_t count = stream_count + to_stream;

            char szFormatted [64];
        sprintf (szFormatted, "  Streaming:  %zi texture", count);

        streaming_text  = szFormatted;
        streaming_text += (count != 1) ? 's' : ' ';

        sprintf (szFormatted, " [%7.2f MiB]", (double)InterlockedExchangeAdd (&streaming_bytes, 0) / (1024.0f * 1024.0f));
        streaming_text += szFormatted;

        if (to_stream)
        {
          sprintf (szFormatted, " (%zu queued)", to_stream);
          streaming_text += szFormatted;
        }

        if (count)
          dwLastStream = dwTime;
      }

      if (dwLastResample < dwTime - 150)
        resampling_text = "";

      if (dwLastStream < dwTime - 150)
        streaming_text = "";

      mod_text = resampling_text + streaming_text;

      if (mod_text != "")
        last_queue_update = dwTime;
      
      LeaveCriticalSection (&osd_cs);
    }
  }
}

void
TBFix_LoadQueuedTextures (void)
{
  TBFix_UpdateQueueOSD ();

  int loads = 0;

  std::vector <tbf_tex_load_s *> finished_resamples;
  std::vector <tbf_tex_load_s *> finished_streams = stream_pool.getFinished ();

  if (resample_pool != nullptr)
    finished_resamples = resample_pool->getFinished ();

  for (auto it = finished_resamples.begin (); it != finished_resamples.end (); /*it++*/)
  {
    tbf_tex_load_s* load =
      *it;

    QueryPerformanceCounter (&load->end);

    if (true)
    {
      tex_log->Log ( L"[%s] Finished %s texture %08x (%5.2f MiB in %9.4f ms)",
                       (load->type == tbf_tex_load_s::Stream) ? L"Inject Tex" :
                         (load->type == tbf_tex_load_s::Immediate) ? L"Inject Tex" :
                                                                     L" Resample ",
                       (load->type == tbf_tex_load_s::Stream) ? L"streaming" :
                         (load->type == tbf_tex_load_s::Immediate) ? L"loading" :
                                                                     L"filtering",
                         load->checksum,
                           (double)load->SrcDataSize / (1024.0f * 1024.0f),
                             1000.0f * (double)(load->end.QuadPart - load->start.QuadPart) /
                                       (double)load->freq.QuadPart );
    }

    tbf::RenderFix::Texture* pTex =
      tbf::RenderFix::tex_mgr.getTexture (load->checksum);

    if (pTex != nullptr)
    {
      pTex->load_time = (float)(1000.0 * (double)(load->end.QuadPart - load->start.QuadPart) /
                                          (double)load->freq.QuadPart);
    }

    ISKTextureD3D9* pSKTex =
      (ISKTextureD3D9 *)load->pDest;

    if (pSKTex != nullptr)
    {
      if (pSKTex->refs == 0 && load->pSrc != nullptr)
      {
        tex_log->Log (L"[ Tex. Mgr ] >> Original texture no longer referenced, discarding new one!");
        load->pSrc->Release ();
      }

      else
      {
        QueryPerformanceCounter (&pSKTex->last_used);

        pSKTex->pTexOverride  = load->pSrc;
        pSKTex->override_size = load->SrcDataSize;

        // The original size info is completely wrong once we start generating mipmaps ;)
        //
        if (load->type == tbf_tex_load_s::Resample)
        {
          pSKTex->override_size = 0;

          for (UINT i = 0; i < pSKTex->pTexOverride->GetLevelCount (); i++)
          {
            D3DSURFACE_DESC                         desc = { };
            pSKTex->pTexOverride->GetLevelDesc (i, &desc);

            int bytes_per_pel = SK_D3D9_BytesPerPixel (desc.Format);

            // If bytes_per_pel is < 0, we have to handle DXT alignment craziness to be accurate...

            if (bytes_per_pel >= 0)
              pSKTex->override_size += desc.Width * desc.Height * bytes_per_pel;

            else
            {
              // Assume once this stuff gets into VRAM that it is tightly-packed,
              //   it would be stupid for the driver to do otherwise.
              UINT stride = bytes_per_pel == -1 ?
               std::max (1UL, ((desc.Width + 3UL) / 4UL) ) * 8UL :
               std::max (1UL, ((desc.Width + 3UL) / 4UL) ) * 16UL;

               size_t lod_size = stride * (desc.Height / 4 +
                                           desc.Height % 4);

               pSKTex->override_size += lod_size;
            }
          }

          load->SrcDataSize = (UINT)pSKTex->override_size;
        }

        tbf::RenderFix::tex_mgr.addInjected (load->SrcDataSize);
      }

      finished_streaming (load->checksum);

      ++loads;

      // Remove the temporary reference
      load->pDest->Release ();
    }

    ++it;

    delete load;
  }

  for (auto it = finished_streams.begin (); it != finished_streams.end (); /*it++*/)
  {
    tbf_tex_load_s* load =
      *it;

    QueryPerformanceCounter (&load->end);

    if (true)
    {
      tex_log->Log ( L"[%s] Finished %s texture %08x (%5.2f MiB in %9.4f ms)",
                       (load->type == tbf_tex_load_s::Stream) ? L"Inject Tex" :
                         (load->type == tbf_tex_load_s::Immediate) ? L"Inject Tex" :
                                                                     L" Resample ",
                       (load->type == tbf_tex_load_s::Stream) ? L"streaming" :
                         (load->type == tbf_tex_load_s::Immediate) ? L"loading" :
                                                                     L"filtering",
                         load->checksum,
                           (double)load->SrcDataSize / (1024.0f * 1024.0f),
                             1000.0f * (double)(load->end.QuadPart - load->start.QuadPart) /
                                       (double)load->freq.QuadPart );
    }

    tbf::RenderFix::Texture* pTex =
      tbf::RenderFix::tex_mgr.getTexture (load->checksum);

    if (pTex != nullptr)
    {
      pTex->load_time = (float)(1000.0 * (double)(load->end.QuadPart - load->start.QuadPart) /
                                           (double)load->freq.QuadPart);
    }

    ISKTextureD3D9* pSKTex =
      (ISKTextureD3D9 *)load->pDest;

    if (pSKTex != nullptr)
    {
      if (pSKTex->refs == 0 && load->pSrc != nullptr)
      {
        tex_log->Log (L"[ Tex. Mgr ] >> Original texture no longer referenced, discarding new one!");
        load->pSrc->Release ();
      }

      else
      {
        QueryPerformanceCounter (&pSKTex->last_used);

        pSKTex->pTexOverride  = load->pSrc;
        pSKTex->override_size = load->SrcDataSize;

        tbf::RenderFix::tex_mgr.addInjected (load->SrcDataSize);
      }

      finished_streaming (load->checksum);

      ++loads;

      // Remove the temporary reference
      load->pDest->Release ();
    }

    ++it;

    delete load;
  }

  //
  // If the size changes, check to see if we need a purge - if so, schedule one.
  //
  static uint64_t last_size = 0ULL;

  if (last_size != tbf::RenderFix::tex_mgr.cacheSizeTotal () )
  {
    last_size = tbf::RenderFix::tex_mgr.cacheSizeTotal ();

    if ( last_size >
           (1024ULL * 1024ULL) * (uint64_t)config.textures.max_cache_in_mib )
      __need_purge = true;
  }

  if ( (! InterlockedExchangeAdd (&streaming,  0)) &&
       (! InterlockedExchangeAdd (&resampling, 0)) &&
       (! pending_loads ()) )
  {
    if (__need_purge)
    {
      tbf::RenderFix::tex_mgr.purge ();
      __need_purge = false;
    }
  }

  tbf::RenderFix::tex_mgr.updateOSD ();
}

#include <set>

std::unordered_set <uint32_t> resample_blacklist;
bool                          resample_blacklist_init = false;

void
TBFix_ReloadPadButtons (void)
{
  if (tbf::RenderFix::pad_buttons.tex_xboxone != nullptr)
  {
    uint32_t checksum = tbf::RenderFix::pad_buttons.crc32_xboxone;

    tbf::RenderFix::Texture* pTex = 
      tbf::RenderFix::tex_mgr.getTexture (tbf::RenderFix::pad_buttons.crc32_xboxone);

    IDirect3DTexture9* pD3DTex =
      pTex->d3d9_tex;

    void* dontcare;
    if ( pD3DTex != nullptr &&
         pD3DTex->QueryInterface (IID_SKTextureD3D9, &dontcare) == S_OK )
    {
      ISKTextureD3D9* pSKTex =
        (ISKTextureD3D9 *)pD3DTex;

      IDirect3DDevice9* pDevice = nullptr;

      wchar_t wszFile [MAX_PATH + 2] = { L'\0' };

      _swprintf ( wszFile,
                    L"TBFix_Res\\Gamepads\\%s\\Buttons.dds",
                      config.input.gamepad.texture_set.c_str () );

      if (GetFileAttributesW (wszFile) != INVALID_FILE_ATTRIBUTES)
      {
        tex_log->LogEx (true, L"[Inject Tex] Injecting custom gamepad buttons... ");

        pD3DTex->GetDevice (&pDevice);

        tbf_tex_load_s* load_op = new tbf_tex_load_s;

        load_op->SrcDataSize =
          injectable_textures.count (checksum) == 0 ?
            0 : (UINT)injectable_textures [checksum].size;

        load_op->pDevice  = pDevice;
        load_op->checksum = checksum;
        load_op->type     = tbf_tex_load_s::Immediate;
        wcsncpy (load_op->wszFilename, wszFile, MAX_PATH);

        tex_log->LogEx ( false, L"blocking (deferred)\n" );

        wcsncpy ( load_op->wszFilename,
                    wszFile,
                      MAX_PATH );

        load_op->pDest     = pD3DTex;

        EnterCriticalSection        (&cs_tex_stream);

        ((ISKTextureD3D9 *)pD3DTex)->must_block = true;

        if (is_streaming (load_op->checksum))
        {
          ISKTextureD3D9* pTexOrig =
            (ISKTextureD3D9 *)textures_in_flight [load_op->checksum]->pDest;

          // Remap the output of the in-flight texture
          textures_in_flight [load_op->checksum]->pDest =
            pD3DTex;

          if (tbf::RenderFix::tex_mgr.getTexture (load_op->checksum)  != nullptr)
          {
            for ( int i = 0;
                      i < tbf::RenderFix::tex_mgr.getTexture (load_op->checksum)->refs;
                    ++i ) {
              pD3DTex->AddRef ();
            }
          }

          ////tsf::RenderFix::tex_mgr.removeTexture (pTexOrig);
        }

        else
        {
          textures_in_flight.insert ( std::make_pair ( load_op->checksum,
                                       load_op ) );

          resample_pool->postJob (load_op);
        }

        current_tex = pSKTex->tex_crc32;
        LeaveCriticalSection (&cs_tex_stream);
      }
    }
  }
}

COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3DXCreateTextureFromFileInMemoryEx_Detour (
  _In_    LPDIRECT3DDEVICE9  pDevice,
  _In_    LPCVOID            pSrcData,
  _In_    UINT               SrcDataSize,
  _In_    UINT               Width,
  _In_    UINT               Height,
  _In_    UINT               MipLevels,
  _In_    DWORD              Usage,
  _In_    D3DFORMAT          Format,
  _In_    D3DPOOL            Pool,
  _In_    DWORD              Filter,
  _In_    DWORD              MipFilter,
  _In_    D3DCOLOR           ColorKey,
  _Inout_ D3DXIMAGE_INFO     *pSrcInfo,
  _Out_   PALETTEENTRY       *pPalette,
  _Out_   LPDIRECT3DTEXTURE9 *ppTexture
)
{
  bool inject_thread = false;

#ifndef NO_TLS
  TBF_TLS* pTLS = TBF_GetTLS ();

  inject_thread = pTLS->d3d9.texinject_thread;
#else
  EnterCriticalSection (&cs_tex_inject);

  if (inject_tids.count (GetCurrentThreadId ()))
  {
    inject_thread = true;
  }

  LeaveCriticalSection (&cs_tex_inject);
#endif

  // Injection would recurse slightly and cause impossible to diagnose reference counting problems
  //   with texture caching if we did not check for this!
  if (inject_thread) {
    return D3DXCreateTextureFromFileInMemoryEx_Original (
      pDevice,
        pSrcData, SrcDataSize,
          Width, Height, MipLevels,
            Usage,
              Format,
                Pool,
                  Filter, MipFilter, ColorKey,
                    pSrcInfo, pPalette,
                      ppTexture );
  }

  if (resample_blacklist_init == false)
  {
    // Do Not Resample Logos
    resample_blacklist.insert (0xfa3d03df);
    resample_blacklist.insert (0x545908bb);

    resample_blacklist_init = true;
  }

  // Performance statistics for caching system
         LARGE_INTEGER start, end;
  static LARGE_INTEGER freq = { 0LL };

  if (freq.QuadPart == 0LL)
    QueryPerformanceFrequency (&freq);

  QueryPerformanceCounter     (&start);

  uint32_t checksum =
    crc32 (0, pSrcData, SrcDataSize);

  // Don't dump or cache these
  if (Usage == D3DUSAGE_DYNAMIC || Usage == D3DUSAGE_RENDERTARGET)
    checksum = 0x00;

  if (config.textures.cache && checksum != 0x00)
  {
    tbf::RenderFix::Texture* pTex =
      tbf::RenderFix::tex_mgr.getTexture (checksum);

    if (pTex != nullptr)
    {
      tbf::RenderFix::tex_mgr.refTexture (pTex);

      *ppTexture = pTex->d3d9_tex;

      return S_OK;
    }

    tbf::RenderFix::tex_mgr.missTexture ();
  }

  bool resample = false;

  // Necessary to make D3DX texture write functions work
  if ( Pool == D3DPOOL_DEFAULT && (config.textures.on_demand_dump ||
       ( config.textures.dump                         &&
        (! dumped_textures.count     (checksum))      &&
        (! injectable_textures.count (checksum)) ) ) )
    Usage = D3DUSAGE_DYNAMIC;


  D3DXIMAGE_INFO info = { 0 };
  D3DXGetImageInfoFromFileInMemory (pSrcData, SrcDataSize, &info);


  bool power_of_two_in_one_way =  
    (! (info.Width  & (info.Width  - 1)))  !=  (! (info.Height & (info.Height - 1)));


  // Textures that would be incorrectly filtered if resampled
  if (power_of_two_in_one_way)
    non_power_of_two_textures.insert (checksum);


  // Generate complete mipmap chains for best image quality
  //  (will increase load-time on uncached textures)
  if ((Pool == D3DPOOL_DEFAULT) && config.textures.remaster)
  {
    if (true)
    {
      bool power_of_two_in =
        (! (info.Width  & (info.Width  - 1)))  &&  (! (info.Height & (info.Height - 1)));

      bool power_of_two_out =
        (! (Width  & (Width  - 1)))            &&  (! (Height & (Height - 1)));

      if (power_of_two_in && power_of_two_out)
      {
        if (info.MipLevels > 1/* || config.textures.uncompressed*/)
        {
          resample = true;
        }
      }
    }
  }

  HRESULT         hr           = E_FAIL;
  tbf_tex_load_s* load_op      = nullptr;

  wchar_t wszInjectFileName [MAX_PATH] = { L'\0' };

  bool remap_stream = is_streaming (checksum);

  //
  // Generic injectable textures
  //
  if ( (! inject_thread) &&
            injectable_textures.find (checksum) !=
                     injectable_textures.end () )
  {
    tex_log->LogEx ( true, L"[Inject Tex] Injectable texture for checksum (%08x)... ",
                       checksum );

    tbf_tex_record_s record = injectable_textures [checksum];

    if (record.method == DontCare)
      record.method = Streaming;

    // If -1, load from disk...
    if (record.archive == std::numeric_limits <unsigned int>::max ())
    {
      if (record.method == Streaming)
        _swprintf ( wszInjectFileName, L"%s\\inject\\textures\\streaming\\%08x%s",
                      TBFIX_TEXTURE_DIR,
                        checksum,
                          TBFIX_TEXTURE_EXT );
      else if (record.method == Blocking)
        _swprintf ( wszInjectFileName, L"%s\\inject\\textures\\blocking\\%08x%s",
                      TBFIX_TEXTURE_DIR,
                        checksum,
                          TBFIX_TEXTURE_EXT );
    }

    load_op           = new tbf_tex_load_s;
    load_op->pDevice  = pDevice;
    load_op->checksum = checksum;

    if (record.method == Streaming)
      load_op->type   = tbf_tex_load_s::Stream;
    else
      load_op->type   = tbf_tex_load_s::Immediate;

    wcscpy (load_op->wszFilename, wszInjectFileName);

    if (load_op->type == tbf_tex_load_s::Stream)
    {
      if ((! remap_stream))
        tex_log->LogEx ( false, L"streaming\n" );
      else
        tex_log->LogEx ( false, L"in-flight already\n" );
    }

    else
    {
      tex_log->LogEx ( false, L"blocking (deferred)\n" );
    }
  }

  bool will_replace = config.textures.quick_load && resample;

  //tex_log->Log (L"D3DXCreateTextureFromFileInMemoryEx (... MipLevels=%lu ...)", MipLevels);
  hr =
    D3DXCreateTextureFromFileInMemoryEx_Original ( pDevice,
                                                     pSrcData,         SrcDataSize,
                                                       Width,          Height,    will_replace ? 1 : MipLevels,
                                                         Usage,        Format,    Pool,
                                                           Filter,     MipFilter, ColorKey,
                                                             pSrcInfo, pPalette,
                                                               ppTexture );

  if (SUCCEEDED (hr))
  {
    new ISKTextureD3D9 (ppTexture, SrcDataSize, checksum);

    const uint32_t license_crc32 = 0x86c4b6d0UL;

    if (checksum == license_crc32)
    {
      wchar_t wszFile [MAX_PATH + 2] = { L'\0' };

      lstrcatW ( wszFile, L"TBFix_Res\\license.dds" );

      if (GetFileAttributesW (wszFile) != INVALID_FILE_ATTRIBUTES)
      {
        tex_log->LogEx (true, L"[Inject Tex] Injecting custom license disclaimer... ");

        load_op           = new tbf_tex_load_s;
        load_op->pDevice  = pDevice;
        load_op->checksum = checksum;
        load_op->type     = tbf_tex_load_s::Immediate;
        wcsncpy (load_op->wszFilename, wszFile, MAX_PATH);

        if (load_op->type == tbf_tex_load_s::Stream) {
          if ((! remap_stream))
            tex_log->LogEx ( false, L"streaming\n" );
          else
            tex_log->LogEx ( false, L"in-flight already\n" );
        } else {
          tex_log->LogEx ( false, L"blocking (deferred)\n" );
        }

        resample = false;
      }
    }

    if (checksum == tbf::RenderFix::pad_buttons.crc32_ps4) {
      tbf::RenderFix::pad_buttons.tex_ps4 = *ppTexture;
    }

    if (checksum == tbf::RenderFix::pad_buttons.crc32_xboxone) {
      tbf::RenderFix::pad_buttons.tex_xboxone = *ppTexture;

      wchar_t wszFile [MAX_PATH + 2] = { L'\0' };

      _swprintf ( wszFile,
                    L"TBFix_Res\\Gamepads\\%s\\Buttons.dds",
                      config.input.gamepad.texture_set.c_str () );

      if (GetFileAttributesW (wszFile) != INVALID_FILE_ATTRIBUTES)
      {
        WIN32_FILE_ATTRIBUTE_DATA
          file_attrib_data = { 0 };

        GetFileAttributesEx ( wszFile,
                                GetFileExInfoStandard,
                                  &file_attrib_data );

        tbf_tex_record_s rec;
        rec.size    =  ULARGE_INTEGER { file_attrib_data.nFileSizeLow,
                                          file_attrib_data.nFileSizeHigh }.QuadPart;
        rec.archive = std::numeric_limits <unsigned int>::max ();
        rec.method  =  Blocking;

        if (! injectable_textures.count (checksum))
          injectable_textures.insert (std::make_pair (checksum, rec));
        else {
          injectable_textures [checksum] = rec;
        }

        tex_log->LogEx (true, L"[Inject Tex] Injecting custom gamepad buttons... ");

        load_op           = new tbf_tex_load_s;
        load_op->pDevice  = pDevice;
        load_op->checksum = checksum;
        load_op->type     = tbf_tex_load_s::Immediate;
        wcsncpy (load_op->wszFilename, wszFile, MAX_PATH);

        if (load_op->type == tbf_tex_load_s::Stream) {
          if ((! remap_stream))
            tex_log->LogEx ( false, L"streaming\n" );
          else
            tex_log->LogEx ( false, L"in-flight already\n" );
        } else {
          tex_log->LogEx ( false, L"blocking (deferred)\n" );
        }

        resample = false;
      }
    }


    if ( load_op != nullptr && ( load_op->type == tbf_tex_load_s::Stream ||
                                 load_op->type == tbf_tex_load_s::Immediate ) ) {
      load_op->SrcDataSize =
        injectable_textures.count (checksum) == 0 ?
          0 : (UINT)injectable_textures [checksum].size;

      load_op->pDest = *ppTexture;
      EnterCriticalSection        (&cs_tex_stream);

      if (load_op->type == tbf_tex_load_s::Immediate)
        ((ISKTextureD3D9 *)*ppTexture)->must_block = true;

      if (is_streaming (load_op->checksum)) {
        ISKTextureD3D9* pTexOrig =
          (ISKTextureD3D9 *)textures_in_flight [load_op->checksum]->pDest;

        // Remap the output of the in-flight texture
        textures_in_flight [load_op->checksum]->pDest =
          *ppTexture;

        if (tbf::RenderFix::tex_mgr.getTexture (load_op->checksum)  != nullptr) {
          for ( int i = 0;
                    i < tbf::RenderFix::tex_mgr.getTexture (load_op->checksum)->refs;
                  ++i ) {
            (*ppTexture)->AddRef ();
          }
        }

        ////tsf::RenderFix::tex_mgr.removeTexture (pTexOrig);
      }

      else {
        textures_in_flight.insert ( std::make_pair ( load_op->checksum,
                                     load_op ) );

        stream_pool.postJob (load_op);
        //resample_pool->postJob (load_op);
      }

      LeaveCriticalSection        (&cs_tex_stream);
    }

#if 0
    //
    // TODO:  Actually stream these, but block if the game tries to call SetTexture (...)
    //          while the texture is in-flight.
    //
    else if (load_op != nullptr && load_op->type == tsf_tex_load_s::Immediate) {
      QueryPerformanceFrequency        (&load_op->freq);
      QueryPerformanceCounter (&load_op->start);

      EnterCriticalSection (&cs_tex_inject);
      inject_tids.insert   (GetCurrentThreadId ());
      LeaveCriticalSection (&cs_tex_inject);

      load_op->pDest = *ppTexture;

      hr = InjectTexture (load_op);

      EnterCriticalSection (&cs_tex_inject);
      inject_tids.erase    (GetCurrentThreadId ());
      LeaveCriticalSection (&cs_tex_inject);

      QueryPerformanceCounter (&load_op->end);

      if (SUCCEEDED (hr)) {
        tex_log->Log ( L"[Inject Tex] Finished synchronous texture %08x (%5.2f MiB in %9.4f ms)",
                        load_op->checksum,
                          (double)load_op->SrcDataSize / (1024.0f * 1024.0f),
                            1000.0f * (double)(load_op->end.QuadPart - load_op->start.QuadPart) /
                                      (double) load_op->freq.QuadPart );
        ISKTextureD3D9* pSKTex =
          (ISKTextureD3D9 *)*ppTexture;

        pSKTex->pTexOverride  = load_op->pSrc;
        pSKTex->override_size = load_op->SrcDataSize;

        pSKTex->last_used     = load_op->end;

        tsf::RenderFix::tex_mgr.addInjected (load_op->SrcDataSize);
      } else {
        tex_log->Log ( L"[Inject Tex] *** FAILED synchronous texture %08x",
                        load_op->checksum );
      }

      delete load_op;
      load_op = nullptr;
    }
#endif

    else if (resample) {
      load_op              = new tbf_tex_load_s;

      load_op->pDevice     = pDevice;
      load_op->checksum    = checksum;
      load_op->type        = tbf_tex_load_s::Resample;

      load_op->pSrcData    = new uint8_t [SrcDataSize];
      load_op->SrcDataSize = SrcDataSize;

      _swprintf (load_op->wszFilename, L"Resample_%x.dds", checksum);

      memcpy (load_op->pSrcData, pSrcData, SrcDataSize);

      (*ppTexture)->AddRef ();
      load_op->pDest       = *ppTexture;

      resample_pool->postJob (load_op);
    }
  }

  else if (load_op != nullptr) {
    delete load_op;
    load_op = nullptr;
  }

  QueryPerformanceCounter (&end);

  if (SUCCEEDED (hr))
  {
    if (config.textures.cache && checksum != 0x00)
    {
      tbf::RenderFix::Texture* pTex =
        new tbf::RenderFix::Texture ();

      pTex->crc32 = checksum;

      pTex->d3d9_tex = *(ISKTextureD3D9 **)ppTexture;
      pTex->d3d9_tex->AddRef ();
      pTex->refs++;

      pTex->load_time = (float)( 1000.0 *
                          (double)(end.QuadPart - start.QuadPart) /
                          (double)freq.QuadPart );

      tbf::RenderFix::tex_mgr.addTexture (checksum, pTex, SrcDataSize);
    }

    if (false) {//config.textures.log) {
      tex_log->Log ( L"[Load Trace] Texture:   (%lu x %lu) * <LODs: %lu> - FAST_CRC32: %X",
                      Width, Height, (*ppTexture)->GetLevelCount (), checksum );
      tex_log->Log ( L"[Load Trace]              Usage: %-20s - Format: %-20s",
                      SK_D3D9_UsageToStr    (Usage).c_str (),
                        SK_D3D9_FormatToStr (Format).c_str () );
      tex_log->Log ( L"[Load Trace]                Pool: %s",
                      SK_D3D9_PoolToStr (Pool) );
      tex_log->Log ( L"[Load Trace]      Load Time: %6.4f ms", 
                    1000.0f * (double)(end.QuadPart - start.QuadPart) / (double)freq.QuadPart );
    }
  }

  if ( config.textures.dump && (! inject_thread) && (! injectable_textures.count (checksum)) &&
                          (! dumped_textures.count (checksum)) )
  {
    D3DXIMAGE_INFO info = { 0 };
    D3DXGetImageInfoFromFileInMemory (pSrcData, SrcDataSize, &info);

    D3DFORMAT fmt_real = info.Format;

    TBF_DumpTexture (fmt_real, checksum, *ppTexture);
  }

  return hr;
}

bool
TBF_DeleteDumpedTexture (D3DFORMAT fmt, uint32_t checksum)
{
  wchar_t wszPath [MAX_PATH];
  _swprintf ( wszPath, L"%s\\dump",
                TBFIX_TEXTURE_DIR );

  if (GetFileAttributesW (wszPath) != FILE_ATTRIBUTE_DIRECTORY)
    CreateDirectoryW (wszPath, nullptr);

  _swprintf ( wszPath, L"%s\\dump\\textures",
                TBFIX_TEXTURE_DIR );

  if (GetFileAttributesW (wszPath) != FILE_ATTRIBUTE_DIRECTORY)
    CreateDirectoryW (wszPath, nullptr);

  _swprintf ( wszPath, L"%s\\%s",
                wszPath,
                 SK_D3D9_FormatToStr (fmt, false).c_str () );

  if (GetFileAttributesW (wszPath) != FILE_ATTRIBUTE_DIRECTORY)
    CreateDirectoryW (wszPath, nullptr);

  wchar_t wszFileName [MAX_PATH] = { L'\0' };
  _swprintf ( wszFileName, L"%s\\dump\\textures\\%s\\%08x%s",
                TBFIX_TEXTURE_DIR,
                  SK_D3D9_FormatToStr (fmt, false).c_str (),
                    checksum,
                      TBFIX_TEXTURE_EXT );

  if (GetFileAttributesW (wszFileName) != INVALID_FILE_ATTRIBUTES)
  {
    if (DeleteFileW (wszFileName))
    {
      dumped_textures.erase (checksum);
      return true;
    }
  }

  return false;
}

bool
TBF_IsTextureDumped (uint32_t checksum)
{
  return dumped_textures.count (checksum);
}

HRESULT
TBF_DumpTexture (D3DFORMAT fmt, uint32_t checksum, IDirect3DTexture9* pTex)
{
  if ( (! injectable_textures.count (checksum)) &&
       (! dumped_textures.count     (checksum)) )
  {
    D3DFORMAT fmt_real = fmt;

    bool compressed = (fmt_real >= D3DFMT_DXT1 && fmt_real <= D3DFMT_DXT5);

    wchar_t wszPath [MAX_PATH];
    _swprintf ( wszPath, L"%s\\dump",
                  TBFIX_TEXTURE_DIR );

    if (GetFileAttributesW (wszPath) != FILE_ATTRIBUTE_DIRECTORY)
      CreateDirectoryW (wszPath, nullptr);

    _swprintf ( wszPath, L"%s\\dump\\textures",
                  TBFIX_TEXTURE_DIR );

    if (GetFileAttributesW (wszPath) != FILE_ATTRIBUTE_DIRECTORY)
      CreateDirectoryW (wszPath, nullptr);

    _swprintf ( wszPath, L"%s\\%s",
                  wszPath,
                   SK_D3D9_FormatToStr (fmt_real, false).c_str () );

    if (GetFileAttributesW (wszPath) != FILE_ATTRIBUTE_DIRECTORY)
      CreateDirectoryW (wszPath, nullptr);

    wchar_t wszFileName [MAX_PATH] = { L'\0' };
    _swprintf ( wszFileName, L"%s\\dump\\textures\\%s\\%08x%s",
                  TBFIX_TEXTURE_DIR,
                    SK_D3D9_FormatToStr (fmt_real, false).c_str (),
                      checksum,
                        TBFIX_TEXTURE_EXT );

    HRESULT hr = D3DXSaveTextureToFile (wszFileName, D3DXIFF_DDS, pTex, NULL);

    if (SUCCEEDED (hr))
      dumped_textures.insert (checksum);

    return hr;
  }

  return E_FAIL;
}

std::vector <ISKTextureD3D9 *> remove_textures;

tbf::RenderFix::Texture*
tbf::RenderFix::TextureManager::getTexture (uint32_t checksum)
{
  EnterCriticalSection (&cs_cache);

    auto rem = remove_textures.begin ();

    while (rem != remove_textures.end ())
    {
      if ((*rem)->pTexOverride != nullptr)
      {
        InterlockedDecrement (&injected_count);
        InterlockedAdd64     (&injected_size, -(*rem)->override_size);
      }

      if ((*rem)->pTex)         (*rem)->pTex->Release         ();
      if ((*rem)->pTexOverride) (*rem)->pTexOverride->Release ();

      (*rem)->pTex         = nullptr;
      (*rem)->pTexOverride = nullptr;

      InterlockedAdd64 (&basic_size,  -(*rem)->tex_size);
      {
        textures.erase     ((*rem)->tex_crc32);
      }

      delete *rem;

      ++rem;
    }

    remove_textures.clear ();

    auto tex = textures.find (checksum);

  LeaveCriticalSection (&cs_cache);

  if (tex != textures.end ())
    return tex->second;

  return nullptr;
}

void
tbf::RenderFix::TextureManager::removeTexture (ISKTextureD3D9* pTexD3D9)
{
  EnterCriticalSection (&cs_cache);

  remove_textures.push_back (pTexD3D9);

  updateOSD ();

  LeaveCriticalSection (&cs_cache);
}

void
tbf::RenderFix::TextureManager::addTexture (uint32_t checksum, tbf::RenderFix::Texture* pTex, size_t size)
{
  pTex->size = size;

  InterlockedAdd64 (&basic_size, pTex->size);

  EnterCriticalSection (&cs_cache);
  {
    textures [checksum] = pTex;
  }

  updateOSD ();

  LeaveCriticalSection (&cs_cache);
}

void
tbf::RenderFix::TextureManager::refTexture (tbf::RenderFix::Texture* pTex)
{
  pTex->d3d9_tex->AddRef ();
  pTex->refs++;

  InterlockedIncrement (&hits);

  if (false) {//config.textures.log) {
    tex_log->Log ( L"[CacheTrace] Cache hit (%X), saved %2.1f ms",
                     pTex->crc32,
                       pTex->load_time );
  }

  InterlockedAdd64 (&bytes_saved, pTex->size);
                    time_saved += pTex->load_time;

  updateOSD ();
}

COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9SetRenderTarget_Detour (
                  _In_ IDirect3DDevice9  *This,
                  _In_ DWORD              RenderTargetIndex,
                  _In_ IDirect3DSurface9 *pRenderTarget
)
{
  static int draw_counter = 0;

  // Ignore anything that's not the primary render device.
  if (This != tbf::RenderFix::pDevice) {
    return D3D9SetRenderTarget (This, RenderTargetIndex, pRenderTarget);
  }

  //if (tsf::RenderFix::tracer.log) {
#ifdef DUMP_RT
    if (D3DXSaveSurfaceToFileW == nullptr) {
      D3DXSaveSurfaceToFileW =
        (D3DXSaveSurfaceToFile_pfn)
          GetProcAddress ( tsf::RenderFix::d3dx9_43_dll,
                             "D3DXSaveSurfaceToFileW" );
    }

    wchar_t wszDumpName [MAX_PATH];

    if (pRenderTarget != pOld) {
      if (pOld != nullptr) {
        wsprintf (wszDumpName, L"dump\\%03d_out_%p.png", draw_counter, pOld);

        dll_log->Log ( L"[FrameTrace] >>> Dumped: Output RT to %s >>>", wszDumpName );

        dumping = true;
        //D3DXSaveSurfaceToFile (wszDumpName, D3DXIFF_PNG, pOld, nullptr, nullptr);
      }
    }
#endif

    //dll_log->Log ( L"[FrameTrace] SetRenderTarget - RenderTargetIndex: %lu, pRenderTarget: %ph",
                    //RenderTargetIndex, pRenderTarget );

#ifdef DUMP_RT
    if (pRenderTarget != pOld) {
      pOld = pRenderTarget;

      wsprintf (wszDumpName, L"dump\\%03d_in_%p.png", ++draw_counter, pRenderTarget);

      dll_log->Log ( L"[FrameTrace] <<< Dumped: Input RT to  %s  <<<", wszDumpName );

      dumping = true;
      //D3DXSaveSurfaceToFile (wszDumpName, D3DXIFF_PNG, pRenderTarget, nullptr, nullptr);
    }
#endif
  //}

  return D3D9SetRenderTarget (This, RenderTargetIndex, pRenderTarget);
}

void
tbf::RenderFix::TextureManager::Init (void)
{
  InitializeCriticalSectionAndSpinCount (&cs_cache, 16384UL);
  InitializeCriticalSectionAndSpinCount (&osd_cs,   2UL);

  // Create the directory to store dumped textures
  if (config.textures.dump)
    CreateDirectoryW (TBFIX_TEXTURE_DIR, nullptr);

  tex_log = TBF_CreateLog (L"logs/textures.log");

  CrcGenerateTable ();

  d3dx9_43_dll = LoadLibrary (L"D3DX9_43.DLL");

  TBF_RefreshDataSources ();

  if ( GetFileAttributesW (TBFIX_TEXTURE_DIR L"\\dump\\textures") !=
         INVALID_FILE_ATTRIBUTES ) {
    WIN32_FIND_DATA fd;
    WIN32_FIND_DATA fd_sub;
    HANDLE          hSubFind = INVALID_HANDLE_VALUE;
    HANDLE          hFind    = INVALID_HANDLE_VALUE;
    int             files    = 0;
    LARGE_INTEGER   liSize   = { 0 };

    tex_log->LogEx ( true, L"[ Dump Tex ] Enumerating dumped textures..." );

    hFind = FindFirstFileW (TBFIX_TEXTURE_DIR L"\\dump\\textures\\*", &fd);

    if (hFind != INVALID_HANDLE_VALUE) {
      do {
        if (fd.dwFileAttributes != INVALID_FILE_ATTRIBUTES) {
          wchar_t wszSubDir [MAX_PATH];
          _swprintf (wszSubDir, L"%s\\dump\\textures\\%s\\*", TBFIX_TEXTURE_DIR, fd.cFileName);

          hSubFind = FindFirstFileW (wszSubDir, &fd_sub);

          if (hSubFind != INVALID_HANDLE_VALUE) {
            do {
              if (wcsstr (_wcslwr (fd_sub.cFileName), L".dds")) {
                uint32_t checksum;
                swscanf (fd_sub.cFileName, L"%08x.dds", &checksum);

                ++files;

                LARGE_INTEGER fsize;

                fsize.HighPart = fd_sub.nFileSizeHigh;
                fsize.LowPart  = fd_sub.nFileSizeLow;

                liSize.QuadPart += fsize.QuadPart;

                dumped_textures.insert (checksum);
              }
            } while (FindNextFileW (hSubFind, &fd_sub) != 0);

            FindClose (hSubFind);
          }
        }
      } while (FindNextFileW (hFind, &fd) != 0);

      FindClose (hFind);
    }

    tex_log->LogEx ( false, L" %lu files (%3.1f MiB)\n",
                       files, (double)liSize.QuadPart / (1024.0 * 1024.0) );
  }

  TBF_CreateDLLHook2 ( config.system.injector.c_str (),
                       "D3D9SetRenderState_Override",
                        D3D9SetRenderState_Detour,
              (LPVOID*)&D3D9SetRenderState );

  TBF_CreateDLLHook2 ( config.system.injector.c_str (),
                       "D3D9BeginScene_Override",
                        D3D9BeginScene_Detour,
              (LPVOID*)&D3D9BeginScene );

  TBF_CreateDLLHook2 ( config.system.injector.c_str (),
                       "D3D9StretchRect_Override",
                        D3D9StretchRect_Detour,
              (LPVOID*)&D3D9StretchRect );

  TBF_CreateDLLHook2 ( config.system.injector.c_str (),
                       "D3D9CreateDepthStencilSurface_Override",
                        D3D9CreateDepthStencilSurface_Detour,
              (LPVOID*)&D3D9CreateDepthStencilSurface );

  TBF_CreateDLLHook2 ( config.system.injector.c_str (),
                       "D3D9CreateTexture_Override",
                        D3D9CreateTexture_Detour,
              (LPVOID*)&D3D9CreateTexture );

  TBF_CreateDLLHook2 ( config.system.injector.c_str (),
                       "D3D9SetTexture_Override",
                        D3D9SetTexture_Detour,
              (LPVOID*)&D3D9SetTexture );

  TBF_CreateDLLHook2 ( config.system.injector.c_str (),
                       "D3D9SetRenderTarget_Override",
                        D3D9SetRenderTarget_Detour,
              (LPVOID*)&D3D9SetRenderTarget );

  TBF_CreateDLLHook2 ( config.system.injector.c_str (),
                       "D3D9SetDepthStencilSurface_Override",
                        D3D9SetDepthStencilSurface_Detour,
              (LPVOID*)&D3D9SetDepthStencilSurface );

  TBF_CreateDLLHook2 ( L"D3DX9_43.DLL",
                        "D3DXCreateTextureFromFileInMemoryEx",
                         D3DXCreateTextureFromFileInMemoryEx_Detour,
              (LPVOID *)&D3DXCreateTextureFromFileInMemoryEx_Original );

  TBF_ApplyQueuedHooks ();

  D3DXSaveTextureToFile =
    (D3DXSaveTextureToFile_pfn)
      GetProcAddress ( tbf::RenderFix::d3dx9_43_dll,
                         "D3DXSaveTextureToFileW" );

  D3DXSaveSurfaceToFileW =
    (D3DXSaveSurfaceToFile_pfn)
      GetProcAddress ( tbf::RenderFix::d3dx9_43_dll,
                         "D3DXSaveSurfaceToFileW" );

  D3DXCreateTextureFromFile =
    (D3DXCreateTextureFromFile_pfn)
      GetProcAddress ( tbf::RenderFix::d3dx9_43_dll,
                         "D3DXCreateTextureFromFileW" );

  D3DXCreateTextureFromFileEx =
    (D3DXCreateTextureFromFileEx_pfn)
      GetProcAddress ( tbf::RenderFix::d3dx9_43_dll,
                         "D3DXCreateTextureFromFileExW" );

  D3DXGetImageInfoFromFileInMemory =
    (D3DXGetImageInfoFromFileInMemory_pfn)
      GetProcAddress ( tbf::RenderFix::d3dx9_43_dll,
                         "D3DXGetImageInfoFromFileInMemory" );

  D3DXGetImageInfoFromFile =
    (D3DXGetImageInfoFromFile_pfn)
      GetProcAddress ( tbf::RenderFix::d3dx9_43_dll,
                         "D3DXGetImageInfoFromFileW" );

  // We don't hook this, but we still use it...
  if (D3D9CreateRenderTarget == nullptr) {
    static HMODULE hModD3D9 =
      GetModuleHandle (config.system.injector.c_str ());
    D3D9CreateRenderTarget =
      (CreateRenderTarget_pfn)
        GetProcAddress (hModD3D9, "D3D9CreateRenderTarget_Override");
  }

  // We don't hook this, but we still use it...
  if (D3D9CreateDepthStencilSurface == nullptr) {
    static HMODULE hModD3D9 =
      GetModuleHandle (config.system.injector.c_str ());
    D3D9CreateDepthStencilSurface =
      (CreateDepthStencilSurface_pfn)
        GetProcAddress (hModD3D9, "D3D9CreateDepthStencilSurface_Override");
  }

  InterlockedExchange64 (&bytes_saved, 0LL);

  time_saved  = 0.0f;

#ifdef NO_TLS
  InitializeCriticalSectionAndSpinCount (&cs_tex_inject,   10000000);
#endif
  InitializeCriticalSectionAndSpinCount (&cs_tex_resample, 100000);
  InitializeCriticalSectionAndSpinCount (&cs_tex_stream,   100000);

  decomp_semaphore = 
    CreateSemaphore ( nullptr,
                        config.textures.worker_threads,
                          config.textures.worker_threads,
                            nullptr );

  resample_pool       = new SK_TextureThreadPool ();

  stream_pool.lrg_tex = new SK_TextureThreadPool ();
  stream_pool.sm_tex  = new SK_TextureThreadPool ();

  SK_ICommandProcessor& command =
    *SK_GetCommandProcessor ();

  command.AddVariable (
    "Textures.Remap",
      TBF_CreateVar (SK_IVariable::Boolean, &__remap_textures) );

  command.AddVariable (
    "Textures.Purge",
      TBF_CreateVar (SK_IVariable::Boolean, &__need_purge) );

  command.AddVariable (
    "Textures.Trace",
      TBF_CreateVar (SK_IVariable::Boolean, &__log_used) );

  command.AddVariable (
    "Textures.ShowCache",
      TBF_CreateVar (SK_IVariable::Boolean, &__show_cache) );

  command.AddVariable (
    "Textures.MaxCacheSize",
      TBF_CreateVar (SK_IVariable::Int,     &config.textures.max_cache_in_mib) );
}

// Skip the purge step on shutdown
bool shutting_down = false;

void
tbf::RenderFix::TextureManager::Shutdown (void)
{
  // It is possible for the DLL to be unloaded before the texture manager is
  //   initialized, in which case a nullptr value for tex_log is the easiest
  //     way to detect this.
  if (tex_log == nullptr)
    return;

  // 16.6 ms per-frame (60 FPS)
  const float frame_time = 16.6f;

  while (! textures_to_stream.empty ())
    textures_to_stream.pop ();

  shutting_down = true;

  tex_mgr.reset ();

  DeleteCriticalSection (&cs_tex_stream);
  DeleteCriticalSection (&cs_tex_resample);
#ifdef NO_TLS
  DeleteCriticalSection (&cs_tex_inject);
#endif

  DeleteCriticalSection (&cs_cache);
  DeleteCriticalSection (&osd_cs);

  CloseHandle (decomp_semaphore);

  tex_log->Log ( L"[Perf Stats] At shutdown: %7.2f seconds (%7.2f frames)"
                 L" saved by cache",
                   time_saved / 1000.0f,
                     time_saved / frame_time );
  tex_log->close ();

  while (! screenshots_to_delete.empty ())
  {
    std::wstring file_to_delete = screenshots_to_delete.front ();
    screenshots_to_delete.pop ();

    DeleteFileW (file_to_delete.c_str ());
  }

  FreeLibrary (d3dx9_43_dll);
}

void
tbf::RenderFix::TextureManager::purge (void)
{
  if (shutting_down)
    return;

  int      released           = 0;
  int      released_injected  = 0;
   int64_t reclaimed          = 0;
   int64_t reclaimed_injected = 0;

  tex_log->Log (L"[ Tex. Mgr ] -- TextureManager::purge (...) -- ");

  // Purge any pending removes
  getTexture (0);

  tex_log->Log ( L"[ Tex. Mgr ]  ***  Current Cache Size: %6.2f MiB "
                                           L"(User Limit: %6.2f MiB)",
                   (double)cacheSizeTotal () / (1024.0 * 1024.0),
                     (double)config.textures.max_cache_in_mib );

  tex_log->Log (L"[ Tex. Mgr ]   Releasing textures...");

  std::unordered_map <uint32_t, tbf::RenderFix::Texture *>::iterator it =
    textures.begin ();

  std::vector <tbf::RenderFix::Texture *> unreferenced_textures;

  while (it != textures.end ()) {
    if ((*it).second->d3d9_tex->can_free)
      unreferenced_textures.push_back ((*it).second);

    ++it;
  }

  std::sort ( unreferenced_textures.begin (),
                unreferenced_textures.end (),
      []( tbf::RenderFix::Texture *a,
          tbf::RenderFix::Texture *b )
    {
      return a->d3d9_tex->last_used.QuadPart <
             b->d3d9_tex->last_used.QuadPart;
    }
  );

  std::vector <tbf::RenderFix::Texture *>::iterator free_it =
    unreferenced_textures.begin ();

  // We need to over-free, or we will likely be purging every other texture load
  int64_t target_size =
    std::max (128, config.textures.max_cache_in_mib - 64) * 1024LL * 1024LL;
  int64_t start_size =
    cacheSizeTotal ();

  while ( start_size - reclaimed > target_size &&
            free_it != unreferenced_textures.end () ) {
    int             tex_refs = -1;
    ISKTextureD3D9* pSKTex   = (*free_it)->d3d9_tex;

    //
    // Skip loads that are in-flight so that we do not hitch
    //
    if (is_streaming ((*free_it)->crc32)) {
      ++free_it;
      continue;
    }

    //
    // Do not evict blocking loads, they are generally small and
    //   will cause performance problems if we have to reload them
    //     again later.
    //
    if (pSKTex->must_block) {
      ++free_it;
      continue;
    }

    int64_t ovr_size  = 0;
    int64_t base_size = 0;

    ++free_it;

    base_size = pSKTex->tex_size;
    ovr_size  = pSKTex->override_size;
    tex_refs  = pSKTex->Release ();

    if (tex_refs == 0) {
      if (ovr_size != 0) {
        reclaimed += ovr_size;

        released_injected++;
        reclaimed_injected += ovr_size;
      }
    } else {
      tex_log->Log (L"[ Tex. Mgr ] Invalid reference count (%lu)!", tex_refs);
    }

    ++released;
    reclaimed  += base_size;
  }

  tex_log->Log ( L"[ Tex. Mgr ]   %4d textures (%4zu remain)",
                   released,
                     textures.size () );

  tex_log->Log ( L"[ Tex. Mgr ]   >> Reclaimed %6.2f MiB of memory (%6.2f MiB from %lu inject)",
                   (double)reclaimed        / (1024.0 * 1024.0),
                   (double)reclaimed_injected / (1024.0 * 1024.0),
                           released_injected );

  updateOSD ();

  tex_log->Log (L"[ Tex. Mgr ] ----------- Finished ------------ ");
}

void
tbf::RenderFix::TextureManager::reset (void)
{
  if (! outstanding_screenshots.empty ())
  {
    tex_log->LogEx (true, L"[Screenshot] A queued screenshot has not finished, delaying device reset...");

    while (! outstanding_screenshots.empty ())
      ;

    tex_log->LogEx (false, L"done!\n");
  }
  

  known.render_targets.clear ();

  int underflows       = 0;

  int ext_refs         = 0;
  int ext_textures     = 0;

  int release_count    = 0;
  int unreleased_count = 0;
  int ref_count        = 0;

  int      released_injected  = 0;
  uint64_t reclaimed          = 0;
  uint64_t reclaimed_injected = 0;

  tex_log->Log (L"[ Tex. Mgr ] -- TextureManager::reset (...) -- ");

  // Purge any pending removes
  getTexture (0);

  tex_log->Log (L"[ Tex. Mgr ]   Releasing textures...");

  std::unordered_map <uint32_t, tbf::RenderFix::Texture *>::iterator it =
    textures.begin ();

  while (it != textures.end ()) {
    ISKTextureD3D9* pSKTex =
      (*it).second->d3d9_tex;

    ++it;

    bool    can_free  = false;
    int64_t base_size = 0;
    int64_t ovr_size  = 0;

    if (pSKTex->can_free) {
      can_free = true;
      base_size = pSKTex->tex_size;
      ovr_size  = pSKTex->override_size;
    }

    else {
      ext_refs     += pSKTex->refs;
      ext_textures ++;

      ++unreleased_count;
      continue;
    }

    int tex_refs = pSKTex->Release ();

    if (tex_refs == 0) {
      if (ovr_size != 0) {
        reclaimed += ovr_size;

        released_injected++;
        reclaimed_injected += ovr_size;
      }

      ++release_count;
      reclaimed += base_size;

      ref_count += 1;
    }

    else {
      ++unreleased_count;
      ext_refs     += tex_refs;
      ext_textures ++;
    }
  }

  tex_log->Log ( L"[ Tex. Mgr ]   %4d textures (%4d references)",
                   release_count + unreleased_count,
                     ref_count + ext_refs );

  if (ext_refs > 0) {
    tex_log->Log ( L"[ Tex. Mgr ] >> WARNING: The game is still holding references (%d) to %d textures !!!",
                     ext_refs, ext_textures );
  }

  tex_log->Log ( L"[ Mem. Mgr ] === Memory Management Summary ===");

  tex_log->Log ( L"[ Mem. Mgr ]  %12.2f MiB Freed",
                   (double)reclaimed         / (1048576.0) );
  tex_log->Log ( L"[ Mem. Mgr ]  %12.2f MiB Leaked",
                   (double)(cacheSizeTotal () - reclaimed)
                                            / (1048576.0) );

  updateOSD ();

  // Commit this immediately, such that D3D9 Reset will not fail in
  //   fullscreen mode...
  TBFix_LoadQueuedTextures ();
  purge                    ();

  tex_log->Log (L"[ Tex. Mgr ] ----------- Finished ------------ ");
}

void
tbf::RenderFix::TextureManager::updateOSD (void)
{
  double cache_basic    = (double)cacheSizeBasic    () / (1048576.0f);
  double cache_injected = (double)cacheSizeInjected () / (1048576.0f);
  double cache_total    = cache_basic + cache_injected;

  osd_stats = "";

  char szFormatted [64];
  sprintf ( szFormatted, "%6zu Total Textures : %8.2f MiB",
              numTextures () + numInjectedTextures (),
                cache_total );
  osd_stats += szFormatted;

  if ( tbf::RenderFix::pDevice != nullptr &&
       tbf::RenderFix::pDevice->GetAvailableTextureMem () / 1048576UL != 4095)
    sprintf ( szFormatted, "    (%4lu MiB Available)\n",
              tbf::RenderFix::pDevice->GetAvailableTextureMem () 
                / 1048576UL );
  else
    sprintf (szFormatted, "\n");

  osd_stats += szFormatted;

  sprintf ( szFormatted, "%6zu  Base Textures : %8.2f MiB    %s\n",
              numTextures (),
                cache_basic,
                  __remap_textures ? "" : "<----" );

  osd_stats += szFormatted;

  sprintf ( szFormatted, "%6lu   New Textures : %8.2f MiB    %s\n",
              numInjectedTextures (),
                cache_injected,
                  __remap_textures ? "<----" : "" );

  osd_stats += szFormatted;

  sprintf ( szFormatted, "%6lu Cache Hits     : %8.2f Seconds Saved",
              hits,
                time_saved / 1000.0f );

  osd_stats += szFormatted;

  if (debug_tex_id != 0x00) {
    osd_stats += "\n\n";

    sprintf ( szFormatted, " Debug Texture : %08x",
                debug_tex_id );

    osd_stats += szFormatted;
  }
}

std::vector <uint32_t> textures_used_last_dump;
             uint32_t  tex_dbg_idx              = 0UL;

void
TBFix_LogUsedTextures (void)
{
  if (__log_used)
  {
    textures_used_last_dump.clear ();
    tex_dbg_idx = 0;

    tex_log->Log (L"[ Tex. Log ] ---------- FrameTrace ----------- ");

    for ( auto it  = textures_used.begin ();
               it != textures_used.end   ();
             ++it ) {
      auto tex_record =
        tbf::RenderFix::tex_mgr.getTexture (*it);

      // Handle the RARE case where a purge happens immediately following
      //   the last frame
      if ( tex_record           != nullptr &&
           tex_record->d3d9_tex != nullptr )
      {
        ISKTextureD3D9* pSKTex =
          (ISKTextureD3D9 *)tex_record->d3d9_tex;

        textures_used_last_dump.push_back (*it);

        tex_log->Log ( L"[ Tex. Log ] %08x.dds  { Base: %6.2f MiB,  "
                       L"Inject: %6.2f MiB,  Load Time: %8.3f ms }",
                         *it,
                           (double)pSKTex->tex_size /
                             (1024.0 * 1024.0),

                     pSKTex->override_size != 0 ? 
                       (double)pSKTex->override_size / 
                             (1024.0 * 1024.0) : 0.0,

                           tbf::RenderFix::tex_mgr.getTexture (*it)->load_time );
      }
    }

    tex_log->Log (L"[ Tex. Log ] ---------- FrameTrace ----------- ");

    __log_used = false;
  }

  textures_used.clear ();
}


CRITICAL_SECTION        SK_TextureWorkerThread::cs_worker_init;
ULONG                   SK_TextureWorkerThread::num_threads_init = 0UL;

HRESULT
WINAPI
ResampleTexture (tbf_tex_load_s* load)
{
  QueryPerformanceFrequency (&load->freq);
  QueryPerformanceCounter   (&load->start);

  D3DXIMAGE_INFO img_info = { };

  D3DXGetImageInfoFromFileInMemory (
    load->pSrcData,
      load->SrcDataSize,
        &img_info );

  HRESULT hr = E_FAIL;

  if (img_info.Depth == 1)
  {
    hr =
    D3DXCreateTextureFromFileInMemoryEx_Original (
      load->pDevice,
          load->pSrcData, load->SrcDataSize,
            img_info.Width, img_info.Height, 0,
              0, config.textures.uncompressed ? D3DFMT_A8R8G8B8 : img_info.Format,
                D3DPOOL_DEFAULT,
                  D3DX_FILTER_TRIANGLE | D3DX_FILTER_DITHER,
                  D3DX_FILTER_BOX      | D3DX_FILTER_DITHER,
                    0,
                      nullptr, nullptr,
                        &load->pSrc );
  }

  else
  {
    tex_log->Log (L"[ Tex. Mgr ] Will not resample cubemap...");
  }

  delete [] load->pSrcData;

  return hr;
}

unsigned int
__stdcall
SK_TextureWorkerThread::ThreadProc (LPVOID user)
{
  EnterCriticalSection (&cs_worker_init);
  {
    DWORD dwThreadId = GetCurrentThreadId ();

    if (! streaming_memory::data_len.count (dwThreadId))
    {
      streaming_memory::data_len [dwThreadId] = 0;
      streaming_memory::data     [dwThreadId] = nullptr;
      streaming_memory::data_age [dwThreadId] = 0;
    }
  }
  LeaveCriticalSection (&cs_worker_init);

  SYSTEM_INFO sysinfo;
  GetSystemInfo (&sysinfo);

  ULONG thread_num    = InterlockedIncrement (&num_threads_init);

  // If a system has more than 4 CPUs (logical or otherwise), let the last one
  //   be dedicated to rendering.
  ULONG processor_num = thread_num % ( sysinfo.dwNumberOfProcessors > 4 ?
                                         sysinfo.dwNumberOfProcessors - 1 :
                                         sysinfo.dwNumberOfProcessors );

  // Tales of Symphonia and Zestiria both pin the render thread to the last
  //   CPU... let's try to keep our worker threads OFF that CPU.

  SetThreadIdealProcessor (GetCurrentThread (),         processor_num);
  SetThreadAffinityMask   (GetCurrentThread (), (1UL << processor_num) & 0xFFFFFFFF);

  // Ghetto sync. barrier, since Windows 7 does not support them...
  while ( InterlockedCompareExchange (
            &num_threads_init,
              config.textures.worker_threads,
                config.textures.worker_threads
          ) < (ULONG)config.textures.worker_threads )
  {
    SwitchToThread ();
  }

  SK_TextureWorkerThread* pThread =
   (SK_TextureWorkerThread *)user;

  DWORD dwWaitStatus = 0;

  struct {
    const DWORD job_start  = WAIT_OBJECT_0;
    const DWORD mem_trim   = WAIT_OBJECT_0 + 1;
    const DWORD thread_end = WAIT_OBJECT_0 + 2;
  } wait;

  do {
    dwWaitStatus =
      WaitForMultipleObjects ( 3,
                                 pThread->control_.ops,
                                   FALSE,
                                     INFINITE );

    // New Work Ready
    if (dwWaitStatus == wait.job_start) {
      tbf_tex_load_s* pStream = pThread->job_;

      start_load ();
      {
        if (pStream->type == tbf_tex_load_s::Resample)
        {
          InterlockedIncrement      (&resampling);

          QueryPerformanceFrequency (&pStream->freq);
          QueryPerformanceCounter   (&pStream->start);

          HRESULT hr =
            ResampleTexture (pStream);

          QueryPerformanceCounter   (&pStream->end);

          InterlockedDecrement      (&resampling);

          if (SUCCEEDED (hr))
            pThread->pool_->postFinished (pStream);

          else {
            tex_log->Log ( L"[ Tex. Mgr ] Texture Resample Failure (hr=%x) for texture %x, blacklisting from future resamples...",
                             hr, pStream->checksum );
            resample_blacklist.insert (pStream->checksum);

            pStream->pDest->Release ();
            pStream->pSrc = pStream->pDest;

            ((ISKTextureD3D9 *)pStream->pSrc)->must_block = false;
            ((ISKTextureD3D9 *)pStream->pSrc)->refs--;

            finished_streaming (pStream->checksum);
          }

          pThread->finishJob ();
        }

        else
        {
          InterlockedIncrement        (&streaming);
          InterlockedExchangeAdd      (&streaming_bytes, pStream->SrcDataSize);

          QueryPerformanceFrequency   (&pStream->freq);
          QueryPerformanceCounter     (&pStream->start);

          HRESULT hr =
            InjectTexture (pStream);

          QueryPerformanceCounter     (&pStream->end);

          InterlockedExchangeSubtract (&streaming_bytes, pStream->SrcDataSize);
          InterlockedDecrement        (&streaming);

          if (SUCCEEDED (hr))
            pThread->pool_->postFinished (pStream);

          else
          {
            HRESULT hr = S_OK;
            tex_log->Log ( L"[ Tex. Mgr ] Texture Injection Failure (hr=%x) for texture %x, removing from injectable list...",
              hr, pStream->checksum);
            if (injectable_textures.count (pStream->checksum))
              injectable_textures.erase (pStream->checksum);

            pStream->pDest->Release ();
            pStream->pSrc = pStream->pDest;

            ((ISKTextureD3D9 *)pStream->pSrc)->must_block = false;
            ((ISKTextureD3D9 *)pStream->pSrc)->refs--;

            finished_streaming (pStream->checksum);
          }

          pThread->finishJob ();
        }
      }
      end_load ();

    }

    else if (dwWaitStatus == (wait.mem_trim))
    {
      // Yay for magic numbers :P   ==> (8 MiB Min Size, 5 Seconds Between Trims)
      //
      const size_t   MIN_SIZE = 8192 * 1024;
      const uint32_t MIN_AGE  = 5000UL;

      size_t before = streaming_memory::data_len [GetCurrentThreadId ()];

      streaming_memory::trim (MIN_SIZE, timeGetTime () - MIN_AGE);

      size_t now    =  streaming_memory::data_len [GetCurrentThreadId ()];

      if (before != now)
      {
        tex_log->Log ( L"[ Mem. Mgr ]  Trimmed %9lzu bytes of temporary memory for tid=%x",
                         before - now,
                           GetCurrentThreadId () );
      }
    }

    else if (dwWaitStatus != (wait.thread_end))
    {
      dll_log->Log ( L"[ Tex. Mgr ] Unexpected Worker Thread Wait Status: %X",
                       dwWaitStatus );
    }
  } while (dwWaitStatus != (wait.thread_end));

  streaming_memory::trim (0, timeGetTime ());

  //CloseHandle (GetCurrentThread ());
  return 0;
}

unsigned int
__stdcall
SK_TextureThreadPool::Spooler (LPVOID user)
{
  SK_TextureThreadPool* pPool =
    (SK_TextureThreadPool *)user;

  WaitForSingleObject (pPool->events_.jobs_added, INFINITE);

  while (WaitForSingleObject (pPool->events_.shutdown, 0) == WAIT_TIMEOUT) {
    tbf_tex_load_s* pJob =
      pPool->getNextJob ();

    while (pJob != nullptr) {
      auto it = pPool->workers_.begin ();

      bool started = false;

      while (it != pPool->workers_.end ()) {
        if (! (*it)->isBusy ()) {
          if (! started) {
            (*it)->startJob (pJob);
            started = true;
          } else {
            (*it)->trim ();
          }
        }

        ++it;
      }

      // All worker threads are busy, so wait...
      if (! started) {
        WaitForSingleObject (pPool->events_.results_waiting, INFINITE);
      } else {
        pJob =
          pPool->getNextJob ();
      }
    }

    const int MAX_TIME_BETWEEN_TRIMS = 1500UL;
    while ( WaitForSingleObject (
              pPool->events_.jobs_added,
                MAX_TIME_BETWEEN_TRIMS ) ==
                               WAIT_TIMEOUT ) {
      auto it = pPool->workers_.begin ();

      while (it != pPool->workers_.end ()) {
        if (! (*it)->isBusy ()) {
          (*it)->trim ();
        }

        ++it;
      }
    }
  }

  //CloseHandle (GetCurrentThread ());
  return 0;
}

void
SK_TextureWorkerThread::finishJob (void)
{
  job_ = nullptr;
}
HMODULE tbf::RenderFix::d3dx9_43_dll = 0;




void
TBF_RefreshDataSources (void)
{
  CFileInStream arc_stream;

  std::unique_ptr <CLookToRead>
                look_stream (new CLookToRead ());

  FileInStream_CreateVTable (&arc_stream);
  LookToRead_CreateVTable   (look_stream.get (), False);

  look_stream->realStream = &arc_stream.s;
  LookToRead_Init           (look_stream.get ());

  injectable_textures.clear ();
  archives.clear            ();

  //
  // Walk injectable textures so we don't have to query the filesystem on every
  //   texture load to check if a injectable one exists.
  //
  if ( GetFileAttributesW (TBFIX_TEXTURE_DIR L"\\inject") !=
         INVALID_FILE_ATTRIBUTES ) {
    WIN32_FIND_DATA fd;
    HANDLE          hFind  = INVALID_HANDLE_VALUE;
    int             files  = 0;
    LARGE_INTEGER   liSize = { 0 };

    tex_log->LogEx ( true, L"[Inject Tex] Enumerating injectable textures..." );

    hFind = FindFirstFileW (TBFIX_TEXTURE_DIR L"\\inject\\textures\\blocking\\*", &fd);

    if (hFind != INVALID_HANDLE_VALUE) {
      do {
        if (fd.dwFileAttributes != INVALID_FILE_ATTRIBUTES) {
          if (wcsstr (_wcslwr (fd.cFileName), TBFIX_TEXTURE_EXT)) {
            uint32_t checksum;
            swscanf (fd.cFileName, L"%x" TBFIX_TEXTURE_EXT, &checksum);

            // Already got this texture...
            if (injectable_textures.count (checksum))
                continue;

            ++files;

            LARGE_INTEGER fsize;

            fsize.HighPart = fd.nFileSizeHigh;
            fsize.LowPart  = fd.nFileSizeLow;

            liSize.QuadPart += fsize.QuadPart;

            tbf_tex_record_s rec;
            rec.size    = (uint32_t)fsize.QuadPart;
            rec.archive = std::numeric_limits <unsigned int>::max ();
            rec.method  = Blocking;

            injectable_textures.insert (std::make_pair (checksum, rec));
          }
        }
      } while (FindNextFileW (hFind, &fd) != 0);

      FindClose (hFind);
    }

    hFind = FindFirstFileW (TBFIX_TEXTURE_DIR L"\\inject\\textures\\streaming\\*", &fd);

    if (hFind != INVALID_HANDLE_VALUE) {
      do {
        if (fd.dwFileAttributes != INVALID_FILE_ATTRIBUTES) {
          if (wcsstr (_wcslwr (fd.cFileName), TBFIX_TEXTURE_EXT)) {
            uint32_t checksum;
            swscanf (fd.cFileName, L"%x" TBFIX_TEXTURE_EXT, &checksum);

            // Already got this texture...
            if (injectable_textures.count (checksum))
                continue;

            ++files;

            LARGE_INTEGER fsize;

            fsize.HighPart = fd.nFileSizeHigh;
            fsize.LowPart  = fd.nFileSizeLow;

            liSize.QuadPart += fsize.QuadPart;

            tbf_tex_record_s rec;
            rec.size    = (uint32_t)fsize.QuadPart;
            rec.archive = std::numeric_limits <unsigned int>::max ();
            rec.method  = Streaming;

            injectable_textures.insert (std::make_pair (checksum, rec));
          }
        }
      } while (FindNextFileW (hFind, &fd) != 0);

      FindClose (hFind);
    }

    hFind = FindFirstFileW (TBFIX_TEXTURE_DIR L"\\inject\\textures\\*", &fd);

    if (hFind != INVALID_HANDLE_VALUE) {
      do {
        if (fd.dwFileAttributes != INVALID_FILE_ATTRIBUTES) {
          if (wcsstr (_wcslwr (fd.cFileName), TBFIX_TEXTURE_EXT)) {
            uint32_t checksum;
            swscanf (fd.cFileName, L"%x" TBFIX_TEXTURE_EXT, &checksum);

            // Already got this texture...
            if (injectable_textures.count (checksum))
                continue;

            ++files;

            LARGE_INTEGER fsize;

            fsize.HighPart = fd.nFileSizeHigh;
            fsize.LowPart  = fd.nFileSizeLow;

            liSize.QuadPart += fsize.QuadPart;

            tbf_tex_record_s rec;
            rec.size    = (uint32_t)fsize.QuadPart;
            rec.archive = std::numeric_limits <unsigned int>::max ();
            rec.method  = DontCare;

            if (! injectable_textures.count (checksum))
              injectable_textures.insert (std::make_pair (checksum, rec));
          }
        }
      } while (FindNextFileW (hFind, &fd) != 0);

      FindClose (hFind);
    }

    hFind = FindFirstFileW (TBFIX_TEXTURE_DIR L"\\inject\\*.*", &fd);

    if (hFind != INVALID_HANDLE_VALUE)
    {
      int archive = 0;

      do
      {
        if (fd.dwFileAttributes != INVALID_FILE_ATTRIBUTES)
        {
          wchar_t* wszArchiveNameLwr =
            _wcslwr (_wcsdup (fd.cFileName));

          if ( wcsstr (wszArchiveNameLwr, L".7z") )
          {
            int tex_count = 0;

            CSzArEx       arc;
            ISzAlloc      thread_alloc;
            ISzAlloc      thread_tmp_alloc;

            thread_alloc.Alloc     = SzAlloc;
            thread_alloc.Free      = SzFree;

            thread_tmp_alloc.Alloc = SzAllocTemp;
            thread_tmp_alloc.Free  = SzFreeTemp;

            wchar_t wszQualifiedArchiveName [MAX_PATH];
            _swprintf ( wszQualifiedArchiveName,
                          L"%s\\inject\\%s",
                            TBFIX_TEXTURE_DIR,
                              fd.cFileName );

            if (InFile_OpenW (&arc_stream.file, wszQualifiedArchiveName))
            {
              tex_log->Log ( L"[Inject Tex]  ** Cannot open archive file: %s",
                               wszQualifiedArchiveName );
              continue;
            }

            SzArEx_Init (&arc);

            if ( SzArEx_Open ( &arc,
                                 &look_stream->s,
                                   &thread_alloc,
                                     &thread_tmp_alloc ) == SZ_OK )
            {
              uint32_t i;

              wchar_t wszEntry [MAX_PATH];

              for (i = 0; i < arc.NumFiles; i++)
              {
                if (SzArEx_IsDir (&arc, i))
                  continue;

                SzArEx_GetFileNameUtf16 (&arc, i, (UInt16 *)wszEntry);

                // Truncate to 32-bits --> there's no way in hell a texture will ever be >= 2 GiB
                size_t fileSize = SzArEx_GetFileSize (&arc, i);

                wchar_t* wszFullName =
                  _wcslwr (_wcsdup (wszEntry));

                if ( wcsstr ( wszFullName, TBFIX_TEXTURE_EXT) )
                {
                  tbf_load_method_t method = DontCare;

                  uint32_t checksum;
                  wchar_t* wszUnqualifiedEntry =
                    wszFullName + wcslen (wszFullName);

                  // Strip the path
                  while (  wszUnqualifiedEntry >= wszFullName &&
                          *wszUnqualifiedEntry != L'/')
                    wszUnqualifiedEntry--;

                  if (*wszUnqualifiedEntry == L'/')
                    ++wszUnqualifiedEntry;

                  swscanf (wszUnqualifiedEntry, L"%x" TBFIX_TEXTURE_EXT, &checksum);

                  // Already got this texture...
                  if ( injectable_textures.count (checksum) ||
                       inject_blacklist.count    (checksum) ) {
                    free (wszFullName);
                    continue;
                  }

                  if (wcsstr (wszFullName, L"streaming"))
                    method = Streaming;
                  else if (wcsstr (wszFullName, L"blocking"))
                    method = Blocking;

                  tbf_tex_record_s rec;
                  rec.size    = (uint32_t)fileSize;
                  rec.archive = archive;
                  rec.fileno  = i;
                  rec.method  = method;

                  injectable_textures.insert (std::make_pair (checksum, rec));

                  ++tex_count;
                  ++files;

                  liSize.QuadPart += rec.size;
                }

                free (wszFullName);
              }

              if (tex_count > 0) {
                ++archive;
                archives.push_back (wszQualifiedArchiveName);
              }
            }

            SzArEx_Free (&arc, &thread_alloc);
            File_Close  (&arc_stream.file);
          }

          free (wszArchiveNameLwr);
        }
      } while (FindNextFileW (hFind, &fd) != 0);

      FindClose (hFind);
    }

    tex_log->LogEx ( false, L" %lu files (%3.1f MiB)\n",
                       files, (double)liSize.QuadPart / (1024.0 * 1024.0) );
  }

  File_Close  (&arc_stream.file);
}


bool
tbf::RenderFix::TextureManager::reloadTexture (uint32_t checksum)
{
  if ( injectable_textures.find (checksum) ==
         injectable_textures.end () )
    return false;

  EnterCriticalSection        (&cs_tex_stream);

  ISKTextureD3D9* pTex = 
    getTexture (checksum)->d3d9_tex;


  if (pTex->pTexOverride != nullptr)
  {
    tex_log->LogEx ( true, L"[Inject Tex] Reloading texture for checksum (%08x)... ",
                         checksum );

    InterlockedDecrement (&injected_count);
    InterlockedAdd64     (&injected_size, -pTex->override_size);

    pTex->pTexOverride->Release ();
    pTex->pTexOverride = nullptr;
  }

  else {
    LeaveCriticalSection (&cs_tex_stream);
    return false;
  }

  tbf_tex_record_s record = injectable_textures [checksum];

  if (record.method == DontCare)
    record.method = Streaming;

  tbf_tex_load_s* load_op      = nullptr;

  wchar_t wszInjectFileName [MAX_PATH] = { L'\0' };

  bool remap_stream = is_streaming (checksum);

  // If -1, load from disk...
  if (record.archive == std::numeric_limits <unsigned int>::max ())
  {
    if (record.method == Streaming)
      _swprintf ( wszInjectFileName, L"%s\\inject\\textures\\streaming\\%08x%s",
                    TBFIX_TEXTURE_DIR,
                      checksum,
                        TBFIX_TEXTURE_EXT );
    else if (record.method == Blocking)
      _swprintf ( wszInjectFileName, L"%s\\inject\\textures\\blocking\\%08x%s",
                    TBFIX_TEXTURE_DIR,
                      checksum,
                        TBFIX_TEXTURE_EXT );
  }

  load_op           = new tbf_tex_load_s;
  load_op->pDevice  = tbf::RenderFix::pDevice;
  load_op->checksum = checksum;

  load_op->type = tbf_tex_load_s::Stream;

  wcscpy (load_op->wszFilename, wszInjectFileName);

  if (load_op->type == tbf_tex_load_s::Stream)
  {
    if ((! remap_stream))
      tex_log->LogEx ( false, L"streaming\n" );
    else
      tex_log->LogEx ( false, L"in-flight already\n" );
  }

  load_op->SrcDataSize =
    injectable_textures.count (checksum) == 0 ?
      0 : (UINT)injectable_textures [checksum].size;

  load_op->pDest = pTex;

  pTex->must_block = false;

  if (is_streaming (load_op->checksum))
  {
    ISKTextureD3D9* pTexOrig =
      (ISKTextureD3D9 *)textures_in_flight [load_op->checksum]->pDest;

    // Remap the output of the in-flight texture
    textures_in_flight [load_op->checksum]->pDest =
      pTex;

    if (getTexture (load_op->checksum) != nullptr)
    {
      for ( int i = 0;
                i < getTexture (load_op->checksum)->refs;
              ++i ) {
        pTex->AddRef ();
      }
    }
  }

  else
  {
    textures_in_flight.insert ( std::make_pair ( load_op->checksum,
                                 load_op ) );

    stream_pool.postJob (load_op);
  }

  LeaveCriticalSection        (&cs_tex_stream);

  if (pending_loads ())
    TBFix_LoadQueuedTextures ();

  return true;
}

void 
tbf::RenderFix::TextureManager::queueScreenshot ( wchar_t* wszFileName,
                                                  bool     hudless )
{
  want_screenshot = true;
  //screenshot_file = wszFileName;
}

bool
tbf::RenderFix::TextureManager::wantsScreenshot (void)
{
  return want_screenshot;
}

HRESULT
tbf::RenderFix::TextureManager::takeScreenshot (IDirect3DSurface9* pSurf)
{
  static int count = 0;

  wchar_t wszOut   [MAX_PATH] = { };
  wchar_t wszThumb [MAX_PATH] = { };
   char    szOut   [MAX_PATH] = { };
   char    szThumb [MAX_PATH] = { };

   CreateDirectoryW (L"Screenshots", nullptr);

          time_t now;
   struct tm*    now_tm;

                time (&now);
  now_tm = localtime (&now);

  const wchar_t* wszTimestamp = config.screenshots.keep ?
     L"Screenshots\\TBFix_NoHUD_%Y_%m_%d-%H'%M'%S.png" :
     L"Screenshots\\TBFix_NoHUD_%Y_%m_%d-%H'%M'%S.tga";

   wcsftime (wszOut, MAX_PATH,  wszTimestamp, now_tm);
  _swprintf (wszThumb, L"Screenshots\\TBFix_NoHUD_Thumbnail%lu.tga", count++ );

  GetCurrentDirectoryA (MAX_PATH, szOut);
  GetCurrentDirectoryA (MAX_PATH, szThumb);

    sprintf ( szOut,   "%s\\%ws",  szOut,   wszOut);
    sprintf ( szThumb, "%s\\%ws",  szThumb, wszThumb);

  if (! config.screenshots.keep)
    screenshots_to_delete.push (wszOut);

  if (config.screenshots.import_to_steam)
    screenshots_to_delete.push (wszThumb);

  want_screenshot = false;

  D3DSURFACE_DESC  desc;
  pSurf->GetDesc (&desc);

  static D3DXLoadSurfaceFromSurface_pfn
    D3DXLoadSurfaceFromSurface =
      (D3DXLoadSurfaceFromSurface_pfn)
        GetProcAddress ( d3dx9_43_dll, "D3DXLoadSurfaceFromSurface" );

  extern HMODULE hInjectorDLL;
  
  typedef void (WINAPI *SK_SteamAPI_AddScreenshotToLibrary_pfn)
    (const char *pchFilename, const char *pchThumbnailFilename, int nWidth, int nHeight);
  
  static SK_SteamAPI_AddScreenshotToLibrary_pfn
    SK_SteamAPI_AddScreenshotToLibrary =
      (SK_SteamAPI_AddScreenshotToLibrary_pfn)
        GetProcAddress ( hInjectorDLL,
                           "SK_SteamAPI_AddScreenshotToLibrary" );

  IDirect3DSurface9* pSurfScreenshot = nullptr;

  if (SUCCEEDED ( D3D9CreateRenderTarget ( tbf::RenderFix::pDevice,
                                             desc.Width, desc.Height,
                                               desc.Format, desc.MultiSampleType, desc.MultiSampleQuality,
                                                 TRUE,
                                                   &pSurfScreenshot, nullptr
                                         )
                )
     )
  {
    if ( SUCCEEDED ( D3D9StretchRect ( tbf::RenderFix::pDevice,
                                         pSurf,           nullptr,
                                         pSurfScreenshot, nullptr,
                                           D3DTEXF_NONE
                                     )
                   )
       )
    {
      outstanding_screenshots.insert (pSurfScreenshot);

      struct screenshot_params_s {
        D3DSURFACE_DESC    desc;
        IDirect3DSurface9* pSurf;
        std::wstring       name_w, thumbnail_w;
        std::string        name, thumbnail;
      } *params =

        new screenshot_params_s {
          desc,
            pSurfScreenshot,
              wszOut, wszThumb,
               szOut,  szThumb
        };

      CreateThread ( nullptr, 0,
                       [](LPVOID user) ->
                       DWORD
      {
        D3DXIMAGE_FILEFORMAT format = config.screenshots.keep ?
           D3DXIFF_PNG : D3DXIFF_TGA;

        screenshot_params_s* params =
          (screenshot_params_s *)user;

        HRESULT hr =
          D3DXSaveSurfaceToFileW ( params->name_w.c_str (),
                                     format, params->pSurf,
                                       nullptr, nullptr );

        bool with_thumbnail = false;

        if (config.screenshots.import_to_steam && SUCCEEDED (hr))
        {
          CComPtr <IDirect3DSurface9> pSurfThumb = nullptr;

          if (SUCCEEDED ( D3D9CreateRenderTarget ( tbf::RenderFix::pDevice,
                                                     200, (UINT)(((float)params->desc.Height / (float)params->desc.Width) * 200),
                                                       params->desc.Format,
                                                         D3DMULTISAMPLE_NONE, 0,
                                                           TRUE, &pSurfThumb,
                                                             nullptr
                                                 )
                        )
             )
          {
            outstanding_screenshots.insert (pSurfThumb);

            // Slightly higher quality filtering than if we just used StretchRect with a linear filter,+
            //   (200 x ...) pixels still sucks, but we can make it suck just a little bit less.
            if (SUCCEEDED ( D3DXLoadSurfaceFromSurface ( pSurfThumb,
                                                           nullptr, nullptr,
                                                             params->pSurf,
                                                               nullptr, nullptr,
                                                                 D3DX_DEFAULT, 0xFF000000
                                                       )
                          )
               )
            {
              if ( SUCCEEDED (
                     D3DXSaveSurfaceToFileW ( params->thumbnail_w.c_str (),
                                                D3DXIFF_TGA, pSurfThumb,
                                                  nullptr, nullptr
                                            )
                             )
                 )
              {
                SK_SteamAPI_AddScreenshotToLibrary (params->name.c_str (), params->thumbnail.c_str (), params->desc.Width, params->desc.Height);
                with_thumbnail = true;
              }
            }

            outstanding_screenshots.erase (pSurfThumb);
          }

          if (! with_thumbnail)
            SK_SteamAPI_AddScreenshotToLibrary (params->name.c_str (), nullptr, params->desc.Width, params->desc.Height);
        }

        outstanding_screenshots.erase (params->pSurf);

        params->pSurf->Release ();

        delete params;
      
        CloseHandle (GetCurrentThread ());
      
        return 0;
      },

      params,
        0,
          nullptr
      );
    } 

    else {
      pSurfScreenshot->Release ();
      return E_FAIL;
    }
  }

  return S_OK;
}