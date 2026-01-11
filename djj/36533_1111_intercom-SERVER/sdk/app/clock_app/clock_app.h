#ifndef _CLOCK_APP_H
#define _CLOCK_APP_H

typedef int 				s32,S32;
typedef unsigned int		u32,U32,uint;
typedef long long int 		s64,S64;
typedef unsigned long long 	u64,U64;
typedef short int 			s16,S16;
typedef unsigned short int 	u16,U16;
typedef signed char			s8,S8;
typedef unsigned char 		u8,U8;



#define MAX_ALARM_NUM   (10)


typedef void (*alarm_action)(void *data);


typedef enum
{
    ALARM_STATE_NOUSED,
    ALARM_STATE_RUNNING,
    ALARM_STATE_STOPED,

} alarm_state_e;
	

typedef struct {
    u16 year;
    u8 month;
	u8 wday;
    u8 day;
    u8 hour;
    u8 minute;
    u8 second;
	u16 ms;
} user_clock_t;


typedef struct
{
	u8 hour;
	u8 minu;
	u8 second;
	u8 snooze_num; // 次数
	u8 snooze_time_minu;  //时间
	alarm_action action;
	u8 *data;
} set_alarm_param_t;

typedef struct
{
	u8 hour;
	u8 min;
	u8 second;
	alarm_state_e state;
	u8 *data;
	alarm_action action;
	u8 snooze_num; // 贪睡次数
	u8 snooze_time_minu;  //每次贪睡时间

} user_alarm_t;


typedef enum
{
	ALARM_1 = 0,
	ALARM_2,
	ALARM_3,
	ALARM_MUS_ON,
	ALARM_MUS_OFF,
	ALARM_LED_ON,
	ALARM_LED_OFF,
	ALARM_MAX = MAX_ALARM_NUM,

} alarm_id_e;

extern user_clock_t sw_rtc;
void soft_clock_init(void);


#endif
