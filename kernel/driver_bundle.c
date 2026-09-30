#include "driver_bundle.h"

static struct pros_driver_bundle_entry entries[PROS_DRIVER_BUNDLE_MAX_ENTRIES];

static int valid_source(enum pros_driver_bundle_source s)
{
    return s >= PROS_DRIVER_BUNDLE_ISO && s <= PROS_DRIVER_BUNDLE_LOCAL;
}
static struct pros_driver_bundle_entry *find(uint32_t id) {
    for (uint32_t i=0;i<PROS_DRIVER_BUNDLE_MAX_ENTRIES;++i) if(entries[i].package_id==id) return &entries[i];
    return 0;
}
int pros_driver_bundle_init(void) {
    for (uint32_t i = 0u; i < PROS_DRIVER_BUNDLE_MAX_ENTRIES; ++i) {
        entries[i].package_id = 0u;
        entries[i].source = PROS_DRIVER_BUNDLE_ISO;
        entries[i].content_sha256_valid = 0u;
        entries[i].version_major = entries[i].version_minor =
            entries[i].version_patch = 0u;
        entries[i].version[0] = 0;
        for (uint32_t d = 0u; d < PROS_FILE_SHA256_BYTES; ++d)
            entries[i].content_sha256[d] = 0u;
    }
    return PROS_DRIVER_OK;
}
int pros_driver_bundle_add(uint32_t id, enum pros_driver_bundle_source s,uint32_t ma,uint32_t mi,uint32_t pa,const uint8_t *d) {
    if(!id||!valid_source(s)||!d) return PROS_DRIVER_EINVAL;
    struct pros_driver_bundle_entry *e=find(id);
    if(!e) for(uint32_t i=0;i<PROS_DRIVER_BUNDLE_MAX_ENTRIES;++i) if(!entries[i].package_id){e=&entries[i];break;}
    if(!e) return PROS_DRIVER_ENOSPC;

    if (e->package_id) {
        struct pros_compat_package package;
        if (pros_driver_compat_get_package(id, &package) != PROS_DRIVER_OK)
            return PROS_DRIVER_ENODEV;
        if (package.tx_state == PROS_COMPAT_TX_VERIFIED ||
            package.tx_state == PROS_COMPAT_TX_STAGED ||
            package.tx_state == PROS_COMPAT_TX_HEALTHY ||
            package.tx_state == PROS_COMPAT_TX_COMMITTED) {
            int same = (e->source == s &&
                        e->version_major == ma &&
                        e->version_minor == mi &&
                        e->version_patch == pa &&
                        e->content_sha256_valid);
            if (same)
                for (uint32_t j = 0u; j < PROS_FILE_SHA256_BYTES; ++j)
                    if (e->content_sha256[j] != d[j])
                        same = 0;
            /*
             * An active entry is immutable. An exact re-add is idempotent;
             * any metadata/content change remains blocked.
             */
            if (same)
                return PROS_DRIVER_OK;
            return PROS_DRIVER_EBUSY;
        }
    }

    e->package_id=id;e->source=s;e->version_major=ma;e->version_minor=mi;e->version_patch=pa;
    for(uint32_t i=0;i<PROS_FILE_SHA256_BYTES;++i)e->content_sha256[i]=d[i];
    e->content_sha256_valid=1u;
    e->version[0] = 0;
    return PROS_DRIVER_OK;
}
int pros_driver_bundle_find(uint32_t id, struct pros_driver_bundle_entry *out) {
    if(!id||!out)return PROS_DRIVER_EINVAL; struct pros_driver_bundle_entry *e=find(id);
    if(!e)return PROS_DRIVER_ENODEV; *out=*e; return PROS_DRIVER_OK;
}
int pros_driver_bundle_remove(uint32_t package_id)
{
    struct pros_driver_bundle_entry *e;
    uint32_t i;

    if (!package_id)
        return PROS_DRIVER_EINVAL;
    e = find(package_id);
    if (!e)
        return PROS_DRIVER_ENODEV;

    {
        struct pros_compat_package package;
        {
            int package_rc = pros_driver_compat_get_package(package_id, &package);
            /*
             * The bundle index is a locator, not the package owner.  A
             * stale locator must remain removable even if its compatibility
             * package was already reclaimed.
             */
            if (package_rc != PROS_DRIVER_OK &&
                package_rc != PROS_DRIVER_ENODEV)
                return package_rc;
            if (package_rc == PROS_DRIVER_ENODEV)
                goto clear_entry;
        }

        /*
         * Removing an indexed candidate must never silently detach an
         * admitted/active transaction.  The compatibility registry owns
         * the package lifecycle; the bundle index is only the locator.
         */
        if (package.tx_state == PROS_COMPAT_TX_VERIFIED ||
            package.tx_state == PROS_COMPAT_TX_STAGED ||
            package.tx_state == PROS_COMPAT_TX_HEALTHY ||
            package.tx_state == PROS_COMPAT_TX_COMMITTED)
            return PROS_DRIVER_EBUSY;
    }

clear_entry:
    e->package_id = 0u;
    e->source = PROS_DRIVER_BUNDLE_ISO;
    e->version_major = e->version_minor = e->version_patch = 0u;
    e->content_sha256_valid = 0u;
    for (i = 0u; i < PROS_FILE_SHA256_BYTES; ++i)
        e->content_sha256[i] = 0u;
    e->version[0] = 0;
    return PROS_DRIVER_OK;
}

int pros_driver_bundle_retire(uint32_t package_id)
{
    int rc;

    if (!package_id)
        return PROS_DRIVER_EINVAL;

    /*
     * Removal is deliberately ordered: first detach the locator, then
     * reclaim the compatibility registry slot.  If registry reclamation is
     * blocked by dependencies, the package remains unregistered from the
     * bundle and can be retried explicitly.
     */
    rc = pros_driver_bundle_remove(package_id);
    if (rc != PROS_DRIVER_OK)
        return rc;

    return pros_driver_compat_unregister_package(package_id);
}

int pros_driver_bundle_compare_version(uint32_t a,uint32_t b,int *out) {
    if(!a||!b||!out)return PROS_DRIVER_EINVAL; struct pros_driver_bundle_entry *x=find(a),*y=find(b);
    if(!x||!y)return PROS_DRIVER_ENODEV;
    if(x->version_major!=y->version_major)*out=x->version_major>y->version_major?1:-1;
    else if(x->version_minor!=y->version_minor)*out=x->version_minor>y->version_minor?1:-1;
    else if(x->version_patch!=y->version_patch)*out=x->version_patch>y->version_patch?1:-1; else *out=0;
    return PROS_DRIVER_OK;
}
int pros_driver_bundle_admit_package(
    enum pros_compat_package_type type,
    enum pros_compat_driver_model model,
    const char *name,
    const struct pros_driver_id *ids,
    uint32_t id_count,
    uint32_t flags,
    enum pros_driver_bundle_source source,
    uint32_t major, uint32_t minor, uint32_t patch,
    const uint8_t *content, uint64_t content_length,
    const struct pros_file_admission_record *admission,
    uint32_t *package_id_out)
{
    uint32_t package_id = 0;
    int rc;

    if (!name || !ids || !id_count || !content || !content_length ||
        !admission || !package_id_out || !valid_source(source))
        return PROS_DRIVER_EINVAL;

    rc = pros_driver_compat_register_package(
        type, model, name, ids, id_count, flags, &package_id);
    if (rc != PROS_DRIVER_OK)
        return rc;

    rc = pros_driver_compat_hash_content(package_id, content, content_length);
    if (rc != PROS_DRIVER_OK) {
        (void)pros_driver_compat_unregister_package(package_id);
        return rc;
    }

    rc = pros_driver_compat_bind_admission(package_id, admission);
    if (rc != PROS_DRIVER_OK) {
        (void)pros_driver_compat_unregister_package(package_id);
        return rc;
    }

    rc = pros_driver_compat_begin(package_id);
    if (rc != PROS_DRIVER_OK) {
        (void)pros_driver_compat_unregister_package(package_id);
        return rc;
    }

    rc = pros_driver_compat_verify(package_id);
    if (rc != PROS_DRIVER_OK) {
        (void)pros_driver_compat_rollback(package_id);
        return rc;
    }

    {
        struct pros_compat_package package;
        if (pros_driver_compat_get_package(package_id, &package) != PROS_DRIVER_OK ||
            !package.content_sha256_valid) {
            (void)pros_driver_compat_rollback(package_id);
            (void)pros_driver_compat_unregister_package(package_id);
            return PROS_DRIVER_EIO;
        }
        rc = pros_driver_bundle_add(
            package_id, source, major, minor, patch, package.content_sha256);
    }
    if (rc != PROS_DRIVER_OK) {
        (void)pros_driver_compat_rollback(package_id);
        (void)pros_driver_compat_unregister_package(package_id);
        return rc;
    }

    *package_id_out = package_id;
    return PROS_DRIVER_OK;
}

int pros_driver_bundle_select(uint32_t package_id, const struct pros_driver_id *hwid,
                              uint32_t *selected_package_id, int *score_out)
{
    if (!package_id || !hwid || !selected_package_id || !score_out)
        return PROS_DRIVER_EINVAL;

    struct pros_driver_bundle_entry *entry = find(package_id);
    struct pros_compat_package package;
    if (!entry || !entry->content_sha256_valid)
        return PROS_DRIVER_ENODEV;
    {
        int package_rc = pros_driver_compat_get_package(package_id, &package);
        if (package_rc != PROS_DRIVER_OK)
            return package_rc == PROS_DRIVER_EINVAL ?
                PROS_DRIVER_ENODEV : package_rc;
        if (!package.content_sha256_valid)
            return PROS_DRIVER_ENODEV;
    }

    /*
     * The offline bundle index is only a locator.  Its digest must agree with
     * the compatibility registry's current package content identity before
     * the package is selectable.  A stale bundle entry must fail closed.
     */
    for (uint32_t i = 0; i < PROS_FILE_SHA256_BYTES; ++i)
        if (entry->content_sha256[i] != package.content_sha256[i])
            return PROS_DRIVER_ENODEV;

    /*
     * Compatibility selection is deliberately delegated to the compatibility
     * registry.  That registry enforces VERIFIED admission, dependency trust,
     * and an active transaction state before a package can match.
     */
    int score = 0;
    int rc = pros_driver_compat_match(package_id, hwid, &score);
    if (rc != PROS_DRIVER_OK)
        return rc;

    *selected_package_id = package_id;
    *score_out = score;
    return PROS_DRIVER_OK;
}

int pros_driver_bundle_select_best(const struct pros_driver_id *hwid,
                                   uint32_t *selected_package_id, int *score_out)
{
    uint32_t best = 0;
    int best_score = -1;
    if (!hwid || !selected_package_id || !score_out)
        return PROS_DRIVER_EINVAL;

    for (uint32_t i = 0; i < PROS_DRIVER_BUNDLE_MAX_ENTRIES; ++i) {
        if (!entries[i].package_id || !entries[i].content_sha256_valid)
            continue;
        {
            struct pros_compat_package package;
            int package_rc = pros_driver_compat_get_package(
                entries[i].package_id, &package);
            if (package_rc != PROS_DRIVER_OK ||
                !package.content_sha256_valid)
                continue;
            {
                int digest_match = 1;
                for (uint32_t d = 0; d < PROS_FILE_SHA256_BYTES; ++d) {
                    if (entries[i].content_sha256[d] != package.content_sha256[d]) {
                        digest_match = 0;
                        break;
                    }
                }
                if (!digest_match) continue;
            }
        }
        int score = 0;
        if (pros_driver_compat_match(entries[i].package_id, hwid, &score) != PROS_DRIVER_OK)
            continue;
        if (score > best_score) {
            best = entries[i].package_id;
            best_score = score;
            continue;
        }
        if (score == best_score && best) {
            struct pros_driver_bundle_entry *cur = find(best);
            int version_cmp = 0;
            if (entries[i].version_major != cur->version_major)
                version_cmp = entries[i].version_major > cur->version_major ? 1 : -1;
            else if (entries[i].version_minor != cur->version_minor)
                version_cmp = entries[i].version_minor > cur->version_minor ? 1 : -1;
            else if (entries[i].version_patch != cur->version_patch)
                version_cmp = entries[i].version_patch > cur->version_patch ? 1 : -1;

            /*
             * Deterministic precedence:
             *   1. hardware match score
             *   2. semantic version
             *   3. bundle source (ISO/removable/local enum order)
             *   4. package ID
             */
            if (version_cmp > 0 ||
                (version_cmp == 0 &&
                 (entries[i].source < cur->source ||
                  (entries[i].source == cur->source &&
                   entries[i].package_id < cur->package_id)))) {
                best = entries[i].package_id;
            }
        }
    }
    if (!best) return PROS_DRIVER_ENODEV;
    *selected_package_id = best;
    *score_out = best_score;
    return PROS_DRIVER_OK;
}

int pros_driver_bundle_selftest(void) {
    uint8_t a[PROS_FILE_SHA256_BYTES],b[PROS_FILE_SHA256_BYTES];
    for(uint32_t i=0;i<PROS_FILE_SHA256_BYTES;++i){a[i]=(uint8_t)i;b[i]=(uint8_t)(i^0x55u);}
    if(pros_driver_bundle_init()!=0)return -1;
    if(pros_driver_compat_init()!=PROS_DRIVER_OK)return -2;
    if(pros_driver_bundle_add(1,PROS_DRIVER_BUNDLE_ISO,1,2,3,a)!=0)return -2;
    if(pros_driver_bundle_add(2,PROS_DRIVER_BUNDLE_REMOVABLE,1,3,0,b)!=0)return -3;
    if(pros_driver_bundle_add(3,(enum pros_driver_bundle_source)-1,1,0,0,a)!=PROS_DRIVER_EINVAL)return -4;
    int cmp=0;if(pros_driver_bundle_compare_version(1,2,&cmp)!=0||cmp>=0)return -5;
    struct pros_driver_bundle_entry e;if(pros_driver_bundle_find(1,&e)!=0)return -6;
    for(uint32_t i=0;i<PROS_FILE_SHA256_BYTES;++i)if(e.content_sha256[i]!=a[i])return -7;

    /*
     * Admission-backed import must hash the actual bytes, bind matching
     * admission evidence, verify the transaction, and only then index it.
     */
    {
        static const struct pros_driver_id import_id = {
            PROS_DRIVER_BUS_PCI, 0x2468u, 0x1357u, 0, 0, 0
        };
        static const uint8_t import_bytes[] = "offline-import";
        struct pros_file_admission_input input;
        struct pros_file_admission_record record;
        struct pros_compat_package package;
        struct pros_driver_bundle_entry imported;
        uint32_t imported_id = 0;

        input.data = import_bytes;
        input.length = sizeof(import_bytes) - 1u;
        input.metadata.format_valid = 1u;
        input.metadata.signature_valid = 0u;
        input.metadata.signature_present = 0u;
        input.metadata.local_trusted = 1u;
        input.metadata.local_denied = 0u;
        input.metadata.reputation = PROS_FILE_REPUTATION_CLEAN;
        input.metadata.provenance_known = 1u;

        if (pros_file_admission_evaluate(&input, &record) != 0 ||
            pros_driver_bundle_admit_package(
                PROS_COMPAT_PACKAGE_INF, PROS_COMPAT_MODEL_WDM,
                "offline-import.inf", &import_id, 1u, 0u,
                PROS_DRIVER_BUNDLE_REMOVABLE, 2u, 1u, 0u,
                import_bytes, sizeof(import_bytes) - 1u, &record,
                &imported_id) != PROS_DRIVER_OK ||
            pros_driver_compat_get_package(imported_id, &package) !=
                PROS_DRIVER_OK ||
            package.tx_state != PROS_COMPAT_TX_VERIFIED ||
            package.trust != PROS_COMPAT_TRUST_VERIFIED ||
            pros_driver_bundle_find(imported_id, &imported) !=
                PROS_DRIVER_OK ||
            !imported.content_sha256_valid)
            return -9;

        for (uint32_t i = 0u; i < PROS_FILE_SHA256_BYTES; ++i)
            if (imported.content_sha256[i] != package.content_sha256[i])
                return -10;

        {
            uint32_t selected = 0;
            int score = 0;
            if (pros_driver_bundle_select(
                    imported_id, &import_id, &selected, &score) !=
                    PROS_DRIVER_OK ||
                selected != imported_id || score < 100)
                return -11;
        }

        {
            static const uint8_t import_bytes_v2[] = "offline-import-v2";
            struct pros_file_admission_input input_v2;
            struct pros_file_admission_record record_v2;
            struct pros_compat_package package_v2;
            struct pros_driver_bundle_entry entry_v2;
            uint32_t package_v2_id = 0;
            uint32_t selected_v2 = 0;
            int score_v2 = 0;

            input_v2.data = import_bytes_v2;
            input_v2.length = sizeof(import_bytes_v2) - 1u;
            input_v2.metadata.format_valid = 1u;
            input_v2.metadata.signature_valid = 0u;
            input_v2.metadata.signature_present = 0u;
            input_v2.metadata.local_trusted = 1u;
            input_v2.metadata.local_denied = 0u;
            input_v2.metadata.reputation = PROS_FILE_REPUTATION_CLEAN;
            input_v2.metadata.provenance_known = 1u;

            if (pros_file_admission_evaluate(&input_v2, &record_v2) !=
                    PROS_DRIVER_OK ||
                pros_driver_bundle_admit_package(
                    PROS_COMPAT_PACKAGE_INF, PROS_COMPAT_MODEL_WDM,
                    "offline-import-v2.inf", &import_id, 1u, 0u,
                    PROS_DRIVER_BUNDLE_LOCAL, 3u, 0u, 0u,
                    import_bytes_v2, sizeof(import_bytes_v2) - 1u,
                    &record_v2, &package_v2_id) != PROS_DRIVER_OK ||
                pros_driver_compat_get_package(package_v2_id, &package_v2) !=
                    PROS_DRIVER_OK ||
                package_v2.tx_state != PROS_COMPAT_TX_VERIFIED ||
                pros_driver_bundle_find(package_v2_id, &entry_v2) !=
                    PROS_DRIVER_OK ||
                pros_driver_bundle_select_best(
                    &import_id, &selected_v2, &score_v2) != PROS_DRIVER_OK ||
                selected_v2 != package_v2_id || score_v2 < 100)
                return -12;

            if (pros_driver_bundle_remove(package_v2_id) != PROS_DRIVER_EBUSY ||
                pros_driver_bundle_find(package_v2_id, &entry_v2) !=
                    PROS_DRIVER_OK)
                return -14;

            if (pros_driver_bundle_add(
                    package_v2_id, PROS_DRIVER_BUNDLE_LOCAL,
                    3u, 0u, 0u, entry_v2.content_sha256) !=
                PROS_DRIVER_OK)
                return -18;

            {
                uint8_t replacement_digest[PROS_FILE_SHA256_BYTES];
                for (uint32_t d = 0u; d < PROS_FILE_SHA256_BYTES; ++d)
                    replacement_digest[d] = (uint8_t)(d ^ 0xA5u);
                if (pros_driver_bundle_add(
                        package_v2_id, PROS_DRIVER_BUNDLE_LOCAL,
                        4u, 0u, 0u, replacement_digest) !=
                    PROS_DRIVER_EBUSY)
                    return -17;
            }

            /*
             * Same hardware score/version must prefer the deterministic
             * source order (ISO before removable/local).
             */
            {
                static const uint8_t import_bytes_v3[] = "offline-import-v3";
                struct pros_file_admission_input input_v3;
                struct pros_file_admission_record record_v3;
                struct pros_compat_package package_v3;
                uint32_t package_v3_id = 0;
                uint32_t selected_v3 = 0;
                int score_v3 = 0;

                input_v3.data = import_bytes_v3;
                input_v3.length = sizeof(import_bytes_v3) - 1u;
                input_v3.metadata.format_valid = 1u;
                input_v3.metadata.signature_valid = 0u;
                input_v3.metadata.signature_present = 0u;
                input_v3.metadata.local_trusted = 1u;
                input_v3.metadata.local_denied = 0u;
                input_v3.metadata.reputation = PROS_FILE_REPUTATION_CLEAN;
                input_v3.metadata.provenance_known = 1u;

                if (pros_file_admission_evaluate(&input_v3, &record_v3) !=
                        PROS_DRIVER_OK ||
                    pros_driver_bundle_admit_package(
                        PROS_COMPAT_PACKAGE_INF, PROS_COMPAT_MODEL_WDM,
                        "offline-import-v3.inf", &import_id, 1u, 0u,
                        PROS_DRIVER_BUNDLE_ISO, 3u, 0u, 0u,
                        import_bytes_v3, sizeof(import_bytes_v3) - 1u,
                        &record_v3, &package_v3_id) != PROS_DRIVER_OK ||
                    pros_driver_compat_get_package(package_v3_id, &package_v3) !=
                        PROS_DRIVER_OK ||
                    package_v3.tx_state != PROS_COMPAT_TX_VERIFIED ||
                    pros_driver_bundle_select_best(
                        &import_id, &selected_v3, &score_v3) != PROS_DRIVER_OK ||
                    selected_v3 != package_v3_id || score_v3 < 100)
                    return -15;

                /*
                 * Equal score/version/source falls through to package ID;
                 * the older package ID must remain the deterministic winner.
                 */
                {
                    static const uint8_t import_bytes_v4[] =
                        "offline-import-v4";
                    struct pros_file_admission_input input_v4;
                    struct pros_file_admission_record record_v4;
                    uint32_t package_v4_id = 0;
                    uint32_t selected_v4 = 0;
                    int score_v4 = 0;

                    input_v4.data = import_bytes_v4;
                    input_v4.length = sizeof(import_bytes_v4) - 1u;
                    input_v4.metadata.format_valid = 1u;
                    input_v4.metadata.signature_valid = 0u;
                    input_v4.metadata.signature_present = 0u;
                    input_v4.metadata.local_trusted = 1u;
                    input_v4.metadata.local_denied = 0u;
                    input_v4.metadata.reputation =
                        PROS_FILE_REPUTATION_CLEAN;
                    input_v4.metadata.provenance_known = 1u;

                    if (pros_file_admission_evaluate(
                            &input_v4, &record_v4) != PROS_DRIVER_OK ||
                        pros_driver_bundle_admit_package(
                            PROS_COMPAT_PACKAGE_INF, PROS_COMPAT_MODEL_WDM,
                            "offline-import-v4.inf", &import_id, 1u, 0u,
                            PROS_DRIVER_BUNDLE_ISO, 3u, 0u, 0u,
                            import_bytes_v4, sizeof(import_bytes_v4) - 1u,
                            &record_v4, &package_v4_id) != PROS_DRIVER_OK ||
                        package_v4_id <= package_v3_id ||
                        pros_driver_bundle_select_best(
                            &import_id, &selected_v4, &score_v4) !=
                            PROS_DRIVER_OK ||
                        selected_v4 != package_v3_id ||
                        score_v4 < 100)
                        return -16;
                }
            }
        }

        {
            struct pros_file_admission_record bad = record;
            bad.sha256[0] ^= 0xFFu;
            if (pros_driver_bundle_admit_package(
                    PROS_COMPAT_PACKAGE_INF, PROS_COMPAT_MODEL_WDM,
                    "offline-import-bad.inf", &import_id, 1u, 0u,
                    PROS_DRIVER_BUNDLE_REMOVABLE, 2u, 1u, 1u,
                    import_bytes, sizeof(import_bytes) - 1u, &bad,
                    &imported_id) == PROS_DRIVER_OK)
                return -12;

        /*
         * A failed tail import must reclaim its registry slot so a later
         * offline import is not blocked by a permanently poisoned entry.
         */
        {
            uint32_t reclaimed_id = 0;
            if (pros_driver_compat_register_package(
                    PROS_COMPAT_PACKAGE_INF, PROS_COMPAT_MODEL_WDM,
                    "offline-reclaim.inf", &import_id, 1u, 0u,
                    &reclaimed_id) != PROS_DRIVER_OK ||
                reclaimed_id != imported_id ||
                pros_driver_compat_unregister_package(reclaimed_id) !=
                    PROS_DRIVER_OK)
                return -13;
        }
        }
    }

    /*
     * A reclaimed compatibility package must not leave an unremovable
     * bundle locator behind.
     */
    {
        static const struct pros_driver_id stale_id = {
            PROS_DRIVER_BUS_PCI, 0xCAFEu, 0xBEEFu, 0, 0, 0
        };
        uint8_t stale_digest[PROS_FILE_SHA256_BYTES];
        uint32_t stale_package = 0;
        for (uint32_t d = 0u; d < PROS_FILE_SHA256_BYTES; ++d)
            stale_digest[d] = (uint8_t)(0x30u + d);

        if (pros_driver_compat_register_package(
                PROS_COMPAT_PACKAGE_INF, PROS_COMPAT_MODEL_WDM,
                "stale-locator.inf", &stale_id, 1u, 0u,
                &stale_package) != PROS_DRIVER_OK ||
            pros_driver_bundle_add(
                stale_package, PROS_DRIVER_BUNDLE_LOCAL,
                1u, 0u, 0u, stale_digest) != PROS_DRIVER_OK ||
            pros_driver_compat_unregister_package(stale_package) !=
                PROS_DRIVER_OK ||
            pros_driver_bundle_remove(stale_package) != PROS_DRIVER_OK ||
            pros_driver_bundle_find(stale_package, &entry_v2) !=
                PROS_DRIVER_ENODEV)
            return -19;
    }

    /*
     * A bundle entry must track the compatibility registry's current content
     * identity.  Deliberately change the package bytes after indexing and
     * verify that selection fails closed rather than using stale metadata.
     */
    {
        static const struct pros_driver_id id = {
            PROS_DRIVER_BUS_PCI, 0x1234u, 0x5678u, 0, 0, 0
        };
        static const uint8_t old_bytes[] = "bundle-old";
        static const uint8_t new_bytes[] = "bundle-new";
        uint32_t package_id = 0;
        uint32_t selected = 0;
        int score = 0;

        if (pros_driver_compat_register_package(
                PROS_COMPAT_PACKAGE_INF, PROS_COMPAT_MODEL_WDM,
                "bundle-selftest.inf", &id, 1u, 0, &package_id)
                != PROS_DRIVER_OK ||
            pros_driver_compat_hash_content(
                package_id, old_bytes, sizeof(old_bytes) - 1u)
                != PROS_DRIVER_OK ||
            pros_driver_bundle_add(
                package_id, PROS_DRIVER_BUNDLE_LOCAL, 1u, 0u, 0u, a)
                != PROS_DRIVER_OK)
            return -8;

        {
            struct pros_compat_package package;
            if (pros_driver_compat_get_package(package_id, &package)
                    != PROS_DRIVER_OK ||
                !package.content_sha256_valid ||
                pros_driver_bundle_add(
                    package_id, PROS_DRIVER_BUNDLE_LOCAL, 1u, 0u, 0u,
                    package.content_sha256) != PROS_DRIVER_OK)
                return -9;
        }

        if (pros_driver_compat_hash_content(
                package_id, new_bytes, sizeof(new_bytes) - 1u)
                != PROS_DRIVER_OK)
            return -10;

        if (pros_driver_bundle_select(
                package_id, &id, &selected, &score) != PROS_DRIVER_ENODEV)
            return -11;

        if (pros_driver_compat_rollback(package_id) != PROS_DRIVER_OK ||
            pros_driver_bundle_select_best(
                &id, &selected, &score) != PROS_DRIVER_ENODEV)
            return -22;
    }
    return 0;
}
