use cmake::Config;
fn main() {
    let mut cfg = &mut Config::new("snmalloc");

    let build_type = if cfg!(feature = "debug") {
        "Debug"
    } else {
        "Release"
    };

    if cfg!(all(windows, target_env = "msvc")) {
        cfg = cfg.generator("Visual Studio 15 2017 Win64")
            .define("SNMALLOC_RUST_SUPPORT", "ON")
            .build_arg("--config")
            .build_arg(build_type)
    } else {
        cfg = cfg.generator("Ninja")
            .define("SNMALLOC_RUST_SUPPORT", "ON")
            .define("CMAKE_BUILD_TYPE", build_type)
    }

    let target = if cfg!(feature = "1mib") {
        "snmallocshim-1mib"
    } else {
        "snmallocshim"
    };

    let mut dst = if cfg!(feature = "cache-friendly") {
        cfg.define("CACHE_FRIENDLY_OFFSET", "64").build_target(target).build()
    } else {
        cfg.build_target(target).build()
    };
    
    dst.push("./build");
    println!("cargo:rustc-link-search=native={}", dst.display());
    println!("cargo:rustc-link-lib={}", target);
    if cfg!(unix) {
        println!("cargo:rustc-link-lib=dylib=stdc++");
        println!("cargo:rustc-link-lib=dylib=atomic");
    }
}