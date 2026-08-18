fn main() {
    println!("cargo:rerun-if-changed=ui/app.slint");
    println!("cargo:rerun-if-changed=ui/icons");

    slint_build::compile("ui/app.slint").expect("Slint UI compilation failed");
}