//! GitHub Releases: find the release this app should flash and its
//! `manifest.json`, download and verify images, and remember when we last
//! checked (once a day).
//!
//! `releases/latest` never returns a prerelease, so a stable app is only
//! offered stable firmware. An app that is itself a prerelease (its version
//! has a `-`, like `2.0.0-rc.2`) asks for its own tag first, so it flashes
//! the firmware built with it, and falls back to the latest release
//! ([`release_for_app`]).

use std::collections::HashMap;
use std::fs;
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};

use serde::Deserialize;
use sha2::{Digest, Sha256};

use crate::manifest::{BoardImage, Manifest};

/// Asset name CI publishes.
pub const MANIFEST_ASSET: &str = "manifest.json";
/// Update checks run at most this often.
pub const CHECK_INTERVAL_SECS: i64 = 24 * 3600;

/// Errors from the release side. `Display` is written for people; the
/// technical detail, where there is one, is in [`ReleaseError::detail`].
#[derive(Debug)]
pub enum ReleaseError {
    /// `releases/latest` (or the app's own tag) is 404: nothing published
    /// yet (or no such repo).
    NoRelease,
    /// No network, DNS failure, refused or timed-out connection.
    Offline(String),
    /// GitHub's API rate limit (HTTP 403/429).
    RateLimited,
    /// Any other HTTP failure.
    Http(String),
    /// The release has no `manifest.json`, or it doesn't parse.
    Manifest(String),
    /// The manifest names a file the release doesn't have.
    MissingAsset(String),
    /// Downloaded bytes don't match the manifest's sha256.
    HashMismatch {
        /// Asset name.
        file: String,
        /// Manifest hash.
        expected: String,
        /// Hash of what we got.
        actual: String,
    },
    /// Local file I/O.
    Io(io::Error),
}

impl ReleaseError {
    /// The technical detail behind the message, for logs.
    pub fn detail(&self) -> Option<String> {
        match self {
            ReleaseError::Offline(d) | ReleaseError::Http(d) | ReleaseError::Manifest(d) => Some(d.clone()),
            ReleaseError::HashMismatch { expected, actual, .. } => Some(format!("expected {expected}, got {actual}")),
            ReleaseError::Io(e) => Some(e.to_string()),
            _ => None,
        }
    }

    fn from_ureq(url: &str, e: ureq::Error) -> Self {
        match e {
            ureq::Error::StatusCode(404) if url.ends_with("/releases/latest") || url.contains("/releases/tags/") => {
                ReleaseError::NoRelease
            }
            ureq::Error::StatusCode(403 | 429) => ReleaseError::RateLimited,
            ureq::Error::StatusCode(c) => ReleaseError::Http(format!("HTTP {c} from {url}")),
            ureq::Error::HostNotFound
            | ureq::Error::ConnectionFailed
            | ureq::Error::Timeout(_)
            | ureq::Error::Io(_) => ReleaseError::Offline(format!("{url}: {e}")),
            other => ReleaseError::Http(format!("{url}: {other}")),
        }
    }
}

impl std::fmt::Display for ReleaseError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ReleaseError::NoRelease => write!(f, "No firmware release was found."),
            ReleaseError::Offline(_) => {
                write!(f, "Couldn't reach GitHub. Check your internet connection and try again.")
            }
            ReleaseError::RateLimited => {
                write!(f, "GitHub is limiting requests from this network. Try again later.")
            }
            ReleaseError::Http(_) => write!(f, "Couldn't read the release response from GitHub. Try again later."),
            ReleaseError::Manifest(_) => {
                write!(f, "Couldn't read this release's manifest.json. Check that the app supports this release.")
            }
            ReleaseError::MissingAsset(n) => write!(f, "This release is missing {n}."),
            ReleaseError::HashMismatch { file, .. } => {
                write!(f, "Checksum mismatch for {file}. Download it again before flashing.")
            }
            ReleaseError::Io(e) => match e.kind() {
                io::ErrorKind::PermissionDenied => write!(f, "No permission to read or write that file."),
                io::ErrorKind::NotFound => write!(f, "File not found."),
                _ => write!(f, "{e}"),
            },
        }
    }
}

impl std::error::Error for ReleaseError {}

impl From<io::Error> for ReleaseError {
    fn from(e: io::Error) -> Self {
        ReleaseError::Io(e)
    }
}

#[derive(Debug, Deserialize)]
struct GhRelease {
    tag_name: String,
    assets: Vec<GhAsset>,
}

#[derive(Debug, Deserialize)]
struct GhAsset {
    name: String,
    browser_download_url: String,
}

/// A release with a parsed manifest.
#[derive(Debug, Clone)]
pub struct Release {
    /// Git tag, e.g. `v1.9.0`.
    pub tag: String,
    /// Its manifest.
    pub manifest: Manifest,
    /// Asset name → download URL.
    pub assets: HashMap<String, String>,
}

const USER_AGENT: &str = concat!("clawdmeter/", env!("CARGO_PKG_VERSION"));

/// The `releases/latest` API URL for `repo`.
pub fn latest_url(repo: &str) -> String {
    format!("https://api.github.com/repos/{repo}/releases/latest")
}

fn get(url: &str) -> Result<ureq::http::Response<ureq::Body>, ReleaseError> {
    ureq::get(url)
        .header("User-Agent", USER_AGENT)
        .header("Accept", "application/vnd.github+json, application/octet-stream, */*")
        .call()
        .map_err(|e| ReleaseError::from_ureq(url, e))
}

fn get_string(url: &str) -> Result<String, ReleaseError> {
    get(url)?.body_mut().read_to_string().map_err(|e| ReleaseError::Offline(format!("{url}: {e}")))
}

/// Turn a GitHub "latest release" API response into a [`Release`], given a
/// fetcher for the manifest asset (split out for tests).
fn release_from_api(
    api_json: &str,
    fetch: impl Fn(&str) -> Result<String, ReleaseError>,
) -> Result<Release, ReleaseError> {
    let gh: GhRelease = serde_json::from_str(api_json).map_err(|e| ReleaseError::Manifest(e.to_string()))?;
    let assets: HashMap<String, String> = gh.assets.into_iter().map(|a| (a.name, a.browser_download_url)).collect();
    let url = assets.get(MANIFEST_ASSET).ok_or_else(|| ReleaseError::MissingAsset(MANIFEST_ASSET.into()))?;
    let manifest = Manifest::parse(&fetch(url)?).map_err(|e| ReleaseError::Manifest(e.to_string()))?;
    Ok(Release { tag: gh.tag_name, manifest, assets })
}

/// The `releases/tags/<tag>` API URL for `repo`.
pub fn tag_url(repo: &str, tag: &str) -> String {
    format!("https://api.github.com/repos/{repo}/releases/tags/{tag}")
}

/// Fetch the latest release of `repo` (`owner/name`).
pub fn latest_release(repo: &str) -> Result<Release, ReleaseError> {
    let api = get_string(&latest_url(repo))?;
    release_from_api(&api, get_string)
}

/// A version with a pre-release part (`2.0.0-rc.2`).
pub fn is_prerelease(version: &str) -> bool {
    version.contains('-')
}

/// The release an app at `app_version` should flash and offer as an update:
/// for a prerelease app, its own tag `v<version>` when it is published, else
/// (and always for a stable app) the latest release.
pub fn release_for_app(repo: &str, app_version: &str) -> Result<Release, ReleaseError> {
    release_for_app_with(repo, app_version, get_string)
}

/// [`release_for_app`] with the HTTP fetch injected (tests).
fn release_for_app_with(
    repo: &str,
    app_version: &str,
    fetch: impl Fn(&str) -> Result<String, ReleaseError>,
) -> Result<Release, ReleaseError> {
    let v = app_version.trim().trim_start_matches(['v', 'V']);
    if is_prerelease(v) {
        match fetch(&tag_url(repo, &format!("v{v}"))) {
            Ok(api) => return release_from_api(&api, &fetch),
            Err(ReleaseError::NoRelease) => log::info!("no release tagged v{v} in {repo}; using the latest release"),
            Err(e) => return Err(e),
        }
    }
    release_from_api(&fetch(&latest_url(repo))?, &fetch)
}

/// Hex SHA-256 of a byte slice.
pub fn sha256_hex(data: &[u8]) -> String {
    let d = Sha256::digest(data);
    d.iter().map(|b| format!("{b:02x}")).collect()
}

/// Check `data` against a manifest entry.
pub fn verify(board: &BoardImage, data: &[u8]) -> Result<(), ReleaseError> {
    let actual = sha256_hex(data);
    if actual.eq_ignore_ascii_case(board.sha256.trim()) {
        Ok(())
    } else {
        Err(ReleaseError::HashMismatch { file: board.file.clone(), expected: board.sha256.clone(), actual })
    }
}

/// Download `board`'s image into `cache_dir` (reusing a cached copy whose
/// hash still matches), verify it, and return its bytes. `progress` gets
/// `(downloaded, total)`; total is 0 when unknown.
pub fn fetch_image(
    release: &Release,
    board: &BoardImage,
    cache_dir: &Path,
    mut progress: impl FnMut(u64, u64),
) -> Result<Vec<u8>, ReleaseError> {
    // The file name comes from the network: never let it escape the cache.
    let name = Path::new(&board.file)
        .file_name()
        .and_then(|n| n.to_str())
        .ok_or_else(|| ReleaseError::Manifest(format!("bad file name {:?}", board.file)))?;
    let path = cache_dir.join(name);
    if let Ok(data) = fs::read(&path) {
        if verify(board, &data).is_ok() {
            progress(data.len() as u64, data.len() as u64);
            return Ok(data);
        }
    }
    let url = release.assets.get(&board.file).ok_or_else(|| ReleaseError::MissingAsset(board.file.clone()))?;
    let resp = get(url)?;
    let total =
        resp.headers().get("content-length").and_then(|v| v.to_str().ok()).and_then(|v| v.parse().ok()).unwrap_or(0);
    let mut reader = resp.into_body().into_reader();
    let mut data = Vec::with_capacity(total as usize);
    let mut buf = [0u8; 16 * 1024];
    loop {
        let n = reader.read(&mut buf).map_err(|e| ReleaseError::Offline(format!("{url}: {e}")))?;
        if n == 0 {
            break;
        }
        data.extend_from_slice(&buf[..n]);
        progress(data.len() as u64, total);
    }
    verify(board, &data)?;
    fs::create_dir_all(cache_dir)?;
    let tmp = path.with_extension("part");
    fs::File::create(&tmp)?.write_all(&data)?;
    fs::rename(&tmp, &path)?;
    Ok(data)
}

/// Remembers the last update check in `<cache>/last-update-check`.
#[derive(Debug, Clone)]
pub struct UpdateCheck {
    stamp: PathBuf,
}

impl UpdateCheck {
    /// Stamp file under `cache_dir`.
    pub fn new(cache_dir: &Path) -> Self {
        UpdateCheck { stamp: cache_dir.join("last-update-check") }
    }

    /// Unix time of the last check.
    pub fn last(&self) -> Option<i64> {
        fs::read_to_string(&self.stamp).ok()?.trim().parse().ok()
    }

    /// A day has passed (or we never checked).
    pub fn due(&self, now: i64) -> bool {
        self.last().is_none_or(|t| now - t >= CHECK_INTERVAL_SECS || t > now)
    }

    /// Record a check.
    pub fn mark(&self, now: i64) -> io::Result<()> {
        if let Some(d) = self.stamp.parent() {
            fs::create_dir_all(d)?;
        }
        fs::write(&self.stamp, now.to_string())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const API: &str = r#"{"tag_name":"v1.9.0","assets":[
      {"name":"manifest.json","browser_download_url":"https://x/manifest.json"},
      {"name":"a-v1.9.0.bin","browser_download_url":"https://x/a.bin"}]}"#;
    const MANIFEST: &str = r#"{"version":"1.9.0","boards":[{"env":"a","chip":"esp32s3","flash_size":"16MB",
      "display_name":"A","file":"a-v1.9.0.bin","sha256":"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"}]}"#;

    #[test]
    fn api_to_release() {
        let r = release_from_api(API, |u| {
            assert_eq!(u, "https://x/manifest.json");
            Ok(MANIFEST.to_owned())
        })
        .unwrap();
        assert_eq!(r.tag, "v1.9.0");
        assert_eq!(r.manifest.boards[0].env, "a");
        assert_eq!(r.assets["a-v1.9.0.bin"], "https://x/a.bin");
        let no_manifest = r#"{"tag_name":"v1","assets":[]}"#;
        assert!(matches!(release_from_api(no_manifest, |_| unreachable!()), Err(ReleaseError::MissingAsset(_))));
    }

    fn api(tag: &str) -> String {
        format!(
            r#"{{"tag_name":"{tag}","assets":[{{"name":"manifest.json","browser_download_url":"https://x/{tag}/manifest.json"}}]}}"#
        )
    }

    fn manifest(version: &str) -> String {
        MANIFEST.replace("1.9.0", version)
    }

    #[test]
    fn a_prerelease_app_flashes_its_own_tag() {
        let fetch = |u: &str| -> Result<String, ReleaseError> {
            match u {
                "https://api.github.com/repos/o/r/releases/tags/v2.0.0-rc.2" => Ok(api("v2.0.0-rc.2")),
                "https://api.github.com/repos/o/r/releases/latest" => Ok(api("v1.9.0")),
                "https://x/v2.0.0-rc.2/manifest.json" => Ok(manifest("2.0.0-rc.2")),
                "https://x/v1.9.0/manifest.json" => Ok(manifest("1.9.0")),
                _ => Err(ReleaseError::NoRelease),
            }
        };
        assert_eq!(release_for_app_with("o/r", "2.0.0-rc.2", fetch).unwrap().tag, "v2.0.0-rc.2");
        assert_eq!(release_for_app_with("o/r", "v2.0.0-rc.2", fetch).unwrap().tag, "v2.0.0-rc.2");
        // Its tag isn't published: the latest release.
        assert_eq!(release_for_app_with("o/r", "2.0.0-rc.3", fetch).unwrap().tag, "v1.9.0");
        // A stable app never asks for a tag.
        let stable = |u: &str| -> Result<String, ReleaseError> {
            assert!(!u.contains("/tags/"), "{u}");
            fetch(u)
        };
        assert_eq!(release_for_app_with("o/r", "1.9.0", stable).unwrap().tag, "v1.9.0");
    }

    #[test]
    fn prerelease_lookup_errors() {
        // Only prereleases published: latest is 404, the app's tag answers.
        let only_rc = |u: &str| -> Result<String, ReleaseError> {
            match u {
                "https://api.github.com/repos/o/r/releases/tags/v2.0.0-rc.2" => Ok(api("v2.0.0-rc.2")),
                "https://x/v2.0.0-rc.2/manifest.json" => Ok(manifest("2.0.0-rc.2")),
                _ => Err(ReleaseError::NoRelease),
            }
        };
        assert_eq!(release_for_app_with("o/r", "2.0.0-rc.2", only_rc).unwrap().manifest.version, "2.0.0-rc.2");
        assert!(matches!(release_for_app_with("o/r", "2.0.0", only_rc), Err(ReleaseError::NoRelease)));
        // Rate limited on the tag: reported, not retried against latest.
        let limited = |u: &str| -> Result<String, ReleaseError> {
            assert!(u.contains("/tags/"), "{u}");
            Err(ReleaseError::RateLimited)
        };
        assert!(matches!(release_for_app_with("o/r", "2.0.0-rc.2", limited), Err(ReleaseError::RateLimited)));
        assert!(matches!(
            ReleaseError::from_ureq(&tag_url("o/r", "v2.0.0-rc.9"), ureq::Error::StatusCode(404)),
            ReleaseError::NoRelease
        ));
    }

    #[test]
    fn hashing_and_cached_image() {
        assert_eq!(sha256_hex(b"hello"), "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
        let r = release_from_api(API, |_| Ok(MANIFEST.to_owned())).unwrap();
        let b = &r.manifest.boards[0];
        assert!(verify(b, b"hello").is_ok());
        assert!(matches!(verify(b, b"nope"), Err(ReleaseError::HashMismatch { .. })));

        let dir = std::env::temp_dir().join(format!("cm-rel-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("a-v1.9.0.bin"), b"hello").unwrap();
        let mut seen = None;
        let data = fetch_image(&r, b, &dir, |d, t| seen = Some((d, t))).unwrap();
        assert_eq!((data.as_slice(), seen), (&b"hello"[..], Some((5, 5))));

        let evil = BoardImage { file: "../../etc/passwd".into(), ..b.clone() };
        // Path components are stripped: it looks for `passwd` in the cache.
        assert!(fetch_image(&r, &evil, &dir, |_, _| {}).is_err());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn error_copy() {
        let latest = latest_url("o/r");
        assert_eq!(
            ReleaseError::from_ureq(&latest, ureq::Error::StatusCode(404)).to_string(),
            "No firmware release was found."
        );
        assert!(matches!(
            ReleaseError::from_ureq("https://x/a.bin", ureq::Error::StatusCode(404)),
            ReleaseError::Http(_)
        ));
        assert!(matches!(ReleaseError::from_ureq(&latest, ureq::Error::StatusCode(403)), ReleaseError::RateLimited));
        let off = ReleaseError::from_ureq(&latest, ureq::Error::HostNotFound);
        assert!(matches!(off, ReleaseError::Offline(_)));
        assert!(!off.to_string().contains("http"), "no URL dumps in the message");
        assert!(off.detail().unwrap().contains(&latest), "but the log keeps it");
        let bad = ReleaseError::HashMismatch { file: "a.bin".into(), expected: "aa".into(), actual: "bb".into() };
        assert!(bad.to_string().starts_with("Checksum mismatch for a.bin."));
    }

    #[test]
    fn daily_check() {
        let dir = std::env::temp_dir().join(format!("cm-upd-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let u = UpdateCheck::new(&dir);
        assert!(u.due(1000));
        u.mark(1000).unwrap();
        assert!(!u.due(1000 + 3600));
        assert!(u.due(1000 + CHECK_INTERVAL_SECS));
        assert!(u.due(10), "clock went backwards");
        let _ = fs::remove_dir_all(&dir);
    }
}
