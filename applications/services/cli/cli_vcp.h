/**
 * @file cli_vcp.h
 * VCP HAL API
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECORD_CLI_VCP "cli_vcp"

typedef struct CliVcp CliVcp;

void cli_vcp_enable(CliVcp* cli_vcp);
void cli_vcp_disable(CliVcp* cli_vcp);

/* Active-recon capture: while a capture is open, bytes the host writes to the
 * CDC serial are diverted from the CLI into a buffer instead. This is what lets
 * the Flipper type a recon command as an HID keyboard and read the command's
 * output back over its own serial ("Mr Robot" loop). Begin, read for a while,
 * then End -- the CLI resumes untouched. Not reentrant: one capture at a time. */
void cli_vcp_capture_begin(void);
size_t cli_vcp_capture_read(uint8_t* buffer, size_t size, uint32_t timeout_ms);
void cli_vcp_capture_end(void);
#ifdef __cplusplus
}
#endif
