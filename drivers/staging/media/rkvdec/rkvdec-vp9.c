// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip Video Decoder VP9 backend
 *
 * Copyright (C) 2019 Collabora, Ltd.
 *	Boris Brezillon <boris.brezillon@collabora.com>
 *
 * Copyright (C) 2016 Rockchip Electronics Co., Ltd.
 *	Alpha Lin <Alpha.Lin@rock-chips.com>
 */

#include <linux/kernel.h>
#include <linux/vmalloc.h>
#include <media/v4l2-mem2mem.h>

#include "rkvdec.h"
#include "rkvdec-regs.h"

#define RKVDEC_VP9_PROBE_SIZE		4864
#define RKVDEC_VP9_COUNT_SIZE		13232
#define RKVDEC_VP9_MAX_SEGMAP_SIZE	73728

struct rkvdec_vp9_intra_mode_probs {
	u8 y_mode[105];
	u8 uv_mode[23];
};

struct rkvdec_vp9_intra_only_frame_probs {
	u8 coef_intra[4][2][128];
	struct rkvdec_vp9_intra_mode_probs intra_mode[10];
};

struct rkvdec_vp9_inter_frame_probs {
	u8 y_mode[4][9];
	u8 comp_mode[5];
	u8 comp_ref[5];
	u8 single_ref[5][2];
	u8 inter_mode[7][3];
	u8 interp_filter[4][2];
	u8 padding0[11];
	u8 coef[2][4][2][128];
	u8 uv_mode_0_2[3][9];
	u8 padding1[5];
	u8 uv_mode_3_5[3][9];
	u8 padding2[5];
	u8 uv_mode_6_8[3][9];
	u8 padding3[5];
	u8 uv_mode_9[9];
	u8 padding4[7];
	u8 padding5[16];
	struct {
		u8 joint[3];
		u8 sign[2];
		u8 class[2][10];
		u8 class0_bit[2];
		u8 bits[2][10];
		u8 class0_fr[2][2][3];
		u8 fr[2][3];
		u8 class0_hp[2];
		u8 hp[2];
	} mv;
};

struct rkvdec_vp9_probs {
	u8 partition[16][3];
	u8 pred[3];
	u8 tree[7];
	u8 skip[3];
	u8 tx32[2][3];
	u8 tx16[2][2];
	u8 tx8[2][1];
	u8 is_inter[4];
	/* 128 bit alignment */
	u8 padding0[3];
	union {
		struct rkvdec_vp9_inter_frame_probs inter;
		struct rkvdec_vp9_intra_only_frame_probs intra_only;
	};
};

/* Data structure describing auxiliary buffer format. */
struct rkvdec_vp9_priv_tbl {
	struct rkvdec_vp9_probs probs;
	u8 segmap[2][RKVDEC_VP9_MAX_SEGMAP_SIZE];
};

struct rkvdec_vp9_refs_counts {
	u32 eob[2];
	u32 coeff[3];
};

struct rkvdec_vp9_inter_frame_symbol_counts {
	u32 partition[16][4];
	u32 skip[3][2];
	u32 inter[4][2];
	u32 tx32p[2][4];
	u32 tx16p[2][4];
	u32 tx8p[2][2];
	u32 y_mode[4][10];
	u32 uv_mode[10][10];
	u32 comp[5][2];
	u32 comp_ref[5][2];
	u32 single_ref[5][2][2];
	u32 mv_mode[7][4];
	u32 filter[4][3];
	u32 mv_joint[4];
	u32 sign[2][2];
	/* add 1 element for align */
	u32 classes[2][11 + 1];
	u32 class0[2][2];
	u32 bits[2][10][2];
	u32 class0_fp[2][2][4];
	u32 fp[2][4];
	u32 class0_hp[2][2];
	u32 hp[2][2];
	struct rkvdec_vp9_refs_counts ref_cnt[2][4][2][6][6];
};

struct rkvdec_vp9_intra_frame_symbol_counts {
	u32 partition[4][4][4];
	u32 skip[3][2];
	u32 intra[4][2];
	u32 tx32p[2][4];
	u32 tx16p[2][4];
	u32 tx8p[2][2];
	struct rkvdec_vp9_refs_counts ref_cnt[2][4][2][6][6];
};

struct rkvdec_vp9_run {
	struct rkvdec_run base;
	const struct v4l2_ctrl_vp9_frame_decode_params *decode_params;
};

struct rkvdec_vp9_frame_info {
	u32 valid : 1;
	u32 segmapid : 1;
	u32 frame_context_idx : 2;
	u32 reference_mode : 2;
	u32 tx_mode : 3;
	u32 interpolation_filter : 3;
	u32 flags;
	u64 timestamp;
	struct v4l2_vp9_segmentation seg;
	struct v4l2_vp9_loop_filter lf;
};

struct rkvdec_vp9_ctx {
	struct rkvdec_aux_buf priv_tbl;
	struct rkvdec_aux_buf count_tbl;
	struct v4l2_ctrl_vp9_frame_ctx frame_context;
	struct rkvdec_vp9_frame_info cur;
	struct rkvdec_vp9_frame_info last;
};

static u32 rkvdec_fastdiv(u32 dividend, u16 divisor)
{
#define DIV_INV(d)	(u32)(((1ULL << 32) + ((d) - 1)) / (d))
#define DIVS_INV(d0, d1, d2, d3, d4, d5, d6, d7, d8, d9)	\
	DIV_INV(d0), DIV_INV(d1), DIV_INV(d2), DIV_INV(d3),	\
	DIV_INV(d4), DIV_INV(d5), DIV_INV(d6), DIV_INV(d7),	\
	DIV_INV(d8), DIV_INV(d9)

	static const u32 inv[] = {
		DIV_INV(2), DIV_INV(3), DIV_INV(4), DIV_INV(5),
		DIV_INV(6), DIV_INV(7), DIV_INV(8), DIV_INV(9),
		DIVS_INV(10, 11, 12, 13, 14, 15, 16, 17, 18, 19),
		DIVS_INV(20, 21, 22, 23, 24, 25, 26, 27, 28, 29),
		DIVS_INV(30, 31, 32, 33, 34, 35, 36, 37, 38, 39),
		DIVS_INV(40, 41, 42, 43, 44, 45, 46, 47, 48, 49),
		DIVS_INV(50, 51, 52, 53, 54, 55, 56, 57, 58, 59),
		DIVS_INV(60, 61, 62, 63, 64, 65, 66, 67, 68, 69),
		DIVS_INV(70, 71, 72, 73, 74, 75, 76, 77, 78, 79),
		DIVS_INV(80, 81, 82, 83, 84, 85, 86, 87, 88, 89),
		DIVS_INV(90, 91, 92, 93, 94, 95, 96, 97, 98, 99),
		DIVS_INV(100, 101, 102, 103, 104, 105, 106, 107, 108, 109),
		DIVS_INV(110, 111, 112, 113, 114, 115, 116, 117, 118, 119),
		DIVS_INV(120, 121, 122, 123, 124, 125, 126, 127, 128, 129),
		DIVS_INV(130, 131, 132, 133, 134, 135, 136, 137, 138, 139),
		DIVS_INV(140, 141, 142, 143, 144, 145, 146, 147, 148, 149),
		DIVS_INV(150, 151, 152, 153, 154, 155, 156, 157, 158, 159),
		DIVS_INV(160, 161, 162, 163, 164, 165, 166, 167, 168, 169),
		DIVS_INV(170, 171, 172, 173, 174, 175, 176, 177, 178, 179),
		DIVS_INV(180, 181, 182, 183, 184, 185, 186, 187, 188, 189),
		DIVS_INV(190, 191, 192, 193, 194, 195, 196, 197, 198, 199),
		DIVS_INV(200, 201, 202, 203, 204, 205, 206, 207, 208, 209),
		DIVS_INV(210, 211, 212, 213, 214, 215, 216, 217, 218, 219),
		DIVS_INV(220, 221, 222, 223, 224, 225, 226, 227, 228, 229),
		DIVS_INV(230, 231, 232, 233, 234, 235, 236, 237, 238, 239),
		DIVS_INV(240, 241, 242, 243, 244, 245, 246, 247, 248, 249),
		DIV_INV(250), DIV_INV(251), DIV_INV(252), DIV_INV(253),
		DIV_INV(254), DIV_INV(255), DIV_INV(256),
	};

	if (divisor == 0)
		return 0;
	else if (divisor == 1)
		return dividend;

	if (WARN_ON(divisor - 2 >= ARRAY_SIZE(inv)))
		return dividend;

	return ((u64)dividend * inv[divisor - 2]) >> 32;
}

static const u8 vp9_kf_y_mode_prob[10][10][9] = {
	{
		/* above = dc */
		{ 137,  30,  42, 148, 151, 207,  70,  52,  91 },/*left = dc  */
		{  92,  45, 102, 136, 116, 180,  74,  90, 100 },/*left = v   */
		{  73,  32,  19, 187, 222, 215,  46,  34, 100 },/*left = h   */
		{  91,  30,  32, 116, 121, 186,  93,  86,  94 },/*left = d45 */
		{  72,  35,  36, 149,  68, 206,  68,  63, 105 },/*left = d135*/
		{  73,  31,  28, 138,  57, 124,  55, 122, 151 },/*left = d117*/
		{  67,  23,  21, 140, 126, 197,  40,  37, 171 },/*left = d153*/
		{  86,  27,  28, 128, 154, 212,  45,  43,  53 },/*left = d207*/
		{  74,  32,  27, 107,  86, 160,  63, 134, 102 },/*left = d63 */
		{  59,  67,  44, 140, 161, 202,  78,  67, 119 } /*left = tm  */
	}, {  /* above = v */
		{  63,  36, 126, 146, 123, 158,  60,  90,  96 },/*left = dc  */
		{  43,  46, 168, 134, 107, 128,  69, 142,  92 },/*left = v   */
		{  44,  29,  68, 159, 201, 177,  50,  57,  77 },/*left = h   */
		{  58,  38,  76, 114,  97, 172,  78, 133,  92 },/*left = d45 */
		{  46,  41,  76, 140,  63, 184,  69, 112,  57 },/*left = d135*/
		{  38,  32,  85, 140,  46, 112,  54, 151, 133 },/*left = d117*/
		{  39,  27,  61, 131, 110, 175,  44,  75, 136 },/*left = d153*/
		{  52,  30,  74, 113, 130, 175,  51,  64,  58 },/*left = d207*/
		{  47,  35,  80, 100,  74, 143,  64, 163,  74 },/*left = d63 */
		{  36,  61, 116, 114, 128, 162,  80, 125,  82 } /*left = tm  */
	}, {  /* above = h */
		{  82,  26,  26, 171, 208, 204,  44,  32, 105 },/*left = dc  */
		{  55,  44,  68, 166, 179, 192,  57,  57, 108 },/*left = v   */
		{  42,  26,  11, 199, 241, 228,  23,  15,  85 },/*left = h   */
		{  68,  42,  19, 131, 160, 199,  55,  52,  83 },/*left = d45 */
		{  58,  50,  25, 139, 115, 232,  39,  52, 118 },/*left = d135*/
		{  50,  35,  33, 153, 104, 162,  64,  59, 131 },/*left = d117*/
		{  44,  24,  16, 150, 177, 202,  33,  19, 156 },/*left = d153*/
		{  55,  27,  12, 153, 203, 218,  26,  27,  49 },/*left = d207*/
		{  53,  49,  21, 110, 116, 168,  59,  80,  76 },/*left = d63 */
		{  38,  72,  19, 168, 203, 212,  50,  50, 107 } /*left = tm  */
	}, {  /* above = d45 */
		{ 103,  26,  36, 129, 132, 201,  83,  80,  93 },/*left = dc  */
		{  59,  38,  83, 112, 103, 162,  98, 136,  90 },/*left = v   */
		{  62,  30,  23, 158, 200, 207,  59,  57,  50 },/*left = h   */
		{  67,  30,  29,  84,  86, 191, 102,  91,  59 },/*left = d45 */
		{  60,  32,  33, 112,  71, 220,  64,  89, 104 },/*left = d135*/
		{  53,  26,  34, 130,  56, 149,  84, 120, 103 },/*left = d117*/
		{  53,  21,  23, 133, 109, 210,  56,  77, 172 },/*left = d153*/
		{  77,  19,  29, 112, 142, 228,  55,  66,  36 },/*left = d207*/
		{  61,  29,  29,  93,  97, 165,  83, 175, 162 },/*left = d63 */
		{  47,  47,  43, 114, 137, 181, 100,  99,  95 } /*left = tm  */
	}, {  /* above = d135 */
		{  69,  23,  29, 128,  83, 199,  46,  44, 101 },/*left = dc  */
		{  53,  40,  55, 139,  69, 183,  61,  80, 110 },/*left = v   */
		{  40,  29,  19, 161, 180, 207,  43,  24,  91 },/*left = h   */
		{  60,  34,  19, 105,  61, 198,  53,  64,  89 },/*left = d45 */
		{  52,  31,  22, 158,  40, 209,  58,  62,  89 },/*left = d135*/
		{  44,  31,  29, 147,  46, 158,  56, 102, 198 },/*left = d117*/
		{  35,  19,  12, 135,  87, 209,  41,  45, 167 },/*left = d153*/
		{  55,  25,  21, 118,  95, 215,  38,  39,  66 },/*left = d207*/
		{  51,  38,  25, 113,  58, 164,  70,  93,  97 },/*left = d63 */
		{  47,  54,  34, 146, 108, 203,  72, 103, 151 } /*left = tm  */
	}, {  /* above = d117 */
		{  64,  19,  37, 156,  66, 138,  49,  95, 133 },/*left = dc  */
		{  46,  27,  80, 150,  55, 124,  55, 121, 135 },/*left = v   */
		{  36,  23,  27, 165, 149, 166,  54,  64, 118 },/*left = h   */
		{  53,  21,  36, 131,  63, 163,  60, 109,  81 },/*left = d45 */
		{  40,  26,  35, 154,  40, 185,  51,  97, 123 },/*left = d135*/
		{  35,  19,  34, 179,  19,  97,  48, 129, 124 },/*left = d117*/
		{  36,  20,  26, 136,  62, 164,  33,  77, 154 },/*left = d153*/
		{  45,  18,  32, 130,  90, 157,  40,  79,  91 },/*left = d207*/
		{  45,  26,  28, 129,  45, 129,  49, 147, 123 },/*left = d63 */
		{  38,  44,  51, 136,  74, 162,  57,  97, 121 } /*left = tm  */
	}, {  /* above = d153 */
		{  75,  17,  22, 136, 138, 185,  32,  34, 166 },/*left = dc  */
		{  56,  39,  58, 133, 117, 173,  48,  53, 187 },/*left = v   */
		{  35,  21,  12, 161, 212, 207,  20,  23, 145 },/*left = h   */
		{  56,  29,  19, 117, 109, 181,  55,  68, 112 },/*left = d45 */
		{  47,  29,  17, 153,  64, 220,  59,  51, 114 },/*left = d135*/
		{  46,  16,  24, 136,  76, 147,  41,  64, 172 },/*left = d117*/
		{  34,  17,  11, 108, 152, 187,  13,  15, 209 },/*left = d153*/
		{  51,  24,  14, 115, 133, 209,  32,  26, 104 },/*left = d207*/
		{  55,  30,  18, 122,  79, 179,  44,  88, 116 },/*left = d63 */
		{  37,  49,  25, 129, 168, 164,  41,  54, 148 } /*left = tm  */
	}, {  /* above = d207 */
		{  82,  22,  32, 127, 143, 213,  39,  41,  70 },/*left = dc  */
		{  62,  44,  61, 123, 105, 189,  48,  57,  64 },/*left = v   */
		{  47,  25,  17, 175, 222, 220,  24,  30,  86 },/*left = h   */
		{  68,  36,  17, 106, 102, 206,  59,  74,  74 },/*left = d45 */
		{  57,  39,  23, 151,  68, 216,  55,  63,  58 },/*left = d135*/
		{  49,  30,  35, 141,  70, 168,  82,  40, 115 },/*left = d117*/
		{  51,  25,  15, 136, 129, 202,  38,  35, 139 },/*left = d153*/
		{  68,  26,  16, 111, 141, 215,  29,  28,  28 },/*left = d207*/
		{  59,  39,  19, 114,  75, 180,  77, 104,  42 },/*left = d63 */
		{  40,  61,  26, 126, 152, 206,  61,  59,  93 } /*left = tm  */
	}, {  /* above = d63 */
		{  78,  23,  39, 111, 117, 170,  74, 124,  94 },/*left = dc  */
		{  48,  34,  86, 101,  92, 146,  78, 179, 134 },/*left = v   */
		{  47,  22,  24, 138, 187, 178,  68,  69,  59 },/*left = h   */
		{  56,  25,  33, 105, 112, 187,  95, 177, 129 },/*left = d45 */
		{  48,  31,  27, 114,  63, 183,  82, 116,  56 },/*left = d135*/
		{  43,  28,  37, 121,  63, 123,  61, 192, 169 },/*left = d117*/
		{  42,  17,  24, 109,  97, 177,  56,  76, 122 },/*left = d153*/
		{  58,  18,  28, 105, 139, 182,  70,  92,  63 },/*left = d207*/
		{  46,  23,  32,  74,  86, 150,  67, 183,  88 },/*left = d63 */
		{  36,  38,  48,  92, 122, 165,  88, 137,  91 } /*left = tm  */
	}, {  /* above = tm */
		{  65,  70,  60, 155, 159, 199,  61,  60,  81 },/*left = dc  */
		{  44,  78, 115, 132, 119, 173,  71, 112,  93 },/*left = v   */
		{  39,  38,  21, 184, 227, 206,  42,  32,  64 },/*left = h   */
		{  58,  47,  36, 124, 137, 193,  80,  82,  78 },/*left = d45 */
		{  49,  50,  35, 144,  95, 205,  63,  78,  59 },/*left = d135*/
		{  41,  53,  52, 148,  71, 142,  65, 128,  51 },/*left = d117*/
		{  40,  36,  28, 143, 143, 202,  40,  55, 137 },/*left = d153*/
		{  52,  34,  29, 129, 183, 227,  42,  35,  43 },/*left = d207*/
		{  42,  44,  44, 104, 105, 164,  64, 130,  80 },/*left = d63 */
		{  43,  81,  53, 140, 169, 204,  68,  84,  72 } /*left = tm  */
	}
};

static const u8 kf_partition_probs[16][3] = {
	/* 8x8 -> 4x4 */
	{ 158,  97,  94 },	/* a/l both not split   */
	{  93,  24,  99 },	/* a split, l not split */
	{  85, 119,  44 },	/* l split, a not split */
	{  62,  59,  67 },	/* a/l both split       */
	/* 16x16 -> 8x8 */
	{ 149,  53,  53 },	/* a/l both not split   */
	{  94,  20,  48 },	/* a split, l not split */
	{  83,  53,  24 },	/* l split, a not split */
	{  52,  18,  18 },	/* a/l both split       */
	/* 32x32 -> 16x16 */
	{ 150,  40,  39 },	/* a/l both not split   */
	{  78,  12,  26 },	/* a split, l not split */
	{  67,  33,  11 },	/* l split, a not split */
	{  24,   7,   5 },	/* a/l both split       */
	/* 64x64 -> 32x32 */
	{ 174,  35,  49 },	/* a/l both not split   */
	{  68,  11,  27 },	/* a split, l not split */
	{  57,  15,   9 },	/* l split, a not split */
	{  12,   3,   3 },	/* a/l both split       */
};

static const u8 kf_uv_mode_prob[10][9] = {
	{ 144,  11,  54, 157, 195, 130,  46,  58, 108 },  /* y = dc   */
	{ 118,  15, 123, 148, 131, 101,  44,  93, 131 },  /* y = v    */
	{ 113,  12,  23, 188, 226, 142,  26,  32, 125 },  /* y = h    */
	{ 120,  11,  50, 123, 163, 135,  64,  77, 103 },  /* y = d45  */
	{ 113,   9,  36, 155, 111, 157,  32,  44, 161 },  /* y = d135 */
	{ 116,   9,  55, 176,  76,  96,  37,  61, 149 },  /* y = d117 */
	{ 115,   9,  28, 141, 161, 167,  21,  25, 193 },  /* y = d153 */
	{ 120,  12,  32, 145, 195, 142,  32,  38,  86 },  /* y = d207 */
	{ 116,  12,  64, 120, 140, 125,  49, 115, 121 },  /* y = d63  */
	{ 102,  19,  66, 162, 182, 122,  35,  59, 128 }   /* y = tm   */
};

static void write_coeff_plane(const u8 coef[6][6][3], u8 *coeff_plane)
{
	unsigned int idx = 0;
	u8 byte_count = 0, p;
	s32 k, m, n;

	for (k = 0; k < 6; k++) {
		for (m = 0; m < 6; m++) {
			for (n = 0; n < 3; n++) {
				p = coef[k][m][n];
				coeff_plane[idx++] = p;
				byte_count++;
				if (byte_count == 27) {
					idx += 5;
					byte_count = 0;
				}
			}
		}
	}
}

static void init_intra_only_probs(struct rkvdec_ctx *ctx,
				  const struct rkvdec_vp9_run *run)
{
	const struct v4l2_ctrl_vp9_frame_decode_params *dec_params;
	struct rkvdec_vp9_ctx *vp9_ctx = ctx->priv;
	struct rkvdec_vp9_priv_tbl *tbl = vp9_ctx->priv_tbl.cpu;
	struct rkvdec_vp9_intra_only_frame_probs *rkprobs;
	const struct v4l2_vp9_probabilities *probs;
	unsigned int i, j, k, m;

	rkprobs = &tbl->probs.intra_only;
	dec_params = run->decode_params;
	probs = &dec_params->probs;

	/*
	 * intra only 149 x 128 bits ,aligned to 152 x 128 bits coeff related
	 * prob 64 x 128 bits
	 */
	for (i = 0; i < ARRAY_SIZE(probs->coef); i++) {
		for (j = 0; j < ARRAY_SIZE(probs->coef[0]); j++)
			write_coeff_plane(probs->coef[i][j][0],
					  rkprobs->coef_intra[i][j]);
	}

	/* intra mode prob  80 x 128 bits */
	for (i = 0; i < ARRAY_SIZE(vp9_kf_y_mode_prob); i++) {
		u32 byte_count = 0;
		int idx = 0;

		/* vp9_kf_y_mode_prob */
		for (j = 0; j < ARRAY_SIZE(vp9_kf_y_mode_prob[0]); j++) {
			for (k = 0; k < ARRAY_SIZE(vp9_kf_y_mode_prob[0][0]);
			     k++) {
				u8 val = vp9_kf_y_mode_prob[i][j][k];

				rkprobs->intra_mode[i].y_mode[idx++] = val;
				byte_count++;
				if (byte_count == 27) {
					byte_count = 0;
					idx += 5;
				}
			}
		}

		idx = 0;
		if (i < 4) {
			for (m = 0; m < (i < 3 ? 23 : 21); m++) {
				const u8 *ptr = &kf_uv_mode_prob[0][0];

				rkprobs->intra_mode[i].uv_mode[idx++] = ptr[i * 23 + m];
			}
		}
	}
}

static void init_inter_probs(struct rkvdec_ctx *ctx,
			     const struct rkvdec_vp9_run *run)
{
	const struct v4l2_ctrl_vp9_frame_decode_params *dec_params;
	struct rkvdec_vp9_ctx *vp9_ctx = ctx->priv;
	struct rkvdec_vp9_priv_tbl *tbl = vp9_ctx->priv_tbl.cpu;
	struct rkvdec_vp9_inter_frame_probs *rkprobs;
	const struct v4l2_vp9_probabilities *probs;
	unsigned int i, j, k;

	rkprobs = &tbl->probs.inter;
	dec_params = run->decode_params;
	probs = &dec_params->probs;

	/*
	 * inter probs
	 * 151 x 128 bits, aligned to 152 x 128 bits
	 * inter only
	 * intra_y_mode & inter_block info 6 x 128 bits
	 */

	memcpy(rkprobs->y_mode, probs->y_mode, sizeof(rkprobs->y_mode));
	memcpy(rkprobs->comp_mode, probs->comp_mode,
	       sizeof(rkprobs->comp_mode));
	memcpy(rkprobs->comp_ref, probs->comp_ref,
	       sizeof(rkprobs->comp_ref));
	memcpy(rkprobs->single_ref, probs->single_ref,
	       sizeof(rkprobs->single_ref));
	memcpy(rkprobs->inter_mode, probs->inter_mode,
	       sizeof(rkprobs->inter_mode));
	memcpy(rkprobs->interp_filter, probs->interp_filter,
	       sizeof(rkprobs->interp_filter));

	/* 128 x 128 bits coeff related */
	for (i = 0; i < ARRAY_SIZE(probs->coef); i++) {
		for (j = 0; j < ARRAY_SIZE(probs->coef[0]); j++) {
			for (k = 0; k < ARRAY_SIZE(probs->coef[0][0]); k++)
				write_coeff_plane(probs->coef[i][j][k],
						  rkprobs->coef[k][i][j]);
		}
	}

	/* intra uv mode 6 x 128 */
	memcpy(rkprobs->uv_mode_0_2, &probs->uv_mode[0],
	       sizeof(rkprobs->uv_mode_0_2));
	memcpy(rkprobs->uv_mode_3_5, &probs->uv_mode[3],
	       sizeof(rkprobs->uv_mode_3_5));
	memcpy(rkprobs->uv_mode_6_8, &probs->uv_mode[6],
	       sizeof(rkprobs->uv_mode_6_8));
	memcpy(rkprobs->uv_mode_9, &probs->uv_mode[9],
	       sizeof(rkprobs->uv_mode_9));

	/* mv related 6 x 128 */
	memcpy(rkprobs->mv.joint, probs->mv.joint,
	       sizeof(rkprobs->mv.joint));
	memcpy(rkprobs->mv.sign, probs->mv.sign,
	       sizeof(rkprobs->mv.sign));
	memcpy(rkprobs->mv.class, probs->mv.class,
	       sizeof(rkprobs->mv.class));
	memcpy(rkprobs->mv.class0_bit, probs->mv.class0_bit,
	       sizeof(rkprobs->mv.class0_bit));
	memcpy(rkprobs->mv.bits, probs->mv.bits,
	       sizeof(rkprobs->mv.bits));
	memcpy(rkprobs->mv.class0_fr, probs->mv.class0_fr,
	       sizeof(rkprobs->mv.class0_fr));
	memcpy(rkprobs->mv.fr, probs->mv.fr,
	       sizeof(rkprobs->mv.fr));
	memcpy(rkprobs->mv.class0_hp, probs->mv.class0_hp,
	       sizeof(rkprobs->mv.class0_hp));
	memcpy(rkprobs->mv.hp, probs->mv.hp,
	       sizeof(rkprobs->mv.hp));
}

static void init_probs(struct rkvdec_ctx *ctx,
		       const struct rkvdec_vp9_run *run)
{
	const struct v4l2_ctrl_vp9_frame_decode_params *dec_params;
	struct rkvdec_vp9_ctx *vp9_ctx = ctx->priv;
	struct rkvdec_vp9_priv_tbl *tbl = vp9_ctx->priv_tbl.cpu;
	struct rkvdec_vp9_probs *rkprobs = &tbl->probs;
	const struct v4l2_vp9_segmentation *seg;
	const struct v4l2_vp9_probabilities *probs;
	bool intra_only;

	dec_params = run->decode_params;
	probs = &dec_params->probs;
	seg = &dec_params->seg;

	memset(rkprobs, 0, sizeof(*rkprobs));

	intra_only = !!(dec_params->flags &
			(V4L2_VP9_FRAME_FLAG_KEY_FRAME |
			 V4L2_VP9_FRAME_FLAG_INTRA_ONLY));

	/* sb info  5 x 128 bit */
	memcpy(rkprobs->partition,
	       intra_only ? kf_partition_probs : probs->partition,
	       sizeof(rkprobs->partition));

	memcpy(rkprobs->pred, seg->pred_probs, sizeof(rkprobs->pred));
	memcpy(rkprobs->tree, seg->tree_probs, sizeof(rkprobs->tree));
	memcpy(rkprobs->skip, probs->skip, sizeof(rkprobs->skip));
	memcpy(rkprobs->tx32, probs->tx32, sizeof(rkprobs->tx32));
	memcpy(rkprobs->tx16, probs->tx16, sizeof(rkprobs->tx16));
	memcpy(rkprobs->tx8, probs->tx8, sizeof(rkprobs->tx8));
	memcpy(rkprobs->is_inter, probs->is_inter, sizeof(rkprobs->is_inter));

	if (intra_only)
		init_intra_only_probs(ctx, run);
	else
		init_inter_probs(ctx, run);
}

struct vp9d_ref_config {
	u32 reg_frm_size;
	u32 reg_hor_stride;
	u32 reg_y_stride;
	u32 reg_yuv_stride;
	u32 reg_ref_base;
};

static struct vp9d_ref_config ref_config[3] = {
	{
		.reg_frm_size = RKVDEC_REG_VP9_FRAME_SIZE(0),
		.reg_hor_stride = RKVDEC_VP9_HOR_VIRSTRIDE(0),
		.reg_y_stride = RKVDEC_VP9_LAST_FRAME_YSTRIDE,
		.reg_yuv_stride = RKVDEC_VP9_LAST_FRAME_YUVSTRIDE,
		.reg_ref_base = RKVDEC_REG_VP9_LAST_FRAME_BASE,
	},
	{
		.reg_frm_size = RKVDEC_REG_VP9_FRAME_SIZE(1),
		.reg_hor_stride = RKVDEC_VP9_HOR_VIRSTRIDE(1),
		.reg_y_stride = RKVDEC_VP9_GOLDEN_FRAME_YSTRIDE,
		.reg_yuv_stride = 0,
		.reg_ref_base = RKVDEC_REG_VP9_GOLDEN_FRAME_BASE,
	},
	{
		.reg_frm_size = RKVDEC_REG_VP9_FRAME_SIZE(2),
		.reg_hor_stride = RKVDEC_VP9_HOR_VIRSTRIDE(2),
		.reg_y_stride = RKVDEC_VP9_ALTREF_FRAME_YSTRIDE,
		.reg_yuv_stride = 0,
		.reg_ref_base = RKVDEC_REG_VP9_ALTREF_FRAME_BASE,
	}
};

static struct rkvdec_decoded_buffer *
get_ref_buf(struct rkvdec_ctx *ctx, struct vb2_v4l2_buffer *dst, u64 timestamp)
{
	struct v4l2_m2m_ctx *m2m_ctx = ctx->fh.m2m_ctx;
	struct vb2_queue *cap_q = &m2m_ctx->cap_q_ctx.q;
	int buf_idx;

	/*
	 * If a ref is unused or invalid, address of current destination
	 * buffer is returned.
	 */
	buf_idx = vb2_find_timestamp(cap_q, timestamp, 0);
	if (buf_idx < 0)
		return vb2_to_rkvdec_decoded_buf(&dst->vb2_buf);

	return vb2_to_rkvdec_decoded_buf(vb2_get_buffer(cap_q, buf_idx));
}

static dma_addr_t get_mv_base_addr(struct rkvdec_decoded_buffer *buf)
{
	u32 aligned_pitch, aligned_height, yuv_len;

	aligned_height = round_up(buf->vp9.height, 64);
	aligned_pitch = round_up(buf->vp9.width * buf->vp9.bit_depth, 512) / 8;
	yuv_len = (aligned_height * aligned_pitch * 3) / 2;

	return vb2_dma_contig_plane_dma_addr(&buf->base.vb.vb2_buf, 0) +
	       yuv_len;
}

static void
config_ref_registers(struct rkvdec_ctx *ctx,
		     const struct rkvdec_vp9_run *run,
		     struct rkvdec_decoded_buffer **ref_bufs,
		     enum v4l2_vp9_ref_id id)
{
	u32 aligned_pitch, aligned_height, y_len, yuv_len;
	struct rkvdec_decoded_buffer *buf = ref_bufs[id];
	struct rkvdec_dev *rkvdec = ctx->dev;

	aligned_height = round_up(buf->vp9.height, 64);
	writel_relaxed(RKVDEC_VP9_FRAMEWIDTH(buf->vp9.width) |
		       RKVDEC_VP9_FRAMEHEIGHT(buf->vp9.height),
		       rkvdec->regs + ref_config[id].reg_frm_size);

	writel_relaxed(vb2_dma_contig_plane_dma_addr(&buf->base.vb.vb2_buf, 0),
		       rkvdec->regs + ref_config[id].reg_ref_base);

	if (&buf->base.vb == run->base.bufs.dst)
		return;

	aligned_pitch = round_up(buf->vp9.width * buf->vp9.bit_depth, 512) / 8;
	y_len = aligned_height * aligned_pitch;
	yuv_len = (y_len * 3) / 2;

	writel_relaxed(RKVDEC_HOR_Y_VIRSTRIDE(aligned_pitch / 16) |
		       RKVDEC_HOR_UV_VIRSTRIDE(aligned_pitch / 16),
		       rkvdec->regs + ref_config[id].reg_hor_stride);
	writel_relaxed(RKVDEC_VP9_REF_YSTRIDE(y_len / 16),
		       rkvdec->regs + ref_config[id].reg_y_stride);

	if (!ref_config[id].reg_yuv_stride)
		return;

	writel_relaxed(RKVDEC_VP9_REF_YUVSTRIDE(yuv_len / 16),
		       rkvdec->regs + ref_config[id].reg_yuv_stride);
}

static bool seg_featured_enabled(const struct v4l2_vp9_segmentation *seg,
				 enum v4l2_vp9_segment_feature feature,
				 unsigned int segid)
{
	u8 mask = V4L2_VP9_SEGMENT_FEATURE_ENABLED(feature);

	return !!(seg->feature_enabled[segid] & mask);
}

static void
config_seg_registers(struct rkvdec_ctx *ctx,
		     unsigned int segid)
{
	struct rkvdec_vp9_ctx *vp9_ctx = ctx->priv;
	const struct v4l2_vp9_segmentation *seg;
	struct rkvdec_dev *rkvdec = ctx->dev;
	s16 feature_val;
	u8 feature_id;
	u32 val = 0;

	seg = vp9_ctx->last.valid ? &vp9_ctx->last.seg : &vp9_ctx->cur.seg;
	feature_id = V4L2_VP9_SEGMENT_FEATURE_QP_DELTA;
	if (seg_featured_enabled(seg, feature_id, segid)) {
		feature_val = seg->feature_data[segid][feature_id];
		val |= RKVDEC_SEGID_FRAME_QP_DELTA_EN(1) |
		       RKVDEC_SEGID_FRAME_QP_DELTA(feature_val);
	}

	feature_id = V4L2_VP9_SEGMENT_FEATURE_LF;
	if (seg_featured_enabled(seg, feature_id, segid)) {
		feature_val = seg->feature_data[segid][feature_id];
		val |= RKVDEC_SEGID_FRAME_LOOPFILTER_VALUE_EN(1) |
		       RKVDEC_SEGID_FRAME_LOOPFILTER_VALUE(feature_val);
	}

	feature_id = V4L2_VP9_SEGMENT_FEATURE_REF_FRAME;
	if (seg_featured_enabled(seg, feature_id, segid)) {
		feature_val = seg->feature_data[segid][feature_id];
		val |= RKVDEC_SEGID_REFERINFO_EN(1) |
		       RKVDEC_SEGID_REFERINFO(feature_val);
	}

	feature_id = V4L2_VP9_SEGMENT_FEATURE_SKIP;
	if (seg_featured_enabled(seg, feature_id, segid))
		val |= RKVDEC_SEGID_FRAME_SKIP_EN(1);

	if (!segid &&
	    (seg->flags & V4L2_VP9_SEGMENTATION_FLAG_ABS_OR_DELTA_UPDATE))
		val |= RKVDEC_SEGID_ABS_DELTA(1);

	writel_relaxed(val, rkvdec->regs + RKVDEC_VP9_SEGID_GRP(segid));
}

static void
update_dec_buf_info(struct rkvdec_decoded_buffer *buf,
		    const struct v4l2_ctrl_vp9_frame_decode_params *dec_params)
{
	buf->vp9.width = dec_params->frame_width_minus_1 + 1;
	buf->vp9.height = dec_params->frame_height_minus_1 + 1;
	buf->vp9.bit_depth = dec_params->bit_depth;
}

static void
update_ctx_cur_info(struct rkvdec_vp9_ctx *vp9_ctx,
		struct rkvdec_decoded_buffer *buf,
		const struct v4l2_ctrl_vp9_frame_decode_params *dec_params)
{
	vp9_ctx->cur.valid = true;
	vp9_ctx->cur.frame_context_idx = dec_params->frame_context_idx;
	vp9_ctx->cur.reference_mode = dec_params->reference_mode;
	vp9_ctx->cur.tx_mode = dec_params->tx_mode;
	vp9_ctx->cur.interpolation_filter = dec_params->interpolation_filter;
	vp9_ctx->cur.flags = dec_params->flags;
	vp9_ctx->cur.timestamp = buf->base.vb.vb2_buf.timestamp;
	vp9_ctx->cur.seg = dec_params->seg;
	vp9_ctx->cur.lf = dec_params->lf;
}

static void
update_ctx_last_info(struct rkvdec_vp9_ctx *vp9_ctx)
{
	vp9_ctx->last = vp9_ctx->cur;
}

static void config_registers(struct rkvdec_ctx *ctx,
			     const struct rkvdec_vp9_run *run)
{
	u32 y_len, uv_len, yuv_len, bit_depth, aligned_height, aligned_pitch;
	const struct v4l2_ctrl_vp9_frame_decode_params *dec_params;
	struct rkvdec_decoded_buffer *ref_bufs[V4L2_REF_ID_CNT];
	struct rkvdec_decoded_buffer *dst, *last, *mv_ref;
	struct rkvdec_vp9_ctx *vp9_ctx = ctx->priv;
	u32 val, stream_len, last_frame_info = 0;
	const struct v4l2_vp9_segmentation *seg;
	struct rkvdec_dev *rkvdec = ctx->dev;
	dma_addr_t addr;
	bool intra_only;
	unsigned int i;

	dec_params = run->decode_params;
	dst = vb2_to_rkvdec_decoded_buf(&run->base.bufs.dst->vb2_buf);
	for (i = 0; i < ARRAY_SIZE(ref_bufs); i++)
		ref_bufs[i] = get_ref_buf(ctx, &dst->base.vb,
					  dec_params->refs[i]);

	if (vp9_ctx->last.valid)
		last = get_ref_buf(ctx, &dst->base.vb, vp9_ctx->last.timestamp);
	else
		last = dst;

	update_dec_buf_info(dst, dec_params);
	update_ctx_cur_info(vp9_ctx, dst, dec_params);
	seg = &dec_params->seg;

	intra_only = !!(dec_params->flags &
			(V4L2_VP9_FRAME_FLAG_KEY_FRAME |
			 V4L2_VP9_FRAME_FLAG_INTRA_ONLY));

	writel_relaxed(RKVDEC_MODE(RKVDEC_MODE_VP9),
		       rkvdec->regs + RKVDEC_REG_SYSCTRL);

	bit_depth = dec_params->bit_depth;
	aligned_height = round_up(ctx->decoded_fmt.fmt.pix_mp.height, 64);

	aligned_pitch = round_up(ctx->decoded_fmt.fmt.pix_mp.width *
				 bit_depth,
				 512) / 8;
	y_len = aligned_height * aligned_pitch;
	uv_len = y_len / 2;
	yuv_len = y_len + uv_len;

	writel_relaxed(RKVDEC_Y_HOR_VIRSTRIDE(aligned_pitch / 16) |
		       RKVDEC_UV_HOR_VIRSTRIDE(aligned_pitch / 16),
		       rkvdec->regs + RKVDEC_REG_PICPAR);
	writel_relaxed(RKVDEC_Y_VIRSTRIDE(y_len / 16),
		       rkvdec->regs + RKVDEC_REG_Y_VIRSTRIDE);
	writel_relaxed(RKVDEC_YUV_VIRSTRIDE(yuv_len / 16),
		       rkvdec->regs + RKVDEC_REG_YUV_VIRSTRIDE);

	stream_len = vb2_get_plane_payload(&run->base.bufs.src->vb2_buf, 0);
	writel_relaxed(RKVDEC_STRM_LEN(stream_len),
		       rkvdec->regs + RKVDEC_REG_STRM_LEN);

	/*
	 * Reset count buffer, because decoder only output intra related syntax
	 * counts when decoding intra frame, but update entropy need to update
	 * all the probabilities.
	 */
	if (intra_only)
		memset(vp9_ctx->count_tbl.cpu, 0, vp9_ctx->count_tbl.size);

	vp9_ctx->cur.segmapid = vp9_ctx->last.segmapid;
	if (!intra_only &&
	    !(dec_params->flags & V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT) &&
	    (!(seg->flags & V4L2_VP9_SEGMENTATION_FLAG_ENABLED) ||
	     (seg->flags & V4L2_VP9_SEGMENTATION_FLAG_UPDATE_MAP)))
		vp9_ctx->cur.segmapid++;

	for (i = 0; i < ARRAY_SIZE(ref_bufs); i++)
		config_ref_registers(ctx, run, ref_bufs, i);

	for (i = 0; i < 8; i++)
		config_seg_registers(ctx, i);

	writel_relaxed(RKVDEC_VP9_TX_MODE(dec_params->tx_mode) |
		       RKVDEC_VP9_FRAME_REF_MODE(dec_params->reference_mode),
		       rkvdec->regs + RKVDEC_VP9_CPRHEADER_CONFIG);

	if (!intra_only) {
		const struct v4l2_vp9_loop_filter *lf;
		s8 delta;

		if (vp9_ctx->last.valid)
			lf = &vp9_ctx->last.lf;
		else
			lf = &vp9_ctx->cur.lf;

		val = 0;
		for (i = 0; i < ARRAY_SIZE(lf->ref_deltas); i++) {
			delta = lf->ref_deltas[i];
			val |= RKVDEC_REF_DELTAS_LASTFRAME(i, delta);
		}

		writel_relaxed(val,
			       rkvdec->regs + RKVDEC_VP9_REF_DELTAS_LASTFRAME);

		for (i = 0; i < ARRAY_SIZE(lf->mode_deltas); i++) {
			delta = lf->mode_deltas[i];
			last_frame_info |= RKVDEC_MODE_DELTAS_LASTFRAME(i,
									delta);
		}
	}

	if (vp9_ctx->last.valid && !intra_only &&
	    vp9_ctx->last.seg.flags & V4L2_VP9_SEGMENTATION_FLAG_ENABLED)
		last_frame_info |= RKVDEC_SEG_EN_LASTFRAME;

	if (vp9_ctx->last.valid &&
	    vp9_ctx->last.flags & V4L2_VP9_FRAME_FLAG_SHOW_FRAME)
		last_frame_info |= RKVDEC_LAST_SHOW_FRAME;

	if (vp9_ctx->last.valid &&
	    vp9_ctx->last.flags &
	    (V4L2_VP9_FRAME_FLAG_KEY_FRAME | V4L2_VP9_FRAME_FLAG_INTRA_ONLY))
		last_frame_info |= RKVDEC_LAST_INTRA_ONLY;

	if (vp9_ctx->last.valid &&
	    last->vp9.width == dst->vp9.width &&
	    last->vp9.height == dst->vp9.height)
		last_frame_info |= RKVDEC_LAST_WIDHHEIGHT_EQCUR;

	writel_relaxed(last_frame_info,
		       rkvdec->regs + RKVDEC_VP9_INFO_LASTFRAME);

	writel_relaxed(stream_len - dec_params->compressed_header_size -
		       dec_params->uncompressed_header_size,
		       rkvdec->regs + RKVDEC_VP9_LASTTILE_SIZE);

	for (i = 0; !intra_only && i < ARRAY_SIZE(ref_bufs); i++) {
		u32 refw = ref_bufs[i]->vp9.width;
		u32 refh = ref_bufs[i]->vp9.height;
		u32 hscale, vscale;

		hscale = (refw << 14) /	dst->vp9.width;
		vscale = (refh << 14) / dst->vp9.height;
		writel_relaxed(RKVDEC_VP9_REF_HOR_SCALE(hscale) |
			       RKVDEC_VP9_REF_VER_SCALE(vscale),
			       rkvdec->regs + RKVDEC_VP9_REF_SCALE(i));
	}

	addr = vb2_dma_contig_plane_dma_addr(&dst->base.vb.vb2_buf, 0);
	writel_relaxed(addr, rkvdec->regs + RKVDEC_REG_DECOUT_BASE);
	addr = vb2_dma_contig_plane_dma_addr(&run->base.bufs.src->vb2_buf, 0);
	writel_relaxed(addr, rkvdec->regs + RKVDEC_REG_STRM_RLC_BASE);
	writel_relaxed(vp9_ctx->priv_tbl.dma +
		       offsetof(struct rkvdec_vp9_priv_tbl, probs),
		       rkvdec->regs + RKVDEC_REG_CABACTBL_PROB_BASE);
	writel_relaxed(vp9_ctx->count_tbl.dma,
		       rkvdec->regs + RKVDEC_REG_VP9COUNT_BASE);

	writel_relaxed(vp9_ctx->priv_tbl.dma +
		       offsetof(struct rkvdec_vp9_priv_tbl, segmap) +
		       (RKVDEC_VP9_MAX_SEGMAP_SIZE * vp9_ctx->cur.segmapid),
		       rkvdec->regs + RKVDEC_REG_VP9_SEGIDCUR_BASE);
	writel_relaxed(vp9_ctx->priv_tbl.dma +
		       offsetof(struct rkvdec_vp9_priv_tbl, segmap) +
		       (RKVDEC_VP9_MAX_SEGMAP_SIZE * (!vp9_ctx->cur.segmapid)),
		       rkvdec->regs + RKVDEC_REG_VP9_SEGIDLAST_BASE);

	if (!intra_only &&
	    !(dec_params->flags & V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT) &&
	    vp9_ctx->last.valid)
		mv_ref = last;
	else
		mv_ref = dst;

	writel_relaxed(get_mv_base_addr(mv_ref),
		       rkvdec->regs + RKVDEC_VP9_REF_COLMV_BASE);

	writel_relaxed(ctx->decoded_fmt.fmt.pix_mp.width |
		       (ctx->decoded_fmt.fmt.pix_mp.height << 16),
		       rkvdec->regs + RKVDEC_REG_PERFORMANCE_CYCLE);
}

static int
validate_dec_params(struct rkvdec_ctx *ctx,
	const struct v4l2_ctrl_vp9_frame_decode_params *dec_params)
{
	u32 width, height;

	/* We only support profile 0. */
	if (dec_params->profile != 0)
		return -EINVAL;

	width = dec_params->frame_width_minus_1 + 1;
	height = dec_params->frame_height_minus_1 + 1;

	/*
	 * Userspace should update the capture/decoded format when the
	 * resolution changes.
	 */
	if (width != ctx->decoded_fmt.fmt.pix_mp.width ||
	    height != ctx->decoded_fmt.fmt.pix_mp.height)
		return -EINVAL;

	return 0;
}

static int rkvdec_vp9_run_preamble(struct rkvdec_ctx *ctx,
				   struct rkvdec_vp9_run *run)
{
	const struct v4l2_ctrl_vp9_frame_decode_params *dec_params;
	const struct v4l2_ctrl_vp9_frame_ctx *fctx = NULL;
	struct rkvdec_vp9_ctx *vp9_ctx = ctx->priv;
	struct v4l2_ctrl *ctrl;
	u8 frm_ctx;
	int ret;

	rkvdec_run_preamble(ctx, &run->base);

	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			       V4L2_CID_MPEG_VIDEO_VP9_FRAME_DECODE_PARAMS);
	WARN_ON(!ctrl);

	dec_params = ctrl ? ctrl->p_cur.p : NULL;
	if (WARN_ON(!dec_params))
		return -EINVAL;

	ret = validate_dec_params(ctx, dec_params);

	run->decode_params = dec_params;

	/* No need to load the frame context if we don't need to update it. */
	if (!(dec_params->flags & V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX))
		return 0;

	/*
	 * When a refresh context is requested in parallel mode, we should just
	 * update the context with the probs passed in the decode parameters.
	 */
	if (dec_params->flags & V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE) {
		vp9_ctx->frame_context.probs = dec_params->probs;
		return 0;
	}

	frm_ctx = run->decode_params->frame_context_idx;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_MPEG_VIDEO_VP9_FRAME_CONTEXT(frm_ctx));
	if (WARN_ON(!ctrl))
		return 0;

	fctx = ctrl->p_cur.p;
	vp9_ctx->frame_context = *fctx;

	/*
	 * For intra-only frames, we must update the context TX and skip probs
	 * with the value passed in the decode params.
	 */
	if (dec_params->flags &
	    (V4L2_VP9_FRAME_FLAG_KEY_FRAME | V4L2_VP9_FRAME_FLAG_INTRA_ONLY)) {
		struct v4l2_vp9_probabilities *probs;

		probs =  &vp9_ctx->frame_context.probs;
		memcpy(probs->skip, dec_params->probs.skip,
		       sizeof(probs->skip));
		memcpy(probs->tx8, dec_params->probs.tx8,
		       sizeof(probs->tx8));
		memcpy(probs->tx16, dec_params->probs.tx16,
		       sizeof(probs->tx16));
		memcpy(probs->tx32, dec_params->probs.tx32,
		       sizeof(probs->tx32));
	}

	return 0;
}

static int rkvdec_vp9_run(struct rkvdec_ctx *ctx)
{
	struct rkvdec_dev *rkvdec = ctx->dev;
	struct rkvdec_vp9_run run = { };
	int ret;

	ret = rkvdec_vp9_run_preamble(ctx, &run);
	if (ret) {
		rkvdec_run_postamble(ctx, &run.base);
		return ret;
	}

	/* Prepare probs. */
	init_probs(ctx, &run);

	/* Configure hardware registers. */
	config_registers(ctx, &run);

	rkvdec_run_postamble(ctx, &run.base);

	schedule_delayed_work(&rkvdec->watchdog_work, msecs_to_jiffies(2000));

	writel(1, rkvdec->regs + RKVDEC_REG_PREF_LUMA_CACHE_COMMAND);
	writel(1, rkvdec->regs + RKVDEC_REG_PREF_CHR_CACHE_COMMAND);

	writel(0xe, rkvdec->regs + RKVDEC_REG_STRMD_ERR_EN);
	/* Start decoding! */
	writel(RKVDEC_INTERRUPT_DEC_E | RKVDEC_CONFIG_DEC_CLK_GATE_E |
	       RKVDEC_TIMEOUT_E | RKVDEC_BUF_EMPTY_E,
	       rkvdec->regs + RKVDEC_REG_INTERRUPT);

	return 0;
}

static u8 adapt_prob(u8 p1, u32 ct0, u32 ct1, u16 max_count, u32 update_factor)
{
	u32 ct = ct0 + ct1, p2;
	u32 lo = 1;
	u32 hi = 255;

	if (!ct)
		return p1;

	p2 = ((ct0 << 8) + (ct >> 1)) / ct;
	p2 = clamp(p2, lo, hi);
	ct = min_t(u32, ct, max_count);

	if (WARN_ON(max_count >= 257))
		return p1;

	update_factor = rkvdec_fastdiv(update_factor * ct, max_count);

	return p1 + (((p2 - p1) * update_factor + 128) >> 8);
}

#define BAND_6(band) ((band) == 0 ? 3 : 6)

static void adapt_coeff(u8 coef[6][6][3],
			const struct rkvdec_vp9_refs_counts ref_cnt[6][6],
			u32 uf)
{
	s32 l, m, n;

	for (l = 0; l < 6; l++) {
		for (m = 0; m < BAND_6(l); m++) {
			u8 *p = coef[l][m];
			const u32 n0 = ref_cnt[l][m].coeff[0];
			const u32 n1 = ref_cnt[l][m].coeff[1];
			const u32 n2 = ref_cnt[l][m].coeff[2];
			const u32 neob = ref_cnt[l][m].eob[1];
			const u32 eob_count = ref_cnt[l][m].eob[0];
			const u32 branch_ct[3][2] = {
				{ neob, eob_count - neob },
				{ n0, n1 + n2 },
				{ n1, n2 }
			};

			for (n = 0; n < 3; n++)
				p[n] = adapt_prob(p[n], branch_ct[n][0],
						  branch_ct[n][1], 24, uf);
		}
	}
}

static void
adapt_coef_probs(struct v4l2_vp9_probabilities *probs,
		 const struct rkvdec_vp9_refs_counts ref_cnt[2][4][2][6][6],
		 unsigned int uf)
{
	unsigned int i, j, k;

	for (i = 0; i < ARRAY_SIZE(probs->coef); i++) {
		for (j = 0; j < ARRAY_SIZE(probs->coef[0]); j++) {
			for (k = 0; k < ARRAY_SIZE(probs->coef[0][0]);
			     k++) {
				adapt_coeff(probs->coef[i][j][k],
					    ref_cnt[k][i][j],
					    uf);
			}
		}
	}
}

static void adapt_intra_frame_probs(struct rkvdec_ctx *ctx,
				    struct rkvdec_decoded_buffer *dst)
{
	struct rkvdec_vp9_ctx *vp9_ctx = ctx->priv;
	struct v4l2_vp9_probabilities *probs = &vp9_ctx->frame_context.probs;
	const struct rkvdec_vp9_intra_frame_symbol_counts *sym_cnts;

	sym_cnts = vp9_ctx->count_tbl.cpu;
	adapt_coef_probs(probs, sym_cnts->ref_cnt, 112);
}

static void
adapt_skip_probs(struct v4l2_vp9_probabilities *probs,
		 const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(probs->skip); i++)
		probs->skip[i] = adapt_prob(probs->skip[i],
					    sym_cnts->skip[i][0],
					    sym_cnts->skip[i][1],
					    20, 128);
}

static void
adapt_is_inter_probs(struct v4l2_vp9_probabilities *probs,
		const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(probs->is_inter); i++)
		probs->is_inter[i] = adapt_prob(probs->is_inter[i],
						sym_cnts->inter[i][0],
						sym_cnts->inter[i][1],
						20, 128);
}

static void
adapt_comp_mode_probs(struct v4l2_vp9_probabilities *probs,
		const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(probs->comp_mode); i++)
		probs->comp_mode[i] = adapt_prob(probs->comp_mode[i],
						 sym_cnts->comp[i][0],
						 sym_cnts->comp[i][1],
						 20, 128);
}

static void
adapt_comp_ref_probs(struct v4l2_vp9_probabilities *probs,
		const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(probs->comp_ref); i++)
		probs->comp_ref[i] = adapt_prob(probs->comp_ref[i],
						sym_cnts->comp_ref[i][0],
						sym_cnts->comp_ref[i][1],
						20, 128);
}

static void
adapt_single_ref_probs(struct v4l2_vp9_probabilities *probs,
		const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(probs->single_ref); i++) {
		u8 *p = probs->single_ref[i];

		p[0] = adapt_prob(p[0], sym_cnts->single_ref[i][0][0],
				  sym_cnts->single_ref[i][0][1], 20, 128);
		p[1] = adapt_prob(p[1], sym_cnts->single_ref[i][1][0],
				  sym_cnts->single_ref[i][1][1], 20, 128);
	}
}

static void
adapt_partition_probs(struct v4l2_vp9_probabilities *probs,
		const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(probs->partition); i++) {
		const u32 *c = sym_cnts->partition[i];
		u8 *p = probs->partition[i];

		p[0] = adapt_prob(p[0], c[0], c[1] + c[2] + c[3], 20, 128);
		p[1] = adapt_prob(p[1], c[1], c[2] + c[3], 20, 128);
		p[2] = adapt_prob(p[2], c[2], c[3], 20, 128);
	}
}

static void
adapt_tx_probs(struct v4l2_vp9_probabilities *probs,
	       const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(probs->tx8); i++) {
		u8 *p16x16 = probs->tx16[i];
		u8 *p32x32 = probs->tx32[i];
		const u32 *c16 = sym_cnts->tx16p[i];
		const u32 *c32 = sym_cnts->tx32p[i];
		const u32 *c8 = sym_cnts->tx8p[i];
		u8 *p8x8 = probs->tx8[i];

		p8x8[0] = adapt_prob(p8x8[0], c8[0], c8[1], 20, 128);
		p16x16[0] = adapt_prob(p16x16[0], c16[0], c16[1] + c16[2],
				       20, 128);
		p16x16[1] = adapt_prob(p16x16[1], c16[1], c16[2], 20, 128);
		p32x32[0] = adapt_prob(p32x32[0], c32[0],
				       c32[1] + c32[2] + c32[3], 20, 128);
		p32x32[1] = adapt_prob(p32x32[1], c32[1], c32[2] + c32[3],
				       20, 128);
		p32x32[2] = adapt_prob(p32x32[2], c32[2], c32[3], 20, 128);
	}
}

static void
adapt_interp_filter_probs(struct v4l2_vp9_probabilities *probs,
		const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(probs->interp_filter); i++) {
		u8 *p = probs->interp_filter[i];
		const u32 *c = sym_cnts->filter[i];

		p[0] = adapt_prob(p[0], c[0], c[1] + c[2], 20, 128);
		p[1] = adapt_prob(p[1], c[1], c[2], 20, 128);
	}
}

static void
adapt_inter_mode_probs(struct v4l2_vp9_probabilities *probs,
		const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(probs->inter_mode); i++) {
		const u32 *c = sym_cnts->mv_mode[i];
		u8 *p = probs->inter_mode[i];

		p[0] = adapt_prob(p[0], c[2], c[1] + c[0] + c[3], 20, 128);
		p[1] = adapt_prob(p[1], c[0], c[1] + c[3], 20, 128);
		p[2] = adapt_prob(p[2], c[1], c[3], 20, 128);
	}
}

static void
adapt_mv_probs(struct v4l2_vp9_probabilities *probs,
	       const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts,
	       bool high_prec_mv)
{
	const u32 *c = sym_cnts->mv_joint;
	u8 *p = probs->mv.joint;
	unsigned int i, j;
	u32 sum;

	p[0] = adapt_prob(p[0], c[0], c[1] + c[2] + c[3], 20, 128);
	p[1] = adapt_prob(p[1], c[1], c[2] + c[3], 20, 128);
	p[2] = adapt_prob(p[2], c[2], c[3], 20, 128);

	for (i = 0; i < ARRAY_SIZE(probs->mv.sign); i++) {
		p = probs->mv.sign;

		p[i] = adapt_prob(p[i], sym_cnts->sign[i][0],
				  sym_cnts->sign[i][1], 20, 128);

		p = probs->mv.class[i];
		c = sym_cnts->classes[i];
		sum = c[1] + c[2] + c[3] + c[4] + c[5] + c[6] + c[7] + c[8] +
		      c[9] + c[10];
		p[0] = adapt_prob(p[0], c[0], sum, 20, 128);
		sum -= c[1];
		p[1] = adapt_prob(p[1], c[1], sum, 20, 128);
		sum -= c[2] + c[3];
		p[2] = adapt_prob(p[2], c[2] + c[3], sum, 20, 128);
		p[3] = adapt_prob(p[3], c[2], c[3], 20, 128);
		sum -= c[4] + c[5];
		p[4] = adapt_prob(p[4], c[4] + c[5], sum, 20, 128);
		p[5] = adapt_prob(p[5], c[4], c[5], 20, 128);
		sum -= c[6];
		p[6] = adapt_prob(p[6], c[6], sum, 20, 128);
		p[7] = adapt_prob(p[7], c[7] + c[8], c[9] + c[10], 20, 128);
		p[8] = adapt_prob(p[8], c[7], c[8], 20, 128);
		p[9] = adapt_prob(p[9], c[9], c[10], 20, 128);

		p = probs->mv.class0_bit;
		p[i] = adapt_prob(p[i],
				  sym_cnts->class0[i][0],
				  sym_cnts->class0[i][1], 20, 128);

		p = probs->mv.bits[i];
		for (j = 0; j < 10; j++)
			p[j] = adapt_prob(p[j], sym_cnts->bits[i][j][0],
					  sym_cnts->bits[i][j][1], 20, 128);

		for (j = 0; j < 2; j++) {
			p = probs->mv.class0_fr[i][j];
			c = sym_cnts->class0_fp[i][j];
			p[0] = adapt_prob(p[0], c[0], c[1] + c[2] + c[3],
					  20, 128);
			p[1] = adapt_prob(p[1], c[1], c[2] + c[3], 20, 128);
			p[2] = adapt_prob(p[2], c[2], c[3], 20, 128);
		}

		p = probs->mv.fr[i];
		c = sym_cnts->fp[i];
		p[0] = adapt_prob(p[0], c[0], c[1] + c[2] + c[3], 20, 128);
		p[1] = adapt_prob(p[1], c[1], c[2] + c[3], 20, 128);
		p[2] = adapt_prob(p[2], c[2], c[3], 20, 128);

		if (!high_prec_mv)
			continue;

		p = probs->mv.class0_hp;
		p[i] = adapt_prob(p[i], sym_cnts->class0_hp[i][0],
				  sym_cnts->class0_hp[i][1], 20, 128);

		p = probs->mv.hp;
		p[i] = adapt_prob(p[i], sym_cnts->hp[i][0],
				  sym_cnts->hp[i][1], 20, 128);
	}
}

static void
adapt_intra_mode_probs(u8 *p, const u32 *c)
{
	u32 sum = 0, s2;
	unsigned int i;

	for (i = V4L2_VP9_INTRA_PRED_MODE_V; i <= V4L2_VP9_INTRA_PRED_MODE_TM;
	     i++)
		sum += c[i];

	p[0] = adapt_prob(p[0], c[V4L2_VP9_INTRA_PRED_MODE_DC], sum, 20, 128);
	sum -= c[V4L2_VP9_INTRA_PRED_MODE_TM];
	p[1] = adapt_prob(p[1], c[V4L2_VP9_INTRA_PRED_MODE_TM], sum, 20, 128);
	sum -= c[V4L2_VP9_INTRA_PRED_MODE_V];
	p[2] = adapt_prob(p[2], c[V4L2_VP9_INTRA_PRED_MODE_V], sum, 20, 128);
	s2 = c[V4L2_VP9_INTRA_PRED_MODE_H] + c[V4L2_VP9_INTRA_PRED_MODE_D135] +
	     c[V4L2_VP9_INTRA_PRED_MODE_D117];
	sum -= s2;
	p[3] = adapt_prob(p[3], s2, sum, 20, 128);
	s2 -= c[V4L2_VP9_INTRA_PRED_MODE_H];
	p[4] = adapt_prob(p[4], c[V4L2_VP9_INTRA_PRED_MODE_H], s2, 20, 128);
	p[5] = adapt_prob(p[5], c[V4L2_VP9_INTRA_PRED_MODE_D135],
			  c[V4L2_VP9_INTRA_PRED_MODE_D117], 20, 128);
	sum -= c[V4L2_VP9_INTRA_PRED_MODE_D45];
	p[6] = adapt_prob(p[6], c[V4L2_VP9_INTRA_PRED_MODE_D45],
			  sum, 20, 128);
	sum -= c[V4L2_VP9_INTRA_PRED_MODE_D63];
	p[7] = adapt_prob(p[7], c[V4L2_VP9_INTRA_PRED_MODE_D63], sum,
			  20, 128);
	p[8] = adapt_prob(p[8], c[V4L2_VP9_INTRA_PRED_MODE_D153],
			  c[V4L2_VP9_INTRA_PRED_MODE_D207], 20, 128);
}

static void
adapt_y_intra_mode_probs(struct v4l2_vp9_probabilities *probs,
		const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(probs->y_mode); i++)
		adapt_intra_mode_probs(probs->y_mode[i], sym_cnts->y_mode[i]);
}

static void
adapt_uv_intra_mode_probs(struct v4l2_vp9_probabilities *probs,
		const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(probs->uv_mode); i++)
		adapt_intra_mode_probs(probs->uv_mode[i],
				       sym_cnts->uv_mode[i]);
}

static void
adapt_inter_frame_probs(struct rkvdec_ctx *ctx,
			struct rkvdec_decoded_buffer *dst)
{
	struct rkvdec_vp9_ctx *vp9_ctx = ctx->priv;
	struct v4l2_vp9_probabilities *probs = &vp9_ctx->frame_context.probs;
	const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts;

	sym_cnts = vp9_ctx->count_tbl.cpu;
	/* coefficients */
	if (vp9_ctx->last.valid &&
	    !(vp9_ctx->last.flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME))
		adapt_coef_probs(probs, sym_cnts->ref_cnt, 112);
	else
		adapt_coef_probs(probs, sym_cnts->ref_cnt, 128);

	/* skip flag */
	adapt_skip_probs(probs, sym_cnts);

	/* intra/inter flag */
	adapt_is_inter_probs(probs, sym_cnts);

	/* comppred flag */
	adapt_comp_mode_probs(probs, sym_cnts);

	/* reference frames */
	adapt_comp_ref_probs(probs, sym_cnts);

	if (vp9_ctx->cur.reference_mode != V4L2_VP9_REF_MODE_COMPOUND)
		adapt_single_ref_probs(probs, sym_cnts);

	/* block partitioning */
	adapt_partition_probs(probs, sym_cnts);

	/* tx size */
	if (vp9_ctx->cur.tx_mode == V4L2_VP9_TX_MODE_SELECT)
		adapt_tx_probs(probs, sym_cnts);

	/* interpolation filter */
	if (vp9_ctx->cur.interpolation_filter == V4L2_VP9_INTERP_FILTER_SWITCHABLE)
		adapt_interp_filter_probs(probs, sym_cnts);

	/* inter modes */
	adapt_inter_mode_probs(probs, sym_cnts);

	/* mv probs */
	adapt_mv_probs(probs, sym_cnts,
		       !!(vp9_ctx->cur.flags &
			  V4L2_VP9_FRAME_FLAG_ALLOW_HIGH_PREC_MV));

	/* y intra modes */
	adapt_y_intra_mode_probs(probs, sym_cnts);

	/* uv intra modes */
	adapt_uv_intra_mode_probs(probs, sym_cnts);
}

static void adapt_probs(struct rkvdec_ctx *ctx,
			struct rkvdec_decoded_buffer *dst)
{
	struct rkvdec_vp9_ctx *vp9_ctx = ctx->priv;
	bool intra_only;

	intra_only = !!(vp9_ctx->cur.flags &
			(V4L2_VP9_FRAME_FLAG_KEY_FRAME |
			 V4L2_VP9_FRAME_FLAG_INTRA_ONLY));

	if (intra_only)
		adapt_intra_frame_probs(ctx, dst);
	else
		adapt_inter_frame_probs(ctx, dst);
}

static void rkvdec_vp9_done(struct rkvdec_ctx *ctx,
			    struct vb2_v4l2_buffer *src_buf,
			    struct vb2_v4l2_buffer *dst_buf,
			    enum vb2_buffer_state result)
{
	struct rkvdec_decoded_buffer *dec_dst_buf;
	const struct v4l2_ctrl_vp9_frame_ctx *fctx;
	struct rkvdec_vp9_ctx *vp9_ctx = ctx->priv;
	struct v4l2_ctrl *ctrl;
	unsigned int fctx_idx;

	if (result == VB2_BUF_STATE_ERROR)
		goto out_update_last;

	if (!(vp9_ctx->cur.flags & V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX))
		goto out_update_last;

	fctx_idx = vp9_ctx->cur.frame_context_idx;
	fctx = ctrl->p_cur.p;

	if (!(vp9_ctx->cur.flags &
	      (V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT |
	       V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE)))
		adapt_probs(ctx, dec_dst_buf);

	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_MPEG_VIDEO_VP9_FRAME_CONTEXT(fctx_idx));
	if (WARN_ON(!ctrl))
		goto out_update_last;

	v4l2_ctrl_s_ctrl_compound(ctrl, &vp9_ctx->frame_context,
				  sizeof(vp9_ctx->frame_context));

out_update_last:
	update_ctx_last_info(vp9_ctx);
}

static int rkvdec_vp9_start(struct rkvdec_ctx *ctx)
{
	struct rkvdec_dev *rkvdec = ctx->dev;
	struct rkvdec_vp9_priv_tbl *priv_tbl;
	struct rkvdec_vp9_ctx *vp9_ctx;
	u8 *count_tbl;
	int ret;

	vp9_ctx = kzalloc(sizeof(*vp9_ctx), GFP_KERNEL);
	if (!vp9_ctx)
		return -ENOMEM;

	ctx->priv = vp9_ctx;

	priv_tbl = dma_alloc_coherent(rkvdec->dev, sizeof(*priv_tbl),
				      &vp9_ctx->priv_tbl.dma, GFP_KERNEL);
	if (!priv_tbl) {
		ret = -ENOMEM;
		goto err_free_ctx;
	}

	vp9_ctx->priv_tbl.size = sizeof(*priv_tbl);
	vp9_ctx->priv_tbl.cpu = priv_tbl;
	memset(priv_tbl, 0, sizeof(*priv_tbl));

	count_tbl = dma_alloc_coherent(rkvdec->dev, RKVDEC_VP9_COUNT_SIZE,
				       &vp9_ctx->count_tbl.dma, GFP_KERNEL);
	if (!count_tbl) {
		ret = -ENOMEM;
		goto err_free_priv_tbl;
	}

	vp9_ctx->count_tbl.size = RKVDEC_VP9_COUNT_SIZE;
	vp9_ctx->count_tbl.cpu = count_tbl;
	memset(count_tbl, 0, sizeof(*count_tbl));

	return 0;

err_free_priv_tbl:
	dma_free_coherent(rkvdec->dev, vp9_ctx->priv_tbl.size,
			  vp9_ctx->priv_tbl.cpu, vp9_ctx->priv_tbl.dma);

err_free_ctx:
	kfree(vp9_ctx);
	return ret;
}

static void rkvdec_vp9_stop(struct rkvdec_ctx *ctx)
{
	struct rkvdec_vp9_ctx *vp9_ctx = ctx->priv;
	struct rkvdec_dev *rkvdec = ctx->dev;

	dma_free_coherent(rkvdec->dev, vp9_ctx->count_tbl.size,
			  vp9_ctx->count_tbl.cpu, vp9_ctx->count_tbl.dma);
	dma_free_coherent(rkvdec->dev, vp9_ctx->priv_tbl.size,
			  vp9_ctx->priv_tbl.cpu, vp9_ctx->priv_tbl.dma);
	kfree(vp9_ctx);
}

static int rkvdec_vp9_adjust_fmt(struct rkvdec_ctx *ctx,
				 struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *fmt = &f->fmt.pix_mp;

	fmt->num_planes = 1;
	fmt->plane_fmt[0].sizeimage = fmt->width * fmt->height * 2;
	return 0;
}

const struct rkvdec_coded_fmt_ops rkvdec_vp9_fmt_ops = {
	.adjust_fmt = rkvdec_vp9_adjust_fmt,
	.start = rkvdec_vp9_start,
	.stop = rkvdec_vp9_stop,
	.run = rkvdec_vp9_run,
	.done = rkvdec_vp9_done,
};
