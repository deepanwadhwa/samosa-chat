import mlx.core as mx
import mlx_lm
import json

model_path = "models/maple"
model, tokenizer = mlx_lm.load(model_path)

prompt = "The quick brown fox jumps over the lazy dog"
tokens = tokenizer.encode(prompt)
x = mx.array([tokens])

# We can intercept intermediate outputs
h = model.model.word_embeddings(x)
save_dict = {"x": x, "emb": h}

# Let's run just the first layer
cache = [None] * len(model.model.layers)
mask = mlx_lm.models.base.create_attention_mask(h, cache[0])
h_layer0 = model.model.layers[0](h, mask=mask, cache=cache[0])
save_dict["h_layer0"] = h_layer0

h_layer1 = model.model.layers[1](h_layer0, mask=mask, cache=cache[1])
save_dict["h_layer1"] = h_layer1

mx.save_safetensors("tests/fixtures/maple/gate_c_debug.safetensors", save_dict)
print("Debug fixture generated!")
