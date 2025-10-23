# Single server reload

The `reload_server` signal validates a single HTTP server configuration file and
then requests a normal `reload`. Only after the server file passes validation
does nginx set `ngx_reconfigure`, so any syntax errors are caught before worker
processes are recycled.

## CLI usage

```
nginx -s reload_server=conf.d/example.conf
```

The argument must resolve (after prefix expansion) to a file that contains
exactly one `server { ... }` block. The master process records the normalized
path in `logs/server-reload` before signalling the nginx master PID.

## Runtime flow

1. `ngx_signal_process()` normalizes the target path, writes it to
   `logs/server-reload`, and sends the new signal (`SIGSYS` on Unix) to the
   master process.
2. The master and each worker notice `ngx_reload_server` and call the HTTP
   handler through `ngx_reload_server_handler`.
3. `ngx_http_reload_server_block()` reads the control file, parses the server
   file, enforces the validation rules below, and, on success, raises
   `NGX_RECONFIGURE_SIGNAL` (SIGHUP). The master then performs the standard
   reload path, replacing the runtime cycle and respawning workers.

## Validation rules

The reload handler parses the target file with the standard HTTP configuration engine. The file must resolve to exactly one `server { ... }` block, but inside that block you can use the same directives and nested `location` hierarchy that nginx accepts during a normal reload. Features such as `return`, `rewrite`, `proxy_pass`, `try_files`, and `error_page` are validated in-place before the master process raises `SIGHUP`.

If the parser encounters invalid syntax, nginx logs the error, the `nginx -s reload_server=...` command exits with a non-zero status, and the master skips the reload without touching the running worker processes. When validation fails because the file defines multiple `server` blocks, the master still logs the error and declines to reload, but the signalling process exits successfully—the caller must check the error log to confirm the failure. Because validation runs in isolation, directives that depend on configuration defined elsewhere (for example, shared upstream groups) may still fail when the subsequent full reload rebuilds the complete configuration.

Trailing comments and whitespace are ignored, but every directive must end with `;`. When validation fails, nginx logs the error and aborts the reload request. You can retry the command after fixing the server configuration.

## Example

The configuration under `docs/examples/single-server-reload/` demonstrates a
main `nginx.conf` that includes individual server files from `conf.d/`. The
Perl test `tests/reload_server.t` starts nginx under a private prefix, edits the
server file, runs `nginx -s reload_server=...`, and verifies that the updated
content is served after the full reload completes.
