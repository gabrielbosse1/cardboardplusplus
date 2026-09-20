/// Compiles the Slint UI at build time so `app.rs` can include the generated
/// structs. Re-runs only when the .slint source or icons change.
fn main() {
    println!("cargo:rerun-if-changed=ui/app.slint");
    println!("cargo:rerun-if-changed=ui/icons");
    slint_build::compile("ui/app.slint").expect("Slint UI compilation failed");
}