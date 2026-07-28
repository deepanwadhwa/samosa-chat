//! Samosa's local-only Gigatoken adapter.
//!
//! GTK1/v2 is a private, bounded, little-endian protocol.  The adapter never
//! opens a user path other than the verified tokenizer passed at startup and
//! never loads a repository ID or URL.  The gateway owns process supervision,
//! admission, timeouts, and the final tokenizer/model activation decision.

use eyre::{Result, eyre};
use samosa_gigatoken_core::bpe::tiktoken::Tokenizer;
use samosa_gigatoken_core::load_tokenizer::hf::load_hf_bpe;
use samosa_gigatoken_core::load_tokenizer::tiktoken::load_tiktoken;
use samosa_gigatoken_core::pretokenize::PretokenizerType;
use serde::Deserialize;
use std::collections::BTreeMap;
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::thread;

const MAGIC: u32 = 0x3154_4B47; // "GTK1" in little-endian order
const VERSION: u16 = 2;
const HEADER: usize = 12;
const MAX_FRAME: usize = 8 * 1024 * 1024;
const MAX_TEXT: usize = 2 * 1024 * 1024;
const MAX_DOCS: u32 = 4096;
const MAX_SEGMENTS: u32 = 4096;
const MAX_TOKENIZER: u64 = 256 * 1024 * 1024;
const MAX_TOKENS_PER_REQUEST: u64 = 4 * 1024 * 1024;
const KIMI_RESERVED_SPECIALS: u32 = 258;
const UPSTREAM_COMMIT: &str = "34a1599f0c0ae7d7cd0d1c530e6522320158b360";

const OP_HEALTH: u16 = 1;
const OP_ENCODE_BATCH: u16 = 2;
const OP_ENCODE_PROMPT: u16 = 3;
const OP_CANCEL: u16 = 4;
const OP_SHUTDOWN: u16 = 5;
const OP_RESULT: u16 = 0x8001;
const OP_ERROR: u16 = 0x8002;

const ERR_BAD_FRAME: u16 = 1;
const ERR_INVALID_REQUEST: u16 = 2;
const ERR_FINGERPRINT_MISMATCH: u16 = 3;
const ERR_INVALID_UTF8: u16 = 4;
const ERR_LIMIT_EXCEEDED: u16 = 5;
const ERR_UNKNOWN_OPERATION: u16 = 6;
const ERR_CANCELED: u16 = 7;
const ERR_BUSY: u16 = 8;
const ERR_SHUTTING_DOWN: u16 = 9;

#[derive(Clone)]
struct AdapterInfo {
    tokenizer_sha256: [u8; 32],
    vocab_size: u32,
}

#[derive(Deserialize)]
struct HfModel {
    vocab: BTreeMap<String, u32>,
}

#[derive(Deserialize)]
struct HfAddedToken {
    id: u32,
}

#[derive(Deserialize)]
struct HfTokenizer {
    model: HfModel,
    #[serde(default)]
    added_tokens: Vec<HfAddedToken>,
}

#[derive(Clone)]
struct RequestMeta {
    request_id: u64,
    build_commit: String,
    model_id: String,
    model_version: String,
    tokenizer_sha256: [u8; 32],
    vocab_size: u32,
    policy_fingerprint: Vec<u8>,
    source_sha256: [u8; 32],
    item_count: u32,
    total_input_bytes: u64,
    max_output_tokens: u64,
    max_output_bytes: u64,
}

struct Frame {
    op: u16,
    payload: Vec<u8>,
}

fn read_exact_or_eof<R: Read>(r: &mut R, buf: &mut [u8]) -> io::Result<bool> {
    let mut got = 0;
    while got < buf.len() {
        let n = r.read(&mut buf[got..])?;
        if n == 0 { return Ok(got == 0); }
        got += n;
    }
    Ok(false)
}

fn read_frame<R: Read>(r: &mut R) -> Result<Option<Frame>> {
    let mut h = [0u8; HEADER];
    if read_exact_or_eof(r, &mut h).map_err(|e| eyre!("read frame: {e}"))? { return Ok(None); }
    let magic = u32::from_le_bytes(h[0..4].try_into().unwrap());
    let version = u16::from_le_bytes(h[4..6].try_into().unwrap());
    let op = u16::from_le_bytes(h[6..8].try_into().unwrap());
    let len = u32::from_le_bytes(h[8..12].try_into().unwrap()) as usize;
    if magic != MAGIC { return Err(eyre!("bad frame magic")); }
    if version != VERSION { return Err(eyre!("unsupported frame version {version}")); }
    if len > MAX_FRAME { return Err(eyre!("frame exceeds {MAX_FRAME} bytes")); }
    let mut payload = vec![0u8; len];
    r.read_exact(&mut payload).map_err(|e| eyre!("read frame payload: {e}"))?;
    Ok(Some(Frame { op, payload }))
}

fn write_frame<W: Write>(w: &mut W, op: u16, payload: &[u8]) -> Result<()> {
    if payload.len() > MAX_FRAME { return Err(eyre!("response exceeds frame limit")); }
    let mut h = [0u8; HEADER];
    h[0..4].copy_from_slice(&MAGIC.to_le_bytes());
    h[4..6].copy_from_slice(&VERSION.to_le_bytes());
    h[6..8].copy_from_slice(&op.to_le_bytes());
    h[8..12].copy_from_slice(&(payload.len() as u32).to_le_bytes());
    w.write_all(&h)?;
    w.write_all(payload)?;
    w.flush()?;
    Ok(())
}

fn put_u16(out: &mut Vec<u8>, value: u16) { out.extend_from_slice(&value.to_le_bytes()); }
fn put_u32(out: &mut Vec<u8>, value: u32) { out.extend_from_slice(&value.to_le_bytes()); }
fn put_u64(out: &mut Vec<u8>, value: u64) { out.extend_from_slice(&value.to_le_bytes()); }

fn take<'a>(input: &'a [u8], pos: &mut usize, len: usize) -> Result<&'a [u8]> {
    if len > input.len().saturating_sub(*pos) { return Err(eyre!("truncated request")); }
    let out = &input[*pos..*pos + len];
    *pos += len;
    Ok(out)
}

fn take_u16(input: &[u8], pos: &mut usize) -> Result<u16> { Ok(u16::from_le_bytes(take(input, pos, 2)?.try_into().unwrap())) }
fn take_u32(input: &[u8], pos: &mut usize) -> Result<u32> { Ok(u32::from_le_bytes(take(input, pos, 4)?.try_into().unwrap())) }
fn take_u64(input: &[u8], pos: &mut usize) -> Result<u64> { Ok(u64::from_le_bytes(take(input, pos, 8)?.try_into().unwrap())) }

fn take_string(input: &[u8], pos: &mut usize, max: usize) -> Result<String> {
    let len = take_u16(input, pos)? as usize;
    if len > max { return Err(eyre!("string exceeds limit")); }
    let bytes = take(input, pos, len)?;
    Ok(std::str::from_utf8(bytes).map_err(|_| eyre!("string is not UTF-8"))?.to_owned())
}

fn take_digest(input: &[u8], pos: &mut usize) -> Result<[u8; 32]> {
    Ok(take(input, pos, 32)?.try_into().unwrap())
}

fn parse_meta(payload: &[u8]) -> Result<(RequestMeta, usize)> {
    let mut p = 0;
    let request_id = take_u64(payload, &mut p)?;
    if request_id == 0 { return Err(eyre!("request ID must be nonzero")); }
    let build_commit = take_string(payload, &mut p, 128)?;
    let model_id = take_string(payload, &mut p, 128)?;
    let model_version = take_string(payload, &mut p, 128)?;
    let tokenizer_sha256 = take_digest(payload, &mut p)?;
    let vocab_size = take_u32(payload, &mut p)?;
    let policy_fingerprint = {
        let len = take_u16(payload, &mut p)? as usize;
        if len > 1024 { return Err(eyre!("policy fingerprint exceeds limit")); }
        take(payload, &mut p, len)?.to_vec()
    };
    let source_sha256 = take_digest(payload, &mut p)?;
    let item_count = take_u32(payload, &mut p)?;
    let total_input_bytes = take_u64(payload, &mut p)?;
    let max_output_tokens = take_u64(payload, &mut p)?;
    let max_output_bytes = take_u64(payload, &mut p)?;
    if item_count > MAX_DOCS || total_input_bytes > MAX_TEXT as u64 * MAX_DOCS as u64 ||
       max_output_tokens > MAX_TOKENS_PER_REQUEST || max_output_bytes > MAX_FRAME as u64 {
        return Err(eyre!("request ceiling exceeded"));
    }
    Ok((RequestMeta { request_id, build_commit, model_id, model_version,
                      tokenizer_sha256, vocab_size, policy_fingerprint,
                      source_sha256, item_count, total_input_bytes,
                      max_output_tokens, max_output_bytes }, p))
}

struct Sha256 {
    state: [u32; 8],
    bits: u64,
    block: [u8; 64],
    used: usize,
}

impl Sha256 {
    fn new() -> Self {
        Self { state: [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19],
               bits: 0, block: [0; 64], used: 0 }
    }

    fn update(&mut self, data: &[u8]) {
        self.bits = self.bits.wrapping_add((data.len() as u64) * 8);
        let mut rest = data;
        while !rest.is_empty() {
            let n = (64 - self.used).min(rest.len());
            self.block[self.used..self.used + n].copy_from_slice(&rest[..n]);
            self.used += n;
            rest = &rest[n..];
            if self.used == 64 { self.compress(); self.used = 0; }
        }
    }

    fn compress(&mut self) {
        const K: [u32; 64] = [
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
        ];
        let mut w = [0u32; 64];
        for i in 0..16 { let p = i * 4; w[i] = u32::from_be_bytes(self.block[p..p + 4].try_into().unwrap()); }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16].wrapping_add(s0).wrapping_add(w[i - 7]).wrapping_add(s1);
        }
        let mut a = self.state[0]; let mut b = self.state[1]; let mut c = self.state[2]; let mut d = self.state[3];
        let mut e = self.state[4]; let mut f = self.state[5]; let mut g = self.state[6]; let mut h = self.state[7];
        for i in 0..64 {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let ch = (e & f) ^ ((!e) & g);
            let t1 = h.wrapping_add(s1).wrapping_add(ch).wrapping_add(K[i]).wrapping_add(w[i]);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let maj = (a & b) ^ (a & c) ^ (b & c);
            let t2 = s0.wrapping_add(maj);
            h = g; g = f; f = e; e = d.wrapping_add(t1); d = c; c = b; b = a; a = t1.wrapping_add(t2);
        }
        self.state[0] = self.state[0].wrapping_add(a); self.state[1] = self.state[1].wrapping_add(b);
        self.state[2] = self.state[2].wrapping_add(c); self.state[3] = self.state[3].wrapping_add(d);
        self.state[4] = self.state[4].wrapping_add(e); self.state[5] = self.state[5].wrapping_add(f);
        self.state[6] = self.state[6].wrapping_add(g); self.state[7] = self.state[7].wrapping_add(h);
    }

    fn finalize(mut self) -> [u8; 32] {
        self.block[self.used] = 0x80; self.used += 1;
        if self.used > 56 { self.block[self.used..].fill(0); self.compress(); self.used = 0; }
        self.block[self.used..56].fill(0);
        self.block[56..64].copy_from_slice(&self.bits.to_be_bytes());
        self.compress();
        let mut out = [0u8; 32];
        for (i, word) in self.state.iter().enumerate() { out[i * 4..i * 4 + 4].copy_from_slice(&word.to_be_bytes()); }
        out
    }
}

fn digest_bytes(bytes: &[u8]) -> [u8; 32] {
    let mut h = Sha256::new(); h.update(bytes); h.finalize()
}

fn sha256_file(path: &Path) -> Result<[u8; 32]> {
    let data = std::fs::read(path)?;
    Ok(digest_bytes(&data))
}

fn error_payload(request_id: u64, code: u16, retryable: bool, detail: &str) -> Vec<u8> {
    let detail = detail.as_bytes();
    let bounded = &detail[..detail.len().min(MAX_FRAME.saturating_sub(20))];
    let mut out = Vec::with_capacity(20 + bounded.len());
    put_u64(&mut out, request_id);
    put_u16(&mut out, code);
    put_u16(&mut out, retryable as u16);
    put_u32(&mut out, bounded.len() as u32);
    out.extend_from_slice(bounded);
    out
}

fn result_prefix(request_id: u64) -> Vec<u8> {
    let mut out = Vec::with_capacity(8);
    put_u64(&mut out, request_id);
    out
}

fn validate_meta(meta: &RequestMeta, info: &AdapterInfo) -> Result<()> {
    if meta.build_commit != UPSTREAM_COMMIT { return Err(eyre!("gigatoken commit mismatch")); }
    if meta.model_id.is_empty() || meta.model_version.is_empty() { return Err(eyre!("model identity is required")); }
    if meta.tokenizer_sha256 != info.tokenizer_sha256 { return Err(eyre!("tokenizer SHA-256 mismatch")); }
    if meta.vocab_size != info.vocab_size { return Err(eyre!("vocabulary size mismatch")); }
    if meta.policy_fingerprint.is_empty() { return Err(eyre!("policy fingerprint is required")); }
    Ok(())
}

fn encode_document(tokenizer: &mut Tokenizer, text: &[u8], out: &mut Vec<u32>) -> Result<()> {
    if text.len() > MAX_TEXT { return Err(eyre!("document exceeds text limit")); }
    let text = std::str::from_utf8(text).map_err(|_| eyre!("text is not valid UTF-8"))?;
    let pt = tokenizer.pretokenizer_type();
    tokenizer.memoized_encode_flat(pt.pretokenize(text.as_bytes()), out);
    Ok(())
}

fn encode_prompt_segment(tokenizer: &mut Tokenizer, trusted: bool, text: &[u8], out: &mut Vec<u32>) -> Result<()> {
    if text.len() > MAX_TEXT { return Err(eyre!("prompt segment exceeds text limit")); }
    let text = std::str::from_utf8(text).map_err(|_| eyre!("text is not valid UTF-8"))?;
    if trusted {
        tokenizer.encode_with_added_tokens_flat(text.as_bytes(), out);
    } else {
        encode_document(tokenizer, text.as_bytes(), out)?;
    }
    Ok(())
}

fn check_output(meta: &RequestMeta, ids: &[u32]) -> Result<()> {
    if ids.len() as u64 > meta.max_output_tokens { return Err(eyre!("token output exceeds request ceiling")); }
    if ids.iter().any(|id| *id >= meta.vocab_size) { return Err(eyre!("token ID outside declared vocabulary")); }
    let bytes = ids.len().checked_mul(4).ok_or_else(|| eyre!("token output overflow"))?;
    if bytes as u64 > meta.max_output_bytes || bytes > MAX_FRAME.saturating_sub(24) {
        return Err(eyre!("token output exceeds frame ceiling"));
    }
    Ok(())
}

fn parse_batch(meta: &RequestMeta, body: &[u8]) -> Result<Vec<Vec<u8>>> {
    let mut p = 0;
    let count = take_u32(body, &mut p)?;
    if count != meta.item_count || count > MAX_DOCS { return Err(eyre!("document count mismatch")); }
    let mut total = 0u64;
    let mut docs = Vec::with_capacity(count as usize);
    for _ in 0..count {
        let len = take_u32(body, &mut p)? as usize;
        if len > MAX_TEXT { return Err(eyre!("document exceeds text limit")); }
        let bytes = take(body, &mut p, len)?.to_vec();
        std::str::from_utf8(&bytes).map_err(|_| eyre!("text is not valid UTF-8"))?;
        total = total.checked_add(len as u64).ok_or_else(|| eyre!("input size overflow"))?;
        docs.push(bytes);
    }
    if p != body.len() || total != meta.total_input_bytes { return Err(eyre!("input byte ceiling mismatch")); }
    let mut h = Sha256::new();
    for doc in &docs { h.update(&(doc.len() as u32).to_le_bytes()); h.update(doc); }
    if h.finalize() != meta.source_sha256 { return Err(eyre!("source SHA-256 mismatch")); }
    Ok(docs)
}

fn parse_prompt(meta: &RequestMeta, body: &[u8]) -> Result<Vec<(bool, Vec<u8>)>> {
    let mut p = 0;
    let count = take_u32(body, &mut p)?;
    if count != meta.item_count || count > MAX_SEGMENTS { return Err(eyre!("segment count mismatch")); }
    let mut total = 0u64;
    let mut segments = Vec::with_capacity(count as usize);
    let mut h = Sha256::new();
    for _ in 0..count {
        let kind = *take(body, &mut p, 1)?.first().unwrap();
        if kind > 1 { return Err(eyre!("unknown prompt segment type")); }
        let len = take_u32(body, &mut p)? as usize;
        if len > MAX_TEXT { return Err(eyre!("prompt segment exceeds text limit")); }
        let bytes = take(body, &mut p, len)?.to_vec();
        std::str::from_utf8(&bytes).map_err(|_| eyre!("text is not valid UTF-8"))?;
        total = total.checked_add(len as u64).ok_or_else(|| eyre!("input size overflow"))?;
        h.update(&[kind]); h.update(&(len as u32).to_le_bytes()); h.update(&bytes);
        segments.push((kind == 0, bytes));
    }
    if p != body.len() || total != meta.total_input_bytes { return Err(eyre!("input byte ceiling mismatch")); }
    if h.finalize() != meta.source_sha256 { return Err(eyre!("source SHA-256 mismatch")); }
    Ok(segments)
}

fn encode_batch(tokenizer: &mut Tokenizer, meta: &RequestMeta, body: &[u8], canceled: &AtomicU64) -> Result<Vec<u8>> {
    let docs = parse_batch(meta, body)?;
    let mut lengths = Vec::with_capacity(docs.len());
    let mut ids = Vec::new();
    for doc in docs {
        if canceled.load(Ordering::Relaxed) == meta.request_id { return Err(eyre!("canceled")); }
        let before = ids.len();
        encode_document(tokenizer, &doc, &mut ids)?;
        lengths.push((ids.len() - before) as u32);
        check_output(meta, &ids)?;
    }
    let mut out = result_prefix(meta.request_id);
    put_u32(&mut out, lengths.len() as u32);
    put_u32(&mut out, ids.len() as u32);
    for len in lengths { put_u32(&mut out, len); }
    for id in ids { put_u32(&mut out, id); }
    Ok(out)
}

fn encode_prompt(tokenizer: &mut Tokenizer, meta: &RequestMeta, body: &[u8], canceled: &AtomicU64) -> Result<Vec<u8>> {
    let segments = parse_prompt(meta, body)?;
    let mut ids = Vec::new();
    for (trusted, text) in segments {
        if canceled.load(Ordering::Relaxed) == meta.request_id { return Err(eyre!("canceled")); }
        encode_prompt_segment(tokenizer, trusted, &text, &mut ids)?;
        check_output(meta, &ids)?;
    }
    let mut out = result_prefix(meta.request_id);
    put_u32(&mut out, ids.len() as u32);
    for id in ids { put_u32(&mut out, id); }
    Ok(out)
}

fn load_kimi(path: &PathBuf) -> Result<(Tokenizer, u32)> {
    let rank_data = std::fs::read(path)?;
    std::str::from_utf8(&rank_data).map_err(|_| eyre!("tiktoken.model is not UTF-8"))?;
    let rank_count = rank_data.split(|b| *b == b'\n').filter(|line| !line.is_empty()).count() as u32;
    if rank_count == 0 { return Err(eyre!("Kimi tiktoken.model has no ranks")); }
    let config_path = path.parent().unwrap_or_else(|| Path::new(".")).join("tokenizer_config.json");
    let config_data = std::fs::read(&config_path)
        .map_err(|e| eyre!("Kimi tokenizer_config.json is required next to tiktoken.model: {e}"))?;
    let config: KimiTokenizerConfig = sonic_rs::from_slice(&config_data)?;
    let mut specials = BTreeMap::<u32, String>::new();
    for id in rank_count..rank_count.saturating_add(KIMI_RESERVED_SPECIALS) {
        specials.insert(id, format!("<|reserved_token_{id}|>"));
    }
    let mut max_id = rank_count.saturating_add(KIMI_RESERVED_SPECIALS).saturating_sub(1);
    for (id_text, token) in config.added_tokens_decoder {
        let id: u32 = id_text.parse().map_err(|_| eyre!("invalid Kimi added-token id {id_text:?}"))?;
        if id < rank_count { return Err(eyre!("Kimi added-token id {id} overlaps mergeable ranks")); }
        max_id = max_id.max(id);
        specials.insert(id, token.content);
    }
    let entries = specials.into_iter().map(|(id, content)| (content, id)).collect();
    Ok((load_tiktoken(path, PretokenizerType::Kimi, entries)?, max_id + 1))
}

#[derive(Deserialize)]
struct KimiAddedToken { content: String }
#[derive(Deserialize)]
struct KimiTokenizerConfig { #[serde(default)] added_tokens_decoder: BTreeMap<String, KimiAddedToken> }

fn load_tokenizer(path: &Path) -> Result<(Tokenizer, u32)> {
    if path.file_name().and_then(|s| s.to_str()) == Some("tiktoken.model") { return load_kimi(&path.to_path_buf()); }
    let data = std::fs::read(path)?;
    let parsed: HfTokenizer = sonic_rs::from_slice(&data)?;
    let mut max_id = parsed.model.vocab.values().copied().max().unwrap_or(0);
    for token in &parsed.added_tokens { max_id = max_id.max(token.id); }
    Ok((load_hf_bpe(path)?, max_id + 1))
}

fn health_payload(request_id: u64, info: &AdapterInfo) -> Vec<u8> {
    let mut out = result_prefix(request_id);
    put_u32(&mut out, info.vocab_size);
    out.extend_from_slice(&info.tokenizer_sha256);
    put_u16(&mut out, UPSTREAM_COMMIT.len() as u16);
    out.extend_from_slice(UPSTREAM_COMMIT.as_bytes());
    out
}

fn response_error(writer: &Arc<Mutex<io::BufWriter<io::Stdout>>>, request_id: u64, code: u16, retryable: bool, detail: &str) {
    if let Ok(mut w) = writer.lock() { let _ = write_frame(&mut *w, OP_ERROR, &error_payload(request_id, code, retryable, detail)); }
}

fn main() -> Result<()> {
    let mut args = std::env::args_os().skip(1);
    let tokenizer_path = PathBuf::from(args.next().ok_or_else(|| eyre!("usage: samosa-gigatoken-adapter TOKENIZER_JSON|TIKTOKEN_MODEL"))?);
    if args.next().is_some() { return Err(eyre!("unexpected argument")); }
    let metadata = std::fs::metadata(&tokenizer_path)?;
    if !metadata.is_file() || metadata.len() > MAX_TOKENIZER { return Err(eyre!("tokenizer path is not a bounded regular file")); }
    let tokenizer_sha256 = sha256_file(&tokenizer_path)?;
    let (tokenizer, vocab_size) = load_tokenizer(&tokenizer_path)?;
    let info = AdapterInfo { tokenizer_sha256, vocab_size };
    let tokenizer = Arc::new(Mutex::new(tokenizer));
    let canceled_request = Arc::new(AtomicU64::new(0));
    let shutting_down = Arc::new(AtomicBool::new(false));
    let stdout = Arc::new(Mutex::new(io::BufWriter::new(io::stdout())));
    {
        let mut w = stdout.lock().map_err(|_| eyre!("stdout lock poisoned"))?;
        let mut ready = result_prefix(0);
        ready.extend_from_slice(b"samosa-gigatoken/2\0local-only\0");
        write_frame(&mut *w, OP_RESULT, &ready)?;
    }
    let mut active: Option<(u64, thread::JoinHandle<()>)> = None;
    let mut stdin = io::stdin().lock();
    while let Some(frame) = read_frame(&mut stdin)? {
        if let Some((request_id, handle)) = active.take() {
            if handle.is_finished() { let _ = handle.join(); }
            else { active = Some((request_id, handle)); }
        }
        let parsed = parse_meta(&frame.payload);
        let (meta, body_pos) = match parsed {
            Ok(value) => value,
            Err(e) => { response_error(&stdout, 0, ERR_BAD_FRAME, false, &e.to_string()); continue; }
        };
        if shutting_down.load(Ordering::Relaxed) {
            response_error(&stdout, meta.request_id, ERR_SHUTTING_DOWN, true, "adapter is shutting down");
            continue;
        }
        if let Some((active_id, _)) = active.as_ref() {
            let active_target = *active_id;
            if frame.op == OP_CANCEL {
                let mut p = body_pos;
                let target = take_u64(&frame.payload, &mut p).unwrap_or(0);
                if target == active_target { canceled_request.store(target, Ordering::Release); }
                let mut out = result_prefix(meta.request_id); put_u64(&mut out, target);
                if let Ok(mut w) = stdout.lock() { write_frame(&mut *w, OP_RESULT, &out)?; }
            } else if frame.op == OP_SHUTDOWN {
                shutting_down.store(true, Ordering::Release);
                canceled_request.store(active_target, Ordering::Release);
                if let Some((_, handle)) = active.take() { let _ = handle.join(); }
                let mut out = result_prefix(meta.request_id); out.extend_from_slice(b"shutdown");
                if let Ok(mut w) = stdout.lock() { write_frame(&mut *w, OP_RESULT, &out)?; }
                break;
            } else {
                response_error(&stdout, meta.request_id, ERR_BUSY, true, "another encode request is active");
            }
            continue;
        }
        if frame.op == OP_CANCEL {
            response_error(&stdout, meta.request_id, ERR_INVALID_REQUEST, false, "no encode request is active");
            continue;
        }
        if frame.op != OP_HEALTH && frame.op != OP_SHUTDOWN {
            if let Err(e) = validate_meta(&meta, &info) {
                response_error(&stdout, meta.request_id, ERR_FINGERPRINT_MISMATCH, false, &e.to_string());
                continue;
            }
        }
        if frame.op == OP_HEALTH {
            if let Ok(mut w) = stdout.lock() { write_frame(&mut *w, OP_RESULT, &health_payload(meta.request_id, &info))?; }
            continue;
        }
        if frame.op == OP_SHUTDOWN {
            shutting_down.store(true, Ordering::Release);
            let mut out = result_prefix(meta.request_id); out.extend_from_slice(b"shutdown");
            if let Ok(mut w) = stdout.lock() { write_frame(&mut *w, OP_RESULT, &out)?; }
            break;
        }
        let body = frame.payload[body_pos..].to_vec();
        let tokenizer_ref = Arc::clone(&tokenizer);
        let stdout_ref = Arc::clone(&stdout);
        let cancel_ref = Arc::clone(&canceled_request);
        cancel_ref.store(0, Ordering::Release);
        let request_id = meta.request_id;
        let handle = thread::spawn(move || {
            let result = tokenizer_ref.lock().map_err(|_| eyre!("tokenizer lock poisoned")).and_then(|mut tok| {
                if frame.op == OP_ENCODE_BATCH { encode_batch(&mut tok, &meta, &body, &cancel_ref) }
                else if frame.op == OP_ENCODE_PROMPT { encode_prompt(&mut tok, &meta, &body, &cancel_ref) }
                else { Err(eyre!("unknown operation")) }
            });
            match result {
                Ok(payload) => if let Ok(mut w) = stdout_ref.lock() { let _ = write_frame(&mut *w, OP_RESULT, &payload); },
                Err(e) => {
                    let (code, retryable) = if cancel_ref.load(Ordering::Acquire) == request_id || e.to_string() == "canceled" { (ERR_CANCELED, true) }
                        else if e.to_string() == "unknown operation" { (ERR_UNKNOWN_OPERATION, false) }
                        else if e.to_string().contains("UTF-8") { (ERR_INVALID_UTF8, false) }
                        else if e.to_string().contains("ceiling") || e.to_string().contains("limit") { (ERR_LIMIT_EXCEEDED, false) }
                        else if e.to_string().contains("SHA-256") || e.to_string().contains("mismatch") { (ERR_FINGERPRINT_MISMATCH, false) }
                        else { (ERR_INVALID_REQUEST, false) };
                    response_error(&stdout_ref, request_id, code, retryable, &e.to_string());
                }
            }
        });
        active = Some((request_id, handle));
    }
    if let Some((request_id, handle)) = active {
        canceled_request.store(request_id, Ordering::Release);
        let _ = handle.join();
    }
    Ok(())
}
