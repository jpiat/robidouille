#ifndef __RaspiCamCV__
#define __RaspiCamCV__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _RASPIVID_STATE RASPIVID_STATE;

typedef struct
{
	int width;
	int height;
	int bitrate;
	int framerate;
	int monochrome;
} RASPIVID_CONFIG;

enum exposure_mode {
	OFF,
	AUTO,
	NIGHT,
	NIGHT_PREVIEW,
	BACKLIGHT,
	SPOTLIGHT,
	SPORTS,
	SNOW,
	BEACH,
	VERY_LONG,
	FIXED_FPS,
	ANTI_SHAKE,
	FIREWORKS
};

typedef struct {
	int sharpness ;
	int contrast ;
	int brightness ;
	int saturation ;
	int shutter_speed ;
	int hflip;
	int vflip;
	enum exposure_mode exposure ;
} RASPIVID_PROPERTIES;


typedef struct {
	RASPIVID_STATE * pState;
} RaspiCamCvCapture;

typedef struct _IplImage IplImage;

// Mirror of CV_CAP_PROP_* properties in opencv's highgui_c.h
enum
{
    RPI_CAP_PROP_FRAME_WIDTH    =3,
    RPI_CAP_PROP_FRAME_HEIGHT   =4,
    RPI_CAP_PROP_FPS            =5,
    RPI_CAP_PROP_MONOCHROME		=19,
    RPI_CAP_PROP_BITRATE		=37,   // no natural mapping here - used CV_CAP_PROP_SETTINGS
    RPI_CAP_PROP_BRIGHTNESS    =10,
    RPI_CAP_PROP_CONTRAST      =11,
    RPI_CAP_PROP_SATURATION    =12,
    RPI_CAP_PROP_HUE           =13,
    RPI_CAP_PROP_GAIN          =14,
    RPI_CAP_PROP_EXPOSURE      =15,

};

RaspiCamCvCapture * raspiCamCvCreateCameraCapture2(int index, RASPIVID_CONFIG* config);
RaspiCamCvCapture * raspiCamCvCreateCameraCapture3(int index, RASPIVID_CONFIG* config, RASPIVID_PROPERTIES* properties, int non_blocking);
RaspiCamCvCapture * raspiCamCvCreateCameraCapture(int index);
void raspiCamCvReleaseCapture(RaspiCamCvCapture ** capture);
double raspiCamCvGetCaptureProperty(RaspiCamCvCapture * capture, int property_id);
int raspiCamCvSetCaptureProperty(RaspiCamCvCapture * capture, int property_id, double value);
IplImage * raspiCamCvQueryFrame(RaspiCamCvCapture * capture);

int raspiCamCvGrab(RaspiCamCvCapture * capture);
IplImage * raspiCamCvRetrieve(RaspiCamCvCapture * capture);

#ifdef __cplusplus
}
#endif

#endif
