#include <nginx.h>

#include "http_response_parser.h"
#include "ngx_stream_upstream_check_module.h"


/* ngx_spinlock is defined without a matching unlock primitive */
#define ngx_spinlock_unlock(lock)      \
    (void) ngx_atomic_cmp_set(lock, ngx_pid, 0)

static ngx_int_t ngx_stream_check_get_shm_name(ngx_str_t *shm_name,
    ngx_pool_t *pool);
static ngx_int_t ngx_stream_upstream_check_get_shm_name(ngx_str_t *shm_name, 
    ngx_pool_t *pool, ngx_uint_t generation);
static char *ngx_stream_upstream_check_init_shm(ngx_conf_t *cf, void *conf);
static ngx_int_t ngx_stream_upstream_check_init_shm_zone(ngx_shm_zone_t *shm_zone,
    void *data);
/*
static ngx_uint_t ngx_stream_upstream_check_add_dynamic_peer_shm(ngx_pool_t *pool,
    ngx_stream_upstream_check_srv_conf_t *ucscf, ngx_addr_t *peer_addr);
static void ngx_stream_upstream_check_clear_dynamic_peer_shm(
    ngx_stream_check_peer_shm_t *peer_shm);
*/
static ngx_int_t ngx_stream_check_init_process(ngx_cycle_t *cycle);

static void ngx_stream_check_peek_handler(ngx_event_t *event);

static void ngx_stream_check_send_handler(ngx_event_t *event);
static void ngx_stream_check_recv_handler(ngx_event_t *event);

static ngx_int_t ngx_stream_check_http_init(ngx_stream_check_peer_conf_t *peer_conf);
static ngx_int_t ngx_stream_check_http_parse(ngx_stream_check_peer_conf_t *peer_conf);
static void ngx_stream_check_http_reinit(ngx_stream_check_peer_conf_t *peer_conf);

static ngx_int_t ngx_stream_check_ssl_hello_init(ngx_stream_check_peer_conf_t *peer_conf);
static ngx_int_t ngx_stream_check_ssl_hello_parse(ngx_stream_check_peer_conf_t *peer_conf);
static void ngx_stream_check_ssl_hello_reinit(ngx_stream_check_peer_conf_t *peer_conf);

static ngx_int_t ngx_stream_check_mysql_init(ngx_stream_check_peer_conf_t *peer_conf);
static ngx_int_t ngx_stream_check_mysql_parse(ngx_stream_check_peer_conf_t *peer_conf);
static void ngx_stream_check_mysql_reinit(ngx_stream_check_peer_conf_t *peer_conf);

static ngx_int_t ngx_stream_check_pop3_init(ngx_stream_check_peer_conf_t *peer_conf);
static ngx_int_t ngx_stream_check_pop3_parse(ngx_stream_check_peer_conf_t *peer_conf);
static void ngx_stream_check_pop3_reinit(ngx_stream_check_peer_conf_t *peer_conf);

static ngx_int_t ngx_stream_check_imap_init(ngx_stream_check_peer_conf_t *peer_conf);
static ngx_int_t ngx_stream_check_imap_parse(ngx_stream_check_peer_conf_t *peer_conf);
static void ngx_stream_check_imap_reinit(ngx_stream_check_peer_conf_t *peer_conf);

static char *ngx_stream_upstream_check(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

static check_conf_t *ngx_stream_get_check_type_conf(ngx_str_t *str);

static void *ngx_stream_upstream_check_create_main_conf(ngx_conf_t *cf);
static char *ngx_stream_upstream_check_init_main_conf(ngx_conf_t *cf, void *conf);
static void *ngx_stream_upstream_check_create_srv_conf(ngx_conf_t *cf);
static char *ngx_stream_upstream_check_init_srv_conf(ngx_conf_t *cf, void *conf);

static char * ngx_stream_upstream_check_status(ngx_conf_t *cf, 
    ngx_command_t *cmd, void *conf);

static ngx_conf_bitmask_t  ngx_check_http_expect_alive_masks[] = {
    { ngx_string("http_2xx"), NGX_CHECK_HTTP_2XX },
    { ngx_string("http_3xx"), NGX_CHECK_HTTP_3XX },
    { ngx_string("http_4xx"), NGX_CHECK_HTTP_4XX },
    { ngx_string("http_5xx"), NGX_CHECK_HTTP_5XX },
    { ngx_null_string, 0 }
};

static ngx_command_t  ngx_stream_upstream_check_commands[] = {

    { ngx_string("check"),
      NGX_STREAM_UPS_CONF|NGX_CONF_1MORE,
      ngx_stream_upstream_check,
      NGX_STREAM_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("check_http_send"),
      NGX_STREAM_UPS_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_STREAM_SRV_CONF_OFFSET,
      offsetof(ngx_stream_upstream_check_srv_conf_t, send),
      NULL },

    { ngx_string("check_http_expect_alive"),
      NGX_STREAM_UPS_CONF|NGX_CONF_1MORE,
      ngx_conf_set_bitmask_slot,
      NGX_STREAM_SRV_CONF_OFFSET,
      offsetof(ngx_stream_upstream_check_srv_conf_t, code.status_alive),
      &ngx_check_http_expect_alive_masks },

    { ngx_string("check_shm_size"),
      NGX_STREAM_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_STREAM_MAIN_CONF_OFFSET,
      offsetof(ngx_stream_upstream_check_main_conf_t, check_shm_size),
      NULL },

    { ngx_string("stream_check_status"),
      NGX_STREAM_SRV_CONF|NGX_CONF_NOARGS,
      ngx_stream_upstream_check_status,
      0,
      0,
      NULL },

    ngx_null_command
};

static ngx_stream_module_t  ngx_stream_upstream_check_module_ctx = {
    NULL,                                       /* postconfiguration */

    ngx_stream_upstream_check_create_main_conf, /* create main configuration */
    ngx_stream_upstream_check_init_main_conf,   /* init main configuration */

    ngx_stream_upstream_check_create_srv_conf,  /* create server configuration */
    NULL,                                       /* merge server configuration */
};


ngx_module_t  ngx_stream_upstream_check_module = {
    NGX_MODULE_V1,
    &ngx_stream_upstream_check_module_ctx,        /* module context */
    ngx_stream_upstream_check_commands,           /* module directives */
    NGX_STREAM_MODULE,                            /* module type */
    NULL,                                         /* init master */
    NULL,                                         /* init module */
    ngx_stream_check_init_process,                /* init process */
    NULL,                                         /* init thread */
    NULL,                                         /* exit thread */
    NULL,                                         /* exit process */
    NULL,                                         /* exit master */
    NGX_MODULE_V1_PADDING
};


#define RANDOM "NGX_STREAM_CHECK_SSL_HELLO\n\n\n\n\n"

/* This is the SSLv3 CLIENT HELLO packet used in conjunction with the
 * check type of ssl_hello to ensure that the remote server speaks SSL.
 *
 * Check RFC 2246 (TLSv1.0) sections A.3 and A.4 for details.
 *
 * Some codes copy from HAProxy 1.4.1
 */
static const char sslv3_client_hello_pkt[] = {
	"\x16"                /* ContentType         : 0x16 = Hanshake           */
	"\x03\x00"            /* ProtocolVersion     : 0x0300 = SSLv3            */
	"\x00\x79"            /* ContentLength       : 0x79 bytes after this one */
	"\x01"                /* HanshakeType        : 0x01 = CLIENT HELLO       */
	"\x00\x00\x75"        /* HandshakeLength     : 0x75 bytes after this one */
	"\x03\x00"            /* Hello Version       : 0x0300 = v3               */
	"\x00\x00\x00\x00"    /* Unix GMT Time (s)   : filled with <now> (@0x0B) */
	RANDOM                /* Random   : must be exactly 28 bytes  */
	"\x00"                /* Session ID length   : empty (no session ID)     */
	"\x00\x4E"            /* Cipher Suite Length : 78 bytes after this one   */
	"\x00\x01" "\x00\x02" "\x00\x03" "\x00\x04" /* 39 most common ciphers :  */
	"\x00\x05" "\x00\x06" "\x00\x07" "\x00\x08" /* 0x01...0x1B, 0x2F...0x3A  */
	"\x00\x09" "\x00\x0A" "\x00\x0B" "\x00\x0C" /* This covers RSA/DH,       */
	"\x00\x0D" "\x00\x0E" "\x00\x0F" "\x00\x10" /* various bit lengths,      */
	"\x00\x11" "\x00\x12" "\x00\x13" "\x00\x14" /* SHA1/MD5, DES/3DES/AES... */
	"\x00\x15" "\x00\x16" "\x00\x17" "\x00\x18"
	"\x00\x19" "\x00\x1A" "\x00\x1B" "\x00\x2F"
	"\x00\x30" "\x00\x31" "\x00\x32" "\x00\x33"
	"\x00\x34" "\x00\x35" "\x00\x36" "\x00\x37"
	"\x00\x38" "\x00\x39" "\x00\x3A"
	"\x01"                /* Compression Length  : 0x01 = 1 byte for types   */
	"\x00"                /* Compression Type    : 0x00 = NULL compression   */
};


#define HANDSHAKE    0x16
#define SERVER_HELLO 0x02

static check_conf_t  ngx_check_types[] = {

    { NGX_STREAM_CHECK_TCP,
      "tcp",
      ngx_null_string,
      0,
      ngx_stream_check_peek_handler,
      ngx_stream_check_peek_handler,
      NULL,
      NULL,
      NULL,
      0 },

    { NGX_STREAM_CHECK_HTTP,
      "http",
      ngx_string("GET / HTTP/1.0\r\n\r\n"),
      NGX_CONF_BITMASK_SET | NGX_CHECK_HTTP_2XX | NGX_CHECK_HTTP_3XX,
      ngx_stream_check_send_handler,
      ngx_stream_check_recv_handler,
      ngx_stream_check_http_init,
      ngx_stream_check_http_parse,
      ngx_stream_check_http_reinit,
      1 },

    { NGX_STREAM_CHECK_SSL_HELLO,
      "ssl_hello",
      ngx_string(sslv3_client_hello_pkt),
      0,
      ngx_stream_check_send_handler,
      ngx_stream_check_recv_handler,
      ngx_stream_check_ssl_hello_init,
      ngx_stream_check_ssl_hello_parse,
      ngx_stream_check_ssl_hello_reinit,
      1 },

    { NGX_STREAM_CHECK_MYSQL,
      "mysql",
      ngx_null_string,
      0,
      ngx_stream_check_send_handler,
      ngx_stream_check_recv_handler,
      ngx_stream_check_mysql_init,
      ngx_stream_check_mysql_parse,
      ngx_stream_check_mysql_reinit,
      1 },

    { NGX_STREAM_CHECK_POP3,
      "pop3",
      ngx_null_string,
      0,
      ngx_stream_check_send_handler,
      ngx_stream_check_recv_handler,
      ngx_stream_check_pop3_init,
      ngx_stream_check_pop3_parse,
      ngx_stream_check_pop3_reinit,
      1 },

    { NGX_STREAM_CHECK_IMAP,
      "imap",
      ngx_null_string,
      0,
      ngx_stream_check_send_handler,
      ngx_stream_check_recv_handler,
      ngx_stream_check_imap_init,
      ngx_stream_check_imap_parse,
      ngx_stream_check_imap_reinit,
      1 },

    {0, "", ngx_null_string, 0, NULL, NULL, NULL, NULL, NULL, 0}
};


static ngx_uint_t ngx_stream_check_shm_generation = 0;
static ngx_stream_check_peers_conf_t *check_peers_ctx = NULL;


static char *
ngx_stream_upstream_check(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) 
{
    ngx_stream_upstream_check_srv_conf_t  *ucscf = conf;

    ngx_str_t   *value, s;
    ngx_uint_t   i, rise, fall;
    ngx_msec_t   interval, timeout;

    /*set default*/
    rise = 2;
    fall = 5;
    interval = 30000;
    timeout = 1000;

    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "type=", 5) == 0) {
            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            ucscf->check_type_conf = ngx_stream_get_check_type_conf(&s);

            if ( ucscf->check_type_conf == NULL) {
                goto invalid_check_parameter;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "interval=", 9) == 0) {
            s.len = value[i].len - 9;
            s.data = value[i].data + 9;

            interval = ngx_atoi(s.data, s.len);
            if (interval == (ngx_msec_t) NGX_ERROR) {
                goto invalid_check_parameter;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "timeout=", 8) == 0) {
            s.len = value[i].len - 8;
            s.data = value[i].data + 8;

            timeout = ngx_atoi(s.data, s.len);
            if (timeout == (ngx_msec_t) NGX_ERROR) {
                goto invalid_check_parameter;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "rise=", 5) == 0) {
            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            rise = ngx_atoi(s.data, s.len);
            if (rise == (ngx_uint_t) NGX_ERROR) {
                goto invalid_check_parameter;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "fall=", 5) == 0) {
            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            fall = ngx_atoi(s.data, s.len);
            if (fall == (ngx_uint_t) NGX_ERROR) {
                goto invalid_check_parameter;
            }

            continue;
        }

        goto invalid_check_parameter;
    }

    ucscf->check_interval = interval;
    ucscf->check_timeout = timeout;
    ucscf->fall_count = fall;
    ucscf->rise_count = rise;

    if (ucscf->check_type_conf == NULL) {
        s.len = sizeof("tcp") - 1;
        s.data =(u_char *) "tcp";

        ucscf->check_type_conf = ngx_stream_get_check_type_conf(&s);
    }

    return NGX_CONF_OK;

invalid_check_parameter:

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "invalid parameter \"%V\"", &value[i]);

    return NGX_CONF_ERROR;
}


check_conf_t *
ngx_stream_get_check_type_conf(ngx_str_t *str) 
{
    ngx_uint_t i;

    for (i = 0; ;i++) {

        if (ngx_check_types[i].type == 0) {
            break;
        }

        if (ngx_strncmp(str->data, (u_char *)ngx_check_types[i].name,
                        str->len) == 0) {
            return &ngx_check_types[i];
        }
    }

    return NULL;
}


ngx_uint_t
ngx_stream_check_add_peer(ngx_conf_t *cf, ngx_stream_upstream_srv_conf_t *uscf,
    ngx_addr_t *peer)
{
    ngx_stream_check_peer_conf_t           *peer_conf;
    ngx_stream_check_peers_conf_t          *peers_conf;
    ngx_stream_upstream_check_srv_conf_t   *ucscf;
    ngx_stream_upstream_check_main_conf_t  *ucmcf;

    if (uscf->srv_conf == NULL) {
        return NGX_ERROR;
    }

    ucscf = ngx_stream_conf_get_module_srv_conf(cf, 
                                            ngx_stream_upstream_check_module);
    ucmcf = ngx_stream_conf_get_module_main_conf(cf, 
                                            ngx_stream_upstream_check_module);

    peers_conf = ucmcf->peers_conf;

    peer_conf = ngx_array_push(&peers_conf->peers);
    if (peer_conf == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(peer_conf, sizeof(ngx_stream_check_peer_conf_t));

    peer_conf->index = peers_conf->peers.nelts - 1;
    peer_conf->conf = ucscf;
    peer_conf->peer = peer;

    return peer_conf->index;
}

/*
ngx_uint_t
ngx_stream_upstream_check_add_dynamic_peer(ngx_pool_t *pool,
    ngx_stream_upstream_srv_conf_t *us, ngx_addr_t *peer_addr)
{
    void                                   *elts;
    ngx_uint_t                              i, index;
    ngx_stream_check_peer_shm_t            *peer_shm;
    ngx_stream_check_peers_shm_t           *peers_shm;
    ngx_stream_check_peer_conf_t           *peer, *p, *np;
    ngx_stream_check_peers_conf_t          *peers;
    ngx_stream_upstream_check_srv_conf_t   *ucscf;
    ngx_stream_upstream_check_main_conf_t  *ucmcf;

    if (check_peers_ctx == NULL || us->srv_conf == NULL) {
        return NGX_ERROR;
    }

    ucscf = ngx_stream_conf_upstream_srv_conf(us, ngx_stream_upstream_check_module);

    if(ucscf->check_interval == 0) {
        return NGX_ERROR;
    }

    index = ngx_stream_upstream_check_add_dynamic_peer_shm(pool,
                                                           ucscf, peer_addr);
    if (index == (ngx_uint_t) NGX_ERROR) {
        return index;
    }

    peers_shm = check_peers_ctx->peers_shm;
    peer_shm = peers_shm->peers;

    ucmcf = ngx_stream_cycle_get_module_main_conf(ngx_cycle,
                                               ngx_stream_upstream_check_module);
    peers = ucmcf->peers;
    peer = NULL;

    p = peers->peers.elts;

    ngx_log_debug2(NGX_LOG_DEBUG_STREAM, pool->log, 0,
                   "stream upstream check add dynamic upstream: %p, n: %ui",
                   p, peers->peers.nelts);

    for (i = 0; i < peers->peers.nelts; i++) {

        ngx_log_debug3(NGX_LOG_DEBUG_STREAM, pool->log, 0,
                       "stream upstream check add [%ui], index=%ui, delete:%ud",
                       i, p[i].index, p[i].delete);

        if (p[i].delete) {
            p[i].delete = 0;
            peer = &p[i];
            break;
        }
    }

    if (peer == NULL) {

        elts = peers->peers.elts;

        peer = ngx_array_push(&peers->peers);
        if (peer == NULL) {
            return NGX_ERROR;
        }

        if (elts != peers->peers.elts) {

            ngx_log_error(NGX_LOG_INFO, pool->log, 0,
                          "stream upstream check add peer realloc memory");

            // reset all upstream peers' timers 
            p = elts;
            np = peers->peers.elts;

            for (i = 0; i < peers->peers.nelts - 1; i++) {

                if (p[i].delete) {
                    continue;
                }
                ngx_log_error(NGX_LOG_INFO, pool->log, 0,
                              "old peer: %p, new peer: %p,"
                              "old timer: %p, new timer: %p",
                              np[i].check_ev.data, &np[i],
                              &p[i].check_ev, &np[i].check_ev);

                ngx_stream_upstream_check_clear_peer(&p[i]);

                ngx_memzero(&np[i].pc, sizeof(ngx_peer_connection_t));
                np[i].check_data = NULL;
                np[i].pool = NULL;

                ngx_stream_upstream_check_add_timer(&np[i],
                                                  np[i].conf->check_type_conf,
                                                  0, pool->log);
            }
        }
    }

    ngx_memzero(peer, sizeof(ngx_stream_upstream_check_peer_t));

    peer->conf = ucscf;
    peer->index = index;
//    peer->upstream_name = &us->host; TODO:
    peer->peer = peer_addr;

    ngx_log_debug3(NGX_LOG_DEBUG_STREAM, pool->log, 0,
                   "stream upstream check add dynamic upstream: %V, "
                   "peer: %V, index: %ui",
                   &us->host, &peer_addr->name, index);

    peer->check_peer_addr = peer->peer_addr;

    peer->shm = &peer_shm[index];

    ngx_stream_upstream_check_add_timer(peer, ucscf->check_type_conf,
                                        0, pool->log);

    ngx_log_debug3(NGX_LOG_DEBUG_STREAM, pool->log, 0,
                   "stream upstream check add peer: %p, index: %ui",
                   peer, peer->index);

//    peers->checksum +=
//        ngx_murmur_hash2(peer_addr->name.data, peer_addr->name.len);

    return peer->index;
}


void
ngx_stream_upstream_check_delete_dynamic_peer(ngx_str_t *name,
    ngx_addr_t *peer_addr)
{
    ngx_uint_t                          i;
    ngx_stream_check_peer_conf_t       *peer, *chosen;
    ngx_stream_check_peers_conf_t      *peers;

    chosen = NULL;
    peers = check_peers_ctx;
    peer = peers->peers.elts;

    ngx_log_debug2(NGX_LOG_DEBUG_STREAM, ngx_cycle->log, 0,
                   "stream upstream check delete dynamic upstream: %p, n: %ui",
                   peer, peers->peers.nelts);

    ngx_log_debug2(NGX_LOG_DEBUG_STREAM, ngx_cycle->log, 0,
                   "stream upstream check delete dynamic upstream: %V, "
                   "peer: %V", name, &peer_addr->name);

    for (i = 0; i < peers->peers.nelts; i++) {
        if (peer[i].delete) {
            continue;
        }

        ngx_log_debug3(NGX_LOG_DEBUG_STREAM, ngx_cycle->log, 0,
                       "stream upstream check delete [%ui], index=%ui, addr:%V",
                       i, peer[i].index, &peer[i].peer->name);
TODO:
//        if (peer[i].upstream_name->len != name->len
//            || ngx_strncmp(peer[i].upstream_name->data,
//                           name->data, name->len) != 0) {
//            continue;
//        }

        if (peer[i].peer->socklen != peer_addr->socklen
            || ngx_memcmp(peer[i].peer->sockaddr, peer_addr->sockaddr,
                          peer_addr->socklen) != 0) {
            continue;
        }

        chosen = &peer[i];
        break;
    }

    if (chosen == NULL) {
        return;
    }

    ngx_log_debug3(NGX_LOG_DEBUG_STREAM, ngx_cycle->log, 0,
                   "stream upstream check delete peer: %p, index: %ui"
                   chosen, chosen->index);

    ngx_shmtx_lock(&chosen->shm->mutex);

    if (chosen->shm->owner == ngx_pid) {
        chosen->shm->owner = NGX_INVALID_PID;
    }
TODO:
    if (chosen->shm->ref <= 0 && chosen->shm->delete != PEER_DELETED) {
        ngx_stream_upstream_check_clear_dynamic_peer_shm(chosen->shm);
        chosen->shm->delete = PEER_DELETED;
    }
    ngx_shmtx_unlock(&chosen->shm->mutex);

    ngx_stream_upstream_check_clear_peer(chosen);
}


static ngx_uint_t
ngx_stream_upstream_check_add_dynamic_peer_shm(ngx_pool_t *pool,
    ngx_stream_upstream_check_srv_conf_t *ucscf, ngx_addr_t *peer_addr)
{
    ngx_int_t                             rc;
    ngx_uint_t                            i, index;
    ngx_slab_pool_t                      *shpool;
    ngx_stream_check_peer_shm_t          *peer_shm;
    ngx_stream_check_peers_shm_t         *peers_shm;

    if (check_peers_ctx == NULL) {
        return NGX_ERROR;
    }

    shpool = check_peers_ctx->shpool;
    peers_shm = check_peers_ctx->peers_shm;
    peer_shm = peers_shm->peers;
    index = NGX_ERROR;

    ngx_shmtx_lock(&shpool->mutex);

    for (i = 0; i < peers_shm->number; i++) {

        // TODO: lock the peer mutex 
        if (peer_shm[i].delete == PEER_DELETED) {
            continue;
        }

        // TODO: check the peer configure
        // Merge the duplicate peer 
        // check the peer configure by check_type and check_send 
        if (peer_addr->socklen == peer_shm[i].socklen &&
            ngx_memcmp(peer_addr->sockaddr, peer_shm[i].sockaddr,
                       peer_addr->socklen) == 0
            && peer_shm[i].checksum
               == ngx_murmur_hash2(ucscf->send.data, ucscf->send.len))
        {
            ngx_shmtx_unlock(&shpool->mutex);
            return i;
        }
    }

    for (i = 0; i < peers_shm->number; i++) {

        if (peer_shm[i].delete == PEER_DELETED) {
            peer_shm[i].delete = PEER_NORMAL;
            index = i;
            break;
        }
    }

    if (index == (ngx_uint_t) NGX_ERROR) {
        if (peers_shm->number >= peers_shm->max_number) {
            goto fail;
        }

        index = peers_shm->number++;
    }

    ngx_memzero(&peer_shm[index], sizeof(ngx_stream_check_peer_shm_t));

    peer_shm[index].socklen = peer_addr->socklen;
    peer_shm[index].sockaddr = ngx_slab_alloc_locked(shpool,
                                                     peer_shm->socklen);
    if (peer_shm[index].sockaddr == NULL) {
        goto fail;
    }

    ngx_memcpy(peer_shm[index].sockaddr, peer_addr->sockaddr,
               peer_addr->socklen);
TODO:
    rc = ngx_stream_upstream_check_init_shm_peer(&peer_shm[index], NULL,
                                               ucscf->default_down, pool,
                                               &peer_addr->name);
    if (rc != NGX_OK) {
        goto fail;
    }

    // Set tag to peer_shm
//    peer_shm[index].checksum = ngx_murmur_hash2(ucscf->send.data, ucscf->send.len);

    ngx_shmtx_unlock(&shpool->mutex);
    return index;

fail:

    ngx_shmtx_unlock(&shpool->mutex);
    return NGX_ERROR;
}


static void
ngx_stream_upstream_check_clear_dynamic_peer_shm(
    ngx_stream_check_peer_shm_t *peer_shm)
{
    if (check_peers_ctx == NULL) {
        return;
    }

    ngx_slab_free_locked(check_peers_ctx->shpool, peer_shm->sockaddr);
}
*/

ngx_uint_t 
ngx_stream_check_peer_down(ngx_uint_t index)
{
    ngx_stream_check_peer_conf_t     *peer_conf;

    if (check_peers_ctx == NULL || index >= check_peers_ctx->peers.nelts) {
        return 0;
    }

    peer_conf = check_peers_ctx->peers.elts;

    return (peer_conf[index].shm->down);
}


void 
ngx_stream_check_get_peer(ngx_uint_t index) 
{
    ngx_stream_check_peer_conf_t     *peer_conf;

    if (check_peers_ctx == NULL || index >= check_peers_ctx->peers.nelts) {
        return;
    }

    peer_conf = check_peers_ctx->peers.elts;

    ngx_spinlock(&peer_conf[index].shm->lock, ngx_pid, 1024);

    peer_conf[index].shm->access_count++;

    ngx_spinlock_unlock(&peer_conf[index].shm->lock);
}


void 
ngx_stream_check_free_peer(ngx_uint_t index) 
{
    ngx_stream_check_peer_conf_t     *peer_conf;

    if (check_peers_ctx == NULL || index >= check_peers_ctx->peers.nelts) {
        return;
    }

    peer_conf = check_peers_ctx->peers.elts;

    ngx_spinlock(&peer_conf[index].shm->lock, ngx_pid, 1024);

    ngx_spinlock_unlock(&peer_conf[index].shm->lock);
}


#define SHM_NAME_LEN 256


static ngx_int_t 
ngx_stream_check_get_shm_name(ngx_str_t *shm_name, ngx_pool_t *pool) 
{
    u_char    *last;

    shm_name->data = ngx_palloc(pool, SHM_NAME_LEN);
    if (shm_name->data == NULL) {
        return NGX_ERROR;
    }

    last = ngx_snprintf(shm_name->data, SHM_NAME_LEN, "%s#%ui",
           "ngx_stream_upstream_check_module", ngx_stream_check_shm_generation);

    shm_name->len = last - shm_name->data;

    return NGX_OK;
}


static ngx_shm_zone_t *
ngx_shared_memory_find(ngx_cycle_t *cycle, ngx_str_t *name, void *tag)
{
    ngx_uint_t        i;
    ngx_shm_zone_t   *shm_zone;
    ngx_list_part_t  *part;

    part = (ngx_list_part_t *) & (cycle->shared_memory.part);
    shm_zone = part->elts;

    for (i = 0; /* void */ ; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            shm_zone = part->elts;
            i = 0;
        }

        if (name->len != shm_zone[i].shm.name.len) {
            continue;
        }

        if (ngx_strncmp(name->data, shm_zone[i].shm.name.data, name->len)
            != 0)
        {
            continue;
        }

        if (tag != shm_zone[i].tag) {
            continue;
        }

        return &shm_zone[i];
    }

    return NULL;
}


static void 
ngx_stream_check_clean_event(ngx_stream_check_peer_conf_t *peer_conf) 
{
    ngx_connection_t            *c;

    c = peer_conf->pc.connection;

    ngx_log_debug2(NGX_LOG_DEBUG_TCP, c->log, 0, 
                   "tcp check clean event: index:%d, fd: %d", 
                   peer_conf->index, c->fd);

    ngx_close_connection(c);

    if (peer_conf->check_timeout_ev.timer_set) {
        ngx_del_timer(&peer_conf->check_timeout_ev);
    }

    peer_conf->state = NGX_STREAM_CHECK_ALL_DONE;

    if (peer_conf->check_data != NULL && peer_conf->reinit) {
        peer_conf->reinit(peer_conf);
    }

    ngx_spinlock(&peer_conf->shm->lock, ngx_pid, 1024);

    peer_conf->shm->owner = NGX_INVALID_PID;

    ngx_spinlock_unlock(&peer_conf->shm->lock);
}


static void 
ngx_stream_check_clear_all_events() 
{
    ngx_uint_t                        i;
    static ngx_flag_t                 has_cleared = 0;
    ngx_stream_check_peer_shm_t      *peer_shm;
    ngx_stream_check_peers_shm_t     *peers_shm;
    ngx_stream_check_peer_conf_t     *peer_conf;
    ngx_stream_check_peers_conf_t    *peers_conf;

    if (has_cleared || check_peers_ctx == NULL) {
        return;
    }

    has_cleared = 1;

    peers_conf = check_peers_ctx;
    peers_shm = peers_conf->peers_shm;

    peer_conf = peers_conf->peers.elts;
    peer_shm = peers_shm->peers;
    for (i = 0; i < peers_conf->peers.nelts; i++) {
        if (peer_conf[i].check_ev.timer_set) {
            ngx_del_timer(&peer_conf[i].check_ev);
        }
        if (peer_shm[i].owner == ngx_pid) {
            ngx_stream_check_clean_event(&peer_conf[i]);
        }
        if (peer_conf[i].pool != NULL) {
            ngx_destroy_pool(peer_conf[i].pool);
        }
    }
}


static ngx_int_t 
ngx_stream_check_need_exit() 
{
    if (ngx_terminate || ngx_exiting || ngx_quit) {
        ngx_stream_check_clear_all_events();
        return 1;
    }

    return 0;
}


static void 
ngx_stream_check_finish_handler(ngx_event_t *event) 
{
    if (ngx_stream_check_need_exit()) {
        return;
    }
}


static void 
ngx_stream_check_status_update(ngx_stream_check_peer_conf_t *peer_conf,
    ngx_int_t result) 
{
    ngx_stream_upstream_check_srv_conf_t   *ucscf;

    ucscf = peer_conf->conf;

    if (result) {
        peer_conf->shm->rise_count++; 
        peer_conf->shm->fall_count = 0; 
        if (peer_conf->shm->down
            && (peer_conf->shm->rise_count >= ucscf->rise_count)) {

            peer_conf->shm->down = 0; 
        } 
    } else {
        peer_conf->shm->rise_count = 0; 
        peer_conf->shm->fall_count++; 
        if (!peer_conf->shm->down
             && (peer_conf->shm->fall_count >= ucscf->fall_count)) {
            peer_conf->shm->down = 1; 
        }
    }

    peer_conf->shm->access_time = ngx_current_msec; 
}


static void 
ngx_stream_check_timeout_handler(ngx_event_t *event) 
{
    ngx_stream_check_peer_conf_t     *peer_conf;
    
    if (ngx_stream_check_need_exit()) {
        return;
    }

    peer_conf = event->data;

    ngx_log_error(NGX_LOG_ERR, event->log, 0,
                  "check time out with peer: %V ", &peer_conf->peer->name);

    ngx_stream_check_status_update(peer_conf, 0);
    ngx_stream_check_clean_event(peer_conf);
}


static void 
ngx_stream_check_peek_handler(ngx_event_t *event) 
{
    char                              buf[1];
    ngx_int_t                         n;
    ngx_err_t                         err;
    ngx_connection_t                 *c;
    ngx_stream_check_peer_conf_t     *peer_conf;

    if (ngx_stream_check_need_exit()) {
        return;
    }

    c = event->data;
    peer_conf = c->data;

    n = recv(c->fd, buf, 1, MSG_PEEK);

    err = ngx_socket_errno;

    ngx_log_debug2(NGX_LOG_DEBUG_TCP, c->log, err, 
                   "tcp check upstream recv(): %d, fd: %d",
                   n, c->fd);

    if (n >= 0 || err == NGX_EAGAIN) {
        ngx_stream_check_status_update(peer_conf, 1);
    } else {
        c->error = 1;
        ngx_stream_check_status_update(peer_conf, 0);
    }

    ngx_stream_check_clean_event(peer_conf);

    /* dummy */
    ngx_stream_check_finish_handler(event);
}


void
http_field(void *data, const signed char *field, 
    size_t flen, const signed char *value, size_t vlen)
{
#if (NGX_DEBUG)
    ngx_str_t str_field, str_value;

    str_field.data = (u_char *) field;
    str_field.len = flen;

    str_value.data = (u_char *) value;
    str_value.len = vlen;

    ngx_log_debug2(NGX_LOG_DEBUG_TCP, ngx_cycle->log, 0, 
                   "%V: %V", &str_field, &str_value);
#endif
}


void
http_version(void *data, const signed char *at, size_t length)
{
#if (NGX_DEBUG)
    ngx_str_t str;

    str.data = (u_char *) at;
    str.len = length;

    ngx_log_debug1(NGX_LOG_DEBUG_TCP, ngx_cycle->log, 0, 
                   "VERSION: \"%V\"", &str);
#endif
}


void
status_code(void *data, const signed char *at, size_t length)
{
    int                           code;
    ngx_stream_check_ctx         *ctx;
    http_response_parser         *hp;
    ngx_stream_check_peer_conf_t *peer_conf = data;

#if (NGX_DEBUG)
    ngx_str_t                  str;

    str.data = (u_char *) at;
    str.len = length;

    ngx_log_debug1(NGX_LOG_DEBUG_TCP, ngx_cycle->log, 0, 
                   "STATUS_CODE: \"%V\"", &str);
#endif

    ctx = peer_conf->check_data;
    hp = ctx->parser;

    code = ngx_atoi((u_char*)at, length);

    if (code >= 200 && code < 300) {
        hp->status_code_n = NGX_CHECK_HTTP_2XX;

    } else if (code >= 300 && code < 400) {
        hp->status_code_n = NGX_CHECK_HTTP_3XX;

    } else if (code >= 400 && code < 500) {
        hp->status_code_n = NGX_CHECK_HTTP_4XX;

    } else if (code >= 500 && code < 600) {
        hp->status_code_n = NGX_CHECK_HTTP_5XX;

    } else {
        hp->status_code_n = NGX_CHECK_HTTP_ERR;
    }
}


void
reason_phrase(void *data, const signed char *at, size_t length)
{
#if (NGX_DEBUG)
    ngx_str_t str;

    str.data = (u_char *) at;
    str.len = length;

    ngx_log_debug1(NGX_LOG_DEBUG_TCP, ngx_cycle->log, 0, 
                   "REASON_PHRASE: \"%V\"", &str);
#endif
}


void
header_done(void *data, const signed char *at, size_t length)
{

}


static void
check_http_response_parser_init(http_response_parser *hp, 
    void *data) 
{
    hp->data = data;
    hp->http_field = http_field;
    hp->http_version = http_version;
    hp->status_code = status_code;
    hp->status_code_n = 0;
    hp->reason_phrase = reason_phrase;
    hp->header_done = header_done;
    
    http_response_parser_init(hp);
}


static ngx_int_t 
ngx_stream_check_http_init(ngx_stream_check_peer_conf_t *peer_conf) 
{
    ngx_stream_check_ctx                  *ctx;
    ngx_stream_upstream_check_srv_conf_t  *ucscf;
    
    ctx = peer_conf->check_data;
    ucscf = peer_conf->conf;

    ctx->send.start = ctx->send.pos = (u_char *)ucscf->send.data;
    ctx->send.end = ctx->send.last = ctx->send.start + ucscf->send.len;

    ctx->recv.start = ctx->recv.pos = NULL;
    ctx->recv.end = ctx->recv.last = NULL;

    ctx->parser = ngx_pcalloc(peer_conf->pool, sizeof(http_response_parser));
    if (ctx->parser == NULL) {
        return NGX_ERROR;
    }

    check_http_response_parser_init(ctx->parser, peer_conf);

    return NGX_OK;
}


static ngx_int_t 
ngx_stream_check_http_parse(ngx_stream_check_peer_conf_t *peer_conf) 
{
    ssize_t                                n, offset, length;
    ngx_stream_check_ctx                  *ctx;
    http_response_parser                  *hp;
    ngx_stream_upstream_check_srv_conf_t  *ucscf;

    ucscf = peer_conf->conf;
    ctx = peer_conf->check_data;
    hp = ctx->parser;

    if ((ctx->recv.last - ctx->recv.pos) > 0) {
        offset = ctx->recv.pos - ctx->recv.start;
        length = ctx->recv.last - ctx->recv.start;

        n = http_response_parser_execute(hp, (signed char *)ctx->recv.start,
                                         length, offset);
        ctx->recv.pos = ctx->recv.start + n;

        if (http_response_parser_finish(hp) == -1) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "http parse error with peer: %V, recv data: %s", 
                          &peer_conf->peer->name, ctx->recv.start);
            return NGX_ERROR;
        }

        ngx_log_debug2(NGX_LOG_DEBUG_TCP, ngx_cycle->log, 0, 
                       "http_parse: hp->status_code_n: %d, conf: %d",
                       hp->status_code_n, ucscf->code.status_alive);

        if (hp->status_code_n == 0) {
            return NGX_AGAIN;

        } else if (hp->status_code_n & ucscf->code.status_alive) {
            return NGX_OK;

        } else {
            return NGX_ERROR;
        }

    } else {
        return NGX_AGAIN;
    }

    return NGX_OK;
}


static void 
ngx_stream_check_http_reinit(ngx_stream_check_peer_conf_t *peer_conf) 
{
    ngx_stream_check_ctx *ctx;

    ctx = peer_conf->check_data;

    ctx->send.pos = ctx->send.start;
    ctx->send.last = ctx->send.end;

    ctx->recv.pos = ctx->recv.last = ctx->recv.start;

    check_http_response_parser_init(ctx->parser, peer_conf);
}


static ngx_int_t 
ngx_stream_check_ssl_hello_init(ngx_stream_check_peer_conf_t *peer_conf) 
{
    ngx_stream_check_ctx                  *ctx;
    ngx_stream_upstream_check_srv_conf_t  *ucscf;
    
    ctx = peer_conf->check_data;
    ucscf = peer_conf->conf;

    ctx->send.start = ctx->send.pos = (u_char *)ucscf->send.data;
    ctx->send.end = ctx->send.last = ctx->send.start + ucscf->send.len;

    ctx->recv.start = ctx->recv.pos = NULL;
    ctx->recv.end = ctx->recv.last = NULL;

    return NGX_OK;
}


/* a rough check of server ssl_hello responses */
static ngx_int_t 
ngx_stream_check_ssl_hello_parse(ngx_stream_check_peer_conf_t *peer_conf) 
{
    size_t                           size;
    server_ssl_hello_t              *resp;
    ngx_stream_check_ctx            *ctx;

    ctx = peer_conf->check_data;

    size = ctx->recv.last - ctx->recv.pos;
    if (size < sizeof(server_ssl_hello_t)) {
        return NGX_AGAIN;
    } 

    resp = (server_ssl_hello_t *) ctx->recv.pos;

    ngx_log_debug7(NGX_LOG_DEBUG_TCP, ngx_cycle->log, 0, 
                   "tcp check ssl_parse, type: %d, version: %d.%d, "
                   "length: %d, handshanke_type: %d, "
                   "hello_version: %d.%d", 
                   resp->msg_type, resp->version.major, resp->version.minor, 
                   ntohs(resp->length), resp->handshake_type, 
                   resp->hello_version.major, resp->hello_version.minor);

    if (resp->msg_type != HANDSHAKE) {
        return NGX_ERROR;
    }

    if (resp->handshake_type != SERVER_HELLO) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static void 
ngx_stream_check_ssl_hello_reinit(ngx_stream_check_peer_conf_t *peer_conf) 
{
    ngx_stream_check_ctx *ctx;

    ctx = peer_conf->check_data;

    ctx->send.pos = ctx->send.start;
    ctx->send.last = ctx->send.end;

    ctx->recv.pos = ctx->recv.last = ctx->recv.start;
}


static ngx_int_t 
ngx_stream_check_mysql_init(ngx_stream_check_peer_conf_t *peer_conf) 
{
    ngx_stream_check_ctx                  *ctx;
    ngx_stream_upstream_check_srv_conf_t  *ucscf;
    
    ctx = peer_conf->check_data;
    ucscf = peer_conf->conf;

    ctx->send.start = ctx->send.pos = (u_char *)ucscf->send.data;
    ctx->send.end = ctx->send.last = ctx->send.start + ucscf->send.len;

    ctx->recv.start = ctx->recv.pos = NULL;
    ctx->recv.end = ctx->recv.last = NULL;

    return NGX_OK;
}


static ngx_int_t 
ngx_stream_check_mysql_parse(ngx_stream_check_peer_conf_t *peer_conf) 
{
    ngx_stream_check_ctx            *ctx;
    mysql_handshake_init_t          *handshake;

    ctx = peer_conf->check_data;

    if (ctx->recv.last - ctx->recv.pos <= 0 ) {
        return NGX_AGAIN;
    }

    handshake = (mysql_handshake_init_t *) ctx->recv.pos;

    ngx_log_debug3(NGX_LOG_DEBUG_TCP, ngx_cycle->log, 0, 
                   "mysql_parse: packet_number=%d, protocol=%d, server=%s", 
                   handshake->packet_number,
                   handshake->protocol_version,
                   handshake->others);

    /* The mysql greeting packet's serial number always begin with 0. */
    if (handshake->packet_number != 0x00) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static void
ngx_stream_check_mysql_reinit(ngx_stream_check_peer_conf_t *peer_conf) 
{
    ngx_stream_check_ctx *ctx;

    ctx = peer_conf->check_data;

    ctx->send.pos = ctx->send.start;
    ctx->send.last = ctx->send.end;

    ctx->recv.pos = ctx->recv.last = ctx->recv.start;
}


static ngx_int_t 
ngx_stream_check_pop3_init(ngx_stream_check_peer_conf_t *peer_conf) 
{
    ngx_stream_check_ctx                  *ctx;
    ngx_stream_upstream_check_srv_conf_t  *ucscf;
    
    ctx = peer_conf->check_data;
    ucscf = peer_conf->conf;

    ctx->send.start = ctx->send.pos = (u_char *)ucscf->send.data;
    ctx->send.end = ctx->send.last = ctx->send.start + ucscf->send.len;

    ctx->recv.start = ctx->recv.pos = NULL;
    ctx->recv.end = ctx->recv.last = NULL;

    return NGX_OK;
}


static ngx_int_t 
ngx_stream_check_pop3_parse(ngx_stream_check_peer_conf_t *peer_conf) 
{
    u_char                           ch;
    ngx_stream_check_ctx            *ctx;

    ctx = peer_conf->check_data;

    if (ctx->recv.last - ctx->recv.pos <= 0 ) {
        return NGX_AGAIN;
    }

    ch = *(ctx->recv.start);

    ngx_log_debug1(NGX_LOG_DEBUG_TCP, ngx_cycle->log, 0, 
                   "pop3_parse: packet_greeting \"%s\"", ctx->recv.start);

    /*
     * RFC 1939
     * There are currently two status indicators: positive ("+OK") and 
     * negative ("-ERR").  Servers MUST send the "+OK" and "-ERR" in upper case.
     */
    if (ch != '+') {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static void
ngx_stream_check_pop3_reinit(ngx_stream_check_peer_conf_t *peer_conf) 
{
    ngx_stream_check_ctx *ctx;

    ctx = peer_conf->check_data;

    ctx->send.pos = ctx->send.start;
    ctx->send.last = ctx->send.end;

    ctx->recv.pos = ctx->recv.last = ctx->recv.start;
}


static ngx_int_t 
ngx_stream_check_imap_init(ngx_stream_check_peer_conf_t *peer_conf) 
{
    ngx_stream_check_ctx                  *ctx;
    ngx_stream_upstream_check_srv_conf_t  *ucscf;
    
    ctx = peer_conf->check_data;
    ucscf = peer_conf->conf;

    ctx->send.start = ctx->send.pos = (u_char *)ucscf->send.data;
    ctx->send.end = ctx->send.last = ctx->send.start + ucscf->send.len;

    ctx->recv.start = ctx->recv.pos = NULL;
    ctx->recv.end = ctx->recv.last = NULL;

    return NGX_OK;
}


static ngx_int_t 
ngx_stream_check_imap_parse(ngx_stream_check_peer_conf_t *peer_conf) 
{
    u_char                          *p;
    ngx_stream_check_ctx            *ctx;

    ctx = peer_conf->check_data;

    if (ctx->recv.last - ctx->recv.pos <= 0 ) {
        return NGX_AGAIN;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_TCP, ngx_cycle->log, 0, 
                   "imap_parse: packet_greeting \"%s\"", ctx->recv.start);

    /* RFC 3501
     * command         = tag SP (command-any / command-auth / command-nonauth /
     * command-select) CRLF
     */

    p = ctx->recv.start;
    while (p < ctx->recv.last) {

        if (*p == ' ') {
            if ((p + 2) >= ctx->recv.last) {
                return NGX_AGAIN;
            }
            else if (*(p + 1) == 'O' && *(p + 2) == 'K') {
                return NGX_OK;
            }
            else {
                return NGX_ERROR;
            }
        }

        p++;
    }

    return NGX_AGAIN;
}


static void
ngx_stream_check_imap_reinit(ngx_stream_check_peer_conf_t *peer_conf) 
{
    ngx_stream_check_ctx *ctx;

    ctx = peer_conf->check_data;

    ctx->send.pos = ctx->send.start;
    ctx->send.last = ctx->send.end;

    ctx->recv.pos = ctx->recv.last = ctx->recv.start;
}


static void 
ngx_stream_check_send_handler(ngx_event_t *event) 
{
    ssize_t                           size;
    ngx_connection_t                 *c;
    ngx_stream_check_ctx             *ctx;
    ngx_stream_check_peer_conf_t     *peer_conf;

    if (ngx_stream_check_need_exit()) {
        return;
    }

    c = event->data;
    peer_conf = c->data;

    if (c->pool == NULL) {
        ngx_log_error(NGX_LOG_ERR, event->log, 0,
                      "check pool NULL with peer: %V ", &peer_conf->peer->name);

        goto check_send_fail;
    }

    if (peer_conf->state != NGX_STREAM_CHECK_CONNECT_DONE) {
        if (ngx_handle_write_event(c->write, 0) != NGX_OK) {
                goto check_send_fail;
        }

        return;
    }

    if (peer_conf->check_data == NULL) {

        peer_conf->check_data = ngx_pcalloc(c->pool, sizeof(ngx_stream_check_ctx));
        if (peer_conf->check_data == NULL) {
            goto check_send_fail;
        }

        if (peer_conf->init == NULL || peer_conf->init(peer_conf) != NGX_OK) {

            ngx_log_error(NGX_LOG_ERR, event->log, 0,
                          "check init error with peer: %V ",
                          &peer_conf->peer->name);

            goto check_send_fail;
        }
    }

    ctx = peer_conf->check_data;

    while (ctx->send.pos < ctx->send.last) {

        size = c->send(c, ctx->send.pos, ctx->send.last - ctx->send.pos);

#if (NGX_DEBUG)
        ngx_err_t                      err;

        err = (size >=0) ? 0 : ngx_socket_errno;
        ngx_log_debug2(NGX_LOG_DEBUG_TCP, c->log, err, 
                       "tcp check send size: %d, total: %d",
                       size, ctx->send.last - ctx->send.pos);
#endif

        if (size >= 0) {
            ctx->send.pos += size;

        } else if (size == NGX_AGAIN) {
            return;

        } else {
            c->error = 1;
            goto check_send_fail;
        }
    }

    if (ctx->send.pos == ctx->send.last) {
        ngx_log_debug0(NGX_LOG_DEBUG_TCP, c->log, 0, "tcp check send done.");
        peer_conf->state = NGX_STREAM_CHECK_SEND_DONE;
    }

    return;

check_send_fail:
    ngx_stream_check_status_update(peer_conf, 0);
    ngx_stream_check_clean_event(peer_conf);
    return;
}


static void 
ngx_stream_check_recv_handler(ngx_event_t *event) 
{
    ssize_t                           size, n, rc;
    u_char                           *new_buf;
    ngx_connection_t                 *c;
    ngx_stream_check_ctx             *ctx;
    ngx_stream_check_peer_conf_t     *peer_conf;

    if (ngx_stream_check_need_exit()) {
        return;
    }

    c = event->data;
    peer_conf = c->data;

    if (peer_conf->state != NGX_STREAM_CHECK_SEND_DONE) {

        if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
            goto check_recv_fail;
        }

        return;
    }

    ctx = peer_conf->check_data;

    if (ctx->recv.start == NULL) {
        /* 2048, is it enough? */
        ctx->recv.start = ngx_palloc(c->pool, ngx_pagesize/2);
        if (ctx->recv.start == NULL) {
            goto check_recv_fail;
        }

        ctx->recv.last = ctx->recv.pos = ctx->recv.start;
        ctx->recv.end = ctx->recv.start + ngx_pagesize/2;
    }

    while (1) {
        n = ctx->recv.end - ctx->recv.last;
        /* Not enough buffer? Enlarge twice */
        if (n == 0) {
            size = ctx->recv.end - ctx->recv.start;
            new_buf = ngx_palloc(c->pool, size * 2);
            if (new_buf == NULL) {
                goto check_recv_fail;
            }

            ngx_memcpy(new_buf, ctx->recv.start, size);

            ctx->recv.pos = ctx->recv.start = new_buf;
            ctx->recv.last = new_buf + size;
            ctx->recv.end = new_buf + size * 2;

            n = ctx->recv.end - ctx->recv.last;
        }

        size = c->recv(c, ctx->recv.last, n);

#if (NGX_DEBUG)
        ngx_err_t                      err;

        err = (size >= 0) ? 0 : ngx_socket_errno;
        ngx_log_debug2(NGX_LOG_DEBUG_TCP, c->log, err, 
                       "tcp check recv size: %d, peer: %V",
                       size, &peer_conf->peer->name);
#endif

        if (size > 0) {
            ctx->recv.last += size;
            continue;

        } else if (size == 0 || size == NGX_AGAIN) {
            break;

        } else {
            c->error = 1;
            goto check_recv_fail;
        }
    }

    rc = peer_conf->parse(peer_conf); 

    ngx_log_debug2(NGX_LOG_DEBUG_TCP, c->log, 0, 
                   "tcp check parse rc: %d, peer: %V",
                   rc, &peer_conf->peer->name);

    switch (rc) {

    case NGX_AGAIN:
        return;

    case NGX_ERROR:
        ngx_log_error(NGX_LOG_ERR, event->log, 0,
                      "check protocol %s error with peer: %V ", 
                      peer_conf->conf->check_type_conf->name,
                      &peer_conf->peer->name);

        ngx_stream_check_status_update(peer_conf, 0);
        break;

    case NGX_OK:
        /* pass throught */

    default:
        ngx_stream_check_status_update(peer_conf, 1);
    }

    peer_conf->state = NGX_STREAM_CHECK_RECV_DONE;
    ngx_stream_check_clean_event(peer_conf);

    return;

check_recv_fail:

    ngx_stream_check_status_update(peer_conf, 0);
    ngx_stream_check_clean_event(peer_conf);

    return;
}


static void 
ngx_stream_check_connect_handler(ngx_event_t *event) 
{
    ngx_int_t                               rc;
    ngx_connection_t                       *c;
    ngx_stream_check_peer_conf_t           *peer_conf;
    ngx_stream_upstream_check_srv_conf_t   *ucscf;

    if (ngx_stream_check_need_exit()) {
        return;
    }

    peer_conf = event->data;
    ucscf = peer_conf->conf;

    ngx_memzero(&peer_conf->pc, sizeof(ngx_peer_connection_t));

    peer_conf->pc.sockaddr = peer_conf->peer->sockaddr;
    peer_conf->pc.socklen = peer_conf->peer->socklen;
    peer_conf->pc.name = &peer_conf->peer->name;

    peer_conf->pc.get = ngx_event_get_peer;
    peer_conf->pc.log = event->log;
    peer_conf->pc.log_error = NGX_ERROR_ERR; 

    peer_conf->pc.cached = 0;
    peer_conf->pc.connection = NULL;

    rc = ngx_event_connect_peer(&peer_conf->pc);

    if (rc == NGX_ERROR || rc == NGX_DECLINED) {
        ngx_stream_check_status_update(peer_conf, 0);
        return;
    }

    /* NGX_OK or NGX_AGAIN */
    c = peer_conf->pc.connection;
    c->data = peer_conf;
    c->log = peer_conf->pc.log;
    c->sendfile = 0;
    c->read->log = c->log;
    c->write->log = c->log;
    c->pool = peer_conf->pool;

    peer_conf->state = NGX_STREAM_CHECK_CONNECT_DONE;

    c->write->handler = peer_conf->send_handler;
    c->read->handler = peer_conf->recv_handler;

    ngx_add_timer(&peer_conf->check_timeout_ev, ucscf->check_timeout);

    /* The kqueue's loop interface need it. */
    if (rc == NGX_OK) {
        c->write->handler(c->write);
    }
}


static void 
ngx_stream_check_begin_handler(ngx_event_t *event) 
{
    ngx_stream_check_peer_conf_t           *peer_conf;
    ngx_stream_upstream_check_srv_conf_t   *ucscf;

    if (ngx_stream_check_need_exit()) {
        return;
    }

    peer_conf = event->data;
    ucscf = peer_conf->conf;

    ngx_add_timer(event, ucscf->check_interval/2);

    /* This process are processing the event now. */
    if (peer_conf->shm->owner == ngx_pid) {
        return;
    }

    ngx_log_debug4(NGX_LOG_DEBUG_TCP, event->log, 0, 
                   "tcp check begin handler index:%ud, owner: %d, "
                   "ngx_pid: %ud, time:%d", 
                   peer_conf->index, peer_conf->shm->owner, ngx_pid, 
                   (ngx_current_msec - peer_conf->shm->access_time));

    ngx_spinlock(&peer_conf->shm->lock, ngx_pid, 1024);

    if (((ngx_current_msec - peer_conf->shm->access_time) >= ucscf->check_interval) && 
            peer_conf->shm->owner == NGX_INVALID_PID)
    {
        peer_conf->shm->owner = ngx_pid;
    }

    ngx_spinlock_unlock(&peer_conf->shm->lock);

    if (peer_conf->shm->owner == ngx_pid) {
        ngx_stream_check_connect_handler(event);
    }
}


static void *
ngx_stream_upstream_check_create_main_conf(ngx_conf_t *cf) 
{
    ngx_stream_upstream_check_main_conf_t  *ucmcf;

    ucmcf = ngx_pcalloc(cf->pool, sizeof(ngx_stream_upstream_check_main_conf_t));
    if (ucmcf == NULL) {
        return NULL;
    }

    ucmcf->peers_conf = ngx_pcalloc(cf->pool,
                                    sizeof(ngx_stream_check_peers_conf_t));
    if (ucmcf->peers_conf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&ucmcf->peers_conf->peers, cf->pool, 16,
                       sizeof(ngx_stream_check_peer_conf_t)) != NGX_OK)
    {
        return NULL;
    }

    return ucmcf;
}


static char *
ngx_stream_upstream_check_init_main_conf(ngx_conf_t *cf, void *conf) 
{
    ngx_uint_t                               i;
    ngx_stream_upstream_srv_conf_t         **uscfp;
    ngx_stream_upstream_main_conf_t         *umcf;

    umcf = ngx_stream_conf_get_module_main_conf(cf, ngx_stream_upstream_module);

    uscfp = umcf->upstreams.elts;

    for (i = 0; i < umcf->upstreams.nelts; i++) {

        ngx_stream_upstream_check_init_srv_conf(cf, uscfp[i]);
    }

    return ngx_stream_upstream_check_init_shm(cf, conf);
}


static void *
ngx_stream_upstream_check_create_srv_conf(ngx_conf_t *cf)
{
    ngx_stream_upstream_check_srv_conf_t  *ucscf;

    ucscf = ngx_pcalloc(cf->pool, sizeof(ngx_stream_upstream_check_srv_conf_t));
    if (ucscf == NULL) {
        return NULL;
    }

    ucscf->fall_count = NGX_CONF_UNSET_UINT;
    ucscf->rise_count = NGX_CONF_UNSET_UINT;
    ucscf->check_timeout = NGX_CONF_UNSET_MSEC;
    ucscf->check_interval = NGX_CONF_UNSET_MSEC;
    ucscf->check_type_conf = NGX_CONF_UNSET_PTR;

    return ucscf;
}


static char *
ngx_stream_upstream_check_init_srv_conf(ngx_conf_t *cf, void *conf)
{
    check_conf_t                         *check;
    ngx_stream_upstream_srv_conf_t       *us = conf;
    ngx_stream_upstream_check_srv_conf_t *ucscf;

    if (us->srv_conf == NULL) {
        return NGX_CONF_OK;
    }

    ucscf = ngx_stream_conf_upstream_srv_conf(us, ngx_stream_upstream_check_module);

    if (ucscf->fall_count == NGX_CONF_UNSET_UINT) {
        ucscf->fall_count = 2;
    }

    if (ucscf->rise_count == NGX_CONF_UNSET_UINT) {
        ucscf->rise_count = 5;
    }

    if (ucscf->check_interval == NGX_CONF_UNSET_MSEC) {
        ucscf->check_interval = 0;
    }

    if (ucscf->check_timeout == NGX_CONF_UNSET_MSEC) {
        ucscf->check_timeout = 1000;
    }

    if (ucscf->check_type_conf == NGX_CONF_UNSET_PTR) {
        ucscf->check_type_conf = NULL;
    }

    check = ucscf->check_type_conf;

    if (check) {
        if (ucscf->send.len == 0) {
            ucscf->send.data = check->default_send.data;
            ucscf->send.len = check->default_send.len;
        }

        if (ucscf->code.status_alive == 0) {
            ucscf->code.status_alive = check->default_status_alive;
        }
    }

    return NGX_CONF_OK;
}


static char *
ngx_stream_upstream_check_init_shm(ngx_conf_t *cf, void *conf)
{
    ngx_str_t                              *shm_name;
    ngx_uint_t                              shm_size;
    ngx_shm_zone_t                         *shm_zone;
    ngx_stream_upstream_check_main_conf_t  *ucmcf = conf;

    ngx_stream_check_shm_generation++;

    shm_name = &ucmcf->peers_conf->check_shm_name;

    ngx_stream_upstream_check_get_shm_name(shm_name, cf->pool,
                                           ngx_stream_check_shm_generation);

    /* The default check shared memory size is 1M */
    shm_size = 1 * 1024 * 1024;

    shm_size = shm_size < ucmcf->check_shm_size ?
                          ucmcf->check_shm_size : shm_size;

    shm_zone = ngx_shared_memory_add(cf, shm_name, shm_size,
                                     &ngx_stream_upstream_check_module);

    ngx_log_debug2(NGX_LOG_DEBUG_STREAM, cf->log, 0,
                   "stream upstream check, upsteam:%V, shm_zone size:%ui",
                   shm_name, shm_size);

    shm_zone->data = ucmcf->peers_conf;
    check_peers_ctx = ucmcf->peers_conf;

    shm_zone->init = ngx_stream_upstream_check_init_shm_zone;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_stream_upstream_check_get_shm_name(ngx_str_t *shm_name, ngx_pool_t *pool,
    ngx_uint_t generation)
{
    u_char  *last;

    shm_name->data = ngx_palloc(pool, SHM_NAME_LEN);
    if (shm_name->data == NULL) {
        return NGX_ERROR;
    }

    last = ngx_snprintf(shm_name->data, SHM_NAME_LEN, "%s#%ui",
                        "ngx_stream_upstream_check_module", generation);

    shm_name->len = last - shm_name->data;

    return NGX_OK;
}


static ngx_int_t
ngx_stream_upstream_check_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data) 
{
    ngx_uint_t                         i;
    ngx_slab_pool_t                   *shpool;
    ngx_stream_check_peer_shm_t       *peer_shm;
    ngx_stream_check_peers_shm_t      *peers_shm;
    ngx_stream_check_peers_conf_t     *peers_conf;

    peers_conf = shm_zone->data;

    if (peers_conf == NULL || peers_conf->peers.nelts == 0) {
        return NGX_OK;
    }

    if (data) {
        peers_shm = data;
    } else {
        shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

        peers_shm = ngx_slab_alloc(shpool, sizeof(*peers_shm) +
            (peers_conf->peers.nelts - 1) * sizeof(ngx_stream_check_peer_shm_t));

        if (peers_shm == NULL) {
            ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                          "tcp upstream check_shm_size is too small, "
                          "you should set a larger size.");
            return NGX_ERROR;
        }
    }

    peers_shm->generation = ngx_stream_check_shm_generation;

    for (i = 0; i < peers_conf->peers.nelts; i++) {
        peer_shm = &peers_shm->peers[i];

        peer_shm->owner = NGX_INVALID_PID;

        peer_shm->access_time = 0;
        peer_shm->access_count = 0;

        peer_shm->fall_count = 0;
        peer_shm->rise_count = 0;

        peer_shm->down = 1;
    }

    peers_conf->peers_shm = peers_shm;

    return NGX_OK;
}


static ngx_int_t 
ngx_stream_check_init_process(ngx_cycle_t *cycle) 
{
    ngx_str_t                               shm_name;
    ngx_uint_t                              i;
    ngx_msec_t                              t, delay;
    check_conf_t                           *cf;
    ngx_shm_zone_t                         *shm_zone;
    ngx_stream_check_peer_shm_t            *peer_shm;
    ngx_stream_check_peers_shm_t           *peers_shm;
    ngx_stream_check_peer_conf_t           *peer_conf;
    ngx_stream_check_peers_conf_t          *peers_conf;
    ngx_stream_upstream_check_srv_conf_t   *ucscf;

    if (ngx_stream_check_get_shm_name(&shm_name, cycle->pool) == NGX_ERROR) {
        return NGX_ERROR;
    }

    shm_zone = ngx_shared_memory_find(cycle, &shm_name, 
                                      &ngx_stream_upstream_check_module);

    if (shm_zone == NULL || shm_zone->data == NULL) {
        return NGX_OK;
    }

    peers_conf = shm_zone->data;
    peers_shm = peers_conf->peers_shm;

    ngx_log_debug2(NGX_LOG_DEBUG_TCP, cycle->log, 0, 
                   "stream check upstream init_process, shm_name: %V, "
                   "peer number: %ud",
                   &shm_name, peers_conf->peers.nelts);

    srandom(ngx_pid);

    peer_conf = peers_conf->peers.elts;
    peer_shm = peers_shm->peers;

    for (i = 0; i < peers_conf->peers.nelts; i++) {
        peer_conf[i].shm = &peer_shm[i];

        peer_conf[i].check_ev.handler = ngx_stream_check_begin_handler;
        peer_conf[i].check_ev.log = cycle->log;
        peer_conf[i].check_ev.data = &peer_conf[i];
        peer_conf[i].check_ev.timer_set = 0;

        peer_conf[i].check_timeout_ev.handler = ngx_stream_check_timeout_handler;
        peer_conf[i].check_timeout_ev.log = cycle->log;
        peer_conf[i].check_timeout_ev.data = &peer_conf[i];
        peer_conf[i].check_timeout_ev.timer_set = 0;

        ucscf = peer_conf[i].conf;
        cf = ucscf->check_type_conf;

        if (cf->need_pool) {
            peer_conf[i].pool = ngx_create_pool(ngx_pagesize, cycle->log);
            if (peer_conf[i].pool == NULL) {
                return NGX_ERROR;
            }
        }

        peer_conf[i].send_handler = cf->send_handler;
        peer_conf[i].recv_handler = cf->recv_handler;

        peer_conf[i].init = cf->init;
        peer_conf[i].parse = cf->parse;
        peer_conf[i].reinit = cf->reinit;

        /* Default delay interval is 1 second. 
           I don't want to trigger the check event too close. */
        delay = ucscf->check_interval > 1000 ? ucscf->check_interval : 1000;
        t = ngx_random() % delay;

        ngx_add_timer(&peer_conf[i].check_ev, t);
    }

    return NGX_OK;
}


static void
ngx_stream_upstream_status_send(ngx_stream_session_t *s, ngx_buf_t *b)
{
    u_char                  *data;
    ssize_t                  temp_send = 0, send_num = 0, len;
    ngx_connection_t        *c;

    c = s->connection;
    data = b->pos;
    len = b->last - b->pos;

    ngx_log_debug0(NGX_LOG_DEBUG, c->log, 0, "upstream_show_send");

    while (send_num < len) {

        temp_send = c->send(c, data + temp_send, len - send_num);

#if (NGX_DEBUG)
        {
        ngx_err_t  err;

        err = (temp_send >=0) ? 0 : ngx_socket_errno;
        ngx_log_debug2(NGX_LOG_DEBUG, c->log, err,
                       "upsync show send size: %z, total: %z",
                       temp_send, len);

        if (temp_send > 0) {
            ngx_log_debug2(NGX_LOG_DEBUG, c->log, err,
                           "upsync show send content: %*s ", temp_send, data);
        }
        }
#endif

        if (temp_send > 0) {
            send_num += temp_send;

        } else if (temp_send == 0 || temp_send == NGX_AGAIN) {
            continue;

        } else {
            c->error = 1;
            break;
        }
    }

    if (send_num == len) {
        ngx_log_debug0(NGX_LOG_DEBUG, c->log, 0, "upsync_show_send done.");
    }

    return;
}


static void
ngx_stream_upstream_check_status_handler(ngx_stream_session_t *r) 
{
    size_t                            buffer_size;
    ngx_buf_t                        *b;
    ngx_str_t                         shm_name;
    ngx_uint_t                        i;
    ngx_shm_zone_t                   *shm_zone;
    ngx_stream_check_peer_shm_t      *peer_shm;
    ngx_stream_check_peers_shm_t     *peers_shm;
    ngx_stream_check_peer_conf_t     *peer_conf;
    ngx_stream_check_peers_conf_t    *peers_conf;

    if (ngx_stream_check_get_shm_name(&shm_name, r->connection->pool) == NGX_ERROR) {
        return;
    }

    shm_zone = ngx_shared_memory_find((ngx_cycle_t *)ngx_cycle, &shm_name, 
                                      &ngx_stream_upstream_check_module);

    if (shm_zone == NULL || shm_zone->data == NULL) {

        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "[steam upstream check] can not find the "
                      "shared memory zone \"%V\" ", &shm_name);

        return;
    }

    peers_conf = shm_zone->data;
    peers_shm = peers_conf->peers_shm;

    peer_conf = peers_conf->peers.elts;
    peer_shm = peers_shm->peers;

    /* 1/4 pagesize for each record */
    buffer_size = peers_conf->peers.nelts * ngx_pagesize / 4;
    buffer_size = ngx_align(buffer_size, ngx_pagesize) + ngx_pagesize;

    b = ngx_create_temp_buf(r->connection->pool, buffer_size);
    if (b == NULL) {
        return;
    }

    //HTTP header
    b->last = ngx_snprintf(b->last, b->end - b->last,
                           "HTTP/1.0 200 OK\r\n");
    b->last = ngx_snprintf(b->last, b->end - b->last,
                           "Server: nginx\r\n");
    b->last = ngx_snprintf(b->last, b->end - b->last,
                           "Content-Type: text/plain\r\n");
    b->last = ngx_snprintf(b->last, b->end - b->last,
                           "Connection: close\r\n\r\n");

    b->last = ngx_sprintf(b->last, 
            "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\n"
            "\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\n"
            "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n"
            "<head>\n"
            "  <title>Nginx tcp upstream check status</title>\n"
            "</head>\n"
            "<body>\n"
            "<h1>Nginx tcp upstream check status</h1>\n"
            "<h2>Check upstream server number: %ui, shm_name: %V</h2>\n"
            "<table style=\"background-color:white\" cellspacing=\"0\" cellpadding=\"3\" border=\"1\">\n"
            "  <tr bgcolor=\"#C0C0C0\">\n"
            "    <th>Index</th>\n"
            "    <th>Name</th>\n"
            "    <th>Status</th>\n"
            "    <th>Busyness</th>\n"
            "    <th>Rise counts</th>\n"
            "    <th>Fall counts</th>\n"
            "    <th>Access counts</th>\n"
            "    <th>Check type</th>\n"
            "  </tr>\n",
            peers_conf->peers.nelts, &shm_name);

    for (i = 0; i < peers_conf->peers.nelts; i++) {
        b->last = ngx_sprintf(b->last, 
                "  <tr%s>\n"
                "    <td>%ui</td>\n" 
                "    <td>%V</td>\n" 
                "    <td>%s</td>\n" 
                "    <td>%ui</td>\n" 
                "    <td>%ui</td>\n" 
                "    <td>%ui</td>\n" 
                "    <td>%ui</td>\n" 
                "    <td>%s</td>\n" 
                "  </tr>\n",
                peer_shm[i].down ? " bgcolor=\"#FF0000\"" : "",
                i, 
                &peer_conf[i].peer->name, 
                peer_shm[i].down ? "down" : "up",
                peer_shm[i].rise_count, 
                peer_shm[i].fall_count, 
                peer_shm[i].access_count, 
                peer_conf[i].conf->check_type_conf->name);
    }

    b->last = ngx_sprintf(b->last, 
            "</table>\n"
            "</body>\n"
            "</html>\n");

    ngx_stream_upstream_status_send(r, b);

    ngx_stream_close_connection(r->connection);

    return;
}


static char *
ngx_stream_upstream_check_status(ngx_conf_t *cf, 
    ngx_command_t *cmd, void *conf) 
{
    ngx_stream_core_srv_conf_t              *cscf;

    cscf = ngx_stream_conf_get_module_srv_conf(cf, ngx_stream_core_module);

    cscf->handler = ngx_stream_upstream_check_status_handler;

    return NGX_CONF_OK;
}
