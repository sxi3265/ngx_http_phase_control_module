
/*
 * Copyright (C) Hanada
 *
 * Phase tracing, postponement and asynchronous resumption for nginx-tests.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct {
    ngx_uint_t   delay;
    ngx_uint_t   postpone;
    ngx_uint_t   postponed;
} ngx_http_phase_probe_main_conf_t;


typedef struct {
    u_char       trace[512];
    size_t       len;
    ngx_uint_t   waiting;
    ngx_flag_t   ready;
    ngx_event_t  timer;
} ngx_http_phase_probe_ctx_t;


static ngx_int_t ngx_http_phase_probe_init(ngx_conf_t *cf);
static ngx_int_t ngx_http_phase_probe_variables(ngx_conf_t *cf);
static void *ngx_http_phase_probe_create_conf(ngx_conf_t *cf);
static char *ngx_http_phase_probe_content(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_phase_probe_option(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_http_phase_probe_body(ngx_http_request_t *r);
static ngx_int_t ngx_http_phase_probe_phase(ngx_http_request_t *r,
    ngx_uint_t phase);
static void ngx_http_phase_probe_resume(ngx_event_t *ev);
static void ngx_http_phase_probe_cleanup(void *data);
static ngx_int_t ngx_http_phase_probe_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_phase_probe_post_read(ngx_http_request_t *r);
static ngx_int_t ngx_http_phase_probe_server_rewrite(ngx_http_request_t *r);
static ngx_int_t ngx_http_phase_probe_rewrite(ngx_http_request_t *r);
static ngx_int_t ngx_http_phase_probe_preaccess(ngx_http_request_t *r);
static ngx_int_t ngx_http_phase_probe_access(ngx_http_request_t *r);
static ngx_int_t ngx_http_phase_probe_precontent(ngx_http_request_t *r);


static ngx_str_t  ngx_http_phase_probe_names[] = {
    ngx_string("post_read"),
    ngx_string("server_rewrite"),
    ngx_string("rewrite"),
    ngx_string("preaccess"),
    ngx_string("access"),
    ngx_string("precontent"),
};


static ngx_uint_t  ngx_http_phase_probe_phases[] = {
    NGX_HTTP_POST_READ_PHASE,
    NGX_HTTP_SERVER_REWRITE_PHASE,
    NGX_HTTP_REWRITE_PHASE,
    NGX_HTTP_PREACCESS_PHASE,
    NGX_HTTP_ACCESS_PHASE,
    NGX_HTTP_PRECONTENT_PHASE,
};


static ngx_http_handler_pt  ngx_http_phase_probe_handlers[] = {
    ngx_http_phase_probe_post_read,
    ngx_http_phase_probe_server_rewrite,
    ngx_http_phase_probe_rewrite,
    ngx_http_phase_probe_preaccess,
    ngx_http_phase_probe_access,
    ngx_http_phase_probe_precontent,
};


static ngx_command_t  ngx_http_phase_probe_commands[] = {

    { ngx_string("phase_probe"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_phase_probe_content,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("phase_probe_delay"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_http_phase_probe_option,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_phase_probe_main_conf_t, delay),
      NULL },

    { ngx_string("phase_probe_postpone"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_http_phase_probe_option,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_phase_probe_main_conf_t, postpone),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_phase_probe_module_ctx = {
    ngx_http_phase_probe_variables,        /* preconfiguration */
    ngx_http_phase_probe_init,             /* postconfiguration */

    ngx_http_phase_probe_create_conf,      /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_phase_probe_module = {
    NGX_MODULE_V1,
    &ngx_http_phase_probe_module_ctx,      /* module context */
    ngx_http_phase_probe_commands,         /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_http_phase_probe_create_conf(ngx_conf_t *cf)
{
    return ngx_pcalloc(cf->pool, sizeof(ngx_http_phase_probe_main_conf_t));
}


static char *
ngx_http_phase_probe_option(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t   *value;
    ngx_uint_t   i;
    ngx_uint_t  *mask;

    value = cf->args->elts;
    mask = (ngx_uint_t *) ((u_char *) conf + cmd->offset);

    for (i = 0; i < 6; i++) {
        if (ngx_strcmp(value[1].data, ngx_http_phase_probe_names[i].data)
            == 0)
        {
            *mask |= 1 << ngx_http_phase_probe_phases[i];
            return NGX_CONF_OK;
        }
    }

    return "has an invalid phase";
}


static char *
ngx_http_phase_probe_content(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_phase_probe_body;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_phase_probe_phase(ngx_http_request_t *r, ngx_uint_t phase)
{
    ngx_uint_t                         i;
    ngx_http_cleanup_t                *cln;
    ngx_http_phase_handler_t          *ph, *cur, *last;
    ngx_http_phase_handler_t           tmp;
    ngx_http_core_main_conf_t         *cmcf;
    ngx_http_phase_probe_ctx_t        *ctx;
    ngx_http_phase_probe_main_conf_t  *pmcf;

    ctx = ngx_http_get_module_ctx(r, ngx_http_phase_probe_module);
    if (ctx == NULL) {
        ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_phase_probe_ctx_t));
        if (ctx == NULL) {
            return NGX_ERROR;
        }

        ngx_http_set_ctx(r, ctx, ngx_http_phase_probe_module);
    }

    pmcf = ngx_http_get_module_main_conf(r, ngx_http_phase_probe_module);

    if ((pmcf->postpone & (1 << phase))
        && !(pmcf->postponed & (1 << phase)))
    {
        pmcf->postponed |= 1 << phase;
        ctx->ready = 1;
        cmcf = ngx_http_get_module_main_conf(r, ngx_http_core_module);
        ph = cmcf->phase_engine.handlers;
        cur = &ph[r->phase_handler];
        last = &ph[cur->next - (phase == NGX_HTTP_ACCESS_PHASE ? 2 : 1)];

        if (cur < last) {
            tmp = *cur;
            ngx_memmove(cur, cur + 1,
                        (last - cur) * sizeof(ngx_http_phase_handler_t));
            *last = tmp;
            r->phase_handler--;
            return NGX_DECLINED;
        }
    }

    if (pmcf->delay & (1 << phase)) {
        if (ctx->waiting == 0) {
            cln = ngx_http_cleanup_add(r, 0);
            if (cln == NULL) {
                return NGX_ERROR;
            }

            cln->handler = ngx_http_phase_probe_cleanup;
            cln->data = ctx;
            ctx->waiting = 1;
            ctx->timer.handler = ngx_http_phase_probe_resume;
            ctx->timer.data = r;
            ctx->timer.log = r->connection->log;
            r->main->count++;
            ngx_add_timer(&ctx->timer, 10);
            return NGX_DONE;
        }

        if (ctx->waiting == 1) {
            return NGX_DONE;
        }
    }

    for (i = 0; i < 6; i++) {
        if (ngx_http_phase_probe_phases[i] == phase) {
            if (ctx->len + ngx_http_phase_probe_names[i].len + 1
                > sizeof(ctx->trace))
            {
                return NGX_ERROR;
            }

            ngx_memcpy(ctx->trace + ctx->len,
                       ngx_http_phase_probe_names[i].data,
                       ngx_http_phase_probe_names[i].len);
            ctx->len += ngx_http_phase_probe_names[i].len;
            ctx->trace[ctx->len++] = ',';
            break;
        }
    }

    return NGX_DECLINED;
}


static void
ngx_http_phase_probe_resume(ngx_event_t *ev)
{
    ngx_http_request_t          *r;
    ngx_http_phase_probe_ctx_t  *ctx;

    r = ev->data;
    ctx = ngx_http_get_module_ctx(r, ngx_http_phase_probe_module);
    ctx->waiting = 2;
    ctx->ready = 1;

    ngx_http_core_run_phases(r);
    ngx_http_finalize_request(r, NGX_DONE);
}


static void
ngx_http_phase_probe_cleanup(void *data)
{
    ngx_http_phase_probe_ctx_t  *ctx = data;

    if (ctx->timer.timer_set) {
        ngx_del_timer(&ctx->timer);
    }
}


static ngx_int_t
ngx_http_phase_probe_body(ngx_http_request_t *r)
{
    ngx_int_t                    rc;
    ngx_buf_t                   *b;
    ngx_chain_t                  out;
    ngx_http_phase_probe_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_phase_probe_module);
    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = ctx ? ctx->len : 0;
    ngx_str_set(&r->headers_out.content_type, "text/plain");

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    b = ngx_calloc_buf(r->pool);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    b->pos = ctx ? ctx->trace : (u_char *) "";
    b->last = b->pos + r->headers_out.content_length_n;
    b->memory = 1;
    b->last_buf = (r == r->main);
    b->last_in_chain = 1;
    out.buf = b;
    out.next = NULL;

    return ngx_http_output_filter(r, &out);
}


static ngx_int_t
ngx_http_phase_probe_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    u_char                      *p;
    ngx_uint_t                   i, j, n, count;
    ngx_http_phase_handler_pt    checker;
    ngx_http_core_main_conf_t   *cmcf;
    ngx_http_phase_probe_ctx_t  *ctx;

    v->valid = 1;
    v->no_cacheable = 1;
    v->not_found = 0;

    ctx = ngx_http_get_module_ctx(r, ngx_http_phase_probe_module);

    if (data == 0) {
        v->len = ctx ? ctx->len : 0;
        v->data = ctx ? ctx->trace : (u_char *) "";
        return NGX_OK;
    }

    if (data == 1) {
        v->len = 1;
        v->data = (u_char *) (ctx && ctx->ready ? "1" : "0");
        return NGX_OK;
    }

    v->data = ngx_pnalloc(r->pool, 128);
    if (v->data == NULL) {
        return NGX_ERROR;
    }

    p = v->data;
    cmcf = ngx_http_get_module_main_conf(r, ngx_http_core_module);
    n = 0;

    for (i = 0; i < NGX_HTTP_LOG_PHASE; i++) {
        if (i == NGX_HTTP_FIND_CONFIG_PHASE
            || (i == NGX_HTTP_POST_REWRITE_PHASE
                && cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers.nelts)
            || (i == NGX_HTTP_POST_ACCESS_PHASE
                && cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers.nelts))
        {
            n++;
            continue;
        }

        count = cmcf->phases[i].handlers.nelts;
        checker = ngx_http_core_generic_phase;

        if (i == NGX_HTTP_REWRITE_PHASE
            || i == NGX_HTTP_SERVER_REWRITE_PHASE)
        {
            checker = ngx_http_core_rewrite_phase;

        } else if (i == NGX_HTTP_ACCESS_PHASE) {
            checker = ngx_http_core_access_phase;
        }

        for (j = 0; j < 6; j++) {
            if (ngx_http_phase_probe_phases[j] == i && count
                && cmcf->phase_engine.handlers[n].checker != checker)
            {
                p = ngx_cpymem(p, ngx_http_phase_probe_names[j].data,
                              ngx_http_phase_probe_names[j].len);
                *p++ = ',';
            }
        }

        n += count;
    }

    v->len = p - v->data;
    return NGX_OK;
}


static ngx_int_t
ngx_http_phase_probe_variables(ngx_conf_t *cf)
{
    ngx_uint_t            i;
    ngx_http_variable_t  *v;
    ngx_str_t             names[] = {
        ngx_string("phase_probe_trace"),
        ngx_string("phase_probe_ready"),
        ngx_string("phase_probe_wrapped")
    };

    for (i = 0; i < 3; i++) {
        v = ngx_http_add_variable(cf, &names[i], NGX_HTTP_VAR_NOCACHEABLE);
        if (v == NULL) {
            return NGX_ERROR;
        }

        v->get_handler = ngx_http_phase_probe_variable;
        v->data = i;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_phase_probe_init(ngx_conf_t *cf)
{
    ngx_uint_t                  i, phase;
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    for (i = 0; i < 6; i++) {
        phase = ngx_http_phase_probe_phases[i];
        h = ngx_array_push(&cmcf->phases[phase].handlers);
        if (h == NULL) {
            return NGX_ERROR;
        }

        *h = ngx_http_phase_probe_handlers[i];
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_phase_probe_post_read(ngx_http_request_t *r)
{
    return ngx_http_phase_probe_phase(r, NGX_HTTP_POST_READ_PHASE);
}


static ngx_int_t
ngx_http_phase_probe_server_rewrite(ngx_http_request_t *r)
{
    return ngx_http_phase_probe_phase(r, NGX_HTTP_SERVER_REWRITE_PHASE);
}


static ngx_int_t
ngx_http_phase_probe_rewrite(ngx_http_request_t *r)
{
    return ngx_http_phase_probe_phase(r, NGX_HTTP_REWRITE_PHASE);
}


static ngx_int_t
ngx_http_phase_probe_preaccess(ngx_http_request_t *r)
{
    return ngx_http_phase_probe_phase(r, NGX_HTTP_PREACCESS_PHASE);
}


static ngx_int_t
ngx_http_phase_probe_access(ngx_http_request_t *r)
{
    return ngx_http_phase_probe_phase(r, NGX_HTTP_ACCESS_PHASE);
}


static ngx_int_t
ngx_http_phase_probe_precontent(ngx_http_request_t *r)
{
    return ngx_http_phase_probe_phase(r, NGX_HTTP_PRECONTENT_PHASE);
}
