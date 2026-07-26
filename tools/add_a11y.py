import re
with open('assets/app.html', 'r') as f:
    html = f.read()

# Add word-break to chutni metrics to prevent overflow
html = html.replace('.metric-value { font-size: 14px; font-weight: 600; margin-top: 4px; }', 
                   '.metric-value { font-size: 14px; font-weight: 600; margin-top: 4px; word-break: break-all; overflow-wrap: anywhere; }')

# Add modal trapping and a11y roles
html = html.replace('<aside class="chutni-panel" id="chutniPanel" aria-hidden="true">',
                   '<aside class="chutni-panel" id="chutniPanel" aria-hidden="true" role="dialog" aria-label="Chutni Library">')

with open('assets/app.html', 'w') as f:
    f.write(html)
