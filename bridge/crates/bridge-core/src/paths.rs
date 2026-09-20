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
/// Release driver DLL produced by compile-driver.ps1. Read by the General
/// tab installer before copying into the SteamVR drivers directory.
pub fn default_driver_dll() -> String {
    exe_relative(&[
        "driver_cardboardplusplus",
        "x64",
        "Release",
        "driver_cardboardplusplus.dll",
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
    for var in ["ANDROID_HOME", "ANDROID_SDK_ROOT"] {
        if let Ok(v) = std::env::var(var) {
            let p = PathBuf::from(v).join("platform-tools").join("adb.exe");
            if p.exists() {
                return p.to_string_lossy().into_owned();
            }
        }
    }
    if let Ok(local) = std::env::var("LOCALAPPDATA") {
        let p = PathBuf::from(local)
            .join("Android")
            .join("Sdk")
            .join("platform-tools")
            .join("adb.exe");
        if p.exists() {
            return p.to_string_lossy().into_owned();
        }
    }
    String::new()
}
/// SteamVR driver slot the installer copies into:
/// <SteamVR>/drivers/cardboardplusplus. ProgramFiles(x86) first because
/// SteamVR installs 32-bit by default.
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
/// Climbs two levels from the driver slot to the SteamVR root. Used to find
/// vrserver / vrpathreg when registering the driver.
pub fn steamvr_root_from_drivers_dir(dir: &str) -> String {
    PathBuf::from(dir.trim_end_matches(['\\', '/']))
        .parent()
        .and_then(|p| p.parent())
        .map(|p| p.to_string_lossy().into_owned())
        .unwrap_or_default()
}
/// One-time install wizard marker: %LOCALAPPDATA%/CardboardPlusPlus/setup.done.
/// The bridge shows the wizard when this file is absent.
pub fn setup_done_file() -> PathBuf {
    let mut dir = std::env::var("LOCALAPPDATA")
        .map(PathBuf::from)
        .unwrap_or_else(|_| std::env::temp_dir());
    if std::env::var("LOCALAPPDATA").is_ok() {
        dir.push("CardboardPlusPlus");
    }
    dir.join("setup.done")
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
