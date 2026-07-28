//! Minimal local input surface used by the vendored tokenizer core.
//!
//! The upstream crate's file, parquet, compression, and Hub loaders are
//! intentionally not part of the Samosa adapter.  The pretokenizer only needs
//! these two small types for bounded in-memory encoding and its reference
//! oracle.

#[derive(Debug, Clone, Copy)]
pub(crate) struct DocRef<'a>(pub &'a [u8]);

impl<'a> From<&'a [u8]> for DocRef<'a> {
    fn from(value: &'a [u8]) -> Self { Self(value) }
}

impl<'a> std::ops::Deref for DocRef<'a> {
    type Target = &'a [u8];
    fn deref(&self) -> &Self::Target { &self.0 }
}

pub trait Resource: Sync {
    fn as_bytes(&self) -> &[u8];

    fn par_document_chunks<'a>(&'a self, separator: &'a [u8], n: usize)
        -> Vec<DocumentIter<'a>> {
        par_document_chunks(self.as_bytes(), separator, n)
    }
}

impl Resource for [u8] { fn as_bytes(&self) -> &[u8] { self } }
impl Resource for Vec<u8> { fn as_bytes(&self) -> &[u8] { self } }
impl Resource for str { fn as_bytes(&self) -> &[u8] { self.as_bytes() } }
impl Resource for String { fn as_bytes(&self) -> &[u8] { self.as_bytes() } }

pub struct DocumentIter<'a> {
    bytes: &'a [u8], separator: &'a [u8], position: usize, finished: bool,
}

impl<'a> DocumentIter<'a> {
    pub fn new(bytes: &'a [u8], separator: &'a [u8]) -> Self {
        Self { bytes, separator, position: 0, finished: false }
    }
}

impl<'a> Iterator for DocumentIter<'a> {
    type Item = &'a [u8];
    fn next(&mut self) -> Option<Self::Item> {
        while !self.finished {
            if self.position >= self.bytes.len() { self.finished = true; return None; }
            if self.separator.is_empty() {
                self.finished = true;
                return Some(&self.bytes[self.position..]);
            }
            let rest = &self.bytes[self.position..];
            if let Some(off) = memchr::memmem::find(rest, self.separator) {
                let out = &rest[..off];
                self.position += off + self.separator.len();
                if !out.is_empty() { return Some(out); }
            } else {
                self.finished = true;
                if !rest.is_empty() { return Some(rest); }
            }
        }
        None
    }
}

fn par_document_chunks<'a>(bytes: &'a [u8], separator: &'a [u8], n: usize)
    -> Vec<DocumentIter<'a>> {
    if n <= 1 || bytes.is_empty() || separator.is_empty() {
        return vec![DocumentIter::new(bytes, separator)];
    }
    let mut starts = vec![0usize];
    for i in 1..n {
        let target = i * bytes.len() / n;
        if let Some(off) = memchr::memmem::find(&bytes[target..], separator) {
            starts.push(target + off + separator.len());
        }
    }
    starts.push(bytes.len());
    starts.dedup();
    starts.windows(2).map(|w| DocumentIter {
        bytes: &bytes[w[0]..w[1]], separator, position: 0, finished: false,
    }).collect()
}
