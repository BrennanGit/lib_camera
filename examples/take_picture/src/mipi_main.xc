#include <xs1.h>
#include <print.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <platform.h>
#include <string.h>
#include <math.h>
#include "i2c.h"
#include "gc2145.h"
#include "mipi.h"
#include "debayer.h"
#include "yuv_to_rgb.h"

#include "mipi_main.h"
#include "extmem_data.h"

#include "mipi/MipiPacket.h"

// Start port declarations
/* Declaration of the MIPI interface ports:
 * Clock, receiver active, receiver data valid, and receiver data
 */
on tile[MIPI_TILE]:         in port    p_mipi_clk = XS1_PORT_1O;
on tile[MIPI_TILE]:         in port    p_mipi_rxa = XS1_PORT_1E;
on tile[MIPI_TILE]:         in port    p_mipi_rxv = XS1_PORT_1I;
on tile[MIPI_TILE]:buffered in port:32 p_mipi_rxd = XS1_PORT_8A;

on tile[MIPI_TILE]:clock               clk_mipi   = MIPI_CLKBLK;


/**
 * The packet buffer is where the packet decoupler will tell the MIPI receiver
 * thread to store received packets.
 */
// static
// uint8_t packet_buffer[MIPI_PKT_BUFFER_COUNT][MIPI_PKT_SIZE_BYTES] = {0};
static
mipi_packet_t packet_buffer[MIPI_PKT_BUFFER_COUNT];


/*
  Timing -- With GC2145

  astew: Using a 100 MHz timer, I found that the time between a start of frame
         packet and an end of frame packet is 4963948 ticks. I'll run it a 
         couple more times..
      
      4963948
      4963947
      4963948

        Consistent.

      - A frame takes: 49.63948 milliseconds.
      - There are 1200 lines/packets per frame
      - So that's about 41.366 microseconds per line
        - That includes horizontal blanking, so that is _NOT_ the time taken to
          TRANSFER each line. 
        - Instead, it is the total amount of time we have to PROCESS each line
          before we need to be ready for the next.
      - With 41.366us per line, that gives us about 5791 instructions to process
        the line
        - Assuming we're running at 700 MHz and getting a full 140 MIPS on this 
          core.
      - Each line contains 1600 pixels (packed into 2 bytes each)
      - That gives us............ 3.62 instructions per pixel.
        - ....... BUT! If we can crop it, we can increase this, potentially 
          greatly.
  
  It is suddenly clear to me that we need an application with the specific purpose of capturing timing information w.r.t. the image sensor's packet.

  Here's the idea:
    - Create an alternate version of the packet RX thread
    - This version takes a pointer to a buffer instead of two channels
    - This version doesn't actually save any pixel data
    - The buffer passed in is a uint32_t table with shape (N, 3)
    - Every time a packet header is received, it is placed in the first column
    - Immediately after receiving the packet header we get a timestamp of when
      the header was received.
    - Header timestamp goes in second column.
    - If the packet is a short packet, that timestamp also goes in the third 
      column.
    - If the packet is a long packet, it waits for the RxA signal to go low.
    - Once RxA goes low, an event fires, and we grab another timestamp.
    - That timestamp goes in the third column (for long packets)
    - Once we've filled up N rows of this table, return.
    - Dump the table contents to a file.
    - That dump tells us everything we need to know about that device's packet
      behavior and timing.
      
  Only real downside is that the app can't be generic -- it still needs to 
  configure the specific image sensor.


*/


/**
 * 
 */
#pragma unsafe arrays
static
void mipi_packet_handler(
    streaming chanend c_pkt, 
    streaming chanend c_ctrl)
{
    mipi_header_t mipiHeader;
    unsigned pkt_idx = 0;

    unsigned in_frame = 0;
    unsigned line_count = 0;
    unsigned byte_count = 0;

    timer tmr;
    unsigned start_time = 0;
    unsigned end_time = 0;


    unsafe {
      while(1) {
        
        // Advance buffer pointer to next empty buffer
        pkt_idx = (pkt_idx + 1) & (MIPI_PKT_BUFFER_COUNT-1);

        // Send pointer to a free buffer to the MIPI receiver thread 
        mipi_packet_t * unsafe pkt = &packet_buffer[pkt_idx];

        outuint((chanend) c_pkt, (unsigned) pkt);

        do { // We only need to switch to a new buffer on a long packet
          
          // MIPI receiver thread will send the packet header when packet has 
          // been fully received.
          mipiHeader = inuint((chanend) c_pkt);

          assert(mipiHeader == pkt->header);

          // If it's a long packet, break the loop
          if(MIPI_IS_LONG_PACKET(mipiHeader))
            break;

          // Otherwise, handle the short packet
          switch(MIPI_GET_DATA_TYPE(mipiHeader)){

            case MIPI_DT_FRAME_START:
            tmr :> start_time;
            in_frame = 1;
            break;

            case MIPI_DT_FRAME_END:
            tmr :> end_time;

            unsigned ticks = end_time - start_time;

            printf("Received EOF\n");
            printf("Total lines received: %u\n", line_count);
            printf("Total bytes received: %u\n", byte_count);
            printf("Tick count: %u\n", ticks);
            exit(0);
            break;
          }

        } while(1);

        // Handle the long packet
        line_count++;

        byte_count += MIPI_GET_WORD_COUNT(mipiHeader);
      }
    }
}

// typedef enum {
//   INIT,
//   INTRA_FRAME,
//   INTER_FRAME
// } proc_state_t;



// #pragma unsafe arrays
// void mipi_packet_processor(
//     chanend c_packet) 
// {  
//   unsigned line_count = 0;
//   unsigned frame_count  = 0;

//   proc_state_t state = INIT;

//   unsafe {
//     while(1) {
//       mipi_packet_t* unsafe packet;
//       unsigned meh;
//       select {
//         case inuint_byref(c_packet, meh):
//           packet = (mipi_packet_t* unsafe) meh;

//           mipi_counters.rx_packets++;

//           mipi_data_type_t data_type = MIPI_GET_DATA_TYPE(packet->header);
//           unsigned vchan = MIPI_GET_VIRTUAL_CHAN(packet->header);

//           if(data_type <= 0x0F){
//             // Short packet

//             if (data_type == MIPI_FRAMESTART) {

//               if(state == INTRA_FRAME){
//                 printf("ERROR: Received frame start packet while inside frame.\n");
//                 // exit(1);
//               } else { // INIT or INTER_FRAME
//                 state = INTRA_FRAME;
//                 frame_count++;
//                 line_count = 0;
//               }

//               printf("FS: %u\t%u\n", MIPI_GET_WORD_COUNT(packet->header), line_count);

//             } else if (data_type == MIPI_FRAMEEND) {
//               printf("FE\n");
              
//               // if(state == INTRA_FRAME){
//               //   write_image();
//               //   printf("Wrote image. (%u lines)\n", line_count);
//               //   delay_milliseconds(20);
//               //   exit(1);
//               // }

//               if(state == INIT) continue;

//               if(state == INTER_FRAME){
//                 printf("ERROR: Received frame end packet while between frames.\n");
//                 exit(1);
//               }

//               state = INTER_FRAME;

//               if (line_count != MIPI_IMAGE_HEIGHT_PIXELS) {
//                 printf("ERROR: Unexpected line_count. Expected %u; got %u\n", 
//                   MIPI_IMAGE_HEIGHT_PIXELS, line_count);
//                 // exit(1);
//               }

//               if(frame_count == 10){
//                 printf("Received 10 frames. Exiting...\n");
//                 exit(1);
//               }
              
//             } else if (data_type == MIPI_DT_LINE_START) {
//               // printf("LS\n");
//               if(state == INIT) continue;
//               if(state == INTER_FRAME){
//                 printf("ERROR: Received line start packet while between frames.\n");
//                 exit(1);
//               }
//             } else if (data_type == MIPI_DT_LINE_END) {
//               // printf("LE\n");
//               if(state == INIT) continue;
//               if(state == INTER_FRAME){
//                 printf("ERROR: Received line end packet while between frames.\n");
//                 exit(1);
//               }
//             } else if (data_type == MIPI_DT_EOT) {
//               // printf("EoT\n");
//             } else {
//               printf("Unknown short packet type: 0x%02X\n", (unsigned)data_type);
//             }
//           } else {
//               // Long packet

//             if(state == INTRA_FRAME){

//               if (data_type == MIPI_DT_RAW8) {
//                 line_count++;
//               } else if (data_type >= MIPI_DT_RGB444 && data_type <= MIPI_DT_RGB888) {
//                 static unsigned got_rgb = 0;
//                 if(!got_rgb){
//                   printf("GOT RGB!!!!\n");
//                   got_rgb = 1;
//                 }
//               } else if (data_type == MIPI_DT_YUV422_8BIT) {
//                 assert(MIPI_GET_WORD_COUNT(packet->header) == 3200);
//                 // not_silly_memcpy(
//                 //     &image_capture[line_count][0][0], 
//                 //     &packet->payload[0], 
//                 //     MIPI_LINE_WIDTH_BYTES);
//                 static unsigned got_yuv = 0;
//                 if(!got_yuv){
//                   printf("GOT YUV!!!!\n");
//                   got_yuv = 1;
//                 }
//                 line_count++;
//               } else { 
//                 printf("Unknown long packet type: 0x%02X\n", (unsigned)data_type);
//               }

//             }

//           }

//           break;
//       }
//     }
//   }
// }

#define DEMUX_DATATYPE 0
#define DEMUX_MODE     0x00     // no demux
#define DEMUX_EN       0

#define MIPI_CLK_DIV 1
#define MIPI_CFG_CLK_DIV 3

void mipi_main(
    client interface i2c_master_if i2c)
{
    streaming chan c_pkt;
    streaming chan c_ctrl;
    
    write_node_config_reg(tile[MIPI_TILE], 
        XS1_SSWITCH_MIPI_DPHY_CFG3_NUM , 0x7E42);

    MipiPacketRx_init(tile[MIPI_TILE],
                      p_mipi_rxd, p_mipi_rxv, p_mipi_rxa, p_mipi_clk, clk_mipi,
                      DEMUX_EN, DEMUX_DATATYPE, DEMUX_MODE,
                      MIPI_CLK_DIV, MIPI_CFG_CLK_DIV);
                      
    if(gc2145_init(i2c) != 0) {
        printf("GC2145 init failed\n");
    }
    
    gc2145_stream_start(i2c);

    par {
        MipiPacketRx2(p_mipi_rxd, p_mipi_rxa, c_pkt, c_ctrl);

        mipi_packet_handler(c_pkt, c_ctrl);

        // mipi_receiver(c_pkt_rx, p_mipi_rxd, p_mipi_rxa, c_kill);
        // mipi_packet_decoupler(c_pkt_rx, c_kill, c_pkt_proc);
        // mipi_packet_processor(c_pkt_proc);
    }
    
    i2c.shutdown();
}
