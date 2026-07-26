import re

with open('assets/app.html', 'r') as f:
    content = f.read()

# 1. Add CSS
css = """
      /* Chutni UI Styles */
      .chutni-panel { position: fixed; top: 0; right: -400px; width: 400px; height: 100vh; background: var(--surface); box-shadow: -4px 0 24px rgba(0,0,0,0.1); transition: right 0.3s ease; z-index: 100; display: flex; flex-direction: column; overflow-y: auto; }
      .chutni-panel.open { right: 0; }
      .chutni-head { padding: 16px 20px; border-bottom: 1px solid var(--border); display: flex; justify-content: space-between; align-items: center; }
      .chutni-head h2 { margin: 0; font-size: 16px; font-weight: 600; }
      .chutni-content { padding: 20px; flex: 1; }
      .chutni-state { display: none; }
      .chutni-state.active { display: block; }
      
      .chutni-card { background: var(--surface-hover); border: 1px solid var(--border); border-radius: 8px; padding: 16px; margin-bottom: 16px; }
      .chutni-card h3 { margin: 0 0 8px 0; font-size: 14px; font-weight: 600; }
      .chutni-card p { margin: 0 0 12px 0; font-size: 13px; color: var(--text-muted); }
      
      .chutni-metrics { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-top: 12px; }
      .metric-box { background: var(--surface); padding: 10px; border-radius: 6px; border: 1px solid var(--border); }
      .metric-label { font-size: 11px; color: var(--text-muted); text-transform: uppercase; letter-spacing: 0.5px; }
      .metric-value { font-size: 14px; font-weight: 600; margin-top: 4px; }
      
      .progress-bar { height: 6px; background: var(--border); border-radius: 3px; overflow: hidden; margin-top: 12px; }
      .progress-fill { height: 100%; background: var(--accent); transition: width 0.3s; }
      .progress-indeterminate { background: linear-gradient(90deg, transparent, var(--accent), transparent); width: 50%; animation: slide 1.5s infinite linear; }
      @keyframes slide { from { transform: translateX(-100%); } to { transform: translateX(200%); } }
      
      .state-icon { margin-bottom: 12px; }
      .btn-primary { background: var(--accent); color: white; border: none; padding: 8px 16px; border-radius: 6px; cursor: pointer; font-size: 13px; font-weight: 500; }
      .btn-secondary { background: var(--surface-hover); color: var(--text); border: 1px solid var(--border); padding: 8px 16px; border-radius: 6px; cursor: pointer; font-size: 13px; font-weight: 500; }
      .btn-danger { background: var(--warn); color: white; border: none; padding: 8px 16px; border-radius: 6px; cursor: pointer; font-size: 13px; font-weight: 500; }
"""
content = content.replace('</style>', css + '\n    </style>')

# 2. Add Button to Topbar
btn_html = '<button class="icon-button" id="chutniButton" aria-label="Open Chutni Library">Library</button>\n          <button class="icon-button" id="settingsButton"'
content = content.replace('<button class="icon-button" id="settingsButton"', btn_html)

# 3. Add HTML Panel
panel_html = """
  <aside class="chutni-panel" id="chutniPanel" aria-hidden="true">
    <div class="chutni-head"><h2>Chutni Library</h2><button class="close-settings" id="closeChutni" aria-label="Close library">Done</button></div>
    <div class="chutni-content" id="chutniStates">
      
      <!-- Empty State -->
      <div class="chutni-state" id="chutni-empty">
        <div class="chutni-card">
          <div class="state-icon">📁</div>
          <h3>No Local Folders</h3>
          <p>Grant access to a folder to build a private search index. Chutni works entirely offline.</p>
          <button class="btn-primary" onclick="chutniSetState('add')">Add Folder…</button>
        </div>
      </div>
      
      <!-- Add State -->
      <div class="chutni-state" id="chutni-add">
        <div class="chutni-card">
          <h3>Authorize Access</h3>
          <p>Select a folder on your computer to scan.</p>
          <button class="btn-secondary" onclick="chutniSetState('preflight')">Mock Folder Select</button>
          <button class="btn-secondary" onclick="chutniSetState('empty')">Cancel</button>
        </div>
      </div>
      
      <!-- Preflight State -->
      <div class="chutni-state" id="chutni-preflight">
        <div class="chutni-card">
          <h3>Review Summary</h3>
          <p>Found 420 chunks in 100 files.</p>
          <div class="chutni-metrics">
            <div class="metric-box"><div class="metric-label">Size</div><div class="metric-value">1,234 bytes</div></div>
            <div class="metric-box"><div class="metric-label">Files</div><div class="metric-value">100</div></div>
          </div>
          <div style="margin-top: 12px">
            <button class="btn-primary" onclick="chutniSetState('building')">Start Building</button>
            <button class="btn-secondary" onclick="chutniSetState('add')">Back</button>
          </div>
        </div>
      </div>
      
      <!-- Building State -->
      <div class="chutni-state" id="chutni-building">
        <div class="chutni-card">
          <h3>Building Index</h3>
          <p>Reading and summarizing your files...</p>
          <div class="progress-bar"><div class="progress-fill" style="width: 42%"></div></div>
          <div class="chutni-metrics">
            <div class="metric-box"><div class="metric-label">Progress</div><div class="metric-value">180 / 420</div></div>
            <div class="metric-box"><div class="metric-label">Phase</div><div class="metric-value">improving</div></div>
          </div>
          <button class="btn-secondary" style="margin-top:12px" onclick="chutniSetState('paused')">Pause</button>
        </div>
      </div>
      
      <!-- Ready State -->
      <div class="chutni-state" id="chutni-ready">
        <div class="chutni-card">
          <h3>Research (Ready)</h3>
          <p>Connected and up to date.</p>
          <div class="chutni-metrics">
            <div class="metric-box"><div class="metric-label">Chunks</div><div class="metric-value">420</div></div>
            <div class="metric-box"><div class="metric-label">Index Size</div><div class="metric-value">5.6 KB</div></div>
          </div>
          <button class="btn-secondary" style="margin-top:12px" onclick="chutniSetState('forget-confirmation')">Forget Folder</button>
          <button class="btn-secondary" style="margin-top:12px" onclick="chutniSetState('disconnected')">Mock Disconnect</button>
        </div>
      </div>
      
      <!-- Paused State -->
      <div class="chutni-state" id="chutni-paused">
        <div class="chutni-card">
          <h3>Paused</h3>
          <p>Indexing is currently paused.</p>
          <div class="progress-bar"><div class="progress-fill" style="width: 42%; background: var(--text-muted)"></div></div>
          <button class="btn-primary" style="margin-top:12px" onclick="chutniSetState('building')">Resume</button>
        </div>
      </div>
      
      <!-- Permission State -->
      <div class="chutni-state" id="chutni-permission">
        <div class="chutni-card">
          <div class="state-icon" style="color:var(--warn)">🔒</div>
          <h3>Permission Required</h3>
          <p>Samosa lost access to this folder. Please reauthorize it.</p>
          <button class="btn-primary" onclick="chutniSetState('ready')">Reauthorize</button>
        </div>
      </div>
      
      <!-- Disconnected State -->
      <div class="chutni-state" id="chutni-disconnected">
        <div class="chutni-card">
          <div class="state-icon" style="color:var(--warn)">🔌</div>
          <h3>Disconnected</h3>
          <p>The external drive or network share is not mounted.</p>
          <button class="btn-secondary" onclick="chutniSetState('ready')">Check Again</button>
        </div>
      </div>
      
      <!-- Attention State -->
      <div class="chutni-state" id="chutni-attention">
        <div class="chutni-card">
          <div class="state-icon" style="color:var(--warn)">⚠️</div>
          <h3>Action Required</h3>
          <p>A fatal error occurred during processing.</p>
          <button class="btn-secondary" onclick="chutniSetState('ready')">Dismiss</button>
        </div>
      </div>
      
      <!-- Forget Confirmation State -->
      <div class="chutni-state" id="chutni-forget-confirmation">
        <div class="chutni-card">
          <h3>Forget this folder?</h3>
          <p>This will delete the Chutni index for "Research". Your original files will not be changed.</p>
          <button class="btn-danger" onclick="chutniSetState('empty')">Yes, forget it</button>
          <button class="btn-secondary" onclick="chutniSetState('ready')">Cancel</button>
        </div>
      </div>

    </div>
    
    <!-- State testing panel (bottom) -->
    <div style="padding: 10px; border-top: 1px solid var(--border); display: flex; flex-wrap: wrap; gap: 4px;">
      <span style="font-size:10px; width:100%; color:var(--text-muted)">T3.3 Mock States:</span>
      <button onclick="chutniSetState('empty')" style="font-size:10px">Empty</button>
      <button onclick="chutniSetState('add')" style="font-size:10px">Add</button>
      <button onclick="chutniSetState('preflight')" style="font-size:10px">Preflight</button>
      <button onclick="chutniSetState('building')" style="font-size:10px">Building</button>
      <button onclick="chutniSetState('ready')" style="font-size:10px">Ready</button>
      <button onclick="chutniSetState('paused')" style="font-size:10px">Paused</button>
      <button onclick="chutniSetState('permission')" style="font-size:10px">Permission</button>
      <button onclick="chutniSetState('disconnected')" style="font-size:10px">Disconnected</button>
      <button onclick="chutniSetState('attention')" style="font-size:10px">Attention</button>
    </div>
  </aside>
"""
content = content.replace('  <div class="scrim" id="scrim"></div>', '  <div class="scrim" id="scrim"></div>\n' + panel_html)

# 4. Add JS
js = """
      function openChutni() { els.chutniPanel.classList.add("open"); els.chutniPanel.setAttribute("aria-hidden","false"); els.scrim.classList.add("show"); }
      function closeChutni() { els.chutniPanel.classList.remove("open"); els.chutniPanel.setAttribute("aria-hidden","true"); els.scrim.classList.remove("show"); }
      window.chutniSetState = function(state) {
        document.querySelectorAll('.chutni-state').forEach(el => el.classList.remove('active'));
        const target = document.getElementById('chutni-' + state);
        if (target) target.classList.add('active');
      };
      
      // Initialize state
      chutniSetState('empty');
"""
# insert JS
content = content.replace('function openSettings() {', js + '\n      function openSettings() {')

# Hook up events
js_events = """
      els.chutniButton = document.getElementById('chutniButton');
      els.chutniPanel = document.getElementById('chutniPanel');
      els.closeChutni = document.getElementById('closeChutni');
      if(els.chutniButton) els.chutniButton.onclick = openChutni;
      if(els.closeChutni) els.closeChutni.onclick = closeChutni;
"""
content = content.replace('$("settingsButton").onclick=openSettings;', js_events + '      $("settingsButton").onclick=openSettings;')
content = content.replace('els.sidebar.classList.remove("open"); els.scrim.classList.remove("show");', 'els.sidebar.classList.remove("open"); if(els.chutniPanel) els.chutniPanel.classList.remove("open"); els.scrim.classList.remove("show");')

with open('assets/app.html', 'w') as f:
    f.write(content)
