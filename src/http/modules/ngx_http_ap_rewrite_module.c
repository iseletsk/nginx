#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#if (NGX_PCRE)

#define NGX_HTTP_AP_REWRITE_DEFAULT_MAX_LOOPS  10

typedef struct {
    ngx_http_regex_t          *regex;
    ngx_http_complex_value_t   replacement;
    ngx_str_t                  name;

    ngx_uint_t                 status;
    ngx_uint_t                 regex_options;

    unsigned                   last:1;
    unsigned                   redirect:1;
    unsigned                   qs_append:1;
} ngx_http_ap_rewrite_rule_t;

typedef struct {
    ngx_array_t               *rules;
    ngx_flag_t                 engine;
    ngx_uint_t                 max_loops;
} ngx_http_ap_rewrite_loc_conf_t;

static ngx_int_t ngx_http_ap_rewrite_init(ngx_conf_t *cf);
static ngx_int_t ngx_http_ap_rewrite_handler(ngx_http_request_t *r);
static void *ngx_http_ap_rewrite_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_ap_rewrite_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_http_ap_rewrite_rule(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_ap_rewrite_options(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

static ngx_command_t ngx_http_ap_rewrite_commands[] = {

    { ngx_string("RewriteEngine"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_ap_rewrite_loc_conf_t, engine),
      NULL },

    { ngx_string("RewriteRule"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE23,
      ngx_http_ap_rewrite_rule,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("RewriteOptions"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_ap_rewrite_options,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};

static ngx_http_module_t ngx_http_ap_rewrite_module_ctx = {
    NULL,                               /* preconfiguration */
    ngx_http_ap_rewrite_init,           /* postconfiguration */

    NULL,                               /* create main configuration */
    NULL,                               /* init main configuration */

    NULL,                               /* create server configuration */
    NULL,                               /* merge server configuration */

    ngx_http_ap_rewrite_create_loc_conf,/* create location configuration */
    ngx_http_ap_rewrite_merge_loc_conf  /* merge location configuration */
};

ngx_module_t ngx_http_ap_rewrite_module = {
    NGX_MODULE_V1,
    &ngx_http_ap_rewrite_module_ctx,    /* module context */
    ngx_http_ap_rewrite_commands,       /* module directives */
    NGX_HTTP_MODULE,                    /* module type */
    NULL,                               /* init master */
    NULL,                               /* init module */
    NULL,                               /* init process */
    NULL,                               /* init thread */
    NULL,                               /* exit thread */
    NULL,                               /* exit process */
    NULL,                               /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t
ngx_http_ap_rewrite_handler(ngx_http_request_t *r)
{
    ngx_int_t                      rc;
    ngx_uint_t                     i, pass;
    ngx_http_ap_rewrite_rule_t    *rules, *rule;
    ngx_http_ap_rewrite_loc_conf_t *lcf;

    lcf = ngx_http_get_module_loc_conf(r, ngx_http_ap_rewrite_module);

    if (!lcf->engine || lcf->rules == NULL) {
        return NGX_DECLINED;
    }

    rules = lcf->rules->elts;

    for (pass = 0; pass < lcf->max_loops; pass++) {
        ngx_uint_t applied = 0;

        for (i = 0; i < lcf->rules->nelts; i++) {
            ngx_str_t      replacement;
            ngx_str_t      old_args;
            ngx_str_t      new_uri;
            ngx_str_t      new_args;
            ngx_str_t      final_location;
            ngx_table_elt_t *location;
            u_char         *p, *q, *dst, *src;
            size_t          len, total;

            rule = &rules[i];

            rc = ngx_http_regex_exec(r, rule->regex, &r->uri);

            if (rc == NGX_DECLINED) {
                continue;
            }

            if (rc == NGX_ERROR) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "mod_ap_rewrite rule \"%V\" matched \"%V\"",
                           &rule->name, &r->uri);

            if (ngx_http_complex_value(r, &rule->replacement, &replacement)
                != NGX_OK)
            {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            applied = 1;
            old_args = r->args;

            if (rule->redirect) {
                final_location = replacement;

                if (rule->qs_append && old_args.len) {
                    q = ngx_strlchr(final_location.data,
                                    final_location.data + final_location.len,
                                    '?');

                    total = final_location.len + old_args.len + 1;
                    if (q && q + 1 == final_location.data + final_location.len) {
                        total--;
                    }

                    dst = ngx_pnalloc(r->pool, total);
                    if (dst == NULL) {
                        return NGX_HTTP_INTERNAL_SERVER_ERROR;
                    }

                    p = ngx_cpymem(dst, final_location.data,
                                   final_location.len);

                    if (q == NULL) {
                        *p++ = '?';
                    } else if (q + 1 != final_location.data +
                                          final_location.len) {
                        *p++ = '&';
                    }

                    ngx_memcpy(p, old_args.data, old_args.len);
                    p += old_args.len;

                    final_location.data = dst;
                    final_location.len = p - dst;
                }

                dst = ngx_pnalloc(r->pool, final_location.len);
                if (dst == NULL) {
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }

                p = dst;
                src = final_location.data;

                ngx_unescape_uri(&p, &src, final_location.len,
                                 NGX_UNESCAPE_REDIRECT);

                if (src < final_location.data + final_location.len) {
                    p = ngx_movemem(p, src,
                                    final_location.data + final_location.len -
                                    src);
                }

                ngx_http_clear_location(r);

                location = ngx_list_push(&r->headers_out.headers);
                if (location == NULL) {
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }

                location->hash = 1;
                location->next = NULL;
                ngx_str_set(&location->key, "Location");
                location->value.len = p - dst;
                location->value.data = dst;

                r->headers_out.location = location;
                r->headers_out.status = rule->status;

                ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                              "mod_ap_rewrite redirect: \"%V\"", &location->value);

                return (ngx_int_t) rule->status;
            }

            q = ngx_strlchr(replacement.data,
                            replacement.data + replacement.len, '?');

            if (q) {
                new_uri.len = q - replacement.data;
                new_uri.data = replacement.data;

                new_args.len = replacement.len - (new_uri.len + 1);
                new_args.data = q + 1;
            } else {
                new_uri = replacement;
                ngx_str_null(&new_args);
            }

            if (new_uri.len == 0) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "mod_ap_rewrite generated empty URI");
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            p = ngx_pnalloc(r->pool, new_uri.len);
            if (p == NULL) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            ngx_memcpy(p, new_uri.data, new_uri.len);
            r->uri.data = p;
            r->uri.len = new_uri.len;

            if (q == NULL) {
                if (rule->qs_append && old_args.len) {
                    dst = ngx_pnalloc(r->pool, old_args.len);
                    if (dst == NULL) {
                        return NGX_HTTP_INTERNAL_SERVER_ERROR;
                    }

                    ngx_memcpy(dst, old_args.data, old_args.len);
                    r->args.len = old_args.len;
                    r->args.data = dst;

                } else {
                    r->args.len = 0;
                    r->args.data = NULL;
                }

            } else {
                total = new_args.len;

                if (rule->qs_append && old_args.len) {
                    if (new_args.len) {
                        total += 1;
                    }
                    total += old_args.len;
                }

                if (total) {
                    dst = ngx_pnalloc(r->pool, total);
                    if (dst == NULL) {
                        return NGX_HTTP_INTERNAL_SERVER_ERROR;
                    }

                    p = dst;

                    if (new_args.len) {
                        p = ngx_cpymem(p, new_args.data, new_args.len);
                    }

                    if (rule->qs_append && old_args.len) {
                        if (new_args.len) {
                            *p++ = '&';
                        }

                        ngx_memcpy(p, old_args.data, old_args.len);
                        p += old_args.len;
                    }

                    r->args.len = p - dst;
                    r->args.data = dst;

                } else {
                    r->args.len = 0;
                    r->args.data = NULL;
                }
            }

            r->internal = 1;
            r->valid_unparsed_uri = 0;
            r->valid_location = 0;
            r->uri_changed = 1;

            ngx_http_set_exten(r);

            ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                          "mod_ap_rewrite uri: \"%V\", args: \"%V\"",
                          &r->uri, &r->args);

            if (rule->last) {
                return NGX_DECLINED;
            }

            break;
        }

        if (!applied) {
            return NGX_DECLINED;
        }
    }

    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "mod_ap_rewrite exceeded max_loops while processing \"%V\"",
                  &r->uri);

    return NGX_HTTP_INTERNAL_SERVER_ERROR;
}

static ngx_int_t
ngx_http_ap_rewrite_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_SERVER_REWRITE_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_ap_rewrite_handler;

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_ap_rewrite_handler;

    return NGX_OK;
}

static void *
ngx_http_ap_rewrite_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_ap_rewrite_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_ap_rewrite_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->engine = NGX_CONF_UNSET;
    conf->max_loops = NGX_CONF_UNSET_UINT;

    return conf;
}

static char *
ngx_http_ap_rewrite_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_ap_rewrite_loc_conf_t *prev = parent;
    ngx_http_ap_rewrite_loc_conf_t *conf = child;

    ngx_conf_merge_value(conf->engine, prev->engine, 0);
    ngx_conf_merge_uint_value(conf->max_loops, prev->max_loops,
                              NGX_HTTP_AP_REWRITE_DEFAULT_MAX_LOOPS);

    if (conf->rules == NULL) {
        conf->rules = prev->rules;
    }

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_ap_rewrite_parse_flags(ngx_conf_t *cf, ngx_http_ap_rewrite_rule_t *rule,
    ngx_str_t *value)
{
    u_char     *start, *end, *p, *last, *eq;
    ngx_uint_t  len;

    if (value->len < 2 || value->data[0] != '['
        || value->data[value->len - 1] != ']')
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid RewriteRule flags \"%V\"", value);
        return NGX_ERROR;
    }

    start = value->data + 1;
    end = value->data + value->len - 1;

    for (p = start; p < end; /* void */) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == ',')) {
            p++;
        }

        if (p == end) {
            break;
        }

        last = p;
        while (last < end && *last != ',' && *last != ' ' && *last != '\t') {
            last++;
        }

        len = last - p;

        if (len == 0) {
            p = last + 1;
            continue;
        }

        eq = ngx_strlchr(p, last, '=');

        if (eq == NULL) {
            if (len == 1 && (p[0] == 'L' || p[0] == 'l')) {
                rule->last = 1;

            } else if (len == 2
                       && (ngx_strncasecmp(p, (u_char *) "NC", 2) == 0))
            {
                rule->regex_options |= NGX_REGEX_CASELESS;

            } else if (len == 3
                       && (ngx_strncasecmp(p, (u_char *) "QSA", 3) == 0))
            {
                rule->qs_append = 1;

            } else if (len == 3
                       && (ngx_strncasecmp(p, (u_char *) "END", 3) == 0))
            {
                rule->last = 1;

            } else if (len == 1
                       && (p[0] == 'R' || p[0] == 'r'))
            {
                rule->redirect = 1;
                rule->status = NGX_HTTP_MOVED_TEMPORARILY;

            } else if (ngx_strncasecmp(p, (u_char *) "REDIRECT", len) == 0) {
                rule->redirect = 1;
                rule->status = NGX_HTTP_MOVED_TEMPORARILY;

            } else {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "unsupported RewriteRule flag \"%*s\"",
                                   len, p);
                return NGX_ERROR;
            }

        } else {
            ngx_str_t key, val;

            key.len = eq - p;
            key.data = p;

            val.len = last - (eq + 1);
            val.data = eq + 1;

            if (key.len == 1
                && (key.data[0] == 'R' || key.data[0] == 'r'))
            {
                rule->redirect = 1;

                if (val.len == 0) {
                    rule->status = NGX_HTTP_MOVED_TEMPORARILY;

                } else if (ngx_strncasecmp(val.data, (u_char *) "permanent",
                                            val.len) == 0)
                {
                    rule->status = NGX_HTTP_MOVED_PERMANENTLY;

                } else if (ngx_strncasecmp(val.data, (u_char *) "temp",
                                            val.len) == 0)
                {
                    rule->status = NGX_HTTP_MOVED_TEMPORARILY;

                } else if (ngx_strncasecmp(val.data, (u_char *) "seeother",
                                            val.len) == 0)
                {
                    rule->status = NGX_HTTP_SEE_OTHER;

                } else {
                    ngx_int_t n;

                    n = ngx_atoi(val.data, val.len);
                    if (n == NGX_ERROR || n < 300 || n > 399) {
                        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                           "invalid redirect code \"%V\"",
                                           &val);
                        return NGX_ERROR;
                    }

                    rule->status = (ngx_uint_t) n;
                }

            } else if (ngx_strncasecmp(key.data, (u_char *) "NC", key.len) == 0) {
                rule->regex_options |= NGX_REGEX_CASELESS;

            } else if (ngx_strncasecmp(key.data, (u_char *) "QSA", key.len) == 0) {
                rule->qs_append = 1;

            } else {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "unsupported RewriteRule flag \"%V\"",
                                   &key);
                return NGX_ERROR;
            }
        }

        p = last;
    }

    return NGX_OK;
}

static char *
ngx_http_ap_rewrite_rule(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_ap_rewrite_loc_conf_t  *lcf = conf;

    u_char                           errstr[NGX_MAX_CONF_ERRSTR];
    ngx_str_t                       *value;
    ngx_regex_compile_t              rc;
    ngx_http_ap_rewrite_rule_t      *rule;
    ngx_http_compile_complex_value_t ccv;
    ngx_int_t                        rv;
    ngx_uint_t                       options;

    if (lcf->rules == NULL) {
        lcf->rules = ngx_array_create(cf->pool, 4,
                                      sizeof(ngx_http_ap_rewrite_rule_t));
        if (lcf->rules == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    rule = ngx_array_push(lcf->rules);
    if (rule == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(rule, sizeof(ngx_http_ap_rewrite_rule_t));
    rule->status = NGX_HTTP_MOVED_TEMPORARILY;

    value = cf->args->elts;

    if (value[1].len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "empty RewriteRule pattern");
        return NGX_CONF_ERROR;
    }

    if (value[2].len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "empty RewriteRule replacement");
        return NGX_CONF_ERROR;
    }

    if (cf->args->nelts == 4) {
        rv = ngx_http_ap_rewrite_parse_flags(cf, rule, &value[3]);
        if (rv != NGX_OK) {
            return NGX_CONF_ERROR;
        }
    }

    options = rule->regex_options;
    rule->regex_options = 0;

    ngx_memzero(&rc, sizeof(ngx_regex_compile_t));

    rc.pattern = value[1];
    rc.pool = cf->pool;
    rc.err.len = NGX_MAX_CONF_ERRSTR;
    rc.err.data = errstr;
    rc.options = options;

    rule->name.len = value[1].len;
    rule->name.data = ngx_pnalloc(cf->pool, value[1].len);
    if (rule->name.data == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memcpy(rule->name.data, value[1].data, value[1].len);

    rule->regex = ngx_http_regex_compile(cf, &rc);
    if (rule->regex == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[2];
    ccv.complex_value = &rule->replacement;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

static char *
ngx_http_ap_rewrite_options(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_ap_rewrite_loc_conf_t *lcf = conf;

    ngx_str_t  *value;
    ngx_int_t   n;
    ngx_uint_t  i;

    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {
        if (ngx_strncmp(value[i].data, "MaxRedirects=", 13) == 0) {
            n = ngx_atoi(value[i].data + 13, value[i].len - 13);
            if (n == NGX_ERROR || n <= 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid MaxRedirects value \"%V\"",
                                   &value[i]);
                return NGX_CONF_ERROR;
            }

            lcf->max_loops = (ngx_uint_t) n;
            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "unsupported RewriteOptions token \"%V\"",
                           &value[i]);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

#else /* NGX_PCRE */

static ngx_command_t ngx_http_ap_rewrite_commands[] = {
      ngx_null_command
};

static ngx_http_module_t ngx_http_ap_rewrite_module_ctx = {
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

ngx_module_t ngx_http_ap_rewrite_module = {
    NGX_MODULE_V1,
    &ngx_http_ap_rewrite_module_ctx,
    ngx_http_ap_rewrite_commands,
    NGX_HTTP_MODULE,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NGX_MODULE_V1_PADDING
};

#endif /* NGX_PCRE */
