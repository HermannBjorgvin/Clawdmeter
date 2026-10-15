//! A small line diff for the Hooks tab's "here is what will change" preview.

/// One line of a diff.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DiffLine {
    /// Unchanged.
    Same(String),
    /// Only in the new text.
    Added(String),
    /// Only in the old text.
    Removed(String),
}

/// LCS line diff. settings.json files are small, so O(n·m) is fine; inputs
/// past a few thousand lines fall back to "all removed, all added".
pub fn line_diff(old: &str, new: &str) -> Vec<DiffLine> {
    let a: Vec<&str> = old.lines().collect();
    let b: Vec<&str> = new.lines().collect();
    if a.len().saturating_mul(b.len()) > 4_000_000 {
        return a
            .iter()
            .map(|l| DiffLine::Removed((*l).into()))
            .chain(b.iter().map(|l| DiffLine::Added((*l).into())))
            .collect();
    }
    let (n, m) = (a.len(), b.len());
    let mut lcs = vec![vec![0u32; m + 1]; n + 1];
    for i in (0..n).rev() {
        for j in (0..m).rev() {
            lcs[i][j] = if a[i] == b[j] { lcs[i + 1][j + 1] + 1 } else { lcs[i + 1][j].max(lcs[i][j + 1]) };
        }
    }
    let (mut i, mut j) = (0, 0);
    let mut out = Vec::with_capacity(n.max(m));
    while i < n && j < m {
        if a[i] == b[j] {
            out.push(DiffLine::Same(a[i].into()));
            i += 1;
            j += 1;
        } else if lcs[i + 1][j] >= lcs[i][j + 1] {
            out.push(DiffLine::Removed(a[i].into()));
            i += 1;
        } else {
            out.push(DiffLine::Added(b[j].into()));
            j += 1;
        }
    }
    out.extend(a[i..].iter().map(|l| DiffLine::Removed((*l).into())));
    out.extend(b[j..].iter().map(|l| DiffLine::Added((*l).into())));
    out
}

/// Keep changed lines plus `context` unchanged lines around them; runs of
/// hidden lines become a single `Same("…")`.
pub fn with_context(lines: Vec<DiffLine>, context: usize) -> Vec<DiffLine> {
    let changed: Vec<bool> = lines.iter().map(|l| !matches!(l, DiffLine::Same(_))).collect();
    let keep = |i: usize| {
        let lo = i.saturating_sub(context);
        let hi = (i + context + 1).min(lines.len());
        changed[lo..hi].iter().any(|&c| c)
    };
    let mut out = Vec::new();
    let mut skipping = false;
    for (i, l) in lines.iter().enumerate() {
        if keep(i) {
            out.push(l.clone());
            skipping = false;
        } else if !skipping {
            out.push(DiffLine::Same("…".into()));
            skipping = true;
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn adds_and_removes() {
        let d = line_diff("a\nb\nc\n", "a\nx\nc\nd\n");
        assert_eq!(
            d,
            vec![
                DiffLine::Same("a".into()),
                DiffLine::Removed("b".into()),
                DiffLine::Added("x".into()),
                DiffLine::Same("c".into()),
                DiffLine::Added("d".into()),
            ]
        );
    }

    #[test]
    fn context_folds_unchanged_runs() {
        let old: String = (0..20).map(|i| format!("{i}\n")).collect();
        let new = old.replace("10\n", "ten\n");
        let d = with_context(line_diff(&old, &new), 1);
        assert_eq!(d.first(), Some(&DiffLine::Same("…".into())));
        assert!(d.contains(&DiffLine::Added("ten".into())));
        assert!(d.len() <= 7);
    }
}
