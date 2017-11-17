#ifndef __MOLTEN_AGENT_PROTOCOL_PROTOCOL_H
#define __MOLTEN_AGENT_PROTOCOL_PROTOCOL_H

#include "net_client.h"
#include "common.h"

/* for request and response */
#define PROTOCOL_HEADER_SIZE    (sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint32_t))

/* protocol process */
#define PROTOCOL_ECHO           0
#define PROTOCOL_TIME           1
#define PROTOCOL_HEARTBEAT      2

/* return code */
#define PROTOCOL_ERROR_MSG                  -1
#define PROTOCOL_READ_CONTINUE              0
#define PROTOCOL_NEED_REPLY                 1

/* response code */
#define RESPONSE_SUCESS         0
#define RESPONSE_FAIL           1

/* request */
typedef struct {
    /* header */
    uint16_t type; 
    uint32_t size;
    uint32_t validate;

    /* body */
    char *body;
}pro_req;


/* response */
typedef struct {

    /* header */
    uint16_t code;
    uint32_t size;
    uint32_t validate;  /* not use */

    /* body */
    char *body;
}pro_res;

/* add reply */
static void add_reply(net_client *nc, int success, const char *buf, uint32_t size) {
    char header_buf[PROTOCOL_HEADER_SIZE];
    char *tmp_buffer = header_buf;
    int result =  success ? RESPONSE_SUCESS : RESPONSE_FAIL;
    memcpy(tmp_buffer, &result, sizeof(uint16_t)); tmp_buffer += sizeof(uint16_t);
    memcpy(tmp_buffer, &size, sizeof(uint32_t)); tmp_buffer += sizeof(uint32_t);
    memcpy(tmp_buffer, &size, sizeof(uint32_t));

    append_writer(nc->w, header_buf, PROTOCOL_HEADER_SIZE);
    append_writer(nc->w, buf, size);
}

typedef void protocol_process(net_client *nc, const char *body, int body_size);

/* for different protocol */
typedef struct {
    char *name;
    uint16_t sequence;
    protocol_process *process;
    uint8_t need_reply:1;           /* need reply */
}cmd;

/* echo server */
static void process_echo(net_client *nc, const char *body, int body_size) {
    AGENT_SLOG(SLOG_DEBUG, "[echo protocol] rec body :[%s]", body);
    AGENT_SLOG(SLOG_DEBUG, "[echo protocol] read buf :[%s], read pos:[%d], write pos:[%d]", nc->r->read_buf, nc->r->read_pos, nc->r->len);
    add_reply(nc, 1, body, body_size);
}

/* time server */
static void process_time(net_client *nc, const char *body, int body_size) {
    add_reply(nc, 1, "13.14", sizeof("13.14") - 1);
}


cmd cmd_list[] = {
    {"echo",    PROTOCOL_ECHO,       process_echo,       1},
    {"time",    PROTOCOL_TIME,       process_time,       1}
    //PROTOCOL_HEARTBEAT,
};

#define CMD_LIST_SIZE                       (sizeof(cmd_list)/sizeof(cmd))
#define PROTOCOL_REQ_VALIDATE(type, size)    (type *2 + size)

/* unpck response , return body */
char *protocol_unpack_res(char *buf, int buf_size, int *body_size) {
    uint16_t code = *(uint16_t *)buf; buf += sizeof(uint16_t);
    uint32_t size = *(uint32_t *)buf; buf += sizeof(uint32_t);
    uint32_t validate = *(uint32_t *)buf; buf += sizeof(uint32_t);
    
    char *body = smalloc(size);
    memcpy(body, buf, size);
    *body_size = size;
    return body;
}

/* pack req */
char* protocol_pack_req(int type, const char *buf, int size, int *buf_size) {
    char *new_buf = smalloc(PROTOCOL_HEADER_SIZE + size);    
    char *tmp_buf = new_buf;
    int validate = PROTOCOL_REQ_VALIDATE(type, size);
    
    memcpy(tmp_buf, &type, sizeof(uint16_t)); tmp_buf += sizeof(uint16_t);
    memcpy(tmp_buf, &size, sizeof(uint32_t)); tmp_buf += sizeof(uint32_t);
    memcpy(tmp_buf, &validate, sizeof(uint32_t)); tmp_buf += sizeof(uint32_t);
    memcpy(tmp_buf, buf, size); 
    *buf_size = PROTOCOL_HEADER_SIZE + size;
    return new_buf;
}

/* analyze protocol */
int protocol_analyze_req(net_client *nc) {
    int can_read_size = reader_read_size(nc->r);
    if (can_read_size >= PROTOCOL_HEADER_SIZE) {

        char* read_pos = reader_read_start(nc->r);

        /* same host no need le/be problem */
        int type = *(uint16_t *)read_pos; read_pos += sizeof(uint16_t);
        int size = *(uint32_t *)read_pos; read_pos += sizeof(uint32_t);
        int validate = *(uint32_t *)read_pos; read_pos += sizeof(uint32_t);

        /* validate */
        if (validate != (PROTOCOL_REQ_VALIDATE(type, size)))  {
            return PROTOCOL_ERROR_MSG;
        }
        
        /* read from buffer */
        int end_pos = PROTOCOL_HEADER_SIZE + size;
        if (can_read_size >= end_pos) {

            /* can not changed */
            const char* body = reader_read_start(nc->r) + PROTOCOL_HEADER_SIZE;

            /* process */
            if (type <= CMD_LIST_SIZE) {
                cmd_list[type].process(nc, body, size);
            } else {
                return PROTOCOL_ERROR_MSG;
            }
            
            /* forward_reader_pos */
            forward_reader_pos(nc->r, end_pos);

            if (cmd_list[type].need_reply) {
                return PROTOCOL_NEED_REPLY;
            } else {
                return PROTOCOL_READ_CONTINUE;
            }
        } else {
            return PROTOCOL_READ_CONTINUE;
        }
    }
}
#endif
