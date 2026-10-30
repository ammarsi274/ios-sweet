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
