"""Mount the local DSH browser application beneath the result service origin.

DSH's browser build assumes an origin root. Its HTML resource and boot module
URLs are rebased here; a small, early browser bridge rebases fetch, WebSocket
and EventSource requests without changing DSH's internal RPC channel names.
"""
from __future__ import annotations

import http.client
import logging
import re
import select
import socket
from http.server import BaseHTTPRequestHandler
from urllib.parse import urlsplit

DSH_PROXY_PATH = "/ai/dsh/"
LOG = logging.getLogger("result_service.dsh_proxy")
_HOP_HEADERS = {
    "connection", "keep-alive", "proxy-authenticate", "proxy-authorization",
    "te", "trailer", "transfer-encoding", "upgrade",
}


def http_origin(value: str, *, referer: bool = False) -> tuple[str, str, int] | None:
    """Normalize the direct HTTP console origin for validation and proxying."""
    try:
        parsed = urlsplit(value)
        if (parsed.scheme != "http" or not parsed.hostname or parsed.username is not None
                or parsed.password is not None or any(char.isspace() for char in value)
                or (not referer and (parsed.path not in {"", "/"} or parsed.query or parsed.fragment))):
            return None
        return parsed.scheme, parsed.hostname.lower(), parsed.port if parsed.port is not None else 80
    except ValueError:
        return None

# Installed DSH versions use HTTP RPC, WebSocket events, and an SSE HMR stream.
# Keep the bridge in the iframe only: the parent application's /api is untouched.
_BROWSER_BRIDGE = r"""<script>
(() => {
  const prefix = "/ai/dsh";
  // HTTP LAN origins lack randomUUID, but retain cryptographic random bytes.
  // DSH uses UUIDs for its RPC and message identifiers.
  if (!crypto.randomUUID) crypto.randomUUID = () => {
    const bytes = crypto.getRandomValues(new Uint8Array(16));
    bytes[6] = (bytes[6] & 15) | 64;
    bytes[8] = (bytes[8] & 63) | 128;
    const hex = Array.from(bytes, value => value.toString(16).padStart(2, "0")).join("");
    return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
  };
  const rebase = value => {
    const url = new URL(String(value), location.href);
    if (url.host === location.host && /^(https?|wss?):$/.test(url.protocol)
        && !url.pathname.startsWith("/reports/")
        && url.pathname !== prefix && !url.pathname.startsWith(prefix + "/")) {
      url.pathname = prefix + url.pathname;
    }
    return url.href;
  };
  const fetch = window.fetch.bind(window);
  window.fetch = (input, init) => fetch(input instanceof Request
    ? new Request(rebase(input.url), input) : rebase(input), init);
  for (const name of ["WebSocket", "EventSource"]) {
    const Original = window[name];
    if (Original) window[name] = class extends Original {
      constructor(url, ...args) { super(rebase(url), ...args); }
    };
  }
  // Dynamic module preloads and HMR script elements also use root URLs.
  for (const [name, property] of [["HTMLScriptElement", "src"],
      ["HTMLLinkElement", "href"], ["HTMLImageElement", "src"],
      ["HTMLAnchorElement", "href"]]) {
    const prototype = window[name].prototype;
    const descriptor = Object.getOwnPropertyDescriptor(prototype, property);
    Object.defineProperty(prototype, property, {
      ...descriptor, set(value) { descriptor.set.call(this, rebase(value)); }
    });
  }
})();
</script>"""


def _rebase_html(body: bytes) -> bytes:
    html = body.decode("utf-8")
    # Static entry resources and the module graph injected as __DSH_BOOT__.
    html = re.sub(r'((?:src|href)\s*=\s*[\"\x27])/(?!/)',
                  lambda match: match[1] + DSH_PROXY_PATH, html,
                  flags=re.IGNORECASE)
    html = re.sub(r'("url"\s*:\s*")/(?!/)',
                  lambda match: match[1] + DSH_PROXY_PATH, html)
    opening_head = re.search(r"<head(?:\s[^>]*)?>", html, flags=re.IGNORECASE)
    at = opening_head.end() if opening_head else 0
    return (html[:at] + _BROWSER_BRIDGE + html[at:]).encode("utf-8")


def _upstream_headers(handler: BaseHTTPRequestHandler, authority: str,
                      *, websocket: bool = False) -> dict[str, str]:
    connection_headers = {
        value.strip().lower() for value in handler.headers.get("Connection", "").split(",")
    }
    excluded = _HOP_HEADERS | connection_headers | {"host", "accept-encoding"}
    headers = {key: value for key, value in handler.headers.items()
               if key.lower() not in excluded}
    headers["Host"] = authority
    headers["Accept-Encoding"] = "identity"
    # DSH checks same-origin against its own loopback Host. Translate a request
    # from this service's origin while preserving rejection of cross-site ones.
    origin = handler.headers.get("Origin")
    if origin and http_origin(origin) is not None and http_origin(origin) == http_origin(
            "http://" + handler.headers.get("Host", "")):
        headers["Origin"] = f"http://{authority}"
    if websocket:
        headers["Connection"] = "Upgrade"
        headers["Upgrade"] = "websocket"
    else:
        headers["Connection"] = "close"
    return headers


def _location(value: str, authority: str) -> str:
    parsed = urlsplit(value)
    if value.startswith("/") and not value.startswith("//"):
        return DSH_PROXY_PATH.rstrip("/") + value
    if parsed.netloc == authority:
        return DSH_PROXY_PATH.rstrip("/") + parsed.path + (
            f"?{parsed.query}" if parsed.query else ""
        ) + (f"#{parsed.fragment}" if parsed.fragment else "")
    return value


def proxy_request(handler: BaseHTTPRequestHandler, upstream_url: str) -> None:
    """Relay one namespaced request to the managed loopback DSH process."""
    upstream = urlsplit(upstream_url)
    if upstream.hostname != "127.0.0.1" or upstream.scheme != "http":
        raise ValueError("DSH proxy requires the managed loopback HTTP endpoint")
    path = "/" + handler.path[len(DSH_PROXY_PATH):]
    headers = _upstream_headers(handler, upstream.netloc,
                                websocket=handler.headers.get("Upgrade", "").lower() == "websocket")
    if handler.headers.get("Upgrade", "").lower() == "websocket":
        _proxy_websocket(handler, upstream.port or 80, path, headers)
        return
    connection = http.client.HTTPConnection("127.0.0.1", upstream.port, timeout=15)
    sent_headers = False
    try:
        connection.putrequest(handler.command, path, skip_host=True, skip_accept_encoding=True)
        for name, value in headers.items():
            connection.putheader(name, value)
        connection.endheaders()
        remaining = int(handler.headers.get("Content-Length", "0"))
        while remaining > 0:
            chunk = handler.rfile.read(min(65536, remaining))
            if not chunk:
                raise ConnectionError("incomplete DSH request body")
            connection.send(chunk)
            remaining -= len(chunk)
        upstream_socket = connection.sock
        response = connection.getresponse()
        content_type = response.getheader("Content-Type", "")
        is_html = content_type.split(";", 1)[0].strip() == "text/html"
        body = _rebase_html(response.read()) if is_html and handler.command != "HEAD" else None
        handler.send_response(response.status, response.reason)
        for name, value in response.getheaders():
            lower = name.lower()
            if lower in _HOP_HEADERS or lower in {"server", "date"}:
                continue
            if is_html and lower in {"content-length", "etag", "content-encoding"}:
                continue
            if lower == "location":
                value = _location(value, upstream.netloc)
            if lower == "set-cookie":
                value = re.sub(r"(?i)(;\s*Path=)/", r"\g<1>" + DSH_PROXY_PATH, value)
            handler.send_header(name, value)
        if body is not None:
            handler.send_header("Content-Length", str(len(body)))
        handler.send_header("Connection", "close")
        handler.end_headers()
        sent_headers = True
        handler.close_connection = True
        if body is not None:
            handler.wfile.write(body)
        elif handler.command != "HEAD":
            # read1 yields available bytes, so SSE events are forwarded as they
            # arrive, rather than waiting for a full buffer or end of response.
            if upstream_socket is not None:
                upstream_socket.settimeout(None)
            while chunk := response.read1(65536):
                handler.wfile.write(chunk)
                handler.wfile.flush()
    except (OSError, http.client.HTTPException) as exc:
        if not sent_headers:
            raise OSError(f"could not contact DSH Web: {exc}") from exc
        LOG.debug("DSH downstream disconnected", exc_info=True)
    finally:
        connection.close()


def _proxy_websocket(handler: BaseHTTPRequestHandler, port: int, path: str,
                     headers: dict[str, str]) -> None:
    # Negotiate using raw sockets so frames following the 101 headers are not
    # lost in an HTTPResponse buffer. The frame protocol itself stays in DSH.
    with socket.create_connection(("127.0.0.1", port), timeout=15) as upstream:
        request = f"GET {path} HTTP/1.1\r\n" + "".join(
            f"{name}: {value}\r\n" for name, value in headers.items()
        ) + "\r\n"
        upstream.sendall(request.encode("latin-1"))
        response = bytearray()
        while b"\r\n\r\n" not in response:
            chunk = upstream.recv(4096)
            if not chunk:
                raise ConnectionError("DSH closed the WebSocket handshake")
            response.extend(chunk)
            if len(response) > 65536:
                raise ValueError("DSH WebSocket response headers are too large")
        handler.wfile.write(response)
        handler.wfile.flush()
        handler.close_connection = True
        if response.split(b"\r\n", 1)[0].split()[1] != b"101":
            return
        upstream.settimeout(None)
        downstream = handler.connection
        try:
            while True:
                readable, _, _ = select.select([upstream, downstream], [], [])
                for source in readable:
                    chunk = source.recv(65536)
                    if not chunk:
                        return
                    (downstream if source is upstream else upstream).sendall(chunk)
        except OSError:
            LOG.debug("DSH WebSocket disconnected", exc_info=True)
