
#ifndef PACKET_HANDLER_H
#define PACKET_HANDLER_H

#include <stdint.h>

#include "xccompat.h"

#include "camera.h"

#ifdef __XC__
extern "C" {
#endif

/**
 * Represents a received MIPI packet.
 */
typedef struct
{
  mipi_header_t header;
  uint8_t payload[MIPI_MAX_PKT_SIZE_BYTES];
} mipi_packet_t;


// struct to hold the calculated parameters for the ISP
typedef struct {
  float channel_gain[APP_IMAGE_CHANNEL_COUNT];
} isp_params_t;


/**
 * isp_params_t instance owned by the packet handler.
 */
extern
isp_params_t isp_params;


/**
 * Thread entry point for packet handling when decimation and demosaicing are
 * desired.
 */
void mipi_packet_handler(
    streaming_chanend_t c_pkt, 
    streaming_chanend_t c_ctrl,
    streaming_chanend_t c_out_row);

// /**
//  * Thread entry point for packet handling when capturing raw data.
//  */
// void mipi_packet_handler_raw(
//     streaming_chanend_t c_pkt, 
//     streaming_chanend_t c_ctrl,
//     streaming_chanend_t c_out_row,
//     streaming_chanend_t c_user_api);
    
#ifdef __XC__
}
#endif

#endif // PACKET_HANDLER_H
