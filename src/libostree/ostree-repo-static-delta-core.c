/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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
 */

#include "config.h"

#include "ostree-repo-private.h"
#include "ostree-repo-static-delta-private.h"
#include "otutil.h"

gboolean
_ostree_static_delta_parse_checksum_array (GVariant      *array,
                                           guint8       **out_checksums_array,
                                           guint         *out_n_checksums,
                                           GError       **error)
{
  gsize n = g_variant_n_children (array);
  guint n_checksums;

  n_checksums = n / OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN;

  if (G_UNLIKELY(n == 0 ||
                 n > (G_MAXUINT32/OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN) ||
                 (n_checksums * OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN) != n))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid checksum array length %" G_GSIZE_FORMAT, n);
      return FALSE;
    }

  *out_checksums_array = (gpointer)g_variant_get_data (array);
  *out_n_checksums = n_checksums;

  return TRUE;
}


/**
 * ostree_repo_list_static_delta_names:
 * @self: Repo
 * @out_deltas: (out) (element-type utf8): String name of deltas (checksum-checksum.delta)
 * @cancellable: Cancellable
 * @error: Error
 *
 * This function synchronously enumerates all static deltas in the
 * repository, returning its result in @out_deltas.
 */ 
gboolean
ostree_repo_list_static_delta_names (OstreeRepo                  *self,
                                     GPtrArray                  **out_deltas,
                                     GCancellable                *cancellable,
                                     GError                     **error)
{
  gboolean ret = FALSE;
  gs_unref_ptrarray GPtrArray *ret_deltas = NULL;
  gs_unref_object GFileEnumerator *dir_enum = NULL;

  ret_deltas = g_ptr_array_new_with_free_func (g_free);

  if (g_file_query_exists (self->deltas_dir, NULL))
    {
      dir_enum = g_file_enumerate_children (self->deltas_dir, OSTREE_GIO_FAST_QUERYINFO,
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            NULL, error);
      if (!dir_enum)
        goto out;
      
      while (TRUE)
        {
          GFileInfo *file_info;
          GFile *child;
          const char *name;

          if (!gs_file_enumerator_iterate (dir_enum, &file_info, &child,
                                           NULL, error))
            goto out;
          if (file_info == NULL)
            break;

          if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_DIRECTORY)
            continue;

          name = gs_file_get_basename_cached (child);

          {
            gs_unref_object GFile *meta_path = g_file_get_child (child, "meta");

            if (g_file_query_exists (meta_path, NULL))
              {
                g_ptr_array_add (ret_deltas, g_strdup (name));
              }
          }
        }
    }

  ret = TRUE;
  gs_transfer_out_value (out_deltas, &ret_deltas);
 out:
  return ret;
}

gboolean
_ostree_repo_static_delta_part_have_all_objects (OstreeRepo             *repo,
                                                 GVariant               *checksum_array,
                                                 gboolean               *out_have_all,
                                                 GCancellable           *cancellable,
                                                 GError                **error)
{
  gboolean ret = FALSE;
  guint8 *checksums_data;
  guint i,n_checksums;
  gboolean have_object = TRUE;

  if (!_ostree_static_delta_parse_checksum_array (checksum_array,
                                                  &checksums_data,
                                                  &n_checksums,
                                                  error))
    goto out;

  for (i = 0; i < n_checksums; i++)
    {
      guint8 objtype = *checksums_data;
      const guint8 *csum = checksums_data + 1;
      char tmp_checksum[65];

      if (G_UNLIKELY(!ostree_validate_structureof_objtype (objtype, error)))
        goto out;

      ostree_checksum_inplace_from_bytes (csum, tmp_checksum);

      if (!ostree_repo_has_object (repo, (OstreeObjectType) objtype, tmp_checksum,
                                   &have_object, cancellable, error))
        goto out;

      if (!have_object)
        break;

      checksums_data += OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN;
    }

  ret = TRUE;
  *out_have_all = have_object;
 out:
  return ret;
}

/**
 * ostree_repo_static_delta_execute_offline:
 * @self: Repo
 * @dir: Path to a directory containing static delta data
 * @skip_validation: If %TRUE, assume data integrity
 * @cancellable: Cancellable
 * @error: Error
 *
 * Given a directory representing an already-downloaded static delta
 * on disk, apply it, generating a new commit.  The directory must be
 * named with the form "FROM-TO", where both are checksums, and it
 * must contain a file named "meta", along with at least one part.
 */
gboolean
ostree_repo_static_delta_execute_offline (OstreeRepo                    *self,
                                          GFile                         *dir,
                                          gboolean                       skip_validation,
                                          GCancellable                  *cancellable,
                                          GError                      **error)
{
  gboolean ret = FALSE;
  guint i, n;
  gs_unref_object GFile *meta_file = g_file_get_child (dir, "meta");
  gs_unref_variant GVariant *meta = NULL;
  gs_unref_variant GVariant *headers = NULL;

  if (!ot_util_variant_map (meta_file, G_VARIANT_TYPE (OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT),
                            FALSE, &meta, error))
    goto out;

  headers = g_variant_get_child_value (meta, 3);
  n = g_variant_n_children (headers);
  for (i = 0; i < n; i++)
    {
      guint64 size;
      guint64 usize;
      const guchar *csum;
      gboolean have_all;
      gs_unref_variant GVariant *header = NULL;
      gs_unref_variant GVariant *csum_v = NULL;
      gs_unref_variant GVariant *objects = NULL;
      gs_unref_object GFile *part_path = NULL;
      gs_unref_object GInputStream *raw_in = NULL;
      gs_unref_object GInputStream *in = NULL;

      header = g_variant_get_child_value (headers, i);
      g_variant_get (header, "(@aytt@ay)", &csum_v, &size, &usize, &objects);

      if (!_ostree_repo_static_delta_part_have_all_objects (self, objects, &have_all,
                                                            cancellable, error))
        goto out;

      /* If we already have these objects, don't bother executing the
       * static delta.
       */
      if (have_all)
        continue;

      csum = ostree_checksum_bytes_peek_validate (csum_v, error);
      if (!csum)
        goto out;

      part_path = ot_gfile_resolve_path_printf (dir, "%u", i);

      in = (GInputStream*)g_file_read (part_path, cancellable, error);
      if (!in)
        goto out;

      if (!skip_validation)
        {
          gs_free char *expected_checksum = ostree_checksum_from_bytes (csum);
          if (!_ostree_static_delta_part_validate (self, part_path, i,
                                                   expected_checksum,
                                                   cancellable, error))
            goto out;
        }

      {
        GMappedFile *mfile = gs_file_map_noatime (part_path, cancellable, error);
        gs_unref_bytes GBytes *bytes = NULL;

        if (!mfile)
          goto out;

        bytes = g_mapped_file_get_bytes (mfile);
        g_mapped_file_unref (mfile);
        
        if (!_ostree_static_delta_part_execute (self, objects, bytes,
                                                cancellable, error))
          {
            g_prefix_error (error, "executing delta part %i: ", i);
            goto out;
          }
      }
    }

  ret = TRUE;
 out:
  return ret;
}

