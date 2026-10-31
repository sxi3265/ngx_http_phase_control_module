#!/usr/bin/perl

# Inspect the set of wrapped phases across configuration cycles.

use warnings;
use strict;
use Test::More;

BEGIN { use FindBin; chdir($FindBin::Bin); }
use Test::Nginx;

select STDERR; $| = 1;
select STDOUT; $| = 1;

my $t = Test::Nginx->new()->has(qw/http rewrite
	ngx_http_phase_control_module ngx_http_phase_probe_module/)->plan(6);

sub configure {
	my ($generation, $directive) = @_;
	$t->write_file_expand('nginx.conf', <<EOF);
%%TEST_GLOBALS%%
daemon off;
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    $directive
    server {
        listen 127.0.0.1:8080;
        server_name localhost;
        add_header X-Generation $generation;
        add_header X-Wrapped \$phase_probe_wrapped;
        location / { phase_probe; }
    }
}
EOF
}

configure(1, 'skip_access on;');
$t->run();
like(http_get('/'), qr/^X-Wrapped: access,\x0d$/m,
	'initial cycle wraps access');
unlike(http_get('/'), qr/,access,/, 'initial cycle skips access');

configure(2, 'skip_rewrite on;');
$t->reload();
my $response = '';
for (1 .. 100) {
	$response = http_get('/');
	last if $response =~ /^X-Generation: 2\x0d$/m;
	select undef, undef, undef, 0.05;
}
like($response, qr/^X-Wrapped: rewrite,\x0d$/m,
	'reload wraps only the newly configured phase');
like($response, qr/post_read,server_rewrite,preaccess,access,precontent,/,
	'reload restores access and skips rewrite');

configure(3, '');
$t->reload();
for (1 .. 100) {
	$response = http_get('/');
	last if $response =~ /^X-Generation: 3\x0d$/m;
	select undef, undef, undef, 0.05;
}
unlike($response, qr/^X-Wrapped:/mi,
	'reload without directives leaves all checkers unwrapped');
like($response, qr/post_read,server_rewrite,rewrite,preaccess,access,precontent,/,
	'reload without directives restores every phase');

$t->stop();
undef $t;
