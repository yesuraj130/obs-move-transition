import http from 'http';
import fs from 'fs';
import path from 'path';

const PORT = 3000;
const HOST = '0.0.0.0';

const server = http.createServer((req, res) => {
  res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
  res.end(`<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>OBS Move Source Switcher Filter</title>
  <style>
    :root {
      --bg: #0f172a;
      --panel: #1e293b;
      --accent: #38bdf8;
      --text: #f8fafc;
      --muted: #94a3b8;
    }
    body {
      margin: 0;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      background: var(--bg);
      color: var(--text);
      display: flex;
      flex-direction: column;
      align-items: center;
      padding: 24px;
    }
    h1 {
      margin: 0 0 8px 0;
      font-size: 24px;
      color: var(--accent);
    }
    p {
      color: var(--muted);
      margin-top: 0;
    }
    .container {
      max-width: 900px;
      width: 100%;
      display: grid;
      grid-template-columns: 1fr 320px;
      gap: 20px;
    }
    .canvas-box {
      background: #020617;
      border: 1px solid #334155;
      border-radius: 8px;
      position: relative;
      aspect-ratio: 16/9;
      overflow: hidden;
    }
    .layer {
      position: absolute;
      border-radius: 6px;
      display: flex;
      align-items: center;
      justify-content: center;
      font-weight: bold;
      transition: all 0.5s cubic-bezier(0.4, 0, 0.2, 1);
    }
    .bg-layer {
      inset: 0;
      background: radial-gradient(circle at 50% 50%, #1e1b4b 0%, #090d16 100%);
      color: #475569;
      font-size: 14px;
    }
    .cam-layer {
      box-shadow: 0 4px 12px rgba(0,0,0,0.5);
      border: 2px solid rgba(255,255,255,0.2);
    }
    .panel {
      background: var(--panel);
      padding: 16px;
      border-radius: 8px;
      border: 1px solid #334155;
    }
    .panel h2 {
      font-size: 16px;
      margin-top: 0;
      border-bottom: 1px solid #334155;
      padding-bottom: 8px;
    }
    .item-btn {
      display: flex;
      align-items: center;
      justify-content: space-between;
      width: 100%;
      background: #0f172a;
      color: var(--text);
      border: 1px solid #334155;
      padding: 10px 14px;
      border-radius: 6px;
      margin-bottom: 8px;
      cursor: pointer;
      font-size: 14px;
    }
    .item-btn:hover {
      border-color: var(--accent);
    }
    .item-btn.active {
      border-color: #22c55e;
      background: #14532d33;
    }
    .tag {
      font-size: 11px;
      background: #334155;
      padding: 2px 6px;
      border-radius: 4px;
      color: #cbd5e1;
    }
    .log-box {
      margin-top: 16px;
      background: #020617;
      padding: 10px;
      border-radius: 6px;
      font-family: monospace;
      font-size: 11px;
      color: #38bdf8;
      height: 120px;
      overflow-y: auto;
    }
  </style>
</head>
<body>
  <h1>OBS Move Source Switcher Filter</h1>
  <p>Interactive Architecture Simulator & Code Verification Workspace</p>

  <div class="container">
    <div>
      <div class="canvas-box">
        <div class="layer bg-layer">OBS Parent Scene Background (Preserved during Switch)</div>
        <div id="cam" class="layer cam-layer" style="background: #3b82f6; width: 35%; height: 35%; top: 10%; left: 10%;">
          <span id="cam-label">Camera 1 (Top)</span>
        </div>
      </div>
      <div class="log-box" id="logs">
        [READY] Filter initialized. Move Transition private sub-source created.<br>
        [READY] Native obs_scene_duplicate pipeline ready.<br>
        [INFO] Selective hijack active: Parent scene rendering preserved.
      </div>
    </div>

    <div class="panel">
      <h2>Managed Scene Items</h2>
      <button class="item-btn active" onclick="switchTo(1, 'Camera 1 (Top)', '10%', '10%', '35%', '35%', '#3b82f6')">
        <span>👁️ Camera 1 (Top)</span>
        <span class="tag">Layer 1</span>
      </button>
      <button class="item-btn" onclick="switchTo(2, 'Camera 1 (Bottom)', '55%', '55%', '35%', '35%', '#8b5cf6')">
        <span>👁️ Camera 1 (Bottom)</span>
        <span class="tag">Layer 2</span>
      </button>
      <button class="item-btn" onclick="switchTo(3, 'Camera 2 (Center)', '25%', '25%', '50%', '50%', '#ec4899')">
        <span>👁️ Camera 2 (Center)</span>
        <span class="tag">Layer 3</span>
      </button>

      <div style="margin-top: 20px; font-size: 12px; color: var(--muted); line-height: 1.5;">
        <strong>Features implemented in C:</strong><br>
        • Native <code>obs_scene_duplicate</code> wrapper cloning<br>
        • Selective render hijack (0% double-render)<br>
        • Dual support: Source Clone & Raw Layer Duplicates<br>
        • Exact layer-order isolation<br>
        • Dynamic <code>item_add</code> & parent signal lifecycle
      </div>
    </div>
  </div>

  <script>
    function log(msg) {
      const box = document.getElementById('logs');
      box.innerHTML += '<br>' + msg;
      box.scrollTop = box.scrollHeight;
    }

    function switchTo(id, name, top, left, w, h, bg) {
      const btns = document.querySelectorAll('.item-btn');
      btns.forEach((b, i) => b.classList.toggle('active', i === id - 1));

      const cam = document.getElementById('cam');
      log('[TRANSITION START] start_switcher_transition -> Target: ' + name + ' (Layer ' + id + ')');
      log('[HIJACK] video_render: Rendering move_transition surface only.');

      cam.style.top = top;
      cam.style.left = left;
      cam.style.width = w;
      cam.style.height = h;
      cam.style.background = bg;
      document.getElementById('cam-label').innerText = name;

      setTimeout(() => {
        log('[TRANSITION END] t >= 1.0f: Wrapper scenes cleared. Outgoing eye turned off.');
        log('[NORMAL PASS] obs_source_skip_video_filter active.');
      }, 500);
    }
  </script>
</body>
</html>`);
});

server.listen(PORT, HOST, () => {
  console.log(\`Dev server running at http://\${HOST}:\${PORT}\`);
});
