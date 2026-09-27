#ifndef PROSENJIT_ABI_CAPABILITY_H
#define PROSENJIT_ABI_CAPABILITY_H

#include <stdint.h>

/*
 * Capability rights are independent bits so a handle can carry only the
 * authority required by an operation. These values are part of the ABI.
 */
#define PROSENJIT_CAP_READ       (1ULL << 0)
#define PROSENJIT_CAP_WRITE      (1ULL << 1)
#define PROSENJIT_CAP_EXECUTE    (1ULL << 2)
#define PROSENJIT_CAP_DUPLICATE  (1ULL << 3)
#define PROSENJIT_CAP_TRANSFER   (1ULL << 4)
#define PROSENJIT_CAP_ADMIN      (1ULL << 5)

#define PROSENJIT_CAP_RIGHTS_ALL     (PROSENJIT_CAP_READ | PROSENJIT_CAP_WRITE | PROSENJIT_CAP_EXECUTE |      PROSENJIT_CAP_DUPLICATE | PROSENJIT_CAP_TRANSFER | PROSENJIT_CAP_ADMIN)

enum pros_object_type {
    PROSENJIT_OBJECT_NONE = 0,
    PROSENJIT_OBJECT_PROCESS = 1,
    PROSENJIT_OBJECT_THREAD = 2,
    PROSENJIT_OBJECT_MEMORY = 3,
    PROSENJIT_OBJECT_FILE = 4,
    PROSENJIT_OBJECT_DEVICE = 5,
    PROSENJIT_OBJECT_SOCKET = 6,
    PROSENJIT_OBJECT_IPC = 7,
};

#endif
