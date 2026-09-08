"""Run the native curl transport against a loopback HTTP echo server."""
import http.server
import subprocess
import sys
import threading
import tempfile
import ssl
from pathlib import Path


class Handler(http.server.BaseHTTPRequestHandler):
    def respond(self):
        size = int(self.headers.get("Content-Length", "0"))
        if size > 1024 * 1024:
            self.send_error(413)
            return
        data = self.command.encode() + b"\n" + self.rfile.read(size)
        try:
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass

    do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = respond

    def log_message(self, *_args):
        pass


with tempfile.TemporaryDirectory(prefix="msime-curl-test-") as directory:
    cert = str(Path(directory) / "cert.pem")
    key = str(Path(directory) / "key.pem")
    subprocess.run([
        "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
        "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1",
        "-keyout", key, "-out", cert,
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            result = subprocess.run(
                [sys.argv[1], f"https://127.0.0.1:{server.server_port}/echo", cert],
                check=False, timeout=60,
            )
        finally:
            server.shutdown()
            thread.join()
sys.exit(result.returncode)
