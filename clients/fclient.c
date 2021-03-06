#include <sys/socket.h>
#include <stdlib.h>
#include <time.h>

#include "fakio.h"
#include "base/ini.h"
#include "base/sha2.h"
#include "base/fevent.h"

#define REPLY_SIZE 12
#define MAX_PASSWORD 64

typedef struct {
    uint8_t username[MAX_USERNAME];
    uint8_t name_len;
    uint8_t key[32];
    fcrypt_rand_t *r;
    
    char chost[MAX_HOST_LEN];
    char cport[MAX_PORT_LEN];

    char shost[MAX_HOST_LEN];
    char sport[MAX_PORT_LEN];
} fclient_t;

static context_pool_t *pool;
static fclient_t client;

void socks5_handshake1_cb(struct event_loop *loop, int fd, int mask, void *evdata);
void socks5_handshake2_cb(struct event_loop *loop, int fd, int mask, void *evdata);
void server_handshake1_cb(struct event_loop *loop, int fd, int mask, void *evdata);
void server_handshake2_cb(struct event_loop *loop, int fd, int mask, void *evdata);
static void client_readable_cb(struct event_loop *loop, int fd, int mask, void *evdata);
static void client_writable_cb(struct event_loop *loop, int fd, int mask, void *evdata);
static void remote_writable_cb(struct event_loop *loop, int fd, int mask, void *evdata);
static void remote_readable_cb(struct event_loop *loop, int fd, int mask, void *evdata);

static inline int client_fcrypt_ctx_init(fcrypt_ctx_t *ctx, uint8_t bytes[48])
{   
    memcpy(ctx->e_iv, bytes, 16);
    memcpy(ctx->d_iv, bytes+16, 16);
    memcpy(ctx->key, bytes+32, 16);

    ctx->e_pos = ctx->d_pos = 0;
    return fcrypt_set_key(ctx, ctx->key, 128);
}

static void server_accept_cb(struct event_loop *loop, int fd, int mask, void *evdata)
{
    while (1) {
        int client_fd = accept(fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno != EWOULDBLOCK) {
                fakio_log(LOG_WARNING, "accept() failed\n");
                break;
            }
            continue;
        }
        set_nonblocking(client_fd);
        set_socket_option(client_fd);

        LOG_FOR_DEBUG("new client %d comming connection", client_fd);
        create_event(loop, client_fd, EV_RDABLE, &socks5_handshake1_cb, NULL);
        break;
    }
}

void socks5_handshake1_cb(struct event_loop *loop, int fd, int mask, void *evdata)
{
    int rc, client_fd = fd;
    unsigned char buffer[258];

    while (1) {
        rc = recv(client_fd, buffer, 258, 0);
        
        if (rc < 0) {
            if (errno == EAGAIN) {
                return;
            }
            LOG_FOR_DEBUG("recv() failed: %s", strerror(errno));
            break;
        }
        if (rc == 0) {
            LOG_FOR_DEBUG("client %d connection closed\n", client_fd);
            break;
        }

        // Socks5 的第一次通信只传输很少的数据，这里偷了懒，认为只要收到，就收完了
        // Socks5 认证协议，采用 050100，这里不管发起方使用何种协议
        if (buffer[0] == SOCKS_VER) {
            buffer[1] = SOCKS_NO_AUTH;
            
            //TODO:
            rc = send(client_fd, buffer, 2, 0);
            if (rc == 2) {
                delete_event(loop, client_fd, EV_RDABLE);
                create_event(loop, client_fd, EV_RDABLE, &socks5_handshake2_cb, NULL);    
            }
            return;

        } else {
            fakio_log(LOG_WARNING, "Client request not socks5!");
            break;
        }
    }

    delete_event(loop, client_fd, EV_WRABLE);
    delete_event(loop, client_fd, EV_RDABLE);
    close(client_fd);
}


void socks5_handshake2_cb(struct event_loop *loop, int fd, int mask, void *evdata)
{
    int rc, client_fd = fd;
    frequest_t req;
    unsigned char buffer[1024];

    while (1) {
        rc = recv(client_fd, buffer, 512, 0);
        
        if (rc < 0) {
            if (errno == EAGAIN) {
                return;
            }
            LOG_FOR_DEBUG("recv() failed: %s", strerror(errno));
            break;
        }
        if (rc == 0) {
            LOG_FOR_DEBUG("client %d connection closed\n", client_fd);
            break;
        }

        // Socks5 认证协议，采用 050100，这里不管发起方使用何种协议
        if (buffer[0] == SOCKS_VER) {
            int remote_fd = fnet_create_and_connect(client.shost,
                            client.sport, FNET_CONNECT_BLOCK);
            if (remote_fd < 0) {
                fakio_log(LOG_WARNING, "Server don't onnection");
                break;
            }
            set_nonblocking(remote_fd);
            set_socket_option(remote_fd);

            /* 打印请求 */
            socks5_request_resolve(buffer, rc, &req);

            //Reply SOCKS5
            uint8_t reply[16];
            int reply_len = socks5_get_server_reply(client.chost,
                                client.cport, reply);
            //TODO:
            send(client_fd, reply, reply_len, 0);

            
            context_t *c = context_pool_get(pool, MASK_CLIENT|MASK_REMOTE);
            if (c == NULL) {
                fakio_log(LOG_WARNING, "Can't get context!");
                close(remote_fd);
                break;
            }
            LOG_FOR_DEBUG("client %d remote %d at %p", client_fd, remote_fd, c);
            c->client_fd = client_fd;
            c->remote_fd = remote_fd;
            c->loop = loop;

            //Request info

            random_bytes(client.r, FBUF_WRITE_AT(c->req), 16);


            *FBUF_WRITE_SEEK(c->req, 16) = client.name_len;
            memcpy(FBUF_WRITE_SEEK(c->req, 17), client.username, client.name_len);
            
            fcrypt_set_key(c->crypto, client.key, 256);

            buffer[2] = SOCKS_VER;
            int c_len = 1024 - 16 - 1 - client.name_len;

            uint8_t iv[16];
            memcpy(iv, FBUF_DATA_SEEK(c->req, 0), 16);
            
            fcrypt_encrypt_all(c->crypto, iv, c_len, buffer+2,
                               FBUF_WRITE_SEEK(c->req, 16+1+client.name_len));

            FBUF_COMMIT_WRITE(c->req, HANDSHAKE_SIZE);

            delete_event(loop, client_fd, EV_RDABLE);
            create_event(loop, c->remote_fd, EV_WRABLE, &server_handshake1_cb, c);
            return;
        } else {
            fakio_log(LOG_WARNING, "Client request not socks5!");
            break;
        }
    }

    delete_event(loop, client_fd, EV_WRABLE);
    delete_event(loop, client_fd, EV_RDABLE);
    close(client_fd);
}

void server_handshake1_cb(struct event_loop *loop, int fd, int mask, void *evdata)
{

    context_t *c = evdata;

    while (1) {
        int rc = send(fd, FBUF_DATA_AT(c->req), FBUF_DATA_LEN(c->req), 0);
        if (rc < 0) {
            if (errno == EAGAIN) {
                return;
            }
            LOG_FOR_DEBUG("send() to remote %d failed: %s", fd, strerror(errno));
            context_pool_release(c->pool, c, MASK_CLIENT|MASK_REMOTE);
            return;
        }
        
        if (rc >= 0) {
            /* 当发送 rc 字节的数据后，如果系统发送缓冲区满，则会产生 EAGAIN 错误，
             * 此时若 rc < c->recvlen，则再次发送时，会丢失 recv buffer 中的
             * c->recvlen - rc 中的数据，因此应该将其移到 recv buffer 前面
             */
            FBUF_COMMIT_READ(c->req, rc);
            if (FBUF_DATA_LEN(c->req) <= 0) {
                delete_event(loop, fd, EV_WRABLE);
                create_event(loop, c->remote_fd, EV_RDABLE, &server_handshake2_cb, c);
                return;
            }
        }
    }
}


void server_handshake2_cb(struct event_loop *loop, int fd, int mask, void *evdata)
{
    context_t *c = evdata;
    
    while (1) {
        int need = 64 - FBUF_DATA_LEN(c->res);
        int rc = recv(fd, FBUF_WRITE_AT(c->res), need, 0);
        
        if (rc < 0) {
            if (errno == EAGAIN) {
                return;
            }
            LOG_FOR_DEBUG("recv() from remote %d failed: %s", fd, strerror(errno));
            context_pool_release(c->pool, c, MASK_CLIENT|MASK_REMOTE);
            return;
        }
        if (rc == 0) {
            LOG_FOR_DEBUG("remote %d connection closed", fd);
            context_pool_release(c->pool, c, MASK_CLIENT|MASK_REMOTE);
            return;
        }

        FBUF_COMMIT_WRITE(c->res, rc);
        if (FBUF_DATA_LEN(c->res) < 64) {
            continue;
        }
        break;
    }
    
    uint8_t bytes[48];
    fcrypt_decrypt_all(c->crypto, FBUF_DATA_AT(c->res), 48, 
                       FBUF_DATA_SEEK(c->res, 16), bytes);
    client_fcrypt_ctx_init(c->crypto, bytes);
    FBUF_REST(c->res);

    delete_event(loop, fd, EV_RDABLE);
    create_event(loop, c->client_fd, EV_RDABLE, &client_readable_cb, c);
}

static void remote_readable_cb(struct event_loop *loop, int fd, int mask, void *evdata)
{
    context_t *c = evdata;

    while (1) {
        int rc = recv(fd, FBUF_WRITE_AT(c->res), BUFSIZE, 0);

        if (rc < 0) {
            if (errno == EAGAIN) {
                return;
            }
            LOG_FOR_DEBUG("recv() from remote %d failed: %s", fd, strerror(errno));
            context_pool_release(c->pool, c, MASK_CLIENT|MASK_REMOTE);
            return;
        }
        if (rc == 0) {
            LOG_FOR_DEBUG("remote %d connection closed", fd);
            context_pool_release(c->pool, c, MASK_CLIENT|MASK_REMOTE);
            return;
        }
        FBUF_COMMIT_WRITE(c->res, rc);
        
        break;
    }

    fcrypt_decrypt(c->crypto, c->res);
    delete_event(loop, fd, EV_RDABLE);
    create_event(loop, c->client_fd, EV_WRABLE, &client_writable_cb, c);
}


/* client 可写 */
static void remote_writable_cb(struct event_loop *loop, int fd, int mask, void *evdata)
{

    context_t *c = evdata;
    
    while (1) {
        int rc = send(fd, FBUF_DATA_AT(c->req), FBUF_DATA_LEN(c->req), 0);
        if (rc < 0) {
            if (errno == EAGAIN) {
                return;
            }
            LOG_FOR_DEBUG("send() to remote %d failed: %s", fd, strerror(errno));
            context_pool_release(c->pool, c, MASK_CLIENT|MASK_REMOTE);
            return;
        }
        
        if (rc >= 0) {
            /* 当发送 rc 字节的数据后，如果系统发送缓冲区满，则会产生 EAGAIN 错误，
             * 此时若 rc < c->recvlen，则再次发送时，会丢失 recv buffer 中的
             * c->recvlen - rc 中的数据，因此应该将其移到 recv buffer 前面
             */
            FBUF_COMMIT_READ(c->req, rc);
            
            if (FBUF_DATA_LEN(c->req) <= 0) {
                delete_event(loop, fd, EV_WRABLE);
                create_event(loop, c->remote_fd, EV_RDABLE, &remote_readable_cb, c);
                create_event(loop, c->client_fd, EV_RDABLE, &client_readable_cb, c);
                return;
            }
        }
    }
}


static void client_writable_cb(struct event_loop *loop, int fd, int mask, void *evdata)
{
    context_t *c = evdata;

    while (1) {
        int rc = send(fd, FBUF_DATA_AT(c->res), FBUF_DATA_LEN(c->res), 0);
        if (rc < 0) {
            if (errno == EAGAIN) {
                return;
            }
            LOG_FOR_DEBUG("send() failed to client %d: %s", fd, strerror(errno));
            context_pool_release(c->pool, c, MASK_CLIENT|MASK_REMOTE);
            return;
        }
        if (rc >= 0) {
            FBUF_COMMIT_READ(c->res, rc)
            if (FBUF_DATA_LEN(c->res) <= 0) {

                delete_event(loop, fd, EV_WRABLE);
                
                /* 如果 client 端已经关闭，则此次请求结束 */
                if (c->remote_fd == 0) {
                    context_pool_release(c->pool, c, MASK_CLIENT);
                } else {
                    create_event(loop, fd, EV_RDABLE, &client_readable_cb, c);
                    create_event(loop, c->remote_fd, EV_RDABLE, &remote_readable_cb, c);
                }
                break;
            }
        }
    }
}

static void client_readable_cb(struct event_loop *loop, int fd, int mask, void *evdata)
{
    context_t *c = evdata;
    int rc = recv(fd, FBUF_WRITE_AT(c->req), BUFSIZE, 0);

    if (rc < 0) {
        if (errno == EAGAIN) {
                return;
        }
        LOG_FOR_DEBUG("recv() failed form client %d: %s", fd, strerror(errno));
        context_pool_release(c->pool, c, MASK_CLIENT|MASK_REMOTE);
        return;
    }
    if (rc == 0) {
        LOG_FOR_DEBUG("client %d Connection closed", fd);
        context_pool_release(c->pool, c, MASK_REMOTE|MASK_CLIENT);
        return;
    }

    FBUF_COMMIT_WRITE(c->req, rc);
    fcrypt_encrypt(c->crypto, c->req);
    delete_event(loop, fd, EV_RDABLE);
    create_event(loop, c->remote_fd, EV_WRABLE, &remote_writable_cb, c);
}

static int config_handler(void* user, const char* section, const char* name,
                   const char* value)
{
    fclient_t *client = user;

    LOG_FOR_DEBUG("load conf: %s %s %s", section, name, value);

    if (strcmp("server", section) == 0) {
        if (strcmp("host", name) == 0) {
            strcpy(client->shost, value);
        } else if (strcmp("port", name) == 0) {
            strcpy(client->sport, value);
        } else {
            return 0;
        }
        return 1;
    }

    if (strcmp("client", section) == 0) {
        if (strcmp("host", name) == 0) {
            strcpy(client->chost, value);
        } else if (strcmp("port", name) == 0) {
            strcpy(client->cport, value);
        } else {
            return 0;
        }
        return 1;
    }

    if (strcmp("user", section) == 0) {
        if (strcmp("name", name) == 0) {
            size_t nlen = strlen(value);
            if (nlen > MAX_USERNAME) {
                fakio_log(LOG_ERROR, "User Name too long, must %d!", MAX_USERNAME);
                exit(1);
            }
            int i;
            for (i = 0; i < nlen; i++) {
                client->username[i] = value[i];
            }
            client->name_len = nlen;
        } else if (strcmp("password", name) == 0) {
            size_t plen = strlen(value);
            sha2((uint8_t *)value, plen, client->key, 0);
        } else {
            return 0;
        }
        return 1;
    }

    return 0;
}

static void client_load_config_file(const char *filename, fclient_t *client)
{
    FILE *f = fopen(filename, "rb");
    if (f == NULL) {
        fakio_log(LOG_ERROR, "Can't load config file: %s", filename);
        exit(1);
    }

    if (ini_parse_file(f, &config_handler, client) < 0) {
        fakio_log(LOG_ERROR, "Can't load config file: %s", filename);
        exit(1);
    }

    fclose(f);
}


int main (int argc, char *argv[])
{
    if (argc != 2) {
        fakio_log(LOG_ERROR, "Usage: %s --config_path\n", argv[0]);
        exit(1);
    }

    client_load_config_file(argv[1], &client);

    client.r = fcrypt_rand_new();
    if (client.r == NULL) {
        fakio_log(LOG_ERROR, "Start Error!");
        exit(1);
    }

    /* 初始化 Context */
    pool = context_pool_create(100);
    if (pool == NULL) {
        fakio_log(LOG_ERROR, "Start Error!");
        exit(1);
    }
    
    event_loop *loop;
    loop = create_event_loop(100);
    if (loop == NULL) {
        fakio_log(LOG_ERROR, "Create Event Loop Error!");
        exit(1);
    }
    
    /* NULL is 0.0.0.0 */
    int listen_sd = fnet_create_and_bind(client.chost, client.cport);
    if (listen_sd < 0)  {
        fakio_log(LOG_ERROR, "socket() failed");
        exit(1);
    }
    set_nonblocking (listen_sd);
    if (listen(listen_sd, SOMAXCONN) == -1) {
        fakio_log(LOG_ERROR, "socket() failed");
        exit(1);
    }

    create_event(loop, listen_sd, EV_RDABLE, &server_accept_cb, NULL);
    fakio_log(LOG_INFO, "Fakio client start... binding in %s:%s", client.chost, client.cport);
    fakio_log(LOG_INFO, "Fakio client event loop start, use %s", get_event_api_name());
    start_event_loop(loop);

    delete_event_loop(loop);
    
    return 0;
}