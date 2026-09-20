use std::path::{Path, PathBuf};
use super::{driver_deps, paths};
/// Commit count baked by build.rs; doubles as the release tag (r<N>) the
/// installer downloads the driver DLL from. "dev" means no release exists.
pub const BUILD_COUNT: &str = env!("BRIDGE_BUILD_COUNT");
const GITHUB_REPO: &str = "gabrielbosse1/cardboardplusplus";
/// steamvr.vrsettings section + keys this installer manages for the driver.
const DRIVER_SECTION: &str = "driver_cardboardplusplus";
const ENABLE_KEY: &str = "enable";
const BLOCKED_KEY: &str = "blocked_by_safe_mode";
/// SteamVR driver files baked into the bridge binary so installs work without
/// a checkout: manifest, controller profile, legacy bindings, icon.
const MANIFEST: &str =
    include_str!("../../../../driver_cardboardplusplus/resources/driver.vrdrivermanifest");
const CONTROLLER_PROFILE: &str =
    include_str!("../../../../driver_cardboardplusplus/resources/controller_profile.json");
const LEGACY_BINDINGS: &str =
    include_str!("../../../../driver_cardboardplusplus/resources/legacy_bindings_example.json");
const GAME_CONTROLLER_SVG: &[u8] =
    include_bytes!("../../../../driver_cardboardplusplus/resources/game_controller.svg");
/// GitHub release URL for this build's driver DLL. None for "dev" builds,
/// which have no release to download from.
pub fn release_driver_url() -> Option<String> {
    if BUILD_COUNT == "dev" {
        return None;
    }
    Some(format!(
        "https://github.com/{GITHUB_REPO}/releases/download/r{BUILD_COUNT}/driver_cardboardplusplus-r{BUILD_COUNT}.dll"
    ))
}
/// Where the driver DLL comes from: a fresh local compile when present,
/// otherwise the matching GitHub release download.
pub enum DriverDllSource {
    Local(PathBuf),
    Download(String),
}
/// Resolves the DLL source, preferring the local Release build. Errors only
/// when neither exists (dev build outside a checkout with no release).
pub fn driver_dll_source() -> Result<DriverDllSource, String> {
    let local = paths::default_driver_dll();
    if !local.is_empty() && Path::new(&local).exists() {
        return Ok(DriverDllSource::Local(PathBuf::from(local)));
    }
    match release_driver_url() {
        Some(url) => Ok(DriverDllSource::Download(url)),
        None => Err(format!(
            "compiled dll not found at {local} (build the driver first)"
        )),
    }
}
/// Downloads `url` to `dst` (creating parents). Tries curl.exe first for
/// retries/progress, falls back to Invoke-WebRequest. Used for the release
/// driver DLL and the FFmpeg bin zip.
pub fn download_to(url: &str, dst: &Path) -> Result<(), String> {
    if let Some(parent) = dst.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|e| format!("mkdir {}: {e}", parent.display()))?;
    }
    let curl = std::process::Command::new("curl.exe")
        .args(["-fSL", "--retry", "3", "--retry-delay", "5", "-o"])
        .arg(dst)
        .arg(url)
        .output();
    match curl {
        Ok(out) if out.status.success() => return Ok(()),
        Ok(out) => {
            let detail = String::from_utf8_lossy(&out.stderr).trim().to_string();
            if detail.is_empty() {
            } else {
                return Err(format!("download {url}: curl: {detail}"));
            }
        }
        Err(_) => {}
    }
    let ps = format!(
        "Invoke-WebRequest -Uri '{}' -OutFile '{}' -UseBasicParsing",
        url,
        dst.to_string_lossy().replace('\'', "''")
    );
    let out = std::process::Command::new("powershell")
        .args(["-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", &ps])
        .output()
        .map_err(|e| format!("download {url}: could not start powershell: {e}"))?;
    if !out.status.success() {
        let detail = format!(
            "{}{}",
            String::from_utf8_lossy(&out.stdout),
            String::from_utf8_lossy(&out.stderr)
        );
        return Err(format!("download {url}: {}", detail.trim()));
    }
    Ok(())
}
/// Ensures `dst_dir` holds the full pinned FFmpeg runtime for standalone
/// (installed, checkout-less) bridges: keeps existing DLLs when complete,
/// copies from the checkout when available, otherwise downloads + extracts
/// the bin zip. With `refresh` it purges stale majors first. Temp dir is
/// always cleaned up. Returns the installed set for the install log.
pub fn install_ffmpeg_standalone(dst_dir: &Path, refresh: bool) -> Result<Vec<String>, String> {
    let expected = driver_deps::expected_dlls();
    std::fs::create_dir_all(dst_dir)
        .map_err(|e| format!("mkdir {}: {e}", dst_dir.display()))?;
    if refresh {
        for stale in driver_deps::stale_runtime_dlls(dst_dir) {
            std::fs::remove_file(&stale)
                .map_err(|e| format!("remove {}: {e}", stale.display()))?;
        }
    }
    if expected.iter().all(|d| dst_dir.join(d).exists()) {
        return Ok(expected);
    }
    let src_bin = driver_deps::ffmpeg_bin_src();
    if !src_bin.is_empty()
        && expected.iter().all(|d| Path::new(&src_bin).join(d).exists())
    {
        return driver_deps::install_ffmpeg_dlls(dst_dir, refresh);
    }
    let url = driver_deps::bin_zip_url()
        .ok_or_else(|| "no FFmpeg zip URL (deps.json has no bin_zip_url)".to_string())?;
    let tmp = std::env::temp_dir().join(format!("cb-ffmpeg-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&tmp);
    std::fs::create_dir_all(&tmp).map_err(|e| format!("mkdir {}: {e}", tmp.display()))?;
    let result = (|| {
        let zip = tmp.join("ffmpeg-shared.zip");
        download_to(&url, &zip)?;
        let out = std::process::Command::new("powershell")
            .args([
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-Command",
                &format!(
                    "Expand-Archive -LiteralPath '{}' -DestinationPath '{}' -Force",
                    zip.to_string_lossy().replace('\'', "''"),
                    tmp.join("x").to_string_lossy().replace('\'', "''")
                ),
            ])
            .output()
            .map_err(|e| format!("could not start powershell for unzip: {e}"))?;
        if !out.status.success() {
            return Err(format!(
                "unzip failed: {}",
                String::from_utf8_lossy(&out.stderr).trim()
            ));
        }
        for dll in &expected {
            let found = find_file(&tmp, dll).ok_or_else(|| {
                format!("zip has no {dll} - upstream renamed the asset; bump bin_zip_url in deps.json")
            })?;
            driver_deps::copy_over_locked(&found, &dst_dir.join(dll))
                .map_err(|e| format!("copy {dll}: {e}"))?;
        }
        Ok(expected.clone())
    })();
    let _ = std::fs::remove_dir_all(&tmp);
    result
}
/// Writes the baked-in manifest, controller profile, bindings, and icon into
/// the SteamVR driver slot. Resource files are only written when missing so
/// user customizations (bindings) survive reinstalls.
pub fn install_resources(drivers_dir: &Path) -> Result<(), String> {
    std::fs::create_dir_all(drivers_dir)
        .map_err(|e| format!("mkdir {}: {e}", drivers_dir.display()))?;
    std::fs::write(drivers_dir.join("driver.vrdrivermanifest"), MANIFEST)
        .map_err(|e| format!("write driver.vrdrivermanifest: {e}"))?;
    let res = drivers_dir.join("resources");
    std::fs::create_dir_all(&res).map_err(|e| format!("mkdir {}: {e}", res.display()))?;
    for (name, text) in [
        ("driver.vrdrivermanifest", MANIFEST),
        ("controller_profile.json", CONTROLLER_PROFILE),
        ("legacy_bindings_example.json", LEGACY_BINDINGS),
    ] {
        let dst = res.join(name);
        if !dst.exists() {
            std::fs::write(&dst, text).map_err(|e| format!("write {}: {e}", dst.display()))?;
        }
    }
    let svg = res.join("game_controller.svg");
    if !svg.exists() {
        std::fs::write(&svg, GAME_CONTROLLER_SVG)
            .map_err(|e| format!("write {}: {e}", svg.display()))?;
    }
    Ok(())
}
/// steamvr.vrsettings under a SteamVR root. Edited by force_driver_enabled.
pub fn steamvr_settings_path(steamvr_root: &str) -> PathBuf {
    Path::new(steamvr_root).join("config").join("steamvr.vrsettings")
}
/// Forces our driver section to enable=true and clears the safe-mode block
/// in steamvr.vrsettings (backing up to .bak first). Returns true when it
/// changed something; false leaves the file untouched. Missing/unreadable
/// settings also return false (SteamVR recreates them on launch).
pub fn force_driver_enabled(steamvr_root: &str) -> Result<bool, String> {
    let path = steamvr_settings_path(steamvr_root);
    let Ok(text) = std::fs::read_to_string(&path) else {
        return Ok(false);
    };
    let Some(updated) = apply_force_enable(&text) else {
        return Ok(false);
    };
    let bak = path.with_extension("vrsettings.bak");
    std::fs::copy(&path, &bak).map_err(|e| format!("backup {}: {e}", bak.display()))?;
    std::fs::write(&path, updated).map_err(|e| format!("write {}: {e}", path.display()))?;
    Ok(true)
}
/// Pure vrsettings transform behind force_driver_enabled: inserts/creates
/// our section, sets enable=true, drops blocked_by_safe_mode, preserves
/// every other key. None means "already correct, write nothing" — including
/// for garbage input, which is never clobbered.
fn apply_force_enable(text: &str) -> Option<String> {
    let mut v: serde_json::Value = serde_json::from_str(text).ok()?;
    let root = v.as_object_mut()?;
    let section = root
        .entry(DRIVER_SECTION.to_string())
        .or_insert_with(|| serde_json::Value::Object(Default::default()));
    let obj = section.as_object_mut()?;
    let enabled = obj.get(ENABLE_KEY).and_then(|b| b.as_bool()).unwrap_or(true);
    if enabled && !obj.contains_key(BLOCKED_KEY) {
        return None;
    }
    obj.insert(ENABLE_KEY.to_string(), serde_json::Value::Bool(true));
    obj.remove(BLOCKED_KEY);
    serde_json::to_string_pretty(&v).ok().map(|mut s| {
        s.push('\n');
        s
    })
}
/// Case-insensitive recursive file search. Finds DLLs inside the extracted
/// FFmpeg zip no matter how deeply the archive nests them.
fn find_file(dir: &Path, name: &str) -> Option<PathBuf> {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return None;
    };
    for entry in entries.filter_map(|e| e.ok()) {
        let path = entry.path();
        if path.is_dir() {
            if let Some(hit) = find_file(&path, name) {
                return Some(hit);
            }
        } else if path
            .file_name()
            .and_then(|n| n.to_str())
            .is_some_and(|n| n.eq_ignore_ascii_case(name))
        {
            return Some(path);
        }
    }
    None
}
#[cfg(test)]
mod tests {
    // Installer contract tests: release URL tracks the baked build count,
    // the vrsettings transform only touches our section (never garbage or
    // unrelated keys), resources are baked in, and the settings path is
    // stable. Test names read as the spec.
    use super::*;
    #[test]
    fn release_url_matches_this_build() {
        if BUILD_COUNT == "dev" {
            assert!(release_driver_url().is_none());
            return;
        }
        let url = release_driver_url().expect("url");
        assert!(
            url.ends_with(&format!("driver_cardboardplusplus-r{BUILD_COUNT}.dll")),
            "{url}"
        );
        assert!(url.contains(&format!("releases/download/r{BUILD_COUNT}/")), "{url}");
    }
    #[test]
    fn force_enable_leaves_missing_section_alone() {
        assert!(apply_force_enable("{}").is_none());
        let empty = r#"{"driver_cardboardplusplus": {}}"#;
        assert!(apply_force_enable(empty).is_none());
    }
    #[test]
    fn force_enable_repairs_disabled_and_blocked() {
        let before = r#"{
  "steamvr": {"requireHmd": false},
  "driver_cardboardplusplus": {"enable": false, "blocked_by_safe_mode": true, "loadPriority": 100}
}"#;
        let out = apply_force_enable(before).expect("changes");
        let v: serde_json::Value = serde_json::from_str(&out).unwrap();
        assert_eq!(v["driver_cardboardplusplus"]["enable"], true);
        assert!(v["driver_cardboardplusplus"].get("blocked_by_safe_mode").is_none());
        assert_eq!(v["steamvr"]["requireHmd"], false);
        assert_eq!(v["driver_cardboardplusplus"]["loadPriority"], 100);
    }
    #[test]
    fn force_enable_noop_when_already_on() {
        let on = r#"{"driver_cardboardplusplus": {"enable": true}}"#;
        assert!(apply_force_enable(on).is_none());
    }
    #[test]
    fn force_enable_never_clobbers_garbage() {
        assert!(apply_force_enable("not json{{").is_none());
        assert!(apply_force_enable(r#"{"driver_cardboardplusplus": 42}"#).is_none());
    }
    #[test]
    fn embedded_resources_ship() {
        assert!(MANIFEST.contains("cardboardplusplus"));
        assert!(CONTROLLER_PROFILE.len() > 100);
        assert!(LEGACY_BINDINGS.len() > 100);
        assert!(!GAME_CONTROLLER_SVG.is_empty());
    }
    #[test]
    fn settings_path_lives_under_config() {
        let p = steamvr_settings_path("C:\\Steam\\steamapps\\common\\SteamVR");
        assert_eq!(
            p,
            Path::new("C:\\Steam\\steamapps\\common\\SteamVR\\config\\steamvr.vrsettings")
        );
    }
}
