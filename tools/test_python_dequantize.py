import mlx.core as mx

d = mx.load('models/maple/model-00001-of-00003.safetensors')
w = d['model.word_embeddings.weight']
s = d['model.word_embeddings.scales']
b = d['model.word_embeddings.biases']

x = mx.array([10, 20, 30])
w_x = mx.take(w, x, 0)
s_x = mx.take(s, x, 0)
b_x = mx.take(b, x, 0)

dq = mx.dequantize(w_x, scales=s_x, biases=b_x, group_size=64, bits=4)
print("Mean:", mx.mean(mx.abs(dq)).item())
