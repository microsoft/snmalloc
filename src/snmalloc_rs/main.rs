use snmalloc_rs;

#[global_allocator]
static GLOBAL: snmalloc_rs::SnMalloc = snmalloc_rs::SnMalloc;

fn main() {
    // This `Vec` will allocate memory through `GLOBAL` above
    println!("allocation a new vec");
    let mut v = Vec::new();
    println!("push an element");
    v.push(1);
    println!("done");

    let mut stats = snmalloc_rs::SnMallocInfo {
        current_memory_usage: 0,
        peak_memory_usage: 0,
    };
    // snmalloc_rs::load_stats(&mut stats); # gets mangled
    println!("current_memory_usage: {}", stats.current_memory_usage);
    println!("peak_memory_usage: {}", stats.peak_memory_usage);
}