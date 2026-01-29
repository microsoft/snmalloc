use xshell::{cmd, Shell};
use clap::{Parser, Subcommand};
use regex::Regex;

#[derive(Parser)]
#[command(name = "xtask")]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    CompareBuilds,
}

fn main() -> anyhow::Result<()> {
    let cli = Cli::parse();
    match cli.command {
        Commands::CompareBuilds => compare_builds()?,
    }
    Ok(())
}

fn compare_builds() -> anyhow::Result<()> {
    let sh = Shell::new()?;
    
    println!("=== Benchmarking build_cc (WaitOnAddress enabled) ===");
    // Clean to ensure rebuild
    cmd!(sh, "cargo clean -p snmalloc-sys").run()?;
    // Build and run with build_cc
    // Note: We use release mode for benchmarks
    let output_cc = cmd!(sh, "cargo run --release --example bench_contention --no-default-features --features build_cc,usewait-on-address").read()?;
    println!("{}", output_cc);
    let cc_throughput = parse_throughput(&output_cc);

    println!("\n=== Benchmarking build_cmake (Default, WaitOnAddress enabled) ===");
    cmd!(sh, "cargo clean -p snmalloc-sys").run()?;
    // Build and run with build_cmake (explicitly set to avoid confusion)
    // Note: snmalloc-sys/build_cmake is implied by snmalloc-rs features if mapped, but snmalloc-rs default is build_cmake
    let output_cmake = cmd!(sh, "cargo run --release --example bench_contention --no-default-features --features snmalloc-sys/build_cmake,usewait-on-address").read()?;
    println!("{}", output_cmake);
    let cmake_throughput = parse_throughput(&output_cmake);

    println!("\n=== Results Comparison ===");
    println!("build_cc Throughput:    {:.2} Mops/sec", cc_throughput);
    println!("build_cmake Throughput: {:.2} Mops/sec", cmake_throughput);
    
    let diff = (cc_throughput - cmake_throughput) / cmake_throughput * 100.0;
    println!("Difference:             {:.2}%", diff);

    if diff.abs() < 10.0 {
        println!("Performance is roughly equivalent (within 10%). Fix verified.");
    } else if diff < -10.0 {
        println!("WARNING: build_cc is significantly slower than build_cmake.");
    } else {
        println!("build_cc is faster than build_cmake.");
    }

    Ok(())
}

fn parse_throughput(output: &str) -> f64 {
    let re = Regex::new(r"Throughput: ([\d\.]+) Mops/sec").unwrap();
    if let Some(caps) = re.captures(output) {
        caps[1].parse().unwrap_or(0.0)
    } else {
        0.0
    }
}
