/////////////////////////////////////////////////////////////
//
// Many source code lines are copied from RaspiVid.c
// Copyright (c) 2012, Broadcom Europe Ltd
// 
// Lines have been added by Pierre Raufast - mai 2013
// pierre.raufast@gmail.com
// to work with OpenCV 2.3
// visit thinkrpi.wordpress.com
// Enjoy !
// This file display camera in a OpenCv window
//
// For a better world, read Giono's Books
// 
/////////////////////////////////////////////////////////////
//
// Emil Valkov - robidouille@valkov.com
//
// Converted to a library, which exposes an interface
// similar to what OpenCV provides, but uses the RaspiCam
// underneath - September 22 2013.
//
// cvCreateCameraCapture 	-> raspiCamCvCreateCameraCapture
//							-> raspiCamCvCreateCameraCapture2(with config)
// cvReleaseCapture 		-> raspiCamCvReleaseCapture
// cvSetCaptureProperty 	-> raspiCamCvSetCaptureProperty
// cvGetCaptureProperty 	-> raspiCamCvGetCaptureProperty
// cvQueryFrame 			-> raspiCamCvQueryFrame
//
/////////////////////////////////////////////////////////////

#include "RaspiCamCV.h"
#include "flash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>

//new
#include <cv.h>
#include <highgui.h>
#include "time.h"

#include "bcm_host.h"
#include "interface/vcos/vcos.h"

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_logging.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_connection.h"

#include "RaspiCamControl.h"

#include <semaphore.h>

/// Camera number to use - we only have one camera, indexed from 0.
#define CAMERA_NUMBER 0

// Standard port setting for the camera component
#define MMAL_CAMERA_PREVIEW_PORT 0
#define MMAL_CAMERA_VIDEO_PORT 1
#define MMAL_CAMERA_CAPTURE_PORT 2

// Video format information
#define VIDEO_FRAME_RATE_NUM 30
#define VIDEO_FRAME_RATE_DEN 1

/// Video render needs at least 2 buffers.
#define VIDEO_OUTPUT_BUFFERS_NUM 3

// Max bitrate we allow for recording
const int MAX_BITRATE = 30000000; // 30Mbits/s

int mmal_status_to_int(MMAL_STATUS_T status);

/** Structure containing all state information for the current run
 */
typedef struct _RASPIVID_STATE
{
	int finished;
	int width;            	/// Requested width of image
	int height;           	/// requested height of image
	int bitrate;          	/// Requested bitrate
	int framerate;        	/// Requested frame rate (fps)
	int monochrome;			/// Capture in gray only (2x faster)
	int immutableInput;     /// Flag to specify whether encoder works in place or creates a new buffer. Result is preview can display either
                            /// the camera output or the encoder output (with compression artifacts)
	RASPICAM_CAMERA_PARAMETERS camera_parameters; /// Camera setup parameters

	MMAL_COMPONENT_T *camera_component;    /// Pointer to the camera component
	MMAL_COMPONENT_T *encoder_component;   /// Pointer to the encoder component

	MMAL_POOL_T *video_pool; /// Pointer to the pool of buffers used by encoder output port

	IplImage * dstImages [2];
	int dstImageIndex ;

	VCOS_SEMAPHORE_T capture_sem;
	VCOS_SEMAPHORE_T capture_done_sem;

} RASPIVID_STATE;

static void default_status(RASPIVID_STATE *state)
{
   if (!state)
   {
      vcos_assert(0);
      return;
   }

   // Default everything to zero
   memset(state, 0, sizeof(RASPIVID_STATE));

   // Now set anything non-zero
   state->finished          = 0;
   state->width 			= 640;      // use a multiple of 320 (640, 1280)
   state->height 			= 480;		// use a multiple of 240 (480, 960)
   state->bitrate 			= 17000000; // This is a decent default bitrate for 1080p
   state->framerate 		= VIDEO_FRAME_RATE_NUM;
   state->immutableInput 	= 1;
   state->monochrome 		= 0;		// Gray (1) much faster than color (0)

   // Set up the camera_parameters to default
   raspicamcontrol_set_defaults(&state->camera_parameters);
}

/**
 *  buffer header callback function for video
 *
 * @param port Pointer to port from which callback originated
 * @param buffer mmal buffer header pointer
 */
static void video_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
	MMAL_BUFFER_HEADER_T *new_buffer;
	RASPIVID_STATE * state = (RASPIVID_STATE *)port->userdata;

	if (state)
	{
		if (state->finished) {
			vcos_semaphore_post(&state->capture_done_sem);
			return;
		}
		if (buffer->length)
		{
			mmal_buffer_header_mem_lock(buffer);
 			flash_update();

			int w=state->width;	// get image size
			int h=state->height;

			int copy_size = (state->dstImages[state->dstImageIndex]->height) * (state->dstImages[state->dstImageIndex]->widthStep);

			state->dstImageIndex = state->dstImageIndex ? 0 : 1 ;
			memcpy(state->dstImages[state->dstImageIndex]->imageData,buffer->data, copy_size); //buffer is larger than actual image

			vcos_semaphore_post(&state->capture_done_sem);
			vcos_semaphore_wait(&state->capture_sem);

			mmal_buffer_header_mem_unlock(buffer);
		}
		else
		{
			vcos_log_error("buffer null");
		}
	}
	else
	{
		vcos_log_error("Received a encoder buffer callback with no state");
	}

	// release buffer back to the pool
	mmal_buffer_header_release(buffer);

	// and send one back to the port (if still open)
	if (port->is_enabled)
	{
		MMAL_STATUS_T status;

		new_buffer = mmal_queue_get(state->video_pool->queue);

		if (new_buffer)
			status = mmal_port_send_buffer(port, new_buffer);

		if (!new_buffer || status != MMAL_SUCCESS)
			vcos_log_error("Unable to return a buffer to the encoder port");
	}
}


/**
 * Create the camera component, set up its ports
 *
 * @param state Pointer to state control struct
 *
 * @return 0 if failed, pointer to component if successful
 *
 */
static MMAL_COMPONENT_T *create_camera_component(RASPIVID_STATE *state)
{
	MMAL_COMPONENT_T *camera = 0;
	MMAL_ES_FORMAT_T *format;
	MMAL_PORT_T *preview_port = NULL, *video_port = NULL, *still_port = NULL;
	MMAL_STATUS_T status;

	/* Create the component */
	status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &camera);

	if (status != MMAL_SUCCESS)
	{
	   vcos_log_error("Failed to create camera component");
	   goto error;
	}

	if (!camera->output_num)
	{
	   vcos_log_error("Camera doesn't have output ports");
	   goto error;
	}

	video_port = camera->output[MMAL_CAMERA_VIDEO_PORT];
	still_port = camera->output[MMAL_CAMERA_CAPTURE_PORT];

	//  set up the camera configuration
	{
	   MMAL_PARAMETER_CAMERA_CONFIG_T cam_config =
	   {
	      { MMAL_PARAMETER_CAMERA_CONFIG, sizeof(cam_config) },
	      .max_stills_w = state->width,
	      .max_stills_h = state->height,
	      .stills_yuv422 = 0,
	      .one_shot_stills = 0,
	      .max_preview_video_w = state->width,
	      .max_preview_video_h = state->height,
	      .num_preview_video_frames = 3,
	      .stills_capture_circular_buffer_height = 0,
	      .fast_preview_resume = 0,
	      .use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RESET_STC
	   };
	   mmal_port_parameter_set(camera->control, &cam_config.hdr);
	}
	// Set the encode format on the video  port
	
	format = video_port->format;
	if (state->monochrome)
	{
		format->encoding_variant = MMAL_ENCODING_I420;
		format->encoding = MMAL_ENCODING_I420;
	}
	else
	{
		format->encoding = MMAL_ENCODING_RGB24;
		format->encoding_variant = MMAL_ENCODING_RGB24;
	}

	format->es->video.width = VCOS_ALIGN_UP(state->width, 32);
	format->es->video.height = VCOS_ALIGN_UP(state->height, 16);
	format->es->video.crop.x = 0;
	format->es->video.crop.y = 0;
	format->es->video.crop.width = state->width;
	format->es->video.crop.height = state->height;
	format->es->video.frame_rate.num = state->framerate;
	format->es->video.frame_rate.den = VIDEO_FRAME_RATE_DEN;

	status = mmal_port_format_commit(video_port);
	if (status)
	{
	   vcos_log_error("camera video format couldn't be set");
	   goto error;
	}

	// PR : plug the callback to the video port
	status = mmal_port_enable(video_port, video_buffer_callback);
	if (status)
	{
	   vcos_log_error("camera video callback2 error");
	   goto error;
	}

   // Ensure there are enough buffers to avoid dropping frames
   if (video_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
      video_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;


   // Set the encode format on the still  port
   format = still_port->format;
   format->encoding = MMAL_ENCODING_OPAQUE;
   format->encoding_variant = MMAL_ENCODING_I420;
   format->es->video.width = VCOS_ALIGN_UP(state->width, 32);
   format->es->video.height = VCOS_ALIGN_UP(state->height, 16);
   format->es->video.crop.x = 0;
   format->es->video.crop.y = 0;
   format->es->video.crop.width = state->width;
   format->es->video.crop.height = state->height;
   format->es->video.frame_rate.num = 1;
   format->es->video.frame_rate.den = 1;

   status = mmal_port_format_commit(still_port);
   if (status)
   {
      vcos_log_error("camera still format couldn't be set");
      goto error;
   }


	//PR : create pool of message on video port
	MMAL_POOL_T *pool;
	video_port->buffer_size = video_port->buffer_size_recommended;
	video_port->buffer_num = video_port->buffer_num_recommended;
	pool = mmal_port_pool_create(video_port, video_port->buffer_num, video_port->buffer_size);
	if (!pool)
	{
	   vcos_log_error("Failed to create buffer header pool for video output port");
	}
	state->video_pool = pool;

	/* Ensure there are enough buffers to avoid dropping frames */
	if (still_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
	   still_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;

	/* Enable component */
	status = mmal_component_enable(camera);

	if (status)
	{
	   vcos_log_error("camera component couldn't be enabled");
	   goto error;
	}

	raspicamcontrol_set_all_parameters(camera, &state->camera_parameters);

	state->camera_component = camera;
	return camera;

error:

   if (camera)
      mmal_component_destroy(camera);

   return 0;
}

/**
 * Destroy the camera component
 *
 * @param state Pointer to state control struct
 *
 */
static void destroy_camera_component(RASPIVID_STATE *state)
{
   if (state->camera_component)
   {
      mmal_component_destroy(state->camera_component);
      state->camera_component = NULL;
   }
}


/**
 * Destroy the encoder component
 *
 * @param state Pointer to state control struct
 *
 */
static void destroy_encoder_component(RASPIVID_STATE *state)
{
   // Get rid of any port buffers first
   if (state->video_pool)
   {
      mmal_port_pool_destroy(state->encoder_component->output[0], state->video_pool);
   }
}

/**
 * Connect two specific ports together
 *
 * @param output_port Pointer the output port
 * @param input_port Pointer the input port
 * @param Pointer to a mmal connection pointer, reassigned if function successful
 * @return Returns a MMAL_STATUS_T giving result of operation
 *
 */
static MMAL_STATUS_T connect_ports(MMAL_PORT_T *output_port, MMAL_PORT_T *input_port, MMAL_CONNECTION_T **connection)
{
   MMAL_STATUS_T status;

   status =  mmal_connection_create(connection, output_port, input_port, MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT);

   if (status == MMAL_SUCCESS)
   {
      status =  mmal_connection_enable(*connection);
      if (status != MMAL_SUCCESS)
         mmal_connection_destroy(*connection);
   }

   return status;
}

/**
 * Checks if specified port is valid and enabled, then disables it
 *
 * @param port  Pointer the port
 *
 */
static void check_disable_port(MMAL_PORT_T *port)
{
   if (port && port->is_enabled)
      mmal_port_disable(port);
}

double raspiCamCvGetCaptureProperty(RaspiCamCvCapture * capture, int property_id)
{
    switch(property_id)
    {
		case RPI_CAP_PROP_FRAME_HEIGHT:
			return capture->pState->height;
		case RPI_CAP_PROP_FRAME_WIDTH:
			return capture->pState->width;
		case RPI_CAP_PROP_FPS:
			return capture->pState->framerate;
		case RPI_CAP_PROP_MONOCHROME:
			return capture->pState->monochrome;
		case RPI_CAP_PROP_BITRATE:
			return capture->pState->bitrate;
    }
    return 0;
}

int raspiCamCvSetCaptureProperty(RaspiCamCvCapture * capture, int property_id, double value)
{
    int retval = 0; // indicate failure

/* 
	Naive implementation does not work.	Need to reset the camera and restart
	switch(property_id)
	{
		case RPI_CAP_PROP_FRAME_HEIGHT:
			capture->pState->height = value;
			break;
		case RPI_CAP_PROP_FRAME_WIDTH:
			capture->pState->width = value;
			break;
		case RPI_CAP_PROP_FPS:
			capture->pState->framerate = value;
			break;
		default:
			retval = 0;
			break;
	}
*/	
	return retval;
}

RaspiCamCvCapture * raspiCamCvCreateCameraCapture2(int index, RASPIVID_CONFIG* config)
{
	RaspiCamCvCapture * capture = (RaspiCamCvCapture*)malloc(sizeof(RaspiCamCvCapture));
	// Our main data storage vessel..
	RASPIVID_STATE * state = (RASPIVID_STATE*)malloc(sizeof(RASPIVID_STATE));
	capture->pState = state;
	
	MMAL_STATUS_T status = -1;
	MMAL_PORT_T *camera_video_port = NULL;
	MMAL_PORT_T *camera_still_port = NULL;

	bcm_host_init();

	default_status(state);
	
	if (config != NULL)	{
		if (config->width != 0) 		state->width = config->width;
		if (config->height != 0) 		state->height = config->height;
		if (config->bitrate != 0) 		state->bitrate = config->bitrate;
		if (config->framerate != 0) 	state->framerate = config->framerate;
		if (config->monochrome != 0) 	state->monochrome = config->monochrome;
	}

	int w = state->width;
	int h = state->height;
	int pixelSize = state->monochrome ? 1 : 3;

	//should use VCOS_ALIGN_UP to compute width and height and then set image ROI to inner w*h image
	state->dstImages[0] = cvCreateImage(cvSize(VCOS_ALIGN_UP(w, 32),VCOS_ALIGN_UP(h, 16)), IPL_DEPTH_8U, pixelSize); //final picture to display
	state->dstImages[1] = cvCreateImage(cvSize(VCOS_ALIGN_UP(w, 32),VCOS_ALIGN_UP(h, 16)), IPL_DEPTH_8U, pixelSize); // final picture to display
	//re-setting image size to what user passed
	state->dstImages[0]->width = w ;
	state->dstImages[0]->height = h;
	state->dstImages[0]->widthStep = VCOS_ALIGN_UP(w, 32) * pixelSize ;

	state->dstImages[1]->width = w ;
	state->dstImages[1]->height = h;
	state->dstImages[1]->widthStep = VCOS_ALIGN_UP(w, 32) * pixelSize ;

	state->dstImageIndex = 0 ;
	vcos_semaphore_create(&state->capture_sem, "Capture-Sem", 0);
	vcos_semaphore_create(&state->capture_done_sem, "Capture-Done-Sem", 0);

	// create camera
	if (!create_camera_component(state))
	{
	   vcos_log_error("%s: Failed to create camera component", __func__);
	   raspiCamCvReleaseCapture(&capture);
	   return NULL;
	}

	camera_video_port = state->camera_component->output[MMAL_CAMERA_VIDEO_PORT];
	camera_still_port = state->camera_component->output[MMAL_CAMERA_CAPTURE_PORT];

	// assign data to use for callback
	camera_video_port->userdata = (struct MMAL_PORT_USERDATA_T *)state;

	// start capture
	if (mmal_port_parameter_set_boolean(camera_video_port, MMAL_PARAMETER_CAPTURE, 1) != MMAL_SUCCESS)
	{
	   vcos_log_error("%s: Failed to start capture", __func__);
	   raspiCamCvReleaseCapture(&capture);
	   return NULL;
	}

	// Send all the buffers to the video port
		
	int num = mmal_queue_length(state->video_pool->queue);
	int q;
	for (q = 0; q < num; q++)
	{
		MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(state->video_pool->queue);
		
		if (!buffer)
			vcos_log_error("Unable to get a required buffer %d from pool queue", q);
		
		if (mmal_port_send_buffer(camera_video_port, buffer)!= MMAL_SUCCESS)
			vcos_log_error("Unable to send a buffer to encoder output port (%d)", q);
	}

	//mmal_status_to_int(status);	
		
	// Disable all our ports that are not handled by connections
	//check_disable_port(camera_still_port);

	//if (status != 0)
	//	raspicamcontrol_check_configuration(128);

	vcos_semaphore_wait(&state->capture_done_sem);
	return capture;
}


/**
*\brief Create a capture device for the camera
*\param index camera index
*\param config Configuration for the camera (framerate, resolution , ...)
*\param properties Capture properties (exposure, brightness ...)
*\param non_blocking Allow to setup the device for non-blocking capture (use cvGrab and cvRetrieve functions)
*\return The created capture device, NULL if something went wrong
*/
RaspiCamCvCapture * raspiCamCvCreateCameraCapture3(int index, RASPIVID_CONFIG* config, RASPIVID_PROPERTIES* properties, int non_blocking)
{
	RaspiCamCvCapture * capture = (RaspiCamCvCapture*)malloc(sizeof(RaspiCamCvCapture));
	// Our main data storage vessel..
	RASPIVID_STATE * state = (RASPIVID_STATE*)malloc(sizeof(RASPIVID_STATE));
	capture->pState = state;
	
	MMAL_STATUS_T status = -1;
	MMAL_PORT_T *camera_video_port = NULL;
	MMAL_PORT_T *camera_still_port = NULL;

	bcm_host_init();

	default_status(state);
	
	if (config != NULL)	{
		if (config->width != 0) 		state->width = config->width;
		if (config->height != 0) 		state->height = config->height;
		if (config->bitrate != 0) 		state->bitrate = config->bitrate;
		if (config->framerate != 0) 	state->framerate = config->framerate;
		if (config->monochrome != 0) 	state->monochrome = config->monochrome;
	}

	if(properties != NULL){
			state->camera_parameters.brightness = properties->brightness;
			state->camera_parameters.contrast = properties->contrast;
			state->camera_parameters.sharpness = properties->sharpness;
			state->camera_parameters.saturation = properties->saturation;
			state->camera_parameters.hflip = properties->hflip;
			state->camera_parameters.vflip = properties->vflip;
			state->camera_parameters.exposureMode = properties->exposure;
			state->camera_parameters.shutter_speed = properties->shutter_speed;
			state->camera_parameters.awbMode = (properties->awb > 0) ? MMAL_PARAM_AWBMODE_AUTO : MMAL_PARAM_AWBMODE_OFF ;
			state->camera_parameters.awb_gains_r = properties->awb_gr ;
			state->camera_parameters.awb_gains_b = properties->awb_gb ;
	}

	int w = state->width;
	int h = state->height;
	int pixelSize = state->monochrome ? 1 : 3;
	//printf("Adjusted size is : %u x %u \n", VCOS_ALIGN_UP(w, 32), VCOS_ALIGN_UP(h, 16));
	//state->dstImages[0] = cvCreateImage(cvSize(VCOS_ALIGN_UP(w, 32),VCOS_ALIGN_UP(h, 16)), IPL_DEPTH_8U, pixelSize); //final picture to di$
        //state->dstImages[1] = cvCreateImage(cvSize(VCOS_ALIGN_UP(w, 32),VCOS_ALIGN_UP(h, 16)), IPL_DEPTH_8U, pixelSize); // final picture to d$
        state->dstImages[0] = cvCreateImage(cvSize(w,h), IPL_DEPTH_8U, pixelSize); //final picture to
        state->dstImages[1] = cvCreateImage(cvSize(w,h), IPL_DEPTH_8U, pixelSize); // final picture to
	//re-setting image size to what user passed
        /*state->dstImages[0]->width = w ;
        state->dstImages[0]->height = h;
        state->dstImages[0]->widthStep = VCOS_ALIGN_UP(w, 32) * pixelSize ;

        state->dstImages[1]->width = w ;
        state->dstImages[1]->height = h;
        state->dstImages[1]->widthStep = VCOS_ALIGN_UP(w, 32) * pixelSize ;
	*/


	state->dstImageIndex = 0 ;
	vcos_semaphore_create(&state->capture_sem, "Capture-Sem", 0);
	vcos_semaphore_create(&state->capture_done_sem, "Capture-Done-Sem", 0);

	// create camera
	if (!create_camera_component(state))
	{
	   vcos_log_error("%s: Failed to create camera component", __func__);
	   raspiCamCvReleaseCapture(&capture);
	   return NULL;
	}
	
	camera_video_port = state->camera_component->output[MMAL_CAMERA_VIDEO_PORT];
	camera_still_port = state->camera_component->output[MMAL_CAMERA_CAPTURE_PORT];

	// assign data to use for callback
	camera_video_port->userdata = (struct MMAL_PORT_USERDATA_T *)state;

	// start capture
	if (mmal_port_parameter_set_boolean(camera_video_port, MMAL_PARAMETER_CAPTURE, 1) != MMAL_SUCCESS)
	{
	   vcos_log_error("%s: Failed to start capture", __func__);
	   raspiCamCvReleaseCapture(&capture);
	   return NULL;
	}

	// Send all the buffers to the video port
		
	int num = mmal_queue_length(state->video_pool->queue);
	int q;
	for (q = 0; q < num; q++)
	{
		MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(state->video_pool->queue);
		
		if (!buffer)
			vcos_log_error("Unable to get a required buffer %d from pool queue", q);
		
		if (mmal_port_send_buffer(camera_video_port, buffer)!= MMAL_SUCCESS)
			vcos_log_error("Unable to send a buffer to encoder output port (%d)", q);
	}

	//mmal_status_to_int(status);	
		
	// Disable all our ports that are not handled by connections
	//check_disable_port(camera_still_port);

	//if (status != 0)
	//	raspicamcontrol_check_configuration(128);
	if(non_blocking == 0) vcos_semaphore_wait(&state->capture_done_sem);
	return capture;
}



RaspiCamCvCapture * raspiCamCvCreateCameraCapture(int index)
{
	return raspiCamCvCreateCameraCapture2(index, NULL);
}

void raspiCamCvReleaseCapture(RaspiCamCvCapture ** capture)
{
	RASPIVID_STATE * state = (*capture)->pState;

	// Unblock the callback.
	state->finished = 1;
	vcos_semaphore_post(&state->capture_sem);
	vcos_semaphore_wait(&state->capture_done_sem);

	vcos_semaphore_delete(&state->capture_sem);
	vcos_semaphore_delete(&state->capture_done_sem);

	if (state->camera_component)
		mmal_component_disable(state->camera_component);

	destroy_camera_component(state);

	cvReleaseImage(&(state->dstImages[0]));
	cvReleaseImage(&(state->dstImages[1]));

	free(state);
	free(*capture);
	*capture = 0;
}

IplImage * raspiCamCvQueryFrame(RaspiCamCvCapture * capture)
{
	RASPIVID_STATE * state = capture->pState;
	vcos_semaphore_post(&state->capture_sem);
	vcos_semaphore_wait(&state->capture_done_sem);
	return state->dstImages[state->dstImageIndex];
}

int raspiCamCvGrab(RaspiCamCvCapture * capture)
{
        VCOS_STATUS_T  status ;
	RASPIVID_STATE * state = capture->pState;        
	status = vcos_semaphore_trywait(&state->capture_done_sem);
	if(status == VCOS_EAGAIN) return  0 ; //No frame available
	vcos_semaphore_post(&state->capture_sem); // One frame is available, trigger capture of the next one
        return 1 ;
}

IplImage * raspiCamCvRetrieve(RaspiCamCvCapture * capture)
{
	RASPIVID_STATE * state = capture->pState;
        return state->dstImages[state->dstImageIndex]; // retrieve last acquired frame
}


void raspiCamCvSetFlashPattern(unsigned char * pattern, unsigned char pattern_length){
         flash_set_pattern(pattern, pattern_length);
}

unsigned char raspiCamCvFlashEnable(unsigned char pin){
        unsigned char index = flash_init(pin);
	return index ;
}
