// std
#include <stdio.h>
// xcore
#include <xcore/select.h>
#include <xcore/channel_streaming.h>
// user
#include "mipi.h"
#include "utils.h"
#include "user_api.h"

// Pointers to both downsampled or raw
static image_t *user_image;
static int8_t  *image_raw_ptr; 

// In order to interface the handler and api
streaming_chanend_t c_user_api;

// ----------------------------------------------------------------
void camera_api_init(
    streaming_chanend_t c_api)
{
  user_image = NULL;
  c_user_api = c_api;
}

void camera_api_request_update(
    const int8_t image_row[CH][W],
    const unsigned row_index)
{
  if (user_image)
  {
    for (int k = 0; k < CH; k++)
    {
      c_memcpy((void*) &user_image->pix[k][row_index][0],
               (void*) &image_row[k][0],
               sizeof(int8_t) * W);
    }
  }
}

void camera_api_request_begin()
{
  unsigned tmp;
  SELECT_RES(
      CASE_THEN(c_user_api, user_handler),
      DEFAULT_THEN(default_handler))
    {
      user_handler:
        tmp = s_chan_in_word(c_user_api);
        user_image = (image_t*) tmp;
        break;
      default_handler:
        break;
    }
}

unsigned camera_capture_image(
    int8_t image_buff[CH][H][W],
    streaming_chanend_t c_cam_api)
{
  int8_t *p_image = &image_buff[0][0][0];

  s_chan_out_word(c_cam_api, (unsigned)p_image);
  return s_chan_in_word(c_cam_api);
}

void camera_api_request_complete()
{
  if(user_image){
    s_chan_out_word(c_user_api, 1);
    user_image = NULL;
  }
}


// ----------------------------------------------------------------
unsigned camera_capture_image_raw(
  int8_t image_buff[H_RAW*W_RAW],
  streaming_chanend_t c_cam_api
)
{
  image_raw_ptr = (int8_t*)&image_buff[0];
  c_user_api = c_cam_api;
  unsigned tmp = s_chan_in_word(c_cam_api);
  return tmp;
}

void camera_api_request_update_raw(uint16_t line_number, uint8_t* img_row_ptr){
  uint32_t pos = (line_number) * MIPI_LINE_WIDTH_BYTES;
  c_memcpy((void*) &image_raw_ptr[pos], 
         (void*) &img_row_ptr[0], 
         MIPI_LINE_WIDTH_BYTES); 
}

void camera_api_request_complete_raw(){
  s_chan_out_word(c_user_api, 1);
}