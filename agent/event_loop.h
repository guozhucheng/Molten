#ifndef __MOLTEN_EVENT_LOOP_H
#define __MOLTEN_EVENT_LOOP_H

#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include "common.h"
#include "event.h"
#include "protocol.h"

#define MAX_CONNECtiON          1024 /* for debug now */
#define ADDItION_CONNECTION     96   /* keep hole for process used */

#ifdef __linux__
#include "epoll_event.h"
#else
#include "select_event.h"
#endif


/* remove net event */
void remove_net_event(event_loop *el, int fd, int mask) {
    net_event *ne = &el->regist_events[fd];
    if (ne->mask == EVENT_NONE) return;
    
    remove_event_implement(el, fd, mask);
    ne->mask &= ~mask;
    
    /* for select need to update max_fd */
    if (fd == el->max_fd && ne->mask == EVENT_NONE) {
        for(int i = el->max_fd; i >= 0; i--) {
            if (el->regist_events[i].mask != EVENT_NONE) {
                el->max_fd = i;
                break;
            }
        }
    }
}

/* add network event */
int add_net_event(event_loop *el, int fd, void *client, net_event_process process, int mask) {
    net_event *ne = &el->regist_events[fd];
    AGENT_SLOG(SLOG_DEBUG, "[event] add event fd:%d, mask:%d start", fd, mask);
    if (add_event_implement(el, fd, mask) != ACTION_SUCCESS)  return ACTION_FAIL;
    
    ne->mask |= mask;
    ne->client = client;
    if (mask & EVENT_READ) ne->r_handler = process;
    if (mask & EVENT_WRITE) ne->w_handler = process;
    ne->client = client;

    // for select update max fd;
    if (fd > el->max_fd) {
        el->max_fd = fd;
    }
    AGENT_SLOG(SLOG_DEBUG, "[event] add event fd:%d, mask:%d success", fd, mask);
    return ACTION_SUCCESS;
}

/* create main event loop */
event_loop *create_main_event_loop(int max_size) {
    event_loop *main_loop = NULL;
    main_loop = smalloc(sizeof(event_loop));

    main_loop->stop = 0;
    main_loop->max_event = max_size;
    main_loop->regist_events = smalloc(max_size * sizeof(net_event));
    main_loop->fire_events  = smalloc(max_size * sizeof(net_event));
    memset(main_loop->regist_events, 0x00, max_size *sizeof(net_event));

    assert(create_implement_event_loop(main_loop) == ACTION_SUCCESS);

    return main_loop;
}

/* free main loop */
void free_main_event_loop(event_loop *el) {
    free_implement_event_loop(el);
    sfree(el->regist_events);
    sfree(el->fire_events);
    sfree(el);
}


/* stop main loop */
/* relation event loop with main server */
void main_loop_stop(event_loop *el) {
    el->stop = 1;
}

/* execute loop */
uint64_t execute_loop(event_loop *el) {

    struct timeval intval = {0};
#define TICK_TIME       1000 * 100
    // 100 ms tick
    intval.tv_sec = 0;
    intval.tv_usec = TICK_TIME;

    // use time wheel
    // not time heap
     
    for(;;) {

        if (el->stop == 1) break;

        struct timeval start_intval, end_intval;

        // time zone ignore;
        // start intval
        gettimeofday(&start_intval, NULL);

        // todo add time trigger 
        int event_num = event_loop_implement(el, &intval);
        for(int i = 0; i < event_num; i++)  {
            net_event *ne = &el->regist_events[el->fire_events[i].fd];
            int mask = ne->mask;
            int fd = el->fire_events[i].fd;
            
            if (ne->mask & EVENT_READ) {
                AGENT_SLOG(SLOG_DEBUG, "[event loop] read event come %d", fd);
                ne->r_handler(el, fd, ne->client, mask);
            }

            if (ne->mask & EVENT_WRITE) {
                AGENT_SLOG(SLOG_DEBUG, "[event loop] write event come %d", fd);
                ne->w_handler(el, fd, ne->client, mask);
            }
        }

        // end intval
        gettimeofday(&end_intval, NULL);

        // reset intval
        // time to usec
        long start_usec = start_intval.tv_sec * 1000 * 1000 + start_intval.tv_usec;
        long end_usec = end_intval.tv_sec * 1000 * 1000 + end_intval.tv_usec;
        
        /* 1 ms float */
        if (end_usec - start_usec >= (TICK_TIME - 1000 )) {
            intval.tv_usec = TICK_TIME;
        } else {
            intval.tv_usec = end_usec - start_usec;
        }
    }
}

/* release client */
static void release_client(event_loop *el, net_client *c) {
   if (c->fd != -1) {
        remove_net_event(el, c->fd, EVENT_READ);
        remove_net_event(el, c->fd, EVENT_WRITE);
        close(c->fd);
        c->fd = -1;

        //automic
        server.active_client_num--;
   }
   free_client(c);
}

/* echo to client */
void write_to_client(event_loop *el, int fd, void *client, int mask) {
    net_client *c = (net_client *)client;
    int res = writer_write(c->w, fd);          
    if (res == RW_WRITE_FINISH) {
        remove_net_event(el, fd, EVENT_WRITE);
    } else if (res == RW_WRITE_ERROR){
        release_client(el, c);
    }
}

//just level trigger, no need to read until EWOULD
void read_from_client(event_loop *el, int fd, void *client, int mask) {

    AGENT_SLOG(SLOG_DEBUG, "[client read] client read event process fd:%d", fd);
    char read_buf[CLIENT_MAX_READ] = {0};

    // just use echo server for debug;
    net_client *c = (net_client *)client;
    int nread = read(fd, read_buf, CLIENT_MAX_READ);
    if (nread == -1) {
        if (errno == EAGAIN) {
            return;
        } else {
            AGENT_SLOG(SLOG_DEBUG, "[client] client[%d] read error:%d free client", fd, errno);
            release_client(el, c);
            return;
        }
    } else if (nread == 0) {
        //remove event, delete client  
        //todo log
        AGENT_SLOG(SLOG_DEBUG, "[client] client[%d] close", fd);
        release_client(el, c);
        return;
    }

    c->last_read = time(NULL);

    AGENT_SLOG(SLOG_DEBUG, "[client read] client[%d] read buf size:%d", fd, nread);

    // add bufer; 
    append_reader(c->r, read_buf, nread);
    
    /* here it is protocl analyze process */
    int ret = protocol_analyze_req(c);
    if (ret == PROTOCOL_ERROR_MSG) {
        AGENT_SLOG(SLOG_DEBUG, "[client] client[%d] read error protocol", fd);
        release_client(el, c);
        return;
    } else if (ret == PROTOCOL_NEED_REPLY){
        add_net_event(el, c->fd, c, write_to_client, EVENT_WRITE); 
    }

    //// add event;
    //reader_to_writer(c->r, c->w);

    ///* echo server just echo */
    //add_net_event(el, c->fd, c, write_to_client, EVENT_WRITE); 
}


void accept_net_client(event_loop *el, int fd, void *client, int mask) {
    struct sockaddr_storage ca;
    int client_fd;
    char client_ip[CLIENT_IP_MAX_LEN] = {0};
    socklen_t slen = sizeof(ca);
    int port;
    while(1) {
        client_fd = accept(fd, (struct sockaddr *)&ca, &slen);
        if (fd == -1) {
            if (errno == EINTR) {
                continue;
            } else {
                //todo log 
                AGENT_SLOG(SLOG_DEBUG, "[accept] accept client error:%d\n", errno);
                return;
            }
        }
        break;
    }
    
    /* get client ip */
    if (ca.ss_family == AF_INET) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)&ca;
        inet_ntop(AF_INET, (void *)&v4->sin_addr, client_ip, CLIENT_IP_MAX_LEN);
        port = ntohs(v4->sin_port);
    } else {
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&ca;
        inet_ntop(AF_INET6, (void *)&v6->sin6_addr, client_ip, CLIENT_IP_MAX_LEN);
        port = ntohs(v6->sin6_port);
    }

    //todo log
    AGENT_SLOG(SLOG_DEBUG, "[accept] accept client from ip:%s, port:%d, fd:%d\n", client_ip, port, client_fd);

    // todo determine > max_client

    //create client regist connect event
    set_nodelay(client_fd);
    set_noblock(client_fd);
    net_client *nc = new_client(client_fd, client_ip);
    AGENT_SLOG(SLOG_DEBUG, "[accept] accept client from ip:%s, port:%d, fd:%d\n", client_ip, port, client_fd);

    //todo tcp keepalive 
    add_net_event(el, client_fd, (void *)nc, read_from_client, EVENT_READ);

    //todo automic add client_num;
    server.active_client_num++;
    server.total_client_num++;
}
#endif
