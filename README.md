# ngx_http_phase_control_module

Skip selected HTTP request phases before their handlers run. Each phase has an independent switch, evaluated when the request enters that phase.

## Synopsis

```nginx
http {
    server {
        listen 8080;

        location /static/ {
            skip_rewrite on;
            root /srv/www;
        }
    }
}
```

## Installation

Build statically with `--add-module=/path/to/ngx_http_phase_control_module`, or build dynamically with `--add-dynamic-module=/path/to/ngx_http_phase_control_module` and load the resulting module:

```nginx
load_module modules/ngx_http_phase_control_module.so;
```

Build `ngx_expr_module` statically in the same nginx configuration to enable `when` conditions. Without it, the ordinary `on` and `off` switches remain available.

## Conditional configuration

With `ngx_expr_module`, these directives are also available inside `when` blocks in `http`, `server`, or `location`, subject to their context limits:

```nginx
http {
    expr health_check str_eq $uri /healthz;

    server {
        listen 8080;

        when health_check {
            skip_preaccess on;
        }

        location / {
            root /srv/www;
        }
    }
}
```

Use data already available at the phase's entry. A decision cannot depend on variables that would be populated by handlers in the phase being skipped. In particular, an authentication handler in post-read cannot provide the result used to decide whether to skip that same post-read phase.

`skip_preaccess`, `skip_access`, and `skip_precontent` can also be configured inside an `if` block within a location:

```nginx
location / {
    if ($arg_skip_access = yes) {
        skip_access on;
    }
}
```

The `if` runs during rewrite and selects the configuration used by the later phases. Its configuration inherits settings from the enclosing location and can override them with `on` or `off`. If multiple `if` blocks match, nginx uses the configuration selected by the last matching block; settings from separate blocks are not accumulated.

Other modules keep their native `if` inheritance behavior. In particular, nginx does not inherit `try_files` into the implicit location created by `if`; `skip_precontent off` enables the phase but does not restore that missing configuration.

`skip_rewrite` cannot be placed inside `if`, since the rewrite phase has already started when the condition runs. If `skip_rewrite on` is configured outside the block, the `if` itself is skipped. None of these directives supports `if in server`.

Decisions are made once per phase entry, including when the first handler suspends and later resumes. Internal redirects, named-location jumps, and rewrite cycles use the target configuration and reevaluate conditions for the phases nginx enters again. The previous location's `if` selection is not carried over. Phases nginx does not re-enter, such as post-read on an internal redirect, are not run again by this module.

## Directives

Directive values are inherited from the enclosing configuration level unless overridden. The `when` contexts require `ngx_expr_module`.

### skip_post_read

| Syntax: | **skip_post_read** `on` \| `off`; |
| :--- | :--- |
| Default: | `skip_post_read off;` |
| Context: | `http`, `server`, `when` |

Enables or disables skipping the post-read phase. When enabled, all handlers registered in this phase are skipped, including real-IP and internal authentication handlers registered there.

The phase runs after nginx has read the request headers. The directive does not skip reading the request itself.

### skip_server_rewrite

| Syntax: | **skip_server_rewrite** `on` \| `off`; |
| :--- | :--- |
| Default: | `skip_server_rewrite off;` |
| Context: | `http`, `server`, `when` |

Enables or disables skipping the server rewrite phase. When enabled, server-level rewrite scripts and all other handlers registered in this phase are skipped.

### skip_rewrite

| Syntax: | **skip_rewrite** `on` \| `off`; |
| :--- | :--- |
| Default: | `skip_rewrite off;` |
| Context: | `http`, `server`, `location`, `when` |

Enables or disables skipping the location rewrite phase. When enabled, the rewrite script engine (`rewrite`, `return`, `set`, and `if`) and all other rewrite handlers, including `rewrite_by_lua`, are skipped.

A `return` directive in the location therefore does not produce a response when `skip_rewrite` is enabled. Content processing and response filters still run.

### skip_preaccess

| Syntax: | **skip_preaccess** `on` \| `off`; |
| :--- | :--- |
| Default: | `skip_preaccess off;` |
| Context: | `http`, `server`, `location`, `if in location`, `when` |

Enables or disables skipping the preaccess phase. When enabled, all handlers registered in this phase are skipped, including request-rate and connection-limit handlers.

### skip_access

| Syntax: | **skip_access** `on` \| `off`; |
| :--- | :--- |
| Default: | `skip_access off;` |
| Context: | `http`, `server`, `location`, `if in location`, `when` |

Enables or disables skipping the access phase and its post-access result check. When enabled, all access handlers are skipped regardless of whether `satisfy all` or `satisfy any` is configured.

Nginx's usual access-phase bypass for subrequests is preserved.

### skip_precontent

| Syntax: | **skip_precontent** `on` \| `off`; |
| :--- | :--- |
| Default: | `skip_precontent off;` |
| Context: | `http`, `server`, `location`, `if in location`, `when` |

Enables or disables skipping the precontent phase. When enabled, all handlers registered in this phase are skipped, including `try_files`, `mirror`, and `internal_redirect` when configured for this phase.

## Implementation

The module wraps the compiled phase checkers without adding handler entries or changing their indexes. It accommodates handlers that move themselves within a phase, including Lua's postponement behavior. Phases with no corresponding directive anywhere in the configuration are not wrapped. An empty phase needs no wrapper either. Configuration inside `when` counts as configuration even if its condition does not match a particular request.

## Tests

The tests use the nginx-tests framework and are split into two groups:

- `t/*.t` checks request behavior with nginx's built-in modules: realip, rewrite, limit_req, access, try_files, mirror, and SSI. It covers directive scope, location `if`, inheritance, redirects, subrequests, errors, and reloads. Conditional checks run when `ngx_expr_module` is available.
- `t/probe/*.t` uses a test-only C module for phase tracing, inspection of checker wrapping, asynchronous resumption, and handler postponement. This group is skipped automatically when the probe module is unavailable.

Place nginx, ngx_expr_module and nginx-tests in `.ci/nginx`, `.ci/ngx_expr_module` and `.ci/nginx-tests`, then run:

```sh
bash util/run-tests.sh
TEST_EXPR=disabled bash util/run-tests.sh
TEST_LINK=dynamic bash util/run-tests.sh
TEST_PROBE=enabled bash util/run-tests.sh
```

The probe is disabled by default. With `TEST_PROBE=enabled`, both groups run; `TEST_ORDER=after` reverses the static build order relative to the probe. CI covers static and dynamic builds, with and without `ngx_expr_module` and the probe. No Lua code is required by either test group.

## Author and license

Copyright (C) Hanada

Licensed under the [BSD 2-Clause License](LICENSE).
