static BOOL is_duplicate_fragment(ClientData *client, uint32_t offset) {
    return client->inc_message[0].bitmap &&
           check_fragment_received(client->inc_message[0].bitmap, offset, (uint32_t)TEXT_FRAGMENT_SIZE);
}

static BOOL is_message_already_received(uint32_t message_id, uint32_t session_id) {
    return search_message_id(hash_message_id, message_id, session_id);
}

static BOOL is_fragment_offset_valid(uint32_t offset, uint32_t len, uint32_t total_len) {
    return offset < total_len && (offset + len) <= total_len;
}

static void register_ack(ClientData *client, UdpFrame *frame, uint8_t op_code) {
    QueueSeqNumEntry entry = {
        .seq_num = ntohll(frame->header.seq_num),
        .op_code = op_code,
        .session_id = ntohl(frame->header.session_id)
    };
    memcpy(&entry.addr, &client->addr, sizeof(struct sockaddr_in));
    push_seq_num(&queue_seq_num, &entry);
}

static void allocate_message_buffers(ClientData *client, uint32_t message_len) {
    MessageState *msg = &client->inc_message[0];
    msg->fragment_count = (message_len + TEXT_FRAGMENT_SIZE - 1) / TEXT_FRAGMENT_SIZE;
    msg->bitmap_entries_count = (msg->fragment_count + 31) / 32;

    msg->bitmap = calloc(msg->bitmap_entries_count, sizeof(uint32_t));
    msg->buffer = calloc(message_len, sizeof(char));
}

static void finalize_message(ClientData *client, uint32_t message_len) {
    MessageState *msg = &client->inc_message[0];
    msg->buffer[message_len] = '\0';
    snprintf(client->message_file_name, PATH_SIZE, "E:\\message%d_%d.txt", client->message_file_count++, client->session_id);
    if (create_output_file(msg->buffer, msg->bytes_received, client->message_file_name) == RET_VAL_SUCCESS) {
        free(msg->buffer);
        free(msg->bitmap);
        msg->buffer = NULL;
        msg->bitmap = NULL;
    }
}

int process_message_fragment_frame(ClientData *client, UdpFrame *frame) {
    uint32_t msg_id = ntohl(frame->payload.long_text_msg.message_id);
    uint32_t msg_len = ntohl(frame->payload.long_text_msg.message_len);
    uint32_t frag_len = ntohl(frame->payload.long_text_msg.fragment_len);
    uint32_t offset = ntohl(frame->payload.long_text_msg.fragment_offset);

    if (is_duplicate_fragment(client, offset)) {
        send_ack_or_status(client, frame, ERR_DUPLICATE_FRAME);
        return RET_VAL_ERROR;
    }

    if (is_message_already_received(msg_id, client->session_id)) {
        send_ack_or_status(client, frame, STS_TRANSFER_COMPLETE);
        return RET_VAL_ERROR;
    }

    if (!is_fragment_offset_valid(offset, frag_len, msg_len)) {
        fprintf(stderr, "Invalid fragment offset: %u (len: %u, total: %u)\n", offset, frag_len, msg_len);
        return RET_VAL_ERROR;
    }

    MessageState *msg = &client->inc_message[0];
    if (msg->id != msg_id) {
        allocate_message_buffers(client, msg_len);
        msg->id = msg_id;
        insert_message_id(hash_message_id, msg_id, client->session_id);
        print_message_id_hash(hash_message_id);
    }

    memcpy(msg->buffer + offset, frame->payload.long_text_msg.fragment_text, frag_len);
    msg->bytes_received += frag_len;
    mark_fragment_received(msg->bitmap, offset, (uint32_t)TEXT_FRAGMENT_SIZE);

    if (msg->bytes_received == msg_len && check_bitmap(msg->bitmap, msg->fragment_count)) {
        finalize_message(client, msg_len);
    }

    return RET_VAL_SUCCESS;
}














static BOOL is_fragment_duplicate(ClientData *client, uint32_t offset) {
    return client->inc_message[0].bitmap &&
           client->inc_message[0].buffer &&
           check_fragment_received(client->inc_message[0].bitmap, offset, (uint32_t)TEXT_FRAGMENT_SIZE);
}

static void log_fragment_issue(const char *msg, ClientData *client, uint32_t msg_id, uint32_t offset, uint32_t len) {
    fprintf(stderr, "%s - Session ID: %d, Message ID: %d, Fragment Offset: %d, Fragment Length: %d\n",
            msg, client->session_id, msg_id, offset, len);
}

static int handle_fragment_validation(ClientData *client, UdpFrame *frame) {
    uint32_t msg_id = ntohl(frame->payload.long_text_msg.message_id);
    uint32_t msg_len = ntohl(frame->payload.long_text_msg.message_len);
    uint32_t frag_len = ntohl(frame->payload.long_text_msg.fragment_len);
    uint32_t offset   = ntohl(frame->payload.long_text_msg.fragment_offset);

    if (is_fragment_duplicate(client, offset)) {
        register_ack(client, frame, ERR_DUPLICATE_FRAME);
        log_fragment_issue("Received duplicate text message fragment!", client, msg_id, offset, frag_len);
        return RET_VAL_ERROR;
    }

    if (search_message_id(hash_message_id, msg_id, client->session_id)) {
        register_ack(client, frame, STS_TRANSFER_COMPLETE);
        log_fragment_issue("Fragment is part of a previously fully received message!", client, msg_id, offset, frag_len);
        return RET_VAL_ERROR;
    }

    if (offset >= msg_len) {
        register_ack(client, frame, ERR_MALFORMED_FRAME);
        log_fragment_issue("Fragment offset past message bounds!", client, msg_id, offset, frag_len);
        return RET_VAL_ERROR;
    }

    if ((offset + frag_len > msg_len) || (frag_len > TEXT_FRAGMENT_SIZE)) {
        register_ack(client, frame, ERR_MALFORMED_FRAME);
        log_fragment_issue("Fragment length past message bounds!", client, msg_id, offset, frag_len);
        return RET_VAL_ERROR;
    }

    return RET_VAL_SUCCESS;
}






//BACKUP RECEIVE MESSAGE FRAME

// Process received message fragment frame
int process_message_fragment_frame(ClientData *client, UdpFrame *frame){

    // Extract the long text fragment and recombine the long message
    uint32_t recv_message_id = ntohl(frame->payload.long_text_msg.message_id);
    uint32_t recv_message_len = ntohl(frame->payload.long_text_msg.message_len);
    uint32_t recv_fragment_len = ntohl(frame->payload.long_text_msg.fragment_len);
    uint32_t recv_fragment_offset = ntohl(frame->payload.long_text_msg.fragment_offset);

    BOOL is_duplicate_fragment = client->inc_message[0].bitmap != NULL && client->inc_message[0].buffer != NULL &&
                                    check_fragment_received(client->inc_message[0].bitmap, recv_fragment_offset, (uint32_t)TEXT_FRAGMENT_SIZE);
    BOOL is_stray_fragment = (client->inc_message[0].bitmap == NULL || client->inc_message[0].buffer == NULL) &&
                                    search_message_id(hash_message_id, recv_message_id, client->session_id) == TRUE;

    if (is_duplicate_fragment == TRUE) {
        //Client already has bitmap and message buffer allocated so fragment can be processed
        //if the message was already received send duplicate frame ack op_code
        register_ack(client, frame, ERR_DUPLICATE_FRAME);
        fprintf(stderr, "Received duplicate text message fragment! - Session ID: %d, Message ID: %d, Fragment Offset: %d, Fragment Length: %d\n", client->session_id, recv_message_id, recv_fragment_offset, recv_fragment_len);
        return RET_VAL_ERROR;
    }
    if (is_stray_fragment) {
        //the client doesn't have a bitmap/message buffer allocated:
        //      -check if this fragment is part of a message that was fully received previously - if it is send completition ack op_code
        register_ack(client, frame, STS_TRANSFER_COMPLETE);
        fprintf(stderr, "Fragment is part of a previously fully received message! - Session ID: %d, Message ID: %d, Fragment Offset: %d, Fragment Length: %d\n", client->session_id, recv_message_id, recv_fragment_offset, recv_fragment_len);
        return RET_VAL_ERROR;
    }

    if(recv_fragment_offset >= recv_message_len) {
        //if the message has invalid payload metadata send ERR_MALFORMED_FRAME ack op code
        register_ack(client, frame, ERR_MALFORMED_FRAME);
        fprintf(stderr, "Fragment offset past message bounds! - Session ID: %d, Message ID: %d, Fragment Offset: %d, Fragment Length: %d\n", client->session_id, recv_message_id, recv_fragment_offset, recv_fragment_len);
        return RET_VAL_ERROR;
    }
    if (recv_fragment_offset + recv_fragment_len > recv_message_len || recv_fragment_len > TEXT_FRAGMENT_SIZE) {
        //if the message has invalid payload metadata send ERR_MALFORMED_FRAME ack op code 
        register_ack(client, frame, ERR_MALFORMED_FRAME);
        fprintf(stderr, "Fragment len past message bounds! - Session ID: %d, Message ID: %d, Fragment Offset: %d, Fragment Length: %d\n", client->session_id, recv_message_id, recv_fragment_offset, recv_fragment_len);
        return RET_VAL_ERROR;
    } 

    update_statistics(client); 
    BOOL message_id_found = FALSE;
    if(client->inc_message[0].message_id == recv_message_id){       
        message_id_found = TRUE;
        update_message_buffer_entry(&client->inc_message[0], frame->payload.long_text_msg.fragment_text, recv_fragment_offset, recv_fragment_len);
        register_ack(client, frame, STS_ACK);
        if(check_full_message_received(&client->inc_message[0], recv_message_len, client->message_file_name) != RET_VAL_SUCCESS){
            fprintf(stdout, "FILE PATH: %s\n", client->message_file_name);
            return RET_VAL_ERROR;
        }
    }
    if(message_id_found == TRUE) return RET_VAL_SUCCESS;
    
    if(allocate_new_message_buffer_entry(&client->inc_message[0], recv_message_id, recv_message_len) != RET_VAL_SUCCESS){
        return RET_VAL_ERROR;
    }
    update_message_buffer_entry(&client->inc_message[0], frame->payload.long_text_msg.fragment_text, recv_fragment_offset, recv_fragment_len);
    insert_message_id(hash_message_id, recv_message_id, client->session_id);
    register_ack(client, frame, STS_ACK);

    if(check_full_message_received(&client->inc_message[0], recv_message_len, client->message_file_name) != RET_VAL_SUCCESS){
        fprintf(stdout, "FILE PATH: %s\n", client->message_file_name);
        return RET_VAL_ERROR;
    }
   
    return RET_VAL_SUCCESS; 
}
//handle text message
void handle_message_fragment(ClientData *client, UdpFrame *frame) {
    EnterCriticalSection(&list.mutex);
    if (process_message_fragment_frame(client, frame) != RET_VAL_SUCCESS) {
        LeaveCriticalSection(&list.mutex);
        return;
    }
    LeaveCriticalSection(&list.mutex);
    return;
}

