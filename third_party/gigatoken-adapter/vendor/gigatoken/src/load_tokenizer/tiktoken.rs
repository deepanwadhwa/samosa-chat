use crate::bpe::Tokenizer;
use crate::pretokenize::PretokenizerType;
use eyre::{Context, Result, ensure};
use std::path::Path;

/// The base64-per-line mergeable ranks of a .tiktoken/tiktoken.model file,
/// in rank order (merges are reconstructed from this list).
fn load_tiktoken_ranks(file_path: impl AsRef<Path>) -> Result<Vec<Vec<u8>>> {
    use base64::engine::general_purpose::STANDARD as BASE64_STANDARD;
    use base64::prelude::*;
    use std::io::Read;
    let mut buf = String::new();
    std::fs::File::open(&file_path)
        .with_context(|| format!("Failed to read {}", file_path.as_ref().display()))?
        .read_to_string(&mut buf)?;
    buf.lines()
        .enumerate()
        .map(|(i, line)| {
            let (base64_token, id_str) = line
                .split_once(' ')
                .ok_or_else(|| eyre::eyre!("line {i} has no rank field"))?;
            let id = id_str.trim().parse::<u32>()?;
            ensure!(id == i as u32, "rank {id} at line {i}: ranks must be dense");
            Ok(BASE64_STANDARD.decode(base64_token)?)
        })
        .collect()
}

/// Load a tokenizer from a tiktoken rank file with the pretokenizer scheme
/// and special tokens (`(content, id)`) supplied by the caller. A .tiktoken
/// file carries neither — its split regex and specials live in the code
/// that defines the encoding — and a wrong scheme silently changes every
/// encode, so neither may be defaulted here.
pub fn load_tiktoken(
    file_path: impl AsRef<Path>,
    pretokenizer: PretokenizerType,
    special_tokens: Vec<(String, u32)>,
) -> Result<Tokenizer> {
    let rank_vocab = load_tiktoken_ranks(file_path)?;
    let n_ranks = rank_vocab.len() as u32;
    let mut tokenizer = Tokenizer::from_ranks(rank_vocab)?;
    tokenizer.set_pretokenizer_type(pretokenizer);
    for (content, id) in &special_tokens {
        ensure!(
            *id >= n_ranks,
            "special token {content:?} (id {id}) overlaps the {n_ranks} mergeable ranks"
        );
    }
    tokenizer.add_special_tokens(
        special_tokens
            .into_iter()
            .map(|(content, id)| (content.into_bytes(), id.into())),
    );
    Ok(tokenizer)
}
