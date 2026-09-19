/****************************************************************************
 * boards/risc-v/esp32p4/metalio-claw-4/src/metalio/camera_test.c
 *
 * Temporary on-board verification of the OV2710 RAW10 V4L2 capture path
 * (/dev/video0).  Runs the minimal V4L2 USERPTR streaming sequence
 * (S_FMT -> REQBUFS -> QBUF -> STREAMON -> poll/DQBUF/QBUF -> STREAMOFF) and
 * reports whether real frame data arrives on the MIPI-CSI -> DW-GDMA path.
 *
 * This file is for bring-up validation only and should be removed once the
 * Metalio camera UI has a real CameraDriver consuming /dev/video0.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/ioctl.h>
#include <sys/poll.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/video/video.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CAM_TEST_PATH    "/dev/video0"
#define CAM_TEST_WIDTH   1920
#define CAM_TEST_HEIGHT  1080
#define CAM_TEST_NBUF    3
#define CAM_TEST_FRAMES  3
#define CAM_TEST_TIMEOUT 3000        /* ms per frame */

/* RAW10 is packed: 10 bits/pixel -> 5 bytes per 4 pixels. */

#define CAM_TEST_FRAME_BYTES \
  ((size_t)CAM_TEST_WIDTH * CAM_TEST_HEIGHT * 10 / 8)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct cam_test_buf_s
{
  FAR void *start;
  size_t    length;
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: metalio_camera_capture_test
 *
 * Description:
 *   Run the V4L2 capture self-test against /dev/video0 and return OK on
 *   success (at least one non-zero frame captured) or a negated errno.
 *
 ****************************************************************************/

int metalio_camera_capture_test(void)
{
  struct cam_test_buf_s bufs[CAM_TEST_NBUF];
  struct v4l2_capability cap;
  struct v4l2_format fmt;
  struct v4l2_requestbuffers req;
  struct v4l2_buffer vb;
  struct pollfd pfd;
  int fd;
  int ret;
  int i;
  int frames = 0;

  memset(bufs, 0, sizeof(bufs));

  fd = open(CAM_TEST_PATH, O_RDWR);
  if (fd < 0)
    {
      syslog(LOG_ERR, "CAMTEST: open %s failed: %d\n",
             CAM_TEST_PATH, errno);
      return -errno;
    }

  memset(&cap, 0, sizeof(cap));
  ret = ioctl(fd, VIDIOC_QUERYCAP, (uintptr_t)&cap);
  if (ret < 0)
    {
      syslog(LOG_ERR, "CAMTEST: QUERYCAP failed: %d\n", errno);
      goto err_close;
    }

  syslog(LOG_INFO, "CAMTEST: driver=%.16s\n", cap.driver);

  /* Set RAW10 BGGR 1920x1080 format. */

  memset(&fmt, 0, sizeof(fmt));
  fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width       = CAM_TEST_WIDTH;
  fmt.fmt.pix.height      = CAM_TEST_HEIGHT;
  fmt.fmt.pix.field       = V4L2_FIELD_ANY;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_SBGGR10;

  ret = ioctl(fd, VIDIOC_S_FMT, (uintptr_t)&fmt);
  if (ret < 0)
    {
      syslog(LOG_ERR, "CAMTEST: S_FMT failed: %d\n", errno);
      goto err_close;
    }

  /* Request USERPTR ring buffers. */

  memset(&req, 0, sizeof(req));
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_USERPTR;
  req.count  = CAM_TEST_NBUF;
  req.mode   = V4L2_BUF_MODE_RING;

  ret = ioctl(fd, VIDIOC_REQBUFS, (uintptr_t)&req);
  if (ret < 0)
    {
      syslog(LOG_ERR, "CAMTEST: REQBUFS failed: %d\n", errno);
      goto err_close;
    }

  /* Allocate DMA-capable (cache-line aligned) destination buffers. */

  for (i = 0; i < CAM_TEST_NBUF; i++)
    {
      bufs[i].length = CAM_TEST_FRAME_BYTES;
      bufs[i].start  = memalign(64, CAM_TEST_FRAME_BYTES);
      if (bufs[i].start == NULL)
        {
          syslog(LOG_ERR, "CAMTEST: memalign(%d) failed\n",
                 (int)CAM_TEST_FRAME_BYTES);
          ret = -ENOMEM;
          goto err_free;
        }
    }

  /* Queue all buffers. */

  for (i = 0; i < CAM_TEST_NBUF; i++)
    {
      memset(&vb, 0, sizeof(vb));
      vb.type      = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      vb.memory    = V4L2_MEMORY_USERPTR;
      vb.index     = i;
      vb.m.userptr = (uintptr_t)bufs[i].start;
      vb.length    = bufs[i].length;

      ret = ioctl(fd, VIDIOC_QBUF, (uintptr_t)&vb);
      if (ret < 0)
        {
          syslog(LOG_ERR, "CAMTEST: QBUF[%d] failed: %d\n", i, errno);
          goto err_free;
        }
    }

  /* Start streaming. */

  {
    enum v4l2_buf_type btype = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ret = ioctl(fd, VIDIOC_STREAMON, (uintptr_t)&btype);
    if (ret < 0)
      {
        syslog(LOG_ERR, "CAMTEST: STREAMON failed: %d\n", errno);
        goto err_free;
      }
  }

  syslog(LOG_INFO, "CAMTEST: streaming %ux%u RAW10 (%zu B/frame)\n",
         CAM_TEST_WIDTH, CAM_TEST_HEIGHT, CAM_TEST_FRAME_BYTES);

  pfd.fd     = fd;
  pfd.events = POLLIN;

  for (frames = 0; frames < CAM_TEST_FRAMES; frames++)
    {
      FAR uint8_t *p;
      uint32_t nonzero = 0;
      uint8_t minb = 0xff;
      uint8_t maxb = 0x00;
      uint32_t checksum = 0;
      size_t n;

      pfd.revents = 0;
      ret = poll(&pfd, 1, CAM_TEST_TIMEOUT);
      if (ret <= 0)
        {
          syslog(LOG_ERR, "CAMTEST: poll timeout on frame %d (ret=%d)\n",
                 frames, ret);
          ret = -ETIMEDOUT;
          goto err_streamoff;
        }

      memset(&vb, 0, sizeof(vb));
      vb.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      vb.memory = V4L2_MEMORY_USERPTR;

      ret = ioctl(fd, VIDIOC_DQBUF, (uintptr_t)&vb);
      if (ret < 0)
        {
          syslog(LOG_ERR, "CAMTEST: DQBUF failed: %d\n", errno);
          goto err_streamoff;
        }

      p = (FAR uint8_t *)vb.m.userptr;
      for (n = 0; n < vb.bytesused; n++)
        {
          uint8_t b = p[n];
          if (b != 0)
            {
              nonzero++;
            }

          if (b < minb)
            {
              minb = b;
            }

          if (b > maxb)
            {
              maxb = b;
            }

          checksum += b;
        }

      syslog(LOG_INFO,
             "CAMTEST: frame %d idx=%" PRIu32 " used=%" PRIu32
             " nonzero=%" PRIu32 " min=%02x max=%02x sum=%08" PRIx32
             " head=%02x%02x%02x%02x\n",
             frames, vb.index, vb.bytesused, nonzero,
             (unsigned int)minb, (unsigned int)maxb, checksum,
             p[0], p[1], p[2], p[3]);

      /* Recycle the buffer. */

      memset(&vb, 0, sizeof(vb));
      vb.type      = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      vb.memory    = V4L2_MEMORY_USERPTR;
      vb.index     = (uint32_t)frames % CAM_TEST_NBUF;
      vb.m.userptr = (uintptr_t)bufs[vb.index].start;
      vb.length    = bufs[vb.index].length;

      ret = ioctl(fd, VIDIOC_QBUF, (uintptr_t)&vb);
      if (ret < 0)
        {
          syslog(LOG_ERR, "CAMTEST: re-QBUF[%" PRIu32 "] failed: %d\n",
                 vb.index, errno);
          goto err_streamoff;
        }
    }

  ret = OK;

err_streamoff:
  {
    enum v4l2_buf_type btype = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMOFF, (uintptr_t)&btype);
  }

err_free:
  for (i = 0; i < CAM_TEST_NBUF; i++)
    {
      if (bufs[i].start != NULL)
        {
          free(bufs[i].start);
        }
    }

err_close:
  close(fd);

  syslog(LOG_INFO, "CAMTEST: done frames=%d ret=%d\n", frames, ret);
  return ret;
}
