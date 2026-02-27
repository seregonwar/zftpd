use std::env;
use std::path::PathBuf;

fn main() {
    let project_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
    let c_core_dir = PathBuf::from(project_dir.clone()).join("..").join("c_core");
    
    // Tell cargo to look for shared libraries in the specified directory
    let lib_dir = PathBuf::from(project_dir.clone()).join("..").join("..").join("build").join("macos").join("release");
    println!("cargo:rustc-link-search=native={}", lib_dir.display());
    
    // Fallback for Linux
    let linux_lib_dir = PathBuf::from(project_dir).join("..").join("..").join("build").join("linux").join("release");
    println!("cargo:rustc-link-search=native={}", linux_lib_dir.display());

    // Tell cargo to tell rustc to link the shared library.
    println!("cargo:rustc-link-lib=zftpd_ffi");

    // Tell cargo to invalidate the built crate whenever the wrapper changes
    let header_path = c_core_dir.join("pal_ffi.h");
    println!("cargo:rerun-if-changed={}", header_path.display());

    // The bindgen::Builder is the main entry point
    // to bindgen, and lets you build up options for
    // the resulting bindings.
    let bindings = bindgen::Builder::default()
        // The input header we would like to generate
        // bindings for.
        .header(header_path.to_str().unwrap())
        // Include path
        .clang_arg(format!("-I{}", c_core_dir.display()))
        // Tell cargo to invalidate the built crate whenever any of the
        // included header files changed.
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        // Finish the builder and generate the bindings.
        .generate()
        // Unwrap the Result and panic on failure.
        .expect("Unable to generate bindings");

    // Write the bindings to the $OUT_DIR/bindings.rs file.
    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings!");
}
