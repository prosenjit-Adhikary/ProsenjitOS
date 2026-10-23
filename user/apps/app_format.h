#ifndef PROS_USER_APP_FORMAT_H
#define PROS_USER_APP_FORMAT_H

#include <stdint.h>

enum pros_app_file_format {
    PROS_APP_FORMAT_UNKNOWN = 0,
    PROS_APP_FORMAT_ELF32 = 1,
    PROS_APP_FORMAT_ELF64 = 2,
    PROS_APP_FORMAT_PE32 = 3,
    PROS_APP_FORMAT_PE32_PLUS = 4,
    PROS_APP_FORMAT_OLE_CONTAINER = 5
};

/*
 * Classifies bytes, never filenames or extensions. OLE_CONTAINER is not proof
 * that a file is an MSI package. This detector does not authorize execution,
 * validate signatures, load code, or establish application compatibility.
 *
 * Returns 0 for a recognized or unknown format, -1 for invalid arguments,
 * and -2 when a recognized magic prefix has a malformed/truncated header.
 */
int pros_app_detect_format(const uint8_t *data,
                           uint32_t length,
                           enum pros_app_file_format *format_out);

/* Format-family check only; this does not prove ABI/runtime compatibility. */
int pros_app_format_compatible_with_backend(uint32_t backend,
                                            enum pros_app_file_format format);

/*
 * Detect bytes and enforce the backend's executable format family.
 * Returns 0 on family match, -1 for invalid arguments, -2 for malformed
 * recognized headers, and -3 for unknown formats or backend mismatch.
 * This is not full binary validation or permission to execute.
 */
int pros_app_check_binary_backend(uint32_t backend,
                                  const uint8_t *data,
                                  uint32_t length,
                                  enum pros_app_file_format *format_out);

#endif
