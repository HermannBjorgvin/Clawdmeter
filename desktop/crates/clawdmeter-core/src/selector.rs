//! Which account to show when several are configured.
//!
//! The account whose session % rose most recently wins. A
//! rise stamps a monotonic poll counter, so the choice is sticky and a window
//! reset (a drop) is not activity. Before any rise, the highest current
//! session % wins.

use std::collections::HashMap;
use std::hash::Hash;

/// Sticky "most recently moved" selector.
#[derive(Debug, Clone)]
pub struct PlanSelector<K> {
    prev: HashMap<K, u32>,
    last_active: HashMap<K, u64>,
    seq: u64,
}

impl<K> Default for PlanSelector<K> {
    fn default() -> Self {
        PlanSelector { prev: HashMap::new(), last_active: HashMap::new(), seq: 0 }
    }
}

impl<K: Clone + Eq + Hash + Ord> PlanSelector<K> {
    /// Update from this cycle's `{account: session %}` and return the active
    /// account. `None` only when `sessions` is empty. Ties beyond recency and
    /// % fall to the smallest key so the result is deterministic.
    pub fn choose(&mut self, sessions: &[(K, u32)]) -> Option<K> {
        self.seq += 1;
        for (k, s) in sessions {
            if self.prev.get(k).is_some_and(|p| s > p) {
                self.last_active.insert(k.clone(), self.seq);
            }
            self.prev.insert(k.clone(), *s);
        }
        sessions
            .iter()
            .max_by(|(ka, sa), (kb, sb)| {
                let ra = self.last_active.get(ka).copied().unwrap_or(0);
                let rb = self.last_active.get(kb).copied().unwrap_or(0);
                ra.cmp(&rb).then(sa.cmp(sb)).then(kb.cmp(ka))
            })
            .map(|(k, _)| k.clone())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const A: &str = "/a";
    const B: &str = "/b";

    #[test]
    fn startup_picks_highest() {
        let mut s = PlanSelector::default();
        assert_eq!(s.choose(&[(A, 10), (B, 30)]), Some(B));
    }

    #[test]
    fn switches_on_rise() {
        let mut s = PlanSelector::default();
        s.choose(&[(A, 10), (B, 30)]);
        assert_eq!(s.choose(&[(A, 20), (B, 30)]), Some(A));
    }

    #[test]
    fn sticky_when_no_movement() {
        let mut s = PlanSelector::default();
        s.choose(&[(A, 10), (B, 30)]);
        s.choose(&[(A, 20), (B, 30)]);
        assert_eq!(s.choose(&[(A, 20), (B, 30)]), Some(A));
    }

    #[test]
    fn reset_to_zero_is_not_activity() {
        let mut s = PlanSelector::default();
        s.choose(&[(A, 10), (B, 30)]);
        s.choose(&[(A, 20), (B, 30)]);
        s.choose(&[(A, 20), (B, 45)]);
        assert_eq!(s.choose(&[(A, 20), (B, 0)]), Some(B));
    }

    #[test]
    fn larger_value_wins_same_cycle() {
        let mut s = PlanSelector::default();
        s.choose(&[(A, 10), (B, 10)]);
        assert_eq!(s.choose(&[(A, 12), (B, 40)]), Some(B));
    }

    #[test]
    fn empty_and_single() {
        let mut s: PlanSelector<&str> = PlanSelector::default();
        assert_eq!(s.choose(&[]), None);
        assert_eq!(s.choose(&[(A, 0)]), Some(A));
    }
}
