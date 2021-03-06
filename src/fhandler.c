#include "fhandler.h"
#include <sys/socket.h>
#include "fakio.h"


static void client_handshake_cb(struct event_loop *loop, int fd, int mask, void *evdata);
static void client_readable_cb(struct event_loop *loop, int fd, int mask, void *evdata);
static void client_writable_cb(struct event_loop *loop, int fd, int mask, void *evdata);
static void remote_writable_cb(struct event_loop *loop, int fd, int mask, void *evdata);
static void remote_readable_cb(struct event_loop *loop, int fd, int mask, void *evdata);

static long handshake_timeout_cb(struct event_loop *loop, void *evdata)
{   
    context_t *c = evdata;

    if (context_get_mask(c) == MASK_CLIENT) {
        fakio_log(LOG_WARNING,"client %d handshake timeout!", c->client_fd);
        context_pool_release(c->pool, c, MASK_CLIENT);
    }
    
    return EV_TIMER_END;
}

void server_accept_cb(struct event_loop *loop, int fd, int mask, void *evdata)
{
    while (1) {
        int client_fd = accept(fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno != EWOULDBLOCK) {
                fakio_log(LOG_WARNING,"accept() failed: %s", strerror(errno));
                break;
            }
            continue;
        }
        set_nonblocking(client_fd);
        set_socket_option(client_fd);

        fserver_t *server = evdata;
        context_t *c = context_pool_get(server->pool, MASK_CLIENT);
        if (c == NULL) {
            fakio_log(LOG_WARNING,"Client %d Can't get context", client_fd);
            close(client_fd);
            return;
        }
        c->client_fd = client_fd;
        c->loop = loop;
        c->server = server;

        LOG_FOR_DEBUG("new client %d comming connection", client_fd);
        create_event(loop, client_fd, EV_RDABLE, &client_handshake_cb, c);
        create_time_event(loop, 10*1000, &handshake_timeout_cb, c);
        break;
    }
}


static void client_handshake_cb(struct event_loop *loop, int fd, int mask, void *evdata)
{
    int r, need, client_fd = fd;
    context_t *c = evdata;

    while (1) {
        need = HANDSHAKE_SIZE - FBUF_DATA_LEN(c->req);
        int rc = recv(client_fd, FBUF_WRITE_AT(c->req), need, 0);

        if (rc < 0) {
            if (errno == EAGAIN) {
                return;    
            }
            context_pool_release(c->pool, c, MASK_CLIENT);
            return;
        }
        if (rc == 0) {
            LOG_FOR_DEBUG("client %d connection closed", client_fd);
            context_pool_release(c->pool, c, MASK_CLIENT);
            return;
        }

        if (rc > 0) {
            FBUF_COMMIT_WRITE(c->req, rc);
            if (FBUF_DATA_LEN(c->req) < HANDSHAKE_SIZE) {
                continue;
            }
            break;
        }
    }

    // 用户认证
    frequest_t req;
    fakio_request_resolve(FBUF_DATA_AT(c->req), HANDSHAKE_SIZE,
                          &req, FNET_RESOLVE_USER);

    c->user = fuser_find_user(c->server->users, req.username, req.name_len);
    if (c->user == NULL) {
        fakio_log(LOG_WARNING,"user: %s Not Found!", req.username);
        context_pool_release(c->pool, c, MASK_CLIENT);
        return;
    }
    
    fcrypt_set_key(c->crypto, c->user->key, 256);

    uint8_t buffer[HANDSHAKE_SIZE];
    
    fcrypt_decrypt_all(c->crypto, req.IV, HANDSHAKE_SIZE-req.rlen,
                       FBUF_DATA_SEEK(c->req, req.rlen), buffer+req.rlen);

    r = fakio_request_resolve(buffer+req.rlen, HANDSHAKE_SIZE-req.rlen,
                              &req, FNET_RESOLVE_NET);
    if (r != 1) {
        fakio_log(LOG_WARNING,"socks5 request resolve error");
        context_pool_release(c->pool, c, MASK_CLIENT);
        return;
    }
    int remote_fd = fnet_create_and_connect(req.addr, req.port, FNET_CONNECT_NONBLOCK);
    if (remote_fd < 0) {
        context_pool_release(c->pool, c, MASK_CLIENT);
        return;
    }

    if (set_socket_option(remote_fd) < 0) {
        fakio_log(LOG_WARNING,"set socket option error");
    }
    
    LOG_FOR_DEBUG("client %d remote %d at %p", client_fd, remote_fd, c);
    
    c->remote_fd = remote_fd;
    context_set_mask(c, MASK_CLIENT|MASK_REMOTE);
    FBUF_REST(c->req);
    FBUF_REST(c->res);

    random_bytes(c->server->r, buffer, 64);

    uint8_t bytes[64];
    memcpy(bytes, buffer, 64);
    fcrypt_encrypt_all(c->crypto, bytes, 48, buffer+16, buffer+16);

    //TODO:
    send(client_fd, buffer, 64, 0);

    fcrypt_ctx_init(c->crypto, bytes+16);

    delete_event(loop, client_fd, EV_RDABLE);
    create_event(loop, client_fd, EV_RDABLE, &client_readable_cb, c);

    memset(buffer, 0, HANDSHAKE_SIZE);
    return;
}


static void client_readable_cb(struct event_loop *loop, int fd, int mask, void *evdata)
{
    context_t *c = evdata;

    while (1) {
        int rc = recv(fd, FBUF_WRITE_AT(c->req), BUFSIZE, 0);

        if (rc < 0) {
            if (errno == EAGAIN) {
                return;
            }
            LOG_FOR_DEBUG("recv() from client %d failed: %s", fd, strerror(errno));
            context_pool_release(c->pool, c, MASK_CLIENT|MASK_REMOTE);
            return;
        }
        if (rc == 0) {
            LOG_FOR_DEBUG("client %d connection closed", fd);
            context_pool_release(c->pool, c, MASK_CLIENT|MASK_REMOTE);
            return;
        }
        FBUF_COMMIT_WRITE(c->req, rc);
        
        break;
    }

    fcrypt_decrypt(c->crypto, c->req);
    delete_event(loop, fd, EV_RDABLE);
    create_event(loop, c->remote_fd, EV_WRABLE, &remote_writable_cb, c);
}


/* client 可写 */
static void client_writable_cb(struct event_loop *loop, int fd, int mask, void *evdata)
{

    context_t *c = evdata;

    while (1) {
        int rc = send(fd, FBUF_DATA_AT(c->res), FBUF_DATA_LEN(c->res), 0);
        if (rc < 0) {
            if (errno == EAGAIN) {
                return;
            }
            LOG_FOR_DEBUG("send() to client %d failed: %s", fd, strerror(errno));
            context_pool_release(c->pool, c, MASK_CLIENT|MASK_REMOTE);
            return;
        }
        
        if (rc >= 0) {
            /* 当发送 rc 字节的数据后，如果系统发送缓冲区满，则会产生 EAGAIN 错误，
             * 此时若 rc < c->recvlen，则再次发送时，会丢失 recv buffer 中的
             * c->recvlen - rc 中的数据，因此应该将其移到 recv buffer 前面
             */
            FBUF_COMMIT_READ(c->res, rc);
            if (FBUF_DATA_LEN(c->res) <= 0) {
                delete_event(loop, fd, EV_WRABLE);
                create_event(loop, c->client_fd, EV_RDABLE, &client_readable_cb, c);
                create_event(loop, c->remote_fd, EV_RDABLE, &remote_readable_cb, c);
                return;
            }
        }
    }
}


static void remote_writable_cb(struct event_loop *loop, int fd, int mask, void *evdata)
{
    context_t *c = evdata;

    while (1) {
        int rc = send(fd, FBUF_DATA_AT(c->req), FBUF_DATA_LEN(c->req), 0);
        if (rc < 0) {
            if (errno == EAGAIN) {
                return;
            }
            LOG_FOR_DEBUG("send() failed to remote %d: %s", fd, strerror(errno));
            context_pool_release(c->pool, c, MASK_CLIENT|MASK_REMOTE);
            return;
        }
        if (rc >= 0) {
            FBUF_COMMIT_READ(c->req, rc)
            if (FBUF_DATA_LEN(c->req) <= 0) {

                delete_event(loop, fd, EV_WRABLE);
                
                /* 如果 client 端已经关闭，则此次请求结束 */
                if (c->client_fd == 0) {
                    context_pool_release(c->pool, c, MASK_REMOTE);
                } else {
                    create_event(loop, fd, EV_RDABLE, &remote_readable_cb, c);
                    create_event(loop, c->client_fd, EV_RDABLE, &client_readable_cb, c);
                }
                break;
            }
        }
    }
}

static void remote_readable_cb(struct event_loop *loop, int fd, int mask, void *evdata)
{
    context_t *c = evdata;
    if (FBUF_DATA_LEN(c->res) > 0) {
        delete_event(loop, fd, EV_RDABLE);
        return;
    }

    int rc = recv(fd, FBUF_WRITE_AT(c->res), BUFSIZE, 0);
    if (rc < 0) {
        if (errno == EAGAIN) {
                return;
        }
        LOG_FOR_DEBUG("recv() failed form remote %d: %s", fd, strerror(errno));
        context_pool_release(c->pool, c, MASK_CLIENT|MASK_REMOTE);
        return;
    }
    if (rc == 0) {
        LOG_FOR_DEBUG("remote %d Connection closed", fd);
        context_pool_release(c->pool, c, MASK_REMOTE|MASK_CLIENT);
        return;
    }

    FBUF_COMMIT_WRITE(c->res, rc);
    
    fcrypt_encrypt(c->crypto, c->res);
    
    delete_event(loop, fd, EV_RDABLE);
    create_event(loop, c->client_fd, EV_WRABLE, &client_writable_cb, c);
}
