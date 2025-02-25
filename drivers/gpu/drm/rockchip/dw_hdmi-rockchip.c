// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2014, Fuzhou Rockchip Electronics Co., Ltd
 */

#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>
#include <linux/regmap.h>

#include <drm/bridge/dw_hdmi.h>
#include <drm/drm_edid.h>
#include <drm/drm_of.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include "../bridge/synopsys/dw-hdmi.h"

#include "rockchip_drm_drv.h"
#include "rockchip_drm_vop.h"

#define RK3228_GRF_SOC_CON2		0x0408
#define RK3228_HDMI_SDAIN_MSK		BIT(14)
#define RK3228_HDMI_SCLIN_MSK		BIT(13)
#define RK3228_GRF_SOC_CON6		0x0418
#define RK3228_HDMI_HPD_VSEL		BIT(6)
#define RK3228_HDMI_SDA_VSEL		BIT(5)
#define RK3228_HDMI_SCL_VSEL		BIT(4)

#define RK3228_HDMI_DDC_MASK_EN		((3 << 13) | (3 << (13 + 16)))
#define RK3228_HDMI_IO_3V		((7 << 4) | (7 << (4 + 16)))

#define RK3288_GRF_SOC_CON6		0x025C
#define RK3288_HDMI_LCDC_SEL		BIT(4)

#define RK3328_HDMI_SDAIN_MSK		BIT(11)
#define RK3328_HDMI_SCLIN_MSK		BIT(10)
#define RK3328_HDMI_HPD_IOE		BIT(2)
#define RK3328_GRF_SOC_CON2		0x0408
#define RK3328_GRF_SOC_CON3		0x040c
#define RK3328_GRF_SOC_CON4		0x0410
/* need to be unset if hdmi or i2c should control voltage */
#define RK3328_HDMI_SDA5V_GRF		BIT(15)
#define RK3328_HDMI_SCL5V_GRF		BIT(14)
#define RK3328_HDMI_HPD5V_GRF		BIT(13)
#define RK3328_HDMI_CEC5V_GRF		BIT(12)
#define RK3328_HDMI_HPD_SARADC		BIT(13)
#define RK3328_HDMI_CEC_5V		BIT(11)
#define RK3328_HDMI_SDA_5V		BIT(10)
#define RK3328_HDMI_SCL_5V		BIT(9)
#define RK3328_HDMI_HPD_3V		(BIT(8 + 16) | BIT(13 + 16))
#define RK3328_HDMI_HPD_5V		BIT(8)

#define RK3328_HDMI_DDC_MASK_EN		((3 << 10) | (3 << (10 + 16)))
#define RK3328_HDMI_IO_CTRL		(0xf0000000 | BIT(13) | BIT(12))
#define RK3328_HDMI_IO_3V		(7 << (9 + 16))
#define RK3328_HDMI_IO_5V		((7 << 9) | (3 << (9 + 16)))

#define RK3399_GRF_SOC_CON20		0x6250
#define RK3399_HDMI_LCDC_SEL		BIT(6)

#define HIWORD_UPDATE(val, mask)	(val | (mask) << 16)

/**
 * struct rockchip_hdmi_chip_data - splite the grf setting of kind of chips
 * @lcdsel_grf_reg: grf register offset of lcdc select
 * @lcdsel_big: reg value of selecting vop big for HDMI
 * @lcdsel_lit: reg value of selecting vop little for HDMI
 * @chip_type: the number value of the chipset (ie: 3328 for RK3328)
 */
struct rockchip_hdmi_chip_data {
	int	lcdsel_grf_reg;
	u32	lcdsel_big;
	u32	lcdsel_lit;
	bool	ycbcr_444_allowed;
	int	chip_type;
};

struct rockchip_hdmi {
	struct device *dev;
	struct regmap *regmap;
	struct drm_encoder encoder;
	struct drm_bridge bridge;
	const struct rockchip_hdmi_chip_data *chip_data;
	struct clk *vpll_clk;
	struct clk *grf_clk;
	struct clk *hclk_vio;
	struct dw_hdmi *hdmi;
	struct phy *phy;
	unsigned long input_bus_format;
	unsigned long output_bus_format;
};

#define to_rockchip_hdmi(x)	container_of(x, struct rockchip_hdmi, x)
#define to_crtc_state(x)	container_of(x, struct drm_crtc_state, x)

static const struct dw_hdmi_mpll_config rockchip_mpll_cfg[] = {
	{
		27000000, {
			{ 0x00b3, 0x0000},
			{ 0x2153, 0x0000},
			{ 0x40f3, 0x0000}
		},
	}, {
		36000000, {
			{ 0x00b3, 0x0000},
			{ 0x2153, 0x0000},
			{ 0x40f3, 0x0000}
		},
	}, {
		40000000, {
			{ 0x00b3, 0x0000},
			{ 0x2153, 0x0000},
			{ 0x40f3, 0x0000}
		},
	}, {
		54000000, {
			{ 0x0072, 0x0001},
			{ 0x2142, 0x0001},
			{ 0x40a2, 0x0001},
		},
	}, {
		65000000, {
			{ 0x0072, 0x0001},
			{ 0x2142, 0x0001},
			{ 0x40a2, 0x0001},
		},
	}, {
		66000000, {
			{ 0x013e, 0x0003},
			{ 0x217e, 0x0002},
			{ 0x4061, 0x0002}
		},
	}, {
		74250000, {
			{ 0x0072, 0x0001},
			{ 0x2145, 0x0002},
			{ 0x4061, 0x0002}
		},
	}, {
		83500000, {
			{ 0x0072, 0x0001},
		},
	}, {
		108000000, {
			{ 0x0051, 0x0002},
			{ 0x2145, 0x0002},
			{ 0x4061, 0x0002}
		},
	}, {
		106500000, {
			{ 0x0051, 0x0002},
			{ 0x2145, 0x0002},
			{ 0x4061, 0x0002}
		},
	}, {
		146250000, {
			{ 0x0051, 0x0002},
			{ 0x2145, 0x0002},
			{ 0x4061, 0x0002}
		},
	}, {
		148500000, {
			{ 0x0051, 0x0003},
			{ 0x214c, 0x0003},
			{ 0x4064, 0x0003}
		},
	}, {
		~0UL, {
			{ 0x00a0, 0x000a },
			{ 0x2001, 0x000f },
			{ 0x4002, 0x000f },
		},
	}
};

static const struct dw_hdmi_mpll_config rockchip_mpll_cfg_420[] = {
	{
		30666000, {
			{ 0x00b7, 0x0000 },
			{ 0x2157, 0x0000 },
			{ 0x40f7, 0x0000 },
		},
	},  {
		92000000, {
			{ 0x00b7, 0x0000 },
			{ 0x2143, 0x0001 },
			{ 0x40a3, 0x0001 },
		},
	},  {
		184000000, {
			{ 0x0073, 0x0001 },
			{ 0x2146, 0x0002 },
			{ 0x4062, 0x0002 },
		},
	},  {
		340000000, {
			{ 0x0052, 0x0003 },
			{ 0x214d, 0x0003 },
			{ 0x4065, 0x0003 },
		},
	},  {
		600000000, {
			{ 0x0041, 0x0003 },
			{ 0x3b4d, 0x0003 },
			{ 0x5a65, 0x0003 },
		},
	},  {
		~0UL, {
			{ 0x0000, 0x0000 },
			{ 0x0000, 0x0000 },
			{ 0x0000, 0x0000 },
		},
	}
};

static const struct dw_hdmi_curr_ctrl rockchip_cur_ctr[] = {
	/*      pixelclk    bpp8    bpp10   bpp12 */
	{
		40000000,  { 0x0018, 0x0018, 0x0018 },
	}, {
		65000000,  { 0x0028, 0x0028, 0x0028 },
	}, {
		66000000,  { 0x0038, 0x0038, 0x0038 },
	}, {
		74250000,  { 0x0028, 0x0038, 0x0038 },
	}, {
		83500000,  { 0x0028, 0x0038, 0x0038 },
	}, {
		146250000, { 0x0038, 0x0038, 0x0038 },
	}, {
		148500000, { 0x0000, 0x0038, 0x0038 },
	}, {
		~0UL,      { 0x0000, 0x0000, 0x0000},
	}
};

static const struct dw_hdmi_phy_config rockchip_phy_config[] = {
	/*pixelclk   symbol   term   vlev*/
	{ 74250000,  0x8009, 0x0004, 0x0272},
	{ 148500000, 0x802b, 0x0004, 0x028d},
	{ 165000000, 0x802b, 0x0004, 0x0209},
	{ 297000000, 0x8039, 0x0005, 0x028d},
	{ 594000000, 0x8039, 0x0000, 0x019d},
	{ ~0UL,	     0x0000, 0x0000, 0x0000}
};

static int rockchip_hdmi_parse_dt(struct rockchip_hdmi *hdmi)
{
	struct device_node *np = hdmi->dev->of_node;

	hdmi->regmap = syscon_regmap_lookup_by_phandle(np, "rockchip,grf");
	if (IS_ERR(hdmi->regmap)) {
		DRM_DEV_ERROR(hdmi->dev, "Unable to get rockchip,grf\n");
		return PTR_ERR(hdmi->regmap);
	}

	hdmi->vpll_clk = devm_clk_get(hdmi->dev, "vpll");
	if (PTR_ERR(hdmi->vpll_clk) == -ENOENT) {
		hdmi->vpll_clk = NULL;
	} else if (PTR_ERR(hdmi->vpll_clk) == -EPROBE_DEFER) {
		return -EPROBE_DEFER;
	} else if (IS_ERR(hdmi->vpll_clk)) {
		DRM_DEV_ERROR(hdmi->dev, "failed to get grf clock\n");
		return PTR_ERR(hdmi->vpll_clk);
	}

	hdmi->grf_clk = devm_clk_get(hdmi->dev, "grf");
	if (PTR_ERR(hdmi->grf_clk) == -ENOENT) {
		hdmi->grf_clk = NULL;
	} else if (PTR_ERR(hdmi->grf_clk) == -EPROBE_DEFER) {
		return -EPROBE_DEFER;
	} else if (IS_ERR(hdmi->grf_clk)) {
		DRM_DEV_ERROR(hdmi->dev, "failed to get grf clock\n");
		return PTR_ERR(hdmi->grf_clk);
	}

	hdmi->hclk_vio = devm_clk_get(hdmi->dev, "hclk_vio");
	if (PTR_ERR(hdmi->hclk_vio) == -ENOENT) {
		hdmi->hclk_vio = NULL;
	} else if (PTR_ERR(hdmi->hclk_vio) == -EPROBE_DEFER) {
		return -EPROBE_DEFER;
	} else if (IS_ERR(hdmi->hclk_vio)) {
		DRM_DEV_ERROR(hdmi->dev, "failed to get grf clock\n");
		return PTR_ERR(hdmi->hclk_vio);
	}
	return 0;
}

static enum drm_mode_status
dw_hdmi_rockchip_mode_valid(struct drm_connector *connector,
			    const struct drm_display_mode *mode)
{
	const struct dw_hdmi_mpll_config *mpll_cfg = rockchip_mpll_cfg;
	int pclk = mode->clock * 1000;
	bool valid = false;
	int i;

	for (i = 0; mpll_cfg[i].mpixelclock != (~0UL); i++) {
		if (pclk == mpll_cfg[i].mpixelclock) {
			valid = true;
			break;
		}
	}

	return (valid) ? MODE_OK : MODE_BAD;
}

static enum drm_mode_status
dw_hdmi_rk3228_mode_valid(struct drm_connector *connector,
			  const struct drm_display_mode *mode)
{
	struct drm_display_info *info = &connector->display_info;
	int max_tmds_clock = max(info->max_tmds_clock, 165000);
	int clock = mode->clock;

	if (connector->ycbcr_420_allowed && drm_mode_is_420(info, mode) &&
	    (info->color_formats & DRM_COLOR_FORMAT_YCRCB420))
		clock /= 2;

	if (clock > max_tmds_clock || clock > 340000)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static enum drm_mode_status
dw_hdmi_rockchip_bridge_mode_valid(struct drm_bridge *bridge,
				   const struct drm_display_mode *mode)
{
	struct rockchip_hdmi *hdmi = to_rockchip_hdmi(bridge);
	long rate;

	if (hdmi->vpll_clk) {
		rate = clk_round_rate(hdmi->vpll_clk, mode->clock * 1000);
		if (rate < 0)
			return MODE_CLOCK_RANGE;
	}

	return MODE_OK;
}

static void
dw_hdmi_rockchip_bridge_mode_set(struct drm_bridge *bridge,
				 const struct drm_display_mode *mode,
				 const struct drm_display_mode *adjusted_mode)
{
	struct rockchip_hdmi *hdmi = to_rockchip_hdmi(bridge);
	struct drm_crtc_state *crtc_state = to_crtc_state(adjusted_mode);
	struct rockchip_crtc_state *s = to_rockchip_crtc_state(crtc_state);

	if (hdmi->phy)
		phy_set_bus_width(hdmi->phy, s->bus_width);

	clk_set_rate(hdmi->vpll_clk, adjusted_mode->clock * 1000);
}

static void dw_hdmi_rockchip_bridge_disable(struct drm_bridge *bridge)
{
	struct rockchip_hdmi *hdmi = to_rockchip_hdmi(bridge);

	if (hdmi->phy)
		phy_set_bus_width(hdmi->phy, 8);
}

static void dw_hdmi_rockchip_bridge_enable(struct drm_bridge *bridge)
{
	struct rockchip_hdmi *hdmi = to_rockchip_hdmi(bridge);
	struct drm_encoder *encoder = bridge->encoder;
	u32 val;
	int ret;

	if (hdmi->chip_data->lcdsel_grf_reg < 0)
		return;

	ret = drm_of_encoder_active_endpoint_id(hdmi->dev->of_node, encoder);
	if (ret)
		val = hdmi->chip_data->lcdsel_lit;
	else
		val = hdmi->chip_data->lcdsel_big;

	ret = clk_prepare_enable(hdmi->grf_clk);
	if (ret < 0) {
		DRM_DEV_ERROR(hdmi->dev, "failed to enable grfclk %d\n", ret);
		return;
	}

	ret = regmap_write(hdmi->regmap, hdmi->chip_data->lcdsel_grf_reg, val);
	if (ret != 0)
		DRM_DEV_ERROR(hdmi->dev, "Could not write to GRF: %d\n", ret);

	clk_disable_unprepare(hdmi->grf_clk);
	DRM_DEV_DEBUG(hdmi->dev, "vop %s output to hdmi\n",
		      ret ? "LIT" : "BIG");
}

static bool is_rgb(u32 format)
{
	switch (format) {
	case MEDIA_BUS_FMT_RGB888_1X24:
	case MEDIA_BUS_FMT_RGB101010_1X30:
	case MEDIA_BUS_FMT_RGB121212_1X36:
	case MEDIA_BUS_FMT_RGB161616_1X48:
		return true;
	default:
		return false;
	}
}

static bool is_yuv444(u32 format)
{
	switch (format) {
	case MEDIA_BUS_FMT_YUV8_1X24:
	case MEDIA_BUS_FMT_YUV10_1X30:
	case MEDIA_BUS_FMT_YUV12_1X36:
	case MEDIA_BUS_FMT_YUV16_1X48:
		return true;
	default:
		return false;
	}
}

static bool is_yuv420(u32 format)
{
	switch (format) {
	case MEDIA_BUS_FMT_UYYVYY8_0_5X24:
	case MEDIA_BUS_FMT_UYYVYY10_0_5X30:
	case MEDIA_BUS_FMT_UYYVYY12_0_5X36:
	case MEDIA_BUS_FMT_UYYVYY16_0_5X48:
		return true;
	default:
		return false;
	}
}

static bool is_10bit(u32 format)
{
	switch (format) {
	case MEDIA_BUS_FMT_RGB101010_1X30:
	case MEDIA_BUS_FMT_YUV10_1X30:
	case MEDIA_BUS_FMT_UYVY10_1X20:
	case MEDIA_BUS_FMT_UYYVYY10_0_5X30:
		return true;
	default:
		return false;
	}
}

static int
dw_hdmi_rockchip_bridge_atomic_check(struct drm_bridge *bridge,
				     struct drm_bridge_state *bridge_state,
				     struct drm_crtc_state *crtc_state,
				     struct drm_connector_state *conn_state)
{
	struct rockchip_hdmi *hdmi = to_rockchip_hdmi(bridge);
	struct rockchip_crtc_state *s = to_rockchip_crtc_state(crtc_state);
	struct drm_atomic_state *state = bridge_state->base.state;
	struct drm_crtc_state *old_crtc_state;
	struct rockchip_crtc_state *old_state;
	struct drm_bridge *next_bridge;
	struct drm_bridge_state *next_bridge_state;
	u32 format = bridge_state->output_bus_cfg.format;
	u32 next_format = format;

	next_bridge = drm_bridge_get_next_bridge(bridge);
	if (next_bridge) {
		next_bridge_state = drm_atomic_get_new_bridge_state(state, next_bridge);
		next_format = next_bridge_state->output_bus_cfg.format;
	}

	s->output_mode = ROCKCHIP_OUT_MODE_AAAA;
	s->output_type = DRM_MODE_CONNECTOR_HDMIA;
	s->output_bpc = 10;
	s->bus_format = format;

	s->bus_width = is_10bit(next_format) ? 10 : 8;
	if (is_yuv420(next_format)) {
		s->output_mode = ROCKCHIP_OUT_MODE_YUV420;
		s->bus_width /= 2;
	}

	hdmi->input_bus_format = bridge_state->input_bus_cfg.format;
	hdmi->output_bus_format = format;

	if (!crtc_state->mode_changed) {
		old_crtc_state = drm_atomic_get_old_crtc_state(state, conn_state->crtc);
		if (old_crtc_state) {
			old_state = to_rockchip_crtc_state(old_crtc_state);
			if (s->bus_format != old_state->bus_format ||
			    s->bus_width != old_state->bus_width)
				crtc_state->mode_changed = true;
		} else
			crtc_state->mode_changed = true;
	}

	return 0;
}

static u32 *dw_hdmi_rockchip_get_input_bus_fmts(struct drm_bridge *bridge,
					struct drm_bridge_state *bridge_state,
					struct drm_crtc_state *crtc_state,
					struct drm_connector_state *conn_state,
					u32 output_fmt,
					unsigned int *num_input_fmts)
{
	struct rockchip_hdmi *hdmi = to_rockchip_hdmi(bridge);
	struct drm_encoder *encoder = bridge->encoder;
	struct drm_connector *connector = conn_state->connector;
	u32 *input_fmt;
	bool has_10bit = true;

	*num_input_fmts = 0;

	if (drm_of_encoder_active_endpoint_id(hdmi->dev->of_node, encoder))
		has_10bit = false;

	if (!has_10bit && is_10bit(output_fmt))
		return NULL;

	if (is_yuv444(output_fmt)) {
		if (!hdmi->chip_data->ycbcr_444_allowed)
			return NULL;
	} else if (is_yuv420(output_fmt)) {
		if (!connector->ycbcr_420_allowed)
			return NULL;
	} else if (!is_rgb(output_fmt))
		return NULL;

	input_fmt = kzalloc(sizeof(*input_fmt), GFP_KERNEL);
	if (!input_fmt)
		return NULL;

	*num_input_fmts = 1;
	*input_fmt = output_fmt;

	return input_fmt;
}

static const struct drm_bridge_funcs dw_hdmi_rockchip_bridge_funcs = {
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_reset = drm_atomic_helper_bridge_reset,
	.mode_valid = dw_hdmi_rockchip_bridge_mode_valid,
	.mode_set = dw_hdmi_rockchip_bridge_mode_set,
	.enable = dw_hdmi_rockchip_bridge_enable,
	.disable = dw_hdmi_rockchip_bridge_disable,
	.atomic_check = dw_hdmi_rockchip_bridge_atomic_check,
	.atomic_get_input_bus_fmts = dw_hdmi_rockchip_get_input_bus_fmts,
};

static void dw_hdmi_rockchip_genphy_disable(struct dw_hdmi *dw_hdmi, void *data)
{
	struct rockchip_hdmi *hdmi = (struct rockchip_hdmi *)data;

	while (hdmi->phy->power_count > 0)
		phy_power_off(hdmi->phy);
}

static int dw_hdmi_rockchip_genphy_init(struct dw_hdmi *dw_hdmi, void *data,
			     struct drm_display_mode *mode)
{
	struct rockchip_hdmi *hdmi = (struct rockchip_hdmi *)data;

	dw_hdmi_rockchip_genphy_disable(dw_hdmi, data);
	dw_hdmi_set_high_tmds_clock_ratio(dw_hdmi);

	return phy_power_on(hdmi->phy);
}

static void dw_hdmi_rk3228_setup_hpd(struct dw_hdmi *dw_hdmi, void *data)
{
	struct rockchip_hdmi *hdmi = (struct rockchip_hdmi *)data;

	dw_hdmi_phy_setup_hpd(dw_hdmi, data);

	regmap_write(hdmi->regmap,
		RK3228_GRF_SOC_CON6,
		HIWORD_UPDATE(RK3228_HDMI_HPD_VSEL | RK3228_HDMI_SDA_VSEL |
			      RK3228_HDMI_SCL_VSEL,
			      RK3228_HDMI_HPD_VSEL | RK3228_HDMI_SDA_VSEL |
			      RK3228_HDMI_SCL_VSEL));

	regmap_write(hdmi->regmap,
		RK3228_GRF_SOC_CON2,
		HIWORD_UPDATE(RK3228_HDMI_SDAIN_MSK | RK3228_HDMI_SCLIN_MSK,
			      RK3228_HDMI_SDAIN_MSK | RK3228_HDMI_SCLIN_MSK));
}

static enum drm_connector_status
dw_hdmi_rk3328_read_hpd(struct dw_hdmi *dw_hdmi, void *data)
{
	struct rockchip_hdmi *hdmi = (struct rockchip_hdmi *)data;
	enum drm_connector_status status;

	status = dw_hdmi_phy_read_hpd(dw_hdmi, data);

	if (status == connector_status_connected)
		regmap_write(hdmi->regmap,
			RK3328_GRF_SOC_CON4,
			HIWORD_UPDATE(RK3328_HDMI_SDA_5V | RK3328_HDMI_SCL_5V,
				      RK3328_HDMI_SDA_5V | RK3328_HDMI_SCL_5V));
	else
		regmap_write(hdmi->regmap,
			RK3328_GRF_SOC_CON4,
			HIWORD_UPDATE(0, RK3328_HDMI_SDA_5V |
					 RK3328_HDMI_SCL_5V));
	return status;
}

static void dw_hdmi_rk3328_setup_hpd(struct dw_hdmi *dw_hdmi, void *data)
{
	struct rockchip_hdmi *hdmi = (struct rockchip_hdmi *)data;

	dw_hdmi_phy_setup_hpd(dw_hdmi, data);

	/* Enable and map pins to 3V grf-controlled io-voltage */
	regmap_write(hdmi->regmap,
		RK3328_GRF_SOC_CON4,
		HIWORD_UPDATE(0, RK3328_HDMI_HPD_SARADC | RK3328_HDMI_CEC_5V |
				 RK3328_HDMI_SDA_5V | RK3328_HDMI_SCL_5V |
				 RK3328_HDMI_HPD_5V));
	regmap_write(hdmi->regmap,
		RK3328_GRF_SOC_CON3,
		HIWORD_UPDATE(0, RK3328_HDMI_SDA5V_GRF | RK3328_HDMI_SCL5V_GRF |
				 RK3328_HDMI_HPD5V_GRF |
				 RK3328_HDMI_CEC5V_GRF));
	regmap_write(hdmi->regmap,
		RK3328_GRF_SOC_CON2,
		HIWORD_UPDATE(RK3328_HDMI_SDAIN_MSK | RK3328_HDMI_SCLIN_MSK,
			      RK3328_HDMI_SDAIN_MSK | RK3328_HDMI_SCLIN_MSK |
			      RK3328_HDMI_HPD_IOE));
}

static const struct dw_hdmi_phy_ops rk3228_hdmi_phy_ops = {
	.init		= dw_hdmi_rockchip_genphy_init,
	.disable	= dw_hdmi_rockchip_genphy_disable,
	.read_hpd	= dw_hdmi_phy_read_hpd,
	.update_hpd	= dw_hdmi_phy_update_hpd,
	.setup_hpd	= dw_hdmi_rk3228_setup_hpd,
};

static struct rockchip_hdmi_chip_data rk3228_chip_data = {
	.lcdsel_grf_reg = -1,
	.chip_type = 3228,
};

static const struct dw_hdmi_plat_data rk3228_hdmi_drv_data = {
	.mode_valid = dw_hdmi_rk3228_mode_valid,
	.phy_data = &rk3228_chip_data,
	.phy_ops = &rk3228_hdmi_phy_ops,
	.phy_name = "inno_dw_hdmi_phy2",
	.phy_force_vendor = true,
};

static struct rockchip_hdmi_chip_data rk3288_chip_data = {
	.lcdsel_grf_reg = RK3288_GRF_SOC_CON6,
	.lcdsel_big = HIWORD_UPDATE(0, RK3288_HDMI_LCDC_SEL),
	.lcdsel_lit = HIWORD_UPDATE(RK3288_HDMI_LCDC_SEL, RK3288_HDMI_LCDC_SEL),
	.chip_type = 3288,
};

static const struct dw_hdmi_plat_data rk3288_hdmi_drv_data = {
	.mode_valid = dw_hdmi_rockchip_mode_valid,
	.mpll_cfg   = rockchip_mpll_cfg,
	.cur_ctr    = rockchip_cur_ctr,
	.phy_config = rockchip_phy_config,
	.phy_data = &rk3288_chip_data,
};

static const struct dw_hdmi_phy_ops rk3328_hdmi_phy_ops = {
	.init		= dw_hdmi_rockchip_genphy_init,
	.disable	= dw_hdmi_rockchip_genphy_disable,
	.read_hpd	= dw_hdmi_rk3328_read_hpd,
	.update_hpd	= dw_hdmi_phy_update_hpd,
	.setup_hpd	= dw_hdmi_rk3328_setup_hpd,
};

static struct rockchip_hdmi_chip_data rk3328_chip_data = {
	.lcdsel_grf_reg = -1,
	.ycbcr_444_allowed = true,
	.chip_type = 3328,
};

static const struct dw_hdmi_plat_data rk3328_hdmi_drv_data = {
	.mode_valid = dw_hdmi_rk3228_mode_valid,
	.phy_data = &rk3328_chip_data,
	.phy_ops = &rk3328_hdmi_phy_ops,
	.phy_name = "inno_dw_hdmi_phy2",
	.phy_force_vendor = true,
	.use_drm_infoframe = true,
	.ycbcr_420_allowed = true,
};

static struct rockchip_hdmi_chip_data rk3399_chip_data = {
	.lcdsel_grf_reg = RK3399_GRF_SOC_CON20,
	.lcdsel_big = HIWORD_UPDATE(0, RK3399_HDMI_LCDC_SEL),
	.lcdsel_lit = HIWORD_UPDATE(RK3399_HDMI_LCDC_SEL, RK3399_HDMI_LCDC_SEL),
	.chip_type = 3399,
};

static const struct dw_hdmi_plat_data rk3399_hdmi_drv_data = {
	.mode_valid = dw_hdmi_rockchip_mode_valid,
	.mpll_cfg   = rockchip_mpll_cfg,
	.mpll_cfg_420 = rockchip_mpll_cfg_420,
	.cur_ctr    = rockchip_cur_ctr,
	.phy_config = rockchip_phy_config,
	.phy_data = &rk3399_chip_data,
	.use_drm_infoframe = true,
};

static const struct of_device_id dw_hdmi_rockchip_dt_ids[] = {
	{ .compatible = "rockchip,rk3228-dw-hdmi",
	  .data = &rk3228_hdmi_drv_data
	},
	{ .compatible = "rockchip,rk3288-dw-hdmi",
	  .data = &rk3288_hdmi_drv_data
	},
	{ .compatible = "rockchip,rk3328-dw-hdmi",
	  .data = &rk3328_hdmi_drv_data
	},
	{ .compatible = "rockchip,rk3399-dw-hdmi",
	  .data = &rk3399_hdmi_drv_data
	},
	{},
};
MODULE_DEVICE_TABLE(of, dw_hdmi_rockchip_dt_ids);

static unsigned long
dw_hdmi_rockchip_get_input_bus_format(void *data)
{
	struct rockchip_hdmi *hdmi = (struct rockchip_hdmi *)data;
	return hdmi->input_bus_format;
}

static unsigned long
dw_hdmi_rockchip_get_output_bus_format(void *data)
{
	struct rockchip_hdmi *hdmi = (struct rockchip_hdmi *)data;
	return hdmi->output_bus_format;
}

void
dw_hdmi_rockchip_plugged_event(void *data, enum drm_connector_status status)
{
	struct rockchip_hdmi *hdmi = (struct rockchip_hdmi *)data;

	/*
	   When some RK3328 (such as Rock64) first bootup, color can be incorrect.
	   Reproduced when display powered on immediately before board, as well as
	   when display powered back on after being off for awhile.
	   Force dw_hdmi_setup to run again.
	*/
	if (hdmi->hdmi && hdmi->chip_data->chip_type == 3328) {
		hdmi->hdmi->force_setup = true;
	}
}

static int dw_hdmi_rockchip_init(struct rockchip_hdmi *hdmi)
{
	int ret = clk_prepare_enable(hdmi->grf_clk);
	if (ret < 0) {
		dev_err(hdmi->dev, "failed to enable grfclk %d\n", ret);
		return -EPROBE_DEFER;
	}
	switch (hdmi->chip_data->chip_type) {
	case 3328:
		/* Map HPD pin to 3V io */
		regmap_write(hdmi->regmap,
			     RK3328_GRF_SOC_CON4,
			     RK3328_HDMI_IO_3V |
			     RK3328_HDMI_HPD_3V);
		/* Map ddc pin to 5V io */
		regmap_write(hdmi->regmap,
			     RK3328_GRF_SOC_CON3,
			     RK3328_HDMI_IO_CTRL);
		regmap_write(hdmi->regmap,
			     RK3328_GRF_SOC_CON2,
			     RK3328_HDMI_DDC_MASK_EN |
			     BIT(18));
		break;
	case 3228:
		regmap_write(hdmi->regmap, RK3228_GRF_SOC_CON2,
			     RK3228_HDMI_DDC_MASK_EN);
		regmap_write(hdmi->regmap, RK3228_GRF_SOC_CON6,
			     RK3228_HDMI_IO_3V);
		break;
	}
	clk_disable_unprepare(hdmi->grf_clk);
	return 0;
}

static int dw_hdmi_rockchip_bind(struct device *dev, struct device *master,
				 void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct dw_hdmi_plat_data *plat_data;
	const struct of_device_id *match;
	struct drm_device *drm = data;
	struct drm_bridge *next_bridge;
	struct drm_encoder *encoder;
	struct rockchip_hdmi *hdmi;
	int ret;

	if (!pdev->dev.of_node)
		return -ENODEV;

	hdmi = devm_kzalloc(&pdev->dev, sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi)
		return -ENOMEM;

	match = of_match_node(dw_hdmi_rockchip_dt_ids, pdev->dev.of_node);
	plat_data = devm_kmemdup(&pdev->dev, match->data,
					     sizeof(*plat_data), GFP_KERNEL);
	if (!plat_data)
		return -ENOMEM;

	hdmi->dev = &pdev->dev;
	hdmi->chip_data = plat_data->phy_data;

	plat_data->phy_data = hdmi;
	plat_data->get_input_bus_format = dw_hdmi_rockchip_get_input_bus_format;
	plat_data->get_output_bus_format = dw_hdmi_rockchip_get_output_bus_format;
	plat_data->plugged_event = dw_hdmi_rockchip_plugged_event;

	encoder = &hdmi->encoder;

	encoder->possible_crtcs = drm_of_find_possible_crtcs(drm, dev->of_node);
	/*
	 * If we failed to find the CRTC(s) which this encoder is
	 * supposed to be connected to, it's because the CRTC has
	 * not been registered yet.  Defer probing, and hope that
	 * the required CRTC is added later.
	 */
	if (encoder->possible_crtcs == 0)
		return -EPROBE_DEFER;

	ret = rockchip_hdmi_parse_dt(hdmi);
	if (ret) {
		DRM_DEV_ERROR(hdmi->dev, "Unable to parse OF data\n");
		return ret;
	}

	ret = clk_prepare_enable(hdmi->vpll_clk);
	if (ret) {
		DRM_DEV_ERROR(hdmi->dev, "Failed to enable HDMI vpll: %d\n",
			      ret);
		return ret;
	}

	ret = clk_prepare_enable(hdmi->hclk_vio);
	if (ret) {
		DRM_DEV_ERROR(hdmi->dev, "Failed to enable HDMI hclk_vio: %d\n",
			ret);
		return ret;
	}

	hdmi->phy = devm_phy_optional_get(dev, "hdmi");
	if (IS_ERR(hdmi->phy)) {
		ret = PTR_ERR(hdmi->phy);
		if (ret != -EPROBE_DEFER)
			DRM_DEV_ERROR(hdmi->dev, "failed to get phy\n");
		return ret;
	}

	if (hdmi->chip_data->chip_type == 3328 || hdmi->chip_data->chip_type == 3228) {
		ret = dw_hdmi_rockchip_init(hdmi);
		if (ret < 0)
			return ret;
	}

	drm_simple_encoder_init(drm, encoder, DRM_MODE_ENCODER_TMDS);

	hdmi->bridge.funcs = &dw_hdmi_rockchip_bridge_funcs;
	drm_bridge_attach(encoder, &hdmi->bridge, NULL, 0);

	platform_set_drvdata(pdev, hdmi);

	hdmi->hdmi = dw_hdmi_probe(pdev, plat_data);

	/*
	 * If dw_hdmi_probe() fails we'll never call dw_hdmi_unbind(),
	 * which would have called the encoder cleanup.  Do it manually.
	 */
	if (IS_ERR(hdmi->hdmi)) {
		ret = PTR_ERR(hdmi->hdmi);
		drm_encoder_cleanup(encoder);
		clk_disable_unprepare(hdmi->vpll_clk);
	}

	next_bridge = of_drm_find_bridge(pdev->dev.of_node);
	if (next_bridge)
		drm_bridge_attach(encoder, next_bridge, &hdmi->bridge, 0);

	return ret;
}

static void dw_hdmi_rockchip_unbind(struct device *dev, struct device *master,
				    void *data)
{
	struct rockchip_hdmi *hdmi = dev_get_drvdata(dev);

	dw_hdmi_unbind(hdmi->hdmi);
	clk_disable_unprepare(hdmi->vpll_clk);
}

static const struct component_ops dw_hdmi_rockchip_ops = {
	.bind	= dw_hdmi_rockchip_bind,
	.unbind	= dw_hdmi_rockchip_unbind,
};

static int dw_hdmi_rockchip_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &dw_hdmi_rockchip_ops);
}

static int dw_hdmi_rockchip_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &dw_hdmi_rockchip_ops);

	return 0;
}

static int __maybe_unused dw_hdmi_rockchip_resume(struct device *dev)
{
	struct rockchip_hdmi *hdmi = dev_get_drvdata(dev);

	dw_hdmi_resume(hdmi->hdmi);

	return 0;
}

static const struct dev_pm_ops dw_hdmi_rockchip_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(NULL, dw_hdmi_rockchip_resume)
};

struct platform_driver dw_hdmi_rockchip_pltfm_driver = {
	.probe  = dw_hdmi_rockchip_probe,
	.remove = dw_hdmi_rockchip_remove,
	.driver = {
		.name = "dwhdmi-rockchip",
		.pm = &dw_hdmi_rockchip_pm,
		.of_match_table = dw_hdmi_rockchip_dt_ids,
	},
};
