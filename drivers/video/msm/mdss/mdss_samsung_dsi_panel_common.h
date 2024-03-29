/* Copyright (c) 2012, Samsung Electronics. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of Code Aurora Forum, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef SAMSUNG_DSI_PANEL_H
#define SAMSUNG_DSI_PANEL_H
#define MAX_PANEL_NAME_SIZE 100
#define RECOVERY_BRIGHTNESS 180
#define LCD_DEBUG(X, ...) pr_info("[LCD]%s:"X, __func__, ## __VA_ARGS__);

#include "smart_dimming.h"
#include "smart_mtp_common.h"

#define MAX_BL 255

enum mipi_samsung_cmd_list {
	PANEL_READY_TO_ON,
	PANEL_DISP_OFF,
	PANEL_DISPLAY_ON_SEQ,
	PANEL_DISPLAY_OFF_SEQ,
	PANEL_DISPLAY_ON,
	PANEL_DISPLAY_OFF,
	PANEL_DISPLAY_UNBLANK,
	PANEL_DISPLAY_BLANK,
	PANEL_ALL_PIXEL_OFF,
	PANEL_BRIGHT_CTRL,
	PANEL_MTP_ENABLE,
	PANEL_MTP_DISABLE,
	PANEL_NEED_FLIP,
	PANEL_ACL_OFF,
	PANEL_ACL_ON,
	PANEL_LATE_ON,
	PANEL_EARLY_OFF,
	PANEL_TOUCHSENSING_ON,
	PANEL_TOUCHSENSING_OFF,
	PANEL_TEAR_ON,
	PANEL_TEAR_OFF,
	PANEL_LDI_FPS_CHANGE,
	PANEL_LDI_SET_VDD_OFFSET, /*LDI_ADJ_VDD_OFFSET*/
	PANEL_LDI_SET_VDDM_OFFSET, /*LDI_ADJ_VDDM_OFFSET*/
	PANEL_PARTIAL_ON,
	PANEL_PARTIAL_OFF,
	MDNIE_ADB_TEST,
	PANEL_ALPM_ON,
	PANEL_ALPM_OFF,
	PANEL_ALPM_SET_PARTIAL_AREA,
	PANEL_HSYNC_ON,
#if defined(CONFIG_FB_MSM_MIPI_SAMSUNG_OCTA_CMD_FHD_FA2_PT_PANEL)
	PANEL_SET_TE_OSC_B,
	PANEL_SET_TE_RESTORE,
	PANEL_SET_TE,
	PANEL_SET_TE_1,
	PANEL_SET_TE_2,
#endif
#if defined(CONFIG_LCD_HMT)
	PANEL_HMT_BRIGHT,
	PANEL_LOW_PERSISTENCE_BRIGHT,
	PANEL_HMT_DIMMING,
	PANEL_ENABLE,
	PANEL_DISABLE,
	PANEL_HMT_AID,
	PANEL_HMT_REVERSE_ENABLE,
	PANEL_HMT_REVERSE_DISABLE,
	PANEL_HMT_KEY_ENABLE,
#endif
};

enum {
	MIPI_RESUME_STATE,
	MIPI_SUSPEND_STATE,
};

struct cmd_map {
	int *bl_level;
	int *cmd_idx;
	int size;
};

struct candella_lux_map {
	int *lux_tab;
	int *cmd_idx;
	int lux_tab_size;
	int bkl[256];
};

struct display_status {
	unsigned char acl_on;
	unsigned char curr_acl_cond;
	unsigned char is_smart_dim_loaded;
#if defined(CONFIG_LCD_HMT)
	unsigned char is_hmt_smart_dim_loaded;
#endif
	unsigned char is_panel_read_done;
	unsigned char is_mdnie_loaded;
	unsigned char auto_brightness;
	unsigned char recovery_boot_mode;
	unsigned char on;
	unsigned char wait_disp_on;
	int curr_elvss_idx;
	int curr_acl_idx;
	int curr_opr_idx;
	int curr_aid_idx;
	int curr_gamma_idx;
	int curr_vint_idx;
	int bright_level;
	int	recent_bright_level;

	int temperature;
	char temperature_value;
	int elvss_need_update;
	int siop_status;
	int hbm_mode;
	char elvss_value;

#if defined(CONFIG_DUAL_LCD)
	int lcd_sel;
#endif
};

#if defined(CONFIG_LCD_HMT)
#define HMT_SINGLE_SCAN 0
#define HMT_DUAL_SCAN 1
#define HMT_OFF -1

struct hmt_status {
	unsigned int hmt_bl_level;
	unsigned int hmt_on;
	unsigned int hmt_reverse;
	unsigned int hmt_low_persistence;
};
#endif

#if defined(CONFIG_FB_MSM_MIPI_SAMSUNG_OCTA_CMD_FHD_FA2_PT_PANEL)
struct te_fctrl_lookup_table {
	int te;
	char value;
} __packed;
struct te_offset_lookup_table {
	int te;
	int offset;
} __packed;
#endif

struct mipi_samsung_driver_data {
	struct display_status dstat;

	struct msm_fb_data_type *mfd;
	struct mdss_panel_data *pdata;
	struct mdss_dsi_ctrl_pdata *ctrl_pdata;

	struct mutex lock;
#if defined(CONFIG_DUAL_LCD)
	int lcd_panel_cmds;
#endif

#if defined(CONFIG_STATE_NOTIFIER)
	struct notifier_block notif;
#endif
	char panel_name[MAX_PANEL_NAME_SIZE];
	int panel;
	unsigned int manufacture_id;
	unsigned int manufacture_date;
	unsigned int manufacture_time;
	char ddi_id[5];
	unsigned int id2;
	unsigned int id3;
	struct smartdim_conf *sdimconf;
#if defined(CONFIG_LCD_HMT)
	struct hmt_status hmt_stat;
	struct smartdim_conf_hmt *sdimconf_reverse_hmt_single;
#endif
};
enum {
	PANEL_FHD_OCTA_S6E8FA0,
	PANEL_FHD_OCTA_S6E3FA0,
	PANEL_FHD_OCTA_S6E3FA0_CMD,
	PANEL_FHD_OCTA_S6E3FA2_CMD,
	PANEL_WQHD_OCTA_S6E3HA0_CMD,
	PANEL_WQHD_OCTA_S6E3HA2X01_CMD,
	PANEL_WQXGA_OCTA_S6E3HA2X01_CMD,
	PANEL_720P_AMOLED_S6E8AA3X01,
	PANEL_1080P_OCTA_S6E8FA0,
	PANEL_1080P_OCTA_S6E3FA0,
	PANEL_1080P_OCTA_S6E3FA0_CMD,
	PANEL_1080P_YOUM_S6E3FA1_CMD,
#if defined(CONFIG_FB_MSM_MIPI_SAMSUNG_OCTA_VIDEO_WVGA_S6E88A0_PT_PANEL)
	PANEL_WVGA_OCTA_S6E88A0,
#endif
};

struct panel_hrev {
	char *name;
	int panel_code;
};

void mdss_dsi_cmds_send(struct mdss_dsi_ctrl_pdata *ctrl, struct dsi_cmd_desc *cmds, int cnt, int flag);

#if defined(CONFIG_LCD_HMT)
int hmt_enable(int enable);
int hmt_reverse_update(int enable);
int hmt_bright_update(void);
#endif

#endif
