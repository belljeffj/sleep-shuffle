"""A tiny stand-in for Jellyfin 12, enough for the desktop self-test of the NextUI app.
Two shows with three episodes each, Quick Connect, intro segments, and the stream endpoints
the app uses. Every episode plays the same 40-second test clip."""
import http.server, json, os, sys, re, urllib.parse
HERE = os.path.dirname(os.path.abspath(__file__))
MEDIA = os.path.join(HERE, "media")
STATE = {"polls": 0, "token": "tok42"}
LOG = open(os.path.join(HERE, "mock.log"), "a")
SHOWS = [("s1", "How I Met Your Mother"), ("s2", "New Girl")]
EPS = {sid: [{"Name": f"Episode {i}", "ServerId": "srv", "Id": f"{sid}e{i}", "SeriesName": name,
              "ParentIndexNumber": 1, "IndexNumber": i, "RunTimeTicks": 40 * 10**7, "LocationType": "FileSystem",
              "UserData": {"PlaybackPositionTicks": 0, "Key": "k", "ItemId": "x"}} for i in range(1, 4)]
       + [{"Name": "A special", "Id": f"{sid}sp", "SeriesName": name, "ParentIndexNumber": 0, "IndexNumber": 1},
          {"Name": "Missing", "Id": f"{sid}miss", "SeriesName": name, "ParentIndexNumber": 2, "IndexNumber": 1,
           "LocationType": "Virtual"}]
       for sid, name in SHOWS}

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def log_message(self, *a):
        q = urllib.parse.urlparse(self.path)
        LOG.write(f"{self.command} {q.path} {q.query[:200]}\n"); LOG.flush()
    def authed(self):
        h = self.headers.get("Authorization") or ""
        q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        if "api_key" in q: return False  # Jellyfin 12 refuses this
        return f'Token="{STATE["token"]}"' in h or q.get("ApiKey") == [STATE["token"]]
    def j(self, obj, code=200):
        b = json.dumps(obj, separators=(",", ":")).encode()
        self.send_response(code); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def file(self, name, ctype):
        data = open(os.path.join(MEDIA, name), "rb").read()
        self.send_response(200); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data)
    def route(self):
        u = urllib.parse.urlparse(self.path); path = u.path
        if not path.startswith("/jellyfin/"): return self.j({}, 404)
        path = path[len("/jellyfin"):]
        if path == "/System/Info/Public": return self.j({"Version": "12.3.0"})
        if path == "/QuickConnect/Initiate": return self.j({"Secret": "sec", "Code": "123456", "Authenticated": False})
        if path == "/QuickConnect/Connect":
            STATE["polls"] += 1; return self.j({"Authenticated": STATE["polls"] >= 2})
        if path == "/Users/AuthenticateWithQuickConnect":
            return self.j({"User": {"Name": "Jeff", "ServerId": "srv", "Id": "u1"}, "AccessToken": STATE["token"]})
        if not self.authed(): return self.j({}, 401)
        if path == "/Users/u1": return self.j({"Name": "Jeff", "Id": "u1"})
        if path == "/Items": return self.j({"Items": [{"Name": n, "Id": i, "Type": "Series"} for i, n in SHOWS]})
        m = re.match(r"^/Shows/(s\d)/Episodes$", path)
        if m: return self.j({"Items": EPS[m.group(1)], "TotalRecordCount": 5})
        m = re.match(r"^/MediaSegments/(\w+)$", path)
        if m: return self.j({"Items": [{"Type": "Intro", "StartTicks": 0, "EndTicks": 20 * 10**7, "ItemId": m.group(1)}]})
        if re.match(r"^/Videos/\w+/stream\.ts$", path): return self.file("full.ts", "video/mp2t")
        if re.match(r"^/Videos/\w+/master\.m3u8$", path): return self.file("master.m3u8", "application/vnd.apple.mpegurl")
        m = re.match(r"^/Videos/\w+/(main\.m3u8|seg\d+\.ts)$", path)
        if m: return self.file(m.group(1), "video/mp2t")
        if re.match(r"^/Audio/\w+/stream\.mp3$", path): return self.file("audio.mp3", "audio/mpeg")
        if path == "/Videos/ActiveEncodings": self.send_response(204); self.end_headers(); return
        return self.j({"err": path}, 404)
    do_GET = do_POST = do_DELETE = route
http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
