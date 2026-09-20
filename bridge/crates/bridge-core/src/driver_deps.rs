use std::path::{Path, PathBuf};
use super::paths;
/// Copy of deps.json baked in at compile time, so installed bridges (outside
/// a checkout) still know the pinned FFmpeg version and DLL set.
const EMBEDDED_DEPS_JSON: &str =
    include_str!("../../../../driver_cardboardplusplus/lib/ffmpeg/deps.json");
/// Pin used when no manifest is readable anywhere. Must match deps.json or
/// the installer copies a mismatched runtime next to the driver DLL.
pub const FALLBACK_FFMPEG_VERSION: &str = "8.1";
pub const FALLBACK_DLLS: &[&str] = &[
    "avcodec-62.dll",
    "avformat-62.dll",
    "avutil-60.dll",
    "swscale-9.dll",
    "swresample-6.dll",
];
/// Live manifest in the checkout. None for installed bridges (they use the
/// embedded copy via manifest_text).
pub fn manifest_path() -> Option<PathBuf> {
    paths::repo_root().map(|r| {
        r.join("driver_cardboardplusplus")
            .join("lib")
            .join("ffmpeg")
            .join("deps.json")
    })
}
/// Vendored FFmpeg bin directory in the checkout. Empty when there is no
/// checkout; every consumer below treats that as "cannot fetch, use fallback".
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
/// Parses a deps.json manifest into (ffmpeg_version, dlls, bin_zip_url).
/// The URL is optional (vendored checkouts omit it); empty dll lists and
/// non-JSON input are None so callers fall back to the baked-in pin.
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
/// Pinned FFmpeg version for display and mismatch errors. Manifest first,
/// FALLBACK_FFMPEG_VERSION when nothing parses.
pub fn ffmpeg_version() -> String {
    manifest_text()
        .and_then(|t| parse_manifest(&t).map(|(v, _, _)| v))
        .unwrap_or_else(|| FALLBACK_FFMPEG_VERSION.into())
}
/// Runtime DLL set the driver directory must contain. Manifest first,
/// FALLBACK_DLLS when nothing parses.
pub fn expected_dlls() -> Vec<String> {
    manifest_text()
        .and_then(|t| parse_manifest(&t).map(|(_, d, _)| d))
        .unwrap_or_else(|| FALLBACK_DLLS.iter().map(|s| s.to_string()).collect())
}
/// Download URL for the matching FFmpeg bin zip. None when the manifest
/// omits it (fully vendored checkout) — then only local copies are used.
pub fn bin_zip_url() -> Option<String> {
    manifest_text().and_then(|t| parse_manifest(&t).and_then(|(_, _, u)| u))
}
/// Manifest source priority: live file in the checkout, else the baked-in
/// copy. Always Some — the embedded JSON is the last resort.
fn manifest_text() -> Option<String> {
    let from_repo = manifest_path().and_then(|p| std::fs::read_to_string(p).ok());
    if from_repo.is_some() {
        return from_repo;
    }
    Some(EMBEDDED_DEPS_JSON.to_string())
}
/// Lists av*/sw* DLLs in `dir` that are NOT in the pinned set: leftovers
/// from a previous FFmpeg major that would shadow the new runtime.
/// Anything else (driver DLL, notes) is left alone.
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
/// fetch-ffmpeg-deps.ps1 in the checkout. None for installed bridges, which
/// cannot download and must use vendored DLLs.
pub fn fetch_script_path() -> Option<PathBuf> {
    paths::repo_root().map(|r| r.join("scripts").join("fetch-ffmpeg-deps.ps1"))
}
/// Ensures the vendored bin holds every pinned DLL, running the fetch script
/// when something is missing. Ok(()) means install_ffmpeg_dlls can copy.
/// Called before driver installs and bridge staging.
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
/// Copies the pinned FFmpeg runtime next to the driver DLL. With `refresh`
/// it first deletes stale majors via stale_runtime_dlls. Returns the copied
/// set for the install log; errors name the missing DLLs and the pin to bump.
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
const LOCK_ATTEMPTS: u32 = 4;
/// Copies a DLL that may be locked by a running vrserver: retries with 2s
/// pauses, then reports which process to quit via lock_hint.
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
/// Atomically swaps tmp into place (rename), retrying through locks like
/// copy_over_locked. Removes tmp on final failure so no .tmp litter remains.
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
/// Formats a copy/rename failure with the "quit SteamVR" remedy. Shared by
/// copy_over_locked and replace_locked so every locked-DLL error reads the same.
fn lock_hint(dst: &Path, e: &std::io::Error) -> String {
    format!(
        "{}: {} (is SteamVR/vrserver.exe still running? Quit SteamVR and re-run)",
        dst.display(),
        e
    )
}
#[cfg(test)]
mod tests {
    // Pin + file-op tests: the embedded manifest agrees with the fallback
    // constants, the parser accepts/ rejects manifest shapes, stale detection
    // only flags unpinned av*/sw* majors, and the locked-file helpers swap or
    // report correctly using temp dirs. Test names read as the spec.
    use super::*;
    #[test]
    fn embedded_manifest_matches_fallback_pin() {
        let (v, d, u) = parse_manifest(EMBEDDED_DEPS_JSON).expect("embedded parses");
        assert_eq!(v, FALLBACK_FFMPEG_VERSION);
        let fallback: Vec<String> = FALLBACK_DLLS.iter().map(|s| s.to_string()).collect();
        assert_eq!(d, fallback);
        assert!(u.is_some_and(|s| s.starts_with("https://")));
    }
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
