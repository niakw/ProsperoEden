#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Apply Encore's audited backports to the exact Eden source snapshot.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
eden=${1:?usage: apply-eden-backports.sh <eden-source>}
expected=5f142c7926d0c7fcbbd0ce30794d72f638a43b2a
[[ -f $eden/CMakeLists.txt ]] || { echo "Eden source missing: $eden" >&2; exit 1; }
[[ -f $eden/GIT-COMMIT ]] || { echo "Eden pin receipt missing: $eden/GIT-COMMIT" >&2; exit 1; }
[[ $(tr -d '\r\n' < "$eden/GIT-COMMIT") == "$expected" ]] || {
    echo "Encore backports require Eden $expected" >&2; exit 1;
}

validate_gpu() {
python3 - "$eden" <<'PY'
from pathlib import Path
import sys
r=Path(sys.argv[1])
checks={
 'src/video_core/dma_pusher.cpp':['kepler_payload','macro_payload','dma_state.method_count'],
 'src/video_core/engines/kepler_compute.cpp':['upload_dirty = current_dirty','data.was_dirty || source_dirty'],
 'src/video_core/engines/kepler_compute.h':['bool upload_dirty{};','bool was_dirty;'],
 'src/video_core/buffer_cache/buffer_cache.h':['SynchronizeBufferWrites','gpu_modified_ranges.ForEachInRange','runtime.IsFree(buffer_tick)'],
 'src/video_core/buffer_cache/buffer_cache_base.h':['SynchronizeBufferWrites','needs_sync = false'],
 'src/video_core/fence_manager.h':['IsGPUFenceBehaviorAccurate()'],
 'src/video_core/renderer_vulkan/vk_buffer_cache.cpp':['return scheduler.CurrentTick();','return scheduler.IsFree(tick);'],
}
for rel, needles in checks.items():
    text=(r/rel).read_text()
    for n in needles:
        if n not in text: raise SystemExit(f'GPU backport missing from {rel}: {n}')
if 'enable_gpu_buffer_readback' in (r/'src/common/settings.h').read_text():
    raise SystemExit('obsolete GPU buffer readback setting survived')
print('Eden audited GPU backports #4473/#4477: PASS')
PY
}

validate_fw23() {
python3 - "$eden" <<'PY'
from pathlib import Path
import sys
r=Path(sys.argv[1])
checks={
 'src/core/hle/api_version.h':['HOS_VERSION_MAJOR = 23','DISPLAY_VERSION[0x18] = "23.0.0"'],
 'src/core/hle/service/bpc/bpc.cpp':['"bpc:ams"'],
 'src/core/hle/service/nfp/nfp.cpp':['StartDetectionWithFilter'],
 'src/core/hle/service/ns/application_manager_interface.cpp':['Unknown4105'],
 'src/core/hle/service/ns/read_only_application_control_data_interface.cpp':['{23, D<&IReadOnlyApplicationControlDataInterface::GetApplicationControlData3>'],
 'src/core/hle/service/olsc/transfer_task_list_controller.cpp':['GetTransferTaskProgress'],
 'src/core/hle/service/sm/sm.cpp':['max_sessions'],
}
for rel, needles in checks.items():
    text=(r/rel).read_text()
    for n in needles:
        if n not in text: raise SystemExit(f'FW23/service backport missing from {rel}: {n}')
print('Eden audited FW23/service/max_sessions backports: PASS')
PY
}

validate_spinlock_mutex() {
python3 - "$eden" <<'PY2'
from pathlib import Path
import sys
r=Path(sys.argv[1])
thread=(r/'src/core/hle/kernel/k_thread.h').read_text()
slab=(r/'src/core/hle/kernel/k_slab_heap.h').read_text()
cmake=(r/'src/common/CMakeLists.txt').read_text()
for rel,text,needle in [
    ('k_thread.h',thread,'std::mutex m_context_guard{};'),
    ('k_slab_heap.h',slab,'std::mutex m_lock;'),
]:
    if needle not in text: raise SystemExit(f'Eden #4436 missing from {rel}: {needle}')
if 'common/spin_lock.h' in thread or 'common/spin_lock.h' in slab:
    raise SystemExit('Eden #4436 incomplete: kernel still includes Common::SpinLock')
if 'spin_lock.h' in cmake:
    raise SystemExit('Eden #4436 incomplete: spin_lock.h still registered in common CMake')
print('Eden audited kernel mutex backport #4436: PASS')
PY2
}

validate_runtime_hid() {
python3 - "$eden" <<'PY'
from pathlib import Path
import sys
r=Path(sys.argv[1])
checks={
 'src/core/frontend/applets/controller.cpp':['keep_connected','max_supported_players'],
 'src/core/hle/kernel/k_scheduler.cpp':['m_context_guard.try_lock()'],
 'src/core/hle/service/am/frontend/applets.cpp':['std::erase(caller_applet->child_applets'],
 'src/core/hle/service/am/service/library_applet_accessor.cpp':['RequestFocusStateChangedNotification'],
 'src/hid_core/resources/npad/npad.cpp':['ReadCurrentEntry().state.sampling_number'],
 'src/video_core/control/channel_state_cache.h':['P* channel_state = nullptr;'],
}
for rel, needles in checks.items():
    text=(r/rel).read_text()
    for n in needles:
        if n not in text: raise SystemExit(f'Runtime/HID backport missing from {rel}: {n}')
print('Eden audited runtime/HID backports: PASS')
PY
}

apply_one() {
    local patch=$1 receipt=$2 validator=$3
    [[ -s $patch ]] || { echo "Backport patch missing: $patch" >&2; exit 1; }
    local hash recorded
    hash=$(sha256sum "$patch" | awk '{print $1}')
    if [[ -f $receipt ]]; then
        recorded=$(tr -d '\r\n' < "$receipt")
        if [[ $recorded != "$hash" ]]; then
            # The first Nlib identity patch shipped without an HTTP failure reason.
            # Its subsequent revision ONLY adds httplib error reporting, so migrate
            # that single verified old receipt in place. Never reset source or other
            # cached dependencies for this one-line change; unknown revisions fail.
            if [[ $(basename "$patch") == eden-ps5-net-user-agent.patch &&
                  $recorded == 7117c1c3f353157b7fa46a86c6f77226b1183d9b374462cefe8b8dc5f32bf613 ]]; then
                python3 -B "$root/tools/migrate-net-user-agent-cache.py" "$eden"
                "$validator"
                printf '%s\n' "$hash" > "$receipt"
                echo 'Migrated cached Eden HTTP backport in place (no source-cache reset)'
                return
            fi
            if [[ $(basename "$patch") == eden-ps5-bounded-logging.patch ]]; then
                python3 -B "$root/tools/migrate-bounded-logging-cache.py" "$eden"
                "$validator"
                printf '%s\n' "$hash" > "$receipt"
                echo 'Migrated only the known old PS5 logging flush policy in cached Eden source'
                return
            fi
            echo "Backport changed; reset the Eden source cache: $(basename "$patch")" >&2
            exit 1
        fi
        "$validator"
        return
    fi
    if "$validator" >/dev/null 2>&1; then
        printf '%s\n' "$hash" > "$receipt"
        "$validator"
        return
    fi
    (cd "$eden" && git apply --check "$patch") || {
        echo "Pinned Eden source no longer matches $(basename "$patch")" >&2; exit 1;
    }
    (cd "$eden" && git apply "$patch")
    "$validator"
    printf '%s\n' "$hash" > "$receipt"
}

validate_ps5_hid_watchdog() {
python3 - "$eden" <<'PY'
from pathlib import Path
import sys
text=(Path(sys.argv[1])/'src/hid_core/resources/npad/npad.cpp').read_text()
for needle in ['EDEN_HID_NPAD update={}', 'eden_watchdog_sample', 'eden_watchdog_buttons']:
    if needle not in text: raise SystemExit(f'PS5 HID watchdog missing: {needle}')
print('Encore PS5 HID progress watchdog: PASS')
PY
}


validate_ps5_bounded_logging() {
python3 - "$eden" <<'PYLOG'
from pathlib import Path
import sys
text=(Path(sys.argv[1])/'src/common/logging.cpp').read_text()
for needle in [
    'constexpr auto write_limit = 8_MiB;',
    'void RotatePs5() noexcept',
    'log tail rotated; storage remains bounded',
    'first_filename += ".first.txt"',
    'file->SetSize(0)',
    'FS::SeekOrigin::SetOrigin',
    'error_events == 1 || error_events % 64 == 0',
    'entry.log_level >= Level::Critical',
]:
    if needle not in text:
        raise SystemExit(f'Encore bounded PS5 Eden logging missing: {needle}')
ps5=text[text.index('#ifdef PS5_NATIVE', text.index('using namespace Common::Literals;')):
         text.index('#else', text.index('#ifdef PS5_NATIVE', text.index('using namespace Common::Literals;')))]
if 'enabled = false' in ps5:
    raise SystemExit('PS5 log cap must rotate, not disable logging')
print('Encore bounded rotating PS5 Eden logging: PASS')
PYLOG
}

validate_ps5_net_user_agent() {
python3 - "$eden" <<'PYNET'
from pathlib import Path
import sys
text=(Path(sys.argv[1])/'src/common/net/net.cpp').read_text()
for needle in ['Prospero.Eden-Encore/1', 'request.headers.emplace("User-Agent"', 'request.headers.emplace("Accept"',
               'httplib::to_string(result.error())']:
    if needle not in text: raise SystemExit(f'Encore PS5 HTTP identity missing: {needle}')
print('Encore PS5 HTTP identity backport: PASS')
PYNET
}

validate_dummy_thread_waits() {
python3 - "$eden" <<'PYDW'
from pathlib import Path
import sys
r=Path(sys.argv[1])
lock=(r/'src/core/hle/kernel/k_light_lock.cpp').read_text()
thread=(r/'src/core/hle/kernel/k_thread.cpp').read_text()
for needle in [
    'cur_thread->RequestDummyThreadWait(m_kernel);',
    'cur_thread->GetState() != ThreadState::Waiting || cur_thread->IsDummyThread()',
    'cur_thread->ClearWaitQueue();',
]:
    if needle not in lock: raise SystemExit(f'Dummy-thread KLightLock wait fix missing: {needle}')
for needle in [
    'if (m_wait_queue != nullptr)',
    'this->SetWaitResult(wait_result);',
    'this->SetState(kernel, ThreadState::Runnable);',
]:
    if needle not in thread: raise SystemExit(f'Dummy-thread EndWait fix missing: {needle}')
print('Eden dummy host-thread kernel waits backport: PASS')
PYDW
}

validate_dynarmic_icache() {
python3 - "$eden" <<'PYIC'
from pathlib import Path
import sys
r=Path(sys.argv[1])
a64=(r/'src/core/arm/dynarmic/arm_dynarmic_64.cpp').read_text()
a32=(r/'src/core/arm/dynarmic/arm_dynarmic_32.cpp').read_text()
for needle in [
    '#include "core/arm/debug.h"',
    'Core::InvalidateInstructionCacheRange(m_process, cache_line_start, ICACHE_LINE_SIZE);',
    'case Dynarmic::A64::InstructionCacheOperation::InvalidateAllToPoUInnerSharable:',
]:
    if needle not in a64: raise SystemExit(f'Dynarmic A64 i-cache coherence missing: {needle}')
for text,name in ((a64,'A64'),(a32,'A32')):
    marker='void ArmDynarmic'+name[1:]+'::InvalidateCacheRange'
    block=text[text.index(marker):text.index('}', text.index(marker))+1]
    if 'm_cb->last_code_addr = u64(-1);' not in block:
        raise SystemExit(f'Dynarmic {name} cached code page survives range invalidation')
print('Eden Dynarmic cross-core i-cache coherence backport: PASS')
PYIC
}

apply_one "$root/headless/backports/eden-4473-4477.patch" "$eden/.encore-backport-gpu.sha256" validate_gpu
apply_one "$root/headless/backports/eden-4436-spinlock-mutex.patch" "$eden/.encore-backport-4436.sha256" validate_spinlock_mutex
apply_one "$root/headless/backports/eden-fw23-services.patch" "$eden/.encore-backport-fw23.sha256" validate_fw23
apply_one "$root/headless/backports/eden-runtime-hid.patch" "$eden/.encore-backport-runtime-hid.sha256" validate_runtime_hid
# FC27 real-hardware log evidence: account open-context panics, unhandled BSD Ioctl,
# and 0x100-vs-0x10 IPv4 IPC writes. Ship only on the exact pinned Eden tree.
validate_fc27_hle_compat() {
python3 - "$eden" <<'PYFC27'
from pathlib import Path
import sys
r=Path(sys.argv[1]); account=(r/'src/core/hle/service/acc/acc.cpp').read_text()
bsd=(r/'src/core/hle/service/sockets/bsd.cpp').read_text()
for marker in ('{130, &ACC_U0::LoadOpenContext, "LoadOpenContext"}',
               'profile_manager->GetStoredOpenedUsers()', 'profile_manager->OpenUser(user_id)'):
    if marker not in account: raise SystemExit(f'FC27 account HLE missing: {marker}')
for marker in ('{19, &BSD_USA::Ioctl, "Ioctl"}',
               'constexpr u32 kFionbio = 0x8004667e;',
               'BuildErrnoResponse(ctx, Errno::INVAL)',
               'write_buffer.resize(guest_addrin.len);'):
    if marker not in bsd: raise SystemExit(f'FC27 BSD HLE missing: {marker}')
if bsd.count('write_buffer.resize(guest_addrin.len);') != 2:
    raise SystemExit('Both getpeername and getsockname require bounded IPv4 writes')
print('FC27 HLE account/Ioctl/bounded IPv4 backport: PASS')
PYFC27
}
apply_one "$root/headless/backports/eden-fc27-hle-compat.patch" "$eden/.encore-backport-fc27-hle.sha256" validate_fc27_hle_compat
# The host launcher owns Nlib/catalog HTTPS. The guest Switch network must not
# resolve or contact EA/Nintendo/any Internet host, including raw-IP traffic.
validate_guest_offline() {
python3 - "$eden" <<'PYOFFLINE'
from pathlib import Path
import sys
r = Path(sys.argv[1])
p = (r / 'src/core/hle/service/sockets/encore_guest_network_policy.h').read_text()
bsd = (r / 'src/core/hle/service/sockets/bsd.cpp').read_text()
dns = (r / 'src/core/hle/service/sockets/sfdnsres.cpp').read_text()
nifm = (r / 'src/core/hle/service/nifm/nifm.cpp').read_text()
if 'inline constexpr bool kGuestNetworkOffline = true;' not in p:
    raise SystemExit('Guest network policy must be immutable offline')
if 'if (Eden::Encore::kGuestNetworkOffline) return {-1, Errno::NOTCONN};' not in bsd:
    raise SystemExit('Guest BSD socket allocation is not blocked')
if dns.count('if (Eden::Encore::kGuestNetworkOffline) return {0, GetAddrInfoError::NODATA};') != 2:
    raise SystemExit('Both guest DNS entry points must be blocked')
for key in ('enable == 0 || Eden::Encore::kGuestNetworkOffline',
            'if (Eden::Encore::kGuestNetworkOffline || !st.connected)',
            'const auto has_connection = !Eden::Encore::kGuestNetworkOffline'):
    if key not in nifm:
        raise SystemExit('Guest NIFM policy missing: ' + key)
print('Encore guest-only DNS/BSD/NIFM offline isolation: PASS')
PYOFFLINE
}
apply_one "$root/headless/backports/eden-ps5-guest-offline.patch" "$eden/.encore-backport-guest-offline.sha256" validate_guest_offline
# Quarantine: this dummy-thread wait proposal can clear a wait-queue pointer
# while ThreadState::Waiting still holds. NotifyAvailable/CancelWait in pinned Eden
# may dereference that pointer. A patch-apply test is NOT a correctness test.
# Keep the patch for isolated analysis; never ship it automatically.
if [[ ${EDEN_EXPERIMENTAL_DUMMY_THREAD_WAITS:-OFF} == ON ]]; then
    apply_one "$root/headless/backports/eden-dummy-thread-waits.patch" "$eden/.encore-backport-dummy-thread-waits.sha256" validate_dummy_thread_waits
fi
# Cross-core guest I-cache invalidation is separate from FC27's proven waits.
# Qualify as its own controlled A/B, not in the first stability baseline.
if [[ ${EDEN_EXPERIMENTAL_ICACHE_COHERENCE:-OFF} == ON ]]; then
    apply_one "$root/headless/backports/eden-dynarmic-icache-coherence.patch" "$eden/.encore-backport-dynarmic-icache.sha256" validate_dynarmic_icache
fi
validate_sm_host_wait() {
python3 - "$eden" <<'PYSM'
from pathlib import Path
import sys
r=Path(sys.argv[1])
sm=(r/'src/core/hle/service/sm/sm.h').read_text()
audio=(r/'src/core/hle/service/audio/audio_controller.cpp').read_text()
audio_h=(r/'src/core/hle/service/audio/audio_controller.h').read_text()
for needle in ['Kernel::GetCurrentThread(kernel).IsDummyThread()', 'std::this_thread::sleep_for(1ms)',
               'SessionRequestHandlerFactory factory', 'std::scoped_lock']:
    if needle not in sm:
        raise SystemExit(f'Eden host-thread-safe blocking GetService missing: {needle}')
if 'kernel.IsShuttingDown()' in sm:
    raise SystemExit('blocking GetService must not return nullptr merely because shutdown began')
if 'm_set_sys =\n        system.ServiceManager().GetService' in audio:
    raise SystemExit('audctl constructor still blocks on set:sys')
for needle in ['IAudioController::GetSetSys()', 'std::call_once(m_set_sys_once',
               'GetSetSys()->GetAudioOutputMode', 'GetSetSys()->SetAudioOutputMode']:
    if needle not in audio:
        raise SystemExit(f'lazy audctl set:sys lookup missing: {needle}')
for needle in ['std::once_flag m_set_sys_once', 'GetSetSys();']:
    if needle not in audio_h:
        raise SystemExit(f'lazy audctl declaration missing: {needle}')
print('Eden host-thread-safe service waits + lazy audctl lookup: PASS')
PYSM
}

apply_one "$root/headless/backports/eden-ps5-hid-watchdog.patch" "$eden/.encore-backport-ps5-hid-watchdog.sha256" validate_ps5_hid_watchdog
apply_one "$root/headless/backports/eden-ps5-net-user-agent.patch" "$eden/.encore-backport-ps5-net-user-agent.sha256" validate_ps5_net_user_agent
validate_launcher_http_budget() {
    grep -Fq 'const std::size_t timeout_seconds = url == "https://api.nlib.cc" ? 3 : 5;' "$eden/src/common/net/net.cpp" || {
        echo "Launcher network response-time budget missing" >&2; return 1;
    }
}
apply_one "$root/headless/backports/eden-ps5-launcher-fast-http.patch" "$eden/.encore-backport-launcher-http.sha256" validate_launcher_http_budget
apply_one "$root/headless/backports/eden-ps5-bounded-logging.patch" "$eden/.encore-backport-ps5-bounded-logging.sha256" validate_ps5_bounded_logging
# A separate receipt is essential: already-cached bounded-logging sources
# must receive this exact delta rather than accept a changed old hash.
validate_ps5_crash_only_logging() {
python3 - "$eden" <<'PYQUIET'
from pathlib import Path
import sys
s=(Path(sys.argv[1])/'src/common/logging.cpp').read_text()
required=(
    'extern "C" bool eden_native_detailed_logging() noexcept;',
    'extern "C" unsigned eden_native_logging_generation() noexcept;',
    'if (!eden_native_detailed_logging()) return;',
    'const auto generation = eden_native_logging_generation();',
    'file.reset();',
    'last_generation = generation;',
    'if (file && eden_native_detailed_logging()) file->Flush();',
)
for text in required:
    if text not in s:
        raise SystemExit('PS5 crash-only logging patch missing: '+text)
print('Eden PS5 crash-only native file backend: PASS')
PYQUIET
}
apply_one "$root/headless/backports/eden-ps5-crash-only-logging.patch" "$eden/.encore-backport-ps5-crash-only-logging.sha256" validate_ps5_crash_only_logging
validate_ps5_gpu_memory_mapping() {
python3 - "$eden" <<'PYGPU'
from pathlib import Path
import sys
src=(Path(sys.argv[1])/'src/core/device_memory_manager.inc').read_text()
# A restored, fully-patched Eden tree already includes the later C++17
# atomic publication patch. That patch intentionally replaces these four
# unprotected legacy accesses. Validate either coherent *whole stage*:
# never require a stale direct write, and never accept a partial migration.
legacy = ('invalid.continuity_tracker = 0;',
          'valid.continuity_tracker = 0;',
          'entry.continuity_tracker = 0;',
          'tracked_entries[first_page + i].compressed_physical_ptr != backing + i')
atomic = ('AtomicStoreContinuity(invalid, 0);',
          'AtomicStoreContinuity(valid, 0);',
          'AtomicStoreContinuity(entry, 0);',
          'AtomicLoadPhysical(tracked_entries[first_page + i]) != backing + i')
has_legacy = all(token in src for token in legacy)
has_atomic = all(token in src for token in atomic)
if not (has_legacy or has_atomic):
    raise SystemExit('PS5 GPU memory mapping stage missing/incomplete direct or atomic continuity guards')
if has_atomic:
    h=(Path(sys.argv[1])/'src/core/device_memory_manager.h').read_text()
    for token in ('__atomic_load_n(&page.compressed_physical_ptr, __ATOMIC_ACQUIRE)',
                  '__atomic_store_n(&page.continuity_tracker, value, __ATOMIC_RELAXED)'):
        if token not in h:
            raise SystemExit('PS5 cached GPU atomic mapping lacks publication helper: '+token)
    if any(token in src for token in legacy):
        raise SystemExit('PS5 GPU mapping mixes atomic and plain per-page access')
for token in ('observed == first_backing + n',
              'if (addr >= device_as_size || size > device_as_size - addr)',
              'if (address >= device_as_size) return nullptr;',
              '::Eden::GpuFault::ShouldReportRead()', '::Eden::GpuFault::ShouldReportWrite()'):
    if token not in src:
        raise SystemExit(f'Pinned PS5 GPU memory backport missing: {token}')
if src.count('#ifndef PS5_NATIVE') < 4:
    raise SystemExit('PS5 shared translation-cache bypass absent')
print('PS5 GPU remap/continuity backport: PASS')
PYGPU
}
apply_one "$root/headless/backports/eden-ps5-gpu-memory-mapping.patch" "$eden/.encore-backport-ps5-gpu-memory-mapping.sha256" validate_ps5_gpu_memory_mapping
validate_ps5_gpu_remap_reverse() {
python3 - "$eden" <<'PYREMAP'
from pathlib import Path
import sys
code=(Path(sys.argv[1])/'src/core/device_memory_manager.inc').read_text()
for token in ('previous_physical == replacement_physical',
              'compressed_device_addr.GetAndFault(previous_physical - 1U)',
              'impl->multi_dev_address.Unregister(', '::Eden::GpuFault::remap_replaced.fetch_add',
              'EDEN_GPU_REMAP_REVERSE_MISMATCH'):
    if token not in code: raise SystemExit('GPU remap reverse-index fix missing: '+token)
print('PS5 GPU Map reverse-mapping replacement: PASS')
PYREMAP
}
apply_one "$root/headless/backports/eden-ps5-gpu-remap-reverse.patch" "$eden/.encore-backport-ps5-gpu-remap-reverse.sha256" validate_ps5_gpu_remap_reverse
validate_ps5_gpu_unmap_reverse() {
python3 - "$eden" <<'PYUNMAP'
from pathlib import Path
import sys
code=(Path(sys.argv[1])/'src/core/device_memory_manager.inc').read_text()
# The later atomic reverse-table patch rewrites the same comparison.
# A cache restored after that patch must prove its atomic equivalent,
# not resurrect a direct racy read of the reverse mapping table.
legacy='if (base_dev != retiring_page) {'
atomic='if (AtomicLoadReverse(base_dev) != retiring_page) {'
if legacy not in code and atomic not in code:
    raise SystemExit('PS5 GPU Unmap reverse-map comparison guard missing')
if atomic in code:
    h=(Path(sys.argv[1])/'src/core/device_memory_manager.h').read_text()
    if '__atomic_load_n(&slot, __ATOMIC_ACQUIRE)' not in h:
        raise SystemExit('PS5 cached GPU reverse comparison missing atomic acquire helper')
    if legacy in code:
        raise SystemExit('PS5 GPU Unmap still contains plain reverse comparison')
for token in ('EDEN_GPU_UNMAP_REVERSE_MISMATCH',
              'ShouldReportRemapMismatch()'):
    if token not in code: raise SystemExit('PS5 GPU Unmap reverse-map guard missing: '+token)
print('Pinned PS5 GPU reverse-map unmap guard: PASS')
PYUNMAP
}
apply_one "$root/headless/backports/eden-ps5-gpu-unmap-reverse-guard.patch" "$eden/.encore-backport-ps5-gpu-unmap-reverse-guard.sha256" validate_ps5_gpu_unmap_reverse
validate_ps5_gpu_multi_missing() {
python3 - "$eden" <<'PYMULTI'
from pathlib import Path
import sys
s=(Path(sys.argv[1])/'src/core/device_memory_manager.inc').read_text()
for token in ('bool Contains(u32 value, u32 start_entry) const noexcept',
              'steps < storage.size()', 'start_entry > storage.size()',
              'EDEN_GPU_REMAP_MULTI_MISSING', 'EDEN_GPU_UNMAP_MULTI_MISSING'):
    if token not in s: raise SystemExit('PS5 reverse multi-map safe unlink missing: '+token)
print('PS5 multi reverse-map unregistration hardening: PASS')
PYMULTI
}
apply_one "$root/headless/backports/eden-ps5-gpu-multi-missing-guard.patch" "$eden/.encore-backport-ps5-gpu-multi-missing-guard.sha256" validate_ps5_gpu_multi_missing
validate_ps5_gpu_map_bounds() {
python3 - "$eden" <<'PYMAPBOUNDS'
from pathlib import Path
import sys
s=(Path(sys.argv[1])/'src/core/device_memory_manager.inc').read_text()
for token in ('virtual_address >= guest_as_size', 'size > guest_as_size - virtual_address',
              'address >= device_as_size || size > device_as_size - address',
              'EDEN_GPU_MAP_BAD_RANGE', 'EDEN_GPU_UNMAP_BAD_RANGE'):
    if token not in s: raise SystemExit('PS5 GPU Map/Unmap range guard missing: '+token)
print('PS5 GPU Map/Unmap bounds before commit/invalidate: PASS')
PYMAPBOUNDS
}
apply_one "$root/headless/backports/eden-ps5-gpu-map-bounds.patch" "$eden/.encore-backport-ps5-gpu-map-bounds.sha256" validate_ps5_gpu_map_bounds
validate_ps5_gpu_physical_capacity() {
python3 - "$eden" <<'PYPHYSICAL'
from pathlib import Path
import sys
s=(Path(sys.argv[1])/'src/core/device_memory_manager.inc').read_text()
for token in ('GetIntendedMemorySize() >> Memory::YUZU_PAGEBITS',
              'EDEN_GPU_MAP_OUTSIDE_DRAM', 'EDEN_GPU_REVERSE_PHYS_OOB',
              'static_cast<size_t>(phys_addr - 1U) >= compressed_device_addr.size()'):
    if token not in s: raise SystemExit('PS5 GPU physical reverse-map safety missing: '+token)
print('PS5 physical reverse mapping: full 4/6/8/10/12 GiB configured RAM PASS')
PYPHYSICAL
}
apply_one "$root/headless/backports/eden-ps5-gpu-physical-capacity.patch" "$eden/.encore-backport-ps5-gpu-physical-capacity.sha256" validate_ps5_gpu_physical_capacity
validate_ps5_gpu_remap_invalidation() {
python3 - "$eden" <<'PYREMAPCACHE'
from pathlib import Path
import sys
code=(Path(sys.argv[1])/'src/core/device_memory_manager.inc').read_text()
for token in ('bool replaced_gpu_mapping = false;', 'std::unique_lock lk(mapping_guard);',
              'replaced_gpu_mapping = true;', 'lk.unlock();',
              'device_inter->InvalidateRegion(address, size);',
              'remap_cache_evictions.fetch_add'):
    if token not in code: raise SystemExit('PS5 GPU texture invalidation on remap missing: '+token)
assert code.index('lk.unlock();') < code.index('remap_cache_evictions.fetch_add')
print('PS5 GPU changed physical map invalidation outside mapping mutex: PASS')
PYREMAPCACHE
}
apply_one "$root/headless/backports/eden-ps5-gpu-remap-cache-invalidate.patch" "$eden/.encore-backport-ps5-gpu-remap-cache-invalidate.sha256" validate_ps5_gpu_remap_invalidation
validate_ps5_gpu_empty_multi_head() {
python3 - "$eden" <<'PYMULTIHEAD'
from pathlib import Path
import sys
s=(Path(sys.argv[1])/'src/core/device_memory_manager.inc').read_text()
for token in ('first != 0 ? impl->multi_dev_address.ReleaseEntry(first) : 0',
              'new_start != 0 ? impl->multi_dev_address.ReleaseEntry(new_start) : 0'):
    if token not in s: raise SystemExit('unsafe zero-head MultiAddressContainer release: '+token)
print('PS5 GPU last reverse node never calls ReleaseEntry(0): PASS')
PYMULTIHEAD
}
apply_one "$root/headless/backports/eden-ps5-gpu-empty-multi-head.patch" "$eden/.encore-backport-ps5-gpu-empty-multi-head.sha256" validate_ps5_gpu_empty_multi_head
validate_ps5_gpu_physical_read_bounds() {
python3 - "$eden" <<'PYPHYSREAD'
from pathlib import Path
import sys
s=(Path(sys.argv[1])/'src/core/device_memory_manager.inc').read_text()
for token in ('EDEN_GPU_READ_WRITE_PHYS_OOB',
              'first_phys >= compressed_device_addr.size()',
              'compressed_device_addr.size() - first_phys',
              'static_cast<size_t>(phys_addr - 1U) >= compressed_device_addr.size()',
              'static_cast<size_t>(phys_addr - 1U) < compressed_device_addr.size()'):
    if token not in s:
        raise SystemExit('missing bounded PS5 GPU physical block reader: ' + token)
walk=s[s.index('void DeviceMemoryManager<Traits>::WalkBlock('):
       s.index('void DeviceMemoryManager<Traits>::ReadBlock(')]
if walk.index('first_phys >= compressed_device_addr.size()') > walk.index('on_memory(copy_amount, mem_ptr)'):
    raise SystemExit('unsafe physical DRAM continuity check after block read')
for method in ('ReadBlock(', 'ReadBlockUnsafe('):
    segment=s[s.index('void DeviceMemoryManager<Traits>::'+method):]
    if segment.index('compressed_device_addr.size()') > segment.index('std::memcpy(dest_pointer, mem_ptr + page_offset, size)'):
        raise SystemExit('unsafe physical DRAM check after ' + method + ' copy')
print('PS5 GPU bounded physical block readers, no invalid page dereference: PASS')
PYPHYSREAD
}
apply_one "$root/headless/backports/eden-ps5-gpu-physical-read-bounds.patch" "$eden/.encore-backport-ps5-gpu-physical-read-bounds.sha256" validate_ps5_gpu_physical_read_bounds
validate_ps5_gpu_block_flush_bounds() {
python3 - "$eden" <<'PYBLOCKFLUSH'
from pathlib import Path
import sys
s=(Path(sys.argv[1])/'src/core/device_memory_manager.inc').read_text()
read=s[s.index('void DeviceMemoryManager<Traits>::ReadBlock('):
       s.index('void DeviceMemoryManager<Traits>::WriteBlock(')]
write=s[s.index('void DeviceMemoryManager<Traits>::WriteBlock('):
        s.index('void DeviceMemoryManager<Traits>::ReadBlockUnsafe(')]
for token in ('if (size == 0) return;',
              'if (address >= device_as_size || size > device_as_size - address)',
              'std::memset(dest_pointer, 0, size);'):
    if token not in read:
        raise SystemExit('missing fail-closed GPU invalid-block read guard: '+token)
if read.index('std::memset(dest_pointer, 0, size);') > read.index('device_inter->FlushRegion(address, size);'):
    raise SystemExit('invalid GPU read flushed before its range was checked')
if 'if (size != 0 && address < device_as_size && size <= device_as_size - address)' not in write:
    raise SystemExit('invalid GPU block write still invalidates renderer region')
print('PS5 GPU reject invalid flush/write-invalidation extents: PASS')
PYBLOCKFLUSH
}
apply_one "$root/headless/backports/eden-ps5-gpu-block-flush-bounds.patch" "$eden/.encore-backport-ps5-gpu-block-flush-bounds.sha256" validate_ps5_gpu_block_flush_bounds
validate_ps5_gpu_span_physical_bounds() {
python3 - "$eden" <<'PYSPANBOUNDS'
from pathlib import Path
import sys
s=(Path(sys.argv[1])/'src/core/device_memory_manager.inc').read_text()
# WalkBlock already has its own independent physical DRAM bound check.
# Count only inside EACH GetSpan overload, not in the entire .inc file:
# two GetSpan guards + WalkBlock must not produce a false build failure.
need=('first_phys >= compressed_device_addr.size()',
      'page_count > compressed_device_addr.size() - first_phys')
for signature in ('u8* DeviceMemoryManager<Traits>::GetSpan(',
                  'const u8* DeviceMemoryManager<Traits>::GetSpan('):
    start=s.index(signature)
    end=s.index('template <typename Traits>',start)
    block=s[start:end]
    for token in need:
        if block.count(token) != 1:
            raise SystemExit('GPU GetSpan physical bounds missing or duplicated: '+signature+' '+token)
    if not (block.index('if (backing == 0) return nullptr;') <
            block.index('first_phys >= compressed_device_addr.size()') <
            block.index('page_count > compressed_device_addr.size() - first_phys') <
            block.index('for (size_t i = 1; i < page_count; ++i)')):
        raise SystemExit('GPU GetSpan physical capacity check incorrectly placed: '+signature)
print('PS5 GPU direct-span physical bounds (const and mutable): PASS')
PYSPANBOUNDS
}
apply_one "$root/headless/backports/eden-ps5-gpu-span-physical-bounds.patch" "$eden/.encore-backport-ps5-gpu-span-physical-bounds.sha256" validate_ps5_gpu_span_physical_bounds
validate_ps5_gpu_reverse_inline() {
python3 - "$eden" <<'PYGPUINLINE'
from pathlib import Path
import sys
s=(Path(sys.argv[1])/'src/core/device_memory_manager.h').read_text()
for token in ('if ((address >> page_bits) >= compressed_device_addr.size()) return;',
              'if (base == 0) return;',
              'if (host < physical_base ||',
              'host - physical_base >= (compressed_device_addr.size() << page_bits)',
              'if (address >= device_as_size) return 0;',
              'static_cast<size_t>(paddr - 1) >= compressed_device_addr.size()'):
    if token not in s: raise SystemExit('GPU inline reverse lookup guard missing: '+token)
print('PS5 inline CPU->GPU reverse lookup capacity and missing-mapping guard: PASS')
PYGPUINLINE
}
apply_one "$root/headless/backports/eden-ps5-gpu-reverse-inline-bounds.patch" "$eden/.encore-backport-ps5-gpu-reverse-inline-bounds.sha256" validate_ps5_gpu_reverse_inline
validate_ps5_gpu_atomic_forward_table() {
python3 - "$eden" <<'PYGPUATOMIC'
from pathlib import Path
import sys
root=Path(sys.argv[1])/'src/core'
h=(root/'device_memory_manager.h').read_text()
c=(root/'device_memory_manager.inc').read_text()
for token in ('__atomic_load_n(&page.compressed_physical_ptr, __ATOMIC_ACQUIRE)',
              '__atomic_store_n(&page.compressed_physical_ptr, value, __ATOMIC_RELEASE)',
              '__atomic_load_n(&page.continuity_tracker, __ATOMIC_RELAXED)',
              '__atomic_store_n(&page.continuity_tracker, value, __ATOMIC_RELAXED)',
              '__atomic_load_n(&page.cpu_backing_address, __ATOMIC_ACQUIRE)',
              '__atomic_store_n(&page.cpu_backing_address, value, __ATOMIC_RELEASE)',
              'void InsertCPUBacking(size_t page_index, VAddr address, Asid asid)'):
    if token not in h:
        raise SystemExit('GPU atomic page publication missing: '+token)
if 'constexpr void InsertCPUBacking' in h:
    raise SystemExit('non-constexpr GPU atomic publisher called from constexpr method')
for field in ('.compressed_physical_ptr', '.continuity_tracker', '.cpu_backing_address'):
    if field in c:
        raise SystemExit('data-racy GPU read/write bypasses atomic helper: '+field)
for token in ('AtomicStorePhysical(valid, phys_addr)',
              'AtomicStorePhysical(entry, 0)',
              'AtomicLoadPhysical(tracked_entries[page_index])',
              'AtomicLoadContinuity(tracked_entries[page_index])',
              'AtomicLoadPhysical(tracked_entries[first_page + i])'):
    if token not in c:
        raise SystemExit('GPU atomic writer/reader missing: '+token)
print('PS5 GPU forward-page atomic publication; no coarse hot-path mutex: PASS')
PYGPUATOMIC
}
apply_one "$root/headless/backports/eden-ps5-gpu-atomic-forward-table.patch" "$eden/.encore-backport-ps5-gpu-atomic-forward-table.sha256" validate_ps5_gpu_atomic_forward_table
validate_ps5_gpu_atomic_reverse_table() {
python3 - "$eden" <<'PYREVERSEATOMIC'
from pathlib import Path
import sys
r=Path(sys.argv[1])/'src/core'
h=(r/'device_memory_manager.h').read_text()
c=(r/'device_memory_manager.inc').read_text()
for token in ('__atomic_load_n(&slot, __ATOMIC_ACQUIRE)',
              '__atomic_store_n(&slot, value, __ATOMIC_RELEASE)',
              'AtomicLoadReverse(compressed_device_addr[(address >> page_bits)])'):
    if token not in h:
        raise SystemExit('missing uniform GPU physical reverse slot publication: '+token)
for token in ('AtomicStoreReverse(reverse, 0)',
              'AtomicStoreReverse(base_dev, 0)',
              'AtomicLoadReverse(compressed_device_addr[phys_addr - 1U])',
              'AtomicLoadReverse(compressed_device_addr[phys_addr])'):
    if token not in c:
        raise SystemExit('missing GPU reverse mapping atomic reader/writer: '+token)
for old in ('const u32 base_dev = compressed_device_addr[',
            'u32 backing = compressed_device_addr[',
            'compressed_device_addr.GetAndFault(phys_addr - 1U) ='):
    if old in c:
        raise SystemExit('plain reverse GPU slot access bypassed atomic builtins: '+old)
print('PS5 GPU physical reverse scalar slots: same atomic protocol in all C++ TUs PASS')
PYREVERSEATOMIC
}
apply_one "$root/headless/backports/eden-ps5-gpu-atomic-reverse-table.patch" "$eden/.encore-backport-ps5-gpu-atomic-reverse-table.sha256" validate_ps5_gpu_atomic_reverse_table
validate_ps5_gpu_asid_lifetime() {
python3 - "$eden" <<'PYGPUASID'
from pathlib import Path
import sys
r=Path(sys.argv[1])/'src/core'
h=(r/'device_memory_manager.h').read_text()
c=(r/'device_memory_manager.inc').read_text()
for token in ('#include <shared_mutex>',
              'mutable std::shared_mutex process_registry_guard',
              'std::shared_lock registry_lk(process_registry_guard);'):
    if token not in h:
        raise SystemExit('GPU process registry lifetime lock missing: '+token)
for name in ('Map(', 'TrackContinuityImpl(', 'RegisterProcess(', 'UnregisterProcess(',
             'UpdatePagesCachedCountNoLock(', 'UpdatePagesCachedCount(', 'UpdatePagesCachedBatch('):
    if 'DeviceMemoryManager<Traits>::'+name not in c:
        raise SystemExit('GPU ASID lifetime missing method: '+name)
checks=('asid.id >= registered_processes.size()',
        'registered_processes[asid.id] == nullptr',
        'std::unique_lock registry_lk(process_registry_guard)',
        'std::shared_lock registry_lk(process_registry_guard)',
        'if (size == 0 || addr >= device_as_size || size > device_as_size - addr)',
        'asid_2.id >= registered_processes.size()',
        'memory_device_inter != nullptr',
        'asid = asid_2;')
for token in checks:
    if token not in c:
        raise SystemExit('GPU stale ASID guard missing: '+token)
# No registry lock on per-texture/per-block GPU data operations.
for method in ('ReadBlock(', 'ReadBlockUnsafe(', 'WriteBlock(', 'WriteBlockUnsafe(', 'GetSpan('):
    begin=c.index('DeviceMemoryManager<Traits>::'+method)
    end=c.find('template <typename Traits>',begin+5)
    if 'registry_lk' in c[begin:end if end>=0 else len(c)]:
        raise SystemExit('GPU registry mutex accidentally added to hot path '+method)
print('PS5 GPU registered ASID lifetime, duplicate unregister and cache bounds: PASS')
PYGPUASID
}
apply_one "$root/headless/backports/eden-ps5-gpu-asid-lifetime-guard.patch" "$eden/.encore-backport-ps5-gpu-asid-lifetime-guard.sha256" validate_ps5_gpu_asid_lifetime
validate_ps5_gpu_asid_no_reuse() {
python3 - "$eden" <<'PYASIDABA'
from pathlib import Path
import sys
r=Path(sys.argv[1])/'src/core'
h=(r/'device_memory_manager.h').read_text()
s=(r/'device_memory_manager.inc').read_text()
register=s[s.index('Asid DeviceMemoryManager<Traits>::RegisterProcess('):
           s.index('void DeviceMemoryManager<Traits>::UnregisterProcess(')]
unregister=s[s.index('void DeviceMemoryManager<Traits>::UnregisterProcess('):
             s.index('void DeviceMemoryManager<Traits>::UpdatePagesCachedCountNoLock(')]
if 'id_pool' in register+unregister+h:
    raise SystemExit('GPU ASID ABA risk: recycled id pool still exists')
for token in ('registered_processes.emplace_back(memory_device_inter)',
              'registered_processes.size() - 1U',
              'constexpr size_t max_ids',
              'memory_device_inter == nullptr',
              'return Asid{static_cast<size_t>(-1)}'):
    if token not in register:
        raise SystemExit('missing monotonic bounded GPU ASID registration: '+token)
if 'registered_processes[asid.id] = nullptr' not in unregister:
    raise SystemExit('retired GPU ASID must be tombstoned')
if 'registered_processes[asid.id] == nullptr' not in unregister:
    raise SystemExit('duplicate GPU ASID unregister must be idempotent')
print('PS5 GPU ASID tombstones prevent recycled-process aliasing: PASS')
PYASIDABA
}
apply_one "$root/headless/backports/eden-ps5-gpu-asid-no-reuse.patch" "$eden/.encore-backport-ps5-gpu-asid-no-reuse.sha256" validate_ps5_gpu_asid_no_reuse
validate_ps5_guest_mapping_diagnostics() {
python3 - "$eden" <<'PYMAP'
from pathlib import Path
import sys
source=(Path(sys.argv[1])/'src/core/memory.cpp').read_text()
for token in ('EDEN_GUEST_MAP_POINTER_ZERO', 'EDEN_GUEST_MAPPED_NULL_POINTER',
              'ShouldReportGuestMapZero()', 'ShouldReportGuestNullMapped()'):
    if token not in source:
        raise SystemExit('Pinned guest CPU memory diagnostic missing: '+token)
print('PS5 guest page-table storm reports bounded: PASS')
PYMAP
}
apply_one "$root/headless/backports/eden-ps5-guest-mapping-diagnostics.patch" "$eden/.encore-backport-ps5-guest-mapping-diagnostics.sha256" validate_ps5_guest_mapping_diagnostics
validate_ps5_guest_walk_memory() {
python3 - "$eden" <<'PYWALK'
from pathlib import Path
import sys
s=(Path(sys.argv[1])/'src/core/memory.cpp').read_text()
assert 'current_page_table->entries.CommitRegion(page_index, page_index + (size >> YUZU_PAGEBITS) + 1);' in s
assert '// WalkBlock reads PageEntryData; it never writes the page table.' in s
assert 'if (!pointer) {' in s and 'on_unmapped(offset, copy_amount, current_vaddr);' in s
assert 'Preserve the zero-fill / write-discard contract' in s
print('Pinned PS5 guest memory walker: read-only lookup and invalid-pointer guard PASS')
PYWALK
}
apply_one "$root/headless/backports/eden-ps5-guest-walk.patch" "$eden/.encore-backport-ps5-guest-walk.sha256" validate_ps5_guest_walk_memory
validate_ps5_guest_zero_alias() {
python3 - "$eden" <<'PYALIAS'
from pathlib import Path
import sys
s=(Path(sys.argv[1])/'src/core/memory.cpp').read_text()
for token in ('IsDirectBackingAlias(u64 address, std::size_t bytes) const',
              'GetIntendedMemorySize()', 'guest_alias_mapped.fetch_add',
              'guest_alias_access.fetch_add', 'return reinterpret_cast<u8*>(vaddr)',
              'on_memory(offset, copy_amount, reinterpret_cast<u8*>(current_vaddr))'):
    if token not in s:
        raise SystemExit(f'PS5 delta-zero guest pointer alias recovery missing: {token}')
print('Pinned PS5 guest delta-zero pointer alias guard: PASS')
PYALIAS
}
apply_one "$root/headless/backports/eden-ps5-guest-zero-alias.patch" "$eden/.encore-backport-ps5-guest-zero-alias.sha256" validate_ps5_guest_zero_alias
validate_ps5_guest_null_backing() {
python3 - "$eden" <<'PYNULLBACK'
from pathlib import Path
import sys
source=(Path(sys.argv[1])/'src/core/memory.cpp').read_text()
begin=source.index('void MapPages(Common::PageTable&')
end=source.index('    template<typename F, typename G>',begin)
mapping=source[begin:end]
for required in ('auto* backing = system.DeviceMemory().GetPointer<u8>(target);',
                 'if (backing == nullptr) {',
                 'invalid.Store(false, Common::PageType::Unmapped, current_block, 0);',
                 'EDEN_GUEST_MAP_NO_BACKING',
                 'auto host_ptr = reinterpret_cast<u64>(backing) - (base << YUZU_PAGEBITS);',
                 'IsDirectBackingAlias(base << YUZU_PAGEBITS, YUZU_PAGESIZE)'):
    if required not in mapping:
        raise SystemExit('PS5 guest page null physical backing guard missing: '+required)
assert mapping.index('if (backing == nullptr)') < mapping.index('entry.Store(false, type, current_block, host_ptr)')
assert mapping.count('if (backing == nullptr) {') == 1
print('Pinned PS5 guest MapPages no physical backing: explicit Unmapped, zero-delta alias preserved PASS')
PYNULLBACK
}
apply_one "$root/headless/backports/eden-ps5-guest-map-null-backing.patch" "$eden/.encore-backport-ps5-guest-map-null-backing.sha256" validate_ps5_guest_null_backing
validate_ps5_guest_span() {
python3 - "$eden" <<'PYSPAN'
from pathlib import Path
import sys
source=(Path(sys.argv[1])/'src/core/memory.cpp').read_text()
for needed in ('(addr + size - 1) >> YUZU_PAGEBITS',
               'if (!size || !AddressSpaceContains(*current_page_table, addr, size))',
               'if (p != delta || t != type || b != block) return nullptr;',
               'static_cast<const Impl&>(*this).GetSpan(addr, size)'):
    if needed not in source: raise SystemExit('PS5 guest span validation missing: '+needed)
print('Pinned PS5 GetSpan correct end page and contiguous page guard PASS')
PYSPAN
}
apply_one "$root/headless/backports/eden-ps5-guest-span.patch" "$eden/.encore-backport-ps5-guest-span.sha256" validate_ps5_guest_span
# Citron fixes inform this host-worker SM/audctl proposal. It compiles on the
# pinned Eden source, but PS5 service-init/shutdown behavior is not yet proven.
# Keep it OFF in the baseline; qualify with a separate controlled HLE A/B.
if [[ ${EDEN_EXPERIMENTAL_SM_HOST_WAIT:-OFF} == ON ]]; then
    apply_one "$root/headless/backports/eden-sm-host-wait.patch" "$eden/.encore-backport-sm-host-wait.sha256" validate_sm_host_wait
fi
