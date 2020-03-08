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

    if cfg!(all(windows, target_env = "msvc")) {
        cfg = cfg.define("CMAKE_CXX_FLAGS_RELEASE", "/MD /O2 /Ob2 /DNDEBUG");
        cfg = cfg.define("CMAKE_C_FLAGS_RELEASE", "/MD /O2 /Ob2 /DNDEBUG");
    }
    
    if cfg!(all(windows, target_env = "gnu")) {
        cfg = cfg.define("CMAKE_SH", "CMAKE_SH-NOTFOUND");
    }

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
    
    if cfg!(all(windows, target_env = "msvc")) {
        println!("cargo:rustc-link-search=native={}/{}", dst.display(), build_type);
    } else {
        println!("cargo:rustc-link-search=native={}", dst.display());
    }
    
    if cfg!(target_os = "windows") {
        println!("cargo:rustc-link-lib=dylib=mincore");
    }
    
    if cfg!(target_os = "macos") {
        println!("cargo:rustc-link-lib=dylib=c++");
    }
    
    if cfg!(target_os = "openbsd") {
        println!("cargo:rustc-link-lib=dylib=c++");
    }
    
    if cfg!(target_os = "freebsd") {
        println!("cargo:rustc-link-lib=dylib=c++");
    }
    
    if cfg!(target_os = "linux") {
        println!("cargo:rustc-link-lib=dylib=stdc++");
        println!("cargo:rustc-link-lib=dylib=atomic");
    }
}
