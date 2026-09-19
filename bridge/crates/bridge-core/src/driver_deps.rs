//! Driver runtime dependencies: what FFmpeg DLL versions the SteamVR driver
//! needs, and how to install them next to it.
//!
//! Single source of truth is `driver_cardboardplusplus/lib/ffmpeg/deps.json`
//! (pinned majors + exact DLL names). Everything here falls back to the
//! hardcoded pin below when the manifest can't be read, so the installer
//! never silently ships a wrong/skewed set — it fails with the expected
//! names instead. Keep the fallback in sync with `deps.json` when bumping.

use std::path::{Path, PathBuf};

use super::paths;

/// Pinned FFmpeg version (mirrors `deps.json`).
pub const FALLBACK_FFMPEG_VERSION: &str = "8.1";

/// Pinned runtime DLLs (mirrors `deps.json`). The driver links 4 libs (see
/// `driver_cardboardplusplus.vcxproj`) but BtbN's avcodec-62.dll
/// hard-imports swresample-6.dll, so it ships too.
pub const FALLBACK_DLLS: &[&str] = &[
    "avcodec-62.dll",
    "avformat-62.dll",
    "avutil-60.dll",
    "swscale-9.dll",
    "swresample-6.dll",
];

/// Manifest location inside the repo checkout. `None` when the checkout
/// can't be located.
pub fn manifest_path() -> Option<PathBuf> {
    paths::repo_root().map(|r| {
        r.join("driver_cardboardplusplus")
            .join("lib")
            .join("ffmpeg")
            .join("deps.json")
    })
}

/// Vendored runtime DLL dir (`lib/ffmpeg/bin`). Empty when unresolvable.
pub fn ffmpeg_bin_src() -> String {
    match paths::repo_root() {
        Some(r) => r
            .join("driver_cardboardplusplus")
            .join("lib")
            .join("ffmpeg")
            .join("bin")
            .to_string_lossy()
            .into_owned(),
        None => String::new(),
    }
}

fn parse_manifest(text: &str) -> Option<(String, Vec<String>, Option<String>)> {
    let v: serde_json::Value = serde_json::from_str(text).ok()?;
    let version = v.get("ffmpeg_version")?.as_str()?.to_string();
    let dlls = v
        .get("dlls")?
        .as_array()?
        .iter()
        .filter_map(|d| d.as_str().map(str::to_string))
        .collect::<Vec<_>>();
    if dlls.is_empty() {
        return None;
    }
    let url = v
        .get("bin_zip_url")
        .and_then(|u| u.as_str())
        .map(str::to_string);
    Some((version, dlls, url))
}

/// Pinned FFmpeg version from the manifest, fallback when unreadable.
pub fn ffmpeg_version() -> String {
    manifest_path()
        .and_then(|p| std::fs::read_to_string(p).ok())
        .and_then(|t| parse_manifest(&t).map(|(v, _, _)| v))
        .unwrap_or_else(|| FALLBACK_FFMPEG_VERSION.into())
}

/// Expected runtime DLL names from the manifest, fallback when unreadable.
pub fn expected_dlls() -> Vec<String> {
    manifest_path()
        .and_then(|p| std::fs::read_to_string(p).ok())
        .and_then(|t| parse_manifest(&t).map(|(_, d, _)| d))
        .unwrap_or_else(|| FALLBACK_DLLS.iter().map(|s| s.to_string()).collect())
}

/// GitHub zip URL for the pinned runtime, if the manifest names one.
pub fn bin_zip_url() -> Option<String> {
    manifest_path()
        .and_then(|p| std::fs::read_to_string(p).ok())
        .and_then(|t| parse_manifest(&t).and_then(|(_, _, u)| u))
}

/// Stale FFmpeg runtimes in `dir`: `av*.dll` / `sw*.dll` not in the pinned
/// set (e.g. `avcodec-61.dll` left over from an older pin). The driver DLL
/// itself never matches these prefixes.
pub fn stale_runtime_dlls(dir: &Path) -> Vec<PathBuf> {
    let expected = expected_dlls();
    let Ok(entries) = std::fs::read_dir(dir) else {
        return Vec::new();
    };
    entries
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| {
            let Some(name) = p.file_name().and_then(|n| n.to_str()) else {
                return false;
            };
            let lower = name.to_ascii_lowercase();
            (lower.starts_with("av") || lower.starts_with("sw"))
                && lower.ends_with(".dll")
                && !expected.iter().any(|d| d.eq_ignore_ascii_case(name))
        })
        .collect()
}

/// Fetch script shared with the build/install scripts. `None` when the
/// checkout can't be located.
pub fn fetch_script_path() -> Option<PathBuf> {
    paths::repo_root().map(|r| r.join("scripts").join("fetch-ffmpeg-deps.ps1"))
}

/// Download the pinned runtime DLLs from GitHub into the vendored `bin/`.
/// No-op when everything is already present. Shells out to the repo's
/// `scripts/fetch-ffmpeg-deps.ps1` (same fetch the PS scripts use) instead
/// of re-implementing zip handling — driver installs are Windows-only.
pub fn ensure_ffmpeg_bin() -> Result<(), String> {
    let src_bin = ffmpeg_bin_src();
    if src_bin.is_empty() {
        return Err("repo checkout not found — cannot locate vendored FFmpeg".into());
    }
    let src = Path::new(&src_bin);
    if expected_dlls().iter().all(|d| src.join(d).exists()) {
        return Ok(());
    }
    let Some(script) = fetch_script_path().filter(|p| p.exists()) else {
        return Err("fetch script missing (scripts/fetch-ffmpeg-deps.ps1)".into());
    };
    let out = std::process::Command::new("powershell")
        .args([
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            &script.to_string_lossy(),
        ])
        .output()
        .map_err(|e| format!("could not start powershell for FFmpeg fetch: {e}"))?;
    if !out.status.success() {
        let detail = format!(
            "{}{}",
            String::from_utf8_lossy(&out.stdout),
            String::from_utf8_lossy(&out.stderr)
        );
        return Err(format!("FFmpeg fetch failed: {}", detail.trim()));
    }
    Ok(())
}
/// Missing runtimes are auto-fetched from GitHub first (same script the
/// build/install scripts use); only a stale pin still errors, listing the
/// missing names. When `refresh` is set, stale runtimes are deleted first
/// so only the pinned set remains.
pub fn install_ffmpeg_dlls(dst_dir: &Path, refresh: bool) -> Result<Vec<String>, String> {
    let src_bin = ffmpeg_bin_src();
    if src_bin.is_empty() {
        return Err("repo checkout not found — cannot locate vendored FFmpeg".into());
    }
    let src = Path::new(&src_bin);
    let expected = expected_dlls();
    if expected.iter().any(|d| !src.join(d).exists()) {
        ensure_ffmpeg_bin()?;
    }
    let missing: Vec<_> = expected
        .iter()
        .filter(|d| !src.join(d).exists())
        .cloned()
        .collect();
    if !missing.is_empty() {
        return Err(format!(
            "FFmpeg {} runtime DLLs still missing under {} ({}) after fetch; the pin moved — bump dlls + bin_zip_url in deps.json",
            ffmpeg_version(),
            src.display(),
            missing.join(", ")
        ));
    }
    std::fs::create_dir_all(dst_dir)
        .map_err(|e| format!("mkdir {}: {e}", dst_dir.display()))?;
    if refresh {
        for stale in stale_runtime_dlls(dst_dir) {
            std::fs::remove_file(&stale)
                .map_err(|e| format!("remove {}: {e}", stale.display()))?;
        }
    }
    for dll in &expected {
        copy_over_locked(&src.join(dll), &dst_dir.join(dll))
            .map_err(|e| format!("copy {}: {}", dll, e))?;
    }
    Ok(expected)
}

/// Install attempts (rename/copy run through here from both bridge installers).
const LOCK_ATTEMPTS: u32 = 4;

/// Copy that survives a briefly-locked target (SteamVR holds the loaded
/// DLLs open). Same shape as `Copy-WithRetry` in scripts/install-driver.ps1.
pub fn copy_over_locked(src: &Path, dst: &Path) -> Result<(), String> {
    let mut last = String::new();
    for _ in 0..LOCK_ATTEMPTS {
        match std::fs::copy(src, dst) {
            Ok(_) => return Ok(()),
            Err(e) => {
                last = lock_hint(dst, &e);
                std::thread::sleep(std::time::Duration::from_secs(2));
            }
        }
    }
    Err(last)
}

/// Atomic replace via temp-file rename, retrying while the target is locked.
/// A failed copy never leaves a missing/half-written DLL.
pub fn replace_locked(tmp: &Path, dst: &Path) -> Result<(), String> {
    let mut last = String::new();
    for _ in 0..LOCK_ATTEMPTS {
        match std::fs::rename(tmp, dst) {
            Ok(_) => return Ok(()),
            Err(e) => {
                last = lock_hint(dst, &e);
                std::thread::sleep(std::time::Duration::from_secs(2));
            }
        }
    }
    let _ = std::fs::remove_file(tmp);
    Err(last)
}

fn lock_hint(dst: &Path, e: &std::io::Error) -> String {
    format!(
        "{}: {} (is SteamVR/vrserver.exe still running? Quit SteamVR and re-run)",
        dst.display(),
        e
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn manifest_parses_version_and_dlls() {
        let text = r#"{"ffmpeg_version":"8.1","dlls":["avcodec-62.dll","avutil-60.dll"],"bin_zip_url":"https://example.invalid/f.zip"}"#;
        let (v, d, u) = parse_manifest(text).expect("parses");
        assert_eq!(v, "8.1");
        assert_eq!(d, vec!["avcodec-62.dll", "avutil-60.dll"]);
        assert_eq!(u.as_deref(), Some("https://example.invalid/f.zip"));
    }

    #[test]
    fn manifest_url_is_optional() {
        let text = r#"{"ffmpeg_version":"8.1","dlls":["avcodec-62.dll"]}"#;
        let (_, _, u) = parse_manifest(text).expect("parses");
        assert_eq!(u, None);
    }

    #[test]
    fn manifest_rejects_empty_dlls() {
        assert!(parse_manifest(r#"{"ffmpeg_version":"8.0","dlls":[]}"#).is_none());
        assert!(parse_manifest("not json").is_none());
    }

    #[test]
    fn fallback_lists_five_pinned_dlls() {
        assert_eq!(FALLBACK_DLLS.len(), 5);
        assert!(FALLBACK_DLLS.iter().all(|d| d.ends_with(".dll")));
    }

    #[test]
    fn stale_detection_keeps_pinned_removes_old_major() {
        let dir = std::env::temp_dir().join(format!("cb-deps-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        // ponytail: derive the pinned name so this survives version bumps.
        let pinned = expected_dlls().first().cloned().unwrap();
        for name in [
            pinned.as_str(),
            "avcodec-61.dll",
            "swscale-8.dll",
            "driver_cardboardplusplus.dll",
            "notes.txt",
        ] {
            std::fs::write(dir.join(name), b"x").unwrap();
        }
        let mut stale: Vec<String> = stale_runtime_dlls(&dir)
            .iter()
            .filter_map(|p| {
                p.file_name()
                    .and_then(|n| n.to_str())
                    .map(str::to_string)
            })
            .collect();
        stale.sort();
        // ponytail: pinned avcodec-62.dll + the driver DLL + notes.txt stay.
        assert_eq!(stale, vec!["avcodec-61.dll", "swscale-8.dll"]);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn replace_locked_swaps_atomically() {
        let dir = std::env::temp_dir().join(format!("cb-replace-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let tmp = dir.join("new.dll.tmp-1");
        let dst = dir.join("driver.dll");
        std::fs::write(&tmp, b"new").unwrap();
        std::fs::write(&dst, b"old").unwrap();
        replace_locked(&tmp, &dst).expect("rename succeeds");
        assert_eq!(std::fs::read(&dst).unwrap(), b"new");
        assert!(!tmp.exists());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn lock_hint_names_steamvr() {
        let err = std::io::Error::new(std::io::ErrorKind::PermissionDenied, "access denied");
        let hint = lock_hint(Path::new("C:\\dst\\driver.dll"), &err);
        assert!(hint.contains("vrserver.exe"), "{hint}");
    }
}
