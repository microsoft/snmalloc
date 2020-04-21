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

    let triple = std::env::var("TARGET").unwrap();
    if triple.contains("android") {
        if let Ok(ndk) = std::env::var("ANDROID_NDK") {
            cfg = cfg.define("CMAKE_TOOLCHAIN_FILE", format!("{}/build/cmake/android.toolchain.cmake", ndk));
        } else {
            eprintln!("please set ANDROID_NDK environment variable");
            std::process::abort();
        }

        if let Ok(platform) = std::env::var("ANDROID_PLATFORM") {
            cfg = cfg.define("ANDROID_PLATFORM", platform);
        }

        if cfg!(feature = "android-lld") {
            cfg = cfg.define("ANDROID_LD", "lld");
        }

        if cfg!(feature = "android-shared-stl") {
            println!("cargo:rustc-link-lib=dylib=c++_shared");
            cfg = cfg.define("ANDROID_STL", "c++_shared");
        }

        if triple.contains("aarch64") {
            cfg = cfg.define("ANDROID_ABI", "arm64-v8a");
        } else if triple.contains("armv7") {
            cfg = cfg.define("ANDROID_ABI", "armeabi-v7a")
                .define("ANDROID_ARM_MODE", "arm");
        } else if triple.contains("x86_64") {
            cfg = cfg.define("ANDROID_ABI", "x86_64");
        } else if triple.contains("i686") {
            cfg = cfg.define("ANDROID_ABI", "x86_64");
        } else if triple.contains("neon") {
            cfg = cfg.define("ANDROID_ABI", "armeabi-v7a with NEON")
        } else if triple.contains("arm") {
            cfg = cfg.define("ANDROID_ABI", "armeabi-v7a");
        }
    }

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
        println!("cargo:rustc-link-lib=dylib=mincore");
        println!("cargo:rustc-link-search=native={}/{}", dst.display(), build_type);
    } else {
        println!("cargo:rustc-link-search=native={}", dst.display());
    }
    
    if cfg!(all(windows, target_env = "gnu")) {
        let path = std::env::var("MINGW64_BIN").unwrap_or_else(|_| {
            eprintln!("please set MINGW64_BIN so that we can link atomic library");
            std::process::abort();
        });
        println!("cargo:rustc-link-search=native={}", path);
        println!("cargo:rustc-link-lib=dylib=stdc++");
        println!("cargo:rustc-link-lib=dylib=atomic-1"); // TODO: fix me
        println!("cargo:rustc-link-lib=dylib=winpthread");
        println!("cargo:rustc-link-lib=dylib=gcc_s");
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
