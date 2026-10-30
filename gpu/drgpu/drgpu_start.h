/*
 * a5xx start sequence as HMD's kernel wrote it on this phone (ftrace
 * kgsl_regwrite, research/vendor-adreno/trace/ftrace-cold.txt lines 272-483:
 * a5xx_hwcg_set + a5xx_start, second power-on). Generated, not hand-typed.
 * Left out: CP_CONTEXT_SWITCH_SMMU_INFO_LO/HI (0x840/0x841), a preemption
 * buffer address in the kernel's GPU address space.
 */
static const uint32_t a5xx_start_seq[][2] = {
	{ 0x67, 0x55 },	/* RBBM_CLOCK_CNTL */
	{ 0x68, 0x2222222 },	/* RBBM_CLOCK_CNTL_SP0 */
	{ 0x69, 0x2222222 },	/* RBBM_CLOCK_CNTL_SP1 */
	{ 0x6c, 0x2222220 },	/* RBBM_CLOCK_CNTL2_SP0 */
	{ 0x6d, 0x2222220 },	/* RBBM_CLOCK_CNTL2_SP1 */
	{ 0x70, 0xf3cf },	/* RBBM_CLOCK_HYST_SP0 */
	{ 0x71, 0xf3cf },	/* RBBM_CLOCK_HYST_SP1 */
	{ 0x74, 0x80 },	/* RBBM_CLOCK_DELAY_SP0 */
	{ 0x75, 0x80 },	/* RBBM_CLOCK_DELAY_SP1 */
	{ 0x4a, 0x22222222 },	/* RBBM_CLOCK_CNTL_TP0 */
	{ 0x4b, 0x22222222 },	/* RBBM_CLOCK_CNTL_TP1 */
	{ 0x4e, 0x22222222 },	/* RBBM_CLOCK_CNTL2_TP0 */
	{ 0x4f, 0x22222222 },	/* RBBM_CLOCK_CNTL2_TP1 */
	{ 0x52, 0x2222 },	/* RBBM_CLOCK_CNTL3_TP0 */
	{ 0x53, 0x2222 },	/* RBBM_CLOCK_CNTL3_TP1 */
	{ 0xb0, 0x77777777 },	/* RBBM_CLOCK_HYST_TP0 */
	{ 0xb1, 0x77777777 },	/* RBBM_CLOCK_HYST_TP1 */
	{ 0xb4, 0x77777777 },	/* RBBM_CLOCK_HYST2_TP0 */
	{ 0xb5, 0x77777777 },	/* RBBM_CLOCK_HYST2_TP1 */
	{ 0xb8, 0x7777 },	/* RBBM_CLOCK_HYST3_TP0 */
	{ 0xb9, 0x7777 },	/* RBBM_CLOCK_HYST3_TP1 */
	{ 0xa4, 0x11111111 },	/* RBBM_CLOCK_DELAY_TP0 */
	{ 0xa5, 0x11111111 },	/* RBBM_CLOCK_DELAY_TP1 */
	{ 0xa8, 0x11111111 },	/* RBBM_CLOCK_DELAY2_TP0 */
	{ 0xa9, 0x11111111 },	/* RBBM_CLOCK_DELAY2_TP1 */
	{ 0xac, 0x1111 },	/* RBBM_CLOCK_DELAY3_TP0 */
	{ 0xad, 0x1111 },	/* RBBM_CLOCK_DELAY3_TP1 */
	{ 0x5a, 0x22222222 },	/* RBBM_CLOCK_CNTL_UCHE */
	{ 0x5b, 0x22222222 },	/* RBBM_CLOCK_CNTL2_UCHE */
	{ 0x5c, 0x22222222 },	/* RBBM_CLOCK_CNTL3_UCHE */
	{ 0x5d, 0x222222 },	/* RBBM_CLOCK_CNTL4_UCHE */
	{ 0x5e, 0x444444 },	/* RBBM_CLOCK_HYST_UCHE */
	{ 0x5f, 0x2 },	/* RBBM_CLOCK_DELAY_UCHE */
	{ 0x78, 0x22222222 },	/* RBBM_CLOCK_CNTL_RB0 */
	{ 0x79, 0x22222222 },	/* RBBM_CLOCK_CNTL_RB1 */
	{ 0x7c, 0x222222 },	/* RBBM_CLOCK_CNTL2_RB0 */
	{ 0x7d, 0x222222 },	/* RBBM_CLOCK_CNTL2_RB1 */
	{ 0x82, 0x22220 },	/* RBBM_CLOCK_CNTL_CCU0 */
	{ 0x83, 0x22220 },	/* RBBM_CLOCK_CNTL_CCU1 */
	{ 0x8a, 0x5522222 },	/* RBBM_CLOCK_CNTL_RAC */
	{ 0x8b, 0x505555 },	/* RBBM_CLOCK_CNTL2_RAC */
	{ 0x86, 0x4040404 },	/* RBBM_CLOCK_HYST_RB_CCU0 */
	{ 0x87, 0x4040404 },	/* RBBM_CLOCK_HYST_RB_CCU1 */
	{ 0x80, 0x7444044 },	/* RBBM_CLOCK_HYST_RAC */
	{ 0x8c, 0x2 },	/* RBBM_CLOCK_DELAY_RB_CCU_L1_0 */
	{ 0x8d, 0x2 },	/* RBBM_CLOCK_DELAY_RB_CCU_L1_1 */
	{ 0x81, 0x10011 },	/* RBBM_CLOCK_DELAY_RAC */
	{ 0x63, 0x4222222 },	/* RBBM_CLOCK_CNTL_TSE_RAS_RBBM */
	{ 0x60, 0x2222222 },	/* RBBM_CLOCK_MODE_GPC */
	{ 0x91, 0x2222 },	/* RBBM_CLOCK_MODE_VFD */
	{ 0x64, 0x0 },	/* RBBM_CLOCK_HYST_TSE_RAS_RBBM */
	{ 0x62, 0x4104004 },	/* RBBM_CLOCK_HYST_GPC */
	{ 0x90, 0x0 },	/* RBBM_CLOCK_HYST_VFD */
	{ 0x66, 0x0 },	/* RBBM_CLOCK_DELAY_HLSQ */
	{ 0x65, 0x4000 },	/* RBBM_CLOCK_DELAY_TSE_RAS_RBBM */
	{ 0x61, 0x200 },	/* RBBM_CLOCK_DELAY_GPC */
	{ 0x92, 0x2222 },	/* RBBM_CLOCK_DELAY_VFD */
	{ 0x67, 0xaaa8aa00 },	/* RBBM_CLOCK_CNTL */
	{ 0x533, 0x182 },	/* RBBM_ISDB_CNT */
	{ 0x67, 0xaaa82a00 },	/* RBBM_CLOCK_CNTL */
	{ 0xf803, 0x0 },	/* RBBM_SECVID_TSB_CNTL */
	{ 0xf800, 0xe8000000 },	/* RBBM_SECVID_TSB_TRUSTED_BASE_LO */
	{ 0xf801, 0x0 },	/* RBBM_SECVID_TSB_TRUSTED_BASE_HI */
	{ 0xf802, 0x10000000 },	/* RBBM_SECVID_TSB_TRUSTED_SIZE */
	{ 0x860, 0x1 },	/* CP_ADDR_MODE_CNTL */
	{ 0xbc1, 0x1 },	/* VSC_ADDR_MODE_CNTL */
	{ 0xc81, 0x1 },	/* GRAS_ADDR_MODE_CNTL */
	{ 0xcc5, 0x1 },	/* RB_ADDR_MODE_CNTL */
	{ 0xd01, 0x1 },	/* PC_ADDR_MODE_CNTL */
	{ 0xe05, 0x1 },	/* HLSQ_ADDR_MODE_CNTL */
	{ 0xe41, 0x1 },	/* VFD_ADDR_MODE_CNTL */
	{ 0xe61, 0x1 },	/* VPC_ADDR_MODE_CNTL */
	{ 0xe80, 0x1 },	/* UCHE_ADDR_MODE_CNTL */
	{ 0xec1, 0x1 },	/* SP_ADDR_MODE_CNTL */
	{ 0xf01, 0x1 },	/* TPL1_ADDR_MODE_CNTL */
	{ 0xf810, 0x1 },	/* RBBM_SECVID_TSB_ADDR_MODE_CNTL */
	{ 0x46b, 0x6 },	/* RBBM_PERFCTR_RBBM_SEL_0 */
	{ 0x3108, 0x1 },
	{ 0x3108, 0x0 },
	{ 0x3100, 0x1 },	/* VBIF_PERF_PWR_CNT_EN0 */
	{ 0x30cb, 0x1 },
	{ 0x30cb, 0x0 },
	{ 0x30d3, 0x55 },	/* VBIF_PERF_CNT_SEL3 */
	{ 0x30c3, 0x1 },
	{ 0x469, 0x0 },	/* RBBM_PERFCTR_LOAD_VALUE_LO */
	{ 0x46a, 0x0 },	/* RBBM_PERFCTR_LOAD_VALUE_HI */
	{ 0x466, 0x8000 },	/* RBBM_PERFCTR_LOAD_CMD1 */
	{ 0x469, 0x0 },	/* RBBM_PERFCTR_LOAD_VALUE_LO */
	{ 0x46a, 0x0 },	/* RBBM_PERFCTR_LOAD_VALUE_HI */
	{ 0x467, 0x2000 },	/* RBBM_PERFCTR_LOAD_CMD2 */
	{ 0x469, 0x0 },	/* RBBM_PERFCTR_LOAD_VALUE_LO */
	{ 0x46a, 0x0 },	/* RBBM_PERFCTR_LOAD_VALUE_HI */
	{ 0x467, 0x4000 },	/* RBBM_PERFCTR_LOAD_CMD2 */
	{ 0x469, 0x0 },	/* RBBM_PERFCTR_LOAD_VALUE_LO */
	{ 0x46a, 0x0 },	/* RBBM_PERFCTR_LOAD_VALUE_HI */
	{ 0x467, 0x8000 },	/* RBBM_PERFCTR_LOAD_CMD2 */
	{ 0x3049, 0x3 },	/* VBIF_ROUND_ROBIN_QOS_ARB */
	{ 0x302a, 0x9 },	/* VBIF_GATE_OFF_WRREQ_EN */
	{ 0x46f, 0xffffffff },	/* RBBM_PERFCTR_GPU_BUSY_MASKED */
	{ 0x93, 0x1 },	/* RBBM_AHB_CNTL0 */
	{ 0x2f, 0x4003ffff },	/* RBBM_INTERFACE_HANG_INT_CNTL */
	{ 0x464, 0x1 },	/* RBBM_PERFCTR_CNTL */
	{ 0xe96, 0x2 },	/* UCHE_CACHE_WAYS */
	{ 0xe89, 0xffff0000 },	/* UCHE_TRAP_BASE_LO */
	{ 0xe8a, 0x1ffff },	/* UCHE_TRAP_BASE_HI */
	{ 0xe87, 0xffff0000 },	/* UCHE_WRITE_THRU_BASE_LO */
	{ 0xe88, 0x1ffff },	/* UCHE_WRITE_THRU_BASE_HI */
	{ 0xe8b, 0x100000 },	/* UCHE_GMEM_RANGE_MIN_LO */
	{ 0xe8c, 0x0 },	/* UCHE_GMEM_RANGE_MIN_HI */
	{ 0xe8d, 0x143fff },	/* UCHE_GMEM_RANGE_MAX_LO */
	{ 0xe8e, 0x0 },	/* UCHE_GMEM_RANGE_MAX_HI */
	{ 0x825, 0x40 },	/* CP_MEQ_THRESHOLDS */
	{ 0x826, 0x400 },	/* CP_MERCIU_SIZE */
	{ 0x820, 0x80000060 },	/* CP_ROQ_THRESHOLDS_2 */
	{ 0x81f, 0x40201b16 },	/* CP_ROQ_THRESHOLDS_1 */
	{ 0xd00, 0x80100000 },	/* PC_DBG_ECO_CNTL */
	{ 0xcc4, 0x100200 },	/* RB_DBG_ECO_CNT */
	{ 0xe81, 0x20000000 },	/* UCHE_MODE_CNTL */
	{ 0x833, 0x2000000 },	/* CP_CHICKEN_DBG */
	{ 0x94, 0xa6ffffff },	/* RBBM_AHB_CNTL1 */
	{ 0x95, 0x3f },	/* RBBM_AHB_CNTL2 */
	{ 0xf02, 0x80 },	/* TPL1_MODE_CNTL */
	{ 0xcc6, 0x2 },	/* RB_MODE_CNTL */
	{ 0xe8f, 0x1 },	/* UCHE_DBG_ECO_CNTL_2 */
	{ 0xe60, 0x400 },	/* VPC_DBG_ECO_CNTL */
	{ 0xe60, 0x800400 },	/* VPC_DBG_ECO_CNTL */
	{ 0xe04, 0x0 },	/* HLSQ_DBG_ECO_CNTL */
	{ 0x8a0, 0x7 },	/* CP_PROTECT_CNTL */
	{ 0x880, 0x62000010 },	/* CP_PROTECT_REG_0 */
	{ 0x881, 0x63000020 },
	{ 0x882, 0x64000040 },
	{ 0x883, 0x65000080 },
	{ 0x884, 0x66000100 },
	{ 0x885, 0x66000200 },
	{ 0x886, 0x6403e000 },
	{ 0x887, 0x6103d000 },
	{ 0x888, 0x66002000 },
	{ 0x889, 0x63002100 },
	{ 0x88a, 0x65002200 },
	{ 0x88b, 0x60002a80 },
	{ 0x88c, 0x60003300 },
	{ 0x88d, 0x610033c0 },
	{ 0x88e, 0x630039a0 },
	{ 0x88f, 0x640039c0 },
	{ 0x890, 0x64003a00 },
	{ 0x891, 0x6e040000 },
	{ 0x38, 0x33d003fe },	/* RBBM_INT_0_MASK */
	{ 0xc93, 0x6 },	/* GRAS_PERFCTR_TSE_SEL_3 */
	{ 0xed9, 0x27 },	/* SP_PERFCTR_SP_SEL_9 */
	{ 0xeda, 0x35 },	/* SP_PERFCTR_SP_SEL_10 */
	{ 0xedb, 0x1 },	/* SP_PERFCTR_SP_SEL_11 */
	{ 0x46b, 0x6 },	/* RBBM_PERFCTR_RBBM_SEL_0 */
	{ 0x30cb, 0x1 },
	{ 0x30cb, 0x0 },
	{ 0x30d3, 0x55 },	/* VBIF_PERF_CNT_SEL3 */
	{ 0x30c3, 0x1 },
	{ 0x3108, 0x1 },
	{ 0x3108, 0x0 },
	{ 0x3100, 0x1 },	/* VBIF_PERF_PWR_CNT_EN0 */
};
