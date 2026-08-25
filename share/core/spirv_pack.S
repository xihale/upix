#if defined(__APPLE__)
.section __DATA,__const
#else
.section .rodata
#endif
.global upix_spirv_pack_blob
.global upix_spirv_pack_blob_end
upix_spirv_pack_blob:
.incbin "spirv_pack.bin"
upix_spirv_pack_blob_end:
