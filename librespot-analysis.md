# librespot

Loader adapters found: 411

## Shared libraries (NEEDED)
- libgcc_s.so.1: NOT PROVIDED: needs a virtual library
- libm.so.6: NOT PROVIDED: needs a virtual library
- libc.so.6: provided
- ld-linux-x86-64.so.2: NOT PROVIDED: needs a virtual library

## Imports: 173 undefined symbols, 35 MISSING (+10 weak)
- MISSING `_Unwind_Backtrace` (FUNC)
- MISSING `_Unwind_GetDataRelBase` (FUNC)
- MISSING `_Unwind_GetIP` (FUNC)
- MISSING `_Unwind_GetIPInfo` (FUNC)
- MISSING `_Unwind_GetLanguageSpecificData` (FUNC)
- MISSING `_Unwind_GetRegionStart` (FUNC)
- MISSING `_Unwind_GetTextRelBase` (FUNC)
- MISSING `_Unwind_Resume` (FUNC)
- MISSING `_Unwind_SetGR` (FUNC)
- MISSING `_Unwind_SetIP` (FUNC)
- MISSING `__libc_start_main` (FUNC)  [also missing for PokeMMO: harmless unless called]
- MISSING `__res_init` (FUNC)
- MISSING `chroot` (FUNC)
- MISSING `environ` (OBJECT)  [also missing for PokeMMO: harmless unless called]
- MISSING `execvp` (FUNC)  [also missing for PokeMMO: harmless unless called]
- MISSING `fork` (FUNC)  [also missing for PokeMMO: harmless unless called]
- MISSING `fstatat64` (FUNC)
- MISSING `kill` (FUNC)  [also missing for PokeMMO: harmless unless called]
- MISSING `pause` (FUNC)
- MISSING `posix_spawn_file_actions_adddup2` (FUNC)
- MISSING `posix_spawn_file_actions_destroy` (FUNC)
- MISSING `posix_spawn_file_actions_init` (FUNC)
- MISSING `posix_spawnattr_destroy` (FUNC)
- MISSING `posix_spawnattr_init` (FUNC)
- MISSING `posix_spawnattr_setflags` (FUNC)
- MISSING `posix_spawnattr_setpgroup` (FUNC)
- MISSING `posix_spawnattr_setsigdefault` (FUNC)
- MISSING `posix_spawnp` (FUNC)
- MISSING `setgid` (FUNC)
- MISSING `setgroups` (FUNC)
- MISSING `setpgid` (FUNC)
- MISSING `setsid` (FUNC)
- MISSING `setuid` (FUNC)
- MISSING `waitid` (FUNC)  [also missing for PokeMMO: harmless unless called]
- MISSING `waitpid` (FUNC)  [also missing for PokeMMO: harmless unless called]
- weak, missing `_ITM_deregisterTMCloneTable`
- weak, missing `_ITM_registerTMCloneTable`
- weak, missing `__cxa_finalize`
- weak, missing `__gmon_start__`
- weak, missing `gettid`
- weak, missing `pidfd_getpid`
- weak, missing `pidfd_spawnp`
- weak, missing `posix_spawn_file_actions_addchdir`
- weak, missing `posix_spawn_file_actions_addchdir_np`
- weak, missing `statx`

## Relocations
- R_X86_64_GLOB_DAT: 169
- R_X86_64_JUMP_SLOT: 5
- R_X86_64_RELATIVE: 26258

## Thread-local storage
- PT_TLS: present: TLS 0xaafc10 0x0000000000ab7c10 0x0000000000ab7c10 0x0000e8 0x000238 R 0x8

## Raw system calls and %fs
- raw `syscall` instructions: 0 in 0 functions
- %fs:0x28 (stack canary) reads: 50
- other %fs accesses (thread-local storage): 596 in 267 functions
  - `<librespot_core::dealer::protocol::MessageOrRequest as serde_core::de::Deserialize>::deserialize::<&mut serde_` x24
  - `<tokio::runtime::scheduler::current_thread::CurrentThread>::block_on::<librespot::main::{closure#0}>` x14
  - `<tokio::runtime::scheduler::current_thread::CurrentThread>::block_on::<librespot_playback::player::PlayerInter` x14
  - `tokio::runtime::context::runtime::enter_runtime::<<tokio::runtime::scheduler::multi_thread::MultiThread>::bloc` x12
  - `tokio::runtime::context::runtime::enter_runtime::<<tokio::runtime::scheduler::multi_thread::MultiThread>::bloc` x12
  - `<tokio::runtime::park::CachedParkThread>::block_on::<<tokio::sync::mpsc::unbounded::UnboundedReceiver<librespo` x11
  - `tokio::task::spawn::spawn::<core::pin::Pin<alloc::boxed::Box<<librespot_connect::spirc::SpircTask>::run::{clos` x9
  - `tokio::task::spawn::spawn::<librespot_core::session::DispatchTask<futures_util::stream::stream::split::SplitSt` x9

## Entry point
- dynamic symbols named main/_start: main

