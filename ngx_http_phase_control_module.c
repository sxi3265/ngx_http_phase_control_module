
/*
 * Copyright (C) Hanada
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#if (NGX_EXPR)
#include <ngx_http_expr_module.h>
#endif


typedef struct {
    ngx_uint_t                 configured;
    ngx_uint_t                 first[NGX_HTTP_LOG_PHASE];
    ngx_http_phase_handler_pt  checker[NGX_HTTP_LOG_PHASE];
} ngx_http_phase_control_main_conf_t;


typedef struct {
#if (NGX_EXPR)
    ngx_array_t               *skip[NGX_HTTP_LOG_PHASE];
#else
    ngx_flag_t                 skip[NGX_HTTP_LOG_PHASE];
#endif
} ngx_http_phase_control_conf_t;


typedef struct {
    ngx_uint_t                 evaluated;
    ngx_uint_t                 uri_changes;
} ngx_http_phase_control_ctx_t;


typedef struct {
    ngx_uint_t                 phase;
    ngx_uint_t                 conf;
    ngx_http_phase_handler_pt  checker;
} ngx_http_phase_control_phase_t;


static void *ngx_http_phase_control_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_phase_control_create_conf(ngx_conf_t *cf);
static char *ngx_http_phase_control_merge_srv_conf(ngx_conf_t *cf,
    void *parent, void *child);
static char *ngx_http_phase_control_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);
static char *ngx_http_phase_control_merge_conf(ngx_conf_t *cf,
    void *parent, void *child, ngx_uint_t scope);
static char *ngx_http_phase_control_skip(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_http_phase_control_init_module(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_phase_control_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph, ngx_uint_t phase);
static ngx_int_t ngx_http_phase_control_post_read_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph);
static ngx_int_t ngx_http_phase_control_server_rewrite_phase(
    ngx_http_request_t *r, ngx_http_phase_handler_t *ph);
static ngx_int_t ngx_http_phase_control_rewrite_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph);
static ngx_int_t ngx_http_phase_control_preaccess_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph);
static ngx_int_t ngx_http_phase_control_access_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph);
static ngx_int_t ngx_http_phase_control_precontent_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph);


static ngx_http_phase_control_phase_t  ngx_http_phase_control_phases[] = {
    { NGX_HTTP_POST_READ_PHASE, NGX_HTTP_SRV_CONF_OFFSET,
      ngx_http_phase_control_post_read_phase },

    { NGX_HTTP_SERVER_REWRITE_PHASE, NGX_HTTP_SRV_CONF_OFFSET,
      ngx_http_phase_control_server_rewrite_phase },

    { NGX_HTTP_REWRITE_PHASE, NGX_HTTP_LOC_CONF_OFFSET,
      ngx_http_phase_control_rewrite_phase },

    { NGX_HTTP_PREACCESS_PHASE, NGX_HTTP_LOC_CONF_OFFSET,
      ngx_http_phase_control_preaccess_phase },

    { NGX_HTTP_ACCESS_PHASE, NGX_HTTP_LOC_CONF_OFFSET,
      ngx_http_phase_control_access_phase },

    { NGX_HTTP_PRECONTENT_PHASE, NGX_HTTP_LOC_CONF_OFFSET,
      ngx_http_phase_control_precontent_phase },
};


static ngx_command_t  ngx_http_phase_control_commands[] = {

    { ngx_string("skip_post_read"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
#endif
                        |NGX_CONF_FLAG,
      ngx_http_phase_control_skip,
      NGX_HTTP_SRV_CONF_OFFSET,
      offsetof(ngx_http_phase_control_conf_t, skip[NGX_HTTP_POST_READ_PHASE]),
      &ngx_http_phase_control_phases[0] },

    { ngx_string("skip_server_rewrite"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
#endif
                        |NGX_CONF_FLAG,
      ngx_http_phase_control_skip,
      NGX_HTTP_SRV_CONF_OFFSET,
      offsetof(ngx_http_phase_control_conf_t,
               skip[NGX_HTTP_SERVER_REWRITE_PHASE]),
      &ngx_http_phase_control_phases[1] },

    { ngx_string("skip_rewrite"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
                        |NGX_HTTP_LOC_WHEN_CONF
#endif
                        |NGX_CONF_FLAG,
      ngx_http_phase_control_skip,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_phase_control_conf_t, skip[NGX_HTTP_REWRITE_PHASE]),
      &ngx_http_phase_control_phases[2] },

    { ngx_string("skip_preaccess"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
                        |NGX_HTTP_LOC_WHEN_CONF
#endif
                        |NGX_CONF_FLAG,
      ngx_http_phase_control_skip,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_phase_control_conf_t, skip[NGX_HTTP_PREACCESS_PHASE]),
      &ngx_http_phase_control_phases[3] },

    { ngx_string("skip_access"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
                        |NGX_HTTP_LOC_WHEN_CONF
#endif
                        |NGX_CONF_FLAG,
      ngx_http_phase_control_skip,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_phase_control_conf_t, skip[NGX_HTTP_ACCESS_PHASE]),
      &ngx_http_phase_control_phases[4] },

    { ngx_string("skip_precontent"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
                        |NGX_HTTP_LOC_WHEN_CONF
#endif
                        |NGX_CONF_FLAG,
      ngx_http_phase_control_skip,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_phase_control_conf_t, skip[NGX_HTTP_PRECONTENT_PHASE]),
      &ngx_http_phase_control_phases[5] },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_phase_control_module_ctx = {
    NULL,                                    /* preconfiguration */
    NULL,                                    /* postconfiguration */

    ngx_http_phase_control_create_main_conf, /* create main configuration */
    NULL,                                    /* init main configuration */

    ngx_http_phase_control_create_conf,      /* create server configuration */
    ngx_http_phase_control_merge_srv_conf,   /* merge server configuration */

    ngx_http_phase_control_create_conf,      /* create location configuration */
    ngx_http_phase_control_merge_loc_conf    /* merge location configuration */
};


ngx_module_t  ngx_http_phase_control_module = {
    NGX_MODULE_V1,
    &ngx_http_phase_control_module_ctx,      /* module context */
    ngx_http_phase_control_commands,         /* module directives */
    NGX_HTTP_MODULE,                         /* module type */
    NULL,                                    /* init master */
    ngx_http_phase_control_init_module,      /* init module */
    NULL,                                    /* init process */
    NULL,                                    /* init thread */
    NULL,                                    /* exit thread */
    NULL,                                    /* exit process */
    NULL,                                    /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_http_phase_control_create_main_conf(ngx_conf_t *cf)
{
    return ngx_pcalloc(cf->pool,
                       sizeof(ngx_http_phase_control_main_conf_t));
}


static void *
ngx_http_phase_control_create_conf(ngx_conf_t *cf)
{
    ngx_uint_t                      i;
    ngx_http_phase_control_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_phase_control_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    for (i = 0; i < NGX_HTTP_LOG_PHASE; i++) {
#if (NGX_EXPR)
        conf->skip[i] = NGX_CONF_UNSET_PTR;
#else
        conf->skip[i] = NGX_CONF_UNSET;
#endif
    }

    return conf;
}


static char *
ngx_http_phase_control_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child)
{
    return ngx_http_phase_control_merge_conf(cf, parent, child,
                                             NGX_HTTP_SRV_CONF_OFFSET);
}


static char *
ngx_http_phase_control_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    return ngx_http_phase_control_merge_conf(cf, parent, child,
                                             NGX_HTTP_LOC_CONF_OFFSET);
}


static char *
ngx_http_phase_control_merge_conf(ngx_conf_t *cf, void *parent, void *child,
    ngx_uint_t scope)
{
    ngx_http_phase_control_conf_t  *prev = parent;
    ngx_http_phase_control_conf_t  *conf = child;

    ngx_uint_t                           i, phase;
    ngx_http_phase_control_main_conf_t  *pmcf;

    pmcf = ngx_http_conf_get_module_main_conf(cf,
                                              ngx_http_phase_control_module);

    for (i = 0; i < sizeof(ngx_http_phase_control_phases)
                    / sizeof(ngx_http_phase_control_phases[0]); i++)
    {
        phase = ngx_http_phase_control_phases[i].phase;

        if (ngx_http_phase_control_phases[i].conf != scope
            || !(pmcf->configured & (1 << phase)))
        {
            continue;
        }

#if (NGX_EXPR)
        if (ngx_conf_merge_expr_flag_value(cf, &conf->skip[phase],
                                           prev->skip[phase], 0)
            != NGX_OK)
        {
            return NGX_CONF_ERROR;
        }
#else
        ngx_conf_merge_value(conf->skip[phase], prev->skip[phase], 0);
#endif
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_phase_control_skip(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char                                *rv;
    ngx_command_t                        flag;
    ngx_http_phase_control_phase_t      *phase;
    ngx_http_phase_control_main_conf_t  *pmcf;

    phase = cmd->post;
    flag = *cmd;
    flag.post = NULL;

#if (NGX_EXPR)
    rv = ngx_conf_set_conditional_flag_slot(cf, &flag, conf);
#else
    rv = ngx_conf_set_flag_slot(cf, &flag, conf);
#endif

    if (rv != NGX_CONF_OK) {
        return rv;
    }

    pmcf = ngx_http_conf_get_module_main_conf(cf,
                                              ngx_http_phase_control_module);
    pmcf->configured |= 1 << phase->phase;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_phase_control_init_module(ngx_cycle_t *cycle)
{
    ngx_uint_t                           i, j, n, count, phase;
    ngx_http_phase_handler_t            *ph;
    ngx_http_core_main_conf_t           *cmcf;
    ngx_http_phase_control_main_conf_t  *pmcf;

    pmcf = ngx_http_cycle_get_module_main_conf(cycle,
                                               ngx_http_phase_control_module);
    if (pmcf == NULL || pmcf->configured == 0) {
        return NGX_OK;
    }

    cmcf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_core_module);
    ph = cmcf->phase_engine.handlers;
    n = 0;

    /* The core has built the phase engine; no handler entries are added. */

    for (i = 0; i < NGX_HTTP_LOG_PHASE; i++) {
        if (i == NGX_HTTP_FIND_CONFIG_PHASE) {
            n++;
            continue;
        }

        if (i == NGX_HTTP_POST_REWRITE_PHASE) {
            if (cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers.nelts) {
                n++;
            }

            continue;
        }

        if (i == NGX_HTTP_POST_ACCESS_PHASE) {
            if (cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers.nelts) {
                n++;
            }

            continue;
        }

        count = cmcf->phases[i].handlers.nelts;

        if ((pmcf->configured & (1 << i)) && count) {
            pmcf->first[i] = n;
            pmcf->checker[i] = ph[n].checker;

            for (j = 1; j < count; j++) {
                if (ph[n + j].checker != pmcf->checker[i]) {
                    ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                                  "inconsistent checkers in HTTP phase %ui", i);
                    return NGX_ERROR;
                }
            }
        }

        n += count;
    }

    for (i = 0; i < sizeof(ngx_http_phase_control_phases)
                    / sizeof(ngx_http_phase_control_phases[0]); i++)
    {
        phase = ngx_http_phase_control_phases[i].phase;

        if (pmcf->checker[phase] == NULL) {
            continue;
        }

        n = pmcf->first[phase];
        count = cmcf->phases[phase].handlers.nelts;

        /*
         * Wrap every entry so moving handlers within the phase, as ngx_lua
         * does, cannot move the entry check away from the phase boundary.
         */

        for (j = 0; j < count; j++) {
            ph[n + j].checker = ngx_http_phase_control_phases[i].checker;
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_phase_control_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph, ngx_uint_t phase)
{
    ngx_flag_t                           skip;
    ngx_http_phase_control_ctx_t        *ctx;
    ngx_http_phase_control_conf_t       *conf;
    ngx_http_phase_control_main_conf_t  *pmcf;

    pmcf = ngx_http_get_module_main_conf(r, ngx_http_phase_control_module);

    if ((ngx_uint_t) r->phase_handler != pmcf->first[phase]
        || (phase == NGX_HTTP_ACCESS_PHASE && r != r->main))
    {
        return pmcf->checker[phase](r, ph);
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_phase_control_module);

    if (ctx == NULL) {
        ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_phase_control_ctx_t));
        if (ctx == NULL) {
            ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
            return NGX_OK;
        }

        ngx_http_set_ctx(r, ctx, ngx_http_phase_control_module);
    }

    /*
     * Internal redirects clear module contexts.  A rewrite cycle does not,
     * but it consumes uri_changes, so it starts a new round of decisions.
     */

    if (ctx->uri_changes != r->uri_changes) {
        ctx->uri_changes = r->uri_changes;
        ctx->evaluated = 0;
    }

    if (ctx->evaluated & (1 << phase)) {
        return pmcf->checker[phase](r, ph);
    }

    ctx->evaluated |= 1 << phase;

    if (phase == NGX_HTTP_POST_READ_PHASE
        || phase == NGX_HTTP_SERVER_REWRITE_PHASE)
    {
        conf = ngx_http_get_module_srv_conf(r, ngx_http_phase_control_module);

    } else {
        conf = ngx_http_get_module_loc_conf(r, ngx_http_phase_control_module);
    }

#if (NGX_EXPR)
    skip = ngx_http_get_expr_flag_value(r, conf->skip[phase]);
#else
    skip = conf->skip[phase];
#endif

    if (skip == 1) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "phase control skip: %ui", phase);

        if (phase == NGX_HTTP_ACCESS_PHASE) {
            r->access_code = 0;
        }

        r->phase_handler = ph->next;
        return NGX_AGAIN;
    }

    /* The original checker may finalize r; do not access it afterwards. */

    return pmcf->checker[phase](r, ph);
}


static ngx_int_t
ngx_http_phase_control_post_read_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph)
{
    return ngx_http_phase_control_phase(r, ph, NGX_HTTP_POST_READ_PHASE);
}


static ngx_int_t
ngx_http_phase_control_server_rewrite_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph)
{
    return ngx_http_phase_control_phase(r, ph, NGX_HTTP_SERVER_REWRITE_PHASE);
}


static ngx_int_t
ngx_http_phase_control_rewrite_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph)
{
    return ngx_http_phase_control_phase(r, ph, NGX_HTTP_REWRITE_PHASE);
}


static ngx_int_t
ngx_http_phase_control_preaccess_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph)
{
    return ngx_http_phase_control_phase(r, ph, NGX_HTTP_PREACCESS_PHASE);
}


static ngx_int_t
ngx_http_phase_control_access_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph)
{
    return ngx_http_phase_control_phase(r, ph, NGX_HTTP_ACCESS_PHASE);
}


static ngx_int_t
ngx_http_phase_control_precontent_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph)
{
    return ngx_http_phase_control_phase(r, ph, NGX_HTTP_PRECONTENT_PHASE);
}
