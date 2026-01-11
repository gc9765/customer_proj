#include "sys_config.h"
#include "tx_platform.h"
#include "list.h"
#include "dev.h"
#include "typesdef.h"
#include "devid.h"
#include "osal/irq.h"
#include "osal/string.h"
#include "dev/scale/hgscale.h"
#include "osal/semaphore.h"
#include "openDML.h"
#include "osal/mutex.h"
#include "custom_mem/custom_mem.h"
#include "lib/lcd/lcd.h"
#include "keyScan.h"
#include "../lvgl.h"
#include "ui_language.h"
#include "fly_demo.h"

#include "lwip/netif.h"
#include "hal/netdev.h"

lv_obj_t * ui_homeWifiDot = NULL;

typedef enum {
    ALBUM_MODE_NORMAL = 0,
    ALBUM_MODE_WECHAT_PICK,     // 微聊挑图发送
    ALBUM_MODE_WECHAT_PREVIEW,  // 微聊聊天消息预览（只看不发）
} album_mode_t;

extern uint8_t g_camera_from_page;
extern void album_enter_from(uint8_t from_page, album_mode_t mode);
extern uint8_t s_cam_shot_return_pending;
void home_wifi_dot_update(void);
void pair_popup_hide(void);

extern char pair_str[100];

void ui_event_homePage(lv_event_t * e){

    lv_event_code_t event_code = lv_event_get_code(e);
    lv_obj_t * target = lv_event_get_target(e);
 	uint32_t* key_val = (uint32_t*)e->param;

	if(event_code==USER_KEY_EVENT)
    {
		switch(*key_val)
		{
			case AD_LEFT:         
				if(camera_gvar.pagebtn_index>0)
					--camera_gvar.pagebtn_index;
				else
					camera_gvar.pagebtn_index =3;
				//printf("## up camera_gvar.pagebtn_index=%d \n",camera_gvar.pagebtn_index);
				camera_gvar.immediately_reflash_flag=1;
				break;
			
			case AD_RIGHT:
				if(camera_gvar.pagebtn_index<3)
					++camera_gvar.pagebtn_index;
				else
				   camera_gvar.pagebtn_index =0;

				camera_gvar.immediately_reflash_flag=1;
				//printf("##down  camera_gvar.pagebtn_index=%d \n",camera_gvar.pagebtn_index);
				break;
			

			case AD_PRESS:
	//		    if(page_index==0)
				if (camera_gvar.pagebtn_index == 1) {   // 1 改成你主页“相机图标”的 index
					g_camera_from_page = PAGE_HOME;
					s_cam_shot_return_pending = 0;      //防止拍完回微信
				}

				lv_page_select(camera_gvar.pagebtn_index+1);
				/* 你的 index=2 是相册按钮（row2 第一个），所以 page==3 一般就是 PAGE_ALBUM */
				if (camera_gvar.pagebtn_index == 2) {
					album_enter_from(PAGE_HOME, ALBUM_MODE_NORMAL);   // 重置为主页浏览模式
				}
	//            lv_page_select(PAGE_INTERCOM + camera_gvar.pagebtn_index);
				break;

			case KEY_POWEROFF:
				lv_page_select(PAGE_POWEROFF);
				break;

			default:
				break;
		}
	}

	
}
static void home_battery_create(void)
{
    if (ui_homeBatImg && lv_obj_is_valid(ui_homeBatImg)) return;

    ui_homeBatImg = lv_img_create(lv_layer_top());
    lv_img_set_src(ui_homeBatImg, ui_imgset_iconBat[get_batlevel()]);
    lv_obj_set_align(ui_homeBatImg, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_pos(ui_homeBatImg, -5, 5);

    lv_obj_clear_flag(ui_homeBatImg, LV_OBJ_FLAG_CLICKABLE);
}

static void home_overlays_show(void)
{
    // HOME 下显示：电池 / wifi 点
    home_battery_create();
    if (ui_homeBatImg && lv_obj_is_valid(ui_homeBatImg))
        lv_obj_clear_flag(ui_homeBatImg, LV_OBJ_FLAG_HIDDEN);

    home_wifi_dot_update(); // 内部会控制是否 hidden


}

void home_overlays_hide(void)
{
    if (ui_homeBatImg && lv_obj_is_valid(ui_homeBatImg))
        lv_obj_add_flag(ui_homeBatImg, LV_OBJ_FLAG_HIDDEN);

    if (ui_homeWifiDot && lv_obj_is_valid(ui_homeWifiDot))
        lv_obj_add_flag(ui_homeWifiDot, LV_OBJ_FLAG_HIDDEN);

    pair_popup_hide();
}


void pair_popup_create(void)
{
    // 防止重复创建
    if (ui_pairPanel && lv_obj_is_valid(ui_pairPanel)) {
        return;
    }

    // 直接挂到 top layer（确保永远在最上面），不做全屏遮罩
    ui_pairPanel = lv_obj_create(lv_layer_top());
    lv_obj_clear_flag(ui_pairPanel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ui_pairPanel, 220, 120);
    lv_obj_center(ui_pairPanel);

    // 弹窗样式
    lv_obj_set_style_bg_color(ui_pairPanel, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(ui_pairPanel, 240, 0);
    lv_obj_set_style_radius(ui_pairPanel, 12, 0);
    lv_obj_set_style_border_width(ui_pairPanel, 2, 0);
    lv_obj_set_style_border_color(ui_pairPanel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_all(ui_pairPanel, 12, 0);

    // ⭐ 不要让它吃掉按键/点击（关键）
    lv_obj_clear_flag(ui_pairPanel, LV_OBJ_FLAG_CLICKABLE);

    // 文本
    ui_pairTeimlabel = lv_label_create(ui_pairPanel);
    lv_obj_set_style_text_font(ui_pairTeimlabel, &alifangyuan28, 0);
    lv_obj_set_style_text_color(ui_pairTeimlabel, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_long_mode(ui_pairTeimlabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui_pairTeimlabel, 220 - 24);
    lv_obj_center(ui_pairTeimlabel);

    // 初始隐藏
    lv_obj_add_flag(ui_pairPanel, LV_OBJ_FLAG_HIDDEN);
}

void pair_popup_show(const char *text)
{
	if (camera_gvar.page_cur != PAGE_HOME) return;
    pair_popup_create();
    if (!ui_pairPanel || !lv_obj_is_valid(ui_pairPanel)) return;

    lv_label_set_text(ui_pairTeimlabel, text ? text : "");
    lv_obj_clear_flag(ui_pairPanel, LV_OBJ_FLAG_HIDDEN);

    // 保证层级在最前
    lv_obj_move_foreground(ui_pairPanel);

    // 如果你还要 WiFi 点永远在最前（可选）
    if (ui_homeWifiDot && lv_obj_is_valid(ui_homeWifiDot)) {
        lv_obj_move_foreground(ui_homeWifiDot);
    }
}

void pair_popup_hide(void)
{
    if (ui_pairPanel && lv_obj_is_valid(ui_pairPanel)) {
        lv_obj_add_flag(ui_pairPanel, LV_OBJ_FLAG_HIDDEN);
    }
}


void home_wifi_dot_create(void)
{
    if (ui_homeWifiDot && lv_obj_is_valid(ui_homeWifiDot)) return;

    /* 放到屏幕上，确保是“真正左上角”，不受 flex 布局影响 */
    ui_homeWifiDot = lv_obj_create(lv_layer_top());
    lv_obj_clear_flag(ui_homeWifiDot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ui_homeWifiDot, 8, 8);
    lv_obj_set_align(ui_homeWifiDot, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(ui_homeWifiDot, 6, 6);   // 你觉得太贴边就调大点

    lv_obj_set_style_radius(ui_homeWifiDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ui_homeWifiDot, 1, 0);
    lv_obj_set_style_border_color(ui_homeWifiDot, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(ui_homeWifiDot, LV_OPA_COVER, 0);

    lv_obj_move_foreground(ui_homeWifiDot);
}

//void home_wifi_dot_update(void)
//{
//	home_wifi_dot_create();
//    if (!ui_homeWifiDot || !lv_obj_is_valid(ui_homeWifiDot)) return;
//
//    /* 只在 HOME 显示，其它页面隐藏避免叠层 */
//    if (camera_gvar.page_cur != PAGE_HOME) {
//        lv_obj_add_flag(ui_homeWifiDot, LV_OBJ_FLAG_HIDDEN);
//        return;
//    }
//    lv_obj_clear_flag(ui_homeWifiDot, LV_OBJ_FLAG_HIDDEN);
//
//    /* 连接成功绿色，否则灰色 */
//    if (get_wifi_connect_flag()) {
//        lv_obj_set_style_bg_color(ui_homeWifiDot, lv_color_hex(0x00FF00), 0);
//    } else {
//        lv_obj_set_style_bg_color(ui_homeWifiDot, lv_color_hex(0x6D6C6C), 0);
//    }
//}

void home_wifi_dot_update(void)
{
    // 静态变量记录上一次的状态 (0:未连, 1:已连, 0xFF:初始化)
    static uint8_t last_wifi_status = 0xFF; 
    uint8_t current_status;

    // 1. 如果对象不存在，尝试创建（防止空指针）
    home_wifi_dot_create();
    if (!ui_homeWifiDot || !lv_obj_is_valid(ui_homeWifiDot)) return;

    // 2. 如果不在主页，隐藏并重置状态
    if (camera_gvar.page_cur != PAGE_HOME) {
        if (!lv_obj_has_flag(ui_homeWifiDot, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(ui_homeWifiDot, LV_OBJ_FLAG_HIDDEN);
        }
        last_wifi_status = 0xFF; // 离开页面后重置，确保下次回来能刷新
        return;
    }
    
    // 3. 确保显示
    if (lv_obj_has_flag(ui_homeWifiDot, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(ui_homeWifiDot, LV_OBJ_FLAG_HIDDEN);
    }

    // 4. 获取当前底层 WiFi 状态 (1=已连, 0=未连)
    current_status = get_wifi_connect_flag() ? 1 : 0;
	os_printf("DEBUG: UI check wifi status: %d (last: %d)\r\n", current_status, last_wifi_status);
    // 5. ⭐ 核心优化：只有状态发生变化时，才去调用 LVGL 的绘图函数
    if (current_status != last_wifi_status) 
    {
        last_wifi_status = current_status; // 更新历史记录

        if (current_status) {
            // 变绿
            lv_obj_set_style_bg_color(ui_homeWifiDot, lv_color_hex(0x00FF00), 0);
        } else {
            // 变灰
            lv_obj_set_style_bg_color(ui_homeWifiDot, lv_color_hex(0x6D6C6C), 0);
        }
        printf("## UI: WiFi status changed to %d\n", current_status);
    }
}
extern struct sys_config sys_cfgs; // 确保能访问全局配置
void ui_homePage_screen_init(){	
	static lv_style_t menuPanelStyle;
	static lv_style_t pageBtnStyle;
	static lv_style_t btnImgStyle;
	static lv_style_t btnTextStyle;
	if (home_group) {               // 防止重复创建泄漏
		lv_indev_set_group(indev_keypad, NULL);
		lv_group_del(home_group);
		home_group = NULL;
	}


	home_group = lv_group_create();
	lv_indev_set_group(indev_keypad, home_group);
	group_cur = home_group;

/****init  page  style***/
	lv_style_reset(&menuPanelStyle);
	lv_style_init(&menuPanelStyle);
	lv_style_set_width(&menuPanelStyle, lv_obj_get_width(lv_scr_act()));
	lv_style_set_height(&menuPanelStyle, lv_obj_get_height(lv_scr_act()));
	lv_style_set_bg_color(&menuPanelStyle, lv_color_hex(0x000000));	//0x101018

	// lv_style_set_bg_opa(&menuPanelStyle, 255);	
	lv_style_set_shadow_color(&menuPanelStyle, lv_color_make(0x00, 0x00, 0x00));
	lv_style_set_border_color(&menuPanelStyle, lv_color_make(0x00, 0x00, 0x00));
	lv_style_set_outline_color(&menuPanelStyle, lv_color_make(0x00, 0x00, 0x00));
	lv_style_set_border_width(&menuPanelStyle, 0);
	lv_style_set_radius(&menuPanelStyle,0);
	lv_style_set_pad_all(&menuPanelStyle, 0);
	lv_style_set_pad_gap(&menuPanelStyle,0);

/****init  page_btn  style***/  // 修改按钮样式
	lv_style_init(&pageBtnStyle);
	lv_style_set_width(&pageBtnStyle, 65);  //60-->80 增加宽度以显示完整英文单词
	lv_style_set_height(&pageBtnStyle, 90);  //110->90
	 lv_style_set_bg_color(&pageBtnStyle, lv_color_hex(0x101018));	//0x101018
	lv_style_set_bg_opa(&pageBtnStyle, 0);	
	lv_style_set_shadow_color(&pageBtnStyle, lv_color_make(0x00, 0x00, 0x00));
	lv_style_set_border_color(&pageBtnStyle, lv_color_make(0x00, 0x00, 0x00));
	lv_style_set_outline_color(&pageBtnStyle, lv_color_make(0x00, 0x00, 0x00));
	//lv_style_set_outline_opa(&pageBtnStyle, 0);	
	lv_style_set_outline_width(&pageBtnStyle, 0);	
	lv_style_set_border_width(&pageBtnStyle, 0);
	lv_style_set_pad_all(&pageBtnStyle, 0);
	lv_style_set_pad_gap(&pageBtnStyle,0);

//	lv_style_set_radius(&pageBtnStyle, 0);  // 为按钮容器设置圆角半径

/****init  btn_img  style***/  //图标样式
	lv_style_init(&btnImgStyle);
	lv_style_set_width(&btnImgStyle, 65);    // 减小图标宽度
	lv_style_set_height(&btnImgStyle, 65);   // 减小图标高度
	 lv_style_set_bg_color(&btnImgStyle, lv_color_hex(0x000000));	//0x101018
	lv_style_set_bg_opa(&btnImgStyle, 0);
	lv_style_set_shadow_color(&btnImgStyle, lv_color_make(0x00, 0x00, 0x00));
	lv_style_set_border_color(&btnImgStyle, lv_color_make(0x00, 0x00, 0x00));
	lv_style_set_outline_color(&btnImgStyle, lv_color_make(0x00, 0x00, 0x00));
	lv_style_set_outline_width(&btnImgStyle, 0);
	lv_style_set_border_width(&btnImgStyle, 0);
	lv_style_set_radius(&btnImgStyle, 0);  // 设置主界面图标圆角半径为12像素
	//lv_style_set_pad_all(&btnImgStyle, 0);
//	 lv_obj_set_style_pad_bottom(ui_intercomImg, 10, 0);  // 图标底部增加10像素间距
	
	/****init  btn_text  style***/
	lv_style_init(&btnTextStyle);
	lv_style_set_text_color(&btnTextStyle,lv_color_hex(0x808080));
	lv_style_set_text_letter_space(&btnTextStyle, 0);

	// 启用自动换行和居中对齐
	lv_style_set_text_decor(&btnTextStyle, LV_TEXT_DECOR_NONE);  // 无装饰
	lv_style_set_text_align(&btnTextStyle, LV_TEXT_ALIGN_CENTER);  // 居中对齐
	if (camSetParam.languageType == English) {
		lv_style_set_text_line_space(&btnTextStyle, -4); // 设置行间距
	}else{
		lv_style_set_text_line_space(&btnTextStyle, 0);
	}
	
/*-----------home page---------------*/

    // 控件部分
	ui_homePage = lv_obj_create(lv_scr_act());
	lv_obj_clear_flag( ui_homePage, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
	// lv_obj_set_flex_flow(ui_homePage,LV_FLEX_FLOW_COLUMN);
	// lv_obj_set_flex_align(ui_homePage, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_EVENLY);
	lv_obj_add_style(ui_homePage, &menuPanelStyle, 0);
	lv_obj_set_style_text_font(ui_homePage, &alifangyuan16, 0);
	lv_obj_set_flex_flow(ui_homePage,LV_FLEX_FLOW_COLUMN);      //
	lv_obj_set_flex_align(ui_homePage, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_top(ui_homePage, 46, 0);  // 顶部间距
//	lv_obj_set_style_bg_img_src(ui_homePage, &intercom_bglogo, 0);
	lv_obj_set_style_pad_row(ui_homePage, 16, 0);      // 行间距60像素
    lv_obj_set_style_pad_bottom(ui_homePage, 0, 0);   // 底部边距

	
	
    // 创建第一行容器 (对讲 + 相机)
    lv_obj_t * row1 = lv_obj_create(ui_homePage);
    lv_obj_set_size(row1, LV_PCT(100), LV_SIZE_CONTENT);  // 宽度100%，高度自适应
    lv_obj_clear_flag(row1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_left(row1, 30, 0);  // 减少左边距从35到15
	lv_obj_set_style_pad_column(row1, 50, 0);  // 减少列间距从50到30
	// 添加透明背景设置
    lv_obj_set_style_bg_opa(row1, LV_OPA_TRANSP, 0);  // 设置背景透明
    lv_obj_set_style_border_width(row1, 0, 0);         // 移除边框
    lv_obj_set_style_outline_width(row1, 0, 0);       // 移除轮廓

	
    // 创建第二行容器 (相册 + 设置)
    lv_obj_t * row2 = lv_obj_create(ui_homePage);
    lv_obj_set_size(row2, LV_PCT(100), LV_SIZE_CONTENT);  // 宽度100%，高度自适应
    lv_obj_clear_flag(row2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row2, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row2, 30, 0);  // 减少左边距从35到15
	lv_obj_set_style_pad_column(row2, 50, 0);  // 减少列间距从50到30
	lv_obj_set_style_bg_opa(row2, LV_OPA_TRANSP, 0);  // 设置背景透明
    lv_obj_set_style_border_width(row2, 0, 0);         // 移除边框
    lv_obj_set_style_outline_width(row2, 0, 0);       // 移除轮廓

	
    curPage_obj = ui_homePage;
    lv_obj_add_event_cb(curPage_obj, event_handler, LV_EVENT_ALL, NULL);

/**ui_intercomBtn **/
	lv_obj_t * ui_intercomBtn = lv_obj_create(row1);
	lv_obj_clear_flag( ui_intercomBtn, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
	lv_obj_set_flex_flow(ui_intercomBtn,LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(ui_intercomBtn, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_style(ui_intercomBtn, &pageBtnStyle, 0);

	lv_obj_t *ui_intercomImg = lv_obj_create(ui_intercomBtn);
	lv_obj_add_style(ui_intercomImg, &btnImgStyle, 0);
	lv_obj_set_style_radius(ui_intercomImg, 12, 0);  // 设置圆角
	lv_obj_set_style_clip_corner(ui_intercomImg, 1, 0);  // 裁剪圆角外的内容
	// 创建真正的图片控件放进容器，并保存到全局数组中
	lv_obj_t * icon = lv_img_create(ui_intercomImg);
	lv_img_set_src(icon, ui_imgset_iconHomeintercom[0]);
	lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
	lv_obj_center(icon);
	// 保存图片控件指针，用于后续切换
	intercomPage_icon = icon;

	lv_obj_t *ui_intercomText = lv_label_create(ui_intercomBtn);
	lv_obj_set_size(ui_intercomText, LV_PCT(100), LV_SIZE_CONTENT);  // 宽度100%（相对于按钮），高度自适应
	if (camSetParam.languageType == English) {
		lv_obj_set_pos(ui_intercomText, 0, 65);  // 设置文本位置：从顶部35像素开始（在图标下方）
	}else{
		lv_obj_set_pos(ui_intercomText, 0, 35);
	}
	lv_obj_add_style(ui_intercomText, &btnTextStyle, 0);
	lv_label_set_long_mode(ui_intercomText, LV_LABEL_LONG_WRAP);  // 启用自动换行
	//lv_label_set_text(ui_intercomText,"Camera");
	lv_label_set_text(ui_intercomText, (const char *)ui_language_switch[camSetParam.languageType][INTERCOM_STR]);

	intercomPage_btn = ui_intercomImg;		
	lv_obj_add_event_cb(intercomPage_btn, event_handler, LV_EVENT_ALL, NULL);
	lv_group_add_obj(home_group, intercomPage_btn);
	user_pagebtn_list[0].pagebtn=intercomPage_btn;
	user_pagebtn_list[0].nimg=ui_imgset_iconHomeintercom[0];
	user_pagebtn_list[0].bimg=ui_imgset_iconHomeintercom[1];


/**ui_cameraBtn **/
	lv_obj_t * ui_cameraBtn = lv_obj_create(row1);
	lv_obj_clear_flag( ui_cameraBtn, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
	lv_obj_set_flex_flow(ui_cameraBtn,LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(ui_cameraBtn, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_style(ui_cameraBtn, &pageBtnStyle, 0);

	lv_obj_t *ui_cameraImg = lv_obj_create(ui_cameraBtn);
	lv_obj_add_style(ui_cameraImg, &btnImgStyle, 0);
	lv_obj_set_style_radius(ui_cameraImg, 12, 0);  // 设置圆角
	lv_obj_set_style_clip_corner(ui_cameraImg, 1, 0);  // 裁剪圆角外的内容
	// 创建真正的图片控件放进容器，并保存到全局数组中
	lv_obj_t * icon2 = lv_img_create(ui_cameraImg);
	lv_img_set_src(icon2, ui_imgset_iconHomeCamera[0]);
	lv_obj_set_style_bg_opa(icon2, LV_OPA_TRANSP, 0);
	lv_obj_center(icon2);
	// 保存图片控件指针，用于后续切换
	cameraPage_icon = icon2;

	lv_obj_t *ui_cameraText = lv_label_create(ui_cameraBtn);
	lv_obj_set_size(ui_cameraText, LV_PCT(100), LV_SIZE_CONTENT);  // 宽度100%（相对于按钮），高度自适应
	if (camSetParam.languageType == English) {
		lv_obj_set_pos(ui_cameraText, 0, 65);  // 设置文本位置：从顶部35像素开始（在图标下方）
	}else{
		lv_obj_set_pos(ui_cameraText, 0, 35);
	}
	lv_obj_add_style(ui_cameraText, &btnTextStyle, 0);
	lv_label_set_long_mode(ui_cameraText, LV_LABEL_LONG_WRAP);  // 启用自动换行
	//lv_label_set_text(ui_cameraText,"Camera");
	lv_label_set_text(ui_cameraText, (const char *)ui_language_switch[camSetParam.languageType][TAKEPHOTO_STR]);

	cameraPage_btn = ui_cameraImg;		
	lv_obj_add_event_cb(cameraPage_btn, event_handler, LV_EVENT_ALL, NULL);
	lv_group_add_obj(home_group, cameraPage_btn);
	user_pagebtn_list[1].pagebtn=cameraPage_btn;
	user_pagebtn_list[1].nimg=ui_imgset_iconHomeCamera[0];
	user_pagebtn_list[1].bimg=ui_imgset_iconHomeCamera[1];

	/**ui_albumBtn **/

	lv_obj_t * ui_albumBtn = lv_obj_create(row2);
	lv_obj_clear_flag( ui_albumBtn, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
	lv_obj_set_flex_flow(ui_albumBtn,LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(ui_albumBtn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_style(ui_albumBtn, &pageBtnStyle, 0);

	lv_obj_t *ui_albumImg = lv_obj_create(ui_albumBtn);
	lv_obj_add_style(ui_albumImg, &btnImgStyle, 0);
	lv_obj_set_style_radius(ui_albumImg, 12, 0);  // 设置圆角
	lv_obj_set_style_clip_corner(ui_albumImg, 1, 0);  // 裁剪圆角外的内容
	// 创建真正的图片控件放进容器，并保存到全局数组中
	lv_obj_t * icon3 = lv_img_create(ui_albumImg);
	lv_img_set_src(icon3, ui_imgset_iconHomePlayer[0]);
	lv_obj_set_style_bg_opa(icon3, LV_OPA_TRANSP, 0);
	lv_obj_center(icon3);
	// 保存图片控件指针，用于后续切换
	albumPage_icon = icon3;

	lv_obj_t *ui_albumText = lv_label_create(ui_albumBtn);
	lv_obj_set_size(ui_albumText, LV_PCT(100), LV_SIZE_CONTENT);  // 宽度100%（相对于按钮），高度自适应
	lv_obj_set_pos(ui_albumText, 0, 35);  // 设置文本位置：从顶部35像素开始（在图标下方）
	lv_obj_add_style(ui_albumText, &btnTextStyle, 0);
	lv_label_set_long_mode(ui_albumText, LV_LABEL_LONG_WRAP);  // 启用自动换行
	lv_label_set_text(ui_albumText, (const char *)ui_language_switch[camSetParam.languageType][PHOTO_STR]);

	albumPage_btn = ui_albumImg;	
	lv_obj_add_event_cb(albumPage_btn, event_handler, LV_EVENT_ALL, NULL);
	lv_group_add_obj(home_group, albumPage_btn);
	user_pagebtn_list[2].pagebtn=albumPage_btn;
	user_pagebtn_list[2].nimg=ui_imgset_iconHomePlayer[0];
	user_pagebtn_list[2].bimg=ui_imgset_iconHomePlayer[1];
	

	/**ui_settBtn - 设置图标按钮**/
    lv_obj_t * ui_settBtn = lv_obj_create(row2);
    lv_obj_clear_flag( ui_settBtn, LV_OBJ_FLAG_SCROLLABLE );    ///Flags
    lv_obj_set_flex_flow(ui_settBtn,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_settBtn, LV_FLEX_ALIGN_SPACE_BETWEEN,LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_style(ui_settBtn, &pageBtnStyle, 0);
    
    lv_obj_t *ui_settImg = lv_obj_create(ui_settBtn);
    lv_obj_add_style(ui_settImg, &btnImgStyle, 0);
    lv_obj_set_style_radius(ui_settImg, 12, 0);  // 设置圆角
    lv_obj_set_style_clip_corner(ui_settImg, 1, 0);  // 裁剪圆角外的内容
    // 创建真正的图片控件放进容器，并保存到全局数组中
    lv_obj_t * icon4 = lv_img_create(ui_settImg);
    lv_img_set_src(icon4, ui_imgset_iconHomeMenu[0]);
	lv_obj_set_style_bg_opa(icon4, LV_OPA_TRANSP, 0);
    lv_obj_center(icon4);
    // 保存图片控件指针，用于后续切换
    settPage_icon = icon4;
    
    lv_obj_t *ui_settText = lv_label_create(ui_settBtn);
    lv_obj_set_size(ui_settText, LV_PCT(100), LV_SIZE_CONTENT);  // 宽度100%（相对于按钮），高度自适应
    lv_obj_set_pos(ui_settText, 0, 35);  // 设置文本位置：从顶部35像素开始（在图标下方）
    lv_obj_add_style(ui_settText, &btnTextStyle, 0);
    lv_label_set_long_mode(ui_settText, LV_LABEL_LONG_WRAP);  // 启用自动换行
    lv_label_set_text(ui_settText, (const char*)ui_language_switch[camSetParam.languageType][SETTING_STR]); //使用多语言
    
    settPage_btn = ui_settImg;    // 连接到全局变量
    lv_obj_add_event_cb(settPage_btn, event_handler, LV_EVENT_ALL,NULL);
    lv_group_add_obj(home_group, settPage_btn);
    
    // 添加到主界面图标数组 (第4个图标，index=3)
    user_pagebtn_list[3].pagebtn=settPage_btn;
    user_pagebtn_list[3].nimg=ui_imgset_iconHomeMenu[0];  //普通状态
    user_pagebtn_list[3].bimg=ui_imgset_iconHomeMenu[1];  //高亮状态
	
	// 顶部状态栏部分
	// 电池图标直接挂到 screen 上
//    ui_homeBatImg = lv_img_create(lv_scr_act());
//    lv_img_set_src(ui_homeBatImg, ui_imgset_iconBat[get_batlevel()]);
//    lv_obj_set_width(ui_homeBatImg, LV_SIZE_CONTENT);
//    lv_obj_set_height(ui_homeBatImg, LV_SIZE_CONTENT);
//    lv_obj_set_align(ui_homeBatImg, LV_ALIGN_TOP_RIGHT);  // 真正的屏幕右上角
//    lv_obj_set_pos(ui_homeBatImg, -5, 5); // 根据屏幕分辨率微调一点点
//    lv_obj_move_foreground(ui_homeBatImg);

	camera_gvar.immediately_reflash_flag=1;
	
	home_overlays_show();//电池和WiFi
	pair_popup_create();

	if (camera_gvar.pair_out_times && !camera_gvar.pair_success) {
		sprintf(pair_str, "配对 时间:%02d", camera_gvar.pair_out_times);
		pair_popup_show(pair_str);
	}
	home_start_pair_once();

}