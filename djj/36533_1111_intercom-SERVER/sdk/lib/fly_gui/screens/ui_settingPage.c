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


// 全局数组存储设置项按钮对象，便于安全访问
static lv_obj_t *setting_buttons[SETMENU_MAX] = {NULL};
static lv_obj_t *setting_labels[SETMENU_MAX]  = {NULL};   // 保存每个按钮里的 label

// 安全的高亮更新函数
static void update_setting_highlight(void) {
    printf("Updating highlight, current index: %d\n", camera_gvar.settingtab_index);

    for(int i = 0; i < SETMENU_MAX; i++) {
        if(setting_buttons[i] != NULL) {
			bool is_sel = (i == camera_gvar.settingtab_index);
            if(is_sel) {
                // 选中状态：蓝色高亮半透明背景
                lv_obj_set_style_bg_color(setting_buttons[i], lv_color_hex(0x3a7bd5), 0);
                lv_obj_set_style_bg_opa(setting_buttons[i], 180, 0);  // 半透明高亮
                //printf("Set highlight for button %d\n", i);
            } else {
                // 默认状态：完全透明
                lv_obj_set_style_bg_opa(setting_buttons[i], LV_OPA_TRANSP, 0);
            }
			// 文字颜色（这里用 setting_labels）
            if(setting_labels[i] != NULL) {
                if(is_sel) {
                    // 高亮选中：白色
                    lv_obj_set_style_text_color(setting_labels[i], lv_color_hex(0xFFFFFF), 0);
                } 
				else {
                    // 未选中：灰色
                    lv_obj_set_style_text_color(setting_labels[i], lv_color_hex(0x666666), 0);
                }
            }
        } else {
            printf("Button %d is NULL\n", i);
        }
    }
}

void ui_event_settPage(lv_event_t * e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    lv_obj_t * target = lv_event_get_target(e);
    uint32_t* key_val = (uint32_t*)e->param;

    if(event_code == USER_KEY_EVENT)
    {
        switch(*key_val)
        {
            case AD_LEFT:
            case AD_VOL_UP:
                // 向上选择设置项
                printf("AD_LEFT/VOL_UP pressed, current index: %d\n", camera_gvar.settingtab_index);
                if(camera_gvar.settingtab_index > 0)
                    --camera_gvar.settingtab_index;
                else
                    camera_gvar.settingtab_index = SETMENU_MAX - 1;
                printf("New index: %d\n", camera_gvar.settingtab_index);

                // 更新高亮显示
                update_setting_highlight();

                // 简化的焦点组操作
                if(setting_group) {
                    lv_group_focus_prev(setting_group);
                }
                break;

            case AD_RIGHT:
            case AD_VOL_DOWN:
                // 向下选择设置项
                printf("AD_RIGHT/VOL_DOWN pressed, current index: %d\n", camera_gvar.settingtab_index);
                if(camera_gvar.settingtab_index < (SETMENU_MAX - 1))
                    ++camera_gvar.settingtab_index;
                else
                    camera_gvar.settingtab_index = 0;
                printf("New index: %d\n", camera_gvar.settingtab_index);

                // 更新高亮显示
                update_setting_highlight();

                // 简化的焦点组操作
                if(setting_group) {
                    lv_group_focus_next(setting_group);
                }
                break;

            case AD_PRESS:
                // 确认选择设置项
                // 这里可以根据当前选中的设置项执行相应操作
                // 例如：进入二级设置菜单或切换设置值
                printf("Setting item selected: %d\n", camera_gvar.settingtab_index);
                break;

            case KEY_STICKER:
            case KEY_M:
            case KEY_CALL:
			
                // 返回主页
                lv_page_select(PAGE_HOME);
                break;

            case KEY_POWEROFF:
                lv_page_select(PAGE_POWEROFF);
                break;

            default:
                break;
        }

        // 暂时禁用高亮更新逻辑，专注于修复按键导航功能
        // 高亮功能将通过LVGL的焦点状态来处理
        printf("Key event processed, current setting index: %d\n", camera_gvar.settingtab_index);
    }
}

void ui_settingPage_screen_init()
{
    static lv_style_t settingPanelStyle;
    static lv_style_t settingBtnStyle;
    static lv_style_t iconImgStyle;
    static lv_style_t labelStyle;
    //printf("Starting setting page initialization\n");

	// 重置设置选项到第一个
    camera_gvar.settingtab_index = 0;
    //printf("Reset setting index to 0\n");

    setting_group = lv_group_create();
    if(!setting_group) {
        printf("Failed to create setting group\n");
        return;
    }

    lv_indev_set_group(indev_keypad, setting_group);
    group_cur = setting_group;

    /* 初始化页面样式 - 设置为透明 */
    lv_style_reset(&settingPanelStyle);
    lv_style_init(&settingPanelStyle);
    lv_style_set_width(&settingPanelStyle, lv_obj_get_width(lv_scr_act()));
    lv_style_set_height(&settingPanelStyle, lv_obj_get_height(lv_scr_act()));
    lv_style_set_bg_opa(&settingPanelStyle, LV_OPA_TRANSP);  // 透明背景
    lv_style_set_border_width(&settingPanelStyle, 0);
    lv_style_set_radius(&settingPanelStyle, 0);
    lv_style_set_pad_all(&settingPanelStyle, 5);
    lv_style_set_pad_gap(&settingPanelStyle, 2);

    /* 初始化设置项按钮样式 - 透明背景，只在高亮时显示 */
    lv_style_init(&settingBtnStyle);
    lv_style_set_width(&settingBtnStyle, lv_pct(100)); //宽度100%
    lv_style_set_height(&settingBtnStyle, 30);  // 60 → 45，减小按钮高度
    lv_style_set_bg_opa(&settingBtnStyle, LV_OPA_TRANSP);  // 默认透明
    lv_style_set_border_width(&settingBtnStyle, 0);
    lv_style_set_radius(&settingBtnStyle, 6);    // 8 → 6，减小圆角
    lv_style_set_pad_all(&settingBtnStyle, 8);    // 10 → 8，减小内边距

    /* 初始化图标样式 */
    lv_style_init(&iconImgStyle);
    lv_style_set_width(&iconImgStyle, 24);    // 32 → 24，减小图标尺寸
    lv_style_set_height(&iconImgStyle, 24);   // 32 → 24，减小图标尺寸

    /* 初始化文本样式 */
    lv_style_init(&labelStyle);
    lv_style_set_text_color(&labelStyle, lv_color_hex(0x666666));
    lv_style_set_text_font(&labelStyle, &alifangyuan16);  // 使用阿里巴巴方圆字体


    /* 创建设置页面 */
    ui_settingPage = lv_obj_create(lv_scr_act());
    lv_obj_clear_flag(ui_settingPage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(ui_settingPage, &settingPanelStyle, 0);
    lv_obj_set_flex_flow(ui_settingPage, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_settingPage, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);


    /* 创建顶部栏 - 完全透明 */
    ui_settTopBar = lv_obj_create(ui_settingPage);
    lv_obj_set_size(ui_settTopBar, lv_pct(100), 30);  // 顶部栏高度
    lv_obj_set_style_bg_opa(ui_settTopBar, LV_OPA_TRANSP, 0);  // 完全透明背景
    lv_obj_set_style_border_width(ui_settTopBar, 0, 0);
    lv_obj_set_style_radius(ui_settTopBar, 0, 0);
    lv_obj_clear_flag(ui_settTopBar, LV_OBJ_FLAG_SCROLLABLE);

    // 可选：内边距（不是必须）
    lv_obj_set_style_pad_left(ui_settTopBar, 0, 0);
    lv_obj_set_style_pad_right(ui_settTopBar, 0, 0);
    lv_obj_set_style_pad_top(ui_settTopBar, 0, 0);
    lv_obj_set_style_pad_bottom(ui_settTopBar, 0, 0);

    /* 中间：标题，真正居中 */
    lv_obj_t *titleLabel = lv_label_create(ui_settTopBar);
    lv_label_set_text(titleLabel, (const char*)ui_language_switch[camSetParam.languageType][SETTING_STR]);
    lv_obj_set_style_text_color(titleLabel, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(titleLabel, &alifangyuan18, 0);
    lv_obj_set_style_text_opa(titleLabel, LV_OPA_COVER, 0);  // 确保文字完全不透明
    // 给文字添加阴影或描边以确保在透明背景上清晰可见
    lv_obj_set_style_text_letter_space(titleLabel, 1, 0);  // 增加字间距
    lv_obj_align(titleLabel, LV_ALIGN_CENTER, 0, 0);   // 居中对齐

    /* 右侧：电量图标 */
    ui_settBatImg = lv_img_create(ui_settTopBar);
    lv_img_set_src(ui_settBatImg, ui_imgset_iconBat[get_batlevel()]);
    lv_obj_align(ui_settBatImg, LV_ALIGN_RIGHT_MID, -5, 0); // 右边留 5px

    /* 创建设置项列表容器 */
    lv_obj_t *menuContainer = lv_obj_create(ui_settingPage);
    lv_obj_set_size(menuContainer, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(menuContainer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menuContainer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(menuContainer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(menuContainer, 0, 0);
    lv_obj_set_style_pad_all(menuContainer, 3, 0);
    lv_obj_set_style_pad_gap(menuContainer, 1, 0);  // 减小行间距：5 → 2
    lv_obj_clear_flag(menuContainer, LV_OBJ_FLAG_SCROLLABLE);

    /* 创建设置项数组 - 使用多语言系统 */
    const char *setting_texts[SETMENU_MAX];

    // 根据当前语言设置对应的文本
    setting_texts[SETMENU_LANGUAGE] = (const char*)ui_language_switch[camSetParam.languageType][LANGUAGE_STR];
    setting_texts[SETMENU_BRIGHTNESS] = "亮度";  // 这个可能需要在语言文件中添加
    setting_texts[SETMENU_SLEEP] = (const char*)ui_language_switch[camSetParam.languageType][XN_AUTOOFF_STR];
    setting_texts[SETMENU_SCREEN_SAVER] = (const char*)ui_language_switch[camSetParam.languageType][XN_SCREENPR_STR];
    setting_texts[SETMENU_SOUND] = (const char*)ui_language_switch[camSetParam.languageType][XN_VOLUME_STR];
    setting_texts[SETMENU_FORMAT] = (const char*)ui_language_switch[camSetParam.languageType][FORMAT_STR];
    setting_texts[SETMENU_VERSION] = (const char*)ui_language_switch[camSetParam.languageType][XN_VERSION_STR];

    const lv_img_dsc_t *setting_icons[SETMENU_MAX] = {
        &iconMenuLanguage,    // 语言
        &iconMenuVolume,      // 亮度 (使用音量图标替代)
        &iconMenuPoff,        // 睡眠模式 (使用关机图标替代)
        &iconMenuSoff,        // 屏保模式
        &iconMenuVolume,      // 声音
        &iconMenuFormat,      // 格式化
        &iconMenuVersion      // 版本信息
    };

    /* 创建设置项按钮 */
    for(int i = 0; i < SETMENU_MAX; i++) {
        lv_obj_t *btn = lv_obj_create(menuContainer);
        if(!btn) {
            printf("Failed to create setting button %d\n", i);
            setting_buttons[i] = NULL;
            continue;
        }

        // 将按钮对象存储到全局数组
        setting_buttons[i] = btn;
        printf("Stored setting button %d at address %p\n", i, btn);

        lv_obj_add_style(btn, &settingBtnStyle, 0);
        lv_obj_add_event_cb(btn, ui_event_settPage, LV_EVENT_ALL, NULL);

        // 将按钮添加到焦点组，使按键可以正确导航
        if(setting_group) {
            lv_group_add_obj(setting_group, btn);
        }

        // 设置初始高亮状态 - 只有选中项显示高亮，其他透明
        if(i == camera_gvar.settingtab_index) {
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x3a7bd5), 0);
            lv_obj_set_style_bg_opa(btn, 180, 0);  // 半透明高亮
            printf("Set initial highlight for button %d\n", i);
        } else {
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);  // 完全透明
        }

        /* 图标 - 临时注释掉用于测试 */
        // lv_obj_t *icon = lv_img_create(btn);
        // lv_img_set_src(icon, setting_icons[i]);
        // lv_obj_add_style(icon, &iconImgStyle, 0);
        // lv_obj_align(icon, LV_ALIGN_LEFT_MID, 15, 0);

        /* 文本标签 - 调整位置适应无图标的情况 */
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, setting_texts[i]);

		// 保存 label 指针，后面用于改颜色
		setting_labels[i] = label;
		
//		/* 普通状态样式（未选中） */
        lv_obj_add_style(label, &labelStyle, 0);
		lv_style_set_text_font(&labelStyle, &alifangyuan16);// 这里不设置 text_color，颜色我们用 set_style_text_color 动态改
		
		// 初始颜色：未选中为灰色
		lv_obj_set_style_text_color(label, lv_color_hex(0x666666), 0);

        lv_obj_align(label, LV_ALIGN_LEFT_MID, 15, 0);  // 调整到左对齐，适应无图标的情况

        /* 添加分隔线（除了最后一项） */
        if(i < SETMENU_MAX - 1) {
            lv_obj_t *separator = lv_obj_create(menuContainer);
            lv_obj_set_size(separator, lv_pct(85), 1);
            lv_obj_set_style_bg_color(separator, lv_color_hex(0x404040), 0);
            lv_obj_set_style_bg_opa(separator, LV_OPA_50, 0);
            lv_obj_set_style_border_width(separator, 0, 0);
        }
    }
	
	curPage_obj = ui_settingPage;
    /* 添加事件回调 */
    lv_obj_add_event_cb(curPage_obj, event_handler, LV_EVENT_ALL, NULL);

    // 设置初始焦点到当前选中的按钮
    if(setting_group && setting_buttons[camera_gvar.settingtab_index] != NULL) {
        lv_group_focus_obj(setting_buttons[camera_gvar.settingtab_index]);
		update_setting_highlight();
        //printf("Set initial focus to button %d\n", camera_gvar.settingtab_index);
    }

    printf("Setting page initialization completed, buttons created: %d\n", SETMENU_MAX);
}