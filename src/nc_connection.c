/*
 * twemproxy - A fast and lightweight proxy for memcached protocol.
 * Copyright (C) 2011 Twitter, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/uio.h>

#include <nc_core.h>
#include <nc_server.h>
#include <nc_client.h>
#include <nc_proxy.h>
#include <proto/nc_proto.h>

/*
 *                   nc_connection.[ch]
 *                Connection (struct conn)
 *                 +         +          +
 *                 |         |          |
 *                 |       Proxy        |
 *                 |     nc_proxy.[ch]  |
 *                 /                    \
 *              Client                Server
 *           nc_client.[ch]         nc_server.[ch]
 *
 * Nutcracker essentially multiplexes m client connections over n server
 * connections. Usually m >> n, so that nutcracker can pipeline requests
 * from several clients over a server connection and hence use the connection
 * bandwidth to the server efficiently
 *
 * Client and server connection maintain two fifo queues for requests:
 *
 * 1). in_q (imsg_q):  queue of incoming requests
 * 2). out_q (omsg_q): queue of outstanding (outgoing) requests
 *
 * Request received over the client connection are forwarded to the server by
 * enqueuing the request in the chosen server's in_q. From the client's
 * perspective once the request is forwarded, it is outstanding and is tracked
 * in the client's out_q (unless the request was tagged as noreply). The server
 * in turn picks up requests from its own in_q in fifo order and puts them on
 * the wire. Once the request is outstanding on the wire, and a response is
 * expected for it, the server keeps track of outstanding requests it in its
 * own out_q.
 *
 * The server's out_q enables us to pair a request with a response while the
 * client's out_q enables us to pair request and response in the order in
 * which they are received from the client.
 *
 *
 *      Clients                             Servers
 *                                    .
 *    in_q: <empty>                   .
 *    out_q: req11 -> req12           .   in_q:  req22
 *    (client1)                       .   out_q: req11 -> req21 -> req12
 *                                    .   (server1)
 *    in_q: <empty>                   .
 *    out_q: req21 -> req22 -> req23  .
 *    (client2)                       .
 *                                    .   in_q:  req23
 *                                    .   out_q: <empty>
 *                                    .   (server2)
 *
 * In the above example, client1 has two pipelined requests req11 and req12
 * both of which are outstanding on the server connection server1. On the
 * other hand, client2 has three requests req21, req22 and req23, of which
 * only req21 is outstanding on the server connection while req22 and
 * req23 are still waiting to be put on the wire. The fifo of client's
 * out_q ensures that we always send back the response of request at the head
 * of the queue, before sending out responses of other completed requests in
 * the queue.
 */

static uint64_t ntotal_conn;       /* total # connections counter from start */
static uint32_t ncurr_conn;        /* current # connections */
static uint32_t ncurr_cconn;       /* current # client connections */

/*
 * Return the context associated with this connection.
 */
struct context *
conn_to_ctx(struct conn *conn)
{
    struct server_pool *pool;

    if (conn->notice) {
        struct thread_data *tdata;
        tdata = conn->owner;
        return tdata->ctx;
    }

    if (conn->source_type == NC_SOURCE_TYPE_PROXY) {
        struct manage *manager = conn->owner;
        return manager->ctx;
    }

    if (conn->proxy || conn->client) {
        pool = conn->owner;
    } else {
        struct server *server = conn->owner;
        pool = server->owner;
    }

    return pool->ctx;
}

static struct conn *
_conn_get(struct conn_base *cb)
{
    struct conn *conn;

    if (cb != NULL && !TAILQ_EMPTY(&cb->free_connq)) {
        ASSERT(cb->nfree_connq > 0);

        conn = TAILQ_FIRST(&cb->free_connq);
        cb->nfree_connq--;
        TAILQ_REMOVE(&cb->free_connq, conn, conn_tqe);
    } else {
        conn = nc_alloc(sizeof(*conn));
        if (conn == NULL) {
            return NULL;
        }
        conn->cb = cb;
    }

    conn->owner = NULL;

    conn->sd = -1;
    /* {family, addrlen, addr} are initialized in enqueue handler */

    TAILQ_INIT(&conn->imsg_q);
    TAILQ_INIT(&conn->omsg_q);
    conn->rmsg = NULL;
    conn->smsg = NULL;

    /*
     * Callbacks {recv, recv_next, recv_done}, {send, send_next, send_done},
     * {close, active}, parse, {ref, unref}, {enqueue_inq, dequeue_inq} and
     * {enqueue_outq, dequeue_outq} are initialized by the wrapper.
     */

    conn->send_bytes = 0;
    conn->recv_bytes = 0;

    conn->events = 0;
    conn->err = 0;
    conn->recv_active = 0;
    conn->recv_ready = 0;
    conn->send_active = 0;
    conn->send_ready = 0;

    conn->client = 0;
    conn->proxy = 0;
    conn->connecting = 0;
    conn->connected = 0;
    conn->eof = 0;
    conn->done = 0;
    conn->authenticated = 0;
    conn->notice = 0;
    conn->source_type = 0;

    if (cb != NULL) {
        cb->ntotal_conn++;
        cb->ncurr_conn++;
    }

    STATS_LOCK();
    ntotal_conn ++;
    ncurr_conn ++;
    STATS_UNLOCK();
    
    return conn;
}

struct conn *
conn_get(void *owner, bool client, unsigned source_type, struct conn_base *cb)
{
    struct conn *conn;

    conn = _conn_get(cb);
    if (conn == NULL) {
        return NULL;
    }

    /* this connection either handles redis or memcache messages */
    if (source_type == NC_SOURCE_TYPE_REDIS) {
        conn->source_type = NC_SOURCE_TYPE_REDIS;
    } else if (source_type == NC_SOURCE_TYPE_PROXY){
        conn->source_type = NC_SOURCE_TYPE_PROXY;
    } else if (source_type == NC_SOURCE_TYPE_MC){
        conn->source_type = NC_SOURCE_TYPE_MC;
    } else {
        NOT_REACHED();
    }

    conn->client = client ? 1 : 0;

    if (conn->client) {
        /*
         * client receives a request, possibly parsing it, and sends a
         * response downstream.
         */
        conn->recv = msg_recv;
        conn->recv_next = req_recv_next;
        conn->recv_done = req_recv_done;

        conn->send = msg_send;
        conn->send_next = rsp_send_next;
        conn->send_done = rsp_send_done;

        conn->close = client_close;
        conn->active = client_active;

        conn->ref = client_ref;
        conn->unref = client_unref;

        conn->enqueue_inq = NULL;
        conn->dequeue_inq = NULL;
        conn->enqueue_outq = req_client_enqueue_omsgq;
        conn->dequeue_outq = req_client_dequeue_omsgq;
        conn->post_connect = NULL;
        conn->swallow_msg = NULL;

        if (cb) cb->ncurr_cconn++;
        
        STATS_LOCK();
        ncurr_cconn ++;
        STATS_UNLOCK();
    } else {
        /*
         * server receives a response, possibly parsing it, and sends a
         * request upstream.
         */
        conn->recv = msg_recv;
        conn->recv_next = rsp_recv_next;
        conn->recv_done = rsp_recv_done;

        conn->send = msg_send;
        conn->send_next = req_send_next;
        conn->send_done = req_send_done;

        conn->close = server_close;
        conn->active = server_active;

        conn->ref = server_ref;
        conn->unref = server_unref;

        conn->enqueue_inq = req_server_enqueue_imsgq;
        conn->dequeue_inq = req_server_dequeue_imsgq;
        conn->enqueue_outq = req_server_enqueue_omsgq;
        conn->dequeue_outq = req_server_dequeue_omsgq;
        if (source_type == NC_SOURCE_TYPE_REDIS) {
          conn->post_connect = redis_post_connect;
          conn->swallow_msg = redis_swallow_msg;
        } else {
          conn->post_connect = memcache_post_connect;
          conn->swallow_msg = memcache_swallow_msg;
        }
    }

    if (source_type == NC_SOURCE_TYPE_PROXY) {
        
    }
    conn->ref(conn, owner);
    log_debug(LOG_VVERB, "get conn %p client %d", conn, conn->client);

    return conn;
}

struct conn *
conn_get_proxy(void *owner)
{
    struct server_pool *pool = owner;
    struct conn *conn;

    conn = _conn_get(NULL);
    if (conn == NULL) {
        return NULL;
    }

    if (pool->redis) {
        conn->source_type = NC_SOURCE_TYPE_REDIS;
    } else {
        conn->source_type = NC_SOURCE_TYPE_MC;
    }
    
    conn->proxy = 1;

    conn->recv = proxy_recv;
    conn->recv_next = NULL;
    conn->recv_done = NULL;

    conn->send = NULL;
    conn->send_next = NULL;
    conn->send_done = NULL;

    conn->close = proxy_close;
    conn->active = NULL;

    conn->ref = proxy_ref;
    conn->unref = proxy_unref;

    conn->enqueue_inq = NULL;
    conn->dequeue_inq = NULL;
    conn->enqueue_outq = NULL;
    conn->dequeue_outq = NULL;
    conn->cb = NULL;

    conn->ref(conn, owner);

    log_debug(LOG_VVERB, "get conn %p proxy %d", conn, conn->proxy);

    return conn;
}

struct conn *
conn_get_manage(void *owner)
{
    struct conn *conn;

    conn = _conn_get(NULL);
    if (conn == NULL) {
        return NULL;
    }

    conn->source_type = NC_SOURCE_TYPE_PROXY;
    
    conn->proxy = 1;

    conn->recv = proxy_recv;
    conn->recv_next = NULL;
    conn->recv_done = NULL;

    conn->send = NULL;
    conn->send_next = NULL;
    conn->send_done = NULL;

    conn->close = proxy_close;
    conn->active = NULL;

    conn->ref = manage_ref;
    conn->unref = manage_unref;

    conn->enqueue_inq = NULL;
    conn->dequeue_inq = NULL;
    conn->enqueue_outq = NULL;
    conn->dequeue_outq = NULL;
    conn->cb = NULL;

    conn->ref(conn, owner);

    log_debug(LOG_VVERB, "get conn %p proxy %d", conn, conn->proxy);

    return conn;
}

struct conn *
conn_get_notice(void *owner)
{
    struct conn *conn;

    conn = _conn_get(NULL);
    if (conn == NULL) {
        return NULL;
    }

    conn->notice = 1;

    conn->recv = notice_recv;
    conn->recv_next = NULL;
    conn->recv_done = NULL;

    conn->send = NULL;
    conn->send_next = NULL;
    conn->send_done = NULL;

    conn->close = NULL;
    conn->active = NULL;

    conn->ref = notice_ref;
    conn->unref = notice_unref;

    conn->enqueue_inq = NULL;
    conn->dequeue_inq = NULL;
    conn->enqueue_outq = NULL;
    conn->dequeue_outq = NULL;
    conn->cb = NULL;

    conn->ref(conn, owner);

    log_debug(LOG_VVERB, "get conn %p notice %d", conn, conn->notice);

    return conn;
}

static void
conn_free(struct conn *conn)
{
    log_debug(LOG_VVERB, "free conn %p", conn);
    nc_free(conn);
}

void
conn_put(struct conn *conn)
{
    struct conn_base *cb = conn->cb;
    ASSERT(conn->sd < 0);
    ASSERT(conn->owner == NULL);

    log_debug(LOG_VVERB, "put conn %p", conn);

    if (cb == NULL) {
        conn_free(conn);
        return;
    }

    cb->nfree_connq++;
    TAILQ_INSERT_HEAD(&cb->free_connq, conn, conn_tqe);

    if (conn->client) {
        cb->ncurr_cconn--;

        STATS_LOCK();
        ncurr_cconn --;
        STATS_UNLOCK();
    }
    cb->ncurr_conn--;

    STATS_LOCK();
    ncurr_conn --;
    STATS_UNLOCK();
}

void
conn_init(struct conn_base *cb)
{
    log_debug(LOG_DEBUG, "conn size %d", sizeof(struct conn));
    cb->nfree_connq = 0;
    cb->ntotal_conn = 0;
    cb->ncurr_cconn = 0;
    cb->ncurr_cconn = 0;
    TAILQ_INIT(&cb->free_connq);
}

void
conn_deinit(struct conn_base *cb)
{
    struct conn *conn, *nconn; /* current and next connection */

    for (conn = TAILQ_FIRST(&cb->free_connq); conn != NULL;
         conn = nconn, cb->nfree_connq--) {
        ASSERT(cb->nfree_connq > 0);
        nconn = TAILQ_NEXT(conn, conn_tqe);
        conn_free(conn);
    }
    ASSERT(cb->nfree_connq == 0);
}

int
nc_connect_init(struct context *ctx)
{
    ctx->cb = nc_alloc(sizeof(struct conn_base));
    if (ctx->cb == NULL) {
        return NC_ENOMEM;
    }

    conn_init(ctx->cb);
    return NC_OK;
}

void
nc_connect_deinit(struct context *ctx)
{
    if (ctx->cb != NULL) {
        conn_deinit(ctx->cb);
        ctx->cb = NULL;
    }
}

ssize_t
conn_recv(struct conn *conn, void *buf, size_t size)
{
    ssize_t n;

    ASSERT(buf != NULL);
    ASSERT(size > 0);
    ASSERT(conn->recv_ready);

    for (;;) {
        n = nc_read(conn->sd, buf, size);

        log_debug(LOG_VERB, "recv on sd %d %zd of %zu", conn->sd, n, size);

        if (n > 0) {
            if (n < (ssize_t) size) {
                conn->recv_ready = 0;
            }
            conn->recv_bytes += (size_t)n;
            return n;
        }

        if (n == 0) {
            conn->recv_ready = 0;
            conn->eof = 1;
            log_debug(LOG_INFO, "recv on sd %d eof rb %zu sb %zu", conn->sd,
                      conn->recv_bytes, conn->send_bytes);
            return n;
        }

        if (errno == EINTR) {
            log_debug(LOG_VERB, "recv on sd %d not ready - eintr", conn->sd);
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            conn->recv_ready = 0;
            log_debug(LOG_VERB, "recv on sd %d not ready - eagain", conn->sd);
            return NC_EAGAIN;
        } else {
            conn->recv_ready = 0;
            conn->err = errno;
            log_error("recv on sd %d failed: %s", conn->sd, strerror(errno));
            return NC_ERROR;
        }
    }

    NOT_REACHED();

    return NC_ERROR;
}

ssize_t
conn_sendv(struct conn *conn, struct array *sendv, size_t nsend)
{
    ssize_t n;

    ASSERT(array_n(sendv) > 0);
    ASSERT(nsend != 0);
    ASSERT(conn->send_ready);

    for (;;) {
        n = nc_writev(conn->sd, sendv->elem, sendv->nelem);

        log_debug(LOG_VERB, "sendv on sd %d %zd of %zu in %"PRIu32" buffers",
                  conn->sd, n, nsend, sendv->nelem);

        if (n > 0) {
            if (n < (ssize_t) nsend) {
                conn->send_ready = 0;
            }
            conn->send_bytes += (size_t)n;
            return n;
        }

        if (n == 0) {
            log_warn("sendv on sd %d returned zero", conn->sd);
            conn->send_ready = 0;
            return 0;
        }

        if (errno == EINTR) {
            log_debug(LOG_VERB, "sendv on sd %d not ready - eintr", conn->sd);
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            conn->send_ready = 0;
            log_debug(LOG_VERB, "sendv on sd %d not ready - eagain", conn->sd);
            return NC_EAGAIN;
        } else {
            conn->send_ready = 0;
            conn->err = errno;
            log_error("sendv on sd %d failed: %s", conn->sd, strerror(errno));
            return NC_ERROR;
        }
    }

    NOT_REACHED();

    return NC_ERROR;
}

uint32_t
conn_ncurr_conn()
{
    return ncurr_conn;
}

uint64_t
conn_ntotal_conn()
{
    return ntotal_conn;
}

uint32_t
conn_ncurr_cconn()
{
    return ncurr_cconn;
}

/*
 * Returns true if the connection is authenticated or doesn't require
 * authentication, otherwise return false
 */
bool
conn_authenticated(struct conn *conn)
{
    struct server_pool *pool;

    ASSERT(!conn->proxy);

    pool = conn->client ? conn->owner : ((struct server *)conn->owner)->owner;

    if (!pool->require_auth) {
        return true;
    }

    if (!conn->authenticated) {
        return false;
    }

    return true;
}
