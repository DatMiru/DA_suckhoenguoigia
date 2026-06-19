#include "WebDashboard.h"
#include <WebServer.h>
#include <WiFi.h>

WebServer server(80);

// =========================================================
// HTML Dashboard (PROGMEM để tiết kiệm RAM)
// =========================================================
static const char DASHBOARD_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="vi">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Health Monitor</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
<style>
@import url('https://fonts.googleapis.com/css2?family=Inter:wght@300;400;600;700;900&display=swap');
:root{--bg:#f8fafc;--card:#ffffff;--border:#e2e8f0;--red:#ef4444;--blue:#3b82f6;--green:#10b981;--yellow:#f59e0b;--dim:#64748b}
*{margin:0;padding:0;box-sizing:border-box}
body{background:var(--bg);background-image:radial-gradient(ellipse 60% 40% at 15% 25%,rgba(59,130,246,.04) 0%,transparent 60%),radial-gradient(ellipse 50% 40% at 85% 75%,rgba(239,77,109,.04) 0%,transparent 60%);min-height:100vh;font-family:'Inter',sans-serif;color:#1e293b;padding:18px}
header{display:flex;justify-content:space-between;align-items:center;margin-bottom:20px;padding-bottom:14px;border-bottom:1px solid var(--border)}
.logo{font-size:1rem;font-weight:700;color:var(--blue);letter-spacing:3px;text-transform:uppercase}
.live{display:flex;align-items:center;gap:8px;font-size:.75rem;color:var(--dim)}
.dot{width:8px;height:8px;border-radius:50%;background:var(--green);box-shadow:0 0 8px var(--green);animation:blink 2s infinite}
.dot.off{background:#94a3b8;box-shadow:none;animation:none}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.3}}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:14px;margin-bottom:16px}
.card{background:var(--card);border:1px solid var(--border);border-radius:16px;padding:18px;backdrop-filter:blur(12px);transition:border-color .3s,transform .2s}
.card:hover{border-color:#cbd5e1;box-shadow:0 10px 15px -3px rgba(0,0,0,0.05);transform:translateY(-2px)}
.lbl{font-size:.65rem;font-weight:600;letter-spacing:2px;text-transform:uppercase;color:var(--dim);margin-bottom:10px}
.val{font-size:2.8rem;font-weight:900;line-height:1}
.unit{font-size:.8rem;color:var(--dim);margin-top:2px}
.vred{color:var(--red)}
.vblue{color:var(--blue)}
.vgreen{color:var(--green)}
.vyellow{color:var(--yellow)}
.bat-bar{margin-top:10px;height:5px;background:rgba(0,0,0,.06);border-radius:3px;overflow:hidden}
.bat-fill{height:100%;border-radius:3px;background:linear-gradient(90deg,var(--red),var(--yellow),var(--green));transition:width .6s}
.status-row{display:flex;align-items:center;gap:12px;margin-top:6px}
.status-icon{font-size:2.2rem}
.status-txt{font-size:1rem;font-weight:700}
.safe{color:var(--green)}.danger{color:var(--red);animation:flash .5s infinite}.warn{color:var(--yellow)}
@keyframes flash{0%,100%{opacity:1}50%{opacity:.2}}
.chart-card{grid-column:1/-1}
.chart-wrap{position:relative;height:150px}
.info-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px;margin-bottom:18px}
.info-box{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:12px;font-size:.75rem}
.info-box span{color:var(--dim);display:block;margin-bottom:3px}
.info-box strong{font-size:.95rem;font-weight:700}
footer{text-align:center;font-size:.65rem;color:var(--dim);padding-top:10px;border-top:1px solid var(--border)}
</style>
</head>
<body>
<header>
  <div class="logo">⚕ Health Monitor</div>
  <div class="live"><div class="dot" id="dot"></div><span id="liveText">Đang kết nối...</span></div>
</header>

<div class="grid">
  <div class="card">
    <div class="lbl">❤️ Nhịp Tim</div>
    <div class="val vred" id="hrVal">--</div>
    <div class="unit">BPM</div>
  </div>
  <div class="card">
    <div class="lbl">💧 SpO2</div>
    <div class="val vblue" id="spo2Val">--</div>
    <div class="unit">%</div>
  </div>
  <div class="card">
    <div class="lbl">🔋 Pin</div>
    <div class="val vgreen" id="batVal">--</div>
    <div class="unit" id="batVolt">V</div>
    <div class="bat-bar"><div class="bat-fill" id="batFill" style="width:0%"></div></div>
  </div>
  <div class="card">
    <div class="lbl">🛡 Trạng Thái</div>
    <div class="status-row">
      <div class="status-icon" id="stIcon">🟢</div>
      <div class="status-txt safe" id="stTxt">AN TOÀN</div>
    </div>
  </div>
  <div class="card chart-card">
    <div class="lbl">📈 Sóng Quang Mạch PPG (IR — Thời Gian Thực)</div>
    <div class="chart-wrap"><canvas id="ppgChart"></canvas></div>
  </div>
</div>

<div class="info-grid">
  <div class="info-box"><span>Chế Độ Hiện Tại</span><strong id="modeVal">--</strong></div>
  <div class="info-box"><span>Gia Tốc Lọc</span><strong id="accelVal">-- g</strong></div>
  <div class="info-box"><span>Góc Nghiêng</span><strong id="angleVal">--°</strong></div>
  <div class="info-box"><span>HR Cơ Sở (Baseline)</span><strong id="baseHRVal">-- BPM</strong></div>
  <div class="info-box"><span>Trạng Thái HR Mode</span><strong id="hrModeVal">--</strong></div>
  <div class="info-box"><span>Cập Nhật Lần Cuối</span><strong id="lastUp">--</strong></div>
</div>

<footer>ESP32 Health Monitor — Rev10 &nbsp;|&nbsp; Đồ Án Tốt Nghiệp 2026</footer>

<script>
const N = 150;
const ppgBuf = new Array(N).fill(0);
const ppgQueue = [];
const ctx = document.getElementById('ppgChart').getContext('2d');
const chart = new Chart(ctx, {
  type:'line',
  data:{
    labels: ppgBuf.map(()=>''),
    datasets:[{
      data: ppgBuf,
      borderColor:'#ff4d6d',
      borderWidth:1.5,
      pointRadius:0,
      tension:0.3,
      fill:true,
      backgroundColor:'rgba(255,77,109,.08)'
    }]
  },
  options:{
    animation:false,
    responsive:true,
    maintainAspectRatio:false,
    plugins:{legend:{display:false}},
    scales:{
      x:{display:false},
      y:{display:false,min:-200,max:200}
    }
  }
});

function updateChart(newPts) {
  if (!newPts || newPts.length === 0) return;
  newPts.forEach(v => {
    if (ppgQueue.length < 300) ppgQueue.push(v);
  });
}

let ppgAccumulator = 0;
function renderPPG() {
  if (ppgQueue.length > 0) {
    let ptsToDraw = 0;
    // 40 điểm / 400ms = 100 điểm/s. Tại 60fps -> trung bình vẽ 1.67 điểm/frame
    if (ppgQueue.length > 120) {
      ptsToDraw = Math.min(ppgQueue.length, 6);
    } else if (ppgQueue.length > 50) {
      ptsToDraw = 3;
    } else if (ppgQueue.length > 10) {
      ppgAccumulator += 1.67;
      ptsToDraw = Math.floor(ppgAccumulator);
      ppgAccumulator -= ptsToDraw;
    } else {
      ptsToDraw = 1;
    }
    
    ptsToDraw = Math.min(ptsToDraw, ppgQueue.length);
    if (ptsToDraw > 0) {
      for (let i = 0; i < ptsToDraw; i++) {
        ppgBuf.shift();
        ppgBuf.push(ppgQueue.shift());
      }
      chart.data.datasets[0].data = [...ppgBuf];
      const mn = Math.min(...ppgBuf), mx = Math.max(...ppgBuf);
      const pad = Math.max(20, (mx - mn) * 0.15);
      chart.options.scales.y.min = mn - pad;
      chart.options.scales.y.max = mx + pad;
      chart.update('none');
    }
  }
  requestAnimationFrame(renderPPG);
}
requestAnimationFrame(renderPPG);

const HR_MODES = {0:'IDLE/SLEEP', 1:'ĐO BASELINE', 2:'KHẨN CẤP', 3:'THỦ CÔNG'};
let failCount = 0;

async function fetchData() {
  try {
    const res = await fetch('/data', {signal: AbortSignal.timeout(2000)});
    const d = await res.json();
    failCount = 0;

    // Live indicator
    document.getElementById('dot').className = 'dot';
    document.getElementById('liveText').textContent = 'Kết nối trực tiếp';

    // Vitals
    document.getElementById('hrVal').textContent   = d.hr   > 0 ? d.hr   : '--';
    document.getElementById('spo2Val').textContent = d.spo2 > 0 ? d.spo2 : '--';

    // Battery
    if (d.bat >= 0) {
      document.getElementById('batVal').textContent  = d.bat.toFixed(1);
      document.getElementById('batVolt').textContent = d.volt.toFixed(3) + ' V';
      document.getElementById('batFill').style.width = d.bat + '%';
    } else {
      document.getElementById('batVal').textContent  = 'N/A';
    }

    // Status
    const ic = document.getElementById('stIcon');
    const tx = document.getElementById('stTxt');
    tx.className = 'status-txt';
    if (d.fall) {
      ic.textContent='🚨'; tx.textContent='TÉ NGÃ!';      tx.classList.add('danger');
    } else if (d.impact) {
      ic.textContent='⚠️'; tx.textContent='VA CHẠM...';   tx.classList.add('warn');
    } else if (d.manual) {
      ic.textContent='🔬'; tx.textContent='THỦ CÔNG';     tx.classList.add('warn');
    } else {
      ic.textContent='🟢'; tx.textContent='AN TOÀN';      tx.classList.add('safe');
    }

    // Info boxes
    let mode = d.fall?'🚨 BÁO ĐỘNG':d.impact?'⚠️ PHÂN TÍCH':d.manual?'🔬 MANUAL':'✅ BÌNH THƯỜNG';
    document.getElementById('modeVal').textContent    = mode;
    document.getElementById('accelVal').textContent   = d.accel.toFixed(2) + ' g';
    document.getElementById('angleVal').textContent   = d.angle.toFixed(1) + '°';
    document.getElementById('baseHRVal').textContent  = d.baseHR + ' BPM';
    document.getElementById('hrModeVal').textContent  = HR_MODES[d.hrMode] || '--';
    document.getElementById('lastUp').textContent     = new Date().toLocaleTimeString('vi-VN');

    // PPG waveform
    if (d.ppg && d.ppg.length > 0) updateChart(d.ppg);

  } catch(e) {
    failCount++;
    if (failCount > 3) {
      document.getElementById('dot').className = 'dot off';
      document.getElementById('liveText').textContent = 'Mất kết nối...';
    }
  }
}

setInterval(fetchData, 400);
fetchData();
</script>
</body>
</html>
)rawhtml";

// =========================================================
// /data — trả về JSON cho client
// =========================================================
static void handleData() {
    // Lấy 40 điểm PPG mới nhất — snapshot nhanh với critical section (Core 0 đọc, Core 1 ghi)
    const int SEND_PTS = 40;
    float localPPG[SEND_PTS];
    int   localIdx;
    portENTER_CRITICAL(&ppgMux);
    localIdx = (int)ppgWriteIdx;
    for (int i = 0; i < SEND_PTS; i++) {
        int idx = (localIdx - SEND_PTS + i + PPG_BUFFER_SIZE) % PPG_BUFFER_SIZE;
        localPPG[i] = ppgBuffer[idx];
    }
    portEXIT_CRITICAL(&ppgMux);

    String ppgJson = "[";
    for (int i = 0; i < SEND_PTS; i++) {
        ppgJson += String(localPPG[i], 1);
        if (i < SEND_PTS - 1) ppgJson += ",";
    }
    ppgJson += "]";

    String json = "{";
    json += "\"hr\":"     + String(beatAvg)          + ",";
    json += "\"spo2\":"   + String(currentSpO2)       + ",";
    json += "\"bat\":"    + String(batteryPercent, 1)  + ",";
    json += "\"volt\":"   + String(batteryVoltage, 3)  + ",";
    json += "\"fall\":"   + String(fallConfirmed  ? 1:0) + ",";
    json += "\"impact\":" + String(impactDetected ? 1:0) + ",";
    json += "\"manual\":" + String(isManualMode   ? 1:0) + ",";
    json += "\"accel\":"  + String(filteredAccel, 3)   + ",";
    json += "\"angle\":"  + String(angle, 2)            + ",";
    json += "\"baseHR\":" + String(baselineHR)          + ",";
    json += "\"hrMode\":" + String((int)hrState)        + ",";
    json += "\"ppg\":"    + ppgJson;
    json += "}";

    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

static void handleRoot() {
    server.send_P(200, "text/html", DASHBOARD_HTML);
}

static void handleNotFound() {
    server.send(404, "text/plain", "Not found");
}

// =========================================================
// Public API
// =========================================================
void initWebDashboard() {
    server.on("/",     handleRoot);
    server.on("/data", handleData);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("[WEB] Dashboard server started on port 80.");
    Serial.print("[WEB] URL: http://"); Serial.println(WiFi.localIP());
}

void handleWebDashboard() {
    server.handleClient();
}
