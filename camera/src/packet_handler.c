

#include <stdio.h>
#include <assert.h>

#include <xcore/channel_streaming.h>

#include "packet_handler.h"
#include "image_vfilter.h"
#include "image_hfilter.h"
#include "user_api.h"
#include "utils.h"
#include "sensor.h"



// State needed for the vertical filter
static
vfilter_acc_t vfilter_accs[APP_IMAGE_CHANNEL_COUNT][VFILTER_ACC_COUNT];


// Contains the local state info for the packet handler thread.
static struct {
  unsigned wait_for_frame_start;
  unsigned frame_number;
  unsigned in_line_number;
  unsigned out_line_number;
} ph_state = {
  .wait_for_frame_start = 1, 
  .frame_number = 0,
  .in_line_number = 0,
  .out_line_number = 0,
};

hfilter_state_t hfilter_state[APP_IMAGE_CHANNEL_COUNT];

// Initial channel scales
#define AWB_gain_RED    1.3
#define AWB_gain_GREEN  0.8
#define AWB_gain_BLUE   1.3
// #define AWB_gain_RED    1.0
// #define AWB_gain_GREEN  1.0
// #define AWB_gain_BLUE   1.0

isp_params_t isp_params = {
  .channel_gain = {
    AWB_gain_RED,
    AWB_gain_GREEN,
    AWB_gain_BLUE
  }
};


static 
void handle_frame_start(
    const mipi_packet_t* pkt)
{
  // New frame is starting, reset the vertical filter accumulator states.
  for(int c = 0; c < APP_IMAGE_CHANNEL_COUNT; c++){
    pixel_hfilter_update_scale(&hfilter_state[c], 
                               isp_params.channel_gain[c], 
                               (c == 0)? 0 : 1);

    image_vfilter_frame_init(&vfilter_accs[c][0]);
  }

  camera_api_request_begin();
}


static
void handle_unknown_packet(
    const mipi_packet_t* pkt)
{
  //TODO: manage uknown packets
  // uknown packets could be the following:
  // 1 - sensor specific packets (this could be useful for having more information about the frame)
  // 2 - error packets (in this case mipi reciever will raise an exception, but in the future we want to handle them here)
}

#define HFILTER_INPUT_STRIDE  (APP_DECIMATION_FACTOR)

/**
 * Handle a row of pixel data.
 * 
 * This function handles horizontal and vertical decimation of the image data.
 * 
 * Returns true iff output_buffer[][] has been filled and is ready to be sent to
 * the next thread.
 */
static
unsigned handle_pixel_data(
    const mipi_packet_t* pkt,
    int8_t output_buffer[APP_IMAGE_CHANNEL_COUNT][APP_IMAGE_WIDTH_PIXELS])
{

  // Bayer pattern is RGGB; even index rows have RG data, 
  // odd index rows have GB data.
  unsigned pattern = ph_state.in_line_number % 2;

  // Temporary buffer to store horizontally-filtered row data. [1]
  int8_t hfilt_row[APP_IMAGE_WIDTH_PIXELS];
 
  if(pattern == 0){ // Packet contains RGRGRGRGRGRGRGRGRG...
    ////// RED
    pixel_hfilter(&hfilt_row[0],
                  (int8_t*) &pkt->payload[0],
                  &hfilter_state[CHAN_RED].coef[0],
                  hfilter_state[CHAN_RED].acc_init,
                  hfilter_state[CHAN_RED].shift,
                  HFILTER_INPUT_STRIDE,
                  APP_IMAGE_WIDTH_PIXELS);

    
    image_vfilter_process_row(&output_buffer[CHAN_RED][0],
                              &vfilter_accs[CHAN_RED][0],
                              &hfilt_row[0]);

    ////// GREEN
    pixel_hfilter(&hfilt_row[0],
                  (int8_t*) &pkt->payload[0],
                  &hfilter_state[CHAN_GREEN].coef[0],
                  hfilter_state[CHAN_GREEN].acc_init,
                  hfilter_state[CHAN_GREEN].shift,
                  HFILTER_INPUT_STRIDE,
                  APP_IMAGE_WIDTH_PIXELS);

    // we now it is not the las row [2]
    image_vfilter_process_row(&output_buffer[CHAN_GREEN][0],
                              &vfilter_accs[CHAN_GREEN][0],
                              &hfilt_row[0]);

  } 
  else { // Packet contains GBGBGBGBGBGBGBGBGBGB...
    ////// BLUE
    pixel_hfilter(&hfilt_row[0],
                  (int8_t*) &pkt->payload[0],
                  &hfilter_state[CHAN_BLUE].coef[0],
                  hfilter_state[CHAN_BLUE].acc_init,
                  hfilter_state[CHAN_BLUE].shift,
                  HFILTER_INPUT_STRIDE,
                  APP_IMAGE_WIDTH_PIXELS);

    unsigned new_row = image_vfilter_process_row(
                            &output_buffer[CHAN_BLUE][0],
                            &vfilter_accs[CHAN_BLUE][0],
                            &hfilt_row[0]);

    // If new_row is true, then the vertical decimator has output a new row for
    // each of the three color channels, and so we should signal this upwards.
    if(new_row){
      return 1;
    }
  }
  return 0;
}


static 
void on_new_output_row(
    const int8_t pix_out[APP_IMAGE_CHANNEL_COUNT][APP_IMAGE_WIDTH_PIXELS],
    streaming_chanend_t c_out_row)
{
  // Pass the output row along for statistics processing
  s_chan_out_word(c_out_row, (unsigned) &pix_out[0][0] );

  // Write image data to user buffer if there is one waiting
  camera_api_request_update(pix_out, ph_state.out_line_number);

  ph_state.out_line_number++;
}



static
void handle_frame_end(
    int8_t pix_out[APP_IMAGE_CHANNEL_COUNT][APP_IMAGE_WIDTH_PIXELS],
    const mipi_packet_t* pkt,
    streaming_chanend_t c_out_row)
{
  // Drain the vertical filter's accumulators
  image_vfilter_drain(&pix_out[CHAN_RED][0], &vfilter_accs[CHAN_RED][0]);
  image_vfilter_drain(&pix_out[CHAN_GREEN][0], &vfilter_accs[CHAN_GREEN][0]);
  if(image_vfilter_drain(&pix_out[CHAN_BLUE][0], &vfilter_accs[CHAN_BLUE][0])){
    // Pass final row(s) to the statistics thread
    on_new_output_row(pix_out, c_out_row);
  }

  // Signal statistics thread to do frame-end work by sending NULL.
  s_chan_out_word(c_out_row, (unsigned) NULL);

  // If user is waiting for image, this signals that it's done.
  camera_api_request_complete();

  // printf("\n");
  // printf("out lines: %u\n", ph_state.out_line_number);
  // printf("in lines: %u\n", ph_state.in_line_number);
}


void handle_no_expected_lines(){
      if(ph_state.in_line_number >= SENSOR_RAW_IMAGE_HEIGHT_PIXELS){
        // We've received more lines of image data than we expected.
        #ifdef ASSERT_ON_TOO_MANY_LINES
          assert(0);
        #endif
        }
}

// send the line number and the actual raw to the user api
void handle_expected_format_raw(const mipi_packet_t* pkt){
  camera_api_request_update_raw(
    (uint16_t) ph_state.in_line_number, 
    (uint8_t *) &pkt->payload[0]);
}

// end of frame
void handle_frame_end_raw(){
  camera_api_request_complete_raw();
}

/**
 * Process a single packet.
 * 
 * This function keeps track of where we are within the input and output image
 * frames. It also passes the packet along to a function for processing
 * depending upon the data type.
 */
static
void handle_packet(
    const mipi_packet_t* pkt,
    streaming_chanend_t c_out_row)
{

  /*
   * These buffers store rows of the fully decimated image. They are passed
   * along to the statistics thread once the packet handler thread no longer
   * needs them.
   *
   * Two are needed -- the one the decimator is currently filling, and the one
   * that the statistics thread is currently using.
   */
  __attribute__((aligned(8)))
  static int8_t output_buff[2][APP_IMAGE_CHANNEL_COUNT][APP_IMAGE_WIDTH_PIXELS];
  static unsigned out_dex = 0;


  // definitions
  const mipi_header_t header = pkt->header;
  const mipi_data_type_t data_type = MIPI_GET_DATA_TYPE(header);

  // At start-up we usually want to wait for a new frame before processing
  // anything
  if(ph_state.wait_for_frame_start 
     && data_type != MIPI_DT_FRAME_START) return;

  /*
    The idea here is that logic that keeps the packet handler in a coherent
    state, like tracking frame and line numbers, should go directly in here, but
    logic that actually interprets, processes or reacts to packet data should go
    into the individual functions.
  */
  switch(data_type)
  {
    case MIPI_DT_FRAME_START: 
      ph_state.wait_for_frame_start = 0;
      ph_state.in_line_number = 0;
      ph_state.out_line_number = 0;
      ph_state.frame_number++;

      handle_frame_start(pkt);   
      break;

    case MIPI_DT_FRAME_END:   
      handle_frame_end(output_buff[out_dex], pkt, c_out_row);
      out_dex = 1 - out_dex;
      break;

    case MIPI_EXPECTED_FORMAT:     
      handle_no_expected_lines();

      if(handle_pixel_data(pkt, output_buff[out_dex])){
        on_new_output_row(output_buff[out_dex], c_out_row);
        out_dex = 1 - out_dex;
      }

      ph_state.in_line_number++;
      break;

    default:              
        // We've received a packet we don't know how to interpret.
      handle_unknown_packet(pkt);   
      break;
  }
}


static
void handle_packet_raw(
    const mipi_packet_t* pkt,
    streaming_chanend_t c_out_row)
{

  // definitions
  const mipi_header_t header = pkt->header;
  const mipi_data_type_t data_type = MIPI_GET_DATA_TYPE(header);

  // At start-up we usually want to wait for a new frame before processing
  // anything
  if(ph_state.wait_for_frame_start 
     && data_type != MIPI_DT_FRAME_START) return;

  /*
    The idea here is that logic that keeps the packet handler in a coherent
    state, like tracking frame and line numbers, should go directly in here, but
    logic that actually interprets, processes or reacts to packet data should go
    into the individual functions.
  */
  switch(data_type)
  {
    case MIPI_DT_FRAME_START: 
      ph_state.wait_for_frame_start = 0;
      ph_state.in_line_number = 0;
      ph_state.out_line_number = 0;
      ph_state.frame_number++;
      break;

    case MIPI_DT_FRAME_END:   
      handle_frame_end_raw();
      break;

    case MIPI_EXPECTED_FORMAT:     
      handle_no_expected_lines();
      handle_expected_format_raw(pkt);

      ph_state.in_line_number++;
      break;

    default:              
      handle_unknown_packet(pkt);   
      break;
  }
}


/**
 * Top level of the packet handling thread. Receives MIPI packets from the
 * packet receiver and passes them to `handle_packet()` for parsing and
 * processing.
 */
void mipi_packet_handler(
    streaming_chanend_t c_pkt, 
    streaming_chanend_t c_ctrl,
    streaming_chanend_t c_out_row,
    streaming_chanend_t c_user_api)
{
  /*
   * These buffers will be used to hold received MIPI packets while they're
   * being processed.
   */
  __attribute__((aligned(8)))
  mipi_packet_t packet_buffer[MIPI_PKT_BUFFER_COUNT];
  unsigned pkt_idx = 0;

  camera_api_init(c_user_api);
  
  // Give the MIPI packet receiver a first buffer
  s_chan_out_word(c_pkt, (unsigned) &packet_buffer[pkt_idx] );

  while(1) {
    pkt_idx = (pkt_idx + 1) & (MIPI_PKT_BUFFER_COUNT-1);

    mipi_packet_t * pkt = (mipi_packet_t*) s_chan_in_word(c_pkt);
    // Swap buffers with the receiver thread. Give it the next buffer
    // to fill and take the last filled buffer from it.    
    s_chan_out_word(c_pkt, (unsigned) &packet_buffer[pkt_idx] );

    // Process the packet
    //const mipi_header_t header = pkt->header;
    //const mipi_data_type_t data_type = MIPI_GET_DATA_TYPE(header);

    // unsigned time_start = measure_time();
    handle_packet(pkt, c_out_row);
    // unsigned time_proc = measure_time() - time_start;

  }
}


/**
 * Top level of the packet handling thread. Receives MIPI packets from the
 * packet receiver and passes them to `handle_packet()` for parsing and
 * processing.
 */
void mipi_packet_handler_raw(
    streaming_chanend_t c_pkt, 
    streaming_chanend_t c_ctrl,
    streaming_chanend_t c_out_row,
    streaming_chanend_t c_user_api)
{
  /*
   * These buffers will be used to hold received MIPI packets while they're
   * being processed.
   */
  __attribute__((aligned(8)))
  mipi_packet_t packet_buffer[MIPI_PKT_BUFFER_COUNT];
  unsigned pkt_idx = 0;

  camera_api_init(c_user_api);
  
  // Give the MIPI packet receiver a first buffer
  s_chan_out_word(c_pkt, (unsigned) &packet_buffer[pkt_idx] );

  while(1) {
    pkt_idx = (pkt_idx + 1) & (MIPI_PKT_BUFFER_COUNT-1);

    mipi_packet_t * pkt = (mipi_packet_t*) s_chan_in_word(c_pkt);
    // Swap buffers with the receiver thread. Give it the next buffer
    // to fill and take the last filled buffer from it.    
    s_chan_out_word(c_pkt, (unsigned) &packet_buffer[pkt_idx] );

    // Process the packet
    //const mipi_header_t header = pkt->header;
    //const mipi_data_type_t data_type = MIPI_GET_DATA_TYPE(header);

    // unsigned time_start = measure_time();
    handle_packet_raw(pkt, c_out_row);
    // unsigned time_proc = measure_time() - time_start;
  }
}


// NOTES
//[1]
/*
  //  NOTE: If vertical filtering has to go into a separate thread, this buffer
  //  will need to be passed to another thread and must be done differently.
  //  NOTE: This buffer will only hold one color channel at a time because the
  //  horizontal filter separates color planes.

[2]
// We don't need to watch for new rows from the vertical decimator here
// because we know a priori that blue will be the last one out.

*/