#!/usr/bin/perl

# Functional regressions using nginx's built-in modules, without a probe.

use warnings;
use strict;
use Test::More;

BEGIN { use FindBin; chdir($FindBin::Bin); }
use Test::Nginx;

select STDERR; $| = 1;
select STDOUT; $| = 1;

my $t = Test::Nginx->new()->has(qw/http rewrite realip limit_req access ssi
	ngx_http_phase_control_module/)->plan(107);
my @phases = qw/post_read server_rewrite rewrite preaccess access precontent/;
my %blocked = (
	server_rewrite => 418, rewrite => 418, preaccess => 429,
	access => 403, precontent => 418
);

$t->write_file('ok', 'native');
$t->write_file('other', 'rewritten');
$t->write_file('second', 'second');
$t->write_file('sub', 'subrequest');
$t->write_file('index.shtml', '<!--#include virtual="/sub" -->');

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
        $server
    }
}
EOF
	$t->run();
}

sub request {
	my ($uri) = @_;
	return http(<<EOF);
GET $uri HTTP/1.0
Host: localhost
X-Real-IP: 192.0.2.1

EOF
}

sub status_is {
	my ($uri, $expected, $name) = @_;
	my ($status) = request($uri) =~ /^HTTP\/1\.[01] (\d+)/;
	is($status, $expected, $name);
}

sub body_is {
	my ($uri, $expected, $name) = @_;
	like(request($uri), qr/\x0d\x0a\x0d\x0a\Q$expected\E$/, $name);
}

sub phase_config {
	my ($phase, $http, $server, $location) = @_;
	my $blocker = '';

	if ($phase eq 'post_read') {
		$server .= ' set_real_ip_from 127.0.0.1; real_ip_header X-Real-IP;';
		# Respond before preaccess, where realip has another handler.
		$blocker = 'return 200 $remote_addr;';

	} elsif ($phase eq 'server_rewrite') {
		$server .= ' return 418;';

	} elsif ($phase eq 'rewrite') {
		$blocker = 'return 418;';

	} elsif ($phase eq 'preaccess') {
		$http .= ' limit_req_zone $uri zone=requests:1m rate=1r/m;';
		$blocker = 'limit_req zone=requests; limit_req_status 429;';

	} elsif ($phase eq 'access') {
		$blocker = 'deny all;';

	} elsif ($phase eq 'precontent') {
		$blocker = 'try_files /missing =418;';
	}

	start($http, "$server location / { $location $blocker }");
}

sub phase_is {
	my ($phase, $skip, $name, $uri) = @_;
	$uri ||= '/ok';
	if ($phase eq 'post_read') {
		body_is($uri, $skip ? '127.0.0.1' : '192.0.2.1', $name);
		return;
	}

	request($uri) if $phase eq 'preaccess';
	status_is($uri, $skip ? 200 : $blocked{$phase}, $name);
}

for my $phase (@phases) {
	phase_config($phase, '', '', '');
	phase_is($phase, 0, "$phase runs by default");

	phase_config($phase, "skip_$phase on;", '', '');
	phase_is($phase, 1, "http skip_$phase is inherited");

	phase_config($phase, "skip_$phase on;", "skip_$phase off;", '');
	phase_is($phase, 0, "server off restores $phase");

	phase_config($phase, '', "skip_$phase on;", '');
	phase_is($phase, 1, "server skip_$phase takes effect");
}

for my $phase (@phases[2 .. 5]) {
	phase_config($phase, '', "skip_$phase on;", "skip_$phase off;");
	phase_is($phase, 0, "location off restores $phase");

	phase_config($phase, '', '', "skip_$phase on;");
	phase_is($phase, 1, "location skip_$phase takes effect");
}

start('', 'location / { satisfy any; deny all; skip_access on; }');
status_is('/ok', 200, 'skip_access also bypasses satisfy any');

start('', <<'EOF');
set $marker before;
location / {
    skip_rewrite on;
    set $marker after;
    add_header X-Marker $marker;
}
EOF
like(request('/ok'), qr/^X-Marker: before\x0d$/m,
	'skip_rewrite skips set bytecode');

for my $skip (qw/on off/) {
	start('', "location / { skip_rewrite $skip; rewrite ^/ok\$ /other last; }");
	body_is('/ok', $skip eq 'on' ? 'native' : 'rewritten',
		"skip_rewrite $skip controls URI rewriting");
}

start('', <<'EOF');
location = /first {
    skip_rewrite off;
    rewrite ^ /second last;
}
location = /second {
    skip_rewrite on;
    return 418;
}
EOF
body_is('/first', 'second', 'rewrite cycle reads the new location configuration');

start('', <<'EOF');
error_page 404 = /second;
location = /first { return 404; }
location = /second {
    skip_rewrite on;
    return 418;
}
EOF
body_is('/first', 'second', 'error_page redirect reads the new configuration');

start('', <<'EOF');
location = /ok { try_files /missing @second; }
location @second {
    skip_rewrite on;
    return 418;
}
EOF
body_is('/ok', 'native', 'named location applies skip_rewrite');

start('', <<'EOF');
location = /index.shtml {
    default_type text/html;
    ssi on;
}
location = /sub {
    skip_access off;
    deny all;
}
EOF
body_is('/index.shtml', 'subrequest', 'subrequests retain native access bypass');

sub config_error {
	my ($http, $server, $error, $name) = @_;
	$t->stop();
	$t->write_file_expand('invalid.conf', <<EOF);
%%TEST_GLOBALS%%
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    $http
    server { $server }
}
EOF
	my $nginx = $ENV{TEST_NGINX_BINARY} || 'nginx';
	my $dir = $t->testdir();
	my $output = `"$nginx" -p "$dir/" -c invalid.conf -t 2>&1`;
	isnt($?, 0, $name);
	like($output, qr/\Q$error\E/, 'configuration error is reported');
}

for my $phase (@phases) {
	config_error('', "skip_$phase maybe;", 'invalid value',
		"skip_$phase rejects invalid values");
	config_error('', "skip_$phase on; skip_$phase off;", 'is duplicate',
		"skip_$phase rejects duplicates");
	config_error('', "if (\$arg_x) { skip_$phase on; }", 'is not allowed here',
		"skip_$phase is not allowed inside server if");
}
for my $phase (@phases[0 .. 1]) {
	config_error('', "location / { skip_$phase on; }", 'is not allowed here',
		"skip_$phase is not allowed in location");
}

SKIP: {
	skip 'ngx_expr_module is not available', 27
		unless $t->has_module('ngx_expr_module');

	my $expr = 'expr requested str_eq $arg_skip yes;';
	for my $phase (@phases) {
		phase_config($phase, "$expr when requested { skip_$phase on; }", '', '');
		phase_is($phase, 1, "matching http when skips $phase", '/ok?skip=yes');
		phase_is($phase, 0, "false http when preserves $phase", '/ok');
	}

	phase_config('access', "$expr skip_access on;",
		'when requested { skip_access off; }', '');
	phase_is('access', 0, 'server when off overrides parent on', '/ok?skip=yes');
	phase_is('access', 1, 'false server condition retains parent on', '/ok');

	for my $phase (@phases[2 .. 5]) {
		phase_config($phase, $expr, '', "when requested { skip_$phase on; }");
		phase_is($phase, 1, "matching location when skips $phase", '/ok?skip=yes');
		phase_is($phase, 0, "false location when preserves $phase", '/ok');
	}

	for my $phase (@phases[0 .. 1]) {
		config_error($expr, "location / { when requested { skip_$phase on; } }",
			'is not allowed here', "location when rejects skip_$phase");
	}

	start('expr second str_eq $uri /second;', <<'EOF');
location / {
    when second { skip_rewrite on; }
    rewrite ^/first$ /second last;
    if ($uri = /second) { return 418; }
}
EOF
	body_is('/first', 'second', 'rewrite loop reevaluates when in the same location');
}

$t->stop();
undef $t;
