#ifndef METALIO_CAMERA_V4L2_DRIVER_H
#define METALIO_CAMERA_V4L2_DRIVER_H

#include "camera_screen.h"

// NuttX /dev/video0 capture driver (OV2710 RAW10 -> BGR888 preview).
CameraScreen::CameraDriver *GetNuttxV4l2CameraDriver();

#endif  // METALIO_CAMERA_V4L2_DRIVER_H
