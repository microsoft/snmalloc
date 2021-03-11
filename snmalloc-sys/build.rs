fn main() {
    let mut build = cc::Build::new();
    build.include("snmalloc/src");
    build.file("snmalloc/src/override/rust.cc".to_string());
    build.flag_if_supported("/O2");
    build.flag_if_supported("/W4");
    build.flag_if_supported("/WX");
    build.flag_if_supported("/wd4127");
    build.flag_if_supported("/wd4324");
    build.flag_if_supported("/wd4201");
    build.flag_if_supported("/Ob2");
    build.flag_if_supported("/DNDEBUG");
    build.flag_if_supported("/EHsc");
    build.flag_if_supported("/std:c++17");
    build.flag_if_supported("-O3");
    build.flag_if_supported("-Wc++17-extensions");
    build.flag_if_supported("-std=c++1z");
    build.flag_if_supported("-std=gnu++1z");
    build.flag_if_supported("-mcx16");
	build.flag_if_supported("-fno-exceptions");
	build.flag_if_supported("-fno-rtti");
	build.flag_if_supported("-g");
	build.flag_if_supported("-fomit-frame-pointer");
    build.cpp(true);
    build.debug(false);

    let triple = std::env::var("TARGET").unwrap();
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").expect("target_os not defined!");
    let target_env = std::env::var("CARGO_CFG_TARGET_ENV").expect("target_env not defined!");
	let target_family = std::env::var("CARGO_CFG_TARGET_FAMILY").expect("target family not set");

	
    if triple.contains("android") {
        if cfg!(feature = "android-lld") {
            build.define("ANDROID_LD", "lld");
        }

        if cfg!(feature = "android-shared-stl") {
            build.define("ANDROID_STL", "c++_shared");
        }

        if triple.contains("aarch64") {
            build.define("ANDROID_ABI", "arm64-v8a");
        } else if triple.contains("armv7") {
            build.define("ANDROID_ABI", "armeabi-v7a");
            build.define("ANDROID_ARM_MODE", "arm");
        } else if triple.contains("x86_64") {
            build.define("ANDROID_ABI", "x86_64");
        } else if triple.contains("i686") {
            build.define("ANDROID_ABI", "x86_64");
        } else if triple.contains("neon") {
            build.define("ANDROID_ABI", "armeabi-v7a with NEON");
        } else if triple.contains("arm") {
            build.define("ANDROID_ABI", "armeabi-v7a");
        }
    }
	
    if target_os=="windows" && target_env == "gnu" {
        build.define("CMAKE_SH", "CMAKE_SH-NOTFOUND");
        if cfg!(feature = "local_dynamic_tls") {
            build.flag_if_supported("-ftls-model=local-dynamic");
        } else {
            build.flag_if_supported("-ftls-model=initial-exec");
        }
    }
	
    if target_family == "unix" && target_os != "haiku" {
        if cfg!(feature = "local_dynamic_tls") {
            build.flag_if_supported("-ftls-model=local-dynamic");
        } else {
            build.flag_if_supported("-ftls-model=initial-exec");
        }
    }

    let target = if cfg!(feature = "1mib") {
        "snmallocshim-1mib-rust"
    } else if cfg!(feature = "16mib") {
        "snmallocshim-16mib-rust"
    } else {
        panic!("please set a chunk configuration");
    };

    if cfg!(feature = "native-cpu") {
        build.define("SNMALLOC_OPTIMISE_FOR_CURRENT_MACHINE", "ON");
		build.flag_if_supported("-march=native");
    }

    if cfg!(feature = "stats") {
        build.define("USE_SNMALLOC_STATS", "ON");
    }

    if cfg!(feature = "qemu") {
        build.define("SNMALLOC_QEMU_WORKAROUND", "ON");
    }

    if cfg!(feature = "cache-friendly") {
        build.define("CACHE_FRIENDLY_OFFSET", "64");
    }

    build.compile(target);

    if cfg!(feature = "android-shared-stl") {
        println!("cargo:rustc-link-lib=dylib=c++_shared");
    }

    if target_env == "msvc" {
        println!("cargo:rustc-link-lib=dylib=mincore");
    }

    if target_os=="windows" && target_env == "gnu" {
        println!("cargo:rustc-link-lib=dylib=stdc++");
        println!("cargo:rustc-link-lib=dylib=atomic");
    }

    if target_os == "macos" {
        println!("cargo:rustc-link-lib=dylib=c++");
    }

    if target_os == "openbsd" {
        println!("cargo:rustc-link-lib=dylib=c++");
    }

    if target_os == "freebsd" {
        println!("cargo:rustc-link-lib=dylib=c++");
    }

    if target_os == "linux" {
        println!("cargo:rustc-link-lib=dylib=stdc++");
        println!("cargo:rustc-link-lib=dylib=atomic");
    };
}
