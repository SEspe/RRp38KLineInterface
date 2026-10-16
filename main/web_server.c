/*
 * web_server.c - web HMI: status, WiFi settings, OTA upload.
 *
 * Handlers adapted from RemoteStart/SlaveWallas (same chip, same IDF).
 * Deliberately read-only towards the K-line: there is no route that arms,
 * inits, sends or scans. Those stay on the USB console (decision 25).
 * GitHub-release install is Track H step 5 and not here yet.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "kline.h"
#include "net.h"
#include "version.h"
#include "web_server.h"

static const char *TAG = "web";

/* ------------------------------------------------------------- HTML -- */

static const char INDEX_HTML[] =
"<!DOCTYPE html><html lang='en'><head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>" FIRMWARE_NAME "</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee}"
".hdr{background:#16213e;padding:12px 16px;display:flex;align-items:center;gap:14px;"
"flex-wrap:wrap;border-bottom:2px solid #e94560}"
".hdr h1{font-size:1.1rem;color:#e94560}.hdr .v{font-size:.75rem;color:#888}"
".tabs{display:flex;background:#16213e;overflow-x:auto}"
".tab{padding:10px 16px;cursor:pointer;color:#aaa;border:none;background:none;"
"font-size:.9rem;border-bottom:3px solid transparent;white-space:nowrap}"
".tab.on{color:#e94560;border-bottom-color:#e94560;font-weight:bold}"
".pane{display:none;padding:16px}.pane.on{display:block}"
".card{background:#16213e;border-radius:8px;padding:14px;margin-bottom:14px}"
".card h3{color:#e94560;font-size:.9rem;margin-bottom:10px}"
".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:8px}"
".row{display:flex;justify-content:space-between;align-items:center;gap:8px;"
"background:#0f3460;border-radius:5px;padding:7px 12px;font-size:.83rem}"
".row b{font-weight:normal;color:#fff;text-align:right}"
".ok{color:#4caf50!important}.bad{color:#e94560!important}.amb{color:#e9b400!important}"
".note{font-size:.78rem;color:#e9b400;margin:8px 0;line-height:1.4}"
".ts{font-size:.7rem;color:#666;margin-top:6px}"
"label{display:block;margin:10px 0 4px;font-size:.85rem}"
"input[type=text],input[type=password]{width:100%;max-width:320px;padding:8px;background:#0f3460;"
"border:1px solid #444;border-radius:5px;color:#eee}"
"input[type=file]{display:block;margin:8px 0;color:#eee}"
".btn{padding:9px 18px;border:none;border-radius:5px;color:#fff;cursor:pointer;"
"font-size:.88rem;margin-top:10px;background:#e94560}"
".btn.s{background:#0f3460;border:1px solid #e94560}"
".nets{max-height:170px;overflow-y:auto;margin:8px 0;max-width:320px}"
".net{background:#0f3460;border-radius:5px;padding:8px 12px;margin-bottom:4px;cursor:pointer;"
"display:flex;justify-content:space-between;font-size:.85rem}"
".net span:last-child{font-size:.72rem;color:#aaa}"
".prog{margin-top:10px;font-size:.85rem;color:#e94560;min-height:1.2em}"
"</style></head><body>"
"<div class='hdr'><h1>RR P38 K-line</h1>"
"<span class='v'>" FIRMWARE_NAME " v" FIRMWARE_VERSION "</span></div>"
"<div class='tabs'>"
"<button class='tab on' onclick='show(\"status\",this)'>Status</button>"
"<button class='tab' onclick='show(\"wifi\",this)'>WiFi</button>"
"<button class='tab' onclick='show(\"ota\",this)'>OTA Update</button>"
"</div>"

"<div id='status' class='pane on'>"
"<div class='card'><h3>K-line</h3><div class='grid' id='kl'>&hellip;</div>"
"<p class='note'>Read-only. Arming, init, send and scan are on the USB console only.</p></div>"
"<div class='card'><h3>Network</h3><div class='grid' id='nw'>&hellip;</div></div>"
"<div class='card'><h3>System</h3><div class='grid' id='sy'>&hellip;</div></div>"
"<div class='ts' id='sts'></div></div>"

"<div id='wifi' class='pane'>"
"<div class='card'><h3>Home WiFi</h3>"
"<p class='note'>Optional. Joining a network with internet lets the board install updates from GitHub. "
"The board's own access point stays on regardless.</p>"
"<button class='btn s' onclick='scan()'>Scan networks</button>"
"<div class='ts' id='scs'></div><div class='nets' id='nets'></div>"
"<form action='/wifi-save' method='POST'>"
"<label>SSID</label><input id='sid' name='ssid' type='text' placeholder='tap a network or type' required>"
"<label>Password</label><input name='pass' type='password' placeholder='blank if open'>"
"<br><button class='btn' type='submit'>Save &amp; reboot</button></form></div>"
"<div class='card'><h3>Access point password</h3>"
"<p class='note'>The password for <b>" WIFI_AP_SSID "</b>. Anyone who has it can open this page and upload firmware. "
"8 to 63 characters.</p>"
"<form action='/ap-save' method='POST'>"
"<label>New password</label><input name='pass' type='password' minlength='8' maxlength='63' required>"
"<br><button class='btn' type='submit'>Save &amp; reboot</button></form></div>"
"<div class='card'><h3>Device</h3>"
"<button class='btn s' onclick='reboot()'>Reboot now</button><div class='ts' id='rbs'></div></div>"
"</div>"

"<div id='ota' class='pane'>"
"<div class='card'><h3>Firmware</h3><div class='grid' id='vr'>&hellip;</div></div>"
"<div class='card'><h3>Upload firmware</h3>"
"<p class='note'>Select a compiled <code>.bin</code>. The K-line transceiver is put to sleep before "
"the reboot. Do not upload while a session is running on the console.</p>"
"<input type='file' id='fw' accept='.bin'>"
"<button class='btn' onclick='upload()'>Upload &amp; flash</button>"
"<div class='prog' id='prog'></div></div></div>"

"<script>"
"function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;');}"
"function row(k,v,c){return '<div class=row><span>'+k+'</span><b class=\"'+(c||'')+'\">'+v+'</b></div>';}"
"function show(id,b){document.querySelectorAll('.pane').forEach(e=>e.classList.remove('on'));"
"document.querySelectorAll('.tab').forEach(e=>e.classList.remove('on'));"
"document.getElementById(id).classList.add('on');b.classList.add('on');"
"if(id==='ota')ver();}"
"function hms(){var d=new Date(),p=n=>(n<10?'0':'')+n;"
"return p(d.getHours())+':'+p(d.getMinutes())+':'+p(d.getSeconds());}"
"function up(s){var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);"
"return (d?d+'d ':'')+h+'h '+m+'m';}"
"function stat(){fetch('/api/status').then(r=>r.json()).then(d=>{var k=d.kline,n=d.net,s=d.sys;"
"document.getElementById('kl').innerHTML="
"row('Build',k.txBuild?'transmit capable':'passive (TX compiled out)',k.txBuild?'amb':'ok')+"
"row('TX armed',k.armed?'YES':'no',k.armed?'bad':'ok')+"
"row('Transport',k.started?'running':'stopped',k.started?'ok':'bad')+"
"row('Baud',k.baud)+row('TX / RX GPIO',k.tx+' / '+k.rx)+"
"row('Transceiver EN',k.en<0?'not used':'GPIO'+k.en)+"
"row('Init bypass',k.drv<0?'none':'GPIO'+k.drv);"
"document.getElementById('nw').innerHTML="
"row('Access point',esc(n.apSsid))+row('Phones on AP',n.apClients)+row('Channel',n.apCh)+"
"row('Home WiFi',n.staSsid?esc(n.staSsid):'not configured')+"
"row('Home link',n.staSsid?(n.staUp?'connected':'not connected'):'\\u2014',n.staUp?'ok':'')+"
"row('Home IP',n.staIp||'\\u2014')+row('RSSI',n.staUp?n.rssi+' dBm':'\\u2014')+"
"row('Link drops',n.drops);"
"document.getElementById('sy').innerHTML="
"row('Uptime',up(s.uptime))+row('Free heap',s.heap)+row('Heap low-water',s.heapMin);"
"document.getElementById('sts').textContent='Updated '+hms();"
"}).catch(()=>{document.getElementById('sts').textContent='No answer at '+hms();});}"
"function ver(){fetch('/api/version').then(r=>r.json()).then(v=>{"
"document.getElementById('vr').innerHTML=row('Installed','v'+esc(v.installed))+"
"row('Stored (other slot)',v.stored?'v'+esc(v.stored).replace(/^v/,''):'\\u2014')+"
"row('Running from',esc(v.running));}).catch(()=>{});}"
"function scan(){var st=document.getElementById('scs');st.textContent='Scanning\\u2026';"
"document.getElementById('nets').innerHTML='';"
"fetch('/api/scan').then(r=>r.json()).then(a=>{"
"st.textContent=a.length+' network'+(a.length!==1?'s':'')+' found';"
"var c=document.getElementById('nets');a.forEach(n=>{var d=document.createElement('div');"
"d.className='net';d.innerHTML='<span>'+esc(n.ssid)+'</span><span>'+n.rssi+' dBm '+(n.auth?'&#128274;':'open')+'</span>';"
"d.onclick=()=>{document.getElementById('sid').value=n.ssid;};c.appendChild(d);});"
"}).catch(()=>{st.textContent='Scan failed \\u2014 try again';});}"
"function reboot(){if(!confirm('Reboot the board now?'))return;"
"document.getElementById('rbs').textContent='Rebooting\\u2026';"
"fetch('/api/reboot',{method:'POST'}).catch(()=>{});}"
"function upload(){var f=document.getElementById('fw').files[0],p=document.getElementById('prog');"
"if(!f){p.textContent='Select a .bin file first';return;}"
"if(!confirm('Flash '+f.name+' and reboot?'))return;"
"var x=new XMLHttpRequest();x.open('POST','/ota/upload',true);"
"x.setRequestHeader('Content-Type','application/octet-stream');"
"x.upload.onprogress=e=>{if(e.lengthComputable)p.textContent='Uploading: '+Math.round(e.loaded/e.total*100)+'%';};"
"x.onload=()=>{p.textContent=x.status===200?'Done. Rebooting \\u2014 reload in about 10 s.':'Error: '+x.status+' '+x.responseText;};"
"x.onerror=()=>{p.textContent='Upload error';};x.send(f);}"
"setInterval(()=>{if(document.getElementById('status').classList.contains('on'))stat();},2000);"
"stat();"
"</script></body></html>";

static const char SAVED_HTML[] =
"<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'></head>"
"<body style='font-family:Arial;background:#1a1a2e;color:#eee;padding:24px'>"
"<h2 style='color:#e94560'>Saved</h2><p style='margin-top:12px'>The board is rebooting to apply it.</p>"
"<p style='margin-top:12px;color:#aaa;font-size:.9rem'>On the board's own access point, reconnect to "
"<b>" WIFI_AP_SSID "</b> and open <a style='color:#4ea8ff' href='http://192.168.4.1/'>192.168.4.1</a>. "
"On home WiFi, try <a style='color:#4ea8ff' href='http://" MDNS_HOSTNAME ".local/'>http://" MDNS_HOSTNAME ".local</a>.</p>"
"</body></html>";

/* ---------------------------------------------------------- helpers -- */

/* Reboot after the response has gone out. */
static void restart_cb(void *arg)
{
    kline_release_bus();
    esp_restart();
}

static void restart_later(uint32_t ms)
{
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t a = { .callback = restart_cb, .name = "restart" };
        if (esp_timer_create(&a, &t) != ESP_OK) {
            restart_cb(NULL);
        }
    }
    esp_timer_start_once(t, (uint64_t)ms * 1000);
}

/* In-place decode of application/x-www-form-urlencoded text. Without it an
 * SSID or password with a space or symbol is saved encoded and never
 * matches. Decoded text never grows. */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        int hi, lo;
        if (*s == '+') {
            *o++ = ' ';
            s++;
        } else if (*s == '%' && (hi = hexval(s[1])) >= 0 && (lo = hexval(s[2])) >= 0) {
            *o++ = (char)(hi * 16 + lo);
            s += 3;
        } else {
            *o++ = *s++;
        }
    }
    *o = '\0';
}

/* Copies form field `key` out of an urlencoded body and decodes it. */
static void form_field(const char *body, const char *key, char *out, size_t len)
{
    out[0] = '\0';
    size_t kl = strlen(key);
    for (const char *p = body; (p = strstr(p, key)) != NULL; p += kl) {
        if ((p == body || p[-1] == '&') && p[kl] == '=') {
            p += kl + 1;
            size_t n = strcspn(p, "&");
            if (n >= len) {
                n = len - 1;
            }
            memcpy(out, p, n);
            out[n] = '\0';
            url_decode(out);
            return;
        }
    }
}

static int read_body(httpd_req_t *req, char *buf, size_t len)
{
    int want = MIN(req->content_len, (int)len - 1);
    int got = 0;
    while (got < want) {
        int r = httpd_req_recv(req, buf + got, want - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return -1;
        }
        got += r;
    }
    buf[got] = '\0';
    return got;
}

/* Escapes " and \ and drops control characters, for a JSON string body. */
static void json_esc(char *dst, size_t dsz, const char *src)
{
    size_t j = 0;
    for (; *src; src++) {
        unsigned char c = (unsigned char)*src;
        if (c < 0x20) {
            continue;
        }
        size_t need = (c == '"' || c == '\\') ? 2 : 1;
        if (j + need >= dsz) {
            break;
        }
        if (need == 2) {
            dst[j++] = '\\';
        }
        dst[j++] = (char)c;
    }
    dst[j] = '\0';
}

/* --------------------------------------------------------- handlers -- */

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, sizeof(INDEX_HTML) - 1);
}

/* GET /api/status - the console's `status` fields plus network and heap. */
static esp_err_t h_status(httpd_req_t *req)
{
    net_status_t n;
    net_get_status(&n);
    char ap[70], sta[70];
    json_esc(ap, sizeof(ap), n.ap_ssid);
    json_esc(sta, sizeof(sta), n.sta_ssid);

    char buf[640];
    snprintf(buf, sizeof(buf),
        "{\"kline\":{\"txBuild\":%s,\"armed\":%s,\"started\":%s,\"baud\":%d,"
        "\"tx\":%d,\"rx\":%d,\"en\":%d,\"drv\":%d},"
        "\"net\":{\"apSsid\":\"%s\",\"apClients\":%d,\"apCh\":%d,\"staSsid\":\"%s\","
        "\"staUp\":%s,\"staIp\":\"%s\",\"rssi\":%d,\"drops\":%lu},"
        "\"sys\":{\"uptime\":%lld,\"heap\":%lu,\"heapMin\":%lu}}",
#if CONFIG_RRP38_ALLOW_TX
        "true",
#else
        "false",
#endif
        kline_tx_armed() ? "true" : "false",
        kline_started() ? "true" : "false",
        CONFIG_RRP38_KLINE_BAUD,
        CONFIG_RRP38_KLINE_TX_GPIO, CONFIG_RRP38_KLINE_RX_GPIO,
        CONFIG_RRP38_EN_GPIO, CONFIG_RRP38_INIT_DRIVER_GPIO,
        ap, n.ap_clients, n.ap_channel, sta,
        n.sta_connected ? "true" : "false", n.sta_ip, n.sta_rssi,
        (unsigned long)n.sta_disconnects,
        (long long)(esp_timer_get_time() / 1000000),
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* Keeps only printable ASCII: the other slot can hold an image built by
 * anything, so its descriptor is not trusted. */
static void ver_sanitize(char *dst, size_t cap, const char *src, size_t src_len)
{
    size_t n = 0;
    for (size_t i = 0; i < src_len && src[i] && n < cap - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') {
            dst[n++] = (char)c;
        }
    }
    dst[n] = '\0';
}

/* GET /api/version - running version, and what sits in the inactive slot
 * (the previous image, still bootable). "stored" is empty if that slot was
 * never written. */
static esp_err_t h_version(httpd_req_t *req)
{
    char stored[40] = "";
    esp_app_desc_t desc;
    const esp_partition_t *nxt = esp_ota_get_next_update_partition(NULL);
    if (nxt && esp_ota_get_partition_description(nxt, &desc) == ESP_OK) {
        ver_sanitize(stored, sizeof(stored), desc.version, sizeof(desc.version));
    }
    const esp_partition_t *run = esp_ota_get_running_partition();

    char buf[200];
    snprintf(buf, sizeof(buf),
             "{\"name\":\"%s\",\"installed\":\"%s\",\"stored\":\"%s\",\"running\":\"%s\"}",
             FIRMWARE_NAME, FIRMWARE_VERSION, stored, run ? run->label : "");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* GET /api/scan - nearby networks for the WiFi tab. */
static esp_err_t h_scan(httpd_req_t *req)
{
    wifi_scan_config_t scfg = {
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 300,
    };
    esp_wifi_scan_start(&scfg, true);

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > 20) {
        count = 20;
    }
    wifi_ap_record_t *recs = count ? malloc(count * sizeof(*recs)) : NULL;
    if (recs) {
        esp_wifi_scan_get_ap_records(&count, recs);
    } else {
        count = 0;
        esp_wifi_clear_ap_list();
    }

    char *buf = malloc(count * 100 + 8);
    if (!buf) {
        free(recs);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "[]");
    }
    size_t n = 0;
    buf[n++] = '[';
    for (int i = 0; i < count; i++) {
        if (!recs[i].ssid[0]) {
            continue;
        }
        char esc[70];
        json_esc(esc, sizeof(esc), (const char *)recs[i].ssid);
        n += snprintf(buf + n, count * 100 + 8 - n, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                      n > 1 ? "," : "", esc, recs[i].rssi, (int)recs[i].authmode);
    }
    buf[n++] = ']';
    buf[n] = '\0';
    free(recs);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, buf);
    free(buf);
    return err;
}

/* POST /wifi-save - home network credentials. */
static esp_err_t h_wifi_save(httpd_req_t *req)
{
    char body[300], ssid[33], pass[65];
    if (read_body(req, body, sizeof(body)) < 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    }
    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "pass", pass, sizeof(pass));
    if (!ssid[0] || net_save_sta(ssid, pass) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "could not save");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, SAVED_HTML);
    ESP_LOGI(TAG, "home WiFi set to \"%s\", rebooting", ssid);
    restart_later(800);
    return ESP_OK;
}

/* POST /ap-save - the access point's own password. */
static esp_err_t h_ap_save(httpd_req_t *req)
{
    char body[200], pass[65];
    if (read_body(req, body, sizeof(body)) < 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    }
    form_field(body, "pass", pass, sizeof(pass));
    if (net_save_ap_pass(pass) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password must be 8 to 63 characters");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, SAVED_HTML);
    ESP_LOGI(TAG, "AP password changed, rebooting");
    restart_later(800);
    return ESP_OK;
}

/* POST /api/reboot */
static esp_err_t h_reboot(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "OK");
    ESP_LOGW(TAG, "reboot requested from the web page");
    restart_later(300);
    return ESP_OK;
}

/* POST /ota/upload - raw .bin, streamed into the inactive slot. */
static esp_err_t h_ota_upload(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
    }
    esp_ota_handle_t ota = 0;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
    }
    ESP_LOGI(TAG, "OTA: receiving %d bytes into %s", req->content_len, part->label);

    char buf[1024];
    int remaining = req->content_len;
    int timeouts = 0;
    bool ok = true;
    while (remaining > 0) {
        int got = httpd_req_recv(req, buf, MIN(remaining, (int)sizeof(buf)));
        if (got <= 0) {
            /* A few timeouts are normal on a slow link; a hard abort must not
             * wedge the server worker. */
            if (got == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) {
                continue;
            }
            ok = false;
            break;
        }
        timeouts = 0;
        if (esp_ota_write(ota, buf, got) != ESP_OK) {
            ok = false;
            break;
        }
        remaining -= got;
    }
    if (!ok) {
        esp_ota_abort(ota);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
    }
    if (esp_ota_end(ota) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "image did not validate");
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not select new image");
    }
    httpd_resp_sendstr(req, "OK");
    ESP_LOGI(TAG, "OTA: image accepted, rebooting into %s", part->label);
    restart_later(500);
    return ESP_OK;
}

/* ------------------------------------------------------------ start -- */

esp_err_t web_server_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;      /* headroom: an exact-fit cap silently drops routes */
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;    /* abandoned connections must not wedge the server */
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return ESP_FAIL;
    }
    static const httpd_uri_t routes[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = h_root      },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = h_status    },
        { .uri = "/api/version", .method = HTTP_GET,  .handler = h_version   },
        { .uri = "/api/scan",    .method = HTTP_GET,  .handler = h_scan      },
        { .uri = "/wifi-save",   .method = HTTP_POST, .handler = h_wifi_save },
        { .uri = "/ap-save",     .method = HTTP_POST, .handler = h_ap_save   },
        { .uri = "/api/reboot",  .method = HTTP_POST, .handler = h_reboot    },
        { .uri = "/ota/upload",  .method = HTTP_POST, .handler = h_ota_upload},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (httpd_register_uri_handler(server, &routes[i]) != ESP_OK) {
            ESP_LOGE(TAG, "failed to register %s", routes[i].uri);
        }
    }
    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}
