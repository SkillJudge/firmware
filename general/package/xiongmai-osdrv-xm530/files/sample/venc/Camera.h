//
//  "$Id: Camera.h 4 2011-09-14 14:13:30Z xiaoyongli $"
//
//  Copyright (c)2008-2008, ZheJiang JuFeng Technology Stock CO.LTD.
//  All Rights Reserved.
//
//	Description:	
//	Revisions:		Year-Month-Day  SVN-Author  Modification
//
#ifndef __CAMERA_H__
#define __CAMERA_H__ 

#ifdef __cplusplus
extern "C" {
#endif
#include "xm_type.h"
#include "xm_comm_video.h"
#ifdef SOC_NONE
#include "mpi_vencTx.h"
#endif

#define GET_REJECT_SHAKE_TIME(cfgTime)  ((cfgTime)*20)
#define BYTE		unsigned char
#define WORD	unsigned short

//��AHD_NextChipһ��
// 3MP 13x; 4MP 14x; 5M 15x
typedef enum {
	SENSOR_CHIP_UNKNOW	=	0,
	SENSOR_CHIP_H42		=	92,
	SENSOR_CHIP_AR0140	=	94,
	SENSOR_CHIP_AR0130	=	95,
	SENSOR_CHIP_SC1035	=	96,	
	SENSOR_CHIP_SC1037	=	97,
	SENSOR_CHIP_H81		=	98,
	SENSOR_CHIP_H65		=	99,
	
	SENSOR_CHIP_SP140A	=	81,
	SENSOR_CHIP_H62		=	82,
	SENSOR_CHIP_BG0703	=	83,
	SENSOR_CHIP_SC1145	=	84,
	SENSOR_CHIP_SC1135	=	85,
	SENSOR_CHIP_OV9732	=	86,
	SENSOR_CHIP_OV9750	=	87,
	SENSOR_CHIP_SP1409	=	88,
	SENSOR_CHIP_MIS1002	=	89,
	
	SENSOR_CHIP_SC2045	=	21,
	SENSOR_CHIP_IMX222	=	22,
	SENSOR_CHIP_IMX322	=	23,
	SENSOR_CHIP_AR0237_IR	=	24,
	SENSOR_CHIP_SC2035	=	25,
	SENSOR_CHIP_F02		=	26,
	SENSOR_CHIP_AR0237	=	27,
	SENSOR_CHIP_IMX323	=	28,
	SENSOR_CHIP_PS5220	=	29,
	
	SENSOR_CHIP_SC2135	=	71,
	SENSOR_CHIP_F22 	=	72,
	SENSOR_CHIP_BG0803 	=	73,
	SENSOR_CHIP_PS5230 	=	74,
	SENSOR_CHIP_PS3210 	=	75,
	SENSOR_CHIP_GC2023 	=	76,
	SENSOR_CHIP_SC2145	=	77,
	SENSOR_CHIP_HNX083	=	78,
	SENSOR_CHIP_HNX993	=	79,
	
	SENSOR_CHIP_SC1235	=	50,
	SENSOR_CHIP_BF3016	=	51,
	SENSOR_CHIP_IMX307 	=	52,
	SENSOR_CHIP_SC2235E	=	53,	
	
	SENSOR_CHIP_BG0806 	=	60,
	SENSOR_CHIP_IMX291 	=	61,
	SENSOR_CHIP_PS5250	=	62,
	SENSOR_CHIP_SC2235	=	63,
	SENSOR_CHIP_SC2145H	=	64,
	SENSOR_CHIP_GC2033 	=	65,
	SENSOR_CHIP_F28 	=	66,
	SENSOR_CHIP_SC2235P =	67,
	SENSOR_CHIP_MIS2236 =	68,
	SENSOR_CHIP_SC2315 	=	69,
	
	SENSOR_CHIP_AR0330 	=	130,
	SENSOR_CHIP_SC3035 	=	131,
	SENSOR_CHIP_AUGE 	=	132,
	SENSOR_CHIP_OV4689 	=	140,
	SENSOR_CHIP_SC4236 	=	141,
	SENSOR_CHIP_K02 	=	142,
	SENSOR_CHIP_PS5510 	=	150,
	SENSOR_CHIP_K03 	=	151,
	SENSOR_CHIP_SC5035 	=	152,
	SENSOR_CHIP_SC5235 	=	153,
	SENSOR_CHIP_SC5300 	=	154,
	SENSOR_CHIP_IMX335 	=	155,
	SENSOR_CHIP_SC5239 	=	156,

	SENSOR_CHIP_OV12895 	=	180,
	SENSOR_CHIP_APOLLO 	=	181,
}XM_SENSOR_CHIP;


enum dvr_info_cmd_hs
{
	PRODUCT_TYPE 		= 0,
	VIDEO_CHANNEL 		= 1,
	AUDIO_CHANNEL 		= 2,
	ALARM_IN 			= 3,
	ALARM_OUT			= 4,
	FVIDEO_CHIP			= 5,
	DSP_CHIP 			= 6,
	ANALOG_AUDIO_MODE	= 7,
	TALKBACK			= 8,
	BVIDEO_CHIP			= 9,
	STORE_INTERFACE 	= 10,
	MATRIX		 		= 11,
	WIRELESS_INTERFACE	= 12,
	HD_ENCODE			= 13,
	HD_VERSION 			= 14,
	VD_INTERFACE        = 15,
	NET_INTERFACE       = 16,
	HD_INFO_LEN			= 17
};


enum netmode{
	NET_LAN = 0,
	NET_WLAN_8188EU
};

typedef enum _resolution_bit_{
	RSLTION_BIT_720P = 0,
	RSLTION_BIT_960P,
	RSLTION_BIT_1080P,
	RSLTION_BIT_1536P,
	RSLTION_BIT_4MP,
	RSLTION_BIT_5MP,
	RSLTION_BIT_8MP,

	RSLTION_BIT_BUTT
}RSLTION_BIT;


typedef enum{
	IPC_UNKNOWN = 0x2000,
	IPC_50X10 = 0x2001, 																					  
	IPC_53X13 = 0x2002,
	IPC_RA50X10 = 0x2003,
	IPC_50X10_XYD = 0x2004,
	IPC_50X10_SW = 0x2005,		
	IPC_53X13_SWI = 0x2006,		//IPC_53X13_SWI	�ɵ�
	IPC_RA50X10_C = 0x2007,
	IPC_RA53X13 = 0x2008,
	IPC_53X13_SW = 0x2009,		
	IPC_53X13_SWL = 0x200A,	// IPC_53X13_SWL	�׹�Ʒɵ�
	IPC_RA53X13_C = 0x200B,
	IPC_50X10_SWC = 0x200C,
	IPC_50X20_SWL = 0x200E, 
	IPC_50X20_SWI = 0x200F, 
	IPC_53X13_XYD = 0x2010,
	IPC_RA50X20_C = 0x2011,	
	IPC_50X30_SWL = 0x2012,
	IPC_50X10_SWCL = 0x2013,
	IPC_53X13_SWCL = 0x2014,
	IPC_50X20_SWCL = 0x2018,
	IPC_50X30_SWI = 0x2015,
	IPC_RA50X20 = 0x2016,
	IPC_50X20_SWC = 0x2017,
	IPC_50X10_SW_S = 0x2019,//32M
	IPC_53X13_SWL_S = 0x201A,
	IPC_50X10_SWC_S = 0x201B,
	IPC_RA50X10_C_S = 0x201C,
	IPC_53X13_SWI_S = 0x201D,
	IPC_XM530_RA50X20 = 0x3001,
	IPC_XM530_80X20 = 0x3002,
	IPC_XM530_80X50 = 0x3005,
	XM350AI_60X20	= 0x6000,	// ��ʱ����
	NR_IPC
}IPC_NICKNAME_E;

typedef enum {
	BLIGHT_CLOSE = 0x10,
	BLIGHT_OPEN = 0x11,
	CLIGHT_CLOSE = 0x20,
	MUSICLIGHT_OPEN = 0x21,
	MOODLIGHT_OPEN = 0x22,
}enLIGHT_CTRL;


typedef struct xm_COORD_S
{
    XM_S32 s32X;
    XM_S32 s32Y;
}COORD_S;

/************************************************************************
 *
 *��ȡ�豸���ض�����ID
 * ����ö��IPC_NICKNAME_E
 *
 ************************************************************************/
extern int get_hwinfo(int info_cmd);
extern int GetProductNickName(void);
/************************************************************************
�ӿ����ڿ�: 	libdvr.so
��������: 		��ȡProductDefinition��Ĳ�Ʒ�ͺ�
�������:		�洢�ַ����ĵ�ַ
���ز���:		0:	�ɹ�
					-1:	ʧ��
 ************************************************************************/
int GetProductString(char *pString);

///��������̬����
typedef struct ENCODE_STATICPARAM
{
	char reserved[2];
	int  profile;    
	int level;
	int reserved1[4];
} ENCODE_STATICPARAM;


#define NAME_LEN  16
typedef struct ispconfig_json_s 
{
	char deviceType[NAME_LEN];
	char oemName[NAME_LEN];
	unsigned char u8InfraredIO;
	unsigned char u8InfraredSwap;
	unsigned char u8IrCutIO;
	unsigned char u8IrCutSwap;

/*****************************************
u8IRLed: (0: default   ����ģ��)
	bit0: 	(1: ������Ƶ�(WL/IR)        0: Ӳ�����Ƶ�(����))
	bit4:	(1: Ӳ����					0: �����)
*****************************************/
	unsigned char u8IRLed;
} ISPCONFIG_JSON_S;

/*
typedef enum WB_MODE
{
	WB_DISABLE,			// ��ֹ
	WB_AUTO,			// �Զ�
	WB_DAYLIGHT,		// �չ� 6500k
	WB_CLOUDY,			// ���� 7500k
	WB_INCANDESCENCE,	// ���ȹ� 5000k
	WB_FLUORESCENT,		// �չ�� 4400k
	WB_TUNGSTEN,			// ��˿�� 2800k
	WB_MANUAL			// �ֶ�
}WB_MODE;
*/
typedef enum IRCUT_SWITCH_MODE
{
	IRCUT_SYN_INFRARED,
	IRCUT_SWITCH_AUTO,
	IRCUT_BUTT
}IRCUT_SWITCH_MODE;
typedef enum DNC_MODE
{
	DNC_AUTO,			// �Զ��л�
	DNC_MULTICOLOR,		// ��ɫ
	DNC_BLACKWHITE,		// ǿ��Ϊ�ڰ�ģʽ
	DNC_INTE_WHITE_INF, //���ܾ���
	DNC_WHITELAMP_AUTO, //����ů��
	DNC_IRLAMP_AUTO,	//���ܺ���
	DNC_LP_MODE,		//����ģʽ
	DNC_BUTT
}DNC_MODE;

typedef enum IRCUT_MODE
{
	IRCUT_NIGHT,///����
	IRCUT_DAY,///�����˹�Ƭ
	IRCUT_AUTO,
}IRCUT_MODE;

typedef enum CAMERA_SCENE
{
	SCENE_AUTO,
	SCENE_INDOOR,
	SCENE_OUTDOOR,
	SCENE_BUTT,
}CAMERA_SCENE;
typedef enum IRCUT_SWITCH_DIRECTION
{
	NORMAL_DIRECTION,
//	CONTRARY_DIRECTION
}IRCUT_SWITCH_DIRECTION;

typedef struct XM_MOVEMENT_DATA_S
{
	 int 	reg_addr; 
	 int 	data; 
	 int   mode;
}MOVEMENT_DATA_S ;

/// ��Ƶ��ɫ��ʽ
typedef struct CAMERA_COLOR{
	unsigned char	Brightness;		///< ���ȣ�ȡֵ0-100��
	unsigned char	Contrast;		///< �Աȶȣ�ȡֵ0-100��
	unsigned char 	Saturation;		///< ���Ͷȣ�ȡֵ0-100��
	unsigned char 	Hue;			///< ɫ����ȡֵ0-100��
	unsigned char 	Gain;			///< ���棬ȡֵ0-100��bit7��λ��ʾ�Զ����棬����λ�����ԡ�
	unsigned char	WhiteBalance;	///< �Զ��׵�ƽ���ƣ�bit7��λ��ʾ�����Զ�����.0x0,0x1,0x2�ֱ�����,��,�ߵȼ�
	unsigned short	Acutance;       	///< ��ȣ�ȡֵ0-15, ��8λ��ʾˮƽ��ȣ���8Ϊ��ʾ��ֱ��ȡ�
}CAMERA_COLOR;	// ��VIDEO_COLORһ��


// �������
typedef struct tagCAPTURE_FORMAT
{
    BYTE    Compression;        /*!< ѹ��ģʽ */
    BYTE    BitRateControl;     /*!< �������� */
    BYTE    ImageSize;          /*!< ͼ��ֱ��� */
    BYTE    ImageQuality;       /*!< ͼ���� */
    BYTE    FramesPerSecond;    /*!< ֡�� */
    BYTE    AVOption;           /*!< ����Ƶѡ�� */
    WORD    BitRate;            ///< �ο�����ֵ��KbpsΪ��λ
    BYTE    GOP;                /*< ֡������ֵ������ֵ49��99*/
    BYTE    reserved[3];        /*< �����ֽ�*/
} CAPTURE_FORMAT;


typedef struct stCAM_INIT_DATE
{
	XM_S32 mask;			//����
	XM_U8 u8GammaDay;
	XM_U8 u8GammaNight;
	XM_U8 u8LumDefDay;
	XM_U8 u8LumDefNight;
	XM_U8 u8InfrGpioNum;		// �ư�GPIO
	XM_U16 u16GainDef;			// x1
	XM_U16 u16GainDefSD;		// x1
	XM_U16 u16GainMax;			// x1
	XM_U32 u32DnThrDay[5];
	XM_U32 u32DnThrNight[5];
	XM_U32 u32EshutterLvEn;		// E Shutter enable level
	XM_U32 u32EshutterLvDis;		// E Shutter Disbale level	

	XM_U32 u32awb_agc;			// bit7: En  bit0~bit6: Choice
	XM_U32 u32AgcSLvlAwb;		// >= limit awb gain
	XM_U32 u32AgcELvlAwb;		// <= not limit awb gain
	XM_U32 u32CscLumCon;		// HighByte -> LowByte [Lum Con]
	XM_U32 u32CscAcutance;		// Autance
	XM_U32 u32CscHueSat;		// HighByte -> LowByte [Hue Sat]

	XM_S32 s32TgtNum;	//[0,3]
	XM_U32 au32TgtExp[4];
	XM_U32 au32TgtLum[4];
	XM_U32 u32FlashExtCfgEn;		// 0xA55A: Enable
	XM_U32 u32GammaAgc;		// bit7: En  bit0~bit6: Choice
	XM_U32 u32GamAgcStartLvl;	// > StartLevel  use AgcGamma
	XM_U32 u32GamAgcEndLvl;		// < EndLevel use NormalGamma
	XM_S32 s32Rvs;				// Bit0: Infrared swap
}CAM_INIT_DATA;

//����VI���ڳߴ�Ľṹ��
typedef struct st_VI_WIN_S
{
    XM_U32 u32Width;
    XM_U32 u32Height;
    XM_U32 u32TotalWidth;
    XM_U32 u32TotalHeight;
}VI_WIN_S;


// ���ð�ƽ�����
int camera_set_wb_mode(unsigned int mode);


/*************************************************************************
��������:	��ȡ/���� ��ҹģʽ
�������:	0:	Auto
				1:    Color
				2: 	BW
note:
*************************************************************************/
int camera_get_dnc_mode(XM_U32 *pMode);
int camera_set_dnc_mode(unsigned int mode);


// ��ȡ֧�ֵ��ع�ȼ���
// ����ֵ<0����ȡʧ�ܣ�>=0��ʾ�ȼ���������ȼ�����������speeds�С�
// speeds���鳤�ȱ����㹻��ȡ16����ˡ�
int camera_get_exposure_speeds(int vstd, unsigned int *speeds);


/*************************************************************************
��������:	�ع�ģʽ����
�������:	level:	0		�Զ��ع�
						1~9		�ֶ��ع�(�̶�����)	
				value1:	�ع�����(min,us)
				value2:	�ع�����(max,us)
note:	
	�Զ��ع�(��������Ч)  		level :0
	�ֶ��ع�(�̶�����)			level :1~9
*************************************************************************/
int camera_get_exposure_level(int *pLevel, XM_U32 *pValue1, XM_U32 *pValue2);
int camera_set_exposure_level(int level, unsigned int value1, unsigned int value2);

/*******************************************************************
��������:	����/��ȡ�Զ�����
�������:	s32GainMax:	�������	( 0~100	def: 50)
				s32AutoEn:	AutoGain ʹ��	(0~1		def: 1)
�������:	��
���ز���:	0 	�ɹ�
				-1	����
Note:			Lycai
*******************************************************************/
int camera_get_gain(int *pGainLevel, int *pAutoEn);
int camera_set_gain(int s32GainMax, int s32AutoEn);


// ���òο���ƽֵ
// level:�ο���ƽֵ��ȡֵ0-100��
int camera_get_refrence_level(void);
int camera_set_refrence_level(int level);


// ��ȡƽ������ֵ
int camera_get_luminance(void);

//��ȡ״̬��,>=0��ʾ״̬����,<0��ʾ״̬�쳣
int camera_get_status(int *status);

//���ڵ���
int camera_debug(char *cmd);

//����xm 2a��İ汾��Ϣ
int camera_aew_get_version(char *str);

//����WB����
void  camera_get_wb_params(void *param);

//�����Զ���Ȧģʽ
int camera_set_aperture(unsigned int mode);

//���ñ��ⲹ��ģʽ
int camera_get_blc(XM_U32 *pMode);
int camera_set_blc(unsigned int mode);

// �����龰ģʽ
int camera_get_scene(CAMERA_SCENE* pScene);
int camera_set_scene(CAMERA_SCENE scene);


/*******************************************************************
��������:	��ȡ����������״̬(��ǰ)
�������:	��
�������:	*pu8EShutterSts:
					 fps' = fps*0x10/gu8EshutterSts
���ز���:	0:	�ɹ�
				-1:	ʧ��
*******************************************************************/
int camera_get_es_status(XM_U8 *pu8EShutterSts);



/*************************************************************************
��������: 	��֡���ܣ�ʹ�ܣ�
�������:	es_shutter: 
					0:		1/1
					2:		1/2
					4:		1/3
					6:		1/4
�������:	��
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_get_es_shutter(int *pEshutterLevel);
int camera_set_es_shutter(int es_shutter);
///�����չ�Ʒ�������
//en = 0 ������ en= 1����
int camera_get_reject_flicker(int *pEn);
int camera_set_reject_flicker(int en);

// ��ҹ�л���ֵ
int camera_get_dnc_thr(int *pDnc_thr);
int camera_set_dnc_thr(int dnc_thr); 		//��ҹת����ֵ 10-50��Ĭ��30

/*******************************************************************
��������:	��ȡ/���� ��ҹ���������
�������:	��
�������:	* pLevel:	�ȼ�(1~10)
���ز���:	0 	�ɹ�
				-1	����
Note:			Lycai
*******************************************************************/
int camera_get_ae_sensitivity(int*pLevel);
int camera_set_ae_sensitivity(int ae_sensitivity); ///ae��������� 1-10��Ĭ��Ϊ5

/*******************************************************************
��������:	��ȡ/���� AE���������
�������:	��
�������:	* pLevel:	�ȼ�(1~10)
���ز���:	0 	�ɹ�
				-1	����
Note:			Lycai
*******************************************************************/
int camera_get_ae_sensitivity2(int* pLevel);
int camera_set_ae_sensitivity2(int level);


//int camera_set_RGBGain(int channel, int gainval);//����RGB����1 REDͨ��  2 GREENͨ��  3BLUEͨ��
int camera_get_Infrared(void);//��ȡ�豸����״̬ 1��ʾ����� 0��ʾ�ر� 2��ʾоƬ��֧��
int camera_set_Ircut(int mode);//����ircut״̬
int camera_save_debug_cmd(char *cmd);

/*******************************************************************
��������:	��ȡ/������ҹ����ȼ�
�������:	daynight:		0(Day)	1(Night)
				nf_level: 		0~5   Def:3
�������:	��
���ز���:	0 	�ɹ�
				-1	����
Note:			Lycai
*******************************************************************/
int CameraGetNFLevel(int daynight, int *pNrLevel);
int CameraSetNFLevel(int daynight, int nf_level);
//swap 0:������ 1������
int CameraGetSwapICR(int *pSwap);
int CameraSwapICR(int swap);

int test_movement(int x,int y,int z);

int movement_ircut(int level);

int movement_gpioset(int addr,int mode);

int movement_addrset(MOVEMENT_DATA_S *);

// DWDR
int camera_get_wdr(int* pLevel, int* pEnable);
int camera_set_wdr(int level, int enable);


int Camera_Get_StyleMode(int *pChoice);
int Camera_Set_StyleMode(int choice);		//�������:   0, 1, 2

int Camera_Get_DebugFile(char *fliename, unsigned int choice, unsigned int depth);

int Movement_LumTarget_Change(int lum_now);		//�ı�Ŀ������

int Camera_Get_DebugFile(char *fliename, unsigned int choice, unsigned int depth);

int camera_scan_task(XM_U32 u32Tms);

// IR-CUT ģʽ
int camera_get_ircut_mode(int *pIrcutMode);
int camera_set_ircut_mode(int ircut_mode);

// ����
int camera_get_mirror(int *pMirror);
int camera_set_mirror(int mirror);

// ��ת
int camera_get_flip(int *pFlip);
int camera_set_flip(int flip);


/*************************************************************************
��������: 	ͼ����ɫ(Web) �ӿ�
�������:	channel:	 ��Ч
				pColor:
					Brightness: ����(0~100)
					Contrast: �Աȶ�(0~100)
					Saturation: ���Ͷ�(0~100)
					Hue:	ɫ��(0~100)
					Acutance: ��(0~15)
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_get_color(int channel, CAMERA_COLOR * pColor);
int camera_set_color(int channel, CAMERA_COLOR * pColor);


/*************************************************************************
��������: 	��ȡ/���� ��Ƶ��ʽ(PAL/NTSC)
�������:	channel:	 ��Ч
				u32Vstd: 0:(UN)
						1:PAL
						2:NTSC
�������:	*pu32Vstd	0:(UN)
							1:PAL
							2:NTSC
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_get_vstd(int channel,unsigned int *pu32Vstd);
int camera_set_vstd(int channel,unsigned int u32Vstd);



// u32Level: 	0~100
// def:		0
/*************************************************************************
��������: 	��ȡ/���� ȥα��
�������:	u32Level: 	0~100(def:0)
�������:	pu32Level:	��ǰ�ȼ�
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_set_antiFalseColor(unsigned int u32Level);
int camera_get_antiFalseColor(unsigned int *pu32Level);

// u32Level: 	0~100
// def:		0
/*************************************************************************
��������: 	��ȡ/���� ȥ���
�������:	u32Level: 	0~100(def:0)
�������:	pu32Level:	��ǰ�ȼ�
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_set_sawtooth(unsigned int u32Level);
int camera_get_sawtooth(unsigned int *pu32Level);

//��������ع���
//Enable = 0 ������ Enable = 1����
int camera_set_hlc(int Enable);
int camera_get_hlc(int *pEnable);


/*************************************************************************
��������: 	camera_set_format
�������:	chn
				u32Type: 0MainStream	1:SubStream1	2:SubStream2
				pstFormat: encode paramer
�������:	none
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_set_format(int chn, unsigned int u32Type,const CAPTURE_FORMAT *pstFormat);


/*************************************************************************
��������: 	��֡��չ����(XM320ʹ��)
�������:	u8Mode:		1: Read 	
							2: Write
				pu8Status: 			fps
							0:		1/1
							2:		1/2
�������:	��
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_es_shutter_ex(XM_U8 u8Mode, XM_U8 *pu8Status);


/*************************************************************************
��������: 	��Ʒ�ͺ�(��/д)
�������:	u8Mode:		1: Read 	
							2: Write
				*pu32ProductType:	��Ʒ�ͺ�
�������:	*pu32ProductType:	��Ʒ�ͺ�
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_productType(XM_U8 u8Mode, XM_U32 *pu32ProductType);



/*************************************************************************
��������: 	��ȡ/�����ڲ�����
�������:	pstCamearPara: ָ�������ַ
�������:	��
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_para_get(CAM_INIT_DATA *pstCamearPara);
int camera_para_set(CAM_INIT_DATA *pstCamearPara);



/*************************************************************************
��������: 	����/��ȡ ����AWBɫ��
�������:	u8MinCt: 	���ɫ��	bit7: En  bit0~bit6: Choice
			u32Start: 	��ʼ��������(x1024)
			u32End: 	�ſ���������(x1024)
�������:	��
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_awbLimit_set(XM_U8 u8MinCt, XM_U32 u32Start, XM_U32 u32End);
int camera_awbLimit_get(XM_U8 *pu8MinCt, XM_U32 *pu32Start, XM_U32 *pu32End);

/*************************************************************************
��������: 	����/��ȡ Burstʹ��(BW)
�������:	u8Enable: 0:Disable 	1:Enable
�������:	��
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_bwBurst_set(XM_U8 u8Enable);
int camera_bwBurst_get(XM_U8 *pu8Enable);



int camera_get_vdaMovState(XM_U32 *pData);


int camera_set_wbRB(XM_U8 u8Data);
int camera_get_wbRB(XM_U8 *pu8Data);

int camera_set_wbGM(XM_U8 u8Data);
int camera_get_wbGM(XM_U8 *pu8Data);

int camera_set_encoderinfo(XM_U8 *pu8Dta);

/*************************************************************************
��������:	��ȡ��ҹ״̬
�������:	pu8Mode:
					0:	Color
					1:    BW
note:
*************************************************************************/
int camera_get_dn_state(XM_U8 *pu8Mode);


/*************************************************************************
��������:	����/��ʽ/�ֱ��� ģʽ�л�
�������:	u8Mode:
					0x00:AHD	0x01:CVI		0x02:TVI		0x10:XVI
				u8VstdMode:	
					1: PAL	2:NTSC
				u8RlstMode:
					0:1M 1:2M 3:3M 4:4M 5:5M
note:
*************************************************************************/
int camera_set_isp_para(XM_U8 u8Mode, XM_U8 u8VstdMode, XM_U8 u8RlstMode);
int camera_get_isp_para(XM_U8 *pu8Mode);


int camera_set_language(int s32Language);



/*************************************************************************
��������: 	����/��ȡ LEDģʽ
�������:	ps32Mode:	0xAB(	A:	0: IR   1: WL   --- ����ȡʱ��Ч
							 	B:	0:Auto  1:Manual  2:Intelligence)
				ps32State: 	0:Close 1:Open
�������:	��
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_set_ledMode(int s32Mode, int s32State);
int camera_get_ledMode(int *ps32Mode, int *ps32State);

/*************************************************************************
��������: 	���� LED����
�������:	s32Type:	0: IR   1: WL  2:Double
�������:	��
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_set_ledType(int s32Type);
int camera_get_ledType(int *ps32Type);




/*****************************************************************************
��������:	�ⲿ����ѡ��gamma
�������:    	u32Gamma: [High Byte->Low Byte] = [u8IdxWgt(Idx2)  u8Idx2  u8Idx1]
				u8Idx1:	0: 		disable gamma  (����)
						1~15:	LinearGamma
						128~255:	WdrGamma
				u8Idx2:	0: 		disable gamma  (����)
						0~15:	LinearGamma
						128~255:	WdrGamma
				u8IdxWgt: (0~255)
						u8Idx1 Weight: 255-u8IdxWgt
						u8Idx2 Weight: u8IdxWgt				
���ز���:    0: �ɹ� -1: ʧ��
*****************************************************************************/
int camera_set_gamma(XM_U32 u32Gamma);
int camera_get_gamma(XM_U32 *pu32Gamma);


int camera_set_smartVda(XM_U8 u8MovFlg);
int camera_get_smartVda(XM_U8 *pu8MovFlg);


/*************************************************************************
��������: 	��ȡ�ȶ�״̬(����)
�������:	��
�������:	pu8StateFlg
					0: �ȶ�
					1: ����LED
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_get_stabState(XM_U8 *pu8StateFlg);

/*************************************************************************
��������: 	��ȡ ����LED����ʱ��(ms)
�������:	s32Tms:  ����ʱ��(ms) ----(����100ms)
�������:	��
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_set_ledHold(int s32Tms);


/*************************************************************************
��������: 	2Mץͼ�ӿ�
�������:	��
�������:	pstSnapVFInfo
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_snap(VIDEO_FRAME_INFO_S *pstSnapVFInfo);


/*************************************************************************
��������: 	����ͼ������
�������:	mode: ����
				stCoord: ������Ϣ
�������:	��
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_set_coord(int mode, COORD_S stCoord);

/*************************************************************************
��������: 	����ͼ������
�������:	pstViWin 
�������:	��
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_get_vi_resolution(VI_WIN_S *pstViWin);


int camera_init(XM_U32 *pu32ProductType);



typedef struct st_AIRSLT_INFO_S
{
    XM_U8 u8AlarmFlg;	//1: ��������   	0:δ����
    XM_U32 u32TargetNum;	// Ŀ�����;
    XM_U32 u32Info[64];		// ������Ϣ(�����)
}AIRSLT_INFO_S;


/*************************************************************************
��������: 		�������ܱ�����Ϣ(���μ��)
�������:		u8Cmd:  ������
			pstAiInfoRlst:	������Ϣ
�������:	��
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_set_aiInfo(XM_U8 u8Cmd, AIRSLT_INFO_S *pstAiInfoRlst);



/*************************************************************************
��������: 		�������η�����ʹ��
�������:		en:	1:ʹ��   0:����
�������:	��
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_set_aeweight(int en);
int camera_get_aeweight(int *ps32En);


/*************************************************************************
��������:		�����������ֵ
�������:		dnc_thr:
			 1~5(default:3) (ԽС��Խ���кڰ�)
�������:	��
���ز���:	0:	�ɹ�
				-1: ����
*************************************************************************/
int camera_set_softIr_thr(int dnc_thr);


#ifdef SOC_NONE
int camera_get_txAttr(XM_U8 *pu8En, VENC_TX_ATTR *pstTxAttr);
int camera_set_txAttr(XM_U8 u8En, VENC_TX_ATTR stTxAttr);
#endif




//#if(defined SOC_ALIOS) || (defined SOC_XMSDK)
typedef struct xm_CAMERACFG_TO_ISP
{
	unsigned int u32ProductType;
	unsigned int u32StdType;		// 0: unknow 1:PAL 2:NTSC
	unsigned int u32RsltType;		// 0: 720P	1:1080P	2:960P 3:1536P		101:P1080_S1 	102:P1080_S2

	/****************************
	u32IRLed:  
		bit31:
			0	not use
			1	use
		bit0:
			0  	��ͨģʽ������ͬ��/�Զ�ͬ��... (Ӳ�����ƺ����)��
			1	������Ƶ�
		bit4:
			0	�����
			1	Ӳ����
	****************************/
	unsigned int u32IRLed;
	unsigned int au32RsltCh[4][4];	// (Ch0_Width Ch0_Height Ch1_Width Ch1_Height) * 4

	/****************************
	u32Infrared:  
		bit31:
			0	not use
			1	use
		bit0: InfraredSwap
		bit8~bit15: InfraredGPIO (0xFF: choice by source)
	****************************/
	unsigned int u32Infrared;

	/****************************
	u32IrCut:  
		bit31:
			0	not use
			1	use
		bit0: IRCUT Swap
		bit8~bit15: InfraredGPIO (0xFF: choice by source)
	****************************/
	unsigned int u32IrCut;
	unsigned int au32Rsv[10];
}CAMERACFG_TO_ISP;

/*************************************************************************
��������: 	����ISP���ҵ��(���ͼ����ش���)
�������:	pstViWin 
�������:	��
���ز���:	0:	�ɹ�
				-1:	����
*************************************************************************/
int camera_isp_task(CAMERACFG_TO_ISP *pstCfg);


/*************************************************************************
��������:	ͼ����ؽ����˳�����
�������:	��
�������:	��
���ز���:		0:	�ɹ�
			-1:	����
*************************************************************************/
int camera_exit(void);


/***************************************************************
��������:	ȥ�����
�������:	enable:	�򿪹ر�[0,1]
				level:	ǿ�ȵȼ�[0,100]
���ز���:	0:		�ɹ�
				-1:		����
***************************************************************/
int Camera_SetClearFog(int enable, int level);


//#endif

#ifdef __cplusplus
}
#endif

#endif

