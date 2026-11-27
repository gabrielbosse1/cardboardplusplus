use std::path::PathBuf;
/// Presence probe identifying the repo root: the driver solution file only
/// exists at the top of a checkout, so walking up to it works from any
/// build output directory (target/debug, staged install, ...).
const REPO_MARKER: &str = "driver_cardboardplusplus/driver_cardboardplusplus.sln";
/// Walks up from the running exe to the repo root. Returns None for installs
/// outside a checkout (then callers fall back to exe-relative defaults).
pub fn repo_root() -> Option<PathBuf> {
    let exe = std::env::current_exe().ok()?;
    let mut dir = exe.parent()?.to_path_buf();
    for _ in 0..6 {
        if dir.join(REPO_MARKER).exists() {
            return Some(dir);
        }
        if !dir.pop() {
            break;
        }
    }
    None
}
/// Joins path parts onto the repo root. Empty string when the root is
/// unknown; callers treat that as "artifact not staged".
fn exe_relative(parts: &[&str]) -> String {
    let Some(mut p) = repo_root() else {
        return String::new();
    };
    for part in parts {
        p.push(part);
    }
    p.to_string_lossy().into_owned()
}
/// Release driver artifact produced by the platform build script. Read by the
/// General tab installer before copying into the SteamVR drivers directory.
#[cfg(windows)]
pub fn default_driver_dll() -> String {
    exe_relative(&[
        "driver_cardboardplusplus",
        "x64",
        "Release",
        "driver_cardboardplusplus.dll",
    ])
}
#[cfg(not(windows))]
pub fn default_driver_dll() -> String {
    exe_relative(&[
        "driver_cardboardplusplus",
        "build",
        "libdriver_cardboardplusplus.so",
    ])
}
/// Debug APK produced by compile-app.ps1. Read by the General tab installer
/// before pushing to the phone over ADB.
pub fn default_apk() -> String {
    exe_relative(&[
        "cardboardplusplus-android",
        "build",
        "outputs",
        "apk",
        "debug",
        "app-debug.apk",
    ])
}
/// Locates adb.exe without a hardcoded user path: ANDROID_HOME /
/// ANDROID_SDK_ROOT first, then the default LOCALAPPDATA SDK install.
/// Empty string when no SDK is found (installer then prompts the user).
pub fn default_adb() -> String {
    #[cfg(windows)]
    let adb_name = "adb.exe";
    #[cfg(not(windows))]
    let adb_name = "adb";
    for var in ["ANDROID_HOME", "ANDROID_SDK_ROOT"] {
        if let Ok(v) = std::env::var(var) {
            let p = PathBuf::from(v).join("platform-tools").join(adb_name);
            if p.exists() {
                return p.to_string_lossy().into_owned();
            }
        }
    }
    #[cfg(windows)]
    if let Ok(local) = std::env::var("LOCALAPPDATA") {
        let p = PathBuf::from(local)
            .join("Android")
            .join("Sdk")
            .join("platform-tools")
            .join(adb_name);
        if p.exists() {
            return p.to_string_lossy().into_owned();
        }
    }
    #[cfg(not(windows))]
    if let Some(home) = std::env::var_os("HOME") {
        for p in [
            PathBuf::from(&home).join("Android/Sdk/platform-tools").join(adb_name),
            PathBuf::from(&home).join(".android/sdk/platform-tools").join(adb_name),
        ] {
            if p.exists() {
                return p.to_string_lossy().into_owned();
            }
        }
    }
    if which_adb() {
        return adb_name.to_string();
    }
    String::new()
}
#[cfg(not(windows))]
fn which_adb() -> bool {
    std::env::var_os("PATH").map_or(false, |paths| {
        std::env::split_paths(&paths).any(|p| p.join("adb").is_file())
    })
}
#[cfg(windows)]
fn which_adb() -> bool { false }
/// SteamVR driver slot the installer copies into:
/// <SteamVR>/drivers/cardboardplusplus. ProgramFiles(x86) first because
/// SteamVR installs 32-bit by default.
#[cfg(windows)]
pub fn default_steamvr_drivers_dir() -> String {
    let pf = std::env::var("ProgramFiles(x86)")
        .or_else(|_| std::env::var("ProgramFiles"))
        .unwrap_or_else(|_| r"C:\Program Files (x86)".to_string());
    PathBuf::from(pf)
        .join("Steam")
        .join("steamapps")
        .join("common")
        .join("SteamVR")
        .join("drivers")
        .join("cardboardplusplus")
        .to_string_lossy()
        .into_owned()
}
#[cfg(not(windows))]
pub fn default_steamvr_drivers_dir() -> String {
    if let Some(dir) = linux_registered_driver_dir() {
        return dir.to_string_lossy().into_owned();
    }
    home().map(|h| h.join(".local/share/SteamVR/drivers/cardboardplusplus").to_string_lossy().into_owned())
        .unwrap_or_default()
}
#[cfg(not(windows))]
fn home() -> Option<PathBuf> { std::env::var_os("HOME").map(PathBuf::from) }
/// SteamVR runtime directory across native, Flatpak, Snap and system layouts.
#[cfg(not(windows))]
fn linux_steamvr_dir() -> Option<PathBuf> {
    let home = home()?;
    let mut candidates = vec![home.join(".steam/steam/steamapps/common/SteamVR"),
                              home.join(".local/share/Steam/steamapps/common/SteamVR")];
    if let Ok(flatpak) = std::env::var("FLATPAK_HOME") {
        candidates.push(PathBuf::from(flatpak).join(".steam/steam/steamapps/common/SteamVR"));
    }
    candidates.push(home.join(".var/app/com.valvesoftware.Steam/.steam/steam/steamapps/common/SteamVR"));
    candidates.push(home.join("snap/steam/common/SteamVR"));
    candidates.push(PathBuf::from("/opt/SteamVR"));
    candidates.into_iter().find(|p| {
        p.join("bin/linux64/openvr_api.so").exists() || p.join("bin/linux64/libopenvr_api.so").exists()
    })
}
#[cfg(not(windows))]
fn linux_registered_driver_dir() -> Option<PathBuf> {
    let reg = linux_steamvr_dir()?.join("bin/vrpathreg.sh");
    if !reg.exists() { return None; }
    let out = std::process::Command::new(&reg).output().ok()?;
    for line in String::from_utf8_lossy(&out.stdout).lines() {
        if let Some((name, path)) = line.split_once(':') {
            if name.trim().eq_ignore_ascii_case("cardboardplusplus") {
                let p = path.trim();
                if p.starts_with('/') && p.ends_with("/drivers/cardboardplusplus") {
                    return Some(PathBuf::from(p));
                }
            }
        }
    }
    None
}
/// Climbs two levels from the Windows driver slot to the SteamVR root.
#[cfg(windows)]
pub fn steamvr_root_from_drivers_dir(dir: &str) -> String {
    PathBuf::from(dir.trim_end_matches(['\\', '/']))
        .parent()
        .and_then(|p| p.parent())
        .map(|p| p.to_string_lossy().into_owned())
        .unwrap_or_default()
}
/// Linux config root: <home>/.local/share/Steam/config/steamvr.vrsettings.
#[cfg(not(windows))]
pub fn steamvr_root_from_drivers_dir(_dir: &str) -> String {
    home().map(|h| h.join(".local/share/Steam").to_string_lossy().into_owned())
        .unwrap_or_default()
}
/// Per-user data dir used by the wizard marker and the staged sidecar files.
pub fn user_data_dir() -> PathBuf {
    if let Ok(v) = std::env::var("LOCALAPPDATA") {
        return PathBuf::from(v).join("CardboardPlusPlus");
    }
    if let Ok(v) = std::env::var("XDG_DATA_HOME") {
        return PathBuf::from(v).join("CardboardPlusPlus");
    }
    if let Some(h) = std::env::var_os("HOME") {
        return PathBuf::from(h).join(".local/share/CardboardPlusPlus");
    }
    std::env::temp_dir().join("CardboardPlusPlus")
}
/// One-time install wizard marker: <user data dir>/setup.done.
/// The bridge shows the wizard when this file is absent.
pub fn setup_done_file() -> PathBuf {
    user_data_dir().join("setup.done")
}
/// True after the wizard completed once. Checked at bridge startup.
pub fn is_setup_done() -> bool {
    setup_done_file().exists()
}
/// Writes the wizard marker (creating the directory). Called when the user
/// finishes or dismisses the one-time setup wizard.
pub fn mark_setup_done() {
    let path = setup_done_file();
    if let Some(parent) = path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let _ = std::fs::write(&path, "done");
}
/// Ensures SteamVR scans the driver slot. Windows uses a fixed slot; Linux
/// registers the external path with vrpathreg.sh when SteamVR is installed.
pub fn register_driver(dir: &str) -> bool {
    #[cfg(windows)]
    {
        let _ = dir;
        true
    }
    #[cfg(not(windows))]
    {
        let Some(runtime) = linux_steamvr_dir() else { return false; };
        let reg = runtime.join("bin/vrpathreg.sh");
        if !reg.exists() {
            return false;
        }
        std::process::Command::new(&reg)
            .args(["adddriver", dir])
            .output()
            .map(|o| o.status.success())
            .unwrap_or(false)
    }
}
/// Finds MSBuild via vswhere for the driver build step. None when Visual
/// Studio is missing (the installer then tells the user what to install).
pub fn find_msbuild() -> Option<String> {
    let pf = std::env::var("ProgramFiles(x86)").ok()?;
    let vswhere = PathBuf::from(pf)
        .join("Microsoft Visual Studio")
        .join("Installer")
        .join("vswhere.exe");
    if !vswhere.exists() {
        return None;
    }
    let out = std::process::Command::new(&vswhere)
        .args([
            "-latest",
            "-requires",
            "Microsoft.Component.MSBuild",
            "-find",
            r"MSBuild\**\Bin\amd64\MSBuild.exe",
        ])
        .output()
        .ok()?;
    let first = String::from_utf8_lossy(&out.stdout)
        .lines()
        .next()
        .unwrap_or("")
        .trim()
        .to_string();
    if !first.is_empty() && PathBuf::from(&first).exists() {
        Some(first)
    } else {
        None
    }
}
