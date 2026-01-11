//#include <test_util.h>
#include <csi_config.h>
//#include <test_kernel_config.h>
#include "csi_kernel.h"
#include "lwip/netif.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "netif/ethernetif.h"
#include "typesdef.h"
#include "osal\mutex.h"
#include "lib/net/eloop/eloop.h"
#include "event.h"
#include "sys_config.h"
#include <string.h>
#include "lwip/tcpip.h"
#include "osal/sleep.h"
#include "dev/csi/hgdvp.h"
#include "list.h"
#include "dev.h"
#include "g_sensor.h"
#include "devid.h"
#include "osal/irq.h"
#include "osal/string.h"
#include "dev/jpg/hgjpg.h"
#include "jpgdef.h"
#include "lib/video/dvp/jpeg/jpg.h"
#include "math.h"
#include "dev/adc/hgadc_v0.h"
#include "hal/gpio.h"
#include <string.h>
#include "typesdef.h"
#include "sys_config.h"
#include "osal/task.h"
#include "osal/mutex.h"
#include "lwip/netif.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "netif/ethernetif.h"
#include "osal\mutex.h"
#include "lib/net/eloop/eloop.h"
#include "event.h"
#include "osal/sleep.h"
#include "osal/string.h"
#include "stream_frame.h"
#include "jpgdef.h"
#include "utlist.h"
#include "custom_mem/custom_mem.h"
#include "hal/pwm.h"
#include "hal/timer_device.h"
#include "osal_file.h"

#define UDP_SERVICE_CREAT_PRI			OS_TASK_PRIORITY_NORMAL
#define UDP_SERVICE_CREAT_STACK_SIZE		1024
struct os_task udp_service_creat_task;


/* UDP服务器端IP及PORT */
#define UDP_SERVER_PORT							30309
#define UDP_SERVER_IP							"192.168.1.100"//"192.168.1.1"
struct sockaddr_in udp_service_addr,udp_client_addr;


void udpKeepAlive_service_creat()
{
	int udp_socket_fd;
	int err = -1;
	char* buffer = "hello client";
	char receive_buf[50];
    char ipbuf[64] = {0};
	uint32_t addrlen = (sizeof(struct sockaddr_in));

	udp_socket_fd = socket(AF_INET, SOCK_DGRAM,IPPROTO_UDP);
	if (udp_socket_fd < 0) {
		printf("*service creat socket failed\n");
		return 0;
	}
	udp_service_addr.sin_family = AF_INET;
	udp_service_addr.sin_port = htons(UDP_SERVER_PORT);
	udp_service_addr.sin_addr.s_addr = inet_addr(UDP_SERVER_IP);

	err = bind(udp_socket_fd,(struct socketaddr*)&udp_service_addr,(sizeof(struct sockaddr_in)));
	if (err == -1) {
		printf("*bind error\r\n");
		closesocket(udp_socket_fd);
		return  0;
	}

	printf("Server listening on port %d...\n", UDP_SERVER_PORT);

    /* 等待客户连接 */
    while(1)
    {
    err = recvfrom(udp_socket_fd,receive_buf,5,0,(struct sockaddr*)&udp_client_addr,(socklen_t)addrlen);   //读取客户端数据确定客户端的IP
    printf("\n#rvbuf:%s\n",receive_buf);
    printf("clientIP: %s, port: %d\n",
	 			inet_ntop(AF_INET, &udp_client_addr.sin_addr.s_addr, ipbuf, sizeof(ipbuf)),
	 			ntohs(udp_client_addr.sin_port));
    }

}


/*main*/
void udpKeepAlive_service_init(void)
{
    int sClient;
    OS_TASK_INIT(	"udp_service_creat",
				&udp_service_creat_task, 
				udpKeepAlive_service_creat, 
				(int)sClient, 
				UDP_SERVICE_CREAT_PRI, 
				UDP_SERVICE_CREAT_STACK_SIZE);
    
}




#define UDP_CLIENT_CREAT_PRI			OS_TASK_PRIORITY_NORMAL
#define UDP_CLIENT_CREAT_STACK_SIZE		1024
/* UDP服务器端IP及PORT */
#define UDP_DEST_PORT							30309
#define UDP_DEST_IP								"192.168.1.100"//"192.168.1.100"

struct os_task udp_client_creat_task;

volatile uint8_t wakeup_sent_cnt =0;
volatile uint8_t sleep_sent_cnt =0;

void client_send_wakeup_cmd(uint8_t cnt)
{
	wakeup_sent_cnt=cnt;
}

void client_send_sleep_cmd(uint8_t cnt)
{
	sleep_sent_cnt=cnt;
}


void udpKeepAlive_client_creat() 
{
	int udp_socket_fd;
	int err;
	struct sockaddr_in udp_service_addr;
	socklen_t addrlen = sizeof(struct sockaddr);
	char* buffer = "hello\n";
	char* wakecmd_buffer= "wake up";
	char* sleepcmd_buffer= "Sleep";

	int buffer_size = strlen(buffer);
	int wakecmd_buffer_size = strlen(wakecmd_buffer);
	int sleepcmd_buffer_size = strlen(sleepcmd_buffer);




	udp_socket_fd = socket(AF_INET, SOCK_DGRAM,IPPROTO_UDP);

	udp_service_addr.sin_addr.s_addr = inet_addr(UDP_DEST_IP);
	udp_service_addr.sin_family = AF_INET;
	udp_service_addr.sin_port = htons(UDP_DEST_PORT);


	while(1) {
		#if 0
		err = sendto(udp_socket_fd,buffer,buffer_size,0,(struct sockaddr*)&udp_service_addr,addrlen);
		if (err < 0) {
		    printf("***s\n");
		}
		#endif

		if(wakeup_sent_cnt)
		{
			wakeup_sent_cnt--;
			err = sendto(udp_socket_fd,wakecmd_buffer,wakecmd_buffer_size,0,(struct sockaddr*)&udp_service_addr,addrlen);
			if (err < 0) {
				printf("***s\n");
			}
			printf("## send wakeup cmd cnt=%d \n",wakeup_sent_cnt);
		}
		if(sleep_sent_cnt)
				{
			sleep_sent_cnt--;
			err = sendto(udp_socket_fd,sleepcmd_buffer,sleepcmd_buffer_size,0,(struct sockaddr*)&udp_service_addr,addrlen);
			if (err < 0) {
				printf("***s\n");
			}
			printf("## send sleep cmd cnt=%d \n",sleep_sent_cnt);
		}

		os_sleep_ms(1000);
	}


}

void udpKeepAlive_client_init(void)
{
    int sClient;
    OS_TASK_INIT(	"udp_client_creat",
				&udp_client_creat_task, 
				udpKeepAlive_client_creat, 
				(int)sClient, 
				UDP_CLIENT_CREAT_PRI, 
				UDP_CLIENT_CREAT_STACK_SIZE);
    
}



/*main*/
void udpKeepAlive_init(void)
{
	#if 0
	udpKeepAlive_service_init();
	#else
    udpKeepAlive_client_init();
	#endif
}




