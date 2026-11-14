#if defined(__APPLE__)
.section __TEXT,__const
.p2align 2
.globl _upix_spirv_pack_blob
.globl _upix_spirv_pack_blob_end
_upix_spirv_pack_blob:
.incbin "spirv_pack.bin"
_upix_spirv_pack_blob_end:
#else
.section .rodata
.global upix_spirv_pack_blob
.global upix_spirv_pack_blob_end
upix_spirv_pack_blob:
.incbin "spirv_pack.bin"
upix_spirv_pack_blob_end:
#endif
#if defined(__linux__) && defined(__ELF__)
.section .note.GNU-stack,"",%progbits
#endif
