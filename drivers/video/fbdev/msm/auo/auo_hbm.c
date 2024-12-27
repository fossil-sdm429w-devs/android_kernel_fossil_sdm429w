/* Copyright (c) 2018, The Linux Foundation. All rights reserved.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#ifndef __AUO_HBM___C
#define __AUO_HBM___C

#include <linux/videodev2.h>
#include <linux/bootmem.h>
#include <linux/console.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/memory.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/msm_mdp.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/proc_fs.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/file.h>
#include <linux/kthread.h>
#include <linux/dma-buf.h>
#include "mdss_fb.h"

#include "mdss_dsi_cmd.h"
#include "mdss_dsi.h"
#include "mdss_smmu.h"

#include "auo_hbm.h"

#define AUO_PARSE_DT_ERR	(1)
#define AUO_UNINITILIZE		(false)
#define AUO_PARSE_DT_SUCCESS	(0)

/* Protects access For both FB device node and mdss_dsi driver */
static DEFINE_MUTEX(boost_mode_lock);

static bool auo_cur_hbm  = AUO_UNINITILIZE;
static bool g_auo_pre_init = AUO_UNINITILIZE;

ssize_t mdss_fb_get_boost_mode(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret;

	ret = scnprintf(buf, PAGE_SIZE, "%d\n",
		auo_cur_hbm ? 1:0);

	pr_err("%s: boost_mode: %d\n", __func__, auo_cur_hbm);
	return ret;
}
EXPORT_SYMBOL(mdss_fb_get_boost_mode);

ssize_t mdss_fb_set_boost_mode(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int rc = 0;
	int boost_mode = 0;

	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = fbi->par;
	struct mdss_dsi_ctrl_pdata *ctrl;

	ctrl = container_of(dev_get_platdata(&mfd->pdev->dev),
				struct mdss_dsi_ctrl_pdata, panel_data);
	if (!ctrl) {
		pr_err("%s: DSI ctrl not available\n", __func__);
		return -EINVAL;
	}

	rc = kstrtoint(buf, 10, &boost_mode);
	pr_err("%s enable: %s\n", __func__, buf);
	if (rc) {
		pr_err("kstrtoint failed. rc=%d\n", rc);
		return rc;
	}

	if (!mdss_fb_is_power_on_interactive(mfd)) {
		pr_err("only normal mode can be the entry to HBM\n");
		return -EINVAL;
	}

	if (mfd->panel_info->type !=  MIPI_CMD_PANEL) {
		pr_err("support for command mode panel only\n");
	} else
		mdss_auo_dsi_set_boost_mode(ctrl,boost_mode ? true:false);

	return count;
}
EXPORT_SYMBOL(mdss_fb_set_boost_mode);

static void mdss_dsi_brightness_boost_on(struct mdss_dsi_ctrl_pdata *ctrl)
{
	struct dsi_panel_cmds *hbm_on_cmds = NULL;

	/* U128BLX and U128BLA only has one hbm modes */
#if !(defined (CONFIG_TOUCHSCREEN_RAYDIUM_U128BLX01_CHIPSET) || \
	defined (CONFIG_TOUCHSCREEN_RAYDIUM_U128BLA03_CHIPSET))
	pr_err("%s: id3_code[0] = 0x%02x\n", __func__, ctrl->id3_code[0]);
	switch (ctrl->id3_code[0]) {
	case 0x01:
		hbm_on_cmds = &ctrl->hbm0_on_cmds;
		break;
	case 0x03:
		hbm_on_cmds = &ctrl->hbm1_on_cmds;
		break;

	case 0x04:
		hbm_on_cmds = &ctrl->hbm1_on_cmds;
		break;
	default:
		/*
		 * technically speaking, we won't get here since
		 * the value would be set anyway during boot phase
		 */
		pr_err("%s: HBM err: default case (%d)\n", __func__,
				ctrl->id3_code[0]);

		if (hbm_on_cmds == NULL)
			hbm_on_cmds = &ctrl->hbm1_on_cmds;
		break;
	}
#else
	hbm_on_cmds = &ctrl->hbm0_on_cmds;
#endif
	if (hbm_on_cmds->cmd_cnt) {
		mdss_dsi_panel_cmds_send(ctrl, hbm_on_cmds, CMD_REQ_COMMIT);

		pr_err("%s(): boost on!\n", __func__);
	}
}

static void mdss_dsi_brightness_boost_off(struct mdss_dsi_ctrl_pdata *ctrl)
{
	struct dsi_panel_cmds *hbm_off_cmds = NULL;

	/* write back to HBM off command flow */
	hbm_off_cmds = &ctrl->hbm_off_cmds;

	/* For AUO 390p: H120BLX01.0: need to read_back_param */
#if !(defined (CONFIG_TOUCHSCREEN_RAYDIUM_U128BLX01_CHIPSET) || \
	defined (CONFIG_TOUCHSCREEN_RAYDIUM_U128BLA03_CHIPSET))

	pr_err("%s: read_back_param[0] = 0x%02x\n", __func__,
			ctrl->read_back_param[0]);
	hbm_off_cmds->cmds[12].payload[1] = ctrl->read_back_param[0];
#endif
	if (hbm_off_cmds->cmd_cnt) {
		mdss_dsi_panel_cmds_send(ctrl, hbm_off_cmds, CMD_REQ_COMMIT);
		pr_err("%s: boost off!\n", __func__);
	}
}


#if !( defined(CONFIG_TOUCHSCREEN_RAYDIUM_U128BLX01_CHIPSET) || \
	defined(CONFIG_TOUCHSCREEN_RAYDIUM_U128BLA03_CHIPSET))
static int auo_dsi_cmd_read(struct mdss_dsi_ctrl_pdata *ctrl,
		char page, char addr, void (*fxn)(int), char *rbuf, int len)
{
	static unsigned char _dcs_cmd[2] = {0x00, 0x00};
	static struct dsi_cmd_desc _dcs_read_cmd = {
		{DTYPE_DCS_READ, 1, 0, 1, 5, sizeof(_dcs_cmd)}, _dcs_cmd};

	struct dcs_cmd_req cmdreq;
	struct mdss_panel_info *pinfo;

	pinfo = &(ctrl->panel_data.panel_info);
	if (pinfo->dcs_cmd_by_left) {
		if (ctrl->ndx != DSI_CTRL_LEFT)
		return -EINVAL;
	}

	/* switch to the correct page prior to reading */
	mdss_dsi_switch_page(ctrl, page);

	/* clear the returned buffer */
	memset(rbuf, 0, len);

	_dcs_cmd[0] = addr;
	memset(&cmdreq, 0, sizeof(cmdreq));
	cmdreq.cmds = &_dcs_read_cmd;
	cmdreq.cmds_cnt = 1;
	cmdreq.flags = CMD_REQ_RX | CMD_REQ_COMMIT;
	cmdreq.rlen = len;
	cmdreq.rbuf = rbuf;
	cmdreq.cb = fxn; /* call back */
	/*
	 * blocked here, until call back called
	 */

	return mdss_dsi_cmdlist_put(ctrl, &cmdreq);
}
#endif /* H120BLX01.0 */

int mdss_auo_dsi_read_id_code(struct mdss_dsi_ctrl_pdata *ctrl)
{
	if (!g_auo_pre_init) {
		g_auo_pre_init = true;
		/* For AUO U128BLX  and H128BLXcontroller
		*  Not supported read id_code, initialize to 0
		*/
#if defined(CONFIG_TOUCHSCREEN_RAYDIUM_U128BLX01_CHIPSET) || \
	defined(CONFIG_TOUCHSCREEN_RAYDIUM_U128BLA03_CHIPSET)
		ctrl->id3_code[0] = 0;
#else
		/* For AUO 390p: H120BLX01.0 controller
		*  Supported command read id_code
		*/
		auo_dsi_cmd_read(ctrl, 0x01, 0x19, NULL,
					ctrl->read_back_param, 1);
		pr_err("%s: read_back_param[0] = 0x%02x\n", __func__,
				ctrl->read_back_param[0]);
		auo_dsi_cmd_read(ctrl, 0x00, 0xDC, NULL,
				ctrl->id3_code, 1);
		pr_err("%s: id3_code[0] = 0x%02x\n", __func__,
				ctrl->id3_code[0]);
		/* switch back to original page */
		mdss_dsi_switch_page(ctrl, 0x00);
#endif
	}
	return 0;
}
EXPORT_SYMBOL(mdss_auo_dsi_read_id_code);

int mdss_auo_dsi_parse_hbm_dt(struct device_node *np,
		struct mdss_dsi_ctrl_pdata *ctrl_pdata)
{
#if defined (CONFIG_TOUCHSCREEN_RAYDIUM_U128BLX01_CHIPSET) || \
	defined (CONFIG_TOUCHSCREEN_RAYDIUM_U128BLA03_CHIPSET)
	pr_err("%s: parse hbm\n", __func__);
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->hbm0_on_cmds,
		"qcom,mdss-dsi-hbm0-on-command", NULL);
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->hbm1_on_cmds,
		"qcom,mdss-dsi-hbm1-on-command", NULL);
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->hbm_off_cmds,
		"qcom,mdss-dsi-hbm-off-command", NULL);
#endif
	return 0;
}
EXPORT_SYMBOL(mdss_auo_dsi_parse_hbm_dt);

/* Enable/disable boost mode of AUO panel */
int mdss_auo_dsi_set_boost_mode(struct mdss_dsi_ctrl_pdata *ctrl,
					bool boost_mode)
{
	mutex_lock(&boost_mode_lock);
	if (boost_mode) {
		if (!auo_cur_hbm) {
			mdss_dsi_brightness_boost_on(ctrl);
			auo_cur_hbm = 1;
		}
	} else {
		if (auo_cur_hbm) {
			mdss_dsi_brightness_boost_off(ctrl);
			auo_cur_hbm = 0;
		}
	}
	mutex_unlock(&boost_mode_lock);
	return 0;
}
EXPORT_SYMBOL(mdss_auo_dsi_set_boost_mode);


#endif /*__AUO_HBM___C */


