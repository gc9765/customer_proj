#ifndef __WECHAT_MSG_H__
#define __WECHAT_MSG_H__

#include <stdint.h>
#include "key2_wav_record.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 消息类型 */
typedef enum {
    WECHAT_MSG_VOICE = 1,   /* 语音(文件分片) */
    WECHAT_MSG_IMAGE = 2,   /* 图片(文件分片) */
    WECHAT_MSG_EMOJI = 3,   /* 表情(单包) */
} wechat_msg_type_t;

#define EMOJI_COUNT     12
typedef enum {
    WECHAT_EMOJI_SMILE = 0,
    WECHAT_EMOJI_GRIN,
    WECHAT_EMOJI_LAUGH,
    WECHAT_EMOJI_ROFL,
    WECHAT_EMOJI_BLUSH,
    WECHAT_EMOJI_LOVE,
    WECHAT_EMOJI_KISS,
    WECHAT_EMOJI_COOL,
    WECHAT_EMOJI_CRY,
    WECHAT_EMOJI_ANGRY,
    WECHAT_EMOJI_THUMB,
    WECHAT_EMOJI_PRAY,
    WECHAT_EMOJI_MAX
} wechat_emoji_id_t;

/* payload 最大长度（含头） */

/* 协议版本 */
#define WECHAT_MSG_VER           2

#pragma pack(push, 1)
typedef struct {
    uint8_t  magic;       /* 固定 0x57 'W' */
    uint8_t  type;        /* wechat_msg_type_t */
    uint16_t msg_id;      /* 一条消息 ID */
    uint16_t seq;         /* 当前分片序号，从 0 开始 */
    uint16_t total;       /* 总分片数 */
    uint32_t data_len;    /* 本包 payload 字节数 */
    uint8_t  sec;         /* 语音总秒数(1~60)，非语音=0 */
    uint8_t  rsv[3];      /* 保留：建议 rsv[0]=WECHAT_MSG_VER */
} wechat_msg_hdr_t;
#pragma pack(pop)

#define WECHAT_UDP_RX_LIMIT      270
#define WECHAT_MSG_MAX_PAYLOAD   (WECHAT_UDP_RX_LIMIT - (uint16_t)sizeof(wechat_msg_hdr_t))


typedef struct {
    uint8_t  active;
    uint16_t msg_id;
    uint16_t total;
    uint16_t expect_seq;
    uint8_t  sec;

    void    *fp;
    char     path[64];
    uint32_t pcm_bytes;
} wechat_rx_voice_ctx_t;

typedef struct {
    uint8_t  active;
    uint16_t msg_id;
    uint16_t total;
    uint16_t expect_seq;

    void    *fp;
    char     path[64];
    uint32_t bytes;
} wechat_rx_img_ctx_t;

/* 初始化：创建发送队列 + 发送线程（不再创建 socket、不再 bind 端口） */
int wechat_msg_init(void);

/* intercom 收到 “聊天扩展包” 后，把 payload 丢进来解析 */
void wechat_msg_on_rx_from_intercom(const uint8_t *data, uint16_t len);

/* 发送表情（payload=2字节 emoji_id） */
int wechat_emoji_send(uint16_t emoji_id);

/* 发送图片：从内存拆包 */
int wechat_image_send(const uint8_t *data, uint32_t len);

/* 发送 wav 语音：从 SD 读 wav 文件的 PCM 数据拆包发送（每包头都带 sec） */
int wechat_send_wav_as_voice_msg(uint32_t frq,
                                 const char *wav_path,
                                 uint16_t *out_msg_id,
                                 uint8_t  *out_sec);



extern const unsigned char wav_header[];  /* 你工程里已有的模板头 */
extern uint8_t wechat_ui_can_render_now(void);

/* 如果页面不在前台，你想也能写日志：把 UI 文件里的 append_* 做成非 static 或提供 wrapper */
extern void wechat_history_append_voice_public(wechat_msg_from_t from, uint8_t sec, uint16_t msg_id, const char *wav_path);
extern void wechat_history_append_photo_public(wechat_msg_from_t from, const char *img_path);
extern void wechat_history_append_emoji_public(wechat_msg_from_t from, uint16_t emoji_id);

#define WECHAT_RX_WAV_DIR   "0:/DCIM"
#define WECHAT_RX_WAV_FRQ   8000   /* 你现在系统语音就是 8k */


#ifdef __cplusplus
}
#endif

#endif /* __WECHAT_MSG_H__ */