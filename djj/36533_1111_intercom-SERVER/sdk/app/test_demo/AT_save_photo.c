/***************************************************
    该demo主要是使用AT命令拍一张照片,前提要将jpeg打开
***************************************************/
#include "osal/string.h"
#include "custom_mem/custom_mem.h"
#include "stream_frame.h"
#include "osal/task.h"
#include "osal_file.h"
#include "video_app/video_app.h"
#include "fatfs/ff.h"

void at_save_photo_thread(void *d);
struct AT_PHOTO
{
    struct os_task task;
    uint32_t photo_num;
    uint8_t filename_prefix[4];
    uint8_t running;

};

static struct AT_PHOTO *photo_s = NULL;

int32 demo_atcmd_save_photo(const char *cmd, char *argv[], uint32 argc)
{
	#if OPENDML_EN &&  SDH_EN && FS_EN
    if(argc < 2)
    {
        os_printf("%s argc too small:%d,should more 2 arg\n",__FUNCTION__,argc);
        return 0;
    }
    if(os_atoi(argv[0]) == 0)
    {
        if(photo_s)
        {
            photo_s->running = 0;
        }
        else
        {
            os_printf("%s takephoto num err:%d\n",__FUNCTION__,os_atoi(argv[0]));
        }
        
        return 0;
    } 
    photo_s = custom_malloc(sizeof(struct AT_PHOTO));
    if(photo_s)
    {
        memset(photo_s,0,sizeof(struct AT_PHOTO));
        //连续拍照多少张
        photo_s->photo_num = os_atoi(argv[0]);
        int prefix_len = strlen(argv[1]);
        if(prefix_len > 3)
        {
            os_memcpy(photo_s->filename_prefix,argv[1],3);
        }
        else
        {
            os_memcpy(photo_s->filename_prefix,argv[1],prefix_len);
        }

        //创建拍照的任务
        OS_TASK_INIT("at_photo", &photo_s->task, at_save_photo_thread, (uint32)photo_s, OS_TASK_PRIORITY_NORMAL, 1024);  
    }
	#endif
    return 0;
}

void bbm_take_photo(uint8_t num)
{
    if(photo_s)
    {
        printf("%s takephoto num err:%d\n",__FUNCTION__,num);
        return;
    }

    photo_s = custom_malloc(sizeof(struct AT_PHOTO));

    if(photo_s)
    {
        memset(photo_s,0,sizeof(struct AT_PHOTO));
        //连续拍照多少张
        if(num<=0)
        {
            num = 1;
        }
        photo_s->photo_num = num;
        
        OS_TASK_INIT("at_photo", &photo_s->task, at_save_photo_thread, (uint32)photo_s, OS_TASK_PRIORITY_NORMAL, 1024);  
    }
}

uint8_t get_bbm_take_photo_status(void)
{
    if(photo_s)
    {
		os_printf("## photo_s->running\n");
        return photo_s->running;
    }
	os_printf("## get_bbm_take_photo_status == 0\n");
    return 0;
}



static int opcode_func(stream *s,void *priv,int opcode)
{
	int res = 0;
	//_os_printf("%s:%d\topcode:%d\n",__FUNCTION__,__LINE__,opcode);
	switch(opcode)
	{
		case STREAM_OPEN_ENTER:
		break;
		case STREAM_OPEN_EXIT:
		{
            enable_stream(s,1);
		}
		break;
		case STREAM_OPEN_FAIL:
		break;
		default:
			//默认都返回成功
		break;
	}
	return res;
}

extern int no_frame_record_video2(void *fp,void *d,int flen);
void at_save_photo_thread(void *d)
{
    struct data_structure *get_f = NULL;
    struct AT_PHOTO *p_s = (struct AT_PHOTO *)d;
    stream *s = NULL;
    uint32_t flen;
    char filename[64] = {0};
	
	// 检查SD卡状态
    uint8_t sd_status = get_sd_status();
    os_printf("SD card status: %d (0=IDLE, 3=OFF)\n", sd_status);

    if(sd_status == 3) {
        os_printf("SD card not detected!\n");
        goto at_save_photo_thread_end;
    }
	
    s = open_stream_available(R_AT_SAVE_PHOTO,0,8,opcode_func,NULL);
    if(!s)
    {   
		printf("No photo stream!\r\n");
        goto at_save_photo_thread_end;
    }
    p_s->running = 1;
    void *fp = NULL;
    uint32_t err_count = 0;
	
	// 添加目录创建
    DIR dir;
    FRESULT ret = f_opendir(&dir, "0:/DCIM");
    if(ret != FR_OK){
        os_printf("Creating DCIM directory...\n");
        f_mkdir("0:/DCIM");
    } else {
        f_closedir(&dir);
    }
	
    start_jpeg();
    while(p_s->photo_num && p_s->running)
    {
        get_f = recv_real_data(s);
        if(get_f)
        {
            err_count = 0;
            
			os_sprintf(filename,"0:/DCIM/%sJPEG%04d.jpg",p_s->filename_prefix,(uint32_t)os_jiffies()%9999);
            os_printf("filename:%s\n",filename);
            fp = osal_fopen(filename,"wb+");
            if(!fp)
            {
                os_printf("filename: fp err !!!!!!\n");
                free_data(get_f);
				get_f = NULL;
				p_s->photo_num--;
				continue;  // 继续而不是goto end
            }
            flen = get_stream_real_data_len(get_f);
            no_frame_record_video2(fp,get_f,flen);

			

//			 // ===== 使用sd_save.c中的函数 =====
//              extern void *creat_takephoto_file(char *dir_name);
//              extern char *JPG_FILE_NAME;
//
//              // 设置文件名前缀（可选，如果需要的话）
//              // p_s->filename_prefix 在这里是 "BBM"
//
//              fp = creat_takephoto_file("0:/DCIM");  // 使用验证过的函数
//              if(!fp)
//              {
//                  os_printf("creat_takephoto_file failed!\n");
//                  free_data(get_f);
//                  get_f = NULL;
//                  p_s->photo_num--;
//                  continue;
//              }
//
//              // 创建成功，继续写入数据
//              no_frame_record_video_psram(fp,get_f,get_stream_real_data_len(get_f));
			  //========================================================
            
			free_data(get_f);
            get_f = NULL;
            osal_fclose(fp);
            p_s->photo_num--;
        }
        else
        {
            err_count++;
            os_sleep_ms(1);
            if(err_count++ > 1000)
            {
                goto at_save_photo_thread_end;
            }
        }
        
    }
    at_save_photo_thread_end:
    if(get_f)
    {
        free_data(get_f);
        get_f = NULL;
    }
    
    if(s)
    {
        stop_jpeg();
        close_stream(s);
    }
    p_s->running = 0;
    custom_free((void*)p_s);
    photo_s = NULL;
}