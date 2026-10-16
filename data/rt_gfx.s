	.section .rodata
	.balign 4
	.global gRtSpriteGfx
gRtSpriteGfx:
	.incbin "graphics/rt/sprites.bin"
	.balign 4
	.global gRtFxAtlasGfx
gRtFxAtlasGfx:
	.incbin "graphics/rt/fx_atlas.bin"
	.balign 4
