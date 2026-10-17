	.section .rodata
	.balign 4
	.global gRtSpriteGfx
gRtSpriteGfx:
	.incbin "graphics/rt/sprites_lc.bin"
	.balign 4
	.global gRtFxAtlasGfx
gRtFxAtlasGfx:
	.incbin "graphics/rt/fx_atlas.bin"
	.balign 4
	.global gRtFxDynGfx
gRtFxDynGfx:
	.incbin "graphics/rt/fx_dyn.bin"
	.balign 4
