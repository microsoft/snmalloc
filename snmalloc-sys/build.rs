use cmake::Config;

fn main() {
    let mut cfg = &mut Config::new("snmalloc");

    let build_type = if cfg!(feature = "debug") {
        "Debug"
    } else {
        "Release"
    };

    cfg = cfg.define("SNMALLOC_RUST_SUPPORT", "ON")
        .profile(build_type);

    let target = if cfg!(feature = "1mib") {
        "snmallocshim-1mib-rust"
    } else {
        "snmallocshim-rust"
    };

    let mut dst = if cfg!(feature = "cache-friendly") {
        cfg.define("CACHE_FRIENDLY_OFFSET", "64").build_target(target).build()
    } else {
        cfg.build_target(target).build()
    };

    dst.push("./build");

    println!("cargo:rustc-link-lib={}", target);
    if cfg!(unix) {
        println!("cargo:rustc-link-search=native={}", dst.display());
        println!("cargo:rustc-link-lib=dylib=stdc++");
        println!("cargo:rustc-link-lib=dylib=atomic");
    } else {
        println!("cargo:rustc-link-search=native={}/{}", dst.display(), build_type);
        println!("cargo:rustc-link-lib=dylib=mincore");
    }
}
