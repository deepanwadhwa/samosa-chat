import mlx.core as mx
import mlx_lm
import json
import numpy as np

model, tokenizer = mlx_lm.load("models/maple")

with open("tests/fixtures/maple/greedy_parity_refs.json", "r") as f:
    lines = f.readlines()

# Let's take the first failing prompt: Hello world!
# We know it fails at decode pos 26.
# Let's rebuild the EXACT prefix of the golden generation up to that point.
prompt = "Hello world!"
expected = [264, 1032, 330, 28165, 330, 3192, 11, 22129, 323, 279, 13732, 287, 856, 1729, 311, 576, 264, 4016, 286, 2038, 279, 5025, 279, 11463, 11, 279, 264, 11463, 11, 323, 279, 14099]

formatted = tokenizer.apply_chat_template([{"role": "user", "content": prompt}], tokenize=False, add_generation_prompt=True)
input_ids = tokenizer.encode(formatted)

pos = 26
prefix = input_ids + expected[:pos]

print(f"Prefix length: {len(prefix)}")

x = mx.array([prefix], dtype=mx.int32)
logits = model(x)
mx.eval(logits)

next_logits = logits[0, -1, :]
next_t = mx.argmax(next_logits, axis=-1).item()

print(f"Python next token: {next_t}")
np.save("tests/fixtures/maple/hello_world_pos26_logits.npy", np.array(next_logits))



        next_logits = logits[0, -1, :]
        next_t = mx.argmax(next_logits, axis=-1).item()

        print(f"Python next token: {next_t}")

        # Let's save these python logits to compare with C++!
        np.save("tests/fixtures/maple/hello_world_pos26_logits.npy", np.array(next_logits))
        break
