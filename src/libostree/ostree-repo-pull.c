/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011,2012,2013 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ostree.h"
#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-repo-static-delta-private.h"
#include "ostree-fetcher.h"
#include "otutil.h"

typedef struct {
  OstreeRepo   *repo;
  OstreeRepoPullFlags flags;
  char         *remote_name;
  OstreeRepoMode remote_mode;
  OstreeFetcher *fetcher;
  SoupURI      *base_uri;

  GMainContext    *main_context;
  GMainLoop    *loop;
  GCancellable *cancellable;
  OstreeAsyncProgress *progress;

  gboolean      transaction_resuming;
  enum {
    OSTREE_PULL_PHASE_FETCHING_REFS,
    OSTREE_PULL_PHASE_FETCHING_OBJECTS
  }             phase;
  gint          n_scanned_metadata;
  SoupURI       *fetching_sync_uri;
  
  gboolean          gpg_verify;

  GPtrArray        *static_delta_metas;
  GHashTable       *scanned_metadata; /* Maps object name to itself */
  GHashTable       *requested_metadata; /* Maps object name to itself */
  GHashTable       *requested_content; /* Maps object name to itself */
  guint             n_outstanding_metadata_fetches;
  guint             n_outstanding_metadata_write_requests;
  guint             n_outstanding_content_fetches;
  guint             n_outstanding_content_write_requests;
  guint             n_outstanding_deltapart_fetches;
  guint             n_outstanding_deltapart_write_requests;
  gint              n_requested_metadata;
  gint              n_requested_content;
  guint             n_fetched_deltaparts;
  guint             n_fetched_metadata;
  guint             n_fetched_content;

  gboolean      have_previous_bytes;
  guint64       previous_bytes_sec;
  guint64       previous_total_downloaded;

  GError      **async_error;
  gboolean      caught_error;
} OtPullData;

typedef struct {
  OtPullData  *pull_data;
  GVariant    *object;
  gboolean     is_detached_meta;
} FetchObjectData;

typedef struct {
  OtPullData  *pull_data;
  char *expected_checksum;
} FetchStaticDeltaData;

static SoupURI *
suburi_new (SoupURI   *base,
            const char *first,
            ...) G_GNUC_NULL_TERMINATED;

static gboolean scan_one_metadata_object (OtPullData         *pull_data,
                                          const char         *csum,
                                          OstreeObjectType    objtype,
                                          guint               recursion_depth,
                                          GCancellable       *cancellable,
                                          GError            **error);

static gboolean scan_one_metadata_object_c (OtPullData         *pull_data,
                                            const guchar       *csum,
                                            OstreeObjectType    objtype,
                                            guint               recursion_depth,
                                            GCancellable       *cancellable,
                                            GError            **error);

static SoupURI *
suburi_new (SoupURI   *base,
            const char *first,
            ...)
{
  va_list args;
  GPtrArray *arg_array;
  const char *arg;
  char *subpath;
  SoupURI *ret;

  arg_array = g_ptr_array_new ();
  g_ptr_array_add (arg_array, (char*)soup_uri_get_path (base));
  g_ptr_array_add (arg_array, (char*)first);

  va_start (args, first);
  
  while ((arg = va_arg (args, const char *)) != NULL)
    g_ptr_array_add (arg_array, (char*)arg);
  g_ptr_array_add (arg_array, NULL);

  subpath = g_build_filenamev ((char**)arg_array->pdata);
  g_ptr_array_unref (arg_array);
  
  ret = soup_uri_copy (base);
  soup_uri_set_path (ret, subpath);
  g_free (subpath);
  
  va_end (args);
  
  return ret;
}

static gboolean
update_progress (gpointer user_data)
{
  OtPullData *pull_data = user_data;
  guint outstanding_writes = pull_data->n_outstanding_content_write_requests +
    pull_data->n_outstanding_metadata_write_requests +
    pull_data->n_outstanding_deltapart_write_requests;
  guint outstanding_fetches = pull_data->n_outstanding_content_fetches +
    pull_data->n_outstanding_metadata_fetches +
    pull_data->n_outstanding_deltapart_fetches;
  guint64 bytes_transferred = ostree_fetcher_bytes_transferred (pull_data->fetcher);
  guint fetched = pull_data->n_fetched_metadata + pull_data->n_fetched_content;
  guint requested = pull_data->n_requested_metadata + pull_data->n_requested_content;
  guint n_scanned_metadata = pull_data->n_scanned_metadata;
 
  g_assert (pull_data->progress);

  ostree_async_progress_set_uint (pull_data->progress, "outstanding-fetches", outstanding_fetches);
  ostree_async_progress_set_uint (pull_data->progress, "outstanding-writes", outstanding_writes);
  ostree_async_progress_set_uint (pull_data->progress, "fetched", fetched);
  ostree_async_progress_set_uint (pull_data->progress, "requested", requested);
  ostree_async_progress_set_uint (pull_data->progress, "scanned-metadata", n_scanned_metadata);
  ostree_async_progress_set_uint64 (pull_data->progress, "bytes-transferred", bytes_transferred);

  if (pull_data->fetching_sync_uri)
    {
      gs_free char *uri_string = soup_uri_to_string (pull_data->fetching_sync_uri, TRUE);
      gs_free char *status_string = g_strconcat ("Requesting ", uri_string, NULL);
      ostree_async_progress_set_status (pull_data->progress, status_string);
    }
  else
    ostree_async_progress_set_status (pull_data->progress, NULL);

  return TRUE;
}

static void
throw_async_error (OtPullData          *pull_data,
                   GError              *error)
{
  if (error)
    {
      if (!pull_data->caught_error)
        {
          pull_data->caught_error = TRUE;
          g_propagate_error (pull_data->async_error, error);
          g_main_loop_quit (pull_data->loop);
        }
      else
        {
          g_error_free (error);
        }
    }
}

static void
check_outstanding_requests_handle_error (OtPullData          *pull_data,
                                         GError              *error)
{
  gboolean current_fetch_idle = (pull_data->n_outstanding_metadata_fetches == 0 &&
                                 pull_data->n_outstanding_content_fetches == 0 &&
                                 pull_data->n_outstanding_deltapart_fetches == 0);
  gboolean current_write_idle = (pull_data->n_outstanding_metadata_write_requests == 0 &&
                                 pull_data->n_outstanding_content_write_requests == 0 &&
                                 pull_data->n_outstanding_deltapart_write_requests == 0 );
  gboolean current_idle = current_fetch_idle && current_write_idle;

  throw_async_error (pull_data, error);

  switch (pull_data->phase)
    {
    case OSTREE_PULL_PHASE_FETCHING_REFS:
      if (!pull_data->fetching_sync_uri)
        g_main_loop_quit (pull_data->loop);
      break;
    case OSTREE_PULL_PHASE_FETCHING_OBJECTS:
      if (current_idle)
        {
          g_debug ("pull: idle, exiting mainloop");
          
          g_main_loop_quit (pull_data->loop);
        }
      break;
    }
}

static gboolean
idle_check_outstanding_requests (gpointer user_data)
{
  check_outstanding_requests_handle_error (user_data, NULL);
  return FALSE;
}

static gboolean
run_mainloop_monitor_fetcher (OtPullData   *pull_data)
{
  GSource *update_timeout = NULL;
  GSource *idle_src;

  if (pull_data->progress)
    {
      update_timeout = g_timeout_source_new_seconds (1);
      g_source_set_callback (update_timeout, update_progress, pull_data, NULL);
      g_source_attach (update_timeout, g_main_loop_get_context (pull_data->loop));
      g_source_unref (update_timeout);
    }

  idle_src = g_idle_source_new ();
  g_source_set_callback (idle_src, idle_check_outstanding_requests, pull_data, NULL);
  g_source_attach (idle_src, pull_data->main_context);
  g_main_loop_run (pull_data->loop);

  if (update_timeout)
    g_source_destroy (update_timeout);

  return !pull_data->caught_error;
}

typedef struct {
  OtPullData     *pull_data;
  GInputStream   *result_stream;
} OstreeFetchUriSyncData;

static void
fetch_uri_sync_on_complete (GObject        *object,
                            GAsyncResult   *result,
                            gpointer        user_data) 
{
  OstreeFetchUriSyncData *data = user_data;

  data->result_stream = ostree_fetcher_stream_uri_finish ((OstreeFetcher*)object,
                                                          result, data->pull_data->async_error);
  data->pull_data->fetching_sync_uri = NULL;
  g_main_loop_quit (data->pull_data->loop);
}

static gboolean
fetch_uri_contents_membuf_sync (OtPullData    *pull_data,
                                SoupURI        *uri,
                                gboolean        add_nul,
                                gboolean        allow_noent,
                                GBytes        **out_contents,
                                GCancellable   *cancellable,
                                GError        **error)
{
  gboolean ret = FALSE;
  const guint8 nulchar = 0;
  gs_free char *ret_contents = NULL;
  gs_unref_object GMemoryOutputStream *buf = NULL;
  OstreeFetchUriSyncData fetch_data = { 0, };

  g_assert (error != NULL);

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  fetch_data.pull_data = pull_data;

  pull_data->fetching_sync_uri = uri;
  ostree_fetcher_stream_uri_async (pull_data->fetcher, uri, cancellable,
                                   fetch_uri_sync_on_complete, &fetch_data);

  run_mainloop_monitor_fetcher (pull_data);
  if (!fetch_data.result_stream)
    {
      if (allow_noent && g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (error);
          ret = TRUE;
          *out_contents = NULL;
        }
      goto out;
    }

  buf = (GMemoryOutputStream*)g_memory_output_stream_new (NULL, 0, g_realloc, g_free);
  if (g_output_stream_splice ((GOutputStream*)buf, fetch_data.result_stream,
                              G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                              cancellable, error) < 0)
    goto out;

  if (add_nul)
    {
      if (!g_output_stream_write ((GOutputStream*)buf, &nulchar, 1, cancellable, error))
        goto out;
    }

  if (!g_output_stream_close ((GOutputStream*)buf, cancellable, error))
    goto out;

  ret = TRUE;
  *out_contents = g_memory_output_stream_steal_as_bytes (buf);
 out:
  g_clear_object (&(fetch_data.result_stream));
  return ret;
}

static gboolean
fetch_uri_contents_utf8_sync (OtPullData  *pull_data,
                              SoupURI     *uri,
                              char       **out_contents,
                              GCancellable  *cancellable,
                              GError     **error)
{
  gboolean ret = FALSE;
  gs_unref_bytes GBytes *bytes = NULL;
  gs_free char *ret_contents = NULL;
  gsize len;

  if (!fetch_uri_contents_membuf_sync (pull_data, uri, TRUE, FALSE,
                                       &bytes, cancellable, error))
    goto out;

  ret_contents = g_bytes_unref_to_data (bytes, &len);
  bytes = NULL;

  if (!g_utf8_validate (ret_contents, -1, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid UTF-8");
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_contents, &ret_contents);
 out:
  return ret;
}

static void
enqueue_one_object_request (OtPullData        *pull_data,
                            const char        *checksum,
                            OstreeObjectType   objtype,
                            gboolean           is_detached_meta);

static gboolean
scan_dirtree_object (OtPullData   *pull_data,
                     const char   *checksum,
                     int           recursion_depth,
                     GCancellable *cancellable,
                     GError      **error)
{
  gboolean ret = FALSE;
  int i, n;
  gs_unref_variant GVariant *tree = NULL;
  gs_unref_variant GVariant *files_variant = NULL;
  gs_unref_variant GVariant *dirs_variant = NULL;

  if (recursion_depth > OSTREE_MAX_RECURSION)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Exceeded maximum recursion");
      goto out;
    }

  if (!ostree_repo_load_variant (pull_data->repo, OSTREE_OBJECT_TYPE_DIR_TREE, checksum,
                                 &tree, error))
    goto out;

  /* PARSE OSTREE_SERIALIZED_TREE_VARIANT */
  files_variant = g_variant_get_child_value (tree, 0);
  dirs_variant = g_variant_get_child_value (tree, 1);
      
  n = g_variant_n_children (files_variant);
  for (i = 0; i < n; i++)
    {
      const char *filename;
      gboolean file_is_stored;
      gs_unref_variant GVariant *csum = NULL;
      gs_free char *file_checksum = NULL;

      g_variant_get_child (files_variant, i, "(&s@ay)", &filename, &csum);

      if (!ot_util_filename_validate (filename, error))
        goto out;

      file_checksum = ostree_checksum_from_bytes_v (csum);

      if (!ostree_repo_has_object (pull_data->repo, OSTREE_OBJECT_TYPE_FILE, file_checksum,
                                   &file_is_stored, cancellable, error))
        goto out;
      
      if (!file_is_stored && !g_hash_table_lookup (pull_data->requested_content, file_checksum))
        {
          g_hash_table_insert (pull_data->requested_content, file_checksum, file_checksum);
          enqueue_one_object_request (pull_data, file_checksum, OSTREE_OBJECT_TYPE_FILE, FALSE);
          file_checksum = NULL;  /* Transfer ownership */
        }
    }
      
  n = g_variant_n_children (dirs_variant);
  for (i = 0; i < n; i++)
    {
      const char *dirname;
      gs_unref_variant GVariant *tree_csum = NULL;
      gs_unref_variant GVariant *meta_csum = NULL;

      g_variant_get_child (dirs_variant, i, "(&s@ay@ay)",
                           &dirname, &tree_csum, &meta_csum);

      if (!ot_util_filename_validate (dirname, error))
        goto out;

      if (!scan_one_metadata_object_c (pull_data, ostree_checksum_bytes_peek (tree_csum),
                                       OSTREE_OBJECT_TYPE_DIR_TREE, recursion_depth + 1,
                                       cancellable, error))
        goto out;
      
      if (!scan_one_metadata_object_c (pull_data, ostree_checksum_bytes_peek (meta_csum),
                                       OSTREE_OBJECT_TYPE_DIR_META, recursion_depth + 1,
                                       cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
fetch_ref_contents (OtPullData    *pull_data,
                    const char    *ref,
                    char         **out_contents,
                    GCancellable  *cancellable,
                    GError       **error)
{
  gboolean ret = FALSE;
  gs_free char *ret_contents = NULL;
  SoupURI *target_uri = NULL;

  target_uri = suburi_new (pull_data->base_uri, "refs", "heads", ref, NULL);
  
  if (!fetch_uri_contents_utf8_sync (pull_data, target_uri, &ret_contents, cancellable, error))
    goto out;

  g_strchomp (ret_contents);

  if (!ostree_validate_checksum_string (ret_contents, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_contents, &ret_contents);
 out:
  if (target_uri)
    soup_uri_free (target_uri);
  return ret;
}

static void
content_fetch_on_write_complete (GObject        *object,
                                 GAsyncResult   *result,
                                 gpointer        user_data)
{
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  OstreeObjectType objtype;
  const char *expected_checksum;
  gs_free guchar *csum = NULL;
  gs_free char *checksum = NULL;

  if (!ostree_repo_write_content_finish ((OstreeRepo*)object, result, 
                                         &csum, error))
    goto out;

  checksum = ostree_checksum_from_bytes (csum);

  ostree_object_name_deserialize (fetch_data->object, &expected_checksum, &objtype);
  g_assert (objtype == OSTREE_OBJECT_TYPE_FILE);

  g_debug ("write of %s complete", ostree_object_to_string (checksum, objtype));

  if (strcmp (checksum, expected_checksum) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted content object; checksum expected='%s' actual='%s'",
                   expected_checksum, checksum);
      goto out;
    }

  pull_data->n_fetched_content++;
 out:
  pull_data->n_outstanding_content_write_requests--;
  check_outstanding_requests_handle_error (pull_data, local_error);
  g_variant_unref (fetch_data->object);
  g_free (fetch_data);
}

static void
content_fetch_on_complete (GObject        *object,
                           GAsyncResult   *result,
                           gpointer        user_data) 
{
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  GCancellable *cancellable = NULL;
  guint64 length;
  gs_unref_object GFileInfo *file_info = NULL;
  gs_unref_variant GVariant *xattrs = NULL;
  gs_unref_object GInputStream *file_in = NULL;
  gs_unref_object GInputStream *object_input = NULL;
  gs_unref_object GFile *temp_path = NULL;
  const char *checksum;
  OstreeObjectType objtype;

  temp_path = ostree_fetcher_request_uri_with_partial_finish ((OstreeFetcher*)object, result, error);
  if (!temp_path)
    goto out;

  ostree_object_name_deserialize (fetch_data->object, &checksum, &objtype);
  g_assert (objtype == OSTREE_OBJECT_TYPE_FILE);

  g_debug ("fetch of %s complete", ostree_object_to_string (checksum, objtype));
  
  if (!ostree_content_file_parse (TRUE, temp_path, FALSE,
                                  &file_in, &file_info, &xattrs,
                                  cancellable, error))
    {
      /* If it appears corrupted, delete it */
      (void) gs_file_unlink (temp_path, NULL, NULL);
      goto out;
    }

  /* Also, delete it now that we've opened it, we'll hold
   * a reference to the fd.  If we fail to write later, then
   * the temp space will be cleaned up.
   */
  (void) gs_file_unlink (temp_path, NULL, NULL);

  if (!ostree_raw_file_to_content_stream (file_in, file_info, xattrs,
                                          &object_input, &length,
                                          cancellable, error))
    goto out;
  
  pull_data->n_outstanding_content_write_requests++;
  ostree_repo_write_content_async (pull_data->repo, checksum,
                                   object_input, length,
                                   cancellable,
                                   content_fetch_on_write_complete, fetch_data);

 out:
  pull_data->n_outstanding_content_fetches--;
  check_outstanding_requests_handle_error (pull_data, local_error);
}

static void
on_metadata_writed (GObject           *object,
                    GAsyncResult      *result,
                    gpointer           user_data)
{
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  const char *expected_checksum;
  OstreeObjectType objtype;
  gs_free char *checksum = NULL;
  gs_free guchar *csum = NULL;
  gs_free char *stringified_object = NULL;

  if (!ostree_repo_write_metadata_finish ((OstreeRepo*)object, result, 
                                          &csum, error))
    goto out;

  checksum = ostree_checksum_from_bytes (csum);

  ostree_object_name_deserialize (fetch_data->object, &expected_checksum, &objtype);
  g_assert (OSTREE_OBJECT_TYPE_IS_META (objtype));

  stringified_object = ostree_object_to_string (checksum, objtype);
  g_debug ("write of %s complete", stringified_object);

  if (strcmp (checksum, expected_checksum) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted metadata object; checksum expected='%s' actual='%s'",
                   expected_checksum, checksum);
      goto out;
    }

  if (!scan_one_metadata_object_c (pull_data, csum, objtype, 0,
                                   pull_data->cancellable, error))
    goto out;

 out:
  pull_data->n_outstanding_metadata_write_requests--;
  g_variant_unref (fetch_data->object);
  g_free (fetch_data);

  check_outstanding_requests_handle_error (pull_data, local_error);
}

static void
meta_fetch_on_complete (GObject           *object,
                        GAsyncResult      *result,
                        gpointer           user_data)
{
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  gs_unref_variant GVariant *metadata = NULL;
  gs_unref_object GFile *temp_path = NULL;
  const char *checksum;
  OstreeObjectType objtype;
  GError *local_error = NULL;
  GError **error = &local_error;

  ostree_object_name_deserialize (fetch_data->object, &checksum, &objtype);
  g_debug ("fetch of %s complete", ostree_object_to_string (checksum, objtype));

  temp_path = ostree_fetcher_request_uri_with_partial_finish ((OstreeFetcher*)object, result, error);
  if (!temp_path)
    {
      if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        goto out;
      else if (fetch_data->is_detached_meta)
        {
          /* There isn't any detached metadata, just fetch the commit */
          g_clear_error (&local_error);
          enqueue_one_object_request (pull_data, checksum, objtype, FALSE);
        }

      goto out;
    }

  if (fetch_data->is_detached_meta)
    {
      if (!ot_util_variant_map (temp_path, G_VARIANT_TYPE ("a{sv}"),
                                FALSE, &metadata, error))
        goto out;

      /* Now delete it, see comment in corresponding content fetch path */
      (void) gs_file_unlink (temp_path, NULL, NULL);

      if (!ostree_repo_write_commit_detached_metadata (pull_data->repo, checksum, metadata,
                                                       pull_data->cancellable, error))
        goto out;

      enqueue_one_object_request (pull_data, checksum, objtype, FALSE);
    }
  else
    {
      if (!ot_util_variant_map (temp_path, ostree_metadata_variant_type (objtype),
                                FALSE, &metadata, error))
        goto out;

      (void) gs_file_unlink (temp_path, NULL, NULL);
      
      ostree_repo_write_metadata_async (pull_data->repo, objtype, checksum, metadata,
                                        pull_data->cancellable,
                                        on_metadata_writed, fetch_data);
      pull_data->n_outstanding_metadata_write_requests++;
    }

 out:
  g_assert (pull_data->n_outstanding_metadata_fetches > 0);
  pull_data->n_outstanding_metadata_fetches--;
  pull_data->n_fetched_metadata++;
  throw_async_error (pull_data, local_error);
  if (local_error)
    {
      g_variant_unref (fetch_data->object);
      g_free (fetch_data);
    }
}

static void
static_deltapart_fetch_on_complete (GObject           *object,
                                    GAsyncResult      *result,
                                    gpointer           user_data)
{
  FetchStaticDeltaData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  gs_unref_variant GVariant *metadata = NULL;
  gs_unref_object GFile *temp_path = NULL;
  const char *checksum;
  OstreeObjectType objtype;
  GError *local_error = NULL;
  GError **error = &local_error;

  g_debug ("fetch static delta part %s complete", fetch_data->expected_checksum);

  temp_path = ostree_fetcher_request_uri_with_partial_finish ((OstreeFetcher*)object, result, error);
  if (!temp_path)
    goto out;
  

 out:
  g_assert (pull_data->n_outstanding_deltapart_fetches > 0);
  pull_data->n_outstanding_deltapart_fetches--;
  pull_data->n_fetched_deltaparts++;
  throw_async_error (pull_data, local_error);
  if (local_error)
    {
      g_free (fetch_data->expected_checksum);
      g_free (fetch_data);
    }
}

static gboolean
scan_commit_object (OtPullData         *pull_data,
                    const char         *checksum,
                    guint               recursion_depth,
                    GCancellable       *cancellable,
                    GError            **error)
{
  gboolean ret = FALSE;
  gs_unref_variant GVariant *commit = NULL;
  gs_unref_variant GVariant *tree_contents_csum = NULL;
  gs_unref_variant GVariant *tree_meta_csum = NULL;
  GVariantIter *iter = NULL;

  if (recursion_depth > OSTREE_MAX_RECURSION)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Exceeded maximum recursion");
      goto out;
    }

#ifdef HAVE_GPGME
  if (pull_data->gpg_verify)
    {
      if (!ostree_repo_verify_commit (pull_data->repo,
                                      checksum,
                                      NULL,
                                      NULL,
                                      cancellable,
                                      error))
        goto out;
    }
#endif

  if (!ostree_repo_load_variant (pull_data->repo, OSTREE_OBJECT_TYPE_COMMIT, checksum,
                                 &commit, error))
    goto out;

  /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
  g_variant_get_child (commit, 6, "@ay", &tree_contents_csum);
  g_variant_get_child (commit, 7, "@ay", &tree_meta_csum);

  if (!scan_one_metadata_object_c (pull_data,
                                   ostree_checksum_bytes_peek (tree_contents_csum),
                                   OSTREE_OBJECT_TYPE_DIR_TREE, recursion_depth + 1,
                                   cancellable, error))
    goto out;

  if (!scan_one_metadata_object_c (pull_data,
                                   ostree_checksum_bytes_peek (tree_meta_csum),
                                   OSTREE_OBJECT_TYPE_DIR_META, recursion_depth + 1,
                                   cancellable, error))
    goto out;
  
  ret = TRUE;
 out:
  if (iter)
    g_variant_iter_free (iter);
  return ret;
}

static gboolean
scan_one_metadata_object (OtPullData         *pull_data,
                          const char         *csum,
                          OstreeObjectType    objtype,
                          guint               recursion_depth,
                          GCancellable       *cancellable,
                          GError            **error)
{
  guchar buf[32];
  ostree_checksum_inplace_to_bytes (csum, buf);
  
  return scan_one_metadata_object_c (pull_data, buf, objtype,
                                     recursion_depth,
                                     cancellable, error);
}

static gboolean
scan_one_metadata_object_c (OtPullData         *pull_data,
                            const guchar         *csum,
                            OstreeObjectType    objtype,
                            guint               recursion_depth,
                            GCancellable       *cancellable,
                            GError            **error)
{
  gboolean ret = FALSE;
  gs_unref_variant GVariant *object = NULL;
  gs_free char *tmp_checksum = NULL;
  gboolean is_requested;
  gboolean is_stored;

  tmp_checksum = ostree_checksum_from_bytes (csum);
  object = ostree_object_name_serialize (tmp_checksum, objtype);

  if (g_hash_table_lookup (pull_data->scanned_metadata, object))
    return TRUE;

  is_requested = g_hash_table_lookup (pull_data->requested_metadata, tmp_checksum) != NULL;
  if (!ostree_repo_has_object (pull_data->repo, objtype, tmp_checksum, &is_stored,
                               cancellable, error))
    goto out;

  if (!is_stored && !is_requested)
    {
      char *duped_checksum = g_strdup (tmp_checksum);
      gboolean do_fetch_detached;

      g_hash_table_insert (pull_data->requested_metadata, duped_checksum, duped_checksum);

      do_fetch_detached = (objtype == OSTREE_OBJECT_TYPE_COMMIT);
      enqueue_one_object_request (pull_data, tmp_checksum, objtype, do_fetch_detached);
    }
  else if (is_stored)
    {
      if (pull_data->transaction_resuming || is_requested)
        {
          switch (objtype)
            {
            case OSTREE_OBJECT_TYPE_COMMIT:
              if (!scan_commit_object (pull_data, tmp_checksum, recursion_depth,
                                       pull_data->cancellable, error))
                goto out;
              break;
            case OSTREE_OBJECT_TYPE_DIR_META:
              break;
            case OSTREE_OBJECT_TYPE_DIR_TREE:
              if (!scan_dirtree_object (pull_data, tmp_checksum, recursion_depth,
                                        pull_data->cancellable, error))
                goto out;
              break;
            default:
              g_assert_not_reached ();
              break;
            }
        }
      g_hash_table_insert (pull_data->scanned_metadata, g_variant_ref (object), object);
      pull_data->n_scanned_metadata++;
    }

  ret = TRUE;
 out:
  return ret;
}

static void
enqueue_one_object_request (OtPullData        *pull_data,
                            const char        *checksum,
                            OstreeObjectType   objtype,
                            gboolean           is_detached_meta)
{
  SoupURI *obj_uri = NULL;
  gboolean is_meta;
  FetchObjectData *fetch_data;
  gs_free char *objpath = NULL;

  g_debug ("queuing fetch of %s.%s", checksum,
           ostree_object_type_to_string (objtype));

  if (is_detached_meta)
    {
      char buf[_OSTREE_LOOSE_PATH_MAX];
      _ostree_loose_path_with_suffix (buf, checksum, OSTREE_OBJECT_TYPE_COMMIT,
                                      pull_data->remote_mode, "meta");
      obj_uri = suburi_new (pull_data->base_uri, "objects", buf, NULL);
    }
  else
    {
      objpath = _ostree_get_relative_object_path (checksum, objtype, TRUE);
      obj_uri = suburi_new (pull_data->base_uri, objpath, NULL);
    }

  is_meta = OSTREE_OBJECT_TYPE_IS_META (objtype);
  if (is_meta)
    {
      pull_data->n_outstanding_metadata_fetches++;
      pull_data->n_requested_metadata++;
    }
  else
    {
      pull_data->n_outstanding_content_fetches++;
      pull_data->n_requested_content++;
    }
  fetch_data = g_new0 (FetchObjectData, 1);
  fetch_data->pull_data = pull_data;
  fetch_data->object = ostree_object_name_serialize (checksum, objtype);
  fetch_data->is_detached_meta = is_detached_meta;
  ostree_fetcher_request_uri_with_partial_async (pull_data->fetcher, obj_uri, pull_data->cancellable,
                                                 is_meta ? meta_fetch_on_complete : content_fetch_on_complete, fetch_data);
  soup_uri_free (obj_uri);
}

static gboolean
repo_get_string_key_inherit (OstreeRepo          *repo,
                             const char          *section,
                             const char          *key,
                             char               **out_value,
                             GError             **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  GKeyFile *config;
  gs_free char *ret_value = NULL;

  config = ostree_repo_get_config (repo);

  ret_value = g_key_file_get_value (config, section, key, &temp_error);
  if (temp_error)
    {
      OstreeRepo *parent = ostree_repo_get_parent (repo);
      if (parent &&
          (g_error_matches (temp_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND)
           || g_error_matches (temp_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_GROUP_NOT_FOUND)))
        {
          g_clear_error (&temp_error);
          if (!repo_get_string_key_inherit (parent, section, key, &ret_value, error))
            goto out;
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  ret = TRUE;
  ot_transfer_out_value (out_value, &ret_value);
 out:
  return ret;
}

static gboolean
load_remote_repo_config (OtPullData    *pull_data,
                         GKeyFile     **out_keyfile,
                         GCancellable  *cancellable,
                         GError       **error)
{
  gboolean ret = FALSE;
  gs_free char *contents = NULL;
  GKeyFile *ret_keyfile = NULL;
  SoupURI *target_uri = NULL;

  target_uri = suburi_new (pull_data->base_uri, "config", NULL);
  
  if (!fetch_uri_contents_utf8_sync (pull_data, target_uri, &contents,
                                     cancellable, error))
    goto out;

  ret_keyfile = g_key_file_new ();
  if (!g_key_file_load_from_data (ret_keyfile, contents, strlen (contents),
                                  0, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_keyfile, &ret_keyfile);
 out:
  g_clear_pointer (&ret_keyfile, (GDestroyNotify) g_key_file_unref);
  g_clear_pointer (&target_uri, (GDestroyNotify) soup_uri_free);
  return ret;
}

static gboolean
fetch_metadata_to_verify_delta_superblock (OtPullData      *pull_data,
                                           const char      *from_revision,
                                           const char      *checksum,
                                           GBytes          *superblock_data,
                                           GCancellable    *cancellable,
                                           GError         **error)
{
  gboolean ret = FALSE;
  gs_free char *meta_path = _ostree_get_relative_static_delta_detachedmeta_path (from_revision, checksum);
  gs_unref_bytes GBytes *detached_meta_data = NULL;
  SoupURI *target_uri = NULL;
  gs_unref_object GFile *temp_input_path = NULL;
  gs_unref_object GOutputStream *temp_input_stream = NULL;
  gs_unref_object GInputStream *superblock_in = NULL;
  gs_unref_variant GVariant *metadata = NULL;

  target_uri = suburi_new (pull_data->base_uri, meta_path, NULL);

  if (!fetch_uri_contents_membuf_sync (pull_data, target_uri, FALSE, FALSE,
                                       &detached_meta_data,
                                       pull_data->cancellable, error))
    {
      g_prefix_error (error, "GPG verification enabled, but failed to fetch metadata: ");
      goto out;
    }

  superblock_in = g_memory_input_stream_new_from_bytes (superblock_data);

  if (!gs_file_open_in_tmpdir (pull_data->repo->tmp_dir, 0644,
                               &temp_input_path, &temp_input_stream,
                               cancellable, error))
    goto out;

  if (0 > g_output_stream_splice (temp_input_stream, superblock_in,
                                  G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
                                  G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                  cancellable, error))
    goto out;

  metadata = ot_variant_new_from_bytes (G_VARIANT_TYPE ("a{sv}"),
                                        detached_meta_data,
                                        FALSE);

  if (!_ostree_repo_gpg_verify_file_with_metadata (pull_data->repo, temp_input_path,
                                                   metadata, NULL, NULL,
                                                   cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
request_static_delta_superblock_sync (OtPullData  *pull_data,
                                      const char  *from_revision,
                                      const char  *to_revision,
                                      GVariant   **out_delta_superblock,
                                      GCancellable *cancellable,
                                      GError     **error)
{
  gboolean ret = FALSE;
  gs_unref_variant GVariant *ret_delta_superblock = NULL;
  gs_free char *delta_name = _ostree_get_relative_static_delta_path (from_revision, to_revision);
  gs_unref_bytes GBytes *delta_superblock_data = NULL;
  gs_unref_bytes GBytes *delta_meta_data = NULL;
  gs_unref_variant GVariant *delta_superblock = NULL;
  SoupURI *target_uri = NULL;
  
  target_uri = suburi_new (pull_data->base_uri, delta_name, NULL);
  
  if (!fetch_uri_contents_membuf_sync (pull_data, target_uri, FALSE, TRUE,
                                       &delta_superblock_data,
                                       pull_data->cancellable, error))
    goto out;
  
  if (delta_superblock_data)
    {
      if (pull_data->gpg_verify)
        {
          if (!fetch_metadata_to_verify_delta_superblock (pull_data,
                                                          from_revision,
                                                          to_revision,
                                                          delta_superblock_data,
                                                          pull_data->cancellable, error))
            goto out;
        }

      ret_delta_superblock = ot_variant_new_from_bytes ((GVariantType*)OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT,
                                                        delta_superblock_data, FALSE);
    }
  
  ret = TRUE;
  gs_transfer_out_value (out_delta_superblock, &ret_delta_superblock);
 out:
  return ret;
}

static gboolean
process_one_static_delta (OtPullData   *pull_data,
                          const char   *from_revision,
                          const char   *to_revision,
                          GVariant     *delta_superblock,
                          GCancellable *cancellable,
                          GError      **error)
{
  gs_unref_variant GVariant *headers = NULL;
  gs_free char *delta_relpath = _ostree_get_relative_static_delta_path (from_revision, to_revision);
  gs_unref_object GFile *last_delta_dir =
    g_file_get_child (pull_data->repo->remote_cache_dir, "delta");
  gs_unref_object GFile *this_delta_superblock_path =
    g_file_get_child (last_delta_dir, delta_relpath);
  gsize len;
  const char *file_contents;

  if (!gs_file_ensure_directory (this_delta_dir, TRUE, cancellable, error))
    goto out;

  file_contents = g_bytes_get_data (delta_superblock, &len);

  if (!g_file_replace_contents (this_delta_superblock_path, ))
    goto out;
  
  /* Parsing OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT */
  headers = g_variant_get_child_value (meta, 3);
  n = g_variant_n_children (headers);
  for (i = 0; i < n; i++)
    {
      const guchar *csum;
      gs_unref_variant GVariant *header = NULL;
      gboolean have_all = FALSE;
      SoupURI *target_uri = NULL;
      gs_free char *deltapart_path = NULL;
      FetchStaticDeltaData *fetch_data;

      header = g_variant_get_child_value (headers, i);
      g_variant_get (header, "(@aytt@ay)", &csum_v, &size, &usize, &objects);

      csum = ostree_checksum_bytes_peek_validate (csum_v, error);
      if (!csum)
        goto out;

      if (!_ostree_repo_static_delta_part_have_all_objects (self,
                                                            objects,
                                                            &have_all,
                                                            cancellable, error))
        goto out;

      if (have_all)
        {
          g_debug ("Have all objects from static delta %s-%s part %u",
                   from_revision, to_revision,
                   i);
          continue;
        }

      deltapart_path = _ostree_get_relative_static_delta_part_path (from_revision,
                                                                    to_revision,
                                                                    i);

      fetch_data = g_new0 (FetchStaticDeltaData, 1);
      fetch_data->pull_data = pull_data;
      fetch_data->expected_checksum = ostree_checksum_from_bytes_v (csum_v);

      target_uri = suburi_new (pull_data->base_uri, deltapart_path, NULL);
      ostree_fetcher_request_uri_with_partial_async (pull_data->fetcher, target_uri, pull_data->cancellable,
                                                     static_deltapart_fetch_on_complete,
                                                     fetch_data);
      soup_uri_free (target_uri);
    }
}

gboolean
ostree_repo_pull (OstreeRepo               *self,
                  const char               *remote_name,
                  char                    **refs_to_fetch,
                  OstreeRepoPullFlags       flags,
                  OstreeAsyncProgress      *progress,
                  GCancellable             *cancellable,
                  GError                  **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  gboolean tls_permissive = FALSE;
  OstreeFetcherConfigFlags fetcher_flags = 0;
  gs_free char *remote_key = NULL;
  gs_free char *path = NULL;
  gs_free char *baseurl = NULL;
  gs_unref_hashtable GHashTable *requested_refs_to_fetch = NULL;
  gs_unref_hashtable GHashTable *commits_to_fetch = NULL;
  gs_free char *remote_mode_str = NULL;
  GSource *queue_src = NULL;
  OtPullData pull_data_real = { 0, };
  OtPullData *pull_data = &pull_data_real;
  GKeyFile *config = NULL;
  GKeyFile *remote_config = NULL;
  char **configured_branches = NULL;
  guint64 bytes_transferred;
  guint64 start_time;
  guint64 end_time;

  pull_data->async_error = error;
  pull_data->main_context = g_main_context_ref_thread_default ();
  pull_data->loop = g_main_loop_new (pull_data->main_context, FALSE);
  pull_data->flags = flags;

  pull_data->repo = self;
  pull_data->progress = progress;

  pull_data->scanned_metadata = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                                       (GDestroyNotify)g_variant_unref, NULL);
  pull_data->requested_content = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                        (GDestroyNotify)g_free, NULL);
  pull_data->requested_metadata = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                         (GDestroyNotify)g_free, NULL);

  start_time = g_get_monotonic_time ();

  pull_data->remote_name = g_strdup (remote_name);
  config = ostree_repo_get_config (self);

  remote_key = g_strdup_printf ("remote \"%s\"", pull_data->remote_name);
  if (!repo_get_string_key_inherit (self, remote_key, "url", &baseurl, error))
    goto out;
  pull_data->base_uri = soup_uri_new (baseurl);

#ifdef HAVE_GPGME
  if (!ot_keyfile_get_boolean_with_default (config, remote_key, "gpg-verify",
                                            TRUE, &pull_data->gpg_verify, error))
    goto out;
#else
  pull_data->gpg_verify = FALSE;
#endif

  pull_data->phase = OSTREE_PULL_PHASE_FETCHING_REFS;

  if (!ot_keyfile_get_boolean_with_default (config, remote_key, "tls-permissive",
                                            FALSE, &tls_permissive, error))
    goto out;
  if (tls_permissive)
    fetcher_flags |= OSTREE_FETCHER_FLAGS_TLS_PERMISSIVE;

  pull_data->fetcher = ostree_fetcher_new (pull_data->repo->tmp_dir,
                                           fetcher_flags);

  if (!pull_data->base_uri)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to parse url '%s'", baseurl);
      goto out;
    }

  if (!load_remote_repo_config (pull_data, &remote_config, cancellable, error))
    goto out;

  if (!ot_keyfile_get_value_with_default (remote_config, "core", "mode", "bare",
                                          &remote_mode_str, error))
    goto out;

  if (!ostree_repo_mode_from_string (remote_mode_str, &pull_data->remote_mode, error))
    goto out;

  if (pull_data->remote_mode != OSTREE_REPO_MODE_ARCHIVE_Z2)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't pull from archives with mode \"%s\"",
                   remote_mode_str);
      goto out;
    }

  pull_data->static_delta_metas = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);

  requested_refs_to_fetch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  commits_to_fetch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  if (refs_to_fetch != NULL)
    {
      char **strviter;
      for (strviter = refs_to_fetch; *strviter; strviter++)
        {
          const char *branch = *strviter;
          char *contents;

          if (ostree_validate_checksum_string (branch, NULL))
            {
              char *key = g_strdup (branch);
              g_hash_table_insert (commits_to_fetch, key, key);
            }
          else
            {
              if (!fetch_ref_contents (pull_data, branch, &contents, cancellable, error))
                goto out;
      
              /* Transfer ownership of contents */
              g_hash_table_insert (requested_refs_to_fetch, g_strdup (branch), contents);
            }
        }
    }
  else
    {
      char **branches_iter;

      configured_branches = g_key_file_get_string_list (config, remote_key, "branches", NULL, NULL);
      branches_iter = configured_branches;

      if (!(branches_iter && *branches_iter))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "No configured branches for remote %s", pull_data->remote_name);
          goto out;
        }
      for (;branches_iter && *branches_iter; branches_iter++)
        {
          const char *branch = *branches_iter;
          char *contents;
              
          if (!fetch_ref_contents (pull_data, branch, &contents, cancellable, error))
            goto out;

          /* Transfer ownership of contents */
          g_hash_table_insert (requested_refs_to_fetch, g_strdup (branch), contents);
        }
    }

  pull_data->phase = OSTREE_PULL_PHASE_FETCHING_OBJECTS;

  if (!ostree_repo_prepare_transaction (pull_data->repo, &pull_data->transaction_resuming,
                                        cancellable, error))
    goto out;

  g_debug ("resuming transaction: %s", pull_data->transaction_resuming ? "true" : " false");

  g_hash_table_iter_init (&hash_iter, commits_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *commit = value;
      if (!scan_one_metadata_object (pull_data, commit, OSTREE_OBJECT_TYPE_COMMIT,
                                     0, pull_data->cancellable, error))
        goto out;
    }

  g_hash_table_iter_init (&hash_iter, requested_refs_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      gs_free char *from_revision = NULL;
      const char *ref = key;
      const char *to_revision = value;
      GVariant *delta_superblock = NULL;

      if (!ostree_repo_resolve_rev (pull_data->repo, ref, TRUE,
                                    &from_revision, error))
        goto out;

      if (from_revision)
        {
          if (!request_static_delta_superblock_sync (pull_data, from_revision, to_revision,
                                                     &delta_superblock, cancellable, error))
            goto out;
        }
          
      if (!delta_superblock)
        {
          if (!scan_one_metadata_object (pull_data, to_revision, OSTREE_OBJECT_TYPE_COMMIT,
                                         0, pull_data->cancellable, error))
            goto out;
        }
      else
        {
          if (!process_one_static_delta (pull_data, from_revision, to_revision,
                                         delta_superblock,
                                         cancellable, error))
            goto out;
        }
    }

  /* Now await work completion */
  if (!run_mainloop_monitor_fetcher (pull_data))
    goto out;
  
  g_assert_cmpint (pull_data->n_outstanding_metadata_fetches, ==, 0);
  g_assert_cmpint (pull_data->n_outstanding_metadata_write_requests, ==, 0);
  g_assert_cmpint (pull_data->n_outstanding_content_fetches, ==, 0);
  g_assert_cmpint (pull_data->n_outstanding_content_write_requests, ==, 0);

  g_hash_table_iter_init (&hash_iter, requested_refs_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *ref = key;
      const char *checksum = value;
      gs_free char *remote_ref = NULL;
      gs_free char *original_rev = NULL;
          
      remote_ref = g_strdup_printf ("%s/%s", pull_data->remote_name, ref);

      if (!ostree_repo_resolve_rev (pull_data->repo, remote_ref, TRUE, &original_rev, error))
        goto out;
          
      if (original_rev && strcmp (checksum, original_rev) == 0)
        {
        }
      else
        {
          ostree_repo_transaction_set_ref (pull_data->repo, pull_data->remote_name, ref, checksum);

        }
    }

  if (!ostree_repo_commit_transaction (pull_data->repo, NULL, cancellable, error))
    goto out;

  end_time = g_get_monotonic_time ();

  bytes_transferred = ostree_fetcher_bytes_transferred (pull_data->fetcher);
  if (bytes_transferred > 0 && pull_data->progress)
    {
      guint shift; 
      gs_free char *msg = NULL;

      if (bytes_transferred < 1024)
        shift = 1;
      else
        shift = 1024;

      msg = g_strdup_printf ("%u metadata, %u content objects fetched; %" G_GUINT64_FORMAT " %s transferred in %u seconds", 
                             pull_data->n_fetched_metadata, pull_data->n_fetched_content,
                             (guint64)(bytes_transferred / shift),
                             shift == 1 ? "B" : "KiB",
                             (guint) ((end_time - start_time) / G_USEC_PER_SEC));
      ostree_async_progress_set_status (pull_data->progress, msg);
    }

  ret = TRUE;
 out:
  if (pull_data->main_context)
    g_main_context_unref (pull_data->main_context);
  if (pull_data->loop)
    g_main_loop_unref (pull_data->loop);
  g_strfreev (configured_branches);
  g_clear_object (&pull_data->fetcher);
  g_free (pull_data->remote_name);
  if (pull_data->base_uri)
    soup_uri_free (pull_data->base_uri);
  if (queue_src)
    g_source_destroy (queue_src);
  g_clear_pointer (&pull_data->static_delta_metas, (GDestroyNotify) g_ptr_array_unref);
  g_clear_pointer (&pull_data->scanned_metadata, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->requested_content, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->requested_metadata, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&remote_config, (GDestroyNotify) g_key_file_unref);
  return ret;
}
