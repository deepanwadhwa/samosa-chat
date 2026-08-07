import subprocess
import os
import sys

# Try to import mlx_lm, skip if not available
try:
    import mlx.core as mx
    from mlx_lm import load, generate_step
except ImportError:
    print("mlx_lm not available, skipping parity test (this is normal in CI).")
    sys.exit(0)

# 10 diverse prompts
prompts = [
    "Hello world!",
    "What is the capital of France?",
    "Write a short poem about a cat.",
    "Summarize the plot of Romeo and Juliet in one sentence.",
    "Translate 'Good morning' to Spanish.",
    "Explain quantum computing simply.",
    "How do you make a chocolate cake?",
    "Who won the World Cup in 2018?",
    "What are the primary colors?",
    "List three benefits of regular exercise."
]

model_path = "models/maple"
if not os.path.exists(os.path.join(model_path, "config.json")):
    print("Real model not installed at models/maple. Parity test skipped.")
    sys.exit(0)

print(f"Loading MLX reference model from {model_path}...")
model, tokenizer = load(model_path)

build_dir = os.environ.get("BUILD_DIR", "build")
samosa_exe = os.path.join(build_dir, "samosa-maple")

if not os.path.exists(samosa_exe):
    print(f"Native binary {samosa_exe} not found. Build it first.")
    sys.exit(1)

success = True

for i, prompt in enumerate(prompts):
    print(f"Testing prompt {i+1}/10: {prompt}")
    
    # 1. Generate with native samosa-maple
    try:
        res = subprocess.run(
            [samosa_exe, "--model-dir", model_path, "--prompt", prompt, "--max-tokens", "32"],
            capture_output=True, text=True, check=True
        )
    except subprocess.CalledProcessError as e:
        print("samosa-maple crashed!")
        print(e.stderr)
        sys.exit(1)
        
    native_tokens = []
    for line in res.stdout.splitlines():
        if line.startswith("TOKENS: "):
            tok_str = line.replace("TOKENS: ", "").strip()
            if tok_str:
                native_tokens = [int(x) for x in tok_str.split(",")]
    
    # 2. Generate with mlx_lm (reference)
    messages = [{"role": "user", "content": prompt}]
    formatted_prompt = tokenizer.apply_chat_template(messages, tokenize=False, add_generation_prompt=True)
    input_ids = tokenizer.encode(formatted_prompt)
    
    ref_tokens = []
    # generate_step yields (token, logprob)
    # We just run for 32 steps
    generator = generate_step(mx.array(input_ids), model)
    for _ in range(32):
        try:
            token, _ = next(generator)
            ref_tokens.append(token.item())
        except StopIteration:
            break
            
    print(f"  Reference tokens: {ref_tokens}")
    print(f"  Native tokens:    {native_tokens}")
    
    if ref_tokens == native_tokens:
        print("  MATCH")
    else:
        print("  MISMATCH!")
        success = False

if success:
    print("All 10 prompts matched exactly!")
    # create stamp file
    with open(".maple_real_validation_passed", "w") as f:
        f.write("OK")
    sys.exit(0)
else:
    print("E2E Parity failed!")
    if os.path.exists(".maple_real_validation_passed"):
        os.remove(".maple_real_validation_passed")
    sys.exit(1)
