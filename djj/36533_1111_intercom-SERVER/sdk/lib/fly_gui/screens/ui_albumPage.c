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
#include "osal/msgqueue.h"

#include "keyScan.h"
#include "../lvgl.h"
#include "ui_language.h"
#include "fly_demo.h"
#include "clock_app.h"
#include "lib/fly_gui/screens/ui_albumPage.h"
#include <csi_kernel.h>

// 内存踩踏检测宏定义
#define NODE_MAGIC_PATTERN    0xDEADBEEF
#define NODE_CANARY_PATTERN   0x12345678

// 安全的FileNode访问包装
static inline int check_node_integrity(const FileNode *node) {
    if (!node) return 0;

    // 检查name字段前4字节是否被破坏
    uint32_t *name_start = (uint32_t*)node->name;
    if (name_start[0] == NODE_MAGIC_PATTERN || name_start[0] == 0x1100c0ff ||
        name_start[0] == 0x00000000 || name_start[0] == 0xFFFFFFFF) {
        os_printf("CORRUPT: name field corrupted! node=0x%08x, name_start=0x%08x\r\n",
               (uint32_t)node, name_start[0]);
        return -1;
    }

    // 检查next指针是否合理
    if (node->next == (FileNode*)0x1100c0ff || node->next == NULL) {
        // next为NULL是可能的（链表末尾），但如果是在中间位置就异常
        os_printf("DEBUG: node->next=0x%08x (checking if this is valid)\r\n",
               (uint32_t)node->next);
    }

    // 检查name字段是否包含不可打印字符
    for (int i = 0; i < 16 && i < 128; i++) {
        if (node->name[i] == 0) break; // 字符串结束
        if (node->name[i] < 0x20 || node->name[i] > 0x7E) {
            os_printf("CORRUPT: name[%d]=0x%02x invalid character!\r\n", i, node->name[i]);
            return -2;
        }
    }

    return 1;
}

k_task_handle_t preview_task;

uint8_t album_thumbnail_buffers[MAX_THUMBNAILS_PRELOAD][SCALE_ALBUM_CONFIG_W*CAMERA_ALBUM_SCALE_H*2] __attribute__ ((aligned(32), section(".psram.src")));

// 全局初始化状态，防止重复初始化
static bool album_page_initialized = false;
static bool album_init_in_progress = false;

typedef enum {
    ALBUM_MODE_NORMAL = 0,
    ALBUM_MODE_WECHAT_PICK,     // 微聊挑图发送
    ALBUM_MODE_WECHAT_PREVIEW,  // 微聊聊天消息预览（只看不发）
} album_mode_t;

static album_mode_t s_album_mode = ALBUM_MODE_NORMAL;
static uint8_t s_album_return_page = PAGE_HOME;  // 默认从主页进
static uint8_t s_album_has_target = 0;
static char    s_album_target_name[64];   // 只存 basename，例如 "IMG_0001.JPG"
static uint8_t s_album_preview_only = 0; 

extern FileNode *album_list_head;      // 链表头
extern FileNode *album_list_tail;      // 链表尾
extern FileNode *album_cur_selected;   // 当前选中的节点指针

extern void album_next_file(void);     // 移动指针到下一个
extern void album_prev_file(void);     // 移动指针到上一个
extern void delete_selected_file(void);// 删除当前节点

extern Vpp_stream photo_msg;
extern lcd_msg lcd_info;
extern volatile vf_cblk g_vf_cblk;
extern gui_msg gui_cfg;

extern uint8_t g_img_from_page;
extern uint8_t  s_wechat_focus_idx;

extern void prevp_AnimationStart(void);
extern void wechat_update_focus_style(void);
extern void wechat_set_pending_photo_bubble(const char *img_path);
void wechat_set_return_focus_photo(const char *img_path);
extern int wechat_send_image_file_as_msg(const char *img_path, uint16_t *out_msg_id, uint32_t *out_size);

// lcd.c 中申请用于相册缩略图yuv数据缓存
extern uint8 album_decode_config_mem[SCALE_ALBUM_CONFIG_W * CAMERA_ALBUM_SCALE_H + SCALE_ALBUM_CONFIG_W * CAMERA_ALBUM_SCALE_H/2] __attribute__ ((aligned(4),section(".psram.src")));
extern uint8 album_decode_config_mem1[SCALE_ALBUM_CONFIG_W * CAMERA_ALBUM_SCALE_H + SCALE_ALBUM_CONFIG_W * CAMERA_ALBUM_SCALE_H/2] __attribute__ ((aligned(4),section(".psram.src")));
extern uint8 album_decode_config_mem2[SCALE_ALBUM_CONFIG_W * CAMERA_ALBUM_SCALE_H + SCALE_ALBUM_CONFIG_W * CAMERA_ALBUM_SCALE_H/2] __attribute__ ((aligned(4),section(".psram.src")));
//extern  uint8 album_decode_config_mem3[SCALE_ALBUM_CONFIG_W * CAMERA_ALBUM_SCALE_H+SCALE_ALBUM_CONFIG_W * CAMERA_ALBUM_SCALE_H/2] __attribute__ ((aligned(4),section(".psram.src")));

extern uint8 *album_decode_mem;
extern uint8 *album_decode_mem1;
extern uint8 *album_decode_mem2;
uint8 *album_decode_mem3;

struct os_msgqueue album_yuv_msgq;
struct os_semaphore gui_sync_sem;

album_state_t        album_state; 
album_preload_sys_t  album_preload_inf;    //相册缩略预加载信息

// 相册选中状态 (保持向后兼容)
static int8_t current_selected_index = -1;  // 当前选中的图片索引，-1表示未选中
static uint8_t total_photos = 0;             // 当前显示的照片总数

static void gui_pause_cb(void *param) {
    lv_timer_pause(lv_disp_get_default()->refr_timer);
	os_sema_up(&gui_sync_sem);// 通知暂停完成
}

static void gui_resume_cb(void *param) {
    lv_timer_resume(lv_disp_get_default()->refr_timer);
    lv_obj_t *img_obj = (lv_obj_t *)param;
    if(img_obj) lv_obj_invalidate(img_obj); // 触发重绘
    os_sema_up(&gui_sync_sem); // 通知恢复完成
}

void lvgl_pause_refresh(void) {
    lv_async_call(gui_pause_cb, NULL);       // 异步到 GUI 线程
    os_sema_down(&gui_sync_sem, osWaitForever); // 等暂停完成
}

void lvgl_resume_refresh(uint8_t img_indicate) {

    lv_async_call(gui_resume_cb, albumDis[img_indicate].img);   // 异步到 GUI 线程
    os_sema_down(&gui_sync_sem, osWaitForever); // 等恢复完成
}

//void preview_files(void)
//{
//	// 根据实际照片数量设置，最多显示9张
//	uint32_t actual_photos = camera_gvar.album_filenums;
//	album_preload_inf.total_fnum = (actual_photos > MAX_THUMBNAILS_PRELOAD) ? MAX_THUMBNAILS_PRELOAD : actual_photos;
//
//	// 确保从第一个文件开始处理
//	if (album_list_head) {
//		album_cur_selected = album_list_head;
//		printf("preview_files: Reset to first file %s\n", album_cur_selected->name);
//	}
//
//	for(int i=0; i < album_preload_inf.total_fnum; i++)  // 所有相册界面照片全部缩略一遍
//	 {
//	    // 当前album_cur_selected应该已经指向正确的文件，不需要提前移动
//	    sprintf((char *)name_rec_photo,"%s%s","0:/DCIM/",album_cur_selected->name);
//		// 缩略图处理使能 , scale2_done中根据该标志位判断是背景图sacle还是相册scale
//		album_preload_inf.preload_enabled = 1;
//	 	printf("name_rec_photo:%s\r\n",name_rec_photo);
//		printf("jpeg_photo_explain:w=%d ,h=%d \r\n",album_preload_inf.thumbnail_width,album_preload_inf.thumbnail_height);
//	 	jpeg_photo_explain(name_rec_photo,album_preload_inf.thumbnail_width ,album_preload_inf.thumbnail_height);
//
//		// 移动到下一个文件（为下次循环准备），但最后一次不需要移动
//		if(i < album_preload_inf.total_fnum - 1) {
//			album_next_file(); //索引
//		}
//	 }
//}


void preview_jpg_loop()
{
	uint32 t1, t2;

	os_msgq_init(&album_yuv_msgq, 9);
	os_sema_init(&gui_sync_sem , 0);

	while(1)
	{
		// 检查是否需要处理相册图片
		if(!(camera_gvar.page_cur == PAGE_ALBUM &&
		     album_preload_inf.system_initialized &&
		     album_preload_inf.pending_updates > 0)) {
			os_sleep_ms(10);
			continue;
		}

		// 本轮要写入的缩略图槽位：用 loaded_count（比 current_index 更直观）
		int idx = album_preload_inf.loaded_count;   // 0..8
		if(idx >= MAX_THUMBNAILS_PRELOAD) {
			os_sleep_ms(10);
			continue;
		}

		// 使用文件索引从数组获取文件信息（完全避免指针操作）
		uint8_t file_idx = album_preload_inf.current_file_index;
		if(file_idx >= album_preload_inf.file_list_count) {
			os_printf("WARN: current_file_index %d >= file_list_count %d, resetting\r\n",
			       file_idx, album_preload_inf.file_list_count);
			album_preload_inf.current_file_index = 0;
			os_sleep_ms(10);
			continue;
		}

		// 从文件信息数组获取当前文件
		album_file_info_t *file_info = &album_preload_inf.file_list[file_idx];
		if(!file_info->is_valid || strlen(file_info->filename) == 0) {
			os_printf("ERROR: File info at index %d is invalid, skipping\r\n", file_idx);
			// 跳到下一个文件
			album_preload_inf.current_file_index++;
			album_preload_inf.pending_updates--;
			os_sleep_ms(10);
			continue;
		}

		// 调试：打印当前文件信息
		os_printf("Processing file %d: %s\r\n", file_idx, file_info->filename);

		t1 = os_jiffies();

		// 清空消息队列，避免旧指针残留
		while(os_msgq_get(&album_yuv_msgq, 0) != 0) {
			// 清空队列
		}

		// 触发解码当前文件
		const char *filename = file_info->filename;
		snprintf((char*)name_rec_photo, 32, "0:/DCIM/%s", filename);

		// 调试：添加延迟，看看是否是SD卡访问时序问题
		os_sleep_ms(50);

		// 设置标志并调用JPEG解码
		album_preload_inf.preload_enabled = 1;
		os_printf("Calling jpeg_photo_explain: %s\r\n", name_rec_photo);
		jpeg_photo_explain(name_rec_photo,
		                   album_preload_inf.thumbnail_width,
		                   album_preload_inf.thumbnail_height);


		// 阻塞等待YUV数据
		uint8_t* yuv_data = (uint8_t*)os_msgq_get(&album_yuv_msgq, osWaitForever);
		if(!yuv_data) {
			os_printf("ERROR: Failed to get YUV data for file %s\r\n", name_rec_photo);
			os_sleep_ms(10);
			continue;
		}

		os_printf("Received YUV data, converting to RGB...\r\n");

		// cache invalidate + 转 RGB
		uint32_t w = album_preload_inf.thumbnail_width;
		uint32_t h = album_preload_inf.thumbnail_height;
		if (w != SCALE_ALBUM_CONFIG_W || h != CAMERA_ALBUM_SCALE_H) {
			os_printf("FATAL: thumb size mismatch w=%u h=%u\r\n", w, h);
			continue;
		}
		printf("## w= %d,h=%d\r\n",w,h);
		uint32_t yuv_size = w*h + w*h/2;
		printf("## yuv_size=%d\r\n",yuv_size);
		// YUV数据缓存失效
		csi_dcache_invalid_range(yuv_data, yuv_size);
		printf("## \r\n");
		// 暂停GUI刷新
		lvgl_pause_refresh();

		// 计算RGB缓冲区大小并检查边界
		uint32_t expected_rgb_size = SCALE_ALBUM_CONFIG_W * CAMERA_ALBUM_SCALE_H * 2;
		uint32_t actual_rgb_size = w * h * 2;
		printf("## expected_rgb_size=%d ,actual_rgb_size=%d\r\n",expected_rgb_size,actual_rgb_size);
		if(actual_rgb_size > expected_rgb_size) {
			os_printf("ERROR: RGB size overflow! expected=%u, actual=%u, skipping\r\n",
			       expected_rgb_size, actual_rgb_size);
			lvgl_resume_refresh(idx);
			// 跳过这个文件，但不减少计数器
			os_sleep_ms(10);
			continue;
		}

		// YUV转RGB565
		yuv420p_to_rgb565((uint8_t *)yuv_data, album_preload_inf.thumbnails[idx].rgb_buffer, w, h);

		// 清理RGB数据缓存
		sys_dcache_clean_range((uint32_t*)album_preload_inf.thumbnails[idx].rgb_buffer, w*h*2);

		// 恢复GUI刷新
		lvgl_resume_refresh(idx);

		// 标记缩略图有效
		album_preload_inf.thumbnails[idx].is_valid = true;
		album_preload_inf.thumbnails[idx].load_time = os_jiffies();
		strncpy(album_preload_inf.thumbnails[idx].filename, file_info->filename, 31);
		album_preload_inf.thumbnails[idx].filename[31] = '\0';

		// 更新计数器
		album_preload_inf.loaded_count++;
		album_preload_inf.pending_updates--;

		// 推进文件索引
		if(album_preload_inf.pending_updates <= 0) {
			os_printf("All %d files processed\r\n", album_preload_inf.loaded_count);
		} else {
			album_preload_inf.current_file_index++;
			os_printf("Advanced to file %d, remaining: %d\r\n",
			       album_preload_inf.current_file_index, album_preload_inf.pending_updates);
		}

		t2 = os_jiffies();
		os_printf("File processing completed in %d ms\r\n", t2-t1);

		// 立即更新显示
		if(idx < 9) {
			albumDis[idx].dsc.data = album_preload_inf.thumbnails[idx].rgb_buffer;
			lv_img_set_src(albumDis[idx].img, &(albumDis[idx].dsc));
			lv_obj_clear_flag(albumDis[idx].img, LV_OBJ_FLAG_HIDDEN);
			os_printf("Updated display for thumbnail[%d]\r\n", idx);
		}
	}
}

void albumnails_thread_init(){
	// albumnails_thread 用于接收scale2_done结束后的yuv数据转rgb ,pro=15,
	csi_kernel_task_new((k_task_entry_t)preview_jpg_loop, "preview_album", NULL, 15, 0, NULL, 1024, &preview_task);
}


// 选中框管理函数
void album_select_photo(int8_t index) {
    // 检查索引范围
    if(index < 0 || index >= total_photos || index >= 9) {
        return;
    }

    // 隐藏之前的选中框
    if(current_selected_index >= 0 && current_selected_index < 9) {
        lv_obj_add_flag(albumDis[current_selected_index].select_border, LV_OBJ_FLAG_HIDDEN);
    }

    // 显示新的选中框
    current_selected_index = index;
    lv_obj_clear_flag(albumDis[current_selected_index].select_border, LV_OBJ_FLAG_HIDDEN);

    os_printf("Selected photo index: %d\n", current_selected_index);
}

// 移动选中框
void album_move_selection(int8_t direction) {
    if(total_photos == 0) return;

    int8_t new_index = current_selected_index;

    if(current_selected_index == -1) {
        // 如果没有选中任何图片，选中第一张
        new_index = 0;
    } else {
        new_index = current_selected_index + direction;

        // 边界检查
        if(new_index < 0) new_index = total_photos - 1;
        if(new_index >= total_photos) new_index = 0;
    }

    album_select_photo(new_index);
}

void lv_ablum_9_flush(void)
{
	if(camera_gvar.page_cur == PAGE_ALBUM)//photo
	{
		
		uint8_t ablum_flash_num = album_preload_inf.total_fnum; //待预览数更新
		if(ablum_flash_num)
		{
			printf("## lv_ablum_9_flush,ablum_flash_num=%d \n\r",ablum_flash_num);

			// 更新照片总数
			total_photos = ablum_flash_num;

			// 重置选中状态
			current_selected_index = -1;

			for(uint8_t i=0;i<9;i++)
			{
				printf("i=%d,album_preload_inf.thumbnails[i].is_valid = %d.\r\n",i,album_preload_inf.thumbnails[i].is_valid);
				// 检查 is_valid 标志，避免显示未填充/旧数据
				if(i<ablum_flash_num && album_preload_inf.thumbnails[i].is_valid)
				{
					albumDis[i].dsc.data = album_preload_inf.thumbnails[i].rgb_buffer;
					lv_img_set_src(albumDis[i].img, &(albumDis[i].dsc));
					lv_obj_clear_flag(albumDis[i].img, LV_OBJ_FLAG_HIDDEN );   /// Flags
					printf("### Update albumDis[%d].dsc.data (addr=0x%08x)\r\n",
					       i, (uint32_t)album_preload_inf.thumbnails[i].rgb_buffer);
					// 隐藏选中框
					lv_obj_add_flag(albumDis[i].select_border, LV_OBJ_FLAG_HIDDEN );
				}
				else
				{
					lv_obj_add_flag( albumDis[i].img, LV_OBJ_FLAG_HIDDEN );   /// Flags
					lv_obj_add_flag( albumDis[i].select_border, LV_OBJ_FLAG_HIDDEN );   /// 隐藏选中框
//					printf("Hide thumbnail[%d] (invalid or out of range)\r\n", i);
					//albumDis[i].dsc.data = NULL;
				}

				// up sem
			}

			// 如果有照片，默认选中第一张
			if(total_photos > 0) {
				album_select_photo(0);
			}

			ablum_flash_num =0;
		}
	}
}


// ========================================
// SD卡文件变化检测辅助函数
// ========================================

// 快速获取当前SD卡中的文件数量（不构建完整列表）
static uint32_t get_album_file_count(void)
{
    uint32_t count = 0;
    DIR album_dir;
    FILINFO finfo;
    FRESULT ret;

    // 打开SD卡目录
    ret = f_opendir(&album_dir, "0:/DCIM");
    if(ret != FR_OK) {
        return 0;
    }

    // 遍历文件并计数
    while(1) {
        ret = f_readdir(&album_dir, &finfo);
        if(ret != FR_OK || finfo.fname[0] == 0) {
            break;
        }

        // 检查是否是JPEG或AVI文件
        if(strncmp(finfo.fname, "JPEG", 4) == 0 || strncmp(finfo.fname, "AVI", 3) == 0) {
            count++;
        }
    }

    f_closedir(&album_dir);
    return count;
}

// ========================================
// 相册缓存清理函数
// ========================================

// 清理所有缩略图缓存和状态
void album_clear_all_cache(void)
{
    os_printf("Clearing album thumbnail cache after SD card format...\r\n");

    // 1. 清理相册文件列表
    free_album_list();
    album_cur_selected = NULL;

    // 2. 重置文件数量
    camera_gvar.album_filenums = 0;

    // 3. 清理预加载系统状态
    album_preload_inf.loaded_count = 0;
    album_preload_inf.current_index = 0;
    album_preload_inf.total_fnum = 0;
    album_preload_inf.pending_updates = 0;
    album_preload_inf.preload_enabled = false;
    album_preload_inf.scan_completed = false;
    album_preload_inf.system_initialized = false;

    // 4. 清理所有缩略图缓冲区标记
    for(int i = 0; i < MAX_THUMBNAILS_PRELOAD; i++) {
        album_preload_inf.thumbnails[i].is_valid = false;
        album_preload_inf.thumbnails[i].filename[0] = '\0';
        album_preload_inf.thumbnails[i].file_entry = NULL;

        // 清理RGB缓冲区内容（可选，但有助于释放内存）
        if(album_preload_inf.thumbnails[i].rgb_buffer) {
            memset(album_preload_inf.thumbnails[i].rgb_buffer, 0,
                   SCALE_ALBUM_CONFIG_W * CAMERA_ALBUM_SCALE_H * 2);
        }
    }

    // 5. 重置相册状态
    album_state.mode = ALBUM_MODE_GRID;
    album_state.grid_selection = -1;
    album_state.single_current = 0;
    album_state.single_image_loaded = false;

    // 6. 清理LVGL缓存（如果有）
    lv_img_cache_invalidate_src(NULL);

    // 7. 重置初始化标志，强制下次进入时重新初始化
    // 注意：由于album_initialized是static变量，我们无法直接重置它
    // 但我们重置了文件数量为0，下次检查时会触发重新初始化

    os_printf("Album thumbnail cache cleared successfully\r\n");
}

// ========================================
// 相册页面封装API实现
// ========================================
album_result_t album_page_init(void)
{
    os_printf("Initializing album page...\n");

    // 强制一次性初始化：如果已经初始化过，直接返回
    if(album_page_initialized) {
        os_printf("Album page already initialized, skipping duplicate init.\r\n");
        return ALBUM_RESULT_SUCCESS;
    }

    // 防止重复初始化和竞争条件
    if(album_init_in_progress) {
        os_printf("Album initialization already in progress, skipping...\r\n");
        return ALBUM_RESULT_SUCCESS;
    }

    // 设置初始化进行中标志
    album_init_in_progress = true;

    // 检查是否需要重新扫描（SD卡文件可能发生变化）
    bool need_rescan = true;

    if(0) { // 改为始终检查，因为已经有互斥保护
          // 检查文件数量是否发生变化
          uint32_t current_file_count = get_album_file_count();
          os_printf("Checking file changes: previous=%d, current=%d\r\n",
                 camera_gvar.album_filenums, current_file_count);

          if(current_file_count == camera_gvar.album_filenums && current_file_count > 0) {
              need_rescan = false;
              os_printf("Album page already initialized with %d files, no changes detected\r\n", current_file_count);
              return ALBUM_RESULT_SUCCESS;
          } else {
              os_printf("File count changed: was %d, now %d. Rescanning...\r\n",
                     camera_gvar.album_filenums, current_file_count);
              need_rescan = true;

              // 需要重新扫描时清理旧数据
              free_album_list();
              // 重置预加载系统状态
              album_preload_inf.loaded_count = 0;
              album_preload_inf.current_index = 0;
              album_preload_inf.pending_updates = 0;
          }
      }

      // 检查系统状态
      if(!album_decode_config_mem || !album_decode_config_mem1 ||!album_decode_config_mem2) {
          os_printf("ERROR: Album decode memory not allocated\r\n");
          return ALBUM_RESULT_MEM_ERROR;
      }

    // 只有在需要重新扫描时才执行扫描
    if(need_rescan) {
        // 1. 扫描SD卡文件
        os_printf("Rescanning SD card for new files...\r\n");
        camera_gvar.album_filenums = scan_album_files();
        if(camera_gvar.album_filenums == 0) {
            os_printf("No photo files found in SD card\n");
            album_page_initialized = true;  // 即使没有文件也要标记为已初始化，防止重复扫描
            return ALBUM_RESULT_NO_FILES;
        }
        os_printf("Found %d photo files\n", camera_gvar.album_filenums);
        os_printf("=== NEW ALBUM CODE: Using file info array ===\n");  // 版本标记

        // 2. 填充文件信息数组（避免后续使用FileNode指针）
        album_preload_inf.file_list_count = 0;
        album_preload_inf.current_file_index = 0;

        FileNode *node = album_list_head;
        uint8_t file_idx = 0;
        while(node && file_idx < MAX_ALBUM_FILES) {
            // 复制文件信息到数组
            strncpy(album_preload_inf.file_list[file_idx].filename,
                   node->name, 31);
            album_preload_inf.file_list[file_idx].filename[31] = '\0';
            album_preload_inf.file_list[file_idx].file_size = 0;  // 暂时无法获取大小
            album_preload_inf.file_list[file_idx].file_type = node->filetype;  // 直接使用FILETYPE枚举
            album_preload_inf.file_list[file_idx].is_valid = true;

            os_printf("File[%d]: %s (type=%d)\r\n", file_idx,
                   album_preload_inf.file_list[file_idx].filename,
                   album_preload_inf.file_list[file_idx].file_type);

            file_idx++;
            node = node->next;
        }

        album_preload_inf.file_list_count = file_idx;
        album_preload_inf.total_fnum = file_idx;

        os_printf("Populated file info array: %d files\r\n", album_preload_inf.file_list_count);
      }

    // 只有在需要重新扫描时才执行完整的初始化
    if(need_rescan) {
        // 初始化相册状态
        album_state.mode = ALBUM_MODE_GRID;
        album_state.grid_selection = 0;
        album_state.single_current = 0;
        album_state.single_image_loaded = false;

        // 缩略图尺寸配置
        album_preload_inf.thumbnail_width = SCALE_ALBUM_CONFIG_W;
        album_preload_inf.thumbnail_height = CAMERA_ALBUM_SCALE_H;

        album_preload_inf.loaded_count = 0;
        album_preload_inf.current_index = 0;
        album_preload_inf.total_fnum = camera_gvar.album_filenums;
        album_preload_inf.preload_enabled = false;
        album_preload_inf.scan_completed = false;
        album_preload_inf.system_initialized = false;

        // 正确初始化pending_updates
        album_preload_inf.pending_updates = camera_gvar.album_filenums;
        os_printf("Initialized pending_updates to %d (total files: %d)\r\n",
               album_preload_inf.pending_updates, camera_gvar.album_filenums);


        // 注意：JPEG解码器配置将在每次调用jpeg_photo_explain前进行，
        // 不需要在这里预先配置

        // rgb缓冲区链接，使用PSRAM静态分配的album_thumbnail_buffers
            for(int i = 0; i < MAX_THUMBNAILS_PRELOAD; i++) {
                // 直接使用静态分配的PSRAM缓冲区，并检查对齐
                uint32_t buffer_addr = (uint32_t)album_thumbnail_buffers[i];

                // 设置RGB缓冲区指针
                album_preload_inf.thumbnails[i].rgb_buffer = album_thumbnail_buffers[i];

                // 清空缓冲区，避免残留数据
                memset(album_thumbnail_buffers[i], 0, SCALE_ALBUM_CONFIG_W * CAMERA_ALBUM_SCALE_H * 2);

                album_preload_inf.thumbnails[i].is_valid = false;
                album_preload_inf.thumbnails[i].filename[0] = '\0';
                album_preload_inf.thumbnails[i].file_entry = NULL;

                // 调试：打印RGB缓冲区设置
                os_printf("RGB buffer[%d] set to: 0x%08x (cleared)\r\n", i,
                       (uint32_t)album_preload_inf.thumbnails[i].rgb_buffer);
            }

            // 设置初始化完成标志
            album_preload_inf.system_initialized = true;
            album_page_initialized = true;  // 设置全局初始化标志
	}

    // 设置相册列表和打印
    if(camera_gvar.album_filenums) {
        album_cur_selected = album_list_head;
        print_album_list();

        // 启动串行预览处理（在preview_jpg_loop中进行）
        os_printf("Starting serial preview for %d files (pending_updates: %d)\r\n",
               camera_gvar.album_filenums, album_preload_inf.pending_updates);

        // 在初始化时配置JPEG解码器（只配置一次）
        album_decode_mem  = album_decode_config_mem;
        album_decode_mem1 = album_decode_config_mem;
        album_decode_mem2 = album_decode_config_mem;

        // 重置decode_num，确保从0开始（避免缓冲区混乱）
        extern volatile uint32 decode_num;
        decode_num = 0;
        os_printf("Reset decode_num to 0 for album processing\r\n");

        set_lcd_photo1_config(album_preload_inf.thumbnail_width,
                             album_preload_inf.thumbnail_height, 0);
        jpg_decode_scale_config((uint32_t)album_decode_mem);

        // 给硬件一点时间完成配置
        os_sleep_ms(100);

        os_printf("JPEG decoder configured for album thumbnails\r\n");

        // 初始化文件索引（从0开始）
        album_preload_inf.current_file_index = 0;
        os_printf("Initialized current_file_index to 0, total files: %d\r\n",
               album_preload_inf.file_list_count);

        // 注意：preload_enabled将在每次处理文件前设置
        // 不在这里设置，避免影响第一次scale2_done的判断
    }

//    os_printf("Album page initialization completed: Grid mode enabled (pending: %d)\n",
//           album_preload_inf.pending_updates);

    // 标记初始化完成
    album_page_initialized = true;
    album_init_in_progress = false;

    return ALBUM_RESULT_SUCCESS;
}


void album_enter_from(uint8_t from_page, album_mode_t mode)
{
    s_album_return_page = from_page;
    g_img_from_page     = from_page;
    s_album_mode        = mode;

    if (mode != ALBUM_MODE_WECHAT_PREVIEW) {
        s_album_has_target = 0;
        s_album_target_name[0] = '\0';
    }

    printf("[album] enter from=%d mode=%d\r\n", from_page, mode);

    global_avi_exit = 1;
    lv_page_select(PAGE_ALBUM);   // 关键：真正进入相册页
}


static const char *album_basename(const char *path)
{
    if (!path) return "";
    const char *p = path;
    const char *last = path;
    while (*p) {
        if (*p == '/' || *p == '\\') last = p + 1;
        p++;
    }
    return last;
}

static char upc(char c)
{
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    return c;
}

static int str_ieq(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";
    while (*a && *b) {
        if (upc(*a) != upc(*b)) return 0;
        a++; b++;
    }
    return (*a == 0 && *b == 0);
}

/* 设置“进入相册后要定位预览的文件” */
void album_set_target_preview(const char *full_path)
{
    if (!full_path || !full_path[0]) return;

    const char *base = album_basename(full_path);
    if (!base || !base[0]) return;
    printf("[album] set target preview full='%s' base='%s'\r\n",
           full_path ? full_path : "(null)",
           base ? base : "(null)");
		   
    os_strncpy(s_album_target_name, base, sizeof(s_album_target_name) - 1);
    s_album_target_name[sizeof(s_album_target_name) - 1] = '\0';
    s_album_has_target = 1;

    s_album_mode = ALBUM_MODE_WECHAT_PREVIEW;
    s_album_return_page = PAGE_WECHAT;
    g_img_from_page     = PAGE_WECHAT;

    printf("[album] set target preview: %s\r\n", s_album_target_name);
}

static void album_try_jump_to_target(void)
{
    if (!s_album_has_target) return;
    if (!album_list_head) { s_album_has_target = 0; return; }

    FileNode *curr = album_list_head;
    uint8_t found = 0;
    int dbg_i = 0;

    printf("[album] searching list for: '%s'\r\n", s_album_target_name);

    while (curr) {
        if (curr->name) {
            int eq  = (os_strcmp(curr->name, s_album_target_name) == 0);
            int ieq = str_ieq(curr->name, s_album_target_name);

            if (dbg_i < 6) {
                printf("[album] cmp[%d] item='%s' eq=%d ieq=%d\r\n",
                       dbg_i, curr->name, eq, ieq);
            }
            dbg_i++;

            if (ieq) { 
                album_cur_selected = curr;
                found = 1;
                break;
            }
        }
        curr = curr->next;
    }

    if (found && album_cur_selected) {
        snprintf((char *)name_rec_photo, 32, "%s%s", "0:/DCIM/", album_cur_selected->name);
        printf("[album] target found: %s (total_scanned=%d)\r\n", name_rec_photo, dbg_i);

        album_file_preview(name_rec_photo, album_cur_selected->filetype);
        lv_label_set_text(ui_fileFormatLabel, album_cur_selected->name);
    } else {
        printf("[album] target not found (total_scanned=%d)\r\n", dbg_i);
    }

    s_album_has_target = 0;
}


/* =================================================================
 * UI 事件处理核心逻辑
 * ================================================================= */
void ui_event_albumPage(lv_event_t * e){
	uint8_t name[16];
	uint32_t* key_val = (uint32_t*)e->param;
	lv_event_code_t code = lv_event_get_code(e);
	if(code==USER_KEY_EVENT)
	{
		switch(*key_val)
		{

			case AD_RIGHT:
			    if (s_album_mode == ALBUM_MODE_WECHAT_PREVIEW && g_img_from_page == PAGE_WECHAT) {
					printf("[album] PREVIEW locked: ignore RIGHT\r\n");
					break;
				}
				#if 1
				// 1. 移动链表指针
				album_next_file();
				prevp_AnimationStart();
				if(album_cur_selected) {
                    snprintf((char *)name_rec_photo, 32, "%s%s", "0:/DCIM/", album_cur_selected->name);
                    printf("view: %s\r\n", name_rec_photo);
                    // 切换前清除缓存，防止内存碎片
                    lv_img_cache_invalidate_src(NULL);
                    album_file_preview(name_rec_photo, album_cur_selected->filetype);
                    lv_label_set_text(ui_fileFormatLabel, album_cur_selected->name);
                }

				#else
				if(ablumlist==NULL)
				break;
				if(camera_gvar.album_fileindex)
				{
					camera_gvar.album_fileindex--;
				}
				else
					camera_gvar.album_fileindex = (camera_gvar.album_filenums-1);

				prevp_AnimationStart();
				snprintf((char *)name_rec_photo, 32, "%s%s", "0:/DCIM/", ablumlist[camera_gvar.album_fileindex].name);
				printf("name_rec_photo:%s\r\n",name_rec_photo);
				album_file_preview(name_rec_photo,ablumlist[camera_gvar.album_fileindex].filetype);
				lv_label_set_text(ui_fileFormatLabel,ablumlist[camera_gvar.album_fileindex].name);
				#endif
			break;
			

			case AD_LEFT: 
			    if (s_album_mode == ALBUM_MODE_WECHAT_PREVIEW && g_img_from_page == PAGE_WECHAT) {
					printf("[album] PREVIEW locked: ignore LEFT\r\n");
					break;
				}
				#if 1
				album_prev_file();
				prevp_AnimationStart();
				if(album_cur_selected) {
                    snprintf((char *)name_rec_photo, 32, "%s%s", "0:/DCIM/", album_cur_selected->name);
                    printf("view: %s\r\n", name_rec_photo);
                    lv_img_cache_invalidate_src(NULL);
                    album_file_preview(name_rec_photo, album_cur_selected->filetype);
                    lv_label_set_text(ui_fileFormatLabel, album_cur_selected->name);
                }

				#else
				if(ablumlist==NULL)
				break;
				if(camera_gvar.album_fileindex<(camera_gvar.album_filenums-1))
				{
					camera_gvar.album_fileindex++;
				}
				else
				{
					camera_gvar.album_fileindex =0;
				}
				nextp_AnimationStart();

				snprintf((char *)name_rec_photo, 32, "%s%s", "0:/DCIM/", ablumlist[camera_gvar.album_fileindex].name);
				printf("name_rec_photo:%s\r\n",name_rec_photo);
				album_file_preview(name_rec_photo,ablumlist[camera_gvar.album_fileindex].filetype);
				lv_label_set_text(ui_fileFormatLabel,ablumlist[camera_gvar.album_fileindex].name);
				#endif			
			break;
			
			case AD_VOL_DOWN:
			os_printf("## KEY_VOL_DOWN=\n");

			if(camSetParam.volumeSet)
			{
				camSetParam.volumeSet--;
			}
			render_vol_level(camSetParam.volumeSet);
			volume_adjust(camSetParam.volumeSet);
			volDisAnimationStart();
			os_printf("## cur_volumeSet=%d \n",camSetParam.volumeSet);
			break;

			case AD_VOL_UP:
			os_printf("## KEY_VOL_UP=\n");

			if(camSetParam.volumeSet<10)
			{
				camSetParam.volumeSet ++;
			}
			render_vol_level(camSetParam.volumeSet);
			volume_adjust(camSetParam.volumeSet);
			volDisAnimationStart();
			os_printf("## cur_volumeSet=%d \n",camSetParam.volumeSet);
			break;

			case KEY_CAMERA:
			case AD_PRESS:
			{
				if (!album_cur_selected) break;

				if (album_cur_selected->filetype == FT_AVI) {
					if (global_avi_exit) {
						os_printf("replay\r\n\r\n");
						album_file_preview(name_rec_photo, album_cur_selected->filetype);
						os_sleep_ms(10);
					}
					global_avi_running ^= BIT(0);
					break;
				}

				/* ====== 非 AVI：图片浏览 / 微聊挑图 / 微聊预览 ====== */

				/* 1) 微聊预览：OK = 回微聊（不发送） */
				if (s_album_return_page == PAGE_WECHAT &&
					s_album_mode == ALBUM_MODE_WECHAT_PREVIEW) {

					printf("[album] PREVIEW OK -> back to wechat (no send)\r\n");
					wechat_set_return_focus_photo((const char *)name_rec_photo);

					s_album_mode = ALBUM_MODE_NORMAL;
					global_avi_exit = 1;
					lv_page_select(PAGE_WECHAT);
					break;
				}

				/* 2) 微聊挑图：OK = 发送 + 回微聊 + 高亮相册按钮 */
				if (s_album_return_page == PAGE_WECHAT &&
					s_album_mode == ALBUM_MODE_WECHAT_PICK) {

					printf("[album] PICK -> send photo to wechat: %s\r\n", name_rec_photo);

					wechat_set_pending_photo_bubble((const char *)name_rec_photo);
					// 强制刷新 UI
					lv_refr_now(NULL);
					
					uint16_t msg_id = 0;
					uint32_t size   = 0;
					int ret = wechat_send_image_file_as_msg((const char *)name_rec_photo, &msg_id, &size);
					printf("[album] send_image ret=%d msg_id=%u size=%u\r\n",
						   ret, (unsigned)msg_id, (unsigned)size);

					lv_img_cache_invalidate_src(NULL);
					
					s_album_mode = ALBUM_MODE_NORMAL;
					wechat_request_focus_idx(4);
					global_avi_exit = 1;
					lv_page_select(PAGE_WECHAT);
					break;
				}

				/* 3) 主页/普通浏览：OK 不允许跳转微聊（你可以在这里做“全屏/隐藏UI”等浏览功能） */
				printf("[album] NORMAL OK -> browse only, no jump\r\n");
				break;
			}
			
			case KEY_RECORD:
			case KEY_DELECT:
				if(camera_gvar.sd_online==0)
					break;
				if((global_avi_running)&&(album_cur_selected->filetype==FT_AVI))
				{
					global_avi_running=0;
					os_sleep_ms(10);
				}

				delete_selected_file();

				if (album_cur_selected)
				{
					//prevp_AnimationStart();
					sprintf((char *)name_rec_photo,"%s%s","0:/DCIM/",album_cur_selected->name);
					printf("name_rec_photo:%s\r\n",name_rec_photo);
					album_file_preview(name_rec_photo,album_cur_selected->filetype);
					lv_label_set_text(ui_fileFormatLabel,album_cur_selected->name);
				}
				break;
			
			case KEY_FORMAT_SD:
				if((camera_gvar.sd_online==0)||camera_gvar.notice_anim_times)
					break;

				lv_label_set_text(ui_dialogContent,"SD卡 格式化 #ff0088 开始#");
				noticeAnimationStart(255);
				if(FR_OK==format_sdcard())
				{
					// success
					lv_label_set_text(ui_dialogContent,"SD卡 格式化 #ff0088 成功#");
					noticeAnimationStart(2);

					// 清理相册缩略图缓存
					album_clear_all_cache();

					os_printf("SD card formatted and album cache cleared\r\n");
				}
				else
				{
					//failed 
					lv_label_set_text(ui_dialogContent,"SD卡 格式化 #ff0088 失败#");
					noticeAnimationStart(2);
				}
				break;
			
			case AD_BACK:
			case KEY_BACK:
				global_avi_exit = 1;

				if (g_img_from_page == PAGE_WECHAT) {
					if (s_album_mode == ALBUM_MODE_WECHAT_PREVIEW) {
						printf("[album] preview-only BACK -> return to wechat\r\n");
						wechat_set_return_focus_photo((const char *)name_rec_photo);
						s_album_preview_only = 0;
						// 预览返回：不改 wechat_focus_idx（由 wechat_try_restore_after_album 去恢复消息焦点）
					} else {
						// 挑图返回：高亮相册按钮
						s_wechat_focus_idx = 4;
					}

					s_album_mode = ALBUM_MODE_NORMAL;
					lv_page_select(PAGE_WECHAT);

					// 如果是挑图返回，这里再刷新一下底部高亮
					if (s_wechat_focus_idx == 4) wechat_update_focus_style();
				} else {
					s_album_mode = ALBUM_MODE_NORMAL;
					lv_page_select(s_album_return_page);

					if (s_album_return_page == PAGE_HOME) {
						camera_gvar.pagebtn_index = 2;
						camera_gvar.immediately_reflash_flag = 1;
					}
				}
				break;
				
			case KEY_BROWSE:
				global_avi_exit = 1;
				lv_page_select(PAGE_CAMERA);
				break;

				case KEY_POWEROFF:
				global_avi_exit = 1;
				lv_page_select(PAGE_POWEROFF);
				break;


			default:
			break;
		}
	}
	
}


void ui_albumPage_screen_init(){

	static lv_style_t albumPageStyle;

	album_group = lv_group_create();
	lv_indev_set_group(indev_keypad, album_group);
	group_cur = album_group;

	lv_style_reset(&albumPageStyle);
	lv_style_init(&albumPageStyle);
	// 设置全屏尺寸和背景图片，使用LVGL背景方式
	lv_style_set_width(&albumPageStyle, lv_obj_get_width(lv_scr_act()));
	lv_style_set_height(&albumPageStyle, lv_obj_get_height(lv_scr_act()));

	lv_style_set_bg_color(&albumPageStyle, lv_color_hex(0x000000));
	lv_style_set_bg_opa(&albumPageStyle, 255);	
	lv_style_set_shadow_color(&albumPageStyle, lv_color_make(0x00, 0x00, 0x00));
	lv_style_set_border_width(&albumPageStyle, 0);
//	 lv_style_set_outline_color(&menuPanelStyle, lv_color_make(0x00, 0x00, 0x00));
	lv_style_set_pad_top(&albumPageStyle, 0);
	lv_style_set_pad_bottom(&albumPageStyle, 0);
	lv_style_set_pad_left(&albumPageStyle,  0);
 	lv_style_set_pad_right(&albumPageStyle, 0);
	lv_style_set_radius(&albumPageStyle,0);

//	ui_albumPage = lv_obj_create(NULL);
	ui_albumPage = lv_obj_create(lv_scr_act()); //直接创建在屏幕上，背景图片会失效？
	lv_obj_clear_flag( ui_albumPage, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
	curPage_obj = ui_albumPage;
	lv_obj_add_style(ui_albumPage, &albumPageStyle, 0);
	lv_obj_set_style_text_font(ui_albumPage, &ui_font_alimamaShuHei16, 0);

	lv_obj_add_event_cb(curPage_obj, event_handler, LV_EVENT_ALL, NULL);

	ui_albumTopBar = lv_obj_create(ui_albumPage);
	lv_obj_set_width( ui_albumTopBar, lv_pct(100));
	lv_obj_set_height( ui_albumTopBar, lv_pct(12));
	lv_obj_set_align( ui_albumTopBar, LV_ALIGN_TOP_MID );
	lv_obj_clear_flag( ui_albumTopBar, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
	lv_obj_set_style_radius(ui_albumTopBar, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(ui_albumTopBar, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT );
	lv_obj_set_style_bg_opa(ui_albumTopBar, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(ui_albumTopBar, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_pad_left(ui_albumTopBar, 5, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_pad_right(ui_albumTopBar, 5, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_pad_top(ui_albumTopBar, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_pad_bottom(ui_albumTopBar, 0, LV_PART_MAIN| LV_STATE_DEFAULT);

	// ui_aSdIconImg = lv_img_create(ui_albumTopBar);
	// lv_img_set_src(ui_aSdIconImg, &ui_img_img_icon32x32_sdicon_32x32_png);
	// lv_obj_set_width( ui_aSdIconImg, LV_SIZE_CONTENT);  /// 1
	// lv_obj_set_height( ui_aSdIconImg, LV_SIZE_CONTENT);   /// 1
	// lv_obj_set_x( ui_aSdIconImg, 32 );
	// lv_obj_set_y( ui_aSdIconImg, 0 );
	// lv_obj_add_flag( ui_aSdIconImg, LV_OBJ_FLAG_ADV_HITTEST );   /// Flags
	// lv_obj_clear_flag( ui_aSdIconImg, LV_OBJ_FLAG_SCROLLABLE );    /// Flags

	// ui_aSdStaLabel = lv_label_create(ui_aSdIconImg);
	// lv_obj_set_width( ui_aSdStaLabel, LV_SIZE_CONTENT);  /// 1
	// lv_obj_set_height( ui_aSdStaLabel, LV_SIZE_CONTENT);   /// 1
	// lv_obj_set_align( ui_aSdStaLabel, LV_ALIGN_CENTER );
	// lv_label_set_text(ui_aSdStaLabel,LV_SYMBOL_OK); //""
	// lv_obj_set_style_text_color(ui_aSdStaLabel, lv_color_hex(0x05F80A), LV_PART_MAIN | LV_STATE_DEFAULT );
	// lv_obj_set_style_text_opa(ui_aSdStaLabel, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
	
	ui_aSdIconImg = lv_img_create(ui_albumTopBar);
	lv_img_set_src(ui_aSdIconImg, &iconSdc);
	lv_obj_set_width( ui_aSdIconImg, LV_SIZE_CONTENT);  /// 1
	lv_obj_set_height( ui_aSdIconImg, LV_SIZE_CONTENT);   /// 1
	lv_obj_set_x( ui_aSdIconImg, 32 );
	lv_obj_set_y( ui_aSdIconImg, 0 );
	lv_obj_add_flag( ui_aSdIconImg, LV_OBJ_FLAG_ADV_HITTEST );   /// Flags
	lv_obj_clear_flag( ui_aSdIconImg, LV_OBJ_FLAG_SCROLLABLE );    /// Flags

	if(camera_gvar.sd_online)
		lv_obj_clear_flag(ui_aSdIconImg, LV_OBJ_FLAG_HIDDEN );   /// Flags 
	else
		lv_obj_add_flag(ui_aSdIconImg, LV_OBJ_FLAG_HIDDEN); 

#if 0
	ui_albumPrinterFlag = lv_img_create(ui_albumTopBar);
	lv_img_set_src(ui_albumPrinterFlag, &iconPrinter);
	lv_obj_set_width( ui_albumPrinterFlag, LV_SIZE_CONTENT);  /// 1
	lv_obj_set_height( ui_albumPrinterFlag, LV_SIZE_CONTENT);   /// 1
	lv_obj_set_x( ui_albumPrinterFlag, -32 );
	lv_obj_set_y( ui_albumPrinterFlag, 0 );
	lv_obj_set_align( ui_albumPrinterFlag, LV_ALIGN_RIGHT_MID );
	lv_obj_add_flag( ui_albumPrinterFlag, LV_OBJ_FLAG_ADV_HITTEST );   /// Flags
	lv_obj_clear_flag( ui_albumPrinterFlag, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
	//lv_obj_add_flag( ui_albumPrinterFlag, LV_OBJ_FLAG_HIDDEN );   /// Flags
#endif

#if 1
	ui_albumBatImg = lv_img_create(ui_albumTopBar);
	lv_img_set_src(ui_albumBatImg, ui_imgset_iconBat[get_batlevel()]);
	lv_obj_set_width( ui_albumBatImg, LV_SIZE_CONTENT);  /// 1
	lv_obj_set_height( ui_albumBatImg, LV_SIZE_CONTENT);   /// 1
	lv_obj_set_align( ui_albumBatImg, LV_ALIGN_RIGHT_MID );
	lv_obj_add_flag( ui_albumBatImg, LV_OBJ_FLAG_ADV_HITTEST );   /// Flags
	lv_obj_clear_flag( ui_albumBatImg, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
#else
	ui_aBatIconLabel = lv_label_create(ui_albumTopBar);
	lv_obj_set_width( ui_aBatIconLabel, LV_SIZE_CONTENT);  /// 1
	lv_obj_set_height( ui_aBatIconLabel, LV_SIZE_CONTENT);   /// 1
	lv_obj_set_align( ui_aBatIconLabel, LV_ALIGN_TOP_RIGHT );
	lv_label_set_text(ui_aBatIconLabel,LV_SYMBOL_BATTERY_1);//""
	lv_obj_set_style_text_color(ui_aBatIconLabel, lv_color_hex(0x4AA1FF), LV_PART_MAIN | LV_STATE_DEFAULT );
	lv_obj_set_style_text_opa(ui_aBatIconLabel, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_aBatIconLabel, &lv_font_montserrat_22, LV_PART_MAIN| LV_STATE_DEFAULT);
#endif

	ui_albumIconImg = lv_img_create(ui_albumTopBar);
	lv_img_set_src(ui_albumIconImg, &iconFlagPlay);
	lv_obj_set_width( ui_albumIconImg, LV_SIZE_CONTENT);  /// 1
	lv_obj_set_height( ui_albumIconImg, LV_SIZE_CONTENT);   /// 1
	lv_obj_add_flag( ui_albumIconImg, LV_OBJ_FLAG_ADV_HITTEST );   /// Flags
	lv_obj_clear_flag( ui_albumIconImg, LV_OBJ_FLAG_SCROLLABLE );    /// Flags


    // 底部状态栏？
//	ui_albumBtmBar = lv_obj_create(ui_albumPage);
//	lv_obj_set_width( ui_albumBtmBar, lv_pct(100));
//	lv_obj_set_height( ui_albumBtmBar, lv_pct(12));
//	lv_obj_set_align( ui_albumBtmBar, LV_ALIGN_BOTTOM_MID );
//	lv_obj_clear_flag( ui_albumBtmBar, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
//	lv_obj_set_style_radius(ui_albumBtmBar, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
////	lv_obj_set_style_bg_color(ui_albumBtmBar, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT );
//	lv_obj_set_style_bg_opa(ui_albumBtmBar, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_border_width(ui_albumBtmBar, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_pad_left(ui_albumBtmBar, 5, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_pad_right(ui_albumBtmBar, 5, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_pad_top(ui_albumBtmBar, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_pad_bottom(ui_albumBtmBar, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//
//	ui_fileFormatLabel = lv_label_create(ui_albumBtmBar);
//	lv_obj_set_width( ui_fileFormatLabel, LV_SIZE_CONTENT);  /// 1
//	lv_obj_set_height( ui_fileFormatLabel, LV_SIZE_CONTENT);   /// 1
//	lv_obj_set_align( ui_fileFormatLabel, LV_ALIGN_BOTTOM_LEFT );
//	
//	//lv_label_set_text(ui_fileFormatLabel,"MOV00003.AVI");
//	#if 1
//	if(camera_gvar.album_filenums)
//	{
//		lv_label_set_text(ui_fileFormatLabel,album_cur_selected->name);
//	}
//	else
//		lv_label_set_text(ui_fileFormatLabel,"NULL");
//
//	#else
//	lv_label_set_text(ui_fileFormatLabel,ablumlist[camera_gvar.album_fileindex].name);
//	#endif
//
//	lv_obj_set_style_text_color(ui_fileFormatLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT );
//	lv_obj_set_style_text_opa(ui_fileFormatLabel, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_text_font(ui_fileFormatLabel, &lv_font_montserrat_16, LV_PART_MAIN| LV_STATE_DEFAULT);


//	ui_albumPrevBtn = lv_obj_create(ui_albumPage);
//	lv_obj_set_width( ui_albumPrevBtn, 28);
//	lv_obj_set_height( ui_albumPrevBtn, 25);
//    lv_obj_align(ui_albumPrevBtn,LV_ALIGN_LEFT_MID,0,0);
//	lv_obj_set_style_bg_opa(ui_albumPrevBtn, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_border_width(ui_albumPrevBtn, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_bg_img_src( ui_albumPrevBtn, ui_imgset_iconPrev[0], LV_PART_MAIN | LV_STATE_DEFAULT );
//
//
//	ui_albumNextBtn = lv_obj_create(ui_albumPage);
//	lv_obj_set_width( ui_albumNextBtn, 28);
//	lv_obj_set_height( ui_albumNextBtn, 25);
//    lv_obj_align(ui_albumNextBtn,LV_ALIGN_RIGHT_MID,0,0);
//	lv_obj_set_style_bg_opa(ui_albumNextBtn, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_border_width(ui_albumNextBtn, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_bg_img_src( ui_albumNextBtn, ui_imgset_iconNext[0], LV_PART_MAIN | LV_STATE_DEFAULT );

#if 0
	ui_fileNumsLabel = lv_label_create(ui_albumBtmBar);
	lv_obj_set_width( ui_fileNumsLabel, LV_SIZE_CONTENT);  /// 1
	lv_obj_set_height( ui_fileNumsLabel, LV_SIZE_CONTENT);   /// 1
	lv_obj_set_align( ui_fileNumsLabel, LV_ALIGN_BOTTOM_RIGHT );
	lv_label_set_text(ui_fileNumsLabel,"0011/0011");
	lv_obj_set_style_text_color(ui_fileNumsLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT );
	lv_obj_set_style_text_opa(ui_fileNumsLabel, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_fileNumsLabel, &lv_font_montserrat_16, LV_PART_MAIN| LV_STATE_DEFAULT);
#endif
	// lv_obj_add_event_cb(ui_albumPage, ui_event_albumPage, LV_EVENT_ALL, NULL);

	#if 0// notic dialog
	ui_dialogPanel = lv_obj_create(ui_albumPage);
	lv_obj_set_width( ui_dialogPanel, 160);
	lv_obj_set_height( ui_dialogPanel, 120);
	lv_obj_set_align( ui_dialogPanel, LV_ALIGN_CENTER );
	// lv_obj_set_flex_flow(ui_dialogPanel,LV_FLEX_FLOW_COLUMN_WRAP);
	// lv_obj_set_flex_align(ui_dialogPanel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_clear_flag( ui_dialogPanel, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
//	lv_obj_set_style_bg_color(ui_dialogPanel, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT );
	lv_obj_set_style_bg_opa(ui_dialogPanel, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
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
	lv_label_set_text(ui_dialogContent,"SD卡 #ff0088 格式化成功#");

    // lv_obj_t *btn_cancel = lv_btn_create(ui_dialogPanel);
    // lv_obj_set_size(btn_cancel, 36, 26);  
	// lv_obj_set_align( btn_cancel, LV_ALIGN_BOTTOM_LEFT );

	// lv_obj_t *label_cancel = lv_label_create(btn_cancel);
    // lv_label_set_text(label_cancel, "取消");
	// lv_obj_set_align( label_cancel, LV_ALIGN_CENTER );

    // lv_obj_t *btn_confirm = lv_btn_create(ui_dialogPanel);
    // lv_obj_set_size(btn_confirm, 36, 26);  
	// lv_obj_set_align( btn_confirm, LV_ALIGN_BOTTOM_RIGHT );
	
    // lv_obj_t *label_confirm = lv_label_create(btn_confirm);
    // lv_label_set_text(label_confirm, "继续");
	// lv_obj_set_align( label_confirm, LV_ALIGN_CENTER );

	#endif

//	ui_volPanel = lv_obj_create(ui_albumPage);	
//	lv_obj_set_width( ui_volPanel, 160);
//	lv_obj_set_height( ui_volPanel, 120);
//	lv_obj_set_align( ui_volPanel, LV_ALIGN_CENTER );
//	lv_obj_set_flex_flow(ui_volPanel,LV_FLEX_FLOW_COLUMN_WRAP);
//	lv_obj_set_flex_align(ui_volPanel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
//
//	lv_obj_clear_flag( ui_volPanel, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
//	lv_obj_set_style_bg_color(ui_volPanel, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT );
//	lv_obj_set_style_bg_opa(ui_volPanel, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_border_width(ui_volPanel, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_pad_left(ui_volPanel, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_pad_right(ui_volPanel, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_pad_top(ui_volPanel, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_pad_bottom(ui_volPanel, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_pad_row(ui_volPanel, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_pad_column(ui_volPanel, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_add_flag( ui_volPanel, LV_OBJ_FLAG_HIDDEN );   /// Flags

//	#if 1
//	ui_lisglabel = lv_label_create(ui_volPanel);
//	lv_obj_set_style_text_font(ui_lisglabel, &lv_font_montserrat_28, 0);
//	lv_obj_set_style_text_color(ui_lisglabel, lv_color_hex(0xFCA702), 0);
//	lv_label_set_text(ui_lisglabel, LV_SYMBOL_VOLUME_MAX);
//	#else
//	lv_obj_t *ui_lisgImage = lv_img_create(ui_volPanel);
//	lv_img_set_src(ui_lisgImage, &iconMenuVolume1);
//	lv_obj_set_width( ui_lisgImage, LV_SIZE_CONTENT);  /// 1
//	lv_obj_set_height( ui_lisgImage, LV_SIZE_CONTENT);   /// 1
//	lv_obj_set_align( ui_lisgImage, LV_ALIGN_CENTER );
//	lv_obj_add_flag( ui_lisgImage, LV_OBJ_FLAG_ADV_HITTEST );   /// Flags
//	lv_obj_clear_flag( ui_lisgImage, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
//	#endif

//	lv_obj_t * ui_list = lv_obj_create(ui_volPanel);
//	lv_obj_set_width( ui_list, 180);
//	lv_obj_set_height( ui_list, 40);
//	lv_obj_set_align( ui_list, LV_ALIGN_CENTER );
//	lv_obj_set_flex_flow(ui_list,LV_FLEX_FLOW_ROW);
//	lv_obj_set_flex_align(ui_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
//	lv_obj_clear_flag( ui_list, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
//	lv_obj_set_style_radius(ui_list, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_bg_color(ui_list, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT );
//	lv_obj_set_style_bg_opa(ui_list, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	lv_obj_set_style_border_width(ui_list, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//
//	for(uint8_t i=0;i<10;i++)
//	{
//		ui_volumeLevels[i]=lv_obj_create(ui_list);
//		lv_obj_set_width( ui_volumeLevels[i], 6);
//		lv_obj_set_height( ui_volumeLevels[i], 16);
//		lv_obj_set_align( ui_volumeLevels[i], LV_ALIGN_CENTER );
//		lv_obj_clear_flag( ui_volumeLevels[i], LV_OBJ_FLAG_SCROLLABLE );    /// Flags
//		lv_obj_set_style_radius(ui_volumeLevels[i], 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//		lv_obj_set_style_bg_color(ui_volumeLevels[i], lv_color_hex(0x6D6C6C), LV_PART_MAIN | LV_STATE_DEFAULT );
//		lv_obj_set_style_bg_opa(ui_volumeLevels[i], 255, LV_PART_MAIN| LV_STATE_DEFAULT);
//		lv_obj_set_style_border_width(ui_volumeLevels[i], 0, LV_PART_MAIN| LV_STATE_DEFAULT);
//	}

	
//	if(camSetParam.volumeSet>10)
//		camSetParam.volumeSet=10;
//
//	render_vol_level(camSetParam.volumeSet);
//
//	{
//		extern void mute_speaker(uint8_t mute);
//		mute_speaker(0);
//
//	}

	album_9_Panel = lv_obj_create(ui_albumPage);
	lv_obj_set_width( album_9_Panel, 240);  // 屏幕宽度
	lv_obj_set_height( album_9_Panel, 290);  // 减去顶部状态栏空间（320-40=280）
	lv_obj_set_align( album_9_Panel, LV_ALIGN_BOTTOM_MID );  // 底部居中对齐，避开顶部状态栏
	lv_obj_set_flex_flow(album_9_Panel,LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(album_9_Panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_clear_flag( album_9_Panel, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
	lv_obj_set_style_bg_color(album_9_Panel, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT );
	lv_obj_set_style_bg_opa(album_9_Panel, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(album_9_Panel, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_pad_left(album_9_Panel, 5, LV_PART_MAIN| LV_STATE_DEFAULT);   // 左右内边距
	lv_obj_set_style_pad_right(album_9_Panel, 5, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_pad_top(album_9_Panel, 5, LV_PART_MAIN| LV_STATE_DEFAULT);    // 上下内边距
	lv_obj_set_style_pad_bottom(album_9_Panel, 5, LV_PART_MAIN| LV_STATE_DEFAULT);
	lv_obj_set_style_pad_row(album_9_Panel, 8, LV_PART_MAIN| LV_STATE_DEFAULT);    // 行间距
	lv_obj_set_style_pad_column(album_9_Panel, 8, LV_PART_MAIN| LV_STATE_DEFAULT); // 列间距

	// 提取常用参数到局部变量，避免在循环中重复访问结构体
	const uint16_t thumb_width = album_preload_inf.thumbnail_width;
	const uint16_t thumb_height = album_preload_inf.thumbnail_height;

	for(uint8_t i=0;i<9;i++)
	{

		albumDis[i].panel = lv_obj_create(album_9_Panel);
		lv_obj_set_width( albumDis[i].panel, thumb_height + 2);
		lv_obj_set_height( albumDis[i].panel, thumb_width + 2);
		lv_obj_set_align( albumDis[i].panel, LV_ALIGN_CENTER );
		lv_obj_clear_flag( albumDis[i].panel, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
		lv_obj_set_style_border_color(albumDis[i].panel, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT );
		lv_obj_set_style_border_opa(albumDis[i].panel, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(albumDis[i].panel, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
		lv_obj_set_style_bg_opa(albumDis[i].panel, 0, LV_PART_MAIN| LV_STATE_DEFAULT);

		albumDis[i].img = lv_img_create(albumDis[i].panel);
		lv_obj_set_width( albumDis[i].img, LV_SIZE_CONTENT);  /// 1
		lv_obj_set_height( albumDis[i].img, LV_SIZE_CONTENT);   /// 1
		lv_obj_set_align( albumDis[i].img, LV_ALIGN_CENTER );
		lv_obj_add_flag( albumDis[i].img, LV_OBJ_FLAG_HIDDEN );   /// Flags

		// 创建选中框（初始隐藏）
		albumDis[i].select_border = lv_obj_create(albumDis[i].panel);
		lv_obj_set_width(albumDis[i].select_border, thumb_height + 1);    // 比图片大6像素
		lv_obj_set_height(albumDis[i].select_border, thumb_width + 1);  // 比图片大6像素
		lv_obj_align(albumDis[i].select_border, LV_ALIGN_CENTER, 0, 0);
		lv_obj_set_style_bg_opa(albumDis[i].select_border, 0, LV_PART_MAIN);  // 透明背景
		lv_obj_set_style_border_width(albumDis[i].select_border, 3, LV_PART_MAIN);  // 3像素边框，更明显
		lv_obj_set_style_border_color(albumDis[i].select_border, lv_color_hex(0x0078FF), LV_PART_MAIN);  // 蓝色边框
		lv_obj_set_style_radius(albumDis[i].select_border, 2, LV_PART_MAIN);  // 设置圆角半径
		// 确保选中框在最上层，不被图片覆盖
		lv_obj_move_foreground(albumDis[i].select_border);
		lv_obj_add_flag(albumDis[i].select_border, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏

		albumDis[i].dsc.data = NULL;
		albumDis[i].dsc.data_size = 2 * thumb_width * thumb_height;
		albumDis[i].dsc.header.w = thumb_width;
		albumDis[i].dsc.header.h = thumb_height;
		albumDis[i].dsc.header.cf = LV_IMG_CF_TRUE_COLOR;


	    lv_img_set_src(albumDis[i].img, &(albumDis[i].dsc));

	    // 设置图像旋转90度，解决横向显示问题
	    lv_img_set_angle(albumDis[i].img, 900);  // 90度旋转，单位是0.1度

	    // 设置图像缩放以适应旋转后的尺寸
	    int32_t zoom = 256;  // 原始大小 (256 = 1.0x)
	    lv_img_set_zoom(albumDis[i].img, zoom);

	    // 设置图像中心对齐，确保旋转后居中显示
//	    lv_obj_set_style_transform_align(albumDis[i].img, LV_ALIGN_CENTER, 0);
	}
	
	/* 如果是从 WeChat 点“图片气泡”进来的：自动定位预览 */
	album_try_jump_to_target();
	
	if (s_album_mode == ALBUM_MODE_WECHAT_PREVIEW && g_img_from_page == PAGE_WECHAT) {
		lv_obj_add_flag(ui_albumPrevBtn, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_albumNextBtn, LV_OBJ_FLAG_HIDDEN);
	} else {
//		lv_obj_clear_flag(ui_albumPrevBtn, LV_OBJ_FLAG_HIDDEN);
//		lv_obj_clear_flag(ui_albumNextBtn, LV_OBJ_FLAG_HIDDEN);
	}
	

}

// 添加状态监控函数
void album_print_status(void)
{
    os_printf("=== Album Status ===\r\n");
    os_printf("Total files: %d\r\n", album_preload_inf.total_fnum);
    os_printf("Loaded count: %d\r\n", album_preload_inf.loaded_count);
    os_printf("Pending updates: %d\r\n", album_preload_inf.pending_updates);
    os_printf("Current index: %d\r\n", album_preload_inf.current_index);
    os_printf("System initialized: %s\r\n", album_preload_inf.system_initialized ? "YES" : "NO");
    os_printf("Preload enabled: %s\r\n", album_preload_inf.preload_enabled ? "YES" : "NO");

    for(int i = 0; i < MAX_THUMBNAILS_PRELOAD && i < album_preload_inf.loaded_count; i++) {
        os_printf("  [%d] %s - %s (addr: 0x%x)\r\n",
               i,
               album_preload_inf.thumbnails[i].filename,
               album_preload_inf.thumbnails[i].is_valid ? "VALID" : "INVALID",
               album_preload_inf.thumbnails[i].rgb_buffer);
    }
    os_printf("==================\r\n");
}

// 紧急重置功能
void album_emergency_reset(void)
{
    os_printf("Emergency album reset triggered\r\n");

    // 停止所有处理
    album_preload_inf.preload_enabled = false;
    album_preload_inf.system_initialized = false;

    // 清空消息队列 - 使用循环读取来清空队列
    while(os_msgq_get(&album_yuv_msgq, 0) != 0) {
        // 继续读取直到队列为空
    }

    // 重置所有状态
    album_preload_inf.pending_updates = 0;
    album_preload_inf.loaded_count = 0;
    album_preload_inf.current_index = 0;

    // 清理缩略图缓存
    for(int i = 0; i < MAX_THUMBNAILS_PRELOAD; i++) {
        album_preload_inf.thumbnails[i].is_valid = false;
        album_preload_inf.thumbnails[i].filename[0] = '\0';
        album_preload_inf.thumbnails[i].file_entry = NULL;
    }

    os_printf("Emergency reset completed\r\n");
}