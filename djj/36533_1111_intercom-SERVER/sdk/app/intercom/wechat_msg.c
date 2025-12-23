
#include "wechat_msg.h"

#include "sys_config.h"
#include "typesdef.h"
#include "osal/task.h"
#include "osal/mutex.h"
#include "osal/semaphore.h"
#include "osal/string.h"
#include "custom_mem/custom_mem.h"
#include "osal_file.h"
uint16_t wechat_msg_alloc_id(void);
extern uint8_t get_wifi_connect_flag(void);
/*******************************/
#define DBG_BYTES 20

static int is_dbg_frame_u16(uint16_t seq, uint16_t total)
{
	return (seq == 0 || seq == 1 || seq == 2 || seq == (uint16_t)(total - 1));
}

static void dump_first_hex_n(const char *tag, const uint8_t *buf, uint32_t len, uint32_t n)
{
    if (!buf || len == 0) return;
    if (n > len) n = len;

    printf("[DBG] %s len=%u first=%u : ", tag, (unsigned)len, (unsigned)n);
    for (uint32_t i = 0; i < n; i++) printf("%02X ", buf[i]);
    printf("\r\n");
}
/*******************************/

/* 复用 intercom(5008) 扩展包发送接口 */
extern int intercom_chat_send_raw(const uint8_t *payload, uint16_t len);

#define WECHAT_LOG(fmt, ...) os_printf("[WECHAT_MSG] " fmt "\r\n", ##__VA_ARGS__)

/* 发送队列大小 */
#define WECHAT_TX_QUEUE_SIZE    64

typedef struct {
    uint16_t len;
    uint8_t  buf[sizeof(wechat_msg_hdr_t) + WECHAT_MSG_MAX_PAYLOAD];
} wechat_tx_node_t;

static wechat_tx_node_t s_tx_queue[WECHAT_TX_QUEUE_SIZE];
static uint16_t         s_tx_head = 0;
static uint16_t         s_tx_tail = 0;
static struct os_mutex  s_tx_mutex;
static struct os_semaphore s_tx_sem;

static struct os_task s_wechat_send_task;

static uint8_t  s_inited = 0;
static uint16_t s_msg_id_counter = 1;

static uint16_t wechat_msg_next_id(void)
{
    if (s_msg_id_counter == 0xFFFF) s_msg_id_counter = 1;
    return s_msg_id_counter++;
}

/* 组包打印 */
static void wechat_dump_hex(const char *tag, const uint8_t *buf, uint16_t len)
{
    if (!buf || len == 0) return;

    printf("[WECHAT_MSG] %s len=%u\r\n", tag, (unsigned)len);

    for (uint16_t i = 0; i < len; i++) {
        if ((i % 16) == 0) {
            printf("[WECHAT_MSG] %04u: ", (unsigned)i);
        }
        printf("%02X ", buf[i]);

        if ((i % 16) == 15 || i == (uint16_t)(len - 1)) {
            printf("\r\n");
        }
    }
}

/* 发送队列 push */
static int wechat_tx_queue_push(const uint8_t *data, uint16_t len)
{
    if (!data || len == 0) return -1;
    if (len > sizeof(s_tx_queue[0].buf)) return -2;

    if (os_mutex_lock(&s_tx_mutex, -1) < 0) return -3;

    uint16_t next_tail = (uint16_t)((s_tx_tail + 1) % WECHAT_TX_QUEUE_SIZE);
    if (next_tail == s_tx_head) {
        os_mutex_unlock(&s_tx_mutex);
        WECHAT_LOG("tx queue full, drop");
        return -4;
    }

    s_tx_queue[s_tx_tail].len = len;
    os_memcpy(s_tx_queue[s_tx_tail].buf, data, len);
    s_tx_tail = next_tail;

    os_mutex_unlock(&s_tx_mutex);
    os_sema_up(&s_tx_sem);
    return 0;
}

static int wechat_tx_queue_pop(wechat_tx_node_t *out)
{
    if (!out) return -1;

    if (os_mutex_lock(&s_tx_mutex, -1) < 0) return -2;

    if (s_tx_head == s_tx_tail) {
        os_mutex_unlock(&s_tx_mutex);
        return -3;
    }

    *out = s_tx_queue[s_tx_head];
    s_tx_head = (uint16_t)((s_tx_head + 1) % WECHAT_TX_QUEUE_SIZE);

    os_mutex_unlock(&s_tx_mutex);
    return 0;
}

/* 发送线程：出队 -> intercom_chat_send_raw() */
static void wechat_msg_send_thread(void *arg)
{
    (void)arg;
    wechat_tx_node_t node;

    WECHAT_LOG("send thread start");


//    while (1) {
//        os_sema_down(&s_tx_sem, -1);
//
//        while (wechat_tx_queue_pop(&node) == 0) {
//            int ret = intercom_chat_send_raw(node.buf, node.len);
//            if (ret != 0) {
//                WECHAT_LOG("intercom_chat_send_raw fail ret=%d len=%u", ret, node.len);
//                os_sleep_ms(2);
//            }
//        }
//    }
}

int wechat_msg_init(void)
{
    if (s_inited) return 0;
    s_inited = 1;

    os_mutex_init(&s_tx_mutex);
    os_sema_init(&s_tx_sem, 0);

    OS_TASK_INIT("wechat_msg_send",
                 &s_wechat_send_task,
                 wechat_msg_send_thread,
                 NULL,
                 OS_TASK_PRIORITY_NORMAL,
                 2048);

    WECHAT_LOG("init ok (reuse intercom port)");
    return 0;
}

/* intercom 收到聊天扩展包后调用这里 */
void wechat_msg_on_rx_from_intercom(const uint8_t *data, uint16_t len)
{
    if (!data || len < sizeof(wechat_msg_hdr_t)) return;

    const wechat_msg_hdr_t *hdr = (const wechat_msg_hdr_t *)data;
    if (hdr->magic != 0x57) {
        WECHAT_LOG("invalid magic=0x%02X", hdr->magic);
        return;
    }

    if (hdr->data_len > WECHAT_MSG_MAX_PAYLOAD) {
        WECHAT_LOG("invalid data_len=%u", (unsigned)hdr->data_len);
        return;
    }

    uint16_t need = (uint16_t)(sizeof(wechat_msg_hdr_t) + hdr->data_len);
    if (len < need) {
        WECHAT_LOG("short packet len=%u need=%u", len, need);
        return;
    }

    const uint8_t *payload = data + sizeof(wechat_msg_hdr_t);

    switch (hdr->type) {
    case WECHAT_MSG_VOICE:
        wechat_on_voice_packet_ex(hdr->msg_id, hdr->seq, hdr->total, hdr->sec, payload, hdr->data_len);
        break;
    case WECHAT_MSG_IMAGE:
        wechat_on_image_packet(hdr->msg_id, hdr->seq, hdr->total, payload, hdr->data_len);
        break;
    case WECHAT_MSG_EMOJI:
        wechat_on_emoji_packet(hdr->msg_id, hdr->seq, hdr->total, payload, hdr->data_len);
        break;
    default:
        WECHAT_LOG("unknown type=%u", hdr->type);
        break;
    }
}

/* ============ 发送 API ============ */

int wechat_emoji_send(uint16_t emoji_id)
{
	printf("wechat_emoji_send start!\r\n");
	    if (!get_wifi_connect_flag()) {
        printf("[WECHAT_MSG] offline, skip emoji send\n");
        return -1;
    }
	
    if (!s_inited) wechat_msg_init();

    uint8_t buf[sizeof(wechat_msg_hdr_t) + 2];
    wechat_msg_hdr_t *hdr = (wechat_msg_hdr_t *)buf;
    uint8_t *payload = buf + sizeof(wechat_msg_hdr_t);

    hdr->magic    = 0x57;
    hdr->type     = WECHAT_MSG_EMOJI;
    hdr->msg_id   = wechat_msg_next_id();
    hdr->seq      = 0;
    hdr->total    = 1;
    hdr->data_len = 2;
    hdr->sec      = 0;
    hdr->rsv[0]   = WECHAT_MSG_VER;
    hdr->rsv[1]   = 0;
    hdr->rsv[2]   = 0;

    payload[0] = (uint8_t)(emoji_id & 0xFF);
    payload[1] = (uint8_t)((emoji_id >> 8) & 0xFF);

    uint16_t total_len = (uint16_t)(sizeof(wechat_msg_hdr_t) + 2);

    WECHAT_LOG("emoji_send: msg_id=%u emoji_id=%u", hdr->msg_id, emoji_id);
    wechat_dump_hex("emoji packet", buf, total_len);

	int ret = intercom_chat_send_raw(buf, total_len);
	WECHAT_LOG("emoji_send ret=%d len=%u", ret, total_len);
	return ret;
	
}


#define WECHAT_IMAGE_SEND_GAP_MS   1  
int wechat_send_image_file_as_msg(const char *img_path,
                                  uint16_t *out_msg_id,
                                  uint32_t *out_size)
{
    if (!img_path || !img_path[0]) return -1;
    if (!s_inited) wechat_msg_init();
    
	if (!get_wifi_connect_flag()) {
        WECHAT_LOG("offline, skip image send");
        return -10;
    }
	
    void *fp = osal_fopen(img_path, "rb");
    if (!fp) {
        WECHAT_LOG("open img fail: %s", img_path);
        return -2;
    }

    /* 取文件大小：如果你没有 osal_fsize(fp) 这种形式，就换成你工程里对应接口 */
    uint32_t fsz = (uint32_t)osal_fsize(fp);
    if (fsz == 0) {
        osal_fclose(fp);
        WECHAT_LOG("img size=0: %s", img_path);
        return -3;
    }

    uint16_t msg_id = wechat_msg_next_id();
    uint16_t total  = (uint16_t)((fsz + WECHAT_MSG_MAX_PAYLOAD - 1) / WECHAT_MSG_MAX_PAYLOAD);

    if (out_msg_id) *out_msg_id = msg_id;
    if (out_size)   *out_size   = fsz;

    uint32_t sent = 0;
    uint16_t seq  = 0;

    while (sent < fsz) {
        uint32_t chunk = fsz - sent;
        if (chunk > WECHAT_MSG_MAX_PAYLOAD) chunk = WECHAT_MSG_MAX_PAYLOAD;

        uint8_t buf[sizeof(wechat_msg_hdr_t) + WECHAT_MSG_MAX_PAYLOAD];
        wechat_msg_hdr_t *hdr = (wechat_msg_hdr_t *)buf;
        uint8_t *payload = buf + sizeof(wechat_msg_hdr_t);

        int rb = osal_fread(payload, 1, (int)chunk, fp);
        if (rb != (int)chunk) {
            WECHAT_LOG("read img short: want=%u got=%d path=%s", (unsigned)chunk, rb, img_path);
            osal_fclose(fp);
            return -4;
        }

        hdr->magic    = 0x57;
        hdr->type     = WECHAT_MSG_IMAGE;
        hdr->msg_id   = msg_id;
        hdr->seq      = seq;
        hdr->total    = total;
        hdr->data_len = (uint32_t)rb;
        hdr->sec      = 0;
        hdr->rsv[0]   = WECHAT_MSG_VER;
        hdr->rsv[1]   = 0;
        hdr->rsv[2]   = 0;

        uint16_t total_len = (uint16_t)(sizeof(wechat_msg_hdr_t) + (uint16_t)rb);
		if (is_dbg_frame_u16(seq, total)) {
			char tag1[96];
			os_sprintf(tag1, "IMG RD  msg=%u seq=%u/%u payload20",
					   (unsigned)msg_id, (unsigned)seq, (unsigned)total);
			dump_first_hex_n(tag1, payload, (uint32_t)rb, DBG_BYTES);

			char tag2[96];
			os_sprintf(tag2, "IMG PKT msg=%u seq=%u/%u hdr+20",
					   (unsigned)msg_id, (unsigned)seq, (unsigned)total);
			dump_first_hex_n(tag2, buf, (uint32_t)total_len,
							 (uint32_t)(sizeof(wechat_msg_hdr_t) + DBG_BYTES));
		}
//        /* 入队发送（你现在 send thread 里没真正发也没关系，先验证打包/收端组装） */
//        if (wechat_tx_queue_push(buf, total_len) < 0) {
//            WECHAT_LOG("tx queue full while send img");
//            osal_fclose(fp);
//            return -5;
//        }       
		/* 同步直发 */
        int ret = intercom_chat_send_raw(buf, total_len);
        if (ret != 0) {
            WECHAT_LOG("img pkt send fail ret=%d seq=%u/%u len=%u",
                       ret, (unsigned)seq, (unsigned)total, (unsigned)total_len);
            osal_fclose(fp);
            return -6;
        }

        /* 打关键帧，便于你对照收端组包 */
        if (is_dbg_frame_u16(seq, total)) {
            char tag2[96];
            os_sprintf(tag2, "IMG PKT msg=%u seq=%u/%u hdr+20",
                       (unsigned)msg_id, (unsigned)seq, (unsigned)total);
            dump_first_hex_n(tag2, buf, (uint32_t)total_len,
                             (uint32_t)(sizeof(wechat_msg_hdr_t) + DBG_BYTES));
        }

        sent += (uint32_t)rb;
        seq++;
		
		os_sleep_ms(WECHAT_IMAGE_SEND_GAP_MS);
    }

    osal_fclose(fp);

    WECHAT_LOG("send image done: msg_id=%u bytes=%u pkts=%u path=%s",
               msg_id, (unsigned)fsz, (unsigned)total, img_path);

    return 0;
}
#define WECHAT_VOICE_SEND_GAP_MS   1

int wechat_send_wav_as_voice_msg_ex(uint32_t frq,
                                    const char *wav_path,
                                    uint16_t msg_id,
                                    uint8_t  sec)
{
    if (!wav_path || !wav_path[0]) return -1;
    if (!s_inited) wechat_msg_init();
    
	if (!get_wifi_connect_flag()) {
        WECHAT_LOG("offline, skip voice send");
        return -10;
    }
	
    void *fp = osal_fopen(wav_path, "rb");
    if (!fp) return -2;

    WaveHeader wh;
    int rd = osal_fread(&wh, 1, sizeof(wh), fp);
    if (rd != (int)sizeof(wh)) { osal_fclose(fp); return -3; }

    uint32_t pcm_bytes = (uint32_t)wh.dwDATALen;
    uint16_t total = (uint16_t)((pcm_bytes + WECHAT_MSG_MAX_PAYLOAD - 1) / WECHAT_MSG_MAX_PAYLOAD);

    uint16_t seq = 0;
    uint32_t sent = 0;

    while (sent < pcm_bytes) {
        uint32_t chunk = pcm_bytes - sent;
        if (chunk > WECHAT_MSG_MAX_PAYLOAD) chunk = WECHAT_MSG_MAX_PAYLOAD;

        uint8_t buf[sizeof(wechat_msg_hdr_t) + WECHAT_MSG_MAX_PAYLOAD];
        wechat_msg_hdr_t *hdr = (wechat_msg_hdr_t *)buf;
        uint8_t *payload = buf + sizeof(wechat_msg_hdr_t);

        int rb = osal_fread(payload, 1, (int)chunk, fp);
        if (rb != (int)chunk) { osal_fclose(fp); return -4; }

        hdr->magic    = 0x57;
        hdr->type     = WECHAT_MSG_VOICE;
        hdr->msg_id   = msg_id;
        hdr->seq      = seq;
        hdr->total    = total;
        hdr->data_len = (uint32_t)rb;
        hdr->sec      = sec;
        hdr->rsv[0]   = WECHAT_MSG_VER;
        hdr->rsv[1]   = 0;
        hdr->rsv[2]   = 0;

        uint16_t total_len = (uint16_t)(sizeof(wechat_msg_hdr_t) + (uint16_t)rb);
		
		// 关键：同步直发
        int ret = intercom_chat_send_raw(buf, total_len);
        if (ret != 0) {
            WECHAT_LOG("voice pkt send fail ret=%d seq=%u/%u len=%u",
                       ret, (unsigned)seq, (unsigned)total, (unsigned)total_len);
            osal_fclose(fp);
            return -6;
        }

        // 验证阶段：打几个关键帧的 hex（可选）
        if (is_dbg_frame_u16(seq, total)) {
            char tag[96];
            os_sprintf(tag, "VOICE PKT msg=%u seq=%u/%u hdr+20",
                       (unsigned)msg_id, (unsigned)seq, (unsigned)total);
            dump_first_hex_n(tag, buf, (uint32_t)total_len,
                             (uint32_t)(sizeof(wechat_msg_hdr_t) + DBG_BYTES));
        }


        /* 真正发送：你按需打开（建议走队列 + send_thread） */
        // if (wechat_tx_queue_push(buf, total_len) < 0) { osal_fclose(fp); return -5; }

        sent += (uint32_t)rb;
        seq++;
		
		os_sleep_ms(WECHAT_VOICE_SEND_GAP_MS);
    }

    osal_fclose(fp);
	WECHAT_LOG("voice send done msg=%u", msg_id);
    return 0;
}
static uint8_t wechat_calc_sec_from_wav(const WaveHeader *wh)
{
    if (!wh) return 1;
    uint32_t bps = (uint32_t)wh->fmt_bytepsec;
    uint32_t pcm = (uint32_t)wh->dwDATALen;
    if (bps == 0) return 1;
    uint32_t sec = (pcm + bps - 1) / bps;
    if (sec == 0) sec = 1;
    if (sec > 255) sec = 255;
    return (uint8_t)sec;
}


/* 旧接口保留：内部 alloc id + 算 sec + 调 _ex */
int wechat_send_wav_as_voice_msg(uint32_t frq,
                                 const char *wav_path,
                                 uint16_t *out_msg_id,
                                 uint8_t  *out_sec)
{
    /* 这里沿用你原本算 sec 的方式也行；我给个简单的：先从文件头拿 pcm_bytes 算 */
    uint16_t msg_id = wechat_msg_alloc_id();

    /* 先给个默认，真正 sec 建议在 worker 用 pcm_bytes 算好传进来 */
    uint8_t sec = 1;

    if (out_msg_id) *out_msg_id = msg_id;
    if (out_sec)   *out_sec = sec;

    return wechat_send_wav_as_voice_msg_ex(frq, wav_path, msg_id, sec);
}



//找一个不重名的文件名
static uint32_t wechat_find_free_index(const char *prefix, const char *ext)
{
    char path[64];
    void *fp;
    for (uint32_t i = 1; i <= 9999; i++) {
        os_sprintf(path, WECHAT_RX_WAV_DIR "/%s%04u.%s", prefix, (unsigned)i, ext);
        fp = osal_fopen(path, "rb");
        if (!fp) return i;
        osal_fclose(fp);
    }
    return 1;
}

static wechat_rx_voice_ctx_t s_rx_voice;

static void wechat_rx_voice_abort(void)
{
    if (s_rx_voice.fp) {
        osal_fclose(s_rx_voice.fp);
        s_rx_voice.fp = NULL;
    }
    s_rx_voice.active = 0;
    s_rx_voice.msg_id = 0;
    s_rx_voice.total  = 0;
    s_rx_voice.expect_seq = 0;
    s_rx_voice.sec = 0;
    s_rx_voice.pcm_bytes = 0;
    s_rx_voice.path[0] = '\0';
}



void wechat_on_voice_packet_ex(uint16_t msg_id,
                               uint16_t seq,
                               uint16_t total,
                               uint8_t  sec,
                               const uint8_t *data,
                               uint32_t len)
{
    if (!data || len == 0 || total == 0) return;

    /* 新消息起始：必须从 seq=0 开始 */
    if (!s_rx_voice.active) {
        if (seq != 0) return;

        uint32_t idx = wechat_find_free_index("rcv", "wav");
        os_sprintf(s_rx_voice.path, WECHAT_RX_WAV_DIR "/rcv_%04u.wav", (unsigned)idx);

        void *fp = osal_fopen(s_rx_voice.path, "wb+");
        if (!fp) {
            WECHAT_LOG("rx voice fopen fail: %s", s_rx_voice.path);
            wechat_rx_voice_abort();
            return;
        }

        /* 写占位 WAV 头 */
        WaveHeader wh;
        os_memset(&wh, 0, sizeof(wh));
        os_memcpy(&wh, wav_header, sizeof(WaveHeader));

        wh.fmt_pcm        = 1;
        wh.channels       = 1;
        wh.fmt_samplehz   = WECHAT_RX_WAV_FRQ;
        wh.fmt_bitpsample = 16;
        wh.fmt_bytesample = wh.channels * (wh.fmt_bitpsample / 8);
        wh.fmt_bytepsec   = wh.fmt_samplehz * wh.fmt_bytesample;
        wh.dwDATALen      = 0;
        wh.total_Len      = sizeof(WaveHeader) - 8;

        int hw = osal_fwrite(&wh, 1, sizeof(wh), fp);
        if (hw != (int)sizeof(wh)) {
            osal_fclose(fp);
            WECHAT_LOG("rx voice write header fail");
            wechat_rx_voice_abort();
            return;
        }

        s_rx_voice.active = 1;
        s_rx_voice.fp     = fp;
        s_rx_voice.msg_id = msg_id;
        s_rx_voice.total  = total;
        s_rx_voice.expect_seq = 0;
        s_rx_voice.sec    = sec;
        s_rx_voice.pcm_bytes = 0;

        WECHAT_LOG("rx voice start msg=%u total=%u sec=%u file=%s",
                   msg_id, total, sec, s_rx_voice.path);
    }

    /* 校验同一条消息 */
    if (!s_rx_voice.active || s_rx_voice.msg_id != msg_id) {
        /* 收到新 msg_id 但没从0开始：丢弃；如果 seq==0 则重开 */
        if (seq == 0) {
            wechat_rx_voice_abort();
            wechat_on_voice_packet_ex(msg_id, seq, total, sec, data, len);
        }
        return;
    }

    /* 顺序写：只接受 expect_seq */
    if (seq < s_rx_voice.expect_seq) {
        /* 重复包，忽略 */
        return;
    }
    if (seq != s_rx_voice.expect_seq) {
        /* 乱序/丢包：本版本先直接 abort（后续你再加重传/offset写） */
        WECHAT_LOG("rx voice out-of-order msg=%u expect=%u got=%u -> abort",
                   msg_id, s_rx_voice.expect_seq, seq);
        wechat_rx_voice_abort();
        return;
    }

    /* 写 PCM 分片 */
    int wr = osal_fwrite((void*)data, 1, (int)len, s_rx_voice.fp);
    if (wr != (int)len) {
        WECHAT_LOG("rx voice write short msg=%u want=%u got=%d", msg_id, (unsigned)len, wr);
        wechat_rx_voice_abort();
        return;
    }

    s_rx_voice.pcm_bytes += len;
    s_rx_voice.expect_seq++;

    /* 完成：回填头 + close + 通知 UI 或写日志 */
    if (s_rx_voice.expect_seq >= s_rx_voice.total) {
        WaveHeader wh;
        os_memset(&wh, 0, sizeof(wh));
        os_memcpy(&wh, wav_header, sizeof(WaveHeader));
        wh.fmt_pcm        = 1;
        wh.channels       = 1;
        wh.fmt_samplehz   = WECHAT_RX_WAV_FRQ;
        wh.fmt_bitpsample = 16;
        wh.fmt_bytesample = wh.channels * (wh.fmt_bitpsample / 8);
        wh.fmt_bytepsec   = wh.fmt_samplehz * wh.fmt_bytesample;

        wh.dwDATALen = s_rx_voice.pcm_bytes;
        wh.total_Len = s_rx_voice.pcm_bytes + sizeof(WaveHeader) - 8;

        osal_fseek(s_rx_voice.fp, 0);
        (void)osal_fwrite(&wh, 1, sizeof(wh), s_rx_voice.fp);
        osal_fclose(s_rx_voice.fp);
        s_rx_voice.fp = NULL;

        WECHAT_LOG("rx voice done msg=%u bytes=%u file=%s",
                   msg_id, (unsigned)s_rx_voice.pcm_bytes, s_rx_voice.path);

		/* 先写日志（总是） */
		wechat_history_append_voice_public(WEICHAT_MSG_FROM_PEER,
										  s_rx_voice.sec,
										  msg_id,
										  s_rx_voice.path);

		/* 若在微信页：投递 UI 左侧显示（already_logged=1） */
		if (wechat_ui_can_render_now()) {
			(void)wechat_ui_post_voice_ex(WEICHAT_MSG_FROM_PEER, msg_id, s_rx_voice.sec,
										 s_rx_voice.path, 1);
		}
        wechat_rx_voice_abort();
    }
}



static wechat_rx_img_ctx_t s_rx_img;

static void wechat_rx_img_abort(void)
{
    if (s_rx_img.fp) {
        osal_fclose(s_rx_img.fp);
        s_rx_img.fp = NULL;
    }
    s_rx_img.active = 0;
    s_rx_img.msg_id = 0;
    s_rx_img.total  = 0;
    s_rx_img.expect_seq = 0;
    s_rx_img.bytes = 0;
    s_rx_img.path[0] = '\0';
}

void wechat_on_image_packet(uint16_t msg_id,
                            uint16_t seq,
                            uint16_t total,
                            const uint8_t *data,
                            uint32_t len)
{
    if (!data || len == 0 || total == 0) return;

    if (!s_rx_img.active) {
        if (seq != 0) return;

		uint32_t idx = wechat_find_free_index("JPEG", "JPG");
		os_sprintf(s_rx_img.path, WECHAT_RX_WAV_DIR "/JPEG%04u.JPG", (unsigned)idx);


        void *fp = osal_fopen(s_rx_img.path, "wb+");
        if (!fp) {
            WECHAT_LOG("rx img fopen fail: %s", s_rx_img.path);
            wechat_rx_img_abort();
            return;
        }

        s_rx_img.active = 1;
        s_rx_img.fp = fp;
        s_rx_img.msg_id = msg_id;
        s_rx_img.total  = total;
        s_rx_img.expect_seq = 0;
        s_rx_img.bytes = 0;

        WECHAT_LOG("rx img start msg=%u total=%u file=%s", msg_id, total, s_rx_img.path);
    }

    if (!s_rx_img.active || s_rx_img.msg_id != msg_id) {
        if (seq == 0) {
            wechat_rx_img_abort();
            wechat_on_image_packet(msg_id, seq, total, data, len);
        }
        return;
    }

    if (seq < s_rx_img.expect_seq) return;  /* dup */
    if (seq != s_rx_img.expect_seq) {
        WECHAT_LOG("rx img out-of-order msg=%u expect=%u got=%u -> abort",
                   msg_id, s_rx_img.expect_seq, seq);
        wechat_rx_img_abort();
        return;
    }

    int wr = osal_fwrite((void*)data, 1, (int)len, s_rx_img.fp);
    if (wr != (int)len) {
        WECHAT_LOG("rx img write short msg=%u want=%u got=%d", msg_id, (unsigned)len, wr);
        wechat_rx_img_abort();
        return;
    }
	
    s_rx_img.bytes += len;
    s_rx_img.expect_seq++;

    if (s_rx_img.expect_seq >= s_rx_img.total) {
        osal_fclose(s_rx_img.fp);
        s_rx_img.fp = NULL;

        WECHAT_LOG("rx img done msg=%u bytes=%u file=%s",
                   msg_id, (unsigned)s_rx_img.bytes, s_rx_img.path);
		
		wechat_history_append_photo_public(WEICHAT_MSG_FROM_PEER, s_rx_img.path);
		
		if (wechat_ui_can_render_now()) {
			(void)wechat_ui_post_photo_ex(WEICHAT_MSG_FROM_PEER, s_rx_img.path, 1);
		}

        wechat_rx_img_abort();
    }
}


uint16_t wechat_msg_alloc_id(void)
{
    if (!s_inited) wechat_msg_init();
    return wechat_msg_next_id();
}