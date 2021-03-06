/*
 * INFORMATION ****************************************************
 */



#include "sender.h"




server_command_params_t unpackCommand(uint8_t* pkt)
{
        
    server_command_params_t command;
    command.receiptTime = millis();
    
    command.cmd = pkt[0];
    
    switch(command.cmd)
    {
        
        case CMD_STATUS:
            debug_log("SENDER: Got STATUS request.\r\n");
            memcpy(&command.timestamp,pkt+1,sizeof(unsigned long));
            memcpy(&command.ms,       pkt+5,sizeof(unsigned short));
            break;
        case CMD_STARTREC:
            debug_log("SENDER: Got STARTREC request.\r\n");
            memcpy(&command.timestamp,pkt+1,sizeof(unsigned long));     // get timestamp from packet
            memcpy(&command.ms,       pkt+5,sizeof(unsigned short));    // get milliseconds from packet
            memcpy(&command.timeout,  pkt+7,sizeof(unsigned short));    // get timeout from packet
            break;
        case CMD_ENDREC:
            debug_log("SENDER: Got ENDREC request.\r\n");
            break;
        case CMD_REQUNSENT:
            debug_log("SENDER: Got REQUNSENT request.\r\n");
            break;
        case CMD_REQSINCE:
            debug_log("SENDER: Got REQSINCE request.\r\n");
            memcpy(&command.timestamp,pkt+1,sizeof(unsigned long));
            memcpy(&command.ms,       pkt+5,sizeof(unsigned short));
            send.from = NO_CHUNK;
            send.bufContents = SENDBUF_EMPTY;
            send.loc = SEND_LOC_HEADER;
            break;
        case CMD_IDENTIFY:
            debug_log("SENDER: Got IDENTIFY request.\r\n");
            memcpy(&command.timeout,pkt+1,sizeof(unsigned short));
            break;
        default:
            debug_log("SENDER: Got INVALID request.\r\n");
            command.cmd = CMD_INVALID;
            break;
    }
    return command;
}


void sender_init()
{
    send.firstUnsent = 0;
    send.from = NO_CHUNK;
    send.source = SRC_FLASH;
    send.bufContents = SENDBUF_EMPTY;
    send.loc = SEND_LOC_HEADER;
    send.numSamples = 0;
    
    unsigned long earliestUnsentTime = MODERN_TIME;
    
    for(int c = 0; c <= LAST_FLASH_CHUNK; c++)
    {
        mic_chunk_t* chunkPtr = (mic_chunk_t*)ADDRESS_OF_CHUNK(c);

        unsigned long timestamp = (*chunkPtr).timestamp;
        unsigned long chunkCheck = (*chunkPtr).check;
        
        //debug_log("time: 0x%lX\r\n", timestamp);
        
        if (timestamp != 0xffffffffUL && timestamp > MODERN_TIME)  //is the timestamp possibly valid?
        { 
            if (timestamp == chunkCheck && chunkCheck != 0)  //is it a completely stored, but unsent, chunk?
            { 
                if (timestamp < earliestUnsentTime)  //is it earlier than the earliest one sent so far?
                { 
                    send.firstUnsent = c; //keep track of latest stored chunk
                    earliestUnsentTime = timestamp;
                }
            }
        }
        //nrf_delay_ms(50);
    }
    
    
    dateReceived = false;
    pendingCommand.cmd = CMD_NONE;   
}

bool updateSender()
{
    
    // This will be the function return value.  if there is any sending operations in-progress, this will be set to true.
    //   if not, it will return false, so that the main loop knows it can go to sleep early.
    bool senderActive = false;
    
    
    if(pendingCommand.cmd != CMD_NONE)
    {
        senderActive = true;  
        lastReceipt = millis();
        
        server_command_params_t command;
        command = pendingCommand;       // local copy, in case interrupt changes it.
        
        // ===================================================================
        // ======== Status request - send back packet of status info. ========
        if(command.cmd == CMD_STATUS)
        {
            // If the packet is already prepared, try sending it.
            if(send.bufContents == SENDBUF_STATUS)  // are we currently waiting to send status packet
            {
                if(BLEwrite(send.buf,send.bufSize))   // try sending packet
                {
                    send.bufContents = SENDBUF_EMPTY;   // buffer has been sent
                    
                    // Set badge internal timestamp
                    //   There might be a delay between command receipt and command handling, so account for that.
                    unsigned long msCorrection = millis() - command.receiptTime;
                    unsigned long sCorrection = 0;
                    while(msCorrection >= 1000UL)
                    {
                        msCorrection -= 1000;
                        sCorrection++;
                    }
                    debug_log("Setting time to %lX, %lums.\r\n",command.timestamp+sCorrection,command.ms+msCorrection);
                    setTimeFractional(command.timestamp+sCorrection,command.ms+msCorrection);
                    if(!dateReceived)
                    {
                        updateAdvData();
                        dateReceived = true;
                    }
                    
                    pendingCommand.cmd = CMD_NONE;      // we're done with that pending command
                    debug_log("SENDER: Sent status.\r\n");
                }
            }
            else    // otherwise prepare status packet
            {
                send.buf[0] = (dateReceived) ? 1 : 0;
                send.buf[1] = 1;  // UNSENT DATA READY
                send.buf[2] = (isCollecting) ? 1 : 0;  // COLLECTING DATA
                
                // Reply with onboard timestamp (0 if none set)
                unsigned long timestamp = 0;
                unsigned short ms = 0;
                if(dateReceived)
                {
                    timestamp = now();
                    ms = nowFractional();
                }
                memcpy(send.buf+3,&timestamp,sizeof(unsigned long));
                memcpy(send.buf+7,&ms,sizeof(unsigned short));
                
                float voltage = getBatteryVoltage();
                memcpy(send.buf+9,&voltage,sizeof(float));
                
                send.bufContents = SENDBUF_STATUS;
                send.bufSize = SENDBUF_STATUS_SIZE;
                
                /*if(!collecting)
                {
                    startCollector();  // !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! TEMPORARY UNTIL CMD_STARTREC IS IMPLEMENTED
                }*/
            }
        }
        
        // ==========================================
        // ========  Data-sending commands.  ========
        else if(command.cmd == CMD_REQUNSENT || command.cmd == CMD_REQSINCE)
        {
            // --------------------------------
            // ----- initializing sending -----
            // If send.from isn't set, then we just got the request, and we need to find where to start sending from
            if(send.from == NO_CHUNK)
            {
                // If request is REQSINCE, we need to find the first chunk that includes the requested timestamp.
                if(command.cmd == CMD_REQSINCE)
                {
                    // Walk back from most recent data till we find a chunk that includes the requested timestamp
                    
                    // if nothing else, send the in-progress chunk
                    send.from = collect.to;
                    send.source = SRC_REALTIME;
                    
                    // look for potential chunks, in RAM first, starting right before current collector chunk
                    int latestRAMchunk = (collect.to > 0) ? collect.to-1 : LAST_RAM_CHUNK;
                    // advance through all RAM chunks except current collecting chunk
                    for(int c=latestRAMchunk; c != collect.to; c = (c > 0) ? c-1 : LAST_RAM_CHUNK)
                    {   
                        unsigned long timestamp = micBuffer[c].timestamp;
                        unsigned long check = micBuffer[c].check;
                        
                        // Check to see if the candidate chunk is one we should send
                        if(check == timestamp || check == CHECK_TRUNC)      // is it a valid RAM chunk?
                        {
                            if(timestamp > MODERN_TIME && timestamp < FUTURE_TIME)   // is the timestamp valid?
                            {
                                if(timestamp >= command.timestamp)  // is the chunk within the requested time?
                                {
                                    send.from = c;  // potentially start sending from this chunk (need to check others still)
                                    send.source = SRC_RAM;
                                }
                                else      // if RAM chunk is earlier than requested time, we're done looking in RAM
                                {
                                    break;
                                }
                            }
                        }
                    }
                    
                    // send.from is now the earliest chunk in RAM that we should send.
                    //   There might be earlier relevant chunks in FLASH though.
                    // FLASH might be only partly filled - many invalid chunks.  If we see a few in a row, then we're
                    //   probably at the end of valid data in FLASH, so we should stop looking.  keep track with below variable
                    int invalid = 0;  // counts how many invalid chunks we see in a row, when we encounter any.
                    
                    // look through FLASH, from latest stored chunk (one chunk before store.to)
                    int latestFLASHchunk = (store.to > 0) ? store.to-1 : LAST_FLASH_CHUNK;
                    // advance through all FLASH chunks except current storing chunk
                    for(int c=latestFLASHchunk; c != store.to; c = (c > 0) ? c-1 : LAST_FLASH_CHUNK)
                    {
                        mic_chunk_t* chunkPtr = (mic_chunk_t*)ADDRESS_OF_CHUNK(c);
                        unsigned long timestamp = (*chunkPtr).timestamp;
                        unsigned long check = (*chunkPtr).check;
                        
                        // is it a valid chunk (check == timestamp and timestamp is valid, OR check is a special value)
                        if((check == timestamp && timestamp > MODERN_TIME && timestamp < FUTURE_TIME)
                            || check == CHECK_TRUNC || check == CHECK_SENT || check == CHECK_TRUNC_SENT)
                        {
                            invalid = 0;  // reset counter of sequential invalid chunks
                            
                            if(timestamp >= command.timestamp)  // is the chunk within the requested time?
                            {  
                                send.from = c;  // potentially start sending from this chunk (need to check others still)
                                send.source = SRC_FLASH;
                            }
                            else      // if RAM chunk is earlier than requested time, we're done looking in FLASH
                            {
                                break;
                            }
                        }
                        else
                        {
                            invalid++;
                            if(invalid > 5)     // If we have seen a few invalid chunks in a row, we should stop looking.
                            {
                                break;          // Stop looking, we've found the earliest data available.
                            }
                        }
                    } 
                }    // if(command.cmd == CMD_REQSINCE)
                
                // If request is REQUNSENT, just send from the last sent chunk.
                else
                {
                    //send.from = send.firstUnsent;    NOT IMPLEMENTED YET
                    debug_log("ERR: CMD_REQUNSENT unimplemented.\r\n");
                    pendingCommand.cmd = CMD_NONE;
                }
                
                debug_log("SENDER: sending since: s:%c c:%d\r\n",
                                            (send.source==SRC_FLASH) ? 'F' : ((send.source==SRC_RAM)?'R':'C'),
                                            send.from);
                
                send.loc = SEND_LOC_HEADER;  // we'll need to send a header first
                
            }   // if(send.from == NO_CHUNK)
            
            // -----------------------------
            // ----- executing sending -----
            // If send.from is set, then we're actually sending data.
            else
            {
                // -- packet already prepared
                // If a data packet is already prepared, try sending it.  Following actions depend on what was sent.
                if(send.bufContents == SENDBUF_HEADER || send.bufContents == SENDBUF_SAMPLES || send.bufContents == SENDBUF_END)
                {
                    if(BLEwrite(send.buf,send.bufSize))
                    {
                        switch(send.bufContents)
                        {
                            case SENDBUF_HEADER:
                                // If we finished sending a chunk header, we can start sending the chunk data.
                                send.loc = 0;  // start sending data from beginning of chunk
                                break;
                            case SENDBUF_SAMPLES:
                                // If we finished sending a packet of data, we need to advance through the chunk's sample buffer
                                send.loc += send.bufSize;  // increment loc by number of samples we just sent
                                
                                // If we reached the end of the chunk, we need to advance to the next chunk (if there is one ready)
                                if(send.loc >= send.numSamples)
                                {
                                    debug_log("SENDER: sent s:%c c:%d n:%d\r\n",
                                            (send.source==SRC_FLASH) ? 'F' : ((send.source==SRC_RAM)?'R':'C'),
                                            send.from, send.numSamples);
                                            
                                    // advance to next chunk
                                    switch(send.source)
                                    {
                                        case SRC_FLASH:
                                            // look for another unsent FLASH chunk
                                            do
                                            {
                                                // increment to next FLASH chunk
                                                send.from = (send.from < LAST_FLASH_CHUNK) ? send.from+1 : 0;
                                                
                                                mic_chunk_t* chunkPtr = (mic_chunk_t*)ADDRESS_OF_CHUNK(send.from);
                                                unsigned long timestamp = (*chunkPtr).timestamp;
                                                unsigned long check = (*chunkPtr).check;
                        
                                                // is it a valid chunk 
                                                //   (check == timestamp and timestamp is valid, OR check is a special value)
                                                if((check == timestamp && timestamp > MODERN_TIME && timestamp < FUTURE_TIME)
                                                    || check == CHECK_TRUNC || check == CHECK_SENT || check == CHECK_TRUNC_SENT)
                                                {
                                                    break;  // from while(send.from != store.to)
                                                }
                                                // If chunk isn't valid, we need to keep looking
                                            } while(send.from != store.to);    
                                            
                                            // If send.from is earlier than store.to, then we have more FLASH chunks to send.
                                            if(send.from != store.to)
                                            {
                                                // send.from is next FLASH chunk to be sent
                                                break;  // from switch(send.source)
                                            }
                                            
                                            // Else we need to look through RAM next.  Switch to RAM:
                                            send.source = SRC_RAM;
                                            send.from = collect.to;
                                            // and advance to first RAM chunk (below)
                                            // Fall through:
                                        case SRC_RAM:
                                            // look for another unsent RAM chunk
                                            do
                                            {
                                                // increment to next RAM chunk
                                                send.from = (send.from < LAST_RAM_CHUNK) ? send.from+1: 0;
                                                
                                                unsigned long timestamp = micBuffer[send.from].timestamp;
                                                unsigned long check = micBuffer[send.from].check;
                                                
                                                // is it a valid chunk 
                                                //   (check == timestamp and timestamp is valid, OR check is a special value)
                                                if((check == timestamp && timestamp > MODERN_TIME && timestamp < FUTURE_TIME)
                                                    || check == CHECK_TRUNC)
                                                {
                                                    break;  // from switch(send.source)
                                                }
                                                
                                                // If chunk isn't valid, we need to keep looking
                                            } while(send.from != collect.to);
                                            
                                            // If send.from hasn't wrapped around to collect.to yet, we have more RAM chunks to send
                                            if(send.from != collect.to)
                                            {
                                                // send.from is next RAM chunk to be sent
                                                break;  // from switch(send.source)
                                            }
                                            
                                            // Else we need to send the real-time (incomplete) chunk
                                            
                                            send.source = SRC_REALTIME;
                                            
                                            if(collect.loc > 0)  // have any samples been collected into the real-time chunk
                                            {
                                                // send.from is collect.to, the real-time chunk
                                                break;  // from switch(send.source)
                                            }
                                            
                                            // If collect.loc == 0, then there's no data in the real-time chunk yet, and we're done.
                                            // Fall through:
                                        case SRC_REALTIME:
                                            // if we've sent the realtime data, we're all done, and should send a null header
                                            send.from = SEND_FROM_END;
                                            break;
                                            
                                        default:
                                            break;
                                    }   // switch(send.source)
                                    
                                    send.loc = SEND_LOC_HEADER;  // need to send header of next chunk first
                                }  // if(send.loc >= send.numSamples)
                                // Else we need to send another data packet in this chunk
                                break;
                            case SENDBUF_END:
                                debug_log("SENDER: sent null header.  Sending complete.\r\n");
                                pendingCommand.cmd = CMD_NONE;  // if we sent the terminating null header, we're done sending data
                                senderActive = false;   // all done with sending, updateSender will return this (false)
                                break;
                            default:
                                break;
                        }   // switch(send.bufContents)
                        
                        
                        send.bufContents = SENDBUF_EMPTY;
                    }   //if(BLEwrite(send.buf,send.bufSize))
                    
                }   // if(data packet was already prepared)
                
                // -- else need to prepare packet 
                // If the send packet buffer is empty, we need to fill it.
                else
                {
                    // If we sent all the data, we need to send an empty header to terminate
                    if(send.from == SEND_FROM_END)      // terminating null header
                    {
                        memset(send.buf,0,SENDBUF_HEADER_SIZE);  // clear send buffer to send null header
                        send.bufSize = SENDBUF_HEADER_SIZE;
                        send.bufContents = SENDBUF_END;
                    }
                    
                    // Else there's data to be sent
                    else
                    {
                    
                        // Get pointer to current chunk
                        mic_chunk_t* chunkPtr = &micBuffer[send.from];
                        switch(send.source)
                        {
                            case SRC_FLASH:
                                chunkPtr = (mic_chunk_t*)ADDRESS_OF_CHUNK(send.from);
                                break;
                            case SRC_RAM:
                            case SRC_REALTIME:
                                chunkPtr = &micBuffer[send.from];
                                break;
                            default:
                                break;
                        }
                        
                        if(send.loc == SEND_LOC_HEADER)
                        {
                            // Compose header
                            memcpy(send.buf,    &((*chunkPtr).timestamp),        sizeof(unsigned long));   // timestamp
                            memcpy(send.buf+4,  &((*chunkPtr).msTimestamp),      sizeof(unsigned short));  // timestamp ms
                            memcpy(send.buf+6,  &((*chunkPtr).battery),          sizeof(float));           // battery voltage
                            
                            unsigned short period = samplePeriod;  // cast to unsigned short
                            memcpy(send.buf+10, &period, sizeof(unsigned short));  // sample period ms
                            
                            if(send.source == SRC_REALTIME)
                            {
                                send.numSamples = collect.loc;  // all collected samples so far
                            }
                            else if((*chunkPtr).check == CHECK_TRUNC || (*chunkPtr).check == CHECK_TRUNC_SENT)
                            {
                                send.numSamples = (*chunkPtr).samples[SAMPLES_PER_CHUNK-1];  // last byte of sample array is number
                                                                                             //   of samples in truncated chunk
                            }
                            else
                            {
                                send.numSamples = SAMPLES_PER_CHUNK;  // full chunk
                            }
                            
                            unsigned short num = send.numSamples;  // cast to unsigned short
                            memcpy(send.buf+12, &num, sizeof(unsigned short));  // number of samples
                            
                            send.bufContents = SENDBUF_HEADER;
                            send.bufSize = SENDBUF_HEADER_SIZE;
                        }
                        
                        else    // else we're sending data
                        {
                            // compose next packet of data
                            
                            int samplesLeft = send.numSamples - send.loc;
                            // Must send 20 or fewer samples at a time.
                            if(samplesLeft > SAMPLES_PER_PACKET)
                            {
                                send.bufSize = SAMPLES_PER_PACKET;
                            }
                            else
                            {
                                send.bufSize = samplesLeft;
                            }
                            memcpy(send.buf, (*chunkPtr).samples + send.loc, send.bufSize);  // fill buffer with samples
                            send.bufContents = SENDBUF_SAMPLES;
                        }
                        
                    }
                    
                    
                    
                            
                }
                
            }
            
        }  // else if(command.cmd == CMD_REQUNSENT || command.cmd == CMD_REQSINCE)
        
        else if(command.cmd == CMD_STARTREC)
        {
            // If the packet is already prepared, try sending it.
            if(send.bufContents == SENDBUF_TIMESTAMP)  // are we currently waiting to send status packet
            {
                if(BLEwrite(send.buf,send.bufSize))   // try sending packet
                {
                    send.bufContents = SENDBUF_EMPTY;   // buffer has been sent
                    
                    
                    // Set badge internal timestamp
                    //   There might be a delay between command receipt and command handling, so account for that.
                    unsigned long msCorrection = millis() - command.receiptTime;
                    unsigned long sCorrection = 0;
                    while(msCorrection >= 1000UL)
                    {
                        msCorrection -= 1000;
                        sCorrection++;
                    }
                    debug_log("Setting time to %lX, %lums.\r\n",command.timestamp+sCorrection,command.ms+msCorrection);
                    setTimeFractional(command.timestamp+sCorrection,command.ms+msCorrection);
                    
                    if(!dateReceived)
                    {
                        updateAdvData();
                        dateReceived = true;
                    }

                    // Timeout value expressed as minutes - convert to ms.
                    debug_log("SENDER: starting collector, timeout %d minutes.\r\n",(int)command.timeout);
                    collectorTimeout = ((unsigned long)command.timeout) * 60UL * 1000UL;
                    startCollector();
                    pendingCommand.cmd = CMD_NONE;
                }
            }
            else    // otherwise prepare timestamp packet
            {
                unsigned long timestamp = 0;
                unsigned short ms = 0;
                if(dateReceived)
                {
                    timestamp = now();
                    ms = nowFractional();
                }
            
                memcpy(send.buf,&timestamp,sizeof(unsigned long));
                memcpy(send.buf+4,&ms,sizeof(unsigned short));
                
                send.bufContents = SENDBUF_TIMESTAMP;
                send.bufSize = SENDBUF_TIMESTAMP_SIZE;
            }
            
        }
        
        else if(command.cmd == CMD_ENDREC)
        {
            debug_log("SENDER: stopping collector.\r\n");
            stopCollector();
            pendingCommand.cmd = CMD_NONE;
        }
        
        else if(command.cmd == CMD_IDENTIFY)
        {
            if(command.timeout == 0)
            {
                led_timeout_cancel();
                nrf_gpio_pin_write(LED_2,0);   // clunky - sender.c doesn't see LED_OFF define
                debug_log("SENDER: LED off.\r\n");
            }
        
            else 
            {
                if(command.timeout > 30) command.timeout = 30;  // clip to 30seconds
                unsigned long timeout_ms = ((unsigned long)command.timeout) * 1000UL;
                led_timeout_set(timeout_ms);
                nrf_gpio_pin_write(LED_2,1); // clunky - sender.c doesn't see LED_ON define
                debug_log("SENDER: LED on for %ds.\r\n",command.timeout);
            }
            pendingCommand.cmd = CMD_NONE;
        }
        
        else if(command.cmd == CMD_INVALID)
        {
            pendingCommand.cmd = CMD_NONE;
        }
        
        
        
        
    }   //if(pendingCommand.cmd != CMD_NONE)
    
    
    // Collector timeout.  Stop collector if server is unseen for a long time
    if(collectorTimeout > 0)  // 0 means timeout disabled
    {
        if(isCollecting && (millis() - lastReceipt >= collectorTimeout))
        {
            debug_log("SENDER: collector timeout.  Stopping collector...\r\n");
            stopCollector();
        }
    }
    
    
    
    return senderActive;
}


/*void BLEonReceive(ble_nus_t * p_nus, uint8_t * p_data, uint16_t length) 
{
    if(length > 0)
    {
        pendingCommand = unpackCommand(p_data);
    }
}*/