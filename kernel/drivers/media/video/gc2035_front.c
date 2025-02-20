/*
o* Driver for MT9M001 CMOS Image Sensor from Micron
 *
 * Copyright (C) 2008, Guennadi Liakhovetski <kernel@pengutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#define lily_debug 0
#include <linux/videodev2.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/log2.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/circ_buf.h>
#include <linux/miscdevice.h>
#include <media/v4l2-common.h>
#include <media/v4l2-chip-ident.h>
#include <media/soc_camera.h>
#include <plat/rk_camera.h>
#if lily_debug
#include <linux/proc_fs.h>
static struct i2c_client * g_client;
#endif

static int debug;
module_param(debug, int, S_IRUGO|S_IWUSR);

#define dprintk(level, fmt, arg...) do {			\
	if (debug >= level) 					\
	printk(KERN_WARNING fmt , ## arg); } while (0)

#define SENSOR_TR(format, ...) printk(KERN_ERR format, ## __VA_ARGS__)
#define SENSOR_DG(format, ...) dprintk(1, format, ## __VA_ARGS__)


#define _CONS(a,b) a##b
#define CONS(a,b) _CONS(a,b)

#define __STR(x) #x
#define _STR(x) __STR(x)
#define STR(x) _STR(x)

#define MIN(x,y)   ((x<y) ? x: y)
#define MAX(x,y)    ((x>y) ? x: y)

/* Sensor Driver Configuration */
#define SENSOR_NAME RK29_CAM_SENSOR_GC2035_FRONT
#define SENSOR_V4L2_IDENT  V4L2_IDENT_GC2035_FRONT
#define SENSOR_ID 0x2035
#define SENSOR_MIN_WIDTH     800
#define SENSOR_MIN_HEIGHT    600
#define SENSOR_MAX_WIDTH    1600
#define SENSOR_MAX_HEIGHT   1200
#define SENSOR_INIT_WIDTH	 800			/* Sensor pixel size for sensor_init_data array */
#define SENSOR_INIT_HEIGHT  600
#define SENSOR_INIT_WINSEQADR sensor_svga
#define SENSOR_INIT_PIXFMT V4L2_MBUS_FMT_YUYV8_2X8

#define CONFIG_SENSOR_WhiteBalance	1
#define CONFIG_SENSOR_Brightness	0
#define CONFIG_SENSOR_Contrast      0
#define CONFIG_SENSOR_Saturation    0
#define CONFIG_SENSOR_Effect        0
#define CONFIG_SENSOR_Scene         1
#define CONFIG_SENSOR_DigitalZoom   0
#define CONFIG_SENSOR_Focus         0
#define CONFIG_SENSOR_Exposure      0
#define CONFIG_SENSOR_Flash         0
#define CONFIG_SENSOR_Mirror        0
#define CONFIG_SENSOR_Flip          0

#define CONFIG_SENSOR_I2C_SPEED     250000       /* Hz */
/* Sensor write register continues by preempt_disable/preempt_enable for current process not be scheduled */
#define CONFIG_SENSOR_I2C_NOSCHED   0
#define CONFIG_SENSOR_I2C_RDWRCHK   0

#define SENSOR_BUS_PARAM  (SOCAM_MASTER | SOCAM_PCLK_SAMPLE_RISING |\
                          SOCAM_HSYNC_ACTIVE_HIGH | SOCAM_VSYNC_ACTIVE_HIGH |\
                          SOCAM_DATA_ACTIVE_HIGH | SOCAM_DATAWIDTH_8  |SOCAM_MCLK_24MHZ)

#define COLOR_TEMPERATURE_CLOUDY_DN     6500
#define COLOR_TEMPERATURE_CLOUDY_UP      8000
#define COLOR_TEMPERATURE_CLEARDAY_DN  5000
#define COLOR_TEMPERATURE_CLEARDAY_UP    6500
#define COLOR_TEMPERATURE_OFFICE_DN     3500
#define COLOR_TEMPERATURE_OFFICE_UP     5000
#define COLOR_TEMPERATURE_HOME_DN       2500
#define COLOR_TEMPERATURE_HOME_UP       3500

#define SENSOR_NAME_STRING(a) STR(CONS(SENSOR_NAME, a))
#define SENSOR_NAME_VARFUN(a) CONS(SENSOR_NAME, a)

#define SENSOR_AF_IS_ERR    (0x00<<0)
#define SENSOR_AF_IS_OK		(0x01<<0)
#define SENSOR_INIT_IS_ERR   (0x00<<28)
#define SENSOR_INIT_IS_OK    (0x01<<28)

struct reginfo
{
    u8 reg;
    u8 val;
};

//flash off in fixed time to prevent from too hot , zyc
struct  flash_timer{
    struct soc_camera_device *icd;
	struct hrtimer timer;
};
static enum hrtimer_restart flash_off_func(struct hrtimer *timer);

static struct  flash_timer flash_off_timer;
static int flash_on_off=0;
//for user defined if user want to customize the series , zyc

/* init 352X288 SVGA */
static struct reginfo sensor_init_data[] ={
	{0xfe,0x80},
	{0xfe,0x80},
	{0xfe,0x80},
	{0xfc,0x06},
	{0xf2,0x00},
	{0xf3,0x00},
	{0xf4,0x00},
	{0xf5,0x00},
	{0xf9,0xfe}, //[0] pll enable
	{0xfa,0x00},
	{0xf6,0x00},
	{0xf7,0x15}, //pll enable
	#if defined(CONFIG_MALATA_D7806) || defined(CONFIG_MALATA_D8009)
	{0xf8,0x84},
	#else
	{0xf8,0x85},
	#endif
	{0xfe,0x00},
	#if defined(CONFIG_CAMERA_EMI_ENABLE)
	{0xfa,0x11},
	#endif
	{0x82,0x00},
	{0xb3,0x60},
	{0xb4,0x40},
	{0xb5,0x60},
	{0x03,0x02},
	{0x04,0x80},
		//========================measure window,0x
	{0xfe,0x00},
	{0xec,0x06},//04 2012.10.26
	{0xed,0x06},//04 2012.10.26
	{0xee,0x62},//60 2012.10.26
	{0xef,0x92},//90 2012.10.26
		////=============================analog
	{0x0a,0x00}, //row start
	{0x0c,0x02}, //col start
	{0x0d,0x04},
	{0x0e,0xc0},
	{0x0f,0x06}, //Window setting
	{0x10,0x58}, 
#if defined(CONFIG_MALATA_D7803) || defined(CONFIG_MALATA_D7005)
	{0x17,0x17}, //[0]mirror [1]flip
#else
	{0x17,0x14}, //[0]mirror [1]flip
#endif
	{0x18,0x0a}, //0a 2012.10.26
	{0x19,0x0a}, //AD pipe number

	{0x1a,0x01}, //CISCTL mode4


	{0x1b,0x8b},
	{0x1c,0x05},//add by lanking 20130403
	{0x1e,0x88}, //analog mode1 [7] tx-high en [5:3]COL_bias
	{0x1f,0x08}, //[3] tx-low en//

	{0x20,0x05}, //[0]adclk mode,0x[1]rowclk_MODE [2]rsthigh_en
	{0x21,0x0f}, //[6:4]rsg
	{0x22,0xf0}, //[3:0]vref
	{0x23,0xc3}, //f3//ADC_r
	{0x24,0x16}, //pad drive

		//==============================aec
		//AEC
	{0xfe,0x01},
	{0x11,0x20},//AEC_out_slope,0x
	{0x1f,0xa0},//max_post_gain
	{0x20,0x50},//max_pre_gain
	{0x47,0x30},//AEC_outdoor_th
	{0x0b,0x10},//
	#if defined(CONFIG_MALATA_D1014)||defined(CONFIG_MALATA_D8009)
	{0x13,0x7a},//y_target
	#else
	{0x13,0x80},//y_target
	#endif

		
#if defined(CONFIG_MALATA_D7806) || defined(CONFIG_MALATA_D8009)

	///banding for f8->0x84
	{0xfe, 0x00},
	{0x05 , 0x01},//hb
	{0x06 , 0x05},
	{0x07 , 0x00},//vb
	{0x08 , 0x72},
	{0xfe , 0x01},
	{0x27 , 0x00},//step
	{0x28 , 0x92},
	{0x29 , 0x05},//level1
	{0x2a , 0x22},
	{0x2b , 0x05},//level2
	{0x2c , 0xb4},
	{0x2d , 0x05},////level3
	{0x2e , 0xb4},
	{0x2f , 0x07},//level4
	{0x30 , 0x6a},
	{0x3e , 0x40},  //0x40
	{0xfe , 0x00},
	
#else
	{0xfe,0x00},
	{0x05 , 0x01},//hb
	{0x06 , 0x11},
	{0x07 , 0x00},//vb
	{0x08 , 0x50},
	{0xfe , 0x01},
	{0x27 , 0x00},//step
	{0x28 , 0xa0},
	{0x29 , 0x05},//level1
	{0x2a , 0x00},
	{0x2b , 0x06},//level2
	{0x2c , 0x40},
	{0x2d , 0x07},//6e8//level3
	{0x2e , 0x80},
	{0x2f , 0x0f},//level4
	{0x30 , 0x00},
	{0x3e , 0x40},	
#endif
	{0xfe , 0x00},
	{0xfe , 0x00},  //0x , 0x , 0x , 0x , 0x
	{0xb6 , 0x03}, //AEC enable
	{0xfe , 0x00},

	/////////BLK//////
	{0x3f,0x00}, //prc close
	{0x40,0x77},//
	{0x42,0x7f},
	{0x43,0x30},

	{0x5c,0x08},
	{0x5e,0x20},
	{0x5f,0x20},
	{0x60,0x20},
	{0x61,0x20},
	{0x62,0x20},
	{0x63,0x20},
	{0x64,0x20},
	{0x65,0x20},

	///block////////////
	{0x80,0xff},
	{0x81,0x26},//38,0x//skin_Y 8c_debug
	{0x87,0x90}, //[7]middle gamma 
	{0x84,0x00}, //output put foramat
	{0x86,0x07}, //02 //sync plority 
	{0x8b,0xbc},
	{0xb0,0x80}, //globle gain
	{0xc0,0x40},//Yuv bypass


		//===============================lsc

   #if defined(CONFIG_MALATA_D1014)||defined(CONFIG_MALATA_D8009)
     // 0x17 0x14
      {0xfe, 0x00},
      {0xae, 0x78},
	{0xfe, 0x01},
	{0xc2, 0x15},
	{0xc3, 0x0c},
	{0xc4, 0x04},
	{0xc8, 0x15},
	{0xc9, 0x0e},
	{0xca, 0x03},
	{0xbc, 0x24},
	{0xbd, 0x10},
	{0xbe, 0x0d},
	{0xb6, 0x2e},
	{0xb7, 0x1f},
	{0xb8, 0x00},
	{0xc5, 0x00},
	{0xc6, 0x00},
	{0xc7, 0x00},
	{0xcb, 0x00},
	{0xcc, 0x00},
	{0xcd, 0x05},
	{0xbf, 0x00},
	{0xc0, 0x00},
	{0xc1, 0x00},
	{0xb9, 0x10},
	{0xba, 0x0b},
	{0xbb, 0x25},
	{0xaa, 0x11},
	{0xab, 0x1b},
	{0xac, 0x0e},
	{0xad, 0x0e},
	{0xae, 0x12},
	{0xaf, 0x12},
	{0xb0, 0x1d},
	{0xb1, 0x1a},
	{0xb2, 0x19},
	{0xb3, 0x13},
	{0xb4, 0x16},
	{0xb5, 0x11},
	{0xd0, 0xf0},
	{0xd2, 0xc0},
	{0xd3, 0xf8},
	{0xd8, 0xf0},
	{0xda, 0xf0},
	{0xdb, 0xf0},
	{0xdc, 0x30},
	{0xde, 0x40},
	{0xdf, 0x50},
	{0xd4, 0x20},
	{0xd6, 0x30},
	{0xd7, 0x48},
	{0xa4, 0x00},
	{0xa5, 0x00},
	{0xa6, 0x06},
	{0xa7, 0x00},
	{0xa8, 0x00},
	{0xa9, 0x00},
	{0xa1, 0x80},
	{0xa2, 0x80},
	#elif defined(CONFIG_MALATA_D1013)
	{0xfe, 0x01},
	{0xc2, 0x15},
{0xc3, 0x0c},  
{0xc4, 0x04}, 
{0xc8, 0x15},
{0xc9, 0x0e},   
{0xca, 0x03},  //
{0xbc, 0x27},
{0xbd, 0x10},  
{0xbe, 0x0d},
{0xb6, 0x2e},
{0xb7, 0x1f},   
{0xb8, 0x00},   //
{0xc5, 0x00},
{0xc6, 0x00},
{0xc7, 0x00},
{0xcb, 0x00},
{0xcc, 0x00},  
{0xcd, 0x05},  //
{0xbf, 0x00},
{0xc0, 0x00},
{0xc1, 0x00},
{0xb9, 0x10},
{0xba, 0x0b},
{0xbb, 0x25},   //
{0xaa, 0x11},
{0xab, 0x1b},
{0xac, 0x0e},
{0xad, 0x0e},
{0xae, 0x12},
{0xaf, 0x12},     ///
{0xb0, 0x1d},
{0xb1, 0x1a},
{0xb2, 0x19},
{0xb3, 0x13},
{0xb4, 0x16},
{0xb5, 0x11},    ///
{0xd0, 0x25},
{0xd2, 0x00},
{0xd3, 0x39},
{0xd8, 0x09},
{0xda, 0x06},
{0xdb, 0x0f},   ///
{0xdc, 0x06},
{0xde, 0x09},
{0xdf, 0x0c},
{0xd4, 0x10},
{0xd6, 0x00},
{0xd7, 0x25},    ///
{0xa4, 0x00},
{0xa5, 0x00},
{0xa6, 0x06},
{0xa7, 0x00},
{0xa8, 0x00},
{0xa9, 0x00},
{0xa1, 0x80},
{0xa2, 0x80},
#else
	{0xfe,0x01},
	{0xc2,0x14},
	{0xc3,0x0b},
	{0xc4,0x06},
	{0xc8,0x1b},
	{0xc9,0x0c},
	{0xca,0x00},
	{0xbc,0x19},
	{0xbd,0x0c},
	{0xbe,0x0b},
	{0xb6,0x28},
	{0xb7,0x10},
	{0xb8,0x00},
	{0xc5,0x00},
	{0xc6,0x00},
	{0xc7,0x00},
	{0xcb,0x22},
	{0xcc,0x0f},
	{0xcd,0x07},
	{0xbf,0x10},
	{0xc0,0x0c},
	{0xc1,0x0b},
	{0xb9,0x10},
	{0xba,0x0f},
	{0xbb,0x0c},
	{0xaa,0x1a},
	{0xab,0x15},
	{0xac,0x1c},
	{0xad,0x23},
	{0xae,0x1e},
	{0xaf,0x26},
	{0xb0,0x12},
	{0xb1,0x0f},
	{0xb2,0x14},
	{0xb3,0x13},
	{0xb4,0x12},
	{0xb5,0x1c},
	{0xd0,0x2f},
	{0xd2,0x39},
	{0xd3,0x40},
	{0xd8,0x0f},
	{0xda,0x24},
	{0xdb,0x19},
	{0xdc,0x00},
	{0xde,0x0a},
	{0xdf,0x0b},
	{0xd4,0x00},
	{0xd6,0x0d},
	{0xd7,0x04},
	{0xa4,0x00},
	{0xa5,0x00},
	{0xa6,0x60},
	{0xa7,0x06},
	{0xa8,0x00},
	{0xa9,0x00},
	{0xa1,0x80},
	{0xa2,0x80},
       #endif
		//=================================cc
	{0xfe,0x02},
	{0xc0,0x01},
	{0xc1,0x40}, //Green_cc for d
	{0xc2,0xfc},
	{0xc3,0x05},
	{0xc4,0xec},
	{0xc5,0x42},
	{0xc6,0xf8},
	{0xc7,0x40},//for cwf 
	{0xc8,0xf8},
	{0xc9,0x06},
	{0xca,0xfd},
	{0xcb,0x3e},
	{0xcc,0xf3},
	{0xcd,0x36},//for A
	{0xce,0xf6},
	{0xcf,0x04},
	{0xe3,0x0c},
	{0xe4,0x44},
	{0xe5,0xe5},
	{0xfe,0x00},

		//==============================awb
		//AWB clear
	{0xfe,0x01},
	{0x4f,0x00},
	{0x4d,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4d,0x10}, // 10
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4d,0x20}, // 20
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4d,0x30},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00}, // 30
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4d,0x40}, // 40
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4d,0x50}, // 50
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4d,0x60}, // 60
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4d,0x70}, // 70
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4d,0x80}, // 80
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4d,0x90}, // 90
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4d,0xa0}, // a0
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4d,0xb0}, // b0
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4d,0xc0}, // c0
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4d,0xd0}, // d0
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4e,0x00},
	{0x4f,0x01},
		/////// awb value////////
	{0xfe,0x01},
	{0x4f,0x00},
	{0x4d,0x30},
	{0x4e,0x00},
	{0x4e,0x80},
	{0x4e,0x80},
	{0x4e,0x02},
	{0x4e,0x02},
	{0x4d,0x40},
	{0x4e,0x00},
	{0x4e,0x80},
	{0x4e,0x80},
	{0x4e,0x02},
	{0x4e,0x02},
	{0x4e,0x02},
	{0x4d,0x53},
	{0x4e,0x08},
	{0x4e,0x04},
	{0x4d,0x62},
	{0x4e,0x10},
	{0x4d,0x72},
	{0x4e,0x20},
	{0x4f,0x01},

		/////awb////
	{0xfe,0x01},
	{0x50,0x88},//c0//[6]green mode
	{0x52,0x40},
	{0x54,0x60},
	{0x56,0x06},
	{0x57,0x20}, //pre adjust
	{0x58,0x01}, 
	{0x5b,0x02}, //AWB_gain_delta
	{0x61,0xaa},//R/G stand
	{0x62,0xaa},//R/G stand
	{0x71,0x00},
	{0x74,0x10},//AWB_C_max
	{0x77,0x08}, // 0x//AWB_p2_x
	{0x78,0xfd}, //AWB_p2_y
	{0x86,0x30},
	{0x87,0x00},
	{0x88,0x04},//06},[1]dark mode
	{0x8a,0xc0},//awb move mode
	{0x89,0x75},
	{0x84,0x08},//auto_window
	{0x8b,0x00}, // 0x//awb compare luma
	{0x8d,0x70}, //awb gain limit R 
	{0x8e,0x70},//G
	{0x8f,0xf4},//B
	{0xfe,0x00},
	{0x82,0x02},//awb_en

		///==========================asde
	{0xfe,0x01},
	{0x21,0xbf},
	{0xfe,0x02},
	{0xa4,0x00},//asde_offset_slope
	{0xa5,0x40}, //lsc_th
	{0xa2,0xa0}, //lsc_dec_slope
	{0xa6,0x80}, //dd_th
	{0xa7,0x80}, //ot_th
	{0xab,0x31}, //[0]b_dn_effect_dark_inc_or_dec
	{0xa9,0x6f}, //[7:4] ASDE_DN_b_slope_high
		//[3:0] ASDE_DN_b_slope_low
	{0xb0,0x99}, //0x//edge effect slope low
	{0xb1,0x34},//edge effect slope low
	{0xb3,0x70}, //saturation dec slope  80
	{0xb4,0x41},	//ASDE_auto_saturation   anstor add  
	{0xde,0xb6},
	{0x38,0x0f }, // auto_gray_dec_slope  0f
	{0x39,0x40}, //
	{0xfe,0x00},
	{0x81,0x26},

	{0xfe,0x02},
	{0x83,0x00},//[6]green_bks_auto [5]gobal_green_bks
	{0x84,0x45},//RB offset
		///=================YCP
	#if defined(CONFIG_MALATA_D1014)||defined(CONFIG_MALATA_D8009)
	{0xd1,0x34},//saturation_cb
	{0xd2,0x33},//saturation_Cr
	#elif defined(CONFIG_MALATA_D1013)
	{0xd1,0x36},//saturation_cb
	{0xd2,0x35},//saturation_Cr
	#else
	{0xd1,0x40},//saturation_cb
	{0xd2,0x40},//saturation_Cr
	#endif
	{0xd3,0x40},//contrast 2012.10.26????��?
	{0xd4,0x80},//contrast center 2012.10.26????��?
	{0xd5,0xfc},//luma_offset 2012.10.26????��?
	{0xdc,0x30},
	{0xdd,0xb8},//edge_sa_g,b
	{0xfe,0x00},
		////=================dndd
	{0xfe,0x02},
	{0x88,0x1f},//  15  dn_b_base
	{0x8c,0xf6}, //[2]b_in_dark_inc
	{0x89,0x01}, //  03  dn_c_weight
		////==================EE 2012.10.26
	{0xfe,0x02},
	{0x90,0x6c},// EEINTP mode1
	{0x97,0x48},// edge effect
		////==============RGB Gamma ???��?gamma 2012.10.26
	{0xfe,0x02},
	{0x15,0x05},
	{0x16,0x0b},
	{0x17,0x10},
	{0x18,0x16},
	{0x19,0x24},
	{0x1a,0x32},
	{0x1b,0x42},
	{0x1c,0x4e},
	{0x1d,0x64},
	{0x1e,0x76},
	{0x1f,0x86},
	{0x20,0x94},
	{0x21,0x9f},
	{0x22,0xb4},
	{0x23,0xc3},
	{0x24,0xce},
	{0x25,0xd7},
	{0x26,0xe3},
	{0x27,0xec},
	{0x28,0xf0},   //  0xf7
	{0x29,0xf6},   //  0xff 
		///=================y gamma
	{0xfe,0x02},
	{0x2b,0x00},
	{0x2c,0x04},
	{0x2d,0x09},
	{0x2e,0x18},
	{0x2f,0x27},
	{0x30,0x37},
	{0x31,0x49},
	{0x32,0x5c},
	{0x33,0x7e},
	{0x34,0xa0},
	{0x35,0xc0},
	{0x36,0xe0},
	{0x37,0xf0},   // 0xff
		/////1600x1200size// 
	{0xfe,0x00},//
	{0x90,0x01}, //0x//crop enable
	{0x94 , 0x04},// x_start   hhs_0609
	{0x95,0x04},//1600x1200
	{0x96,0xb0},
	{0x97,0x06},
	{0x98,0x40},
	{0xfe,0x03},
	{0x42,0x40}, 
	{0x43,0x06}, //output buf width 800x2
	{0x41,0x02}, // Pclk_polarity
	{0x40,0x40},
	{0x17,0x00}, //widv
	{0xfe,0x00},

		////output DVP/////
		//Sleep(100},
	{0xfe,0x00},
	{0x82,0xfe},
	{0xf2,0x70}, 
	{0xf3,0xff},
	{0xf4,0x00},
	{0xf5,0x30},
      ////////sabsumple  800X600//////
	{0xfe,0x00},	
	{0xfa,0x00},
	{0xc8,0x00},
	{0x99,0x22},
	{0x9a,0x07},
	{0x9b,0x00},  
	{0x9c,0x00},
	{0x9d,0x00},
	{0x9e,0x00},
	{0x9f,0x00},
	{0xa0,0x00},
	{0xa1,0x00},
	{0xa2,0x00},
	{0x90,0x01},
	{0x95,0x02},
	{0x96,0x58},
	{0x97,0x03},
	{0x98,0x20},
	
 #if 0   
        /////////  re zao///
	{0xfe,0x00},
	{0x22,0xd0},
	{0xfe,0x01},
	{0x21,0xff},
	{0xfe,0x02},  
	{0x8a,0x33},
	{0x8c,0x76},
	{0x8d,0x85},
	{0xa6,0xf0},	
	{0xae,0x9f},
	{0xa2,0x90},
	{0xa5,0x40},  
	{0xa7,0x30},
	{0xb0,0x88},
	{0x38,0x0b},
	{0x39,0x30},
	{0xfe,0x00},  
	{0x87,0xb0},

       //// small  RGB gamma////
	{0xfe , 0x02},
	{0x15 , 0x0b},
	{0x16 , 0x0e},
	{0x17 , 0x10},
	{0x18 , 0x12},
	{0x19 , 0x19},
	{0x1a , 0x21},
	{0x1b , 0x29},
	{0x1c , 0x31},
	{0x1d , 0x41},
	{0x1e , 0x50},
	{0x1f , 0x5f},
	{0x20 , 0x6d},
	{0x21 , 0x79},
	{0x22 , 0x91},
	{0x23 , 0xa5},
	{0x24 , 0xb9},
	{0x25 , 0xc9},
	{0x26 , 0xe1},
	{0x27 , 0xee},
	{0x28 , 0xf7},
	{0x29 , 0xff},
	
 	////dark sun/////
	{0xfe , 0x02},
	{0x40 , 0x06},
	{0x41 , 0x23},
	{0x42 , 0x3f},
	{0x43 , 0x06},
	{0x44 , 0x00},
	{0x45 , 0x00},
	{0x46 , 0x14},
	{0x47 , 0x09},
 
  #endif
	#if defined(CONFIG_CAMERA_EMI_ENABLE)
	{0xfa,0x11},
	#else
	{0xfa,0x00},
	#endif
	{0x00,0x00}, 
};
	


/* 1600X1200 UXGA */
static struct reginfo sensor_uxga[] = 
{
 
  	 ////////sabsumple  1600X1200//////
	{0xfe,0x00},	
	{0xfa,0x11},
	{0xc8,0x00},
	{0x99,0x11},
	{0x9a,0x06},
	{0x9b,0x00},
	{0x9c,0x00},
	{0x9d,0x00},
	{0x9e,0x00},
	{0x9f,0x00},
	{0xa0,0x00},
	{0xa1,0x00},
	{0xa2,0x00},
	{0x90,0x01},
	{0x95,0x04},
	{0x96,0xb0},
	{0x97,0x06},
	{0x98,0x40},
	{0x00,0x00}, 
};

	

/* 1280X1024 SXGA */
static struct reginfo sensor_sxga[] =
{
	{0x00,0x00}, 
};
static struct reginfo sensor_720p[] =
{
       {0x00,0x00},
	{0xfe ,0x00},
	{0x0a ,0x00}, //row start   f0
	{0x0c ,0x00}, //col start     a0
	{0x0d ,0x04},
	{0x0e ,0xc0},
	{0x0f ,0x06}, //Window setting
	{0x10 ,0x58}, 
	//measure window	
	{0xfe,0x00},
	{0xec,0x04},
	{0xed,0x04},
	{0xee,0x50},
	{0xef,0x58},
	{0x05 , 0x01},//hb
	{0x06 , 0x11},
	{0x07 , 0x00},//b
	{0x08 , 0xf6},
	{0xfe , 0x01},
	{0x27 , 0x00},//step
	{0x28 , 0xa1},
	{0x29 , 0x03},//level1  16 fps
	{0x2a , 0xc6},
	{0x2b , 0x04},//level2  14
	{0x2c , 0x67},
	{0x2d , 0x06},//level3   10
	{0x2e , 0x4a},
	{0x2f , 0x07},//level4  8
	{0x30 , 0x8c},	
	{0x3e,0x40},//0x40 0x00
	{0xfe,0x00},
	{0xb6,0x03},//aec on
	{0x9a,0x06},  //0x07
	{0x99,0x11},
	{0x90 ,0x01},  //crop enable
	{0x95 ,0x02},
	{0x96 ,0xd0},
	{0x97 ,0x05},
	{0x98 ,0x00},   

#if 0
	{0xfe ,0x03},
	{0x42 ,0x80}, 
	{0x43 ,0x06}, //output buf width
	{0x41 ,0x00}, // delay
	{0x40 ,0x00}, //fifo half full trig
	{0x17 ,0x01}, //widv 
#endif
	{0xfe ,0x00},
	{0xc8,0x00},
	{0xfa,0x11},
	{0x00,0x00},
};

/* 800X600 SVGA*/
static struct reginfo sensor_svga[] =
{	
       ////////sabsumple  800X600//////
	{0xfe ,0x00},
	{0x0a,0x00}, //row start
	{0x0c,0x00}, //col start
	{0x0d,0x04},
	{0x0e,0xc0},
	{0x0f,0x06}, //Window setting
	{0x10,0x58}, 

	//measure window
	{0xfe,0x00},
	{0xb6,0x03},//aec on
	{0xfe,0x00},
	{0xec,0x06},//04 2012.10.26
	{0xed,0x06},//04 2012.10.26
	{0xee,0x62},//60 2012.10.26
	{0xef,0x92},//90 2012.10.26	
	{0x05 , 0x01},//hb
	{0x06 , 0x11},
	{0x07 , 0x00},//vb
	{0x08 , 0x50},
	{0xfe , 0x01},
	{0x27 , 0x00},//step
	{0x28 , 0xa0},
	{0x29 , 0x05},//level1
	{0x2a , 0x00},
	{0x2b , 0x05},//level2
	{0x2c , 0x00},
	{0x2d , 0x06},//6e8//level3
	{0x2e , 0xe0},
	{0x2f , 0x0a},//level4
	{0x30 , 0x00},
	//{0x3e , 0x40},
	{0xfe,0x00},	
	{0xb6,0x03},	
	{0xfa,0x00},
	{0xc8,0x00},
	{0x99,0x22},
	{0x9a,0x07},
	{0x9b,0x00},  
	{0x9c,0x00},
	{0x9d,0x00},
	{0x9e,0x00},
	{0x9f,0x00},  
	{0xa0,0x00},
	{0xa1,0x00},
	{0xa2,0x00},
	{0x90,0x01},
	{0x95,0x02},
	{0x96,0x58},  
	{0x97,0x03},
	{0x98,0x20},
	#if defined(CONFIG_CAMERA_EMI_ENABLE)
	{0xfa,0x11},
	#else
	{0xfa,0x00}, 
	#endif
	{0x00 , 0x00},

	/*************************/

};
	
	

/* 640X480 VGA */
static struct reginfo sensor_vga[] =
{
	

	{0x00 , 0x00},
};

/* 352X288 CIF */
static struct reginfo sensor_cif[] =
{
    {0x0, 0x0},
};

/* 320*240 QVGA */
static  struct reginfo sensor_qvga[] =
{
    

    {0x0, 0x0},
};

/* 176X144 QCIF*/
static struct reginfo sensor_qcif[] =
{
	
    {0x0, 0x0},
};

static  struct reginfo sensor_ClrFmt_YUYV[]=
{
   
         {0x0, 0x0},
};

static  struct reginfo sensor_ClrFmt_UYVY[]=
{
           {0x0, 0x0},
};

#if CONFIG_SENSOR_WhiteBalance
static  struct reginfo sensor_WhiteB_Auto[]=
{
                     {0xfe, 0x00},
			{0xb3, 0x61},
			{0xb4, 0x40},
			{0xb5, 0x61},
			{0x82, 0xfe},
	              {0x0, 0x0},
};
/* Cloudy Colour Temperature : 6500K - 8000K  */
static  struct reginfo sensor_WhiteB_Cloudy[]=
{
                      {0xfe, 0x00},
			{0x82, 0xfc},
			{0xb3, 0x58},
			{0xb4, 0x32},
			{0xb5, 0x42},
	              {0x0, 0x0},
};
/* ClearDay Colour Temperature : 5000K - 6500K  */
static  struct reginfo sensor_WhiteB_ClearDay[]=
{
    //Sunny
                     {0xfe, 0x00},
			{0x82, 0xfc},
			{0xb3, 0x50},
			{0xb4, 0x32},
			{0xb5, 0x42},
			{0x0, 0x0},
};
/* Office Colour Temperature : 3500K - 5000K  */
static  struct reginfo sensor_WhiteB_TungstenLamp1[]=
{
    //Office
         {0xfe, 0x00},
			{0x82, 0xfc},
			{0xb3, 0x40},
			{0xb4, 0x45},
			{0xb5, 0xa8},
			{0x0, 0x0},
			

};
/* Home Colour Temperature : 2500K - 3500K  */
static  struct reginfo sensor_WhiteB_TungstenLamp2[]=
{
    //Home
        {0xfe, 0x00},
			{0x82, 0xfc},
			{0xb3, 0x4a},
			{0xb4, 0x40},
			{0xb5, 0x88},
			{0x0, 0x0},
};
static struct reginfo *sensor_WhiteBalanceSeqe[] = {sensor_WhiteB_Auto, sensor_WhiteB_TungstenLamp1,sensor_WhiteB_TungstenLamp2,
    sensor_WhiteB_ClearDay, sensor_WhiteB_Cloudy,NULL,
};
#endif

#if CONFIG_SENSOR_Brightness
static  struct reginfo sensor_Brightness0[]=
{
    // Brightness -2
  
	{0xfe, 0x01},
	{0x13, 0x70},
	{0xfe, 0x02},
	{0xd5, 0xe0},
       {0x00, 0x00},
};

static  struct reginfo sensor_Brightness1[]=
{
    // Brightness -1
 
	{0xfe, 0x01},
	{0x13, 0x78},
	{0xfe, 0x02},
	{0xd5, 0xf0},
       {0x00, 0x00}
};

static  struct reginfo sensor_Brightness2[]=
{
    //  Brightness 0

	{0xfe, 0x01},
	{0x13, 0x80},
	{0xfe, 0x02},
	{0xd5, 0x00},
       {0x00, 0x00}
};

static  struct reginfo sensor_Brightness3[]=
{
    // Brightness +1
	{0xfe, 0x01},
	{0x13, 0x88},
	{0xfe, 0x02},
	{0xd5, 0x10},
       {0x00, 0x00}
};

static  struct reginfo sensor_Brightness4[]=
{
    //  Brightness +2
	{0xfe, 0x01},
	{0x13, 0x90},
	{0xfe, 0x02},
	{0xd5, 0x20},
       {0x00, 0x00}
};

static  struct reginfo sensor_Brightness5[]=
{
    //  Brightness +3
	{0xfe, 0x01},
	{0x13, 0x98},
	{0xfe, 0x02},
	{0xd5, 0x30},

	{0x00, 0x00}
};
static struct reginfo *sensor_BrightnessSeqe[] = {sensor_Brightness0, sensor_Brightness1, sensor_Brightness2, sensor_Brightness3,
    sensor_Brightness4, sensor_Brightness5,NULL,
};

#endif

#if CONFIG_SENSOR_Effect
static  struct reginfo sensor_Effect_Normal[] =
{
	{0xfe, 0x00},
	{0x83, 0xe0},
	{0x00, 0x00}		
};

static  struct reginfo sensor_Effect_WandB[] =
{
	{0xfe, 0x00},
	{0x83, 0x12},			
	{0x00, 0x00}
};

static  struct reginfo sensor_Effect_Sepia[] =
{
	{0xfe, 0x00},
	{0x83, 0x82},
	{0x00, 0x00}
};

static  struct reginfo sensor_Effect_Negative[] =
{
    //Negative
	{0xfe, 0x00},
	{0x83, 0x01},
	{0x00, 0x00}
};
static  struct reginfo sensor_Effect_Bluish[] =
{
    // Bluish
	{0xfe, 0x00},
	{0x83, 0x62},
	{0x00, 0x00}
};

static  struct reginfo sensor_Effect_Green[] =
{
    //  Greenish
	{0xfe, 0x00},
	{0x83, 0x52},
	{0x00, 0x00}
};
static struct reginfo *sensor_EffectSeqe[] = {sensor_Effect_Normal, sensor_Effect_WandB, sensor_Effect_Negative,sensor_Effect_Sepia,
    sensor_Effect_Bluish, sensor_Effect_Green,NULL,
};
#endif
#if CONFIG_SENSOR_Exposure
#if 0
static  struct reginfo sensor_Exposure0[]=
{
    //-3
    	{0xfe,0x01},
    	{0x13,0x40},
    	{0xfe,0x00},
       {0x00,0x00}
};

static  struct reginfo sensor_Exposure1[]=
{
    //-2
	{0xfe,0x01},
    	{0x13,0x50},
    	{0xfe,0x00},
       {0x00,0x00}
};

static  struct reginfo sensor_Exposure2[]=
{
    //-0.3EV
	{0xfe,0x01},
    	{0x13,0x60},
    	{0xfe,0x00},
       {0x00,0x00}
};

static  struct reginfo sensor_Exposure3[]=
{
    //default
  	{0xfe,0x01},
    	{0x13,0x70},
    	{0xfe,0x00},
       {0x00,0x00}
};

static  struct reginfo sensor_Exposure4[]=
{
    // 1
	{0xfe,0x01},
    	{0x13,0x78},
    	{0xfe,0x00},
       {0x00,0x00}};

static  struct reginfo sensor_Exposure5[]=
{
    // 2
 	{0xfe,0x01},
    	{0x13,0x88},
    	{0xfe,0x00},
       {0x00,0x00}
};

static  struct reginfo sensor_Exposure6[]=
{
    // 3
  	{0xfe,0x01},
    	{0x13,0x98},
    	{0xfe,0x00},
       {0x00,0x00}
};
#endif
static  struct reginfo sensor_Exposure0[]=
{
    //-3
    	{0xfe,0x01},
    	{0x13,0x60},
    	{0xfe,0x00},
       {0x00,0x00}
};

static  struct reginfo sensor_Exposure1[]=
{
    //-2
	{0xfe,0x01},
    	{0x13,0x70},
    	{0xfe,0x00},
       {0x00,0x00}
};

static  struct reginfo sensor_Exposure2[]=
{
    //-0.3EV
	{0xfe,0x01},
    	{0x13,0x78},
    	{0xfe,0x00},
       {0x00,0x00}
};

static  struct reginfo sensor_Exposure3[]=
{
    //default
  	{0xfe,0x01},
    	{0x13,0x80},
    	{0xfe,0x00},
       {0x00,0x00}
};

static  struct reginfo sensor_Exposure4[]=
{
    // 1
	{0xfe,0x01},
    	{0x13,0x88},
    	{0xfe,0x00},
       {0x00,0x00}};

static  struct reginfo sensor_Exposure5[]=
{
    // 2
 	{0xfe,0x01},
    	{0x13,0x90},
    	{0xfe,0x00},
       {0x00,0x00}
};

static  struct reginfo sensor_Exposure6[]=
{
    // 3
  	{0xfe,0x01},
    	{0x13,0x98},
    	{0xfe,0x00},
       {0x00,0x00}
};

static struct reginfo *sensor_ExposureSeqe[] = {sensor_Exposure0, sensor_Exposure1, sensor_Exposure2, sensor_Exposure3,
    sensor_Exposure4, sensor_Exposure5,sensor_Exposure6,NULL,
};
#endif
#if CONFIG_SENSOR_Saturation
static  struct reginfo sensor_Saturation0[]=
{

    {0x00, 0x00}
};

static  struct reginfo sensor_Saturation1[]=
{
    {0x00, 0x00}
};

static  struct reginfo sensor_Saturation2[]=
{
    {0x00, 0x00}
};
static struct reginfo *sensor_SaturationSeqe[] = {sensor_Saturation0, sensor_Saturation1, sensor_Saturation2, NULL,};

#endif
#if CONFIG_SENSOR_Contrast
static  struct reginfo sensor_Contrast0[]=
{
    //Contrast -3
    {0x00, 0x00}
};

static  struct reginfo sensor_Contrast1[]=
{
    //Contrast -2
    {0x00, 0x00}
};

static  struct reginfo sensor_Contrast2[]=
{
    // Contrast -1
    {0x00, 0x00}
};

static  struct reginfo sensor_Contrast3[]=
{
    //Contrast 0
    {0x00, 0x00}
};

static  struct reginfo sensor_Contrast4[]=
{
    //Contrast +1
    {0x00, 0x00}
};


static  struct reginfo sensor_Contrast5[]=
{
    //Contrast +2
    {0x00, 0x00}
};

static  struct reginfo sensor_Contrast6[]=
{
   
    //Contrast +3
    {0x00, 0x00}

};
static struct reginfo *sensor_ContrastSeqe[] = {sensor_Contrast0, sensor_Contrast1, sensor_Contrast2, sensor_Contrast3,
    sensor_Contrast4, sensor_Contrast5, sensor_Contrast6, NULL,
};

#endif
#if CONFIG_SENSOR_Mirror
static  struct reginfo sensor_MirrorOn[]=
{
    {0x17 , 0x14},
    {0x00 , 0x00}
};

static  struct reginfo sensor_MirrorOff[]=
{
    {0x17 , 0x15},
    {0x00 , 0x00}
};
static struct reginfo *sensor_MirrorSeqe[] = {sensor_MirrorOff, sensor_MirrorOn,NULL,};
#endif
#if CONFIG_SENSOR_Flip
static  struct reginfo sensor_FlipOn[]=
{
    {0x17 , 0x16},
    {0x00 , 0x00}
};

static  struct reginfo sensor_FlipOff[]=
{
    {0x17 , 0x17},
    {0x00 , 0x00}
};
static struct reginfo *sensor_FlipSeqe[] = {sensor_FlipOff, sensor_FlipOn,NULL,};

#endif
#if CONFIG_SENSOR_Scene
static  struct reginfo sensor_SceneAuto[] =
{
{0xfe,0x01},

{0x3e,0x40},  	
{0xfe,0x00},

{0x00, 0x00}
};

static  struct reginfo sensor_SceneNight[] =
{



{0xfe,0x01},

{0x3e,0x60},  	
{0xfe,0x00},
{0x00, 0x00}

};
static struct reginfo *sensor_SceneSeqe[] = {sensor_SceneAuto, sensor_SceneNight,NULL,};

#endif
#if CONFIG_SENSOR_DigitalZoom
static struct reginfo sensor_Zoom0[] =
{
    {0x0, 0x0},
};

static struct reginfo sensor_Zoom1[] =
{
     {0x0, 0x0},
};

static struct reginfo sensor_Zoom2[] =
{
    {0x0, 0x0},
};


static struct reginfo sensor_Zoom3[] =
{
    {0x0, 0x0},
};
static struct reginfo *sensor_ZoomSeqe[] = {sensor_Zoom0, sensor_Zoom1, sensor_Zoom2, sensor_Zoom3, NULL,};
#endif
static const struct v4l2_querymenu sensor_menus[] =
{
	#if CONFIG_SENSOR_WhiteBalance
    { .id = V4L2_CID_DO_WHITE_BALANCE,  .index = 0,  .name = "auto",  .reserved = 0, }, {  .id = V4L2_CID_DO_WHITE_BALANCE,  .index = 1, .name = "incandescent",  .reserved = 0,},
    { .id = V4L2_CID_DO_WHITE_BALANCE,  .index = 2,  .name = "fluorescent", .reserved = 0,}, {  .id = V4L2_CID_DO_WHITE_BALANCE, .index = 3,  .name = "daylight", .reserved = 0,},
    { .id = V4L2_CID_DO_WHITE_BALANCE,  .index = 4,  .name = "cloudy-daylight", .reserved = 0,},
    #endif

	#if CONFIG_SENSOR_Effect
    { .id = V4L2_CID_EFFECT,  .index = 0,  .name = "none",  .reserved = 0, }, {  .id = V4L2_CID_EFFECT,  .index = 1, .name = "mono",  .reserved = 0,},
    { .id = V4L2_CID_EFFECT,  .index = 2,  .name = "negative", .reserved = 0,}, {  .id = V4L2_CID_EFFECT, .index = 3,  .name = "sepia", .reserved = 0,},
    { .id = V4L2_CID_EFFECT,  .index = 4, .name = "posterize", .reserved = 0,} ,{ .id = V4L2_CID_EFFECT,  .index = 5,  .name = "aqua", .reserved = 0,},
    #endif

	#if CONFIG_SENSOR_Scene
    { .id = V4L2_CID_SCENE,  .index = 0, .name = "auto", .reserved = 0,} ,{ .id = V4L2_CID_SCENE,  .index = 1,  .name = "night", .reserved = 0,},
    #endif

	#if CONFIG_SENSOR_Flash
    { .id = V4L2_CID_FLASH,  .index = 0,  .name = "off",  .reserved = 0, }, {  .id = V4L2_CID_FLASH,  .index = 1, .name = "auto",  .reserved = 0,},
    { .id = V4L2_CID_FLASH,  .index = 2,  .name = "on", .reserved = 0,}, {  .id = V4L2_CID_FLASH, .index = 3,  .name = "torch", .reserved = 0,},
    // honghaishen_test hhs_1224
    { .id = V4L2_CID_OPEN_FLASH,  .index = 0,  .name = "off",  .reserved = 0, }, {  .id = V4L2_CID_OPEN_FLASH,  .index = 1, .name = "on",  .reserved = 0,},
    #endif
};

static  struct v4l2_queryctrl sensor_controls[] =
{
	#if CONFIG_SENSOR_WhiteBalance
    {
        .id		= V4L2_CID_DO_WHITE_BALANCE,
        .type		= V4L2_CTRL_TYPE_MENU,
        .name		= "White Balance Control",
        .minimum	= 0,
        .maximum	= 4,
        .step		= 1,
        .default_value = 0,
    },
    #endif

	#if CONFIG_SENSOR_Brightness
	{
        .id		= V4L2_CID_BRIGHTNESS,
        .type		= V4L2_CTRL_TYPE_INTEGER,
        .name		= "Brightness Control",
        .minimum	= -3,
        .maximum	= 2,
        .step		= 1,
        .default_value = 0,
    },
    #endif

	#if CONFIG_SENSOR_Effect
	{
        .id		= V4L2_CID_EFFECT,
        .type		= V4L2_CTRL_TYPE_MENU,
        .name		= "Effect Control",
        .minimum	= 0,
        .maximum	= 5,
        .step		= 1,
        .default_value = 0,
    },
	#endif

	#if CONFIG_SENSOR_Exposure
	{
        .id		= V4L2_CID_EXPOSURE,
        .type		= V4L2_CTRL_TYPE_INTEGER,
        .name		= "Exposure Control",
        .minimum	= 0,
        .maximum	= 6,
        .step		= 1,
        .default_value = 3,
    },
	#endif

	#if CONFIG_SENSOR_Saturation
	{
        .id		= V4L2_CID_SATURATION,
        .type		= V4L2_CTRL_TYPE_INTEGER,
        .name		= "Saturation Control",
        .minimum	= 0,
        .maximum	= 2,
        .step		= 1,
        .default_value = 0,
    },
    #endif

	#if CONFIG_SENSOR_Contrast
	{
        .id		= V4L2_CID_CONTRAST,
        .type		= V4L2_CTRL_TYPE_INTEGER,
        .name		= "Contrast Control",
        .minimum	= -3,
        .maximum	= 3,
        .step		= 1,
        .default_value = 0,
    },
	#endif

	#if CONFIG_SENSOR_Mirror
	{
        .id		= V4L2_CID_HFLIP,
        .type		= V4L2_CTRL_TYPE_BOOLEAN,
        .name		= "Mirror Control",
        .minimum	= 0,
        .maximum	= 1,
        .step		= 1,
        .default_value = 1,
    },
    #endif

	#if CONFIG_SENSOR_Flip
	{
        .id		= V4L2_CID_VFLIP,
        .type		= V4L2_CTRL_TYPE_BOOLEAN,
        .name		= "Flip Control",
        .minimum	= 0,
        .maximum	= 1,
        .step		= 1,
        .default_value = 1,
    },
    #endif

	#if CONFIG_SENSOR_Scene
    {
        .id		= V4L2_CID_SCENE,
        .type		= V4L2_CTRL_TYPE_MENU,
        .name		= "Scene Control",
        .minimum	= 0,
        .maximum	= 1,
        .step		= 1,
        .default_value = 0,
    },
    #endif

	#if CONFIG_SENSOR_DigitalZoom
    {
        .id		= V4L2_CID_ZOOM_RELATIVE,
        .type		= V4L2_CTRL_TYPE_INTEGER,
        .name		= "DigitalZoom Control",
        .minimum	= -1,
        .maximum	= 1,
        .step		= 1,
        .default_value = 0,
    }, {
        .id		= V4L2_CID_ZOOM_ABSOLUTE,
        .type		= V4L2_CTRL_TYPE_INTEGER,
        .name		= "DigitalZoom Control",
        .minimum	= 0,
        .maximum	= 3,
        .step		= 1,
        .default_value = 0,
    },
    #endif

	#if CONFIG_SENSOR_Focus
	{
        .id		= V4L2_CID_FOCUS_RELATIVE,
        .type		= V4L2_CTRL_TYPE_INTEGER,
        .name		= "Focus Control",
        .minimum	= -1,
        .maximum	= 1,
        .step		= 1,
        .default_value = 0,
    }, {
        .id		= V4L2_CID_FOCUS_ABSOLUTE,
        .type		= V4L2_CTRL_TYPE_INTEGER,
        .name		= "Focus Control",
        .minimum	= 0,
        .maximum	= 255,
        .step		= 1,
        .default_value = 125,
    },
    #endif

	#if CONFIG_SENSOR_Flash
	{
        .id		= V4L2_CID_FLASH,
        .type		= V4L2_CTRL_TYPE_MENU,
        .name		= "Flash Control",
        .minimum	= 0,
        .maximum	= 3,
        .step		= 1,
        .default_value = 0,
    },
    // honghaishen_test hhs_1224
   {
        .id		= V4L2_CID_OPEN_FLASH,
        .type		= V4L2_CTRL_TYPE_MENU,
        .name		= "Flash status",
        .minimum	= 0,
        .maximum	= 1,
        .step		= 1,
        .default_value = 0,
    },
	#endif
};

static int sensor_probe(struct i2c_client *client, const struct i2c_device_id *did);
static int sensor_video_probe(struct soc_camera_device *icd, struct i2c_client *client);
static int sensor_g_control(struct v4l2_subdev *sd, struct v4l2_control *ctrl);
static int sensor_s_control(struct v4l2_subdev *sd, struct v4l2_control *ctrl);
static int sensor_g_ext_controls(struct v4l2_subdev *sd,  struct v4l2_ext_controls *ext_ctrl);
static int sensor_s_ext_controls(struct v4l2_subdev *sd,  struct v4l2_ext_controls *ext_ctrl);
static int sensor_suspend(struct soc_camera_device *icd, pm_message_t pm_msg);
static int sensor_resume(struct soc_camera_device *icd);
static int sensor_set_bus_param(struct soc_camera_device *icd,unsigned long flags);
static unsigned long sensor_query_bus_param(struct soc_camera_device *icd);
static int sensor_set_effect(struct soc_camera_device *icd, const struct v4l2_queryctrl *qctrl, int value);
static int sensor_set_whiteBalance(struct soc_camera_device *icd, const struct v4l2_queryctrl *qctrl, int value);
static int sensor_deactivate(struct i2c_client *client);

static struct soc_camera_ops sensor_ops =
{
    .suspend                     = sensor_suspend,
    .resume                       = sensor_resume,
    .set_bus_param		= sensor_set_bus_param,
    .query_bus_param	= sensor_query_bus_param,
    .controls		= sensor_controls,
    .menus                         = sensor_menus,
    .num_controls		= ARRAY_SIZE(sensor_controls),
    .num_menus		= ARRAY_SIZE(sensor_menus),
};

/* only one fixed colorspace per pixelcode */
struct sensor_datafmt {
	enum v4l2_mbus_pixelcode code;
	enum v4l2_colorspace colorspace;
};

/* Find a data format by a pixel code in an array */
static const struct sensor_datafmt *sensor_find_datafmt(
	enum v4l2_mbus_pixelcode code, const struct sensor_datafmt *fmt,
	int n)
{
	int i;
	for (i = 0; i < n; i++)
		if (fmt[i].code == code)
			return fmt + i;

	return NULL;
}

static const struct sensor_datafmt sensor_colour_fmts[] = {
	{V4L2_MBUS_FMT_UYVY8_2X8, V4L2_COLORSPACE_JPEG},
    {V4L2_MBUS_FMT_YUYV8_2X8, V4L2_COLORSPACE_JPEG},	
    
    
};

typedef struct sensor_info_priv_s
{
    int whiteBalance;
    int brightness;
    int contrast;
    int saturation;
    int effect;
    int scene;
    int digitalzoom;
    int focus;
    int flash;
    int exposure;
	bool snap2preview;
	bool video2preview;
    unsigned char mirror;                                        /* HFLIP */
    unsigned char flip;                                          /* VFLIP */
    unsigned int winseqe_cur_addr;
    struct sensor_datafmt fmt;
    unsigned int funmodule_state;
} sensor_info_priv_t;

struct sensor
{
    struct v4l2_subdev subdev;
    struct i2c_client *client;
    sensor_info_priv_t info_priv;
    int model;	/* V4L2_IDENT_OV* codes from v4l2-chip-ident.h */
#if CONFIG_SENSOR_I2C_NOSCHED
	atomic_t tasklock_cnt;
#endif
	struct rk29camera_platform_data *sensor_io_request;
    struct rk29camera_gpio_res *sensor_gpio_res;
};

static struct sensor* to_sensor(const struct i2c_client *client)
{
    return container_of(i2c_get_clientdata(client), struct sensor, subdev);
}

static int sensor_task_lock(struct i2c_client *client, int lock)
{
#if CONFIG_SENSOR_I2C_NOSCHED
	int cnt = 3;
    struct sensor *sensor = to_sensor(client);

	if (lock) {
		if (atomic_read(&sensor->tasklock_cnt) == 0) {
			while ((atomic_read(&client->adapter->bus_lock.count) < 1) && (cnt>0)) {
				SENSOR_TR("\n %s will obtain i2c in atomic, but i2c bus is locked! Wait...\n",SENSOR_NAME_STRING());
				msleep(35);
				cnt--;
			}
			if ((atomic_read(&client->adapter->bus_lock.count) < 1) && (cnt<=0)) {
				SENSOR_TR("\n %s obtain i2c fail in atomic!!\n",SENSOR_NAME_STRING());
				goto sensor_task_lock_err;
			}
			preempt_disable();
		}

		atomic_add(1, &sensor->tasklock_cnt);
	} else {
		if (atomic_read(&sensor->tasklock_cnt) > 0) {
			atomic_sub(1, &sensor->tasklock_cnt);

			if (atomic_read(&sensor->tasklock_cnt) == 0)
				preempt_enable();
		}
	}
	return 0;
sensor_task_lock_err:
	return -1; 
#else
    return 0;
#endif

}

/* sensor register write */
static int sensor_write(struct i2c_client *client, u8 reg, u8 val)
{
    int err,cnt;
    u8 buf[2];
    struct i2c_msg msg[1];

    buf[0] = reg;
    buf[1] = val;


    msg->addr = client->addr;
    msg->flags = client->flags;
    msg->buf = buf;
    msg->len = sizeof(buf);
    msg->scl_rate = CONFIG_SENSOR_I2C_SPEED;         /* ddl@rock-chips.com : 100kHz */
    msg->read_type = 0;               /* fpga i2c:0==I2C_NORMAL : direct use number not enum for don't want include spi_fpga.h */

    cnt = 3;
    err = -EAGAIN;

    while ((cnt-- > 0) && (err < 0)) {                       /* ddl@rock-chips.com :  Transfer again if transent is failed   */
        err = i2c_transfer(client->adapter, msg, 1);

        if (err >= 0) {
            return 0;
        } else {
        	SENSOR_TR("\n %s write reg(0x%x, val:0x%x) failed, try to write again!\n",SENSOR_NAME_STRING(),reg, val);
            udelay(10);
        }
    }

    return err;
}

/* sensor register read */
static int sensor_read(struct i2c_client *client, u8 reg, u8 *val)
{
    int err,cnt;
    u8 buf[1];
    struct i2c_msg msg[2];

    buf[0] = reg ;

    msg[0].addr = client->addr;
    msg[0].flags = client->flags;
    msg[0].buf = buf;
    msg[0].len = sizeof(buf);
    msg[0].scl_rate = CONFIG_SENSOR_I2C_SPEED;       /* ddl@rock-chips.com : 100kHz */
    msg[0].read_type = 2;   /* fpga i2c:0==I2C_NO_STOP : direct use number not enum for don't want include spi_fpga.h */

    msg[1].addr = client->addr;
    msg[1].flags = client->flags|I2C_M_RD;
    msg[1].buf = buf;
    msg[1].len = 1;
    msg[1].scl_rate = CONFIG_SENSOR_I2C_SPEED;                       /* ddl@rock-chips.com : 100kHz */
    msg[1].read_type = 2;                             /* fpga i2c:0==I2C_NO_STOP : direct use number not enum for don't want include spi_fpga.h */

    cnt = 3;
    err = -EAGAIN;
    while ((cnt-- > 0) && (err < 0)) {                       /* ddl@rock-chips.com :  Transfer again if transent is failed   */
        err = i2c_transfer(client->adapter, msg, 2);

        if (err >= 0) {
            *val = buf[0];
            return 0;
        } else {
        	SENSOR_TR("\n %s read reg(0x%x val:0x%x) failed, try to read again! \n",SENSOR_NAME_STRING(),reg, *val);
            udelay(10);
        }
    }

    return err;
}

/* write a array of registers  */
static int sensor_write_array(struct i2c_client *client, struct reginfo *regarray)
{
    int err = 0, cnt;
    int i = 0;
#if CONFIG_SENSOR_I2C_RDWRCHK    
	char valchk;
#endif

	cnt = 0;
	if (sensor_task_lock(client, 1) < 0)
		goto sensor_write_array_end;

    while (regarray[i].reg != 0)
    {
        err = sensor_write(client, regarray[i].reg, regarray[i].val);
        if (err < 0)
        {
            if (cnt-- > 0) {
			    SENSOR_TR("%s..write failed current reg:0x%x, Write array again !\n", SENSOR_NAME_STRING(),regarray[i].reg);
				i = 0;
				continue;
            } else {
                SENSOR_TR("%s..write array failed!!!\n", SENSOR_NAME_STRING());
                err = -EPERM;
				goto sensor_write_array_end;
            }
        } else {
        #if CONFIG_SENSOR_I2C_RDWRCHK
			sensor_read(client, regarray[i].reg, &valchk);
			if (valchk != regarray[i].val)
				SENSOR_TR("%s Reg:0x%x write(0x%x, 0x%x) fail\n",SENSOR_NAME_STRING(), regarray[i].reg, regarray[i].val, valchk);
		#endif
        }
        i++;
    }

sensor_write_array_end:
	sensor_task_lock(client,0);
	return err;
}

#if CONFIG_SENSOR_I2C_RDWRCHK
static int sensor_check_array(struct i2c_client *client, struct reginfo *regarray)
{
    int cnt;
    int i = 0;
	char valchk;

	cnt = 0;
	valchk = 0;
    while (regarray[i].reg != 0)
    {
		sensor_read(client, regarray[i].reg, &valchk);
		if (valchk != regarray[i].val)
			SENSOR_TR("%s Reg:0x%x read(0x%x, 0x%x) error\n",SENSOR_NAME_STRING(), regarray[i].reg, regarray[i].val, valchk);

        i++;
    }
    return 0;
}
#endif
static int sensor_ioctrl(struct soc_camera_device *icd,enum rk29sensor_power_cmd cmd, int on)
{
	struct soc_camera_link *icl = to_soc_camera_link(icd);
	int ret = 0;

    SENSOR_DG("%s %s  cmd(%d) on(%d)\n",SENSOR_NAME_STRING(),__FUNCTION__,cmd,on);
	switch (cmd)
	{	
		case Sensor_PowerDown:
		{
			if (icl->powerdown) {
				ret = icl->powerdown(icd->pdev, on);
				if (ret == RK29_CAM_IO_SUCCESS) {
					if (on == 0) {
						mdelay(2);
						if (icl->reset)
							icl->reset(icd->pdev);
					}
				} else if (ret == RK29_CAM_EIO_REQUESTFAIL) {
					ret = -ENODEV;
					goto sensor_power_end;
				}
			}
 
			break;
			
		}
		case Sensor_Flash:
		{
			struct i2c_client *client = to_i2c_client(to_soc_camera_control(icd));
    		struct sensor *sensor = to_sensor(client);

			if (sensor->sensor_io_request && sensor->sensor_io_request->sensor_ioctrl) {
				sensor->sensor_io_request->sensor_ioctrl(icd->pdev,Cam_Flash, on);
			}
            break;
		}
		default:
		{
			SENSOR_TR("%s %s cmd(0x%x) is unknown!",SENSOR_NAME_STRING(),__FUNCTION__,cmd);
			break;
		}
	}
sensor_power_end:
	return ret;
}
static enum hrtimer_restart flash_off_func(struct hrtimer *timer){
	struct flash_timer *fps_timer = container_of(timer, struct flash_timer, timer);
    sensor_ioctrl(fps_timer->icd,Sensor_Flash,0);
	SENSOR_DG("%s %s !!!!!!",SENSOR_NAME_STRING(),__FUNCTION__);
    return 0;
    
}
static enum hrtimer_restart flash_on_func(struct hrtimer *timer){
	struct flash_timer *fps_timer = container_of(timer, struct flash_timer, timer);
	sensor_ioctrl(fps_timer->icd,Sensor_Flash,Flash_On);
	flash_off_timer.timer.function = flash_off_func;
	hrtimer_forward_now(&(flash_off_timer.timer), ktime_set(0, 700*1000*1300));
	return HRTIMER_RESTART;
}
static int sensor_init(struct v4l2_subdev *sd, u32 val)
{
    struct i2c_client *client = v4l2_get_subdevdata(sd);
    struct soc_camera_device *icd = client->dev.platform_data;
    struct sensor *sensor = to_sensor(client);
	const struct v4l2_queryctrl *qctrl;
    const struct sensor_datafmt *fmt;
    char value;
    int ret,pid = 0;

    SENSOR_DG("\n%s..%s.. \n",SENSOR_NAME_STRING(),__FUNCTION__);

	if (sensor_ioctrl(icd, Sensor_PowerDown, 0) < 0) {
		ret = -ENODEV;
		goto sensor_INIT_ERR;
	}

    /* soft reset */
	if (sensor_task_lock(client,1)<0)
		goto sensor_INIT_ERR;
	/* check if it is an sensor sensor */
    ret = sensor_read(client, 0xf0, &value);
    if (ret != 0) {
        SENSOR_TR("read chip id high byte failed\n");
        ret = -ENODEV;
        goto sensor_INIT_ERR;
    }

    pid |= (value << 8);

    ret = sensor_read(client, 0xf1, &value);
    if (ret != 0) {
        SENSOR_TR("read chip id low byte failed\n");
        ret = -ENODEV;
        goto sensor_INIT_ERR;
    }

    pid |= (value & 0xff);
    SENSOR_TR("\n %s() %s  pid = 0x%x\n",__func__, SENSOR_NAME_STRING(), pid);
    if (pid == SENSOR_ID) {
        sensor->model = SENSOR_V4L2_IDENT;
    } else {
        SENSOR_TR("error: %s mismatched   pid = 0x%x\n", SENSOR_NAME_STRING(), pid);
        ret = -ENODEV;
        goto sensor_INIT_ERR;
    }

    ret = sensor_write_array(client, sensor_init_data);


    mdelay(300);

	
    if (ret != 0)
    {
        SENSOR_TR("error: %s initial failed\n",SENSOR_NAME_STRING());
        goto sensor_INIT_ERR;
    }
	sensor_task_lock(client,0);
    
    sensor->info_priv.winseqe_cur_addr  = (int)SENSOR_INIT_WINSEQADR;
    fmt = sensor_find_datafmt(SENSOR_INIT_PIXFMT,sensor_colour_fmts, ARRAY_SIZE(sensor_colour_fmts));
    if (!fmt) {
        SENSOR_TR("error: %s initial array colour fmts is not support!!",SENSOR_NAME_STRING());
        ret = -EINVAL;
        goto sensor_INIT_ERR;
    }
	sensor->info_priv.fmt = *fmt;

    /* sensor sensor information for initialization  */
	qctrl = soc_camera_find_qctrl(&sensor_ops, V4L2_CID_DO_WHITE_BALANCE);
	if (qctrl)
    	sensor->info_priv.whiteBalance = qctrl->default_value;
	qctrl = soc_camera_find_qctrl(&sensor_ops, V4L2_CID_BRIGHTNESS);
	if (qctrl)
    	sensor->info_priv.brightness = qctrl->default_value;
	qctrl = soc_camera_find_qctrl(&sensor_ops, V4L2_CID_EFFECT);
	if (qctrl)
    	sensor->info_priv.effect = qctrl->default_value;
	qctrl = soc_camera_find_qctrl(&sensor_ops, V4L2_CID_EXPOSURE);
	if (qctrl)
        sensor->info_priv.exposure = qctrl->default_value;

	qctrl = soc_camera_find_qctrl(&sensor_ops, V4L2_CID_SATURATION);
	if (qctrl)
        sensor->info_priv.saturation = qctrl->default_value;
	qctrl = soc_camera_find_qctrl(&sensor_ops, V4L2_CID_CONTRAST);
	if (qctrl)
        sensor->info_priv.contrast = qctrl->default_value;
	qctrl = soc_camera_find_qctrl(&sensor_ops, V4L2_CID_HFLIP);
	if (qctrl)
        sensor->info_priv.mirror = qctrl->default_value;
	qctrl = soc_camera_find_qctrl(&sensor_ops, V4L2_CID_VFLIP);
	if (qctrl)
        sensor->info_priv.flip = qctrl->default_value;
	qctrl = soc_camera_find_qctrl(&sensor_ops, V4L2_CID_SCENE);
	if (qctrl)
        sensor->info_priv.scene = qctrl->default_value;
	qctrl = soc_camera_find_qctrl(&sensor_ops, V4L2_CID_ZOOM_ABSOLUTE);
	if (qctrl)
        sensor->info_priv.digitalzoom = qctrl->default_value;

    /* ddl@rock-chips.com : if sensor support auto focus and flash, programer must run focus and flash code  */
	#if CONFIG_SENSOR_Focus
    sensor_set_focus();
    qctrl = soc_camera_find_qctrl(&sensor_ops, V4L2_CID_FOCUS_ABSOLUTE);
	if (qctrl)
        sensor->info_priv.focus = qctrl->default_value;
	#endif

	#if CONFIG_SENSOR_Flash	
	qctrl = soc_camera_find_qctrl(&sensor_ops, V4L2_CID_FLASH);
	if (qctrl)
        sensor->info_priv.flash = qctrl->default_value;
    flash_off_timer.icd = icd;
	flash_off_timer.timer.function = flash_off_func;
    #endif

    SENSOR_DG("\n%s..%s.. icd->width = %d.. icd->height %d\n",SENSOR_NAME_STRING(),((val == 0)?__FUNCTION__:"sensor_reinit"),icd->user_width,icd->user_height);
    sensor->info_priv.funmodule_state |= SENSOR_INIT_IS_OK;
    return 0;
sensor_INIT_ERR:
    sensor->info_priv.funmodule_state &= ~SENSOR_INIT_IS_OK;
	sensor_task_lock(client,0);
	sensor_deactivate(client);
    return ret;
}

static int sensor_deactivate(struct i2c_client *client)
{
	struct soc_camera_device *icd = client->dev.platform_data;

    struct sensor *sensor = to_sensor(client);
	SENSOR_DG("\n%s..%s.. Enter\n",SENSOR_NAME_STRING(),__FUNCTION__);

	/* ddl@rock-chips.com : all sensor output pin must change to input for other sensor */
    if (sensor->info_priv.funmodule_state & SENSOR_INIT_IS_OK) {
    }
	sensor_ioctrl(icd, Sensor_PowerDown, 1);
	sensor_ioctrl(icd, Sensor_Flash, Flash_Off);
    msleep(100); 

	/* ddl@rock-chips.com : sensor config init width , because next open sensor quickly(soc_camera_open -> Try to configure with default parameters) */
	icd->user_width = SENSOR_INIT_WIDTH;
    icd->user_height = SENSOR_INIT_HEIGHT;
    sensor->info_priv.funmodule_state &= ~SENSOR_INIT_IS_OK;
	
	return 0;
}

static  struct reginfo sensor_power_down_sequence[]=
{

    {0x00,0x00}
};
static int sensor_suspend(struct soc_camera_device *icd, pm_message_t pm_msg)
{
    int ret;
    struct i2c_client *client = to_i2c_client(to_soc_camera_control(icd));

    if (pm_msg.event == PM_EVENT_SUSPEND) {
        SENSOR_DG("\n %s Enter Suspend.. \n", SENSOR_NAME_STRING());
        ret = sensor_write_array(client, sensor_power_down_sequence) ;
        if (ret != 0) {
            SENSOR_TR("\n %s..%s WriteReg Fail.. \n", SENSOR_NAME_STRING(),__FUNCTION__);
            return ret;
        } else {
            ret = sensor_ioctrl(icd, Sensor_PowerDown, 1);
            if (ret < 0) {
			    SENSOR_TR("\n %s suspend fail for turn on power!\n", SENSOR_NAME_STRING());
                return -EINVAL;
            }
        }
    } else {
        SENSOR_TR("\n %s cann't suppout Suspend..\n",SENSOR_NAME_STRING());
        return -EINVAL;
    }
    return 0;
}

static int sensor_resume(struct soc_camera_device *icd)
{
	int ret;

    ret = sensor_ioctrl(icd, Sensor_PowerDown, 0);
    if (ret < 0) {
		SENSOR_TR("\n %s resume fail for turn on power!\n", SENSOR_NAME_STRING());
        return -EINVAL;
    }

	SENSOR_DG("\n %s Enter Resume.. \n", SENSOR_NAME_STRING());

    return 0;

}

static int sensor_set_bus_param(struct soc_camera_device *icd,
                                unsigned long flags)
{

    return 0;
}

static unsigned long sensor_query_bus_param(struct soc_camera_device *icd)
{
    struct soc_camera_link *icl = to_soc_camera_link(icd);
    unsigned long flags = SENSOR_BUS_PARAM;

    return soc_camera_apply_sensor_flags(icl, flags);
}

static int sensor_g_fmt(struct v4l2_subdev *sd, struct v4l2_mbus_framefmt *mf)
{
    struct i2c_client *client = v4l2_get_subdevdata(sd);
    struct soc_camera_device *icd = client->dev.platform_data;
    struct sensor *sensor = to_sensor(client);

    mf->width	= icd->user_width;
	mf->height	= icd->user_height;
	mf->code	= sensor->info_priv.fmt.code;
	mf->colorspace	= sensor->info_priv.fmt.colorspace;
	mf->field	= V4L2_FIELD_NONE;

    return 0;
}
static bool sensor_fmt_capturechk(struct v4l2_subdev *sd, struct v4l2_mbus_framefmt *mf)
{
    bool ret = false;

	if ((mf->width == 1024) && (mf->height == 768)) {
		ret = true;
	} else if ((mf->width == 1280) && (mf->height == 1024)) {
		ret = true;
	} else if ((mf->width == 1600) && (mf->height == 1200)) {
		ret = true;
	} else if ((mf->width == 2048) && (mf->height == 1536)) {
		ret = true;
	} else if ((mf->width == 2592) && (mf->height == 1944)) {
		ret = true;
	}

	if (ret == true)
		SENSOR_DG("%s %dx%d is capture format\n", __FUNCTION__, mf->width, mf->height);
	return ret;
}

static bool sensor_fmt_videochk(struct v4l2_subdev *sd, struct v4l2_mbus_framefmt *mf)
{
    bool ret = false;

	if ((mf->width == 1280) && (mf->height == 720)) {
		ret = true;
	} else if ((mf->width == 1920) && (mf->height == 1080)) {
		ret = true;
	}

	if (ret == true)
		SENSOR_DG("%s %dx%d is video format\n", __FUNCTION__, mf->width, mf->height);
	return ret;
}
static  unsigned   int shutter_h,shutter_l;
static int sensor_s_fmt(struct v4l2_subdev *sd, struct v4l2_mbus_framefmt *mf)
{
    int ret1;

    struct i2c_client *client = v4l2_get_subdevdata(sd);
    const struct sensor_datafmt *fmt;
    struct sensor *sensor = to_sensor(client);
	const struct v4l2_queryctrl *qctrl;
	struct soc_camera_device *icd = client->dev.platform_data;
    struct reginfo *winseqe_set_addr=NULL;
    int ret=0, set_w,set_h;
	int hhs_h;
#if  1
char value;
unsigned   int pid=0,shutter,temp_reg;

#endif

	fmt = sensor_find_datafmt(mf->code, sensor_colour_fmts,
				   ARRAY_SIZE(sensor_colour_fmts));
	if (!fmt) {
        ret = -EINVAL;
        goto sensor_s_fmt_end;
    }

	if (sensor->info_priv.fmt.code != mf->code) {
		switch (mf->code)
		{
			case V4L2_MBUS_FMT_YUYV8_2X8:
			{
				winseqe_set_addr = sensor_ClrFmt_YUYV;
				break;
			}
			case V4L2_MBUS_FMT_UYVY8_2X8:
			{
				winseqe_set_addr = sensor_ClrFmt_UYVY;
				break;
			}
			default:
				break;
		}
		if (winseqe_set_addr != NULL) {
            sensor_write_array(client, winseqe_set_addr);
			sensor->info_priv.fmt.code = mf->code;
            sensor->info_priv.fmt.colorspace= mf->colorspace;            
			SENSOR_DG("%s v4l2_mbus_code:%d set success!\n", SENSOR_NAME_STRING(),mf->code);
		} else {
			SENSOR_TR("%s v4l2_mbus_code:%d is invalidate!\n", SENSOR_NAME_STRING(),mf->code);
		}
	}

    set_w = mf->width;
    set_h = mf->height;
	hhs_h = mf->height;
	
	if (((set_w <= 176) && (set_h <= 144)) && sensor_qcif[0].reg)
	{
		winseqe_set_addr = sensor_qcif;
        set_w = 176;
        set_h = 144;
	}
	else if (((set_w <= 320) && (set_h <= 240)) && sensor_qvga[0].reg)
    {
        winseqe_set_addr = sensor_qvga;
        set_w = 320;
        set_h = 240;
    }
    else if (((set_w <= 352) && (set_h<= 288)) && sensor_cif[0].reg)
    {
        winseqe_set_addr = sensor_cif;
        set_w = 352;
        set_h = 288;
    }
    else if (((set_w <= 640) && (set_h <= 480)) && sensor_vga[0].reg)
    {
        winseqe_set_addr = sensor_vga;
        set_w = 640;
        set_h = 480;
    }
    else if (((set_w <= 800) && (set_h <= 600)) && sensor_svga[0].reg)
    {
        winseqe_set_addr = sensor_svga;
        set_w = 800;
        set_h = 600;
    }
     else if (((set_w <= 1280) && (set_h <= 720)) && sensor_720p[0].reg)
    {
        winseqe_set_addr = sensor_720p;
        set_w = 1280;
        set_h = 720;
    }	
    else if (((set_w <= 1280) && (set_h <= 1024)) && sensor_sxga[0].reg)
    {
        winseqe_set_addr = sensor_sxga;
        set_w = 1280;
        set_h = 1024;
    }
    else if (((set_w <= 1600) && (set_h <= 1200)) && sensor_uxga[0].reg)
    {
        winseqe_set_addr = sensor_uxga;
        set_w = 1600;
        set_h = 1200;
    }
    else
    {
        winseqe_set_addr = SENSOR_INIT_WINSEQADR;               /* ddl@rock-chips.com : Sensor output smallest size if  isn't support app  */
        set_w = SENSOR_INIT_WIDTH;
        set_h = SENSOR_INIT_HEIGHT;
		SENSOR_TR("\n %s..%s Format is Invalidate. pix->width = %d.. pix->height = %d\n",SENSOR_NAME_STRING(),__FUNCTION__,mf->width,mf->height);
    }

    if ((int)winseqe_set_addr  != sensor->info_priv.winseqe_cur_addr) {
        #if CONFIG_SENSOR_Flash
        if (sensor_fmt_capturechk(sd,mf) == true) {      /* ddl@rock-chips.com : Capture */
            if ((sensor->info_priv.flash == 1) || (sensor->info_priv.flash == 2)) {
		// honghaishen_test hhs_1224
               // sensor_ioctrl(icd, Sensor_Flash, Flash_On);  
                SENSOR_DG("%s flash on in capture!\n", SENSOR_NAME_STRING());
            }           
        } else {                                        /* ddl@rock-chips.com : Video */
            if ((sensor->info_priv.flash == 1) || (sensor->info_priv.flash == 2)) {
		// honghaishen_test hhs_1224
             //   sensor_ioctrl(icd, Sensor_Flash, Flash_Off);   
                SENSOR_DG("%s flash off in preivew!\n", SENSOR_NAME_STRING());
            }
        }
        #endif
	if ((winseqe_set_addr == sensor_svga)||(winseqe_set_addr == sensor_vga) )
	{
	sensor_write(client, 0xb6, 0x00);  // AEC ON
	sensor_write(client, 0x03, shutter_h);
	sensor_write(client, 0x04, shutter_l);
     //  msleep(50);
	printk("set preview for rewrite 0x03");
	
	} 
	/*[A]fixed the issue add 720p recording,xmhhs,20130222*/
	if(winseqe_set_addr == sensor_720p ||sensor->info_priv.winseqe_cur_addr == sensor_720p)
		sensor_write_array(client,sensor_init_data);
	/*[A]END xmhhs,20130222,Bug-Id<RK-69>*/
        ret |= sensor_write_array(client, winseqe_set_addr);
	/*[A]fixed the issue add 720p recording,xmhhs,20130222*/
		if(winseqe_set_addr == sensor_720p)
		msleep(500);
	/*[A]END xmhhs,20130222,Bug-Id<RK-69>*/
	#if 1
		if (winseqe_set_addr == sensor_uxga) { 


		        	sensor_write(client, 0xfe, 0x00);
				sensor_write(client, 0xb6, 0x02);  // AEC OFF
				sensor_read(client, 0x03, &value);
				shutter_h=value;
				pid |= (value << 8);
			       sensor_read(client, 0x04, &value);
				 shutter_l=value;
				pid |= (value & 0xff);
				shutter=pid;
				temp_reg= shutter /2;	// 2
				if(temp_reg < 1) temp_reg = 1;
				sensor_write(client, 0x03, ((temp_reg>>8)&0xff));
			       sensor_write(client, 0x04, (temp_reg&0xff));
        }
#endif   

        if (ret != 0) {
            SENSOR_TR("%s set format capability failed\n", SENSOR_NAME_STRING());
            #if CONFIG_SENSOR_Flash
            if (sensor_fmt_capturechk(sd,mf) == true) {
                if ((sensor->info_priv.flash == 1) || (sensor->info_priv.flash == 2)) {
			// honghaishen_test hhs_1224
                 //   sensor_ioctrl(icd, Sensor_Flash, Flash_Off);   hhs
                    SENSOR_TR("%s Capture format set fail, flash off !\n", SENSOR_NAME_STRING());
                }
            }
            #endif
            goto sensor_s_fmt_end;
        }

        sensor->info_priv.winseqe_cur_addr  = (int)winseqe_set_addr;

		if (sensor_fmt_capturechk(sd,mf) == true) {				    /* ddl@rock-chips.com : Capture */
			//qctrl = soc_camera_find_qctrl(&sensor_ops, V4L2_CID_EFFECT);
			//sensor_set_effect(icd, qctrl,sensor->info_priv.effect);
			if (sensor->info_priv.whiteBalance != 0) {
				qctrl = soc_camera_find_qctrl(&sensor_ops, V4L2_CID_DO_WHITE_BALANCE);
				sensor_set_whiteBalance(icd, qctrl,sensor->info_priv.whiteBalance);
			}
			sensor->info_priv.snap2preview = true;
		} else if (sensor_fmt_videochk(sd,mf) == true) {			/* ddl@rock-chips.com : Video */
			//qctrl = soc_camera_find_qctrl(&sensor_ops, V4L2_CID_EFFECT);
			//sensor_set_effect(icd, qctrl,sensor->info_priv.effect);
			qctrl = soc_camera_find_qctrl(&sensor_ops, V4L2_CID_DO_WHITE_BALANCE);
			sensor_set_whiteBalance(icd, qctrl,sensor->info_priv.whiteBalance);
			sensor->info_priv.video2preview = true;
		} else if ((sensor->info_priv.snap2preview == true) || (sensor->info_priv.video2preview == true)) {
			//qctrl = soc_camera_find_qctrl(&sensor_ops, V4L2_CID_EFFECT);
			//sensor_set_effect(icd, qctrl,sensor->info_priv.effect);
			qctrl = soc_camera_find_qctrl(&sensor_ops, V4L2_CID_DO_WHITE_BALANCE);
			sensor_set_whiteBalance(icd, qctrl,sensor->info_priv.whiteBalance);
            //msleep(600);
			sensor->info_priv.video2preview = false;
			sensor->info_priv.snap2preview = false;
		}

        SENSOR_DG("\n%s..%s.. icd->width = %d.. icd->height %d\n",SENSOR_NAME_STRING(),__FUNCTION__,set_w,set_h);
    }
    else
    {
        SENSOR_DG("\n %s .. Current Format is validate. icd->width = %d.. icd->height %d\n",SENSOR_NAME_STRING(),set_w,set_h);
    }
	if(flash_on_off == 1)
	{
		sensor_ioctrl(icd, Sensor_Flash, Flash_Off);
		flash_on_off =0 ;
		flash_off_timer.timer.function = flash_on_func;
		hrtimer_cancel(&(flash_off_timer.timer));
       	hrtimer_start(&(flash_off_timer.timer),ktime_set(0, 250*1000*1200),HRTIMER_MODE_REL);
	}
	mf->width = set_w;
    mf->height = set_h;
	sensor_write(client, 0xfe, 0x00);
	if(hhs_h == 720)
	{
           sensor_write(client, 0xb6, 0x03);
	}
 //    Sensor_CropSet(mf,20);  if the fov is not pass , we can change the value;
	//msleep(100);
sensor_s_fmt_end:
    return ret;
}

static int sensor_try_fmt(struct v4l2_subdev *sd, struct v4l2_mbus_framefmt *mf)
{
    struct i2c_client *client = v4l2_get_subdevdata(sd);
    struct sensor *sensor = to_sensor(client);
    const struct sensor_datafmt *fmt;
    int ret = 0,set_w,set_h;
   
	fmt = sensor_find_datafmt(mf->code, sensor_colour_fmts,
				   ARRAY_SIZE(sensor_colour_fmts));
	if (fmt == NULL) {
		fmt = &sensor->info_priv.fmt;
        mf->code = fmt->code;
	} 

    if (mf->height > SENSOR_MAX_HEIGHT)
        mf->height = SENSOR_MAX_HEIGHT;
    else if (mf->height < SENSOR_MIN_HEIGHT)
        mf->height = SENSOR_MIN_HEIGHT;

    if (mf->width > SENSOR_MAX_WIDTH)
        mf->width = SENSOR_MAX_WIDTH;
    else if (mf->width < SENSOR_MIN_WIDTH)
        mf->width = SENSOR_MIN_WIDTH;

    set_w = mf->width;
    set_h = mf->height;

	if (((set_w <= 176) && (set_h <= 144)) && sensor_qcif[0].reg)
	{
        set_w = 176;
        set_h = 144;
	}
	else if (((set_w <= 320) && (set_h <= 240)) && sensor_qvga[0].reg)
    {
        set_w = 320;
        set_h = 240;
    }
    else if (((set_w <= 352) && (set_h<= 288)) && sensor_cif[0].reg)
    {
        set_w = 352;
        set_h = 288;
    }
    else if (((set_w <= 640) && (set_h <= 480)) && sensor_vga[0].reg)
    {
        set_w = 640;
        set_h = 480;
    }
    else if (((set_w <= 800) && (set_h <= 600)) && sensor_svga[0].reg)
    {
        set_w = 800;
        set_h = 600;
    }
     else if (((set_w <= 1280) && (set_h <= 720)) && sensor_720p[0].reg)
    {
        set_w = 1280;
        set_h = 720;
    }	
    else if (((set_w <= 1280) && (set_h <= 1024)) && sensor_sxga[0].reg)
    {
        set_w = 1280;
        set_h = 1024;
    }
    else if (((set_w <= 1600) && (set_h <= 1200)) && sensor_uxga[0].reg)
    {
        set_w = 1600;
        set_h = 1200;
    }
    else
    {
        set_w = SENSOR_INIT_WIDTH;
        set_h = SENSOR_INIT_HEIGHT;
    }

	mf->width = set_w;
    mf->height = set_h;
    mf->colorspace = fmt->colorspace;
    
    return ret;
}

 static int sensor_g_chip_ident(struct v4l2_subdev *sd, struct v4l2_dbg_chip_ident *id)
{
    struct i2c_client *client = v4l2_get_subdevdata(sd);

    if (id->match.type != V4L2_CHIP_MATCH_I2C_ADDR)
        return -EINVAL;

    if (id->match.addr != client->addr)
        return -ENODEV;

    id->ident = SENSOR_V4L2_IDENT;      /* ddl@rock-chips.com :  Return gc2035  identifier */
    id->revision = 0;

    return 0;
}
#if CONFIG_SENSOR_Brightness
static int sensor_set_brightness(struct soc_camera_device *icd, const struct v4l2_queryctrl *qctrl, int value)
{
    struct i2c_client *client = to_i2c_client(to_soc_camera_control(icd));

    if ((value >= qctrl->minimum) && (value <= qctrl->maximum))
    {
        if (sensor_BrightnessSeqe[value - qctrl->minimum] != NULL)
        {
            if (sensor_write_array(client, sensor_BrightnessSeqe[value - qctrl->minimum]) != 0)
            {
                SENSOR_TR("%s..%s WriteReg Fail.. \n",SENSOR_NAME_STRING(), __FUNCTION__);
                return -EINVAL;
            }
            SENSOR_DG("%s..%s : %x\n",SENSOR_NAME_STRING(),__FUNCTION__, value);
            return 0;
        }
    }
	SENSOR_TR("\n %s..%s valure = %d is invalidate..    \n",SENSOR_NAME_STRING(),__FUNCTION__,value);
    return -EINVAL;
}
#endif
#if CONFIG_SENSOR_Effect
static int sensor_set_effect(struct soc_camera_device *icd, const struct v4l2_queryctrl *qctrl, int value)
{
    struct i2c_client *client = to_i2c_client(to_soc_camera_control(icd));

    if ((value >= qctrl->minimum) && (value <= qctrl->maximum))
    {
        if (sensor_EffectSeqe[value - qctrl->minimum] != NULL)
        {
            if (sensor_write_array(client, sensor_EffectSeqe[value - qctrl->minimum]) != 0)
            {
                SENSOR_TR("%s..%s WriteReg Fail.. \n",SENSOR_NAME_STRING(), __FUNCTION__);
                return -EINVAL;
            }
            SENSOR_DG("%s..%s : %x\n",SENSOR_NAME_STRING(),__FUNCTION__, value);
            return 0;
        }
    }
	SENSOR_TR("\n %s..%s valure = %d is invalidate..    \n",SENSOR_NAME_STRING(),__FUNCTION__,value);
    return -EINVAL;
}
#endif
#if CONFIG_SENSOR_Exposure
static int sensor_set_exposure(struct soc_camera_device *icd, const struct v4l2_queryctrl *qctrl, int value)
{
    struct i2c_client *client = to_i2c_client(to_soc_camera_control(icd));

    if ((value >= qctrl->minimum) && (value <= qctrl->maximum))
    {
        if (sensor_ExposureSeqe[value - qctrl->minimum] != NULL)
        {
            if (sensor_write_array(client, sensor_ExposureSeqe[value - qctrl->minimum]) != 0)
            {
                SENSOR_TR("%s..%s WriteReg Fail.. \n",SENSOR_NAME_STRING(), __FUNCTION__);
                return -EINVAL;
            }
            SENSOR_DG("%s..%s : %x\n",SENSOR_NAME_STRING(),__FUNCTION__, value);
            return 0;
        }
    }
	SENSOR_TR("\n %s..%s valure = %d is invalidate..    \n",SENSOR_NAME_STRING(),__FUNCTION__,value);
    return -EINVAL;
}
#endif
#if CONFIG_SENSOR_Saturation
static int sensor_set_saturation(struct soc_camera_device *icd, const struct v4l2_queryctrl *qctrl, int value)
{
    struct i2c_client *client = to_i2c_client(to_soc_camera_control(icd));

    if ((value >= qctrl->minimum) && (value <= qctrl->maximum))
    {
        if (sensor_SaturationSeqe[value - qctrl->minimum] != NULL)
        {
            if (sensor_write_array(client, sensor_SaturationSeqe[value - qctrl->minimum]) != 0)
            {
                SENSOR_TR("%s..%s WriteReg Fail.. \n",SENSOR_NAME_STRING(), __FUNCTION__);
                return -EINVAL;
            }
            SENSOR_DG("%s..%s : %x\n",SENSOR_NAME_STRING(),__FUNCTION__, value);
            return 0;
        }
    }
    SENSOR_TR("\n %s..%s valure = %d is invalidate..    \n",SENSOR_NAME_STRING(),__FUNCTION__,value);
    return -EINVAL;
}
#endif
#if CONFIG_SENSOR_Contrast
static int sensor_set_contrast(struct soc_camera_device *icd, const struct v4l2_queryctrl *qctrl, int value)
{
    struct i2c_client *client = to_i2c_client(to_soc_camera_control(icd));

    if ((value >= qctrl->minimum) && (value <= qctrl->maximum))
    {
        if (sensor_ContrastSeqe[value - qctrl->minimum] != NULL)
        {
            if (sensor_write_array(client, sensor_ContrastSeqe[value - qctrl->minimum]) != 0)
            {
                SENSOR_TR("%s..%s WriteReg Fail.. \n",SENSOR_NAME_STRING(), __FUNCTION__);
                return -EINVAL;
            }
            SENSOR_DG("%s..%s : %x\n",SENSOR_NAME_STRING(),__FUNCTION__, value);
            return 0;
        }
    }
    SENSOR_TR("\n %s..%s valure = %d is invalidate..    \n",SENSOR_NAME_STRING(),__FUNCTION__,value);
    return -EINVAL;
}
#endif
#if CONFIG_SENSOR_Mirror
static int sensor_set_mirror(struct soc_camera_device *icd, const struct v4l2_queryctrl *qctrl, int value)
{
    struct i2c_client *client = to_i2c_client(to_soc_camera_control(icd));

    if ((value >= qctrl->minimum) && (value <= qctrl->maximum))
    {
        if (sensor_MirrorSeqe[value - qctrl->minimum] != NULL)
        {
            if (sensor_write_array(client, sensor_MirrorSeqe[value - qctrl->minimum]) != 0)
            {
                SENSOR_TR("%s..%s WriteReg Fail.. \n",SENSOR_NAME_STRING(), __FUNCTION__);
                return -EINVAL;
            }
            SENSOR_DG("%s..%s : %x\n",SENSOR_NAME_STRING(),__FUNCTION__, value);
            return 0;
        }
    }
    SENSOR_TR("\n %s..%s valure = %d is invalidate..    \n",SENSOR_NAME_STRING(),__FUNCTION__,value);
    return -EINVAL;
}
#endif
#if CONFIG_SENSOR_Flip
static int sensor_set_flip(struct soc_camera_device *icd, const struct v4l2_queryctrl *qctrl, int value)
{
    struct i2c_client *client = to_i2c_client(to_soc_camera_control(icd));

    if ((value >= qctrl->minimum) && (value <= qctrl->maximum))
    {
        if (sensor_FlipSeqe[value - qctrl->minimum] != NULL)
        {
            if (sensor_write_array(client, sensor_FlipSeqe[value - qctrl->minimum]) != 0)
            {
                SENSOR_TR("%s..%s WriteReg Fail.. \n",SENSOR_NAME_STRING(), __FUNCTION__);
                return -EINVAL;
            }
            SENSOR_DG("%s..%s : %x\n",SENSOR_NAME_STRING(),__FUNCTION__, value);
            return 0;
        }
    }
    SENSOR_TR("\n %s..%s valure = %d is invalidate..    \n",SENSOR_NAME_STRING(),__FUNCTION__,value);
    return -EINVAL;
}
#endif
#if CONFIG_SENSOR_Scene
static int sensor_set_scene(struct soc_camera_device *icd, const struct v4l2_queryctrl *qctrl, int value)
{
    struct i2c_client *client = to_i2c_client(to_soc_camera_control(icd));

    if ((value >= qctrl->minimum) && (value <= qctrl->maximum))
    {
        if (sensor_SceneSeqe[value - qctrl->minimum] != NULL)
        {
            if (sensor_write_array(client, sensor_SceneSeqe[value - qctrl->minimum]) != 0)
            {
                SENSOR_TR("%s..%s WriteReg Fail.. \n",SENSOR_NAME_STRING(), __FUNCTION__);
                return -EINVAL;
            }
            SENSOR_DG("%s..%s : %x\n",SENSOR_NAME_STRING(),__FUNCTION__, value);
            return 0;
        }
    }
    SENSOR_TR("\n %s..%s valure = %d is invalidate..    \n",SENSOR_NAME_STRING(),__FUNCTION__,value);
    return -EINVAL;
}
#endif
#if CONFIG_SENSOR_WhiteBalance
static int sensor_set_whiteBalance(struct soc_camera_device *icd, const struct v4l2_queryctrl *qctrl, int value)
{
    struct i2c_client *client = to_i2c_client(to_soc_camera_control(icd));

    if ((value >= qctrl->minimum) && (value <= qctrl->maximum))
    {
        if (sensor_WhiteBalanceSeqe[value - qctrl->minimum] != NULL)
        {
            if (sensor_write_array(client, sensor_WhiteBalanceSeqe[value - qctrl->minimum]) != 0)
            {
                SENSOR_TR("%s..%s WriteReg Fail.. \n",SENSOR_NAME_STRING(), __FUNCTION__);
                return -EINVAL;
            }
            SENSOR_DG("%s..%s : %x\n",SENSOR_NAME_STRING(),__FUNCTION__, value);
            return 0;
        }
    }
	SENSOR_TR("\n %s..%s valure = %d is invalidate..    \n",SENSOR_NAME_STRING(),__FUNCTION__,value);
    return -EINVAL;
}
#endif
#if CONFIG_SENSOR_DigitalZoom
static int sensor_set_digitalzoom(struct soc_camera_device *icd, const struct v4l2_queryctrl *qctrl, int *value)
{
    struct i2c_client *client = to_i2c_client(to_soc_camera_control(icd));
    struct sensor *sensor = to_sensor(client);
	const struct v4l2_queryctrl *qctrl_info;
    int digitalzoom_cur, digitalzoom_total;

	qctrl_info = soc_camera_find_qctrl(&sensor_ops, V4L2_CID_ZOOM_ABSOLUTE);
	if (qctrl_info)
		return -EINVAL;

    digitalzoom_cur = sensor->info_priv.digitalzoom;
    digitalzoom_total = qctrl_info->maximum;

    if ((value > 0) && (digitalzoom_cur >= digitalzoom_total))
    {
        SENSOR_TR("%s digitalzoom is maximum - %x\n", SENSOR_NAME_STRING(), digitalzoom_cur);
        return -EINVAL;
    }

    if  ((value < 0) && (digitalzoom_cur <= qctrl_info->minimum))
    {
        SENSOR_TR("%s digitalzoom is minimum - %x\n", SENSOR_NAME_STRING(), digitalzoom_cur);
        return -EINVAL;
    }

    if ((value > 0) && ((digitalzoom_cur + value) > digitalzoom_total))
    {
        value = digitalzoom_total - digitalzoom_cur;
    }

    if ((value < 0) && ((digitalzoom_cur + value) < 0))
    {
        value = 0 - digitalzoom_cur;
    }

    digitalzoom_cur += value;

    if (sensor_ZoomSeqe[digitalzoom_cur] != NULL)
    {
        if (sensor_write_array(client, sensor_ZoomSeqe[digitalzoom_cur]) != 0)
        {
            SENSOR_TR("%s..%s WriteReg Fail.. \n",SENSOR_NAME_STRING(), __FUNCTION__);
            return -EINVAL;
        }
        SENSOR_DG("%s..%s : %x\n",SENSOR_NAME_STRING(),__FUNCTION__, value);
        return 0;
    }

    return -EINVAL;
}
#endif
#if CONFIG_SENSOR_Flash
// honghaishen_test hhs_1224
static int sensor_set_flash(struct soc_camera_device *icd, const struct v4l2_queryctrl *qctrl, int value)
{    
    if ((value >= qctrl->minimum) && (value <= qctrl->maximum)) {
        if (value == 3) 
	{       /* ddl@rock-chips.com: torch */
            sensor_ioctrl(icd, Sensor_Flash, Flash_Torch_On);   /* Flash On */
        }
	else if(value ==2)
	{
        	sensor_ioctrl(icd, Sensor_Flash, Flash_Torch_On);
		flash_on_off = 1;
	}
	else if(value ==0)
	{
            sensor_ioctrl(icd, Sensor_Flash, Flash_Off);
        }
        SENSOR_DG("%s..%s : %x\n",SENSOR_NAME_STRING(),__FUNCTION__, value);
        return 0;
    }
    
	SENSOR_TR("\n %s..%s valure = %d is invalidate..    \n",SENSOR_NAME_STRING(),__FUNCTION__,value);
    return -EINVAL;
}

#if 0
static int sensor_set_flash(struct soc_camera_device *icd, const struct v4l2_queryctrl *qctrl, int value)
{    
    if ((value >= qctrl->minimum) && (value <= qctrl->maximum)) {
        if (value == 3) {       /* ddl@rock-chips.com: torch */
            sensor_ioctrl(icd, Sensor_Flash, Flash_Torch);   /* Flash On */
        } else {
            sensor_ioctrl(icd, Sensor_Flash, Flash_Off);
        }
        SENSOR_DG("%s..%s : %x\n",SENSOR_NAME_STRING(),__FUNCTION__, value);
        return 0;
    }
    
	SENSOR_TR("\n %s..%s valure = %d is invalidate..    \n",SENSOR_NAME_STRING(),__FUNCTION__,value);
    return -EINVAL;
}
#endif
#endif

static int sensor_g_control(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
    struct i2c_client *client = v4l2_get_subdevdata(sd);
    struct sensor *sensor = to_sensor(client);
    const struct v4l2_queryctrl *qctrl;

    qctrl = soc_camera_find_qctrl(&sensor_ops, ctrl->id);

    if (!qctrl)
    {
        SENSOR_TR("\n %s ioctrl id = %d  is invalidate \n", SENSOR_NAME_STRING(), ctrl->id);
        return -EINVAL;
    }

    switch (ctrl->id)
    {
        case V4L2_CID_BRIGHTNESS:
            {
                ctrl->value = sensor->info_priv.brightness;
                break;
            }
        case V4L2_CID_SATURATION:
            {
                ctrl->value = sensor->info_priv.saturation;
                break;
            }
        case V4L2_CID_CONTRAST:
            {
                ctrl->value = sensor->info_priv.contrast;
                break;
            }
        case V4L2_CID_DO_WHITE_BALANCE:
            {
                ctrl->value = sensor->info_priv.whiteBalance;
                break;
            }
        case V4L2_CID_EXPOSURE:
            {
                ctrl->value = sensor->info_priv.exposure;
                break;
            }
        case V4L2_CID_HFLIP:
            {
                ctrl->value = sensor->info_priv.mirror;
                break;
            }
        case V4L2_CID_VFLIP:
            {
                ctrl->value = sensor->info_priv.flip;
                break;
            }
        default :
                break;
    }
    return 0;
}



static int sensor_s_control(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
    struct i2c_client *client = v4l2_get_subdevdata(sd);
    struct sensor *sensor = to_sensor(client);
    struct soc_camera_device *icd = client->dev.platform_data;
    const struct v4l2_queryctrl *qctrl;


    qctrl = soc_camera_find_qctrl(&sensor_ops, ctrl->id);

    if (!qctrl)
    {
        SENSOR_TR("\n %s ioctrl id = %d  is invalidate \n", SENSOR_NAME_STRING(), ctrl->id);
        return -EINVAL;
    }

    switch (ctrl->id)
    {
#if CONFIG_SENSOR_Brightness
        case V4L2_CID_BRIGHTNESS:
            {
                if (ctrl->value != sensor->info_priv.brightness)
                {
                    if (sensor_set_brightness(icd, qctrl,ctrl->value) != 0)
                    {
                        return -EINVAL;
                    }
                    sensor->info_priv.brightness = ctrl->value;
                }
                break;
            }
#endif
#if CONFIG_SENSOR_Exposure
        case V4L2_CID_EXPOSURE:
            {
                if (ctrl->value != sensor->info_priv.exposure)
                {
                    if (sensor_set_exposure(icd, qctrl,ctrl->value) != 0)
                    {
                        return -EINVAL;
                    }
                    sensor->info_priv.exposure = ctrl->value;
                }
                break;
            }
#endif
#if CONFIG_SENSOR_Saturation
        case V4L2_CID_SATURATION:
            {
                if (ctrl->value != sensor->info_priv.saturation)
                {
                    if (sensor_set_saturation(icd, qctrl,ctrl->value) != 0)
                    {
                        return -EINVAL;
                    }
                    sensor->info_priv.saturation = ctrl->value;
                }
                break;
            }
#endif
#if CONFIG_SENSOR_Contrast
        case V4L2_CID_CONTRAST:
            {
                if (ctrl->value != sensor->info_priv.contrast)
                {
                    if (sensor_set_contrast(icd, qctrl,ctrl->value) != 0)
                    {
                        return -EINVAL;
                    }
                    sensor->info_priv.contrast = ctrl->value;
                }
                break;
            }
#endif
#if CONFIG_SENSOR_WhiteBalance
        case V4L2_CID_DO_WHITE_BALANCE:
            {
                if (ctrl->value != sensor->info_priv.whiteBalance)
                {
                    if (sensor_set_whiteBalance(icd, qctrl,ctrl->value) != 0)
                    {
                        return -EINVAL;
                    }
                    sensor->info_priv.whiteBalance = ctrl->value;
                }
                break;
            }
#endif
#if CONFIG_SENSOR_Mirror
        case V4L2_CID_HFLIP:
            {
                if (ctrl->value != sensor->info_priv.mirror)
                {
                    if (sensor_set_mirror(icd, qctrl,ctrl->value) != 0)
                        return -EINVAL;
                    sensor->info_priv.mirror = ctrl->value;
                }
                break;
            }
#endif
#if CONFIG_SENSOR_Flip
        case V4L2_CID_VFLIP:
            {
                if (ctrl->value != sensor->info_priv.flip)
                {
                    if (sensor_set_flip(icd, qctrl,ctrl->value) != 0)
                        return -EINVAL;
                    sensor->info_priv.flip = ctrl->value;
                }
                break;
            }
#endif
        default:
            break;
    }

    return 0;
}
static int sensor_g_ext_control(struct soc_camera_device *icd , struct v4l2_ext_control *ext_ctrl)
{
    const struct v4l2_queryctrl *qctrl;
	char y_value=0;
    struct i2c_client *client = to_i2c_client(to_soc_camera_control(icd));
    struct sensor *sensor = to_sensor(client);

    qctrl = soc_camera_find_qctrl(&sensor_ops, ext_ctrl->id);

    if (!qctrl)
    {
        SENSOR_TR("\n %s ioctrl id = %d  is invalidate \n", SENSOR_NAME_STRING(), ext_ctrl->id);
        return -EINVAL;
    }

    switch (ext_ctrl->id)
    {
        case V4L2_CID_SCENE:
            {
                ext_ctrl->value = sensor->info_priv.scene;
                break;
            }
        case V4L2_CID_EFFECT:
            {
                ext_ctrl->value = sensor->info_priv.effect;
                break;
            }
        case V4L2_CID_ZOOM_ABSOLUTE:
            {
                ext_ctrl->value = sensor->info_priv.digitalzoom;
                break;
            }
        case V4L2_CID_ZOOM_RELATIVE:
            {
                return -EINVAL;
            }
        case V4L2_CID_FOCUS_ABSOLUTE:
            {
                ext_ctrl->value = sensor->info_priv.focus;
                break;
            }
        case V4L2_CID_FOCUS_RELATIVE:
            {
                return -EINVAL;
            }
        case V4L2_CID_FLASH:
            {
                ext_ctrl->value = sensor->info_priv.flash;
                break;
            }
		// honghaishen_test hhs_1224 start
	case V4L2_CID_OPEN_FLASH:
		{
	         sensor_write(client, 0xfe, 0x01);
		 sensor_read(client, 0x14, &y_value);
		 sensor_write(client, 0xfe, 0x00);
		 if(y_value > 0x40)
		 	ext_ctrl ->value = 0;
		 else 
		 	ext_ctrl->value =1;	
		}
	// honghaishen_test hhs_1224 end
        default :
            break;
    }
    return 0;
}
static int sensor_s_ext_control(struct soc_camera_device *icd, struct v4l2_ext_control *ext_ctrl)
{
    const struct v4l2_queryctrl *qctrl;
    struct i2c_client *client = to_i2c_client(to_soc_camera_control(icd));
    struct sensor *sensor = to_sensor(client);
    int val_offset;

    qctrl = soc_camera_find_qctrl(&sensor_ops, ext_ctrl->id);

    if (!qctrl)
    {
        SENSOR_TR("\n %s ioctrl id = %d  is invalidate \n", SENSOR_NAME_STRING(), ext_ctrl->id);
        return -EINVAL;
    }

	val_offset = 0;
    switch (ext_ctrl->id)
    {
#if CONFIG_SENSOR_Scene
        case V4L2_CID_SCENE:
            {
                if (ext_ctrl->value != sensor->info_priv.scene)
                {
                    if (sensor_set_scene(icd, qctrl,ext_ctrl->value) != 0)
                        return -EINVAL;
                    sensor->info_priv.scene = ext_ctrl->value;
                }
                break;
            }
#endif
#if CONFIG_SENSOR_Effect
        case V4L2_CID_EFFECT:
            {
                if (ext_ctrl->value != sensor->info_priv.effect)
                {
                    if (sensor_set_effect(icd, qctrl,ext_ctrl->value) != 0)
                        return -EINVAL;
                    sensor->info_priv.effect= ext_ctrl->value;
                }
                break;
            }
#endif
#if CONFIG_SENSOR_DigitalZoom
        case V4L2_CID_ZOOM_ABSOLUTE:
            {
                if ((ext_ctrl->value < qctrl->minimum) || (ext_ctrl->value > qctrl->maximum))
                    return -EINVAL;

                if (ext_ctrl->value != sensor->info_priv.digitalzoom)
                {
                    val_offset = ext_ctrl->value -sensor->info_priv.digitalzoom;

                    if (sensor_set_digitalzoom(icd, qctrl,&val_offset) != 0)
                        return -EINVAL;
                    sensor->info_priv.digitalzoom += val_offset;

                    SENSOR_DG("%s digitalzoom is %x\n",SENSOR_NAME_STRING(),  sensor->info_priv.digitalzoom);
                }

                break;
            }
        case V4L2_CID_ZOOM_RELATIVE:
            {
                if (ext_ctrl->value)
                {
                    if (sensor_set_digitalzoom(icd, qctrl,&ext_ctrl->value) != 0)
                        return -EINVAL;
                    sensor->info_priv.digitalzoom += ext_ctrl->value;

                    SENSOR_DG("%s digitalzoom is %x\n", SENSOR_NAME_STRING(), sensor->info_priv.digitalzoom);
                }
                break;
            }
#endif
#if CONFIG_SENSOR_Focus
        case V4L2_CID_FOCUS_ABSOLUTE:
            {
                if ((ext_ctrl->value < qctrl->minimum) || (ext_ctrl->value > qctrl->maximum))
                    return -EINVAL;

                if (ext_ctrl->value != sensor->info_priv.focus)
                {
                    val_offset = ext_ctrl->value -sensor->info_priv.focus;

                    sensor->info_priv.focus += val_offset;
                }

                break;
            }
        case V4L2_CID_FOCUS_RELATIVE:
            {
                if (ext_ctrl->value)
                {
                    sensor->info_priv.focus += ext_ctrl->value;

                    SENSOR_DG("%s focus is %x\n", SENSOR_NAME_STRING(), sensor->info_priv.focus);
                }
                break;
            }
#endif
#if CONFIG_SENSOR_Flash
        case V4L2_CID_FLASH:
            {
                if (sensor_set_flash(icd, qctrl,ext_ctrl->value) != 0)
                    return -EINVAL;
                sensor->info_priv.flash = ext_ctrl->value;

                SENSOR_DG("%s flash is %x\n",SENSOR_NAME_STRING(), sensor->info_priv.flash);
                break;
            }
#endif
        default:
            break;
    }

    return 0;
}

static int sensor_g_ext_controls(struct v4l2_subdev *sd, struct v4l2_ext_controls *ext_ctrl)
{
    struct i2c_client *client = v4l2_get_subdevdata(sd);
    struct soc_camera_device *icd = client->dev.platform_data;
    int i, error_cnt=0, error_idx=-1;


    for (i=0; i<ext_ctrl->count; i++) {
        if (sensor_g_ext_control(icd, &ext_ctrl->controls[i]) != 0) {
            error_cnt++;
            error_idx = i;
        }
    }

    if (error_cnt > 1)
        error_idx = ext_ctrl->count;

    if (error_idx != -1) {
        ext_ctrl->error_idx = error_idx;
        return -EINVAL;
    } else {
        return 0;
    }
}

static int sensor_s_ext_controls(struct v4l2_subdev *sd, struct v4l2_ext_controls *ext_ctrl)
{
    struct i2c_client *client = v4l2_get_subdevdata(sd);
    struct soc_camera_device *icd = client->dev.platform_data;
    int i, error_cnt=0, error_idx=-1;


    for (i=0; i<ext_ctrl->count; i++) {
        if (sensor_s_ext_control(icd, &ext_ctrl->controls[i]) != 0) {
            error_cnt++;
            error_idx = i;
        }
    }

    if (error_cnt > 1)
        error_idx = ext_ctrl->count;

    if (error_idx != -1) {
        ext_ctrl->error_idx = error_idx;
        return -EINVAL;
    } else {
        return 0;
    }
}

/* Interface active, can use i2c. If it fails, it can indeed mean, that
 * this wasn't our capture interface, so, we wait for the right one */
static int sensor_video_probe(struct soc_camera_device *icd,
			       struct i2c_client *client)
{
    char value;
    int ret,pid = 0;
    struct sensor *sensor = to_sensor(client);

    /* We must have a parent by now. And it cannot be a wrong one.
     * So this entire test is completely redundant. */
    if (!icd->dev.parent ||
	    to_soc_camera_host(icd->dev.parent)->nr != icd->iface)
		return -ENODEV;

	if (sensor_ioctrl(icd, Sensor_PowerDown, 0) < 0) {
		ret = -ENODEV;
		goto sensor_video_probe_err;
	}

    /* soft reset */

    /* check if it is an sensor sensor */
	    // honghaishen_test
    ret = sensor_read(client, 0xf0, &value);
    if (ret != 0) {
        SENSOR_TR("read chip id high byte failed\n");
        ret = -ENODEV;
        goto sensor_video_probe_err;
    }

    pid |= (value << 8);

    ret = sensor_read(client, 0xf1, &value);
    if (ret != 0) {
        SENSOR_TR("read chip id low byte failed\n");
        ret = -ENODEV;
        goto sensor_video_probe_err;
    }

    pid |= (value & 0xff);
    SENSOR_TR("\n %s() %s  pid = 0x%x\n",__func__, SENSOR_NAME_STRING(), pid);
    if (pid == SENSOR_ID) {
        sensor->model = SENSOR_V4L2_IDENT;
    } else {
        SENSOR_TR("error: %s mismatched   pid = 0x%x\n", SENSOR_NAME_STRING(), pid);
        ret = -ENODEV;
        goto sensor_video_probe_err;
    }

sensor->model = SENSOR_V4L2_IDENT; 
    return 0;

sensor_video_probe_err:

    return ret;
}
static long sensor_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
    struct soc_camera_device *icd = client->dev.platform_data;
    struct sensor *sensor = to_sensor(client);
//#if CONFIG_SENSOR_Flash
    int i;
//#endif
    int ret = 0;
    
	SENSOR_DG("\n%s..%s..cmd:%x \n",SENSOR_NAME_STRING(),__FUNCTION__,cmd);
	switch (cmd)
	{
		case RK29_CAM_SUBDEV_DEACTIVATE:
		{
			sensor_deactivate(client);
			break;
		}

		case RK29_CAM_SUBDEV_IOREQUEST:
		{
			sensor->sensor_io_request = (struct rk29camera_platform_data*)arg;           
            if (sensor->sensor_io_request != NULL) { 
                sensor->sensor_gpio_res = NULL;
                for (i=0; i<RK29_CAM_SUPPORT_NUMS;i++) {
                    if (sensor->sensor_io_request->gpio_res[i].dev_name && 
                        (strcmp(sensor->sensor_io_request->gpio_res[i].dev_name, dev_name(icd->pdev)) == 0)) {
                        sensor->sensor_gpio_res = (struct rk29camera_gpio_res*)&sensor->sensor_io_request->gpio_res[i];
                    }
                }
                if (sensor->sensor_gpio_res == NULL) {
                    SENSOR_TR("%s %s obtain gpio resource failed when RK29_CAM_SUBDEV_IOREQUEST \n",SENSOR_NAME_STRING(),__FUNCTION__);
                    ret = -EINVAL;
                    goto sensor_ioctl_end;
                }
            } else {
                SENSOR_TR("%s %s RK29_CAM_SUBDEV_IOREQUEST fail\n",SENSOR_NAME_STRING(),__FUNCTION__);
                ret = -EINVAL;
                goto sensor_ioctl_end;
            }
            /* ddl@rock-chips.com : if gpio_flash havn't been set in board-xxx.c, sensor driver must notify is not support flash control 
               for this project */
            #if CONFIG_SENSOR_Flash	
        	if (sensor->sensor_gpio_res) {
                if (sensor->sensor_gpio_res->gpio_flash == INVALID_GPIO) {
                    for (i = 0; i < icd->ops->num_controls; i++) {
                		if (V4L2_CID_FLASH == icd->ops->controls[i].id) {
                			//memset((char*)&icd->ops->controls[i],0x00,sizeof(struct v4l2_queryctrl));  
                              sensor_controls[i].id=0xffff;         			
                		}
                    }
                    sensor->info_priv.flash = 0xff;
                    SENSOR_DG("%s flash gpio is invalidate!\n",SENSOR_NAME_STRING());
                }else{ //two cameras are the same,need to deal diffrently ,zyc
                    for (i = 0; i < icd->ops->num_controls; i++) {
                           if(0xffff == icd->ops->controls[i].id){
                              sensor_controls[i].id=V4L2_CID_FLASH;
                           }               
                    }
                }
        	}
            #endif
			break;
		}
		default:
		{
			SENSOR_TR("%s %s cmd(0x%x) is unknown !\n",SENSOR_NAME_STRING(),__FUNCTION__,cmd);
			break;
		}
	}
sensor_ioctl_end:
	return ret;

}
static int sensor_enum_fmt(struct v4l2_subdev *sd, unsigned int index,
			    enum v4l2_mbus_pixelcode *code)
{
	if (index >= ARRAY_SIZE(sensor_colour_fmts))
		return -EINVAL;

	*code = sensor_colour_fmts[index].code;
	return 0;
}
static int sensor_s_stream(struct v4l2_subdev *sd, int enable)    // honghaishen_test hhs_0111
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct soc_camera_device *icd = client->dev.platform_data;
}
static struct v4l2_subdev_core_ops sensor_subdev_core_ops = {
	.init		= sensor_init,
	.g_ctrl		= sensor_g_control,
	.s_ctrl		= sensor_s_control,
	.g_ext_ctrls          = sensor_g_ext_controls,
	.s_ext_ctrls          = sensor_s_ext_controls,
	.g_chip_ident	= sensor_g_chip_ident,
	.ioctl = sensor_ioctl,
};

static struct v4l2_subdev_video_ops sensor_subdev_video_ops = {
	.s_mbus_fmt	= sensor_s_fmt,
	.g_mbus_fmt	= sensor_g_fmt,
	.try_mbus_fmt	= sensor_try_fmt,
	.enum_mbus_fmt	= sensor_enum_fmt,
	.s_stream = sensor_s_stream,
};

static struct v4l2_subdev_ops sensor_subdev_ops = {
	.core	= &sensor_subdev_core_ops,
	.video = &sensor_subdev_video_ops,
};

#if lily_debug
int ov5642_write_proc (struct file *file, const char *buffer,
                      unsigned long count, void *data)
{
	char kbuf[14]={0,0,0,0,0,0,0,0,0,0,0,0,0,0};
		char vol[10]={0,0,0,0,0,0,0,0,0,0};
		char v[4]={0,0,0,0};
	unsigned long totime;
	unsigned short vol2=0;
	char offset = 0;
	int32_t rc = 0;
	unsigned short vol1=0;
	//u16 reg = 0;
	char  val = 0;
	struct clk *clk = NULL;
	char value;

	if (count >= 14)
		return -EINVAL;
	if (copy_from_user(kbuf, buffer, count))
		return -EFAULT;
	
	if ('w' == kbuf[0]) {
		memcpy(vol, kbuf+1, 2);
		offset = (char) simple_strtoul(vol, NULL, 16);
		memcpy(v,kbuf+3,2);
		value = (u8) simple_strtoul(v,NULL,16);
		sensor_write(g_client, offset, value);
		printk("honghaishen_test %x %x \n ",offset ,value);
	
	}
	else if('r'==kbuf[0])
		{
		memcpy(vol, kbuf+1, 2);
		offset = (char) simple_strtoul(vol, NULL, 16);
		sensor_read(g_client,offset,&val);
		printk("honghaishen_test read val is %x \n",val);
		}
	else if('i'==kbuf[0])
		{
		 sensor_write_array(g_client, sensor_init_data);
		}
	
	return count;
}
int ov5642_read_proc (char *buffer, char **buffer_location, off_t offset,
                            int buffer_length, int *zero, void *ptr)
{

	if(offset > 0)
		return 0;
	return offset;
}
static void create_ov5642_proc_file(void)
{
	struct proc_dir_entry *ov5642_proc_file =
		create_proc_entry("ov5642", 0666, NULL);

	if (ov5642_proc_file) {
		ov5642_proc_file->read_proc = ov5642_read_proc;
		ov5642_proc_file->write_proc = ov5642_write_proc;
	} else
		printk(KERN_NOTICE "lily:(%d)%s:proc file create failed! \n",__LINE__,__func__);
}
#endif

static int sensor_probe(struct i2c_client *client,
			 const struct i2c_device_id *did)
{
    struct sensor *sensor;
    struct soc_camera_device *icd = client->dev.platform_data;
    struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
    struct soc_camera_link *icl;
    int ret;
#if lily_debug
   g_client = client;
#endif

    SENSOR_DG("\n%s..%s..%d..\n",__FUNCTION__,__FILE__,__LINE__);
    if (!icd) {
        dev_err(&client->dev, "%s: missing soc-camera data!\n",SENSOR_NAME_STRING());
        return -EINVAL;
    }
	#if lily_debug
	create_ov5642_proc_file();
	#endif

    icl = to_soc_camera_link(icd);
    if (!icl) {
        dev_err(&client->dev, "%s driver needs platform data\n", SENSOR_NAME_STRING());
        return -EINVAL;
    }

    if (!i2c_check_functionality(adapter, I2C_FUNC_I2C)) {
        dev_warn(&adapter->dev,
        	 "I2C-Adapter doesn't support I2C_FUNC_I2C\n");
        return -EIO;
    }

    sensor = kzalloc(sizeof(struct sensor), GFP_KERNEL);
    if (!sensor)
        return -ENOMEM;

    v4l2_i2c_subdev_init(&sensor->subdev, client, &sensor_subdev_ops);

    /* Second stage probe - when a capture adapter is there */
    icd->ops		= &sensor_ops;

    sensor->info_priv.fmt = sensor_colour_fmts[0];
    
	#if CONFIG_SENSOR_I2C_NOSCHED
	atomic_set(&sensor->tasklock_cnt,0);
	#endif

    ret = sensor_video_probe(icd, client);
    if (ret < 0) {
        icd->ops = NULL;
        i2c_set_clientdata(client, NULL);
        kfree(sensor);
		sensor = NULL;
    }
	hrtimer_init(&(flash_off_timer.timer), CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    SENSOR_DG("\n%s..%s..%d  ret = %x \n",__FUNCTION__,__FILE__,__LINE__,ret);
    return ret;
}

static int sensor_remove(struct i2c_client *client)
{
    struct sensor *sensor = to_sensor(client);
    struct soc_camera_device *icd = client->dev.platform_data;

    icd->ops = NULL;
    i2c_set_clientdata(client, NULL);
    client->driver = NULL;
    kfree(sensor);

    return 0;
}

static const struct i2c_device_id sensor_id[] = {
	{SENSOR_NAME_STRING(), 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sensor_id);

static struct i2c_driver sensor_i2c_driver = {
	.driver = {
		.name = SENSOR_NAME_STRING(),
	},
	.probe		= sensor_probe,
	.remove		= sensor_remove,
	.id_table	= sensor_id,
};

static int __init sensor_mod_init(void)
{
    SENSOR_DG("\n%s..%s.. \n",__FUNCTION__,SENSOR_NAME_STRING());
    return i2c_add_driver(&sensor_i2c_driver);
}

static void __exit sensor_mod_exit(void)
{
    i2c_del_driver(&sensor_i2c_driver);
}

device_initcall_sync(sensor_mod_init);
module_exit(sensor_mod_exit);

MODULE_DESCRIPTION(SENSOR_NAME_STRING(Camera sensor driver));
MODULE_AUTHOR("ddl <kernel@rock-chips>");
MODULE_LICENSE("GPL");



