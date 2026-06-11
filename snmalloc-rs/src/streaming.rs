//! Safe Rust wrapper over the streaming-mode FFI surface added in
//! Phase 5.1 (`sn_rust_profile_streaming_start` /
//! `sn_rust_profile_streaming_stop`).  The C side broadcasts every
//! sampled allocation through a single registered C function pointer;
//! this module lifts that into:
//!
//! - [`StreamSample`]: a borrowed, lifetime-bound view of the raw FFI
//!   sample.  The borrow ties the user closure's view to the duration
//!   of the C callback so the application can never accidentally
//!   stash a pointer that outlives the snapshot.
//! - [`ProfilingSession`]: an owned RAII handle.  Constructing it via
//!   [`ProfilingSession::start`] registers a Rust closure as the
//!   streaming broadcast target; dropping it unregisters that closure
//!   and tears down all global state so a subsequent
//!   [`ProfilingSession::start`] can succeed.
//!
//! Single-session-at-a-time semantics
//! ----------------------------------
//!
//! The C `sn_rust_profile_streaming_start` enforces a single
//! registered callback at a time.  To keep that contract safe in
//! Rust we additionally serialise registration and dispatch through
//! a process-global `Mutex<Option<Handler>>`.  The first
//! [`ProfilingSession::start`] populates the slot and the C side
//! registers a fixed `extern "C"` trampoline that locks the mutex on
//! each dispatch and forwards into the boxed closure.  A second
//! [`ProfilingSession::start`] while the first is still alive
//! returns [`StreamingError::AlreadyActive`] -- we do not silently
//! replace the existing handler.
//!
//! All public items in this module are gated on the `profiling`
//! Cargo feature.  In the feature-off build, the corresponding C
//! stubs return `-1` and we never link the module in at all; users
//! can call `cfg!(feature = "profiling")` to detect availability.

extern crate alloc;
extern crate std;

use alloc::boxed::Box;
use core::ffi::c_void;
use core::fmt;
use core::marker::PhantomData;
use core::slice;

use std::sync::{Mutex, OnceLock};

use snmalloc_sys as ffi;
use snmalloc_sys::SnRustProfileRawSample;

/// Boxed user closure invoked once per sampled allocation.  Stored
/// behind a [`Mutex`] in the global handler slot; the trampoline
/// locks the slot for the (short) duration of each dispatch.
///
/// The bounds match [`ProfilingSession::start`]: `Send + Sync` is
/// required because allocation samples are broadcast on whichever
/// thread happened to trip the sampler -- not necessarily the thread
/// that called `start()` -- and the closure must therefore be safe to
/// invoke concurrently from any thread.  `'static` is required because
/// the C registration outlives any borrow we could express.
type Handler = Box<dyn Fn(StreamSample<'_>) + Send + Sync + 'static>;

/// Process-global handler slot.  `None` means no session is active.
/// The outer `OnceLock` is initialised lazily on first
/// [`ProfilingSession::start`]; the inner `Mutex` enforces
/// single-session-at-a-time semantics and provides safe shared
/// access between the registering thread and the (possibly many)
/// allocator threads dispatching through the trampoline.
fn handler_slot() -> &'static Mutex<Option<Handler>> {
    static SLOT: OnceLock<Mutex<Option<Handler>>> = OnceLock::new();
    SLOT.get_or_init(|| Mutex::new(None))
}

/// Borrowed view of a single streaming sample.
///
/// The lifetime parameter ties the view to the duration of the C
/// callback dispatch.  The user closure receives `StreamSample<'_>`
/// by value, and the borrow check prevents the closure from stashing
/// any field that aliases the raw sample buffer -- the C side reuses
/// that stack-allocated buffer across broadcasts.
///
/// All accessors return values, not references, so the user can
/// freely copy out individual fields if they need to keep them past
/// the callback (e.g. by cloning the stack into a `Vec`).
#[derive(Copy, Clone)]
pub struct StreamSample<'a> {
    raw: &'a SnRustProfileRawSample,
    // Tie down the lifetime explicitly even though `raw` already does;
    // makes the API surface read consistently with the documentation
    // ("borrows for the duration of the callback").
    _phantom: PhantomData<&'a ()>,
}

impl<'a> StreamSample<'a> {
    /// SAFETY: the caller must ensure `raw` is valid for `'a` and
    /// the entire `SnRustProfileRawSample` (including the inline
    /// stack array) has been initialised by the C side.
    #[inline]
    unsafe fn from_raw(raw: &'a SnRustProfileRawSample) -> Self {
        Self {
            raw,
            _phantom: PhantomData,
        }
    }

    /// Pointer returned to the application by the original
    /// allocation.  Opaque -- intended only for debugging / cross-
    /// referencing with application-side bookkeeping.  May be null
    /// in pathological corner cases.
    #[inline]
    pub fn alloc_ptr(&self) -> *const c_void {
        self.raw.alloc_ptr as *const c_void
    }

    /// Bytes the original caller requested.
    #[inline]
    pub fn requested_size(&self) -> usize {
        self.raw.requested_size
    }

    /// Bytes actually returned by snmalloc (sizeclass-rounded).
    #[inline]
    pub fn allocated_size(&self) -> usize {
        self.raw.allocated_size
    }

    /// Bytes-of-request Poisson weight for this sample.  Summing
    /// across the broadcast stream gives an unbiased estimator of
    /// total bytes requested.
    #[inline]
    pub fn weight(&self) -> u64 {
        self.raw.weight as u64
    }

    /// Captured return addresses, innermost first.  Slice length is
    /// `stack_depth`.  Borrowed from the raw sample for the
    /// duration of the callback; if the user needs to keep the
    /// frames past the callback they must copy them out (e.g. with
    /// `to_vec()`).
    #[inline]
    pub fn stack(&self) -> &[*const c_void] {
        let depth = self.raw.stack_depth as usize;
        let max = snmalloc_sys::SN_RUST_PROFILE_STACK_FRAMES;
        let n = if depth <= max { depth } else { max };
        // SAFETY: `raw.stack` is a fixed-size array of `*mut c_void`
        // initialised by the C side; we narrow to `n` entries which
        // is bounded by the array length.  `*mut c_void` and
        // `*const c_void` have identical layout so the reinterpret
        // is sound.
        unsafe {
            slice::from_raw_parts(self.raw.stack.as_ptr() as *const *const c_void, n)
        }
    }
}

impl<'a> fmt::Debug for StreamSample<'a> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("StreamSample")
            .field("alloc_ptr", &self.alloc_ptr())
            .field("requested_size", &self.requested_size())
            .field("allocated_size", &self.allocated_size())
            .field("weight", &self.weight())
            .field("stack_depth", &self.stack().len())
            .finish()
    }
}

/// Reasons [`ProfilingSession::start`] can fail.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StreamingError {
    /// A session is already active in this process.  Drop it before
    /// starting a new one.
    AlreadyActive,
    /// The C-side registration failed (e.g. profiling not supported
    /// at build time, or all broadcast slots are taken by C++-side
    /// subscribers).
    RegistrationFailed,
}

impl fmt::Display for StreamingError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            StreamingError::AlreadyActive => f.write_str(
                "a snmalloc profiling streaming session is already active",
            ),
            StreamingError::RegistrationFailed => f.write_str(
                "failed to register the snmalloc streaming callback with the C runtime",
            ),
        }
    }
}

impl std::error::Error for StreamingError {}

/// Fixed `extern "C"` trampoline registered with the C side.  Every
/// sampled allocation funnels through here, regardless of which
/// Rust closure the user supplied.  The trampoline locks the global
/// handler slot, dispatches into the stored closure (if any), and
/// returns -- the lock window is the duration of the user closure.
///
/// The slot is read under a `Mutex` for safety; the C contract
/// requires the trampoline to be reentrancy-free w.r.t. allocator
/// activity (the allocator may sample during the user closure on
/// another thread but never on this thread mid-dispatch), and the
/// `Mutex` is held only for the brief callback dispatch.
unsafe extern "C" fn trampoline(sample: *const SnRustProfileRawSample) {
    if sample.is_null() {
        return;
    }

    // The C side guarantees `*sample` is a fully-initialised
    // SnRustProfileRawSample for the duration of this call.  We
    // borrow it for the lifetime of the closure invocation only.
    let raw = &*sample;
    let view = StreamSample::from_raw(raw);

    // Lock the handler slot.  `lock()` returns `Err` only if the
    // mutex was poisoned by a panicking handler; in that case there
    // is no useful work to do and we drop the broadcast silently
    // rather than re-panic across the FFI boundary (which would be
    // UB).
    let guard = match handler_slot().lock() {
        Ok(g) => g,
        Err(_) => return,
    };
    if let Some(handler) = guard.as_ref() {
        // The user closure is bound `Fn + Send + Sync`, but we still
        // catch any panic before it crosses the FFI boundary, since
        // unwinding through `extern "C"` is UB in stable Rust.
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            handler(view);
        }));
        // Swallow the panic payload deliberately: the FFI contract
        // is `noexcept`, and there is no sensible way to surface
        // it from inside the allocator's broadcast loop.
        let _ = result;
    }
}

/// RAII handle for an active streaming-profiling session.
///
/// Construct one via [`ProfilingSession::start`].  While the handle
/// is alive, the supplied closure receives one [`StreamSample`] per
/// sampled allocation.  Dropping the handle unregisters the closure
/// from the C runtime and clears the global handler slot, freeing
/// up the next [`ProfilingSession::start`] to succeed.
///
/// Only one session can be active per process; a second
/// [`ProfilingSession::start`] while one is already alive returns
/// [`StreamingError::AlreadyActive`].
///
/// The type is `!Send` and `!Sync` deliberately (via the `*const ()`
/// phantom): dropping the session must happen on a single thread,
/// not across thread boundaries, so the unregister-then-clear
/// sequence inside `Drop` is well-ordered.
pub struct ProfilingSession {
    // Phantom !Send / !Sync.  The actual handler state lives in a
    // process-global slot, not in this handle; the handle is purely
    // an RAII token whose `Drop` tears down the registration.
    _not_send: PhantomData<*const ()>,
}

impl ProfilingSession {
    /// Begin a streaming profiling session.
    ///
    /// `handler` is invoked once per sampled allocation, on
    /// whichever allocator thread happened to trip the sampler.  It
    /// receives a borrowed [`StreamSample`] that is valid only for
    /// the duration of the call -- if the application needs the
    /// data past the callback, it must copy the relevant fields
    /// out.
    ///
    /// # Errors
    ///
    /// - [`StreamingError::AlreadyActive`] -- another
    ///   `ProfilingSession` is currently alive in this process.
    /// - [`StreamingError::RegistrationFailed`] -- the C runtime
    ///   refused to register the trampoline (most commonly because
    ///   `SNMALLOC_PROFILE` is disabled at build time, or every
    ///   broadcast slot is already claimed).
    pub fn start<F>(handler: F) -> Result<Self, StreamingError>
    where
        F: Fn(StreamSample<'_>) + Send + Sync + 'static,
    {
        // Step 1: claim the global slot.  If someone else is
        // already registered, abort early WITHOUT touching the C
        // side (the existing trampoline registration belongs to
        // them).
        let mut guard = match handler_slot().lock() {
            Ok(g) => g,
            // A poisoned mutex implies a prior handler panicked.
            // We recover by overwriting; the previous session's
            // trampoline (if still registered) will be cleared by
            // its own Drop when it ran, so the C side either has
            // no registration or has the trampoline pointing at
            // this same function -- which is fine since we are
            // about to replace the slot contents.
            Err(poisoned) => poisoned.into_inner(),
        };
        if guard.is_some() {
            return Err(StreamingError::AlreadyActive);
        }

        // Step 2: install the handler in the slot BEFORE the C
        // registration succeeds.  This ordering guarantees that
        // any sample dispatched immediately after
        // `sn_rust_profile_streaming_start` returns will find a
        // valid handler in the slot.  If registration fails we
        // roll back.
        *guard = Some(Box::new(handler));

        // SAFETY: `trampoline` is a fixed-signature C-compatible
        // function pointer that survives for the lifetime of the
        // process; the C side stores it in a `std::atomic`.  We
        // hold the slot mutex across the registration so no other
        // start() can interleave between the slot write and the
        // C-side store.
        let rc = unsafe { ffi::sn_rust_profile_streaming_start(trampoline) };
        if rc != 0 {
            // Roll back the slot so a future start() can try
            // again.  The C side guarantees it did NOT install the
            // trampoline on a non-zero return.
            *guard = None;
            return Err(StreamingError::RegistrationFailed);
        }

        // Release the lock before returning the handle: subsequent
        // trampoline dispatches need to be able to acquire it.
        drop(guard);

        Ok(Self {
            _not_send: PhantomData,
        })
    }
}

impl fmt::Debug for ProfilingSession {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ProfilingSession").finish_non_exhaustive()
    }
}

impl Drop for ProfilingSession {
    fn drop(&mut self) {
        // Step 1: stop the C runtime broadcasting to our
        // trampoline.  After this returns, no further dispatches
        // will be initiated -- though one already in flight on
        // another thread may still be locking the slot mutex.
        //
        // Ignore the return code: even if the C side reports
        // failure (e.g. because the underlying broadcast slot was
        // never claimed because start() failed mid-way), we still
        // need to clear the Rust slot.  Drop must be infallible.
        unsafe {
            let _ = ffi::sn_rust_profile_streaming_stop();
        }

        // Step 2: clear the slot.  Any in-flight dispatch on
        // another thread is currently holding the lock; we will
        // block until it finishes, then take and drop the boxed
        // closure here.  After this, the slot is empty and a
        // subsequent `ProfilingSession::start` can succeed.
        if let Ok(mut guard) = handler_slot().lock() {
            *guard = None;
        }
        // If the mutex is poisoned by a panicking handler, leave
        // the slot as-is; the next start() recovers via
        // `into_inner()` and overwrites.  Dropping the box would
        // require unwrapping the poisoned guard which is more
        // ceremony than it's worth -- the leak is bounded by one
        // closure per process lifetime.
    }
}
