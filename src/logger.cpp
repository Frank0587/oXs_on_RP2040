#include <stdio.h>
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "hardware/dma.h"

#include "tools.h"
#include "logger.h"
#include "param.h"

#include "uart_logger_tx.pio.h"
#include "sbus_out_pwm.h"


// to log the data, we use
// - 2 buffers to store the data to be sent via a PIO UART
// - a variable that says the index of the buffer used to write new data
// - a uart pio: we use PIO 1 SM 2
// - a dma to transfert the data from one buffer to the PIO
// - a push function to fill the data in the buffer

// On core 0, in the loop handling telemetry data generated by core 1:
//    if there are data to process, we write in the buffer the header, the LSB of millis(), one data with full timestamp if MSB changed
//    then for each received data, we push the data (type and compressed value) to the buffer
// in the function that write to the buffer;
//    we do byte stuffing(if requested) and write one byte in the buffer
//    after each byte being written, we check if there is still place;
//       if not, we wait that dma has finished,(or give an error)
//       we start new dma (for filled buffer),
//       we change the current buffer (and reset write pointer) 




extern CONFIG config;
#define LOG_BUFFER_LEN 512 // to do change to 512
uint8_t logBuffer[2][LOG_BUFFER_LEN]; // 2 log buffers; this could be increased if UART is to slow and block other process
uint16_t logLen= 0;               // number of char in writing buffer
uint8_t logWritingBuffer = 0;         // buffer being currently use to write new data.

PIO loggerPio = pio1; // we use pio 1; DMA is hardcoded to use it
uint loggerSmTx = 2;  // we use the state machine 2 for Tx; DMA is harcoded to use it (DREQ) 

int logger_dma_chan;
dma_channel_config loggerDmaConfig;


LOGGER::LOGGER() {}


// setup logger
void LOGGER::begin(void ) {
    
    // init PIO    
    if ( pio_can_add_program(loggerPio, &uart_logger_tx_program) == false){
        printf("error : can not upload logger PIO\n");
        return;
    } 
    uint  loggerOffsetTx = pio_add_program(loggerPio, &uart_logger_tx_program); // upload the program
    uart_logger_tx_program_init(loggerPio, loggerSmTx, loggerOffsetTx, config.pinLogger, config.loggerBaudrate);
    pio_sm_set_enabled(loggerPio, loggerSmTx,true); // Start the sm

    // init DMA but no yet start it.
    logger_dma_chan = dma_claim_unused_channel(true);
    loggerDmaConfig = dma_channel_get_default_config(logger_dma_chan);
    channel_config_set_read_increment(&loggerDmaConfig, true);
    channel_config_set_write_increment(&loggerDmaConfig, false);
    channel_config_set_dreq(&loggerDmaConfig, DREQ_PIO1_TX2);  // use PIO 1 and state machine 2 
    channel_config_set_transfer_data_size(&loggerDmaConfig, DMA_SIZE_8);
    dma_channel_configure(
        logger_dma_chan,
        &loggerDmaConfig,
        &pio1_hw->txf[loggerSmTx], // Write address (only need to set this once) of pio1 sm2
        &logBuffer[0][0],   // we start using the first buffer             
        LOG_BUFFER_LEN , // do not yet provide the number of bytes (DMA cycles)
        false             // Don't start yet
    );

} // end begin

uint32_t lastLogMillis = 0;
#define MAX_LOG_INTERVAL_MS 1000 // max interval between 2 log packet

void LOGGER::logByteNoStuff(uint8_t c){
    // write the byte to current buffer
    // if the buffer is full, start the DMA to send it via pio uart and switch to the other buffer
    logBuffer[logWritingBuffer][logLen++] = c;
    //printf("logLen = %i\n", logLen);
    // when one buffer is full or once every second
    if ( (logLen >=LOG_BUFFER_LEN) || (( millisRp() - lastLogMillis) > MAX_LOG_INTERVAL_MS) ){
        //printf("a log buffer is full\n");
        //if (dma_channel_is_busy(logger_dma_chan)) {
        //    printf("log dma is busy\n");
        //} else {
        //    printf("log dma is not busy\n");
        //}
        while (dma_channel_is_busy(logger_dma_chan)) ; // wait that dma of the other buffer is done
        lastLogMillis = millisRp();                    // reset the timestamp  
        // set read address of dma and start it
        //printf("sending by DMA buffer %i\n",logWritingBuffer );
        dma_channel_set_read_addr (logger_dma_chan, &logBuffer[logWritingBuffer][0], false);
        dma_channel_set_trans_count (logger_dma_chan, logLen , true) ; // in principe DMA could already be started when changing the read address
        logWritingBuffer++;
        if (logWritingBuffer > 1) logWritingBuffer = 0;
        logLen = 0;
    }       
}

void LOGGER::logBytewithStuff(uint8_t c){
    if (c == 0x7E) {
        logByteNoStuff(0x7D);
        logByteNoStuff(0x5E);
    } else if (c == 0x7D) {
        logByteNoStuff(0x7D);
        logByteNoStuff(0x5D);
    } else {
      logByteNoStuff(c);
    }
}

void LOGGER::logint32withStuff(uint8_t type ,int32_t value){
    if ((value & 0XFFFFFF00) == 0) { // if only one byte to send for value
        logBytewithStuff( type  | 0XC0);
        logBytewithStuff( (uint8_t) value);
    } else if ((value & 0XFFFF0000) == 0) { //2 bytes to send
        logBytewithStuff( type | 0X80);
        logBytewithStuff( (uint8_t) (value >> 8));
        logBytewithStuff( (uint8_t) value );
    } else if ( (value & 0XFF000000) == 0) { //3 bytes to send
        logBytewithStuff( type | 0X40);
        logBytewithStuff( (uint8_t) (value >> 16));
        logBytewithStuff( (uint8_t) (value >> 8));
        logBytewithStuff( (uint8_t) value );
    } else  {                          //4 bytes to send
        logBytewithStuff( type );
        logBytewithStuff( (uint8_t) (value >> 24));
        logBytewithStuff( (uint8_t) (value >> 16));
        logBytewithStuff( (uint8_t) (value >> 8));
        logBytewithStuff( (uint8_t) value );
    } 
}

//extern uint16_t rcSbusOutChannels[16];
extern uint16_t rcChannelsUs[16]; // Rc channels from receiver in Us

extern uint32_t lastRcChannels;


void LOGGER::logAllRcChannels(){   // log all 16 rc channels 
// format is 0X7E + 40 (= type of rc channels) + 16 X a uint16 for each RC channel in usec
    if (lastRcChannels == 0) return;   // skip when we do not yet have Rc channels
    logByteNoStuff(0X7E);
    logTimestampMs(millisRp());
    logByteNoStuff(40);
    //uint16_t pwmUsec ;
    for (uint8_t i=0;i<16; i++){
        //pwmUsec = fmap( rcSbusOutChannels[i]  );
        //logBytewithStuff( (uint8_t) (pwmUsec >> 8));
        //logBytewithStuff( (uint8_t) pwmUsec );
        logBytewithStuff( (uint8_t) (rcChannelsUs[i] >> 8));
        logBytewithStuff( (uint8_t) rcChannelsUs[i] );
        
    }
}

void LOGGER::logTimestampMs(uint32_t value){
    logBytewithStuff( (uint8_t) (value >> 24));
    logBytewithStuff( (uint8_t) (value >> 16));
    logBytewithStuff( (uint8_t) (value >> 8));
    logBytewithStuff( (uint8_t) value );
}