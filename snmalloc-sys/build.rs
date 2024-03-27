#[cfg(feature = "build_cc")]
fn main() {
    let (debug, optim_unix, optim_msvc, prof_msvc_hint) = if cfg!(feature = "debug") {
        (true, "-O0", "/O0", "/DEBUG")
    } else {
        (false, "-O3", "/O2", "/RELEASE")
    };
    let mut build = cc::Build::new();
    build.include("snmalloc/src/snmalloc");
    build.file("snmalloc/src/snmalloc/override/rust.cc".to_string());
    build.flag_if_supported("/O2");
    build.flag_if_supported("/Zi");
    build.flag_if_supported("/nologo");
    build.flag_if_supported("/W4");
    build.flag_if_supported("/WX");
    build.flag_if_supported("/wd4127");
    build.flag_if_supported("/wd4324");
    build.flag_if_supported("/wd4201");
    build.flag_if_supported("/Ob2");
    build.flag_if_supported("/DNDEBUG");
    build.flag_if_supported("/EHsc");
    build.flag_if_supported("/Gd");
    build.flag_if_supported("/TP");
    build.flag_if_supported("/Gm-");
    build.flag_if_supported("/GS");
    build.flag_if_supported("/fp:precise");
    build.flag_if_supported("/Zc:wchar_t");
    build.flag_if_supported("/Zc:forScope");
    build.flag_if_supported("/Zc:inline");
    build.flag_if_supported(prof_msvc_hint);
    build.flag_if_supported(optim_msvc);
    build.flag_if_supported(optim_unix);
    build.flag_if_supported("-mcx16");
    build.flag_if_supported("-fno-exceptions");
    build.flag_if_supported("-fno-rtti");
    build.flag_if_supported("-g");
    build.flag_if_supported("-fomit-frame-pointer");
    build.flag_if_supported("-fpermissive");
    build.static_crt(true);
    build.cpp(true);
    build.debug(debug);
    if cfg!(feature = "usecxx17") {
        build.flag_if_supported("-std=c++17");
        build.flag_if_supported("/std:c++17");
        build.flag_if_supported("-Wc++17-extensions");
        build.flag_if_supported("/Wc++17-extensions");
        build.define("SNMALLOC_USE_CXX17", "1");
    } else {
        build.flag_if_supported("-std=c++20");
        build.flag_if_supported("/std:c++20");
        build.flag_if_supported("-Wc++20-extensions");
        build.flag_if_supported("/Wc++20-extensions");
    }

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
            build.define("ANDROID_ABI", "x86");
        } else if triple.contains("neon") {
            build.define("ANDROID_ABI", "armeabi-v7a with NEON");
        } else if triple.contains("arm") {
            build.define("ANDROID_ABI", "armeabi-v7a");
        }
    }

    if target_family == "unix" || target_env == "gnu" && target_os != "haiku" {
        if cfg!(feature = "local_dynamic_tls") {
            build.flag_if_supported("-ftls-model=local-dynamic");
        } else {
            build.flag_if_supported("-ftls-model=initial-exec");
        }
    }

    if cfg!(feature = "win8compat") {
        build.define("WINVER", "0x0603");
    }

    let target = if cfg!(feature = "check") {
        "snmallocshim-rust"
    } else {
        "snmallocshim-checks-rust"
    };

    if cfg!(feature = "native-cpu") {
        build.define("SNMALLOC_OPTIMISE_FOR_CURRENT_MACHINE", "ON");
        build.flag_if_supported("-march=native");
    }

    if cfg!(feature = "qemu") {
        build.define("SNMALLOC_QEMU_WORKAROUND", "ON");
    }

    if cfg!(feature = "cache-friendly") {
        eprintln!(
            "cache-friendly feature flag is deprecated and no longer has any effect. \
            it may be removed in a future release"
        );
        // The following code no longer works
        // build.define("CACHE_FRIENDLY_OFFSET", "64");
    }

    if cfg!(feature = "lto") {
        build.define("SNMALLOC_IPO", "ON");
    }

    if cfg!(feature = "notls") {
        build.define("SNMALLOC_ENABLE_DYNAMIC_LOADING", "ON");
    }

    build.compile(target);

    if target_env == "msvc" {
        if cfg!(not(feature = "win8compat")) {
            println!("cargo:rustc-link-lib=dylib=mincore");
        }
    }

    if target_os == "windows" && target_env == "gnu" {
        println!("cargo:rustc-link-lib=dylib=atomic");
    }

    if target_os == "linux" {
        println!("cargo:rustc-link-lib=dylib=atomic");
    };

    if cfg!(target_os = "freebsd") {
        // using THREAD_DESTRUCTOR
    } else if cfg!(all(unix, not(target_os = "macos"))) {
        if cfg!(target_env = "gnu") {
            println!("cargo:rustc-link-lib=c_nonshared");
        }
    } else if cfg!(windows) {
        // no need
    } else {
        // link c++ runtime
        println!(
            "cargo:rustc-link-lib={}",
            std::env::var("CXXSTDLIB").unwrap_or_else(|_| {
                if cfg!(target_os = "macos") || cfg!(target_os = "openbsd") {
                    "c++".to_string()
                } else {
                    "stdc++".to_string()
                }
            })
        )
    }
}

#[cfg(not(feature = "build_cc"))]
fn main() {
    let mut cfg = &mut cmake::Config::new("snmalloc");

    let build_type = if cfg!(feature = "debug") {
        "Debug"
    } else {
        "Release"
    };

    cfg = cfg
        .define("SNMALLOC_RUST_SUPPORT", "ON")
        .profile(build_type)
        .very_verbose(true);

    let triple = std::env::var("TARGET").unwrap();
    if triple.contains("android") {
        if let Ok(ndk) = std::env::var("ANDROID_NDK") {
            cfg = cfg.define(
                "CMAKE_TOOLCHAIN_FILE",
                format!("{}/build/cmake/android.toolchain.cmake", ndk),
            );
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
            cfg = cfg
                .define("ANDROID_ABI", "armeabi-v7a")
                .define("ANDROID_ARM_MODE", "arm");
        } else if triple.contains("x86_64") {
            cfg = cfg.define("ANDROID_ABI", "x86_64");
        } else if triple.contains("i686") {
            cfg = cfg.define("ANDROID_ABI", "x86");
        } else if triple.contains("neon") {
            cfg = cfg.define("ANDROID_ABI", "armeabi-v7a with NEON")
        } else if triple.contains("arm") {
            cfg = cfg.define("ANDROID_ABI", "armeabi-v7a");
        }
    }

    if cfg!(all(windows, target_env = "msvc")) {
        cfg = cfg.define("CMAKE_CXX_FLAGS_RELEASE", "/O2 /Ob2 /DNDEBUG /EHsc");
        cfg = cfg.define("CMAKE_C_FLAGS_RELEASE", "/O2 /Ob2 /DNDEBUG /EHsc");
        if cfg!(feature = "win8compat") {
            cfg = cfg.define("WIN8COMPAT", "ON")
        }
    }

    if cfg!(all(windows, target_env = "gnu")) {
        cfg = cfg.define("CMAKE_SH", "CMAKE_SH-NOTFOUND");
    }

    let target = if cfg!(feature = "check") {
        "snmallocshim-checks-rust"
    } else {
        "snmallocshim-rust"
    };

    if cfg!(feature = "native-cpu") {
        cfg = cfg.define("SNMALLOC_OPTIMISE_FOR_CURRENT_MACHINE", "ON")
    }

    if cfg!(feature = "usecxx17") {
        cfg = cfg.define("SNMALLOC_USE_CXX17", "ON")
    }

    if cfg!(feature = "stats") {
        cfg = cfg.define("USE_SNMALLOC_STATS", "ON")
    }

    if cfg!(feature = "qemu") {
        cfg = cfg.define("SNMALLOC_QEMU_WORKAROUND", "ON")
    }

    let mut dst = cfg.build_target(target).build();

    dst.push("./build");

    println!("cargo:rustc-link-lib={}", target);

    if cfg!(all(windows, target_env = "msvc")) {
        if cfg!(not(feature = "win8compat")) {
            println!("cargo:rustc-link-lib=dylib=mincore");
        }
        println!(
            "cargo:rustc-link-search=native={}/{}",
            dst.display(),
            build_type
        );
    } else {
        println!("cargo:rustc-link-search=native={}", dst.display());
    }

    if cfg!(all(windows, target_env = "gnu")) {
        let stdout = std::process::Command::new("gcc")
            .args(["-print-search-dirs"])
            .output()
            .unwrap_or_else(|_| {
                eprintln!("Cannot run gcc.exe");
                std::process::abort();
            })
            .stdout;

        let outputs = String::from_utf8(stdout).unwrap_or_else(|_| {
            eprintln!("gcc output contains non-utf8 characters");
            std::process::abort();
        });

        outputs
            .lines()
            .filter(|line| line.starts_with("libraries: ="))
            .map(|line| line.split_at("libraries: =".len()).1)
            .flat_map(|line| line.split(';'))
            .for_each(|path| {
                println!("cargo:rustc-link-search=native={}", path);
            });
        println!("cargo:rustc-link-lib=dylib=atomic");
        println!("cargo:rustc-link-lib=dylib=winpthread");
        println!("cargo:rustc-link-lib=dylib=gcc_s");
    }

    // linux: using PTHREAD_DESTRUCTORS
    if cfg!(target_os = "linux") {
        println!("cargo:rustc-link-lib=dylib=atomic");
    }

    if cfg!(target_os = "freebsd") {
        // using THREAD_DESTRUCTOR
    } else if cfg!(all(unix, not(target_os = "macos"))) {
        // using PTHREAD_DESTRUCTOR
        if cfg!(target_env = "gnu") {
            println!("cargo:rustc-link-lib=c_nonshared");
        }
    } else if cfg!(windows) {
        // not need for explicit c++ runtime
    } else {
        // link c++ runtime
        println!(
            "cargo:rustc-link-lib={}",
            std::env::var("CXXSTDLIB").unwrap_or_else(|_| {
                if cfg!(target_os = "macos") || cfg!(target_os = "openbsd") {
                    "c++".to_string()
                } else {
                    "stdc++".to_string()
                }
            })
        )
    }
}
