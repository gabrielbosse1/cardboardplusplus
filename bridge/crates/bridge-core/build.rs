/// Bakes the git commit count into BRIDGE_BUILD_COUNT at compile time.
/// Non-git checkouts (source exports) get "dev", which disables the release
/// DLL download path in driver_install.
fn main() {
    let count = std::process::Command::new("git")
        .args(["rev-list", "--count", "HEAD"])
        .output()
        .ok()
        .filter(|o| o.status.success())
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "dev".to_string());
    println!("cargo:rustc-env=BRIDGE_BUILD_COUNT={count}");
}
