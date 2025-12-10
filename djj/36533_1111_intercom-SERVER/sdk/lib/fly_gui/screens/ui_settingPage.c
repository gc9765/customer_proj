#include "sys_config.h"
#include "typesdef.h"
#include "dev.h"
#include "devid.h"
#include "hal/gpio.h"
#include "osal/irq.h"
#include "osal/string.h"
#include "lib/lcd/lcd.h"
#include "lib/lcd/gui.h"
#include "keyScan.h"
#include "../lvgl.h"
#include "ui_language.h"
#include "fly_demo.h"
#include "syscfg.h"
#include "clock_app.h"
#include "hal/pwm.h"

/*======================== 外部符号 ========================*/

/* 按键输入设备 & 通用事件处理 */
extern lv_indev_t *indev_keypad;
extern void event_handler(lv_event_t * e);

/* fly_demo.c/.h 中已有定义 */
extern camera_global_t camera_gvar;
extern cam_set_t camSetParam;

/* 其它 UI 全局 */
extern lv_obj_t *curPage_obj;
extern uint8_t get_batlevel(void);
extern const lv_img_dsc_t *ui_imgset_iconBat[];

/* 页面跳转 */
extern void lv_page_select(uint8_t page);

/* 格式化 SD 卡（你自己实现，暂时可空函数占位） */
extern void format_sdcard(void);

/*======================== 本文件全局 ========================*/

/* 页面对象 */
lv_obj_t *ui_settingPage  = NULL;
lv_obj_t *ui_settTopBar   = NULL;
lv_obj_t *ui_settBatImg   = NULL;

/* 顶部标题标签 */
lv_obj_t *titleLabel       = NULL;

/* 按键 group */
lv_group_t *setting_group = NULL;

/* 一级菜单按钮 & 文本 */
static lv_obj_t *setting_buttons[SETMENU_MAX] = { NULL };
static lv_obj_t *setting_labels[SETMENU_MAX]  = { NULL };

/* 一级菜单/二级菜单容器 */
static lv_obj_t *menuContainer    = NULL;   /* 一级菜单列表容器 */
static lv_obj_t *subMenuContainer = NULL;   /* 二级菜单列表容器 */

/* 二级菜单最多 8 个选项 */
#define SUBMENU_MAX   8
static lv_obj_t *sub_buttons[SUBMENU_MAX] = { NULL };
static lv_obj_t *sub_labels[SUBMENU_MAX]  = { NULL };

/* 当前是否在二级菜单 */
static bool     in_subpage      = false;
/* 当前二级菜单对应哪个一级项（0~SETMENU_MAX-1） */
static uint8_t  cur_sub_id      = 0xFF;
/* 当前二级菜单选中索引 */
static uint8_t  sub_sel_index   = 0;
/* 当前二级菜单项数量 */
static uint8_t  sub_item_cnt    = 0;

/* 亮度级别 0~4 对应 1~5 档 */
static uint8_t  g_brightness_level = 2;

/* 样式 */
static lv_style_t settingPanelStyle;
static lv_style_t settingBtnStyle;
static lv_style_t labelStyle;

/*======================== 二级菜单文本表 ========================*/

/* 语言：中文 / English */
static const char *lang_opts[]      = { "中文", "English" };

/* 亮度：1~5 档 */
static const char *bright_opts[]    = { "1", "2", "3", "4", "5" };

/* 自动关机：关、1、3、5、10 分钟 */
static const char *autooff_opts[]   = { "关闭", "1分钟", "3分钟", "5分钟", "10分钟" };

/* 屏幕保护：关、1、3、5、10 分钟 */
static const char *scrsaver_opts[]  = { "关闭", "1分钟", "3分钟", "5分钟", "10分钟" };

/* 声音：静音、低、中、高 */
static const char *volume_opts[]    = { "静音", "低", "中", "高" };

/* 格式化：确定 / 取消 */
static const char *format_opts[]    = { "确定", "取消" };

/*======================== 辅助函数声明 ========================*/

static void update_setting_highlight(void);
static void update_sub_highlight(void);
static void create_submenu(uint8_t main_index);
static void setting_apply_sub_choice(void);

/*======================== 一级菜单高亮 ========================*/

static void update_setting_highlight(void)
{
    int i;
    for (i = 0; i < SETMENU_MAX; i++) {
        if (setting_buttons[i] == NULL) continue;

        bool is_sel = (i == camera_gvar.settingtab_index);

        /* 背景 */
        if (is_sel) {
            lv_obj_set_style_bg_color(setting_buttons[i], lv_color_hex(0x3a7bd5), 0);
            lv_obj_set_style_bg_opa(setting_buttons[i], 180, 0);
        } else {
            lv_obj_set_style_bg_opa(setting_buttons[i], LV_OPA_TRANSP, 0);
        }

        /* 文本颜色 */
        if (setting_labels[i]) {
            lv_obj_set_style_text_color(setting_labels[i],
                                        is_sel ? lv_color_hex(0xFFFFFF)
                                               : lv_color_hex(0x666666),
                                        0);
        }
    }

    /* 恢复顶部标题为"设置" */
    if (ui_settTopBar && titleLabel) {
        lv_label_set_text(titleLabel,
            (const char*)ui_language_switch[camSetParam.languageType][SETTING_STR]);
    }
}

/*======================== 二级菜单高亮 ========================*/

static void update_sub_highlight(void)
{
    int i;
    for (i = 0; i < sub_item_cnt; i++) {
        if (sub_buttons[i] == NULL) continue;

        bool is_sel = (i == sub_sel_index);

        if (is_sel) {
            lv_obj_set_style_bg_color(sub_buttons[i], lv_color_hex(0x3a7bd5), 0);
            lv_obj_set_style_bg_opa(sub_buttons[i], 180, 0);
        } else {
            lv_obj_set_style_bg_opa(sub_buttons[i], LV_OPA_TRANSP, 0);
        }

        if (sub_labels[i]) {
            lv_obj_set_style_text_color(sub_labels[i],
                                        is_sel ? lv_color_hex(0xFFFFFF)
                                               : lv_color_hex(0x666666),
                                        0);
        }
    }
}

/*======================== 创建二级菜单 ========================*/

static void create_submenu(uint8_t main_index)
{
    /* 先删除旧的二级菜单 */
    if (subMenuContainer) {
        lv_obj_del(subMenuContainer);
        subMenuContainer = NULL;
    }

    /* 子项指针清零 */
    int i;
    for (i = 0; i < SUBMENU_MAX; i++) {
        sub_buttons[i] = NULL;
        sub_labels[i]  = NULL;
    }

    /* 创建容器 */
    subMenuContainer = lv_obj_create(ui_settingPage);
    lv_obj_set_size(subMenuContainer, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(subMenuContainer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(subMenuContainer,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(subMenuContainer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(subMenuContainer, 0, 0);
    /* 精确控制各方向边距，设置位置避免与顶部状态栏重叠 */
//    lv_obj_set_pos(subMenuContainer, 0, 35);  // 设置Y坐标为35像素，避免覆盖顶部状态栏
	lv_obj_set_style_pad_top(subMenuContainer, 20, 0);     // 顶部边距：5像素
//	lv_obj_set_style_pad_bottom(subMenuContainer, 3, 0); // 底部边距：3像素
//	lv_obj_set_style_pad_left(subMenuContainer, 10, 0);  // 左边距：10像素
//	lv_obj_set_style_pad_right(subMenuContainer, 10, 0); // 右边距：10像素
//	lv_obj_set_style_pad_gap(subMenuContainer, 1, 0);    // 元素间距：1像素
    lv_obj_clear_flag(subMenuContainer, LV_OBJ_FLAG_SCROLLABLE);

    /* 更新顶部标题为对应的二级菜单名称 */
    const char *sub_title = NULL;
    switch (main_index) {
        case SETMENU_LANGUAGE:
            sub_title = (const char*)ui_language_switch[camSetParam.languageType][LANGUAGE_STR];
            break;
        case SETMENU_BRIGHTNESS:
            sub_title = "亮度";
            break;
        case SETMENU_SLEEP:
            sub_title = (const char*)ui_language_switch[camSetParam.languageType][XN_AUTOOFF_STR];
            break;
        case SETMENU_SCREEN_SAVER:
            sub_title = (const char*)ui_language_switch[camSetParam.languageType][XN_SCREENPR_STR];
            break;
        case SETMENU_SOUND:
            sub_title = (const char*)ui_language_switch[camSetParam.languageType][XN_VOLUME_STR];
            break;
        case SETMENU_FORMAT:
            sub_title = (const char*)ui_language_switch[camSetParam.languageType][FORMAT_STR];
            break;
        case SETMENU_VERSION:
            sub_title = (const char*)ui_language_switch[camSetParam.languageType][XN_VERSION_STR];
            break;
        default:
            sub_title = (const char*)ui_language_switch[camSetParam.languageType][SETTING_STR];
            break;
    }

    /* 更新顶部标题显示 */
    if (ui_settTopBar && titleLabel) {
        lv_label_set_text(titleLabel, sub_title);
    }

    const char **table = NULL;
    sub_item_cnt       = 0;
    sub_sel_index      = 0;

    switch (main_index) {
    case SETMENU_LANGUAGE:
        table        = lang_opts;
        sub_item_cnt = sizeof(lang_opts) / sizeof(lang_opts[0]);
        sub_sel_index = camSetParam.languageType; /* 0/1 */
        if (sub_sel_index >= sub_item_cnt) sub_sel_index = 0;
        break;

    case SETMENU_BRIGHTNESS:
        table        = bright_opts;
        sub_item_cnt = sizeof(bright_opts) / sizeof(bright_opts[0]);
        sub_sel_index = g_brightness_level;       /* 0~4 */
        if (sub_sel_index >= sub_item_cnt) sub_sel_index = 0;
        break;

    case SETMENU_SLEEP:
        table        = autooff_opts;
        sub_item_cnt = sizeof(autooff_opts) / sizeof(autooff_opts[0]);
        sub_sel_index = (uint8_t)camSetParam.autOffSet;  /* OFF_TIME 枚举 0~4 */
        if (sub_sel_index >= sub_item_cnt) sub_sel_index = 0;
        break;

    case SETMENU_SCREEN_SAVER:
        table        = scrsaver_opts;
        sub_item_cnt = sizeof(scrsaver_opts) / sizeof(scrsaver_opts[0]);
        sub_sel_index = (uint8_t)camSetParam.screenProtectSet; /* PRO_TIME 枚举 0~4 */
        if (sub_sel_index >= sub_item_cnt) sub_sel_index = 0;
        break;

    case SETMENU_SOUND:
        table        = volume_opts;
        sub_item_cnt = sizeof(volume_opts) / sizeof(volume_opts[0]);
        sub_sel_index = camSetParam.volumeSet*3;    /* 0~3 */
        if (sub_sel_index >= sub_item_cnt) sub_sel_index = 0;
        break;

    case SETMENU_FORMAT:
        table        = format_opts;
        sub_item_cnt = sizeof(format_opts) / sizeof(format_opts[0]);
        sub_sel_index = 1;                        /* 默认选“取消” */
        break;

    case SETMENU_VERSION:
    {
        /* 版本信息：单行文本 */
        lv_obj_t *label = lv_label_create(subMenuContainer);
        lv_label_set_text(label, "DJJ Ver: 1.0.0-2025-12-04"); /* 这里换成真实版本字符串 */
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(label, &alifangyuan16, 0);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        sub_item_cnt = 0;
        return;
    }

    default:
        break;
    }

    if (table == NULL || sub_item_cnt == 0) {
        return;
    }

    for (i = 0; i < sub_item_cnt && i < SUBMENU_MAX; i++) {
        lv_obj_t *btn = lv_obj_create(subMenuContainer);
        sub_buttons[i] = btn;

        lv_obj_set_size(btn, lv_pct(100), 30);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_pad_all(btn, 8, 0);

        lv_obj_t *label = lv_label_create(btn);
        sub_labels[i] = label;
        lv_label_set_text(label, table[i]);
        lv_obj_set_style_text_font(label, &alifangyuan16, 0);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);  // 改为居中显示

        if (i < sub_item_cnt - 1) {
            lv_obj_t *sep = lv_obj_create(subMenuContainer);
            lv_obj_set_size(sep, lv_pct(85), 1);
            lv_obj_set_style_bg_color(sep, lv_color_hex(0x404040), 0);
            lv_obj_set_style_bg_opa(sep, LV_OPA_50, 0);
            lv_obj_set_style_border_width(sep, 0, 0);
        }
    }

    update_sub_highlight();
}

/*======================== 应用二级菜单结果 ========================*/
static struct hgpwm_v0 *s_bl_pwm = NULL;
static void setting_apply_sub_choice(void)
{
    switch (cur_sub_id) {
    case SETMENU_LANGUAGE:
        camSetParam.languageType = sub_sel_index;         /* 0:中文, 1:English */
        break;

    case SETMENU_BRIGHTNESS:
        {
			g_brightness_level = sub_sel_index;               /* 1~5 */
//			os_printf("//// g_brightness_level = %d\n",g_brightness_level);
			lcd_set_brightness(g_brightness_level);
		}
        break;

    case SETMENU_SLEEP:
        camSetParam.autOffSet = (OFF_TIME)sub_sel_index;  /* 0~4 */
        break;

    case SETMENU_SCREEN_SAVER:
        camSetParam.screenProtectSet = (PRO_TIME)sub_sel_index;
        break;

    case SETMENU_SOUND:
        camSetParam.volumeSet = sub_sel_index;            /* 0~3 */
		if(camSetParam.volumeSet == 9) camSetParam.volumeSet = 10;
		volume_adjust(camSetParam.volumeSet);             //设置音量
        break;

    case SETMENU_FORMAT:
        if (sub_sel_index == 0) {
            /* 选中了“确定” */
            format_sdcard();
        }
        break;

    case SETMENU_VERSION:
        /* 只读，无操作 */
        break;

    default:
        break;
    }

    /* 如果有保存到 Flash 的函数，可以在这里调用一次，比如：
       save_cam_settings(); */
}

/*======================== 按键事件回调 ========================*/

void ui_event_settPage(lv_event_t * e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    uint32_t *key_val          = (uint32_t *)e->param;

    if (event_code != USER_KEY_EVENT || key_val == NULL) {
        return;
    }

    /*--------------- 一级菜单按键 ---------------*/
    if (!in_subpage) {
        switch (*key_val) {
        case AD_LEFT:
//        case AD_VOL_UP:
            if (camera_gvar.settingtab_index > 0)
                camera_gvar.settingtab_index--;
            else
                camera_gvar.settingtab_index = SETMENU_MAX - 1;
            update_setting_highlight();
            break;

        case AD_RIGHT:
//        case AD_VOL_DOWN:
            if (camera_gvar.settingtab_index < (SETMENU_MAX - 1))
                camera_gvar.settingtab_index++;
            else
                camera_gvar.settingtab_index = 0;
            update_setting_highlight();
            break;
		

        case AD_PRESS:
            /* 进入二级菜单 */
            in_subpage = true;
            cur_sub_id = camera_gvar.settingtab_index;

            if (menuContainer)
                lv_obj_add_flag(menuContainer, LV_OBJ_FLAG_HIDDEN);

            create_submenu(cur_sub_id);
            break;

        case AD_BACK:
//        case KEY_M:
//        case KEY_CALL:
            /* 一级菜单时返回主页 */
            lv_page_select(PAGE_HOME);
            break;

        case KEY_POWEROFF:
            lv_page_select(PAGE_POWEROFF);
            break;

        default:
            break;
        }
        return;
    }

    /*--------------- 二级菜单按键 ---------------*/

    /* 版本信息页：只有一行文字，按返回/确认都回一级 */
    if (cur_sub_id == SETMENU_VERSION) {
        switch (*key_val) {
//        case KEY_STICKER:
//        case KEY_M:
//        case KEY_CALL:
        case AD_PRESS:
            in_subpage = false;
            if (subMenuContainer) {
                lv_obj_del(subMenuContainer);
                subMenuContainer = NULL;
            }
            if (menuContainer)
                lv_obj_clear_flag(menuContainer, LV_OBJ_FLAG_HIDDEN);
            update_setting_highlight();
            break;
        default:
            break;
        }
        return;
    }

    switch (*key_val) {
    case AD_LEFT:
//    case AD_VOL_UP:
        if (sub_item_cnt > 0) {
            if (sub_sel_index > 0)
                sub_sel_index--;
            else
                sub_sel_index = sub_item_cnt - 1;
            update_sub_highlight();
        }
        break;

    case AD_RIGHT:
//    case AD_VOL_DOWN:
        if (sub_item_cnt > 0) {
            if (sub_sel_index < sub_item_cnt - 1)
                sub_sel_index++;
            else
                sub_sel_index = 0;
            update_sub_highlight();
        }
        break;

    case AD_PRESS:
        /* 应用并返回一级菜单 */
        setting_apply_sub_choice();

        in_subpage = false;
        if (subMenuContainer) {
            lv_obj_del(subMenuContainer);
            subMenuContainer = NULL;
        }
        if (menuContainer)
            lv_obj_clear_flag(menuContainer, LV_OBJ_FLAG_HIDDEN);
        update_setting_highlight();
        break;

    case AD_BACK:
//    case KEY_M:
//    case KEY_CALL:
        /* 取消，不保存，直接回一级 */
        in_subpage = false;
        if (subMenuContainer) {
            lv_obj_del(subMenuContainer);
            subMenuContainer = NULL;
        }
        if (menuContainer)
            lv_obj_clear_flag(menuContainer, LV_OBJ_FLAG_HIDDEN);
        update_setting_highlight();
        break;

    default:
        break;
    }
}

/*======================== 设置界面初始化 ========================*/

void ui_settingPage_screen_init(void)
{
    int i;

    /* 状态重置 */
    camera_gvar.settingtab_index = 0;
    in_subpage  = false;
    cur_sub_id  = 0xFF;
    sub_item_cnt = 0;

    /* group */
    setting_group = lv_group_create();
    if (!setting_group) {
        printf("Failed to create setting group\n");
        return;
    }
    lv_indev_set_group(indev_keypad, setting_group);
    group_cur = setting_group;

    /* =================================  页面初始化  ==================================*/
    /* 页面样式 */
    lv_style_reset(&settingPanelStyle);
    lv_style_init(&settingPanelStyle);
    lv_style_set_width(&settingPanelStyle, lv_obj_get_width(lv_scr_act()));
    lv_style_set_height(&settingPanelStyle, lv_obj_get_height(lv_scr_act()));
    lv_style_set_bg_opa(&settingPanelStyle, LV_OPA_TRANSP);  //bg_opa 透明度
    lv_style_set_border_width(&settingPanelStyle, 0);
    lv_style_set_radius(&settingPanelStyle, 0);
    lv_style_set_pad_all(&settingPanelStyle, 5);
    lv_style_set_pad_gap(&settingPanelStyle, 2);

    /* 按钮样式 */
    lv_style_init(&settingBtnStyle);
    lv_style_set_width(&settingBtnStyle, lv_pct(100));
    lv_style_set_height(&settingBtnStyle, 30);
    lv_style_set_bg_opa(&settingBtnStyle, LV_OPA_TRANSP);
    lv_style_set_border_width(&settingBtnStyle, 0);
    lv_style_set_radius(&settingBtnStyle, 6);
    lv_style_set_pad_all(&settingBtnStyle, 8);

    /* 文本样式（未选中灰色） */
    lv_style_init(&labelStyle);
    lv_style_set_text_color(&labelStyle, lv_color_hex(0x666666));  // 文本颜色
    lv_style_set_text_font(&labelStyle, &alifangyuan16);

    /* 根对象 */
    ui_settingPage = lv_obj_create(lv_scr_act());
    lv_obj_clear_flag(ui_settingPage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(ui_settingPage, &settingPanelStyle, 0);
    lv_obj_set_flex_flow(ui_settingPage, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_settingPage,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);

    /* 顶部栏（透明） */
    ui_settTopBar = lv_obj_create(ui_settingPage);
    lv_obj_set_size(ui_settTopBar, lv_pct(100), 30);
    lv_obj_set_style_bg_opa(ui_settTopBar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui_settTopBar, 0, 0);
    lv_obj_set_style_radius(ui_settTopBar, 0, 0);
    lv_obj_clear_flag(ui_settTopBar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_pad_left(ui_settTopBar, 0, 0);
    lv_obj_set_style_pad_right(ui_settTopBar, 0, 0);
    lv_obj_set_style_pad_top(ui_settTopBar, 0, 0);
    lv_obj_set_style_pad_bottom(ui_settTopBar, 0, 0);

    titleLabel = lv_label_create(ui_settTopBar);
    lv_label_set_text(titleLabel,
        (const char*)ui_language_switch[camSetParam.languageType][SETTING_STR]);
    lv_obj_set_style_text_color(titleLabel, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(titleLabel, &alifangyuan18, 0);
    lv_obj_set_style_text_opa(titleLabel, LV_OPA_COVER, 0);
    lv_obj_set_style_text_letter_space(titleLabel, 1, 0);
    lv_obj_align(titleLabel, LV_ALIGN_CENTER, 0, 0);

	// 设置界面不再重复添加电池图标，因为主界面添加电池图标时直接添加在屏幕根节点lv_scr_act()
//    ui_settBatImg = lv_img_create(ui_settTopBar);
//    lv_img_set_src(ui_settBatImg, ui_imgset_iconBat[get_batlevel()]);
//    lv_obj_align(ui_settBatImg, LV_ALIGN_RIGHT_MID, -5, 0);

    /* 一级菜单容器 */
    menuContainer = lv_obj_create(ui_settingPage);
    lv_obj_set_size(menuContainer, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(menuContainer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menuContainer,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(menuContainer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(menuContainer, 0, 0);
    lv_obj_set_style_pad_all(menuContainer, 3, 0);
    lv_obj_set_style_pad_gap(menuContainer, 1, 0);
    lv_obj_clear_flag(menuContainer, LV_OBJ_FLAG_SCROLLABLE);

    /* 一级菜单文本（多语言） */
    const char *setting_texts[SETMENU_MAX];
    setting_texts[SETMENU_LANGUAGE]     = (const char*)ui_language_switch[camSetParam.languageType][LANGUAGE_STR];
    setting_texts[SETMENU_BRIGHTNESS]   = "亮度";
    setting_texts[SETMENU_SLEEP]        = (const char*)ui_language_switch[camSetParam.languageType][XN_AUTOOFF_STR];
    setting_texts[SETMENU_SCREEN_SAVER] = (const char*)ui_language_switch[camSetParam.languageType][XN_SCREENPR_STR];
    setting_texts[SETMENU_SOUND]        = (const char*)ui_language_switch[camSetParam.languageType][XN_VOLUME_STR];
    setting_texts[SETMENU_FORMAT]       = (const char*)ui_language_switch[camSetParam.languageType][FORMAT_STR];
    setting_texts[SETMENU_VERSION]      = (const char*)ui_language_switch[camSetParam.languageType][XN_VERSION_STR];

    for (i = 0; i < SETMENU_MAX; i++) {
        setting_buttons[i] = NULL;
        setting_labels[i]  = NULL;
    }

    /* 创建一级菜单项 */
    for (i = 0; i < SETMENU_MAX; i++) {
        lv_obj_t *btn = lv_obj_create(menuContainer);
        setting_buttons[i] = btn;

        lv_obj_add_style(btn, &settingBtnStyle, 0);
        lv_obj_add_event_cb(btn, ui_event_settPage, LV_EVENT_ALL, NULL);

        if (setting_group)
            lv_group_add_obj(setting_group, btn);

        lv_obj_t *label = lv_label_create(btn);
        setting_labels[i] = label;
        lv_label_set_text(label, setting_texts[i]);
        lv_obj_add_style(label, &labelStyle, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 15, 0);

        if (i < SETMENU_MAX - 1) {
            lv_obj_t *sep = lv_obj_create(menuContainer);
            lv_obj_set_size(sep, lv_pct(85), 1);
            lv_obj_set_style_bg_color(sep, lv_color_hex(0x404040), 0);
            lv_obj_set_style_bg_opa(sep, LV_OPA_50, 0);
            lv_obj_set_style_border_width(sep, 0, 0);
        }
    }

    curPage_obj = ui_settingPage;
    lv_obj_add_event_cb(curPage_obj, event_handler, LV_EVENT_ALL, NULL);

    /* 初始焦点 & 高亮 */
    if (setting_group && setting_buttons[camera_gvar.settingtab_index]) {
        lv_group_focus_obj(setting_buttons[camera_gvar.settingtab_index]);
    }
    update_setting_highlight();
	
//    printf("Setting page initialization completed\n");
}
