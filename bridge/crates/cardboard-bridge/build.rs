/// Compiles the Slint UI at build time so `app.rs` can include the generated
/// structs. Re-runs only when the .slint source or icons change. On Windows
/// also stamps ui/icons/app.ico into the exe so shortcuts show the logo.
fn main() {
    println!("cargo:rerun-if-changed=ui/app.slint");
    println!("cargo:rerun-if-changed=ui/icons");
    slint_build::compile("ui/app.slint").expect("Slint UI compilation failed");
    if std::env::var("CARGO_CFG_WINDOWS").is_ok() {
        println!("cargo:rerun-if-changed=ui/icons/app.ico");
        println!("cargo:rerun-if-changed=app.rc");
        embed_resource::compile("app.rc", embed_resource::NONE)
            .manifest_required()
            .expect("Windows resource compilation failed");
    }
}