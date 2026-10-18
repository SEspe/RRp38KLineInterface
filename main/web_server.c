/*
 * web_server.c - web HMI: status, WiFi settings, OTA upload.
 *
 * Handlers adapted from RemoteStart/SlaveWallas (same chip, same IDF).
 * Deliberately read-only towards the K-line: there is no route that arms,
 * inits, sends or scans. Those stay on the USB console (decision 25). The
 * K-line actions here are starting and stopping a passive sniff, and taking
 * a waveform sample, which only listens to a pin.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "kline.h"
#include "net.h"
#include "sniff.h"
#include "version.h"
#include "waveform.h"
#include "web_server.h"

static const char *TAG = "web";

/* ---------------------------------------------------- GitHub install --
 * Every release carries two app images, named by .github/workflows/
 * release.yml as RRp38KLine_<variant>_v<version>.bin: "passive" (TX compiled
 * out) and "tx". This build's own variant is below; installing the other one
 * is how a board moves between them without USB.
 *
 * The install runs in its own task so the page can poll progress while it
 * downloads. One writer (the task), one reader (httpd), no locking. */
#define GH_RELEASES_URL     "https://api.github.com/repos/SEspe/RRp38KLineInterface/releases?per_page=8"
#define GH_ASSET_URL_PREFIX "https://github.com/SEspe/RRp38KLineInterface/releases/download/"
#define GH_REPO_URL         "https://github.com/SEspe/RRp38KLineInterface"
#define GH_ASSET_PREFIX     FIRMWARE_NAME "_"

#if CONFIG_RRP38_ALLOW_TX
#define FW_VARIANT "tx"
#else
#define FW_VARIANT "passive"
#endif

enum { GH_OTA_IDLE = 0, GH_OTA_DOWNLOADING, GH_OTA_SUCCESS, GH_OTA_ERROR };
static volatile int     g_gh_ota_state   = GH_OTA_IDLE;
static volatile int     g_gh_ota_percent = 0;
static volatile int64_t g_gh_ota_total   = 0;
static char             g_gh_ota_url[256] = "";
static char             g_gh_ota_err[128] = "";
static char             g_gh_ota_trace[600] = "";   /* HTTP event trace: diagnoses a failed
                                                       install without a USB cable */

/* Traces esp_http_client events during the download, exposed on the
 * progress endpoint. SlaveWallas found its redirect-buffer bug this way. */
static esp_err_t gh_http_event_handler(esp_http_client_event_t *evt)
{
    size_t len = strlen(g_gh_ota_trace);
    if (len >= sizeof(g_gh_ota_trace) - 60) return ESP_OK;
    char *p = g_gh_ota_trace + len;
    size_t room = sizeof(g_gh_ota_trace) - len;
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR: {
        esp_tls_error_handle_t eh = (esp_tls_error_handle_t)evt->data;
        if (eh) {
            snprintf(p, room, "|ERR(le=%d,tlsc=%d,flags=%d)",
                     (int)eh->last_error, eh->esp_tls_error_code, eh->esp_tls_flags);
        } else {
            snprintf(p, room, "|ERR(noeh)");
        }
        break;
    }
    case HTTP_EVENT_ON_CONNECTED:       snprintf(p, room, "|CONN"); break;
    case HTTP_EVENT_HEADERS_SENT:       snprintf(p, room, "|SENT"); break;
    case HTTP_EVENT_ON_HEADERS_COMPLETE:
        snprintf(p, room, "|HDRDONE(st=%d)", esp_http_client_get_status_code(evt->client));
        break;
    case HTTP_EVENT_ON_HEADER:
        if (evt->header_key && strcasecmp(evt->header_key, "Location") == 0) {
            snprintf(p, room, "|LOC=%.50s", evt->header_value);
        }
        break;
    case HTTP_EVENT_ON_FINISH:          snprintf(p, room, "|FIN"); break;
    case HTTP_EVENT_DISCONNECTED:       snprintf(p, room, "|DISC"); break;
    default: break;
    }
    return ESP_OK;
}

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
".ctl{display:flex;gap:12px;flex-wrap:wrap}"
".ctl label{margin-top:0}"
"select,input[type=number]{display:block;margin-top:4px;padding:7px;background:#0f3460;"
"border:1px solid #444;border-radius:5px;color:#eee;width:120px}"
"pre.log{font-family:Consolas,Menlo,monospace;font-size:.75rem;line-height:1.45;"
"max-height:60vh;overflow:auto;white-space:pre;background:#0b1730;padding:10px;border-radius:5px}"
"pre.log .t{color:#888}pre.log .g{color:#e9b400}"
"#wc{display:block;width:100%;height:220px;background:#0b1730;border-radius:5px}"
"</style></head><body>"
"<div class='hdr'><h1>RR P38 K-line</h1>"
"<span class='v'>" FIRMWARE_NAME " v" FIRMWARE_VERSION "</span></div>"
"<div class='tabs'>"
"<button class='tab on' onclick='show(\"status\",this)'>Status</button>"
"<button class='tab' onclick='show(\"sniff\",this)'>Sniff</button>"
"<button class='tab' onclick='show(\"debug\",this)'>Debug</button>"
"<button class='tab' onclick='show(\"wifi\",this)'>WiFi</button>"
"<button class='tab' onclick='show(\"ota\",this)'>OTA Update</button>"
"</div>"

"<div id='status' class='pane on'>"
"<div class='card'><h3>K-line</h3><div class='grid' id='kl'>&hellip;</div>"
"<p class='note'>Read-only. Arming, init, send and scan are on the USB console only.</p></div>"
"<div class='card'><h3>Network</h3><div class='grid' id='nw'>&hellip;</div></div>"
"<div class='card'><h3>System</h3><div class='grid' id='sy'>&hellip;</div></div>"
"<div class='ts' id='sts'></div></div>"

"<div id='sniff' class='pane'>"
"<div class='card'><h3>Passive capture</h3>"
"<p class='note'>Listens only; nothing is transmitted. Bytes are split into frames on silent gaps. "
"While a capture runs, console init and send commands are refused.</p>"
"<div class='ctl'>"
"<label>Duration<select id='sd'><option value='60'>1 min</option><option value='300'>5 min</option>"
"<option value='600' selected>10 min</option><option value='1800'>30 min</option><option value='3600'>60 min</option></select></label>"
"<label>Split gap (ms)<input id='sg' type='number' min='1' max='1000' value='10'></label></div>"
"<button class='btn' id='sb' onclick='sniffGo()'>Start</button> "
"<button class='btn s' onclick='sniffClear()'>Clear</button> "
"<button class='btn s' onclick='sniffSave()'>Download</button>"
"<div class='ts' id='ss'></div></div>"
"<div class='card'><h3>Frames</h3><pre class='log' id='sl'></pre></div></div>"

"<div id='debug' class='pane'>"
"<div class='card'><h3>Bus echo test</h3>"
"<p class='note'>Sends the bytes on the K-line through the transceiver and checks that each one comes back, while sampling RX (the waveform is drawn below). It transmits, so the car must be unplugged from the K-line. The page asks first.</p>"
"<div class='ctl'><label>Bytes (hex, up to 16)<input id='eth' type='text' value='55'></label></div>"
"<button class='btn' onclick='echoTest()'>Run echo test</button>"
"<button class='btn s' onclick='echoClear()'>Clear</button>"
"<div class='ts' id='et'></div>"
"<div class='ts' id='ed'></div></div>"
"<div class='card'><h3>Signal waveform</h3>"
"<p class='note'>Samples the pin level for a short window and draws every edge. Reads only: nothing is driven. "
"A quiet bus is a flat high line; a module talking shows as a pulse train. Samples cannot run during a capture or a console command.</p>"
"<div class='ctl'>"
"<label>Pin<select id='wp'><option value='rx' selected>RX (bus)</option><option value='tx'>TX (to transceiver)</option></select></label>"
"<label>Window (ms)<input id='ww' type='number' min='1' max='200' value='50'></label></div>"
"<div class='ctl'><label>Command<input id='wcmd' type='text' value='wave -p rx -t 50'></label></div>"
"<button class='btn' onclick='waveCmd()'>Run</button> "
"<button class='btn s' onclick='waveSample()'>Sample</button> "
"<button class='btn s' id='wsb' onclick='waveScan()'>Scan for activity</button>"
"<p class='note'>Scan repeats a sample each second, for up to 60 tries, and stops at the first one with edges.</p>"
"<div class='ts' id='ws'></div></div>"
"<div class='card'><h3>Pulse train</h3><canvas id='wc'></canvas>"
"<div class='grid' id='wst' style='margin-top:10px'></div></div></div>"

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
"<div class='card'><h3>Install from GitHub</h3>"
"<p class='note'>Needs the board on WiFi with internet (home WiFi tab). Each release has a "
"<b>passive</b> image (transmit compiled out, for listening in the car) and a <b>tx</b> image. "
"Installing the other kind switches the board between them.</p>"
"<button class='btn s' onclick='ghList()'>Check GitHub</button> "
"<a class='btn s' style='display:inline-block;text-decoration:none' href='" GH_REPO_URL "/releases' target='_blank' rel='noopener'>Releases page</a>"
"<div class='ts' id='ghs'></div>"
"<select id='ghsel' style='display:none;width:100%;max-width:420px'></select>"
"<button class='btn' id='ghbtn' style='display:none' onclick='ghInstall()'>Install selected</button>"
"<div class='prog' id='ghp'></div></div>"
"<div class='card'><h3>Upload firmware</h3>"
"<p class='note'>Select a compiled <code>.bin</code>. The K-line transceiver is put to sleep before "
"the reboot. Refused while a capture, init or send is using the K-line.</p>"
"<input type='file' id='fw' accept='.bin'>"
"<button class='btn' onclick='upload()'>Upload &amp; flash</button>"
"<div class='prog' id='prog'></div></div></div>"

"<script>"
"function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;');}"
"function row(k,v,c){return '<div class=row><span>'+k+'</span><b class=\"'+(c||'')+'\">'+v+'</b></div>';}"
"function show(id,b){document.querySelectorAll('.pane').forEach(e=>e.classList.remove('on'));"
"document.querySelectorAll('.tab').forEach(e=>e.classList.remove('on'));"
"document.getElementById(id).classList.add('on');b.classList.add('on');"
"if(id==='ota')ver();if(id==='sniff')sniffPoll();}"
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
"var VV={},GR=[],GT=null;"
"function vnum(v){return String(v||'').replace(/^v/,'').split('.').map(x=>parseInt(x,10)||0);}"
"function volder(a,b){var A=vnum(a),B=vnum(b);for(var i=0;i<Math.max(A.length,B.length);i++){"
"var x=A[i]||0,y=B[i]||0;if(x!==y)return x<y;}return false;}"
"function ver(){fetch('/api/version').then(r=>r.json()).then(v=>{VV=v;"
"document.getElementById('vr').innerHTML=row('Installed','v'+esc(v.installed)+' '+esc(v.variant),v.variant==='tx'?'amb':'ok')+"
"row('Stored (other slot)',v.stored?'v'+esc(v.stored).replace(/^v/,''):'\\u2014')+"
"row('Running from',esc(v.running))+(GR.length?row('Newest on GitHub',ghNewest(v.variant)):'');}).catch(()=>{});}"
"function ghNewest(vt){var n='';GR.forEach(r=>{if(r.variant===vt&&(!n||volder(n,r.version)))n=r.version;});"
"return n?'v'+esc(n)+(volder(VV.installed,n)?' \\u2014 newer':''):'\\u2014';}"
"function ghList(){var s=document.getElementById('ghs'),sel=document.getElementById('ghsel');"
"s.textContent='Asking GitHub\\u2026';sel.style.display='none';document.getElementById('ghbtn').style.display='none';"
"fetch('/api/ota/github/releases').then(r=>r.ok?r.json():r.text().then(t=>{throw new Error(t);})).then(a=>{"
"GR=a;ver();if(!a.length){s.textContent='No releases with RRp38KLine images yet.';return;}"
"s.textContent=a.length+' image'+(a.length!==1?'s':'')+' found';sel.innerHTML='';"
"a.forEach((r,i)=>{var o=document.createElement('option');o.value=i;"
"o.textContent=r.tag+' \\u2014 '+r.variant+(r.variant===VV.variant?'':' (switches build)')+' \\u2014 '+r.published.slice(0,10);"
"sel.appendChild(o);});"
"var own=a.findIndex(r=>r.variant===VV.variant);if(own>=0)sel.value=own;"
"sel.style.display='block';document.getElementById('ghbtn').style.display='inline-block';"
"}).catch(e=>{s.textContent='Failed: '+e.message;});}"
"function ghInstall(){var r=GR[document.getElementById('ghsel').value];if(!r)return;"
"var m='Install '+r.asset+' and reboot?';"
"if(r.variant!==VV.variant)m+=r.variant==='tx'?'\\n\\nThis is the TRANSMIT-CAPABLE build. It still needs arm --yes on the USB console before it sends anything.'"
":'\\n\\nThis is the PASSIVE build: transmit is compiled out until a tx image is installed again.';"
"if(!confirm(m))return;var p=document.getElementById('ghp');p.textContent='Starting\\u2026';"
"document.getElementById('ghbtn').disabled=true;"
"fetch('/api/ota/github/install',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'url='+encodeURIComponent(r.url)+'&size='+r.size}).then(x=>{if(!x.ok)return x.text().then(t=>{throw new Error(t);});"
"GT=setInterval(ghPoll,1000);}).catch(e=>{p.textContent='Could not start: '+e.message;"
"document.getElementById('ghbtn').disabled=false;});}"
"function ghPoll(){fetch('/api/ota/github/progress').then(r=>r.json()).then(d=>{var p=document.getElementById('ghp');"
"if(d.state==='downloading')p.textContent='Downloading and flashing: '+d.percent+'%';"
"else if(d.state==='success'){p.textContent='Done. Rebooting \\u2014 reload in about 10 s.';clearInterval(GT);}"
"else if(d.state==='error'){p.textContent='Error: '+d.error+' '+d.trace;clearInterval(GT);"
"document.getElementById('ghbtn').disabled=false;}}).catch(()=>{});}"
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
"var SQ=0,SS=-1,SL=[],SR=false;"
"function pad(s,n){s=String(s);while(s.length<n)s=' '+s;return s;}"
"function sniffPoll(){fetch('/api/sniff?since='+SQ).then(r=>r.json()).then(d=>{"
"if(d.session!==SS){if(SS!==-1&&d.frames+d.f.length>0)SL.push('--- capture '+d.session+' ---');SS=d.session;}"
"d.f.forEach(f=>{SQ=Math.max(SQ,f[0]);"
"SL.push('['+pad((f[1]/1000).toFixed(3),9)+'s +'+pad(f[2],5)+'ms] '+f[4]+(f[3]>" "48" "?' \\u2026 ('+f[3]+' bytes)':''));});"
"while(SL.length>2000)SL.shift();"
"if(d.f.length){var e=document.getElementById('sl'),b=e.scrollTop+e.clientHeight>=e.scrollHeight-20;"
"e.textContent=SL.join('\\n');if(b)e.scrollTop=e.scrollHeight;}"
"SR=d.running;document.getElementById('sb').textContent=SR?'Stop':'Start';"
"document.getElementById('ss').textContent=SR?'Capturing ('+d.owner+'), '+d.remain+' s left \\u2014 '"
"+d.frames+' frames, '+d.bytes+' bytes, split at '+d.gap+' ms'"
":(d.owner?'K-line busy: '+d.owner:'Idle \\u2014 last capture '+d.frames+' frames, '+d.bytes+' bytes');"
"if(d.f.length===32)sniffPoll();}).catch(()=>{});}"
"function sniffGo(){if(SR){fetch('/api/sniff/stop',{method:'POST'}).then(sniffPoll);return;}"
"var q='secs='+document.getElementById('sd').value+'&gap='+document.getElementById('sg').value;"
"fetch('/api/sniff/start?'+q,{method:'POST'}).then(r=>r.ok?sniffPoll():r.text().then(t=>{"
"document.getElementById('ss').textContent='Could not start: '+t;}));}"
"function sniffClear(){SL=[];document.getElementById('sl').textContent='';}"
"function sniffSave(){var a=document.createElement('a'),d=new Date();"
"a.href=URL.createObjectURL(new Blob([SL.join('\\n')+'\\n'],{type:'text/plain'}));"
"a.download='kline-sniff-'+d.toISOString().slice(0,19).replace(/[:T]/g,'-')+'.txt';a.click();}"
"var WS=false;"
"function waveGet(){var p=document.getElementById('wp').value,m=document.getElementById('ww').value;"
"return fetch('/api/wave?pin='+p+'&ms='+m).then(r=>r.ok?r.json():r.text().then(t=>{throw new Error(t);}));}"
"function waveSample(){waveGet().then(waveDraw).catch(e=>{document.getElementById('ws').textContent='Sample failed: '+e.message;});}"
"function wmsg(m){document.getElementById('ws').textContent=m;}"
"function waveCmd(){var t=document.getElementById('wcmd').value.split(' ').filter(Boolean);"
"if(t[0]!=='wave')return wmsg('Usage: wave [-p rx|tx] [-t ms]');"
"for(var i=1;i<t.length;i++){"
"if(t[i]==='-p'&&t[i+1]){var p=t[++i];if(p!=='rx'&&p!=='tx')return wmsg('pin must be rx or tx');document.getElementById('wp').value=p;}"
"else if(t[i]==='-t'&&t[i+1]){document.getElementById('ww').value=t[++i];}"
"else return wmsg('Unknown argument: '+t[i]);}"
"waveSample();}"
"function waveScan(){var b=document.getElementById('wsb'),n=0;"
"function done(msg){WS=false;b.textContent='Scan for activity';if(msg)document.getElementById('ws').textContent=msg;}"
"if(WS){WS=false;return;}WS=true;b.textContent='Stop';"
"function step(){if(!WS)return done('');if(++n>60)return done('No activity in 60 samples.');"
"waveGet().then(d=>{waveDraw(d);if(d.edges>0)return done('Activity after '+n+' sample(s): '+d.edges+' edges.');setTimeout(step,1000);})"
".catch(e=>done('Sample failed: '+e.message));}"
"step();}"
"function echoTest(){var h=document.getElementById('eth').value||'55';"
"if(!confirm('The car must be unplugged from the K-line. This sends '+h+' on the bus. Continue?'))return;"
"var e=document.getElementById('et');e.textContent='Sending...';"
"fetch('/api/echo-test',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'confirm=car-unplugged&hex='+encodeURIComponent(h)})"
".then(r=>r.text().then(t=>{if(!r.ok){e.textContent='Refused: '+t;return;}"
"var d=JSON.parse(t);e.textContent='Echo '+d.echo+': sent '+d.sent+(d.echo==='ok'?'':' ('+d.err+')');"
"if(d.wave){waveDraw(d.wave);document.getElementById('ed').textContent=frameDecode(d.wave);}}))"
".catch(x=>{e.textContent='Failed: '+x.message;});}"
"function echoClear(){['et','ed','ws','wst'].forEach(function(id){document.getElementById(id).textContent='';});"
"var c=document.getElementById('wc');c.getContext('2d').clearRect(0,0,c.width,c.height);}"
"function frameDecode(d){var bit=1000000/10400,e=d.e||[],out=[],tEnd=-1;"
"function levelAt(t){var l=d.level0;for(var i=0;i<e.length&&e[i][0]<=t;i++)l=e[i][1];return l;}"
"for(var i=0;i<e.length;i++){if(e[i][1]!==0||e[i][0]<tEnd)continue;var s=e[i][0],v=0;"
"for(var k=1;k<=8;k++)v|=levelAt(s+(k+0.5)*bit)<<(k-1);"
"out.push(v.toString(16).toUpperCase().padStart(2,'0')+' @'+(s/1000).toFixed(2)+' ms'+(levelAt(s+9.5*bit)?'':' (stop bit low)'));"
"tEnd=s+10*bit;}"
"return out.length?'Decoded at 10400 baud: '+out.join(', '):'No frames decoded.';}"
"function wus(u){return u>=1000?(u/1000).toFixed(2)+' ms':u+' us';}"
"function waveDraw(d){var c=document.getElementById('wc'),g=c.getContext('2d'),dp=window.devicePixelRatio||1,W=c.clientWidth,H=c.clientHeight;"
"c.width=W*dp;c.height=H*dp;g.setTransform(dp,0,0,dp,0,0);"
"var L=34,R=W-10,hy=26,ly=H-26,T=d.window_us,X=t=>L+(R-L)*t/T,Y=l=>l?hy:ly;"
"g.fillStyle='#0b1730';g.fillRect(0,0,W,H);"
"g.font='10px Arial';g.strokeStyle='#223';g.lineWidth=1;"
"for(var i=0;i<=10;i++){var gx=Math.round(X(T*i/10))+.5;g.beginPath();g.moveTo(gx,0);g.lineTo(gx,H);g.stroke();"
"g.fillStyle='#666';g.fillText(wus(Math.round(T*i/10)),gx+2,H-6);}"
"g.fillStyle='#888';g.fillText('1',8,hy+4);g.fillText('0',8,ly+4);"
"g.strokeStyle='#4caf50';g.lineWidth=1.5;g.beginPath();"
"var px=X(0),py=Y(d.level0);g.moveTo(px,py);"
"d.e.forEach(e=>{var x=X(e[0]);g.lineTo(x,py);py=Y(e[1]);g.lineTo(x,py);});"
"g.lineTo(X(T),py);g.stroke();"
"if(d.truncated){g.fillStyle='#e9b400';g.fillText('edge buffer full: window cut short',L+6,hy-8);}"
"var sb=d.est_baud?'est. '+d.est_baud+' baud from shortest pulse':'no complete pulse';"
"document.getElementById('ws').textContent=d.pin.toUpperCase()+', '+wus(d.window_us)+': '+d.edges+' edges, '+sb+'.';"
"document.getElementById('wst').innerHTML="
"row('Pin',d.pin.toUpperCase())+row('Level at start',d.level0?'high':'low')+"
"row('Edges',d.edges+(d.truncated?' (truncated)':''),d.truncated?'amb':'')+"
"row('High time',wus(d.high_us))+row('Low time',wus(d.low_us))+"
"row('Shortest high',d.min_high_us?wus(d.min_high_us):'none')+row('Shortest low',d.min_low_us?wus(d.min_low_us):'none')+"
"row('Est. baud',d.est_baud?String(d.est_baud):'none')+row('Window',wus(d.window_us));}"
"setInterval(()=>{if(document.getElementById('sniff').classList.contains('on'))sniffPoll();},700);"
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

    char buf[220];
    snprintf(buf, sizeof(buf),
             "{\"name\":\"%s\",\"variant\":\"%s\",\"installed\":\"%s\",\"stored\":\"%s\",\"running\":\"%s\"}",
             FIRMWARE_NAME, FW_VARIANT, FIRMWARE_VERSION, stored, run ? run->label : "");
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

/* GET /api/sniff?since=N - capture state plus frames newer than N, at most
 * 32 per call; the page asks again at once when it gets a full batch.
 * Frames are [seq, t_ms, gap_ms, len, "hex"]. */
#define SNIFF_BATCH 32

static esp_err_t h_sniff_get(httpd_req_t *req)
{
    char q[48], v[16];
    uint32_t since = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) {
        since = strtoul(v, NULL, 10);
    }

    sniff_state_t st;
    sniff_get_state(&st);
    sniff_frame_t *fr = malloc(SNIFF_BATCH * sizeof(*fr));
    size_t cap = 256 + SNIFF_BATCH * (SNIFF_FRAME_MAX * 3 + 48);
    char *buf = malloc(cap);
    if (!fr || !buf) {
        free(fr);
        free(buf);
        return httpd_resp_send_500(req);
    }
    size_t nf = sniff_frames_since(since, fr, SNIFF_BATCH);

    size_t n = snprintf(buf, cap,
        "{\"running\":%s,\"owner\":\"%s\",\"session\":%lu,\"seq\":%lu,\"bytes\":%lu,"
        "\"frames\":%lu,\"gap\":%lu,\"remain\":%lu,\"f\":[",
        st.running ? "true" : "false", st.owner, (unsigned long)st.session,
        (unsigned long)st.last_seq, (unsigned long)st.bytes, (unsigned long)st.frames,
        (unsigned long)st.gap_ms, (unsigned long)st.remain_s);
    for (size_t i = 0; i < nf; i++) {
        const sniff_frame_t *f = &fr[i];
        n += snprintf(buf + n, cap - n, "%s[%lu,%lu,%lu,%u,\"", i ? "," : "",
                      (unsigned long)f->seq, (unsigned long)f->t_ms,
                      (unsigned long)f->gap_ms, (unsigned)f->len);
        size_t kept = f->len < SNIFF_FRAME_MAX ? f->len : SNIFF_FRAME_MAX;
        for (size_t j = 0; j < kept; j++) {
            n += snprintf(buf + n, cap - n, j ? " %02X" : "%02X", f->d[j]);
        }
        n += snprintf(buf + n, cap - n, "\"]");
    }
    snprintf(buf + n, cap - n, "]}");
    free(fr);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, buf);
    free(buf);
    return err;
}

/* POST /api/sniff/start?secs=600&gap=10 - passive only. */
static esp_err_t h_sniff_start(httpd_req_t *req)
{
    char q[48], v[16];
    uint32_t secs = 600, gap = 10;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "secs", v, sizeof(v)) == ESP_OK) {
            secs = strtoul(v, NULL, 10);
        }
        if (httpd_query_key_value(q, "gap", v, sizeof(v)) == ESP_OK) {
            gap = strtoul(v, NULL, 10);
        }
    }
    if (sniff_start_bg(secs, gap) != ESP_OK) {
        char msg[48];
        snprintf(msg, sizeof(msg), "K-line busy (%s)", kline_owner()[0] ? kline_owner() : "not started");
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, msg);
    }
    return httpd_resp_sendstr(req, "OK");
}

/* POST /api/sniff/stop - ends a capture, whether started here or on the
 * console. */
static esp_err_t h_sniff_stop(httpd_req_t *req)
{
    sniff_stop();
    return httpd_resp_sendstr(req, "OK");
}

/* One sample as a JSON object: summary numbers and the edge list. Returns the
 * length written. */
static size_t wave_json(char *buf, size_t cap, const char *pin, const wave_result_t *r,
                        const wave_edge_t *ed, size_t ne)
{
    size_t n = snprintf(buf, cap,
        "{\"pin\":\"%s\",\"window_us\":%lu,\"level0\":%u,\"edges\":%lu,\"truncated\":%s,"
        "\"high_us\":%lu,\"low_us\":%lu,\"min_high_us\":%lu,\"min_low_us\":%lu,"
        "\"est_baud\":%lu,\"e\":[",
        pin, (unsigned long)r->window_us, (unsigned)r->level0,
        (unsigned long)r->edges, r->truncated ? "true" : "false", (unsigned long)r->high_us,
        (unsigned long)r->low_us, (unsigned long)r->min_high_us, (unsigned long)r->min_low_us,
        (unsigned long)r->est_baud);
    for (size_t i = 0; i < ne; i++) {
        n += snprintf(buf + n, cap - n, "%s[%lu,%u]", i ? "," : "",
                      (unsigned long)ed[i].t_us, (unsigned)ed[i].level);
    }
    return n + snprintf(buf + n, cap - n, "]}");
}

/* GET /api/wave?pin=rx|tx&ms=50 - one sample of a pin's level: summary
 * numbers and the edge list. Listens only. */
static esp_err_t h_wave(httpd_req_t *req)
{
    char q[48], v[16];
    wave_pin_t pin = WAVE_PIN_RX;
    uint32_t ms = 50;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "pin", v, sizeof(v)) == ESP_OK && strcmp(v, "tx") == 0) {
            pin = WAVE_PIN_TX;
        }
        if (httpd_query_key_value(q, "ms", v, sizeof(v)) == ESP_OK) {
            ms = strtoul(v, NULL, 10);
        }
    }
    ms = ms > WAVE_MAX_US / 1000 ? WAVE_MAX_US / 1000 : ms;

    wave_result_t r;
    if (wave_sample(pin, ms * 1000, &r) != ESP_OK) {
        char msg[48];
        snprintf(msg, sizeof(msg), "K-line busy (%s)", kline_owner());
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, msg);
    }

    wave_edge_t *ed = malloc(WAVE_EDGES_MAX * sizeof(*ed));
    size_t cap = 512 + WAVE_EDGES_MAX * 16;
    char *buf = malloc(cap);
    if (!ed || !buf) {
        free(ed);
        free(buf);
        return httpd_resp_send_500(req);
    }
    size_t ne = wave_edges(ed, WAVE_EDGES_MAX);
    wave_json(buf, cap, pin == WAVE_PIN_TX ? "tx" : "rx", &r, ed, ne);
    free(ed);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, buf);
    free(buf);
    return err;
}

#define ECHO_MAX_BYTES   16
#define ECHO_WINDOW_US   200000    /* RX sample around the send */

/* Form values arrive percent-encoded ('+' or %20 for a space). The query
 * helper does not decode them, so do it here, in place. */
static void form_decode(char *s)
{
    char *d = s;
    while (*s) {
        if (*s == '+') {
            *d++ = ' ';
            s++;
        } else if (s[0] == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], '\0' };
            *d++ = (char)strtol(h, NULL, 16);
            s += 3;
        } else {
            *d++ = *s++;
        }
    }
    *d = '\0';
}

typedef struct {
    wave_result_t result;
    TaskHandle_t  waiter;
} echo_capture_t;

/* Samples RX in the background while the caller transmits. */
static void echo_capture_task(void *arg)
{
    echo_capture_t *c = (echo_capture_t *)arg;
    wave_capture(WAVE_PIN_RX, ECHO_WINDOW_US, &c->result);
    xTaskNotifyGive(c->waiter);
    vTaskDelete(NULL);
}

/* POST /api/echo-test, body: confirm=car-unplugged&hex=55 FF ...
 * Sends the bytes on the K-line with the echo check and samples RX over the
 * same window. It transmits, so the car must be unplugged. The page asks
 * first; the exact confirmation is required here too. */
static esp_err_t h_echo_test(httpd_req_t *req)
{
    char body[320] = {0};
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body) || httpd_req_recv(req, body, len) != len) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "send confirm=car-unplugged&hex=55");
    }
    char v[256];
    if (httpd_query_key_value(body, "confirm", v, sizeof(v)) != ESP_OK ||
        strcmp(v, "car-unplugged") != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "confirmation missing");
    }
    if (httpd_query_key_value(body, "hex", v, sizeof(v)) != ESP_OK) {
        strlcpy(v, "55", sizeof(v));
    } else {
        form_decode(v);
    }

    uint8_t bytes[ECHO_MAX_BYTES];
    size_t n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(v, " ,", &save); tok; tok = strtok_r(NULL, " ,", &save)) {
        /* Each token is one or more byte pairs: "55", "00FF" or "00 FF". */
        for (char *p = tok; *p; p += 2) {
            if (!p[1] || !isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1]) ||
                n >= ECHO_MAX_BYTES) {
                httpd_resp_set_status(req, "400 Bad Request");
                return httpd_resp_sendstr(req, "give up to 16 hex bytes, 00..FF");
            }
            char pair[3] = { p[0], p[1], '\0' };
            bytes[n++] = (uint8_t)strtoul(pair, NULL, 16);
        }
    }
    if (n == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "no bytes given");
    }

    if (!kline_claim("web echo")) {
        char msg[64];
        snprintf(msg, sizeof(msg), "K-line busy (%s)", kline_owner());
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, msg);
    }
    echo_capture_t *cap = calloc(1, sizeof(*cap));
    if (!cap) {
        kline_unclaim();
        return httpd_resp_send_500(req);
    }
    cap->waiter = xTaskGetCurrentTaskHandle();
    if (xTaskCreate(echo_capture_task, "echo_wave", 4096, cap, 5, NULL) != pdPASS) {
        free(cap);
        kline_unclaim();
        return httpd_resp_send_500(req);
    }
    vTaskDelay(pdMS_TO_TICKS(5));    /* let the sample start before the send */
    esp_err_t err = kline_echo(bytes, n);
    bool captured = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) > 0;
    kline_unclaim();

    char sent[ECHO_MAX_BYTES * 3 + 1] = "";
    for (size_t i = 0; i < n; i++) {
        char t[4];
        snprintf(t, sizeof(t), i ? " %02X" : "%02X", bytes[i]);
        strlcat(sent, t, sizeof(sent));
    }

    size_t cap_json = 512 + WAVE_EDGES_MAX * 16;
    wave_edge_t *ed = malloc(WAVE_EDGES_MAX * sizeof(*ed));
    char *buf = malloc(cap_json);
    if (!ed || !buf) {
        free(ed);
        free(buf);
        free(cap);
        return httpd_resp_send_500(req);
    }
    size_t out = snprintf(buf, cap_json,
        "{\"echo\":\"%s\",\"err\":\"%s\",\"sent\":\"%s\",\"wave\":",
        err == ESP_OK ? "ok" : "fail", esp_err_to_name(err), sent);
    if (captured) {
        size_t ne = wave_edges(ed, WAVE_EDGES_MAX);
        out += wave_json(buf + out, cap_json - out, "rx", &cap->result, ed, ne);
    } else {
        out += snprintf(buf + out, cap_json - out, "null");
    }
    snprintf(buf + out, cap_json - out, "}");
    free(ed);
    free(cap);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, buf);
    free(buf);
    return r;
}

/* POST /api/reboot */
static esp_err_t h_reboot(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "OK");
    ESP_LOGW(TAG, "reboot requested from the web page");
    restart_later(300);
    return ESP_OK;
}

/* No OTA while a capture, init or send holds the K-line: the reboot at the
 * end would cut it off mid-session. Sends the refusal itself. */
static bool refuse_if_bus_busy(httpd_req_t *req)
{
    const char *owner = kline_owner();
    if (!owner[0]) {
        return false;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "K-line busy (%s) - stop it before updating", owner);
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_sendstr(req, msg);
    return true;
}

/* POST /ota/upload - raw .bin, streamed into the inactive slot. */
static esp_err_t h_ota_upload(httpd_req_t *req)
{
    if (refuse_if_bus_busy(req)) {
        return ESP_OK;
    }
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

/* ------------------------------------------------- GitHub releases -- */

/* One literal key the release scanner watches for in the streaming body. */
typedef struct { const char *pat; size_t len; size_t k; } gh_key_t;

/* Advances a literal matcher by one byte; true when the whole key completes. */
static bool gh_key_feed(gh_key_t *k, char c)
{
    if (c == k->pat[k->k]) {
        if (++k->k == k->len) {
            k->k = 0;
            return true;
        }
    } else {
        k->k = (c == k->pat[0]) ? 1 : 0;
    }
    return false;
}

/* Splits a release asset filename "RRp38KLine_<variant>_v<ver>.bin" into
 * variant and version. Both are restricted to plain characters, since they
 * come off the network and end up in a JSON body. */
static bool gh_parse_asset(const char *base, char *variant, size_t vcap, char *ver, size_t vercap)
{
    const size_t plen = strlen(GH_ASSET_PREFIX);
    size_t blen = strlen(base);
    if (blen <= plen + 4 || strncmp(base, GH_ASSET_PREFIX, plen) != 0 ||
        strcmp(base + blen - 4, ".bin") != 0) {
        return false;
    }
    const char *v = base + plen;
    const char *sep = strstr(v, "_v");
    if (!sep || sep == v || (size_t)(sep - v) >= vcap) {
        return false;
    }
    size_t verlen = (size_t)(base + blen - 4 - (sep + 2));
    if (verlen == 0 || verlen >= vercap) {
        return false;
    }
    for (const char *c = v; c < base + blen - 4; c++) {
        bool ok = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') ||
                  (*c >= 'A' && *c <= 'Z') || *c == '.' || *c == '-' || *c == '_';
        if (!ok) {
            return false;
        }
    }
    memcpy(variant, v, (size_t)(sep - v));
    variant[sep - v] = '\0';
    memcpy(ver, sep + 2, verlen);
    ver[verlen] = '\0';
    return true;
}

/* Opens the releases listing. On failure sends the error itself. */
static esp_http_client_handle_t gh_open_releases(httpd_req_t *req)
{
    esp_http_client_config_t cfg = {
        .url = GH_RELEASES_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "HTTP client init failed");
        return NULL;
    }
    esp_http_client_set_header(client, "User-Agent", "RRp38KLine-ESP32");
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");
    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "GitHub unreachable - is the board on home WiFi?");
        return NULL;
    }
    esp_http_client_fetch_headers(client);   /* -1 for a chunked body; only the status matters */
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        char msg[64];
        snprintf(msg, sizeof(msg), "GitHub API returned HTTP %d", status);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
        return NULL;
    }
    return client;
}

/* GET /api/ota/github/releases - every passive/tx image in the newest
 * releases, newest first: [{tag,published,variant,version,asset,url,size}].
 *
 * The body is scanned byte by byte as it streams in instead of being parsed
 * whole: the listing grows with every release, and holding it plus a JSON
 * tree would eventually not fit (RemoteStart hit that). Key order makes it
 * work: tag_name and published_at precede a release's assets, and within an
 * asset size precedes browser_download_url, whose last path segment is the
 * filename. */
static esp_err_t h_ota_gh_releases(httpd_req_t *req)
{
    esp_http_client_handle_t client = gh_open_releases(req);
    if (!client) {
        return ESP_OK;
    }
    const size_t out_cap = 6144;
    char *out = malloc(out_cap);
    if (!out) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }

    gh_key_t k_tag  = { "\"tag_name\":\"",             12, 0 };
    gh_key_t k_pub  = { "\"published_at\":\"",         16, 0 };
    gh_key_t k_size = { "\"size\":",                    7, 0 };
    gh_key_t k_url  = { "\"browser_download_url\":\"", 24, 0 };

    enum { CAP_NONE = 0, CAP_TAG, CAP_PUB, CAP_SIZE, CAP_URL };
    int    cap = CAP_NONE;
    size_t caplen = 0;
    char   tag[40] = "", pub[40] = "", url[220] = "";
    long   asize = 0;
    const size_t dlen = strlen(GH_ASSET_URL_PREFIX);

    size_t n = 0;
    out[n++] = '[';
    bool first = true, full = false;
    char buf[512];
    int  r;

    while (!full && (r = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < r && !full; i++) {
            char c = buf[i];
            if (cap != CAP_NONE) {
                bool done = (cap == CAP_SIZE) ? (c < '0' || c > '9') : (c == '"');
                if (!done) {
                    switch (cap) {   /* anything overlong is truncated */
                    case CAP_TAG:  if (caplen < sizeof(tag) - 1) tag[caplen++] = c; break;
                    case CAP_PUB:  if (caplen < sizeof(pub) - 1) pub[caplen++] = c; break;
                    case CAP_URL:  if (caplen < sizeof(url) - 1) url[caplen++] = c; break;
                    case CAP_SIZE: if (caplen < 12) { asize = asize * 10 + (c - '0'); caplen++; } break;
                    }
                    continue;
                }
                if (cap == CAP_TAG) tag[caplen] = '\0';
                if (cap == CAP_PUB) pub[caplen] = '\0';
                if (cap == CAP_URL) {
                    url[caplen] = '\0';
                    const char *slash = strrchr(url, '/');
                    const char *base  = slash ? slash + 1 : url;
                    char variant[16], ver[24];
                    if (strncmp(url, GH_ASSET_URL_PREFIX, dlen) == 0 &&
                        gh_parse_asset(base, variant, sizeof(variant), ver, sizeof(ver))) {
                        char entry[480];
                        int m = snprintf(entry, sizeof(entry),
                            "%s{\"tag\":\"%s\",\"published\":\"%s\",\"variant\":\"%s\","
                            "\"version\":\"%s\",\"asset\":\"%s\",\"url\":\"%s\",\"size\":%ld}",
                            first ? "" : ",", tag, pub, variant, ver, base, url, asize);
                        if (m > 0 && n + (size_t)m < out_cap - 2) {
                            memcpy(out + n, entry, (size_t)m);
                            n += (size_t)m;
                            first = false;
                        } else {
                            full = true;   /* newest are already in */
                        }
                    }
                }
                cap = CAP_NONE;
                /* fall through: the terminating byte still feeds the matchers */
            }
            /* Every matcher sees every byte, then at most one starts a capture. */
            bool m_tag  = gh_key_feed(&k_tag,  c);
            bool m_pub  = gh_key_feed(&k_pub,  c);
            bool m_size = gh_key_feed(&k_size, c);
            bool m_url  = gh_key_feed(&k_url,  c);
            if      (m_tag)  { cap = CAP_TAG;  caplen = 0; }
            else if (m_pub)  { cap = CAP_PUB;  caplen = 0; }
            else if (m_size) { cap = CAP_SIZE; caplen = 0; asize = 0; }
            else if (m_url)  { cap = CAP_URL;  caplen = 0; }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    out[n++] = ']';
    out[n] = '\0';
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out);
    free(out);
    return err;
}

/* Background download + flash for a GitHub install. */
static void gh_ota_task(void *arg)
{
    g_gh_ota_trace[0] = '\0';
    esp_http_client_config_t http_cfg = {
        .url = g_gh_ota_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        /* GitHub redirects the asset to a signed URL around 2 KB long. The
         * request line is built in buffer_size_tx (512 by default), so both
         * buffers must be large or the redirect fails with a bare ESP_FAIL. */
        .buffer_size    = 4096,
        .buffer_size_tx = 4096,
        .event_handler  = gh_http_event_handler,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        snprintf(g_gh_ota_err, sizeof(g_gh_ota_err), "begin:%s", esp_err_to_name(err));
        g_gh_ota_state = GH_OTA_ERROR;
        ESP_LOGE(TAG, "GitHub OTA begin failed: %s (%s)", esp_err_to_name(err), g_gh_ota_url);
        vTaskDelete(NULL);
        return;
    }
    do {
        err = esp_https_ota_perform(handle);
        int64_t read = esp_https_ota_get_image_len_read(handle);
        if (g_gh_ota_total > 0) {
            int pct = (int)(read * 100 / g_gh_ota_total);
            g_gh_ota_percent = pct > 99 ? 99 : pct;
        }
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    esp_err_t finish_err = ESP_FAIL;
    bool complete = esp_https_ota_is_complete_data_received(handle);
    if (err == ESP_OK && complete) {
        finish_err = esp_https_ota_finish(handle);
    } else {
        esp_https_ota_abort(handle);
    }
    if (finish_err == ESP_OK) {
        g_gh_ota_percent = 100;
        g_gh_ota_state   = GH_OTA_SUCCESS;
        ESP_LOGI(TAG, "GitHub OTA done, rebooting");
        vTaskDelay(pdMS_TO_TICKS(500));
        kline_release_bus();
        esp_restart();
    }
    snprintf(g_gh_ota_err, sizeof(g_gh_ota_err), "%s:%s",
             err == ESP_OK ? "finish" : "perform",
             esp_err_to_name(err == ESP_OK ? finish_err : err));
    g_gh_ota_state = GH_OTA_ERROR;
    ESP_LOGE(TAG, "GitHub OTA failed: %s", g_gh_ota_err);
    vTaskDelete(NULL);
}

/* POST /api/ota/github/install - form body url=...&size=N. The URL must be
 * this repo's release-download path, so a crafted request cannot point the
 * board at an arbitrary host. Returns at once; the page polls progress. */
static esp_err_t h_ota_gh_install(httpd_req_t *req)
{
    if (g_gh_ota_state == GH_OTA_DOWNLOADING) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "install already running");
    }
    if (refuse_if_bus_busy(req)) {
        return ESP_OK;
    }
    char body[400], url[256], size[16];
    if (read_body(req, body, sizeof(body)) < 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    }
    form_field(body, "url", url, sizeof(url));
    form_field(body, "size", size, sizeof(size));
    if (strncmp(url, GH_ASSET_URL_PREFIX, strlen(GH_ASSET_URL_PREFIX)) != 0 ||
        strstr(url, "..") || strchr(url, '?')) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "URL not from this repo's releases");
    }
    strlcpy(g_gh_ota_url, url, sizeof(g_gh_ota_url));
    g_gh_ota_total   = strtoll(size, NULL, 10);
    g_gh_ota_percent = 0;
    g_gh_ota_err[0]  = '\0';
    g_gh_ota_state   = GH_OTA_DOWNLOADING;
    if (xTaskCreate(gh_ota_task, "gh_ota", 8192, NULL, 4, NULL) != pdPASS) {
        g_gh_ota_state = GH_OTA_ERROR;
        strlcpy(g_gh_ota_err, "could not start install task", sizeof(g_gh_ota_err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, g_gh_ota_err);
    }
    ESP_LOGI(TAG, "GitHub OTA: %s", g_gh_ota_url);
    return httpd_resp_sendstr(req, "OK");
}

/* GET /api/ota/github/progress */
static esp_err_t h_ota_gh_progress(httpd_req_t *req)
{
    const char *st = g_gh_ota_state == GH_OTA_DOWNLOADING ? "downloading" :
                     g_gh_ota_state == GH_OTA_SUCCESS     ? "success"     :
                     g_gh_ota_state == GH_OTA_ERROR       ? "error"       : "idle";
    char buf[900];
    snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"percent\":%d,\"error\":\"%s\",\"trace\":\"%s\"}",
             st, g_gh_ota_percent, g_gh_ota_err, g_gh_ota_trace);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* ------------------------------------------------------------ start -- */

esp_err_t web_server_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 24;      /* headroom: an exact-fit cap silently drops routes */
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;    /* abandoned connections must not wedge the server */
    /* Half of a budget: httpd also holds a listener and a control socket, so
     * 7 sessions use 9 of CONFIG_LWIP_MAX_SOCKETS (16). The rest is for the
     * outbound TLS socket of a GitHub fetch; RemoteStart's installs failed
     * when a polling page had taken every descriptor. */
    cfg.max_open_sockets = 7;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return ESP_FAIL;
    }
    static const httpd_uri_t routes[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = h_root      },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = h_status    },
        { .uri = "/api/version", .method = HTTP_GET,  .handler = h_version   },
        { .uri = "/api/scan",    .method = HTTP_GET,  .handler = h_scan      },
        { .uri = "/api/sniff",   .method = HTTP_GET,  .handler = h_sniff_get },
        { .uri = "/api/sniff/start", .method = HTTP_POST, .handler = h_sniff_start },
        { .uri = "/api/sniff/stop",  .method = HTTP_POST, .handler = h_sniff_stop  },
        { .uri = "/api/wave",    .method = HTTP_GET,  .handler = h_wave      },
        { .uri = "/api/echo-test", .method = HTTP_POST, .handler = h_echo_test },
        { .uri = "/wifi-save",   .method = HTTP_POST, .handler = h_wifi_save },
        { .uri = "/ap-save",     .method = HTTP_POST, .handler = h_ap_save   },
        { .uri = "/api/reboot",  .method = HTTP_POST, .handler = h_reboot    },
        { .uri = "/ota/upload",  .method = HTTP_POST, .handler = h_ota_upload},
        { .uri = "/api/ota/github/releases", .method = HTTP_GET,  .handler = h_ota_gh_releases },
        { .uri = "/api/ota/github/install",  .method = HTTP_POST, .handler = h_ota_gh_install  },
        { .uri = "/api/ota/github/progress", .method = HTTP_GET,  .handler = h_ota_gh_progress },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (httpd_register_uri_handler(server, &routes[i]) != ESP_OK) {
            ESP_LOGE(TAG, "failed to register %s", routes[i].uri);
        }
    }
    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}
