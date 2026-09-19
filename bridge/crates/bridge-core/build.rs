// Bakes the git commit count into the exe as BRIDGE_BUILD_COUNT, so a
// release bridge knows which GitHub release (`r<count>`) to download its
// driver DLL from. Falls back to "dev" when git is unavailable (e.g.
// source export); the install flow then requires a local checkout build.
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
