#ifndef _UI_LANGUAGE_H_
#define _UI_LANGUAGE_H_

#include <stdio.h>
#include "typesdef.h"

enum
{
    SOUND_STR,
    ISO_STR,
    RECORD_STR,
    INTERCOM_STR,
    TAKEPHOTO_STR,
    SETTING_STR,
    FORMAT_STR,
    WIFI_STR,
    HZ_STR,
    CYCLE_STR,
    USB_STR,
    BATTERY_STR,
    LANGUAGE_STR,
    EXIT_STR,
	LANGUAGE_CN_STR,
    LANGUAGE_EN_STR,
    YES_STR,
    NO_STR, 
    OPEN_STR,
    CLOSE_STR,
    NEXT_STR,
    CONTINUOUS_STR,
    SPIN_STR,
    PHOTO_STR,
    PLAYBACK_PHOTO_STR,
    PLAYBACK_REC_STR,
    PLAY_STR,
    NEXT_REC_STR,
    GAME_STR,
    MUSIC_STR,
    LARGER_STR,
    RAHMEN_STR,
    USBDEV_STR,
    UDISK_STR,
    UVC_STR,
    VFX_STR,
    PRT_STR,
    XN_FORMAT_STR,
    XN_CAMRESOL_STR,
    XN_RECRESOL_STR,
    XN_VOLUME_STR,
    XN_BRIGHTNESS_STR,
    XN_DATE_STR,
    XN_PRINTM_STR,
    XN_PRINTD_STR,
    XN_LANGUAGE_STR,
    XN_VERSION_STR,
    XN_DEFAULTSET_STR,
    XN_FREQUENCY_STR,
    XN_CYCLICREC_STR,
    XN_SCREENPR_STR,
    XN_AUTOOFF_STR,

    /* 二级菜单选项 */
    OPT_CLOSE_STR,
    OPT_1MIN_STR,
    OPT_3MIN_STR,
    OPT_5MIN_STR,
    OPT_10MIN_STR,
    OPT_MUTE_STR,
    OPT_LOW_STR,
    OPT_MEDIUM_STR,
    OPT_HIGH_STR,
    OPT_CONFIRM_STR,
    OPT_CANCEL_STR,

    LANGUAGE_STR_MAX,
};


enum
{
    English,
    Chinese,
    Language_MAX,
};


extern const uint8_t ui_language_switch[Language_MAX][LANGUAGE_STR_MAX+1][128];
#endif