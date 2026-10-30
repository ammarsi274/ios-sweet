/*
 * Nokia's CP microcode for the Adreno 512 (vendor_b /vendor/firmware,
 * not in git: build-drg.sh checks the SHA-256 and passes the directory).
 * Laid out as HMD's kgsl _load_firmware() does: the file without its first
 * dword, page aligned (the image is loaded at a page-aligned address and
 * runs with the MMU off, so these are physical addresses the CP can fetch).
 */
	.section .rodata.gpufw, "a"
	.balign	4096
	.global	drg_pm4_fw, drg_pm4_fw_end
drg_pm4_fw:
	.incbin	"a530_pm4.fw", 4
drg_pm4_fw_end:
	.balign	4096
	.global	drg_pfp_fw, drg_pfp_fw_end
drg_pfp_fw:
	.incbin	"a530_pfp.fw", 4
drg_pfp_fw_end:
	.balign	4096

/*
 * Nokia's zap shader (a512_zap, PAS id 0xd): the .mdt (ELF header, program
 * headers and hash segment, handed to TZ as metadata) and the one loadable
 * segment, .b02. Laid out and loaded by drg_gpu.c as HMD's PIL does.
 */
	.balign	8
	.global	drg_zap_mdt, drg_zap_mdt_end
drg_zap_mdt:
	.incbin	"a512_zap.mdt"
drg_zap_mdt_end:
	.balign	8
	.global	drg_zap_b02, drg_zap_b02_end
drg_zap_b02:
	.incbin	"a512_zap.b02"
drg_zap_b02_end:
	.balign	8

/*
 * GPU work captured from Nokia's driver in TWRP (research/vendor-adreno/
 * capture/cap1.bin, drgles bench -capture; kept out of git, hash checked by
 * build-drg.sh): replayed by drg_gpu.c (drg/GPU-IOS-DESIGN.md gate I2).
 */
	.balign	8
	.global	drg_cap, drg_cap_end
drg_cap:
	.incbin	"cap1.bin"
drg_cap_end:
	.balign	8
