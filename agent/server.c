#include "server.h"

// all config in server, string not use const, alloc from heap
m_server server;

void write_pid_file() {
    FILE *fp = fopen(server.pid_file, "w");
    if (fp) {
        fprintf(fp, "%d\n", server.pid);
        fclose(fp);
    }
}

void daemonize() {
    int fd;
    if (fork() != 0) exit(0); //parent exit
    setsid();
    if ((fd = open("/dev/null", O_RDWR, 0)) != 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
}

static void sig_stop_server(int sig) {
    char *msg;
    switch(sig) {
    case SIGTERM:
        msg = "Received SIGTERM to shutdown ... ";
        break;
    case SIGINT:
        msg = "Received SIGINIT to shutdown ... ";
        break;
    default:
        msg = "Received SIGNAL to shutdown ... ";
    }
    server.prepare_stop = 1; 
    server.el->stop = 1;
    AGENT_SLOG(SLOG_DEBUG, "[server]agent server prepare to stop");
}

static void dump_server_status(int sig) {
    //dump server run status
    printf("------------server status--------------\n");
    printf("---------server memory used------------\n");

    //read from smalloc and read from proc
    printf("smalloc: %d\n", dump_used_bytes());

    //dump active client
    printf("-------------client detail------------\n");
    printf("active_client:%d, total_client:%d\n", server.active_client_num, server.total_client_num);
}

static void set_signal_handlers() {
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    struct sigaction act;
    
    //not mask other signal
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler =  sig_stop_server;
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGINT, &act, NULL);

    //we add debug handle for SIGUSR
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = dump_server_status;
    sigaction(SIGUSR1, &act, NULL);
    
    //we can add debug handle for SIGSEGV
}

static void server_init() {
    AGENT_SLOG(SLOG_DEBUG, "[server]agent server start prepare ...");

    if (server.daemon)  daemonize();
    server.pid_file = sstrdup(SERVER_DEFAULT_PID_FILE);
    server.pid = getpid();
    server.uptime = time(NULL);
    server.active_client_num = 0;
    server.total_client_num = 0;
    server.prepare_stop = 0;

    write_pid_file();

    /* todo lock pid file for one instance */

    set_signal_handlers();
    AGENT_SLOG(SLOG_DEBUG, "[server]agent server start ...");
}

static void server_shutdown() {
    sfree(server.pid_file);
    AGENT_SLOG(SLOG_DEBUG, "[server]agent server stop");
}

// module one main thread to collect net io, other thread to execute  task
int main() {

    slog_init(SLOG_STDOUT, "");
    server_init();
    
    // we need feature
    // 1 heartbeat to sw
    // 2 check data from molten
    // 3 receive tracing data from molten transfer to sw
    
    int server_fd;
    if ((server_fd = create_tcp_listen_server("127.0.0.1", 12200, 256, AF_INET, NULL)) == A_FAIL) {
       AGENT_SLOG(SLOG_ERROR, "[server] create tcp listen socket error");
       exit(-1);
    }

    server.listen_fd = server_fd;
    event_loop *el = create_main_event_loop(1024);
    server.el = el;

    add_net_event(el, server_fd, NULL, accept_net_client, EVENT_READ);

    //uint64_t  period;
    // simple control info
    // one thread to collect info

    // 100 ms tick   
    execute_loop(server.el);

    server_shutdown();
    slog_destroy();
    //timer
    //tick
}
