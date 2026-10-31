#!/usr/bin/perl

# Native location-if configuration and redirection regressions.

use warnings;
use strict;
use Test::More;

BEGIN { use FindBin; chdir($FindBin::Bin); }
use Test::Nginx;

select STDERR; $| = 1;
select STDOUT; $| = 1;

my $t = Test::Nginx->new()->has(qw/http rewrite access limit_req mirror ssi
	ngx_http_phase_control_module/)->plan(56);
my @phases = qw/preaccess access precontent/;
my %blockers = (
	preaccess => 'limit_req zone=requests; limit_req_status 429;',
	access => 'deny all;',
	precontent => 'mirror /_mirror; mirror_request_body off;'
);
my %blocked = (preaccess => 429, access => 403, precontent => 418);

$t->write_file('ok', 'native');
$t->write_file('target', 'target');
$t->write_file('sub', 'subrequest');
$t->write_file('index.shtml', '<!--#include virtual="/sub?go=1" -->');
mkdir $t->testdir() . '/named' or die "mkdir: $!";
$t->write_file('named/start', 'named');

sub start {
	my ($server, $http) = @_;
	$http ||= '';
	$t->stop();
	$t->write_file('mirror.log', '');
	$t->write_file_expand('nginx.conf', <<EOF);
%%TEST_GLOBALS%%
daemon off;
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    limit_req_zone \$uri zone=requests:1m rate=1r/m;
    $http
    server {
        listen 127.0.0.1:8080;
        server_name localhost;
        $server
        location = /_mirror {
            internal;
            mirror off;
            log_subrequest on;
            access_log %%TESTDIR%%/mirror.log;
            return 204;
        }
    }
}
EOF
	$t->run();
}

sub phase_is {
	my ($phase, $uri, $skip, $name) = @_;
	if ($phase eq 'precontent') {
		my $before = () = $t->read_file('mirror.log') =~ /\n/g;
		my ($status) = http_get($uri) =~ /^HTTP\/1\.[01] (\d+)/;
		my $after = () = $t->read_file('mirror.log') =~ /\n/g;
		is_deeply([$status, $after - $before], [200, $skip ? 0 : 1], $name);
		return;
	}

	http_get($uri) if $phase eq 'preaccess';
	my ($status) = http_get($uri) =~ /^HTTP\/1\.[01] (\d+)/;
	is($status, $skip ? 200 : $blocked{$phase}, $name);
}

for my $phase (@phases) {
	start(<<EOF);
location / {
    if (\$arg_go) { skip_$phase on; }
    $blockers{$phase}
}
EOF
	phase_is($phase, '/ok?go=1', 1, "matching location if skips $phase");
	phase_is($phase, '/ok', 0, "false location if preserves $phase");

	start(<<EOF);
location / {
    skip_$phase on;
    if (\$arg_go) { skip_$phase off; }
    $blockers{$phase}
}
EOF
	phase_is($phase, '/ok?go=1', 0, "location if off restores $phase");
	phase_is($phase, '/ok', 1, "false location if inherits skip_$phase on");
}

start(<<'EOF');
location / {
    skip_preaccess on;
    skip_access on;
    skip_precontent on;
    if ($arg_go) { skip_access off; }
    limit_req zone=requests;
    limit_req_status 429;
    mirror /_mirror;
    mirror_request_body off;
}
EOF
for (1 .. 2) {
	phase_is('precontent', '/ok?go=1', 1,
		"if preserves the other inherited phase switches, request $_");
}

start(<<'EOF');
location / {
    skip_access on;
    location /ok {
        if ($arg_go) { skip_access off; }
        deny all;
    }
}
EOF
phase_is('access', '/ok', 1, 'nested location inherits parent switch');
phase_is('access', '/ok?go=1', 0, 'nested location if overrides parent switch');

start(<<'EOF');
location / {
    if ($arg_first) { skip_access on; }
    if ($arg_second) { skip_precontent off; }
    deny all;
}
EOF
phase_is('access', '/ok?first=1', 1, 'false later if keeps the selected configuration');
phase_is('access', '/ok?first=1&second=1', 0,
	'later matching if inherits the location, not an earlier sibling if');

start(<<'EOF');
location / {
    if ($arg_go) {
        skip_access on;
        break;
    }
    deny all;
}
EOF
phase_is('access', '/ok?go=1', 1, 'rewrite break retains the matching if configuration');

start(<<'EOF');
location / {
    skip_rewrite on;
    if ($arg_go) { skip_access on; }
    deny all;
}
EOF
phase_is('access', '/ok?go=1', 0, 'skip_rewrite prevents the if from executing');

for my $kind (qw/uri named/) {
	for my $phase (@phases) {
		for my $target (qw/on off/) {
			my $source = $target eq 'on' ? 'off' : 'on';
			my $destination = $kind eq 'uri' ? '/target' : '@target';
			my $condition = $kind eq 'uri' ? '/target' : '/start';
			my $root = $kind eq 'uri' ? '' : 'root %%TESTDIR%%/named;';
			start(<<EOF);
location = /start {
    if (\$arg_go) { skip_$phase $source; }
    error_page 404 = $destination;
}
location $destination {
    $root
    if (\$uri = $condition) { skip_$phase $target; }
    $blockers{$phase}
}
EOF
			phase_is($phase, '/start?go=1', $target eq 'on',
				"$kind redirect reevaluates $phase from $source to $target");
		}
	}
}

for my $phase (@phases) {
	start(<<EOF);
location / {
    if (\$uri = /target) { skip_$phase on; }
    rewrite ^/start\$ /target last;
    $blockers{$phase}
}
EOF
	phase_is($phase, '/start', 1, "rewrite cycle reevaluates if for $phase");
}

start(<<'EOF');
location = /index.shtml {
    default_type text/html;
    ssi on;
}
location = /sub {
    if ($arg_go) { skip_access off; }
    deny all;
}
EOF
like(http_get('/index.shtml'), qr/\x0d\x0a\x0d\x0asubrequest$/,
	'location if preserves native access bypass for subrequests');

SKIP: {
	skip 'ngx_expr_module is not available', 2
		unless $t->has_module('ngx_expr_module');
	start(<<'EOF', 'expr requested str_eq $arg_when yes;');
location / {
    when requested { skip_access on; }
    if ($arg_go) { skip_precontent off; }
    deny all;
}
EOF
	phase_is('access', '/ok?when=yes&go=1', 1,
		'if inherits a conditional switch for another phase');
	phase_is('access', '/ok?go=1', 0,
		'false inherited when preserves access checks inside if');
}

$t->stop();
my @invalid;
for my $phase (qw/post_read server_rewrite rewrite/) {
	push @invalid, ["skip_$phase on;", 'is not allowed here'];
}
for my $phase (@phases) {
	push @invalid, ["skip_$phase on; skip_$phase off;", 'is duplicate'];
	push @invalid, ["skip_$phase maybe;", 'invalid value'];
}
for my $case (@invalid) {
	my ($directive, $error) = @$case;
	$t->write_file_expand('invalid.conf', <<EOF);
%%TEST_GLOBALS%%
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    server { location / { if (\$arg_go) { $directive } } }
}
EOF
	my $nginx = $ENV{TEST_NGINX_BINARY} || 'nginx';
	my $dir = $t->testdir();
	my $output = `"$nginx" -p "$dir/" -c invalid.conf -t 2>&1`;
	isnt($?, 0, "location if rejects $directive");
	like($output, qr/\Q$error\E/, 'configuration error is reported');
}

undef $t;
