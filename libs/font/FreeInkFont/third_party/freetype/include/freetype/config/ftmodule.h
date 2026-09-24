/* FreeInkFont curated module list: TrueType (+GX variations) via sfnt and the
 * smooth (anti-aliased) renderer always; everything else is opt-in. The
 * classic B/W raster and the auto-hinter (FREEINK_FONT_ENABLE_MONOCHROME /
 * FREEINK_FONT_ENABLE_AUTOHINT) since most consumers render grayscale-AA only
 * and never need FT_LOAD_FORCE_AUTOHINT to actually hint anything; psnames
 * (FREEINK_FONT_ENABLE_PSNAMES) since it serves glyph-NAME lookups
 * (FT_Get_Glyph_Name / AGL Unicode synthesis), which a codepoint-driven cmap
 * renderer never performs — at ~64KB of text (mostly the Adobe Glyph List)
 * it would be the largest module in this build, with no TrueType
 * functionality attached. No CFF/Type1/BDF/etc. */
#if FREEINK_FONT_ENABLE_PSNAMES
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
#endif
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
#if FREEINK_FONT_ENABLE_MONOCHROME
FT_USE_MODULE( FT_Renderer_Class, ft_raster1_renderer_class )
#endif
#if FREEINK_FONT_ENABLE_AUTOHINT
FT_USE_MODULE( FT_Module_Class, autofit_module_class )
#endif
