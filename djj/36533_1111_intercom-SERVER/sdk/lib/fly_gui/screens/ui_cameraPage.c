#include "sys_config.h"
#include "typesdef.h"
#include "lib/video/dvp/cmos_sensor/csi.h"
#include "dev.h"
#include "devid.h"
#include "hal/gpio.h"
#include "hal/lcdc.h"
#include "hal/spi.h"
#include "osal/irq.h"
#include "osal/string.h"
#include "dev/vpp/hgvpp.h"
#include "dev/scale/hgscale.h"
#include "dev/jpg/hgjpg.h"
#include "dev/lcdc/hglcdc.h"
#include "osal/semaphore.h"
//#include "lib/sdhost/sdhost.h"
#include "lib/lcd/lcd.h"
#include "lib/lcd/gui.h"
#include "dev/vpp/hgvpp.h"
#include "dev/csi/hgdvp.h"
#include "lib/video/dvp/jpeg/jpg.h"
#include "hal/dma.h"
#include "lv_demo_widgets.h"
#include "openDML.h"
#include "osal/mutex.h"
#include "avidemux.h"
#include "playback/playback.h"
#include "lib/vef/video_ef.h"
#include "vpp_ipf_src.h"
#include "lib/umac/ieee80211.h"

#include "keyScan.h"
#include "../lvgl.h"
#include "ui_language.h"
#include "fly_demo.h"
#include "clock_app.h"
#include "syscfg.h"
#include "sonic_process.h"
#include "magic_sound.h"
#include "vpp_ipf_src.h"
#include "play_pcmtone.h"

#include "osal/task.h"
#include "osal/semaphore.h"
#include "osal/mutex.h"


uint8_t s_cam_shot_return_pending = 0;

/* 发送 worker */
static uint8_t              s_cam_send_inited = 0;
static struct os_task       s_cam_send_task;
static struct os_semaphore  s_cam_send_sem;
static struct os_mutex      s_cam_send_mtx;
static char                 s_cam_send_path[64] = {0};
static volatile uint8_t     s_cam_send_has_job = 0;

/* LVGL timers：拍照完成检查 + 回微信后再 post */
static lv_timer_t *cam_return_timer = NULL;
static lv_timer_t *cam_post_timer   = NULL;
static char        s_cam_post_path[64] = {0};

extern volatile uint8_t bbm_displaydecode_run;
extern Vpp_stream photo_msg;
extern lcd_msg lcd_info;
extern volatile vf_cblk g_vf_cblk;
extern gui_msg gui_cfg;

extern volatile uint8_t itp_finish;
extern uint32_t get_takephoto_thread_status(void);
extern uint8_t  get_bbm_take_photo_status(void);
extern void     bbm_take_photo(uint8_t num);
extern int      bbm_start_record(void);
extern void     bbm_stop_record(void);

extern void     client_send_wakeup_cmd(uint8_t cnt);
extern void     client_send_sleep_cmd(uint8_t cnt);

extern uint8_t  get_wifi_connect_flag(void);
extern int      wechat_send_image_file_as_msg(const char *img_path, uint16_t *out_msg_id, uint32_t *out_size);

extern uint8_t  g_camera_from_page;
extern uint8_t  s_wechat_focus_idx;
extern void     wechat_update_focus_style(void);

extern volatile char g_last_shot_path[64];                 // 来自 AT_save_photo.c
extern void     wechat_set_pending_photo_bubble(const char *img_path); // 来自 ui_WeChatPage.c
extern void     wechat_request_focus_idx(uint8_t idx);     // 来自 ui_WeChatPage.c

static void cam_photo_send_request(const char *img_path);
static void cam_post_to_wechat_cb(lv_timer_t *t);
static void cam_photo_send_worker(void *arg);
static void cam_photo_send_init_once(void);
static void cam_check_photo_done_cb(lv_timer_t *t);

/* 简单安全拷贝 */
static void safe_strcpy(char *dst, uint32_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    uint32_t i = 0;
    for (; i + 1 < dst_sz && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}


void doubleSenor_pdn_set(void)
{
	gpio_iomap_output(PIN_DVP_PDN1,GPIO_IOMAP_OUTPUT);
	gpio_set_val(PIN_DVP_PDN1,1);
	gpio_iomap_output(PIN_DVP_PDN,GPIO_IOMAP_OUTPUT);
	gpio_set_val(PIN_DVP_PDN,1);
	os_sleep_ms(20);

	if(camera_gvar.camera_switch)
	{
		gpio_set_val(PIN_DVP_PDN1,0);
	}
	else
	{
		gpio_set_val(PIN_DVP_PDN,0);
	}
}
void dvp_frontback_exchange(void)
{
	struct jpg_device * jpeg0_dev = (struct jpg_device *)dev_get(HG_JPG0_DEVID);		

#if DVP_EN
	struct dvp_device *dvp_device = (struct dvp_device*)dev_get(HG_DVP_DEVID);
	struct vpp_device *vpp_device = (struct vpp_device*)dev_get(HG_VPP_DEVID);
#endif

	os_printf("%s %d\r\n",__FUNCTION__);

	//打印机使能
	
	{
#if DVP_EN
		gpio_iomap_output(PIN_DVP_PDN,GPIO_IOMAP_OUTPUT);
		gpio_set_val(PIN_DVP_PDN,1);
		gpio_iomap_output(PIN_DVP_PDN1,GPIO_IOMAP_OUTPUT);
		gpio_set_val(PIN_DVP_PDN1,1);
		
		jpg_close(jpeg0_dev);
		dvp_close(dvp_device);
		vpp_close(vpp_device);

		bool csi_ret;
		bool csi_cfg();
		bool csi_open();



	#if SDH_I2C2_REUSE
		uint32 sdhost_i2c2_exchange(int sdh_stop_en);
		sdhost_i2c2_exchange(1);
	#endif
		csi_ret = csi_cfg();
		if(csi_ret)
			csi_open();
		//vpp_close(vpp_device);
		jpg_open(jpeg0_dev);
	#if SDH_I2C2_REUSE
		sdhost_i2c2_exchange(0);
	#endif

#endif
	}

}
//网络发送 worker
static void cam_photo_send_worker(void *arg)
{
    (void)arg;
    os_printf("[camimg] worker start\r\n");

    while (1) {
        os_sema_down(&s_cam_send_sem, -1);

        char path[64] = {0};

        os_mutex_lock(&s_cam_send_mtx, -1);
        if (s_cam_send_has_job) {
            safe_strcpy(path, sizeof(path), s_cam_send_path);
            s_cam_send_has_job = 0;
        }
        os_mutex_unlock(&s_cam_send_mtx);

        if (!path[0]) continue;

        if (!get_wifi_connect_flag()) {
            os_printf("[camimg] offline, skip send: %s\r\n", path);
            continue;
        }

        uint16_t msg_id = 0;
        uint32_t size   = 0;
        int ret = wechat_send_image_file_as_msg(path, &msg_id, &size);

        os_printf("[camimg] send ret=%d msg_id=%u size=%u path=%s\r\n",
                  ret, (unsigned)msg_id, (unsigned)size, path);
    }
}

static void cam_photo_send_init_once(void)
{
    if (s_cam_send_inited) return;
    s_cam_send_inited = 1;

    os_mutex_init(&s_cam_send_mtx);
    os_sema_init(&s_cam_send_sem, 0);

    OS_TASK_INIT("cam_img_send",
                 &s_cam_send_task,
                 cam_photo_send_worker,
                 0,
                 OS_TASK_PRIORITY_NORMAL,
                 3072);   /* 栈给大一点，发送里通常会有局部buf */
}

/* UI线程调用：投递发送请求（不阻塞） */
static void cam_photo_send_request(const char *img_path)
{
    if (!img_path || !img_path[0]) return;

    cam_photo_send_init_once();

    os_mutex_lock(&s_cam_send_mtx, -1);
    safe_strcpy(s_cam_send_path, sizeof(s_cam_send_path), img_path);
    s_cam_send_has_job = 1;                 /* 覆盖旧任务，只发最新一张 */
    os_mutex_unlock(&s_cam_send_mtx);

    os_sema_up(&s_cam_send_sem);
}

static void cam_post_to_wechat_cb(lv_timer_t *t)
{
    (void)t;

    /* 等真的回到微信页再做 */
    if (camera_gvar.page_cur != PAGE_WECHAT) return;

    if (cam_post_timer) {
        lv_timer_del(cam_post_timer);
        cam_post_timer = NULL;
    }

    if (s_cam_post_path[0]) {
        /* 现在在 wechat 页，调用它就不会 skip ui_post 了 */
        wechat_set_pending_photo_bubble(s_cam_post_path);

        /* 再后台发送（你之前的 worker 投递） */
        cam_photo_send_request(s_cam_post_path);

        s_cam_post_path[0] = '\0';
    }
}

//拍照完成轮询 timer
static void cam_check_photo_done_cb(lv_timer_t *t)
{
    (void)t;

    if (get_bbm_take_photo_status() == 0) {

        if (cam_return_timer) {
            lv_timer_del(cam_return_timer);
            cam_return_timer = NULL;
        }

        char path[64] = {0};
        safe_strcpy(path, sizeof(path), (const char *)g_last_shot_path);

        printf("[Camera] Photo saved: %s. Returning to WeChat...\r\n", path);
		if (g_last_shot_path[0] != '\0') {
			os_strncpy(s_cam_post_path, (const char *)g_last_shot_path, sizeof(s_cam_post_path)-1);
			s_cam_post_path[sizeof(s_cam_post_path)-1] = '\0';
		}

		/* 先回微信页 */
		wechat_request_focus_idx(3);
		s_cam_shot_return_pending = 0;
		lv_page_select(PAGE_WECHAT);

		/* 再等微信页起来后做 UI + send */
		if (cam_post_timer == NULL) {
			cam_post_timer = lv_timer_create(cam_post_to_wechat_cb, 50, NULL);  // 50~200ms都行
		}
    }
}

//相机页按键事件
void ui_event_cameraPage(lv_event_t * e){
	uint32_t* key_val = (uint32_t*)e->param;
	lv_event_code_t code = lv_event_get_code(e);
	struct vpp_device *vpp_dev;
	struct scale_device *scale_dev;
	struct dma_device *dma1_dev;
	uint32_t retval = 0;

	dma1_dev = (struct dma_device *)dev_get(HG_M2MDMA_DEVID); 
	vpp_dev = (struct vpp_device *)dev_get(HG_VPP_DEVID);
	scale_dev = (struct scale_device *)dev_get(HG_SCALE1_DEVID);

	if(code==USER_KEY_EVENT)
	{
		switch(*key_val)
		{
			case AD_VOL_UP:
				break;

			case AD_VOL_DOWN:
				break;

			case AD_LEFT: 
				break;

			case AD_RIGHT:		
				break;
			case AD_BACK:
			case KEY_BACK:
				// 如果正在等待自动返回，取消定时器
				if (cam_return_timer) {
					lv_timer_del(cam_return_timer);
					cam_return_timer = NULL;
					s_cam_shot_return_pending = 0;
				}
				if(rec_open)
				{
					bbm_stop_record();
					rec_open = 0;
				}

				if (g_camera_from_page == PAGE_WECHAT) 
				{ 
					printf("## camera back -> WECHAT\n"); 
					wechat_request_focus_idx(3);
					lv_page_select(PAGE_WECHAT);
				} else 
				{ 
					printf("## camera back -> HOME\n"); 
					lv_page_select(PAGE_HOME); 
					camera_gvar.pagebtn_index = 1; // 看你主页相机是第几个 
					camera_gvar.immediately_reflash_flag = 1;
				}
				if(camera_gvar.camera_switch==1)
				{
					camera_gvar.camera_switch = 0;
					printf("##camera_gvar.camera_switch =%d \n\r",camera_gvar.camera_switch);
					dvp_frontback_exchange();
				}
				break;

			case AD_PRESS:
			case KEY_CAMERA:	
				os_printf("## take photo  AD_PRESS\n");	
				printf("rec_open = %d\r\n",rec_open);
				if(rec_open==0)
				{
					#if 1
					if(camera_gvar.sd_online==0)
					{
						noticeAnimationStart(4);
						break;
					}

					/*came界面 拍照按键声音*/
					play_pcmtone(&shottone);
					/*came界面 拍照按键声音*/


					os_printf("## take photo\n");
					takePhotoAnimationStart();

					if(get_bbm_take_photo_status()==0){
						bbm_take_photo(1);
						/* 如果相机是从微信进来的：拍完自动回微信 */
						if (g_camera_from_page == PAGE_WECHAT) {
							s_cam_shot_return_pending = 1;
							// 创建定时器，每100ms检查一次拍照是否完成
							if (cam_return_timer == NULL) {
								cam_return_timer = lv_timer_create(cam_check_photo_done_cb, 100, NULL);
							}
						}
					}
					#else
					printf("gui_cfg.take_photo_num:%d\r\n",gui_cfg.take_photo_num);

					if(!get_takephoto_thread_status())
					{
						take_photo_thread_init(gui_cfg.photo_w,gui_cfg.photo_h,gui_cfg.take_photo_num);

						takePhotoAnimationStart();
					}
					else
					{
						os_printf("%s err,get_takephoto_thread_status:%d\n",__FUNCTION__,get_takephoto_thread_status());
					}
					#endif
				}
				break;
			
			default:
				break;
		}
	}
	else if(code==LV_EVENT_CLICKED)
	{

	}
}


void ui_cameraPage_screen_init(){
	static lv_style_t cameraPageStyle;

	static lv_style_t debugstyle;	

	lv_style_init(&debugstyle);
	lv_style_set_text_font(&debugstyle,&lv_font_montserrat_18);
	
	lv_style_reset(&cameraPageStyle);
	lv_style_init(&cameraPageStyle);
	lv_style_set_width(&cameraPageStyle, lv_obj_get_width(lv_scr_act()));
	lv_style_set_height(&cameraPageStyle, lv_obj_get_height(lv_scr_act()));
	//lv_style_set_bg_color(&cameraPageStyle, lv_color_hex(0x040404));	
	lv_style_set_bg_color(&cameraPageStyle, lv_color_hex(0x000000));	
	//lv_style_set_bg_opa(&cameraPageStyle, 0);	
	lv_style_set_shadow_color(&cameraPageStyle, lv_color_make(0x00, 0x00, 0x00));
	lv_style_set_border_width(&cameraPageStyle, 0);
	lv_style_set_radius(&cameraPageStyle,0);
	// lv_style_set_outline_color(&menuPanelStyle, lv_color_make(0x00, 0x00, 0x00));
	lv_style_set_pad_all(&cameraPageStyle, 0);
	lv_style_set_pad_gap(&cameraPageStyle,0);


	ui_cameraPage = lv_obj_create(lv_scr_act());

	lv_obj_clear_flag( ui_cameraPage, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
	curPage_obj = ui_cameraPage;
	lv_obj_add_style(ui_cameraPage, &cameraPageStyle, 0);
	lv_obj_set_style_text_font(ui_cameraPage, &ui_font_alimamaShuHei16, 0);

	//if(get_wifi_connect_flag())
	//	lv_obj_set_style_bg_opa(ui_cameraPage, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
	//else
	lv_obj_set_style_bg_opa(ui_cameraPage, 255, LV_PART_MAIN| LV_STATE_DEFAULT);

	lv_obj_add_event_cb(curPage_obj, event_handler, LV_EVENT_ALL, NULL);




	ui_camTopBar = lv_obj_create(ui_cameraPage);
	lv_obj_set_width( ui_camTopBar, lv_pct(100));
	lv_obj_set_height( ui_camTopBar, lv_pct(12));
	lv_obj_set_align( ui_camTopBar, LV_ALIGN_TOP_MID );
	lv_obj_clear_flag( ui_camTopBar, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
	lv_obj_set_style_radius(ui_camTopBar, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(ui_camTopBar, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT );
	lv_obj_set_style_bg_opa(ui_camTopBar, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(ui_camTopBar, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_pad_left(ui_camTopBar, 5, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_pad_right(ui_camTopBar, 5, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_pad_top(ui_camTopBar, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_pad_bottom(ui_camTopBar, 0, LV_PART_MAIN| LV_STATE_DEFAULT);

// SD卡状态图标
#if 0
	ui_csdIconImg = lv_img_create(ui_camTopBar);
	lv_img_set_src(ui_csdIconImg, &iconSdc);
	lv_obj_set_width( ui_csdIconImg, LV_SIZE_CONTENT);  /// 1
	lv_obj_set_height( ui_csdIconImg, LV_SIZE_CONTENT);   /// 1
	lv_obj_set_x( ui_csdIconImg, -32 );
	lv_obj_set_y( ui_csdIconImg, 0 );
	lv_obj_set_align( ui_csdIconImg, LV_ALIGN_RIGHT_MID );
	lv_obj_add_flag( ui_csdIconImg, LV_OBJ_FLAG_ADV_HITTEST );   /// Flags
	lv_obj_clear_flag( ui_csdIconImg, LV_OBJ_FLAG_SCROLLABLE );    /// Flags

	if(camera_gvar.sd_online)
		lv_obj_clear_flag(ui_csdIconImg, LV_OBJ_FLAG_HIDDEN );   /// Flags 
	else
		lv_obj_add_flag(ui_csdIconImg, LV_OBJ_FLAG_HIDDEN); 
#endif

#if 0
	ui_DvIconImg = lv_img_create(ui_camTopBar);
	lv_img_set_src(ui_DvIconImg, &iconDv_r);
	lv_obj_set_width( ui_DvIconImg, LV_SIZE_CONTENT);  /// 1
	lv_obj_set_height( ui_DvIconImg, LV_SIZE_CONTENT);   /// 1
	lv_obj_align(ui_DvIconImg, LV_ALIGN_TOP_MID, 0, 0);

	lv_obj_add_flag( ui_DvIconImg, LV_OBJ_FLAG_ADV_HITTEST );   /// Flags
	lv_obj_clear_flag( ui_DvIconImg, LV_OBJ_FLAG_SCROLLABLE );    /// Flags

	lv_obj_add_flag(ui_DvIconImg, LV_OBJ_FLAG_HIDDEN); 
#endif

#if 0
	ui_cSdStaLabel = lv_label_create(ui_csdIconImg);
	lv_obj_set_width( ui_cSdStaLabel, LV_SIZE_CONTENT);  /// 1
	lv_obj_set_height( ui_cSdStaLabel, LV_SIZE_CONTENT);   /// 1
	lv_obj_set_align( ui_cSdStaLabel, LV_ALIGN_CENTER );
	lv_label_set_text(ui_cSdStaLabel,LV_SYMBOL_OK);//""
	lv_obj_set_style_text_color(ui_cSdStaLabel, lv_color_hex(0x05F80A), LV_PART_MAIN | LV_STATE_DEFAULT );
	lv_obj_set_style_text_opa(ui_cSdStaLabel, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_cSdStaLabel, &lv_font_montserrat_14, LV_PART_MAIN| LV_STATE_DEFAULT);
#endif
#if 1
	ui_camIconImg = lv_img_create(ui_camTopBar);
	lv_img_set_src(ui_camIconImg, &iconFlagPhoto);
	lv_obj_set_width( ui_camIconImg, LV_SIZE_CONTENT);  /// 1
	lv_obj_set_height( ui_camIconImg, LV_SIZE_CONTENT);   /// 1
	lv_obj_add_flag( ui_camIconImg, LV_OBJ_FLAG_ADV_HITTEST );   /// Flags
	lv_obj_clear_flag( ui_camIconImg, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
#endif

//电量图标
//#if 1
//	ui_camBatImg = lv_img_create(ui_camTopBar);
//	lv_img_set_src(ui_camBatImg, ui_imgset_iconBat[get_batlevel()]);
//	lv_obj_set_width( ui_camBatImg, LV_SIZE_CONTENT);  /// 1
//	lv_obj_set_height( ui_camBatImg, LV_SIZE_CONTENT);   /// 1
//	lv_obj_set_align( ui_camBatImg, LV_ALIGN_RIGHT_MID );
//
//	lv_obj_add_flag( ui_camBatImg, LV_OBJ_FLAG_ADV_HITTEST );   /// Flags
//	lv_obj_clear_flag( ui_camBatImg, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
//#else
//	ui_cBatIconLabel = lv_label_create(ui_camTopBar);
//	lv_obj_set_width( ui_cBatIconLabel, LV_SIZE_CONTENT);  /// 1
//	lv_obj_set_height( ui_cBatIconLabel, LV_SIZE_CONTENT);   /// 1
//	lv_obj_set_align( ui_cBatIconLabel, LV_ALIGN_TOP_RIGHT );
//	lv_label_set_text(ui_cBatIconLabel,LV_SYMBOL_BATTERY_1);//""
//	lv_obj_set_style_text_color(ui_cBatIconLabel, lv_color_hex(0x4AA1FF), LV_PART_MAIN | LV_STATE_DEFAULT );
//	lv_obj_set_style_text_opa(ui_cBatIconLabel, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_text_font(ui_cBatIconLabel, &lv_font_montserrat_22, LV_PART_MAIN| LV_STATE_DEFAULT);
//#endif

	ui_camPrinterFlag = lv_img_create(ui_camTopBar);
	lv_img_set_src(ui_camPrinterFlag, &iconPrinter);
	lv_obj_set_width( ui_camPrinterFlag, LV_SIZE_CONTENT);  /// 1
	lv_obj_set_height( ui_camPrinterFlag, LV_SIZE_CONTENT);   /// 1
	lv_obj_set_x( ui_camPrinterFlag, -64 );
	lv_obj_set_y( ui_camPrinterFlag, 0 );
	lv_obj_set_align( ui_camPrinterFlag, LV_ALIGN_RIGHT_MID );
	lv_obj_add_flag( ui_camPrinterFlag, LV_OBJ_FLAG_ADV_HITTEST );   /// Flags
	lv_obj_clear_flag( ui_camPrinterFlag, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
	lv_obj_add_flag( ui_camPrinterFlag, LV_OBJ_FLAG_HIDDEN );   /// Flags
	camera_gvar.printer_flag =0;

//	ui_camBtmBar = lv_obj_create(ui_cameraPage);
//	lv_obj_set_width( ui_camBtmBar, lv_pct(100));
//	lv_obj_set_height( ui_camBtmBar, lv_pct(12));
//	lv_obj_set_align( ui_camBtmBar, LV_ALIGN_BOTTOM_MID );
//	lv_obj_clear_flag( ui_camBtmBar, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
//	lv_obj_set_style_radius(ui_camBtmBar, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_bg_color(ui_camBtmBar, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT );
//	lv_obj_set_style_bg_opa(ui_camBtmBar, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_border_width(ui_camBtmBar, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_pad_left(ui_camBtmBar, 5, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_pad_right(ui_camBtmBar, 5, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_pad_top(ui_camBtmBar, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_pad_bottom(ui_camBtmBar, 0, LV_PART_MAIN| LV_STATE_DEFAULT);

#if 0
	ui_cTimeIconLabel = lv_label_create(ui_camBtmBar);
	lv_obj_set_width( ui_cTimeIconLabel, LV_SIZE_CONTENT);  /// 1
	lv_obj_set_height( ui_cTimeIconLabel, LV_SIZE_CONTENT);   /// 1
	lv_obj_set_align( ui_cTimeIconLabel, LV_ALIGN_BOTTOM_LEFT );
	//lv_label_set_text(ui_cTimeIconLabel,"2023/11/30 14:59:48");
	lv_obj_set_style_text_color(ui_cTimeIconLabel, lv_color_hex(0xF8D00B), LV_PART_MAIN | LV_STATE_DEFAULT );
	lv_obj_set_style_text_opa(ui_cTimeIconLabel, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_cTimeIconLabel, &lv_font_montserrat_16, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_clock_display(ui_cTimeIconLabel,&sw_rtc,NULL);

#endif
	
	ui_RecTimeIconLabel = lv_label_create(ui_camBtmBar);
	lv_obj_set_width( ui_RecTimeIconLabel, LV_SIZE_CONTENT);  /// 1
	lv_obj_set_height( ui_RecTimeIconLabel, LV_SIZE_CONTENT);   /// 1
	lv_obj_set_align( ui_RecTimeIconLabel,  LV_ALIGN_TOP_MID );
	lv_label_set_text(ui_RecTimeIconLabel,"00:00:00");
	lv_obj_set_style_text_color(ui_RecTimeIconLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT );
	lv_obj_set_style_text_opa(ui_RecTimeIconLabel, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_RecTimeIconLabel, &lv_font_montserrat_20, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_label_set_recolor(ui_RecTimeIconLabel,true);
	
//	lv_obj_add_flag(ui_RecTimeIconLabel, LV_OBJ_FLAG_HIDDEN); 



	// ui_photoQualityLabel = lv_label_create(ui_camBtmBar);
	// lv_obj_set_width( ui_photoQualityLabel, LV_SIZE_CONTENT);  /// 1
	// lv_obj_set_height( ui_photoQualityLabel, LV_SIZE_CONTENT);   /// 1
	// lv_obj_set_align( ui_photoQualityLabel, LV_ALIGN_BOTTOM_RIGHT );
	// lv_label_set_text(ui_photoQualityLabel,"1M");
	// lv_obj_set_style_text_color(ui_photoQualityLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT );
	// lv_obj_set_style_text_opa(ui_photoQualityLabel, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
	// lv_obj_set_style_text_font(ui_photoQualityLabel, &lv_font_montserrat_20, LV_PART_MAIN| LV_STATE_DEFAULT);

	//lv_obj_add_event_cb(ui_cameraPage, ui_event_cameraPage, LV_EVENT_ALL, NULL);
	// ui_focusBtn = lv_imgbtn_create(ui_cameraPage);
	// lv_imgbtn_set_src(ui_focusBtn, LV_IMGBTN_STATE_RELEASED, NULL, &iconFocus, NULL);
	// lv_imgbtn_set_src(ui_focusBtn, LV_IMGBTN_STATE_PRESSED, NULL, &iconFocusP, NULL);
	// lv_obj_set_width( ui_focusBtn, 168);
	// lv_obj_set_height( ui_focusBtn, 128);
	// lv_obj_set_align( ui_focusBtn, LV_ALIGN_CENTER );


	ui_focusImg = lv_img_create(ui_cameraPage);
	lv_img_set_src(ui_focusImg, ui_imgset_iconFocus[0]);
	lv_obj_set_width( ui_focusImg, LV_SIZE_CONTENT);  /// 1
	lv_obj_set_height( ui_focusImg, LV_SIZE_CONTENT);   /// 1
	lv_obj_add_flag( ui_focusImg, LV_OBJ_FLAG_ADV_HITTEST );   /// Flags
	lv_obj_clear_flag( ui_focusImg, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
	lv_obj_set_align( ui_focusImg, LV_ALIGN_CENTER );

	lv_obj_add_flag( ui_focusImg, LV_OBJ_FLAG_HIDDEN );   /// Flags


	#if 1// notic dialog
	ui_dialogPanel = lv_obj_create(ui_cameraPage);
	lv_obj_set_width( ui_dialogPanel, 160);
	lv_obj_set_height( ui_dialogPanel, 120);
	lv_obj_set_align( ui_dialogPanel, LV_ALIGN_CENTER );
	// lv_obj_set_flex_flow(ui_dialogPanel,LV_FLEX_FLOW_COLUMN_WRAP);
	// lv_obj_set_flex_align(ui_dialogPanel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_clear_flag( ui_dialogPanel, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
	lv_obj_set_style_bg_color(ui_dialogPanel, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT );
	lv_obj_set_style_bg_opa(ui_dialogPanel, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(ui_dialogPanel, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_pad_left(ui_dialogPanel, 5, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_pad_right(ui_dialogPanel, 5, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_pad_top(ui_dialogPanel, 5, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_pad_bottom(ui_dialogPanel, 5, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_pad_row(ui_dialogPanel, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_pad_column(ui_dialogPanel, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_add_flag( ui_dialogPanel, LV_OBJ_FLAG_HIDDEN );   /// Flags
	lv_obj_set_style_text_font(ui_dialogPanel, &alifangyuan16, 0);


	lv_obj_t * ui_dialogTitle = lv_label_create(ui_dialogPanel);
	lv_obj_set_width( ui_dialogTitle, LV_SIZE_CONTENT);  /// 1
	lv_obj_set_height( ui_dialogTitle, LV_SIZE_CONTENT);   /// 1
	lv_obj_set_align( ui_dialogTitle, LV_ALIGN_TOP_LEFT );
	lv_obj_set_style_text_color(ui_dialogTitle, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT );
	lv_obj_set_style_text_opa(ui_dialogTitle, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
	//lv_obj_set_style_text_font(ui_dialogTitle, &lv_font_montserrat_16, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_label_set_text(ui_dialogTitle,"提醒:");

	ui_dialogContent = lv_label_create(ui_dialogPanel);
	lv_obj_set_width( ui_dialogContent, LV_SIZE_CONTENT);  /// 1
	lv_obj_set_height( ui_dialogContent, LV_SIZE_CONTENT);   /// 1
	lv_obj_set_align( ui_dialogContent, LV_ALIGN_CENTER );
	lv_obj_set_style_text_color(ui_dialogContent, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT );
	lv_obj_set_style_text_opa(ui_dialogContent, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_label_set_recolor(ui_dialogContent, 1);
	lv_label_set_text(ui_dialogContent,"请插入#ff0088 存储卡#");
	

	
	#if 0
    lv_obj_t *btn_cancel = lv_btn_create(ui_dialogPanel);
    lv_obj_set_size(btn_cancel, 36, 26);  
	lv_obj_set_align( btn_cancel, LV_ALIGN_BOTTOM_LEFT );

	lv_obj_t *label_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(label_cancel, "取消");
	lv_obj_set_align( label_cancel, LV_ALIGN_CENTER );

    lv_obj_t *btn_confirm = lv_btn_create(ui_dialogPanel);
    lv_obj_set_size(btn_confirm, 36, 26);  
	lv_obj_set_align( btn_confirm, LV_ALIGN_BOTTOM_RIGHT );
	
    lv_obj_t *label_confirm = lv_label_create(btn_confirm);
    lv_label_set_text(label_confirm, "继续");
	lv_obj_set_align( label_confirm, LV_ALIGN_CENTER );
	#endif

	#endif

}