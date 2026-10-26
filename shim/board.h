#pragma once
/*
 * Board constants. Default is sweet (Redmi Note 10 Pro, SM7150); build with
 * -DBOARD_DRG for the Nokia 6.1 Plus (DRG, SDM636). DRG values come from its
 * live FDT (OrangeFox/TWRP /sys/firmware/fdt).
 */

#ifdef BOARD_DRG
#define BOARD_NAME		"drg"
#define FB_BASE			0x9d400000UL	/* reserved-memory splash_region */
#define FB_H			2280
#define IMEM_RESTART_REASON	0x146bf65cUL	/* qcom,msm-imem@146bf000 restart_reason@65c */
#define PSHOLD			0x010ac000UL	/* restart@10ac000 qcom,pshold */
#define GICR_BASE		0x17b00000UL
#define WDT_BASE		0x17817000UL	/* qcom,msm-watchdog */
#define WDT_EN			0x8
#else
#define BOARD_NAME		"sweet"
#define FB_BASE			0x9c000000UL	/* reserved-memory cont_splash_region */
#define FB_H			2400
#define IMEM_RESTART_REASON	0x146aa65cUL	/* qcom,msm-imem restart_reason@65c */
#define PSHOLD			0x0c264000UL
#define GICR_BASE		0x17a60000UL
#endif

#define FB_W			1080
#define FB_PHYS			FB_BASE
