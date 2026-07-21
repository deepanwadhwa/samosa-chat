import urllib.request
import json
import base64
from PIL import Image
import io

# Create a small red image
img = Image.new('RGB', (128, 128), color = 'red')
buf = io.BytesIO()
img.save(buf, format='PNG')
img_b64 = base64.b64encode(buf.getvalue()).decode("utf-8")

payload = {
    "messages": [
        {
            "role": "user",
            "content": [
                {
                    "type": "text",
                    "text": "What color is this image?"
                },
                {
                    "type": "image_url",
                    "image_url": {
                        "url": f"data:image/png;base64,{img_b64}"
                    }
                }
            ]
        }
    ],
    "max_tokens": 100
}

req = urllib.request.Request(
    "http://127.0.0.1:8642/v1/chat/completions",
    data=json.dumps(payload).encode("utf-8"),
    headers={"Content-Type": "application/json"}
)

try:
    with urllib.request.urlopen(req) as resp:
        print(resp.read().decode("utf-8"))
except Exception as e:
    print("Error:", e)
    if hasattr(e, "read"):
        print(e.read().decode("utf-8"))

