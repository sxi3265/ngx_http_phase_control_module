#!/usr/bin/perl

# Probe-assisted conditional decisions, resumption and handler postponement.

use warnings;
use strict;
use Test::More;

BEGIN { use FindBin; chdir($FindBin::Bin); }
use Test::Nginx;

select STDERR; $| = 1;
select STDOUT; $| = 1;

my $t = Test::Nginx->new()->has(qw/http rewrite ngx_expr_module
	ngx_http_phase_control_module ngx_http_phase_probe_module/)->plan(51);
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
    expr requested str_eq \$arg_skip yes;
    expr ready str_eq \$phase_probe_ready 1;
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

for my $phase (@phases) {
	my $trace = join('', map { "$_," } grep { $_ ne $phase } @phases);
	start("when requested { skip_$phase on; }", 'location / { phase_probe; }');
	body_is('/?skip=yes', $trace, "http when can skip $phase");
	body_is('/', $all, "false when preserves $phase");
	like(http_get('/'), qr/^X-Wrapped: \Q$phase\E,\x0d$/m,
		"when-only directive wraps only $phase even when condition is false");
}

start('skip_access on;', <<'EOF');
when requested { skip_access off; }
location / { phase_probe; }
EOF
body_is('/?skip=yes', $all, 'server when off overrides inherited on');
body_is('/', 'post_read,server_rewrite,rewrite,preaccess,precontent,',
	'false server condition inherits parent on');

for my $phase (@phases) {
	start("phase_probe_delay $phase;", <<EOF);
when ready { skip_$phase on; }
location / { phase_probe; }
EOF
	body_is('/', $all, "$phase decision survives asynchronous first handler");
}

for my $phase (@phases) {
	my $trace = join('', map { "$_," } grep { $_ ne $phase } @phases);
	start("phase_probe_postpone $phase;", <<EOF);
when ready { skip_$phase on; }
when requested { skip_$phase on; }
location / { phase_probe; }
EOF
	body_is('/', $all, "$phase decision survives first-handler postponement");
	body_is('/?skip=yes', $trace, "$phase still skips after handlers have moved");
	body_is('/', $all, "$phase remains enabled for the next ordinary request");
}

start('', <<'EOF');
expr second str_eq $uri /second;
location / {
    when second { skip_rewrite on; }
    rewrite ^/first$ /second last;
    if ($uri = /second) { return 418; }
    phase_probe;
}
EOF
like(http_get('/first'), qr/^HTTP\/1\.1 200 /,
	'rewrite loop reevaluates when even in the same location');

start('', <<'EOF');
location /scope {
    when requested { skip_access on; }
    deny all;
    phase_probe;
}
EOF
like(http_get('/scope?skip=yes'), qr/^HTTP\/1\.1 200 /,
	'location when skips access');
like(http_get('/scope'), qr/^HTTP\/1\.1 403 /,
	'location condition miss retains access checks');

$t->stop();
for my $phase (@phases[0 .. 1]) {
	$t->write_file_expand('invalid.conf', <<EOF);
%%TEST_GLOBALS%%
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    expr requested str_eq \$arg_skip yes;
    server {
        location / { when requested { skip_$phase on; } }
    }
}
EOF
	my $nginx = $ENV{TEST_NGINX_BINARY} || 'nginx';
	my $dir = $t->testdir();
	my $output = `"$nginx" -p "$dir/" -c invalid.conf -t 2>&1`;
	isnt($?, 0, "location when rejects skip_$phase");
	like($output, qr/is not allowed here/, 'context error is reported');
}

undef $t;
