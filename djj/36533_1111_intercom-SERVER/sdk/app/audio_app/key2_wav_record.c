/***************************************************
 * key2_wav_record.c
 *
 * 1) PTT：R_RECORD_AUDIO 流 -> WAV 文件 (SD)
 * 2) 松键后：worker 异步等待落盘 -> 读取 WAV -> 拆包发送 -> UI pending 气泡
 *
 * 目录：
 *  - WAV 文件目录统一 0:/DCIM
 *  - 文件名 PREFIX_0001.wav ~ PREFIX_9999.wav 自动找空号
 ***************************************************/
#include "sys_config.h"
#include "tx_platform.h"
#include "typesdef.h"
#include "stream_frame.h"
#include "stream_define.h"
#include "osal/task.h"
#include "osal/string.h"
#include "osal_file.h"
#include "custom_mem/custom_mem.h"
#include "osal/mutex.h"
#include "osal/semaphore.h"

#include "key2_wav_record.h"
#include "keyWork.h"
#include "keyScan.h"

#define WAV_DIR     "0:/DCIM"
#define SAMPLERATE  8000
#define PAGE_WECHAT 1

#define WECHAT_ACT_RECORD   1

extern int  wechat_guard_action(int act);         // 1=允许, 0=拦截(并 request 离线弹窗)
extern void wechat_request_offline_popup(void);   // 可选：如果你想在这里直接 request

/* 你 worker 里实际调用的是 _ex 版本，补齐声明 */
extern uint16_t wechat_msg_alloc_id(void);
extern int wechat_send_wav_as_voice_msg_ex(uint32_t frq,
                                          const char *wav_path,
                                          uint16_t msg_id,
                                          uint8_t  sec);
										  
extern void mute_speaker(uint8 enable);
extern void audio_adc_mute(void);
extern void audio_adc_unmute(void);
extern void audio_dac_set_filter_type(int filter_type);
extern int  get_audio_dac_set_filter_type(void);

/* digit mute 判断会用到 */
extern uint8_t rec_open;

/* wechat UI：用 pending 方式让 UI 线程创建气泡 */
extern void wechat_set_pending_voice_bubble(uint16_t msg_id,
                                            uint8_t  sec,
                                            const char *wav_path);

/* wechat_msg.c：从 wav 文件读 pcm 拆包发送，返回 msg_id/sec */
extern int wechat_send_wav_as_voice_msg(uint32_t frq,
                                       const char *wav_path,
                                       uint16_t *out_msg_id,
                                       uint8_t  *out_sec);


extern const unsigned char wav_header[];

/* ====== 当前页面/焦点 ====== */
extern uint8_t s_wechat_focus_idx;
extern struct {
    uint8_t page_cur;
    uint8_t page_back;
    uint8_t pagebtn_index;
} camera_gvar;

typedef struct {
    uint32_t keyvalue;
    uint32_t extern_value;
} wechat_key_evt_t;

#define WECHAT_KEY_Q_SIZE  16

static volatile uint8_t s_kq_head = 0;
static volatile uint8_t s_kq_tail = 0;
static wechat_key_evt_t s_kq[WECHAT_KEY_Q_SIZE];

static struct os_semaphore s_kq_sem;
static uint8_t s_kq_inited = 0;

static struct os_task s_wechat_key_task;


/* ====== 视频对讲模式开关 ====== */
uint8_t g_video_intercom_mode = 0;

/* ============================================================
 * 录音完成通知（落盘完成后 sem_up）
 * ============================================================ */
static struct os_semaphore s_wav_done_sem;
static uint8_t  s_wav_sem_inited = 0;

/* 最近一次完成的结果（由录音线程写，worker 读） */
static char     s_last_wav_path[64] = {0};
static uint32_t s_last_pcm_bytes    = 0;
static volatile uint32_t s_last_gen = 0;

/* 每次录音一个 gen */
static volatile uint32_t s_rec_gen    = 0;
static volatile uint32_t s_active_gen = 0;

/* ============================================================
 * 异步 worker：按键回调不阻塞，把“等待落盘+发送+UI绑定”交给 worker
 * ============================================================ */
typedef struct {
    uint32_t frq;
    uint32_t gen;
} wechat_voice_job_t;

#define WECHAT_JOB_Q_SIZE  4

static wechat_voice_job_t s_job_q[WECHAT_JOB_Q_SIZE];
static uint8_t s_job_head = 0;
static uint8_t s_job_tail = 0;

static struct os_mutex     s_job_mtx;
static struct os_semaphore s_job_sem;
static uint8_t             s_worker_inited = 0;
static struct os_task      s_wechat_voice_worker_task;

static void wechat_voice_worker_thread(void *arg);

/******************************/
#define DBG_BYTES          20

static int is_dbg_frame_u32(uint32_t idx)
{
    return (idx == 20 || idx == 30 || idx == 90);
}

static void dump_first_hex(const char *tag, const uint8_t *buf, uint32_t len, uint32_t n)
{
    if (!buf || len == 0) return;
    if (n > len) n = len;

    printf("[DBG] %s len=%u first=%u : ", tag, (unsigned)len, (unsigned)n);
    for (uint32_t i = 0; i < n; i++) printf("%02X ", buf[i]);
    printf("\r\n");
}
/******************************/



//把任务塞进队列 + 唤醒 worker
static int wechat_voice_job_push(uint32_t frq, uint32_t gen)
{
    os_mutex_lock(&s_job_mtx, -1);

    uint8_t next = (uint8_t)((s_job_tail + 1) % WECHAT_JOB_Q_SIZE);
    if (next == s_job_head) {
        os_mutex_unlock(&s_job_mtx);
        os_printf("[wechat] job queue full, drop gen=%u\r\n", (unsigned)gen);
        return -1;
    }

    s_job_q[s_job_tail].frq = frq;
    s_job_q[s_job_tail].gen = gen;
    s_job_tail = next;

    os_mutex_unlock(&s_job_mtx);
    os_sema_up(&s_job_sem);
    return 0;
}

//从队列取出一个任务给 worker 执行
static int wechat_voice_job_pop(wechat_voice_job_t *out)
{
    os_mutex_lock(&s_job_mtx, -1);

    if (s_job_head == s_job_tail) {
        os_mutex_unlock(&s_job_mtx);
        return -1;
    }

    *out = s_job_q[s_job_head];
    s_job_head = (uint8_t)((s_job_head + 1) % WECHAT_JOB_Q_SIZE);

    os_mutex_unlock(&s_job_mtx);
    return 0;
}

static void wechat_voice_worker_init(void)
{
    if (s_worker_inited) return;
    s_worker_inited = 1;

    os_mutex_init(&s_job_mtx);
    os_sema_init(&s_job_sem, 0);

    OS_TASK_INIT("wechat_voice_work",
                 &s_wechat_voice_worker_task,
                 wechat_voice_worker_thread,
                 0,
                 OS_TASK_PRIORITY_NORMAL,
                 2048);

    if (!s_wav_sem_inited) {
        os_sema_init(&s_wav_done_sem, 0);
        s_wav_sem_inited = 1;
    }
}

/* ============================================================
 * WAV 文件编号查找（0:/DCIM/prefix_0001.wav ~ 9999）
 * ============================================================ */
static uint32_t find_free_wav_index(const char *prefix)
{
    char path[64];
    void *fp;
    uint32_t i;

    if (!prefix || !prefix[0]) {
        prefix = "def";
    }

    for (i = 1; i <= 9999; i++) {
        os_sprintf(path, WAV_DIR "/%s_%04u.wav", prefix, (unsigned)i);
        fp = osal_fopen(path, "rb");
        if (!fp) {
            return i;
        }
        osal_fclose(fp);
    }

    return 1;
}

/* ============================================================
 * 录音任务结构
 * ============================================================ */
struct KEY_AUDIO
{
    uint32_t          frq;
    struct os_task    task;
    uint8_t           filename_prefix[4];
    volatile uint8_t  running;
    uint32_t          minute;
    uint32_t          gen;      /* 本次录音 gen */
};

static struct KEY_AUDIO *audio_s = NULL;

/* ============================================================
 * 录音线程：R_RECORD_AUDIO -> WAV
 * ============================================================ */
static int opcode_func(stream *s, void *priv, int opcode)
{
    (void)priv;
    switch (opcode)
    {
        case STREAM_OPEN_EXIT:
            enable_stream(s, 1);
            break;
        default:
            break;
    }
    return 0;
}

static void key_save_audio_thread(void *d)
{
	uint32_t frame_idx = 0;   // 记录从流里读到的“第几帧(320B)”
    struct KEY_AUDIO *a_s = (struct KEY_AUDIO *)d;
    WaveHeader header;

    os_memset(&header, 0, sizeof(WaveHeader));
    os_memcpy(&header, wav_header, sizeof(WaveHeader));

    header.fmt_pcm        = 1;          // PCM
    header.channels       = 1;          // mono
    header.fmt_samplehz   = a_s->frq;
    header.fmt_bitpsample = 16;

    header.fmt_bytesample = header.channels * (header.fmt_bitpsample / 8);
    header.fmt_bytepsec   = header.fmt_samplehz * header.fmt_bytesample;

    char filename[64] = {0};

    uint32_t w_count = 0;
    void    *fp = NULL;
    stream  *s  = NULL;
    struct data_structure *get_f = NULL;
    uint8_t *buf = NULL;

    /* 打开流：R_RECORD_AUDIO */
    s = open_stream_available(R_RECORD_AUDIO, 0, 8, opcode_func, NULL);
    if (!s) {
        os_printf("[wav] open_stream R_RECORD_AUDIO fail\r\n");
        goto end;
    }

    uint32_t sn = find_free_wav_index((const char *)a_s->filename_prefix);
    os_sprintf(filename, WAV_DIR "/%s_%04u.wav", a_s->filename_prefix, (unsigned)sn);
    os_printf("[wav] record name: %s (gen=%u)\r\n", filename, (unsigned)a_s->gen);

    fp = osal_fopen(filename, "wb+");
    if (!fp) {
        os_printf("[wav] fopen fail\r\n");
        goto end;
    }

    int hdr_w = osal_fwrite(&header, 1, sizeof(WaveHeader), fp);
    if (hdr_w != (int)sizeof(WaveHeader)) {
        os_printf("[wav] write header fail: got=%d expect=%u\r\n",
                  hdr_w, (unsigned)sizeof(WaveHeader));
        goto end;
    }

    uint32_t start_time = os_jiffies();

    while (a_s->running &&
           (os_jiffies() - start_time) / 1000 < a_s->minute * 60)
    {
        get_f = recv_real_data(s);
        if (get_f) {
            uint32_t flen = get_stream_real_data_len(get_f);
            buf = get_stream_real_data(get_f);
			/* 语音帧计数/打印 */
//			if (flen == 320) {
//				if (is_dbg_frame_u32(frame_idx)) {
//					char tag[64];
//					os_sprintf(tag, "STREAM frame=%u", (unsigned)frame_idx);
//					dump_first_hex(tag, buf, flen, DBG_BYTES);
//				}
//				frame_idx++;
//			}
            int wr = osal_fwrite(buf, 1, flen, fp);

            free_data(get_f);
            get_f = NULL;

            if (wr != (int)flen) {
                os_printf("[wav] write short: want=%u got=%d\r\n",
                          (unsigned)flen, wr);
                if (wr > 0) w_count += (uint32_t)wr;
                break;
            }
            w_count += (uint32_t)wr;
        } else {
            os_sleep_ms(1);
        }
    }

    /* 回填头 */
    header.dwDATALen = w_count;
    header.total_Len = w_count + sizeof(WaveHeader) - 8;

    os_printf("[wav] end gen=%u bytes=%u\r\n", (unsigned)a_s->gen, (unsigned)w_count);

    if (fp) {
        osal_fseek(fp, 0);
        (void)osal_fwrite(&header, 1, sizeof(header), fp);
        osal_fclose(fp);
        fp = NULL;
    }

end:
    if (s) {
        close_stream(s);
        s = NULL;
    }

    /* 写回结果给 worker */
    os_strncpy(s_last_wav_path, filename, sizeof(s_last_wav_path) - 1);
    s_last_wav_path[sizeof(s_last_wav_path) - 1] = '\0';
    s_last_pcm_bytes = w_count;
    s_last_gen = a_s->gen;

    os_sema_up(&s_wav_done_sem);

    a_s->running = 0;
    custom_free((void *)a_s);
    audio_s = NULL;
}

/* ============================================================
 * API：start/stop/wait
 * ============================================================ */
int key_wav_record_start(uint32_t frq, const char *prefix, uint32_t max_minute)
{
    if (audio_s) {
        os_printf("[wav] %s already running\r\n", __FUNCTION__);
        return -1;
    }

    wechat_voice_worker_init();

    audio_s = (struct KEY_AUDIO *)custom_malloc(sizeof(struct KEY_AUDIO));
    if (!audio_s) {
        os_printf("[wav] %s malloc fail\r\n", __FUNCTION__);
        return -2;
    }
    os_memset(audio_s, 0, sizeof(struct KEY_AUDIO));

    audio_s->frq = frq;

    if (max_minute == 0) audio_s->minute = ~0U;
    else                 audio_s->minute = max_minute;

    if (prefix && prefix[0]) {
        int len = os_strlen(prefix);
        if (len > 3) len = 3;
        os_memcpy(audio_s->filename_prefix, prefix, len);
    } else {
        os_memcpy(audio_s->filename_prefix, "def", 3);
    }

    /* gen */
    audio_s->gen = ++s_rec_gen;
    s_active_gen = audio_s->gen;

    /* running 在 start 里置 1，避免线程启动后覆盖 stop */
    audio_s->running = 1;

    /* 清空本次结果 */
    os_memset(s_last_wav_path, 0, sizeof(s_last_wav_path));
    s_last_pcm_bytes = 0;
    s_last_gen = 0;

    OS_TASK_INIT("key_audio",
                 &audio_s->task,
                 key_save_audio_thread,
                 (uint32)audio_s,
                 OS_TASK_PRIORITY_NORMAL,
                 1024);

    os_printf("[wav] start ok gen=%u frq=%u\r\n", (unsigned)s_active_gen, (unsigned)frq);
    return 0;
}

void key_wav_record_stop(void)
{
    if (!audio_s) return;
    audio_s->running = 0;
}

/* 等待编号为 gen 的录音线程把 WAV写完*/
int key_wav_record_stop_wait_ex(uint32_t expect_gen,
                                char *out_path, uint32_t out_len,
                                uint32_t *out_pcm_bytes,
                                int32_t timeout_ms)
{
    if (!s_wav_sem_inited) return -1;

    int32_t remain = timeout_ms;
    uint32_t t0 = os_jiffies();

    while (1) {
        int ret = os_sema_down(&s_wav_done_sem, remain);
        if (ret < 0) return -2;

        if (s_last_gen == expect_gen && s_last_wav_path[0] != '\0') {
            if (out_path && out_len) {
                os_strncpy(out_path, s_last_wav_path, out_len - 1);
                out_path[out_len - 1] = '\0';
            }
            if (out_pcm_bytes) *out_pcm_bytes = s_last_pcm_bytes;
            return 0;
        }

        /* 不是我要的 gen：忽略继续等 */
        if (timeout_ms >= 0) {
            uint32_t now = os_jiffies();
            int32_t used = (int32_t)(now - t0);
            remain = timeout_ms - used;
            if (remain <= 0) return -3;
        }
    }
}

/* 兼容旧接口：等待当前 s_active_gen */
int key_wav_record_stop_wait(char *out_path, uint32_t out_len,
                             uint32_t *out_pcm_bytes,
                             int32_t timeout_ms)
{
    return key_wav_record_stop_wait_ex(s_active_gen,
                                       out_path, out_len,
                                       out_pcm_bytes,
                                       timeout_ms);
}

/* ============================================================
 * worker：等待存储完成 -> 发送 -> UI pending 气泡
 * ============================================================ */
static void wechat_voice_worker_thread(void *arg)
{
    (void)arg;
    wechat_voice_job_t job;

    os_printf("[wechat] voice worker start\r\n");

    while (1) {
        os_sema_down(&s_job_sem, -1);

        while (wechat_voice_job_pop(&job) == 0) {
            char     wav_path[64] = {0};
            uint32_t pcm_bytes = 0;
            uint16_t msg_id = 0;
            uint8_t  sec = 0;

            int wav_ret = key_wav_record_stop_wait_ex(job.gen,
                                                      wav_path, sizeof(wav_path),
                                                      &pcm_bytes,
                                                      5000);
            if (wav_ret != 0) {
                os_printf("[wechat] worker fail: gen=%u wav_ret=%d\r\n",
                          (unsigned)job.gen, wav_ret);
                continue;
            }

            if (pcm_bytes == 0 || wav_path[0] == '\0') {
                os_printf("[wechat] worker drop: gen=%u pcm_bytes=0 path=%s\r\n",
                          (unsigned)job.gen, wav_path);
                continue;
            }

            /* 1) 先分配 msg_id（这样可以先显示） */
            msg_id = wechat_msg_alloc_id();

            /* 2) 先算 sec（不要依赖 send 返回） */
            {
                uint32_t bytes_per_sec = job.frq * 2; // 16bit mono
                uint32_t s = bytes_per_sec ? ((pcm_bytes + bytes_per_sec - 1) / bytes_per_sec) : 1;
                if (s < 1) s = 1;
                if (s > 60) s = 60;      // 或 PCM_MAX_SEC
                sec = (uint8_t)s;
            }

            /* 3) 先显示 pending 气泡（UI 线程去真正创建） */
            wechat_set_pending_voice_bubble(msg_id, sec, wav_path);
			
			/* 4) 发送前再做一次离线保护：录音过程中掉线的情况 */
			if (!wechat_guard_action(WECHAT_ACT_RECORD)) {
				os_printf("[wechat] worker offline, skip send: gen=%u msg_id=%u\r\n",
						  (unsigned)job.gen, (unsigned)msg_id);
				continue; // 不发送，后续你可以扩展为“失败状态”或“待重试”
			}

            /* 5) 再后台发送（失败也不要影响“已显示”） */
            {
                int send_ret = wechat_send_wav_as_voice_msg_ex(job.frq, wav_path, msg_id, sec);
                if (send_ret != 0) {
                    os_printf("[wechat] worker send fail: gen=%u msg_id=%u ret=%d\r\n",
                              (unsigned)job.gen, (unsigned)msg_id, send_ret);
                    /* 可选：这里你以后加 wechat_ui_post_voice_state(msg_id, FAIL) */
                    continue;
                }
            }

            os_printf("[wechat] worker ok: gen=%u msg_id=%u sec=%u file=%s bytes=%u\r\n",
                      (unsigned)job.gen, (unsigned)msg_id, (unsigned)sec,
                      wav_path, (unsigned)pcm_bytes);
        }
    }
}


/* ============================================================
 * PTT 按键回调：LDOWN start；LUP stop + 异步 job
 * ============================================================ */
static uint8_t s_key2_recording = 0;

static uint32_t wechat_ptt_key_cb(struct key_callback_list_s *callback_list,
                                 uint32_t keyvalue,
                                 uint32_t extern_value)
{
    (void)callback_list;
    (void)extern_value;

    if (g_video_intercom_mode) return 0;
    if (camera_gvar.page_cur != PAGE_WECHAT) return 0;
    if (s_wechat_focus_idx != 1) return 0;
    if ((keyvalue >> 8) != KEY_CALL) return 0;

    uint32_t key_val = (keyvalue & 0xff);

    if (key_val == KEY_EVENT_LDOWN) {
        if (!s_key2_recording) {
			        
			/* 离线拦截：不允许录音/发送类动作 */
			if (!wechat_guard_action(WECHAT_ACT_RECORD)) {
				return 0;
			}
			
            s_key2_recording = 1;

            /* 让音频链路不要 digit mute */
            rec_open = 1;

            /* 切通路：关喇叭、停播放、开 MIC */
            mute_speaker(1);
            audio_dac_set_filter_type(SOUND_NONE);
            audio_adc_unmute();

            if (key_wav_record_start(SAMPLERATE, "wct", 0) != 0) {
                os_printf("[wechat] key_wav_record_start fail\r\n");
                /* 失败的话恢复一下，避免卡状态 */
                audio_adc_mute();
                mute_speaker(0);
                audio_dac_set_filter_type(SOUND_INTERCOM);
                rec_open = 0;
                s_key2_recording = 0;
            } else {
                os_printf("[wechat] record start gen=%u\r\n", (unsigned)s_active_gen);
            }
			wechat_record_ui_request(1);
        }
    }
    else if (key_val == KEY_EVENT_LUP) {
        if (s_key2_recording) {
            s_key2_recording = 0;

            /* 1) 让录音线程退出（不等待） */
            key_wav_record_stop();

            /* 2) 立即恢复音频路径（不阻塞） */
            audio_adc_mute();
            mute_speaker(0);
            audio_dac_set_filter_type(SOUND_INTERCOM);
            rec_open = 0;

            /* 3) 把 “等待落盘+发送+UI绑定” 交给 worker */
            (void)wechat_voice_job_push(SAMPLERATE, s_active_gen);
			wechat_record_ui_request(0);
            os_printf("[wechat] record stop async gen=%u\r\n", (unsigned)s_active_gen);
        }
		
    }

    return 0;
}

void wechat_ptt_key_init(void)
{
    wechat_voice_worker_init();
    add_keycallback(wechat_ptt_key_cb, NULL);
}