#!/usr/bin/perl

# Reload functional coverage using the native access module.

use warnings;
use strict;
use Test::More;

BEGIN { use FindBin; chdir($FindBin::Bin); }
use Test::Nginx;

select STDERR; $| = 1;
select STDOUT; $| = 1;

my $t = Test::Nginx->new()->has(qw/http access
	ngx_http_phase_control_module/)->plan(6);
$t->write_file('ok', 'native');

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
        add_header X-Generation $generation always;
        location / { deny all; }
    }
}
EOF
}

sub response {
	my ($generation) = @_;
	my $response = '';
	for (1 .. 100) {
		$response = http_get('/ok');
		last if $response =~ /^X-Generation: $generation\x0d$/m;
		select undef, undef, undef, 0.05;
	}
	return $response;
}

configure(1, 'skip_access on;');
$t->run();
my $response = response(1);
like($response, qr/^X-Generation: 1\x0d$/m, 'initial configuration is active');
like($response, qr/^HTTP\/1\.[01] 200 /, 'initial configuration skips deny');

configure(2, 'skip_access off;');
$t->reload();
$response = response(2);
like($response, qr/^X-Generation: 2\x0d$/m, 'reload applies the new configuration');
like($response, qr/^HTTP\/1\.[01] 403 /, 'reload restores native access checks');

configure(3, '');
$t->reload();
$response = response(3);
like($response, qr/^X-Generation: 3\x0d$/m, 'configuration without switches loads');
like($response, qr/^HTTP\/1\.[01] 403 /, 'removing switches preserves native deny');

$t->stop();
undef $t;
