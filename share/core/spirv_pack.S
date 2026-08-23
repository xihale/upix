#if defined(__APPLE__)
.section __DATA,__const
#else
.section .rodata
#endif
.global crossscale_spirv_pack_blob
.global crossscale_spirv_pack_blob_end
crossscale_spirv_pack_blob:
.incbin "spirv_pack.bin"
crossscale_spirv_pack_blob_end:
