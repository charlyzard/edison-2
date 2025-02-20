/* drivers/input/touchscreen/goodix_touch.c
 *
 * Copyright (C) 2010 - 2011 Goodix, Inc.
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/earlysuspend.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <mach/gpio.h>

#include <linux/irq.h>
#include <linux/syscalls.h>
#include <linux/reboot.h>
#include <linux/proc_fs.h>
#include <linux/async.h>
#include <linux/gt8110.h>

#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/completion.h>
#include <asm/uaccess.h>
#include <mach/board.h>

#define PEN_DOWN 1
#define PEN_RELEASE 0
#define PEN_DOWN_UP 2


#define GOODIX_I2C_NAME "Goodix-TS"

#define POLL_TIME		10	//actual query spacing interval:POLL_TIME+6

#define MAX_FINGER_NUM	 10

struct rk_touch_info
{
	u32 press;
	u32 x ;
	u32 y ;
	int status ; // 0 1
} ;

struct rk_ts_data{
	struct i2c_client *client;
	struct input_dev *input_dev;
	int irq;
	int irq_pin;
	int pwr_pin;
	int rst_pin;
	int lock_pin;
	int valid_indicate_pin;
	int read_mode;
	struct hrtimer timer;
	struct workqueue_struct *ts_wq;
	struct delayed_work  ts_work;
	char phys[32];
	struct early_suspend early_suspend;
	int (*power)(struct rk_ts_data * ts, int on);
	int (*ts_init)(struct rk_ts_data*ts);
	int (*input_parms_init)(struct rk_ts_data *ts);
	void (*get_touch_info)(struct rk_ts_data *ts,char *point_num,struct rk_touch_info *info_buf);  //get touch data info
	uint16_t abs_x_max;
	uint16_t abs_y_max;
	uint8_t max_touch_num;
	bool		pendown;
};

struct goodix_ts_data {
	uint16_t addr;
	uint8_t bad_data;
	struct i2c_client *client;
	struct input_dev *input_dev;
	int use_reset;
	int use_irq;
	int read_mode;
	struct hrtimer timer;
	struct delayed_work  work;
	char phys[32];
	int retry;
	struct early_suspend early_suspend;
	int (*power)(struct goodix_ts_data * ts, int on);
	uint16_t abs_x_max;
	uint16_t abs_y_max;
	uint8_t max_touch_num;
	uint8_t int_trigger_type;
	bool		pendown;
};

static const char *rk_ts_name = "Goodix Capacitive TouchScreen";
static struct workqueue_struct *goodix_wq;
static struct i2c_client * i2c_connect_client = NULL;
static struct proc_dir_entry *goodix_proc_entry;


#define READ_COOR_ADDR 0x01



//*************************Firmware Update part*******************************
#define CONFIG_TOUCHSCREEN_GOODIX_IAP
#ifdef CONFIG_TOUCHSCREEN_GOODIX_IAP
#define UPDATE_NEW_PROTOCOL

static unsigned int oldcrc32 = 0xFFFFFFFF;
static unsigned int crc32_table[256];
static unsigned int ulPolynomial = 0x04c11db7;
static unsigned char rd_cfg_addr;
static unsigned char rd_cfg_len;
static unsigned char g_enter_isp = 0;

static int goodix_update_write(struct file *filp, const char __user *buff, unsigned long len, void *data);
static int goodix_update_read( char *page, char **start, off_t off, int count, int *eof, void *data );

#define PACK_SIZE 					64					//update file package size
#define MAX_TIMEOUT					60000				//update time out conut
#define MAX_I2C_RETRIES				20					//i2c retry times

//I2C buf address
#define ADDR_CMD					80
#define ADDR_STA					81
#ifdef UPDATE_NEW_PROTOCOL
	#define ADDR_DAT				0
#else
	#define ADDR_DAT				82
#endif

//moudle state
#define NEW_UPDATE_START			0x01
#define UPDATE_START				0x02
#define SLAVE_READY					0x08
#define UNKNOWN_ERROR				0x00
#define FRAME_ERROR					0x10
#define CHECKSUM_ERROR				0x20
#define TRANSLATE_ERROR				0x40
#define FLASH_ERROR					0X80

//error no
#define ERROR_NO_FILE				2	//ENOENT
#define ERROR_FILE_READ				23	//ENFILE
#define ERROR_FILE_TYPE				21	//EISDIR
#define ERROR_GPIO_REQUEST			4	//EINTR
#define ERROR_I2C_TRANSFER			5	//EIO
#define ERROR_NO_RESPONSE			16	//EBUSY
#define ERROR_TIMEOUT				110	//ETIMEDOUT

//update steps
#define STEP_SET_PATH              1
#define STEP_CHECK_FILE            2
#define STEP_WRITE_SYN             3
#define STEP_WAIT_SYN              4
#define STEP_WRITE_LENGTH          5
#define STEP_WAIT_READY            6
#define STEP_WRITE_DATA            7
#define STEP_READ_STATUS           8
#define FUN_CLR_VAL                9
#define FUN_CMD                    10
#define FUN_WRITE_CONFIG           11

//fun cmd
#define CMD_DISABLE_TP             0
#define CMD_ENABLE_TP              1
#define CMD_READ_VER               2
#define CMD_READ_RAW               3
#define CMD_READ_DIF               4
#define CMD_READ_CFG               5
#define CMD_SYS_REBOOT             101

//read mode
#define MODE_RD_VER                1
#define MODE_RD_RAW                2
#define MODE_RD_DIF                3
#define MODE_RD_CFG                4
#endif

static struct rk_touch_info *info_buf;

static int dbg_thresd = 0;
#define DBG(x...) do { if(unlikely(dbg_thresd)) printk(KERN_INFO x); } while (0)


/*******************************************************	
Description:
	Read data from the i2c slave device;
	This operation consisted of 2 i2c_msgs,the first msg used
	to write the operate address,the second msg used to read data.

Parameter:
	client:	i2c device.
	buf[0]:operate address.
	buf[1]~buf[len]:read data buffer.
	len:operate length.
	
return:
	numbers of i2c_msgs to transfer
*********************************************************/
static int goodix_i2c_read_bytes(struct i2c_client *client, uint8_t *buf, int len)
{
	struct i2c_msg msgs[2];
	int ret=-1;
	int retries = 0;

	msgs[0].flags = client->flags;
	msgs[0].addr=client->addr;
	msgs[0].len=1;
	msgs[0].buf=&buf[0];
	msgs[0].udelay = client->udelay;
	msgs[0].scl_rate=200 * 1000;

	msgs[1].flags = client->flags | I2C_M_RD;
	msgs[1].addr=client->addr;
	msgs[1].len=len-1;
	msgs[1].buf=&buf[1];
	msgs[1].udelay = client->udelay;
	msgs[0].scl_rate=200 * 1000;

	while(retries<5)
	{
		ret=i2c_transfer(client->adapter,msgs, 2);
		if(ret == 2)break;
		retries++;
	}
	return ret;
}

/*******************************************************	
Description:
	write data to the i2c slave device.

Parameter:
	client:	i2c device.
	buf[0]:operate address.
	buf[1]~buf[len]:write data buffer.
	len:operate length.
	
return:
	numbers of i2c_msgs to transfer.
*********************************************************/
static int goodix_i2c_write_bytes(struct i2c_client *client,uint8_t *data,int len)
{
	struct i2c_msg msg;
	int ret=-1;
	int retries = 0;

	msg.flags=!I2C_M_RD;
	msg.addr=client->addr;
	msg.len=len;
	msg.buf=data;		
	msg.udelay = client->udelay;
	msg.scl_rate=200 * 1000;
	
	while(retries<5)
	{
		ret=i2c_transfer(client->adapter,&msg, 1);
		if(ret == 1)break;
		retries++;
	}
	return ret;
}

static int goodix_config_ok(struct rk_ts_data *ts)
{
        int ret = 0;
        unsigned int w,h, n;
	uint8_t rd_cfg_buf[7] = {0x66,};
        
	ret = goodix_i2c_read_bytes(ts->client, rd_cfg_buf, 7);

	w = (rd_cfg_buf[2]<<8) + rd_cfg_buf[1];
	h = (rd_cfg_buf[4]<<8) + rd_cfg_buf[3];
        n = rd_cfg_buf[5];


	ts->abs_x_max = w;
	ts->abs_y_max = h;
	ts->max_touch_num = n;
        
	printk("goodix_ts_init: X_MAX = %d,Y_MAX = %d,MAX_TOUCH_NUM = %d\n",ts->abs_x_max,ts->abs_y_max,ts->max_touch_num);

        return 0;
}
/*******************************************************
Description:
	Goodix touchscreen initialize function.

Parameter:
	ts:	i2c client private struct.
	
return:
	Executive outcomes.0---succeed.
*******************************************************/
static int goodix_init_panel(struct rk_ts_data *ts)
{
	int ret=-1, retry = 10;
	uint8_t rd_cfg_buf[7] = {0x66,};
	char test_data = 0;

#ifndef CONFIG_MALATA_D8002
	uint8_t config_info[] = {
	0x65,0x02,0x00,0x10,0x00,0x10,0x0A,0x62,0x4A,0x00,
	0x0F,0x28,0x02,0x10,0x10,0x00,0x00,0x20,0x00,0x00,
	0x10,0x10,0x10,0x00,0x37,0x00,0x00,0x00,0x01,0x02,
	0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,
	0x0D,0xFF,0xFF,0x00,0x01,0x02,0x03,0x04,0x05,0x06,
	0x07,0x08,0x09,0x0A,0x0B,0x0C,0xFF,0xFF,0xFF,0x00,
	0x00,0x3C,0x64,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00
        };
#else
uint8_t config_info[] = {
	0x65,0x01,0x00,0x10,0x00,0x10,0x05,0x6E,0x0A,0x00,
	0x0F,0x1E,0x02,0x08,0x10,0x00,0x00,0x27,0x00,0x00,
	0x50,0x10,0x10,0x11,0x37,0x00,0x00,0x00,0x01,0x02,
	0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0xFF,
	0xFF,0xFF,0xFF,0x00,0x01,0x02,0x03,0x04,0x05,0x06,
	0x07,0x08,0x09,0x0A,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,
	0x00,0x50,0x64,0x50,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x20
        };
#endif
        ret=goodix_i2c_write_bytes(ts->client,config_info,95);
        if( ret == 1)
        {
        	printk("GT8110 send config para successfully!\n");
	}
	else
	{
		printk("GT8110 send config para failed!\n");
	}	
	ts->abs_x_max = 4096;
	ts->abs_y_max = 4096;
        ts->max_touch_num = 10;
        
	return 0;

}

//fjp add ===============================
static bool goodix_get_status(char *p1,int*p2)
{
	bool status = PEN_DOWN;
	if((*p2==PEN_DOWN) && (*p1==PEN_RELEASE))
		{
			*p2 = PEN_DOWN_UP; //�ոյ���
			 status = PEN_RELEASE; 
		}
	else if((*p2==PEN_RELEASE) && (*p1==PEN_RELEASE))
		{
		   *p2 = PEN_RELEASE;
			status = PEN_RELEASE; 
		}
	else
		{
			*p2 = PEN_DOWN;
		}
	return status;
}

//===================================
/*******************************************************
Description:
	Read goodix touchscreen version function.

Parameter:
	ts:	i2c client private struct.
	
return:
	Executive outcomes.0---succeed.
*******************************************************/
static int  goodix_read_version(struct rk_ts_data *ts, char **version)
{
	int ret = -1, count = 0;
	char *version_data;
	char *p;

	*version = (char *)vmalloc(18);
	version_data = *version;
	if(!version_data)
		return -ENOMEM;
	p = version_data;
	memset(version_data, 0, 18);
	version_data[0]=240;	
	ret=goodix_i2c_read_bytes(ts->client,version_data, 17);
	if (ret < 0) 
		return ret;
	version_data[17]='\0';
	
	if(*p == '\0')
		return 0; 	
	do 					
	{
		if((*p > 122) || (*p < 48 && *p != 32) || (*p >57 && *p  < 65) 
			||(*p > 90 && *p < 97 && *p  != '_'))		//check illeqal character
			count++;
	}while(*++p != '\0' );
	if(count > 2)
		return 0;
	else 
		return 1;	
}

static int last_touch_num = -1;
static void goodix_get_touch_info(struct rk_ts_data *ts,char *point_num,struct rk_touch_info* info_buf)
{
	uint8_t  point_data[(1-READ_COOR_ADDR)+1+2+5*MAX_FINGER_NUM+1]={ 0 };  //read address(1byte)+key index(1byte)+point mask(2bytes)+5bytes*MAX_FINGER_NUM+coor checksum(1byte)
	uint8_t  check_sum = 0;
	int ret ;
	uint16_t  finger_current = 0;
	uint16_t  finger_bit = 0;
	unsigned int  count = 0, point_count = 0;
	unsigned char touch_num = 0;
  	uint8_t chksum_err = 0;
	unsigned int position = 0;	
	uint8_t track_id[MAX_FINGER_NUM] = {0};
	u8 index;
	u8 temp =0;
	point_data[0] =  0;//READ_COOR_ADDR;//0x65;//READ_COOR_ADDR;		//read coor address
	
	if(!ts||!info_buf)
	{
		printk("goodix ts or info_buf is null\n");
		return;
	}

	ret=goodix_i2c_read_bytes(ts->client, point_data, sizeof(point_data)/sizeof(point_data[0]));
	if(ret != 2)	
	{
	    printk("goodix read error\n");
	}	
	finger_current =  (point_data[2]<<8) + point_data[1];
	
	DBG("finger_current:%d ==== max_touch_num:%d\n", finger_current,ts->max_touch_num);//add by fjp 2010-9-28
	
	/*printk(" GT8110 read point data info\n");
	for(index = 0 ; index < 20 ; index++)
	{
		printk("  %x ",point_data[index]);
	}*/
	
	if(finger_current)
	{	
		point_count = 0;
		finger_bit = finger_current;
		for(count = 0; (finger_bit != 0) && (count < ts->max_touch_num); count++)//cal how many point touch currntly
		{
			if(finger_bit & 0x01)
			{
				track_id[count] = PEN_DOWN;
				point_count++;
			}
			finger_bit >>= 1;
		}
		touch_num = point_count;

		check_sum = point_data[2 - READ_COOR_ADDR] + point_data[3 - READ_COOR_ADDR]; 			//cal coor checksum
		count = 4 - READ_COOR_ADDR;
		for(point_count *= 5; point_count > 0; point_count--)
			check_sum += point_data[count++];
		check_sum += point_data[count];
		//if(check_sum  != 0)			//checksum verify error
		if(0)
		{
			printk("coor checksum error!\n");
		}
		else
		{
			chksum_err = 0;
		}
	}

	//printk("current point num:%d\n",touch_num);
	*point_num = touch_num;
	if(touch_num < last_touch_num)  //some flinger release
	{
		//printk("%d flinger release\n",last_touch_num-touch_num);
		/*for(index = touch_num; index < last_touch_num; index++)
			info_buf[index].status = 0;*/
		*point_num = last_touch_num;
		 touch_num = last_touch_num;
	}
	last_touch_num = touch_num;
	//printk(" X and Y point value is\n");	
	for(index = 0; index < touch_num; index++)
	{
	     if(goodix_get_status(&track_id[index],&info_buf[index].status))
	     	{
		position = 3 + 4*(temp++);
		info_buf[index].x = (unsigned int) (point_data[position+1]<<8) + (unsigned int)( point_data[position]);
		info_buf[index].y  = (unsigned int)(point_data[position+3]<<8) + (unsigned int) (point_data[position+2]);
		info_buf[index].press = (unsigned int) (point_data[position+4]);
		//printk(" x = %d , y = %d , p =  %d ", 	info_buf[index].x,info_buf[index].y, info_buf[index].press);
	     	}
	}
	
}


/*******************************************************
Description:
	Goodix touchscreen work function.

Parameter:
	ts:	i2c client private struct.
	
return:
	Executive outcomes.0---succeed.
*******************************************************/
static void  rk_ts_work_func(struct work_struct *pwork)
{	
	int i =0;
	char point_num;
	struct rk_ts_data *ts = container_of(to_delayed_work(pwork), struct rk_ts_data, ts_work);

	if(!ts)
	{
		printk("container of rk_ts_data fail\n");
		return;
	}
	if(!info_buf)
	{
		printk("info_buf fail\n");
		return;
	}

	if(ts->get_touch_info)
	{
		 ts->get_touch_info(ts,&point_num,info_buf);
	}

#if 0//#ifdef CONFIG_MALATA_D8002
	if((gpio_get_value(ts->lock_pin) == GPIO_LOW)||(gpio_get_value(ts->valid_indicate_pin) == GPIO_HIGH))
	{
		for(i=0; i< point_num; i++)
		{
			info_buf[i].status = PEN_DOWN_UP;
		}
	}
#endif
	for(i=0; i< point_num; i++)
	{
	   DBG("info_buf[i].status =====%d\n",info_buf[i].status);
	      if(info_buf[i].status==PEN_DOWN_UP)
		{
		       info_buf[i].status=PEN_RELEASE;
			   DBG("the key %d is up------\n",i);
			input_mt_slot(ts->input_dev, i);
			input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false);
			continue;
		}
		if(info_buf[i].status==PEN_DOWN)
		{
			input_mt_slot(ts->input_dev, i);
			input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, true);
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, info_buf[i].press);
#ifdef CONFIG_MALATA_D8002
			input_report_abs(ts->input_dev, ABS_MT_POSITION_X, 4096 - info_buf[i].x);
#else
			input_report_abs(ts->input_dev, ABS_MT_POSITION_X, info_buf[i].x);
#endif
			input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, info_buf[i].y);
                        DBG("touch point %d %d >>x:%d>>y:%d\n",i,info_buf[i].status,info_buf[i].x,info_buf[i].y);//add by fjp 2010-9-28	
		}
		
       
          
		
		
	}
	input_sync(ts->input_dev);
	
    if(gpio_get_value(ts->irq_pin) == GPIO_LOW)
    //if(point_num)
    {
       
        DBG("touch down .............\n");//add by fjp 2010-9-28
       	queue_delayed_work(ts->ts_wq, &ts->ts_work,msecs_to_jiffies(20));
	//	goto exit;
		
    }
    else
    {
		
        DBG("touch up>>x:%d>>y:%d\n",info_buf[0].x,info_buf[0].y);//add by fjp 2010-9-28
		/*input_mt_slot(ts->input_dev, 0);
		input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, true);
		input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0);
		
		input_mt_slot(ts->input_dev, 0);
		input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false);*/

		DBG("point_num+++++++++++ = %d\n", point_num);//add by fjp 2010-9-28
		for(i=0; i< point_num; i++)
		{
	//	  printk("info_buf[i].status +++++++%d\n",info_buf[i].status);
			 if(info_buf[i].status)
		      	{
	              input_mt_slot(ts->input_dev, i);//������ϱ�
			input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false);		
			//input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0);
			info_buf[i].status= PEN_RELEASE;
			

			DBG("release+++++++++++ = %d\n", i);//add by fjp 2010-9-28

		      	}
		}
		input_sync(ts->input_dev);
		ts->pendown =PEN_RELEASE;
		last_touch_num = 0;
		
	enable_irq(ts->irq);		
      }
         
      
//exit:
	  //kfree(info_buf);
	  
  }
	
	
/*******************************************************
Description:
	Timer interrupt service routine.

Parameter:
	timer:	timer struct pointer.
	
return:
	Timer work mode. HRTIMER_NORESTART---not restart mode
*******************************************************/
static enum hrtimer_restart goodix_ts_timer_func(struct hrtimer *timer)
{
	struct rk_ts_data *ts = container_of(timer, struct rk_ts_data, timer);
	queue_delayed_work(goodix_wq,&ts->ts_work,0);
	hrtimer_start(&ts->timer, ktime_set(0, (POLL_TIME+6)*1000000), HRTIMER_MODE_REL);
	return HRTIMER_NORESTART;
}
/*******************************************************
Description:
	External interrupt service routine.

Parameter:
	irq:	interrupt number.
	dev_id: private data pointer.
	
return:
	irq execute status.
*******************************************************/
static irqreturn_t rk_ts_irq_handler(int irq, void *dev_id)
{

	struct rk_ts_data *ts = (struct rk_ts_data*)dev_id;
	disable_irq_nosync(ts->irq);
	queue_delayed_work(ts->ts_wq, &ts->ts_work,0);
	
	return IRQ_HANDLED;
}

static int rk_ts_suspend(struct i2c_client *client, pm_message_t mesg)
{
	int ret;
	struct rk_ts_data *ts = i2c_get_clientdata(client);

       
	
	disable_irq(ts->irq);
	
#if 1
	if (ts->power) {
		ret = ts->power(ts, 0);
		if (ret < 0)
			printk(KERN_ERR "goodix_ts_resume power off failed\n");
	}
#endif
	return 0;
}

static int rk_ts_resume(struct i2c_client *client)
{
	int ret;
	struct rk_ts_data *ts = i2c_get_clientdata(client);
	
#if 1
	if (ts->power) {
		ret = ts->power(ts, 1);
		if (ret < 0)
			printk(KERN_ERR "goodix_ts_resume power on failed\n");
	}
#endif
	
	enable_irq(client->irq);

	return 0;
}



#ifdef CONFIG_HAS_EARLYSUSPEND
static void rk_ts_early_suspend(struct early_suspend *h)
{
	struct rk_ts_data *ts;
	ts = container_of(h, struct rk_ts_data, early_suspend);
	rk_ts_suspend(ts->client, PMSG_SUSPEND);
}

static void rk_ts_late_resume(struct early_suspend *h)
{
	struct rk_ts_data *ts;
	ts = container_of(h, struct rk_ts_data, early_suspend);
	rk_ts_resume(ts->client);
}
#endif

/*******************************************************
Description:
	Goodix touchscreen power manage function.

Parameter:
	on:	power status.0---suspend;1---resume.
	
return:
	Executive outcomes.-1---i2c transfer error;0---succeed.
*******************************************************/
static int goodix_ts_power(struct rk_ts_data * ts, int on)
{
	int ret = -1;
	unsigned char i2c_control_buf[2] = {0x38,  0x56};		//suspend cmd
	int retry = 0;
	if(on != 0 && on !=1)
	{
		printk(KERN_DEBUG "%s: Cant't support this command.", rk_ts_name);
		return -EINVAL;
	}
	
	
	if(on == 0)		//suspend
	{
		gpio_set_value(ts->rst_pin,GPIO_LOW);
		msleep(20);
	    	gpio_set_value(ts->rst_pin,GPIO_HIGH);
	    	msleep(200);
        while(retry<5)
		{
			ret = goodix_i2c_write_bytes(ts->client, i2c_control_buf, 2);
			if(ret == 1)
			{
				printk(KERN_DEBUG "touch goodix Send suspend cmd successed \n");
				break;
			}
		       retry++;
			msleep(10);
		}
		if(ret > 0)
		  ret = 0;
		if( ts->pwr_pin != 0)
			gpio_set_value(ts->pwr_pin,GPIO_LOW);
	}
	else if(on == 1)		//resume
	{
		if( ts->pwr_pin != 0)
			gpio_set_value(ts->pwr_pin,GPIO_HIGH);
		printk(KERN_DEBUG "touch goodix int resume\n");
		gpio_set_value(ts->rst_pin,GPIO_LOW);	
		msleep(20);
	    gpio_set_value(ts->rst_pin,GPIO_HIGH);
		ret = 0;
	}	 
	return ret;
}


static int goodix_input_params_init(struct rk_ts_data *ts)
{
	int ret ;
	ts->input_dev = input_allocate_device();
	if (ts->input_dev == NULL) {
		ret = -ENOMEM;
		printk(KERN_ALERT "Failed to allocate input device\n");
		return ret;
	}


	__set_bit(INPUT_PROP_DIRECT, ts->input_dev->propbit);
	__set_bit(EV_ABS, ts->input_dev->evbit);

	input_mt_init_slots(ts->input_dev, ts->max_touch_num);
	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X, 0, /*ts->abs_x_max*/4096, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y, 0, /*ts->abs_y_max*/4096, 0, 0);
	sprintf(ts->phys, "input/rkts");
	ts->input_dev->name = rk_ts_name;
	ts->input_dev->phys = ts->phys;
	ts->input_dev->id.bustype = BUS_I2C;
	ts->input_dev->id.vendor = 0xDEAD;
	ts->input_dev->id.product = 0xBEEF;
	ts->input_dev->id.version = 10427;	//screen firmware version
	
	ret = input_register_device(ts->input_dev);
	if (ret) {
		printk(KERN_ALERT "Probe: Unable to register %s input device\n", ts->input_dev->name);
		return -1;
	}

	return 0 ;
	
}
	
static int goodix_ts_init(struct rk_ts_data *ts)
{
	int ret ;
	char *version_info = NULL;
#if 1
	char test_data = 1;
	char retry;
	for(retry=0;retry < 1; retry++)    //test goodix
	{
		ret =goodix_i2c_write_bytes(ts->client, &test_data, 1);
		if (ret > 0)
			break;
	}
	if(ret <= 0)
	{
		printk(KERN_INFO "I2C communication ERROR!Goodix touchscreen driver become invalid\n");
		return -1;
	}	
#endif	
	
	ret = goodix_read_version(ts, &version_info);
	if(ret <= 0)
	{
		printk(KERN_INFO"Read version data failed!\n");
	}
	else
	{
		printk("goodix_ts_init: version %s\n", (version_info+1));
	}
	
	ret=goodix_init_panel(ts);
	if(ret != 0) {
		printk("goodix panel init fail\n");
		return -1;
	}

	vfree(version_info);
	#ifdef CONFIG_TOUCHSCREEN_GOODIX_IAP
	goodix_proc_entry = create_proc_entry("goodix-update", 0666, NULL);
	if(goodix_proc_entry == NULL)
	{
		printk("Couldn't create proc entry!\n");
		ret = -ENOMEM;
		return ret ;
	}
	else
	{
		printk("goodix_ts_init: create proc entry success!\n");
		goodix_proc_entry->write_proc = goodix_update_write;
		goodix_proc_entry->read_proc = goodix_update_read;

	}
#endif

	return 0;
}
/*******************************************************
Description:
	Goodix touchscreen probe function.

Parameter:
	client:	i2c device struct.
	id:device id.
	
return:
	Executive outcomes. 0---succeed.
*******************************************************/
static int rk_ts_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int ret = 0;
	struct rk_ts_data *ts;
	struct goodix_8110_platform_data *pdata ;
	
	printk(KERN_INFO "Install touch driver.\n");
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) 
	{
		dev_err(&client->dev, "Must have I2C_FUNC_I2C.\n");
		ret = -ENODEV;
		goto exit;
	}

	ts = kzalloc(sizeof(struct rk_ts_data), GFP_KERNEL);
	if (ts == NULL) {
		printk(KERN_ALERT "alloc for struct rk_ts_data fail\n");
		ret = -ENOMEM;
		goto exit;
	}

	pdata = client->dev.platform_data;
	ts->irq_pin = pdata->irq_pin;
	ts->rst_pin = pdata->reset;
	ts->lock_pin = pdata->mode_check_pin;
	ts->valid_indicate_pin = pdata->valid_indicate_pin;
	ts->pwr_pin = pdata->power_control;
	ts->pendown =PEN_RELEASE;
	ts->client = client;
	ts->ts_init = goodix_ts_init;	
	ts->power = goodix_ts_power;
	ts->get_touch_info = goodix_get_touch_info;
	ts->input_parms_init = goodix_input_params_init;
	i2c_set_clientdata(client, ts);

	if(pdata->init_platform_hw)
		pdata->init_platform_hw();

	gpio_set_value(pdata->reset, 1);
	msleep(200);
	
	if(ts->ts_init)
	{
		ret = ts->ts_init(ts);
		if(ret < 0)
		{
			printk(KERN_ALERT "rk ts init fail\n");
			goto exit;
		}
	}

	if(ts->input_parms_init)
	{
		ts->input_parms_init(ts);
	}

	i2c_connect_client = client;
	#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,32)
		ts->ts_wq= create_rt_workqueue("rk_ts_wq");		//create a work queue and worker thread
	#else
		ts->ts_wq= create_workqueue("rk_ts_wq"); 
	#endif
	if (!ts->ts_wq){
		printk(KERN_ALERT "creat touch screen workqueue failed\n");
	    return -ENOMEM;
	}
	
	INIT_DELAYED_WORK(&ts->ts_work, rk_ts_work_func);
#ifdef CONFIG_HAS_EARLYSUSPEND
	ts->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	ts->early_suspend.suspend = rk_ts_early_suspend;
	ts->early_suspend.resume = rk_ts_late_resume;
	register_early_suspend(&ts->early_suspend);
#endif

	info_buf= kzalloc(ts->max_touch_num*sizeof(struct rk_touch_info), GFP_KERNEL);
	if(!info_buf)
	{
		printk(KERN_ALERT "alloc for rk_touch_info fail\n");
		goto err_input_register_device_failed;
	}

	ts->irq=gpio_to_irq(ts->irq_pin)	;		//If not defined in client
	if (ts->irq)
	{
		ret = gpio_request(pdata->irq_pin, "TS_IRQ_PIN");	//Request IO
		if (ret < 0) 
		{
			printk(KERN_ALERT "Failed to request for touch irq\n");
			goto err_input_register_device_failed;
		}
		else
		{
			gpio_direction_input(pdata->irq_pin);
		}

		ret  = request_irq(ts->irq, rk_ts_irq_handler ,IRQ_TYPE_LEVEL_LOW/*IRQF_TRIGGER_FALLING*/,client->name, ts);
		if (ret != 0) {
			printk(KERN_ALERT "Cannot allocate ts INT!ERRNO:%d\n", ret);
			gpio_free(ts->irq_pin);
			goto err_input_register_device_failed;
		}
	}
	printk("goodix_ts_init: probe successfully!\n");
	return 0;

	
err_input_register_device_failed:
	input_free_device(ts->input_dev);
	i2c_set_clientdata(client, NULL);	
	kfree(ts);
exit:
	return ret;
}


/*******************************************************
Description:
	Goodix touchscreen driver release function.

Parameter:
	client:	i2c device struct.
	
return:
	Executive outcomes. 0---succeed.
*******************************************************/
static int rk_ts_remove(struct i2c_client *client)
{
	struct rk_ts_data *ts = i2c_get_clientdata(client);
#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&ts->early_suspend);
#endif
#ifdef CONFIG_TOUCHSCREEN_GOODIX_IAP
	remove_proc_entry("goodix-update", NULL);
#endif
	
	gpio_free(ts->irq_pin);
	free_irq(ts->irq, ts);
	dev_notice(&client->dev,"The driver is removing...\n");
	i2c_set_clientdata(client, NULL);
	input_unregister_device(ts->input_dev);
	if(info_buf)
		kfree(info_buf);
	kfree(ts);
	return 0;
}

static void rk_ts_shutdown(struct i2c_client *client)
{
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct rk_ts_data *ts = i2c_get_clientdata(client);
	unregister_early_suspend(&ts->early_suspend);
#endif
}

//******************************Begin of firmware update surpport*******************************
#ifdef CONFIG_TOUCHSCREEN_GOODIX_IAP
/**
@brief CRC cal proc,include : Reflect,init_crc32_table,GenerateCRC32
@param global var oldcrc32
@return states
*/
static unsigned int Reflect(unsigned long int ref, char ch)
{
	unsigned int value=0;
	int i;
	for(i = 1; i < (ch + 1); i++)
	{
		if(ref & 1)
			value |= 1 << (ch - i);
		ref >>= 1;
	}
	return value;
}
/*---------------------------------------------------------------------------------------------------------*/
/*  CRC Check Program INIT								                                           		   */
/*---------------------------------------------------------------------------------------------------------*/
static void init_crc32_table(void)
{
	unsigned int temp;
	unsigned int t1,t2;
	unsigned int flag;
	int i,j;
	for(i = 0; i <= 0xFF; i++)
	{
		temp=Reflect(i, 8);
		crc32_table[i]= temp<< 24;
		for (j = 0; j < 8; j++)
		{

			flag=crc32_table[i]&0x80000000;
			t1=(crc32_table[i] << 1);
			if(flag==0)
				t2=0;
			else
				t2=ulPolynomial;
			crc32_table[i] =t1^t2 ;

		}
		crc32_table[i] = Reflect(crc32_table[i], 32);
	}
}
/*---------------------------------------------------------------------------------------------------------*/
/*  CRC main Program									                                           		   */
/*---------------------------------------------------------------------------------------------------------*/
static void GenerateCRC32(unsigned char * buf, unsigned int len)
{
	unsigned int i;
	unsigned int t;

	for (i = 0; i != len; ++i)
	{
		t = (oldcrc32 ^ buf[i]) & 0xFF;
		oldcrc32 = ((oldcrc32 >> 8) & 0xFFFFFF) ^ crc32_table[t];
	}
}

static struct file * update_file_open(char * path, mm_segment_t * old_fs_p)
{
	struct file * filp = NULL;
	int errno = -1;
		
	filp = filp_open(path, O_RDONLY, 0644);
	
	if(!filp || IS_ERR(filp))
	{
		if(!filp)
			errno = -ENOENT;
		else 
			errno = PTR_ERR(filp);					
		printk(KERN_ERR "The update file for Guitar open error.\n");
		return NULL;
	}
	*old_fs_p = get_fs();
	set_fs(get_ds());

	filp->f_op->llseek(filp,0,0);
	return filp ;
}

static void update_file_close(struct file * filp, mm_segment_t old_fs)
{
	set_fs(old_fs);
	if(filp)
		filp_close(filp, NULL);
}
static int update_get_flen(char * path)
{
	struct file * file_ck = NULL;
	mm_segment_t old_fs;
	int length ;
	
	file_ck = update_file_open(path, &old_fs);
	if(file_ck == NULL)
		return 0;

	length = file_ck->f_op->llseek(file_ck, 0, SEEK_END);
	//printk("File length: %d\n", length);
	if(length < 0)
		length = 0;
	update_file_close(file_ck, old_fs);
	return length;	
}
static int update_file_check(char * path)
{
	unsigned char buffer[64] = { 0 } ;
	struct file * file_ck = NULL;
	mm_segment_t old_fs;
	int count, ret, length ;
	
	file_ck = update_file_open(path, &old_fs);
	
	if(path != NULL)
		printk("File Path:%s\n", path);
	
	if(file_ck == NULL)
		return -ERROR_NO_FILE;

	length = file_ck->f_op->llseek(file_ck, 0, SEEK_END);
#ifdef GUITAR_MESSAGE
	printk(KERN_INFO "gt801 update: File length: %d\n",length);
#endif	
	if(length <= 0 || (length%4) != 0)
	{
		update_file_close(file_ck, old_fs);
		return -ERROR_FILE_TYPE;
	}
	
	//set file point to the begining of the file
	file_ck->f_op->llseek(file_ck, 0, SEEK_SET);	
	oldcrc32 = 0xFFFFFFFF;
	init_crc32_table();
	while(length > 0)
	{
		ret = file_ck->f_op->read(file_ck, buffer, sizeof(buffer), &file_ck->f_pos);
		if(ret > 0)
		{
			for(count = 0; count < ret;  count++) 	
				GenerateCRC32(&buffer[count],1);			
		}
		else 
		{
			update_file_close(file_ck, old_fs);
			return -ERROR_FILE_READ;
		}
		length -= ret;
	}
	oldcrc32 = ~oldcrc32;
#ifdef GUITAR_MESSAGE	
	printk("CRC_Check: %u\n", oldcrc32);
#endif	
	update_file_close(file_ck, old_fs);
	return 1;	
}

unsigned char wait_slave_ready(struct rk_ts_data *ts, unsigned short *timeout)
{
	unsigned char i2c_state_buf[2] = {ADDR_STA, UNKNOWN_ERROR};
	int ret;
	while(*timeout < MAX_TIMEOUT)
	{
		ret = goodix_i2c_read_bytes(ts->client, i2c_state_buf, 2);
		if(ret <= 0)
			return ERROR_I2C_TRANSFER;
		if(i2c_state_buf[1] & SLAVE_READY)
		{
			return i2c_state_buf[1];
			//return 1;
		}
		msleep(10);
		*timeout += 5;
	}
	return 0;
}

static int goodix_update_write(struct file *filp, const char __user *buff, unsigned long len, void *data)
{
	unsigned char cmd[220];
	int ret = -1;

	static unsigned char update_path[100];
	static unsigned short time_count = 0;
	static unsigned int file_len = 0;
	
	unsigned char i2c_control_buf[2] = {ADDR_CMD, 0};
	unsigned char i2c_states_buf[2] = {ADDR_STA, 0};
	unsigned char i2c_data_buf[PACK_SIZE+1+8] = {ADDR_DAT,};
	//unsigned char i2c_rd_buf[1+4+PACK_SIZE+4];
	unsigned char i2c_rd_buf[160];
	unsigned char retries = 0;
	unsigned int rd_len;
	unsigned char i = 0;
	static unsigned char update_need_config = 0;

	unsigned char checksum_error_times = 0;
#ifdef UPDATE_NEW_PROTOCOL
	unsigned int frame_checksum = 0;
	unsigned int frame_number = 0;
#else
	unsigned char send_crc = 0;
#endif

	struct file * file_data = NULL;
	mm_segment_t old_fs;
	struct rk_ts_data *ts;
	
	ts = i2c_get_clientdata(i2c_connect_client);
	if(ts==NULL)
		return 0;
	
	if(copy_from_user(&cmd, buff, len))
	{
		return -EFAULT;
	}
	switch(cmd[0])
	{
		case STEP_SET_PATH:
			printk(KERN_INFO"Write cmd is:%d,cmd arg is:%s,write len is:%ld\n",cmd[0], &cmd[1], len);
			memset(update_path, 0, 100);
			strncpy(update_path, cmd+1, 100);
			if(update_path[0] == 0)
				return 0;
			else
				return 1;
		case STEP_CHECK_FILE:
			printk(KERN_INFO"Begin to firmware update ......\n");
			ret = update_file_check(update_path);
			if(ret <= 0)
			{
				printk(KERN_INFO"fialed to check update file!\n");
				return ret;
			}
			msleep(500);
			printk(KERN_INFO"Update check file success!\n");
			return 1;
		case STEP_WRITE_SYN:
			printk(KERN_INFO"STEP1:Write synchronization signal!\n");
			i2c_control_buf[1] = UPDATE_START;
			ret = goodix_i2c_write_bytes(ts->client, i2c_control_buf, 2);
			if(ret <= 0)
			{
				ret = ERROR_I2C_TRANSFER;
				return ret;
			}
			//the time include time(APROM -> LDROM) and time(LDROM init)
			msleep(1000);
			return 1;
		case STEP_WAIT_SYN:
			printk(KERN_INFO"STEP2:Wait synchronization signal!\n");
			while(retries < MAX_I2C_RETRIES)
			{
				i2c_states_buf[1] = UNKNOWN_ERROR;
				ret = goodix_i2c_read_bytes(ts->client, i2c_states_buf, 2);
				printk(KERN_INFO"The read byte is:%d\n", i2c_states_buf[1]);
				if(i2c_states_buf[1] & UPDATE_START)
				{
					if(i2c_states_buf[1] & NEW_UPDATE_START)
					{
					#ifdef UPDATE_NEW_PROTOCOL
						update_need_config = 1;
						return 2;
					#else
						return 1;
					#endif
					}
					break;
				}
				msleep(5);
				retries++;
				time_count += 10;
			}
			if((retries >= MAX_I2C_RETRIES) && (!(i2c_states_buf[1] & UPDATE_START)))
			{
				if(ret <= 0)
					return 0;
				else
					return -1;
			}
			return 1;
		case STEP_WRITE_LENGTH:
			printk(KERN_INFO"STEP3:Write total update file length!\n");
			file_len = update_get_flen(update_path);
			if(file_len <= 0)
			{
				printk(KERN_INFO"get update file length failed!\n");
				return -1;
			}
			file_len += 4;
			i2c_data_buf[1] = (file_len>>24) & 0xff;
			i2c_data_buf[2] = (file_len>>16) & 0xff;
			i2c_data_buf[3] = (file_len>>8) & 0xff;
			i2c_data_buf[4] = file_len & 0xff;
			file_len -= 4;
			ret = goodix_i2c_write_bytes(ts->client, i2c_data_buf, 5);
			if(ret <= 0)
			{
				ret = ERROR_I2C_TRANSFER;
				return 0;
			}
			return 1;
		case STEP_WAIT_READY:
			printk(KERN_INFO"STEP4:Wait slave ready!\n");
			ret = wait_slave_ready(ts, &time_count);
			if(ret == ERROR_I2C_TRANSFER)
				return 0;
			if(!ret)
			{
				return -1;
			}
			printk(KERN_INFO"Slave ready!\n");
			return 1;
		case STEP_WRITE_DATA:
#ifdef UPDATE_NEW_PROTOCOL
			printk(KERN_INFO"STEP5:Begin to send file data use NEW protocol!\n");
			file_data = update_file_open(update_path, &old_fs);
			if(file_data == NULL)
			{
				return -1;
			}
			frame_number = 0;
			while(file_len >= 0)
			{
				i2c_data_buf[0] = ADDR_DAT;
				rd_len = (file_len >= PACK_SIZE) ? PACK_SIZE : file_len;
				frame_checksum = 0;
				if(file_len)
				{
					ret = file_data->f_op->read(file_data, i2c_data_buf+1+4, rd_len, &file_data->f_pos);
					if(ret <= 0)
					{
						printk("[GOODiX_ISP_NEW]:Read File Data Failed!\n");
						return -1;
					}
					i2c_data_buf[1] = (frame_number>>24)&0xff;
					i2c_data_buf[2] = (frame_number>>16)&0xff;
					i2c_data_buf[3] = (frame_number>>8)&0xff;
					i2c_data_buf[4] = frame_number&0xff;
					frame_number++;
					frame_checksum = 0;
					for(i=0; i<rd_len; i++)
					{
						frame_checksum += i2c_data_buf[5+i];
					}
					frame_checksum = 0 - frame_checksum;
					i2c_data_buf[5+rd_len+0] = frame_checksum&0xff;
					i2c_data_buf[5+rd_len+1] = (frame_checksum>>8)&0xff;
					i2c_data_buf[5+rd_len+2] = (frame_checksum>>16)&0xff;
					i2c_data_buf[5+rd_len+3] = (frame_checksum>>24)&0xff;
				}
rewrite:
				printk(KERN_INFO"[GOODiX_ISP_NEW]:%d\n", file_len);				
				ret = goodix_i2c_write_bytes(ts->client, i2c_data_buf, 1+4+rd_len+4);
					//if(ret <= 0)
				if(ret != 1)
				{
					printk("[GOODiX_ISP_NEW]:Write File Data Failed!Return:%d\n", ret);
					return 0;
				}

				memset(i2c_rd_buf, 0x00, 1+4+rd_len+4);
				ret = goodix_i2c_read_bytes(ts->client, i2c_rd_buf, 1+4+rd_len+4);
				if(ret != 2)
				{
					printk("[GOODiX_ISP_NEW]:Read File Data Failed!Return:%d\n", ret);
					return 0;
				}
				for(i=1; i<(1+4+rd_len+4); i++)						//check communication
				{
					if(i2c_rd_buf[i] != i2c_data_buf[i])
					{
						i = 0;
						break;
					}
				}
				if(!i)
				{
					i2c_control_buf[0] = ADDR_CMD;
					i2c_control_buf[1] = 0x03;
					goodix_i2c_write_bytes(ts->client, i2c_control_buf, 2);		//communication error
					printk("[GOODiX_ISP_NEW]:File Data Frame readback check Error!\n");
				}
				else
				{
					i2c_control_buf[1] = 0x04;													//let LDROM write flash
					goodix_i2c_write_bytes(ts->client, i2c_control_buf, 2);
				}
				
				//Wait for slave ready signal.and read the checksum
				ret = wait_slave_ready(ts, &time_count);
				if((ret & CHECKSUM_ERROR)||(!i))
				{
					if(i)
					{
						printk("[GOODiX_ISP_NEW]:File Data Frame checksum Error!\n");
					}
					checksum_error_times++;
					msleep(20);
					if(checksum_error_times > 20)				//max retry times.
						return 0;
					goto rewrite;
				}
				checksum_error_times = 0;
				if(ret & (FRAME_ERROR))
				{
					printk("[GOODiX_ISP_NEW]:File Data Frame Miss!\n");
					return 0;
				}
				if(ret == ERROR_I2C_TRANSFER)
					return 0;
				if(!ret)
				{
					return -1;
				}
				if(file_len < PACK_SIZE)
				{
					update_file_close(file_data, old_fs);
					break;
				}
				file_len -= rd_len;
			}//end of while((file_len >= 0))
			return 1;
#else
			printk(KERN_INFO"STEP5:Begin to send file data use OLD protocol!\n");
			file_data = update_file_open(update_path, &old_fs);
			if(file_data == NULL)	//file_data has been opened at the last time
			{
				return -1;
			}
			while((file_len >= 0) && (!send_crc))
			{
				printk(KERN_INFO"[GOODiX_ISP_OLD]:%d\n", file_len);
				i2c_data_buf[0] = ADDR_DAT;
				rd_len = (file_len >= PACK_SIZE) ? PACK_SIZE : file_len;
				if(file_len)
				{
					ret = file_data->f_op->read(file_data, i2c_data_buf+1, rd_len, &file_data->f_pos);
					if(ret <= 0)
					{
						return -1;
					}
				}
				if(file_len < PACK_SIZE)
				{
					send_crc = 1;
					update_file_close(file_data, old_fs);
					i2c_data_buf[file_len+1] = oldcrc32&0xff;
					i2c_data_buf[file_len+2] = (oldcrc32>>8)&0xff;
					i2c_data_buf[file_len+3] = (oldcrc32>>16)&0xff;
					i2c_data_buf[file_len+4] = (oldcrc32>>24)&0xff;
					ret = goodix_i2c_write_bytes(ts->client, i2c_data_buf, (file_len+1+4));
					//if(ret <= 0)
					if(ret != 1)
					{
						printk("[GOODiX_ISP_OLD]:Write File Data Failed!Return:%d\n", ret);
						return 0;
					}
					break;
				}
				else
				{
					ret = goodix_i2c_write_bytes(ts->client, i2c_data_buf, PACK_SIZE+1);
					//if(ret <= 0)
					if(ret != 1)
					{
						printk("[GOODiX_ISP_OLD]:Write File Data Failed!Return:%d\n", ret);
						return 0;
					}
				}
				file_len -= rd_len;
			
				//Wait for slave ready signal.
				ret = wait_slave_ready(ts, &time_count);
				if(ret == ERROR_I2C_TRANSFER)
					return 0;
				if(!ret)
				{
					return -1;
				}
				//Slave is ready.
			}//end of while((file_len >= 0) && (!send_crc))
			return 1;
#endif
		case STEP_READ_STATUS:
			printk(KERN_INFO"STEP6:Read update status!\n");
			while(time_count < MAX_TIMEOUT)
			{
				ret = goodix_i2c_read_bytes(ts->client, i2c_states_buf, 2);
				if(ret <= 0)
				{
					return 0;
				}
				if(i2c_states_buf[1] & SLAVE_READY)
				{
					if(!(i2c_states_buf[1] &0xf0))
					{
						printk(KERN_INFO"The firmware updating succeed!update state:0x%x\n",i2c_states_buf[1]);
						return 1;
					}
					else
					{
						printk(KERN_INFO"The firmware updating failed!update state:0x%x\n",i2c_states_buf[1]);
						return 0;

					}
				}
				msleep(1);
				time_count += 5;
			}
			return -1;
		case FUN_CLR_VAL:								//clear the static val
			time_count = 0;
			file_len = 0;
			update_need_config = 0;
			return 1;
		case FUN_CMD:							//functional command
			if(cmd[1] == CMD_DISABLE_TP)
			{
				printk(KERN_INFO"Disable TS int!\n");
				g_enter_isp = 1;
				disable_irq(ts->irq);
			}
			else if(cmd[1] == CMD_ENABLE_TP)
			{
				printk(KERN_INFO"Enable TS int!\n");
				g_enter_isp = 0;
				enable_irq(ts->irq);
			}
			else if(cmd[1] == CMD_READ_VER)
			{
				printk(KERN_INFO"Read version!\n");
				ts->read_mode = MODE_RD_VER;
			}
			else if(cmd[1] == CMD_READ_RAW)
			{
				printk(KERN_INFO"Read raw data!\n");
				ts->read_mode = MODE_RD_RAW;
				i2c_control_buf[1] = 201;
				ret = goodix_i2c_write_bytes(ts->client, i2c_control_buf, 2);			//read raw data cmd
				if(ret <= 0)
				{
					printk(KERN_INFO"Write read raw data cmd failed!\n");
					return 0;
				}
				msleep(200);
			}
			else if(cmd[1] == CMD_READ_DIF)
			{
				printk(KERN_INFO"Read diff data!\n");
				ts->read_mode = MODE_RD_DIF;
				i2c_control_buf[1] = 202;
				ret = goodix_i2c_write_bytes(ts->client, i2c_control_buf, 2);			//read diff data cmd
				if(ret <= 0)
				{
					printk(KERN_INFO"Write read raw data cmd failed!\n");
					return 0;
				}
				msleep(200);
			}
			else if(cmd[1] == CMD_READ_CFG)
			{
				printk(KERN_INFO"Read config info!\n");
				ts->read_mode = MODE_RD_CFG;
				rd_cfg_addr = cmd[2];
				rd_cfg_len = cmd[3];
			}
			else if(cmd[1] == CMD_SYS_REBOOT)
			{
				printk(KERN_INFO"System reboot!\n");
				sys_sync();
				msleep(200);
				kernel_restart(NULL);
			}
			return 1;
		case FUN_WRITE_CONFIG:
			
			printk(KERN_INFO"Begin write config info!Config length:%d\n",cmd[1]);
			for(i=3; i<cmd[1];i++)
			{
				//if((i-3)%5 == 0)printk("\n");
				printk("(%d):0x%x ", i-3, cmd[i]);
			}
			printk("\n");

			if((cmd[2]>83)&&(cmd[2]<240)&&cmd[1])
			{
				checksum_error_times = 0;
reconfig:
				ret = goodix_i2c_write_bytes(ts->client, cmd+2, cmd[1]); 
				if(ret != 1)
				{
					printk("Write Config failed!return:%d\n",ret);
					return -1;
				}
				if(!update_need_config)return 1;
				
				i2c_rd_buf[0] = cmd[2];
				ret = goodix_i2c_read_bytes(ts->client, i2c_rd_buf, cmd[1]);
				if(ret != 2)
				{
					printk("Read Config failed!return:%d\n",ret);
					return -1;
				}
				for(i=0; i<cmd[1]; i++)
				{
					if(i2c_rd_buf[i] != cmd[i+2])
					{
						printk("Config readback check failed!\n");
						i = 0;
						break;
					}
				}
				if(!i)
				{
					i2c_control_buf[0] = ADDR_CMD;
					i2c_control_buf[1] = 0x03;
					goodix_i2c_write_bytes(ts->client, i2c_control_buf, 2);		//communication error
					checksum_error_times++;
					msleep(20);
					if(checksum_error_times > 20)				//max retry times.
						return 0;
					goto reconfig;
				}
				else
				{
					i2c_control_buf[0] = ADDR_CMD;
					i2c_control_buf[1] = 0x04;					//let LDROM write flash
					goodix_i2c_write_bytes(ts->client, i2c_control_buf, 2);
					return 1;
				}
				
			}
			else
			{
				printk(KERN_INFO"Invalid config addr!\n");
				return -1;
			}
		default:
			return -ENOSYS;
	}
	return 0;
}

static int goodix_update_read( char *page, char **start, off_t off, int count, int *eof, void *data )
{
	int ret = -1;
	struct rk_ts_data *ts;
	int len = 0;
	char *version_info = NULL;
	static unsigned char read_data[1201] = {80, };

	ts = i2c_get_clientdata(i2c_connect_client);
	if(ts==NULL)
		return 0;

	if(ts->read_mode == MODE_RD_VER)		//read version data
	{
		ret = goodix_read_version(ts, &version_info);
		if(ret <= 0)
		{
			printk(KERN_INFO"Read version data failed!\n");
			vfree(version_info);
			return 0;
		}

		for(len=0;len<100;len++)
		{
			if(*(version_info + len) == '\0')
				break;
		}
		printk(KERN_INFO"GOODiX Touchscreen Version is:%s\n", (version_info+1));
		strncpy(page, version_info+1, len + 1);
		vfree(version_info);
		*eof = 1;
		return len+1;
	}
	else if((ts->read_mode == MODE_RD_RAW)||(ts->read_mode == MODE_RD_DIF))		//read raw data or diff
	{
		//printk(KERN_INFO"Read raw data\n");
		ret = goodix_i2c_read_bytes(ts->client, read_data, 1201);
		if(ret <= 0)
		{
			if(ts->read_mode == 2)
				printk(KERN_INFO"Read raw data failed!\n");
			if(ts->read_mode == 3)
				printk(KERN_INFO"Read diff data failed!\n");
			return 0;
		}
		memcpy(page, read_data+1, 1200);
		*eof = 1;
		*start = NULL;
		return 1200;
	}
	else if(ts->read_mode == MODE_RD_CFG)
	{
		if((rd_cfg_addr>83)&&(rd_cfg_addr<240))
		{
			read_data[0] = rd_cfg_addr;
			printk("read config addr is:%d\n", rd_cfg_addr);
		}
		else
		{
			read_data[0] = 101;
			printk("invalid read config addr,use default!\n");
		}
		if((rd_cfg_len<0)||(rd_cfg_len>156))
		{
			printk("invalid read config length,use default!\n");
			rd_cfg_len = 239 - read_data[0];
		}
		printk("read config length is:%d\n", rd_cfg_len);
		ret = goodix_i2c_read_bytes(ts->client, read_data, rd_cfg_len);
		if(ret <= 0)
		{
			printk(KERN_INFO"Read config info failed!\n");
			return 0;
		}
		memcpy(page, read_data+1, rd_cfg_len);
		return rd_cfg_len;
	}
	return len;
}
              
#endif
//******************************End of firmware update surpport*******************************
static const struct i2c_device_id goodix_ts_id[] = {
	{ "Goodix-TS", 0 },
	{ }
};

static struct i2c_driver rk_ts_driver = {
	.probe		= rk_ts_probe,
	.remove		= rk_ts_remove,
	.shutdown	= rk_ts_shutdown,
#ifndef CONFIG_HAS_EARLYSUSPEND
	.suspend	= rk_ts_suspend,
	.resume		= rk_ts_resume,
#endif
	.id_table	= goodix_ts_id,
	.driver = {
		.name	= "Goodix-TS",
		.owner = THIS_MODULE,
	},
};


static struct class *ts_debug_class = NULL;
static ssize_t dbg_mode_show(struct class *cls,struct class_attribute *attr, char *_buf)
{
       printk("%s>>>>>>>>\n",__func__);
       return 0;
}

static ssize_t dbg_mode_store(struct class *cls,struct class_attribute *attr, const char *buf, size_t _count)
{
	dbg_thresd = simple_strtol(buf,NULL,10);
	if(dbg_thresd)
	{
		printk(KERN_INFO "ts debug open\n");
	}
	else
	{
		printk(KERN_INFO "ts debug close");
	}
      
    return _count;
}
static CLASS_ATTR(debug, 0664, dbg_mode_show, dbg_mode_store);

static int dbg_sys_init(void)
{
	int ret ;
	ts_debug_class = class_create(THIS_MODULE, "ts_debug");
   	ret =  class_create_file(ts_debug_class, &class_attr_debug);
    if (ret)
    {
       printk("Fail to creat class hkrkfb.\n");
    }
   return 0;
}


/*******************************************************	
Description:
	Driver Install function.
return:
	Executive Outcomes. 0---succeed.
********************************************************/

static void __init rk_ts_init_async(void *unused, async_cookie_t cookie)
{
	i2c_add_driver(&rk_ts_driver);
	dbg_sys_init();  //for debug
}

static int __init rk_ts_init(void)
{
	async_schedule(rk_ts_init_async, NULL);
	return 0;
}

/*******************************************************	
Description:
	Driver uninstall function.
return:
	Executive Outcomes. 0---succeed.
********************************************************/
static void __exit rk_ts_exit(void)
{
	printk(KERN_ALERT "Touchscreen driver of guitar exited.\n");
	i2c_del_driver(&rk_ts_driver);
}

late_initcall(rk_ts_init);
module_exit(rk_ts_exit);

MODULE_DESCRIPTION("Goodix Touchscreen Driver");
MODULE_LICENSE("GPL");
