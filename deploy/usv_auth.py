#!/usr/bin/env python3
"""usv-auth: Signal K login for the dashboard (SK_LOGIN=1 in /etc/usv/usv.env).

nginx sits in front of Grafana on GRAFANA_PORT and asks this service about
every request (auth_request). The service reads the browser's Signal K
login (the JAUTHENTICATION cookie Signal K sets, or the sktoken cookie
Swiftships-UI keeps; cookies are shared across ports on the same host), asks
the Signal K server who it is (GET /skServer/loginStatus with the token, as
the boat's iWebSocket does) and answers with the user name and a Grafana
role. Grafana trusts those headers from nginx only (auth.proxy, whitelist
127.0.0.1).

Boat role, as Swiftships-UI and iWebSocket decide it: the roles file the
Swiftships-UI plugin writes (/var/lib/swiftships-c2/roles.json,
configuration.roles = [{user, role}]), else the Signal K level (readonly
viewer, readwrite operator, admin supervisor). A read-only account is always
a viewer. Grafana role per boat role: SK_ROLE_MAP (default viewer and
planner Viewer, operator and supervisor Editor). The Audit page is open to
Editors and Admins only.

Not logged in to Signal K (or Signal K unreachable, or its security off):
no user header, so Grafana shows its own login form; the Grafana admin
account keeps working as a fallback.

Answers are cached per token for AUTH_CACHE_S (default 10 s): Grafana
issues many requests per page refresh. A user's Grafana role is also set
through Grafana's API when it changes (GRAFANA_ADMIN_PASSWORD), in case the
Grafana build ignores the role header.

Standard library only. Endpoints: GET /auth (nginx), GET /healthz.
"""
import base64
import hashlib
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BOAT_ROLES = ("viewer", "planner", "operator", "supervisor")
LEVEL_ROLE = {"readonly": "viewer", "readwrite": "operator", "admin": "supervisor"}
DEFAULT_ROLE_MAP = "viewer=Viewer,planner=Viewer,operator=Editor,supervisor=Editor"


def env(name, default=""):
    v = os.environ.get(name, "")
    return v if v != "" else default


def parse_role_map(spec):
    """'viewer=Viewer,operator=Editor' -> {'viewer': 'Viewer', ...}."""
    out = {}
    for part in (spec or "").split(","):
        if "=" not in part:
            continue
        k, v = part.split("=", 1)
        k, v = k.strip().lower(), v.strip().capitalize()
        if k in BOAT_ROLES and v in ("Viewer", "Editor", "Admin"):
            out[k] = v
    return out


def token_from_request(cookie_header, authorization):
    """Signal K's own login cookie first, then Swiftships-UI's copy, then a
    bearer token sent by a script."""
    jauth = sktoken = ""
    for item in (cookie_header or "").split(";"):
        if "=" not in item:
            continue
        name, value = item.split("=", 1)
        name, value = name.strip(), value.strip()
        if name == "JAUTHENTICATION":
            jauth = value
        elif name == "sktoken":
            sktoken = value
    if jauth or sktoken:
        return urllib.parse.unquote(jauth or sktoken)
    auth = (authorization or "").strip()
    if auth.lower().startswith("bearer "):
        return auth[7:].strip()
    return ""


def boat_role(user, level, roles_doc):
    """Same rule as iWebSocket::roleFor."""
    level = (level or "").lower()
    if level not in LEVEL_ROLE:
        return ""
    role = LEVEL_ROLE[level]
    try:
        roles = roles_doc.get("configuration", {}).get("roles", [])
    except AttributeError:
        roles = []
    if isinstance(roles, list) and user:
        for r in roles:
            if isinstance(r, dict) and r.get("user") == user:
                mapped = str(r.get("role", "")).lower()
                if mapped in BOAT_ROLES:
                    role = mapped
    if level == "readonly":
        role = "viewer"
    return role


class Checker:
    def __init__(self, signalk, roles_file, role_map, cache_s=10.0, timeout=2.0):
        self.signalk = signalk.rstrip("/")
        self.roles_file = roles_file
        self.role_map = role_map
        self.cache_s = cache_s
        self.timeout = timeout
        self._cache = {}
        self._lock = threading.Lock()

    def roles_doc(self):
        try:
            with open(self.roles_file, "r", encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    def login_status(self, token):
        req = urllib.request.Request(self.signalk + "/skServer/loginStatus")
        req.add_header("Authorization", "Bearer " + token)
        with urllib.request.urlopen(req, timeout=self.timeout) as r:
            return json.loads(r.read().decode("utf-8") or "{}")

    def check(self, token, now=None):
        """-> (user, boat_role, grafana_role); ('', '', '') when not logged in."""
        if not token:
            return ("", "", "")
        now = time.monotonic() if now is None else now
        key = hashlib.sha256(token.encode()).hexdigest()
        with self._lock:
            hit = self._cache.get(key)
            if hit and now - hit[0] < self.cache_s:
                return hit[1]
        result = ("", "", "")
        try:
            st = self.login_status(token)
            if isinstance(st, dict) and st.get("status") == "loggedIn" and st.get("username"):
                user = str(st["username"])
                role = boat_role(user, st.get("userLevel", ""), self.roles_doc())
                if role:
                    result = (user, role, self.role_map.get(role, "Viewer"))
        except (urllib.error.URLError, OSError, ValueError):
            result = ("", "", "")
        with self._lock:
            self._cache[key] = (now, result)
            if len(self._cache) > 2000:
                self._cache.clear()
        return result


class RoleSync:
    """Sets a user's Grafana org role through the API when it changes (in
    case the Grafana build ignores the role header). Best effort."""

    def __init__(self, grafana, admin_user, admin_password):
        self.grafana = grafana.rstrip("/")
        self.auth = "Basic " + base64.b64encode(
            f"{admin_user}:{admin_password}".encode()).decode() if admin_password else ""
        self._done = {}
        self._lock = threading.Lock()

    def _call(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.grafana + path, data=data, method=method)
        req.add_header("Authorization", self.auth)
        if data is not None:
            req.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(req, timeout=3) as r:
            raw = r.read().decode("utf-8")
            return json.loads(raw) if raw else {}

    def ensure(self, user, role):
        if not self.auth or not user or not role:
            return
        with self._lock:
            if self._done.get(user) == role:
                return
        try:
            users = self._call("GET", "/api/org/users/lookup?query=" + urllib.parse.quote(user) + "&limit=10")
            for u in users or []:
                if u.get("login") == user:
                    if u.get("role") != role:
                        self._call("PATCH", f"/api/org/users/{u['userId']}", {"role": role})
                    with self._lock:
                        self._done[user] = role
                    return
        except (urllib.error.URLError, OSError, ValueError, KeyError):
            pass


def make_handler(checker, sync):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path.startswith("/healthz"):
                self.send_response(200)
                self.send_header("Content-Length", "2")
                self.end_headers()
                self.wfile.write(b"ok")
                return
            if not self.path.startswith("/auth"):
                self.send_response(404)
                self.end_headers()
                return
            token = token_from_request(self.headers.get("Cookie", ""),
                                       self.headers.get("Authorization", ""))
            user, boat, grole = checker.check(token)
            self.send_response(200)
            self.send_header("Content-Length", "0")
            if user:
                self.send_header("X-Usv-User", user)
                self.send_header("X-Usv-Role", grole)
                self.send_header("X-Usv-Boat-Role", boat)
            self.end_headers()
            if user and sync:
                threading.Thread(target=sync.ensure, args=(user, grole), daemon=True).start()

        def log_message(self, *a):
            pass

    return Handler


def main():
    port = int(env("USV_AUTH_PORT", "3102"))
    signalk = env("SK_LOGIN_SIGNALK", "http://127.0.0.1:3000")
    roles_file = env("SK_ROLES_FILE", "/var/lib/swiftships-c2/roles.json")
    role_map = parse_role_map(DEFAULT_ROLE_MAP)
    role_map.update(parse_role_map(env("SK_ROLE_MAP", "")))
    checker = Checker(signalk, roles_file, role_map, float(env("AUTH_CACHE_S", "10")))
    sync = None
    gport = env("GRAFANA_INTERNAL_PORT", "3099")
    if env("GRAFANA_ADMIN_PASSWORD"):
        sync = RoleSync(f"http://127.0.0.1:{gport}", env("GRAFANA_ADMIN_USER", "admin"),
                        env("GRAFANA_ADMIN_PASSWORD"))
    srv = ThreadingHTTPServer(("127.0.0.1", port), make_handler(checker, sync))
    print(f"usv-auth: 127.0.0.1:{port}, Signal K {signalk}, roles {roles_file}, "
          f"roles map {role_map}", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
