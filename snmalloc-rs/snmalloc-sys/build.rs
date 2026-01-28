#![allow(dead_code)]

use std::env;

#[derive(Debug, PartialEq)]
enum Compiler {
    Clang,
    Gcc,
    Msvc,
    Unknown
}

struct BuildConfig {
    debug: bool,
    optim_level: String, 
    target_os: String,
    target_env: String,
    target_family: String,
    target: String,
    out_dir: String,
    build_type: String,
    msystem: Option<String>,
    cmake_cxx_standard: String,  
    target_lib: String,  
    features: BuildFeatures,
    #[cfg(feature = "build_cc")]
    builder: cc::Build,
    #[cfg(not(feature = "build_cc"))]
    builder: cmake::Config,
    compiler: Compiler
}

impl std::fmt::Debug for BuildConfig {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("BuildConfig")
            .field("debug", &self.debug)
            .field("optim_level", &self.optim_level)
            .field("target_os", &self.target_os)
            .field("target_env", &self.target_env)
            .field("target_family", &self.target_family)
            .field("target", &self.target)
            .field("out_dir", &self.out_dir)
            .field("build_type", &self.build_type)
            .field("msystem", &self.msystem)
            .field("cmake_cxx_standard", &self.cmake_cxx_standard)
            .field("target_lib", &self.target_lib)
            .field("features", &self.features)
            .finish()
    }
}

#[derive(Debug, Clone)]
struct BuildFeatures {
    native_cpu: bool,
    qemu: bool,
    wait_on_address: bool,
    lto: bool,
    notls: bool,
    win8compat: bool,
    stats: bool,
    android_lld: bool,
    local_dynamic_tls: bool,
    libc_api: bool,
    tracing: bool,
    fuzzing: bool,
    vendored_stl: bool,
    check_loads: bool,
    pageid: bool,
    gwp_asan: bool,
}

impl BuildConfig {
    fn new() -> Self {
        let debug = cfg!(feature = "debug");
        #[cfg(feature = "build_cc")]
        let builder = cc::Build::new();
        
        #[cfg(not(feature = "build_cc"))]
        let builder = Config::new("../..");

        let mut config = Self {
            debug,
            optim_level: (if debug { "-O0" } else { "-O3" }).to_string(),
            target_os: env::var("CARGO_CFG_TARGET_OS").expect("target_os not defined!"),
            target_env: env::var("CARGO_CFG_TARGET_ENV").expect("target_env not defined!"),
            target_family: env::var("CARGO_CFG_TARGET_FAMILY").expect("target family not set"),
            target: env::var("TARGET").expect("TARGET not set"),
            out_dir: env::var("OUT_DIR").unwrap(),
            build_type: (if debug { "Debug" } else { "Release" }).to_string(),
            msystem: env::var("MSYSTEM").ok(),
            cmake_cxx_standard: (if cfg!(feature = "usecxx17") { "17" } else { "20" }).to_string(),
            target_lib: (if cfg!(feature = "check") {
                "snmallocshim-checks-rust"
            } else {
                "snmallocshim-rust"
            }).to_string(),
            features: BuildFeatures::new(),
            builder,
            compiler: Compiler::Unknown,
        };
        config.compiler = config.detect_compiler();
        config.embed_build_info();
        config
    }

    fn detect_compiler(&self) -> Compiler {
        // Check MSYSTEM for MSYS2 environments
        if let Some(msystem) = &self.msystem {
            match msystem.as_str() {
                "CLANG64" | "CLANGARM64" => return Compiler::Clang,
                "MINGW64" | "UCRT64" => return Compiler::Gcc,
                _ => {}
            }
        }

        // Check target environment
        if let Ok(env) = env::var("CARGO_CFG_TARGET_ENV") {
            match env.as_str() {
                "msvc" => return Compiler::Msvc,
                "gnu" => return Compiler::Gcc,
                _ => {}
            }
        }

        // Check CC environment variable
        if let Ok(cc) = env::var("CC") {
            let cc = cc.to_lowercase();
            if cc.contains("clang") {
                return Compiler::Clang;
            } else if cc.contains("gcc") {
                return Compiler::Gcc;
            }
        }

        // Default based on platform and target
        if self.target.contains("msvc") {
            Compiler::Msvc
        } else if cfg!(windows) {
            Compiler::Gcc // Assume GCC for non-MSVC Windows environments
        } else if cfg!(unix) {
            Compiler::Clang // Default to Clang for Unix-like systems
        } else {
            Compiler::Unknown
        }
    }


    fn embed_build_info(&self) {
        let build_info = [
            ("BUILD_TARGET_OS", &self.target_os),
            ("BUILD_TARGET_ENV", &self.target_env),
            ("BUILD_TARGET_FAMILY", &self.target_family),
            ("BUILD_TARGET", &self.target),
            ("BUILD_CC", &format!("{:#?}", self.compiler)),
            ("BUILD_TYPE", &self.build_type),
            ("BUILD_DEBUG", &self.debug.to_string()),
            ("BUILD_OPTIM_LEVEL", &self.optim_level),
            ("BUILD_CXX_STANDARD", &self.cmake_cxx_standard),
        ];

        for (key, value) in build_info {
            println!("cargo:rustc-env={}={}", key, value);
        }
        
        if let Some(ms) = &self.msystem {
            println!("cargo:rustc-env=BUILD_MSYSTEM={}", ms);
        }
    }

    fn get_cpp_flags(&self) -> [&'static str; 2] {
        if cfg!(feature = "usecxx17") {
            ["-std=c++17", "/std:c++17"]
        } else {
            ["-std=c++20", "/std:c++20"]
        }
    }

    fn is_msvc(&self) -> bool {
        self.target_env == "msvc"
    }

    fn is_gnu(&self) -> bool {
        self.target_env == "gnu"
    }

    fn is_windows(&self) -> bool {
        self.target_os == "windows"
    }

    fn is_linux(&self) -> bool {
        self.target_os == "linux"
    }

    fn is_unix(&self) -> bool {
        self.target_family == "unix"
    }

    fn is_clang_msys(&self) -> bool {
        self.msystem.as_deref().map_or(false, |s| s.contains("CLANG"))
    }

    fn is_ucrt64(&self) -> bool {
        self.msystem.as_deref() == Some("UCRT64")
    }
}

trait BuilderDefine {
    fn define(&mut self, key: &str, value: &str) -> &mut Self;
    fn flag_if_supported(&mut self, flag: &str) -> &mut Self;
    fn build_lib(&mut self, target_lib: &str) -> std::path::PathBuf;
    fn configure_output_dir(&mut self, out_dir: &str) -> &mut Self;
    fn configure_cpp(&mut self, debug: bool) -> &mut Self;
    fn compiler_define(&mut self, key: &str, value: &str) -> &mut Self;
}

#[cfg(feature = "build_cc")]
impl BuilderDefine for cc::Build {
    fn define(&mut self, key: &str, value: &str) -> &mut Self {
        self.define(key, Some(value))
    }
    
    fn flag_if_supported(&mut self, flag: &str) -> &mut Self {
        self.flag_if_supported(flag)
    }
    
    fn build_lib(&mut self, target_lib: &str) -> std::path::PathBuf {
        self.compile(target_lib);
        std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap())
    }

    fn configure_output_dir(&mut self, out_dir: &str) -> &mut Self {
        self.out_dir(out_dir)
    }

    fn configure_cpp(&mut self, debug: bool) -> &mut Self {
        self.include("../../src")
            .file("../../src/snmalloc/override/rust.cc")
            .cpp(true)
            .debug(debug)
            .static_crt(true)
    }

    fn compiler_define(&mut self, key: &str, value: &str) -> &mut Self {
        self.define(key, Some(value))
    }
}

#[cfg(not(feature = "build_cc"))]
impl BuilderDefine for cmake::Config {
    fn define(&mut self, key: &str, value: &str) -> &mut Self {
        self.define(key, value)
    }
    
    fn flag_if_supported(&mut self, _flag: &str) -> &mut Self {
        self
    }
    
    fn build_lib(&mut self, target_lib: &str) -> std::path::PathBuf {
        self.build_target(target_lib).build()
    }

    fn configure_output_dir(&mut self, out_dir: &str) -> &mut Self {
        self.out_dir(out_dir)
    }

    fn configure_cpp(&mut self, debug: bool) -> &mut Self {
        self.profile(if debug { "Debug" } else { "Release" });
        self.define("SNMALLOC_RUST_SUPPORT", "ON")
            .very_verbose(true)
            .define("CMAKE_SH", "CMAKE_SH-NOTFOUND")
            .always_configure(true)
            .static_crt(true)
    }

    fn compiler_define(&mut self, key: &str, value: &str) -> &mut Self {
        self.cxxflag(format!("-D{}={}", key, value))
            .cflag(format!("-D{}={}", key, value))
    }
}

fn apply_defines<T: BuilderDefine>(builder: &mut T, defines: &[(&str, &str)]) {
    for (key, value) in defines {
        builder.define(key, value);
    }
}
impl BuildFeatures {
    fn new() -> Self {
        Self {
            native_cpu: cfg!(feature = "native-cpu"),
            qemu: cfg!(feature = "qemu"),
            wait_on_address: cfg!(feature = "usewait-on-address"),
            lto: cfg!(feature = "lto"),
            notls: cfg!(feature = "notls"),
            win8compat: cfg!(feature = "win8compat"),
            stats: cfg!(feature = "stats"),
            android_lld: cfg!(feature = "android-lld"),
            local_dynamic_tls: cfg!(feature = "local_dynamic_tls"),
            libc_api: cfg!(feature = "libc-api"),
            tracing: cfg!(feature = "tracing"),
            fuzzing: cfg!(feature = "fuzzing"),
            vendored_stl: cfg!(feature = "vendored-stl"),
            check_loads: cfg!(feature = "check-loads"),
            pageid: cfg!(feature = "pageid"),
            gwp_asan: cfg!(feature = "gwp-asan"),
        }
    }
}

fn configure_platform(config: &mut BuildConfig) {
    // Basic optimization and compiler flags
    config.builder
        .flag_if_supported(&config.optim_level)
        .flag_if_supported("-fomit-frame-pointer");

    // C++ standard flags
    for std in config.get_cpp_flags() {
        config.builder.flag_if_supported(std);
    }

    // Common feature configurations
    if config.features.native_cpu {
        config.builder.define("SNMALLOC_OPTIMISE_FOR_CURRENT_MACHINE", "ON");
        #[cfg(feature = "build_cc")]
        config.builder.flag_if_supported("-march=native");
    }

    // GCC LTO support - ensure fat LTO objects are created so they can be used by linkers that don't support LTO plugin
    if config.features.lto && matches!(config.compiler, Compiler::Gcc) && !config.is_msvc() {
        #[cfg(feature = "build_cc")]
        config.builder.flag_if_supported("-ffat-lto-objects");
    }

    // Platform-specific configurations
    if config.is_windows() {
        if config.features.win8compat {
            // Windows 8.1 (0x0603) for compatibility mode
            config.builder.compiler_define("WINVER", "0x0603");
            config.builder.compiler_define("_WIN32_WINNT", "0x0603");
        } else {
            // Windows 10 (0x0A00) default to enable VirtualAlloc2 and WaitOnAddress
            // snmalloc requires NTDDI_WIN10_RS5 logic for these features in pal_windows.h
            config.builder.compiler_define("WINVER", "0x0A00");
            config.builder.compiler_define("_WIN32_WINNT", "0x0A00");
        }

        if config.is_msvc() {
            let msvc_flags = vec![
                "/nologo", "/W4", "/WX", "/wd4127", "/wd4324", "/wd4201",
                "/Ob2", "/EHsc", "/Gd", "/TP", "/Gm-", "/GS",
                "/fp:precise", "/Zc:wchar_t", "/Zc:forScope", "/Zc:inline"
            ];
            for flag in msvc_flags {
                config.builder.flag_if_supported(flag);
            }
            
            if !config.debug {
                #[cfg(feature = "build_cc")]
                config.builder.define("NDEBUG", None);
            }
            
            if config.features.lto {
                config.builder
                    .flag_if_supported("/GL")
                    .define("CMAKE_INTERPROCEDURAL_OPTIMIZATION", "TRUE")
                    .define("SNMALLOC_IPO", "ON");
                println!("cargo:rustc-link-arg=/LTCG");
            }
            
            config.builder
                .define("CMAKE_CXX_FLAGS_RELEASE", "/O2 /Ob2 /DNDEBUG /EHsc")
                .define("CMAKE_C_FLAGS_RELEASE", "/O2 /Ob2 /DNDEBUG /EHsc");
        } else {
            let common_flags = vec!["-mcx16", "-fno-exceptions", "-fno-rtti", "-pthread"];
            for flag in common_flags {
                config.builder.flag_if_supported(flag);
            }
            // Ensure consistent Windows version targeting
            if config.features.win8compat {
                // Windows 8.1 (0x0603) for compatibility mode
                config.builder.compiler_define("WINVER", "0x0603");
                config.builder.compiler_define("_WIN32_WINNT", "0x0603");
            } else {
                // Windows 10 (0x0A00) default to enable VirtualAlloc2 and WaitOnAddress
                // snmalloc requires NTDDI_WIN10_RS5 logic for these features in pal_windows.h
                config.builder.compiler_define("WINVER", "0x0A00");
                config.builder.compiler_define("_WIN32_WINNT", "0x0A00");
            }

            if let Some(msystem) = &config.msystem {
                match msystem.as_str() {
                    "CLANG64" | "CLANGARM64" => {
                        let defines = vec![
                            ("CMAKE_CXX_COMPILER", "clang++"),
                            ("CMAKE_C_COMPILER", "clang"),
                            ("CMAKE_CXX_FLAGS", "-fuse-ld=lld -stdlib=libc++ -mcx16 -Wno-error=unknown-pragmas -Qunused-arguments"),
                            ("CMAKE_C_FLAGS", "-fuse-ld=lld -Wno-error=unknown-pragmas -Qunused-arguments"),
                            ("CMAKE_EXE_LINKER_FLAGS", "-fuse-ld=lld -stdlib=libc++"),
                            ("CMAKE_SHARED_LINKER_FLAGS", "-fuse-ld=lld -stdlib=libc++")
                        ];
                        apply_defines(&mut config.builder, &defines);
                        if config.features.lto {
                            config.builder.flag_if_supported("-flto=thin");
                        }
                    }
                    "UCRT64" | "MINGW64" => {
                        let defines = vec![
                            ("CMAKE_CXX_FLAGS", "-fuse-ld=lld -Wno-error=unknown-pragmas"),
                            ("CMAKE_SYSTEM_NAME", "Windows"),
                            ("CMAKE_C_FLAGS", "-fuse-ld=lld -Wno-error=unknown-pragmas"),
                            ("CMAKE_EXE_LINKER_FLAGS", "-fuse-ld=lld"),
                            ("CMAKE_SHARED_LINKER_FLAGS", "-fuse-ld=lld")
                        ];
                        apply_defines(&mut config.builder, &defines);
                    }
                    _ => {}
                }
            }
        }
    } else if config.is_unix() {
        let unix_flags = vec!["-fPIC", "-pthread", "-fno-exceptions", "-fno-rtti", "-mcx16", "-Wno-unused-parameter"];
        for flag in unix_flags {
            config.builder.flag_if_supported(flag);
        }

        if config.target_os == "freebsd" {
            config.builder.flag_if_supported("-w");
        }

        if config.target_os != "haiku" {
            let tls_model = if config.features.local_dynamic_tls { "-ftls-model=local-dynamic" } else { "-ftls-model=initial-exec" };
            config.builder.flag_if_supported(tls_model);
        }
        
        #[cfg(feature = "build_cc")]
        if config.target_os == "linux" || config.target_os == "android" {
            config.builder.define("SNMALLOC_HAS_LINUX_FUTEX_H", None);
            config.builder.define("SNMALLOC_HAS_LINUX_RANDOM_H", None);
            config.builder.define("SNMALLOC_PLATFORM_HAS_GETENTROPY", None);
        }
    }

    // Feature configurations
    config.builder
        .define("SNMALLOC_QEMU_WORKAROUND", if config.features.qemu { "ON" } else { "OFF" })
        .define("SNMALLOC_ENABLE_DYNAMIC_LOADING", if config.features.notls { "ON" } else { "OFF" })
        .define("USE_SNMALLOC_STATS", if config.features.stats { "ON" } else { "OFF" })
        .define("SNMALLOC_RUST_LIBC_API", if config.features.libc_api { "ON" } else { "OFF" })
        .define("SNMALLOC_USE_CXX17", if cfg!(feature = "usecxx17") { "ON" } else { "OFF" });

    if config.features.tracing {
        config.builder.define("SNMALLOC_TRACING", "ON");
    }
    if config.features.fuzzing {
        config.builder.define("SNMALLOC_ENABLE_FUZZING", "ON");
    }
    if config.features.vendored_stl {
        #[cfg(feature = "build_cc")]
        config.builder.define("SNMALLOC_USE_SELF_VENDORED_STL", "1");
        #[cfg(not(feature = "build_cc"))]
        config.builder.define("SNMALLOC_USE_SELF_VENDORED_STL", "ON");
    }
    
    if config.features.check_loads {
        #[cfg(feature = "build_cc")]
        config.builder.define("SNMALLOC_CHECK_LOADS", "true");
        #[cfg(not(feature = "build_cc"))]
        config.builder.define("SNMALLOC_CHECK_LOADS", "ON");
    } else {
        #[cfg(feature = "build_cc")]
        config.builder.define("SNMALLOC_CHECK_LOADS", "false");
        #[cfg(not(feature = "build_cc"))]
        config.builder.define("SNMALLOC_CHECK_LOADS", "OFF");
    }

    if config.features.pageid {
        #[cfg(feature = "build_cc")]
        config.builder.define("SNMALLOC_PAGEID", "true");
        #[cfg(not(feature = "build_cc"))]
        config.builder.define("SNMALLOC_PAGEID", "ON");
    } else {
        #[cfg(feature = "build_cc")]
        config.builder.define("SNMALLOC_PAGEID", "false");
        #[cfg(not(feature = "build_cc"))]
        config.builder.define("SNMALLOC_PAGEID", "OFF");
    }

    if config.features.gwp_asan {
        config.builder.define("SNMALLOC_ENABLE_GWP_ASAN_INTEGRATION", "ON");
        if let Ok(path) = env::var("SNMALLOC_GWP_ASAN_INCLUDE_PATH") {
            config.builder.define("SNMALLOC_GWP_ASAN_INCLUDE_PATH", path.as_str());
        }
        if let Ok(path) = env::var("SNMALLOC_GWP_ASAN_LIBRARY_PATH") {
            config.builder.define("SNMALLOC_GWP_ASAN_LIBRARY_PATH", path.as_str());
        }
    }

    // Handle wait_on_address configuration for different build systems
    if config.features.wait_on_address {
        #[cfg(feature = "build_cc")]
        config.builder.define("SNMALLOC_USE_WAIT_ON_ADDRESS", "1");
        
        #[cfg(not(feature = "build_cc"))]
        config.builder.define("SNMALLOC_ENABLE_WAIT_ON_ADDRESS", "ON");
    } else {
        #[cfg(feature = "build_cc")]
        config.builder.define("SNMALLOC_USE_WAIT_ON_ADDRESS", "0");
        
        #[cfg(not(feature = "build_cc"))]
        config.builder.define("SNMALLOC_ENABLE_WAIT_ON_ADDRESS", "OFF");
    }

    // Android configuration
    if config.target.contains("android") {
        let ndk = env::var("ANDROID_NDK").expect("ANDROID_NDK environment variable not set");
        config.builder
            .define("CMAKE_TOOLCHAIN_FILE", &*format!("{}/build/cmake/android.toolchain.cmake", ndk))
            .define("ANDROID_PLATFORM", &*env::var("ANDROID_PLATFORM").unwrap_or_default());

        if cfg!(feature = "android-lld") {
            config.builder.define("ANDROID_LD", "lld");
        }

        let (abi, arm_mode) = match config.target.as_str() {
            t if t.contains("aarch64") => ("arm64-v8a", None),
            t if t.contains("armv7") => ("armeabi-v7a", Some("arm")),
            t if t.contains("x86_64") => ("x86_64", None),
            t if t.contains("i686") => ("x86", None),
            t if t.contains("neon") => ("armeabi-v7a with NEON", None),
            t if t.contains("arm") => ("armeabi-v7a", None),
            _ => panic!("Unsupported Android architecture: {}", config.target),
        };
        config.builder.define("ANDROID_ABI", abi);
        if let Some(mode) = arm_mode {
            config.builder.define("ANDROID_ARM_MODE", mode);
        }
    }
}


fn configure_linking(config: &BuildConfig) {

    match () {
        _ if config.is_msvc() => {
            // Windows MSVC specific libraries
            if !config.features.win8compat {
                println!("cargo:rustc-link-lib=mincore");
            }
            // Essential Windows libraries
            println!("cargo:rustc-link-lib=kernel32");
            println!("cargo:rustc-link-lib=user32");
            println!("cargo:rustc-link-lib=advapi32");
            println!("cargo:rustc-link-lib=ws2_32");
            println!("cargo:rustc-link-lib=userenv");
            println!("cargo:rustc-link-lib=bcrypt");
            if config.debug {
                println!("cargo:rustc-link-lib=msvcrtd");
            } else {
                println!("cargo:rustc-link-lib=msvcrt");
            }
        }
        _ if config.is_windows() && config.is_gnu() => {
            println!("cargo:rustc-link-lib=kernel32");
            println!("cargo:rustc-link-lib=bcrypt");
            println!("cargo:rustc-link-lib=winpthread");

            if config.is_clang_msys() {
                println!("cargo:rustc-link-lib=c++");
            } else if config.is_ucrt64() {
                println!("cargo:rustc-link-lib=stdc++");
            } else {
                println!("cargo:rustc-link-lib=stdc++");
                println!("cargo:rustc-link-lib=atomic");
            }
        }
        _ if cfg!(target_os = "freebsd") => {
            println!("cargo:rustc-link-lib=c++");
        }
        _ if config.is_linux() => {
            println!("cargo:rustc-link-lib=atomic");
            println!("cargo:rustc-link-lib=stdc++");
            println!("cargo:rustc-link-lib=pthread");
            println!("cargo:rustc-link-lib=c");
            println!("cargo:rustc-link-lib=gcc_s");
            println!("cargo:rustc-link-lib=util");
            println!("cargo:rustc-link-lib=rt");
            println!("cargo:rustc-link-lib=dl");
            println!("cargo:rustc-link-lib=m");
            
            // Force rust-lld
            println!("cargo:rustc-link-arg=-fuse-ld=lld");

            if cfg!(feature = "usecxx17") && !config.is_clang_msys() {
                println!("cargo:rustc-link-lib=gcc");
            }
        }
        _ if config.is_unix() && !cfg!(any(target_os = "macos", target_os = "freebsd")) => {
            if config.is_gnu() {
                println!("cargo:rustc-link-lib=c_nonshared");
            }
        }
        _ if !config.is_windows() => {
            let cxxlib = if cfg!(any(target_os = "macos", target_os = "openbsd")) {
                "c++"
            } else {
                "stdc++"
            };
            println!("cargo:rustc-link-lib={}", cxxlib);
        }
        _ => {}
    }
}

#[cfg(feature = "build_cc")]
use cc;
#[cfg(not(feature = "build_cc"))]
use cmake::Config;

fn main() {
    let mut config = BuildConfig::new();
    
    config.builder
        .configure_cpp(config.debug)
        .configure_output_dir(&config.out_dir);

    // Apply all configurations
    configure_platform(&mut config);

    // Build and configure output
    println!("cargo:rustc-link-search=/usr/local/lib");
    println!("cargo:rustc-link-search={}", config.out_dir);
    println!("cargo:rustc-link-search={}/build", config.out_dir);
    println!("cargo:rustc-link-search={}/build/Debug", config.out_dir);
    println!("cargo:rustc-link-search={}/build/Release", config.out_dir);
    let mut _dst = config.builder.build_lib(&config.target_lib);
    
    if config.is_linux() {
        // Use whole-archive to ensure all symbols (including FFI exports) are included
        // This is critical for LTO and ensuring sn_rust_* symbols are available
        println!("cargo:rustc-link-arg=-Wl,--whole-archive");
        println!("cargo:rustc-link-lib=static={}", config.target_lib);
        println!("cargo:rustc-link-arg=-Wl,--no-whole-archive");
    } else {
        println!("cargo:rustc-link-lib={}", config.target_lib);
    }

    configure_linking(&config);
}
