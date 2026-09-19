//! Portable default install paths shared by the bridge crates.
//!
//! Nothing here may contain a machine-specific path. Defaults resolve from
//! the running executable's location (repo checkout) and well-known
//! environment variables, so a fresh clone works on any PC. Anything
//! unresolvable returns an empty string and the caller must surface an
//! error telling the user what to fill in.

use std::path::PathBuf;

/// Marker file that identifies the repo root (sits next to `bridge/`).
const REPO_MARKER: &str = "driver_cardboardplusplus/driver_cardboardplusplus.sln";

/// Walk up from the running executable looking for the repo checkout.
pub fn repo_root() -> Option<PathBuf> {
    let exe = std::env::current_exe().ok()?;
    let mut dir = exe.parent()?.to_path_buf();
    // ponytail: 6 levels covers bridge/target/(debug|release)/deps plus installed layouts.
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

fn exe_relative(parts: &[&str]) -> String {
    let Some(mut p) = repo_root() else {
        return String::new();
    };
    for part in parts {
        p.push(part);
    }
    p.to_string_lossy().into_owned()
}

/// Compiled driver DLL produced by the C++ build.
pub fn default_driver_dll() -> String {
    exe_relative(&[
        "driver_cardboardplusplus",
        "x64",
        "Release",
        "driver_cardboardplusplus.dll",
    ])
}

/// Built Android APK.
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

/// adb.exe: `ANDROID_HOME`/`ANDROID_SDK_ROOT` first, then the default SDK
/// location under `%LOCALAPPDATA%`. Empty when no SDK is installed.
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

/// SteamVR addon home. The standard Steam install location under
/// `%ProgramFiles(x86)%` — not personal, just the default; user-editable.
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

/// `...\SteamVR\drivers\cardboardplusplus` -> `...\SteamVR`.
pub fn steamvr_root_from_drivers_dir(dir: &str) -> String {
    PathBuf::from(dir.trim_end_matches(['\\', '/']))
        .parent()
        .and_then(|p| p.parent())
        .map(|p| p.to_string_lossy().into_owned())
        .unwrap_or_default()
}

/// MSBuild via vswhere (same lookup as `scripts/compile-driver.ps1`).
/// `None` when no Visual Studio C++ build tools are installed.
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
