#!/usr/bin/env bash

set -euo pipefail

module_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
nginx_source="${TEST_NGINX_SOURCE:-$module_root/.ci/nginx}"
test_output="$module_root/.ci/test-output"
mkdir -p "$test_output"
exec > >(tee "$test_output/run.log") 2>&1

configure_args=(
    --prefix="$nginx_source/test-prefix"
    --with-debug
    --with-http_ssl_module
    --with-http_realip_module
    --with-http_auth_request_module
)

if [[ "${TEST_EXPR:-enabled}" == enabled ]]; then
    configure_args+=(--with-http_json_module
        --add-module="$module_root/.ci/ngx_expr_module")
fi

if [[ "${TEST_LINK:-static}" == dynamic ]]; then
    module_arg="--add-dynamic-module=$module_root"
else
    module_arg="--add-module=$module_root"
fi

if [[ "${TEST_PROBE:-disabled}" == enabled ]]; then
    probe_arg="--add-module=$module_root/t/lib/ngx_http_phase_probe_module"
    if [[ "${TEST_ORDER:-before}" == before ]]; then
        configure_args+=("$module_arg" "$probe_arg")
    else
        configure_args+=("$probe_arg" "$module_arg")
    fi
else
    configure_args+=("$module_arg")
fi

cd "$nginx_source"
if [[ -f configure ]]; then
    sh ./configure "${configure_args[@]}"
else
    sh auto/configure "${configure_args[@]}"
fi
make -j"${TEST_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}"

export TEST_NGINX_BINARY="$nginx_source/objs/nginx"
export PERL5LIB="$module_root/.ci/nginx-tests/lib"
export TMPDIR="$(mktemp -d /tmp/nginx-phase-control.XXXXXX)"
export TEST_NGINX_LEAVE=1
if [[ "$(id -u)" == 0 ]]; then
    export TEST_NGINX_GLOBALS="${TEST_NGINX_GLOBALS:-user root;}"
fi
if [[ "${TEST_LINK:-static}" == dynamic ]]; then
    export TEST_NGINX_GLOBALS="${TEST_NGINX_GLOBALS:-} load_module $nginx_source/objs/ngx_http_phase_control_module.so;"
fi
trap 'find "$TMPDIR" -type f -name "*.log" -exec cp --parents {} "$test_output/" \;' EXIT

"$TEST_NGINX_BINARY" -V
cd "$module_root"
prove -v t/*.t t/probe/*.t
