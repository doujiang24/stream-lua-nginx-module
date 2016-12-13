
/*
 * Copyright (C) Yichun Zhang (agentzh)
 */


#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"


#include "ngx_stream_lua_util.h"
#include "ngx_stream_lua_socket_tcp.h"
#include "ngx_stream_lua_exception.h"
#include "ngx_stream_lua_pcrefix.h"
#include "ngx_stream_lua_uthread.h"
#include "ngx_stream_lua_consts.h"
#include "ngx_stream_lua_log.h"
#include "ngx_stream_lua_output.h"
#include "ngx_stream_lua_time.h"
#include "ngx_stream_lua_string.h"
#include "ngx_stream_lua_control.h"
#include "ngx_stream_lua_sleep.h"
#include "ngx_stream_lua_phase.h"
#include "ngx_stream_lua_regex.h"
#include "ngx_stream_lua_variable.h"
#include "ngx_stream_lua_shdict.h"
#include "ngx_stream_lua_socket_udp.h"
#include "ngx_stream_lua_timer.h"
#include "ngx_stream_lua_config.h"
#include "ngx_stream_lua_worker.h"
#include "ngx_stream_lua_misc.h"
#include "ngx_stream_lua_coroutine.h"
#include <ngx_md5.h>


static ngx_int_t ngx_stream_lua_flush_pending_output(ngx_stream_session_t *s,
    ngx_stream_lua_ctx_t *ctx);
static lua_State *ngx_stream_lua_new_state(lua_State *parent_vm,
    ngx_cycle_t *cycle, ngx_stream_lua_main_conf_t *lmcf, ngx_log_t *log);
static void ngx_stream_lua_finalize_threads(ngx_stream_session_t *s,
    ngx_stream_lua_ctx_t *ctx, lua_State *L);
static ngx_int_t ngx_stream_lua_on_abort_resume(ngx_stream_session_t *s,
    ngx_stream_lua_ctx_t *ctx);
void ngx_stream_lua_session_handler(ngx_event_t *ev);
static void ngx_stream_lua_set_path(ngx_cycle_t *cycle, lua_State *L,
    int tab_idx, const char *fieldname, const char *path,
    const char *default_path, ngx_log_t *log);
static void ngx_stream_lua_init_registry(lua_State *L, ngx_log_t *log);
static void ngx_stream_lua_init_globals(lua_State *L, ngx_cycle_t *cycle,
    ngx_stream_lua_main_conf_t *lmcf, ngx_log_t *log);
static ngx_int_t ngx_stream_lua_handle_exit(lua_State *L,
    ngx_stream_session_t *s, ngx_stream_lua_ctx_t *ctx);
static void
    ngx_stream_lua_cleanup_zombie_child_uthreads(ngx_stream_session_t *s,
    lua_State *L, ngx_stream_lua_ctx_t *ctx, ngx_stream_lua_co_ctx_t *coctx);
static ngx_int_t ngx_stream_lua_post_zombie_thread(ngx_stream_session_t *s,
    ngx_stream_lua_co_ctx_t *parent, ngx_stream_lua_co_ctx_t *thread);
static int ngx_stream_lua_thread_traceback(lua_State *L, lua_State *co,
    ngx_stream_lua_co_ctx_t *coctx);
static void ngx_stream_lua_close_fake_session(ngx_stream_session_t *s);
static void ngx_stream_lua_finalize_real_session(ngx_stream_session_t *s,
    ngx_int_t rc);
static void ngx_stream_lua_set_lingering_close(ngx_stream_session_t *s,
    ngx_stream_lua_ctx_t *ctx);
static void ngx_stream_lua_lingering_close_handler(ngx_event_t *rev);
static void ngx_stream_lua_empty_handler(ngx_event_t *wev);
static void ngx_stream_lua_inject_ngx_api(lua_State *L,
    ngx_stream_lua_main_conf_t *lmcf, ngx_log_t *log);
static int ngx_stream_lua_get_raw_phase_context(lua_State *L);
static ngx_int_t ngx_stream_lua_process_flushing_coroutines(
    ngx_stream_session_t *s, ngx_stream_lua_ctx_t *ctx);
static void ngx_stream_lua_inject_req_api(ngx_log_t *log, lua_State *L);


enum {
    NGX_STREAM_LUA_LINGERING_BUFFER_SIZE = 4096,
    NGX_STREAM_LUA_DISCARD_BUFFER_SIZE   = 4096
};


#ifndef LUA_PATH_SEP
#define LUA_PATH_SEP ";"
#endif

#define AUX_MARK "\1"


#ifndef NGX_STREAM_LUA_BT_DEPTH
#define NGX_STREAM_LUA_BT_DEPTH  22
#endif


#ifndef NGX_STREAM_LUA_BT_MAX_COROS
#define NGX_STREAM_LUA_BT_MAX_COROS  5
#endif


char ngx_stream_lua_code_cache_key;
char ngx_stream_lua_regex_cache_key;
char ngx_stream_lua_socket_pool_key;
char ngx_stream_lua_coroutines_key;


u_char *
ngx_stream_lua_rebase_path(ngx_pool_t *pool, u_char *src, size_t len)
{
    u_char            *p, *dst;

    if (len == 0) {
        return NULL;
    }

    if (src[0] == '/') {
        /* being an absolute path already */
        dst = ngx_palloc(pool, len + 1);
        if (dst == NULL) {
            return NULL;
        }

        p = ngx_copy(dst, src, len);

        *p = '\0';

        return dst;
    }

    dst = ngx_palloc(pool, ngx_cycle->prefix.len + len + 1);
    if (dst == NULL) {
        return NULL;
    }

    p = ngx_copy(dst, ngx_cycle->prefix.data, ngx_cycle->prefix.len);
    p = ngx_copy(p, src, len);

    *p = '\0';

    return dst;
}


u_char *
ngx_stream_lua_digest_hex(u_char *dest, const u_char *buf, int buf_len)
{
    ngx_md5_t                     md5;
    u_char                        md5_buf[MD5_DIGEST_LENGTH];

    ngx_md5_init(&md5);
    ngx_md5_update(&md5, buf, buf_len);
    ngx_md5_final(md5_buf, &md5);

    return ngx_hex_dump(dest, md5_buf, sizeof(md5_buf));
}


ngx_int_t
ngx_stream_lua_wev_handler(ngx_stream_session_t *s, ngx_stream_lua_ctx_t *ctx)
{
    ngx_int_t                    rc;
    ngx_event_t                 *wev;
    ngx_connection_t            *c;
    ngx_stream_lua_srv_conf_t   *lscf;

    ngx_stream_lua_socket_tcp_upstream_t *u;

    c = s->connection;
    wev = c->write;

    ngx_log_debug3(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream lua run write event handler: "
                   "timedout:%ud, ready:%ud, writing_raw_req_socket:%ud",
                   wev->timedout, wev->ready, ctx->writing_raw_req_socket);

    if (wev->timedout && !ctx->writing_raw_req_socket) {
        if (!wev->delayed) {
            ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT,
                          "stream lua client timed out");
            c->timedout = 1;

            if (ctx->done) {
                ngx_stream_lua_finalize_session(s, NGX_DONE);
                return NGX_OK;
            }

            goto flush_coros;
        }

        wev->timedout = 0;
        wev->delayed = 0;

        if (!wev->ready) {
            lscf = ngx_stream_get_module_srv_conf(s, ngx_stream_lua_module);

            ngx_add_timer(wev, lscf->send_timeout);

            if (ngx_handle_write_event(wev, lscf->send_lowat) != NGX_OK) {
                if (ctx->entered_content_phase) {
                    ngx_stream_lua_finalize_session(s, NGX_ERROR);
                }
                return NGX_ERROR;
            }
        }
    }

    dd("wev ready=%d, timedout=%d", wev->ready, wev->timedout);

    if (!wev->ready && !wev->timedout) {
        goto useless;
    }

    if (ctx->writing_raw_req_socket) {
        ctx->writing_raw_req_socket = 0;

        u = ctx->downstream;
        if (u == NULL) {
            return NGX_ERROR;
        }

        u->write_event_handler(s, u);
        return NGX_DONE;
    }

    if (ctx->downstream_busy_bufs) {
        rc = ngx_stream_lua_flush_pending_output(s, ctx);

        dd("flush pending output returned %d, c->error: %d", (int) rc,
           c->error);

        if (rc != NGX_ERROR && rc != NGX_OK) {
            goto useless;
        }

        /* when rc == NGX_ERROR, c->error must be set */

        if (!ctx->downstream_busy_bufs && ctx->done) {
            ngx_stream_lua_finalize_session(s, NGX_DONE);
            return NGX_DONE;
        }
    }

flush_coros:

    dd("ctx->flushing_coros: %d", (int) ctx->flushing_coros);

    if (ctx->flushing_coros) {
        return ngx_stream_lua_process_flushing_coroutines(s, ctx);
    }

    /* ctx->flushing_coros == 0 */

useless:

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "useless lua write event handler");

    if (ctx->entered_content_phase) {
        if ((c->timedout || c->error) && ctx->done) {
            ngx_stream_lua_finalize_session(s, NGX_DONE);
        }
        return NGX_OK;
    }

    return NGX_DONE;
}


static ngx_int_t
ngx_stream_lua_process_flushing_coroutines(ngx_stream_session_t *s,
    ngx_stream_lua_ctx_t *ctx)
{
    ngx_int_t                    rc, n;
    ngx_uint_t                   i;
    ngx_list_part_t             *part;
    ngx_stream_lua_co_ctx_t     *coctx;

    dd("processing flushing coroutines");

    coctx = &ctx->entry_co_ctx;
    n = ctx->flushing_coros;

    if (coctx->flushing) {
        coctx->flushing = 0;

        ctx->flushing_coros--;
        n--;
        ctx->cur_co_ctx = coctx;

        rc = ngx_stream_lua_flush_resume_helper(s, ctx);
        if (rc == NGX_ERROR || rc >= NGX_OK) {
            return rc;
        }

        /* rc == NGX_DONE */
    }

    if (n) {

        if (ctx->user_co_ctx == NULL) {
            return NGX_ERROR;
        }

        part = &ctx->user_co_ctx->part;
        coctx = part->elts;

        for (i = 0; /* void */; i++) {

            if (i >= part->nelts) {
                if (part->next == NULL) {
                    break;
                }

                part = part->next;
                coctx = part->elts;
                i = 0;
            }

            if (coctx[i].flushing) {
                coctx[i].flushing = 0;
                ctx->flushing_coros--;
                n--;
                ctx->cur_co_ctx = &coctx[i];

                rc = ngx_stream_lua_flush_resume_helper(s, ctx);
                if (rc == NGX_ERROR || rc >= NGX_OK) {
                    return rc;
                }

                /* rc == NGX_DONE */

                if (n == 0) {
                    return NGX_DONE;
                }
            }
        }
    }

    if (n) {
        return NGX_ERROR;
    }

    return NGX_DONE;
}


lua_State *
ngx_stream_lua_init_vm(lua_State *parent_vm, ngx_cycle_t *cycle,
    ngx_pool_t *pool, ngx_stream_lua_main_conf_t *lmcf, ngx_log_t *log,
    ngx_pool_cleanup_t **pcln)
{
    lua_State                       *L;
    ngx_uint_t                       i;
    ngx_pool_cleanup_t              *cln;
    ngx_stream_lua_preload_hook_t   *hook;
    ngx_stream_lua_vm_state_t       *state;

    cln = ngx_pool_cleanup_add(pool, 0);
    if (cln == NULL) {
        return NULL;
    }

    /* create new Lua VM instance */
    L = ngx_stream_lua_new_state(parent_vm, cycle, lmcf, log);
    if (L == NULL) {
        return NULL;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, log, 0, "stream lua initialize the "
                   "global Lua VM %p", L);

    /* register cleanup handler for Lua VM */
    cln->handler = ngx_stream_lua_cleanup_vm;

    state = ngx_alloc(sizeof(ngx_stream_lua_vm_state_t), log);
    if (state == NULL) {
        return NULL;
    }
    state->vm = L;
    state->count = 1;

    cln->data = state;

    if (pcln) {
        *pcln = cln;
    }

    if (lmcf->preload_hooks) {

        /* register the 3rd-party module's preload hooks */

        lua_getglobal(L, "package");
        lua_getfield(L, -1, "preload");

        hook = lmcf->preload_hooks->elts;

        for (i = 0; i < lmcf->preload_hooks->nelts; i++) {

#if 0
            /* TODO */
            ngx_stream_lua_probe_register_preload_package(L, hook[i].package);
#endif

            lua_pushcfunction(L, hook[i].loader);
            lua_setfield(L, -2, (char *) hook[i].package);
        }

        lua_pop(L, 2);
    }

    return L;
}


void
ngx_stream_lua_cleanup_vm(void *data)
{
    lua_State                       *L;
    ngx_stream_lua_vm_state_t       *state = data;

#if (DDEBUG)
    if (state) {
        dd("cleanup VM: c:%d, s:%p", (int) state->count, state->vm);
    }
#endif

    if (state) {
        ngx_log_debug1(NGX_LOG_DEBUG_STREAM, ngx_cycle->log, 0,
                       "stream lua decrementing the reference count "
                       "for Lua VM: %i", state->count);

        if (--state->count == 0) {
            L = state->vm;
            ngx_stream_lua_cleanup_conn_pools(L);
            ngx_log_debug1(NGX_LOG_DEBUG_STREAM, ngx_cycle->log, 0,
                           "stream lua close the global Lua VM %p", L);
            lua_close(L);
            ngx_free(state);
        }
    }
}


void
ngx_stream_lua_reset_ctx(ngx_stream_session_t *s, lua_State *L,
    ngx_stream_lua_ctx_t *ctx)
{
    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, s->connection->log, 0,
                   "stream lua reset ctx");

    ngx_stream_lua_finalize_threads(s, ctx, L);

#if 0
    if (ctx->user_co_ctx) {
        /* no way to destroy a list but clean up the whole pool */
        ctx->user_co_ctx = NULL;
    }
#endif

    ngx_memzero(&ctx->entry_co_ctx, sizeof(ngx_stream_lua_co_ctx_t));

    ctx->entry_co_ctx.co_ref = LUA_NOREF;

#if 0
    /* TODO */
    ctx->entered_rewrite_phase = 0;
    ctx->entered_access_phase = 0;
#endif
    ctx->entered_content_phase = 0;

    ctx->exit_code = 0;
    ctx->exited = 0;
    ctx->resume_handler = ngx_stream_lua_wev_handler;

    ctx->co_op = 0;
}


lua_State *
ngx_stream_lua_new_thread(ngx_stream_session_t *s, lua_State *L, int *ref)
{
    int              base;
    lua_State       *co;

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, s->connection->log, 0,
                   "stream lua creating new thread");

    base = lua_gettop(L);

    lua_pushlightuserdata(L, &ngx_stream_lua_coroutines_key);
    lua_rawget(L, LUA_REGISTRYINDEX);

    co = lua_newthread(L);

    /*  {{{ inherit coroutine's globals to main thread's globals table
     *  for print() function will try to find tostring() in current
     *  globals table.
     */
    /*  new globals table for coroutine */
    ngx_stream_lua_create_new_globals_table(co, 0, 0);

    lua_createtable(co, 0, 1);
    ngx_stream_lua_get_globals_table(co);
    lua_setfield(co, -2, "__index");
    lua_setmetatable(co, -2);

    ngx_stream_lua_set_globals_table(co);
    /*  }}} */

    *ref = luaL_ref(L, -2);

    if (*ref == LUA_NOREF) {
        lua_settop(L, base);  /* restore main thread stack */
        return NULL;
    }

    lua_settop(L, base);
    return co;
}


void
ngx_stream_lua_del_thread(ngx_stream_session_t *s, lua_State *L,
    ngx_stream_lua_ctx_t *ctx, ngx_stream_lua_co_ctx_t *coctx)
{
    if (coctx->co_ref == LUA_NOREF) {
        return;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, s->connection->log, 0,
                   "stream lua deleting light thread");

    lua_pushlightuserdata(L, &ngx_stream_lua_coroutines_key);
    lua_rawget(L, LUA_REGISTRYINDEX);

#if 0
    /* TODO */
    ngx_stream_lua_probe_thread_delete(s, coctx->co, ctx);
#endif

    luaL_unref(L, -1, coctx->co_ref);
    coctx->co_ref = LUA_NOREF;
    coctx->co_status = NGX_STREAM_LUA_CO_DEAD;

    lua_pop(L, 1);
}


void
ngx_stream_lua_session_cleanup_handler(void *data)
{
    ngx_stream_lua_ctx_t          *ctx = data;

    ngx_stream_lua_session_cleanup(ctx, 0 /* forcible */);
}


void
ngx_stream_lua_session_cleanup(ngx_stream_lua_ctx_t *ctx, int forcible)
{
    lua_State                       *L;
    ngx_stream_session_t            *s;
    ngx_stream_lua_main_conf_t      *lmcf;

    /*  force coroutine handling the session quit */
    if (ctx == NULL) {
        dd("ctx is NULL");
        return;
    }

    s = ctx->session;

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, s->connection->log, 0,
                   "stream lua session cleanup: forcible=%d", forcible);

    lmcf = ngx_stream_get_module_main_conf(s, ngx_stream_lua_module);

#if 1
    if (s->connection->fd == (ngx_socket_t) -1) {
        /* being a fake session */
        dd("running_timers--");
        lmcf->running_timers--;
    }
#endif

    L = ngx_stream_lua_get_lua_vm(s, ctx);

    ngx_stream_lua_finalize_threads(s, ctx, L);
}


void
ngx_stream_lua_finalize_session(ngx_stream_session_t *s, ngx_int_t rc)
{
    ngx_stream_lua_ctx_t              *ctx;

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_lua_module);
    if (ctx && ctx->cur_co_ctx) {
        ngx_stream_lua_cleanup_pending_operation(ctx->cur_co_ctx);
    }

    if (s->connection->fd != (ngx_socket_t) -1) {
        ngx_stream_lua_finalize_real_session(s, rc);
        return;
    }

    ngx_stream_lua_finalize_fake_session(s, rc);
}


static void
ngx_stream_lua_finalize_real_session(ngx_stream_session_t *s, ngx_int_t rc)
{
    ngx_event_t                 *wev;
    ngx_connection_t            *c;
    ngx_stream_lua_ctx_t        *ctx;
    ngx_stream_lua_srv_conf_t   *lscf;

    c = s->connection;

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream lua finalize: rc=%i", rc);

    if (rc == NGX_ERROR || rc == NGX_DECLINED) {
        ngx_stream_lua_free_session(s, NGX_STREAM_INTERNAL_SERVER_ERROR);
        return;
    }

    lscf = ngx_stream_get_module_srv_conf(s, ngx_stream_lua_module);

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_lua_module);
    if (ctx == NULL) {
        ngx_stream_lua_free_session(s, NGX_STREAM_OK);
        return;
    }

    if (rc == NGX_DONE) {   /* yield */

        if (ctx->done) {
            ngx_stream_lua_free_session(s, NGX_STREAM_OK);
            return;
        }

        if ((ngx_event_flags & NGX_USE_LEVEL_EVENT)
            && c->write->active)
        {
            dd("done: ctx->downstream_busy_bufs: %p",
               ctx->downstream_busy_bufs);

            if (ctx->downstream_busy_bufs == NULL
                && !ctx->writing_raw_req_socket)
            {
                if (ngx_del_event(c->write, NGX_WRITE_EVENT, 0) != NGX_OK) {
                    ngx_stream_lua_free_session(s,
                                            NGX_STREAM_INTERNAL_SERVER_ERROR);
                }
            }
        }

        return;
    }

    ngx_stream_lua_assert(rc != NGX_AGAIN);

    /* rc == NGX_OK */

    ctx->done = 1;

    if (c->error || c->timedout) {
        ngx_stream_lua_free_session(s, NGX_STREAM_INTERNAL_SERVER_ERROR);
        return;
    }

    if (ctx->downstream_busy_bufs) {
        wev = c->write;

        ngx_log_debug3(NGX_LOG_DEBUG_STREAM, c->log, 0,
                       "stream lua having pending data to flush to "
                       "downstream fd:%d active:%d ready:%d", (int) c->fd,
                       wev->active, wev->ready);

        ngx_add_timer(wev, lscf->send_timeout);

#if 1
        if (!wev->active) {
            if (ngx_handle_write_event(wev, lscf->send_lowat) != NGX_OK) {
                ngx_stream_lua_free_session(s, NGX_STREAM_INTERNAL_SERVER_ERROR);
                return;
            }
        }
#endif

        ctx->write_event_handler = ngx_stream_lua_content_wev_handler;
        ctx->resume_handler = ngx_stream_lua_wev_handler;
        return;
    }

#if (DDEBUG)
    dd("c->buffered: %d, busy_bufs: %p, rev ready: %d, "
       "ctx->lingering_close: %d", (int) c->buffered,
       ctx->downstream_busy_bufs, c->read->ready,
       ctx->lingering_close);
#endif

    if (lscf->lingering_close == NGX_STREAM_LUA_LINGERING_ALWAYS
        || (lscf->lingering_close == NGX_STREAM_LUA_LINGERING_ON
            && (ctx->lingering_close
                || c->read->ready)))
    {
        ngx_stream_lua_set_lingering_close(s, ctx);
        return;
    }

    dd("closing connection upon successful completion");

    ngx_stream_lua_free_session(s, NGX_STREAM_OK);
    return;
}


static void
ngx_stream_lua_set_lingering_close(ngx_stream_session_t *s,
    ngx_stream_lua_ctx_t *ctx)
{
    ngx_event_t                 *rev, *wev;
    ngx_connection_t            *c;
    ngx_stream_lua_srv_conf_t   *lscf;

    c = s->connection;

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream lua set lingering close");

    lscf = ngx_stream_get_module_srv_conf(s, ngx_stream_lua_module);

    rev = c->read;
    rev->handler = ngx_stream_lua_lingering_close_handler;

    ctx->lingering_time = ngx_time() + (time_t) (lscf->lingering_time / 1000);
    ngx_add_timer(rev, lscf->lingering_timeout);

    if (ngx_handle_read_event(rev, 0) != NGX_OK) {
        ngx_stream_lua_free_session(s, NGX_STREAM_INTERNAL_SERVER_ERROR);
        return;
    }

    wev = c->write;
    wev->handler = ngx_stream_lua_empty_handler;

    if (wev->active && (ngx_event_flags & NGX_USE_LEVEL_EVENT)) {
        if (ngx_del_event(wev, NGX_WRITE_EVENT, 0) != NGX_OK) {
            ngx_stream_lua_free_session(s, NGX_STREAM_INTERNAL_SERVER_ERROR);
            return;
        }
    }

    if (ngx_shutdown_socket(c->fd, NGX_WRITE_SHUTDOWN) == -1) {
        ngx_connection_error(c, ngx_socket_errno,
                             ngx_shutdown_socket_n " failed");
        ngx_stream_lua_free_session(s, NGX_STREAM_INTERNAL_SERVER_ERROR);
        return;
    }

    if (rev->ready) {
        ngx_stream_lua_lingering_close_handler(rev);
    }
}


static void
ngx_stream_lua_lingering_close_handler(ngx_event_t *rev)
{
    u_char                       buffer[NGX_STREAM_LUA_LINGERING_BUFFER_SIZE];
    ssize_t                      n;
    ngx_msec_t                   timer;
    ngx_connection_t            *c;
    ngx_stream_session_t        *s;
    ngx_stream_lua_ctx_t        *ctx;
    ngx_stream_lua_srv_conf_t   *lscf;

    c = rev->data;
    s = c->data;

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream lua lingering close handler");

    if (rev->timedout) {
        ngx_stream_lua_free_session(s, NGX_STREAM_INTERNAL_SERVER_ERROR);
        return;
    }

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_lua_module);
    if (ctx == NULL) {
        return;
    }

    timer = (ngx_msec_t) ctx->lingering_time - (ngx_msec_t) ngx_time();
    if ((ngx_msec_int_t) timer <= 0) {
        ngx_stream_lua_free_session(s, NGX_STREAM_OK);
        return;
    }

    do {
        n = c->recv(c, buffer, NGX_STREAM_LUA_LINGERING_BUFFER_SIZE);

        ngx_log_debug1(NGX_LOG_DEBUG_STREAM, c->log, 0,
                       "stream lua lingering read: %d", n);

        if (n == NGX_ERROR || n == 0) {
            ngx_stream_lua_free_session(s, NGX_STREAM_OK);
            return;
        }

#if (NGX_HAVE_EPOLL)
        if ((ngx_event_flags & NGX_USE_EPOLL_EVENT)
            && n < NGX_STREAM_LUA_LINGERING_BUFFER_SIZE)
        {
            /* the current socket is of the stream type,
             * so we don't have to read until EAGAIN
             * with epoll ET, reducing one syscall. */
            rev->ready = 0;
        }
#endif

    } while (rev->ready);

    if (ngx_handle_read_event(rev, 0) != NGX_OK) {
        ngx_stream_lua_free_session(s, NGX_STREAM_INTERNAL_SERVER_ERROR);
        return;
    }

    lscf = ngx_stream_get_module_srv_conf(s, ngx_stream_lua_module);

    timer *= 1000;

    if (timer > lscf->lingering_timeout) {
        timer = lscf->lingering_timeout;
    }

    ngx_add_timer(rev, timer);
}


void
ngx_stream_lua_rd_check_broken_connection(ngx_stream_session_t *s,
    ngx_stream_lua_ctx_t *ctx)
{
    ngx_int_t                   rc;
    ngx_event_t                *rev;
    ngx_connection_t           *c;

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, s->connection->log, 0,
                   "stream lua read check broken connection");

    c = s->connection;

    rc = ngx_stream_lua_check_broken_connection(s, c->read);

    if (rc == NGX_OK) {
        return;
    }

    /* rc == NGX_ERROR || rc > NGX_OK */

    if (ctx->on_abort_co_ctx == NULL) {
        s->connection->error = 1;
        ngx_stream_lua_session_cleanup(ctx, 0);
        ngx_stream_lua_finalize_session(s, rc);
        return;
    }

    if (ctx->on_abort_co_ctx->co_status != NGX_STREAM_LUA_CO_SUSPENDED) {

        /* on_abort already run for the current session handler */

        rev = s->connection->read;

        if ((ngx_event_flags & NGX_USE_LEVEL_EVENT) && rev->active) {
            if (ngx_del_event(rev, NGX_READ_EVENT, 0) != NGX_OK) {
                ngx_stream_lua_session_cleanup(ctx, 0);
                ngx_stream_lua_finalize_session(s, NGX_ERROR);
                return;
            }
        }

        return;
    }

    ctx->uthreads++;
    ctx->resume_handler = ngx_stream_lua_on_abort_resume;
    ctx->on_abort_co_ctx->co_status = NGX_STREAM_LUA_CO_RUNNING;
    ctx->cur_co_ctx = ctx->on_abort_co_ctx;

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, s->connection->log, 0,
                   "stream lua waking up the on_abort callback thread");

    ctx->write_event_handler = ngx_stream_lua_content_wev_handler;

    ctx->write_event_handler(s, ctx);
}


ngx_int_t
ngx_stream_lua_check_broken_connection(ngx_stream_session_t *s, ngx_event_t *ev)
{
    int                  n;
    char                 buf[1];
    ngx_err_t            err;
    ngx_int_t            event;
    ngx_connection_t    *c;

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, ev->log, 0,
                   "stream lua check client, write event:%d", ev->write);

    c = s->connection;

    if (c->error) {
        if ((ngx_event_flags & NGX_USE_LEVEL_EVENT) && ev->active) {

            event = ev->write ? NGX_WRITE_EVENT : NGX_READ_EVENT;

            if (ngx_del_event(ev, event, 0) != NGX_OK) {
                return NGX_ERROR;
            }
        }

        return NGX_STREAM_CLIENT_CLOSED_REQUEST;
    }

#if (NGX_HAVE_KQUEUE)

    if (ngx_event_flags & NGX_USE_KQUEUE_EVENT) {

        if (!ev->pending_eof) {
            return NGX_OK;
        }

        ev->eof = 1;

        if (ev->kq_errno) {
            ev->error = 1;
        }

        ngx_log_error(NGX_LOG_INFO, ev->log, ev->kq_errno,
                      "kevent() reported that client prematurely closed "
                      "connection");

        return NGX_STREAM_CLIENT_CLOSED_REQUEST;
    }

#endif

    n = recv(c->fd, buf, 1, MSG_PEEK);

    err = ngx_socket_errno;

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, ev->log, err,
                   "stream lua recv(): %d", n);

    if (ev->write && (n >= 0 || err == NGX_EAGAIN)) {
        return NGX_OK;
    }

    if ((ngx_event_flags & NGX_USE_LEVEL_EVENT) && ev->active) {
        dd("event is active");

        event = ev->write ? NGX_WRITE_EVENT : NGX_READ_EVENT;

#if 1
        if (ngx_del_event(ev, event, 0) != NGX_OK) {
            return NGX_ERROR;
        }
#endif
    }

    dd("HERE %d", (int) n);

    if (n > 0) {
        return NGX_OK;
    }

    if (n == -1) {
        if (err == NGX_EAGAIN) {
            dd("HERE");
            return NGX_OK;
        }

        ev->error = 1;

    } else { /* n == 0 */
        err = 0;
    }

    ev->eof = 1;

    ngx_log_error(NGX_LOG_INFO, ev->log, err,
                  "stream client prematurely closed connection");

    return NGX_STREAM_CLIENT_CLOSED_REQUEST;
}


static ngx_int_t
ngx_stream_lua_flush_pending_output(ngx_stream_session_t *s,
    ngx_stream_lua_ctx_t *ctx)
{
    if (ctx->downstream_busy_bufs) {
        return ngx_stream_lua_send_chain_link(s, ctx, NULL);
    }

    return NGX_OK;
}


void
ngx_stream_lua_block_reading(ngx_stream_session_t *s,
    ngx_stream_lua_ctx_t *ctx)
{
    ngx_connection_t        *c;

    c = s->connection;

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream lua reading blocked");

    if ((ngx_event_flags & NGX_USE_LEVEL_EVENT)
        && c->read->active)
    {
        if (ngx_del_event(c->read, NGX_READ_EVENT, 0) != NGX_OK) {
            s = c->data;
            ngx_stream_lua_finalize_session(s, NGX_ERROR);
        }
    }
}


static lua_State *
ngx_stream_lua_new_state(lua_State *parent_vm, ngx_cycle_t *cycle,
    ngx_stream_lua_main_conf_t *lmcf, ngx_log_t *log)
{
    lua_State       *L;
    const char      *old_path;
    const char      *new_path;
    size_t           old_path_len;
    const char      *old_cpath;
    const char      *new_cpath;
    size_t           old_cpath_len;

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, log, 0,
                   "stream lua creating new vm state");

    L = luaL_newstate();
    if (L == NULL) {
        return NULL;
    }

    luaL_openlibs(L);

    lua_getglobal(L, "package");

    if (!lua_istable(L, -1)) {
        ngx_log_error(NGX_LOG_EMERG, log, 0,
                      "the \"package\" table does not exist");
        return NULL;
    }

    if (parent_vm) {
        lua_getglobal(parent_vm, "package");
        lua_getfield(parent_vm, -1, "path");
        old_path = lua_tolstring(parent_vm, -1, &old_path_len);
        lua_pop(parent_vm, 1);

        lua_pushlstring(L, old_path, old_path_len);
        lua_setfield(L, -2, "path");

        lua_getfield(parent_vm, -1, "cpath");
        old_path = lua_tolstring(parent_vm, -1, &old_path_len);
        lua_pop(parent_vm, 2);

        lua_pushlstring(L, old_path, old_path_len);
        lua_setfield(L, -2, "cpath");

    } else {
#ifdef LUA_DEFAULT_PATH
#   define LUA_DEFAULT_PATH_LEN (sizeof(LUA_DEFAULT_PATH) - 1)
        ngx_log_debug1(NGX_LOG_DEBUG_STREAM, log, 0,
                       "stream lua prepending default package.path with %s",
                       LUA_DEFAULT_PATH);

        lua_pushliteral(L, LUA_DEFAULT_PATH ";"); /* package default */
        lua_getfield(L, -2, "path"); /* package default old */
        old_path = lua_tolstring(L, -1, &old_path_len);
        lua_concat(L, 2); /* package new */
        lua_setfield(L, -2, "path"); /* package */
#endif

#ifdef LUA_DEFAULT_CPATH
#   define LUA_DEFAULT_CPATH_LEN (sizeof(LUA_DEFAULT_CPATH) - 1)
        ngx_log_debug1(NGX_LOG_DEBUG_STREAM, log, 0,
                       "stream lua prepending default package.cpath with %s",
                       LUA_DEFAULT_CPATH);

        lua_pushliteral(L, LUA_DEFAULT_CPATH ";"); /* package default */
        lua_getfield(L, -2, "cpath"); /* package default old */
        old_cpath = lua_tolstring(L, -1, &old_cpath_len);
        lua_concat(L, 2); /* package new */
        lua_setfield(L, -2, "cpath"); /* package */
#endif

        if (lmcf->lua_path.len != 0) {
            lua_getfield(L, -1, "path"); /* get original package.path */
            old_path = lua_tolstring(L, -1, &old_path_len);

            dd("old path: %s", old_path);

            lua_pushlstring(L, (char *) lmcf->lua_path.data,
                            lmcf->lua_path.len);
            new_path = lua_tostring(L, -1);

            ngx_stream_lua_set_path(cycle, L, -3, "path", new_path, old_path,
                                    log);

            lua_pop(L, 2);
        }

        if (lmcf->lua_cpath.len != 0) {
            lua_getfield(L, -1, "cpath"); /* get original package.cpath */
            old_cpath = lua_tolstring(L, -1, &old_cpath_len);

            dd("old cpath: %s", old_cpath);

            lua_pushlstring(L, (char *) lmcf->lua_cpath.data,
                            lmcf->lua_cpath.len);
            new_cpath = lua_tostring(L, -1);

            ngx_stream_lua_set_path(cycle, L, -3, "cpath", new_cpath, old_cpath,
                                    log);


            lua_pop(L, 2);
        }
    }

    lua_pop(L, 1); /* remove the "package" table */

    ngx_stream_lua_init_registry(L, log);
    ngx_stream_lua_init_globals(L, cycle, lmcf, log);

    return L;
}


ngx_int_t
ngx_stream_lua_run_thread(lua_State *L, ngx_stream_session_t *s,
    ngx_stream_lua_ctx_t *ctx, volatile int nrets)
{
    ngx_connection_t            *c;
    ngx_stream_lua_co_ctx_t     *next_coctx, *parent_coctx, *orig_coctx;
    int                          rv, success = 1;
    lua_State                   *next_co;
    lua_State                   *old_co;
    const char                  *err, *msg, *trace;
#if (NGX_PCRE)
    ngx_pool_t                  *old_pool = NULL;
#endif

    c = s->connection;

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream lua run thread, top:%d", lua_gettop(L));

    /* set Lua VM panic handler */
    lua_atpanic(L, ngx_stream_lua_atpanic);

    dd("ctx = %p", ctx);

    NGX_LUA_EXCEPTION_TRY {

        if (ctx->cur_co_ctx->thread_spawn_yielded) {
#if 0
            /* TODO */
            ngx_stream_lua_probe_info("thread spawn yielded");
#endif

            ctx->cur_co_ctx->thread_spawn_yielded = 0;
            nrets = 1;
        }

        for ( ;; ) {

            dd("calling lua_resume: vm %p, nret %d", ctx->cur_co_ctx->co,
               (int) nrets);

#if (NGX_PCRE)
            /* XXX: work-around to nginx regex subsystem */
            old_pool = ngx_stream_lua_pcre_malloc_init(c->pool);
#endif

            /*  run code */
            dd("ctx: %p", ctx);
            dd("cur co: %p", ctx->cur_co_ctx->co);
            dd("cur co status: %d", ctx->cur_co_ctx->co_status);

            orig_coctx = ctx->cur_co_ctx;

#ifdef NGX_LUA_USE_ASSERT
            dd("%p: saved co top: %d, nrets: %d, true top: %d",
               orig_coctx->co,
               (int) orig_coctx->co_top, (int) nrets,
               (int) lua_gettop(orig_coctx->co));
#endif

#if DDEBUG
            if (lua_gettop(orig_coctx->co) > 0) {
                dd("top elem: %s", luaL_typename(orig_coctx->co, -1));
            }
#endif

            ngx_stream_lua_assert(orig_coctx->co_top + nrets
                                  == lua_gettop(orig_coctx->co));

            rv = lua_resume(orig_coctx->co, nrets);

#if (NGX_PCRE)
            /* XXX: work-around to nginx regex subsystem */
            ngx_stream_lua_pcre_malloc_done(old_pool);
#endif

#if 0
            /* test the longjmp thing */
            if (rand() % 2 == 0) {
                NGX_LUA_EXCEPTION_THROW(1);
            }
#endif

            ngx_log_debug1(NGX_LOG_DEBUG_STREAM, c->log, 0,
                           "stream lua resume returned %d", rv);

            switch (rv) {
            case LUA_YIELD:
                /*  yielded, let event handler do the rest job */
                /*  FIXME: add io cmd dispatcher here */

                ngx_log_debug0(NGX_LOG_DEBUG_STREAM, c->log, 0,
                               "stream lua thread yielded");

#ifdef NGX_LUA_USE_ASSERT
                dd("%p: saving curr top after yield: %d (co-op: %d)",
                   orig_coctx->co,
                   (int) lua_gettop(orig_coctx->co), (int) ctx->co_op);
                orig_coctx->co_top = lua_gettop(orig_coctx->co);
#endif

                if (ctx->exited) {
                    return ngx_stream_lua_handle_exit(L, s, ctx);
                }

                /*
                 * check if coroutine.resume or coroutine.yield called
                 * lua_yield()
                 */
                switch(ctx->co_op) {
                case NGX_STREAM_LUA_USER_CORO_NOP:
                    dd("hit! it is the API yield");

                    ngx_stream_lua_assert(lua_gettop(ctx->cur_co_ctx->co) == 0);

                    ctx->cur_co_ctx = NULL;

                    return NGX_AGAIN;

                case NGX_STREAM_LUA_USER_THREAD_RESUME:

                    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, c->log, 0,
                                   "stream lua user thread resume");

                    ctx->co_op = NGX_STREAM_LUA_USER_CORO_NOP;
                    nrets = lua_gettop(ctx->cur_co_ctx->co) - 1;
                    dd("nrets = %d", nrets);

#ifdef NGX_LUA_USE_ASSERT
                    /* ignore the return value (the thread) already pushed */
                    orig_coctx->co_top--;
#endif

                    break;

                case NGX_STREAM_LUA_USER_CORO_RESUME:
                    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, c->log, 0,
                                   "stream lua coroutine: resume");

                    /*
                     * the target coroutine lies at the base of the
                     * parent's stack
                     */
                    ctx->co_op = NGX_STREAM_LUA_USER_CORO_NOP;

                    old_co = ctx->cur_co_ctx->parent_co_ctx->co;

                    nrets = lua_gettop(old_co);
                    if (nrets) {
                        dd("moving %d return values to parent", nrets);
                        lua_xmove(old_co, ctx->cur_co_ctx->co, nrets);

#ifdef NGX_LUA_USE_ASSERT
                        ctx->cur_co_ctx->parent_co_ctx->co_top -= nrets;
#endif
                    }

                    break;

                default:
                    /* ctx->co_op == NGX_STREAM_LUA_USER_CORO_YIELD */

                    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, c->log, 0,
                                   "stream lua coroutine: yield");

                    ctx->co_op = NGX_STREAM_LUA_USER_CORO_NOP;

                    if (ngx_stream_lua_is_thread(ctx)) {
#if 0
                        /* TODO */
                        ngx_stream_lua_probe_thread_yield(s,
                                                          ctx->cur_co_ctx->co);
#endif

                        /* discard any return values from user
                         * coroutine.yield()'s arguments */
                        lua_settop(ctx->cur_co_ctx->co, 0);

#ifdef NGX_LUA_USE_ASSERT
                        ctx->cur_co_ctx->co_top = 0;
#endif

#if 0
                        /* TODO */
                        ngx_stream_lua_probe_info("set co running");
#endif
                        ctx->cur_co_ctx->co_status = NGX_STREAM_LUA_CO_RUNNING;

                        if (ctx->posted_threads) {
                            ngx_stream_lua_post_thread(s, ctx, ctx->cur_co_ctx);
                            ctx->cur_co_ctx = NULL;
                            return NGX_AGAIN;
                        }

                        /* no pending threads, so resume the thread
                         * immediately */

                        nrets = 0;
                        continue;
                    }

                    /* being a user coroutine that has a parent */

                    nrets = lua_gettop(ctx->cur_co_ctx->co);

                    next_coctx = ctx->cur_co_ctx->parent_co_ctx;
                    next_co = next_coctx->co;

                    /*
                     * prepare return values for coroutine.resume
                     * (true plus any retvals)
                     */
                    lua_pushboolean(next_co, 1);

                    if (nrets) {
                        dd("moving %d return values to next co", nrets);
                        lua_xmove(ctx->cur_co_ctx->co, next_co, nrets);
#ifdef NGX_LUA_USE_ASSERT
                        ctx->cur_co_ctx->co_top -= nrets;
#endif
                    }

                    nrets++;  /* add the true boolean value */

                    ctx->cur_co_ctx = next_coctx;

                    break;
                }

                /* try resuming on the new coroutine again */
                continue;

            case 0:

                ngx_stream_lua_cleanup_pending_operation(ctx->cur_co_ctx);

#if 0
                /* TODO */
                ngx_stream_lua_probe_coroutine_done(s, ctx->cur_co_ctx->co, 1);
#endif

                ctx->cur_co_ctx->co_status = NGX_STREAM_LUA_CO_DEAD;

                if (ctx->cur_co_ctx->zombie_child_threads) {
                    ngx_stream_lua_cleanup_zombie_child_uthreads(s, L, ctx,
                                                               ctx->cur_co_ctx);
                }

                ngx_log_debug0(NGX_LOG_DEBUG_STREAM, c->log, 0,
                               "stream lua light thread ended normally");

                if (ngx_stream_lua_is_entry_thread(ctx)) {

                    lua_settop(L, 0);

                    ngx_stream_lua_del_thread(s, L, ctx, ctx->cur_co_ctx);

                    dd("uthreads: %d", (int) ctx->uthreads);

                    if (ctx->uthreads) {

                        ctx->cur_co_ctx = NULL;
                        return NGX_AGAIN;
                    }

                    /* all user threads terminated already */
                    goto done;
                }

                if (ctx->cur_co_ctx->is_uthread) {
                    /* being a user thread */

                    lua_settop(L, 0);

                    parent_coctx = ctx->cur_co_ctx->parent_co_ctx;

                    if (ngx_stream_lua_coroutine_alive(parent_coctx)) {
                        if (ctx->cur_co_ctx->waited_by_parent) {
                            ngx_stream_lua_probe_info("parent already waiting");
                            ctx->cur_co_ctx->waited_by_parent = 0;
                            success = 1;
                            goto user_co_done;
                        }

                        ngx_stream_lua_probe_info("parent still alive");

                        if (ngx_stream_lua_post_zombie_thread(s, parent_coctx,
                                                            ctx->cur_co_ctx)
                            != NGX_OK)
                        {
                            return NGX_ERROR;
                        }

                        lua_pushboolean(ctx->cur_co_ctx->co, 1);
                        lua_insert(ctx->cur_co_ctx->co, 1);

                        ctx->cur_co_ctx->co_status = NGX_STREAM_LUA_CO_ZOMBIE;
                        ctx->cur_co_ctx = NULL;
                        return NGX_AGAIN;
                    }

                    ngx_stream_lua_del_thread(s, L, ctx, ctx->cur_co_ctx);
                    ctx->uthreads--;

                    if (ctx->uthreads == 0) {
                        if (ngx_stream_lua_entry_thread_alive(ctx)) {
                            ctx->cur_co_ctx = NULL;
                            return NGX_AGAIN;
                        }

                        /* all threads terminated already */
                        goto done;
                    }

                    /* some other user threads still running */
                    ctx->cur_co_ctx = NULL;
                    return NGX_AGAIN;
                }

                /* being a user coroutine that has a parent */

                success = 1;

user_co_done:

                nrets = lua_gettop(ctx->cur_co_ctx->co);

                next_coctx = ctx->cur_co_ctx->parent_co_ctx;

                if (next_coctx == NULL) {
                    /* being a light thread */
                    goto no_parent;
                }

                next_co = next_coctx->co;

                /*
                 * ended successful, coroutine.resume returns true plus
                 * any return values
                 */
                lua_pushboolean(next_co, success);

                if (nrets) {
                    lua_xmove(ctx->cur_co_ctx->co, next_co, nrets);
                }

                if (ctx->cur_co_ctx->is_uthread) {
                    ngx_stream_lua_del_thread(s, L, ctx, ctx->cur_co_ctx);
                    ctx->uthreads--;
                }

                nrets++;
                ctx->cur_co_ctx = next_coctx;

                ngx_stream_lua_probe_info("set parent running");

                next_coctx->co_status = NGX_STREAM_LUA_CO_RUNNING;

                ngx_log_debug0(NGX_LOG_DEBUG_STREAM, c->log, 0,
                               "stream lua coroutine: lua user thread "
                               "ended normally");

                continue;

            case LUA_ERRRUN:
                err = "runtime error";
                break;

            case LUA_ERRSYNTAX:
                err = "syntax error";
                break;

            case LUA_ERRMEM:
                err = "memory allocation error";
                ngx_quit = 1;
                break;

            case LUA_ERRERR:
                err = "error handler error";
                break;

            default:
                err = "unknown error";
                break;
            }

            if (ctx->cur_co_ctx != orig_coctx) {
                ctx->cur_co_ctx = orig_coctx;
            }

            if (lua_isstring(ctx->cur_co_ctx->co, -1)) {
                dd("user custom error msg");
                msg = lua_tostring(ctx->cur_co_ctx->co, -1);

            } else {
                msg = "unknown reason";
            }

            ngx_stream_lua_cleanup_pending_operation(ctx->cur_co_ctx);

#if 0
            /* TODO */
            ngx_stream_lua_probe_coroutine_done(s, ctx->cur_co_ctx->co, 0);
#endif

            ctx->cur_co_ctx->co_status = NGX_STREAM_LUA_CO_DEAD;

            ngx_stream_lua_thread_traceback(L, ctx->cur_co_ctx->co,
                                            ctx->cur_co_ctx);
            trace = lua_tostring(L, -1);

            if (ctx->cur_co_ctx->is_uthread) {

                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "stream lua user thread aborted: %s: %s\n%s",
                              err, msg, trace);

                lua_settop(L, 0);

                parent_coctx = ctx->cur_co_ctx->parent_co_ctx;

                if (ngx_stream_lua_coroutine_alive(parent_coctx)) {
                    if (ctx->cur_co_ctx->waited_by_parent) {
                        ctx->cur_co_ctx->waited_by_parent = 0;
                        success = 0;
                        goto user_co_done;
                    }

                    if (ngx_stream_lua_post_zombie_thread(s, parent_coctx,
                                                        ctx->cur_co_ctx)
                        != NGX_OK)
                    {
                        return NGX_ERROR;
                    }

                    lua_pushboolean(ctx->cur_co_ctx->co, 0);
                    lua_insert(ctx->cur_co_ctx->co, 1);

                    ctx->cur_co_ctx->co_status = NGX_STREAM_LUA_CO_ZOMBIE;
                    ctx->cur_co_ctx = NULL;
                    return NGX_AGAIN;
                }

                ngx_stream_lua_del_thread(s, L, ctx, ctx->cur_co_ctx);
                ctx->uthreads--;

                if (ctx->uthreads == 0) {
                    if (ngx_stream_lua_entry_thread_alive(ctx)) {
                        ctx->cur_co_ctx = NULL;
                        return NGX_AGAIN;
                    }

                    /* all threads terminated already */
                    goto done;
                }

                /* some other user threads still running */
                ctx->cur_co_ctx = NULL;
                return NGX_AGAIN;
            }

            if (ngx_stream_lua_is_entry_thread(ctx)) {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "stream lua entry thread aborted: %s: %s\n%s",
                              err, msg, trace);

                lua_settop(L, 0);

                /* being the entry thread aborted */

                ngx_stream_lua_session_cleanup(ctx, 0);

                if (ctx->no_abort) {
                    ctx->no_abort = 0;
                    return NGX_ERROR;
                }

                return NGX_ERROR;
            }

            /* being a user coroutine that has a parent */

            next_coctx = ctx->cur_co_ctx->parent_co_ctx;
            if (next_coctx == NULL) {
                goto no_parent;
            }

            next_co = next_coctx->co;

            ngx_stream_lua_probe_info("set parent running");

            next_coctx->co_status = NGX_STREAM_LUA_CO_RUNNING;

            /*
             * ended with error, coroutine.resume returns false plus
             * err msg
             */
            lua_pushboolean(next_co, 0);
            lua_xmove(ctx->cur_co_ctx->co, next_co, 1);
            nrets = 2;

            ctx->cur_co_ctx = next_coctx;

            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "stream lua coroutine: %s: %s\n%s", err, msg, trace);

            /* try resuming on the new coroutine again */
            continue;
        }

    } NGX_LUA_EXCEPTION_CATCH {
        dd("nginx execution restored");
    }

    return NGX_ERROR;

no_parent:

    lua_settop(L, 0);

    ctx->cur_co_ctx->co_status = NGX_STREAM_LUA_CO_DEAD;

    ngx_stream_lua_session_cleanup(ctx, 0);

    ngx_log_error(NGX_LOG_ERR, c->log, 0, "stream lua handler aborted: "
                  "user coroutine has no parent");

    return NGX_ERROR;

done:

    return NGX_OK;
}


ngx_int_t
ngx_stream_lua_run_posted_threads(ngx_connection_t *c, lua_State *L,
    ngx_stream_session_t *s, ngx_stream_lua_ctx_t *ctx)
{
    ngx_int_t                        rc;
    ngx_stream_lua_posted_thread_t  *pt;

    for ( ;; ) {
        if (c->destroyed) {
            return NGX_DONE;
        }

        pt = ctx->posted_threads;
        if (pt == NULL) {
            return NGX_DONE;
        }

        ctx->posted_threads = pt->next;

#if 0
        /* TODO */
        ngx_stream_lua_probe_run_posted_thread(s, pt->co_ctx->co,
                                               (int) pt->co_ctx->co_status);
#endif

        if (pt->co_ctx->co_status != NGX_STREAM_LUA_CO_RUNNING) {
            continue;
        }

        ctx->cur_co_ctx = pt->co_ctx;

        rc = ngx_stream_lua_run_thread(L, s, ctx, 0);

        if (rc == NGX_AGAIN) {
            continue;
        }

        if (rc == NGX_DONE) {
            ngx_stream_lua_finalize_session(s, NGX_DONE);
            continue;
        }

        /* rc == NGX_ERROR || rc >= NGX_OK */

#if 1
        if (rc == NGX_OK) {
            ngx_stream_lua_finalize_session(s, NGX_OK);
            return NGX_DONE;
        }
#endif

        return rc;
    }

    /* impossible to reach here */
}


static void
ngx_stream_lua_finalize_threads(ngx_stream_session_t *s,
    ngx_stream_lua_ctx_t *ctx, lua_State *L)
{
#ifdef NGX_LUA_USE_ASSERT
    int                              top;
#endif
    int                              inited = 0, ref;
    ngx_uint_t                       i;
    ngx_list_part_t                 *part;
    ngx_stream_lua_co_ctx_t         *cc, *coctx;

#ifdef NGX_LUA_USE_ASSERT
    top = lua_gettop(L);
#endif

#if 1
    coctx = ctx->on_abort_co_ctx;
    if (coctx && coctx->co_ref != LUA_NOREF) {
        if (coctx->co_status != NGX_STREAM_LUA_CO_SUSPENDED) {
            /* the on_abort thread contributes to the coctx->uthreads
             * counter only when it actually starts running */
            ngx_stream_lua_cleanup_pending_operation(coctx);
            ctx->uthreads--;
        }

#if 0
        /* TODO */
        ngx_stream_lua_probe_thread_delete(s, coctx->co, ctx);
#endif

        lua_pushlightuserdata(L, &ngx_stream_lua_coroutines_key);
        lua_rawget(L, LUA_REGISTRYINDEX);
        inited = 1;

        luaL_unref(L, -1, coctx->co_ref);
        coctx->co_ref = LUA_NOREF;

        coctx->co_status = NGX_STREAM_LUA_CO_DEAD;
        ctx->on_abort_co_ctx = NULL;
    }
#endif

    if (ctx->user_co_ctx) {
        part = &ctx->user_co_ctx->part;
        cc = part->elts;

        for (i = 0; /* void */; i++) {

            if (i >= part->nelts) {
                if (part->next == NULL) {
                    break;
                }

                part = part->next;
                cc = part->elts;
                i = 0;
            }

            coctx = &cc[i];

            ref = coctx->co_ref;

            if (ref != LUA_NOREF) {
                ngx_stream_lua_cleanup_pending_operation(coctx);

#if 0
                /* TODO */
                ngx_stream_lua_probe_thread_delete(s, coctx->co, ctx);
#endif

                if (!inited) {
                    lua_pushlightuserdata(L, &ngx_stream_lua_coroutines_key);
                    lua_rawget(L, LUA_REGISTRYINDEX);
                    inited = 1;
                }

                ngx_stream_lua_assert(lua_gettop(L) - top == 1);

                luaL_unref(L, -1, ref);
                coctx->co_ref = LUA_NOREF;

                coctx->co_status = NGX_STREAM_LUA_CO_DEAD;
                ctx->uthreads--;
            }
        }

        ctx->user_co_ctx = NULL;
    }

    ngx_stream_lua_assert(ctx->uthreads == 0);

    coctx = &ctx->entry_co_ctx;

    ref = coctx->co_ref;
    if (ref != LUA_NOREF) {
        ngx_stream_lua_cleanup_pending_operation(coctx);

#if 0
        /* TODO */
        ngx_stream_lua_probe_thread_delete(s, coctx->co, ctx);
#endif

        if (!inited) {
            lua_pushlightuserdata(L, &ngx_stream_lua_coroutines_key);
            lua_rawget(L, LUA_REGISTRYINDEX);
            inited = 1;
        }

        ngx_stream_lua_assert(lua_gettop(L) - top == 1);

        luaL_unref(L, -1, coctx->co_ref);
        coctx->co_ref = LUA_NOREF;
        coctx->co_status = NGX_STREAM_LUA_CO_DEAD;
    }

    if (inited) {
        lua_pop(L, 1);
    }
}


void
ngx_stream_lua_create_new_globals_table(lua_State *L, int narr, int nrec)
{
    lua_createtable(L, narr, nrec + 1);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "_G");
}


void
ngx_stream_lua_finalize_fake_session(ngx_stream_session_t *s, ngx_int_t rc)
{
    ngx_connection_t          *c;
#if 0 && (NGX_STREAM_SSL)
    /* TODO */
    ngx_ssl_conn_t            *ssl_conn;

    ngx_stream_lua_ssl_cert_ctx_t     *cctx;
#endif

    c = s->connection;

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream lua finalize fake session: %d", rc);

    if (rc == NGX_DONE) {
        return;
    }

    if (rc == NGX_ERROR) {

#if 0 && (NGX_STREAM_SSL)
        /* TODO */

        if (s->connection->ssl) {
            ssl_conn = s->connection->ssl->connection;
            if (ssl_conn) {
                c = ngx_ssl_get_connection(ssl_conn);

                if (c && c->ssl) {
                    cctx = ngx_stream_lua_ssl_get_ctx(c->ssl->connection);
                    if (cctx != NULL) {
                        cctx->exit_code = 0;
                    }
                }
            }
        }

#endif

        ngx_stream_lua_close_fake_session(s);
        return;
    }

    if (c->read->timer_set) {
        ngx_del_timer(c->read);
    }

    if (c->write->timer_set) {
        c->write->delayed = 0;
        ngx_del_timer(c->write);
    }

    ngx_stream_lua_close_fake_session(s);
}


static void
ngx_stream_lua_close_fake_session(ngx_stream_session_t *s)
{
    ngx_connection_t  *c;

    c = s->connection;

    ngx_stream_lua_free_fake_session(s);
    ngx_stream_lua_close_fake_connection(c);
}


void
ngx_stream_lua_free_fake_session(ngx_stream_session_t *s)
{
    ngx_log_t                   *log;
    ngx_stream_lua_ctx_t        *ctx;
    ngx_stream_lua_cleanup_t    *cln;

    log = s->connection->log;

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, log, 0,
                   "stream lua free fake session");

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_lua_module);
    if (ctx == NULL) {
        goto done;
    }

    cln = ctx->cleanup;
    ctx->cleanup = NULL;

    while (cln) {
        if (cln->handler) {
            cln->handler(cln->data);
        }

        cln = cln->next;
    }

done:

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, log, 0, "stream lua close fake "
                   "session");

    if (s->connection->pool == NULL) {
        ngx_log_error(NGX_LOG_ALERT, log, 0, "stream lua fake session "
                      "already closed");
        return;
    }

    s->connection->destroyed = 1;
}


void
ngx_stream_lua_close_fake_connection(ngx_connection_t *c)
{
    ngx_pool_t          *pool;
    ngx_connection_t    *saved_c = NULL;

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream lua close fake stream connection %p", c);

    c->destroyed = 1;

    pool = c->pool;

    if (c->read->timer_set) {
        ngx_del_timer(c->read);
    }

    if (c->write->timer_set) {
        ngx_del_timer(c->write);
    }

    c->read->closed = 1;
    c->write->closed = 1;

    /* we temporarily use a valid fd (0) to make ngx_free_connection happy */

    c->fd = 0;

    if (ngx_cycle->files) {
        saved_c = ngx_cycle->files[0];
    }

    ngx_free_connection(c);

    c->fd = (ngx_socket_t) -1;

    if (ngx_cycle->files) {
        ngx_cycle->files[0] = saved_c;
    }

    if (pool) {
        ngx_destroy_pool(pool);
    }
}


static ngx_int_t
ngx_stream_lua_on_abort_resume(ngx_stream_session_t *s,
    ngx_stream_lua_ctx_t *ctx)
{
    lua_State                   *vm;
    ngx_int_t                    rc;
    ngx_connection_t            *c;

    ctx->resume_handler = ngx_stream_lua_wev_handler;

    c = s->connection;

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream lua resuming the on_abort callback thread");

#if 0
    ngx_stream_lua_probe_info("tcp resume");
#endif

    vm = ngx_stream_lua_get_lua_vm(s, ctx);

    rc = ngx_stream_lua_run_thread(vm, s, ctx, 0);

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream lua run thread returned %d", rc);

    if (rc == NGX_AGAIN) {
        return ngx_stream_lua_run_posted_threads(c, vm, s, ctx);
    }

    if (rc == NGX_DONE) {
        ngx_stream_lua_finalize_session(s, NGX_DONE);
        return ngx_stream_lua_run_posted_threads(c, vm, s, ctx);
    }

    if (ctx->entered_content_phase) {
        ngx_stream_lua_finalize_session(s, rc);
        return NGX_DONE;
    }

    return rc;
}


void
ngx_stream_lua_session_handler(ngx_event_t *ev)
{
    ngx_connection_t        *c;
    ngx_stream_session_t    *s;
    ngx_stream_lua_ctx_t    *ctx;

    c = ev->data;
    s = c->data;

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream lua run session: write:%d", ev->write);

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_lua_module);
    if (ctx == NULL) {
        return;
    }

    if (ev->write) {
        ctx->write_event_handler(s, ctx);

    } else {
        ctx->read_event_handler(s, ctx);
    }
}


static void
ngx_stream_lua_set_path(ngx_cycle_t *cycle, lua_State *L, int tab_idx,
    const char *fieldname, const char *path, const char *default_path,
    ngx_log_t *log)
{
    const char          *tmp_path;
    const char          *prefix;

    /* XXX here we use some hack to simplify string manipulation */
    tmp_path = luaL_gsub(L, path, LUA_PATH_SEP LUA_PATH_SEP,
                         LUA_PATH_SEP AUX_MARK LUA_PATH_SEP);

    lua_pushlstring(L, (char *) cycle->prefix.data, cycle->prefix.len);
    prefix = lua_tostring(L, -1);
    tmp_path = luaL_gsub(L, tmp_path, "$prefix", prefix);
    tmp_path = luaL_gsub(L, tmp_path, "${prefix}", prefix);
    lua_pop(L, 3);

    dd("tmp_path path: %s", tmp_path);

#if (NGX_DEBUG)
    tmp_path =
#else
    (void)
#endif
        luaL_gsub(L, tmp_path, AUX_MARK, default_path);

#if (NGX_DEBUG)
    ngx_log_debug2(NGX_LOG_DEBUG_STREAM, log, 0,
                   "stream lua setting lua package.%s to \"%s\"",
                   fieldname, tmp_path);
#endif

    lua_remove(L, -2);

    /* fix negative index as there's new data on stack */
    tab_idx = (tab_idx < 0) ? (tab_idx - 1) : tab_idx;
    lua_setfield(L, tab_idx, fieldname);
}


static void
ngx_stream_lua_init_registry(lua_State *L, ngx_log_t *log)
{
    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, log, 0,
                   "stream lua initializing lua registry");

    /* {{{ register a table to anchor lua coroutines reliably:
     * {([int]ref) = [cort]} */
    lua_pushlightuserdata(L, &ngx_stream_lua_coroutines_key);
    lua_createtable(L, 0, 32 /* nrec */);
    lua_rawset(L, LUA_REGISTRYINDEX);
    /* }}} */

    /* create the registry entry for the Lua session ctx data table */
    lua_pushliteral(L, ngx_stream_lua_ctx_tables_key);
    lua_createtable(L, 0, 32 /* nrec */);
    lua_rawset(L, LUA_REGISTRYINDEX);

    /* create the registry entry for the Lua socket connection pool table */
    lua_pushlightuserdata(L, &ngx_stream_lua_socket_pool_key);
    lua_createtable(L, 0, 8 /* nrec */);
    lua_rawset(L, LUA_REGISTRYINDEX);

#if (NGX_PCRE)
    /* create the registry entry for the Lua precompiled regex object cache */
    lua_pushlightuserdata(L, &ngx_stream_lua_regex_cache_key);
    lua_createtable(L, 0, 16 /* nrec */);
    lua_rawset(L, LUA_REGISTRYINDEX);
#endif

    /* {{{ register table to cache user code:
     * { [(string)cache_key] = <code closure> } */
    lua_pushlightuserdata(L, &ngx_stream_lua_code_cache_key);
    lua_createtable(L, 0, 8 /* nrec */);
    lua_rawset(L, LUA_REGISTRYINDEX);
    /* }}} */
}


static void
ngx_stream_lua_init_globals(lua_State *L, ngx_cycle_t *cycle,
    ngx_stream_lua_main_conf_t *lmcf, ngx_log_t *log)
{
    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, log, 0,
                   "stream lua initializing lua globals");

    lua_pushlightuserdata(L, cycle);
    lua_setglobal(L, "__ngx_cycle");

    ngx_stream_lua_inject_ngx_api(L, lmcf, log);
}


static ngx_int_t
ngx_stream_lua_handle_exit(lua_State *L, ngx_stream_session_t *s,
    ngx_stream_lua_ctx_t *ctx)
{
    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, s->connection->log, 0,
                   "stream lua thread aborting session with status %d",
                   ctx->exit_code);

    ngx_stream_lua_cleanup_pending_operation(ctx->cur_co_ctx);

#if 0
    /* TODO */
    ngx_stream_lua_probe_coroutine_done(s, ctx->cur_co_ctx->co, 1);
#endif

    ctx->cur_co_ctx->co_status = NGX_STREAM_LUA_CO_DEAD;

    ngx_stream_lua_session_cleanup(ctx, 0);

    if (s->connection->fd == (ngx_socket_t) -1) {  /* fake session */
        return ctx->exit_code;
    }

    if (ctx->exit_code != NGX_ERROR)
    {
        return ctx->exit_code;
    }

    return ctx->exit_code;
}


ngx_int_t
ngx_stream_lua_post_thread(ngx_stream_session_t *s, ngx_stream_lua_ctx_t *ctx,
    ngx_stream_lua_co_ctx_t *coctx)
{
    ngx_stream_lua_posted_thread_t  **p;
    ngx_stream_lua_posted_thread_t   *pt;

    pt = ngx_palloc(s->connection->pool,
                    sizeof(ngx_stream_lua_posted_thread_t));
    if (pt == NULL) {
        return NGX_ERROR;
    }

    pt->co_ctx = coctx;
    pt->next = NULL;

    for (p = &ctx->posted_threads; *p; p = &(*p)->next) { /* void */ }

    *p = pt;

    return NGX_OK;
}


static void
ngx_stream_lua_cleanup_zombie_child_uthreads(ngx_stream_session_t *s,
    lua_State *L, ngx_stream_lua_ctx_t *ctx, ngx_stream_lua_co_ctx_t *coctx)
{
    ngx_stream_lua_posted_thread_t   *pt;

    for (pt = coctx->zombie_child_threads; pt; pt = pt->next) {
        if (pt->co_ctx->co_ref != LUA_NOREF) {
            ngx_stream_lua_del_thread(s, L, ctx, pt->co_ctx);
            ctx->uthreads--;
        }
    }

    coctx->zombie_child_threads = NULL;
}


static ngx_int_t
ngx_stream_lua_post_zombie_thread(ngx_stream_session_t *s,
    ngx_stream_lua_co_ctx_t *parent, ngx_stream_lua_co_ctx_t *thread)
{
    ngx_stream_lua_posted_thread_t  **p;
    ngx_stream_lua_posted_thread_t   *pt;

    pt = ngx_palloc(s->connection->pool,
                    sizeof(ngx_stream_lua_posted_thread_t));
    if (pt == NULL) {
        return NGX_ERROR;
    }

    pt->co_ctx = thread;
    pt->next = NULL;

    for (p = &parent->zombie_child_threads; *p; p = &(*p)->next) { /* void */ }

    *p = pt;

    return NGX_OK;
}


int
ngx_stream_lua_traceback(lua_State *L)
{
    if (!lua_isstring(L, 1)) { /* 'message' not a string? */
        return 1;  /* keep it intact */
    }

    lua_getglobal(L, "debug");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return 1;
    }

    lua_getfield(L, -1, "traceback");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return 1;
    }

    lua_pushvalue(L, 1);  /* pass error message */
    lua_pushinteger(L, 2);  /* skip this function and traceback */
    lua_call(L, 2, 1);  /* call debug.traceback */
    return 1;
}


static int
ngx_stream_lua_thread_traceback(lua_State *L, lua_State *co,
    ngx_stream_lua_co_ctx_t *coctx)
{
    int         base;
    int         level, coid;
    lua_Debug   ar;

    base = lua_gettop(L);
    lua_checkstack(L, 3);
    lua_pushliteral(L, "stack traceback:");
    coid = 0;

    while (co) {

        if (coid >= NGX_STREAM_LUA_BT_MAX_COROS) {
            break;
        }

        lua_checkstack(L, 2);
        lua_pushfstring(L, "\ncoroutine %d:", coid++);

        level = 0;

        while (lua_getstack(co, level++, &ar)) {

            lua_checkstack(L, 5);

            if (level > NGX_STREAM_LUA_BT_DEPTH) {
                lua_pushliteral(L, "\n\t...");
                break;
            }

            lua_pushliteral(L, "\n\t");
            lua_getinfo(co, "Snl", &ar);
            lua_pushfstring(L, "%s:", ar.short_src);

            if (ar.currentline > 0) {
                lua_pushfstring(L, "%d:", ar.currentline);
            }

            if (*ar.namewhat != '\0') {  /* is there a name? */
                lua_pushfstring(L, " in function " LUA_QS, ar.name);

            } else {
                if (*ar.what == 'm') {  /* main? */
                    lua_pushliteral(L, " in main chunk");

                } else if (*ar.what == 'C' || *ar.what == 't') {
                    lua_pushliteral(L, " ?");  /* C function or tail call */

                } else {
                    lua_pushfstring(L, " in function <%s:%d>",
                                    ar.short_src, ar.linedefined);
                }
            }
        }

        if (lua_gettop(L) - base >= 15) {
            lua_concat(L, lua_gettop(L) - base);
        }

        /* check if the coroutine has a parent coroutine*/
        coctx = coctx->parent_co_ctx;
        if (!coctx || coctx->co_status == NGX_STREAM_LUA_CO_DEAD) {
            break;
        }

        co = coctx->co;
    }

    lua_concat(L, lua_gettop(L) - base);
    return 1;
}


ngx_stream_lua_co_ctx_t *
ngx_stream_lua_get_co_ctx(lua_State *L, ngx_stream_lua_ctx_t *ctx)
{
    ngx_uint_t                   i;
    ngx_list_part_t             *part;
    ngx_stream_lua_co_ctx_t     *coctx;

    if (L == ctx->entry_co_ctx.co) {
        return &ctx->entry_co_ctx;
    }

    if (ctx->user_co_ctx == NULL) {
        return NULL;
    }

    part = &ctx->user_co_ctx->part;
    coctx = part->elts;

    /* FIXME: we should use rbtree here to prevent O(n) lookup overhead */

    for (i = 0; /* void */; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            coctx = part->elts;
            i = 0;
        }

        if (coctx[i].co == L) {
            return &coctx[i];
        }
    }

    return NULL;
}


ngx_stream_lua_co_ctx_t *
ngx_stream_lua_create_co_ctx(ngx_stream_session_t *s, ngx_stream_lua_ctx_t *ctx)
{
    ngx_stream_lua_co_ctx_t       *coctx;

    if (ctx->user_co_ctx == NULL) {
        ctx->user_co_ctx = ngx_list_create(s->connection->pool, 4,
                                           sizeof(ngx_stream_lua_co_ctx_t));
        if (ctx->user_co_ctx == NULL) {
            return NULL;
        }
    }

    coctx = ngx_list_push(ctx->user_co_ctx);
    if (coctx == NULL) {
        return NULL;
    }

    ngx_memzero(coctx, sizeof(ngx_stream_lua_co_ctx_t));

    coctx->co_ref = LUA_NOREF;

    return coctx;
}


ngx_stream_lua_cleanup_t *
ngx_stream_lua_cleanup_add(ngx_stream_session_t *s, size_t size)
{
    ngx_stream_lua_cleanup_t      *cln;
    ngx_stream_lua_ctx_t          *ctx;

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_lua_module);
    if (ctx == NULL) {
        return NULL;
    }

    if (ctx->free_cleanup) {
        cln = ctx->free_cleanup;
        ctx->free_cleanup = cln->next;

        dd("reuse cleanup: %p", cln);

        ngx_log_debug1(NGX_LOG_DEBUG_STREAM, s->connection->log, 0,
                       "stream lua stream cleanup reuse: %p", cln);

    } else {
        cln = ngx_palloc(s->connection->pool, sizeof(ngx_stream_lua_cleanup_t));
        if (cln == NULL) {
            return NULL;
        }
    }

    if (size) {
        cln->data = ngx_palloc(s->connection->pool, size);
        if (cln->data == NULL) {
            return NULL;
        }

    } else {
        cln->data = NULL;
    }

    cln->handler = NULL;
    cln->next = ctx->cleanup;
    ctx->cleanup = cln;

    return cln;
}


void
ngx_stream_lua_cleanup_free(ngx_stream_session_t *s,
    ngx_pool_cleanup_pt *cleanup)
{
    ngx_stream_lua_cleanup_t      **last;
    ngx_stream_lua_cleanup_t       *cln;
    ngx_stream_lua_ctx_t           *ctx;

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_lua_module);
    if (ctx == NULL) {
        return;
    }

    cln = (ngx_stream_lua_cleanup_t *)
          ((u_char *) cleanup - offsetof(ngx_stream_lua_cleanup_t, handler));

    dd("cln: %p, cln->handler: %p, &cln->handler: %p",
       cln, cln->handler, &cln->handler);

    last = &ctx->cleanup;

    while (*last) {
        if (*last == cln) {
            *last = cln->next;

            cln->next = ctx->free_cleanup;
            ctx->free_cleanup = cln;

            ngx_log_debug1(NGX_LOG_DEBUG_STREAM, s->connection->log, 0,
                           "stream lua stream cleanup free: %p", cln);

            return;
        }

        last = &(*last)->next;
    }
}


ngx_chain_t *
ngx_stream_lua_chain_get_free_buf(ngx_log_t *log, ngx_pool_t *p,
    ngx_chain_t **free, size_t len)
{
    ngx_buf_t    *b;
    ngx_chain_t  *cl;
    u_char       *start, *end;

    const ngx_buf_tag_t  tag = (ngx_buf_tag_t) &ngx_stream_lua_module;

    if (*free) {
        cl = *free;
        *free = cl->next;
        cl->next = NULL;

        b = cl->buf;
        start = b->start;
        end = b->end;
        if (start && (size_t) (end - start) >= len) {
            ngx_log_debug4(NGX_LOG_DEBUG_STREAM, log, 0,
                           "stream lua reuse free buf memory %O >= %uz, "
                           "cl:%p, p:%p", (off_t) (end - start), len, cl,
                           start);

            ngx_memzero(b, sizeof(ngx_buf_t));

            b->start = start;
            b->pos = start;
            b->last = start;
            b->end = end;
            b->tag = tag;

            if (len) {
                b->temporary = 1;
            }

            return cl;
        }

        ngx_log_debug4(NGX_LOG_DEBUG_STREAM, log, 0,
                       "stream lua reuse free buf chain, but reallocate memory "
                       "because %uz >= %O, cl:%p, p:%p", len,
                       (off_t) (b->end - b->start), cl, b->start);

        if (ngx_buf_in_memory(b) && b->start) {
            ngx_pfree(p, b->start);
        }

        ngx_memzero(b, sizeof(ngx_buf_t));

        if (len == 0) {
            return cl;
        }

        b->start = ngx_palloc(p, len);
        if (b->start == NULL) {
            return NULL;
        }

        b->end = b->start + len;

        dd("buf start: %p", cl->buf->start);

        b->pos = b->start;
        b->last = b->start;
        b->tag = tag;
        b->temporary = 1;

        return cl;
    }

    cl = ngx_alloc_chain_link(p);
    if (cl == NULL) {
        return NULL;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_STREAM, log, 0,
                   "stream lua allocate new chainlink and new buf of "
                   "size %uz, cl:%p", len, cl);

    cl->buf = len ? ngx_create_temp_buf(p, len) : ngx_calloc_buf(p);
    if (cl->buf == NULL) {
        return NULL;
    }

    dd("buf start: %p", cl->buf->start);

    cl->buf->tag = tag;
    cl->next = NULL;

    return cl;
}


static void
ngx_stream_lua_empty_handler(ngx_event_t *wev)
{
    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, wev->log, 0,
                   "stream lua empty handler");
    return;
}


static void
ngx_stream_lua_inject_ngx_api(lua_State *L, ngx_stream_lua_main_conf_t *lmcf,
    ngx_log_t *log)
{
    lua_createtable(L, 0 /* narr */, 56 /* nrec */);    /* ngx.* */

    lua_pushcfunction(L, ngx_stream_lua_get_raw_phase_context);
    lua_setfield(L, -2, "_phase_ctx");

    ngx_stream_lua_inject_core_consts(L);

    ngx_stream_lua_inject_log_api(L);
    ngx_stream_lua_inject_output_api(L);
    ngx_stream_lua_inject_time_api(L);
    ngx_stream_lua_inject_string_api(L);
    ngx_stream_lua_inject_control_api(log, L);
    ngx_stream_lua_inject_sleep_api(L);
    ngx_stream_lua_inject_phase_api(L);

#if (NGX_PCRE)
    ngx_stream_lua_inject_regex_api(L);
#endif

    ngx_stream_lua_inject_req_api(log, L);
    ngx_stream_lua_inject_variable_api(L);
    ngx_stream_lua_inject_shdict_api(lmcf, L);
    ngx_stream_lua_inject_socket_tcp_api(log, L);
    ngx_stream_lua_inject_socket_udp_api(log, L);
    ngx_stream_lua_inject_uthread_api(log, L);
    ngx_stream_lua_inject_timer_api(L);
    ngx_stream_lua_inject_config_api(L);
    ngx_stream_lua_inject_worker_api(L);

    ngx_stream_lua_inject_misc_api(L);

    lua_getglobal(L, "package"); /* ngx package */
    lua_getfield(L, -1, "loaded"); /* ngx package loaded */
    lua_pushvalue(L, -3); /* ngx package loaded ngx */
    lua_setfield(L, -2, "ngx"); /* ngx package loaded */
    lua_pop(L, 2);

    lua_setglobal(L, "ngx");

    ngx_stream_lua_inject_coroutine_api(log, L);
}


static int
ngx_stream_lua_get_raw_phase_context(lua_State *L)
{
    ngx_stream_session_t      *s;
    ngx_stream_lua_ctx_t      *ctx;

    s = lua_touserdata(L, 1);
    if (s == NULL) {
        return 0;
    }

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_lua_module);
    if (ctx == NULL) {
        return 0;
    }

    lua_pushinteger(L, (int) ctx->context);
    return 1;
}


uintptr_t
ngx_stream_lua_escape_uri(u_char *dst, u_char *src, size_t size,
    ngx_uint_t type)
{
    ngx_uint_t      n;
    uint32_t       *escape;
    static u_char   hex[] = "0123456789ABCDEF";

                    /* " ", "#", "%", "?", %00-%1F, %7F-%FF */

    static uint32_t   uri[] = {
        0xffffffff, /* 1111 1111 1111 1111  1111 1111 1111 1111 */

                    /* ?>=< ;:98 7654 3210  /.-, +*)( '&%$ #"!  */
        0xfc00886d, /* 1111 1100 0000 0000  1000 1000 0110 1101 */

                    /* _^]\ [ZYX WVUT SRQP  ONML KJIH GFED CBA@ */
        0x78000000, /* 0111 1000 0000 0000  0000 0000 0000 0000 */

                    /*  ~}| {zyx wvut srqp  onml kjih gfed cba` */
        0xa8000000, /* 1010 1000 0000 0000  0000 0000 0000 0000 */

        0xffffffff, /* 1111 1111 1111 1111  1111 1111 1111 1111 */
        0xffffffff, /* 1111 1111 1111 1111  1111 1111 1111 1111 */
        0xffffffff, /* 1111 1111 1111 1111  1111 1111 1111 1111 */
        0xffffffff  /* 1111 1111 1111 1111  1111 1111 1111 1111 */
    };

                    /* " ", "#", "%", "+", "?", %00-%1F, %7F-%FF */

    static uint32_t   args[] = {
        0xffffffff, /* 1111 1111 1111 1111  1111 1111 1111 1111 */

                    /* ?>=< ;:98 7654 3210  /.-, +*)( '&%$ #"!  */
        0x80000829, /* 1000 0000 0000 0000  0000 1000 0010 1001 */

                    /* _^]\ [ZYX WVUT SRQP  ONML KJIH GFED CBA@ */
        0x00000000, /* 0000 0000 0000 0000  0000 0000 0000 0000 */

                    /*  ~}| {zyx wvut srqp  onml kjih gfed cba` */
        0x80000000, /* 1000 0000 0000 0000  0000 0000 0000 0000 */

        0xffffffff, /* 1111 1111 1111 1111  1111 1111 1111 1111 */
        0xffffffff, /* 1111 1111 1111 1111  1111 1111 1111 1111 */
        0xffffffff, /* 1111 1111 1111 1111  1111 1111 1111 1111 */
        0xffffffff  /* 1111 1111 1111 1111  1111 1111 1111 1111 */
    };

                    /* " ", "#", """, "%", "'", %00-%1F, %7F-%FF */

    static uint32_t   html[] = {
        0xffffffff, /* 1111 1111 1111 1111  1111 1111 1111 1111 */

                    /* ?>=< ;:98 7654 3210  /.-, +*)( '&%$ #"!  */
        0x000000ad, /* 0000 0000 0000 0000  0000 0000 1010 1101 */

                    /* _^]\ [ZYX WVUT SRQP  ONML KJIH GFED CBA@ */
        0x00000000, /* 0000 0000 0000 0000  0000 0000 0000 0000 */

                    /*  ~}| {zyx wvut srqp  onml kjih gfed cba` */
        0x80000000, /* 1000 0000 0000 0000  0000 0000 0000 0000 */

        0xffffffff, /* 1111 1111 1111 1111  1111 1111 1111 1111 */
        0xffffffff, /* 1111 1111 1111 1111  1111 1111 1111 1111 */
        0xffffffff, /* 1111 1111 1111 1111  1111 1111 1111 1111 */
        0xffffffff  /* 1111 1111 1111 1111  1111 1111 1111 1111 */
    };

                    /* " ", """, "%", "'", %00-%1F, %7F-%FF */

    static uint32_t   refresh[] = {
        0xffffffff, /* 1111 1111 1111 1111  1111 1111 1111 1111 */

                    /* ?>=< ;:98 7654 3210  /.-, +*)( '&%$ #"!  */
        0x00000085, /* 0000 0000 0000 0000  0000 0000 1000 0101 */

                    /* _^]\ [ZYX WVUT SRQP  ONML KJIH GFED CBA@ */
        0x00000000, /* 0000 0000 0000 0000  0000 0000 0000 0000 */

                    /*  ~}| {zyx wvut srqp  onml kjih gfed cba` */
        0x80000000, /* 1000 0000 0000 0000  0000 0000 0000 0000 */

        0xffffffff, /* 1111 1111 1111 1111  1111 1111 1111 1111 */
        0xffffffff, /* 1111 1111 1111 1111  1111 1111 1111 1111 */
        0xffffffff, /* 1111 1111 1111 1111  1111 1111 1111 1111 */
        0xffffffff  /* 1111 1111 1111 1111  1111 1111 1111 1111 */
    };

                    /* " ", "%", %00-%1F */

    static uint32_t   memcached[] = {
        0xffffffff, /* 1111 1111 1111 1111  1111 1111 1111 1111 */

                    /* ?>=< ;:98 7654 3210  /.-, +*)( '&%$ #"!  */
        0x00000021, /* 0000 0000 0000 0000  0000 0000 0010 0001 */

                    /* _^]\ [ZYX WVUT SRQP  ONML KJIH GFED CBA@ */
        0x00000000, /* 0000 0000 0000 0000  0000 0000 0000 0000 */

                    /*  ~}| {zyx wvut srqp  onml kjih gfed cba` */
        0x00000000, /* 0000 0000 0000 0000  0000 0000 0000 0000 */

        0x00000000, /* 0000 0000 0000 0000  0000 0000 0000 0000 */
        0x00000000, /* 0000 0000 0000 0000  0000 0000 0000 0000 */
        0x00000000, /* 0000 0000 0000 0000  0000 0000 0000 0000 */
        0x00000000, /* 0000 0000 0000 0000  0000 0000 0000 0000 */
    };

                    /* mail_auth is the same as memcached */

    static uint32_t  *map[] =
        { uri, args, html, refresh, memcached, memcached };


    escape = map[type];

    if (dst == NULL) {

        /* find the number of the characters to be escaped */

        n = 0;

        while (size) {
            if (escape[*src >> 5] & (1 << (*src & 0x1f))) {
                n++;
            }
            src++;
            size--;
        }

        return (uintptr_t) n;
    }

    while (size) {
        if (escape[*src >> 5] & (1 << (*src & 0x1f))) {
            *dst++ = '%';
            *dst++ = hex[*src >> 4];
            *dst++ = hex[*src & 0xf];
            src++;

        } else {
            *dst++ = *src++;
        }
        size--;
    }

    return (uintptr_t) dst;
}


/* XXX we also decode '+' to ' ' */
void
ngx_stream_lua_unescape_uri(u_char **dst, u_char **src, size_t size,
    ngx_uint_t type)
{
    u_char  *d, *s, ch, c, decoded;
    enum {
        sw_usual = 0,
        sw_quoted,
        sw_quoted_second
    } state;

    d = *dst;
    s = *src;

    state = 0;
    decoded = 0;

    while (size--) {

        ch = *s++;

        switch (state) {
        case sw_usual:
            if (ch == '?'
                && (type & (NGX_UNESCAPE_URI|NGX_UNESCAPE_REDIRECT)))
            {
                *d++ = ch;
                goto done;
            }

            if (ch == '%') {
                state = sw_quoted;
                break;
            }

            if (ch == '+') {
                *d++ = ' ';
                break;
            }

            *d++ = ch;
            break;

        case sw_quoted:

            if (ch >= '0' && ch <= '9') {
                decoded = (u_char) (ch - '0');
                state = sw_quoted_second;
                break;
            }

            c = (u_char) (ch | 0x20);
            if (c >= 'a' && c <= 'f') {
                decoded = (u_char) (c - 'a' + 10);
                state = sw_quoted_second;
                break;
            }

            /* the invalid quoted character */

            state = sw_usual;

            *d++ = ch;

            break;

        case sw_quoted_second:

            state = sw_usual;

            if (ch >= '0' && ch <= '9') {
                ch = (u_char) ((decoded << 4) + ch - '0');

                if (type & NGX_UNESCAPE_REDIRECT) {
                    if (ch > '%' && ch < 0x7f) {
                        *d++ = ch;
                        break;
                    }

                    *d++ = '%'; *d++ = *(s - 2); *d++ = *(s - 1);
                    break;
                }

                *d++ = ch;

                break;
            }

            c = (u_char) (ch | 0x20);
            if (c >= 'a' && c <= 'f') {
                ch = (u_char) ((decoded << 4) + c - 'a' + 10);

                if (type & NGX_UNESCAPE_URI) {
                    if (ch == '?') {
                        *d++ = ch;
                        goto done;
                    }

                    *d++ = ch;
                    break;
                }

                if (type & NGX_UNESCAPE_REDIRECT) {
                    if (ch == '?') {
                        *d++ = ch;
                        goto done;
                    }

                    if (ch > '%' && ch < 0x7f) {
                        *d++ = ch;
                        break;
                    }

                    *d++ = '%'; *d++ = *(s - 2); *d++ = *(s - 1);
                    break;
                }

                *d++ = ch;

                break;
            }

            /* the invalid quoted character */

            break;
        }
    }

done:

    *dst = d;
    *src = s;
}


ngx_connection_t *
ngx_stream_lua_create_fake_connection(ngx_pool_t *pool)
{
    ngx_log_t               *log;
    ngx_connection_t        *c;
    ngx_connection_t        *saved_c = NULL;

    /* (we temporarily use a valid fd (0) to make ngx_get_connection happy) */
    if (ngx_cycle->files) {
        saved_c = ngx_cycle->files[0];
    }

    c = ngx_get_connection(0, ngx_cycle->log);

    if (ngx_cycle->files) {
        ngx_cycle->files[0] = saved_c;
    }

    if (c == NULL) {
        return NULL;
    }

    c->fd = (ngx_socket_t) -1;

    if (pool) {
        c->pool = pool;

    } else {
        c->pool = ngx_create_pool(128, c->log);
        if (c->pool == NULL) {
            goto failed;
        }
    }

    log = ngx_pcalloc(c->pool, sizeof(ngx_log_t));
    if (log == NULL) {
        goto failed;
    }

    c->log = log;
    c->log->connection = c->number;
    c->log->action = NULL;
    c->log->data = NULL;

    c->log_error = NGX_ERROR_INFO;

#if 0
    c->buffer = ngx_create_temp_buf(c->pool, 2);
    if (c->buffer == NULL) {
        goto failed;
    }

    c->buffer->start[0] = CR;
    c->buffer->start[1] = LF;
#endif

    c->error = 1;

    dd("created fake connection: %p", c);

    return c;

failed:

    ngx_stream_lua_close_fake_connection(c);
    return NULL;
}


ngx_stream_session_t *
ngx_stream_lua_create_fake_session(ngx_connection_t *c)
{
    ngx_stream_session_t      *s;

    s = ngx_pcalloc(c->pool, sizeof(ngx_stream_session_t));
    if (s == NULL) {
        return NULL;
    }

    s->ctx = ngx_pcalloc(c->pool, sizeof(void *) * ngx_stream_max_module);
    if (s->ctx == NULL) {
        return NULL;
    }

    s->connection = c;

    c->data = s;
    s->signature = NGX_STREAM_MODULE;

    dd("created fake session %p", s);

    return s;
}


int
ngx_stream_lua_do_call(ngx_log_t *log, lua_State *L)
{
    int                 status, base;
#if (NGX_PCRE)
    ngx_pool_t         *old_pool;
#endif

    base = lua_gettop(L);  /* function index */

    lua_pushcfunction(L, ngx_stream_lua_traceback);
                                                 /* push traceback function */

    lua_insert(L, base);  /* put it under chunk and args */

#if (NGX_PCRE)
    old_pool = ngx_stream_lua_pcre_malloc_init(ngx_cycle->pool);
#endif

    status = lua_pcall(L, 0, 0, base);

#if (NGX_PCRE)
    ngx_stream_lua_pcre_malloc_done(old_pool);
#endif

    lua_remove(L, base);

    return status;
}


ngx_int_t
ngx_stream_lua_report(ngx_log_t *log, lua_State *L, int status,
    const char *prefix)
{
    const char      *msg;

    if (status && !lua_isnil(L, -1)) {
        msg = lua_tostring(L, -1);
        if (msg == NULL) {
            msg = "unknown error";
        }

        ngx_log_error(NGX_LOG_ERR, log, 0, "%s error: %s", prefix, msg);
        lua_pop(L, 1);
    }

    /* force a full garbage-collection cycle */
    lua_gc(L, LUA_GCCOLLECT, 0);

    return status == 0 ? NGX_OK : NGX_ERROR;
}


ngx_stream_lua_cleanup_t *
ngx_stream_cleanup_add(ngx_stream_session_t *s, size_t size)
{
    ngx_connection_t            *c;
    ngx_stream_lua_cleanup_t    *cln;
    ngx_stream_lua_ctx_t        *ctx;

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_lua_module);
    if (ctx == NULL) {
        return NULL;
    }

    c = s->connection;

    cln = ngx_palloc(c->pool, sizeof(ngx_stream_lua_cleanup_t));
    if (cln == NULL) {
        return NULL;
    }

    if (size) {
        cln->data = ngx_palloc(c->pool, size);
        if (cln->data == NULL) {
            return NULL;
        }

    } else {
        cln->data = NULL;
    }

    cln->handler = NULL;
    cln->next = ctx->cleanup;
    ctx->cleanup = cln;

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream lua cleanup add: %p", cln);

    return cln;
}


void
ngx_stream_lua_free_session(ngx_stream_session_t *s, ngx_int_t rc)
{
    ngx_stream_lua_ctx_t        *ctx;
    ngx_stream_lua_cleanup_t    *cln;

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, s->connection->log, 0,
                   "stream lua free session");

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_lua_module);
    if (ctx == NULL) {
        ngx_stream_finalize_session(s, rc);
        return;
    }

    cln = ctx->cleanup;
    ctx->cleanup = NULL;

    while (cln) {
        if (cln->handler) {
            cln->handler(cln->data);
        }

        cln = cln->next;
    }

    ngx_stream_finalize_session(s, rc);
}


static void
ngx_stream_lua_inject_req_api(ngx_log_t *log, lua_State *L)
{
    /* ngx.req table */

    lua_createtable(L, 0 /* narr */, 1 /* nrec */);    /* .req */

    ngx_stream_lua_inject_req_socket_api(L);

    lua_setfield(L, -2, "req");
}


void
ngx_stream_lua_process_args_option(ngx_stream_session_t *s, lua_State *L,
    int table, ngx_str_t *args)
{
    u_char              *key;
    size_t               key_len;
    u_char              *value;
    size_t               value_len;
    size_t               len = 0;
    size_t               key_escape = 0;
    uintptr_t            total_escape = 0;
    int                  n;
    int                  i;
    u_char              *p;

    if (table < 0) {
        table = lua_gettop(L) + table + 1;
    }

    n = 0;
    lua_pushnil(L);
    while (lua_next(L, table) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            luaL_error(L, "attempt to use a non-string key in the "
                       "\"args\" option table");
            return;
        }

        key = (u_char *) lua_tolstring(L, -2, &key_len);

        key_escape = 2 * ngx_stream_lua_escape_uri(NULL, key, key_len,
                                                   NGX_ESCAPE_URI);
        total_escape += key_escape;

        switch (lua_type(L, -1)) {
        case LUA_TNUMBER:
        case LUA_TSTRING:
            value = (u_char *) lua_tolstring(L, -1, &value_len);

            total_escape += 2 * ngx_stream_lua_escape_uri(NULL, value,
                                                          value_len,
                                                          NGX_ESCAPE_URI);

            len += key_len + value_len + (sizeof("=") - 1);
            n++;

            break;

        case LUA_TBOOLEAN:
            if (lua_toboolean(L, -1)) {
                len += key_len;
                n++;
            }

            break;

        case LUA_TTABLE:

            i = 0;
            lua_pushnil(L);
            while (lua_next(L, -2) != 0) {
                if (lua_isboolean(L, -1)) {
                    if (lua_toboolean(L, -1)) {
                        len += key_len;

                    } else {
                        lua_pop(L, 1);
                        continue;
                    }

                } else {
                    value = (u_char *) lua_tolstring(L, -1, &value_len);

                    if (value == NULL) {
                        luaL_error(L, "attempt to use %s as query arg value",
                                   luaL_typename(L, -1));
                        return;
                    }

                    total_escape +=
                        2 * ngx_stream_lua_escape_uri(NULL, value,
                                                      value_len,
                                                      NGX_ESCAPE_URI);

                    len += key_len + value_len + (sizeof("=") - 1);
                }

                if (i++ > 0) {
                    total_escape += key_escape;
                }

                n++;
                lua_pop(L, 1);
            }

            break;

        default:
            luaL_error(L, "attempt to use %s as query arg value",
                       luaL_typename(L, -1));
            return;
        }

        lua_pop(L, 1);
    }

    len += (size_t) total_escape;

    if (n > 1) {
        len += (n - 1) * (sizeof("&") - 1);
    }

    dd("len 1: %d", (int) len);

    if (s) {
        p = ngx_palloc(s->connection->pool, len);
        if (p == NULL) {
            luaL_error(L, "no memory");
            return;
        }

    } else {
        p = lua_newuserdata(L, len);
    }

    args->data = p;
    args->len = len;

    i = 0;
    lua_pushnil(L);
    while (lua_next(L, table) != 0) {
        key = (u_char *) lua_tolstring(L, -2, &key_len);

        switch (lua_type(L, -1)) {
        case LUA_TNUMBER:
        case LUA_TSTRING:

            if (total_escape) {
                p = (u_char *) ngx_stream_lua_escape_uri(p, key, key_len,
                                                         NGX_ESCAPE_URI);

            } else {
                dd("shortcut: no escape required");

                p = ngx_copy(p, key, key_len);
            }

            *p++ = '=';

            value = (u_char *) lua_tolstring(L, -1, &value_len);

            if (total_escape) {
                p = (u_char *) ngx_stream_lua_escape_uri(p, value, value_len,
                                                         NGX_ESCAPE_URI);

            } else {
                p = ngx_copy(p, value, value_len);
            }

            if (i != n - 1) {
                /* not the last pair */
                *p++ = '&';
            }

            i++;

            break;

        case LUA_TBOOLEAN:
            if (lua_toboolean(L, -1)) {
                if (total_escape) {
                    p = (u_char *) ngx_stream_lua_escape_uri(p, key, key_len,
                                                             NGX_ESCAPE_URI);

                } else {
                    dd("shortcut: no escape required");

                    p = ngx_copy(p, key, key_len);
                }

                if (i != n - 1) {
                    /* not the last pair */
                    *p++ = '&';
                }

                i++;
            }

            break;

        case LUA_TTABLE:

            lua_pushnil(L);
            while (lua_next(L, -2) != 0) {

                if (lua_isboolean(L, -1)) {
                    if (lua_toboolean(L, -1)) {
                        if (total_escape) {
                            p = (u_char *) ngx_stream_lua_escape_uri(p, key,
    key_len,
    NGX_ESCAPE_URI);

                        } else {
                            dd("shortcut: no escape required");

                            p = ngx_copy(p, key, key_len);
                        }

                    } else {
                        lua_pop(L, 1);
                        continue;
                    }

                } else {

                    if (total_escape) {
                        p = (u_char *)
                                ngx_stream_lua_escape_uri(p, key,
                                                          key_len,
                                                          NGX_ESCAPE_URI);

                    } else {
                        dd("shortcut: no escape required");

                        p = ngx_copy(p, key, key_len);
                    }

                    *p++ = '=';

                    value = (u_char *) lua_tolstring(L, -1, &value_len);

                    if (total_escape) {
                        p = (u_char *)
                                ngx_stream_lua_escape_uri(p, value,
                                                          value_len,
                                                          NGX_ESCAPE_URI);

                    } else {
                        p = ngx_copy(p, value, value_len);
                    }
                }

                if (i != n - 1) {
                    /* not the last pair */
                    *p++ = '&';
                }

                i++;
                lua_pop(L, 1);
            }

            break;

        default:
            luaL_error(L, "should not reach here");
            return;
        }

        lua_pop(L, 1);
    }

    if (p - args->data != (ssize_t) len) {
        luaL_error(L, "buffer error: %d != %d",
                   (int) (p - args->data), (int) len);
        return;
    }
}


void
ngx_stream_lua_set_multi_value_table(lua_State *L, int index)
{
    if (index < 0) {
        index = lua_gettop(L) + index + 1;
    }

    lua_pushvalue(L, -2); /* stack: table key value key */
    lua_rawget(L, index);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1); /* stack: table key value */
        lua_rawset(L, index); /* stack: table */

    } else {
        if (!lua_istable(L, -1)) {
            /* just inserted one value */
            lua_createtable(L, 4, 0);
                /* stack: table key value value table */
            lua_insert(L, -2);
                /* stack: table key value table value */
            lua_rawseti(L, -2, 1);
                /* stack: table key value table */
            lua_insert(L, -2);
                /* stack: table key table value */

            lua_rawseti(L, -2, 2); /* stack: table key table */

            lua_rawset(L, index); /* stack: table */

        } else {
            /* stack: table key value table */
            lua_insert(L, -2); /* stack: table key table value */

            lua_rawseti(L, -2, lua_objlen(L, -2) + 1);
                /* stack: table key table  */
            lua_pop(L, 2); /* stack: table */
        }
    }
}
