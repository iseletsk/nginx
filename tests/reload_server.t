#!/usr/bin/env perl
use strict;
use warnings;

use File::Path qw(make_path remove_tree);
use Fcntl qw(SEEK_SET);
use IO::Socket::INET;
use Test::More;
use Time::HiRes qw(sleep);

my $nginx = './objs/nginx';
if ( !-x $nginx ) {
    plan skip_all => "nginx binary not built (expected $nginx)";
}

plan tests => 18;

my $port = 18080;
my $backend_port = 18081;

my $prefix    = 't/reload_server';
my $conf_dir  = "$prefix/conf.d";
my $logs_dir  = "$prefix/logs";
my $html_v1   = "$prefix/html/v1";
my $html_v2   = "$prefix/html/v2";

END {
    if ( -f "$logs_dir/nginx.pid" ) {
        system($nginx, '-p', "$prefix/", '-c', 'nginx.conf', '-s', 'stop');
        sleep 0.2;
    }

    if ( $ENV{KEEP_RELOAD_TEST} ) {
        warn "preserving test directory $prefix\n";
        return;
    }

    remove_tree($prefix);
}

make_path( $conf_dir, $logs_dir, $html_v1, $html_v2 );

write_file( "$prefix/mime.types", <<'MIME' );
types {
    text/plain txt;
}
MIME

write_file( "$html_v1/index.html", "version=1\n" );
write_file( "$html_v2/index.html", "version=2\n" );

write_file( "$prefix/nginx.conf", <<"NGINX" );
worker_processes  1;

error_log  logs/error.log info;
pid        logs/nginx.pid;

events {
    worker_connections  16;
}

http {
    include       mime.types;
    default_type  text/plain;

    sendfile        on;
    keepalive_timeout  65;

    server {
        listen $backend_port;

        location / {
            return 200 "backend=1\n";
        }
    }

    include conf.d/*.conf;
}
NGINX

write_file( "$conf_dir/example.conf", <<"SERVER" );
server {
    listen $port;
    server_name localhost;

    root html/v1;
    index index.html;
}
SERVER

ok( system($nginx, '-p', "$prefix/", '-c', 'nginx.conf') == 0,
    'started nginx instance' )
  or BAIL_OUT('failed to start nginx');

wait_for_file("$logs_dir/nginx.pid") or BAIL_OUT('pid file not created');
my $pid_before = read_pid("$logs_dir/nginx.pid");

is( http_get($port), "version=1\n", 'served initial configuration' );

write_valid_server("$conf_dir/example.conf");

my $reload = system($nginx, '-p', "$prefix/", '-c', 'nginx.conf',
    '-s', 'reload_server=conf.d/example.conf');
is( $reload, 0, 'reload_server command succeeded' );

sleep 0.5;
wait_for_file("$logs_dir/nginx.pid") or BAIL_OUT('pid file missing after reload');
my $pid_after = read_pid("$logs_dir/nginx.pid");
is( $pid_after, $pid_before, 'master PID unchanged after reload' );

my $error_log = "$logs_dir/error.log";

my $body = wait_for_body($port, '/', "version=2\n");
is( $body, "version=2\n", 'served updated index after reload' );

my ( $rewrite_status, $rewrite_body ) = http_request($port, '/rewrite');
is( $rewrite_status, 418, 'rewrite redirected to fallback status' );
is( $rewrite_body, "fallback\n", 'rewrite fallback body' );

my ( $proxy_status, $proxy_body ) = http_request($port, '/proxy/data');
is( $proxy_status, 200, 'proxy_pass returned success' );
is( $proxy_body, "backend=1\n", 'proxy_pass returned backend response' );

is( http_get($port, '/try'), "version=2\n", 'try_files served index fallback' );

my ( $return_status, $return_body ) = http_request($port, '/return');
is( $return_status, 204, 'return directive status' );
is( $return_body, '', 'return directive empty body' );

write_file( "$conf_dir/example.conf", <<"SERVER" );
server {
    listen $port;
    server_name localhost;

    return 444;
}

server {
    listen @{[ $port + 1 ]};
    return 444;
}
SERVER

my $offset = log_offset($error_log);
my $multiple = system($nginx, '-p', "$prefix/", '-c', 'nginx.conf',
    '-s', 'reload_server=conf.d/example.conf');
is( $multiple, 0, 'reload_server command completed for multiple server blocks' );
sleep 0.1;
my $log_tail = read_log_tail( $error_log, $offset );
like( $log_tail,
    qr/reload_server: file "[^"]+" must define exactly one server block/,
    'logged reason for multiple server blocks' );
like( $log_tail, qr/single server reload failed/, 'reported reload failure for multiple servers' );

write_valid_server("$conf_dir/example.conf");

write_file( "$conf_dir/example.conf", <<"SERVER" );
server {
    listen $port
    server_name localhost;
}
SERVER

$offset = log_offset($error_log);
my $syntax_error = system($nginx, '-p', "$prefix/", '-c', 'nginx.conf',
    '-s', 'reload_server=conf.d/example.conf');
is( $syntax_error >> 8, 1, 'reload_server command rejected syntax error' );
sleep 0.1;
$log_tail = read_log_tail( $error_log, $offset );
like( $log_tail,
    qr/invalid parameter "server_name"/, 'logged parse error for syntax failure' );

write_valid_server("$conf_dir/example.conf");

$body = wait_for_body($port, '/', "version=2\n");
is( $body, "version=2\n",
    'served updated configuration after rejected reload attempts' );

sub write_file {
    my ( $path, $content ) = @_;

    open my $fh, '>', $path or die "unable to write $path: $!";
    print {$fh} $content;
    close $fh;
}

sub write_valid_server {
    my ($path) = @_;

    write_file( $path, <<"SERVER" );
server {
    listen $port;
    server_name localhost;

    root html/v2;
    index index.html;

    error_page 404 = \@fallback;

    location /rewrite {
        rewrite ^ /missing last;
    }

    location /proxy/ {
        proxy_pass http://127.0.0.1:$backend_port/;
    }

    location /try {
        try_files /nope /index.html =404;
    }

    location = /return {
        return 204;
    }

    location \@fallback {
        return 418 "fallback\n";
    }
}
SERVER
}

sub wait_for_file {
    my ($path) = @_;
    for (1 .. 50) {
        return 1 if -f $path;
        sleep 0.1;
    }
    return;
}

sub read_pid {
    my ($path) = @_;
    open my $fh, '<', $path or return;
    my $pid = <$fh>;
    close $fh;
    chomp $pid;
    return $pid;
}

sub http_request {
    my ($port, $path) = @_;
    $path //= '/';

    for (1 .. 20) {
        my $socket = IO::Socket::INET->new(
            PeerAddr => '127.0.0.1',
            PeerPort => $port,
            Proto    => 'tcp'
        );

        if ($socket) {
            print {$socket} "GET $path HTTP/1.0\r\nHost: localhost\r\n\r\n";
            my $response = do { local $/; <$socket> };
            close $socket;

            if (defined $response) {
                my ($status) = $response =~ m{\AHTTP/\d\.\d\s+(\d+)};
                my $body = '';
                if ( $response =~ /\r\n\r\n(.*)\z/s ) {
                    $body = $1;
                }

                return ( $status ? int($status) : 0, $body );
            }
        }

        sleep 0.1;
    }

    BAIL_OUT("failed to fetch HTTP response on port $port");
}

sub http_get {
    my ($port, $path) = @_;

    my ( undef, $body ) = http_request( $port, $path // '/' );

    return $body;
}

sub wait_for_body {
    my ( $port, $path, $expected ) = @_;

    for ( 1 .. 50 ) {
        my $body = http_get( $port, $path );
        return $body if defined $body && $body eq $expected;
        sleep 0.1;
    }

    return http_get( $port, $path );
}

sub log_offset {
    my ($path) = @_;
    return -e $path ? -s $path : 0;
}

sub read_log_tail {
    my ( $path, $offset ) = @_;

    return '' unless -e $path;

    open my $fh, '<', $path or die "unable to read $path: $!";
    seek $fh, $offset, SEEK_SET or die "unable to seek in $path: $!";
    local $/;
    my $content = <$fh>;
    close $fh;

    return defined $content ? $content : '';
}
