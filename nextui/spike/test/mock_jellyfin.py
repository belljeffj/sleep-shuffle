import http.server, json, os, sys, re
MEDIA = os.path.join(os.path.dirname(__file__), "media")
AUDIO_OK = os.environ.get("AUDIO_OK", "1") == "1"
polls = {"n": 0}
log = open(os.path.join(os.path.dirname(__file__), "mock.log"), "a")
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): log.write(self.command + " " + self.path + " auth=" + str(self.headers.get("Authorization"))[:60] + "\n"); log.flush()
    def j(self, obj, code=200):
        b = json.dumps(obj, separators=(",", ":")).encode()
        self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def file(self, name, ctype):
        p = os.path.join(MEDIA, name)
        if not os.path.exists(p): return self.j({}, 404)
        data = open(p, "rb").read()
        self.send_response(200); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data)
    def route(self):
        path = self.path.split("?")[0]
        if path == "/System/Info/Public": return self.j({"Version": "10.10.7", "ServerName": "mock"})
        if path == "/QuickConnect/Initiate": return self.j({"Secret": "sec123", "Code": "482913", "Authenticated": False})
        if path == "/QuickConnect/Connect":
            polls["n"] += 1
            return self.j({"Secret": "sec123", "Code": "482913", "Authenticated": polls["n"] >= 2})
        if path == "/Users/AuthenticateWithQuickConnect":
            n = int(self.headers.get("Content-Length", 0)); body = self.rfile.read(n)
            assert b"sec123" in body
            return self.j({"User": {"Name": "jeff", "ServerId": "srv", "Id": "u1"}, "AccessToken": "tok42", "ServerId": "srv"})
        if path == "/Users/u1": return self.j({"Name": "jeff", "Id": "u1"})
        if path == "/Items": return self.j({"Items": [{"Name": "The \"Pilot\"", "ServerId": "srv", "Id": "ep1", "SeriesName": "Test Show", "ParentIndexNumber": 1, "IndexNumber": 2, "UserData": {"Key": "x"}}], "TotalRecordCount": 1})
        if path == "/Videos/ep1/master.m3u8": return self.file("master.m3u8", "application/vnd.apple.mpegurl")
        m = re.match(r"^/Videos/ep1/(main\.m3u8|seg\d+\.ts)$", path)
        if m: return self.file(m.group(1), "video/mp2t" if m.group(1).endswith(".ts") else "application/vnd.apple.mpegurl")
        if path == "/Audio/ep1/stream.mp3": return self.file("audio.mp3", "audio/mpeg") if AUDIO_OK else self.j({}, 500)
        if path == "/Videos/ActiveEncodings": self.send_response(204); self.end_headers(); return
        return self.j({"err": path}, 404)
    do_GET = do_POST = do_DELETE = route
http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
