
// no color mapping
#define R_DRAWCOLUMN_IRAM_MAYBE
#define R_DRAWCOLUMN_FUNCNAME R_DRAWCOLUMN_FUNCNAME_COMPOSITE(_PointUV)
#define R_DRAWCOLUMN_PIPELINE (R_DRAWCOLUMN_PIPELINE_BASE | RDC_NOCOLMAP)
#include "r_drawcolumn.inl"

// simple depth color mapping, point-filtered: this is the variant actually used
// for ordinary wall/floor rendering every frame (default filter settings), so it's
// the only one worth making IRAM-resident, and only for the standard pipeline
// (not translucent/translated/fuzz, which are comparatively rare sprite effects).
#undef R_DRAWCOLUMN_IRAM_MAYBE
#if (R_DRAWCOLUMN_PIPELINE_TYPE) == RDC_PIPELINE_STANDARD
#define R_DRAWCOLUMN_IRAM_MAYBE IRAM_ATTR
#else
#undef R_DRAWCOLUMN_IRAM_MAYBE
#define R_DRAWCOLUMN_IRAM_MAYBE
#endif
#define R_DRAWCOLUMN_FUNCNAME R_DRAWCOLUMN_FUNCNAME_COMPOSITE(_PointUV_PointZ)
#define R_DRAWCOLUMN_PIPELINE R_DRAWCOLUMN_PIPELINE_BASE
#include "r_drawcolumn.inl"

// z-dither
#undef R_DRAWCOLUMN_IRAM_MAYBE
#define R_DRAWCOLUMN_IRAM_MAYBE
#define R_DRAWCOLUMN_FUNCNAME R_DRAWCOLUMN_FUNCNAME_COMPOSITE(_PointUV_LinearZ)
#define R_DRAWCOLUMN_PIPELINE (R_DRAWCOLUMN_PIPELINE_BASE | RDC_DITHERZ)
#include "r_drawcolumn.inl"

// bilinear with no color mapping
#undef R_DRAWCOLUMN_IRAM_MAYBE
#define R_DRAWCOLUMN_IRAM_MAYBE
#define R_DRAWCOLUMN_FUNCNAME R_DRAWCOLUMN_FUNCNAME_COMPOSITE(_LinearUV)
#define R_DRAWCOLUMN_PIPELINE (R_DRAWCOLUMN_PIPELINE_BASE | RDC_BILINEAR | RDC_NOCOLMAP)
#include "r_drawcolumn.inl"

// bilinear with simple depth color mapping
#undef R_DRAWCOLUMN_IRAM_MAYBE
#define R_DRAWCOLUMN_IRAM_MAYBE
#define R_DRAWCOLUMN_FUNCNAME R_DRAWCOLUMN_FUNCNAME_COMPOSITE(_LinearUV_PointZ)
#define R_DRAWCOLUMN_PIPELINE (R_DRAWCOLUMN_PIPELINE_BASE | RDC_BILINEAR)
#include "r_drawcolumn.inl"

// bilinear + z-dither
#undef R_DRAWCOLUMN_IRAM_MAYBE
#define R_DRAWCOLUMN_IRAM_MAYBE
#define R_DRAWCOLUMN_FUNCNAME R_DRAWCOLUMN_FUNCNAME_COMPOSITE(_LinearUV_LinearZ)
#define R_DRAWCOLUMN_PIPELINE (R_DRAWCOLUMN_PIPELINE_BASE | RDC_BILINEAR | RDC_DITHERZ)
#include "r_drawcolumn.inl"

// rounded with no color mapping
#undef R_DRAWCOLUMN_IRAM_MAYBE
#define R_DRAWCOLUMN_IRAM_MAYBE
#define R_DRAWCOLUMN_FUNCNAME R_DRAWCOLUMN_FUNCNAME_COMPOSITE(_RoundedUV)
#define R_DRAWCOLUMN_PIPELINE (R_DRAWCOLUMN_PIPELINE_BASE | RDC_ROUNDED | RDC_NOCOLMAP)
#include "r_drawcolumn.inl"

// rounded with simple depth color mapping
#undef R_DRAWCOLUMN_IRAM_MAYBE
#define R_DRAWCOLUMN_IRAM_MAYBE
#define R_DRAWCOLUMN_FUNCNAME R_DRAWCOLUMN_FUNCNAME_COMPOSITE(_RoundedUV_PointZ)
#define R_DRAWCOLUMN_PIPELINE (R_DRAWCOLUMN_PIPELINE_BASE | RDC_ROUNDED)
#include "r_drawcolumn.inl"

// rounded + z-dither
#undef R_DRAWCOLUMN_IRAM_MAYBE
#define R_DRAWCOLUMN_IRAM_MAYBE
#define R_DRAWCOLUMN_FUNCNAME R_DRAWCOLUMN_FUNCNAME_COMPOSITE(_RoundedUV_LinearZ)
#define R_DRAWCOLUMN_PIPELINE (R_DRAWCOLUMN_PIPELINE_BASE | RDC_ROUNDED | RDC_DITHERZ)
#include "r_drawcolumn.inl"

#undef R_DRAWCOLUMN_IRAM_MAYBE
#undef R_FLUSHWHOLE_FUNCNAME
#undef R_FLUSHHEADTAIL_FUNCNAME
#undef R_FLUSHQUAD_FUNCNAME
#undef R_DRAWCOLUMN_FUNCNAME_COMPOSITE
#undef R_DRAWCOLUMN_PIPELINE_BITS
