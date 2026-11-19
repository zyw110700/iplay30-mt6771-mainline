// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal DSI "passthrough" component for the MT6771 bring-up.
 *
 * The bootloader already brings up the DSI host, the MIPI TX PHY and the
 * panel.  Until the real mtk_dsi + mtk_mipi_tx + panel stack is ported to
 * MT6771, this component simply provides a DRM encoder and connector with the
 * panel's native mode so that the DDP pipeline (OVL/RDMA/COLOR/... ) can be
 * driven by the mainline mtk-drm KMS driver without touching the DSI block
 * itself.
 */

#include <linux/component.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_encoder.h>
#include <drm/drm_modes.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#include "mtk_ddp_comp.h"
#include "mtk_drm_drv.h"

#define MTK_DSI_PASSTHROUGH_WIDTH	1200
#define MTK_DSI_PASSTHROUGH_HEIGHT	1920
#define MTK_DSI_PASSTHROUGH_VREFRESH	60

#define MTK_DSI_PASSTHROUGH_HFRONT	60
#define MTK_DSI_PASSTHROUGH_HSYNC	20
#define MTK_DSI_PASSTHROUGH_HBACK	60
#define MTK_DSI_PASSTHROUGH_VFRONT	20
#define MTK_DSI_PASSTHROUGH_VSYNC	10
#define MTK_DSI_PASSTHROUGH_VBACK	30

struct mtk_dsi_passthrough {
	struct drm_encoder encoder;
	struct drm_connector connector;
};

static const struct drm_display_mode mtk_dsi_passthrough_mode = {
	.clock = (MTK_DSI_PASSTHROUGH_WIDTH + MTK_DSI_PASSTHROUGH_HFRONT +
		  MTK_DSI_PASSTHROUGH_HSYNC + MTK_DSI_PASSTHROUGH_HBACK) *
		 (MTK_DSI_PASSTHROUGH_HEIGHT + MTK_DSI_PASSTHROUGH_VFRONT +
		  MTK_DSI_PASSTHROUGH_VSYNC + MTK_DSI_PASSTHROUGH_VBACK) *
		 MTK_DSI_PASSTHROUGH_VREFRESH / 1000,
	.hdisplay = MTK_DSI_PASSTHROUGH_WIDTH,
	.hsync_start = MTK_DSI_PASSTHROUGH_WIDTH + MTK_DSI_PASSTHROUGH_HFRONT,
	.hsync_end = MTK_DSI_PASSTHROUGH_WIDTH + MTK_DSI_PASSTHROUGH_HFRONT +
		     MTK_DSI_PASSTHROUGH_HSYNC,
	.htotal = MTK_DSI_PASSTHROUGH_WIDTH + MTK_DSI_PASSTHROUGH_HFRONT +
		  MTK_DSI_PASSTHROUGH_HSYNC + MTK_DSI_PASSTHROUGH_HBACK,
	.vdisplay = MTK_DSI_PASSTHROUGH_HEIGHT,
	.vsync_start = MTK_DSI_PASSTHROUGH_HEIGHT + MTK_DSI_PASSTHROUGH_VFRONT,
	.vsync_end = MTK_DSI_PASSTHROUGH_HEIGHT + MTK_DSI_PASSTHROUGH_VFRONT +
		     MTK_DSI_PASSTHROUGH_VSYNC,
	.vtotal = MTK_DSI_PASSTHROUGH_HEIGHT + MTK_DSI_PASSTHROUGH_VFRONT +
		  MTK_DSI_PASSTHROUGH_VSYNC + MTK_DSI_PASSTHROUGH_VBACK,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int
mtk_dsi_passthrough_detect(struct drm_connector *connector,
			   struct drm_modeset_acquire_ctx *ctx, bool force)
{
	return connector_status_connected;
}

static int mtk_dsi_passthrough_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &mtk_dsi_passthrough_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = 68;
	connector->display_info.height_mm = 109;
	connector->display_info.bpc = 8;

	return 1;
}

static const struct drm_connector_helper_funcs
mtk_dsi_passthrough_connector_helper_funcs = {
	.get_modes = mtk_dsi_passthrough_get_modes,
	.detect_ctx = mtk_dsi_passthrough_detect,
};

static const struct drm_connector_funcs mtk_dsi_passthrough_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int mtk_dsi_passthrough_bind(struct device *dev, struct device *master,
				    void *data)
{
	struct drm_device *drm = data;
	struct mtk_dsi_passthrough *priv = dev_get_drvdata(dev);
	int ret;

	ret = drm_simple_encoder_init(drm, &priv->encoder, DRM_MODE_ENCODER_DSI);
	if (ret)
		return ret;

	ret = mtk_find_possible_crtcs(drm, dev);
	if (ret < 0)
		return ret;
	priv->encoder.possible_crtcs = ret;

	ret = drm_connector_init(drm, &priv->connector,
				 &mtk_dsi_passthrough_connector_funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (ret)
		return ret;

	drm_connector_helper_add(&priv->connector,
				 &mtk_dsi_passthrough_connector_helper_funcs);

	return drm_connector_attach_encoder(&priv->connector, &priv->encoder);
}

static void mtk_dsi_passthrough_unbind(struct device *dev,
				       struct device *master, void *data)
{
	/*
	 * The encoder and connector are destroyed by
	 * drm_mode_config_cleanup() when the master is torn down.
	 */
}

static const struct component_ops mtk_dsi_passthrough_component_ops = {
	.bind = mtk_dsi_passthrough_bind,
	.unbind = mtk_dsi_passthrough_unbind,
};

static int mtk_dsi_passthrough_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_dsi_passthrough *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	ret = component_add(dev, &mtk_dsi_passthrough_component_ops);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add component\n");

	return 0;
}

static void mtk_dsi_passthrough_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mtk_dsi_passthrough_component_ops);
}

static const struct of_device_id mtk_dsi_passthrough_of_ids[] = {
	{ .compatible = "mediatek,mt6771-dsi-passthrough" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_dsi_passthrough_of_ids);

struct platform_driver mtk_dsi_passthrough_driver = {
	.probe = mtk_dsi_passthrough_probe,
	.remove = mtk_dsi_passthrough_remove,
	.driver = {
		.name = "mediatek-dsi-passthrough",
		.of_match_table = mtk_dsi_passthrough_of_ids,
	},
};
