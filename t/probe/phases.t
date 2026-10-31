#!/usr/bin/perl

# Probe-assisted phase tracing, directive scope and configuration inheritance.

use warnings;
use strict;
use Test::More;

BEGIN { use FindBin; chdir($FindBin::Bin); }
use Test::Nginx;

select STDERR; $| = 1;
select STDOUT; $| = 1;

my $t = Test::Nginx->new()->has(qw/http rewrite access ssi
	ngx_http_phase_control_module ngx_http_phase_probe_module/)->plan(92);
my @phases = qw/post_read server_rewrite rewrite preaccess access precontent/;
my $all = join('', map { "$_," } @phases);

sub start {
	my ($http, $server) = @_;
	$t->stop();
	$t->write_file_expand('nginx.conf', <<EOF);
%%TEST_GLOBALS%%
daemon off;
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    $http
    server {
        listen 127.0.0.1:8080;
        server_name localhost;
        add_header X-Wrapped \$phase_probe_wrapped always;
        $server
    }
}
EOF
	$t->run();
}

sub body_is {
	my ($uri, $expected, $name) = @_;
	like(http_get($uri), qr/\x0d\x0a\x0d\x0a\Q$expected\E$/, $name);
}

start('', 'location / { phase_probe; }');
body_is('/', $all, 'no directives preserves every phase');
unlike(http_get('/'), qr/^X-Wrapped:/mi, 'no directives adds no checker wrappers');

for my $phase (@phases) {
	my $trace = join('', map { "$_," } grep { $_ ne $phase } @phases);
	start("skip_$phase on;", 'location / { phase_probe; }');
	body_is('/', $trace, "http skip_$phase is inherited and skips its phase");
	like(http_get('/'), qr/^X-Wrapped: \Q$phase\E,\x0d$/m,
		"only configured $phase is wrapped");

	start("skip_$phase on;", "skip_$phase off; location / { phase_probe; }");
	body_is('/', $all, "server off overrides inherited skip_$phase");

	start('', "skip_$phase on; location / { phase_probe; }");
	body_is('/', $trace, "server skip_$phase skips its phase");
}

for my $phase (@phases[2 .. 5]) {
	my $trace = join('', map { "$_," } grep { $_ ne $phase } @phases);
	start('', <<EOF);
location /scope {
    skip_$phase on;
    phase_probe;
    location /scope/off {
        skip_$phase off;
        phase_probe;
    }
}
location / { phase_probe; }
EOF
	body_is('/scope', $trace, "location skip_$phase skips its phase");
	body_is('/scope/off', $all, "nested location can restore $phase");
	body_is('/', $all, "sibling location keeps $phase enabled");
}

start('', <<'EOF');
set $value before;
location / {
    skip_rewrite on;
    set $value after;
    return 418;
    phase_probe;
    add_header X-Value $value;
}
EOF
like(http_get('/'), qr/^HTTP\/1\.1 200 /, 'skip_rewrite skips return bytecode');
like(http_get('/'), qr/^X-Value: before\x0d$/m, 'skip_rewrite skips set bytecode');

start('', 'skip_server_rewrite on; return 418; location / { phase_probe; }');
like(http_get('/'), qr/^HTTP\/1\.1 200 /, 'skip_server_rewrite skips server bytecode');

for my $satisfy (qw/all any/) {
	start('', "location / { satisfy $satisfy; deny all; skip_access on; phase_probe; }");
	like(http_get('/'), qr/^HTTP\/1\.1 200 /, "skip_access bypasses satisfy $satisfy");
}
start('', 'location / { deny all; skip_access off; phase_probe; }');
like(http_get('/'), qr/^HTTP\/1\.1 403 /, 'skip_access off retains access checks');

start('', 'location / { try_files /missing =418; skip_precontent on; phase_probe; }');
like(http_get('/'), qr/^HTTP\/1\.1 200 /, 'skip_precontent skips try_files');
start('', 'location / { try_files /missing =418; skip_precontent off; phase_probe; }');
like(http_get('/'), qr/^HTTP\/1\.1 418 /, 'skip_precontent off retains try_files');

start('', <<'EOF');
location = /first {
    skip_rewrite off;
    rewrite ^ /second last;
}
location = /second {
    skip_rewrite on;
    return 418;
    phase_probe;
}
EOF
like(http_get('/first'), qr/^HTTP\/1\.1 200 /,
	'rewrite cycle evaluates the new location configuration');

start('', <<'EOF');
error_page 404 = /second;
location = /first { return 404; }
location = /second {
    skip_rewrite on;
    return 418;
    phase_probe;
}
EOF
like(http_get('/first'), qr/^HTTP\/1\.1 200 /,
	'internal redirect evaluates the new location configuration');

start('', <<'EOF');
location = /first { try_files /missing @second; }
location @second {
    skip_rewrite on;
    return 418;
    phase_probe;
}
EOF
like(http_get('/first'), qr/^HTTP\/1\.1 200 /,
	'named location enters the wrapped rewrite phase');

start('phase_probe_postpone rewrite;', <<'EOF');
skip_rewrite off;
set $once "${once}x";
uninitialized_variable_warn off;
add_header X-Once $once;
phase_probe;
EOF
like(http_get('/'), qr/^X-Once: x\x0d$/m,
	'wrapping preserves the server null location rewrite guard');

$t->write_file('index.shtml', '<!--#include virtual="/sub" -->');
start('', <<'EOF');
location = /index.shtml {
    root %%TESTDIR%%;
    default_type text/html;
    ssi on;
}
location = /sub {
    skip_access off;
    deny all;
    phase_probe;
}
EOF
my $sub = http_get('/index.shtml');
like($sub, qr/server_rewrite,rewrite,preaccess,precontent,/,
	'subrequests retain nginx access-phase bypass');
unlike($sub, qr/,access,/, 'subrequest access handlers are not introduced');

$t->stop();

my @invalid;
for my $phase (@phases) {
	push @invalid, ["skip_$phase maybe;", 'invalid value'];
	push @invalid, ["skip_$phase on; skip_$phase off;", 'is duplicate'];
	push @invalid, ["if (\$arg_x) { skip_$phase on; }", 'is not allowed here'];
}
for my $phase (@phases[0 .. 1]) {
	push @invalid, ["location / { skip_$phase on; }", 'is not allowed here'];
}

for my $case (@invalid) {
	my ($directive, $error) = @$case;
	$t->write_file_expand('invalid.conf', <<EOF);
%%TEST_GLOBALS%%
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    server { $directive }
}
EOF
	my $nginx = $ENV{TEST_NGINX_BINARY} || 'nginx';
	my $dir = $t->testdir();
	my $output = `"$nginx" -p "$dir/" -c invalid.conf -t 2>&1`;
	isnt($?, 0, "reject $directive");
	like($output, qr/\Q$error\E/, "configuration error: $error");
}

undef $t;
