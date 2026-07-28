use crate::pretokenize::pretoken::Pretoken;
use rayon::prelude::*;
use std::{
    collections::HashMap,
    hash::{BuildHasher, Hash},
    ops::AddAssign,
};

pub(crate) trait PretokenCountable<'a, S: BuildHasher + Default> {
    fn pretoken_count(self) -> HashMap<Pretoken<'a>, usize, S>;
}

impl<'a, T, S> PretokenCountable<'a, S> for T
where
    T: Iterator<Item = Pretoken<'a>>,
    S: BuildHasher + Default,
{
    fn pretoken_count(self) -> HashMap<Pretoken<'a>, usize, S> {
        self.fold(HashMap::default(), |mut counts, token| {
            *counts.entry(token).or_default() += 1;
            counts
        })
    }
}

pub(crate) trait ParallelMergeCounts<K, V, S> {
    fn par_merge_counts(self) -> HashMap<K, V, S>;
}

impl<T, K, V, S> ParallelMergeCounts<K, V, S> for T
where
    T: ParallelIterator<Item = HashMap<K, V, S>>,
    K: Eq + Hash,
    V: AddAssign + Default,
    S: BuildHasher + Default,
{
    fn par_merge_counts(self) -> HashMap<K, V, S> {
        self.reduce(HashMap::default, |mut acc, counts| {
            if acc.is_empty() {
                return counts;
            }

            for (k, v) in counts {
                *acc.entry(k).or_default() += v;
            }
            acc
        })
    }
}
