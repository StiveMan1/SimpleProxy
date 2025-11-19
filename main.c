#include <errno.h>

#include "libconfig.h"

#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/types.h>

#include <arpa/nameser.h>
#include <arpa/inet.h>

#include <netinet/in.h>

#include <pthread.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>

#include <stdlib.h>
#include <stdio.h>

#include <resolv.h>



typedef struct proxy_client {
    int32_t cli_s;
    int32_t des_s;

    struct proxy_client *next;
    struct proxy_client *prev;
} proxy_client;

typedef struct {
    proxy_client *head;
    proxy_client *last;

    int32_t count;
} clients_list;

typedef struct proxy_server {
    clients_list clients;

    int32_t socket;
    int32_t epoll;

    int32_t ai_family; // AF_INET
    int32_t ai_socktype; // SOCK_STREAM
    int32_t ai_protocol; // IPPROTO_TCP
    int32_t ai_interface; // INADDR_ANY
    int32_t port;

    const char *des_server;
    int32_t des_port;

    pthread_t thread;
    struct proxy_server *next;
} proxy_server;

typedef struct {
    proxy_server *head;
    proxy_server *last;
} servers_list;


#define MAX_PACKET (1024 * 1024)
char *config_file = "/etc/simple-proxy/config.cfg";



void proxy_clients_add(clients_list *clients, const int32_t cli_socket, const int32_t des_socket) {
    proxy_client *cli = calloc(1, sizeof(proxy_client));
    *cli = (proxy_client) {cli_socket, des_socket, NULL, NULL};

    if (++clients->count != 1) clients->last->next = cli;
    else clients->head = cli;

    cli->prev = clients->last;
    clients->last = cli;
}

void proxy_clients_rem(clients_list *clients, proxy_client *cli) {
    if (cli->prev) cli->prev->next = cli->next;
    else clients->head = cli->next;

    if (cli->next) cli->next->prev = cli->prev;
    else clients->last = cli->prev;

    close(cli->cli_s);
    close(cli->des_s);
    --clients->count;
    free(cli);
}

proxy_client *proxy_clients_find(const clients_list *clients, const int fd) {
    for (proxy_client *cli = clients->head; cli != NULL; cli = cli->next) {
        if (cli->cli_s == fd || cli->des_s == fd) return cli;
    }
    return NULL;
}


int32_t destination_socket(const proxy_server *srv) {
    struct addrinfo hints = {0}, *res;
    hints.ai_family = srv->ai_family;
    hints.ai_socktype = srv->ai_socktype;
    hints.ai_protocol = srv->ai_protocol;

    if (getaddrinfo(srv->des_server, NULL, &hints, &res) != 0) return -1;
    ((struct sockaddr_in *)res->ai_addr)->sin_port = htons(srv->des_port);

    const int32_t sock = socket(srv->ai_family, srv->ai_socktype, srv->ai_protocol);
    const int32_t conn = sock == -1? 0 : connect(sock, res->ai_addr, res->ai_addrlen);

    freeaddrinfo(res);
    if (conn != 0) close(sock);
    return conn == 0 ? sock : -1;
}


void *do_server(void *arg) {
    proxy_server *srv = arg;

    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == -1) return NULL;
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);


    srv->epoll = epoll_create(1);
    srv->socket = socket(srv->ai_family, srv->ai_socktype, srv->ai_protocol);

    if (srv->socket == -1) return NULL;


    const int option = 1;
    setsockopt(srv->socket, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));


    struct sockaddr_in proxy_addr = {0};
    proxy_addr.sin_family = srv->ai_family;
    proxy_addr.sin_addr.s_addr = srv->ai_interface;
    proxy_addr.sin_port = htons(srv->port);


    if (bind(srv->socket, (struct sockaddr *) &proxy_addr, sizeof(proxy_addr)) != 0) goto end;
    if (srv->ai_protocol == IPPROTO_TCP && listen(srv->socket, 128) != 0) goto end;


    struct epoll_event ev = {EPOLLIN, {.fd = srv->socket}};
    if (epoll_ctl(srv->epoll, EPOLL_CTL_ADD, srv->socket, &ev) != 0) goto end;


    while (1) {
        const int32_t nfds = epoll_wait(srv->epoll, &ev, 1, -1);
        if (nfds <= 0) continue;

        if (ev.data.fd == srv->socket) {
            const int32_t cli_socket = accept(srv->socket, NULL, NULL);
            const int32_t des_socket = destination_socket(srv);
            printf("PORT %d : %d %d\n", srv->port, cli_socket, des_socket);

            if (cli_socket == -1 || des_socket == -1) goto bad_cli;

            ev = (struct epoll_event) {EPOLLIN, {.fd = cli_socket}};
            if (epoll_ctl(srv->epoll, EPOLL_CTL_ADD, cli_socket, &ev) == -1) goto bad_cli;

            ev = (struct epoll_event) {EPOLLIN, {.fd = des_socket}};
            if (epoll_ctl(srv->epoll, EPOLL_CTL_ADD, des_socket, &ev) == -1) goto bad_cli;

            proxy_clients_add(&srv->clients, cli_socket, des_socket);
            continue;
bad_cli:
            if (cli_socket != -1) close(cli_socket);
            if (des_socket != -1) close(des_socket);
        } else {
            proxy_client *cli = proxy_clients_find(&srv->clients, ev.data.fd);
            if (cli == NULL) continue;

            const int32_t fd_t = ev.data.fd;
            const int32_t fd_f = cli->cli_s == ev.data.fd? cli->des_s : cli->cli_s;

            if (splice(fd_t, NULL, fds[1], NULL, MAX_PACKET, SPLICE_F_MOVE | SPLICE_F_NONBLOCK) > 0 &&
                splice(fds[0], NULL, fd_f, NULL, MAX_PACKET, SPLICE_F_MOVE | SPLICE_F_NONBLOCK) > 0)
                continue;

            epoll_ctl(srv->epoll, EPOLL_CTL_DEL, cli->cli_s, NULL);
            epoll_ctl(srv->epoll, EPOLL_CTL_DEL, cli->des_s, NULL);

            proxy_clients_rem(&srv->clients, cli);
        }
    }
end:
    printf("ERR PORT : %d\n", srv->port);

    close(srv->socket);
    return NULL;
}



void parse_config(proxy_server *srv, const config_setting_t *config) {
    config_setting_lookup_int(config, "domain", &srv->ai_family);
    config_setting_lookup_int(config, "service", &srv->ai_socktype);
    config_setting_lookup_int(config, "protocol", &srv->ai_protocol);
    config_setting_lookup_int(config, "interface", &srv->ai_interface);
    config_setting_lookup_int(config, "port", &srv->port);

    const config_setting_t *destination = config_setting_lookup(config, "destination");

    config_setting_lookup_string(destination, "address", &srv->des_server);
    config_setting_lookup_int(destination, "port", &srv->des_port);
}

void start_new_server(servers_list *servers, const config_setting_t *config) {
    proxy_server *srv = malloc(sizeof(proxy_server));
    *srv = (proxy_server) {{NULL, NULL}, -1, -1, AF_INET, SOCK_STREAM, IPPROTO_TCP, INADDR_ANY};

    parse_config(srv, config);
    pthread_create(&srv->thread, NULL, do_server, srv);

    if (servers->head == NULL) servers->head = srv;
    else servers->last->next = srv;
    servers->last = srv;
}

void join_thread_list(const servers_list *servers) {
    for (proxy_server *srv = servers->head, *next; srv != NULL; srv = next) {
        pthread_join(srv->thread, NULL);

        next = srv->next;
        free(srv);
    }
}

int32_t main(int32_t argc, char **argv) {
    servers_list servers = {NULL, NULL};
    config_t cfg;

    config_init(&cfg);
    if (!config_read_file(&cfg, config_file)) {
        fprintf(stderr, "%s:%d - %s\n", config_error_file(&cfg), config_error_line(&cfg), config_error_text(&cfg));
        config_destroy(&cfg);
        return EXIT_FAILURE;
    }

    const config_setting_t *setting = config_lookup(&cfg, "proxy_servers");
    const int32_t count = config_setting_length(setting);
    printf("Length : %d\n", count);

    for (int i = 0; i < count; i++)
        start_new_server(&servers, config_setting_get_elem(setting, i));

    join_thread_list(&servers);
    config_destroy(&cfg);
}