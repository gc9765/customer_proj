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




void ui_event_homePage(lv_event_t * e){

    lv_event_code_t event_code = lv_event_get_code(e);
    lv_obj_t * target = lv_event_get_target(e);
 	uint32_t* key_val = (uint32_t*)e->param;

	if(event_code==USER_KEY_EVENT)
    {
		switch(*key_val)
		{
			case AD_LEFT: 
			case AD_UP:
            printf("AD_UP \n");            
            if(camera_gvar.pagebtn_index>0)
                --camera_gvar.pagebtn_index;
            else
                camera_gvar.pagebtn_index =2;
			printf("## up camera_gvar.pagebtn_index=%d \n",camera_gvar.pagebtn_index);
			camera_gvar.immediately_reflash_flag=1;
			break;
			
			case AD_RIGHT:
			case AD_DOWN:
            printf("AD_DOWN \n");
             if(camera_gvar.pagebtn_index<2)
                ++camera_gvar.pagebtn_index;
            else
               camera_gvar.pagebtn_index =0;

			camera_gvar.immediately_reflash_flag=1;
			printf("##down  camera_gvar.pagebtn_index=%d \n",camera_gvar.pagebtn_index);
			break;


			case AD_PRESS:
           // printf("AD_PRESS \n");
            //if(page_index==0)
			lv_page_select(camera_gvar.pagebtn_index+1);
            break;

			case KEY_POWEROFF:
			lv_page_select(PAGE_POWEROFF);
			break;

			default:
			break;
		}
	}

	
}

//intercom_bglogo

void ui_homePage_screen_init(){	
	static lv_style_t menuPanelStyle;
	static lv_style_t pageBtnStyle;
	static lv_style_t btnImgStyle;
	static lv_style_t btnTextStyle;

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

/****init  page_btn  style***/
	lv_style_init(&pageBtnStyle);
	lv_style_set_width(&pageBtnStyle, 92);
	lv_style_set_height(&pageBtnStyle, 110);
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

	// lv_style_set_radius(&pageBtnStyle,80);

/****init  btn_img  style***/
	lv_style_init(&btnImgStyle);
	lv_style_set_width(&btnImgStyle, 92);
	lv_style_set_height(&btnImgStyle, 96);
	 lv_style_set_bg_color(&btnImgStyle, lv_color_hex(0x000000));	//0x101018
	//lv_style_set_bg_opa(&btnImgStyle, 0);	
	lv_style_set_shadow_color(&btnImgStyle, lv_color_make(0x00, 0x00, 0x00));
	lv_style_set_border_color(&btnImgStyle, lv_color_make(0x00, 0x00, 0x00));
	lv_style_set_outline_color(&btnImgStyle, lv_color_make(0x00, 0x00, 0x00));
	lv_style_set_outline_width(&btnImgStyle, 0);	
	lv_style_set_border_width(&btnImgStyle, 0);
	//lv_style_set_pad_all(&btnImgStyle, 0);

/****init  btn_text  style***/
	lv_style_init(&btnTextStyle);
	// lv_style_set_width(&btnTextStyle, LV_SIZE_CONTENT);
	// lv_style_set_height(&btnTextStyle, LV_SIZE_CONTENT);
	lv_style_set_text_color(&btnTextStyle,lv_color_hex(0x808080));
	
	
/*-----------home page---------------*/
	ui_homePage = lv_obj_create(lv_scr_act());

	lv_obj_clear_flag( ui_homePage, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
	// lv_obj_set_flex_flow(ui_homePage,LV_FLEX_FLOW_COLUMN);
	// lv_obj_set_flex_align(ui_homePage, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_EVENLY);
	lv_obj_add_style(ui_homePage, &menuPanelStyle, 0);
	lv_obj_set_style_text_font(ui_homePage, &alifangyuan16, 0);
	lv_obj_set_flex_flow(ui_homePage,LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(ui_homePage, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	// lv_obj_set_style_bg_img_src(ui_homePage, &intercom_bglogo, 0);
	curPage_obj = ui_homePage;
	lv_obj_add_event_cb(curPage_obj, event_handler, LV_EVENT_ALL, NULL);

/**ui_intercomBtn **/
	lv_obj_t * ui_intercomBtn = lv_obj_create(ui_homePage);
	lv_obj_clear_flag( ui_intercomBtn, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
	lv_obj_set_flex_flow(ui_intercomBtn,LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(ui_intercomBtn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_style(ui_intercomBtn, &pageBtnStyle, 0);

	lv_obj_t *ui_intercomImg = lv_obj_create(ui_intercomBtn);
	lv_obj_add_style(ui_intercomImg, &btnImgStyle, 0);
	lv_obj_set_style_bg_img_src( ui_intercomImg, ui_imgset_iconHomeintercom[0], LV_PART_MAIN | LV_STATE_DEFAULT );

	lv_obj_t *ui_intercomText = lv_label_create(ui_intercomBtn);
	// lv_obj_set_align( ui_intercomText, LV_ALIGN_BOTTOM_MID );
	lv_obj_add_style(ui_intercomText, &btnTextStyle, 0);
	//lv_label_set_text(ui_intercomText,"Camera");
	lv_label_set_text(ui_intercomText, (const char *)ui_language_switch[camSetParam.languageType][INTERCOM_STR]);

	intercomPage_btn = ui_intercomImg;		
	lv_obj_add_event_cb(intercomPage_btn, event_handler, LV_EVENT_ALL, NULL);
	lv_group_add_obj(home_group, intercomPage_btn);
	user_pagebtn_list[0].pagebtn=intercomPage_btn;
	user_pagebtn_list[0].nimg=ui_imgset_iconHomeintercom[0];
	user_pagebtn_list[0].bimg=ui_imgset_iconHomeintercom[1];


/**ui_cameraBtn **/
	lv_obj_t * ui_cameraBtn = lv_obj_create(ui_homePage);
	lv_obj_clear_flag( ui_cameraBtn, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
	lv_obj_set_flex_flow(ui_cameraBtn,LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(ui_cameraBtn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_style(ui_cameraBtn, &pageBtnStyle, 0);

	lv_obj_t *ui_cameraImg = lv_obj_create(ui_cameraBtn);
	lv_obj_add_style(ui_cameraImg, &btnImgStyle, 0);
	lv_obj_set_style_bg_img_src( ui_cameraImg, ui_imgset_iconHomeCamera[0], LV_PART_MAIN | LV_STATE_DEFAULT );

	lv_obj_t *ui_cameraText = lv_label_create(ui_cameraBtn);
	// lv_obj_set_align( ui_cameraText, LV_ALIGN_BOTTOM_MID );
	lv_obj_add_style(ui_cameraText, &btnTextStyle, 0);
	//lv_label_set_text(ui_cameraText,"Camera");
	lv_label_set_text(ui_cameraText, (const char *)ui_language_switch[camSetParam.languageType][TAKEPHOTO_STR]);

	cameraPage_btn = ui_cameraImg;		
	lv_obj_add_event_cb(cameraPage_btn, event_handler, LV_EVENT_ALL, NULL);
	lv_group_add_obj(home_group, cameraPage_btn);
	user_pagebtn_list[1].pagebtn=cameraPage_btn;
	user_pagebtn_list[1].nimg=ui_imgset_iconHomeCamera[0];
	user_pagebtn_list[1].bimg=ui_imgset_iconHomeCamera[1];

	/**ui_albumBtn **/

	lv_obj_t * ui_albumBtn = lv_obj_create(ui_homePage);
	lv_obj_clear_flag( ui_albumBtn, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
	lv_obj_set_flex_flow(ui_albumBtn,LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(ui_albumBtn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_style(ui_albumBtn, &pageBtnStyle, 0);

	lv_obj_t *ui_albumImg = lv_obj_create(ui_albumBtn);
	lv_obj_add_style(ui_albumImg, &btnImgStyle, 0);
	lv_obj_set_style_bg_img_src( ui_albumImg, ui_imgset_iconHomePlayer[0], LV_PART_MAIN | LV_STATE_DEFAULT );

	lv_obj_t *ui_albumText = lv_label_create(ui_albumBtn);
	lv_obj_add_style(ui_albumText, &btnTextStyle, 0);
	lv_label_set_text(ui_albumText, (const char *)ui_language_switch[camSetParam.languageType][PHOTO_STR]);

	albumPage_btn = ui_albumImg;	
	lv_obj_add_event_cb(albumPage_btn, event_handler, LV_EVENT_ALL, NULL);
	lv_group_add_obj(home_group, albumPage_btn);
	user_pagebtn_list[2].pagebtn=albumPage_btn;
	user_pagebtn_list[2].nimg=ui_imgset_iconHomePlayer[0];
	user_pagebtn_list[2].bimg=ui_imgset_iconHomePlayer[1];

	
	camera_gvar.immediately_reflash_flag=1;

}