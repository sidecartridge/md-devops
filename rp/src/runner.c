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
    case RUNNER_CMD_DONE_CD: {
      uint32_t raw = TPROTO_GET_PAYLOAD_PARAM32(payload);
      int32_t errnum = (int32_t)raw;
      uint32_t now_ms = (uint32_t)to_ms_since_boot(get_absolute_time());
      DPRINTF("Runner: CD done, errno=%ld\n", (long)errnum);
      emul_recordRunnerCdDone(errnum, now_ms);
      SEND_COMMAND_TO_DISPLAY(0);
      return;
    }
    case RUNNER_CMD_DONE_HELLO: {
      DPRINTF("Runner: HELLO — clearing session state\n");
      emul_resetRunnerSession();
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
