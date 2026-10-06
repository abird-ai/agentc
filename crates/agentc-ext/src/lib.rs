//! agentc-ext — a `no_std`-friendly wrapper around `include/agentc_ext.h`.
//!
//! The crate mirrors the C ABI structs exactly (field order included) and adds
//! a thin [`Host`] vtable wrapper plus a small [`Tool`] builder:
//!
//! ```ignore
//! use agentc_ext::{schema, AgcExt, AgcExtHost, AgcExtToolCall, Host, Tool, AGENTC_EXT_ABI};
//!
//! static SCHEMA: &core::ffi::CStr = schema!("{\"type\":\"object\"}");
//!
//! unsafe extern "C" fn run(_h: *const AgcExtHost, _self: *const agentc_ext::AgcExtTool,
//!                          _call: *const AgcExtToolCall, out: *mut core::ffi::c_void,
//!                          _err: *mut bool) -> core::ffi::c_int {
//!     let h = Host::new(_h);
//!     h.out_write(out, b"done");
//!     0
//! }
//!
//! const TOOLS: [Tool; 1] =
//!     [Tool::new(c"demo", c"demo tool", SCHEMA, run).readonly()];
//!
//! unsafe extern "C" fn init(host: *const AgcExtHost) -> core::ffi::c_int {
//!     let h = Host::new(host);
//!     let raw = TOOLS[0].to_raw();
//!     h.add_tool(&raw);
//!     0
//! }
//!
//! #[no_mangle]
//! pub extern "C" fn agentc_ext_demo_init(host: *const AgcExtHost, out: *mut AgcExt) -> core::ffi::c_int {
//!     if host.is_null() || out.is_null() { return 1; }
//!     unsafe {
//!         out.write(AgcExt {
//!             abi_version: AGENTC_EXT_ABI,
//!             struct_size: core::mem::size_of::<AgcExt>() as u32,
//!             name: c"demo".as_ptr(),
//!             version: c"0.1.0".as_ptr(),
//!             order: 0,
//!             init: Some(init),
//!             shutdown: None,
//!             required_host_size: 0,
//!         });
//!     }
//!     0
//! }
//! ```
//!
//! Each extension is compiled with a unique exported symbol
//! (`agentc_ext_<name>_init`, see `tools/gen-exts.py`) to avoid colliding on the
//! canonical `agentc_ext_init` when several extensions share a binary.
#![no_std]

use core::ffi::{c_char, c_int, c_void, CStr};
use core::mem::size_of;

/// Matches `AGENTC_EXT_ABI` in include/agentc_ext.h.
pub const AGENTC_EXT_ABI: u32 = 1;

/* Tool flags (values match AGENTC_TOOL_* in include/agentc_ext.h). */
pub const AGENTC_TOOL_READONLY: u32 = 0x1;
pub const AGENTC_TOOL_DESTRUCTIVE: u32 = 0x2;
pub const AGENTC_TOOL_SEQUENTIAL: u32 = 0x4;
pub const AGENTC_TOOL_HIDDEN: u32 = 0x8;
pub const AGENTC_TOOL_TIMEOUT_DEF_MS: c_int = 120000;

/* Async tool stop reasons and the async timeout clamp. */
pub const AGENTC_EXT_TOOL_FINISHED: c_int = 0;
pub const AGENTC_EXT_TOOL_ERROR: c_int = 1;
pub const AGENTC_EXT_TOOL_TIMEOUT: c_int = 2;
pub const AGENTC_EXT_TOOL_CANCELLED: c_int = 3;
pub const AGENTC_EXT_TOOL_UNLOAD: c_int = 4;
pub const AGENTC_EXT_TOOL_TIMEOUT_MAX_MS: c_int = 1800000;

/* Hook capabilities. Required capability is fixed per hook point. */
pub const AGENTC_HOOK_OBSERVE: u32 = 0x1;
pub const AGENTC_HOOK_OVERRIDE: u32 = 0x2;

/* Status segment slot/style bits. */
pub const AGENTC_PSEG_SLOT_LEFT: u32 = 0;
pub const AGENTC_PSEG_SLOT_RIGHT: u32 = 1;

pub const AGENTC_PSEG_STYLE_DIM: u32 = 0x0001;
pub const AGENTC_PSEG_STYLE_BOLD: u32 = 0x0002;
pub const AGENTC_PSEG_STYLE_ACCENT: u32 = 0x0004;
pub const AGENTC_PSEG_STYLE_WARN: u32 = 0x0008;
pub const AGENTC_PSEG_STYLE_ERROR: u32 = 0x0010;
pub const AGENTC_PSEG_STYLE_OK: u32 = 0x0020;
pub const AGENTC_PSEG_STYLE_ALL: u32 = 0x003F;

/* Custom-provider contribution kinds and view values. */
pub const AGENTC_EXT_ROLE_SYSTEM: u32 = 0;
pub const AGENTC_EXT_ROLE_USER: u32 = 1;
pub const AGENTC_EXT_ROLE_ASSISTANT: u32 = 2;
pub const AGENTC_EXT_ROLE_TOOL: u32 = 3;

pub const AGENTC_EXT_BLK_TEXT: u32 = 0;
pub const AGENTC_EXT_BLK_THINK: u32 = 1;
pub const AGENTC_EXT_BLK_TOOLCALL: u32 = 2;

pub const AGENTC_EXT_STOP_PENDING: c_int = 0;
pub const AGENTC_EXT_STOP_STOP: c_int = 1;
pub const AGENTC_EXT_STOP_LENGTH: c_int = 2;
pub const AGENTC_EXT_STOP_TOOLUSE: c_int = 3;
pub const AGENTC_EXT_STOP_ERROR: c_int = 4;
pub const AGENTC_EXT_STOP_ABORTED: c_int = 5;

pub const AGENTC_EXT_DISCOVER_DEFAULT: c_int = 0;
pub const AGENTC_EXT_DISCOVER_ANTHROPIC: c_int = 1;
pub const AGENTC_EXT_DISCOVER_OLLAMA: c_int = 2;
pub const AGENTC_EXT_DISCOVER_NONE: c_int = 3;

pub const AGENTC_EXT_AUTH_NONE: u32 = 0;
pub const AGENTC_EXT_AUTH_BEARER: u32 = 1;
pub const AGENTC_EXT_AUTH_HEADER: u32 = 2;
pub const AGENTC_EXT_AUTH_QUERY: u32 = 3;

pub const AGENTC_EXT_USAGE_INPUT: u32 = 0x1;
pub const AGENTC_EXT_USAGE_OUTPUT: u32 = 0x2;
pub const AGENTC_EXT_USAGE_CACHE_READ: u32 = 0x4;
pub const AGENTC_EXT_USAGE_CACHE_WRITE: u32 = 0x8;
pub const AGENTC_EXT_USAGE_REASONING: u32 = 0x10;

/// Synchronous tool entry point. Append result text with `host->out_write`
/// (the `out` sink is opaque) and return 0 | -errno. `is_error` is set when the
/// text describes a failure.
pub type ToolRun = unsafe extern "C" fn(
    host: *const AgcExtHost,
    self_: *const AgcExtTool,
    call: *const AgcExtToolCall,
    out: *mut c_void,
    is_error: *mut bool,
) -> c_int;

/// Asynchronous tool start: 0 running, 1 complete, <0 fatal (a fatal
/// start is never followed by a stop). `state` is extension-owned; allocate it
/// with `host->alloc` and release it in the stop callback.
pub type ToolStart = unsafe extern "C" fn(
    host: *const AgcExtHost,
    self_: *const AgcExtTool,
    call: *const AgcExtToolCall,
    out: *mut c_void,
    is_error: *mut bool,
    state: *mut *mut c_void,
) -> c_int;

/// Asynchronous tool step: 0 running, 1 complete, <0 fatal.
pub type ToolStep = unsafe extern "C" fn(
    host: *const AgcExtHost,
    self_: *const AgcExtTool,
    call: *const AgcExtToolCall,
    state: *mut c_void,
    out: *mut c_void,
    is_error: *mut bool,
) -> c_int;

/// Asynchronous tool stop: called exactly once per successful start
/// with one of the `AGENTC_EXT_TOOL_*` reasons.
pub type ToolStop = unsafe extern "C" fn(
    host: *const AgcExtHost,
    self_: *const AgcExtTool,
    state: *mut c_void,
    reason: c_int,
);

pub type CommandRun = unsafe extern "C" fn(
    host: *const AgcExtHost,
    ud: *mut c_void,
    args_json: *const c_char,
    out: *mut c_void,
);

pub type SectionRender =
    unsafe extern "C" fn(host: *const AgcExtHost, ud: *mut c_void, out: *mut c_void) -> c_int;

pub type StatusProvider = unsafe extern "C" fn(
    ud: *mut c_void,
    out: *mut AgcExtStatusSegment,
    max: usize,
    arena: *mut c_char,
    arena_cap: usize,
) -> usize;

/// Hook handler. Return 0 = continue, 1 = handled/stop, <0 = error. Only an
/// `AGENTC_HOOK_OVERRIDE` handler may set `*result_json` (host-allocated).
pub type AgcExtHookFn = unsafe extern "C" fn(
    ud: *mut c_void,
    point: *const c_char,
    payload_json: *const c_char,
    result_json: *mut *mut c_char,
) -> c_int;

pub type ExtInit = unsafe extern "C" fn(host: *const AgcExtHost) -> c_int;
pub type ExtShutdown = unsafe extern "C" fn();

pub type HttpCallback = unsafe extern "C" fn(
    ud: *mut c_void,
    status: c_int,
    headers_json: *const c_char,
    body: *const c_char,
    body_len: u64,
);

/* ------------------------------------------------ custom provider types */

#[repr(C)]
pub struct AgcExtBlockView {
    pub struct_size: u32,
    pub r#type: u32,
    pub text: *const c_char,
    pub text_len: usize,
    pub tool_id: *const c_char,
    pub tool_name: *const c_char,
    pub tool_args: *const c_char,
}

#[repr(C)]
pub struct AgcExtMessageView {
    pub struct_size: u32,
    pub role: u32,
    pub stop_reason: i32,
    pub blocks: *const AgcExtBlockView,
    pub nblocks: usize,
}

#[repr(C)]
pub struct AgcExtToolView {
    pub struct_size: u32,
    pub name: *const c_char,
    pub description: *const c_char,
    pub parameters_json: *const c_char,
    pub flags: u32,
}

#[repr(C)]
pub struct AgcExtRequestView {
    pub struct_size: u32,
    pub provider: *const c_char,
    pub model: *const c_char,
    pub system: *const c_char,
    pub thinking_level: i32,
    pub max_tokens: i64,
    pub messages: *const AgcExtMessageView,
    pub nmessages: usize,
    pub tools: *const AgcExtToolView,
    pub ntools: usize,
}

#[repr(C)]
pub struct AgcExtProviderAuth {
    pub struct_size: u32,
    pub kind: u32,
    pub header: *const c_char,
    pub prefix: *const c_char,
}

#[repr(C)]
pub struct AgcExtProviderModel {
    pub struct_size: u32,
    pub id: *const c_char,
    pub name: *const c_char,
    pub ctx_window: u32,
    pub max_tokens: u32,
    pub reasoning: c_int,
    pub image: c_int,
}

#[repr(C)]
pub struct AgcExtStream {
    pub struct_size: u32,
    pub ud: *mut c_void,
}

#[repr(C)]
pub struct AgcExtUsage {
    pub struct_size: u32,
    pub fields: u32,
    pub input: u32,
    pub output: u32,
    pub cache_read: u32,
    pub cache_write: u32,
    pub reasoning: u32,
}

#[repr(C)]
pub struct AgcExtWireEvent {
    pub struct_size: u32,
    pub event: *const c_char,
    pub data: *const c_char,
    pub data_len: usize,
}

pub type SinkText = unsafe extern "C" fn(st: *mut AgcExtStream, p: *const c_char, n: usize);
pub type SinkThinking =
    unsafe extern "C" fn(st: *mut AgcExtStream, p: *const c_char, n: usize);
pub type SinkToolStart = unsafe extern "C" fn(
    st: *mut AgcExtStream,
    id: *const c_char,
    name: *const c_char,
);
pub type SinkToolArgs = unsafe extern "C" fn(st: *mut AgcExtStream, p: *const c_char, n: usize);
pub type SinkResponseId =
    unsafe extern "C" fn(st: *mut AgcExtStream, id: *const c_char);
pub type SinkUsage = unsafe extern "C" fn(st: *mut AgcExtStream, u: *const AgcExtUsage);
pub type SinkStop = unsafe extern "C" fn(st: *mut AgcExtStream, reason: c_int);
pub type SinkError =
    unsafe extern "C" fn(st: *mut AgcExtStream, message: *const c_char);

#[repr(C)]
pub struct AgcExtStreamSink {
    pub struct_size: u32,
    pub text: Option<SinkText>,
    pub thinking: Option<SinkThinking>,
    pub tool_start: Option<SinkToolStart>,
    pub tool_args: Option<SinkToolArgs>,
    pub response_id: Option<SinkResponseId>,
    pub usage: Option<SinkUsage>,
    pub stop: Option<SinkStop>,
    pub error: Option<SinkError>,
}

pub type ProviderBuildRequest = unsafe extern "C" fn(
    host: *const AgcExtHost,
    self_: *const AgcExtProvider,
    req: *const AgcExtRequestView,
    head_out: *mut c_void,
    body_out: *mut c_void,
) -> c_int;

pub type ProviderStreamOpen = unsafe extern "C" fn(
    host: *const AgcExtHost,
    self_: *const AgcExtProvider,
    st: *mut AgcExtStream,
) -> c_int;

pub type ProviderStreamEvent = unsafe extern "C" fn(
    host: *const AgcExtHost,
    self_: *const AgcExtProvider,
    st: *mut AgcExtStream,
    ev: *const AgcExtWireEvent,
    sink: *const AgcExtStreamSink,
) -> c_int;

pub type ProviderStreamFinish = unsafe extern "C" fn(
    host: *const AgcExtHost,
    self_: *const AgcExtProvider,
    st: *mut AgcExtStream,
    sink: *const AgcExtStreamSink,
) -> c_int;

pub type ProviderStreamClose = unsafe extern "C" fn(
    host: *const AgcExtHost,
    self_: *const AgcExtProvider,
    st: *mut AgcExtStream,
);

#[repr(C)]
pub struct AgcExtProvider {
    pub struct_size: u32,
    pub name: *const c_char,
    pub label: *const c_char,
    pub default_base_url: *const c_char,
    pub path: *const c_char,
    pub env_keys: [*const c_char; 3],
    pub needs_key: c_int,
    pub discover_style: c_int,
    pub auth: *const AgcExtProviderAuth,
    pub models: *const AgcExtProviderModel,
    pub nmodels: usize,
    pub ud: *mut c_void,
    pub build_request: Option<ProviderBuildRequest>,
    pub stream_open: Option<ProviderStreamOpen>,
    pub stream_event: Option<ProviderStreamEvent>,
    pub stream_finish: Option<ProviderStreamFinish>,
    pub stream_close: Option<ProviderStreamClose>,
}

/* ---------------------------------------------------------------- tools */

#[repr(C)]
pub struct AgcExtToolCall {
    pub struct_size: u32,
    pub call_id: *const c_char,
    pub name: *const c_char,
    pub args_json: *const c_char,
    pub signal_token: *const c_char,
}

#[repr(C)]
pub struct AgcExtTool {
    pub struct_size: u32,
    pub flags: u32,
    pub name: *const c_char,
    pub label: *const c_char,
    pub description: *const c_char,
    pub parameters_json: *const c_char,
    pub prompt_snippet: *const c_char,
    pub prompt_guidelines: *const c_char,
    pub ud: *mut c_void,
    pub timeout_ms: c_int,
    pub run: Option<ToolRun>,
    pub start: Option<ToolStart>,
    pub step: Option<ToolStep>,
    pub stop: Option<ToolStop>,
}

#[repr(C)]
pub struct AgcExtCommand {
    pub struct_size: u32,
    pub name: *const c_char,
    pub description: *const c_char,
    pub ud: *mut c_void,
    pub run: Option<CommandRun>,
}

#[repr(C)]
pub struct AgcExtSection {
    pub struct_size: u32,
    pub key: *const c_char,
    pub priority: i32,
    pub ud: *mut c_void,
    pub render: Option<SectionRender>,
}

/* ------------------------------------------------------- status segments */

#[repr(C)]
pub struct AgcExtStatusSegment {
    pub struct_size: u32,
    pub slot: u32,
    pub priority: i32,
    pub style: u32,
    pub text: *const c_char,
}

/* ----------------------------------------------------------------- hooks */

#[repr(C)]
pub struct AgcExtResult {
    pub struct_size: u32,
    pub handled: c_int,
    pub blocked: c_int,
    pub result_json: *mut c_char,
}

/* ------------------------------------------------------------------ host */

#[repr(C)]
pub struct AgcExtHost {
    pub abi_version: u32,
    pub struct_size: u32,
    pub alloc: unsafe extern "C" fn(n: usize) -> *mut c_void,
    pub free: unsafe extern "C" fn(p: *mut c_void),
    pub log: unsafe extern "C" fn(level: c_int, msg: *const c_char),
    pub out_write: unsafe extern "C" fn(out: *mut c_void, bytes: *const c_char, n: usize),
    pub add_tool: unsafe extern "C" fn(tool: *const AgcExtTool),
    pub add_command: unsafe extern "C" fn(cmd: *const AgcExtCommand),
    pub add_section: unsafe extern "C" fn(section: *const AgcExtSection),
    pub add_status: unsafe extern "C" fn(provider: StatusProvider, ud: *mut c_void),
    pub on: unsafe extern "C" fn(
        point: *const c_char,
        caps: u32,
        priority: c_int,
        f: AgcExtHookFn,
        ud: *mut c_void,
    ) -> u64,
    pub off: unsafe extern "C" fn(handle: u64),
    pub emit: unsafe extern "C" fn(
        host: *const AgcExtHost,
        point: *const c_char,
        payload_json: *const c_char,
        out: *mut AgcExtResult,
    ),
    pub defer: unsafe extern "C" fn(
        host: *const AgcExtHost,
        f: unsafe extern "C" fn(ud: *mut c_void),
        ud: *mut c_void,
    ),
    pub is_cancelled:
        unsafe extern "C" fn(host: *const AgcExtHost, signal_token: *const c_char) -> c_int,
    pub cwd: unsafe extern "C" fn(host: *const AgcExtHost) -> *const c_char,
    pub session_id: unsafe extern "C" fn(host: *const AgcExtHost) -> *const c_char,
    pub session_file: unsafe extern "C" fn(host: *const AgcExtHost) -> *const c_char,
    pub system_prompt: unsafe extern "C" fn(host: *const AgcExtHost) -> *const c_char,
    pub append_entry:
        unsafe extern "C" fn(custom_type: *const c_char, data_json: *const c_char),
    pub http_request: unsafe extern "C" fn(
        host: *const AgcExtHost,
        method: *const c_char,
        url: *const c_char,
        headers_json: *const c_char,
        body: *const c_char,
        body_len: u64,
        cb: HttpCallback,
        ud: *mut c_void,
    ) -> u64,
    pub http_cancel: unsafe extern "C" fn(host: *const AgcExtHost, request: u64),
    pub notify: unsafe extern "C" fn(message: *const c_char, level: c_int),
    pub set_status: unsafe extern "C" fn(key: *const c_char, text: *const c_char),
    pub set_model: unsafe extern "C" fn(provider: *const c_char, model: *const c_char) -> c_int,
    pub set_thinking: unsafe extern "C" fn(level: *const c_char),
    pub request_recompose: unsafe extern "C" fn(host: *const AgcExtHost),
    pub strdup_: unsafe extern "C" fn(s: *const c_char) -> *mut c_char,
    pub json_get_str: unsafe extern "C" fn(
        json: *const c_char,
        path: *const c_char,
        dflt: *const c_char,
    ) -> *const c_char,
    pub json_get_int:
        unsafe extern "C" fn(json: *const c_char, path: *const c_char, dflt: i64) -> i64,
    pub json_get_bool:
        unsafe extern "C" fn(json: *const c_char, path: *const c_char, dflt: c_int) -> c_int,
    pub json_escape: unsafe extern "C" fn(s: *const c_char) -> *mut c_char,
    pub set_title: unsafe extern "C" fn(title: *const c_char),
    /// Register a custom provider (validated and copied by the
    /// host). Guard with `AGENTC_EXT_HOST_HAS`/`struct_size` on older hosts.
    pub add_provider: unsafe extern "C" fn(provider: *const AgcExtProvider),
}

/* ------------------------------------------------------------- extension */

#[repr(C)]
pub struct AgcExt {
    pub abi_version: u32,
    pub struct_size: u32,
    pub name: *const c_char,
    pub version: *const c_char,
    pub order: i32,
    pub init: Option<ExtInit>,
    pub shutdown: Option<ExtShutdown>,
    /// Appended after the ABI-1 baseline: minimum `AgcExtHost.struct_size` this
    /// extension needs. 0 means no requirement; a value above the host's size
    /// is refused at registration. Mirrors `required_host_size` in the C header.
    pub required_host_size: u32,
}

/// Borrowed, zero-cost wrapper around the host vtable.
#[derive(Clone, Copy)]
pub struct Host {
    raw: *const AgcExtHost,
}

impl Host {
    pub const fn new(raw: *const AgcExtHost) -> Self {
        Self { raw }
    }

    pub const fn raw(&self) -> *const AgcExtHost {
        self.raw
    }

    /// Allocates zeroed host memory (freed with [`Host::free`]).
    pub unsafe fn alloc(&self, n: usize) -> *mut c_void {
        ((*self.raw).alloc)(n)
    }

    pub unsafe fn free(&self, p: *mut c_void) {
        ((*self.raw).free)(p)
    }

    /// level: 0=debug, 1=info, 2=warn, 3=error.
    pub unsafe fn log(&self, level: c_int, msg: &CStr) {
        ((*self.raw).log)(level, msg.as_ptr())
    }

    /// Appends plain result text to the opaque output sink.
    pub unsafe fn out_write(&self, out: *mut c_void, bytes: &[u8]) {
        ((*self.raw).out_write)(out, bytes.as_ptr() as *const c_char, bytes.len())
    }

    pub unsafe fn add_tool(&self, tool: &AgcExtTool) {
        ((*self.raw).add_tool)(tool)
    }

    pub unsafe fn add_command(&self, cmd: &AgcExtCommand) {
        ((*self.raw).add_command)(cmd)
    }

    pub unsafe fn add_section(&self, section: &AgcExtSection) {
        ((*self.raw).add_section)(section)
    }

    pub unsafe fn add_status(&self, provider: StatusProvider, ud: *mut c_void) {
        ((*self.raw).add_status)(provider, ud)
    }

    pub unsafe fn on(
        &self,
        point: &CStr,
        caps: u32,
        priority: c_int,
        f: AgcExtHookFn,
        ud: *mut c_void,
    ) -> u64 {
        ((*self.raw).on)(point.as_ptr(), caps, priority, f, ud)
    }

    pub unsafe fn off(&self, handle: u64) {
        ((*self.raw).off)(handle)
    }

    pub unsafe fn emit(
        &self,
        point: &CStr,
        payload_json: *const c_char,
        out: *mut AgcExtResult,
    ) {
        ((*self.raw).emit)(self.raw, point.as_ptr(), payload_json, out)
    }

    pub unsafe fn defer(&self, f: unsafe extern "C" fn(ud: *mut c_void), ud: *mut c_void) {
        ((*self.raw).defer)(self.raw, f, ud)
    }

    pub unsafe fn is_cancelled(&self, signal_token: *const c_char) -> bool {
        ((*self.raw).is_cancelled)(self.raw, signal_token) != 0
    }

    pub unsafe fn cwd(&self) -> *const c_char {
        ((*self.raw).cwd)(self.raw)
    }

    pub unsafe fn session_id(&self) -> *const c_char {
        ((*self.raw).session_id)(self.raw)
    }

    pub unsafe fn session_file(&self) -> *const c_char {
        ((*self.raw).session_file)(self.raw)
    }

    pub unsafe fn system_prompt(&self) -> *const c_char {
        ((*self.raw).system_prompt)(self.raw)
    }

    pub unsafe fn append_entry(&self, custom_type: &CStr, data_json: *const c_char) {
        ((*self.raw).append_entry)(custom_type.as_ptr(), data_json)
    }

    pub unsafe fn http_request(
        &self,
        method: &CStr,
        url: &CStr,
        headers_json: *const c_char,
        body: *const c_char,
        body_len: u64,
        cb: HttpCallback,
        ud: *mut c_void,
    ) -> u64 {
        ((*self.raw).http_request)(
            self.raw,
            method.as_ptr(),
            url.as_ptr(),
            headers_json,
            body,
            body_len,
            cb,
            ud,
        )
    }

    pub unsafe fn http_cancel(&self, request: u64) {
        ((*self.raw).http_cancel)(self.raw, request)
    }

    pub unsafe fn notify(&self, message: &CStr, level: c_int) {
        ((*self.raw).notify)(message.as_ptr(), level)
    }

    pub unsafe fn set_status(&self, key: &CStr, text: &CStr) {
        ((*self.raw).set_status)(key.as_ptr(), text.as_ptr())
    }

    pub unsafe fn set_model(&self, provider: &CStr, model: &CStr) -> c_int {
        ((*self.raw).set_model)(provider.as_ptr(), model.as_ptr())
    }

    pub unsafe fn set_thinking(&self, level: &CStr) {
        ((*self.raw).set_thinking)(level.as_ptr())
    }

    pub unsafe fn request_recompose(&self) {
        ((*self.raw).request_recompose)(self.raw)
    }

    pub unsafe fn strdup_(&self, s: *const c_char) -> *mut c_char {
        ((*self.raw).strdup_)(s)
    }

    /// Returns a host-owned string valid until the next pump, or `dflt` when
    /// missing/wrong type.
    pub unsafe fn json_get_str(
        &self,
        json: *const c_char,
        path: &CStr,
        dflt: *const c_char,
    ) -> *const c_char {
        ((*self.raw).json_get_str)(json, path.as_ptr(), dflt)
    }

    pub unsafe fn json_get_int(&self, json: *const c_char, path: &CStr, dflt: i64) -> i64 {
        ((*self.raw).json_get_int)(json, path.as_ptr(), dflt)
    }

    pub unsafe fn json_get_bool(&self, json: *const c_char, path: &CStr, dflt: bool) -> bool {
        ((*self.raw).json_get_bool)(json, path.as_ptr(), dflt as c_int) != 0
    }

    /// Returns a new JSON-escaped string owned by the caller (free with
    /// [`Host::free`]).
    pub unsafe fn json_escape(&self, s: *const c_char) -> *mut c_char {
        ((*self.raw).json_escape)(s)
    }

    pub unsafe fn set_title(&self, title: &CStr) {
        ((*self.raw).set_title)(title.as_ptr())
    }

    pub unsafe fn add_provider(&self, provider: &AgcExtProvider) {
        ((*self.raw).add_provider)(provider)
    }
}

/// Compile-time tool description. Strings must be `'static` C strings.
#[derive(Clone, Copy)]
pub struct Tool {
    pub flags: u32,
    pub name: &'static CStr,
    pub label: &'static CStr,
    pub description: &'static CStr,
    pub schema: &'static CStr,
    pub run: ToolRun,
    pub snippet: Option<&'static CStr>,
    pub guidelines: Option<&'static CStr>,
    pub timeout_ms: c_int,
}

impl Tool {
    pub const fn new(
        name: &'static CStr,
        description: &'static CStr,
        schema: &'static CStr,
        run: ToolRun,
    ) -> Self {
        Self {
            flags: 0,
            name,
            label: name,
            description,
            schema,
            run,
            snippet: None,
            guidelines: None,
            timeout_ms: 0,
        }
    }

    pub const fn label(mut self, label: &'static CStr) -> Self {
        self.label = label;
        self
    }

    pub const fn flags(mut self, flags: u32) -> Self {
        self.flags |= flags;
        self
    }

    pub const fn readonly(mut self) -> Self {
        self.flags |= AGENTC_TOOL_READONLY;
        self
    }

    pub const fn destructive(mut self) -> Self {
        self.flags |= AGENTC_TOOL_DESTRUCTIVE;
        self
    }

    pub const fn sequential(mut self) -> Self {
        self.flags |= AGENTC_TOOL_SEQUENTIAL;
        self
    }

    pub const fn hidden(mut self) -> Self {
        self.flags |= AGENTC_TOOL_HIDDEN;
        self
    }

    pub const fn snippet(mut self, snippet: &'static CStr) -> Self {
        self.snippet = Some(snippet);
        self
    }

    pub const fn guidelines(mut self, guidelines: &'static CStr) -> Self {
        self.guidelines = Some(guidelines);
        self
    }

    pub const fn timeout_ms(mut self, ms: c_int) -> Self {
        self.timeout_ms = ms;
        self
    }

    /// Materializes the raw C struct; pass it to [`Host::add_tool`].
    pub fn to_raw(&self) -> AgcExtTool {
        AgcExtTool {
            struct_size: size_of::<AgcExtTool>() as u32,
            flags: self.flags,
            name: self.name.as_ptr(),
            label: self.label.as_ptr(),
            description: self.description.as_ptr(),
            parameters_json: self.schema.as_ptr(),
            prompt_snippet: self.snippet.map_or(core::ptr::null(), |s| s.as_ptr()),
            prompt_guidelines: self.guidelines.map_or(core::ptr::null(), |s| s.as_ptr()),
            ud: core::ptr::null_mut(),
            timeout_ms: self.timeout_ms,
            run: Some(self.run),
            start: None,
            step: None,
            stop: None,
        }
    }
}

/// Registers a static tool table during extension `init`.
///
/// # Safety
/// `host` must be the live host vtable passed to `agentc_ext_init`.
pub unsafe fn register_tools(host: *const AgcExtHost, tools: &[Tool]) {
    if host.is_null() {
        return;
    }
    for t in tools {
        let raw = t.to_raw();
        ((*host).add_tool)(&raw);
    }
}

#[doc(hidden)]
pub mod __private {
    use core::ffi::CStr;

    /// Appends nothing: the byte slice must already carry its trailing NUL.
    pub const fn cstr(bytes: &'static [u8]) -> &'static CStr {
        // SAFETY: callers (the schema! macro) append exactly one NUL and the
        // input literal contains no interior NUL.
        unsafe { CStr::from_bytes_with_nul_unchecked(bytes) }
    }
}

/// `schema!("{\"type\":\"object\"}")` -> `&'static CStr` without allocation.
#[macro_export]
macro_rules! schema {
    ($s:literal) => {
        $crate::__private::cstr(concat!($s, "\0").as_bytes())
    };
}
