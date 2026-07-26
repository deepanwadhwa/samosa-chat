import re

with open('assets/app.html', 'r') as f:
    html = f.read()

old_ready = """      <!-- Ready State -->
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
      </div>"""

new_ready = """      <!-- Ready State -->
      <div class="chutni-state" id="chutni-ready">
        <div class="chutni-card">
          <h3>Research (Ready)</h3>
          <p>Connected and up to date. Last built today.</p>
          
          <table style="width: 100%; border-collapse: collapse; margin-top: 12px; font-size: 13px;">
            <tr style="border-bottom: 1px solid var(--border)">
              <td style="padding: 6px 0; color: var(--text-muted)">Files Indexed</td>
              <td style="padding: 6px 0; text-align: right; font-weight: 500">80</td>
            </tr>
            <tr style="border-bottom: 1px solid var(--border)">
              <td style="padding: 6px 0; color: var(--text-muted)">Files Skipped</td>
              <td style="padding: 6px 0; text-align: right; font-weight: 500">20</td>
            </tr>
            <tr style="border-bottom: 1px solid var(--border)">
              <td style="padding: 6px 0; color: var(--text-muted)">Chunks Created</td>
              <td style="padding: 6px 0; text-align: right; font-weight: 500">420</td>
            </tr>
            <tr style="border-bottom: 1px solid var(--border)">
              <td style="padding: 6px 0; color: var(--text-muted)">Index Memory</td>
              <td style="padding: 6px 0; text-align: right; font-weight: 500">5.6 KB</td>
            </tr>
            <tr>
              <td style="padding: 6px 0; color: var(--text-muted)">Current Phase</td>
              <td style="padding: 6px 0; text-align: right; font-weight: 500">improving</td>
            </tr>
          </table>
          
          <div style="margin-top: 16px; display: flex; gap: 8px;">
            <button class="btn-secondary" onclick="chutniSetState('forget-confirmation')">Forget Folder</button>
            <button class="btn-secondary" onclick="chutniSetState('disconnected')">Mock Disconnect</button>
          </div>
        </div>
      </div>"""

html = html.replace(old_ready, new_ready)

with open('assets/app.html', 'w') as f:
    f.write(html)
