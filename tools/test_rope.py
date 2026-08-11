import mlx.core as mx

q1 = mx.random.normal((1, 9, 8, 128))
# transposed before rope
q2 = q1.transpose(0, 2, 1, 3)

r1 = mx.fast.rope(q1, 64, traditional=False, base=10000.0, scale=1.0, offset=0)
r1 = r1.transpose(0, 2, 1, 3)

r2 = mx.fast.rope(q2, 64, traditional=False, base=10000.0, scale=1.0, offset=0)

print(mx.max(mx.abs(r1 - r2)).item())
