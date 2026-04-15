// plutosvg_ft_override.cpp
//
// The installed libplutosvg.a was compiled without FreeType support, so the
// exported plutosvg_ft_svg_hooks() function always returns nullptr.  That
// prevents imgui_freetype.cpp from registering the SVG renderer with
// FreeType, causing every OT-SVG glyph (e.g. NotoColorEmoji) to fail with
// FT_Err_Cannot_Render_Glyph and fall back to the tofu glyph ◆.
//
// Fix: redirect the symbol via -Wl,--wrap=plutosvg_ft_svg_hooks (see
// CMakeLists.txt).  The real call lands here and returns the hooks struct
// built from the header-only FreeType integration in <plutosvg/plutosvg-ft.h>.

// plutosvg-ft.h contains C-style casts and C stdlib headers — silence the
// subset of -Wall warnings that would otherwise fire in -std=c++23 mode.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wunused-function"
#include <plutosvg/plutosvg-ft.h>  // defines static SVG_RendererHooks plutosvg_ft_hooks
#pragma GCC diagnostic pop

// The linker wrapper symbol is exposed via an asm label so the C++ identifier
// itself stays out of the reserved __* namespace.
extern "C" const void* wrap_plutosvg_ft_svg_hooks() __asm__("__wrap_plutosvg_ft_svg_hooks");

// imgui_freetype.cpp passes the return value directly to FT_Property_Set.
extern "C" const void* wrap_plutosvg_ft_svg_hooks()
{
    return &plutosvg_ft_hooks;
}
