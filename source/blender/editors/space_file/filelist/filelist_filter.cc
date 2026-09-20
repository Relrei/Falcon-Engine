/* SPDX-FileCopyrightText: 2007 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spfile
 */

#include <algorithm>
#include <string>

#include "AS_asset_representation.hh"
#include "AS_essentials_library.hh"

#include "BLI_fnmatch.h"
#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_string_search.hh"
#include "BLI_string_utf8.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_idtype.hh"

#include "../file_intern.hh"
#include "../filelist.hh"
#include "filelist_intern.hh"

namespace blender {

/* True if should be hidden, based on current filtering. */
static bool is_filtered_hidden(const char *filename,
                               const FileListFilter *filter,
                               const FileListInternEntry *file)
{
  if ((filename[0] == '.') && (filename[1] == '\0')) {
    return true; /* Ignore. */
  }

  if (filter->flags & FLF_HIDE_PARENT) {
    if (filename[0] == '.' && filename[1] == '.' && filename[2] == '\0') {
      return true; /* Ignore. */
    }
  }

  /* Check for _OUR_ "hidden" attribute. This not only mirrors OS-level hidden file
   * attribute but is also set for Linux/Mac "dot" files. See `filelist_readjob_list_dir`.
   */
  if ((filter->flags & FLF_HIDE_DOT) && (file->attributes & FILE_ATTR_HIDDEN)) {
    return true;
  }

  /* For data-blocks (but not the group directories), check the asset-only filter. */
  if (!(file->typeflag & FILE_TYPE_DIR) && (file->typeflag & FILE_TYPE_BLENDERLIB) &&
      (filter->flags & FLF_ASSETS_ONLY) && !(file->typeflag & FILE_TYPE_ASSET))
  {
    return true;
  }

  return false;
}

/**
 * Apply the filter string as file path matching pattern.
 * \return true when the file should be in the result set, false if it should be filtered out.
 */
static bool is_filtered_file_relpath(const FileListInternEntry *file, const FileListFilter *filter)
{
  if (filter->filter_search[0] == '\0') {
    return true;
  }

  /* If there's a filter string, apply it as filter even if FLF_DO_FILTER is not set. */
  return fnmatch(filter->filter_search, file->relpath, FNM_CASEFOLD) == 0;
}

/**
 * Apply the filter string as matching pattern on file name.
 * \return true when the file should be in the result set, false if it should be filtered out.
 */
static bool is_filtered_file_name(const FileListInternEntry *file, const FileListFilter *filter)
{
  if (filter->filter_search[0] == '\0') {
    return true;
  }

  /* If there's a filter string, apply it as filter even if FLF_DO_FILTER is not set. */
  return fnmatch(filter->filter_search, file->name, FNM_CASEFOLD) == 0;
}

/** \return true when the file should be in the result set, false if it should be filtered out. */
static bool is_filtered_file_type(const FileListInternEntry *file, const FileListFilter *filter)
{
  if (is_filtered_hidden(file->relpath, filter, file)) {
    return false;
  }

  if (FILENAME_IS_CURRPAR(file->relpath)) {
    return false;
  }

  /* We only check for types if some type are enabled in filtering. */
  if (filter->filter && (filter->flags & FLF_DO_FILTER)) {
    if (file->typeflag & FILE_TYPE_DIR) {
      if (file->typeflag & (FILE_TYPE_BLENDERLIB | FILE_TYPE_BLENDER | FILE_TYPE_BLENDER_BACKUP)) {
        if (!(filter->filter & (FILE_TYPE_BLENDER | FILE_TYPE_BLENDER_BACKUP))) {
          return false;
        }
      }
      else {
        if (!(filter->filter & FILE_TYPE_FOLDER)) {
          return false;
        }
      }
    }
    else {
      if (!(file->typeflag & filter->filter)) {
        return false;
      }
    }
  }
  return true;
}

bool is_filtered_file(FileListInternEntry *file, const char * /*root*/, FileListFilter *filter)
{
  return is_filtered_file_type(file, filter) &&
         (is_filtered_file_relpath(file, filter) || is_filtered_file_name(file, filter));
}

static bool is_filtered_id_file_type(const FileListInternEntry *file,
                                     const short id_code,
                                     const char *name,
                                     const FileListFilter *filter)
{
  if (!is_filtered_file_type(file, filter)) {
    return false;
  }

  /* We only check for types if some type are enabled in filtering. */
  if ((filter->filter || filter->filter_id) && (filter->flags & FLF_DO_FILTER)) {
    if (id_code) {
      if (!name && (filter->flags & FLF_HIDE_LIB_DIR)) {
        return false;
      }

      const uint64_t filter_id = BKE_idtype_idcode_to_idfilter(id_code);
      if (!(filter_id & filter->filter_id)) {
        return false;
      }
    }
  }

  return true;
}

/**
 * Get the asset metadata of a file, if it represents an asset. This may either be of a local ID
 * (ID in the current #Main) or read from an external asset library.
 */
static AssetMetaData *filelist_file_internal_get_asset_data(const FileListInternEntry *file)
{
  if (asset_system::AssetRepresentation *asset = file->get_asset()) {
    return &asset->get_metadata();
  }
  return nullptr;
}

void prepare_filter_asset_library(const FileList *filelist, FileListFilter *filter)
{
  /* Not used yet for the asset view template. */
  if (!filter->asset_catalog_filter) {
    return;
  }
  BLI_assert_msg(filelist->asset_library,
                 "prepare_filter_asset_library() should only be called when the file browser is "
                 "in asset browser mode");

  file_ensure_updated_catalog_filter_data(filter->asset_catalog_filter, filelist->asset_library);
}

bool is_filtered_asset(FileListInternEntry *file, FileListFilter *filter)
{
  asset_system::AssetRepresentation *asset = file->get_asset();
  const AssetMetaData &asset_data = asset->get_metadata();

  /* Not used yet for the asset view template. */
  if (filter->asset_catalog_filter &&
      !file_is_asset_visible_in_catalog_filter_settings(filter->asset_catalog_filter, &asset_data))
  {
    return false;
  }

  const bool is_online = asset->is_online_only();
  if (((filter->flags & FLF_ASSETS_HIDE_ONLINE) != 0) && is_online) {
    return false;
  }
  if (((filter->flags & FLF_ASSETS_HIDE_OFFLINE) != 0) && !is_online) {
    return false;
  }
  if (asset_system::skip_experimental_asset_catalog(asset_data.catalog_id)) {
    return false;
  }

  /* The actual string search is handled for the whole list at once, to allow sorting of the
   * results. */
  return true;
}

static bool is_filtered_lib_type(FileListInternEntry *file,
                                 const char * /*root*/,
                                 FileListFilter *filter)
{
  if (file->typeflag & FILE_TYPE_BLENDERLIB) {
    return is_filtered_id_file_type(file, file->blentype, file->name, filter);
  }
  return is_filtered_file_type(file, filter);
}

bool is_filtered_lib(FileListInternEntry *file, const char *root, FileListFilter *filter)
{
  return is_filtered_lib_type(file, root, filter) && is_filtered_file_relpath(file, filter);
}

bool is_filtered_main_assets(FileListInternEntry *file,
                             const char * /*dir*/,
                             FileListFilter *filter)
{
  /* "Filtered" means *not* being filtered out... So return true if the file should be visible. */
  return is_filtered_id_file_type(file, file->blentype, file->name, filter) &&
         is_filtered_asset(file, filter);
}

bool is_filtered_asset_library(FileListInternEntry *file, const char *root, FileListFilter *filter)
{
  if (filelist_intern_entry_is_main_file(file)) {
    return is_filtered_main_assets(file, root, filter);
  }

  return is_filtered_lib_type(file, root, filter) && is_filtered_asset(file, filter);
}

void filelist_tag_needs_filtering(FileList *filelist)
{
  filelist->flags |= FL_NEED_FILTERING;
}

bool filelist_needs_filtering(FileList *filelist)
{
  return (filelist->flags & FL_NEED_FILTERING);
}

static void filelist_filter_and_sort_assets(FileList *filelist,
                                            FileListInternEntry **entries_to_filter,
                                            int entries_num)
{
  const FileListFilter &filter = filelist->filter_data;
  if (filter.filter_search[0] == '\0') {
    /* No search text, just copy over the pre-filtered list. */
    if (filelist->filelist_intern.filtered) {
      MEM_delete(filelist->filelist_intern.filtered);
    }
    filelist->filelist_intern.filtered = static_cast<FileListInternEntry **>(MEM_new_uninitialized(
        sizeof(*filelist->filelist_intern.filtered) * size_t(entries_num), __func__));
    memcpy(filelist->filelist_intern.filtered,
           entries_to_filter,
           sizeof(*filelist->filelist_intern.filtered) * size_t(entries_num));
    filelist->filelist.entries_filtered_num = entries_num;
    return;
  }

  /* `filter->filter_search` contains "*the search text*". */
  char filter_search_buf[sizeof(FileListFilter::filter_search)];
  const size_t string_length = STRNCPY_RLEN(filter_search_buf, filter.filter_search);

  /* When doing a name comparison, get rid of the leading/trailing asterisks. */
  filter_search_buf[string_length - 1] = '\0';
  const char *search_str = filter_search_buf + 1;

  string_search::StringSearch<FileListInternEntry> search(nullptr,
                                                          string_search::MainWordsHeuristic::All);

  for (int i = 0; i < entries_num; i++) {
    FileListInternEntry *file = entries_to_filter[i];
    const AssetMetaData *asset_data = filelist_file_internal_get_asset_data(file);
    if (asset_data) {
      std::string searchable_string = file->name;
      for (const AssetTag &asset_tag : asset_data->tags) {
        searchable_string += " ";
        searchable_string += asset_tag.name;
      }
      search.add(searchable_string, file);
    }
    else {
      search.add(file->name, file);
    }
  }

  const Vector<FileListInternEntry *> results = search.query(search_str);

  if (filelist->filelist_intern.filtered) {
    MEM_delete(filelist->filelist_intern.filtered);
  }

  const int num_filtered = results.size();
  filelist->filelist_intern.filtered = static_cast<FileListInternEntry **>(MEM_new_uninitialized(
      sizeof(*filelist->filelist_intern.filtered) * size_t(num_filtered), __func__));
  for (int i = 0; i < num_filtered; i++) {
    filelist->filelist_intern.filtered[i] = results[i];
  }
  filelist->filelist.entries_filtered_num = num_filtered;
}

/* -------------------------------------------------------------------- */
/** \name Falcon: fold numbered image sequences into one entry
 *
 * `render0000.png` ... `render1350.png` are shown as a single `render[0000-1350].png` item that
 * behaves like a movie file. The real files are still in the list internally; all frames but the
 * first get #FileListInternEntry::seq_skip so that they are dropped while filtering. The entry
 * that survives keeps its own `relpath` (the first frame), so everything that resolves a path
 * through it stays valid; only the displayed name and the icon change, plus the expansion done in
 * #file_sfile_to_operator_ex() when the entry is handed over to an operator.
 * \{ */

static void filelist_sequence_group_reset(FileList *filelist)
{
  for (FileListInternEntry &file : filelist->filelist_intern.entries) {
    if (file.seq_name) {
      MEM_delete(file.seq_name);
      file.seq_name = nullptr;
    }
    file.seq_first = 0;
    file.seq_last = 0;
    file.seq_digits = 0;
    file.seq_skip = false;
    file.typeflag &= ~FILE_TYPE_IMAGE_SEQUENCE;
  }
}

static void filelist_sequence_group_build(FileList *filelist)
{
  /* Key is `<head>\x01<tail>\x01<digits>`, so only files that share the exact same name pattern
   * *and* the same amount of digits can end up in the same sequence. */
  Map<std::string, Vector<FileListInternEntry *>> groups;

  for (FileListInternEntry &file : filelist->filelist_intern.entries) {
    if (file.typeflag & (FILE_TYPE_DIR | FILE_TYPE_BLENDERLIB)) {
      continue;
    }
    if (!(file.typeflag & FILE_TYPE_IMAGE)) {
      continue;
    }
    /* Aliases/shortcuts cannot be mixed with regular items in a multi-selection. */
    if (file.redirection_path || !file.relpath) {
      continue;
    }

    const char *filename = BLI_path_basename(file.relpath);
    char head[FILE_MAX], tail[FILE_MAX];
    ushort digits = 0;
    const int framenr = BLI_path_sequence_decode(
        filename, head, sizeof(head), tail, sizeof(tail), &digits);
    if (digits == 0) {
      /* No number in the name at all. */
      continue;
    }

    std::string key = std::string(head) + '\x01' + tail + '\x01' + std::to_string(int(digits));
    groups.lookup_or_add_default(key).append(&file);
    file.seq_first = framenr;
    file.seq_digits = digits;
  }

  for (Vector<FileListInternEntry *> &group : groups.values()) {
    if (group.size() < 2) {
      /* A single image is never a sequence. */
      group[0]->seq_first = 0;
      group[0]->seq_digits = 0;
      continue;
    }

    std::sort(group.begin(), group.end(), [](const auto *a, const auto *b) {
      return a->seq_first < b->seq_first;
    });

    /* Only fold a *complete* run. A gap means we do not know what the user wants, so leave the
     * files alone rather than pretending frames exist. */
    bool contiguous = true;
    for (int i = 1; i < group.size(); i++) {
      if (group[i]->seq_first != group[i - 1]->seq_first + 1) {
        contiguous = false;
        break;
      }
    }
    if (!contiguous) {
      for (FileListInternEntry *file : group) {
        file->seq_first = 0;
        file->seq_digits = 0;
      }
      continue;
    }

    FileListInternEntry *first = group.first();
    const int frame_first = first->seq_first;
    const int frame_last = group.last()->seq_first;
    const ushort digits = first->seq_digits;

    char head[FILE_MAX], tail[FILE_MAX];
    BLI_path_sequence_decode(
        BLI_path_basename(first->relpath), head, sizeof(head), tail, sizeof(tail), nullptr);

    char name[FILE_MAX];
    SNPRINTF(name,
             "%s[%0*d-%0*d]%s",
             head,
             int(digits),
             frame_first,
             int(digits),
             frame_last,
             tail);

    first->seq_name = BLI_strdup(name);
    first->seq_first = frame_first;
    first->seq_last = frame_last;
    first->seq_digits = digits;
    first->typeflag |= FILE_TYPE_IMAGE_SEQUENCE;

    for (int i = 1; i < group.size(); i++) {
      group[i]->seq_first = 0;
      group[i]->seq_digits = 0;
      group[i]->seq_skip = true;
    }
  }
}

/** \} */

void filelist_filter(FileList *filelist)
{
  int num_filtered = 0;
  const int num_files = filelist->filelist.entries_num;
  FileListInternEntry **filtered_tmp;

  if (ELEM(filelist->filelist.entries_num, FILEDIR_NBR_ENTRIES_UNSET, 0)) {
    return;
  }

  if (!(filelist->flags & FL_NEED_FILTERING)) {
    /* Assume it has already been filtered, nothing else to do! */
    return;
  }

  filelist->filter_data.flags &= ~FLF_HIDE_LIB_DIR;
  if (filelist->max_recursion) {
    /* Never show lib ID 'categories' directories when we are in 'flat' mode, unless
     * root path is a blend file. */
    char dir[FILE_MAX_LIBEXTRA];
    if (!filelist_islibrary(filelist, dir, nullptr)) {
      filelist->filter_data.flags |= FLF_HIDE_LIB_DIR;
    }
  }

  if (filelist->prepare_filter_fn) {
    filelist->prepare_filter_fn(filelist, &filelist->filter_data);
  }

  /* Falcon: (re)build the image sequence folding before filtering, it decides which entries are
   * dropped below. */
  filelist_sequence_group_reset(filelist);
  if (filelist->filter_data.flags & FLF_GROUP_SEQUENCES) {
    filelist_sequence_group_build(filelist);
  }

  filtered_tmp = MEM_new_array_uninitialized<FileListInternEntry *>(num_files, __func__);

  /* Filter remap & count how many files are left after filter in a single loop. */
  for (FileListInternEntry &file : filelist->filelist_intern.entries) {
    if (file.seq_skip) {
      continue;
    }
    if (filelist->filter_fn(&file, filelist->filelist.root, &filelist->filter_data)) {
      filtered_tmp[num_filtered++] = &file;
    }
  }

  if (filelist->tags & FILELIST_TAGS_APPLY_FUZZY_SEARCH) {
    filelist_filter_and_sort_assets(filelist, filtered_tmp, num_filtered);
  }
  else {
    if (filelist->filelist_intern.filtered) {
      MEM_delete(filelist->filelist_intern.filtered);
    }
    filelist->filelist_intern.filtered = MEM_new_array_uninitialized<FileListInternEntry *>(
        num_filtered, __func__);
    memcpy(filelist->filelist_intern.filtered,
           filtered_tmp,
           sizeof(*filelist->filelist_intern.filtered) * size_t(num_filtered));
    filelist->filelist.entries_filtered_num = num_filtered;
  }

  //  printf("Filtered: %d over %d entries\n", num_filtered, filelist->filelist.entries_num);

  filelist_cache_clear(filelist->filelist_cache, filelist->filelist_cache->size);
  filelist->flags &= ~FL_NEED_FILTERING;

  MEM_delete(filtered_tmp);
}

void filelist_setfilter_options(FileList *filelist,
                                const bool do_filter,
                                const bool hide_dot,
                                const bool hide_parent,
                                const uint64_t filter,
                                const uint64_t filter_id,
                                const bool filter_assets_only,
                                const bool filter_assets_hide_online,
                                const bool filter_assets_hide_offline,
                                const bool group_sequences,
                                const char *filter_glob,
                                const char *filter_search)
{
  bool update = false;

  if (((filelist->filter_data.flags & FLF_GROUP_SEQUENCES) != 0) != (group_sequences != 0)) {
    filelist->filter_data.flags ^= FLF_GROUP_SEQUENCES;
    update = true;
  }

  if (((filelist->filter_data.flags & FLF_DO_FILTER) != 0) != (do_filter != 0)) {
    filelist->filter_data.flags ^= FLF_DO_FILTER;
    update = true;
  }
  if (((filelist->filter_data.flags & FLF_HIDE_DOT) != 0) != (hide_dot != 0)) {
    filelist->filter_data.flags ^= FLF_HIDE_DOT;
    update = true;
  }
  if (((filelist->filter_data.flags & FLF_HIDE_PARENT) != 0) != (hide_parent != 0)) {
    filelist->filter_data.flags ^= FLF_HIDE_PARENT;
    update = true;
  }
  if (((filelist->filter_data.flags & FLF_ASSETS_ONLY) != 0) != (filter_assets_only != 0)) {
    filelist->filter_data.flags ^= FLF_ASSETS_ONLY;
    update = true;
  }
  if (((filelist->filter_data.flags & FLF_ASSETS_HIDE_ONLINE) != 0) !=
      (filter_assets_hide_online != 0))
  {
    filelist->filter_data.flags ^= FLF_ASSETS_HIDE_ONLINE;
    update = true;
  }
  if (((filelist->filter_data.flags & FLF_ASSETS_HIDE_OFFLINE) != 0) !=
      (filter_assets_hide_offline != 0))
  {
    filelist->filter_data.flags ^= FLF_ASSETS_HIDE_OFFLINE;
    update = true;
  }
  if (filelist->filter_data.filter != filter) {
    filelist->filter_data.filter = filter;
    update = true;
  }
  const uint64_t new_filter_id = (filter & FILE_TYPE_BLENDERLIB) ? filter_id : FILTER_ID_ALL;
  if (filelist->filter_data.filter_id != new_filter_id) {
    filelist->filter_data.filter_id = new_filter_id;
    update = true;
  }
  if (!STREQ(filelist->filter_data.filter_glob, filter_glob)) {
    STRNCPY_UTF8(filelist->filter_data.filter_glob, filter_glob);
    update = true;
  }
  if (BLI_strcmp_ignore_pad(filelist->filter_data.filter_search, filter_search, '*') != 0) {
    BLI_strncpy_ensure_pad(filelist->filter_data.filter_search,
                           filter_search,
                           '*',
                           sizeof(filelist->filter_data.filter_search));
    update = true;
  }

  if (update) {
    /* And now, free filtered data so that we know we have to filter again. */
    filelist_tag_needs_filtering(filelist);
  }
}

}  // namespace blender
