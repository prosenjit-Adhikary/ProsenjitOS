#include "driver_compat.h"

static void compat_zero_digest(uint8_t digest[PROS_FILE_SHA256_BYTES])
{
    uint32_t i;
    for (i = 0; i < PROS_FILE_SHA256_BYTES; ++i) digest[i] = 0;
}

static struct pros_compat_package packages[PROS_DRIVER_COMPAT_MAX_PACKAGES];
static struct pros_driver_id package_ids[PROS_DRIVER_COMPAT_MAX_PACKAGES]
                                         [PROS_DRIVER_COMPAT_MAX_IDS];
static uint32_t package_count;

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
    return -1;
}

static int parse_hex4(const char *p, uint16_t *out)
{
    uint32_t i;
    uint16_t v = 0;
    for (i = 0; i < 4; ++i) {
        int n = hex_value(p[i]);
        if (n < 0) return 0;
        v = (uint16_t)((v << 4) | (uint16_t)n);
    }
    *out = v;
    return 1;
}

static int ci_match4(const char *p, const char *tag)
{
    uint32_t i;
    for (i = 0; i < 4; ++i) {
        char a = p[i], b = tag[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return 0;
    }
    return 1;
}

static int ci_contains(const char *text, uint32_t len, const char *needle)
{
    uint32_t i, j, n = 0;
    while (needle[n]) ++n;
    if (!n || n > len) return 0;
    for (i = 0; i + n <= len; ++i) {
        for (j = 0; j < n; ++j) {
            char a = text[i + j], b = needle[j];
            if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
            if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
            if (a != b) break;
        }
        if (j == n) return 1;
    }
    return 0;
}

static void copy_inf_value(char *out, uint32_t cap, const char *text, uint32_t len,
                              const char *key)
{
    uint32_t i, k = 0, start = 0, end = 0;
    if (!out || !cap || !text || !key) return;
    out[0] = 0;
    while (key[k]) ++k;
    for (i = 0; i + k < len; ++i) {
        uint32_t p = i;
        while (p < len && (text[p] == ' ' || text[p] == '\t')) ++p;
        if (p + k < len && ci_contains(&text[p], k, key)) {
            uint32_t q = p + k;
            while (q < len && (text[q] == ' ' || text[q] == '\t')) ++q;
            if (q < len && text[q] == '=') {
                start = q + 1u;
                while (start < len && (text[start] == ' ' || text[start] == '\t')) ++start;
                end = start;
                while (end < len && text[end] != '\r' && text[end] != '\n') ++end;
                while (end > start && (text[end-1u] == ' ' || text[end-1u] == '\t')) --end;
                {
                    uint32_t n = end - start;
                    if (n >= cap) n = cap - 1u;
                    for (uint32_t j = 0; j < n; ++j) out[j] = text[start+j];
                    out[n] = 0;
                }
                return;
            }
        }
    }
}

static enum pros_compat_driver_model detect_model(const char *text, uint32_t len)
{
    if (ci_contains(text, len, "STORPORT")) return PROS_COMPAT_MODEL_STORPORT;
    if (ci_contains(text, len, "NDIS")) return PROS_COMPAT_MODEL_NDIS;
    if (ci_contains(text, len, "KMDF")) return PROS_COMPAT_MODEL_KMDF;
    if (ci_contains(text, len, "USB")) return PROS_COMPAT_MODEL_USB;
    return PROS_COMPAT_MODEL_WDM;
}

static int valid_model(enum pros_compat_driver_model model)
{
    return model == PROS_COMPAT_MODEL_WDM ||
           model == PROS_COMPAT_MODEL_KMDF ||
           model == PROS_COMPAT_MODEL_NDIS ||
           model == PROS_COMPAT_MODEL_STORPORT ||
           model == PROS_COMPAT_MODEL_USB;
}

static int valid_type(enum pros_compat_package_type type)
{
    return type >= PROS_COMPAT_PACKAGE_INF &&
           type <= PROS_COMPAT_PACKAGE_EXE;
}

int pros_driver_compat_init(void)
{
    uint32_t i;
    package_count = 0;
    for (i = 0; i < PROS_DRIVER_COMPAT_MAX_PACKAGES; ++i) {
        packages[i].id = 0;
        packages[i].status = PROS_COMPAT_STATUS_EMPTY;
        compat_zero_digest(packages[i].content_sha256);
        packages[i].content_sha256_valid = 0;
        packages[i].trust_evidence = PROS_COMPAT_EVIDENCE_NONE;
    }
    return PROS_DRIVER_OK;
}

int pros_driver_compat_supports_model(enum pros_compat_driver_model model)
{
    /*
     * The compatibility ABI is intentionally staged.  Registration and
     * metadata inspection are available first; executable NT/WDM support is
     * not claimed until its runtime exists.
     */
    return valid_model(model) ? PROS_DRIVER_ENOTSUP : PROS_DRIVER_EINVAL;
}

int pros_driver_compat_register_package(
    enum pros_compat_package_type type,
    enum pros_compat_driver_model model,
    const char *name,
    const struct pros_driver_id *ids,
    uint32_t id_count,
    uint32_t flags,
    uint32_t *package_id_out)
{
    struct pros_compat_package *pkg;
    uint32_t i;

    if (!valid_type(type) || !valid_model(model) || !name ||
        !ids || id_count == 0 || id_count > PROS_DRIVER_COMPAT_MAX_IDS ||
        !package_id_out) {
        return PROS_DRIVER_EINVAL;
    }
    if (package_count >= PROS_DRIVER_COMPAT_MAX_PACKAGES) {
        return PROS_DRIVER_ENOSPC;
    }

    pkg = &packages[package_count];
    compat_zero_digest(pkg->content_sha256);
    pkg->content_sha256_valid = 0;
    pkg->id = package_count + 1u;
    pkg->type = type;
    pkg->model = model;
    pkg->status = PROS_COMPAT_STATUS_INSPECTED;
    pkg->flags = flags;
    pkg->tx_state = PROS_COMPAT_TX_IDLE;
    pkg->trust = (flags & PROS_COMPAT_FLAG_SIGNED) ?
        PROS_COMPAT_TRUST_UNKNOWN : PROS_COMPAT_TRUST_UNSIGNED;
    pkg->trust_evidence = PROS_COMPAT_EVIDENCE_NONE;
    pkg->dependency_count = 0;
    pkg->vendor_id = ids[0].vendor_id;
    pkg->device_id = ids[0].device_id;
    pkg->class_code = ids[0].class_code;
    pkg->subclass = ids[0].subclass;
    pkg->prog_if = ids[0].prog_if;
    pkg->id_count = id_count;
    pkg->name = name;
    pkg->provider[0] = 0;
    pkg->driver_ver[0] = 0;
    pkg->class_name[0] = 0;

    for (i = 0; i < id_count; ++i)
        package_ids[package_count][i] = ids[i];

    ++package_count;
    *package_id_out = pkg->id;
    return PROS_DRIVER_OK;
}

int pros_driver_compat_hash_content(
    uint32_t package_id, const uint8_t *data, uint64_t length)
{
    struct pros_compat_package *pkg;
    uint8_t digest[PROS_FILE_SHA256_BYTES];
    uint32_t i;

    if (package_id == 0 || package_id > package_count ||
        (length != 0 && !data))
        return PROS_DRIVER_EINVAL;

    pkg = &packages[package_id - 1u];

    /*
     * Re-hashing is a new content identity.  Never leave trust or an
     * admission decision attached to the previous bytes.
     */
    pros_sha256(data, length, digest);

    if (pkg->content_sha256_valid) {
        for (i = 0; i < PROS_FILE_SHA256_BYTES; ++i)
            if (pkg->content_sha256[i] != digest[i])
                break;
        if (i != PROS_FILE_SHA256_BYTES) {
            pkg->trust = PROS_COMPAT_TRUST_UNKNOWN;
            pkg->trust_evidence = PROS_COMPAT_EVIDENCE_NONE;
            pkg->tx_state = PROS_COMPAT_TX_IDLE;
            if (pkg->status != PROS_COMPAT_STATUS_REJECTED)
                pkg->status = PROS_COMPAT_STATUS_INSPECTED;
        }
    }

    for (i = 0; i < PROS_FILE_SHA256_BYTES; ++i)
        pkg->content_sha256[i] = digest[i];
    pkg->content_sha256_valid = 1u;
    return PROS_DRIVER_OK;
}

int pros_driver_compat_get_package(uint32_t package_id,
                                   struct pros_compat_package *out)
{
    if (!out || package_id == 0 || package_id > package_count)
        return PROS_DRIVER_EINVAL;
    *out = packages[package_id - 1u];
    return PROS_DRIVER_OK;
}

int pros_driver_compat_inspect_inf(const char *name, const char *text,
                                    uint32_t text_len, uint32_t *package_id_out)
{
    struct pros_driver_id ids[PROS_DRIVER_COMPAT_MAX_IDS];
    uint32_t count = 0, i;
    enum pros_compat_driver_model model;

    if (!name || !text || !text_len || !package_id_out)
        return PROS_DRIVER_EINVAL;

    for (i = 0; i + 21u <= text_len && count < PROS_DRIVER_COMPAT_MAX_IDS; ++i) {
        uint16_t vendor, device;
        if (!ci_match4(&text[i], "PCI\\")) continue;
        if (text[i + 4u] != 'V' && text[i + 4u] != 'v') continue;
        if (text[i + 5u] != 'E' && text[i + 5u] != 'e') continue;
        if (text[i + 6u] != 'N' && text[i + 6u] != 'n') continue;
        if (text[i + 7u] != '_') continue;
        if (!parse_hex4(&text[i + 8u], &vendor)) continue;
        if (text[i + 12u] != '&') continue;
        if (text[i + 13u] != 'D' && text[i + 13u] != 'd') continue;
        if (text[i + 14u] != 'E' && text[i + 14u] != 'e') continue;
        if (text[i + 15u] != 'V' && text[i + 15u] != 'v') continue;
        if (text[i + 16u] != '_') continue;
        if (!parse_hex4(&text[i + 17u], &device)) continue;
        if (count && ids[count - 1u].vendor_id == vendor &&
            ids[count - 1u].device_id == device) continue;
        ids[count].bus = PROS_DRIVER_BUS_PCI;
        ids[count].vendor_id = vendor;
        ids[count].device_id = device;
        ids[count].class_code = 0;
        ids[count].subclass = 0;
        ids[count].prog_if = 0;
        ++count;
    }
    if (!count) return PROS_DRIVER_ENODEV;
    model = detect_model(text, text_len);
    int rc = pros_driver_compat_register_package(
        PROS_COMPAT_PACKAGE_INF, model, name, ids, count, 0, package_id_out);
    if (rc != PROS_DRIVER_OK) return rc;
    copy_inf_value(packages[*package_id_out - 1u].class_name,
                   PROS_DRIVER_COMPAT_MAX_CLASS, text, text_len, "Class");
    copy_inf_value(packages[*package_id_out - 1u].provider,
                   PROS_DRIVER_COMPAT_MAX_PROVIDER, text, text_len, "Provider");
    copy_inf_value(packages[*package_id_out - 1u].driver_ver,
                   PROS_DRIVER_COMPAT_MAX_DRIVER_VER, text, text_len, "DriverVer");
    return PROS_DRIVER_OK;
}

int pros_driver_compat_set_trust(uint32_t package_id, enum pros_compat_trust trust)
{
    struct pros_compat_package *pkg;

    if (package_id == 0 || package_id > package_count)
        return PROS_DRIVER_EINVAL;
    if (trust > PROS_COMPAT_TRUST_REJECTED)
        return PROS_DRIVER_EINVAL;

    pkg = &packages[package_id - 1u];

    /*
     * VERIFIED is a conclusion backed by evidence, not a caller-controlled
     * state.  Cryptographic signature evidence remains unavailable until a
     * real verifier is implemented.
     */
    /*
     * VERIFIED may only be established by a dedicated trust authority
     * (currently bind_admission(), and later the cryptographic verifier).
     * This setter may only express downgrade/rejection decisions.
     */
    if (trust == PROS_COMPAT_TRUST_VERIFIED)
        return PROS_DRIVER_EPERM;

    pkg->trust = trust;
    if (trust == PROS_COMPAT_TRUST_REJECTED) {
        pkg->status = PROS_COMPAT_STATUS_REJECTED;
        pkg->tx_state = PROS_COMPAT_TX_ROLLED_BACK;
    } else if (pkg->status == PROS_COMPAT_STATUS_REJECTED) {
        pkg->status = PROS_COMPAT_STATUS_INSPECTED;
        pkg->tx_state = PROS_COMPAT_TX_IDLE;
    }

    /*
     * Any trust downgrade invalidates an in-flight transaction so previously
     * verified/staged state cannot survive a policy change.
     */
    if (trust != PROS_COMPAT_TRUST_VERIFIED &&
        trust != PROS_COMPAT_TRUST_REJECTED) {
        pkg->tx_state = PROS_COMPAT_TX_IDLE;
    }
    return PROS_DRIVER_OK;
}

int pros_driver_compat_set_trust_evidence(
    uint32_t package_id, enum pros_compat_trust_evidence evidence)
{
    if (package_id == 0 || package_id > package_count)
        return PROS_DRIVER_EINVAL;
    if (evidence > PROS_COMPAT_EVIDENCE_TEST_ONLY)
        return PROS_DRIVER_EINVAL;
    if (evidence == PROS_COMPAT_EVIDENCE_CRYPTO_SIGNATURE)
        return PROS_DRIVER_ENOTSUP;
    /*
     * Admission evidence is established only by bind_admission(), where the
     * exact package digest is checked against the admission record.
     */
    if (evidence == PROS_COMPAT_EVIDENCE_ADMISSION_POLICY)
        return PROS_DRIVER_EPERM;
    if (packages[package_id - 1u].status == PROS_COMPAT_STATUS_REJECTED)
        return PROS_DRIVER_EPERM;
    if (packages[package_id - 1u].trust_evidence == evidence) {
        /*
         * Even a repeated policy write must not revive a stale transaction.
         * No state change is needed when the evidence is already identical.
         */
        return PROS_DRIVER_OK;
    }

    /*
     * Evidence is part of the trust decision.  Changing it invalidates any
     * previous VERIFIED/staged state and requires a fresh transaction.
     */
    packages[package_id - 1u].trust_evidence = evidence;
    packages[package_id - 1u].trust = PROS_COMPAT_TRUST_UNKNOWN;
    packages[package_id - 1u].tx_state = PROS_COMPAT_TX_IDLE;
    packages[package_id - 1u].status = PROS_COMPAT_STATUS_INSPECTED;
    return PROS_DRIVER_OK;
}

int pros_driver_compat_bind_admission(
    uint32_t package_id, const struct pros_file_admission_record *record)
{
    uint32_t i;

    if (package_id == 0 || package_id > package_count || !record)
        return PROS_DRIVER_EINVAL;
    if (!packages[package_id - 1u].content_sha256_valid)
        return PROS_DRIVER_EPERM;
    if (packages[package_id - 1u].status == PROS_COMPAT_STATUS_REJECTED)
        return PROS_DRIVER_EPERM;
    if (packages[package_id - 1u].tx_state != PROS_COMPAT_TX_IDLE &&
        packages[package_id - 1u].tx_state != PROS_COMPAT_TX_ROLLED_BACK)
        return PROS_DRIVER_EBUSY;
    if (record->security.action != PROS_FILE_ALLOW)
        return PROS_DRIVER_EPERM;

    /*
     * Admission is bound to the exact package bytes by SHA-256.  This is
     * policy/admission evidence only; it does not turn caller-provided
     * signature metadata into cryptographic signature verification.
     */
    for (i = 0; i < PROS_FILE_SHA256_BYTES; ++i) {
        if (packages[package_id - 1u].content_sha256[i] != record->sha256[i])
            return PROS_DRIVER_EPERM;
    }

    packages[package_id - 1u].trust = PROS_COMPAT_TRUST_VERIFIED;
    packages[package_id - 1u].trust_evidence =
        PROS_COMPAT_EVIDENCE_ADMISSION_POLICY;
    packages[package_id - 1u].tx_state = PROS_COMPAT_TX_IDLE;
    packages[package_id - 1u].status = PROS_COMPAT_STATUS_INSPECTED;
    return PROS_DRIVER_OK;
}

int pros_driver_compat_add_dependency(uint32_t package_id, uint32_t dependency_id)
{
    struct pros_compat_package *pkg;
    uint32_t i;
    if (package_id == 0 || package_id > package_count ||
        dependency_id == 0 || dependency_id > package_count)
        return PROS_DRIVER_EINVAL;
    pkg = &packages[package_id - 1u];
    if (pkg->status == PROS_COMPAT_STATUS_REJECTED)
        return PROS_DRIVER_EPERM;
    if (pkg->tx_state != PROS_COMPAT_TX_IDLE &&
        pkg->tx_state != PROS_COMPAT_TX_ROLLED_BACK)
        return PROS_DRIVER_EBUSY;
    if (pkg->dependency_count >= PROS_DRIVER_COMPAT_MAX_DEPS)
        return PROS_DRIVER_ENOSPC;
    for (i = 0; i < pkg->dependency_count; ++i)
        if (pkg->dependencies[i] == dependency_id)
            return PROS_DRIVER_OK;
    pkg->dependencies[pkg->dependency_count++] = dependency_id;

    /* Dependency metadata changes invalidate prior trust/transaction state. */
    pkg->trust = PROS_COMPAT_TRUST_UNKNOWN;
    pkg->trust_evidence = PROS_COMPAT_EVIDENCE_NONE;
    pkg->tx_state = PROS_COMPAT_TX_IDLE;
    if (pkg->status != PROS_COMPAT_STATUS_REJECTED)
        pkg->status = PROS_COMPAT_STATUS_INSPECTED;
    return PROS_DRIVER_OK;
}

int pros_driver_compat_begin(uint32_t package_id)
{
    if (package_id == 0 || package_id > package_count) return PROS_DRIVER_EINVAL;
    if (packages[package_id - 1u].status == PROS_COMPAT_STATUS_REJECTED)
        return PROS_DRIVER_EPERM;
    if (packages[package_id - 1u].status == PROS_COMPAT_STATUS_EMPTY)
        return PROS_DRIVER_ENODEV;
    if (packages[package_id - 1u].tx_state != PROS_COMPAT_TX_IDLE &&
        packages[package_id - 1u].tx_state != PROS_COMPAT_TX_ROLLED_BACK)
        return PROS_DRIVER_EBUSY;
    packages[package_id - 1u].tx_state = PROS_COMPAT_TX_INSPECTED;
    return PROS_DRIVER_OK;
}

static int compat_dependency_trust_check(uint32_t package_id)
{
    uint8_t seen[PROS_DRIVER_COMPAT_MAX_PACKAGES];
    uint32_t stack[PROS_DRIVER_COMPAT_MAX_PACKAGES];
    uint32_t i, top = 0;

    for (i = 0; i < package_count; ++i)
        seen[i] = 0;

    stack[top++] = package_id;
    while (top) {
        uint32_t id = stack[--top];
        struct pros_compat_package *pkg;

        if (id == 0 || id > package_count)
            return PROS_DRIVER_EINVAL;
        if (seen[id - 1u])
            continue;
        seen[id - 1u] = 1u;

        pkg = &packages[id - 1u];
        if (pkg->status == PROS_COMPAT_STATUS_REJECTED ||
            pkg->trust == PROS_COMPAT_TRUST_REJECTED)
            return PROS_DRIVER_EPERM;
        if (pkg->status == PROS_COMPAT_STATUS_EMPTY)
            return PROS_DRIVER_ENODEV;
        if (pkg->trust != PROS_COMPAT_TRUST_VERIFIED ||
            pkg->trust_evidence == PROS_COMPAT_EVIDENCE_NONE)
            return PROS_DRIVER_EPERM;

        for (i = 0; i < pkg->dependency_count; ++i) {
            uint32_t dep = pkg->dependencies[i];
            if (dep == 0 || dep > package_count)
                return PROS_DRIVER_EINVAL;
            if (seen[dep - 1u])
                continue;
            if (top >= PROS_DRIVER_COMPAT_MAX_PACKAGES)
                return PROS_DRIVER_ENOSPC;
            stack[top++] = dep;
        }
    }
    return PROS_DRIVER_OK;
}

int pros_driver_compat_verify(uint32_t package_id)
{
    struct pros_compat_package *p;
    if (package_id == 0 || package_id > package_count) return PROS_DRIVER_EINVAL;
    p = &packages[package_id - 1u];
    if (p->tx_state != PROS_COMPAT_TX_INSPECTED) return PROS_DRIVER_EPERM;
    if (p->status == PROS_COMPAT_STATUS_REJECTED ||
        p->trust == PROS_COMPAT_TRUST_REJECTED)
        return PROS_DRIVER_EPERM;
    if (p->trust != PROS_COMPAT_TRUST_VERIFIED) return PROS_DRIVER_EPERM;
    if (p->trust_evidence == PROS_COMPAT_EVIDENCE_NONE)
        return PROS_DRIVER_EPERM;
    if (pros_driver_compat_dependency_check(package_id) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    if (compat_dependency_trust_check(package_id) != PROS_DRIVER_OK)
        return PROS_DRIVER_EPERM;
    p->tx_state = PROS_COMPAT_TX_VERIFIED;
    return PROS_DRIVER_OK;
}

int pros_driver_compat_stage(uint32_t package_id)
{
    if (package_id == 0 || package_id > package_count) return PROS_DRIVER_EINVAL;
    if (packages[package_id - 1u].tx_state != PROS_COMPAT_TX_VERIFIED)
        return PROS_DRIVER_EPERM;
    packages[package_id - 1u].tx_state = PROS_COMPAT_TX_STAGED;
    return PROS_DRIVER_OK;
}

int pros_driver_compat_health_check(uint32_t package_id)
{
    if (package_id == 0 || package_id > package_count) return PROS_DRIVER_EINVAL;
    if (packages[package_id - 1u].tx_state != PROS_COMPAT_TX_STAGED)
        return PROS_DRIVER_EPERM;
    /* Runtime support is not yet claimed; staging can be validated only as metadata. */
    return PROS_DRIVER_ENOTSUP;
}

int pros_driver_compat_commit(uint32_t package_id)
{
    if (package_id == 0 || package_id > package_count) return PROS_DRIVER_EINVAL;
    if (packages[package_id - 1u].tx_state != PROS_COMPAT_TX_HEALTHY)
        return PROS_DRIVER_EPERM;
    packages[package_id - 1u].status = PROS_COMPAT_STATUS_SUPPORTED;
    packages[package_id - 1u].tx_state = PROS_COMPAT_TX_COMMITTED;
    return PROS_DRIVER_OK;
}

int pros_driver_compat_rollback(uint32_t package_id)
{
    struct pros_compat_package *pkg;

    if (package_id == 0 || package_id > package_count)
        return PROS_DRIVER_EINVAL;

    pkg = &packages[package_id - 1u];

    /*
     * Rollback is a security boundary, not only a transaction-state change.
     * A rolled-back package must not retain admission-backed trust, because
     * a later begin()/verify() sequence must require fresh admission.
     */
    pkg->trust = PROS_COMPAT_TRUST_UNKNOWN;
    pkg->trust_evidence = PROS_COMPAT_EVIDENCE_NONE;
    pkg->tx_state = PROS_COMPAT_TX_ROLLED_BACK;
    pkg->status = PROS_COMPAT_STATUS_INSPECTED;
    return PROS_DRIVER_OK;
}

int pros_driver_compat_dependency_check(uint32_t package_id)
{
    struct pros_dep_frame {
        uint32_t id;
        uint32_t next_dep;
    };
    uint8_t state[PROS_DRIVER_COMPAT_MAX_PACKAGES];
    struct pros_dep_frame stack[PROS_DRIVER_COMPAT_MAX_PACKAGES];
    uint32_t i, top = 0;

    if (package_id == 0 || package_id > package_count)
        return PROS_DRIVER_EINVAL;
    for (i = 0; i < package_count; ++i) state[i] = 0;

    stack[top].id = package_id;
    stack[top].next_dep = 0;
    state[package_id - 1u] = 1u;
    ++top;

    while (top) {
        struct pros_dep_frame *frame = &stack[top - 1u];
        struct pros_compat_package *pkg = &packages[frame->id - 1u];

        if (frame->next_dep >= pkg->dependency_count) {
            state[frame->id - 1u] = 2u;
            --top;
            continue;
        }

        {
            uint32_t dep = pkg->dependencies[frame->next_dep++];
            if (dep == 0 || dep > package_count)
                return PROS_DRIVER_EINVAL;
            if (dep == frame->id)
                return PROS_DRIVER_EIO;
            if (packages[dep - 1u].status == PROS_COMPAT_STATUS_REJECTED ||
                packages[dep - 1u].trust == PROS_COMPAT_TRUST_REJECTED)
                return PROS_DRIVER_EPERM;
            if (packages[dep - 1u].status == PROS_COMPAT_STATUS_EMPTY)
                return PROS_DRIVER_ENODEV;
            if (state[dep - 1u] == 1u)
                return PROS_DRIVER_EIO;
            if (state[dep - 1u] == 2u)
                continue;
            if (top >= PROS_DRIVER_COMPAT_MAX_PACKAGES)
                return PROS_DRIVER_ENOSPC;
            state[dep - 1u] = 1u;
            stack[top].id = dep;
            stack[top].next_dep = 0;
            ++top;
        }
    }
    return PROS_DRIVER_OK;
}

int pros_driver_compat_match(uint32_t package_id,
                             const struct pros_driver_id *hwid,
                             int *score_out)
{
    uint32_t i;
    int score;
    if (!hwid || !score_out || package_id == 0 || package_id > package_count)
        return PROS_DRIVER_EINVAL;
    if (packages[package_id - 1u].status == PROS_COMPAT_STATUS_REJECTED ||
        packages[package_id - 1u].trust == PROS_COMPAT_TRUST_REJECTED)
        return PROS_DRIVER_ENODEV;
    /*
     * Hardware matching is an executable selection boundary.  Do not expose
     * an unverified compatibility package as a selectable driver.
     */
    if (packages[package_id - 1u].trust != PROS_COMPAT_TRUST_VERIFIED ||
        packages[package_id - 1u].trust_evidence ==
            PROS_COMPAT_EVIDENCE_NONE)
        return PROS_DRIVER_ENODEV;
    /*
     * A verified package is selectable only while its current transaction
     * has reached an active verification/runtime stage.  ROLLED_BACK and
     * IDLE/INSPECTED trust metadata must never resurrect driver selection.
     */
    if (packages[package_id - 1u].tx_state != PROS_COMPAT_TX_VERIFIED &&
        packages[package_id - 1u].tx_state != PROS_COMPAT_TX_STAGED &&
        packages[package_id - 1u].tx_state != PROS_COMPAT_TX_HEALTHY &&
        packages[package_id - 1u].tx_state != PROS_COMPAT_TX_COMMITTED)
        return PROS_DRIVER_ENODEV;
    for (i = 0; i < packages[package_id - 1u].id_count; ++i) {
        const struct pros_driver_id *id = &package_ids[package_id - 1u][i];
        if (id->bus != PROS_DRIVER_BUS_NONE && id->bus != hwid->bus) continue;
        if (id->vendor_id != 0 && id->vendor_id != hwid->vendor_id) continue;
        if (id->device_id != 0 && id->device_id != hwid->device_id) continue;
        if (id->class_code != 0 && id->class_code != hwid->class_code) continue;
        if (id->subclass != 0 && id->subclass != hwid->subclass) continue;
        if (id->prog_if != 0 && id->prog_if != hwid->prog_if) continue;
        score = 1;
        if (id->vendor_id) score += 64;
        if (id->device_id) score += 64;
        if (id->class_code) score += 16;
        if (id->subclass) score += 8;
        if (id->prog_if) score += 4;
        if (id->bus != PROS_DRIVER_BUS_NONE) score += 2;
        if (packages[package_id - 1u].trust == PROS_COMPAT_TRUST_VERIFIED)
            score += 1;
        *score_out = score;
        return PROS_DRIVER_OK;
    }
    return PROS_DRIVER_ENODEV;
}

int pros_driver_compat_get_id(uint32_t package_id, uint32_t index,
                              struct pros_driver_id *out)
{
    if (!out || package_id == 0 || package_id > package_count)
        return PROS_DRIVER_EINVAL;
    if (index >= packages[package_id - 1u].id_count)
        return PROS_DRIVER_ENODEV;
    *out = package_ids[package_id - 1u][index];
    return PROS_DRIVER_OK;
}

static int compat_selftest_admit(uint32_t package_id,
                                  const uint8_t *data, uint64_t length)
{
    struct pros_file_admission_input input;
    struct pros_file_admission_record record;

    input.data = data;
    input.length = length;
    input.metadata.format_valid = 1u;
    input.metadata.signature_valid = 0u;
    input.metadata.signature_present = 0u;
    input.metadata.local_trusted = 1u;
    input.metadata.local_denied = 0u;
    input.metadata.reputation = PROS_FILE_REPUTATION_CLEAN;
    input.metadata.provenance_known = 1u;

    if (pros_driver_compat_hash_content(package_id, data, length)
            != PROS_DRIVER_OK ||
        pros_file_admission_evaluate(&input, &record) != 0 ||
        pros_driver_compat_bind_admission(package_id, &record)
            != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    return PROS_DRIVER_OK;
}

int pros_driver_compat_selftest(void)
{
    static const char inf[] =
        "[Version]\r\n"
        "Class=Net\r\n"
        "Provider=Prosenjit Test Vendor\r\n"
        "DriverVer=09/30/2026,1.2.3\r\n"
        "[Models]\r\n"
        "%Mfg%=Install, PCI\\VEN_1234&DEV_5678\r\n"
        "%Mfg%=Install, PCI\\VEN_ABCD&DEV_0001\r\n"
        "NDIS=1\r\n";
    struct pros_compat_package pkg;
    struct pros_driver_id id;
    uint32_t package_id = 0;

    if (pros_driver_compat_init() != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    /*
     * Signature metadata must remain fail-closed until a real verifier
     * establishes cryptographic evidence.
     */
    {
        static const uint8_t sig_data[] = { 's' };
        static const uint8_t sig[] = { 0x01 };
        static const uint8_t key[] = { 0x02 };
        enum pros_signature_result sig_result;
        if (pros_security_signature_verify(
                PROS_SIGNATURE_ALGORITHM_ED25519,
                sig_data, sizeof(sig_data),
                sig, sizeof(sig), key, sizeof(key), &sig_result) != 0 ||
            sig_result != PROS_SIGNATURE_RESULT_INVALID)
            return PROS_DRIVER_EIO;
    }
    if (pros_driver_compat_inspect_inf(
            "compat-selftest.inf", inf, (uint32_t)(sizeof(inf) - 1u),
            &package_id) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    if (pros_driver_compat_get_package(package_id, &pkg) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    if (pkg.status != PROS_COMPAT_STATUS_INSPECTED ||
        pkg.id_count != 2u ||
        pkg.vendor_id != 0x1234 || pkg.device_id != 0x5678 ||
        pkg.model != PROS_COMPAT_MODEL_NDIS ||
        pkg.provider[0] == 0 || pkg.driver_ver[0] == 0 || pkg.class_name[0] == 0)
        return PROS_DRIVER_EIO;

    /*
     * Package digest is identity/integrity metadata.  Compute it from the
     * inspected package bytes so the self-test does not trust an arbitrary
     * caller-supplied digest.
     */
    {
        uint8_t digest[PROS_FILE_SHA256_BYTES];
        struct pros_compat_package hashed_pkg;

        pros_sha256((const uint8_t *)inf, (uint64_t)(sizeof(inf) - 1u), digest);
        if (pros_driver_compat_hash_content(
                package_id, (const uint8_t *)inf,
                (uint64_t)(sizeof(inf) - 1u)) != PROS_DRIVER_OK)
            return PROS_DRIVER_EIO;
        if (pros_driver_compat_get_package(package_id, &hashed_pkg)
            != PROS_DRIVER_OK || !hashed_pkg.content_sha256_valid)
            return PROS_DRIVER_EIO;
        {
            uint32_t di;
            for (di = 0; di < PROS_FILE_SHA256_BYTES; ++di)
                if (hashed_pkg.content_sha256[di] != digest[di])
                    return PROS_DRIVER_EIO;
        }
        {
            static const uint8_t altered[] = { 'x' };
            struct pros_file_admission_record bad_record;
            struct pros_file_admission_input bad_input;
            bad_input.data = altered;
            bad_input.length = 1u;
            bad_input.metadata.format_valid = 1u;
            bad_input.metadata.signature_valid = 0u;
            bad_input.metadata.signature_present = 0u;
            bad_input.metadata.local_trusted = 1u;
            bad_input.metadata.local_denied = 0u;
            bad_input.metadata.reputation = PROS_FILE_REPUTATION_CLEAN;
            bad_input.metadata.provenance_known = 1u;
            if (pros_file_admission_evaluate(&bad_input, &bad_record) != 0 ||
                pros_driver_compat_bind_admission(package_id, &bad_record)
                    != PROS_DRIVER_EPERM)
                return PROS_DRIVER_EIO;
        }
    }
    if (pros_driver_compat_get_id(package_id, 1u, &id) != PROS_DRIVER_OK ||
        id.vendor_id != 0xabcd || id.device_id != 0x0001)
        return PROS_DRIVER_EIO;
    if (pros_driver_compat_supports_model(PROS_COMPAT_MODEL_WDM)
        != PROS_DRIVER_ENOTSUP)
        return PROS_DRIVER_EIO;

    {
        struct pros_file_admission_input admission_input;
        struct pros_file_admission_record admission_record;
        admission_input.data = (const uint8_t *)inf;
        admission_input.length = (uint64_t)(sizeof(inf) - 1u);
        admission_input.metadata.format_valid = 1u;
        admission_input.metadata.signature_valid = 0u;
        admission_input.metadata.signature_present = 0u;
        admission_input.metadata.local_trusted = 1u;
        admission_input.metadata.local_denied = 0u;
        admission_input.metadata.reputation = PROS_FILE_REPUTATION_CLEAN;
        admission_input.metadata.provenance_known = 1u;
        if (pros_file_admission_evaluate(&admission_input, &admission_record) != 0 ||
            pros_driver_compat_bind_admission(package_id, &admission_record)
                != PROS_DRIVER_OK)
            return PROS_DRIVER_EIO;
    }
    if (pros_driver_compat_begin(package_id) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    if (pros_driver_compat_verify(package_id) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;

    /* A content change must revoke the previous admission/trust decision. */
    {
        static const uint8_t changed_inf[] = "changed";
        struct pros_compat_package changed_pkg;
        if (pros_driver_compat_hash_content(
                package_id, changed_inf, (uint64_t)(sizeof(changed_inf) - 1u))
            != PROS_DRIVER_OK ||
            pros_driver_compat_get_package(package_id, &changed_pkg)
                != PROS_DRIVER_OK ||
            changed_pkg.trust != PROS_COMPAT_TRUST_UNKNOWN ||
            changed_pkg.trust_evidence != PROS_COMPAT_EVIDENCE_NONE ||
            changed_pkg.tx_state != PROS_COMPAT_TX_IDLE)
            return PROS_DRIVER_EIO;
        if (pros_driver_compat_verify(package_id) != PROS_DRIVER_EPERM)
            return PROS_DRIVER_EIO;
    }

    /*
     * The content change revoked the old admission, so a fresh admission and
     * transaction are required before staging.
     */
    {
        struct pros_file_admission_input refreshed_input;
        struct pros_file_admission_record refreshed_record;
        static const uint8_t changed_inf[] = "changed";

        refreshed_input.data = changed_inf;
        refreshed_input.length = (uint64_t)(sizeof(changed_inf) - 1u);
        refreshed_input.metadata.format_valid = 1u;
        refreshed_input.metadata.signature_valid = 0u;
        refreshed_input.metadata.signature_present = 0u;
        refreshed_input.metadata.local_trusted = 1u;
        refreshed_input.metadata.local_denied = 0u;
        refreshed_input.metadata.reputation = PROS_FILE_REPUTATION_CLEAN;
        refreshed_input.metadata.provenance_known = 1u;

        if (pros_file_admission_evaluate(
                &refreshed_input, &refreshed_record) != 0 ||
            pros_driver_compat_bind_admission(
                package_id, &refreshed_record) != PROS_DRIVER_OK ||
            pros_driver_compat_begin(package_id) != PROS_DRIVER_OK ||
            pros_driver_compat_verify(package_id) != PROS_DRIVER_OK)
            return PROS_DRIVER_EIO;
    }
    if (pros_driver_compat_stage(package_id) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    if (pros_driver_compat_health_check(package_id) != PROS_DRIVER_ENOTSUP)
        return PROS_DRIVER_EIO;
    if (pros_driver_compat_rollback(package_id) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;

    /*
     * Trust cannot be promoted without evidence, and changing evidence
     * invalidates a previously established trust decision.
     */
    if (pros_driver_compat_set_trust(
            package_id, PROS_COMPAT_TRUST_VERIFIED) != PROS_DRIVER_EPERM)
        return PROS_DRIVER_EIO;
    if (pros_driver_compat_set_trust_evidence(
            package_id, PROS_COMPAT_EVIDENCE_ADMISSION_POLICY) !=
        PROS_DRIVER_EPERM)
        return PROS_DRIVER_EIO;
    if (pros_driver_compat_set_trust_evidence(
            package_id, PROS_COMPAT_EVIDENCE_LOCAL_POLICY) != PROS_DRIVER_OK ||
        pros_driver_compat_set_trust(
            package_id, PROS_COMPAT_TRUST_VERIFIED) != PROS_DRIVER_EPERM)
        return PROS_DRIVER_EIO;
    if (pros_driver_compat_set_trust_evidence(
            package_id, PROS_COMPAT_EVIDENCE_TEST_ONLY) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    {
        struct pros_compat_package evidence_changed_pkg;
        if (pros_driver_compat_get_package(
                package_id, &evidence_changed_pkg) != PROS_DRIVER_OK ||
            evidence_changed_pkg.trust != PROS_COMPAT_TRUST_UNKNOWN ||
            evidence_changed_pkg.tx_state != PROS_COMPAT_TX_IDLE)
            return PROS_DRIVER_EIO;
    }

    if (pros_driver_compat_match(package_id, &id, &(int){0})
        != PROS_DRIVER_ENODEV)
        return PROS_DRIVER_EIO;

    /*
     * Rollback must revoke driver-selection eligibility as well as the
     * transaction state.
     */
    if (pros_driver_compat_begin(package_id) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    if (pros_driver_compat_rollback(package_id) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    /*
     * Rollback revokes trust/evidence, so begin() alone must not resurrect
     * the old admission-backed verification state.
     */
    if (pros_driver_compat_begin(package_id) != PROS_DRIVER_OK ||
        pros_driver_compat_verify(package_id) != PROS_DRIVER_EPERM)
        return PROS_DRIVER_EIO;
    if (pros_driver_compat_rollback(package_id) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    /*
     * Re-establish a real admission-backed transaction for the positive
     * selection test.  The earlier TEST_ONLY evidence intentionally revoked
     * selection eligibility.
     */
    {
        static const uint8_t selectable_bytes[] = "changed";
        if (compat_selftest_admit(
                package_id, selectable_bytes,
                sizeof(selectable_bytes) - 1u) != PROS_DRIVER_OK ||
            pros_driver_compat_begin(package_id) != PROS_DRIVER_OK ||
            pros_driver_compat_verify(package_id) != PROS_DRIVER_OK)
            return PROS_DRIVER_EIO;
    }
    if (pros_driver_compat_get_id(package_id, 0u, &id)
        != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    if (pros_driver_compat_dependency_check(package_id)
        != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    {
        int score = 0;
        if (pros_driver_compat_match(package_id, &id, &score)
            != PROS_DRIVER_OK || score < 100)
            return PROS_DRIVER_EIO;
    }
    if (pros_driver_compat_rollback(package_id) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    if (pros_driver_compat_match(package_id, &id, &(int){0})
        != PROS_DRIVER_ENODEV)
        return PROS_DRIVER_EIO;

    {
        static const struct pros_driver_id dep_id = {
            PROS_DRIVER_BUS_PCI, 0x4321u, 0x1000u, 0, 0, 0
        };
        uint32_t dep_a = 0, dep_b = 0, dep_c = 0, dep_d = 0;
        int dep_score = 0;
        if (pros_driver_compat_register_package(
                PROS_COMPAT_PACKAGE_INF, PROS_COMPAT_MODEL_WDM,
                "dep-a.inf", &dep_id, 1u, 0, &dep_a) != PROS_DRIVER_OK)
            return PROS_DRIVER_EIO;
        if (pros_driver_compat_register_package(
                PROS_COMPAT_PACKAGE_INF, PROS_COMPAT_MODEL_WDM,
                "dep-b.inf", &dep_id, 1u, 0, &dep_b) != PROS_DRIVER_OK)
            return PROS_DRIVER_EIO;
        if (pros_driver_compat_add_dependency(dep_a, dep_b)
            != PROS_DRIVER_OK)
            return PROS_DRIVER_EIO;
        if (pros_driver_compat_dependency_check(dep_a)
            != PROS_DRIVER_OK)
            return PROS_DRIVER_EIO;
        if (pros_driver_compat_add_dependency(dep_b, dep_a)
            != PROS_DRIVER_OK)
            return PROS_DRIVER_EIO;
        if (pros_driver_compat_dependency_check(dep_a)
            != PROS_DRIVER_EIO)
            return PROS_DRIVER_EIO;
        if (pros_driver_compat_set_trust(dep_b, PROS_COMPAT_TRUST_REJECTED)
            != PROS_DRIVER_OK)
            return PROS_DRIVER_EIO;
        if (pros_driver_compat_begin(dep_b) != PROS_DRIVER_EPERM)
            return PROS_DRIVER_EIO;

        /*
         * Reinitialize after the deliberate cycle/rejection test.  The next
         * graph is independent and must prove the valid shared-dependency
         * topology A -> B,C and B,C -> D.
         */
        if (pros_driver_compat_init() != PROS_DRIVER_OK)
            return PROS_DRIVER_EIO;
        if (pros_driver_compat_register_package(
                PROS_COMPAT_PACKAGE_INF, PROS_COMPAT_MODEL_WDM,
                "dep-a.inf", &dep_id, 1u, 0, &dep_a) != PROS_DRIVER_OK ||
            pros_driver_compat_register_package(
                PROS_COMPAT_PACKAGE_INF, PROS_COMPAT_MODEL_WDM,
                "dep-b.inf", &dep_id, 1u, 0, &dep_b) != PROS_DRIVER_OK ||
            pros_driver_compat_register_package(
                PROS_COMPAT_PACKAGE_INF, PROS_COMPAT_MODEL_WDM,
                "dep-c.inf", &dep_id, 1u, 0, &dep_c) != PROS_DRIVER_OK ||
            pros_driver_compat_register_package(
                PROS_COMPAT_PACKAGE_INF, PROS_COMPAT_MODEL_WDM,
                "dep-d.inf", &dep_id, 1u, 0, &dep_d) != PROS_DRIVER_OK)
            return PROS_DRIVER_EIO;
        if (pros_driver_compat_add_dependency(dep_a, dep_b)
                != PROS_DRIVER_OK ||
            pros_driver_compat_add_dependency(dep_a, dep_c)
                != PROS_DRIVER_OK ||
            pros_driver_compat_add_dependency(dep_b, dep_d)
                != PROS_DRIVER_OK ||
            pros_driver_compat_add_dependency(dep_c, dep_d)
                != PROS_DRIVER_OK)
            return PROS_DRIVER_EIO;
        {
            static const uint8_t dep_a_bytes[] = "dep-a";
            static const uint8_t dep_b_bytes[] = "dep-b";
            static const uint8_t dep_c_bytes[] = "dep-c";
            static const uint8_t dep_d_bytes[] = "dep-d";
            if (compat_selftest_admit(
                    dep_a, dep_a_bytes, sizeof(dep_a_bytes) - 1u) !=
                    PROS_DRIVER_OK ||
                compat_selftest_admit(
                    dep_b, dep_b_bytes, sizeof(dep_b_bytes) - 1u) !=
                    PROS_DRIVER_OK ||
                compat_selftest_admit(
                    dep_c, dep_c_bytes, sizeof(dep_c_bytes) - 1u) !=
                    PROS_DRIVER_OK ||
                compat_selftest_admit(
                    dep_d, dep_d_bytes, sizeof(dep_d_bytes) - 1u) !=
                    PROS_DRIVER_OK)
                return PROS_DRIVER_EIO;
        }
        if (pros_driver_compat_dependency_check(dep_a) != PROS_DRIVER_OK)
            return PROS_DRIVER_EIO;
        if (pros_driver_compat_match(dep_d, &dep_id, &dep_score)
            != PROS_DRIVER_OK || dep_score < 100)
            return PROS_DRIVER_EIO;
    }
    return PROS_DRIVER_OK;
}
