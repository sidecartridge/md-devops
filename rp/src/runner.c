#include "include/runner.h"

#include <stdint.h>

#include "chandler.h"
#include "debug.h"
#include "display.h"
#include "emul.h"
#include "pico/time.h"
#include "tprotocol.h"

// Chandler callback: receives the m68k Runner's report-back commands
// (RUNNER_CMD_DONE_EXECUTE so far). The payload follows the same
// shape gemdrive_command_cb uses — TPROTO_GET_PAYLOAD_PARAM32 reads
// the next 32-bit user payload word in m68k order.
static void __not_in_flash_func(runner_command_cb)(
    TransmissionProtocol *protocol, uint16_t *payload) {
  switch (protocol->command_id) {
    case RUNNER_CMD_DONE_EXECUTE: {
      uint32_t raw = TPROTO_GET_PAYLOAD_PARAM32(payload);
      int32_t exit_code = (int32_t)raw;
      uint32_t now_ms = (uint32_t)to_ms_since_boot(get_absolute_time());
      DPRINTF("Runner: EXECUTE done, exit_code=%ld\n",
              (long)exit_code);
      emul_recordRunnerExecuteDone(exit_code, now_ms);
      // Clear the cartridge sentinel back to NOP so the Runner's
      // poll loop doesn't re-trigger the same RUNNER_CMD_EXECUTE
      // value the moment it falls back through to runner_poll_loop
      // (the m68k can't write to the cartridge address space, so the
      // RP owns the clear).
      SEND_COMMAND_TO_DISPLAY(0);
      return;
    }
    case RUNNER_CMD_DONE_LOAD: {
      // Epic 06 / S5. Payload is i32: >0 = basepage pointer
      // (success), <0 = -GEMDOS errno (load failed), 0 =
      // unexpected (treat as error). The RP-side state setter
      // splits on sign and updates pendingBasepage / load errno
      // accordingly.
      uint32_t raw = TPROTO_GET_PAYLOAD_PARAM32(payload);
      int32_t result = (int32_t)raw;
      uint32_t now_ms = (uint32_t)to_ms_since_boot(get_absolute_time());
      DPRINTF("Runner: LOAD done, result=%ld (%s)\n",
              (long)result,
              result > 0 ? "basepage" : "errno");
      emul_recordRunnerLoadDone(result, now_ms);
      SEND_COMMAND_TO_DISPLAY(0);
      return;
    }
    case RUNNER_CMD_DONE_EXEC: {
      // Epic 06 / S6. Payload i32 = program exit code. The
      // basepage stays loaded (Pexec(4) does NOT free it — that's
      // S7's runner unload), so re-exec on the same basepage
      // works without a fresh load.
      uint32_t raw = TPROTO_GET_PAYLOAD_PARAM32(payload);
      int32_t exit_code = (int32_t)raw;
      uint32_t now_ms = (uint32_t)to_ms_since_boot(get_absolute_time());
      DPRINTF("Runner: EXEC done, exit_code=%ld\n",
              (long)exit_code);
      emul_recordRunnerExecDone(exit_code, now_ms);
      SEND_COMMAND_TO_DISPLAY(0);
      return;
    }
    case RUNNER_CMD_DONE_UNLOAD: {
      // Epic 06 / S7. Payload i32 = GEMDOS Mfree result (0 on
      // success, negative GEMDOS errno on failure). The state
      // setter clears pendingBasepage on success and preserves
      // it on failure — so a follow-up `runner status` honestly
      // reports the basepage is still allocated if Mfree balked.
      uint32_t raw = TPROTO_GET_PAYLOAD_PARAM32(payload);
      int32_t result = (int32_t)raw;
      uint32_t now_ms = (uint32_t)to_ms_since_boot(get_absolute_time());
      DPRINTF("Runner: UNLOAD done, mfree_result=%ld\n",
              (long)result);
      emul_recordRunnerUnloadDone(result, now_ms);
      SEND_COMMAND_TO_DISPLAY(0);
      return;
    }
    case RUNNER_CMD_DONE_CD: {
      uint32_t raw = TPROTO_GET_PAYLOAD_PARAM32(payload);
      int32_t errnum = (int32_t)raw;
      uint32_t now_ms = (uint32_t)to_ms_since_boot(get_absolute_time());
      DPRINTF("Runner: CD done, errno=%ld\n", (long)errnum);
      emul_recordRunnerCdDone(errnum, now_ms);
      SEND_COMMAND_TO_DISPLAY(0);
      return;
    }
    case RUNNER_CMD_DONE_RES: {
      uint32_t raw = TPROTO_GET_PAYLOAD_PARAM32(payload);
      int32_t errnum = (int32_t)raw;
      uint32_t now_ms = (uint32_t)to_ms_since_boot(get_absolute_time());
      DPRINTF("Runner: RES done, errno=%ld\n", (long)errnum);
      emul_recordRunnerResDone(errnum, now_ms);
      SEND_COMMAND_TO_DISPLAY(0);
      return;
    }
    case RUNNER_CMD_DONE_MEMINFO: {
      // send_write_sync packs the payload as: d3.l, d4.l, d5.l (three
      // 32-bit scratch slots) followed by the buffer body, transmitted
      // word-by-word via `tst.b (a0, d.w)`. The PIO captures each word
      // as the m68k saw it (uint16_t native), so payload[j] is already
      // in the right byte order — NO byte-swap needed. Just skip the
      // three scratch slots and read the words directly. m68k stored
      // each u32 as `move.l value, offs(a4)` (high half first), so u32
      // = (word[0] << 16) | word[1].
      TPROTO_NEXT32_PAYLOAD_PTR(payload);
      TPROTO_NEXT32_PAYLOAD_PTR(payload);
      TPROTO_NEXT32_PAYLOAD_PTR(payload);
      runner_meminfo_t snap;
      snap.membot    = ((uint32_t)payload[0] << 16) | payload[1];
      snap.memtop    = ((uint32_t)payload[2] << 16) | payload[3];
      snap.phystop   = ((uint32_t)payload[4] << 16) | payload[5];
      snap.screenmem = ((uint32_t)payload[6] << 16) | payload[7];
      snap.basepage  = ((uint32_t)payload[8] << 16) | payload[9];
      snap.bank0_kb  = payload[10];
      snap.bank1_kb  = payload[11];
      uint32_t now_ms = (uint32_t)to_ms_since_boot(get_absolute_time());
      DPRINTF("Runner: MEMINFO done, phystop=0x%08lX, banks=%u/%u kB\n",
              (unsigned long)snap.phystop, (unsigned)snap.bank0_kb,
              (unsigned)snap.bank1_kb);
      emul_recordRunnerMeminfoDone(&snap, now_ms);
      SEND_COMMAND_TO_DISPLAY(0);
      return;
    }
    case RUNNER_ADV_CMD_DONE_JUMP: {
      // m68k VBL handler is about to rte to a user-supplied address.
      // Clear the sentinel so subsequent VBLs see NOP and just chain
      // — without this, every next ISR would re-read RUNNER_ADV_CMD_JUMP
      // and re-jump in an infinite loop.
      DPRINTF("Runner: ADV JUMP done — clearing sentinel\n");
      SEND_COMMAND_TO_DISPLAY(0);
      return;
    }
    case RUNNER_ADV_CMD_DONE_LOAD_CHUNK: {
      // The m68k VBL handler finished copying the current chunk
      // out of APP_FREE into RAM. Clear the sentinel and flip the
      // chunk-ack flag the streaming HTTP handler is spinning on.
      DPRINTF("Runner: ADV LOAD CHUNK done\n");
      SEND_COMMAND_TO_DISPLAY(0);
      emul_recordRunnerAdvLoadAck();
      return;
    }
    case RUNNER_CMD_DONE_HELLO: {
      // d3 payload layout (Epic 04 / S1+S4):
      //   bit 0      : advanced installed
      //   bits 8..15 : hook_vector_id (0=vbl, 1=etv_timer, 0xFF=unknown)
      // Older firmwares without S1 send HELLO with size 0; d3 reads
      // as 0 → installed=false, hook_vector_id=0 (the default we
      // overwrite to UNKNOWN below if installed is false).
      // emul_resetRunnerSession() clears both fields, so the order
      // matters: reset first, then record.
      uint32_t flags = TPROTO_GET_PAYLOAD_PARAM32(payload);
      bool adv = (flags & 1u) != 0;
      uint8_t vec_id = adv
                           ? (uint8_t)((flags >> 8) & 0xFFu)
                           : (uint8_t)RUNNER_HOOK_VECTOR_UNKNOWN;
      DPRINTF("Runner: HELLO — clearing session state, advanced=%d, hook_vec=%u\n",
              adv ? 1 : 0, (unsigned)vec_id);
      emul_resetRunnerSession();
      emul_recordRunnerAdvancedInstalled(adv);
      emul_recordRunnerAdvHookVector(vec_id);
      // The runner is confirmed back on the air. Cancel any pending
      // relaunch retry storm and clear the sentinel back to NOP so a
      // late-firing CMD_START_RUNNER write doesn't get re-read by the
      // m68k poll loop and re-enter runner_entry recursively.
      emul_scheduleRunnerRelaunch(0);
      SEND_COMMAND_TO_DISPLAY(0);
      return;
    }
    default:
      // Not ours — chandler dispatches to every registered callback.
      return;
  }
}

void runner_init(void) {
  chandler_addCB(runner_command_cb);
  DPRINTF("Runner: chandler callback registered\n");
}
