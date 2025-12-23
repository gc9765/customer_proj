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
#include "stream_frame.h"
#include "keyScan.h"
#include "../lvgl.h"
#include "ui_language.h"
#include "fly_demo.h"
#include "clock_app.h"
#include "syscfg.h"
#include "sonic_process.h"
#include "magic_sound.h"
#include "vpp_ipf_src.h"

#include "lwip/netif.h"
#include "hal/netdev.h"
#include "hal/timer_device.h"
#include "wechat_msg.h"
#include "fatfs/ff.h"   

/* ================== 宏 / 常量 ================== */
#define WECHAT_AVATAR_SIZE      34
#define WECHAT_VOICE_BUBBLE_H   30
#define WECHAT_EMOJI_BUBBLE_H   30
#define WECHAT_PHOTO_BUBBLE_W   160
#define WECHAT_PHOTO_BUBBLE_H   70

#define WECHAT_LOG_FILE         "0:/DCIM/WCTLOG.bin"

#define EMOJI_COL_NUM           4
#define WECHAT_UIQ_SIZE         16
#define WECHAT_MAX_MSG_SLOTS    64
#define WECHAT_RECORD_MAX_SEC   60

/* 长按事件编码（按键线程投递进来） */
#define UI_KEY_EVENT(code, event)   (((code) << 8) | (event))
#define UI_KEY_CALL_LONG_DOWN       UI_KEY_EVENT(KEY_CALL, KEY_EVENT_LDOWN)
#define UI_KEY_CALL_LONG_UP         UI_KEY_EVENT(KEY_CALL, KEY_EVENT_LUP)


extern uint8_t pro_page_cur;
extern uint8_t get_batlevel(void);
extern const lv_img_dsc_t *ui_imgset_iconBat[];
extern void album_set_target_preview(const char *full_path);

extern void wechat_voice_play_from_wav(const char *wav_path);
extern void wechat_voice_stop_play(void);

/* ================== 类型定义 ================== */
typedef enum {
    ALBUM_MODE_NORMAL = 0,
    ALBUM_MODE_WECHAT_PICK,     // 微聊挑图发送
    ALBUM_MODE_WECHAT_PREVIEW,  // 微聊聊天消息预览（只看不发）
} album_mode_t;

typedef enum {
    WECHAT_ACT_INTERCOM = 0,
    WECHAT_ACT_RECORD,
    WECHAT_ACT_EMOJI,
    WECHAT_ACT_CAMERA,
    WECHAT_ACT_PHOTO_PICK,
    WECHAT_ACT_MSG_VIEW_ONLY, // 仅浏览消息（允许）
} wechat_action_t;

typedef enum {
    UIQ_EVT_VOICE = 0,
    UIQ_EVT_PHOTO = 1,
    UIQ_EVT_EMOJI = 2,
} wechat_uiq_evt_t;

typedef struct {
    wechat_uiq_evt_t type;
    wechat_msg_from_t from;
    uint16_t msg_id;
    uint8_t  sec;
    uint16_t emoji_id;
	uint8_t  already_logged; //1=收端已写日志，UI 只显示不再写日志
    char path[64];
} wechat_uiq_item_t;

typedef struct {
    uint8_t  pending;
    uint16_t msg_id;
    uint8_t  sec;
    char     wav_path[64];
} wechat_voice_pending_t;

typedef struct {
    uint8_t pending;
    char    img_path[64];
} wechat_photo_pending_t;

typedef enum {
    WECHAT_FOCUS_BOTTOM = 0,
    WECHAT_FOCUS_MSG    = 1,
} wechat_focus_mode_t;

typedef enum {
    WECHAT_MSG_TYPE_VOICE = 0,
    WECHAT_MSG_TYPE_EMOJI = 1,
    WECHAT_MSG_TYPE_PHOTO = 2,
} wechat_msg_type;

typedef struct {
    uint8_t  type;
    uint8_t  from;
    uint16_t reserved0;
    union {
        struct {
            uint16_t msg_id;
            uint8_t  sec;
            uint8_t  reserved1;
            char     wav_path[64];
        } voice;
        struct {
            uint16_t emoji_id;
            uint16_t reserved2;
            char     reserved_path[64];
        } emoji;
        struct {
            uint16_t reserved3;
            uint16_t reserved4;
            char     img_path[64];
        } photo;
    } u;
} wechat_log_item_t;

typedef struct {
    lv_obj_t          *row;
    lv_obj_t          *bubble;
    wechat_msg_type    type;
    wechat_msg_from_t  from;

    uint16_t           msg_id;
    uint8_t            sec;
    char               wav_path[64];

    uint16_t           emoji_id;
} wechat_msg_slot_t;

/* ================== UI 对象 / 状态变量 ================== */
/* 页面 & 布局对象 */
lv_obj_t *ui_wechatPage;
lv_obj_t *ui_wechatStatusBar;
lv_obj_t *ui_wechatTopBar;
lv_obj_t *ui_wechatMsgArea;
lv_obj_t *ui_wechatBtmBar;

/* 状态栏元素 */
lv_obj_t *ui_wechatWifiDot;
lv_obj_t *ui_wechatBattImg;

/* 底部五个按钮对象 */
lv_obj_t *ui_wechatBtnVideo = NULL;
lv_obj_t *ui_wechatBtnMic   = NULL;
lv_obj_t *ui_wechatBtnEmoji = NULL;
lv_obj_t *ui_wechatBtnCamera= NULL;
lv_obj_t *ui_wechatBtnPhoto = NULL;

static lv_obj_t *s_wechat_btns[5];
static lv_obj_t *s_wechat_btn_labels[5];
uint8_t s_wechat_focus_idx = 0;

static lv_timer_t *ui_pending_message_timer = NULL;

/* emoji 面板 */
static lv_obj_t *ui_emojiPanel = NULL;
static lv_obj_t *s_emoji_btns[EMOJI_COUNT];
static uint8_t   s_emoji_focus_idx = 0;
static uint8_t   s_emoji_panel_visible = 0;

/* 录音状态 */
static uint8_t  wechat_recording = 0;
static uint32_t wechat_press_tick = 0;
static uint32_t wechat_record_start_ms = 0;
static uint32_t wechat_record_sec = 0;

static lv_obj_t   *ui_record_panel = NULL;
static lv_obj_t   *ui_record_arc   = NULL;
static lv_timer_t *ui_record_timer = NULL;

static volatile uint8_t s_req_record_ui_on  = 0;
static volatile uint8_t s_req_record_ui_off = 0;

/* UIQ */
static wechat_uiq_item_t s_uiq[WECHAT_UIQ_SIZE];
static uint8_t s_uiq_head = 0;//指向“下一个要取的”
static uint8_t s_uiq_tail = 0;//指向“下一个要写的”
static struct os_mutex s_uiq_mtx;
static uint8_t s_uiq_inited = 0;

/* pending */
static wechat_voice_pending_t s_wechat_voice_pending;
static wechat_photo_pending_t s_wechat_photo_pending;

/* slots */
static wechat_msg_slot_t s_msg_slots[WECHAT_MAX_MSG_SLOTS];
static uint8_t  s_msg_cnt = 0;
static int16_t  s_wechat_msg_focus_idx = -1;
static uint8_t  s_wechat_focus_mode = WECHAT_FOCUS_BOTTOM;
static wechat_msg_slot_t *s_wechat_playing_slot = NULL;

/* history */
static uint8_t s_wechat_history_loading = 0;
static uint8_t s_wechat_history_loaded  = 0;

/* album restore */
static uint8_t s_wechat_need_restore_after_album = 0;
static char    s_wechat_restore_photo_path[64];
static uint16_t s_wechat_restore_photo_occ = 0;     // 1-based：该 path 在聊天中第几次出现
static int16_t  s_wechat_restore_msg_idx_hint = -1; // 兜底：离开前的 idx

/* 离线弹窗（worker 置位 → UI timer 显示/隐藏） */
static lv_obj_t *s_offline_mask = NULL;
static lv_obj_t *s_offline_panel = NULL;
static lv_timer_t *s_offline_timer = NULL;
static volatile uint8_t s_offline_popup_req = 0;

static volatile uint8_t  s_offline_last_act = 0xFF;
static volatile uint32_t s_offline_block_cnt = 0;

/* emoji 文本表 */
static const char *s_emoji_texts[EMOJI_COUNT] = {
    [WECHAT_EMOJI_SMILE] = "SMILE",
    [WECHAT_EMOJI_GRIN]  = "GRIN",
    [WECHAT_EMOJI_LAUGH] = "LAUGH",
    [WECHAT_EMOJI_ROFL]  = "ROFL",
    [WECHAT_EMOJI_BLUSH] = "BLUSH",
    [WECHAT_EMOJI_LOVE]  = "LOVE",
    [WECHAT_EMOJI_KISS]  = "KISS",
    [WECHAT_EMOJI_COOL]  = "COOL",
    [WECHAT_EMOJI_CRY]   = "CRY",
    [WECHAT_EMOJI_ANGRY] = "ANGRY",
    [WECHAT_EMOJI_THUMB] = "THUMB",
    [WECHAT_EMOJI_PRAY]  = "PRAY",
};

static void wechat_page_deinit(void);
static const char *wechat_basename(const char *path);
static int  wechat_path_equal(const char *a, const char *b);
static lv_coord_t calc_voice_bar_width(int sec, lv_coord_t page_w);
static void wechat_update_msg_focus_style(void);
void wechat_update_focus_style(void);
static void wechat_uiq_init_once(void);
static int  wechat_uiq_push(const wechat_uiq_item_t *it);
static int  wechat_uiq_pop(wechat_uiq_item_t *out);
static void wechat_uiq_drain_in_ui_thread(void);
static void wechat_pending_message_timer_cb(lv_timer_t *t);
static void wechat_msg_drop_oldest_if_full(void);
static wechat_msg_slot_t *wechat_msg_alloc(void);
static void wechat_history_append_item(const wechat_log_item_t *item);
static void wechat_history_append_voice(wechat_msg_from_t from, uint8_t sec, uint16_t msg_id, const char *wav_path);
static void wechat_history_append_emoji(wechat_msg_from_t from, uint16_t emoji_id);
static void wechat_history_append_photo(wechat_msg_from_t from, const char *img_path);
static void wechat_history_load(void);
static void wechat_open_emoji_panel(void);
static void wechat_close_emoji_panel(void);
static void emoji_update_focus_style(void);
static void emoji_panel_on_select(uint8_t idx);
static void wechat_add_emoji_message(wechat_msg_from_t from, uint16_t emoji_id, const char *emoji_text);
static lv_obj_t *wechat_add_voice_message_ui(wechat_msg_from_t from, uint8_t sec, lv_obj_t **out_bubble);
static void wechat_add_voice_message(wechat_msg_from_t from, uint8_t sec);
static void wechat_voice_bubble_event_cb(lv_event_t *e);
static void wechat_stop_current_voice(void);
static void wechat_play_voice_slot(wechat_msg_slot_t *slot);
static lv_obj_t *wechat_add_photo_message_ui(wechat_msg_from_t from, const char *img_path, lv_obj_t **out_bubble);
static void wechat_photo_bubble_event_cb(lv_event_t *e);
static void wechat_record_timer_cb(lv_timer_t *timer);
static void wechat_record_ui_start(void);
static void wechat_record_ui_stop_and_commit(void);
static void wechat_add_voice_message_bound_ex(wechat_msg_from_t from, uint8_t sec, uint16_t msg_id, const char *wav_path, uint8_t do_log);
static void wechat_add_emoji_message_ex(wechat_msg_from_t from, uint16_t emoji_id, const char *emoji_text, uint8_t do_log);										
void wechat_try_restore_after_album(void);


/* 供按键线程/worker/网络线程调用 */
void wechat_request_offline_popup(void)
{
    s_offline_popup_req = 1;
}
void offline_popup_hide_cb(lv_timer_t *t)
{
    (void)t;
    if (s_offline_mask && lv_obj_is_valid(s_offline_mask)) {
        lv_obj_add_flag(s_offline_mask, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_offline_timer) { lv_timer_del(s_offline_timer); s_offline_timer = NULL; }
}

void wechat_show_offline_popup(void)
{
    if (!ui_wechatPage || !lv_obj_is_valid(ui_wechatPage)) return;

    if (!s_offline_mask || !lv_obj_is_valid(s_offline_mask)) {
        s_offline_mask = lv_obj_create(ui_wechatPage);
        lv_obj_set_size(s_offline_mask, lv_obj_get_width(ui_wechatPage), lv_obj_get_height(ui_wechatPage));
        lv_obj_center(s_offline_mask);
        lv_obj_clear_flag(s_offline_mask, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(s_offline_mask, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_offline_mask, LV_OPA_60, 0);
        lv_obj_set_style_border_width(s_offline_mask, 0, 0);
        lv_obj_set_style_pad_all(s_offline_mask, 0, 0);

        s_offline_panel = lv_obj_create(s_offline_mask);
        lv_obj_set_size(s_offline_panel, 180, 80);
        lv_obj_center(s_offline_panel);
        lv_obj_clear_flag(s_offline_panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(s_offline_panel, 10, 0);
        lv_obj_set_style_bg_color(s_offline_panel, lv_color_hex(0x202020), 0);
        lv_obj_set_style_bg_opa(s_offline_panel, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_offline_panel, 2, 0);
        lv_obj_set_style_border_color(s_offline_panel, lv_color_hex(0xFCA702), 0);

        lv_obj_t *lab = lv_label_create(s_offline_panel);
        lv_label_set_text(lab, "CONNECTIONLESS");
        lv_obj_set_style_text_color(lab, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(lab);
    }

    lv_obj_move_foreground(s_offline_mask);
    lv_obj_clear_flag(s_offline_mask, LV_OBJ_FLAG_HIDDEN);

    if (s_offline_timer) { lv_timer_del(s_offline_timer); s_offline_timer = NULL; }
    s_offline_timer = lv_timer_create(offline_popup_hide_cb, 1000, NULL); // 1s自动隐藏
}

static const char *wechat_act_str(wechat_action_t act)
{
    switch (act) {
        case WECHAT_ACT_INTERCOM:     return "INTERCOM";
        case WECHAT_ACT_RECORD:       return "RECORD";
        case WECHAT_ACT_EMOJI:        return "EMOJI";
        case WECHAT_ACT_CAMERA:       return "CAMERA";
        case WECHAT_ACT_PHOTO_PICK:   return "PHOTO_PICK";
        case WECHAT_ACT_MSG_VIEW_ONLY:return "MSG_VIEW_ONLY";
        default:                      return "UNKNOWN";
    }
}

int wechat_guard_action(wechat_action_t act)
{
    uint8_t online = get_wifi_connect_flag();

    if (act == WECHAT_ACT_MSG_VIEW_ONLY) return 1;

    if (!online) {
		s_offline_last_act = (uint8_t)act;
        s_offline_block_cnt++;
		printf("[wechat][offline] BLOCK act=%u(%s) cnt=%lu page=%u focus_idx=%u focus_mode=%u msg_cnt=%u\r\n",
               (unsigned)act, wechat_act_str(act),
               (unsigned long)s_offline_block_cnt,
               (unsigned)camera_gvar.page_cur,
               (unsigned)s_wechat_focus_idx,
               (unsigned)s_wechat_focus_mode,
               (unsigned)s_msg_cnt);
        wechat_request_offline_popup();   // 只置位请求
        return 0;
    }
    return 1;
}

/*********************************/

static void wechat_uiq_init_once(void)
{
    if (s_uiq_inited) return;
    s_uiq_inited = 1;
    os_mutex_init(&s_uiq_mtx);
    s_uiq_head = s_uiq_tail = 0;
}

static int wechat_uiq_push(const wechat_uiq_item_t *it)
{
    if (!it) return -1;
    wechat_uiq_init_once();

    os_mutex_lock(&s_uiq_mtx, -1);

    uint8_t next = (uint8_t)((s_uiq_tail + 1) % WECHAT_UIQ_SIZE);
    if (next == s_uiq_head) {
        os_mutex_unlock(&s_uiq_mtx);
        printf("[wechat][uiq] full, drop type=%d msg_id=%u\n", it->type, it->msg_id);
        return -2;
    }
    s_uiq[s_uiq_tail] = *it;
    s_uiq_tail = next;

    os_mutex_unlock(&s_uiq_mtx);
    return 0;
}

static int wechat_uiq_pop(wechat_uiq_item_t *out)
{
    if (!out) return -1;
    if (!s_uiq_inited) return -2;

    os_mutex_lock(&s_uiq_mtx, -1);

    if (s_uiq_head == s_uiq_tail) {
        os_mutex_unlock(&s_uiq_mtx);
        return -3;
    }

    *out = s_uiq[s_uiq_head];
    s_uiq_head = (uint8_t)((s_uiq_head + 1) % WECHAT_UIQ_SIZE);

    os_mutex_unlock(&s_uiq_mtx);
    return 0;
}

/* ====== UI 是否可以立刻渲染（给接收端判断用）====== */
uint8_t wechat_ui_can_render_now(void)
{
    return (camera_gvar.page_cur == PAGE_WECHAT &&
            ui_wechatPage != NULL &&
            ui_wechatMsgArea != NULL);
}

/* ====== 对外：网络线程/按键线程调用这些，不碰 LVGL ====== */

int wechat_ui_post_voice_ex(wechat_msg_from_t from, uint16_t msg_id, uint8_t sec,
                                  const char *wav_path, uint8_t already_logged)
{
    wechat_uiq_item_t it;
    os_memset(&it, 0, sizeof(it));
    it.type = UIQ_EVT_VOICE;
    it.from = from;
    it.msg_id = msg_id;
    it.sec = sec;
    it.already_logged = already_logged;
    if (wav_path) {
        os_strncpy(it.path, wav_path, sizeof(it.path)-1);
        it.path[sizeof(it.path)-1] = '\0';
    }
    return wechat_uiq_push(&it);
}

int wechat_ui_post_photo_ex(wechat_msg_from_t from, const char *img_path, uint8_t already_logged)
{
	
	static uint32_t g_dbg_ui_post_photo = 0;
	printf("[DBG][photo] ui_post #%u from=%u logged=%u path=%s\r\n",
       ++g_dbg_ui_post_photo, from, already_logged, img_path);

    wechat_uiq_item_t it;
    os_memset(&it, 0, sizeof(it));
    it.type = UIQ_EVT_PHOTO;
    it.from = from;
    it.already_logged = already_logged;
    if (img_path) {
        os_strncpy(it.path, img_path, sizeof(it.path)-1);
        it.path[sizeof(it.path)-1] = '\0';
    }
    return wechat_uiq_push(&it);
}

int wechat_ui_post_emoji_ex(wechat_msg_from_t from, uint16_t emoji_id, uint8_t already_logged)
{
    wechat_uiq_item_t it;
    os_memset(&it, 0, sizeof(it));
    it.type = UIQ_EVT_EMOJI;
    it.from = from;
    it.emoji_id = emoji_id;
    it.already_logged = already_logged;
    return wechat_uiq_push(&it);
}


static uint16_t wechat_calc_photo_occ_upto(int16_t idx, const char *path)
{
    if (!path || !path[0] || idx < 0) return 0;

    uint16_t occ = 0;
    uint8_t cnt = (s_msg_cnt > WECHAT_MAX_MSG_SLOTS) ? WECHAT_MAX_MSG_SLOTS : s_msg_cnt;
    if (idx >= (int16_t)cnt) idx = (int16_t)cnt - 1;

    for (int16_t i = 0; i <= idx; i++) {
        if (s_msg_slots[i].type == WECHAT_MSG_TYPE_PHOTO &&
            wechat_path_equal(s_msg_slots[i].wav_path, path)) {
            occ++;
        }
    }
    return occ;
}

static void wechat_prepare_restore_for_photo_slot(wechat_msg_slot_t *slot)
{
    if (!slot || slot->type != WECHAT_MSG_TYPE_PHOTO) return;

    int16_t idx = (int16_t)(slot - s_msg_slots);
    if (idx < 0 || idx >= (int16_t)s_msg_cnt) {
        // idx 计算异常时，简单兜底：不算 occurrence
        idx = -1;
    }

    s_wechat_need_restore_after_album = 1;

    os_strncpy(s_wechat_restore_photo_path, slot->wav_path,
               sizeof(s_wechat_restore_photo_path) - 1);
    s_wechat_restore_photo_path[sizeof(s_wechat_restore_photo_path) - 1] = '\0';

    s_wechat_restore_photo_occ = wechat_calc_photo_occ_upto(idx, s_wechat_restore_photo_path);
    s_wechat_restore_msg_idx_hint = idx;

    printf("[wechat][restore] prepare path=%s idx=%d occ=%u\r\n",
           s_wechat_restore_photo_path, (int)idx, (unsigned)s_wechat_restore_photo_occ);
}

/*********************************/


/* 本端统一入口：写 log + 投递 UIQ（不直接操作 LVGL）*/
static void wechat_local_show_voice(uint16_t msg_id, uint8_t sec, const char *wav_path)
{
    if (sec == 0) sec = 1;
    if (sec > WECHAT_RECORD_MAX_SEC) sec = WECHAT_RECORD_MAX_SEC;

    /* 1) 写日志 */
    wechat_history_append_voice_public(WEICHAT_MSG_FROM_ME, sec, msg_id, wav_path);

    /* 2) 再投递UI（UI线程里创建气泡） */
    (void)wechat_ui_post_voice_ex(WEICHAT_MSG_FROM_ME, msg_id, sec, wav_path, 1);
}

static void wechat_local_show_photo(const char *img_path)
{	
	static uint32_t g_dbg_local_show_photo = 0;
	printf("[DBG][photo] local_show #%u path=%s\r\n", ++g_dbg_local_show_photo, img_path);

    if (!img_path || !img_path[0]) return;

    wechat_history_append_photo_public(WEICHAT_MSG_FROM_ME, img_path);//写日志
	if (wechat_ui_can_render_now()) {
        (void)wechat_ui_post_photo_ex(WEICHAT_MSG_FROM_ME, img_path, 1);
    } else {
        printf("[wechat] local photo saved, skip ui_post (not in wechat page)\r\n");
    }
}

static void wechat_local_show_emoji(uint16_t emoji_id)
{
    if (emoji_id >= EMOJI_COUNT) return;

    wechat_history_append_emoji_public(WEICHAT_MSG_FROM_ME, emoji_id);  // 写日志 WCTLOG.bin
    (void)wechat_ui_post_emoji_ex(WEICHAT_MSG_FROM_ME, emoji_id, 1);	// 投递 UIQ（already_logged=1）
}



static void wechat_page_deinit(void)
{
	
	/* ===== 离线弹窗清理（防止 1s hide timer 在页面销毁后回调） ===== */
    s_offline_popup_req = 0;

    if (s_offline_timer) {
        lv_timer_del(s_offline_timer);
        s_offline_timer = NULL;
    }

    if (s_offline_mask && lv_obj_is_valid(s_offline_mask)) {
        lv_obj_del(s_offline_mask);      // mask 里包含 panel
    }
    s_offline_mask = NULL;
    s_offline_panel = NULL;
    if (ui_pending_message_timer) {
        lv_timer_del(ui_pending_message_timer);
        ui_pending_message_timer = NULL;
    }
    if (ui_record_timer) {
        lv_timer_del(ui_record_timer);
        ui_record_timer = NULL;
    }

    ui_record_panel = NULL;
    ui_record_arc   = NULL;

    ui_emojiPanel = NULL;

    ui_wechatMsgArea   = NULL;
    ui_wechatTopBar    = NULL;
    ui_wechatStatusBar = NULL;
    ui_wechatBtmBar    = NULL;
    ui_wechatWifiDot   = NULL;
    ui_wechatBattImg   = NULL;
    ui_wechatPage      = NULL;

    s_emoji_panel_visible = 0;
    wechat_recording = 0;
    s_wechat_playing_slot = NULL;
}

static void wechat_page_on_delete(lv_event_t *e)
{
    (void)e;

    /* 防止按键线程继续往已删页面发事件 */
    if (curPage_obj == ui_wechatPage) {
        curPage_obj = NULL;
    }

    /* 统一释放 timer/清所有 UI 指针 */
    wechat_page_deinit();

    /* 额外：把 emoji btn 缓存也清掉 */
    for (int i = 0; i < EMOJI_COUNT; i++) s_emoji_btns[i] = NULL;

    printf("[wechat] page deleted -> deinit done\r\n");
}


static const char *wechat_basename(const char *path)
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



static void wechat_update_msg_focus_style(void)
{
    for (uint8_t i = 0; i < s_msg_cnt && i < WECHAT_MAX_MSG_SLOTS; i++) {
        wechat_msg_slot_t *slot = &s_msg_slots[i];
        if (!slot->bubble) continue;

        if ((int16_t)i == s_wechat_msg_focus_idx &&
            s_wechat_focus_mode == WECHAT_FOCUS_MSG) {

            /* 只在气泡上画边框，不改 row 的背景 */
            lv_obj_set_style_border_width(slot->bubble, 2, 0);
            lv_obj_set_style_border_color(slot->bubble, lv_color_hex(0xFCA702), 0);
            lv_obj_set_style_radius(slot->bubble, 6, 0);
        } else {
            lv_obj_set_style_border_width(slot->bubble, 0, 0);
        }
    }
}

void wechat_record_ui_request(uint8_t on)
{
    if (on)  s_req_record_ui_on  = 1;
    else     s_req_record_ui_off = 1;
}


/* 轮询 UI 队列：把网络线程/本端业务投递过来的事件转成气泡（UI线程安全） */
static void wechat_uiq_drain_in_ui_thread(void)
{
	if (!ui_wechatMsgArea) return;
    wechat_uiq_item_t it;
    while (wechat_uiq_pop(&it) == 0) {
        if (it.type == UIQ_EVT_VOICE) {
            wechat_add_voice_message_bound_ex(it.from, it.sec, it.msg_id, it.path,
                                              it.already_logged ? 0 : 1);
        } else if (it.type == UIQ_EVT_PHOTO) {
            wechat_add_photo_message_bound_ex(it.from, it.path,
                                              it.already_logged ? 0 : 1);
        } else if (it.type == UIQ_EVT_EMOJI) {
            if (it.emoji_id < EMOJI_COUNT) {
                wechat_add_emoji_message_ex(it.from, it.emoji_id,
                                           s_emoji_texts[it.emoji_id],
                                           it.already_logged ? 0 : 1);
            }
        }
    }
}



/* 轮询 pending 的消息气泡，有就创建 UI */
static void wechat_pending_message_timer_cb(lv_timer_t *t)
{
    (void)t;

    if (camera_gvar.page_cur != PAGE_WECHAT) return;
    if (!ui_wechatPage || !ui_wechatMsgArea) return;
    if (!lv_obj_is_valid(ui_wechatPage) || !lv_obj_is_valid(ui_wechatMsgArea)) return;

    {
        static uint8_t last_conn = 0xFF;   // 0xFF 表示“未初始化”
        uint8_t cur_conn = get_wifi_connect_flag(); // 你的 sys_status.wifi_connected

        if (cur_conn != last_conn) {
            last_conn = cur_conn;
            wechat_update_wifi_status(cur_conn);    // 绿/灰点
            printf("[wechat] wifi conn change -> %u\r\n", (unsigned)cur_conn);
        }
    }

    /* 1) UI线程消费队列：网络线程/本端业务线程投递的都从这里进聊天框 */
    wechat_uiq_drain_in_ui_thread();
	
	if (s_offline_popup_req) {
		s_offline_popup_req = 0;

		/* 保险：只有离线才弹（避免刚连上还弹一下） */
		if (!get_wifi_connect_flag()) {
			wechat_show_offline_popup();
		}
	}
	
    /* 2) 录音 UI 请求（只在 UI 线程里真正操作 LVGL） */
    if (s_req_record_ui_on)  { s_req_record_ui_on  = 0; wechat_record_ui_start(); }
    if (s_req_record_ui_off) { s_req_record_ui_off = 0; wechat_record_ui_stop_and_commit(); }

    /* 3) 从相册返回后的焦点恢复：只在需要时跑一次 */
    wechat_try_restore_after_album();
}

static void wechat_msg_drop_oldest_if_full(void)
{
    if (s_msg_cnt < WECHAT_MAX_MSG_SLOTS) return;

    // 1) 删掉最旧那条对应的 LVGL 对象（整行 row）
    if (s_msg_slots[0].row) {
        lv_obj_del(s_msg_slots[0].row);
    }

    // 2) 左移 slot 表
    os_memmove(&s_msg_slots[0], &s_msg_slots[1],
               sizeof(wechat_msg_slot_t) * (WECHAT_MAX_MSG_SLOTS - 1));
    os_memset(&s_msg_slots[WECHAT_MAX_MSG_SLOTS - 1], 0, sizeof(wechat_msg_slot_t));

    // 3) 修正焦点索引（如果在消息模式）
    if (s_wechat_msg_focus_idx >= 0) {
        s_wechat_msg_focus_idx--;
        if (s_wechat_msg_focus_idx < 0) s_wechat_msg_focus_idx = 0;
    }

    // 4) 当前条数保持最大
    s_msg_cnt = WECHAT_MAX_MSG_SLOTS - 1;
}

static wechat_msg_slot_t *wechat_msg_alloc(void)
{
    wechat_msg_drop_oldest_if_full();

    wechat_msg_slot_t *slot = &s_msg_slots[s_msg_cnt];
    os_memset(slot, 0, sizeof(*slot));
    s_msg_cnt++;
    return slot;
}

static void wechat_history_append_item(const wechat_log_item_t *item)
{
    if (!item) return;
    if (s_wechat_history_loading) {
        printf("skip append while loading");
        return;
    }

    printf("append_item enter: type=%u from=%u",
                   (unsigned)item->type, (unsigned)item->from);

    /* 1) 直接用 FatFs 标志以 读写+存在则打开/不存在则创建 的方式打开 */
    F_FILE *fp = osal_open(WECHAT_LOG_FILE, 0,
                           FA_READ | FA_WRITE | FA_OPEN_ALWAYS);
    if (!fp) {
        printf("osal_open '%s' FAIL", WECHAT_LOG_FILE);
        return;
    }
    printf("open log RW+OK: %s", WECHAT_LOG_FILE);

    /* 2) 显式 seek 到文件末尾，做到“追加”效果 */
    uint32_t cur_size = osal_fsize(fp);   // 用 OSAL 封装的 f_size
    uint32_t seek_res = osal_fseek(fp, cur_size);
    if (seek_res != 0) {  // osal_fseek 返回 f_lseek 的结果，FR_OK == 0
        printf("osal_fseek to end fail, res=%u", (unsigned)seek_res);
        osal_fclose(fp);
        return;
    }
    printf("append at offset=%u", (unsigned)cur_size);

    /* 3) 写入一条 log item */
    const uint32_t expect = sizeof(wechat_log_item_t);

    uint32_t w = osal_fwrite((void *)item, 1, expect, fp);
    printf("fwrite ret=%u, expect=%u", (unsigned)w, (unsigned)expect);

    if (w != expect) {
        printf("write log FAIL, bytes=%u", (unsigned)w);
    }

    osal_fclose(fp);
}



//追加一条“语音消息”日志
static void wechat_history_append_voice(wechat_msg_from_t from,
                                        uint8_t           sec,
                                        uint16_t          msg_id,
                                        const char       *wav_path)
{
    if (s_wechat_history_loading) return;  // 正在加载历史时不写 

    wechat_log_item_t item;
    os_memset(&item, 0, sizeof(item));

    item.type = WECHAT_MSG_TYPE_VOICE;
    item.from = (uint8_t)from;
    item.u.voice.msg_id = msg_id;
    item.u.voice.sec    = sec;

    if (wav_path) {
        os_strncpy(item.u.voice.wav_path,
                   wav_path,
                   sizeof(item.u.voice.wav_path) - 1);
        item.u.voice.wav_path[sizeof(item.u.voice.wav_path) - 1] = '\0';
    }

    printf("append_voice: from=%u sec=%u msg_id=%u path=%s",
                   (unsigned)from, (unsigned)sec,
                   (unsigned)msg_id,
                   item.u.voice.wav_path);

    wechat_history_append_item(&item);
}

//追加一条“表情消息”日志
static void wechat_history_append_emoji(wechat_msg_from_t from,
                                        uint16_t          emoji_id)
{
    if (s_wechat_history_loading) return;

    wechat_log_item_t item;
    os_memset(&item, 0, sizeof(item));

    item.type = WECHAT_MSG_TYPE_EMOJI;
    item.from = (uint8_t)from;
    item.u.emoji.emoji_id = emoji_id;

    printf("append_emoji: from=%u emoji_id=%u",
                   (unsigned)from, (unsigned)emoji_id);

    wechat_history_append_item(&item);
}


static void wechat_history_append_photo(wechat_msg_from_t from, const char *img_path)
{
    if (s_wechat_history_loading) return;

    wechat_log_item_t item;
    os_memset(&item, 0, sizeof(item));

    item.type = WECHAT_MSG_TYPE_PHOTO;
    item.from = (uint8_t)from;

    if (img_path) {
        os_strncpy(item.u.photo.img_path, img_path, sizeof(item.u.photo.img_path) - 1);
        item.u.photo.img_path[sizeof(item.u.photo.img_path) - 1] = '\0';
    }

    printf("append_photo: from=%u path=%s",
           (unsigned)from, item.u.photo.img_path);

    wechat_history_append_item(&item);
}


/* 被按键线程/业务线程调用：本端消息统一走 UIQ，不再用 pending 变量 */
void wechat_set_pending_voice_bubble(uint16_t msg_id,
                                     uint8_t  sec,
                                     const char *wav_path)
{
    printf("[wechat] local voice commit: msg_id=%u sec=%u file=%s\r\n",
           msg_id, sec, wav_path ? wav_path : "");

    wechat_local_show_voice(msg_id, sec, wav_path);
}

void wechat_set_pending_photo_bubble(const char *img_path)
{
    static uint32_t g_dbg_local_photo_commit = 0;
	printf("[DBG][photo] commit #%u path=%s\r\n", ++g_dbg_local_photo_commit, img_path);
    wechat_local_show_photo(img_path);
}

/*------------------------------------------------
 *  工具函数
 *----------------------------------------------*/

/* 计算语音条宽度：sec 秒 -> 页面宽度 20%~80% */
/* 计算语音条宽度：sec 秒 -> 在可用宽度里线性变化，避免越界 */
/* 计算语音条宽度：
 * 要求：60s ≈ 一行可用宽度的 80%，短语音不要太短
 */
static lv_coord_t calc_voice_bar_width(int sec, lv_coord_t page_w)
{
    const int max_sec = 60;

    if (sec < 1)       sec = 1;
    if (sec > max_sec) sec = max_sec;

    /* 预留头像和两边 padding，避免越界、顶边 */
    const lv_coord_t side_pad   = 4 * 2;   // row 左右 pad_left/pad_right≈4
    const lv_coord_t avatar_w   = 32;      // 头像宽度
    const lv_coord_t extra_gap  = 8;       // 气泡与头像/文本的间距

    /* 一行里真正给语音气泡可用的总宽度 */
    lv_coord_t usable_w = page_w - side_pad - avatar_w - extra_gap;
    if (usable_w < 60) usable_w = 60;      // 兜底，防止负数或太小

    /* 
     * 约束：
     *   1s  ≈ 30% usable_w
     *   60s ≈ 80% usable_w
     */
    lv_coord_t max_bubble_w = usable_w * 80 / 100;  // 80%
    lv_coord_t min_bubble_w = usable_w * 20 / 100;  // 10%

    if (max_bubble_w < min_bubble_w + 10) {
        // 极端小屏幕时兜底，至少保证一定差值
        max_bubble_w = min_bubble_w + 10;
    }

    /* 对 1~60 秒做线性插值：sec = 1 -> min, sec = 60 -> max */
    lv_coord_t w = min_bubble_w;
    if (max_sec > 1) {
        w = min_bubble_w +
            (max_bubble_w - min_bubble_w) * (sec - 1) / (max_sec - 1);
    }

    /* 再保护一次范围 */
    if (w < min_bubble_w)  w = min_bubble_w;
    if (w > max_bubble_w)  w = max_bubble_w;

    return w;
}



/*------------------------------------------------
 *  聊天内容区：新增一条“语音消息”
 *----------------------------------------------*/
/* 仅负责创建 UI，不做绑定；返回 row 对象 */
static lv_obj_t *wechat_add_voice_message_ui(wechat_msg_from_t from,
                                             uint8_t           sec,
                                             lv_obj_t        **out_bubble)

{
    if (!ui_wechatMsgArea) return NULL;
    if (sec == 0) sec = 1;
    if (sec > WECHAT_RECORD_MAX_SEC) sec = WECHAT_RECORD_MAX_SEC;

    int page_w = lv_obj_get_width(ui_wechatPage);
    lv_coord_t bar_w = calc_voice_bar_width(sec, page_w);

    /* 一行容器 */
    lv_obj_t *row = lv_obj_create(ui_wechatMsgArea);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_bg_color(row, lv_color_hex(0x101018), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);

    lv_obj_set_style_pad_top(row, 0, 0);
    lv_obj_set_style_pad_bottom(row, 0, 0);
    lv_obj_set_style_pad_left(row, 4, 0);
    lv_obj_set_style_pad_right(row, 4, 0);
    lv_obj_set_style_radius(row, 0, 0);

    lv_obj_set_flex_grow(row, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

    char sec_txt[8];
    snprintf(sec_txt, sizeof(sec_txt), "%u\"", (unsigned)sec);

    if (from == WEICHAT_MSG_FROM_PEER) {
        /* 对方语音：整行靠左 */
        lv_obj_set_flex_align(row,
                              LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        /* 左侧头像 */
        lv_obj_t *avatar = lv_obj_create(row);
        lv_obj_set_size(avatar, WECHAT_AVATAR_SIZE, WECHAT_AVATAR_SIZE);
        lv_obj_set_style_radius(avatar, 4, 0);
        lv_obj_set_style_bg_color(avatar, lv_color_hex(0x3A6EA5), 0);
        lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(avatar, 0, 0);
        lv_obj_clear_flag(avatar, LV_OBJ_FLAG_SCROLLABLE);

        /* 语音气泡 */
        lv_obj_t *bubble = lv_obj_create(row);
        lv_obj_set_size(bubble, bar_w, WECHAT_VOICE_BUBBLE_H);
        lv_obj_set_style_radius(bubble, 6, 0);
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0x2C2C34), 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bubble, 0, 0);
        lv_obj_set_style_pad_left(bubble, 6, 0);
        lv_obj_set_style_pad_right(bubble, 6, 0);
        lv_obj_set_style_pad_top(bubble, 4, 0);
        lv_obj_set_style_pad_bottom(bubble, 4, 0);
        lv_obj_set_style_clip_corner(bubble, true, 0);

        lv_obj_t *len_label = lv_label_create(bubble);
        lv_label_set_text(len_label, sec_txt);
        lv_obj_set_style_text_color(len_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(len_label, LV_ALIGN_RIGHT_MID, -2, 0);
		if (out_bubble) *out_bubble = bubble;

    } else {
        /* 自己语音：整行靠右 */
        lv_obj_set_flex_align(row,
                              LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        lv_obj_t *bubble = lv_obj_create(row);
        lv_obj_set_size(bubble, bar_w, WECHAT_VOICE_BUBBLE_H);
        lv_obj_set_style_radius(bubble, 6, 0);
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0x4AA1FF), 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bubble, 0, 0);
        lv_obj_set_style_pad_left(bubble, 6, 0);
        lv_obj_set_style_pad_right(bubble, 6, 0);
        lv_obj_set_style_pad_top(bubble, 4, 0);
        lv_obj_set_style_pad_bottom(bubble, 4, 0);
        lv_obj_set_style_clip_corner(bubble, true, 0);

        lv_obj_t *len_label = lv_label_create(bubble);
        lv_label_set_text(len_label, sec_txt);
        lv_obj_set_style_text_color(len_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(len_label, LV_ALIGN_LEFT_MID, 2, 0);
		
		if (out_bubble) *out_bubble = bubble;
		
        lv_obj_t *avatar = lv_obj_create(row);
        lv_obj_set_size(avatar, WECHAT_AVATAR_SIZE, WECHAT_AVATAR_SIZE);
        lv_obj_set_style_radius(avatar, 4, 0);
        lv_obj_set_style_bg_color(avatar, lv_color_hex(0xF5A623), 0);
        lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(avatar, 0, 0);
        lv_obj_clear_flag(avatar, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* 滚动到底部 */
    lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    
	if (out_bubble && *out_bubble == NULL) {
        // 兜底：理论不会进来
        printf("[wechat] warn: voice bubble ptr is NULL\n");
    }
    return row;
}

/* 老接口：仅创建 UI，不做绑定 */
static void wechat_add_voice_message(wechat_msg_from_t from, uint8_t sec)
{
    (void)wechat_add_voice_message_ui(from, sec, NULL);
}

/* 停止当前正在播放的语音（如果有） */
static void wechat_stop_current_voice(void)
{
    if (s_wechat_playing_slot) {
        /* 底层停止当前语音播放 */
        wechat_voice_stop_play();
        s_wechat_playing_slot = NULL;
        printf("[wechat] stop current voice\n");
    }
}

/* 播放某个语音 slot，自动处理“切歌 / 同一条停止”逻辑 */
static void wechat_play_voice_slot(wechat_msg_slot_t *slot)
{
	
	
    if (!slot) return;
    if (slot->type != WECHAT_MSG_TYPE_VOICE) return;
    if (slot->wav_path[0] == '\0') {
        printf("[wechat] voice slot has no wav_path\n");
        return;
    }

    /* 如果点的是当前正在播的那一条 → 直接停止，相当于“暂停/停止” */
    if (s_wechat_playing_slot == slot) {
        printf("[wechat] click same voice bubble, stop it\n");
        wechat_stop_current_voice();
        return;
    }

    /* 切换到新的：先把旧的停掉，再播新的 */
    if (s_wechat_playing_slot) {
        printf("[wechat] switch voice: stop old then play new\n");
        wechat_stop_current_voice();
    }

    printf("[wechat] play voice: msg_id=%u, sec=%u, file=%s\n",
           slot->msg_id, slot->sec, slot->wav_path);
    wechat_voice_play_from_wav(slot->wav_path);
    s_wechat_playing_slot = slot;
}


/* 点击语音气泡时的事件回调 */
static void wechat_voice_bubble_event_cb(lv_event_t *e)
{
    wechat_msg_slot_t *slot =
        (wechat_msg_slot_t *)lv_event_get_user_data(e);

    if (!slot) return;

    printf("[wechat] voice bubble clicked: msg_id=%u, sec=%u, file=%s\r\n",
           slot->msg_id, slot->sec, slot->wav_path);

    // TODO: 播放接口
//    wechat_voice_play_from_wav(slot->wav_path);
	wechat_play_voice_slot(slot);
}


/* 创建“带绑定”的语音气泡：UI + 填充绑定表 + 安装事件回调 */
/* 对外接口：创建“带绑定信息”的语音气泡 */
static void wechat_add_voice_message_bound_ex(wechat_msg_from_t from,
                                             uint8_t sec, uint16_t msg_id,
                                             const char *wav_path,
                                             uint8_t do_log)
{
    lv_obj_t *bubble = NULL;
    lv_obj_t *row = wechat_add_voice_message_ui(from, sec, &bubble);
    if (!row || !bubble) return;

    wechat_msg_slot_t *slot = wechat_msg_alloc();
    slot->row = row;
    slot->bubble = bubble;
    slot->type = WECHAT_MSG_TYPE_VOICE;
    slot->from = from;
    slot->msg_id = msg_id;
    slot->sec = sec;

    if (wav_path) {
        os_strncpy(slot->wav_path, wav_path, sizeof(slot->wav_path)-1);
        slot->wav_path[sizeof(slot->wav_path)-1] = '\0';
    } else {
        slot->wav_path[0] = '\0';
    }

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, wechat_voice_bubble_event_cb, LV_EVENT_CLICKED, (void *)slot);

    wechat_update_msg_focus_style();

    /* ⭐ 只有需要且不是 history_load 才写日志 */
    if (do_log && !s_wechat_history_loading) {
        wechat_history_append_voice(from, sec, msg_id, slot->wav_path);
    }
}

void wechat_add_voice_message_bound(wechat_msg_from_t from,
                                   uint8_t sec,
                                   uint16_t msg_id,
                                   const char *wav_path)
{
    wechat_add_voice_message_bound_ex(from, sec, msg_id, wav_path, 1);
}



/*------------------------------------------------
 *  录音 UI：浮层 + 进度条 + 秒数
 *----------------------------------------------*/

/* 录音进度定时器回调（LVGL 定时器） */
static void wechat_record_timer_cb(lv_timer_t *timer)
{
    if (!wechat_recording) return;
    if (!ui_record_panel)  return;

    uint32 now     = os_jiffies();
    uint32 diff_ms = now - wechat_record_start_ms;

    /* ① 用 0.02 秒一个刻度更新圆圈（更快更顺） */
    const uint32 step_ms   = 20;                                   // 20ms 一步
    uint32 units           = diff_ms / step_ms;                    // 0.02s 为单位
    uint32 max_units       = WECHAT_RECORD_MAX_SEC * (1000/step_ms); // 60s → 60*50 = 3000

    if (units > max_units) units = max_units;

    if (ui_record_arc) {
        lv_arc_set_value(ui_record_arc, (int)units);
    }

    /* ② 秒数还是按 1s 统计，用来做超时保护 */
    uint32 sec = diff_ms / 1000;
    if (sec > WECHAT_RECORD_MAX_SEC) sec = WECHAT_RECORD_MAX_SEC;

    if (sec != wechat_record_sec) {
        wechat_record_sec = sec;
        // 如果以后想加数字显示，也可以在这里更新 label
    }

    /* ③ 超时自动停止录音 */
    if (wechat_record_sec >= WECHAT_RECORD_MAX_SEC) {
        wechat_recording = 0;
        if (ui_record_timer) lv_timer_pause(ui_record_timer);
        if (ui_record_panel) lv_obj_add_flag(ui_record_panel, LV_OBJ_FLAG_HIDDEN);

        wechat_add_voice_message(WEICHAT_MSG_FROM_ME,
                                 (uint8_t)wechat_record_sec);
    }
}


static inline lv_obj_t *lv_safe_obj(lv_obj_t **p)
{
    if (*p && !lv_obj_is_valid(*p)) *p = NULL;
    return *p;
}

static int wechat_ui_ready(void)
{
    if (camera_gvar.page_cur != PAGE_WECHAT) return 0;
    if (!lv_safe_obj(&ui_wechatPage)) return 0;

    for (int i = 0; i < 5; i++) {
        if (!lv_safe_obj(&s_wechat_btns[i])) return 0;
        if (!lv_safe_obj(&s_wechat_btn_labels[i])) return 0;
    }
    return 1;
}

/* 开始录音：在 KEY_CALL 长按按下的时候调用 */
static void wechat_record_ui_start(void)
{
    if (wechat_recording) return;  // 已经在录了
    if (!lv_safe_obj(&ui_wechatPage)) return;

    if (ui_record_panel && !lv_obj_is_valid(ui_record_panel)) ui_record_panel = NULL;
    if (ui_record_arc   && !lv_obj_is_valid(ui_record_arc))   ui_record_arc   = NULL;
    wechat_recording       = 1;
    wechat_record_start_ms = os_jiffies();
    wechat_record_sec      = 0;

    if (!ui_record_panel) {
		/* 1. 全屏透明容器，只用来居中圆圈 */
		ui_record_panel = lv_obj_create(ui_wechatPage);
		lv_obj_set_size(ui_record_panel,
						lv_obj_get_width(ui_wechatPage),
						lv_obj_get_height(ui_wechatPage));
		lv_obj_align(ui_record_panel, LV_ALIGN_CENTER, 0, 0);

		lv_obj_clear_flag(ui_record_panel, LV_OBJ_FLAG_SCROLLABLE);
		/* 半透明黑色遮罩，让录音状态更突出 */
		lv_obj_set_style_bg_color(ui_record_panel, lv_color_hex(0x000000), 0);
		lv_obj_set_style_bg_opa(ui_record_panel, LV_OPA_60, 0);   // 60% 透明度
		lv_obj_set_style_border_width(ui_record_panel, 0, 0);
		lv_obj_set_style_pad_all(ui_record_panel, 0, 0);

		/* 2. 圆形进度条本体 */
		ui_record_arc = lv_arc_create(ui_record_panel);
		lv_obj_set_size(ui_record_arc, 72, 72);  // 大小你可以再微调
		lv_obj_align(ui_record_arc, LV_ALIGN_CENTER, 0, 20);  // 往下 20 像素

		lv_arc_set_bg_angles(ui_record_arc, 0, 360);
		lv_arc_set_rotation(ui_record_arc, 270);   // 从顶部开始

		/* 用 0.1 秒为单位：范围 0 ～ 60*10 = 600 */
		lv_arc_set_range(ui_record_arc, 0, WECHAT_RECORD_MAX_SEC * (1000/20));
		lv_arc_set_value(ui_record_arc, 0);

		/* 背景弧隐藏 */
		lv_obj_set_style_arc_opa(ui_record_arc, LV_OPA_0, LV_PART_MAIN);

		/* 指示弧改成细红圈，更突出录音 */
		lv_obj_set_style_arc_width(ui_record_arc, 4, LV_PART_INDICATOR);
		lv_obj_set_style_arc_color(ui_record_arc, lv_color_hex(0xFF4444), LV_PART_INDICATOR);

		/* 去掉中间小球 */
		lv_obj_set_style_opa(ui_record_arc, LV_OPA_0, LV_PART_KNOB);
		lv_obj_set_style_bg_opa(ui_record_arc, LV_OPA_0, LV_PART_KNOB);
		lv_obj_set_style_border_width(ui_record_arc, 0, LV_PART_KNOB);

		/* 不可点击 */
		lv_obj_clear_flag(ui_record_arc, LV_OBJ_FLAG_CLICKABLE);

    }

    /* 显示浮层并重置进度 */
    lv_obj_clear_flag(ui_record_panel, LV_OBJ_FLAG_HIDDEN);
    if (ui_record_arc) lv_arc_set_value(ui_record_arc, 0);

    /* 创建/恢复定时器 */
    if (!ui_record_timer) {
        ui_record_timer = lv_timer_create(wechat_record_timer_cb, 20, NULL);
    } else {
        lv_timer_resume(ui_record_timer);
    }

//	wechat_audio_record_start();
    printf("[wechat] record start\n");
}



/* 结束录音并插入语音消息：KEY_CALL 长按释放时调用 */
static void wechat_record_ui_stop_and_commit(void)
{
    if (!wechat_recording) return;

    wechat_recording = 0;
    if (ui_record_timer) {
        lv_timer_pause(ui_record_timer);
    }

    if (ui_record_panel) {
        lv_obj_add_flag(ui_record_panel, LV_OBJ_FLAG_HIDDEN);
    }

    printf("[wechat] record ui stop, sec=%u\n", (unsigned)wechat_record_sec);
}



/* 焦点/非焦点样式更新函数（底部 5 个按钮） */
void wechat_update_focus_style(void)
{
	if (!wechat_ui_ready()) {
        printf("[wechat] focus_style skip (ui not ready) cur=%u\n", (unsigned)camera_gvar.page_cur);
        return;
    }
	
    for (int i = 0; i < 5; i++) {
        lv_obj_t *btn   = s_wechat_btns[i];
        lv_obj_t *label = s_wechat_btn_labels[i];
        if (!btn || !label) continue;

        if (i == s_wechat_focus_idx) {
            /* 焦点：白底 / 橙边 / 图标黑色 */
            lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(btn,  LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(btn, 2, 0);
            lv_obj_set_style_border_color(btn, lv_color_hex(0xFCA702), 0);
            lv_obj_set_style_text_color(label, lv_color_hex(0x000000), 0);
        } else {
            /* 非焦点：深灰背景 / 无边框 / 白色图标 */
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), 0);
            lv_obj_set_style_bg_opa(btn,  LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(btn, 0, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        }
    }
}

/* WiFi 状态更新：connected=1 红点；connected=0 灰点 */


void wechat_update_wifi_status(uint8_t connected)
{
    if (!ui_wechatWifiDot) return;

    lv_color_t c = connected
                   ? lv_color_hex(0x44FF44)   // 绿色：已连接
                   : lv_color_hex(0x666666);  // 灰色：未连接

    lv_obj_set_style_bg_color(ui_wechatWifiDot, c, 0);
}


/* 电量更新：0~100 -> 只更新电池图标（3 档） */
void wechat_update_batt_percent(int percent)
{
    if (!ui_wechatBattImg) {
        return; // UI 还没创建好
    }

    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;

    /* 3 档图标：0、1、2 */
    uint8_t level = 0;  // 对应 ui_imgset_iconBat[0..2]

    if (percent > 66)
        level = 2;      // 高电量
    else if (percent > 33)
        level = 1;      // 中电量
    else
        level = 0;      // 低电量

    lv_img_set_src(ui_wechatBattImg, ui_imgset_iconBat[level]);
}



/* 在聊天内容区域新增一条“表情消息”
 * from = WEICHAT_MSG_FROM_PEER：左侧；WEICHAT_MSG_FROM_ME：右侧
 * emoji_text = 要显示的表情字符串，比如 "😊"
 */
static void wechat_add_emoji_message_ex(wechat_msg_from_t from,
                                       uint16_t emoji_id,
                                       const char *emoji_text,
                                       uint8_t do_log)
{
    if (!ui_wechatMsgArea || !emoji_text) return;

    lv_obj_t *row = lv_obj_create(ui_wechatMsgArea);
	/* 新增：把 row 默认上下 padding 清掉 */
	lv_obj_set_style_pad_top(row, 0, 0);
	lv_obj_set_style_pad_bottom(row, 0, 0);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x101018), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_left(row, 4, 0);
    lv_obj_set_style_pad_right(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

    lv_obj_t *bubble = NULL;

    if (from == WEICHAT_MSG_FROM_PEER) {
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *avatar = lv_obj_create(row);
        lv_obj_set_size(avatar, WECHAT_AVATAR_SIZE, WECHAT_AVATAR_SIZE);
        lv_obj_set_style_radius(avatar, 4, 0);
        lv_obj_set_style_bg_color(avatar, lv_color_hex(0x3A6EA5), 0);
        lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(avatar, 0, 0);
        lv_obj_clear_flag(avatar, LV_OBJ_FLAG_SCROLLABLE);

        bubble = lv_obj_create(row);
        lv_obj_set_height(bubble, WECHAT_EMOJI_BUBBLE_H);
        lv_obj_set_width(bubble, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(bubble, 6, 0);
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0x2C2C34), 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bubble, 0, 0);
        lv_obj_set_style_pad_all(bubble, 8, 0);

        lv_obj_t *label = lv_label_create(bubble);
        lv_label_set_text(label, emoji_text);
        lv_obj_center(label);
		
		lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);

    } else {
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        bubble = lv_obj_create(row);
        lv_obj_set_height(bubble, WECHAT_EMOJI_BUBBLE_H);
        lv_obj_set_width(bubble, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(bubble, 6, 0);
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0x4AA1FF), 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bubble, 0, 0);
        lv_obj_set_style_pad_all(bubble, 8, 0);

        lv_obj_t *label = lv_label_create(bubble);
        lv_label_set_text(label, emoji_text);
        lv_obj_center(label);
		lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);

        lv_obj_t *avatar = lv_obj_create(row);
        lv_obj_set_size(avatar, WECHAT_AVATAR_SIZE, WECHAT_AVATAR_SIZE);
        lv_obj_set_style_radius(avatar, 4, 0);
        lv_obj_set_style_bg_color(avatar, lv_color_hex(0xF5A623), 0);
        lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(avatar, 0, 0);
        lv_obj_clear_flag(avatar, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* 填 slot（用于焦点高亮） */
    if (bubble) {
        wechat_msg_slot_t *slot = wechat_msg_alloc();
        slot->row = row;
        slot->bubble = bubble;
        slot->type = WECHAT_MSG_TYPE_EMOJI;
        slot->from = from;
        slot->emoji_id = emoji_id;
    }

    lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    wechat_update_msg_focus_style();

    if (do_log && !s_wechat_history_loading) {
        wechat_history_append_emoji(from, emoji_id);
    }
}

static void wechat_add_emoji_message(wechat_msg_from_t from,
                                    uint16_t emoji_id,
                                    const char *emoji_text)
{
    wechat_add_emoji_message_ex(from, emoji_id, emoji_text, 1);
}


/*------------------------------------------------
 *  表情面板相关实现
 *----------------------------------------------*/

/* 更新表情面板中某个按钮的选中样式 */
static void emoji_update_focus_style(void)
{
    for (int i = 0; i < EMOJI_COUNT; i++) {
        lv_obj_t *btn = s_emoji_btns[i];
        if (!btn) continue;

        if (i == s_emoji_focus_idx) {
            /* 选中：亮一点 + 边框 */
            lv_obj_set_style_bg_color(btn, lv_color_hex(0xFCA702), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(btn, 2, 0);
            lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), 0);
        } else {
            /* 未选中：暗灰色，无边框 */
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(btn, 0, 0);
        }
    }
}

/* 当用户在面板中按下“确定”选中某个表情时的处理 */
static void emoji_panel_on_select(uint8_t idx)
{
    if (idx >= EMOJI_COUNT) return;

    const char *emoji = s_emoji_texts[idx];
    printf("[wechat] emoji selected: %s (idx=%d)\n", emoji, idx);

    /* 1) 本端显示：统一走 UIQ（log + post） */
    wechat_local_show_emoji((uint16_t)idx);
	
    /* 2) 关闭弹窗 */
    wechat_close_emoji_panel();
	
    /* 3) 发出去（协议里的 emoji_id = idx） */
    if (wechat_emoji_send((uint16_t)idx) == 0) {
        printf("[wechat] emoji_send ok, id=%u\n", (unsigned)idx);
    } else {
        printf("[wechat] emoji_send FAIL, id=%u\n", (unsigned)idx);
    }

}



/* 关闭表情面板 */
  static void wechat_close_emoji_panel(void)
  {
      // 添加额外的安全检查和延时
      if (s_emoji_panel_visible && ui_emojiPanel) {
          if (lv_obj_is_valid(ui_emojiPanel)) {
              // 先隐藏面板，避免用户操作
              lv_obj_add_flag(ui_emojiPanel, LV_OBJ_FLAG_HIDDEN);

              // 安全删除子对象
              for (int i = 0; i < EMOJI_COUNT; i++) {
                  if (s_emoji_btns[i] && lv_obj_is_valid(s_emoji_btns[i])) {
                      lv_obj_del(s_emoji_btns[i]);
                  }
                  s_emoji_btns[i] = NULL;
              }

              // 最后删除面板本身
              lv_obj_del(ui_emojiPanel);
          }
          ui_emojiPanel = NULL;
      }

      // 清空所有相关状态
      for (int i = 0; i < EMOJI_COUNT; i++) {
          s_emoji_btns[i] = NULL;
      }
      s_emoji_panel_visible = 0;
      s_emoji_focus_idx = 0;
  }



/* 打开 / 创建 表情面板 */
static void wechat_open_emoji_panel(void)
{
    if (s_emoji_panel_visible) {
        /* 已经打开了，就不重复创建，可以改成切换逻辑 */
        return;
    }

    if (!ui_wechatPage || !lv_obj_is_valid(ui_wechatPage)) {
        printf("[wechat] ui_wechatPage not ready, can't open emoji panel\n");
        return;
    }
	
	if (ui_emojiPanel && !lv_obj_is_valid(ui_emojiPanel)) {
        ui_emojiPanel = NULL;
    }
    // 保险：如果残留了，先删
    if (ui_emojiPanel) {
        lv_obj_del(ui_emojiPanel);
        ui_emojiPanel = NULL;
    }
	
    int page_w = lv_obj_get_width(ui_wechatPage);
    int page_h = lv_obj_get_height(ui_wechatPage);

    if (!ui_emojiPanel) {
        /* 第一次创建表情面板 */
        ui_emojiPanel = lv_obj_create(ui_wechatPage);
        lv_obj_set_size(ui_emojiPanel, page_w * 4 / 5, page_h * 3 / 5);
        lv_obj_align(ui_emojiPanel, LV_ALIGN_CENTER, 0, 0);

        lv_obj_clear_flag(ui_emojiPanel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(ui_emojiPanel, 8, 0);
        lv_obj_set_style_bg_color(ui_emojiPanel, lv_color_hex(0x202020), 0);
        lv_obj_set_style_bg_opa(ui_emojiPanel, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ui_emojiPanel, 2, 0);
        lv_obj_set_style_border_color(ui_emojiPanel, lv_color_hex(0xFCA702), 0);
        lv_obj_set_style_pad_all(ui_emojiPanel, 6, 0);

        /* 顶部标题 */
        lv_obj_t *title = lv_label_create(ui_emojiPanel);
        lv_label_set_text(title, "选择表情");
        lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

        /* 表情区域容器 */
        lv_obj_t *emoji_cont = lv_obj_create(ui_emojiPanel);
        lv_obj_set_size(emoji_cont, lv_pct(100), lv_pct(100));
        lv_obj_align(emoji_cont, LV_ALIGN_BOTTOM_MID, 0, -4);

        lv_obj_clear_flag(emoji_cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(emoji_cont, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(emoji_cont, 0, 0);
        lv_obj_set_style_pad_all(emoji_cont, 4, 0);

        lv_obj_set_flex_flow(emoji_cont, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(emoji_cont,
                              LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER);

        /* 创建若干个按钮，每个按钮显示一个 emoji */
        int btn_size = 32;

        for (int i = 0; i < EMOJI_COUNT; i++) {
            lv_obj_t *btn = lv_btn_create(emoji_cont);
            lv_obj_set_size(btn, btn_size, btn_size);
            lv_obj_set_style_radius(btn, btn_size / 2, 0);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(btn, 0, 0);

            lv_obj_t *label = lv_label_create(btn);
            lv_label_set_text(label, s_emoji_texts[i]);
            lv_obj_center(label);

            s_emoji_btns[i] = btn;
        }
    }

    /* 面板置顶 + 显示 */
    /* 创建完再 move_foreground（同样确保 valid） */
    if (ui_emojiPanel && lv_obj_is_valid(ui_emojiPanel)) {
        lv_obj_move_foreground(ui_emojiPanel);
        lv_obj_clear_flag(ui_emojiPanel, LV_OBJ_FLAG_HIDDEN);
    }
	
    s_emoji_panel_visible = 1;
    s_emoji_focus_idx = 0;
    emoji_update_focus_style();

    printf("[wechat] emoji panel opened\n");
}

static void wechat_photo_bubble_event_cb(lv_event_t *e)
{
    wechat_msg_slot_t *slot = (wechat_msg_slot_t *)lv_event_get_user_data(e);
    if (!slot) return;

    printf("[wechat] photo bubble clicked: file=%s\r\n", slot->wav_path);

    // 记住“这是第几次出现”，回来的时候精确定位
    wechat_prepare_restore_for_photo_slot(slot);

    album_set_target_preview(slot->wav_path);
	
    start_img_from(PAGE_WECHAT);
}

void wechat_add_photo_message_bound_ex(wechat_msg_from_t from,
                                       const char *img_path,
                                       uint8_t do_log)
{
    static uint32_t g_dbg_ui_create_photo = 0;

    if (!img_path || !img_path[0]) return;

    // 1) 先分配 slot（避免 UI 已创建但 slot 失败）
    wechat_msg_slot_t *slot = wechat_msg_alloc();
    if (!slot) {
        printf("[DBG][photo] wechat_msg_alloc FAIL\n");
        return;
    }

    slot->type = WECHAT_MSG_TYPE_PHOTO;
    slot->from = from;

    // 2) 立刻把路径拷贝到 slot 的稳定内存里
    os_strncpy(slot->wav_path, img_path, sizeof(slot->wav_path) - 1);
    slot->wav_path[sizeof(slot->wav_path) - 1] = '\0';

    printf("[DBG][photo] ui_create #%u from=%u do_log=%u in=%s saved=%s slot=%p\r\n",
           ++g_dbg_ui_create_photo, from, do_log, img_path, slot->wav_path, slot);

    // 3) UI 创建统一使用 slot->wav_path（不要再用外部 img_path）
    lv_obj_t *bubble = NULL;
    lv_obj_t *row = wechat_add_photo_message_ui(from, slot->wav_path, &bubble);
    if (!row || !bubble) {
        // 如果你们 slot 是池子不能 free，就至少把 slot 标记为空闲/无效
        // wechat_msg_free(slot);
        printf("[DBG][photo] ui_create FAIL row=%p bubble=%p\r\n", row, bubble);
        return;
    }

    slot->row = row;
    slot->bubble = bubble;

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, wechat_photo_bubble_event_cb, LV_EVENT_CLICKED, (void *)slot);

    wechat_update_msg_focus_style();

    if (do_log && !s_wechat_history_loading) {
        wechat_history_append_photo(from, slot->wav_path);
    }
}

void wechat_add_photo_message_bound(wechat_msg_from_t from, const char *img_path)
{
    wechat_add_photo_message_bound_ex(from, img_path, 1);
}



static lv_obj_t *wechat_add_photo_message_ui(wechat_msg_from_t from,
                                             const char *img_path,
                                             lv_obj_t **out_bubble)
{
    if (!ui_wechatMsgArea) return NULL;

    lv_obj_t *row = lv_obj_create(ui_wechatMsgArea);
	lv_obj_set_style_pad_top(row, 0, 0);
	lv_obj_set_style_pad_bottom(row, 0, 0);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_bg_color(row, lv_color_hex(0x101018), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_left(row, 4, 0);
    lv_obj_set_style_pad_right(row, 4, 0);

    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

    if (from == WEICHAT_MSG_FROM_PEER) {
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *avatar = lv_obj_create(row);
        lv_obj_set_size(avatar, WECHAT_AVATAR_SIZE, WECHAT_AVATAR_SIZE);
        lv_obj_set_style_radius(avatar, 4, 0);
        lv_obj_set_style_bg_color(avatar, lv_color_hex(0x3A6EA5), 0);
        lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(avatar, 0, 0);
        lv_obj_clear_flag(avatar, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *bubble = lv_obj_create(row);
        lv_obj_set_size(bubble, WECHAT_PHOTO_BUBBLE_W, WECHAT_PHOTO_BUBBLE_H);
        lv_obj_set_style_radius(bubble, 6, 0);
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0x2C2C34), 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bubble, 0, 0);
        lv_obj_set_style_pad_all(bubble, 6, 0);

        lv_obj_t *icon = lv_label_create(bubble);
        lv_label_set_text(icon, LV_SYMBOL_IMAGE);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *name = lv_label_create(bubble);
        lv_label_set_text(name, wechat_basename(img_path));
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 22, 0);

        if (out_bubble) *out_bubble = bubble;

    } else {
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *bubble = lv_obj_create(row);
        lv_obj_set_size(bubble, WECHAT_PHOTO_BUBBLE_W, WECHAT_PHOTO_BUBBLE_H);
        lv_obj_set_style_radius(bubble, 6, 0);
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0x4AA1FF), 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bubble, 0, 0);
        lv_obj_set_style_pad_all(bubble, 6, 0);

        lv_obj_t *icon = lv_label_create(bubble);
        lv_label_set_text(icon, LV_SYMBOL_IMAGE);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *name = lv_label_create(bubble);
        lv_label_set_text(name, wechat_basename(img_path));
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 22, 0);

        if (out_bubble) *out_bubble = bubble;

        lv_obj_t *avatar = lv_obj_create(row);
        lv_obj_set_size(avatar, WECHAT_AVATAR_SIZE, WECHAT_AVATAR_SIZE);
        lv_obj_set_style_radius(avatar, 4, 0);
        lv_obj_set_style_bg_color(avatar, lv_color_hex(0xF5A623), 0);
        lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(avatar, 0, 0);
        lv_obj_clear_flag(avatar, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    return row;
}

void wechat_set_return_focus_photo(const char *img_path)
{
    s_wechat_need_restore_after_album = 1;
    if (img_path) {
        os_strncpy(s_wechat_restore_photo_path, img_path,
                   sizeof(s_wechat_restore_photo_path) - 1);
        s_wechat_restore_photo_path[sizeof(s_wechat_restore_photo_path) - 1] = '\0';
    } else {
        s_wechat_restore_photo_path[0] = '\0';
    }
}


static void wechat_normalize_path(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!src || !src[0]) return;

    // 目标：统一成 "0:/DCIM/xxx.JPG" 这种形式，且把 '\' 变成 '/'
    // 兼容： "0:DCIM/xxx" / "0:/DCIM/xxx" / "DCIM/xxx"
    char tmp[64];
    size_t j = 0;
    const char *p = src;

    // 处理盘符 "0:"
    if (p[0] && p[1] == ':') {
        if (j < sizeof(tmp)-1) tmp[j++] = p[0];
        if (j < sizeof(tmp)-1) tmp[j++] = ':';
        p += 2;
        // "0:DCIM" 这种补一个 '/'
        if (*p != '/' && *p != '\\') {
            if (j < sizeof(tmp)-1) tmp[j++] = '/';
        }
    }

    while (*p && j < sizeof(tmp)-1) {
        char c = *p++;
        if (c == '\\') c = '/';
        tmp[j++] = c;
    }
    tmp[j] = '\0';

    os_strncpy(dst, tmp, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
}

static int wechat_path_equal(const char *a, const char *b)
{
    char na[64], nb[64];
    wechat_normalize_path(na, sizeof(na), a);
    wechat_normalize_path(nb, sizeof(nb), b);

    for (int i = 0;; i++) {
        char ca = na[i];
        char cb = nb[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        if (ca == '\0') return 1;
    }
}


void wechat_try_restore_after_album(void)
{
    if (!s_wechat_need_restore_after_album) return;
    s_wechat_need_restore_after_album = 0;

    s_wechat_focus_idx  = 1;
    s_wechat_focus_mode = WECHAT_FOCUS_MSG;

    int16_t found = -1;
    uint8_t cnt = (s_msg_cnt > WECHAT_MAX_MSG_SLOTS) ? WECHAT_MAX_MSG_SLOTS : s_msg_cnt;

    //如果有 occurrence，就找“第 occ 次出现”
    if (s_wechat_restore_photo_occ > 0 && s_wechat_restore_photo_path[0]) {
        uint16_t occ = 0;
        for (int16_t i = 0; i < (int16_t)cnt; i++) {
            if (s_msg_slots[i].type == WECHAT_MSG_TYPE_PHOTO &&
                wechat_path_equal(s_msg_slots[i].wav_path, s_wechat_restore_photo_path)) {
                occ++;
                if (occ == s_wechat_restore_photo_occ) {
                    found = i;
                    break;
                }
            }
        }
        printf("[wechat][restore] search by occ=%u -> found=%d\r\n",
               (unsigned)s_wechat_restore_photo_occ, (int)found);
    }

    // occ 找不到就用 idx_hint
    if (found < 0 && s_wechat_restore_msg_idx_hint >= 0 &&
        s_wechat_restore_msg_idx_hint < (int16_t)cnt) {
        found = s_wechat_restore_msg_idx_hint;
        printf("[wechat][restore] fallback to idx_hint=%d\r\n", (int)found);
    }

    // 再不行就回到你原来的“最后一条”
    if (found < 0 && s_wechat_restore_photo_path[0]) {
        for (int16_t i = (int16_t)cnt - 1; i >= 0; i--) {
            if (s_msg_slots[i].type == WECHAT_MSG_TYPE_PHOTO &&
                wechat_path_equal(s_msg_slots[i].wav_path, s_wechat_restore_photo_path)) {
                found = i;
                break;
            }
        }
        printf("[wechat][restore] fallback to last match -> %d\r\n", (int)found);
    }

    if (found < 0 && s_msg_cnt > 0) found = (int16_t)(s_msg_cnt - 1);
    s_wechat_msg_focus_idx = found;

    // 用完清掉（避免下次误用）
    s_wechat_restore_photo_occ = 0;
    s_wechat_restore_msg_idx_hint = -1;

    wechat_update_focus_style();
    wechat_update_msg_focus_style();
    if (found >= 0 && s_msg_slots[found].row) {
        lv_obj_scroll_to_view(s_msg_slots[found].row, LV_ANIM_OFF);
    }

    printf("[wechat] restored after album: idx=%d path=%s\r\n",
           (int)found, s_wechat_restore_photo_path);
}

/*------------------------------------------------
 *  按键事件处理
 *----------------------------------------------*/
void ui_event_wechatPage(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    uint32_t *key_val = (uint32_t *)e->param;

    if (event_code != USER_KEY_EVENT || key_val == NULL)
        return;
	printf("[wechat] key_val=0x%08X\r\n", (unsigned)(*key_val));

    /* 表情面板打开时，优先处理它的按键 */
    if (s_emoji_panel_visible) {
        switch (*key_val)
        {
            case AD_LEFT:
                if (s_emoji_focus_idx > 0) {
                    s_emoji_focus_idx--;
                    emoji_update_focus_style();
                }
                return;

            case AD_RIGHT:
                if (s_emoji_focus_idx + 1 < EMOJI_COUNT) {
                    s_emoji_focus_idx++;
                    emoji_update_focus_style();
                }
                return;

            case AD_PRESS:   /* OK 选中当前表情 */
				if (!wechat_guard_action(WECHAT_ACT_EMOJI)) return;
                emoji_panel_on_select(s_emoji_focus_idx);
                return;

            case AD_BACK:    /* 返回键：关闭表情面板 */
                wechat_close_emoji_panel();
                return;

            default:
                return;
        }
    }

    /* 表情面板没打开时，走主逻辑 */
    switch (*key_val)
    {
        case AD_LEFT:
            if (s_wechat_focus_mode == WECHAT_FOCUS_BOTTOM) {
				uint8_t old_idx = s_wechat_focus_idx;
                if (s_wechat_focus_idx == 0)
				{
                    s_wechat_focus_idx = 4;
				}else{
                    s_wechat_focus_idx--;
				}
				/* 焦点从“语音按钮”(idx=1) 离开 → 停止播放 */
				if (old_idx == 1 && s_wechat_focus_idx != 1) {
					wechat_stop_current_voice();
				}
                wechat_update_focus_style();
            } else {
                /* 消息模式：向前选上一条消息（语音 or 表情） */
                if (s_msg_cnt > 0 && s_wechat_msg_focus_idx > 0) {
                    s_wechat_msg_focus_idx--;
                    wechat_update_msg_focus_style();

                    wechat_msg_slot_t *slot =
                        &s_msg_slots[s_wechat_msg_focus_idx];
                    if (slot->row) lv_obj_scroll_to_view(slot->row, LV_ANIM_OFF);
                }
            }
            break;

        case AD_RIGHT:
            if (s_wechat_focus_mode == WECHAT_FOCUS_BOTTOM) {
				uint8_t old_idx = s_wechat_focus_idx;
                s_wechat_focus_idx = (s_wechat_focus_idx + 1) % 5;
				
				/* 焦点从“语音按钮”(idx=1) 离开 → 停止播放 */
				if (old_idx == 1 && s_wechat_focus_idx != 1) {
					wechat_stop_current_voice();
				}
				
                wechat_update_focus_style();
            } else {
                /* 消息模式：向后选下一条消息（语音 or 表情） */
                if (s_msg_cnt > 0 &&
                    s_wechat_msg_focus_idx + 1 < (int16_t)s_msg_cnt) {

                    s_wechat_msg_focus_idx++;
                    wechat_update_msg_focus_style();
                    wechat_msg_slot_t *slot =
                        &s_msg_slots[s_wechat_msg_focus_idx];
                    if (slot->row) lv_obj_scroll_to_view(slot->row, LV_ANIM_OFF);
                }
            }
            break;

        case AD_VOL_UP:
            /* TODO: 音量+ */
            break;

        case AD_VOL_DOWN:
            /* TODO: 音量- */
            break;

        case AD_BACK:
            if (s_wechat_focus_mode == WECHAT_FOCUS_MSG) {
                /* 从消息模式退回到底部按钮模式 */
                s_wechat_focus_mode    = WECHAT_FOCUS_BOTTOM;
                s_wechat_msg_focus_idx = -1;
                wechat_update_msg_focus_style();
                wechat_update_focus_style();
                printf("[wechat] exit msg focus mode, back to bottom bar\n");
            } else {
				/* 离开微聊页面本身 → 也停止播放 */
				wechat_stop_current_voice();
                printf("[wechat] back to HOME\n");
                lv_page_select(PAGE_HOME);
                camera_gvar.pagebtn_index = 0;
            }
            break;

        case AD_PRESS:   /* OK 键 */
            switch (s_wechat_focus_idx)
            {
                case 0: // 视频通话
                    printf("[wechat] video button OK\n");
					if (!wechat_guard_action(WECHAT_ACT_INTERCOM)) return;
					wechat_stop_current_voice();          // 离开微聊 → 停止播放
                    lv_page_select(PAGE_INTERCOM);
                    break;

                case 1: // 语音按钮：在底部模式下，OK = 进入消息模式；在消息模式下，OK = 执行当前消息操作
                    if (s_wechat_focus_mode == WECHAT_FOCUS_BOTTOM) {
						if (!wechat_guard_action(WECHAT_ACT_MSG_VIEW_ONLY)) return;
                        if (s_msg_cnt > 0) {
                            s_wechat_focus_mode    = WECHAT_FOCUS_MSG;
                            s_wechat_msg_focus_idx = (int16_t)(s_msg_cnt - 1);
                            wechat_update_msg_focus_style();

                            wechat_msg_slot_t *slot =
                                &s_msg_slots[s_wechat_msg_focus_idx];
                            if (slot->row) {
                                lv_obj_scroll_to_view(slot->row, LV_ANIM_OFF);
                            }
                            printf("[wechat] enter msg focus mode, idx=%d\n",
                                   (int)s_wechat_msg_focus_idx);
                        } else {
                            printf("[wechat] no messages yet\n");
                        }
                    }  else {
						if (!wechat_guard_action(WECHAT_ACT_MSG_VIEW_ONLY)) return;
						if (s_msg_cnt > 0 &&
							s_wechat_msg_focus_idx >= 0 &&
							s_wechat_msg_focus_idx < (int16_t)s_msg_cnt) {

							wechat_msg_slot_t *slot = &s_msg_slots[s_wechat_msg_focus_idx];

							if (slot->type == WECHAT_MSG_TYPE_VOICE) {
								wechat_play_voice_slot(slot);

							} else if (slot->type == WECHAT_MSG_TYPE_EMOJI) {
								printf("[wechat] selected emoji: id=%u from=%d\n",
									   slot->emoji_id, slot->from);

							} else if (slot->type == WECHAT_MSG_TYPE_PHOTO) {
								printf("[wechat] open selected photo: %s\r\n", slot->wav_path);

								wechat_stop_current_voice();
								wechat_prepare_restore_for_photo_slot(slot);
								album_set_target_preview(slot->wav_path);
								start_img_from(PAGE_WECHAT);
							}
						}
					}
                    break;

                case 2: // 表情
                    printf("[wechat] emoji panel\n");
					if (!wechat_guard_action(WECHAT_ACT_EMOJI)) return;
                    wechat_open_emoji_panel();
                    break;

                case 3: // 拍照发送
                    printf("[wechat] capture & send photo\n");
					if (!wechat_guard_action(WECHAT_ACT_CAMERA)) return;
                    start_camera_from(PAGE_WECHAT);
                    break;

                case 4: // 相册图片
                    printf("[wechat] open photo selector\n");
//                    start_img_from(PAGE_WECHAT);
					if (!wechat_guard_action(WECHAT_ACT_PHOTO_PICK)) return;
					album_enter_from(PAGE_WECHAT, ALBUM_MODE_WECHAT_PICK);
                    break;

                default:
                    break;
            }
            break;

        case UI_KEY_CALL_LONG_DOWN:
            /* 只有在“语音按钮”被选中的时候，才开始录音 */
            if (s_wechat_focus_idx == 1) {
				if (!wechat_guard_action(WECHAT_ACT_RECORD)) return;
                wechat_record_ui_request(1);  
            }
            break;

        case UI_KEY_CALL_LONG_UP:
            if (s_wechat_focus_idx == 1) {
				if (!wechat_guard_action(WECHAT_ACT_RECORD)) return;
                wechat_record_ui_request(0);   // 只置位，不碰 LVGL
			}
            break;

        default:
            break;
    }
}


//从日志文件恢复历史聊天记录（语音 + 表情）

static void wechat_history_load(void)
{
    if (s_wechat_history_loaded) {
        printf("already loaded, skip");
        return;
    }

    void *fp = osal_fopen(WECHAT_LOG_FILE, "rb");
    if (!fp) {
        printf("no history log, skip load (%s)", WECHAT_LOG_FILE);
        s_wechat_history_loaded = 1;
        return;
    }

    printf("load history from %s", WECHAT_LOG_FILE);

    wechat_log_item_t item;
    s_wechat_history_loading = 1;

    while (1) {
        int r = osal_fread(&item, 1, sizeof(wechat_log_item_t), fp);

        if (r == 0) {
            printf("read EOF, r=0");
            break;
        }

        if (r != (int)sizeof(wechat_log_item_t)) {
            printf("read broken record, r=%d (< %u)",
                           r, (unsigned)sizeof(wechat_log_item_t));
            break;
        }

        if (item.type == WECHAT_MSG_TYPE_VOICE) {

            uint8_t sec = item.u.voice.sec;
            if (sec == 0) sec = 1;
            if (sec > WECHAT_RECORD_MAX_SEC) sec = WECHAT_RECORD_MAX_SEC;

            printf("history voice: from=%u msg_id=%u sec=%u path=%s",
                           (unsigned)item.from,
                           (unsigned)item.u.voice.msg_id,
                           (unsigned)sec,
                           item.u.voice.wav_path);

            wechat_add_voice_message_bound(
                (wechat_msg_from_t)item.from,
                sec,
                item.u.voice.msg_id,
                item.u.voice.wav_path
            );

        } else if (item.type == WECHAT_MSG_TYPE_EMOJI) {

            uint16_t emoji_id = item.u.emoji.emoji_id;
            printf("history emoji: from=%u emoji_id=%u",
                           (unsigned)item.from, (unsigned)emoji_id);

            if (emoji_id < EMOJI_COUNT) {
                const char *emoji_txt = s_emoji_texts[emoji_id];
                wechat_add_emoji_message(
                    (wechat_msg_from_t)item.from,
                    emoji_id,
                    emoji_txt
                );
            } else {
                printf("history emoji_id(%u) out of range", emoji_id);
            }
        } else if (item.type == WECHAT_MSG_TYPE_PHOTO) {
			printf("history photo: from=%u path=%s",
				   (unsigned)item.from,
				   item.u.photo.img_path);

			wechat_add_photo_message_bound((wechat_msg_from_t)item.from, item.u.photo.img_path);
		}
		else {
            printf("history unknown type=%u", (unsigned)item.type);
        }
    }

    s_wechat_history_loading = 0;
    s_wechat_history_loaded  = 1;

    osal_fclose(fp);
    printf("history load done, s_msg_cnt=%u", (unsigned)s_msg_cnt);
}

static uint8_t s_wechat_focus_pending_valid = 0;
static uint8_t s_wechat_focus_pending_idx   = 0;

void wechat_request_focus_idx(uint8_t idx)
{
    if (idx > 4) idx = 0;
    s_wechat_focus_pending_valid = 1;
    s_wechat_focus_pending_idx   = idx;
}

/*------------------------------------------------
 *  页面初始化
 *----------------------------------------------*/
void ui_wechatPage_screen_init(void)
{
	userPairstop();
	   
	s_offline_popup_req = 0;
    if (s_offline_timer) { lv_timer_del(s_offline_timer); s_offline_timer = NULL; }
    if (s_offline_mask && lv_obj_is_valid(s_offline_mask)) { lv_obj_del(s_offline_mask); }
    s_offline_mask = NULL;
    s_offline_panel = NULL;
	
	if (ui_record_timer) { lv_timer_del(ui_record_timer); ui_record_timer=NULL; }
	if (ui_record_arc && lv_obj_is_valid(ui_record_arc)) lv_obj_del(ui_record_arc), ui_record_arc=NULL;
	if (ui_record_panel && lv_obj_is_valid(ui_record_panel)) lv_obj_del(ui_record_panel), ui_record_panel=NULL;

	if (ui_emojiPanel && lv_obj_is_valid(ui_emojiPanel)) lv_obj_del(ui_emojiPanel);
	ui_emojiPanel=NULL;
	s_emoji_panel_visible=0;   
	/* 先重置 wechat 消息状态机，防止多次进入页面时状态乱掉 */
    os_memset(s_msg_slots, 0, sizeof(s_msg_slots));
    s_msg_cnt               = 0;
    s_wechat_msg_focus_idx  = -1;
    s_wechat_focus_mode     = WECHAT_FOCUS_BOTTOM;
    s_wechat_playing_slot   = NULL;
    s_wechat_history_loaded = 0;   // ⭐ 关键：允许这次页面重新从文件加载

    s_emoji_panel_visible   = 0;
    s_emoji_focus_idx       = 0;
    s_wechat_focus_idx      = 0;
    wechat_recording        = 0;
    s_wechat_voice_pending.pending = 0;
	
    /* 根据当前屏幕尺寸布局，避免和 SCALE_* 不一致 */
    lv_obj_t *scr = lv_scr_act();
    int page_w = lv_obj_get_width(scr);
    int page_h = lv_obj_get_height(scr);

    const int status_bar_h = 16;  /* 最上方状态栏高度 */
    const int top_bar_h    = 24;  /* 标题栏 */
    const int bottom_bar_h = 44;  /* 底栏稍微高一点，放 36x36 的按钮 */

    /* 根页面 */
    ui_wechatPage = lv_obj_create(scr);
    curPage_obj   = ui_wechatPage;

    camera_gvar.page_cur = PAGE_WECHAT;
	pro_page_cur = PAGE_WECHAT; 
    lv_obj_set_size(ui_wechatPage, page_w, page_h);
    lv_obj_clear_flag(ui_wechatPage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(ui_wechatPage, 0, 0);
    lv_obj_set_style_border_width(ui_wechatPage, 0, 0);
    lv_obj_set_style_bg_color(ui_wechatPage, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ui_wechatPage, LV_OPA_COVER, 0);

    /* 事件：让本页自己处理按键（event_handler 内会调用 ui_event_wechatPage） */
    lv_obj_add_event_cb(ui_wechatPage, event_handler, LV_EVENT_ALL, NULL);

    /* 区域0：最上面的状态栏（WiFi 状态 + 电池图标 + 文本）*/
    ui_wechatStatusBar = lv_obj_create(ui_wechatPage);
    lv_obj_set_size(ui_wechatStatusBar, page_w, status_bar_h);
    lv_obj_align(ui_wechatStatusBar, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_clear_flag(ui_wechatStatusBar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_wechatStatusBar, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(ui_wechatStatusBar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui_wechatStatusBar, 0, 0);
    lv_obj_set_style_pad_all(ui_wechatStatusBar, 0, 0);
    lv_obj_set_style_radius(ui_wechatStatusBar, 0, 0);

    /* 底部分隔线 */
    lv_obj_set_style_border_side(ui_wechatStatusBar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(ui_wechatStatusBar, 1, 0);
    lv_obj_set_style_border_color(ui_wechatStatusBar, lv_color_hex(0x222222), 0);

    /* 左侧 WiFi 圆点 */
    ui_wechatWifiDot = lv_obj_create(ui_wechatStatusBar);
    lv_obj_set_size(ui_wechatWifiDot, 10, 10);
    lv_obj_align(ui_wechatWifiDot, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_radius(ui_wechatWifiDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ui_wechatWifiDot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui_wechatWifiDot, 0, 0);

    /* 默认灰色：未连接 */
    lv_obj_set_style_bg_color(ui_wechatWifiDot, lv_color_hex(0x666666), 0);
	wechat_update_wifi_status(get_wifi_connect_flag());
    /* 右侧：电池图标 */
    lv_obj_t *batt_container = lv_obj_create(ui_wechatStatusBar);
    lv_obj_set_size(batt_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(batt_container, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_clear_flag(batt_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(batt_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(batt_container, 0, 0);
    lv_obj_set_style_pad_all(batt_container, 0, 0);
    lv_obj_set_style_radius(batt_container, 0, 0);

    ui_wechatBattImg = lv_img_create(batt_container);
    lv_img_set_src(ui_wechatBattImg, ui_imgset_iconBat[get_batlevel()]);
    lv_obj_set_width(ui_wechatBattImg, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_wechatBattImg, LV_SIZE_CONTENT);
    lv_obj_align(ui_wechatBattImg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(ui_wechatBattImg, LV_OBJ_FLAG_SCROLLABLE);

    /* 初始化刷新一次 */
    // wechat_update_batt_percent(get_batlevel() * 50);

    /* 区域1：顶部栏：标题 “WeChat”*/
    ui_wechatTopBar = lv_obj_create(ui_wechatPage);
    lv_obj_set_size(ui_wechatTopBar, page_w, top_bar_h);
    lv_obj_align(ui_wechatTopBar, LV_ALIGN_TOP_MID, 0, status_bar_h);
    lv_obj_clear_flag(ui_wechatTopBar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_wechatTopBar, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(ui_wechatTopBar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui_wechatTopBar, 0, 0);
    lv_obj_set_style_pad_left(ui_wechatTopBar, 4, 0);
    lv_obj_set_style_pad_right(ui_wechatTopBar, 4, 0);
    lv_obj_set_style_pad_top(ui_wechatTopBar, 2, 0);
    lv_obj_set_style_pad_bottom(ui_wechatTopBar, 2, 0);
    lv_obj_set_style_radius(ui_wechatTopBar, 0, 0);

    /* 顶部栏底部分隔线 */
    lv_obj_set_style_border_side(ui_wechatTopBar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(ui_wechatTopBar, 1, 0);
    lv_obj_set_style_border_color(ui_wechatTopBar, lv_color_hex(0x222222), 0);

    lv_obj_t *title_label = lv_label_create(ui_wechatTopBar);
    lv_label_set_text(title_label, "WeChat");
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title_label, LV_ALIGN_CENTER, 0, 0);

    /* 区域2：中间聊天内容区（可滚动）*/
    ui_wechatMsgArea = lv_obj_create(ui_wechatPage);
    lv_obj_set_width(ui_wechatMsgArea, page_w);

    int msg_h = page_h - status_bar_h - top_bar_h - bottom_bar_h;
    if (msg_h < 40) msg_h = 40;
    lv_obj_set_height(ui_wechatMsgArea, msg_h);

    lv_obj_align(ui_wechatMsgArea, LV_ALIGN_TOP_MID, 0, status_bar_h + top_bar_h);
    lv_obj_set_scroll_dir(ui_wechatMsgArea, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui_wechatMsgArea, LV_SCROLLBAR_MODE_AUTO);

    /* 聊天区背景 */
    lv_obj_set_style_bg_color(ui_wechatMsgArea, lv_color_hex(0x101018), 0);
    lv_obj_set_style_bg_opa(ui_wechatMsgArea, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui_wechatMsgArea, 0, 0);
    lv_obj_set_style_pad_all(ui_wechatMsgArea, 2, 0);
    lv_obj_set_style_radius(ui_wechatMsgArea, 0, 0);

    lv_obj_set_style_pad_row(ui_wechatMsgArea, 2, 0);
    lv_obj_set_style_pad_column(ui_wechatMsgArea, 0, 0);

    lv_obj_set_flex_flow(ui_wechatMsgArea, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_wechatMsgArea,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
						  
    /* 区域3：底部功能区：5 个等宽按钮 */
    ui_wechatBtmBar = lv_obj_create(ui_wechatPage);
    lv_obj_set_size(ui_wechatBtmBar, page_w, bottom_bar_h);
    lv_obj_align(ui_wechatBtmBar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(ui_wechatBtmBar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_wechatBtmBar, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(ui_wechatBtmBar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui_wechatBtmBar, 0, 0);
    lv_obj_set_style_pad_left(ui_wechatBtmBar, 4, 0);
    lv_obj_set_style_pad_right(ui_wechatBtmBar, 4, 0);
    lv_obj_set_style_pad_top(ui_wechatBtmBar, 4, 0);
    lv_obj_set_style_pad_bottom(ui_wechatBtmBar, 4, 0);
    lv_obj_set_style_radius(ui_wechatBtmBar, 0, 0);

    lv_obj_set_flex_flow(ui_wechatBtmBar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_wechatBtmBar,
                          LV_FLEX_ALIGN_SPACE_AROUND,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    const int btn_sz = 36;

    /* 1. 视频按钮 */
    lv_obj_t *btn_video = lv_btn_create(ui_wechatBtmBar);
    lv_obj_set_size(btn_video, btn_sz, btn_sz);
    lv_obj_set_style_radius(btn_video, btn_sz / 2, 0);
    lv_obj_set_style_bg_color(btn_video, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(btn_video, 0, 0);
    lv_obj_t *label_video = lv_label_create(btn_video);
    lv_label_set_text(label_video, LV_SYMBOL_CALL);
    lv_obj_center(label_video);
    ui_wechatBtnVideo = btn_video;

    /* 2. 语音按钮 */
    lv_obj_t *btn_mic = lv_btn_create(ui_wechatBtmBar);
    lv_obj_set_size(btn_mic, btn_sz, btn_sz);
    lv_obj_set_style_radius(btn_mic, btn_sz / 2, 0);
    lv_obj_set_style_bg_color(btn_mic, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(btn_mic, 0, 0);
    lv_obj_t *label_mic = lv_label_create(btn_mic);
    lv_label_set_text(label_mic, LV_SYMBOL_AUDIO);
    lv_obj_center(label_mic);
    ui_wechatBtnMic = btn_mic;

    /* 3. 表情按钮 */
    lv_obj_t *btn_emoji = lv_btn_create(ui_wechatBtmBar);
    lv_obj_set_size(btn_emoji, btn_sz, btn_sz);
    lv_obj_set_style_radius(btn_emoji, btn_sz / 2, 0);
    lv_obj_set_style_bg_color(btn_emoji, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(btn_emoji, 0, 0);
    lv_obj_t *label_emoji = lv_label_create(btn_emoji);
    lv_label_set_text(label_emoji, "😊");
    lv_obj_center(label_emoji);
    ui_wechatBtnEmoji = btn_emoji;

    /* 4. 拍照按钮 */
    lv_obj_t *btn_camera = lv_btn_create(ui_wechatBtmBar);
    lv_obj_set_size(btn_camera, btn_sz, btn_sz);
    lv_obj_set_style_radius(btn_camera, btn_sz / 2, 0);
    lv_obj_set_style_bg_color(btn_camera, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(btn_camera, 0, 0);
    lv_obj_t *label_camera = lv_label_create(btn_camera);
    lv_label_set_text(label_camera, LV_SYMBOL_IMAGE);
    lv_obj_center(label_camera);
    ui_wechatBtnCamera = btn_camera;

    /* 5. 图片/相册按钮 */
    lv_obj_t *btn_photo = lv_btn_create(ui_wechatBtmBar);
    lv_obj_set_size(btn_photo, btn_sz, btn_sz);
    lv_obj_set_style_radius(btn_photo, btn_sz / 2, 0);
    lv_obj_set_style_bg_color(btn_photo, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(btn_photo, 0, 0);
    lv_obj_t *label_photo = lv_label_create(btn_photo);
    lv_label_set_text(label_photo, LV_SYMBOL_IMAGE);
    lv_obj_center(label_photo);
    ui_wechatBtnPhoto = btn_photo;

    /* 填充数组，给 wechat_update_focus_style 使用 */
    s_wechat_btns[0]       = ui_wechatBtnVideo;
    s_wechat_btns[1]       = ui_wechatBtnMic;
    s_wechat_btns[2]       = ui_wechatBtnEmoji;
    s_wechat_btns[3]       = ui_wechatBtnCamera;
    s_wechat_btns[4]       = ui_wechatBtnPhoto;

    s_wechat_btn_labels[0] = label_video;
    s_wechat_btn_labels[1] = label_mic;
    s_wechat_btn_labels[2] = label_emoji;
    s_wechat_btn_labels[3] = label_camera;
    s_wechat_btn_labels[4] = label_photo;
	

	
    /* 默认焦点在“视频通话” */
    if (s_wechat_focus_pending_valid) {
		s_wechat_focus_idx = s_wechat_focus_pending_idx;
		s_wechat_focus_pending_valid = 0;
	} else {
		s_wechat_focus_idx = 0;
	}
    wechat_update_focus_style();
	lv_obj_add_event_cb(ui_wechatPage, wechat_page_on_delete, LV_EVENT_DELETE, NULL);

	if (!ui_pending_message_timer) {
        ui_pending_message_timer = lv_timer_create(wechat_pending_message_timer_cb,
                                                 50,   /* 50ms 轮询一次够用了 */
                                                 NULL);
    }
	/* 页面创建完毕，加载历史聊天记录  */
    wechat_history_load();
	
	wechat_try_restore_after_album(); 
    printf("## ui_wechatPage2_screen_init (status bar + dark theme) done\n");
}
/* 用来在 UI 里显示表情 */
void wechat_on_emoji_packet(uint16_t msg_id,
                            uint16_t seq,
                            uint16_t total,
                            const uint8_t *data,
                            uint32_t len)
{
	
    if (!data || len < 2) return;

    /* emoji 这种小包建议只认 seq==0（避免未来分片重复处理） */
    if (seq != 0) return;

    uint16_t emoji_id = (uint16_t)(data[0] | (data[1] << 8));
    if (emoji_id >= EMOJI_COUNT) return;
	printf("RX emoji msg=%u id=%u", msg_id, emoji_id);
    /* ⭐ 1) 先持久化（不依赖 UI 是否前台） */
    wechat_history_append_emoji_public(WEICHAT_MSG_FROM_PEER, emoji_id);

    /* ⭐ 2) 如果当前在微信页，投递 UI（左侧），并标记已写日志避免重复 */
    if (wechat_ui_can_render_now()) {
        (void)wechat_ui_post_emoji_ex(WEICHAT_MSG_FROM_PEER, emoji_id, 1);
    }
}



void wechat_history_append_voice_public(wechat_msg_from_t from,
                                       uint8_t sec,
                                       uint16_t msg_id,
                                       const char *wav_path)
{
    wechat_history_append_voice(from, sec, msg_id, wav_path);
}

void wechat_history_append_emoji_public(wechat_msg_from_t from,
                                       uint16_t emoji_id)
{
    wechat_history_append_emoji(from, emoji_id);
}

void wechat_history_append_photo_public(wechat_msg_from_t from,
                                       const char *img_path)
{
    wechat_history_append_photo(from, img_path);
}
