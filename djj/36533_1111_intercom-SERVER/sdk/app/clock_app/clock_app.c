#include "sys_config.h"
#include "typesdef.h"
#include "g_sensor.h"
#include "devid.h"
#include "hal/gpio.h"
#include "osal/irq.h"
#include "osal/string.h"
#include "osal/timer.h"
#include "osal/sleep.h"
#include "clock_app.h"



#if 1 //def SW_COLOCK_EN

user_clock_t sw_rtc;

static user_alarm_t user_alarm[MAX_ALARM_NUM];

static s8 last_timeout_id = -1;

static s8 alarm_id[MAX_ALARM_NUM];

static const u8 day_per_month[12] =
{
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};


/******************************************************************************
* fatfs get_fattime 
******************************************************************************/
unsigned long get_fattime(void)
{
    unsigned long now_time=0;
    // now_time=((DWORD)(new_time.year - 1980) << 25 | (DWORD)new_time.month << 21 | (DWORD)new_time.data << 16);
   
   now_time= ((unsigned long)(sw_rtc.year-1980) << 25) /* Year = 2010 */
	| ((unsigned long)sw_rtc.month << 21) /* Month = 11 */
	| ( (unsigned long)sw_rtc.day << 16) /* Day = 2 */
	| ( (unsigned long)sw_rtc.hour << 11) /* Hour = 15 */
	| ( (unsigned long)sw_rtc.minute << 5) /* Min = 0 */
	| ( (unsigned long)sw_rtc.second >> 1); /* Sec = 0 */

   return now_time;
}


static void clock_init(void)
{
	memset(&sw_rtc, 0, sizeof(sw_rtc));

	sw_rtc.year = 2024;
	sw_rtc.month = 3;
	sw_rtc.day = 19;
	sw_rtc.hour = 8;
}



static void alarm_init(void)
{
	memset(user_alarm, 0, sizeof(user_alarm));

	for (u8 i = 0 ; i < MAX_ALARM_NUM; i++)
	{
		alarm_id[i] = -1;
	}
}

static u8 get_month_day(void)
{
    u8 ajust = 0;

    if (sw_rtc.month == 2 && sw_rtc.year % 4 == 0 && sw_rtc.year % 100 != 0)
	{
        ajust = 1;
    }
    return (day_per_month[sw_rtc.month-1] + ajust);
}

/******************************************************************************/
/**
 *  \brief    clock_run
 *  \note     时钟服务
 *  \retval
 *
 ******************************************************************************/
static void clock_run(void)
{
	u8 month_day = 0;

	sw_rtc.ms += 10;

	if (sw_rtc.ms > 999)
	{
		sw_rtc.ms = 0;
		sw_rtc.second ++;
		if (sw_rtc.second > 59)
		{
			sw_rtc.second = 0;
			sw_rtc.minute ++;
			if (sw_rtc.minute > 59)
			{
				sw_rtc.minute = 0;
				sw_rtc.hour ++;
				if (sw_rtc.hour > 23){
					sw_rtc.hour = 0;
					sw_rtc.day ++;
					month_day = get_month_day();
					if (sw_rtc.day > month_day)
					{
						sw_rtc.day = 1;
						sw_rtc.month ++;
						if (sw_rtc.month > 12){
							sw_rtc.month = 1;
							sw_rtc.year ++;

						}
					}

				}
			}
		}
		//os_printf("year:%d,month:%d,date:%d--hour:%d,min:%d,sec:%d \n",sw_rtc.year,sw_rtc.month,sw_rtc.day,sw_rtc.hour,sw_rtc.minute,sw_rtc.second);
	}
}

/******************************************************************************/
/**
 *  \brief    alarm_id_get
 *  \note     index 闹钟索引
 *  \retval   闹钟id
 *
 ******************************************************************************/
static s8 alarm_id_get(u8 index)
{
	for (u8 i =0; i<MAX_ALARM_NUM; i++)
	{
		if (index == alarm_id[i])
			return i;
	}
	return -1;

}

/******************************************************************************/
/**
 *  \brief    alarm_check
 *  \note     alarm 查询
 *  \retval   none
 *
 ******************************************************************************/
static void alarm_check(void)
{
	for (u8 i = 0; i < MAX_ALARM_NUM; i++)
	{
		if (user_alarm[i].state == ALARM_STATE_NOUSED || user_alarm[i].state == ALARM_STATE_STOPED){
			continue;
		}
		if (user_alarm[i].second == sw_rtc.second){

			if (user_alarm[i].min == sw_rtc.minute){

				if (user_alarm[i].hour == sw_rtc.hour){

					last_timeout_id = alarm_id_get(i);

					if (user_alarm[i].action != NULL){

						if (user_alarm[i].data != NULL){
							(user_alarm[i].action)(user_alarm[i].data);

							os_printf("Alarm on! Alarm id =%d\n",i);
						}else{
							(user_alarm[i].action)(NULL);
							os_printf("Alarm on! alarm id =%d\n",i);
						}

					}
					else{
						os_printf("alarm on! no action\n");
					}
				}

			}
		}
	}
}


/******************************************************************************/
/**
 *  \brief    user_clock 10ms clk
 *  \
 *  \retval
 *
 ******************************************************************************/
void user_clock_irq(void)
{
	clock_run();

	static s8 second = -1;

	if (second != sw_rtc.second)
	{
		second = sw_rtc.second;
		alarm_check();
	}
}





/******************************************************************************/
/**
 *  \brief    user_add_alarm
 *  \param    alarm 闹钟参数
 *  \retval   闹钟索引
 *
 ******************************************************************************/
s8 user_add_alarm(set_alarm_param_t *alarm)
{
	s8 alarm_id = -1;
	uint8_t i;

	for (i = 0; i < MAX_ALARM_NUM; i++)
	{
		if (user_alarm[i].state == ALARM_STATE_NOUSED)
		{
			alarm_id = i;
			user_alarm[i].state = ALARM_STATE_RUNNING;
			user_alarm[i].hour = alarm->hour;
			user_alarm[i].min = alarm->minu;
			user_alarm[i].second = alarm->second;
			user_alarm[i].data = alarm->data;
			user_alarm[i].action = alarm->action;
			user_alarm[i].snooze_num = alarm->snooze_num;
			user_alarm[i].snooze_time_minu = alarm->snooze_time_minu;

			break;
		}
	}
	return alarm_id;
}

/******************************************************************************/
/**
 *  \brief    user_delete_alarm
 *  \param    alarm_id 闹钟id
 *  \retval   -1 失败    0成功
 *
 ******************************************************************************/
s8 user_delete_alarm(s8 *alarm_id)
{
	if (*alarm_id >= MAX_ALARM_NUM)
		return -1;

	user_alarm[*alarm_id].state = ALARM_STATE_NOUSED;
	*alarm_id = -1;
	return 0;
}



/******************************************************************************/
/**
 *  \brief    alarm en/dis able
 *  \
 *  \retval
 *
 ******************************************************************************/
void user_alarm_enable(s8 *alarm_id, bool enable)
{
	if (*alarm_id != -1 && *alarm_id < MAX_ALARM_NUM)
	{
		if (enable){
			user_alarm[*alarm_id].state = ALARM_STATE_RUNNING;
		}else{
			user_alarm[*alarm_id].state = ALARM_STATE_STOPED;
		}
	}
	else if (*alarm_id == -1)
	{

	}
}

/******************************************************************************/
/**
 *  \brief    get alarm status
 *  \
 *  \retval
 *
 ******************************************************************************/
bool user_alarm_enable_get( s8 *alarm_id)
{
	bool ret = 0;
	if(user_alarm[*alarm_id].state == ALARM_STATE_RUNNING){
		ret = 1;
	}
	return ret;
}

/******************************************************************************/
/**
 *  \brief    user_alarm_set
 *
 *  \param    alarm_id 时钟ID 初始化为-1
 *  \param    action  超时服务例程  时间到执行的操作
 *  \param    data    服务例程参数
 *  \param    snooze_num    贪睡次数
 *  \param    snooze_time_minu    贪睡时间
 *  \retval   -1 失败 0 成功
 *
 ******************************************************************************/
s8 user_alarm_set(   s8 *alarm_id,
						u8 hour,
						u8 minu,
						u8 second,
						u8 snooze_num,
						u8 snooze_time_minu,
						alarm_action action,
						void *data)
{
	s8 ret = -1;

	if (*alarm_id != -1 && *alarm_id < MAX_ALARM_NUM)
	{  //有该闹钟 则重新设置属性
		user_alarm[*alarm_id].hour = hour;
		user_alarm[*alarm_id].min = minu;
		user_alarm[*alarm_id].second = second;
		user_alarm[*alarm_id].action = action;
		user_alarm[*alarm_id].data = data;
		user_alarm[*alarm_id].snooze_num = snooze_num;
		user_alarm[*alarm_id].snooze_time_minu = snooze_time_minu;
		ret = 0;
	}
	else
	{
		//没有该闹钟 则根据属性创建闹钟
		set_alarm_param_t alarm;
		alarm.hour = hour;
		alarm.minu = minu;
		alarm.second = second;
		alarm.action = action;
		alarm.snooze_num = snooze_num;
		alarm.snooze_time_minu = snooze_time_minu;
		*alarm_id = user_add_alarm(&alarm);
		if (*alarm_id == -1){
			os_printf("alarm add fail\n");
		}
		else
		{
			os_printf("alarm add success id:%d\n",*alarm_id);
			ret = 0;
		}
		// 默认闹钟打开
		user_alarm_enable(alarm_id, 1);
	}
	return ret;
}





static struct os_timer  clock_timer;
/******************************************************************************
* soft colock
******************************************************************************/
void soft_clock_init(void)
{
	alarm_init();
	clock_init();

	os_timer_init(&clock_timer, user_clock_irq, OS_TIMER_MODE_PERIODIC, NULL);
	os_timer_start(&clock_timer, 10); // 10ms
}


#endif

