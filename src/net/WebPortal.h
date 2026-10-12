#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "bridge/ChronosBridge.h"
#include "glance/GlanceClient.h"
#include "net/Forecast.h"
#include "net/NetTime.h"

// Headless control portal on the LAN (phone-friendly, no JS needed).
//
//   /         status (auto-refreshes; forwards to /enter when a PIN pends)
//   /enter    PIN form (no refresh - typing is never wiped)
//   /weather  location + unit + fetch-now
//   /control  carousel prev/next, hands calibration, clear scenes
//   /wifi     WiFi setup: scan list + credentials form
//
// While NetTime's setup AP is up (headless provisioning), the same server
// also answers on the AP interface: "/" redirects to /wifi, every unknown
// path redirects there too, and a wildcard DNS points all names at the AP
// IP so phones pop the setup page automatically.
//
// Handlers run from portal.handle() on the main task (also called inside
// the PIN wait loop, since the main loop is blocked while pairing).

class WebPortal {
public:
    WebPortal(GlanceClient& glance, ChronosBridge& bridge, NetTime& net, Forecast& forecast)
        : _glance(glance), _bridge(bridge), _net(net), _forecast(forecast) {}

    void begin() {
        // Load the stored mDNS host name (multi-device setups name each
        // bridge differently: `mdns <name>` over serial). Writable open so
        // a fresh NVS gets the namespace created silently.
        _prefs.begin("portal", false);
        _host = _prefs.getString("host", "glancebridge");
        _prefs.end();
    }

    String hostname() const { return _host; }

    // Validate + persist + apply a new mDNS host name (letters, digits,
    // hyphens; no leading/trailing hyphen; 1-32 chars).
    bool setHostname(const String& name) {
        String h = name;
        h.trim();
        h.toLowerCase();
        if (h.length() == 0 || h.length() > 32) {
            return false;
        }
        for (unsigned i = 0; i < h.length(); i++) {
            char c = h[i];
            bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      (c == '-' && i > 0 && i < h.length() - 1);
            if (!ok) {
                return false;
            }
        }
        _host = h;
        _prefs.begin("portal", false);
        _prefs.putString("host", _host);
        _prefs.end();
        if (_running) {
            // Re-announce under the new name; the HTTP server keeps its IP.
            MDNS.end();
            _mdnsUp = false;
            if (WiFi.status() == WL_CONNECTED && MDNS.begin(_host)) {
                MDNS.addService("http", "tcp", 80);
                _mdnsUp = true;
            }
        }
        return true;
    }

    // Service HTTP. Safe to call from any loop. Runs on the LAN (STA)
    // interface and, while the setup AP is up, on the AP interface with
    // captive DNS so phones pop the /wifi page automatically.
    void handle() {
        bool ap = _net.setupActive();
        bool staUp = WiFi.status() == WL_CONNECTED;
        if (!ap && !staUp) {
            _running = false;
            return;
        }
        if (ap) {
            if (!_dnsUp) {
                _dns.start(53, "*", WiFi.softAPIP());
                _dnsUp = true;
                Serial.printf("[portal] captive DNS on %s (setup AP)\n",
                              WiFi.softAPIP().toString().c_str());
            }
            _dns.processNextRequest();
        } else if (_dnsUp) {
            _dns.stop();
            _dnsUp = false;
        }
        if (!_running) {
            startServer();
        }
        // mDNS only makes sense on the LAN side; (re)announce once it's up.
        if (staUp && !_mdnsUp && MDNS.begin(_host)) {
            MDNS.addService("http", "tcp", 80);
            _mdnsUp = true;
            Serial.printf("[portal] mDNS: http://%s.local\n", _host.c_str());
        }
        if (_running) {
            _server.handleClient();
        }
        if (_wifiSubmitPending) {
            // The POST response had time to reach the phone; now the switch
            // (closes the setup AP, starts STA).
            _wifiSubmitPending = false;
            delay(500);
            _net.setCredentials(_pendingSsid, _pendingPass);
        }
    }

    // Mark that a PIN is being requested (status page forwards to the form).
    void setPending(bool pending) { _pending = pending; }

    // True when a PIN was submitted over HTTP since the last poll.
    bool pollWeb(uint32_t& pin) {
        if (!_submitted) {
            return false;
        }
        _submitted = false;
        pin = _webPin;
        return true;
    }

    bool running() const { return _running; }

private:
    // ---------------------------------------------------------------- shell

    void startServer() {
        _server.on("/", [this]() { handleRoot(); });
        _server.on("/enter", [this]() { handleEnter(); });
        _server.on("/pin", [this]() { handlePin(); });
        _server.on("/weather", [this]() { handleWeather(); });
        _server.on("/control", [this]() { handleControl(); });
        _server.on("/wifi", [this]() { handleWifi(); });
        _server.onNotFound([this]() {
            if (_net.setupActive()) {
                // Captive portal: phones/laptops probe arbitrary URLs.
                _server.sendHeader("Location",
                                   "http://" + WiFi.softAPIP().toString() + "/wifi");
            } else {
                _server.sendHeader("Location", "/");
            }
            _server.send(302);
        });
        _server.begin();
        _running = true;
        Serial.printf("[portal] web portal on http://%s/\n",
                      _net.setupActive() ? WiFi.softAPIP().toString().c_str()
                                         : WiFi.localIP().toString().c_str());
    }

    static const char* CSS() {
        return "*{box-sizing:border-box}"
               "body{margin:0;background:#0f1115;color:#e8eaed;"
               "font-family:-apple-system,'Segoe UI',Roboto,Helvetica,sans-serif;"
               "padding-bottom:64px}"
               ".bar{position:sticky;top:0;z-index:2;background:#161a20;"
               "padding:12px 16px;border-bottom:1px solid #2a2f37}"
               ".bar h1{margin:0;font-size:17px;font-weight:600}"
               ".bar .sub{color:#9aa0a6;font-size:12px;margin-top:2px}"
               ".wrap{padding:14px;max-width:520px;margin:0 auto}"
               ".card{background:#1a1e24;border:1px solid #2a2f37;"
               "border-radius:14px;padding:6px 14px;margin-bottom:12px}"
               ".row{display:flex;justify-content:space-between;align-items:center;"
               "padding:10px 0;border-bottom:1px solid #232830;font-size:15px}"
               ".row:last-child{border-bottom:0}"
               ".k{color:#9aa0a6}.v{font-weight:500}"
               ".dot{display:inline-block;width:9px;height:9px;border-radius:50%;"
               "margin-right:7px;vertical-align:1px}"
               ".on{background:#34c759}.off{background:#ff5a5f}"
               "h3{margin:6px 0 10px;font-size:14px;color:#f5b942;"
               "text-transform:uppercase;letter-spacing:1px}"
               ".btn{display:block;text-align:center;padding:13px;border-radius:12px;"
               "background:#232933;color:#e8eaed;text-decoration:none;margin-bottom:10px;"
               "font-size:15px;border:1px solid #2a2f37}"
               ".btn:active{background:#2c333f}"
               ".btn.acc{background:#f5b942;border-color:#f5b942;color:#141414;"
               "font-weight:600}"
               ".btn.acc:active{background:#e0a838}"
               ".btn.warn{background:#2a1c20;border-color:#5a3038;color:#ff7a7f}"
               "input[type=text],input[type=password]{width:100%;padding:12px;"
               "border-radius:10px;"
               "border:1px solid #2a2f37;background:#12151a;color:#e8eaed;"
               "font-size:16px;margin:4px 0 10px}"
               ".pinin{font-size:30px;text-align:center;letter-spacing:10px;"
               "font-weight:600}"
               "label{display:block;color:#9aa0a6;font-size:13px;margin-top:8px}"
               ".radio{display:flex;gap:10px;margin:6px 0 12px}"
               ".radio label{flex:1;text-align:center;padding:11px;border-radius:10px;"
               "border:1px solid #2a2f37;background:#12151a;color:#9aa0a6;"
               "margin:0;font-size:15px}"
               ".radio input{display:none}"
               ".radio input:checked+span{color:#f5b942;font-weight:600}"
               ".nav{position:fixed;bottom:0;left:0;right:0;display:flex;"
               "background:#161a20;border-top:1px solid #2a2f37;"
               "padding:6px 8px calc(6px + env(safe-area-inset-bottom))}"
               ".nav a{flex:1;text-align:center;padding:10px 0;"
               "color:#9aa0a6;text-decoration:none;font-size:13px;border-radius:10px}"
               ".nav a.on{background:#f5b942;color:#0f1115;font-weight:600}"
               ".note{color:#9aa0a6;font-size:13px;line-height:1.5}";
    }

    // Full app shell: header, body, bottom tab nav.
    void page(const char* title, const char* tab, const String& extraMeta,
              const String& body) {
        String html = "<!DOCTYPE html><html><head><meta charset=utf-8>"
                      "<meta name=viewport content='width=device-width,initial-scale=1'>"
                      "<meta name=theme-color content='#161a20'>"
                      "<title>" + String(title) + " - GlanceBridge</title>";
        html += "<style>" + String(CSS()) + "</style>" + extraMeta + "</head><body>";
        // On the setup AP the mDNS name doesn't resolve - show the raw IP.
        String sub = _net.setupActive() ? String("setup &middot; ") + WiFi.softAPIP().toString()
                                        : _host + ".local";
        html += "<div class=bar><h1>GlanceBridge</h1><div class=sub>" + sub + "</div></div>";
        html += "<div class=wrap>" + body + "</div>";
        html += "<div class=nav>";
        if (_net.setupActive()) {
            // Provisioning: only WiFi setup matters here.
            html += navLink("/wifi", "Wi-Fi", tab, "wifi");
        } else {
            html += navLink("/", "Status", tab, "status");
            html += navLink("/weather", "Weather", tab, "weather");
            html += navLink("/control", "Controls", tab, "control");
            html += navLink("/wifi", "Wi-Fi", tab, "wifi");
        }
        html += "</div></body></html>";
        _server.send(200, "text/html", html);
    }

    static String navLink(const char* href, const char* label, const char* tab,
                          const char* name) {
        String s = "<a href='" + String(href) + "'";
        if (strcmp(tab, name) == 0) {
            s += " class=on";
        }
        s += ">" + String(label) + "</a>";
        return s;
    }

    static String dot(bool on) {
        return String("<span class=dot ") + (on ? "on" : "off") + "></span>";
    }

    // ---------------------------------------------------------------- pages

    void handleRoot() {
        if (_net.setupActive()) {
            // Setup AP: the only thing that matters is WiFi provisioning.
            _server.sendHeader("Location", "/wifi");
            _server.send(302);
            return;
        }
        String body = "<div class=card>";
        body += row(dot(_glance.isConnected()), "Clock",
                    _glance.isConnected() ? "connected" : "not connected");
        if (_glance.lastBattery() >= 0) {
            body += row("", "Clock battery", String(_glance.lastBattery()) + "%");
        }
        body += row(dot(_net.isConnected()), "WiFi", _net.isConnected() ? "up" : "down");
        body += row(dot(_net.timeValid()), "NTP time", _net.timeValid() ? "synced" : "no");
        body += row(dot(_bridge.phoneConnected()), "Phone",
                    _bridge.phoneConnected() ? "connected" : "no");
        String fw = _glance.firmwareVersion();
        body += row("", "Firmware", fw.length() ? fw : String("unknown"));
        body += "</div>";

        String weather = _forecast.hasLocation()
                             ? (_forecast.location() + "<br><span class=mut>&deg;" +
                                (_forecast.unit() == "f" ? "F" : "C") + "</span>")
                             : "<span class=mut>not set</span>";
        body += "<div class=card>" + row("", "Weather", weather) + "</div>";
        body += "<div class=card><div class=note>Portal: http://" + _host +
                ".local &middot; PIN form opens here automatically when the "
                "clock asks for one.</div></div>";

        String meta = "<meta http-equiv=refresh content=2>";
        if (_pending) {
            // Proper redirect syntax (content='0; url=...'): an unquoted
            // "content=0 URL=/enter" parses as "reload every 0 s" and the
            // page just hammer-refreshes instead of navigating.
            meta = "<meta http-equiv=refresh content='0; url=/enter'>";
            body = "<div class=card><h3>Pairing</h3><p>The clock is showing a "
                   "PIN on its display.</p></div>";
            page("Pair", "pair", meta, body);
            return;
        }
        page("GlanceBridge", "status", meta, body);
    }

    static String row(const String& dotHtml, const String& key, const String& value) {
        return "<div class=row><span class=k>" + dotHtml + key + "</span><span class=v>" +
               value + "</span></div>";
    }

    // PIN entry form: no auto-refresh (typing must never be wiped).
    void handleEnter() {
        String body = "<div class=card><h3>Pair &mdash; enter PIN</h3>"
                      "<p class=note>Type the digits currently shown on the "
                      "clock:</p>"
                      "<form method=POST action=/pin>"
                      "<input class=pinin type=text name=pin inputmode=numeric "
                      "pattern='[0-9]{1,6}' maxlength=6 autocomplete=off autofocus>"
                      "<button class='btn acc' style='width:100%'>Pair</button>"
                      "</form></div>"
                      "<div class=card><div class=note>Valid ~25 s after the "
                      "PIN appears. If pairing fails the clock shows a fresh "
                      "PIN &mdash; just type the new one.</div></div>";
        page("GlanceBridge - PIN", "status", "", body);
    }

    void handlePin() {
        if (_server.method() != HTTP_POST) {
            _server.sendHeader("Location", "/enter");
            _server.send(302);
            return;
        }
        String pin = _server.arg("pin");
        pin.trim();
        bool digits = pin.length() >= 1 && pin.length() <= 6;
        for (unsigned i = 0; digits && i < pin.length(); i++) {
            if (!isdigit((unsigned char)pin[i])) {
                digits = false;
            }
        }
        if (digits) {
            _webPin = (uint32_t)pin.toInt();
            _submitted = true;
            _server.send(200, "text/html",
                         "<!DOCTYPE html><html><head><meta charset=utf-8>"
                         "<meta name=viewport content='width=device-width,"
                         "initial-scale=1'>"
                         "<meta http-equiv=refresh content='2; url=/'>"
                         "<style>body{background:#0f1115;color:#e8eaed;"
                         "font-family:sans-serif;display:flex;min-height:100vh;"
                         "align-items:center;justify-content:center}</style>"
                         "</head><body><p>Pairing&hellip;</p></body></html>");
        } else {
            _server.sendHeader("Location", "/enter");
            _server.send(302);
        }
    }

    void handleWeather() {
        if (_server.method() == HTTP_POST) {
            String lat = _server.arg("lat");
            String lon = _server.arg("lon");
            String unit = _server.arg("unit");
            String prov = _server.arg("prov");
            String action = _server.arg("action");
            if (prov.length()) {
                _forecast.setProvider(prov);  // schedules an immediate refetch
            }
            if (lat.length() && lon.length()) {
                _forecast.setLocation(lat, lon);
            }
            if (unit == "c" || unit == "f") {
                _forecast.setUnit(unit);
            }
            if (action == "fetch") {
                _forecast.fetch(nullptr);  // uses the (possibly just set) unit
            }
            _server.sendHeader("Location", "/weather");
            _server.send(302);
            return;
        }
        bool f = _forecast.unit() == "f";
        bool nws = _forecast.provider() == "nws";
        String body = "<div class=card><h3>Location</h3>"
                      "<form method=POST action=/weather>"
                      "<label>Latitude</label>"
                      "<input type=text name=lat value='" + _forecast.lat() + "'>"
                      "<label>Longitude</label>"
                      "<input type=text name=lon value='" + _forecast.lon() + "'>";
        body += "<label>Provider</label><div class=radio>"
                "<label><input type=radio name=prov value=om";
        body += nws ? ">" : " checked>";
        body += "<span>Open-Meteo</span></label>"
                "<label><input type=radio name=prov value=nws";
        body += nws ? " checked>" : ">";
        body += "<span>NWS (US)</span></label></div>";
        body += "<label>Unit</label><div class=radio>"
                "<label><input type=radio name=unit value=f";
        body += f ? " checked" : "";
        body += "><span>&deg;F</span></label>"
                 "<label><input type=radio name=unit value=c";
        body += f ? "><span>&deg;C</span>" : " checked><span>&deg;C</span>";
        body += "</label></div>";
        body += "<button class='btn acc' name=action value=save>Save</button>"
                "<button class=btn name=action value=fetch>Fetch now</button>"
                "</form></div>";
        body += "<div class=card><div class=note>Coordinates are decimal "
                "degrees (e.g. 40.586 -98.389). Saving also triggers a "
                "weather refresh; the rings (temp/rain/wind/humidity) "
                "auto-refresh every 30 min and stay in the carousel. "
                "Open-Meteo covers the whole world; the National Weather "
                "Service is US-only but often more accurate there.</div></div>";
        page("GlanceBridge - Weather", "weather", "", body);
    }

    void handleControl() {
        String msg;
        // Brightness comes as a POST form; ops are GET links.
        if (_server.method() == HTTP_POST && _server.hasArg("brightness")) {
            long b = _server.arg("brightness").toInt();
            if (b >= 0 && b <= 255) {
                Settings s = GlanceClient::completeSettings(_glance.lastSettings());
                s.displayBrightness = (int)b;
                bool ok = _glance.writeSettings(&s);
                msg = ok ? ("Brightness " + String((int)b)) : String("Write failed");
            } else {
                msg = "Invalid brightness";
            }
            _server.sendHeader("Location", "/control?done=" + msg);
            _server.send(302);
            return;
        }
        if (_server.hasArg("op")) {
            String op = _server.arg("op");
            if (op == "prev") {
                const uint8_t c = Glance::Cmd::ScenesStop;  // previous face
                msg = _glance.sendCommand(&c, 1) ? "Previous face" : "Failed";
            } else if (op == "next") {
                const uint8_t c = Glance::Cmd::ScenesStart;  // next face
                msg = _glance.sendCommand(&c, 1) ? "Next face" : "Failed";
            } else if (op == "calib") {
                const uint8_t c = Glance::Cmd::StartCalibration;
                msg = _glance.sendCommand(&c, 1) ? "Calibration started" : "Failed";
            } else if (op == "calok") {
                const uint8_t c = Glance::Cmd::ConfirmCalibration;
                msg = _glance.sendCommand(&c, 1) ? "Calibration confirmed" : "Failed";
            } else if (op == "night-on" || op == "night-off") {
                Settings s = GlanceClient::completeSettings(_glance.lastSettings());
                s.nightModeEnabled = (op == "night-on");
                bool ok = _glance.writeSettings(&s);
                msg = ok ? String("Night mode ") + (s.nightModeEnabled ? "on" : "off")
                         : String("Write failed");
            } else if (op == "clear") {
                msg = _glance.sendCommand(Glance::Cmd::ScenesClear,
                                          Glance::ScenePriority::BandSystem)
                          ? "Scenes cleared"
                          : "Failed";
            }
            // Redirect so a browser refresh cannot re-trigger the op.
            _server.sendHeader("Location", "/control?done=" + msg);
            _server.send(302);
            return;
        }
        String body = "<div class=card><h3>Carousel</h3>";
        if (_server.hasArg("done")) {
            body += "<p><b>" + _server.arg("done") + "</b></p>";
        }
        if (!_glance.isConnected()) {
            body += "<p class=note>clock not connected</p>";
        }
        body += "<a class=btn href='/control?op=prev'>&#9664;&nbsp; Previous face</a>"
                "<a class=btn href='/control?op=next'>Next face &nbsp;&#9654;</a>"
                "</div>";
        // Settings (read-modify-write; values reflect the last settings read).
        const Settings& ls = _glance.lastSettings();
        int brightness = ls.has_displayBrightness ? ls.displayBrightness : 128;
        body += "<div class=card><h3>Brightness</h3>"
                "<p class=note>current: " + String(brightness) + "</p>"
                "<form method=POST action=/control>"
                "<input type=range name=brightness min=0 max=255 value=" +
                String(brightness) + " style='width:100%'>"
                "<button class='btn acc' style='width:100%'>Apply</button>"
                "</form></div>";
        bool night = ls.has_nightModeEnabled ? ls.nightModeEnabled : true;
        body += "<div class=card><h3>Night mode</h3><p>";
        body += ls.has_nightModeEnabled ? (night ? "on" : "off") : "unknown";
        body += "</p><a class=btn href='/control?op=night-";
        body += night ? "off'>Turn off" : "on'>Turn on";
        body += "</a><a class=btn href='/control?op=";
        body += night ? "night-on'>Turn on" : "night-off'>Turn off";
        body += "</a></div>";
        body += "<div class=card><h3>Hands calibration</h3>"
                "<a class=btn href='/control?op=calib'>Start calibration</a>"
                "<a class=btn href='/control?op=calok'>Confirm calibration</a>"
                "</div>";
        body += "<div class=card><h3>Maintenance</h3>"
                "<a class='btn warn' href='/control?op=clear'>Clear all scenes</a>"
                "</div>";
        page("GlanceBridge - Controls", "control", "", body);
    }

    // WiFi setup: scan list + credentials form. Served on the setup AP
    // (headless provisioning) and on the LAN (network change without serial).
    void handleWifi() {
        if (_server.method() == HTTP_POST) {
            String ssid = _server.arg("ssid");
            String pass = _server.arg("pass");
            ssid.trim();
            if (ssid.length() == 0) {
                _server.sendHeader("Location", "/wifi");
                _server.send(302);
                return;
            }
            if (_net.setupActive()) {
                // Defer the switch until the response has gone out: closing
                // the setup AP kills the phone's connection mid-response.
                // setCredentials() closes the AP first, then connects.
                _pendingSsid = ssid;
                _pendingPass = pass;
                _wifiSubmitPending = true;
                _server.send(200, "text/html",
                             "<!DOCTYPE html><html><head><meta charset=utf-8>"
                             "<meta name=viewport content='width=device-width,"
                             "initial-scale=1'>"
                             "<style>body{background:#0f1115;color:#e8eaed;"
                             "font-family:sans-serif;display:flex;min-height:100vh;"
                             "align-items:center;justify-content:center;"
                             "text-align:center;padding:20px}</style></head><body>"
                             "<p>Saved - switching to " + htmlEsc(ssid) + ".</p>"
                             "<p><small>This setup network is closing now. "
                             "Reconnect your phone to your own Wi-Fi, then open "
                             "http://" + _host + ".local.<br>If the bridge "
                             "cannot connect, this setup network reappears in "
                             "about 3 minutes.</small></p></body></html>");
            } else {
                _net.setCredentials(ssid, pass);
                _server.sendHeader("Location", "/wifi?done=1");
                _server.send(302);
            }
            return;
        }

        // Scanning is EXPLICIT and ASYNC: a scan hops channels, which kicks
        // phones off the setup AP - so the first page load (captive portal)
        // must never trigger one. An in-progress scan renders a
        // self-refreshing page instead of blocking the server.
        int sc = WiFi.scanComplete();
        if (_server.hasArg("scan") && sc != WIFI_SCAN_RUNNING) {
            WiFi.scanDelete();
            WiFi.scanNetworks(true);  // async
            sc = WIFI_SCAN_RUNNING;
        }
        String body = "<div class=card><h3>Networks</h3>";
        if (_server.hasArg("done")) {
            body += "<p><b>Credentials saved - connecting to " + htmlEsc(_net.ssid()) +
                    "</b></p>";
        }
        if (sc == WIFI_SCAN_RUNNING) {
            body += "<p class=note>Scanning&hellip;</p></div>";
            page("GlanceBridge - Wi-Fi", "wifi",
                 "<meta http-equiv=refresh content=3>", body);
            return;
        }
        if (sc < 0) {
            // Never scanned (or failed): form-first, scan on request.
            body += "<p class=note>Tap the button to look for nearby "
                    "networks, or enter the name manually below.</p>"
                    "<a class=btn href='/wifi?scan=1'>Scan for networks</a></div>";
        } else {
            int shown = 0;
            for (int i = 0; i < sc && shown < 12; i++) {
                String s = WiFi.SSID(i);
                if (s.length() == 0) continue;  // hidden: use the manual form
                // Links carry the scan index (no URL-encoding of SSIDs).
                body += "<a class=btn href='/wifi?n=" + String(i) + "'>";
                if (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) {
                    body += "&#128274; ";
                }
                body += htmlEsc(s) + "<span class=k>&nbsp;&nbsp;" +
                        String(WiFi.RSSI(i)) + " dBm</span></a>";
                shown++;
            }
            if (shown == 0) {
                body += "<p class=note>No networks with a visible name.</p>";
            }
            body += "<a class=btn href='/wifi?scan=1'>Rescan</a></div>";
        }

        // Prefill from a tapped network, else blank for manual entry
        // (hidden SSIDs).
        String sel;
        long n = _server.hasArg("n") ? _server.arg("n").toInt() : -1;
        if (n >= 0 && n < sc) {
            sel = htmlEsc(WiFi.SSID((int)n));
        }
        body += "<div class=card><h3>Connect</h3>"
                "<form method=POST action=/wifi>"
                "<label>Network name (SSID)</label>"
                "<input type=text name=ssid value='" + sel +
                "' autocomplete=off autocapitalize=none>"
                "<label>Password</label>"
                "<input type=password name=pass autocomplete=off>"
                "<button class='btn acc' style='width:100%'>Connect</button>"
                "</form></div>";
        if (_net.ssid().length()) {
            body += "<div class=card><div class=note>Stored network: " +
                    htmlEsc(_net.ssid()) + "</div></div>";
        }
        body += "<div class=card><div class=note>" +
                String(_net.setupActive()
                           ? "The bridge is broadcasting this setup network "
                             "because it could not reach Wi-Fi. Saving switches "
                             "it to your network; the portal then closes."
                           : "Saving reconnects the bridge immediately; NTP time "
                             "re-syncs once the link is up.") +
                "</div></div>";
        page("GlanceBridge - Wi-Fi", "wifi", "", body);
    }

    static String htmlEsc(const String& s) {
        String o;
        o.reserve(s.length());
        for (unsigned i = 0; i < s.length(); i++) {
            char c = s[i];
            if (c == '&') o += "&amp;";
            else if (c == '<') o += "&lt;";
            else if (c == '>') o += "&gt;";
            else if (c == '"') o += "&quot;";
            else if (c == '\'') o += "&#39;";
            else o += c;
        }
        return o;
    }

    // ------------------------------------------------------------------ pin

    GlanceClient& _glance;
    ChronosBridge& _bridge;
    NetTime& _net;
    Forecast& _forecast;
    Preferences _prefs;
    String _host = "glancebridge";
    WebServer _server{80};
    DNSServer _dns;
    volatile bool _pending = false;
    volatile bool _submitted = false;
    volatile uint32_t _webPin = 0;
    bool _running = false;
    bool _dnsUp = false;
    bool _mdnsUp = false;
    bool _wifiSubmitPending = false;
    String _pendingSsid;
    String _pendingPass;
};
