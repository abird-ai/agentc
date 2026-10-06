//! rust_echo — example agentc extension written with the `agentc-ext` crate.
//!
//! Exports the static-linking entry point `agentc_ext_rust_echo_init` (the name
//! tools/gen-exts.py derives for `{"name":"rust_echo"}`). Each Rust extension
//! exports only its per-ident symbol: exporting the canonical
//! `agentc_ext_init` too would collide when several staticlibs link one binary.
#![no_std]

use core::ffi::{c_int, c_void, CStr};
use core::mem::size_of;

use agentc_ext::{
    schema, AgcExt, AgcExtHost, AgcExtTool, AgcExtToolCall, Host, Tool, AGENTC_EXT_ABI,
};

/// Staticlib crates need a panic handler. Extensions never unwind
/// (panic=abort) and should avoid panicking paths entirely; this is the
/// last-resort sink.
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}

static SCHEMA: &CStr = schema!(
    "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"Text to echo back\"}},\"required\":[\"text\"]}"
);

/// Synchronous tool run: append `rust_echo: <text>` to the opaque output sink.
unsafe extern "C" fn echo_run(
    host: *const AgcExtHost,
    _self: *const AgcExtTool,
    call: *const AgcExtToolCall,
    out: *mut c_void,
    is_error: *mut bool,
) -> c_int {
    let h = Host::new(host);
    let text = if call.is_null() {
        core::ptr::null()
    } else {
        h.json_get_str((*call).args_json, c"text", core::ptr::null())
    };
    if text.is_null() {
        if !is_error.is_null() {
            *is_error = true;
        }
        h.out_write(out, b"error: rust_echo: missing required field: text");
        return 0;
    }
    h.out_write(out, b"rust_echo: ");
    // `text` is NUL-terminated. Measure it with volatile loads: a plain byte
    // loop gets folded into a call to libc strlen, which a freestanding
    // agentc binary does not provide.
    let mut n = 0usize;
    loop {
        let b = core::ptr::read_volatile(text.add(n) as *const u8);
        if b == 0 {
            break;
        }
        n += 1;
    }
    h.out_write(out, core::slice::from_raw_parts(text as *const u8, n));
    0
}

static TOOLS: [Tool; 1] = [Tool::new(
    c"rust_echo",
    c"Echo the `text` argument back (Rust example extension).",
    SCHEMA,
    echo_run,
)
.label(c"Rust echo")
.readonly()
.snippet(c"rust_echo(text) - echo text back")];

unsafe extern "C" fn echo_init(host: *const AgcExtHost) -> c_int {
    let h = Host::new(host);
    h.log(1, c"rust_echo: initialized");
    let raw = TOOLS[0].to_raw();
    h.add_tool(&raw);
    0
}

#[no_mangle]
pub extern "C" fn agentc_ext_rust_echo_init(
    host: *const AgcExtHost,
    out: *mut AgcExt,
) -> c_int {
    if host.is_null() || out.is_null() {
        return 1;
    }
    unsafe {
        out.write(AgcExt {
            abi_version: AGENTC_EXT_ABI,
            struct_size: size_of::<AgcExt>() as u32,
            name: c"rust_echo".as_ptr(),
            version: c"0.1.0".as_ptr(),
            order: 0,
            init: Some(echo_init),
            shutdown: None,
            required_host_size: 0,
        });
    }
    0
}
