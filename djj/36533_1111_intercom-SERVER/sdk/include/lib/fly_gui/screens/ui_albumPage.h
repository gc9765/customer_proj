#ifndef UI_ALBUM_PAGE_H
#define UI_ALBUM_PAGE_H

#include "typesdef.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// 预览图片数量
#define MAX_THUMBNAILS_PRELOAD   9       // 改为15张缩略图缓存
#define MAX_ALBUM_FILES          256     // 最大支持的文件数量

typedef struct {
   // struct os_mutex lock;
    lv_img_dsc_t  dsc;
    lv_obj_t * img;
	lv_obj_t * panel;
	lv_obj_t * select_border;  // 选中框对象
} _lv_album_dis;

_lv_album_dis  albumDis[9] ;

// 文件信息结构体（避免使用FileNode指针）
typedef struct {
    char filename[32];      // 文件名（如"JPEG0001.JPG"）
    uint32_t file_size;     // 文件大小
    uint8_t file_type;      // 文件类型 (0=JPEG, 1=AVI，对应playback.h中的枚举值)
    bool is_valid;          // 是否有效
} album_file_info_t;

// 相册操作结果枚举
typedef enum {
    ALBUM_RESULT_SUCCESS = 0,        // 操作成功
    ALBUM_RESULT_NO_FILES,           // 没有找到图片文件
    ALBUM_RESULT_MEM_ERROR,          // 内存分配失败
    ALBUM_RESULT_HARDWARE_ERROR,     // 硬件配置错误
    ALBUM_RESULT_INVALID_PARAM       // 无效参数
} album_result_t;

// 相册显示模式枚举
typedef enum {
    ALBUM_MODE_GRID = 0,    // 网格浏览模式
    ALBUM_MODE_SINGLE = 1,   // 单图浏览模式
} album_display_mode_t;

// 相册状态管理结构体
typedef struct {
    album_display_mode_t mode;        // 当前显示模式
    int8_t grid_selection;            // 网格选中索引
    int8_t single_current;            // 单图当前索引
    bool single_image_loaded;         // 单图是否已加载
} album_state_t;

// 预加载缩略图缓存结构体
typedef struct {
    uint8_t* rgb_buffer;             // RGB565缓冲区指针
    char filename[32];               // 文件名
    bool is_valid;                   // 缓冲区是否有效
    uint32_t file_size;              // 文件大小
    uint32_t load_time;              // 加载时间
    void* file_entry;                // 文件条目指针
} thumbnail_cache_t;


// 相册缩略图智能索引管理 (timer_event中不间断查询索引列表是否需要更新)
typedef struct {
    // === 基础字段保持不变 ===
    thumbnail_cache_t thumbnails[MAX_THUMBNAILS_PRELOAD]; // 15张缩略图缓存
    uint8_t loaded_count;            // 已加载的数量
    uint8_t current_index;           // 当前处理的索引
    uint32_t total_fnum;            // 总文件数
	uint16_t thumbnail_width;        // 缩略图宽度
    uint16_t thumbnail_height;       // 缩略图高度
    bool preload_enabled;            // 预加载开关
    bool scan_completed;             // 扫描完成标志
    bool system_initialized;         // 系统初始化标志

    // === 文件信息数组（替代FileNode链表，避免指针问题）===
    album_file_info_t file_list[MAX_ALBUM_FILES];  // 文件信息数组
    uint8_t file_list_count;         // 实际文件数量
    uint8_t current_file_index;      // 当前处理的文件索引（0~file_list_count-1）

    // === 新增智能索引管理字段 ===

    // 文件变化检测
    uint32_t last_file_count;        // 上次扫描的文件数量
    uint32_t last_file_hash;         // 文件列表哈希值（用于快速检测变化）
    char last_file_names[MAX_THUMBNAILS_PRELOAD][32]; // 缓存的文件名列表（用于比较）

    // 索引映射管理
    uint8_t file_to_cache_map[256];  // 文件索引 → 缓存索引映射 (支持更多文件)
    uint8_t cache_to_file_map[MAX_THUMBNAILS_PRELOAD]; // 缓存索引 → 文件索引映射
    bool cache_validity[MAX_THUMBNAILS_PRELOAD]; // 缓存有效性标记

    // 增量更新控制(SD卡照片有变化时开始更新列表)
    bool incremental_update_enabled; // 增量更新开关
    uint8_t pending_updates;         // 待处理的更新数量
    uint8_t update_queue[MAX_THUMBNAILS_PRELOAD]; // 待更新队列
    uint8_t cleanup_queue[MAX_THUMBNAILS_PRELOAD]; // 待清理队列

    // 预加载专用指针（避免与UI的全局指针冲突）- 已废弃，使用current_file_index替代
    struct FileNode *preload_ptr;    // 预加载专用游标（保留兼容性，但不再使用）

} album_preload_sys_t;



// 相册网格页面生命周期管理API
album_result_t album_page_init(void);
void album_page_deinit(void);
album_result_t album_page_refresh(void);
bool album_page_is_active(void);


// 相册信息查询API
uint32_t album_page_get_file_count(void);
bool album_page_has_files(void);

// 兼容性声明（保持向后兼容）
#ifdef LEGACY_ALBUM_SUPPORT
void preview_files(void);
#endif

// 相册选择功能
void album_select_photo(int8_t index);
void album_move_selection(int8_t direction);

// 状态监控和调试功能
void album_print_status(void);
void album_emergency_reset(void);

#ifdef __cplusplus
}
#endif

#endif // UI_ALBUM_PAGE_H