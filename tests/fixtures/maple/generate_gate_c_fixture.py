import mlx.core as mx
import mlx_lm
import json
import os
import sys

# Load real model
model_path = "models/maple"
model, tokenizer = mlx_lm.load(model_path)

prompt = "The quick brown fox jumps over the lazy dog"
tokens = tokenizer.encode(prompt)
x = mx.array([tokens])

# Get first-token logits
logits = model(x)
first_token_logits = logits[:, -1, :]

# Save fixture
save_dict = {
    "x": x,
    "expected_logits": first_token_logits
}
mx.save_safetensors("tests/fixtures/maple/gate_c_fixture.safetensors", save_dict)
print("Gate C fixture generated successfully!")
