
/*
 * Copyright (c) 2012 Roman Arutyunyan
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <strings.h>

#include "ngx_rtmp.h"
#include "ngx_rtmp_amf0.h"


static void ngx_rtmp_init_session(ngx_connection_t *c);

static void ngx_rtmp_handshake_recv(ngx_event_t *rev);
static void ngx_rtmp_handshake_send(ngx_event_t *rev);

static void ngx_rtmp_recv(ngx_event_t *rev);
static void ngx_rtmp_send(ngx_event_t *rev);
static ngx_int_t ngx_rtmp_receive_message(ngx_rtmp_session_t *s,
        ngx_rtmp_header_t *h, ngx_chain_t *in);

#ifdef NGX_DEBUG
static char*
ngx_rtmp_packet_type(uint8_t type) {
    static char* types[] = {
        "?",
        "chunk_size",
        "abort",
        "ack",
        "user",
        "ack_size",
        "bandwidth",
        "edge",
        "audio",
        "video",
        "?",
        "?",
        "?",
        "?",
        "?",
        "amf3_meta",
        "amf3_shared",
        "amd3_cmd",
        "amf0_meta",
        "amf0_shared",
        "amf0_cmd",
        "?",
        "aggregate"
    };

    return type < sizeof(types) / sizeof(types[0])
        ? types[type]
        : "?";
}
#endif

void
ngx_rtmp_init_connection(ngx_connection_t *c)
{
    ngx_uint_t             i;
    ngx_rtmp_port_t       *port;
    struct sockaddr       *sa;
    struct sockaddr_in    *sin;
    ngx_rtmp_log_ctx_t    *ctx;
    ngx_rtmp_in_addr_t    *addr;
    ngx_rtmp_session_t    *s;
    ngx_rtmp_addr_conf_t  *addr_conf;
#if (NGX_HAVE_INET6)
    struct sockaddr_in6   *sin6;
    ngx_rtmp_in6_addr_t   *addr6;
#endif


    /* find the server configuration for the address:port */

    /* AF_INET only */

    port = c->listening->servers;

    if (port->naddrs > 1) {

        /*
         * There are several addresses on this port and one of them
         * is the "*:port" wildcard so getsockname() is needed to determine
         * the server address.
         *
         * AcceptEx() already gave this address.
         */

        if (ngx_connection_local_sockaddr(c, NULL, 0) != NGX_OK) {
            ngx_rtmp_close_connection(c);
            return;
        }

        sa = c->local_sockaddr;

        switch (sa->sa_family) {

#if (NGX_HAVE_INET6)
        case AF_INET6:
            sin6 = (struct sockaddr_in6 *) sa;

            addr6 = port->addrs;

            /* the last address is "*" */

            for (i = 0; i < port->naddrs - 1; i++) {
                if (ngx_memcmp(&addr6[i].addr6, &sin6->sin6_addr, 16) == 0) {
                    break;
                }
            }

            addr_conf = &addr6[i].conf;

            break;
#endif

        default: /* AF_INET */
            sin = (struct sockaddr_in *) sa;

            addr = port->addrs;

            /* the last address is "*" */

            for (i = 0; i < port->naddrs - 1; i++) {
                if (addr[i].addr == sin->sin_addr.s_addr) {
                    break;
                }
            }

            addr_conf = &addr[i].conf;

            break;
        }

    } else {
        switch (c->local_sockaddr->sa_family) {

#if (NGX_HAVE_INET6)
        case AF_INET6:
            addr6 = port->addrs;
            addr_conf = &addr6[0].conf;
            break;
#endif

        default: /* AF_INET */
            addr = port->addrs;
            addr_conf = &addr[0].conf;
            break;
        }
    }

    s = ngx_pcalloc(c->pool, sizeof(ngx_rtmp_session_t));
    if (s == NULL) {
        ngx_rtmp_close_connection(c);
        return;
    }

    s->main_conf = addr_conf->ctx->main_conf;
    s->srv_conf = addr_conf->ctx->srv_conf;

    s->addr_text = &addr_conf->addr_text;

    c->data = s;
    s->connection = c;

    ngx_log_error(NGX_LOG_INFO, c->log, 0, "*%ui client connected",
                  c->number, &c->addr_text);

    ctx = ngx_palloc(c->pool, sizeof(ngx_rtmp_log_ctx_t));
    if (ctx == NULL) {
        ngx_rtmp_close_connection(c);
        return;
    }

    ctx->client = &c->addr_text;
    ctx->session = s;

    c->log->connection = c->number;
    c->log->handler = ngx_rtmp_log_error;
    c->log->data = ctx;
    c->log->action = "sending client greeting line";

    c->log_error = NGX_ERROR_INFO;

    ngx_rtmp_init_session(c);
}


static void
ngx_rtmp_init_session(ngx_connection_t *c)
{
    ngx_rtmp_session_t        *s;
    ngx_rtmp_core_srv_conf_t  *cscf;
    ngx_buf_t                 *b;
    size_t                     size;

    s = c->data;

    cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);

    s->ctx = ngx_pcalloc(c->pool, sizeof(void *) * ngx_rtmp_max_module);
    if (s->ctx == NULL) {
        ngx_rtmp_close_connection(c);
        return;
    }

    s->in_streams = ngx_pcalloc(c->pool, sizeof(ngx_rtmp_stream_t) 
            * cscf->max_streams);
    if (s->in_streams == NULL) {
        ngx_rtmp_close_connection(c);
        return;
    }
    
    s->in_chunk_size = NGX_RTMP_DEFAULT_CHUNK_SIZE;
    s->in_pool = ngx_create_pool(NGX_RTMP_HANDSHAKE_SIZE + 1
            + sizeof(ngx_pool_t), c->log);

    /* start handshake */
    b = &s->buf;
    size = NGX_RTMP_HANDSHAKE_SIZE + 1;
    b->start = b->pos = b->last = ngx_pcalloc(s->in_pool, size);
    b->end = b->start + size;
    b->temporary = 1;

    c->write->handler = ngx_rtmp_handshake_send;
    c->read->handler  = ngx_rtmp_handshake_recv;

    ngx_rtmp_handshake_recv(c->read);
}


void
ngx_rtmp_handshake_recv(ngx_event_t *rev)
{
    ssize_t                    n;
    ngx_connection_t          *c;
    ngx_rtmp_session_t        *s;
    ngx_rtmp_core_srv_conf_t  *cscf;
    ngx_buf_t                 *b;

    c = rev->data;
    s = c->data;
    cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);

    if (c->destroyed) {
        return;
    }

    if (rev->timedout) {
        ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT, "client timed out");
        c->timedout = 1;
        ngx_rtmp_close_connection(c);
        return;
    }

    if (rev->timer_set) {
        ngx_del_timer(rev);
    }

    b = &s->buf;

    while(b->last != b->end) {

        n = c->recv(c, b->last, b->end - b->last);

        if (n == NGX_ERROR || n == 0) {
            ngx_rtmp_close_connection(c);
            return;
        }

        if (n > 0) {
            if (b->last == b->start
                && s->hs_stage == 0 && *b->last != '\x03') 
            {
                ngx_log_error(NGX_LOG_INFO, c->log, NGX_ERROR, 
                        "invalid handshake signature");
                ngx_rtmp_close_connection(c);
                return;
            }
            b->last += n;
        }

        if (n == NGX_AGAIN) {
            ngx_add_timer(rev, cscf->timeout);
            if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
                ngx_rtmp_close_connection(c);
            }
            return;
        }
    }

    ngx_del_event(c->read, NGX_READ_EVENT, 0);

    if (s->hs_stage++ == 0) {
        ngx_rtmp_handshake_send(c->write);
        return;
    }

    /* handshake done */
    ngx_reset_pool(s->in_pool);

    c->read->handler =  ngx_rtmp_recv;
    c->write->handler = ngx_rtmp_send;

    ngx_log_debug0(NGX_LOG_DEBUG_RTMP, c->log, 0,
            "RTMP handshake done");

    ngx_rtmp_recv(rev);
}


void
ngx_rtmp_handshake_send(ngx_event_t *wev)
{
    ngx_int_t                  n;
    ngx_connection_t          *c;
    ngx_rtmp_session_t        *s;
    ngx_rtmp_core_srv_conf_t  *cscf;
    ngx_buf_t                 *b;

    c = wev->data;
    s = c->data;
    cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);

    if (c->destroyed) {
        return;
    }

    if (wev->timedout) {
        ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT, "client timed out");
        c->timedout = 1;
        ngx_rtmp_close_connection(c);
        return;
    }

    if (wev->timer_set) {
        ngx_del_timer(wev);
    }

    b = &s->buf;

restart:
    while(b->pos != b->last) {
        n = c->send(c, b->pos, b->last - b->pos);
        if (n > 0) {
            b->pos += n;
        }

        if (n == NGX_ERROR) {
            ngx_rtmp_close_connection(c);
            return;
        }

        if (n == NGX_AGAIN) {
            ngx_add_timer(c->write, cscf->timeout);
            if (ngx_handle_write_event(c->write, 0) != NGX_OK) {
                ngx_rtmp_close_connection(c);
                return;
            }
        }
    }

    if (s->hs_stage++ == 1) {
        b->pos = b->start + 1;
        goto restart;
    }

    b->pos = b->last = b->start + 1;
    ngx_del_event(wev, NGX_WRITE_EVENT, 0);
    ngx_rtmp_handshake_recv(c->read);
}


void
ngx_rtmp_recv(ngx_event_t *rev)
{
    ngx_int_t                   n;
    ngx_connection_t           *c;
    ngx_rtmp_session_t         *s;
    ngx_rtmp_core_srv_conf_t   *cscf;
    ngx_rtmp_header_t          *h;
    ngx_rtmp_stream_t          *st, *st0;
    ngx_chain_t                *in, *head;
    ngx_buf_t                  *b;
    u_char                     *p, *pp, *old_pos;
    size_t                      size, fsize, old_size;
    uint8_t                     fmt;
    uint32_t                    csid, timestamp;

    c = rev->data;
    s = c->data;
    b = NULL;
    old_pos = NULL;
    old_size = 0;
    cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);

    if (c->destroyed) {
        return;
    }

    for( ;; ) {

        st = &s->in_streams[s->in_csid];

        /* allocate new buffer */
        if (st->in == NULL) {

            if ((st->in = ngx_alloc_chain_link(s->in_pool)) == NULL
                || (st->in->buf = ngx_calloc_buf(s->in_pool)) == NULL) 
            {
                ngx_log_error(NGX_LOG_INFO, c->log, NGX_ERROR, 
                        "chain alloc failed");
                ngx_rtmp_close_connection(c);
                return;
            }

            st->in->next = NULL;
            b = st->in->buf;
            size = s->in_chunk_size + NGX_RTMP_MAX_CHUNK_HEADER;

            b->start = b->last = b->pos = ngx_palloc(s->in_pool, size);
            if (b->start == NULL) {
                ngx_log_error(NGX_LOG_INFO, c->log, NGX_ERROR, 
                        "buf alloc failed");
                ngx_rtmp_close_connection(c);
                return;
            }
            b->end = b->start + size;
        }

        h  = &st->hdr;
        in = st->in;
        b  = in->buf;

        if (old_size) {

            ngx_log_debug1(NGX_LOG_DEBUG_RTMP, c->log, 0,
                    "reusing formerly read data: %d", old_size);
#if 0
            /* DEBUG! */
            {
               size_t i;
               for(i = 0; i < 16 && i < old_size; ++i) {
                   ngx_log_debug1(NGX_LOG_DEBUG_RTMP, c->log, 0, "reuse %d", (int)*(old_pos + i));
               }
            }
#endif
            b->pos = b->start;
            b->last = ngx_movemem(b->pos, old_pos, old_size);

        } else { 

            if (old_pos) {
                b->pos = b->last = b->start;
            }

            n = c->recv(c, b->last, b->end - b->last);

            if (n == NGX_ERROR || n == 0) {
                ngx_rtmp_close_connection(c);
                return;
            }

            if (n == NGX_AGAIN) {
                if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
                    ngx_rtmp_close_connection(c);
                }
                return;
            }

            b->last += n;
        }

        old_pos = NULL;
        old_size = 0;

        /* parse headers */
        if (b->pos == b->start) {
            p = b->pos;

            /* chunk basic header */
            fmt  = (*p >> 6) & 0x03;
            csid = *p++ & 0x3f;

            if (csid == 0) {
                if (b->last - p < 1)
                    continue;
                csid = 64;
                csid += *(uint8_t*)p++;

            } else if (csid == 1) {
                if (b->last - p < 2)
                    continue;
                csid = 64;
                csid += *(uint8_t*)p++;
                csid += (uint32_t)256 * (*(uint8_t*)p++);
            }

            ngx_log_debug2(NGX_LOG_DEBUG_RTMP, c->log, 0,
                    "RTMP bheader fmt=%d csid=%D",
                    (int)fmt, csid);

            if (csid >= cscf->max_streams) {
                ngx_log_error(NGX_LOG_INFO, c->log, NGX_ERROR,
                    "RTMP chunk stream too big: %D >= %D",
                    csid, cscf->max_streams);
                ngx_rtmp_close_connection(c);
                return;
            }

            /* link orphan */
            if (s->in_csid == 0) {

                /* unlink from stream #0 */
                st->in = st->in->next;

                /* link to new stream */
                s->in_csid = csid;
                st = &s->in_streams[csid];
                if (st->in == NULL) {
                    in->next = in;
                } else {
                    in->next = st->in->next;
                    st->in->next = in;
                }
                st->in = in;
                h = &st->hdr;
                h->csid = csid;
            }

            /* get previous header to inherit data from */
            timestamp = h->timestamp;

            if (fmt <= 2 ) {
                if (b->last - p < 3)
                    continue;
                /* timestamp: 
                 *  big-endian 3b -> little-endian 4b */
                pp = (u_char*)&timestamp;
                pp[2] = *p++;
                pp[1] = *p++;
                pp[0] = *p++;
                pp[3] = 0;

                if (fmt <= 1) {
                    if (b->last - p < 4)
                        continue;
                    /* size:
                     *  big-endian 3b -> little-endian 4b 
                     * type:
                     *  1b -> 1b*/
                    pp = (u_char*)&h->mlen;
                    pp[2] = *p++;
                    pp[1] = *p++;
                    pp[0] = *p++;
                    pp[3] = 0;
                    h->type = *(uint8_t*)p++;

                    if (fmt == 0) {
                        if (b->last - p < 4)
                            continue;
                        /* stream:
                         *  little-endian 4b -> little-endian 4b */
                        pp = (u_char*)&h->msid;
                        pp[0] = *p++;
                        pp[1] = *p++;
                        pp[2] = *p++;
                        pp[3] = *p++;
                    }
                }

                /* extended header */
                if (timestamp == 0x00ffffff) {
                    if (b->last - p < 4)
                        continue;
                    pp = (u_char*)&h->timestamp;
                    pp[3] = *p++;
                    pp[2] = *p++;
                    pp[1] = *p++;
                    pp[0] = *p++;
                } else if (fmt) {
                    h->timestamp += timestamp;
                } else {
                    h->timestamp = timestamp;
                }
            }

            ngx_log_debug6(NGX_LOG_DEBUG_RTMP, c->log, 0,
                    "RTMP mheader %s (%d) "
                    "timestamp=%D mlen=%D len=%D msid=%D",
                    ngx_rtmp_packet_type(h->type), (int)h->type,
                    h->timestamp, h->mlen, st->len, h->msid);

            /* header done */
            b->pos = p;
        }

        size = b->last - b->pos;
        fsize = h->mlen - st->len;

        if (size < ngx_min(fsize, s->in_chunk_size)) 
            continue;

        /* buffer is ready */

        if (fsize > s->in_chunk_size) {
            /* collect fragmented chunks */
            st->len += s->in_chunk_size;

            old_pos = b->pos + s->in_chunk_size;
            old_size = size - s->in_chunk_size;

        } else {
            /* handle! */
            head = st->in->next;
            st->in->next = NULL;
            old_pos = b->pos + fsize;
            old_size = size - fsize;
            st->len = 0;

            if (ngx_rtmp_receive_message(s, h, head) != NGX_OK) {
                ngx_rtmp_close_connection(c);
                return;
            }

            /* add used bufs to stream #0 */
            st0 = &s->in_streams[0];
            st->in->next = st0->in;
            st0->in = head;
            st->in = NULL;
        }

        s->in_csid = 0;
    }
}


#define ngx_rtmp_buf_addref(b) \
    (++*(int*)&((b)->tag))


#define ngx_rtmp_buf_release(b) \
    (--*(int*)&((b)->tag))


static void
ngx_rtmp_send(ngx_event_t *wev)
{
    ngx_connection_t           *c;
    ngx_rtmp_session_t         *s;
    ngx_rtmp_core_srv_conf_t   *cscf;
    ngx_chain_t                *out, *l;

    c = wev->data;
    s = c->data;
    cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);

    if (c->destroyed) {
        return;
    }

    if (wev->timedout) {
        ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT, 
                "client timed out");
        c->timedout = 1;
        ngx_rtmp_close_connection(c);
        return;
    }

    if (wev->timer_set) {
        ngx_del_timer(wev);
    }

    while (s->out) {
        out = c->send_chain(c, s->out, 0);

        if (out == NGX_CHAIN_ERROR) {
            ngx_rtmp_close_connection(c);
            return;
        }

        if (out == s->out 
                && s->out->buf->pos == s->out->buf->last) 
        {
            cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);
            ngx_add_timer(c->write, cscf->timeout);
            if (ngx_handle_write_event(c->write, 0) != NGX_OK) {
                ngx_rtmp_close_connection(c);
            }
            return;
        }

        while (s->out) {

            l = s->out;
            if (l->buf->pos < l->buf->last) {
                break;
            }

            s->out = s->out->next;
            l->next = NULL;

            /* anyone still using this buffer? */
            if (ngx_rtmp_buf_release(l->buf)) {
                ngx_log_debug0(NGX_LOG_DEBUG_RTMP, c->log, 0,
                        "keeping shared buffer");
                continue;
            }

            /* return buffer to core */
            if (ngx_rtmp_release_shared_buf(s, l)) {
                ngx_rtmp_close_connection(c);
                return;
            }

            if (s->out == out) {
                break;
            }
        }
    }

    ngx_del_event(wev, NGX_WRITE_EVENT, 0);
}


ngx_chain_t * 
ngx_rtmp_alloc_shared_buf(ngx_rtmp_session_t *s)
{
    ngx_chain_t               *out;
    ngx_buf_t                 *b;
    size_t                     size;
    ngx_rtmp_core_srv_conf_t  *cscf;
    ngx_connection_t           *c;

    c = s->connection;
    cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);
    
    if (cscf->out_free) {
        out = cscf->out_free;
        cscf->out_free = out->next;

        ngx_log_debug0(NGX_LOG_DEBUG_RTMP, c->log, 0,
                "reuse shared buf");

    } else {

        ngx_log_debug0(NGX_LOG_DEBUG_RTMP, c->log, 0,
                "alloc shared buf");
        
        out = ngx_alloc_chain_link(cscf->out_pool);
        if (out == NULL) {
            return NULL;
        }

        out->buf = ngx_calloc_buf(cscf->out_pool);
        if (out->buf == NULL) {
            return NULL;
        }

        size = cscf->out_chunk_size + NGX_RTMP_MAX_CHUNK_HEADER;

        b = out->buf;
        b->start = ngx_palloc(cscf->out_pool, size);
        b->end = b->start + size;
    }

    out->next = NULL;
    b = out->buf;
    b->pos = b->last = b->start + NGX_RTMP_MAX_CHUNK_HEADER;
    b->tag = (ngx_buf_tag_t)0;
    b->memory = 1;

    return out;
}


ngx_int_t 
ngx_rtmp_release_shared_buf(ngx_rtmp_session_t *s,
        ngx_chain_t *out)
{
    ngx_rtmp_core_srv_conf_t   *cscf;
    size_t                      nbufs;
    ngx_chain_t                *cl;

    cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);

    for(nbufs = 1, cl = out; cl->next; 
                cl = cl->next, ++nbufs);

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
            "release %d shared bufs", nbufs);

    cl->next = cscf->out_free;
    cscf->out_free = out;

    return NGX_OK;
}


void 
ngx_rtmp_prepare_message(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h, 
        ngx_chain_t *out, uint8_t fmt)
{
    ngx_chain_t            *l;
    u_char                 *p, *pp;
    ngx_int_t               hsize, thsize, nbufs;
    uint32_t                mlen, timestamp, ext_timestamp;
    static uint8_t          hdrsize[] = { 12, 8, 4, 1 };
    u_char                  th[3];

    /* detect packet size */
    mlen = 0;
    nbufs = 0; 
    for(l = out; l; l = l->next) {
        mlen += (l->buf->last - l->buf->pos);
        ++nbufs;
    }

    ngx_log_debug8(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
            "RTMP prep %s (%d) fmt=%d csid=%D timestamp=%D "
            "mlen=%D msid=%D nbufs=%d",
            ngx_rtmp_packet_type(h->type), (int)h->type, (int)fmt,
            h->csid, h->timestamp, mlen, h->msid, nbufs);

    /* determine initial header size */
    hsize = hdrsize[fmt];

    if (h->timestamp >= 0x00ffffff) {
        timestamp = 0x00ffffff;
        ext_timestamp = h->timestamp;
        hsize += 4;
    } else {
        timestamp = h->timestamp;
        ext_timestamp = 0;
    }

    if (h->csid >= 64) {
        ++hsize;
        if (h->csid >= 320) {
            ++hsize;
        }
    }

    /* fill initial header */
    out->buf->pos -= hsize;
    p = out->buf->pos;

    /* basic header */
    *p = (fmt << 6);
    if (h->csid >= 2 && h->csid <= 63) {
        *p++ |= (((uint8_t)h->csid) & 0x3f);
    } else if (h->csid >= 64 && h->csid < 320) {
        ++p;
        *p++ = (uint8_t)(h->csid - 64);
    } else {
        *p++ |= 1;
        *p++ = (uint8_t)(h->csid - 64);
        *p++ = (uint8_t)((h->csid - 64) >> 8);
    }

    /* create fmt3 header for successive fragments */
    thsize = p - out->buf->pos;
    ngx_memcpy(th, out->buf->pos, thsize);
    th[0] |= 0xc0;

    /* message header */
    if (fmt <= 2) {
        pp = (u_char*)&timestamp;
        *p++ = pp[2];
        *p++ = pp[1];
        *p++ = pp[0];
        if (fmt <= 1) {
            pp = (u_char*)&mlen;
            *p++ = pp[2];
            *p++ = pp[1];
            *p++ = pp[0];
            *p++ = h->type;
            if (fmt == 0) {
                pp = (u_char*)&h->msid;
                *p++ = pp[0];
                *p++ = pp[1];
                *p++ = pp[2];
                *p++ = pp[3];
            }
        }
    }

    /* extended header */
    if (ext_timestamp) {
        pp = (u_char*)&ext_timestamp;
        *p++ = pp[3];
        *p++ = pp[2];
        *p++ = pp[1];
        *p++ = pp[0];
    }

    /* append headers to successive fragments */
    for(out = out->next; out; out = out->next) {
        out->buf->pos -= thsize;
        ngx_memcpy(out->buf->pos, th, thsize);
    }
}


ngx_int_t
ngx_rtmp_send_message(ngx_rtmp_session_t *s, ngx_chain_t *out)
{
    ngx_chain_t        *l, **ll;
    size_t              nbytes, nbufs;
    ngx_connection_t   *c;

    c = s->connection;
    nbytes = 0;
    nbufs = 0;

    for(l = out; l; l = l->next) {
        ngx_rtmp_buf_addref(l->buf);
        nbytes += (l->buf->last - l->buf->pos);
        ++nbufs;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
            "RTMP send nbytes=%d, nbufs=%d",
            nbytes, nbufs);

    /* TODO: optimize lookup */
    /* TODO: implement dropper */
    for(ll = &s->out; *ll; ll = &(*ll)->next);
    *ll = out;

    ngx_rtmp_send(s->connection->write);

    return c->destroyed ? NGX_ERROR : NGX_OK;
}


static ngx_int_t 
ngx_rtmp_receive_message(ngx_rtmp_session_t *s,
        ngx_rtmp_header_t *h, ngx_chain_t *in)
{
    ngx_rtmp_core_main_conf_t  *cmcf;
    ngx_array_t                *evhs;
    size_t                      n;
    ngx_rtmp_event_handler_pt  *evh;
    ngx_connection_t           *c;

    c = s->connection;
    cmcf = ngx_rtmp_get_module_main_conf(s, ngx_rtmp_core_module);

#ifdef NGX_DEBUG
    {
        int             nbufs;
        ngx_chain_t    *ch;

        for(nbufs = 1, ch = in; 
                ch->next; 
                ch = ch->next, ++nbufs);

        ngx_log_debug7(NGX_LOG_DEBUG_RTMP, c->log, 0,
                "RTMP recv %s (%d) csid=%D timestamp=%D "
                "mlen=%D msid=%D nbufs=%d",
                ngx_rtmp_packet_type(h->type), (int)h->type, 
                h->csid, h->timestamp, h->mlen, h->msid, nbufs);
    }
#endif

    if (h->type >= NGX_RTMP_MSG_MAX) {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, c->log, 0,
                "unexpected RTMP message type: %d", (int)h->type);
        return NGX_OK;
    }

    evhs = &cmcf->events[h->type];
    evh = evhs->elts;

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, c->log, 0,
            "nhandlers: %d", evhs->nelts);

    for(n = 0; n < evhs->nelts; ++n, ++evh) {
        if (!evh) {
            continue;
        }
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, c->log, 0,
                "calling handler %d", n);
           
        if ((*evh)(s, h, in) != NGX_OK) {
            ngx_log_debug1(NGX_LOG_DEBUG_RTMP, c->log, 0,
                    "handler %d failed", n);
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


void
ngx_rtmp_close_connection(ngx_connection_t *c)
{
    ngx_rtmp_session_t                 *s;
    ngx_pool_t                         *pool;
    ngx_rtmp_core_main_conf_t          *cmcf;
    ngx_rtmp_disconnect_handler_pt     *h;
    size_t                              n;

    if (c->destroyed) {
        return;
    }

    s = c->data;
    cmcf = ngx_rtmp_get_module_main_conf(s, ngx_rtmp_core_module);

    ngx_log_debug0(NGX_LOG_DEBUG_RTMP, c->log, 0, "close connection");

    if (s) {
        h = cmcf->disconnect.elts;
        for(n = 0; n < cmcf->disconnect.nelts; ++n, ++h) {
            if (*h) {
                (*h)(s);
            }
        }

        if (s->in_pool) {
            ngx_destroy_pool(s->in_pool);
        }
    }

    c->destroyed = 1;
    pool = c->pool;
    ngx_close_connection(c);
    ngx_destroy_pool(pool);
}


u_char *
ngx_rtmp_log_error(ngx_log_t *log, u_char *buf, size_t len)
{
    u_char              *p;
    ngx_rtmp_session_t  *s;
    ngx_rtmp_log_ctx_t  *ctx;

    if (log->action) {
        p = ngx_snprintf(buf, len, " while %s", log->action);
        len -= p - buf;
        buf = p;
    }

    ctx = log->data;

    p = ngx_snprintf(buf, len, ", client: %V", ctx->client);
    len -= p - buf;
    buf = p;

    s = ctx->session;

    if (s == NULL) {
        return p;
    }

    p = ngx_snprintf(buf, len, ", server: %V", s->addr_text);
    len -= p - buf;
    buf = p;

    return p;
}
