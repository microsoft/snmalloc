use std::sync::mpsc::channel;
use std::thread;
use std::time::Instant;
use std::alloc::Layout;

#[global_allocator]
static ALLOC: snmalloc_rs::SnMalloc = snmalloc_rs::SnMalloc;

const BLOCK_SIZE: usize = 64;
const ITERATIONS: usize = 1_000_000;

struct Ptr(*mut u8);
unsafe impl Send for Ptr {}

fn main() {
    let thread_count = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4);
    println!("Running contention benchmark with {} threads, {} iterations per thread", thread_count, ITERATIONS);

    // Use std::sync::Barrier
    let barrier = std::sync::Arc::new(std::sync::Barrier::new(thread_count + 1));

    let mut senders = Vec::new();
    let mut receivers = Vec::new();

    // Create a ring topology channels
    for _ in 0..thread_count {
        let (tx, rx) = channel::<Ptr>();
        senders.push(tx);
        receivers.push(Some(rx));
    }

    let mut handles = Vec::new();

    // Start timing from here, but actual work starts after barrier
    let _start = Instant::now();

    for i in 0..thread_count {
        let barrier = barrier.clone();
        // Thread i sends to (i + 1) % N
        let tx = senders[(i + 1) % thread_count].clone();
        // Thread i receives from i
        let rx = receivers[i].take().unwrap();
        
        handles.push(thread::spawn(move || {
            // Pre-allocate some items to fill the pipe
            let layout = Layout::from_size_align(BLOCK_SIZE, 8).unwrap();
            
            barrier.wait(); // Synchronize start

            for _ in 0..ITERATIONS {
                // 1. Allocate a new block
                let ptr = unsafe { std::alloc::alloc(layout) };
                
                // 2. Send to next neighbor (who will free it)
                tx.send(Ptr(ptr)).unwrap();
                
                // 3. Receive from prev neighbor (who allocated it)
                let received = rx.recv().unwrap();
                
                // 4. Free the received block
                unsafe { std::alloc::dealloc(received.0, layout) };
            }
        }));
    }

    barrier.wait(); // Start timing
    let loop_start = Instant::now();
    
    for h in handles {
        h.join().unwrap();
    }

    let duration = loop_start.elapsed();
    println!("Benchmark completed in {:.2?}", duration);
    println!("Throughput: {:.2} Mops/sec", (thread_count * ITERATIONS) as f64 / duration.as_secs_f64() / 1_000_000.0);
}
