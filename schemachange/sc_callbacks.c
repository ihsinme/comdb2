/*
   Copyright 2015 Bloomberg Finance L.P.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

#include <memory_sync.h>
#include "schemachange.h"
#include "sc_callbacks.h"
#include "sc_global.h"
#include "sc_add_table.h"
#include "sc_schema.h"
#include "sc_util.h"
#include "sc_lua.h"
#include "sc_queues.h"
#include "translistener.h"
#include "views.h"
#include "logmsg.h"
#include "bdb_net.h"
#include "comdb2_atomic.h"
#include "sc_struct.h"
#include "sc_rename_table.h"

extern void free_cached_idx(uint8_t **cached_idx);

static int reload_rename_table(bdb_state_type *bdb_state, const char *name,
                               const char *newtable)
{
    void *tran = NULL;
    int rc;
    int bdberr = 0;
    uint32_t lid = 0;
    extern uint32_t gbl_rep_lockid;
    struct dbtable *db = get_dbtable_by_name(name);

    if (!db) {
        logmsg(LOGMSG_ERROR, "%s: unable to find table %s\n", __func__, name);
        return -1;
    }

    if (rename_db(db, newtable)) {
        logmsg(LOGMSG_ERROR, "%s: failed to rename %s to %s \n", __func__, name,
               newtable);
        return -1;
    }

    tran = bdb_tran_begin(bdb_state, NULL, &bdberr);
    if (tran == NULL) {
        logmsg(LOGMSG_ERROR, "%s: failed to start tran\n", __func__);
        return -1;
    }

    bdb_get_tran_lockerid(tran, &lid);
    bdb_set_tran_lockerid(tran, gbl_rep_lockid);

    if (bdb_table_version_select(newtable, tran, &db->tableversion, &bdberr)) {
        logmsg(LOGMSG_ERROR,
               "%s: failed to retrieve table version for new %s \n", __func__,
               newtable);
        return -1;
    }

    set_odh_options_tran(db, tran);
    update_dbstore(db);
    create_sqlmaster_records(tran);
    create_sqlite_master();
    BDB_BUMP_DBOPEN_GEN(rename_table, NULL);

    bdb_set_tran_lockerid(tran, lid);
    rc = bdb_tran_abort(thedb->bdb_env, tran, &bdberr);
    if (rc)
        logmsg(LOGMSG_FATAL, "%s failed to abort transaction rc:%d\n", __func__,
               rc);
    return rc;
}

static int reload_rename_table_alias(bdb_state_type *bdb_state,
                                     const char *name, const char *newname)
{
    extern uint32_t gbl_rep_lockid;
    uint32_t lid = 0;
    void *tran = NULL;
    struct dbtable *db = get_dbtable_by_name(name);
    int bdberr = 0;
    int rc;

    if (!db) {
        logmsg(LOGMSG_ERROR, "%s: unable to find table %s\n", __func__, name);
        return -1;
    }

    tran = bdb_tran_begin(bdb_state, NULL, &bdberr);
    if (tran == NULL) {
        logmsg(LOGMSG_ERROR, "%s: failed to start tran\n", __func__);
        return -1;
    }

    bdb_get_tran_lockerid(tran, &lid);
    bdb_set_tran_lockerid(tran, gbl_rep_lockid);

    /* newname is NULL if we remove an alias */
    hash_sqlalias_db(db, newname ? newname : db->tablename);

    create_sqlmaster_records(tran);
    create_sqlite_master();
    BDB_BUMP_DBOPEN_GEN(rename_table_alias, NULL);

    bdb_set_tran_lockerid(tran, lid);
    rc = bdb_tran_abort(thedb->bdb_env, tran, &bdberr);
    if (rc)
        logmsg(LOGMSG_FATAL, "%s failed to abort transaction rc:%d\n", __func__,
               rc);
    return rc;
}

static int reload_stripe_info(bdb_state_type *bdb_state)
{
    void *tran = NULL;
    int rc;
    int bdberr = 0;
    int stripes, blobstripe;
    uint32_t lid = 0;
    extern uint32_t gbl_rep_lockid;

    if (close_all_dbs() != 0)
        exit(1);

    tran = bdb_tran_begin(bdb_state, NULL, &bdberr);
    if (tran == NULL) {
        logmsg(LOGMSG_ERROR, "%s: failed to start tran\n", __func__);
        return -1;
    }

    bdb_get_tran_lockerid(tran, &lid);
    bdb_set_tran_lockerid(tran, gbl_rep_lockid);

    if (bdb_get_global_stripe_info(tran, &stripes, &blobstripe, &bdberr) != 0) {
        logmsg(LOGMSG_ERROR, "%s: failed to retrieve global stripe info\n",
               __func__);
        return -1;
    }

    apply_new_stripe_settings(stripes, blobstripe);

    if (open_all_dbs_tran(tran) != 0)
        exit(1);

    fix_blobstripe_genids(tran);

    bdb_set_tran_lockerid(tran, lid);
    rc = bdb_tran_commit(thedb->bdb_env, tran, &bdberr);
    if (rc)
        logmsg(LOGMSG_FATAL, "%s failed to commit transaction rc:%d\n",
               __func__, rc);

    return 0;
}

static int set_genid_format(bdb_state_type *bdb_state, scdone_t type)
{
    switch (type) {
    case (genid48_enable):
        bdb_genid_set_format(bdb_state, LLMETA_GENID_48BIT);
        break;
    case (genid48_disable):
        bdb_genid_set_format(bdb_state, LLMETA_GENID_ORIGINAL);
        break;
    default:
        break;
    }
    return 0;
}

static int reload_rowlocks(bdb_state_type *bdb_state, scdone_t type)
{
    int bdberr, rc;
    rc = bdb_reload_rowlocks(bdb_state, type, &bdberr);
    switch (gbl_rowlocks) {
    case 0:
    case 1: gbl_sql_tranlevel_default = gbl_sql_tranlevel_preserved; break;
    case 2:
        gbl_sql_tranlevel_preserved = gbl_sql_tranlevel_default;
        gbl_sql_tranlevel_default = SQL_TDEF_SNAPISOL;
        break;
    }
    if (rc != 0) {
        logmsg(LOGMSG_ERROR, "%s: bdb_llog_rowlocks returns %d bdberr=%d\n",
               __func__, rc, bdberr);
    }
    return rc;
}

/* if genid <= sc_genids[stripe] then schemachange has already processed up to
 * that point */
int is_genid_right_of_stripe_pointer(bdb_state_type *bdb_state,
                                     unsigned long long genid,
                                     unsigned long long *sc_genids)
{
    int stripe = get_dtafile_from_genid(genid);
    if (stripe < 0 || stripe >= gbl_dtastripe) {
        logmsg(LOGMSG_FATAL, "%s: genid 0x%llx stripe %d out of range!\n",
               __func__, genid, stripe);
        abort();
    }
    if (!sc_genids[stripe]) {
        /* A genid of zero is invalid.  So, if the schema change cursor is at
         * genid zero it means pretty conclusively that it hasn't done anything
         * yet so we cannot possibly be behind the cursor. */
        return 1;
    }
    return bdb_inplace_cmp_genids(bdb_state, genid, sc_genids[stripe]) > 0;
}

unsigned long long get_genid_stripe_pointer(unsigned long long genid,
                                            unsigned long long *sc_genids)
{
    int stripe = get_dtafile_from_genid(genid);
    if (stripe < 0 || stripe >= gbl_dtastripe) {
        logmsg(LOGMSG_FATAL, "%s: genid 0x%llx stripe %d out of range!\n",
               __func__, genid, stripe);
        abort();
    }
    return sc_genids[stripe];
}

/* delete from new btree when genid is older than schemachange position
 */
int live_sc_post_del_record(struct ireq *iq, void *trans,
                            unsigned long long genid, const void *old_dta,
                            unsigned long long del_keys,
                            blob_buffer_t *oldblobs)
{
    struct dbtable *usedb = iq->usedb;

    iq->usedb = usedb->sc_to;
    if (iq->debug) {
        reqpushprefixf(iq, "%s: ", __func__);
        reqprintf(iq, "deleting genid 0x%llx from new table", genid);
    }

    /*
       fprintf(stderr, "live 0x%llx cursor 0x%llx :: live is"
       " behind cursor - DELETE\n", genid, sc_genids[stripe]);
     */

    int rc = del_new_record(iq, trans, genid, del_keys, old_dta, oldblobs, 1);
    iq->usedb = usedb;
    if (rc != 0 && rc != RC_INTERNAL_RETRY) {
        /* Leave this trace in.  We want to know if live schema change
         * is interfering with real updates. */
        logmsg(LOGMSG_ERROR,
               "live_sc_post_delete rcode %d for delete genid 0x%llx\n", rc,
               genid);
        /* If this goes wrong then abort the schema change. */
        logmsg(LOGMSG_ERROR,
               "Aborting schema change due to unexpected error\n");
        usedb->sc_abort = 1;
        MEMORY_SYNC;
        rc = 0; // should just fail SC
    }

    ATOMIC_ADD32(usedb->sc_deletes, 1);
    if (iq->debug) {
        reqpopprefixes(iq, 1);
    }
    return rc;
}

/* re-compute new partial/expressions indexes for new table */
unsigned long long revalidate_new_indexes(struct ireq *iq, struct dbtable *db,
                                          uint8_t *new_dta,
                                          unsigned long long ins_keys,
                                          blob_buffer_t *blobs, size_t maxblobs)
{
    extern int gbl_partial_indexes;
    extern int gbl_expressions_indexes;
    int rebuild_keys = 0;
    if ((gbl_partial_indexes && db->ix_partial) ||
        (gbl_expressions_indexes && db->ix_expr)) {
        int ixnum;
        if (!gbl_use_plan || !db->plan)
            rebuild_keys = 1;
        else {
            for (ixnum = 0; ixnum < db->nix; ixnum++) {
                if (db->plan->ix_plan[ixnum] == -1) {
                    rebuild_keys = 1;
                    break;
                }
            }
        }
        if (rebuild_keys) {
            if (iq->idxInsert || iq->idxDelete) {
                free_cached_idx(iq->idxInsert);
                free_cached_idx(iq->idxDelete);
                free(iq->idxInsert);
                free(iq->idxDelete);
                iq->idxInsert = iq->idxDelete = NULL;
            }
            ins_keys = -1ULL;
        }
    }

    extern int gbl_partial_indexes;
    if (gbl_partial_indexes && db->ix_partial && rebuild_keys)
        ins_keys = verify_indexes(db, new_dta, blobs, maxblobs, 0);

    return ins_keys;
}

/* this is called from delayed_key_adds() for adding keys to new btree
 * since adding them not-delayed could cause SC to abort erroneously
 */
int live_sc_post_update_delayed_key_adds_int(struct ireq *iq, void *trans,
                                             unsigned long long newgenid,
                                             const void *od_dta,
                                             unsigned long long ins_keys,
                                             int od_len)
{
    struct dbtable *usedb = iq->usedb;
    blob_buffer_t *add_idx_blobs = NULL;
    int rc = 0;

    if (usedb->sc_downgrading) {
        return ERR_NOMASTER;
    }

    if (usedb->sc_from != iq->usedb) {
        return 0;
    }

    if (usedb->sc_live_logical) {
        return 0;
    }

#ifdef DEBUG_SC
    printf("live_sc_post_update_delayed_key_adds_int: looking at genid %llx\n",
           newgenid);
#endif
    /* need to check where the cursor is, even tho that check was done once in
     * post_update */
    int is_gen_gt_scptr = is_genid_right_of_stripe_pointer(
        iq->usedb->handle, newgenid, usedb->sc_to->sc_genids);
    if (is_gen_gt_scptr) {
        if (iq->debug) {
            reqprintf(iq, "live_sc_post_update_delayed_key_adds_int: skip "
                          "genid 0x%llx to the right of scptr",
                      newgenid);
        }
        return 0;
    }

    blob_status_t oldblobs[MAXBLOBS] = {{0}};
    blob_buffer_t add_blobs_buf[MAXBLOBS] = {{0}};
    if (iq->usedb->sc_to->ix_blob) {
        rc =
            save_old_blobs(iq, trans, ".ONDISK", od_dta, 2, newgenid, oldblobs);
        if (rc) {
            fprintf(stderr, "%s() save old blobs failed rc %d\n", __func__, rc);
            return rc;
        }
        blob_status_to_blob_buffer(oldblobs, add_blobs_buf);
        add_idx_blobs = add_blobs_buf;
    }

    /* Convert record from .ONDISK -> .NEW..ONDISK */
    void *new_dta = malloc(usedb->sc_to->lrl);
    if (new_dta == NULL) {
        logmsg(LOGMSG_ERROR, "%s() malloc failed\n", __func__);
        return 1;
    }
    struct convert_failure reason;
    rc = stag_to_stag_buf_blobs(usedb->sc_to->tablename, ".ONDISK", od_dta,
                                ".NEW..ONDISK", new_dta, &reason, add_idx_blobs,
                                add_idx_blobs ? MAXBLOBS : 0, 1);
    if (rc) {
        usedb->sc_abort = 1;
        MEMORY_SYNC;
        free(new_dta);
        free_blob_status_data(oldblobs);
        return 0; // should just fail SC
    }

    ins_keys =
        revalidate_new_indexes(iq, usedb->sc_to, new_dta, ins_keys,
                               add_idx_blobs, add_idx_blobs ? MAXBLOBS : 0);

    /* point to the new table */
    iq->usedb = usedb->sc_to;

    if (iq->debug) {
        reqpushprefixf(iq, "live_sc_post_update_delayed_key_adds_int: ");
        reqprintf(iq, "adding to indices genid 0x%llx in new table", newgenid);
    }

    rc = upd_new_record_add2indices(iq, trans, newgenid, new_dta,
                                    usedb->sc_to->lrl, ins_keys, 1,
                                    add_idx_blobs, 0);
    iq->usedb = usedb;
    if (rc != 0 && rc != RC_INTERNAL_RETRY) {
        logmsg(LOGMSG_ERROR,
               "live_sc_post_update_delayed_key_adds_int rcode %d for "
               "add2indices genid 0x%llx\n",
               rc, newgenid);
        logmsg(LOGMSG_ERROR,
               "Aborting schema change due to unexpected error\n");
        iq->usedb->sc_abort = 1;
        MEMORY_SYNC;
        rc = 0; // should just fail SC
    }
    if (iq->debug) {
        reqpopprefixes(iq, 1);
    }
    free(new_dta);
    free_blob_status_data(oldblobs);
    return rc;
}

int live_sc_post_add_record(struct ireq *iq, void *trans,
                            unsigned long long genid, const uint8_t *od_dta,
                            unsigned long long ins_keys, blob_buffer_t *blobs,
                            size_t maxblobs, int origflags, int *rrn)

{
#ifdef DEBUG_SC
    printf("%s: looking at genid %llx\n", __func__, genid);
#endif
    // this is an INSERT of new row so add_record to sc_to
    char *tagname = ".NEW..ONDISK";
    uint8_t *p_tagname_buf = (uint8_t *)tagname,
            *p_tagname_buf_end = p_tagname_buf + 12;
    int opfailcode = 0;
    int ixfailnum = 0;
    int rc;
    struct dbtable *usedb = iq->usedb;

    /* Convert record from .ONDISK -> .NEW..ONDISK */

    void *new_dta = malloc(usedb->sc_to->lrl);
    if (new_dta == NULL) {
        logmsg(LOGMSG_ERROR, "%s() malloc failed\n", __func__);
        return 1;
    }
    struct convert_failure reason;
    rc = stag_to_stag_buf_blobs(usedb->sc_to->tablename, ".ONDISK",
                                (const char *)od_dta, ".NEW..ONDISK", new_dta,
                                &reason, blobs, maxblobs, 1);
    if (rc) {
        usedb->sc_abort = 1;
        MEMORY_SYNC;
        rc = 0;
        goto done; // should just fail SC
    }

    ins_keys = revalidate_new_indexes(iq, usedb->sc_to, new_dta, ins_keys,
                                      blobs, maxblobs);

    if ((origflags & RECFLAGS_NO_CONSTRAINTS) && usedb->sc_to->n_constraints) {
        int rebuild = usedb->sc_to->plan && usedb->sc_to->plan->dta_plan;
#ifdef DEBUG_SC
        fprintf(stderr, "%s: need to verify_record_constraint genid 0x%llx\n",
                __func__, genid);
#endif
        rc = verify_record_constraint(iq, usedb->sc_to, trans, new_dta,
                                      ins_keys, blobs, maxblobs, ".NEW..ONDISK",
                                      rebuild, 0);
        if (rc) {
            logmsg(LOGMSG_ERROR, "%s: verify_record_constraint rcode %d, genid 0x%llx\n",
                   __func__, rc, genid);
            logmsg(LOGMSG_ERROR, "Aborting schema change due to constraint violation in new schema\n");

            usedb->sc_abort = 1;
            MEMORY_SYNC;
            rc = 0;
            goto done; // should just fail SC
        }
    }

    if (iq->debug) {
        reqpushprefixf(iq, "%s: ", __func__);
        reqprintf(iq, "adding genid 0x%llx to new table", genid);
    }

    iq->usedb = usedb->sc_to;

    int addflags = RECFLAGS_NO_TRIGGERS | RECFLAGS_NEW_SCHEMA | RECFLAGS_KEEP_GENID;

    if (origflags & RECFLAGS_NO_CONSTRAINTS) {
        addflags |= RECFLAGS_NO_CONSTRAINTS;
    }

    rc = add_record(iq, trans, p_tagname_buf, p_tagname_buf_end, new_dta,
                    new_dta + usedb->sc_to->lrl, NULL, blobs, maxblobs,
                    &opfailcode, &ixfailnum, rrn, &genid, ins_keys,
                    BLOCK2_ADDKL, // opcode
                    0,            // blkpos
                    addflags, 0);

    iq->usedb = usedb;

    if (rc != 0 && rc != RC_INTERNAL_RETRY) {
        logmsg(LOGMSG_ERROR, "%s: rcode %d, genid 0x%llx\n", __func__, rc, genid);
        logmsg(LOGMSG_ERROR, "Aborting schema change due to unexpected error\n");
        iq->usedb->sc_abort = 1;
        MEMORY_SYNC;
        rc = 0; // should just fail SC
    }

done:
    if (iq->debug) {
        reqpopprefixes(iq, 1);
    }

    ATOMIC_ADD32(usedb->sc_adds, 1);
    free(new_dta);
    return rc;
}

/* both new and old are to the left of SC ptr, need to update
 */
int live_sc_post_upd_record(struct ireq *iq, void *trans,
                            unsigned long long oldgenid, const void *old_dta,
                            unsigned long long newgenid, const void *new_dta,
                            unsigned long long ins_keys,
                            unsigned long long del_keys, int od_len,
                            int *updCols, blob_buffer_t *blobs, int deferredAdd,
                            blob_buffer_t *oldblobs, blob_buffer_t *newblobs)
{
    struct dbtable *usedb = iq->usedb;

#ifdef DEBUG_SC
    fprintf(stderr, "%s: oldgenid 0x%llx, newgenid "
                    "0x%llx, deferredAdd %d\n",
            __func__, oldgenid, newgenid, deferredAdd);
#endif

    int rc;
    /* point to the new table */
    iq->usedb = usedb->sc_to;

    if (iq->debug) {
        reqpushprefixf(iq, "%s: ", __func__);
        reqprintf(iq,
                  "updating genid 0x%llx to 0x%llx in new table (defered=%d)",
                  oldgenid, newgenid, deferredAdd);
    }

    if (iq->debug) {
        reqpushprefixf(iq, "upd_new_record: ");
    }
    rc = upd_new_record(iq, trans, oldgenid, old_dta, newgenid, new_dta,
                        ins_keys, del_keys, od_len, updCols, blobs, deferredAdd,
                        oldblobs, newblobs, 1);
    iq->usedb = usedb;
    if (rc != 0 && rc != RC_INTERNAL_RETRY) {
        logmsg(LOGMSG_ERROR, "%s: rcode %d for update genid 0x%llx to 0x%llx\n",
               __func__, rc, oldgenid, newgenid);
        logmsg(LOGMSG_ERROR, "Aborting schema change due to unexpected error\n");
        iq->usedb->sc_abort = 1;
        MEMORY_SYNC;
        rc = 0; // should just fail SC
    }

    ATOMIC_ADD32(usedb->sc_updates, 1);
    if (iq->debug) {
        reqpopprefixes(iq, 2);
    }
    return rc;
}

/*
 * Called by the bdb layer when the master is trying to downgrade.
 */
int schema_change_abort_callback(void)
{
    Pthread_mutex_lock(&gbl_sc_lock);
    /* if a schema change is in progress */
    if (get_schema_change_in_progress(__func__, __LINE__)) {
        /* we should safely stop the sc here, but until we find a good way to do
         * that, just kill us */
        exit(1);
    }
    Pthread_mutex_unlock(&gbl_sc_lock);

    return 0;
}

/* Deletes all the files that are no longer needed after a schema change.  Also
 * sets a timer that the checkpoint thread checks by calling
 * sc_del_unused_files_check_progress() */
void sc_del_unused_files_tran(struct dbtable *db, tran_type *tran)
{
    int bdberr;

    if (db == NULL || db->handle == NULL)
        return;

    Pthread_mutex_lock(&gbl_sc_lock);
    sc_del_unused_files_start_ms = comdb2_time_epochms();
    Pthread_mutex_unlock(&gbl_sc_lock);

    if (bdb_attr_get(thedb->bdb_attr, BDB_ATTR_DELAYED_OLDFILE_CLEANUP)) {
        if (bdb_list_unused_files_tran(
                db->handle, tran, &bdberr, 
                "schemachange") || bdberr != BDBERR_NOERROR)
            logmsg(LOGMSG_WARN, "%s: errors listing old files\n", __func__);
    } else {
        if (bdb_del_unused_files_tran(db->handle, tran, &bdberr) ||
            bdberr != BDBERR_NOERROR)
            logmsg(LOGMSG_WARN, "errors deleting files\n");
    }

    Pthread_mutex_lock(&gbl_sc_lock);
    sc_del_unused_files_start_ms = 0;
    Pthread_mutex_unlock(&gbl_sc_lock);
}

void sc_del_unused_files(struct dbtable *db)
{
    sc_del_unused_files_tran(db, NULL);
}

/* Checks to see if a schema change has been trying to delete files for longer
 * then gbl_sc_del_unused_files_threshold_ms, if so it exits */
void sc_del_unused_files_check_progress(void)
{
    int start_ms;

    Pthread_mutex_lock(&gbl_sc_lock);
    start_ms = sc_del_unused_files_start_ms;
    Pthread_mutex_unlock(&gbl_sc_lock);

    /* if a schema change is in progress */
    if (start_ms) {
        int diff_ms = comdb2_time_epochms() - start_ms;
        if (diff_ms > gbl_sc_del_unused_files_threshold_ms) {
            logmsg(LOGMSG_FATAL,
                   "Schema change has been waiting %dms for files to "
                   "be deleted, exiting.\nPlease let the comdb2 team know "
                   "about this, and run 'send <dbname> delfiles "
                   "<schema_changed_table>' on the new master to clean up the "
                   "files we didn't delete\n",
                   diff_ms);
            exit(1);
        }
    }
}

static int delete_table_rep(char *table, void *tran)
{
    int rc, bdberr;
    struct dbtable *db = get_dbtable_by_name(table);
    if (db == NULL) {
        logmsg(LOGMSG_ERROR, "delete_table_rep : invalid table %s\n", table);
        return -1;
    }

    remove_constraint_pointers(db);

    if ((rc = bdb_close_only_sc(db->handle, tran, &bdberr))) {
        logmsg(LOGMSG_ERROR, "bdb_close_only rc %d bdberr %d\n", rc, bdberr);
        return -1;
    }

    delete_db(table);
    MEMORY_SYNC;
    delete_schema(table);
    return 0;
}

static int bthash_callback(const char *table)
{
    int bthashsz;
    logmsg(LOGMSG_INFO, "Replicant bthashing table: %s\n", table);
    struct dbtable *db = get_dbtable_by_name(table);
    if (db && get_db_bthash(db, &bthashsz) == 0) {
        if (bthashsz) {
            logmsg(LOGMSG_INFO,
                   "Building bthash for table %s, size %dkb per stripe\n",
                   db->tablename, bthashsz);
            bdb_handle_dbp_add_hash(db->handle, bthashsz);
        } else {
            logmsg(LOGMSG_INFO, "Deleting bthash for table %s\n",
                   db->tablename);
            bdb_handle_dbp_drop_hash(db->handle);
        }
        return 0;
    } else {
        logmsg(LOGMSG_ERROR, "%s: error updating bthash for %s.\n", __func__,
               table);
        return 1;
    }
}

extern int gbl_assert_systable_locks;
static int replicant_reload_views(const char *name)
{
    int rc;

    rc = views_handle_replicant_reload(name);

    return rc;
}

extern int gbl_assert_systable_locks;

/* TODO fail gracefully now that inline? */
/* called by bdb layer through a callback as a detached thread,
 * we take ownership of table string
 * run on the replecants after the master is done so that they can reload/update
 * their copies of the modified database
 * if this fails, we panic so that we will be restarted back into a consistent
 * state */
int scdone_callback(bdb_state_type *bdb_state, const char table[], void *arg,
                    scdone_t type)
{
    extern uint32_t gbl_rep_lockid;
    if (gbl_assert_systable_locks) {
        switch (type) {
        case llmeta_queue_add:
        case llmeta_queue_alter:
        case llmeta_queue_drop:
            assert(bdb_has_tablename_locked(bdb_state, "comdb2_queues", gbl_rep_lockid, TABLENAME_LOCKED_WRITE));
            break;
        case user_view:
            assert(bdb_has_tablename_locked(bdb_state, "comdb2_views", gbl_rep_lockid, TABLENAME_LOCKED_WRITE));
            break;
        case add: // includes fastinit
        case drop:
        case alter:
            assert(bdb_has_tablename_locked(bdb_state, "comdb2_tables", gbl_rep_lockid, TABLENAME_LOCKED_WRITE));
            break;
        default:
            break;
        }
    }
    switch (type) {
    case luareload:
        return reload_lua();
    case sc_analyze:
        return replicant_reload_analyze_stats();
    case bthash:
        return bthash_callback(table);
    case views:
        return replicant_reload_views(table);
    case rowlocks_on:
    case rowlocks_on_master_only:
    case rowlocks_off: return reload_rowlocks(thedb->bdb_env, type);
    case llmeta_queue_add:
    case llmeta_queue_alter:
    case llmeta_queue_drop:
        return perform_trigger_update_replicant(table, type);
    case genid48_enable:
    case genid48_disable: return set_genid_format(thedb->bdb_env, type);
    case lua_sfunc: return reload_lua_sfuncs();
    case lua_afunc: return reload_lua_afuncs();
    case rename_table:
        return reload_rename_table(bdb_state, table, (char *)arg);
    case rename_table_alias:
        return reload_rename_table_alias(bdb_state, table, (char *)arg);
    case change_stripe:
        return reload_stripe_info(bdb_state);
    default:
        break;
    }

    int add_new_db = 0;
    int rc = 0;
    char *csc2text = NULL;
    char *table_copy = NULL;
    struct dbtable *db = NULL;
    void *tran = NULL;
    int bdberr;
    int highest_ver;
    int dbnum;
    uint32_t lid = 0;

    struct dbtable *olddb = get_dbtable_by_name(table);
    tran = bdb_tran_begin(bdb_state, NULL, &bdberr);
    if (tran == NULL) {
        logmsg(LOGMSG_ERROR, "%s:%d can't begin transaction rc %d\n", __FILE__,
               __LINE__, bdberr);
        rc = bdberr;
        goto done;
    }

    /* This code runs on the replicant to handle an SC_DONE message.  The
     * transaction will have updated (and hold locks for) records in llmeta
     * which we need to look at in order to set up our data structures
     * correctly.  This replaces the tran's lid with replication's lid so that
     * we can query this information without self-deadlocking. */
    bdb_get_tran_lockerid(tran, &lid);
    bdb_set_tran_lockerid(tran, gbl_rep_lockid);

    if (olddb) {
        /* protect us from getting rep_handle_dead'ed to death */
        rc = bdb_get_csc2_highest(tran, table, &highest_ver, &bdberr);
        if (rc && bdberr == BDBERR_DEADLOCK) {
            rc = bdberr;
            goto done;
        }
    }

    if (type != drop && type != user_view &&
        !IS_QUEUEDB_ROLLOVER_SCHEMA_CHANGE_TYPE(type)) {
        if (get_csc2_file_tran(table, -1, &csc2text, NULL, tran)) {
            logmsg(LOGMSG_ERROR, "%s: error getting schema for %s.\n", __func__,
                   table);
            exit(1);
        }
        db = get_dbtable_by_name(table);
        table_copy = strdup(table);
        /* if we can't find a table with that name, we must be trying to add one
         */
        add_new_db = (db == NULL);
    }

    if (type == setcompr) {
        logmsg(LOGMSG_INFO,
               "Replicant setting compression flags for table:%s\n", table);
    } else if (IS_QUEUEDB_ROLLOVER_SCHEMA_CHANGE_TYPE(type)) {
        // TODO: How should we ideally handle failure cases here?
        rc = reopen_qdb(table, 0, tran);
        logmsg(LOGMSG_INFO, "Replicant %s queuedb '%s', rc %d\n",
               (rc == 0) ? "reopened" : "failed to reopen", table, rc);
    } else if (type == add && add_new_db) {
        logmsg(LOGMSG_INFO, "Replicant adding table:%s\n", table);
        dyns_init_globals();
        rc = add_table_to_environment(table_copy, csc2text, NULL, NULL, tran,
                                      timepart_is_next_shard(table_copy));
        dyns_cleanup_globals();
        if (rc) {
            logmsg(LOGMSG_FATAL, "%s: error adding table %s.\n",
                   __func__, table);
            exit(1);
        }
    } else if (type == drop) {
        logmsg(LOGMSG_INFO, "Replicant dropping table:%s\n", table);
        if (delete_table_rep((char *)table, tran)) {
            logmsg(LOGMSG_FATAL, "%s: error deleting table  %s.\n",
                   __func__, table);
            exit(1);
        }
    } else if (type == user_view) {
        rc = llmeta_load_views(thedb, tran);
        if (rc != 0) {
            logmsg(LOGMSG_ERROR, "llmeta_load_views failed\n");
        }
    } else if (type == bulkimport) {
        logmsg(LOGMSG_INFO, "Replicant bulkimporting table:%s\n", table);
        reload_after_bulkimport(db, tran);
    } else {
        assert(type == alter || type == fastinit);

        logmsg(LOGMSG_INFO, "Replicant %s table:%s\n",
               type == alter ? "altering" : "fastinit-ing", table);
        extern int gbl_broken_max_rec_sz;
        int saved_broken_max_rec_sz = gbl_broken_max_rec_sz;
        if (db->lrl > COMDB2_MAX_RECORD_SIZE)
            gbl_broken_max_rec_sz = db->lrl - COMDB2_MAX_RECORD_SIZE;
        if (reload_schema(table_copy, csc2text, tran)) {
            logmsg(LOGMSG_FATAL, "%s: error reloading schema for %s.\n",
                   __func__, table);
            exit(1);
        }
        gbl_broken_max_rec_sz = saved_broken_max_rec_sz;

        /* update the delayed deleted files */
        assert(db && !add_new_db);
    }

    if (type == add || type == drop || type == alter || type == fastinit ||
        type == bulkimport || type == user_view) {
        if (create_sqlmaster_records(tran)) {
            logmsg(LOGMSG_FATAL,
                   "create_sqlmaster_records: error creating sqlite master records for %s.\n",
                   table);
            exit(1);
        }
        create_sqlite_master(); /* create sql statements */
        BDB_BUMP_DBOPEN_GEN(type, NULL);
        if (type == drop || type == user_view)
            goto done;
    }

    free(table_copy);
    free(csc2text);

    /* if we just added the table, get a pointer for it */
    if (add_new_db) {
        db = get_dbtable_by_name(table);
        if (!db) {
            logmsg(LOGMSG_FATAL, "%s: could not find newly created db: %s.\n",
                   __func__, table);
            exit(1);
        }
    }

    if (!IS_QUEUEDB_ROLLOVER_SCHEMA_CHANGE_TYPE(type)) {
        set_odh_options_tran(db, tran);
        db->tableversion = table_version_select(db, tran);
    }

    /* Make sure to add a version 1 schema for instant-schema change tables */
    if (add_new_db && db->odh && db->instant_schema_change) {
        struct schema *ondisk_schema;
        struct schema *ver_one;
        char tag[MAXTAGLEN];

        ondisk_schema = find_tag_schema(db->tablename, ".ONDISK");
        if (NULL == ondisk_schema) {
            logmsg(LOGMSG_FATAL, ".ONDISK not found in %s! PANIC!!\n",
                   db->tablename);
            exit(1);
        }
        ver_one = clone_schema(ondisk_schema);
        sprintf(tag, gbl_ondisk_ver_fmt, 1);
        free(ver_one->tag);
        ver_one->tag = strdup(tag);
        if (ver_one->tag == NULL) {
            logmsg(LOGMSG_FATAL, "strdup failed %s @ %d\n", __func__, __LINE__);
            exit(1);
        }
        add_tag_schema(db->tablename, ver_one);
    }

    if (!IS_QUEUEDB_ROLLOVER_SCHEMA_CHANGE_TYPE(type)) {
        llmeta_dump_mapping_tran(tran, thedb);
        llmeta_dump_mapping_table_tran(tran, thedb, table, 1);
    }

    if (type == add || type == alter) {
        if (create_datacopy_array(db)) {
            logmsg(LOGMSG_FATAL, "create_datacopy_array failed for %s.\n", table);
            exit(1);
        }
    }

    /* Fetch the correct dbnum for this table.  We need this step because db
     * numbers aren't stored in the schema, and it's not handed to us during
     * schema change.  But it is committed to the llmeta table, so we can fetch
     * it from there. */
    if (db != NULL) {
        dbnum = llmeta_get_dbnum_tran(tran, db->tablename, &bdberr);
        if (dbnum == -1) {
            logmsg(LOGMSG_ERROR, "failed to fetch dbnum for table \"%s\"\n",
                   db->tablename);
            rc = BDBERR_MISC;
            goto done;
        }
        db->dbnum = dbnum;

        fix_lrl_ixlen_tran(tran);
    }

    rc = 0;
done:
    if (tran) {
        bdb_set_tran_lockerid(tran, lid);
        /* Replace this lid with the original lid so we don't leak it.  Because
         * we haven't done any work with the original tran, just abort it. */
        rc = bdb_tran_abort(thedb->bdb_env, tran, &bdberr);
        if (rc) {
            logmsg(LOGMSG_FATAL, "%s:%d failed to abort transaction\n",
                   __FILE__, __LINE__);
            exit(1);
        }
    }

    return rc; /* success */
}
