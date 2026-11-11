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
use std::time::Duration;

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

/// [`get`] for a large download: nothing is left waiting for good on a
/// connection that went quiet (sleep, a network change).
fn get_download(url: &str) -> Result<ureq::http::Response<ureq::Body>, ReleaseError> {
    ureq::get(url)
        .config()
        .timeout_connect(Some(Duration::from_secs(30)))
        .timeout_recv_response(Some(Duration::from_secs(30)))
        .timeout_recv_body(Some(Duration::from_secs(20 * 60)))
        .build()
        .header("User-Agent", USER_AGENT)
        .header("Accept", "application/octet-stream, */*")
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

/// What a release says about itself on GitHub.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ReleaseNotes {
    /// Git tag, `v2.5.0`.
    pub tag: String,
    /// Day it was published, `2026-11-02` (empty when unknown).
    pub published: String,
    /// The release's description, as Markdown.
    pub body: String,
}

#[derive(Debug, Deserialize)]
struct GhReleaseNotes {
    tag_name: String,
    #[serde(default)]
    body: Option<String>,
    #[serde(default)]
    published_at: Option<String>,
    #[serde(default)]
    draft: bool,
    #[serde(default)]
    prerelease: bool,
}

/// The API URL listing `repo`'s most recent releases.
pub fn releases_url(repo: &str) -> String {
    format!("https://api.github.com/repos/{repo}/releases?per_page=30")
}

/// The notes of every release an app at `app_version` would pick up by
/// updating to `latest`, newest first: `latest` itself and the releases
/// between the two. Prereleases other than `latest` are left out.
pub fn release_notes(repo: &str, app_version: &str, latest: &str) -> Result<Vec<ReleaseNotes>, ReleaseError> {
    release_notes_from_api(&get_string(&releases_url(repo))?, app_version, latest)
}

/// [`release_notes`] from the API's answer (split out for tests).
fn release_notes_from_api(api_json: &str, app_version: &str, latest: &str) -> Result<Vec<ReleaseNotes>, ReleaseError> {
    let all: Vec<GhReleaseNotes> = serde_json::from_str(api_json).map_err(|e| ReleaseError::Manifest(e.to_string()))?;
    let latest = release_tag(latest).ok_or_else(|| ReleaseError::Manifest(format!("bad release tag {latest:?}")))?;
    let mut notes: Vec<ReleaseNotes> = all
        .into_iter()
        .filter(|r| !r.draft)
        .filter_map(|r| {
            let tag = release_tag(&r.tag_name)?;
            let wanted = crate::manifest::is_newer(&tag, app_version)
                && !crate::manifest::is_newer(&tag, &latest)
                && (!r.prerelease || tag == latest);
            wanted.then(|| ReleaseNotes {
                tag,
                published: r.published_at.unwrap_or_default().chars().take(10).collect(),
                body: r.body.unwrap_or_default(),
            })
        })
        .collect();
    notes.sort_by(|a, b| crate::manifest::compare_versions(&b.tag, &a.tag));
    Ok(notes)
}

/// Asset name of the release's checksum list (`sha256sum` output).
pub const SUMS_ASSET: &str = "SHA256SUMS";

/// A desktop app package a release publishes (`release.yml`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AppPackage {
    /// macOS disk image (universal).
    Dmg,
    /// Windows installer (x86_64).
    Msi,
    /// Debian/Ubuntu package (amd64).
    Deb,
    /// Portable Linux build (x86_64).
    AppImage,
}

impl AppPackage {
    /// The package that updates this install, if the release has one: the
    /// `.dmg` on macOS, the `.msi` on x86_64 Windows when running from where
    /// it installs (`…\Programs\Clawdmeter`), and on x86_64 Linux the AppImage
    /// when running from one or the `.deb` when the binary is the packaged
    /// `/usr/bin/clawdmeter`. Anything else (Nix, a zip or tarball, a local
    /// build, other architectures) has none: a package would install a
    /// second copy beside the one that keeps running.
    pub fn for_install(os: &str, arch: &str, appimage: bool, exe: &Path) -> Option<Self> {
        match (os, arch) {
            ("macos", _) => Some(AppPackage::Dmg),
            ("windows", "x86_64") => {
                let exe = exe.to_string_lossy().replace('\\', "/").to_ascii_lowercase();
                exe.ends_with("/programs/clawdmeter/clawdmeter.exe").then_some(AppPackage::Msi)
            }
            ("linux", "x86_64") if appimage => Some(AppPackage::AppImage),
            ("linux", "x86_64") if exe == Path::new("/usr/bin/clawdmeter") => Some(AppPackage::Deb),
            _ => None,
        }
    }

    /// [`AppPackage::for_install`] for the running process.
    pub fn for_this_install() -> Option<Self> {
        let exe = std::env::current_exe().unwrap_or_default();
        let appimage = std::env::var_os("APPIMAGE").is_some_and(|v| !v.is_empty());
        Self::for_install(std::env::consts::OS, std::env::consts::ARCH, appimage, &exe)
    }

    /// The asset's file name in the release tagged `tag` (`v2.5.0`).
    pub fn asset(self, tag: &str) -> String {
        match self {
            AppPackage::Dmg => format!("clawdmeter-{tag}-macos-universal.dmg"),
            AppPackage::Msi => format!("clawdmeter-{tag}-windows-x86_64.msi"),
            AppPackage::Deb => format!("clawdmeter_{}_amd64.deb", tag.trim_start_matches('v')),
            AppPackage::AppImage => format!("clawdmeter-{tag}-x86_64.AppImage"),
        }
    }

    /// The OS opens this package into an installer (a mounted disk image, the
    /// Windows installer). The others are files to install by hand.
    pub fn opens_installer(self) -> bool {
        matches!(self, AppPackage::Dmg | AppPackage::Msi)
    }
}

/// The git tag for a release version: `2.5.0` and `v2.5.0` both give
/// `v2.5.0`. `None` for anything that isn't a plain version, since the value
/// comes from the network and ends up in URLs and file names.
pub fn release_tag(version: &str) -> Option<String> {
    let v = version.trim().trim_start_matches(['v', 'V']);
    let plain = v.starts_with(|c: char| c.is_ascii_digit())
        && v.len() <= 40
        && v.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | '+'));
    plain.then(|| format!("v{v}"))
}

/// The release's page on GitHub.
pub fn release_page_url(repo: &str, tag: &str) -> String {
    format!("https://github.com/{repo}/releases/tag/{tag}")
}

/// The download URL of one of a release's assets.
pub fn asset_url(repo: &str, tag: &str, asset: &str) -> String {
    format!("https://github.com/{repo}/releases/download/{tag}/{asset}")
}

/// The hash `sha256sum` listed for `name`.
pub fn sums_entry<'a>(sums: &'a str, name: &str) -> Option<&'a str> {
    sums.lines().find_map(|line| {
        let (hash, file) = line.trim().split_once(char::is_whitespace)?;
        (file.trim_start().trim_start_matches('*') == name).then_some(hash)
    })
}

fn file_sha256(path: &Path) -> io::Result<String> {
    let mut file = fs::File::open(path)?;
    let mut hasher = Sha256::new();
    io::copy(&mut file, &mut hasher)?;
    Ok(hasher.finalize().iter().map(|b| format!("{b:02x}")).collect())
}

/// Mark `path` as downloaded from the internet, as a browser would, so the
/// OS runs its usual check when the file is opened: the quarantine attribute
/// on macOS (Gatekeeper), the `Zone.Identifier` stream on Windows
/// (SmartScreen). Best effort: a failure is logged.
fn mark_downloaded(path: &Path, url: &str) {
    #[cfg(target_os = "macos")]
    {
        let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs());
        let value = format!("0081;{now:x};Clawdmeter;");
        let done = std::process::Command::new("/usr/bin/xattr")
            .args(["-w", "com.apple.quarantine", &value])
            .arg(path)
            .status();
        if !done.as_ref().is_ok_and(|s| s.success()) {
            log::warn!("couldn't quarantine {}: {done:?}", path.display());
        }
        let _ = url;
    }
    #[cfg(windows)]
    {
        let mut stream = path.as_os_str().to_owned();
        stream.push(":Zone.Identifier");
        if let Err(e) = fs::write(&stream, format!("[ZoneTransfer]\r\nZoneId=3\r\nHostUrl={url}\r\n")) {
            log::warn!("couldn't mark {} as downloaded: {e}", path.display());
        }
    }
    #[cfg(not(any(target_os = "macos", windows)))]
    let _ = (path, url);
}

/// Download the app package for `tag` from `repo` into `dir`, check it
/// against the release's `SHA256SUMS`, and return where it is. A copy already
/// there with the right hash is reused. `progress` gets `(downloaded,
/// total)`; total is 0 when unknown.
pub fn fetch_app_package(
    repo: &str,
    tag: &str,
    package: AppPackage,
    dir: &Path,
    progress: impl FnMut(u64, u64),
) -> Result<PathBuf, ReleaseError> {
    fetch_app_package_with(repo, tag, package, dir, progress, get_string, |url| {
        let resp = get_download(url)?;
        let total = resp
            .headers()
            .get("content-length")
            .and_then(|v| v.to_str().ok())
            .and_then(|v| v.parse().ok())
            .unwrap_or(0);
        Ok((Box::new(resp.into_body().into_reader()) as Box<dyn Read>, total))
    })
}

/// [`fetch_app_package`] with the HTTP side injected (tests): `fetch` reads
/// a text asset, `open` streams a binary one and gives its length.
fn fetch_app_package_with(
    repo: &str,
    tag: &str,
    package: AppPackage,
    dir: &Path,
    mut progress: impl FnMut(u64, u64),
    fetch: impl Fn(&str) -> Result<String, ReleaseError>,
    open: impl Fn(&str) -> Result<(Box<dyn Read>, u64), ReleaseError>,
) -> Result<PathBuf, ReleaseError> {
    let tag = release_tag(tag).ok_or_else(|| ReleaseError::Manifest(format!("bad release tag {tag:?}")))?;
    let name = package.asset(&tag);
    let sums = fetch(&asset_url(repo, &tag, SUMS_ASSET))?;
    let expected = sums_entry(&sums, &name).ok_or_else(|| ReleaseError::MissingAsset(name.clone()))?.to_owned();
    let path = dir.join(&name);
    if file_sha256(&path).is_ok_and(|have| have.eq_ignore_ascii_case(&expected)) {
        let len = fs::metadata(&path).map(|m| m.len()).unwrap_or(0);
        progress(len, len);
        return Ok(path);
    }
    let url = asset_url(repo, &tag, &name);
    let (mut reader, total) = open(&url)?;
    fs::create_dir_all(dir)?;
    let tmp = dir.join(format!("{name}.part"));
    let written = (|| {
        let mut file = fs::File::create(&tmp)?;
        let mut hasher = Sha256::new();
        let mut buf = [0u8; 64 * 1024];
        let mut done = 0u64;
        loop {
            let n = reader.read(&mut buf).map_err(|e| ReleaseError::Offline(format!("{url}: {e}")))?;
            if n == 0 {
                break;
            }
            file.write_all(&buf[..n])?;
            hasher.update(&buf[..n]);
            done += n as u64;
            progress(done, total);
        }
        file.flush()?;
        let actual: String = hasher.finalize().iter().map(|b| format!("{b:02x}")).collect();
        if !actual.eq_ignore_ascii_case(&expected) {
            return Err(ReleaseError::HashMismatch { file: name.clone(), expected: expected.clone(), actual });
        }
        drop(file);
        // An AppImage is run as it is.
        #[cfg(unix)]
        if package == AppPackage::AppImage {
            use std::os::unix::fs::PermissionsExt as _;
            fs::set_permissions(&tmp, fs::Permissions::from_mode(0o755))?;
        }
        // Before the rename, which keeps the mark: a file under its final
        // name is one that has it.
        mark_downloaded(&tmp, &url);
        fs::rename(&tmp, &path)?;
        Ok(())
    })();
    if let Err(e) = written {
        let _ = fs::remove_file(&tmp);
        return Err(e);
    }
    Ok(path)
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
    fn app_package_per_install() {
        let exe = Path::new("/usr/bin/clawdmeter");
        let home = Path::new("/home/me/.cargo/bin/clawdmeter");
        assert_eq!(AppPackage::for_install("macos", "aarch64", false, home), Some(AppPackage::Dmg));
        let msi = Path::new(r"C:\Users\me\AppData\Local\Programs\Clawdmeter\clawdmeter.exe");
        assert_eq!(AppPackage::for_install("windows", "x86_64", false, msi), Some(AppPackage::Msi));
        let zip = Path::new(r"C:\Tools\clawdmeter.exe");
        assert_eq!(AppPackage::for_install("windows", "x86_64", false, zip), None, "an unzipped copy");
        assert_eq!(AppPackage::for_install("linux", "x86_64", true, home), Some(AppPackage::AppImage));
        assert_eq!(AppPackage::for_install("linux", "x86_64", false, exe), Some(AppPackage::Deb));
        assert_eq!(AppPackage::for_install("linux", "x86_64", false, home), None, "tarball, Nix or local build");
        assert_eq!(AppPackage::for_install("linux", "aarch64", true, exe), None);
        // The names release.yml publishes.
        assert_eq!(AppPackage::Dmg.asset("v2.5.0"), "clawdmeter-v2.5.0-macos-universal.dmg");
        assert_eq!(AppPackage::Msi.asset("v2.5.0"), "clawdmeter-v2.5.0-windows-x86_64.msi");
        assert_eq!(AppPackage::Deb.asset("v2.5.0-rc.1"), "clawdmeter_2.5.0-rc.1_amd64.deb");
        assert_eq!(AppPackage::AppImage.asset("v2.5.0"), "clawdmeter-v2.5.0-x86_64.AppImage");
    }

    #[test]
    fn notes_cover_the_releases_being_installed() {
        let api = r###"[
          {"tag_name":"v2.6.0-rc.1","prerelease":true,"body":"rc","published_at":"2026-11-20T10:00:00Z"},
          {"tag_name":"v2.4.1","body":"## Fixed\n- a","published_at":"2026-11-05T10:00:00Z"},
          {"tag_name":"v2.5.0","body":"## New\n- b","published_at":"2026-11-10T08:30:00Z"},
          {"tag_name":"v2.5.1","draft":true,"body":"draft"},
          {"tag_name":"v2.4.0","body":"old","published_at":"2026-10-01T10:00:00Z"},
          {"tag_name":"nightly","body":"not a version"},
          {"tag_name":"v2.3.0","body":null}]"###;
        let notes = release_notes_from_api(api, "2.4.0", "v2.5.0").unwrap();
        let tags: Vec<&str> = notes.iter().map(|n| n.tag.as_str()).collect();
        assert_eq!(tags, ["v2.5.0", "v2.4.1"], "newest first, nothing older, newer or unpublished");
        assert_eq!(notes[0].published, "2026-11-10");
        assert_eq!(notes[0].body, "## New\n- b");
        // A prerelease is listed only when it is the one being installed.
        let rc = release_notes_from_api(api, "2.5.0", "2.6.0-rc.1").unwrap();
        assert_eq!(rc.iter().map(|n| n.tag.as_str()).collect::<Vec<_>>(), ["v2.6.0-rc.1"]);
        assert!(release_notes_from_api("{}", "2.4.0", "v2.5.0").is_err());
        assert_eq!(releases_url("o/r"), "https://api.github.com/repos/o/r/releases?per_page=30");
    }

    #[test]
    fn tags_and_urls() {
        assert_eq!(release_tag("2.5.0").as_deref(), Some("v2.5.0"));
        assert_eq!(release_tag(" v2.5.0-rc.1\n").as_deref(), Some("v2.5.0-rc.1"));
        for bad in ["", "dev", "v", "2.5.0/../x", "2.5.0?x=1", "2.5.0 beta"] {
            assert_eq!(release_tag(bad), None, "{bad:?}");
        }
        assert_eq!(
            release_page_url("HermannBjorgvin/Clawdmeter", "v2.5.0"),
            "https://github.com/HermannBjorgvin/Clawdmeter/releases/tag/v2.5.0"
        );
        let sums = "aa11  manifest.json\nbb22 *clawdmeter-v2.5.0-macos-universal.dmg\n";
        assert_eq!(sums_entry(sums, "manifest.json"), Some("aa11"));
        assert_eq!(sums_entry(sums, "clawdmeter-v2.5.0-macos-universal.dmg"), Some("bb22"));
        assert_eq!(sums_entry(sums, "universal.dmg"), None);
    }

    #[test]
    fn app_package_is_downloaded_and_verified() {
        let dir = std::env::temp_dir().join(format!("cm-pkg-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let name = "clawdmeter-v2.5.0-macos-universal.dmg";
        let sums = format!("{}  {name}\n", sha256_hex(b"hello"));
        let fetch = |u: &str| {
            assert_eq!(u, "https://github.com/o/r/releases/download/v2.5.0/SHA256SUMS");
            Ok(sums.clone())
        };
        let body = |bytes: &'static [u8]| {
            move |u: &str| {
                assert_eq!(u, format!("https://github.com/o/r/releases/download/v2.5.0/{name}"));
                Ok((Box::new(bytes) as Box<dyn Read>, bytes.len() as u64))
            }
        };
        let mut seen = (0, 0);
        let got =
            fetch_app_package_with("o/r", "2.5.0", AppPackage::Dmg, &dir, |d, t| seen = (d, t), fetch, body(b"hello"))
                .unwrap();
        assert_eq!(got, dir.join(name));
        assert_eq!(fs::read(&got).unwrap(), b"hello");
        assert_eq!(seen, (5, 5));
        #[cfg(target_os = "macos")]
        {
            let out = std::process::Command::new("/usr/bin/xattr").arg(&got).output().unwrap();
            assert!(String::from_utf8_lossy(&out.stdout).contains("com.apple.quarantine"), "marked as downloaded");
        }
        // A verified copy is reused: nothing is downloaded again.
        let again =
            fetch_app_package_with("o/r", "v2.5.0", AppPackage::Dmg, &dir, |_, _| {}, fetch, |_| unreachable!());
        assert_eq!(again.unwrap(), got);
        // Wrong bytes are refused and leave nothing behind.
        fs::remove_file(&got).unwrap();
        let bad = fetch_app_package_with("o/r", "v2.5.0", AppPackage::Dmg, &dir, |_, _| {}, fetch, body(b"jello"));
        assert!(matches!(bad, Err(ReleaseError::HashMismatch { .. })));
        assert_eq!(fs::read_dir(&dir).unwrap().count(), 0);
        // A package the release doesn't list, and a tag that isn't a version.
        let none = fetch_app_package_with("o/r", "v2.5.0", AppPackage::Msi, &dir, |_, _| {}, fetch, |_| unreachable!());
        assert!(matches!(none, Err(ReleaseError::MissingAsset(_))));
        let evil = fetch_app_package_with(
            "o/r",
            "../x",
            AppPackage::Dmg,
            &dir,
            |_, _| {},
            |_| unreachable!(),
            |_| unreachable!(),
        );
        assert!(matches!(evil, Err(ReleaseError::Manifest(_))));
        let _ = fs::remove_dir_all(&dir);
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
